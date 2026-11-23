#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Offline workers only. The owner must outlive every caller and destroy the
// pool on a control thread, never from one of its own jobs. Jobs may not submit
// to or wait/close/stop their own pool; those operations fail before waiting.
// The same guard covers callable destruction during cancellation cleanup.
// Accepted callable captures must remain valid until their futures settle and
// wait()/stop() completes. Running jobs are never forcibly cancelled.
class JobPool {
public:
  enum class ShutdownMode { Drain, CancelPending };

  class Full : public std::runtime_error {
  public:
    Full() : std::runtime_error("JobPool pending queue is full") {}
  };
  class Closed : public std::runtime_error {
  public:
    Closed() : std::runtime_error("JobPool admission is closed") {}
  };
  class Cancelled : public std::runtime_error {
  public:
    Cancelled() : std::runtime_error("JobPool pending job was cancelled") {}
  };

  static constexpr size_t defaultPendingCapacity = 1024;

  explicit JobPool(size_t numThreads, size_t pendingCapacity = defaultPendingCapacity)
      : capacity_(checkedCapacity(numThreads, pendingCapacity)), jobs_(capacity_),
        cancellation_(std::make_exception_ptr(Cancelled{})) {
    workers_.reserve(numThreads);
    try {
      for (size_t i = 0; i < numThreads; ++i)
        workers_.emplace_back([this] { workerLoop(); });
    } catch (...) {
      // A failed thread constructor must not leave joinable threads in a
      // partially constructed vector whose destruction would terminate.
      stop(ShutdownMode::CancelPending);
      throw;
    }
  }

  ~JobPool() { stop(); }
  JobPool(const JobPool&) = delete;
  JobPool& operator=(const JobPool&) = delete;
  JobPool(JobPool&&) = delete;
  JobPool& operator=(JobPool&&) = delete;

  // Admission never waits for queue space. Full and Closed mean the callable
  // was not accepted. Capacity counts pending jobs, excluding running workers.
  // User exceptions (including non-std exceptions) are stored in the future.
  template <typename Callable>
  auto submit(Callable&& callable)
      -> std::future<std::invoke_result_t<std::decay_t<Callable>&>> {
    rejectWorkerOperation();
    using Job = Task<std::decay_t<Callable>>;
    auto job = std::make_unique<Job>(std::forward<Callable>(callable), cancellation_);
    auto result = job->future();
    {
      std::lock_guard lock(mutex_);
      if (!accepting_) throw Closed{};
      if (pending_ == capacity_) throw Full{};
      jobs_[tail_] = std::move(job);
      tail_ = (tail_ + 1) % capacity_;
      ++pending_;
    }
    available_.notify_one();
    return result;
  }

  // Close admission without joining running work. Drain can later escalate to
  // CancelPending, never reopen. CancelPending waits for pending cancellation
  // cleanup, including another caller's batch, before returning; running jobs
  // retain their ordinary result/exception and are not waited on here.
  void close(ShutdownMode mode = ShutdownMode::Drain) {
    rejectWorkerOperation();
    if (mode != ShutdownMode::Drain && mode != ShutdownMode::CancelPending)
      throw std::invalid_argument("Unknown JobPool shutdown mode");
    std::vector<std::unique_ptr<TaskBase>> cancelled;
    size_t cancelCount = 0;
    {
      std::lock_guard lock(mutex_);
      accepting_ = false;
      if (mode == ShutdownMode::CancelPending && pending_ != 0) {
        cancelCount = pending_;
        cancelling_ += cancelCount;
        pending_ = 0;
        // Move the whole bounded ring out without allocating. Callable
        // destruction and future completion happen outside the state lock.
        cancelled.swap(jobs_);
      }
    }
    available_.notify_all();
    {
      const ExecutionScope cleanup(this);
      for (auto& job : cancelled) {
        if (job) {
          job->cancel();
          job.reset();
        }
      }
    }
    if (cancelCount != 0) {
      std::lock_guard lock(mutex_);
      cancelling_ -= cancelCount;
    }
    idle_.notify_all();
    if (mode == ShutdownMode::CancelPending) {
      std::unique_lock lock(mutex_);
      idle_.wait(lock, [this] { return cancelling_ == 0; });
    }
  }

