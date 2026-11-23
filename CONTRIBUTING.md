# Contributing

Start with a bounded, reproducible change. A focused improvement with evidence
is easier to review than a broad engine rewrite.

1. Follow [getting started](docs/GETTING-STARTED.md) and run the default tests.
2. Use a feature branch. Python work also needs the explicit environment and
   tests described in [python/README.md](python/README.md).
3. Explain the problem, resulting behavior and verification in your pull request.
4. Add a regression for a bug. For DSP, compare a fixed baseline and document
   frame counts, tolerances, latency/tails and intentional sound changes.
5. For concurrency, exercise ordering, saturation, cancellation and shutdown;
   use synchronization and the appropriate sanitizer presets.
6. Run `python3 tools/check_repository.py`, `git diff --check` and the local
   secret scan. Keep generated audio, private paths and credentials out of commits.

Tests and public examples must run without recordings, a device, network access
or the maintainer's workspace. Update docs and schemas when contracts change.
For implementation moves, follow the [C++ boundary plan](docs/CPP-BOUNDARIES.md)
and keep source extraction separate from timing or sound changes.
A passing offline test does not establish glitch-free realtime playback;
read the [execution contract](docs/ENGINE-CONTRACT.md).

Good first contributions: a clearer diagnostic, missing failure-path test,
parameter-unit documentation, recipe verification or a small guide improvement.
The [roadmap](docs/ROADMAP.md) gives larger tasks with acceptance criteria.
Discuss substantial API and sound changes before implementation.

By submitting your own contribution, you agree to its inclusion under MAM's
MIT license. Keep third-party notices and identify reused code. No copyright
assignment is requested; contributors retain their credit.
