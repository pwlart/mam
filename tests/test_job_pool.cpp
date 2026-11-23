#include "core/JobPool.hpp"
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Mode = JobPool::ShutdownMode;

// Test completion is established by futures and joins, never elapsed sleep.
// This last-resort bound also covers a regressed join during failure cleanup.
class TestDeadline {
public:
  TestDeadline() : watcher_([this] {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, 10s, [this] { return finished_; })) {
      std::fputs("JobPool test exceeded its shutdown deadline\n", stderr);
      std::_Exit(EXIT_FAILURE);
    }
  }) {}
  ~TestDeadline() {
    { std::lock_guard lock(mutex_); finished_ = true; }
    changed_.notify_one();
    watcher_.join();
  }
private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool finished_ = false;
  std::thread watcher_;
};

// Copies share ownership, so a job never borrows a destroyed test gate.
class Gate {
  struct State { std::mutex mutex; std::condition_variable changed; bool open = false; };
public:
  void wait() const {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [&] { return state_->open; });
  }
  void open() const {
    { std::lock_guard lock(state_->mutex); state_->open = true; }
    state_->changed.notify_all();
  }
private:
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

class PoolCase {
public:
  PoolCase(size_t workers, size_t capacity) : pool(workers, capacity) {}
  ~PoolCase() {
    // Release every deliberately blocked job/caller even after REQUIRE exits.
    for (const auto& gate : gates_) gate.open();
    pool.close(Mode::CancelPending);
    joinCallers();
    pool.stop();
  }
  Gate gate() { gates_.emplace_back(); return gates_.back(); }
  template <class Function> void caller(Function&& function) {
    callers_.emplace_back(std::forward<Function>(function));
  }
  void joinCallers() {
    for (auto& caller : callers_) if (caller.joinable()) caller.join();
  }
private:
  TestDeadline deadline_;
public:
  JobPool pool;
private:
  std::vector<Gate> gates_;
  std::vector<std::thread> callers_;
};

template <class T> bool ready(std::future<T>& future) {
  const bool completed = future.wait_for(2s) == std::future_status::ready;
  CHECK(completed);
  return completed;
}

struct JobFailure : std::runtime_error {
  JobFailure() : std::runtime_error("generated job failure") {}
};

struct CleanupCounts {
  std::atomic<unsigned> destroyed{0};
  std::atomic<unsigned> rejected{0};
};

struct CleanupProbe {
  CleanupProbe(JobPool& owner, std::shared_ptr<CleanupCounts> observed)
      : pool(&owner), counts(std::move(observed)) {}
  JobPool* pool;
  std::shared_ptr<CleanupCounts> counts;
  ~CleanupProbe() {
    try { pool->wait(); }
    catch (const std::logic_error&) { ++counts->rejected; }
    ++counts->destroyed;
  }
};

struct OnDestroy {
  explicit OnDestroy(std::function<void()> action) : action_(std::move(action)) {}
  ~OnDestroy() { action_(); }
private:
  std::function<void()> action_;
};
}

TEST_CASE("JobPool rejects zero workers and zero pending capacity") {
  CHECK_THROWS_AS(JobPool(0, 1), std::invalid_argument);
  CHECK_THROWS_AS(JobPool(1, 0), std::invalid_argument);
  PoolCase fixture(1, 1);
  fixture.pool.stop();
}

TEST_CASE("JobPool returns values and job exceptions without losing its worker") {
  PoolCase fixture(1, 8);
  auto moved = fixture.pool.submit([value = std::make_unique<int>(37)]() mutable {
    return std::move(value);
  });
  auto failed = fixture.pool.submit([]() -> int { throw JobFailure{}; });
  auto survived = fixture.pool.submit([] { return 91; });
  fixture.pool.wait();
  REQUIRE(ready(moved));
  CHECK(*moved.get() == 37);
  REQUIRE(ready(failed));
  CHECK_THROWS_AS((void)failed.get(), JobFailure);
  REQUIRE(ready(survived));
  CHECK(survived.get() == 91);
}

