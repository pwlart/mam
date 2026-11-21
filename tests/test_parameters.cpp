#include "core/ParameterRegistry.hpp"
#include <doctest/doctest.h>

TEST_CASE("linear automation reaches its target at the requested sample") {
    ParameterRegistry<> parameters;
    parameters.prepare(48000);
    parameters.ensureParam(1, 0.0f);
    parameters.rampTo(1, 1.0f, 1.0f);
    CHECK(parameters.current(1) == 0.0f);
    for (int sample = 1; sample < 48; ++sample)
        CHECK(parameters.next(1) == doctest::Approx(static_cast<float>(sample) / 48.0f));
    CHECK(parameters.next(1) == 1.0f);
    CHECK(parameters.next(1) == 1.0f);
}

TEST_CASE("retargeting automation continues from the current level") {
    ParameterRegistry<> parameters;
    parameters.prepare(1000);
    parameters.ensureParam(1, 0.0f);
    parameters.rampTo(1, 1.0f, 4.0f);
    CHECK(parameters.next(1) == 0.25f);
    CHECK(parameters.next(1) == 0.5f);
    parameters.rampTo(1, 0.0f, 2.0f);
    CHECK(parameters.current(1) == 0.5f);
    CHECK(parameters.next(1) == 0.25f);
    CHECK(parameters.next(1) == 0.0f);
}

TEST_CASE("step and zero length automation cancel a pending ramp") {
    ParameterRegistry<> parameters;
    parameters.prepare(1000);
    parameters.ensureParam(1, 0.0f);
    parameters.rampTo(1, 1.0f, 10.0f);
    SUBCASE("zero duration") { parameters.rampTo(1, 0.25f, 0.0f); }
    SUBCASE("step mode") {
        parameters.setSmoothing(1, ParameterRegistry<>::Smoothing::Step);
        parameters.rampTo(1, 0.25f, 100.0f);
    }
    for (int i = 0; i < 20; ++i) CHECK(parameters.next(1) == 0.25f);
}

TEST_CASE("exponential automation stays bounded and finishes on time") {
    ParameterRegistry<> parameters;
    parameters.prepare(48000);
    parameters.ensureParam(1, 0.0f);
    parameters.setSmoothing(1, ParameterRegistry<>::Smoothing::Expo);
    parameters.rampTo(1, 1.0f, 5.0f);
    float previous = 0.0f;
    for (int i = 0; i < 239; ++i) {
        const float value = parameters.next(1);
        CHECK(std::isfinite(value));
        CHECK(value >= previous);
        CHECK(value < 1.0f);
        previous = value;
    }
    CHECK(previous > 0.9f);
    CHECK(parameters.next(1) == 1.0f);
    parameters.rampTo(1, 0.0f, 0.0f);
    CHECK(parameters.next(1) == 0.0f);
}

TEST_CASE("registry capacity and repeated registration preserve existing automation") {
    ParameterRegistry<1> parameters;
    parameters.ensureParam(1, 0.5f);
    parameters.ensureParam(1, 1.0f);
    parameters.ensureParam(2, 1.0f);
    parameters.rampTo(2, 2.0f, 10.0f);
    CHECK(parameters.current(1) == 0.5f);
    CHECK(parameters.next(2) == 0.0f);
}
