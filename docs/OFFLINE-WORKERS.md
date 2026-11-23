# Offline worker ownership and failure

`JobPool` is an offline utility. Submission, futures, allocation and joining
do not belong in an audio callback. The owner must outlive all callers and
destroy the pool from a control thread after callers have stopped using it.

## Admission and completion

The constructor requires positive worker and pending capacities. Pending
capacity defaults to 1,024; running jobs occupy separate worker slots. A fixed
ring stores pending tasks in admission order. Multiple workers can complete
them in any order. This is a queue bound, not a limit on callable/result sizes.

`submit(callable)` returns a future. It never waits for queue space: `Full`
means the pending ring has no free slot; `Closed` means admission has ended.
Neither outcome accepts or runs the callable. Rejected submission may already
have moved its argument; callers retaining a retry must own a reconstructible
job description. Callable construction/allocation can also fail before admission.

Accepted work executes once, unless cancelled while still pending. Return
values and exceptions travel through the future; a throwing job does not kill
a worker. A ready future proves result completion, while `wait()` also waits
for callable cleanup. It leaves admission open, so concurrent producers may
extend that wait. Close admission first when a terminal barrier is needed.

## Closing and joining

| Operation | Meaning |
| --- | --- |
| `close(Drain)` | Reject new submissions; execute every accepted pending job |
| `close(CancelPending)` | Reject new submissions; settle pending jobs with `Cancelled`; allow running jobs to finish normally |
| `wait()` | Wait for pending jobs, running jobs and cancellation cleanup to finish |
| `stop(mode)` | Close admission, join workers, then wait for all cleanup |
| `join()` / destructor | Drain and join |

Drain can escalate to cancel-pending. Admission never reopens. Concurrent and
repeated control calls are supported; worker joining is serialized. Cancellation
calls wait for pending cancellation cleanup, including a concurrent caller's
batch, but do not wait for already running tasks. Cancellation
and worker dequeue share one ownership boundary: each accepted task is either
dequeued for execution or removed for cancellation, never both.

Running tasks are not interrupted. A task that never returns can prevent
`wait()`, `stop()` and destruction from completing. No hard shutdown deadline,
process isolation or user-facing render-cancel command is supplied here.
Jobs and their cleanup must not submit to or control their own pool; these
calls throw `logic_error` before waiting, including nested cleanup contexts.
Cross-pool dependency cycles remain the caller's responsibility. Destructors
must not throw, and destroying a pool from its own job is forbidden.

## Topological render integration

`OfflineTopoScheduler` sizes pending capacity to at least the current level
width and reserves future tracking before admission. Each level gets every
accepted future and waits for worker cleanup before advancing. If submission
or any node fails, siblings settle before graph/segment storage is released;
the first observed exception propagates to the render caller.

Failed renders clear partial output and release stale buffer references. Every
new call prepares/resets nodes and rebuilds topology, including after failure
or a graph/channel change. Retrying arbitrary external side effects is not
automatically safe; callers still need nodes with an appropriate reset contract.
Successful audio summation and node DSP are preserved.

The generated regressions cover queue limits, concurrent admission/shutdown,
drain and cancellation, exceptions, worker reentry and failure while a sibling
is blocked. Saved-build comparisons include serial and four-thread topology
at 44.1 and 48 kHz. Native noise now has a separate [seed contract](RANDOMNESS.md)
and noisy scheduler fixtures. Neither suite establishes universal graph
determinism or fixes remaining transport/modulation boundaries.
