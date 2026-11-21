# Community roadmap

0.1.0 is a community preview. These are proposed priorities, not promised dates.
Issues should carry a small reproducer and the acceptance evidence below.
See [governance](../GOVERNANCE.md) for how changes are reviewed.

| Priority | Work | Done when |
| --- | --- | --- |
| P0 | Bound realtime startup scheduling | Event lists below, at and above queue capacity terminate without lost, duplicated or reordered events; no device needed |
| P0 | Remove allocation and logging from the callback | Precompiled routing/buffers plus a callback harness detect allocations, waits and I/O; telemetry is bounded |
| P1 | Own random state per node | Same seed matches across block/thread counts for noisy graphs; reference sound changes are documented |
| P1 | Rational beat-to-frame timing | Long-duration tempo/swing cases have explicit frame-error bounds; fractional rates no longer accumulate truncation |
| P1 | Bound worker lifecycle and failure handling | Reject submissions after stop; cancellation, join/drain and exceptions have executable tests |
| P1 | Reject unsupported graph cycles | Explicit graph policy replaces silent insertion-order fallback; invalid graphs fail before rendering |
| P2 | One parameter/schema authority | Generated docs and validation share units, IDs, ranges and migration rules |
| P2 | Portable offline core | CoreAudio separated from file/DSP libraries; Linux CI exercises actual offline rendering before claiming support |
| P2 | Isolated plugin host | Fake plugin fixtures cover crash, hang, late response and architecture mismatch outside the audio callback |

## Useful first contributions

- Improve one DigitalMania recipe with a clear parameter explanation and a
  generated-fixture test; preserve attribution and sample-free provenance.
- Add malformed WAV/job cases or a small diagnostic improvement.
- Verify the getting-started steps on a second Mac and report exact versions.
- Make generated players more accessible without adding services or trackers.
- Turn one timing/queue finding into a deterministic regression before its fix.

## Community infrastructure proposals

1. Maintain a short issue backlog with labels for bug, docs, DSP, tests and
   good-first-issue. Each item needs scope, an owner only when volunteered,
   and a runnable acceptance check.
2. Enable required CI checks and protected main after the initial clean import.
   The workflows are supplied; repository settings still require maintainer setup.
3. Publish release source/checksums and optional generated demo audio separately
   from Git. Document the exact source revision and tool versions for each asset.
4. Add reviewers as contributors build experience. Publish decisions and
   compatibility notes; avoid assuming a larger maintainer team already exists.
5. Add fuzzing for file/JSON readers and scheduled dependency/license review.

The full DigitalMania production, reference recordings and personal archive are
outside this community repository. Musical acceptance and software correctness
remain separate judgments.
