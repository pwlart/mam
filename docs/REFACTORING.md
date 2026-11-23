# Engine refactoring

23 November 2026. Small, reviewable changes implement the community roadmap.
Structural extraction and behavior fixes remain separate commits.

## First slice: application and planning boundaries

- `app/CliOptions` owns parsing and entry selection without opening devices,
  mutating engine globals or exiting the caller. Existing flags and defaults
  are preserved; stricter numeric validation is a separate future change.
- `app/SessionPlayback` owns the active realtime session application path.
  The duplicate, unreachable session branch has been removed from `main.cpp`.
- `session/RackTimeline` prepares prefixed commands and loop metadata without
  file/device access. Existing transport rounding, parameter lookup, bar
  overrides and solo/mute decisions are retained.
- `mam_app` and `mam_session` are compiled CMake targets. This first boundary
  extraction does not yet isolate all CoreAudio dependencies or thin every
  remaining command handler out of main.

The first slice adds eight CLI and five rack-planning tests. The 51 native
CTest cases pass. Four generated offline renders match the previous clean
0.1.0 build sample-for-sample at 44.1/48 kHz; eight CLI routes preserve output
and exit codes, apart from the expected build identity/path differences.

## Next slice: bounded session scheduling

Use one owned event schedule for initial enqueue and refill. Remove repeated
loop generation and waits on a stopped consumer. Explicitly define overload,
late-event and shutdown behavior, with generated device-free fixtures before
claiming the startup milestone complete.

Callback allocation/logging, graph validation/preparation, rational transport
time, per-node randomness and worker-pool lifecycle remain separate roadmap
items. These changes do not rerender accepted production masters.
