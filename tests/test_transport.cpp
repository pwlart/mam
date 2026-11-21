#include "offline/TransportGenerator.hpp"
#include <doctest/doctest.h>

static GraphSpec::Transport fourOnTheFloor() {
    GraphSpec::Transport transport;
    transport.bpm = 120;
    transport.lengthBars = 2;
    GraphSpec::TransportPattern pattern;
    pattern.nodeId = "kick";
    pattern.steps = "x...x...x...x...";
    transport.patterns.push_back(pattern);
    return transport;
}

TEST_CASE("transport places quarter notes on exact sample positions at 48 kHz") {
    const auto transport = fourOnTheFloor();
    const auto events = generateCommandsFromTransport(transport, 48000);
    REQUIRE(events.size() == 8);
    for (size_t i = 0; i < events.size(); ++i) {
        CHECK(events[i].sampleTime == i * 24000u);
        CHECK(events[i].nodeId == "kick");
    }
}

TEST_CASE("swing delays offbeats without changing the bar length at 48 kHz") {
    auto transport = fourOnTheFloor();
    transport.patterns.front().steps = "xxxxxxxxxxxxxxxx";
    transport.swingPercent = 50;
    const auto events = generateCommandsFromTransport(transport, 48000);
    REQUIRE(events.size() == 32);
    CHECK(events[0].sampleTime == 0);
    CHECK(events[1].sampleTime == 7500);
    CHECK(events[2].sampleTime == 12000);
    CHECK(events[3].sampleTime == 19500);
    CHECK(events[16].sampleTime == 96000);
}

TEST_CASE("parameter locks arrive before triggers at the same sample") {
    auto transport = fourOnTheFloor();
    GraphSpec::TransportLock lock;
    lock.step = 0;
    lock.paramId = 7;
    lock.value = 0.5f;
    transport.patterns.front().locks.push_back(lock);
    const auto events = generateCommandsFromTransport(transport, 48000);
    REQUIRE(events.size() == 10);
    CHECK(events[0].sampleTime == 0);
    CHECK(events[0].type == "SetParam");
    CHECK(events[0].paramId == 7);
    CHECK(events[0].value == 0.5f);
    CHECK(events[1].sampleTime == 0);
    CHECK(events[1].type == "Trigger");
}

TEST_CASE("tempo changes take effect at the next bar boundary") {
    auto transport = fourOnTheFloor();
    transport.tempoRamps.push_back({1, 60});
    const auto events = generateCommandsFromTransport(transport, 48000);
    REQUIRE(events.size() == 8);
    CHECK(events[3].sampleTime == 72000);
    CHECK(events[4].sampleTime == 96000);
    CHECK(events[5].sampleTime == 144000);
    CHECK(events[7].sampleTime == 240000);
}
