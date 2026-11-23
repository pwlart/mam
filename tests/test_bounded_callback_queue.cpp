#include "core/Command.hpp"
#include "realtime/RealtimeTelemetry.hpp"
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <limits>
#include <span>
#include <thread>

TEST_CASE("bounded callback drain retains cutoff and span overflow events") {
    SpscCommandQueue<8> queue;
    for (SampleTime time : {0u, 2u, 2u, 3u, 8u}) {
        Command command;
        command.sampleTime = time;
        command.value = static_cast<float>(queue.approxSizeProducer());
        REQUIRE(queue.push(command));
    }
    std::array<Command, 2> batch{};
    CHECK(queue.drainUpToBounded(8, {}) == 0);
    CHECK(queue.approxSizeProducer() == 5);
    REQUIRE(queue.drainUpToBounded(8, batch) == 2);
    CHECK(batch[0].sampleTime == 0);
    CHECK(batch[1].sampleTime == 2);
    REQUIRE(queue.drainUpToBounded(8, batch) == 2);
    CHECK(batch[0].sampleTime == 2);
    CHECK(batch[0].value == 2.0f); // A same-time event is not deduplicated.
    CHECK(batch[1].sampleTime == 3);
    CHECK(queue.drainUpToBounded(8, batch) == 0);
    CHECK(queue.approxSizeProducer() == 1);
    REQUIRE(queue.drainUpToBounded(9, batch) == 1);
    CHECK(batch[0].sampleTime == 8);
    CHECK(queue.drainUpToBounded(9, batch) == 0);
}

TEST_CASE("bounded callback drain preserves FIFO across queue wraparound") {
    SpscCommandQueue<4> queue;
    std::array<Command, 8> batch{};
    for (SampleTime base = 0; base < 60; base += 3) {
        for (SampleTime index = 0; index < 3; ++index) {
            Command command;
            command.sampleTime = base + index;
            REQUIRE(queue.push(command));
        }
        CHECK_FALSE(queue.push(Command{}));
        REQUIRE(queue.drainUpToBounded(base + 3, batch) == 3);
        for (size_t index = 0; index < 3; ++index)
            CHECK(batch[index].sampleTime == base + index);
    }
}

TEST_CASE("bounded callback drain stays ordered under concurrent refill") {
    SpscCommandQueue<32> queue;
    constexpr SampleTime total = 20000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::atomic<bool> producerCompleted{false};
    std::latch started(1);
    std::jthread producer([&](std::stop_token stop) {
        started.count_down();
        for (SampleTime index = 0; index < total; ++index) {
            Command command;
            command.sampleTime = index;
            command.type = CommandType::SetParam;
            command.paramId = 7;
            command.value = static_cast<float>(index);
            while (!queue.push(command)) {
                if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return;
                std::this_thread::yield();
            }
        }
        producerCompleted.store(true, std::memory_order_release);
    });
    started.wait();
    std::array<Command, 128> storage{};
    SampleTime received = 0;
    bool ordered = true;
    bool bounded = true;
    size_t iteration = 0;
    while (received < total && std::chrono::steady_clock::now() < deadline) {
        // Large spans must still stop at the initial queue snapshot. Small ones
        // exercise retaining queued events while the producer wraps/refills.
        const size_t limit = (++iteration % 2 == 0) ? storage.size() : 3;
        const size_t count = queue.drainUpToBounded(
            std::numeric_limits<SampleTime>::max(), std::span(storage).first(limit));
        bounded = bounded && count <= limit && count <= queue.capacity();
        for (size_t index = 0; index < count; ++index) {
            const auto& command = storage[index];
            ordered = ordered && command.sampleTime == received && command.paramId == 7 &&
                command.type == CommandType::SetParam && command.value == static_cast<float>(received);
            ++received;
        }
        if (count == 0) std::this_thread::yield();
    }
    producer.request_stop();
    producer.join();
    CHECK(producerCompleted.load(std::memory_order_acquire));
    CHECK(received == total);
    CHECK(ordered);
    CHECK(bounded);
    CHECK(queue.approxSizeProducer() == 0);
}

TEST_CASE("a full callback queue permits a producer to cancel without a consumer") {
    SpscCommandQueue<2> queue;
    Command first;
    first.sampleTime = 4;
    REQUIRE(queue.push(first));
    std::latch fullObserved(1);
    std::atomic<bool> cancelled{false};
    std::atomic<bool> wasFull{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::jthread producer([&](std::stop_token stop) {
        Command next;
        next.sampleTime = 5;
        const bool pushed = queue.push(next);
        wasFull.store(!pushed, std::memory_order_release);
        fullObserved.count_down(); // Also release the test if fullness regresses.
        if (pushed) return;
        do {
            if (stop.stop_requested()) { cancelled.store(true, std::memory_order_release); return; }
            if (std::chrono::steady_clock::now() >= deadline) return;
            std::this_thread::yield();
        } while (!queue.push(next));
    });
    fullObserved.wait();
    producer.request_stop();
    producer.join();
    CHECK(wasFull.load(std::memory_order_acquire));
    CHECK(cancelled.load(std::memory_order_acquire));
    std::array<Command, 2> batch{};
    REQUIRE(queue.drainUpToBounded(6, batch) == 1);
    CHECK(batch[0].sampleTime == 4);
}

TEST_CASE("callback telemetry drops only new records when full and resets stopped") {
    using namespace mam::realtime;
    TelemetryQueue<TelemetryRecord, 4> queue;
    CHECK(queue.capacity() == 3);
    for (size_t index = 0; index < queue.capacity(); ++index) {
        TelemetryRecord record;
        record.kind = TelemetryKind::RackMeter;
        record.index = index;
        record.a = static_cast<double>(index) + 0.25;
        REQUIRE(queue.push(record));
    }
    CHECK_FALSE(queue.push(TelemetryRecord{}));
    CHECK_FALSE(queue.push(TelemetryRecord{}));
    CHECK(queue.dropped() == 2);
    TelemetryRecord received;
    for (size_t index = 0; index < queue.capacity(); ++index) {
        REQUIRE(queue.pop(received));
        CHECK(received.kind == TelemetryKind::RackMeter);
        CHECK(received.index == index);
        CHECK(received.a == static_cast<double>(index) + 0.25);
    }
    CHECK_FALSE(queue.pop(received));
    REQUIRE(queue.push(TelemetryRecord{}));
    queue.reset();
    CHECK(queue.dropped() == 0);
    CHECK_FALSE(queue.pop(received));
    REQUIRE(queue.push(TelemetryRecord{}));
    CHECK(queue.pop(received));
}

TEST_CASE("callback telemetry publishes complete records between threads") {
    struct Record { uint64_t sequence = 0; uint64_t inverse = 0; };
    mam::realtime::TelemetryQueue<Record, 32> queue;
    constexpr uint64_t total = 20000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::atomic<bool> producerCompleted{false};
    std::jthread producer([&](std::stop_token stop) {
        for (uint64_t sequence = 0; sequence < total; ++sequence) {
            const Record record{sequence, ~sequence};
            while (!queue.push(record)) {
                if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return;
                std::this_thread::yield();
            }
        }
        producerCompleted.store(true, std::memory_order_release);
    });
    uint64_t expected = 0;
    bool ordered = true;
    while (expected < total && std::chrono::steady_clock::now() < deadline) {
        Record record;
        if (queue.pop(record)) {
            ordered = ordered && record.sequence == expected && record.inverse == ~expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.request_stop();
    producer.join();
    CHECK(producerCompleted.load(std::memory_order_acquire));
    CHECK(expected == total);
    CHECK(ordered);
}
