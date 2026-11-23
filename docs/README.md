# Documentation

Start with [getting started](GETTING-STARTED.md), then render the
[DigitalMania studies](../examples/digitalmania/README.md).

| Topic | Reference |
| --- | --- |
| Commands and failure behavior | [CLI](CLI.md) |
| Tests, sanitizers and generated fixtures | [Testing](TESTING.md) |
| Offline preparation and analysis | [Python companion](../python/README.md) |
| Integer-frame PCM assembly | [Native stems](NATIVE-STEMS.md) |
| Ownership, clocks and realtime boundaries | [Engine contract](ENGINE-CONTRACT.md) |
| Work priorities and acceptance criteria | [Roadmap](ROADMAP.md) |
| Incremental implementation and verification | [Engine refactoring](REFACTORING.md) |
| Prepared callbacks, capacity and diagnostics | [Prepared realtime processing](PREPARED-REALTIME.md) |
| Offline admission, cancellation and joined cleanup | [Worker lifecycle](OFFLINE-WORKERS.md) |
| Native noise seeds and sound migration | [Randomness](RANDOMNESS.md) |
| Versioning and publishing | [Releasing](RELEASING.md) |
| DigitalMania provenance and migration boundaries | [norockstar integration](NOROCKSTAR.md) |
| Architecture lessons from nodeFlow | [nodeFlow adoption](NODEFLOW-ADOPTION.md) |

## Technical references

[Audio core](MAMAudioCore.md) · [Offline scheduler](OFFLINETOPOSCHEDULER.md) ·
[Sessions](SESSION.md) · [Session buses](SessionBuses.md) ·
[Sidechains](Sidechain.md) · [Modulation](LFO.md) · [MAMIC](MAMIC.md) ·
[Parameters](ParamTables.md) · [Graph schema](schema.graph.v1.json)

These describe the existing engine, including experimental paths and older
format sketches. An illustrative JSON fragment is not a validated runnable
session. The tested public entry points are the DigitalMania recipes and the
native stem/Python job contracts. Native strict schema validation, callback
allocation guarantees and general deterministic parallel synthesis remain gaps.
