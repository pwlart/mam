#include "PythonTools.hpp"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
volatile sig_atomic_t interrupted = 0;
void onSignal(int signal) { interrupted = signal; }

struct Signals {
    struct sigaction oldInt{}, oldTerm{};
    Signals() {
        interrupted = 0;
        struct sigaction action{};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &oldInt) != 0)
            throw std::runtime_error("Cannot install worker interrupt handler");
        if (sigaction(SIGTERM, &action, &oldTerm) != 0) {
            sigaction(SIGINT, &oldInt, nullptr);
            throw std::runtime_error("Cannot install worker termination handler");
        }
    }
    ~Signals() {
        sigaction(SIGINT, &oldInt, nullptr);
        sigaction(SIGTERM, &oldTerm, nullptr);
    }
};

std::string environmentOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}
}

int runPythonTools(int argc, char** argv) {
    try {
        std::string python = environmentOr("MAM_PYTHON", MAM_DEFAULT_PYTHON);
        std::string workerDirectory = environmentOr("MAM_PYTHON_TOOLS_DIR", MAM_DEFAULT_PYTHON_TOOLS_DIR);
        double timeout = 300;
        std::vector<std::string> arguments;
        for (int i = 0; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--python" || argument == "--worker-dir" || argument == "--timeout") {
                if (++i >= argc) throw std::invalid_argument("Missing value for " + argument);
                if (argument == "--python") python = argv[i];
                else if (argument == "--worker-dir") workerDirectory = argv[i];
                else {
                    size_t consumed = 0;
                    const std::string value = argv[i];
                    timeout = std::stod(value, &consumed);
                    if (consumed != value.size() || !std::isfinite(timeout) || timeout <= 0 || timeout > 86400)
                        throw std::invalid_argument("Timeout must be in (0, 86400] seconds");
                }
            } else if (argument == "--help" || argument == "-h") {
                std::puts("mam tools [--python PATH] [--worker-dir DIR] [--timeout SECONDS] doctor\n"
                          "mam tools [options] run JOB.json\n"
                          "Offline worker; MAM_PYTHON and MAM_PYTHON_TOOLS_DIR override build defaults.");
                return 0;
            } else arguments.push_back(argument);
        }
        if (arguments.empty() ||
            !((arguments[0] == "doctor" && arguments.size() == 1) ||
              (arguments[0] == "run" && arguments.size() == 2)))
            throw std::invalid_argument("Use 'mam tools doctor' or 'mam tools run JOB.json'");
        if (python.empty())
            throw std::invalid_argument("Set MAM_PYTHON or --python to the prepared environment's interpreter");
        const auto worker = std::filesystem::path(workerDirectory) / "worker.py";
        if (!std::filesystem::is_regular_file(worker))
            throw std::invalid_argument("Worker not found; set MAM_PYTHON_TOOLS_DIR or --worker-dir");
        std::vector<std::string> command{python, "-I", "-B", worker.string()};
        command.insert(command.end(), arguments.begin(), arguments.end());
        std::vector<char*> pointers;
        for (auto& item : command) pointers.push_back(item.data());
        pointers.push_back(nullptr);

        // No shell, embedded interpreter or audio device. An isolated process
        // group lets timeout/cancellation stop FFmpeg children as well.
        Signals signals;
        posix_spawnattr_t attributes;
        int error = posix_spawnattr_init(&attributes);
        if (error) throw std::runtime_error(std::strerror(error));
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (!error) error = posix_spawnattr_setpgroup(&attributes, 0);
        pid_t child = -1;
        if (!error) error = posix_spawnp(&child, python.c_str(), nullptr, &attributes, pointers.data(), environ);
        posix_spawnattr_destroy(&attributes);
        if (error) {
            std::fprintf(stderr, "MAM tools could not start Python: %s\n", std::strerror(error));
            return 127;
        }
        const auto started = std::chrono::steady_clock::now();
        auto stopped = started;
        bool stopping = false;
        int stopCode = 0, status = 0;
        for (;;) {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) break;
            if (waited < 0 && errno != EINTR) {
                kill(-child, SIGKILL);
                throw std::runtime_error("Cannot wait for Python worker");
            }
            const auto now = std::chrono::steady_clock::now();
            if (!stopping && (interrupted || std::chrono::duration<double>(now - started).count() >= timeout)) {
                stopCode = interrupted ? 128 + interrupted : 124;
                stopping = true;
                stopped = now;
                kill(-child, SIGTERM);
            }
            if (stopping && now - stopped >= std::chrono::seconds(2)) kill(-child, SIGKILL);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stopping) {
            // Reap/stop descendants even if the worker itself exited promptly.
            kill(-child, SIGKILL);
            std::fprintf(stderr, "MAM tools %s\n", stopCode == 124 ? "timed out" : "cancelled");
            return stopCode;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MAM tools: %s\n", error.what());
        return 2;
    }
}