TEST_CASE("JobPool pending capacity rejects full submissions and closed admission wins") {
  auto executions = std::make_shared<std::atomic<unsigned>>(0);
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  PoolCase fixture(1, 2);
  const auto release = fixture.gate();
  auto running = fixture.pool.submit([entered, release, executions] {
    entered->set_value(); release.wait(); ++*executions; return 10;
  });
  REQUIRE(ready(began));
  auto first = fixture.pool.submit([executions] { ++*executions; return 20; });
  auto second = fixture.pool.submit([executions] { ++*executions; return 30; });
  CHECK_THROWS_AS(fixture.pool.submit([executions] { ++*executions; }), JobPool::Full);
  CHECK(executions->load() == 0);
  fixture.pool.close(Mode::Drain);
  CHECK_THROWS_AS(fixture.pool.submit([] {}), JobPool::Closed);
  release.open();
  fixture.pool.stop();
  REQUIRE(ready(running)); REQUIRE(ready(first)); REQUIRE(ready(second));
  CHECK(running.get() == 10); CHECK(first.get() == 20); CHECK(second.get() == 30);
  CHECK(executions->load() == 3);
  CHECK_THROWS_AS(fixture.pool.submit([] {}), JobPool::Closed);
}

TEST_CASE("JobPool drain completes every accepted job exactly once") {
  constexpr size_t count = 48;
  auto executions = std::make_shared<std::array<std::atomic<unsigned>, count>>();
  PoolCase fixture(3, count);
  const auto release = fixture.gate();
  std::vector<std::future<size_t>> jobs;
  for (size_t index = 0; index < count; ++index)
    jobs.push_back(fixture.pool.submit([executions, release, index] {
      release.wait(); ++(*executions)[index]; return index;
    }));
  fixture.pool.close(Mode::Drain);
  CHECK_THROWS_AS(fixture.pool.submit([] {}), JobPool::Closed);
  release.open();
  fixture.pool.stop();
  for (size_t index = 0; index < count; ++index) {
    REQUIRE(ready(jobs[index]));
    CHECK(jobs[index].get() == index);
    CHECK((*executions)[index].load() == 1);
  }
}

TEST_CASE("JobPool cancel settles pending futures while running jobs finish normally") {
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  auto started = std::make_shared<std::atomic<unsigned>>(0);
  auto pendingRuns = std::make_shared<std::atomic<unsigned>>(0);
  PoolCase fixture(2, 4);
  const auto release = fixture.gate();
  std::array<std::future<int>, 2> running;
  for (auto& future : running) future = fixture.pool.submit([entered, started, release] {
    if (started->fetch_add(1) == 1) entered->set_value();
    release.wait(); return 7;
  });
  REQUIRE(ready(began));
  std::array<std::future<void>, 4> pending;
  for (auto& future : pending)
    future = fixture.pool.submit([pendingRuns] { ++*pendingRuns; });
  fixture.pool.close(Mode::CancelPending);
  for (auto& future : pending) {
    REQUIRE(ready(future));
    CHECK_THROWS_AS(future.get(), JobPool::Cancelled);
  }
  for (auto& future : running) CHECK(future.wait_for(0s) == std::future_status::timeout);
  fixture.pool.close(Mode::Drain); // A later drain cannot undo cancellation.
  CHECK_THROWS_AS(fixture.pool.submit([] {}), JobPool::Closed);
  release.open();
  fixture.pool.stop();
  for (auto& future : running) { REQUIRE(ready(future)); CHECK(future.get() == 7); }
  CHECK(pendingRuns->load() == 0);
}

TEST_CASE("JobPool drain can escalate to cancellation while another caller joins") {
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  auto joining = std::make_shared<std::promise<void>>();
  auto joinBegan = joining->get_future();
  auto joined = std::make_shared<std::promise<void>>();
  auto joinDone = joined->get_future();
  PoolCase fixture(1, 1);
  const auto release = fixture.gate();
  auto running = fixture.pool.submit([entered, release] { entered->set_value(); release.wait(); });
  REQUIRE(ready(began));
  auto pending = fixture.pool.submit([] { return 1; });
  fixture.pool.close(Mode::Drain);
  fixture.caller([&fixture, joining, joined] {
    joining->set_value(); fixture.pool.stop(Mode::Drain); joined->set_value();
  });
  REQUIRE(ready(joinBegan));
  CHECK(pending.wait_for(0s) == std::future_status::timeout);
  fixture.pool.close(Mode::CancelPending);
  REQUIRE(ready(pending));
  CHECK_THROWS_AS((void)pending.get(), JobPool::Cancelled);
  CHECK(joinDone.wait_for(0s) == std::future_status::timeout);
  release.open();
  REQUIRE(ready(joinDone));
  fixture.joinCallers();
  REQUIRE(ready(running));
  CHECK_NOTHROW(running.get());
}

