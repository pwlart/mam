# Working on MAM

Read `README.md`, `CONTRIBUTING.md`, `docs/TESTING.md` and the relevant contract.
MAM is standalone: do not assume access to its author's private music workspace.

- Use C++20, CMake targets, RAII, owned state, explicit frame/channel units and
  bounded memory. Keep DSP, CLI parsing, CoreAudio and offline I/O separate.
- Put substantial non-template implementations in compiled source files with
  explicit target dependencies; retain templates and deliberate small inline
  primitives in headers. Follow `docs/CPP-BOUNDARIES.md`; do not require a source
  file for every header or mix mechanical extraction with sound/timing changes.
- Preserve sound/timing unless the issue calls for a change. Explain audible
  differences and retain regression evidence.
- Run default configure/build/CTest for functional changes. Use ASan/UBSan for
  memory/buffer/DSP changes and debug/TSan for concurrency. Python bridge or
  example changes also use the `python` preset and its tests.
- Tests generate their inputs and run without devices, production media or
  network access. Use private temporary/build directories.
- Do not claim an allocation-free callback, universal scheduler determinism
  or native strict schema validation. See `docs/ROADMAP.md` for existing gaps.
- Queue policies must be executable. Never wait for a stopped consumer,
  silently discard triggers, or infer completion from a sleep.
- New random sources must be owned and seeded. Preserve integer frames;
  samples, frames, beats and seconds are distinct units.
- Python is optional and offline. No Python, installs, file or network I/O in
  the audio callback. Strict job schemas and new output destinations remain mandatory.
- Vendored files remain unmodified with pinned provenance, hashes and licenses.
  Keep configure/build/tests offline; dependency installation is explicit.
- Public examples synthesize their signals. No recordings, generated media,
  credentials, local machine paths or build trees in Git. Record provenance.
- `mam --build-info` describes the executable's last build. Keep source/binary
  hashes for production; a dirty revision label is not a clean source snapshot.
- Check docs and commands when updating contracts. Run
  `python3 tools/check_repository.py` and `bash tools/check_secrets.sh` before
  release. Report unrun checks; never suppress failures to make CI green.
- Do not rewrite published history, delete unique media or publish externally
  without an explicit user request. Routine reversible work proceeds within scope.