  // Wait for accepted work, including cancellation cleanup, without closing
  // admission. Concurrent producers may extend this wait; close first when a
  // terminal barrier is needed. Repeated waits on an idle pool return directly.
  void wait() {
    rejectWorkerOperation();
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return pending_ == 0 && active_ == 0 && cancelling_ == 0; });
  }

  // Concurrent/repeated stop and join calls are supported. Close precedes the
  // join lock, allowing another caller to cancel pending work during a drain.
  void stop(ShutdownMode mode = ShutdownMode::Drain) {
    rejectWorkerOperation();
    close(mode);
    std::lock_guard joinLock(joinMutex_);
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    wait();
  }

  void join() { stop(); }
  size_t pendingCapacity() const noexcept { return capacity_; }

private:
  // Keep a stack rather than a single pool marker: a worker in A may close B,
  // whose cancelled callable must not reenter either A or B while unwinding.
  class ExecutionScope {
  public:
    explicit ExecutionScope(const JobPool* pool) noexcept
        : pool_(pool), previous_(execution_) { execution_ = this; }
    ~ExecutionScope() { execution_ = previous_; }
    ExecutionScope(const ExecutionScope&) = delete;
    ExecutionScope& operator=(const ExecutionScope&) = delete;
    const JobPool* pool() const noexcept { return pool_; }
    const ExecutionScope* previous() const noexcept { return previous_; }
  private:
    const JobPool* pool_;
    const ExecutionScope* previous_;
  };

  struct TaskBase {
    virtual ~TaskBase() = default;
    virtual void run() noexcept = 0;
    virtual void cancel() noexcept = 0;
  };

  template <typename Callable>
  class Task final : public TaskBase {
    using Result = std::invoke_result_t<Callable&>;
  public:
    template <typename Input>
    Task(Input&& callable, std::exception_ptr cancellation)
        : cancellation_(std::move(cancellation)),
          callable_(std::forward<Input>(callable)), task_([this]() -> Result {
            if (cancelled_) std::rethrow_exception(cancellation_);
            return std::invoke(callable_);
          }) {}

    std::future<Result> future() { return task_.get_future(); }
    // Exclusive ownership and one invocation per task guarantee a valid,
    // unsatisfied packaged_task. It captures exceptions from the callable.
    void run() noexcept override { task_(); }
    void cancel() noexcept override { cancelled_ = true; task_(); }

  private:
    bool cancelled_ = false;
    std::exception_ptr cancellation_;
    Callable callable_;
    std::packaged_task<Result()> task_;
  };

  static size_t checkedCapacity(size_t threads, size_t capacity) {
    if (threads == 0 || capacity == 0)
      throw std::invalid_argument("JobPool needs positive worker and pending capacities");
    return capacity;
  }

  void rejectWorkerOperation() const {
    for (auto scope = execution_; scope; scope = scope->previous()) {
      if (scope->pool() == this)
        throw std::logic_error("A JobPool task cannot submit to or control its own pool");
    }
  }

  void workerLoop() {
    const ExecutionScope execution(this);
    for (;;) {
      std::unique_ptr<TaskBase> job;
      {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this] { return !accepting_ || pending_ != 0; });
        if (pending_ == 0) break;
        job = std::move(jobs_[head_]);
        head_ = (head_ + 1) % capacity_;
        --pending_;
        ++active_;
      }
      job->run();
      job.reset();
      {
        std::lock_guard lock(mutex_);
        --active_;
      }
      idle_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable available_;
  std::condition_variable idle_;
  std::mutex joinMutex_;
  const size_t capacity_;
  std::vector<std::unique_ptr<TaskBase>> jobs_;
  std::exception_ptr cancellation_;
  std::vector<std::thread> workers_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t pending_ = 0;
  size_t active_ = 0;
  size_t cancelling_ = 0;
  bool accepting_ = true;
  inline static thread_local const ExecutionScope* execution_ = nullptr;
};