TEST_CASE("JobPool wait observes idle completion without closing admission") {
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  auto idle = std::make_shared<std::promise<void>>();
  auto idleDone = idle->get_future();
  PoolCase fixture(1, 1);
  const auto release = fixture.gate();
  auto running = fixture.pool.submit([entered, release] { entered->set_value(); release.wait(); });
  REQUIRE(ready(began));
  fixture.caller([&fixture, idle] { fixture.pool.wait(); idle->set_value(); });
  CHECK(idleDone.wait_for(0s) == std::future_status::timeout);
  release.open();
  REQUIRE(ready(idleDone));
  fixture.joinCallers();
  REQUIRE(ready(running)); CHECK_NOTHROW(running.get());
  auto later = fixture.pool.submit([] { return 42; });
  fixture.pool.wait();
  REQUIRE(ready(later)); CHECK(later.get() == 42);
}

TEST_CASE("JobPool concurrent and repeated shutdown callers share completion") {
  constexpr size_t callers = 8;
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  auto allCalling = std::make_shared<std::promise<void>>();
  auto called = allCalling->get_future();
  auto count = std::make_shared<std::atomic<size_t>>(0);
  auto completed = std::make_shared<std::atomic<size_t>>(0);
  PoolCase fixture(1, 1);
  const auto release = fixture.gate();
  const auto start = fixture.gate();
  auto running = fixture.pool.submit([entered, release] { entered->set_value(); release.wait(); });
  REQUIRE(ready(began));
  for (size_t index = 0; index < callers; ++index)
    fixture.caller([&fixture, start, allCalling, count, completed, index] {
      start.wait();
      if (count->fetch_add(1) + 1 == callers) allCalling->set_value();
      if (index % 2 == 0) fixture.pool.stop(); else fixture.pool.join();
      ++*completed;
    });
  start.open();
  REQUIRE(ready(called));
  CHECK(completed->load() == 0);
  release.open();
  fixture.joinCallers();
  CHECK(completed->load() == callers);
  REQUIRE(ready(running)); CHECK_NOTHROW(running.get());
  CHECK_NOTHROW(fixture.pool.stop());
  CHECK_NOTHROW(fixture.pool.stop(Mode::CancelPending));
  CHECK_NOTHROW(fixture.pool.join());
  CHECK_NOTHROW(fixture.pool.wait());
  CHECK_THROWS_AS(fixture.pool.submit([] {}), JobPool::Closed);
}

TEST_CASE("JobPool concurrent submission and shutdown account for every attempted job") {
  constexpr size_t producers = 4, perProducer = 128, total = producers * perProducer;
  struct Outcomes {
    std::array<std::atomic<unsigned>, total> executions{};
    std::array<unsigned, total> admission{};
    std::array<std::future<size_t>, total> futures;
    std::atomic<bool> unexpected{false};
  };
  auto outcomes = std::make_shared<Outcomes>();
  auto initial = std::make_shared<std::atomic<size_t>>(0);
  auto allInitial = std::make_shared<std::promise<void>>();
  auto initialDone = allInitial->get_future().share();
  PoolCase fixture(3, 16);
  const auto start = fixture.gate();
  for (size_t producer = 0; producer < producers; ++producer)
    fixture.caller([&fixture, start, outcomes, initial, allInitial, producer] {
      start.wait();
      for (size_t offset = 0; offset < perProducer; ++offset) {
        const size_t index = producer * perProducer + offset;
        try {
          outcomes->futures[index] = fixture.pool.submit([outcomes, index] {
            ++outcomes->executions[index]; return index;
          });
          outcomes->admission[index] = 1;
        } catch (const JobPool::Full&) { outcomes->admission[index] = 2; }
        catch (const JobPool::Closed&) { outcomes->admission[index] = 3; }
        catch (...) { outcomes->unexpected.store(true); }
        if (offset == 0 && initial->fetch_add(1) + 1 == producers) allInitial->set_value();
      }
    });
  fixture.caller([&fixture, start, initialDone] {
    start.wait(); initialDone.wait(); fixture.pool.stop(Mode::Drain);
  });
  start.open();
  fixture.joinCallers();
  CHECK_FALSE(outcomes->unexpected.load());
  size_t accepted = 0;
  for (size_t index = 0; index < total; ++index) {
    CAPTURE(index);
    REQUIRE(outcomes->admission[index] != 0);
    if (outcomes->admission[index] == 1) {
      ++accepted;
      REQUIRE(ready(outcomes->futures[index]));
      CHECK(outcomes->futures[index].get() == index);
      CHECK(outcomes->executions[index].load() == 1);
    } else CHECK(outcomes->executions[index].load() == 0);
  }
  CHECK(accepted >= producers); // Shutdown waits for each producer's first attempt.
  CHECK_THROWS_AS(fixture.pool.submit([] {}), JobPool::Closed);
}

