# Changelog

## Unreleased

- Extract CLI parsing and realtime session application code into `mam_app`;
  extract device-independent rack timelines into `mam_session`.
- Remove the unreachable duplicate realtime session branch. Preserve existing
  CLI behavior, transport rounding and offline audio, with characterization tests.
- Use one owned, bounded event cursor for multi-rack session startup and refill.
  Reject an overfull initial window before activation, prevent repeated loops
  and out-of-order one-shots, retain events on queue pressure and report lateness.
- Preserve timestamp-group order during late recovery, join the producer before
  releasing its inputs, reset the clock before activation and stop audio before
  closing metrics. Single-graph feeding and callback allocation remain open.

## 0.1.0 — 2026-11-21

First community snapshot of the existing C++20 engine and optional Python tools.

- Standalone source tree without generated builds, audio or installer bundles.
- MIT license for original code and recipes; retained third-party notices.
- Synthesized DigitalMania examples with source provenance and native mixing.
- Contributor, governance, security, testing and release documentation.
- Vendored nlohmann/json and doctest with offline integrity checks.
- Repository-local Python setup without a parent-workspace dependency.
- Existing graph/stem/Python contracts retained; realtime, random-state and
  strict-schema limitations remain documented.

Earlier development history is preserved separately by the maintainer.
This public repository starts from this reviewed snapshot.
