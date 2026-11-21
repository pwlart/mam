#include "core/Command.hpp"
#include <doctest/doctest.h>
#include <limits>
#include <thread>

TEST_CASE("command queue keeps cutoff events for the next audio block") {
    SpscCommandQueue<4> queue;
    for (SampleTime time : {0u, 128u, 256u}) {
        Command command;
        command.sampleTime = time;
        REQUIRE(queue.push(command));
    }
    CHECK_FALSE(queue.push(Command{}));
    std::vector<Command> events;
    queue.drainUpTo(128, events);
    REQUIRE(events.size() == 1);
    CHECK(events.front().sampleTime == 0);
    Command wrapped;
    wrapped.sampleTime = 384;
    REQUIRE(queue.push(wrapped));
    events.clear();
    queue.drainUpTo(385, events);
    REQUIRE(events.size() == 3);
    CHECK(events[0].sampleTime == 128);
    CHECK(events[1].sampleTime == 256);
    CHECK(events[2].sampleTime == 384);
    CHECK(queue.approxSizeProducer() == 0);
}

TEST_CASE("command queue publishes complete ordered events between threads") {
    SpscCommandQueue<64> queue;
    constexpr SampleTime count = 100000;
    std::jthread producer([&] {
        for (SampleTime time = 0; time < count; ++time) {
            Command command;
            command.sampleTime = time;
            command.type = CommandType::SetParam;
            command.paramId = 7;
            command.value = static_cast<float>(time);
            while (!queue.push(command)) std::this_thread::yield();
        }
    });
    std::vector<Command> batch;
    batch.reserve(queue.capacity());
    SampleTime received = 0;
    bool ordered = true;
    while (received < count) {
        batch.clear();
        queue.drainUpTo(std::numeric_limits<SampleTime>::max(), batch);
        for (const auto& command : batch) {
            ordered = ordered && command.sampleTime == received && command.paramId == 7 &&
                      command.value == static_cast<float>(received) && command.type == CommandType::SetParam;
            ++received;
        }
        if (batch.empty()) std::this_thread::yield();
    }
    CHECK(ordered);
    CHECK(received == count);
}