TEST_CASE("JobPool rejects lifecycle and submission reentry from its own worker") {
  PoolCase fixture(1, 4);
  auto result = fixture.pool.submit([&fixture] {
    unsigned rejected = 0;
    const auto rejects = [&](auto&& operation) {
      try { operation(); } catch (const std::logic_error&) { ++rejected; }
    };
    rejects([&] { (void)fixture.pool.submit([] {}); });
    rejects([&] { fixture.pool.close(); });
    rejects([&] { fixture.pool.wait(); });
    rejects([&] { fixture.pool.stop(); });
    rejects([&] { fixture.pool.join(); });
    return rejected;
  });
  REQUIRE(ready(result));
  CHECK(result.get() == 5);
  auto survived = fixture.pool.submit([] { return true; });
  REQUIRE(ready(survived)); CHECK(survived.get());
}

TEST_CASE("JobPool destructor drains accepted work before borrowed state is released") {
  TestDeadline deadline;
  std::array<std::atomic<unsigned>, 16> executions{};
  std::vector<std::future<void>> futures;
  {
    JobPool pool(2, executions.size());
    for (size_t index = 0; index < executions.size(); ++index)
      futures.push_back(pool.submit([&executions, index] { ++executions[index]; }));
  }
  for (size_t index = 0; index < executions.size(); ++index) {
    REQUIRE(ready(futures[index])); CHECK_NOTHROW(futures[index].get());
    CHECK(executions[index].load() == 1);
  }
}

TEST_CASE("JobPool cancellation destroys captured state before returning with a retained future") {
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  auto counts = std::make_shared<CleanupCounts>();
  PoolCase fixture(1, 1);
  const auto release = fixture.gate();
  auto running = fixture.pool.submit([entered, release] { entered->set_value(); release.wait(); });
  REQUIRE(ready(began));
  auto pending = fixture.pool.submit([
      probe = std::make_unique<CleanupProbe>(fixture.pool, counts)] {});
  fixture.pool.close(Mode::CancelPending);
  CHECK(counts->destroyed.load() == 1);
  CHECK(counts->rejected.load() == 1);
  REQUIRE(ready(pending));
  CHECK_THROWS_AS(pending.get(), JobPool::Cancelled);
  release.open();
  fixture.pool.stop();
  REQUIRE(ready(running)); CHECK_NOTHROW(running.get());
}

TEST_CASE("JobPool idle completion includes callable cleanup while its future remains retained") {
  auto counts = std::make_shared<CleanupCounts>();
  PoolCase fixture(1, 1);
  auto result = fixture.pool.submit([
      probe = std::make_unique<CleanupProbe>(fixture.pool, counts)] { return 5; });
  fixture.pool.wait();
  CHECK(counts->destroyed.load() == 1);
  CHECK(counts->rejected.load() == 1);
  REQUIRE(ready(result));
  CHECK(result.get() == 5);
}

TEST_CASE("JobPool concurrent cancellation callers wait for pending capture cleanup") {
  auto entered = std::make_shared<std::promise<void>>();
  auto began = entered->get_future();
  auto destroying = std::make_shared<std::promise<void>>();
  auto cleanupBegan = destroying->get_future();
  auto secondCalling = std::make_shared<std::promise<void>>();
  auto secondBegan = secondCalling->get_future();
  auto firstFinished = std::make_shared<std::promise<void>>();
  auto firstDone = firstFinished->get_future();
  auto secondFinished = std::make_shared<std::promise<void>>();
  auto secondDone = secondFinished->get_future();
  auto destroyed = std::make_shared<std::atomic<bool>>(false);
  PoolCase fixture(1, 1);
  const auto releaseWorker = fixture.gate();
  const auto releaseCleanup = fixture.gate();
  auto running = fixture.pool.submit([entered, releaseWorker] {
    entered->set_value(); releaseWorker.wait(); return 19;
  });
  REQUIRE(ready(began));
  auto pending = fixture.pool.submit([probe = std::make_unique<OnDestroy>(
      [destroying, releaseCleanup, destroyed] {
        destroying->set_value(); releaseCleanup.wait(); destroyed->store(true);
      })] {});
  fixture.caller([&fixture, firstFinished] {
    fixture.pool.close(Mode::CancelPending); firstFinished->set_value();
  });
  REQUIRE(ready(cleanupBegan));
  fixture.caller([&fixture, secondCalling, secondFinished] {
    secondCalling->set_value();
    fixture.pool.close(Mode::CancelPending); secondFinished->set_value();
  });
  REQUIRE(ready(secondBegan));
  // A bounded negative wait detects the former early-return path. Completion
  // is established below only after the explicit cleanup release handshake.
  CHECK(secondDone.wait_for(50ms) == std::future_status::timeout);
  CHECK_FALSE(destroyed->load());
  releaseCleanup.open();
  REQUIRE(ready(firstDone)); REQUIRE(ready(secondDone));
  fixture.joinCallers();
  CHECK(destroyed->load());
  REQUIRE(ready(pending)); CHECK_THROWS_AS(pending.get(), JobPool::Cancelled);
  CHECK(running.wait_for(0s) == std::future_status::timeout);
  releaseWorker.open(); fixture.pool.stop();
  REQUIRE(ready(running)); CHECK(running.get() == 19);
}

TEST_CASE("JobPool nested cancellation rejects operations on outer cleanup contexts") {
  // Both pools remain alive through either pool's cancellation cleanup,
  // including assertion-failure unwinding before explicit cancellation.
  struct Pools {
    TestDeadline deadline;
    JobPool first{1, 1};
    JobPool second{1, 1};
    Gate releaseFirst, releaseSecond;
    ~Pools() {
      releaseFirst.open(); releaseSecond.open();
      first.close(Mode::CancelPending); second.close(Mode::CancelPending);
      first.stop(); second.stop();
    }
  } pools;
  auto firstEntered = std::make_shared<std::promise<void>>();
  auto firstBegan = firstEntered->get_future();
  auto secondEntered = std::make_shared<std::promise<void>>();
  auto secondBegan = secondEntered->get_future();
  auto counts = std::make_shared<CleanupCounts>();
  auto runningFirst = pools.first.submit([firstEntered, release = pools.releaseFirst] {
    firstEntered->set_value(); release.wait();
  });
  auto runningSecond = pools.second.submit([secondEntered, release = pools.releaseSecond] {
    secondEntered->set_value(); release.wait();
  });
  REQUIRE(ready(firstBegan)); REQUIRE(ready(secondBegan));
  auto inner = pools.second.submit([
      probe = std::make_unique<CleanupProbe>(pools.first, counts)] {});
  auto outer = pools.first.submit([
      probe = std::make_unique<OnDestroy>([&pools] { pools.second.close(Mode::CancelPending); })] {});
  pools.first.close(Mode::CancelPending);
  CHECK(counts->destroyed.load() == 1);
  CHECK(counts->rejected.load() == 1);
  REQUIRE(ready(inner)); CHECK_THROWS_AS(inner.get(), JobPool::Cancelled);
  REQUIRE(ready(outer)); CHECK_THROWS_AS(outer.get(), JobPool::Cancelled);
  pools.releaseFirst.open(); pools.releaseSecond.open();
  pools.first.stop(); pools.second.stop();
  REQUIRE(ready(runningFirst)); CHECK_NOTHROW(runningFirst.get());
  REQUIRE(ready(runningSecond)); CHECK_NOTHROW(runningSecond.get());
}
