# Releasing MAM

The community line starts at 0.1.0. Earlier private development is retained
separately by the author. Do not reintroduce its build trees, media, machine
paths or old Git history into this repository.

## Prepare locally

1. Update the version in CMake and Python package metadata together when needed;
   describe user-visible changes and compatibility limits in the changelog.
2. Run the default and optional Python suites. Run appropriate sanitizers for
   engine changes. Regenerate the DigitalMania studies in a new ignored directory.
3. Run `python3 tools/check_repository.py`, `bash tools/check_secrets.sh`
   (Gitleaks required) and `git diff --check`.
4. Review dependency licenses/hashes, the staged diff and the file list.
   Generated audio, environments, installers, credentials and build reports
   stay outside source control.
5. Commit, rebuild and check `mam --build-info`: the revision must be the release
   commit and the source must be clean. Tag that exact commit.
6. Push/publish only with maintainer authorization. Never silently force-update
   an existing release tag.

Local reports may contain absolute paths; review them before attaching to an
issue or release. Secret scanners are useful checks, not a guarantee of absence.

## Version and artifact policy

Use semantic versions for the CLI/package contract. During 0.x, call out breaking
changes even on minor releases; patch releases should preserve compatible
behavior. Record deliberate DSP/sound changes and format migrations explicitly.

A source release includes code, docs, recipes and vendored notices. Optional
audio assets must be generated solely from the included recipes, with a manifest
containing source commit, build identity, seed, rates, frames and hashes.
The full music recording and third-party source stems are never implied assets.

If a credential is ever exposed, revoke/rotate it first and follow
[security reporting](../SECURITY.md). Removing a branch or rewriting history
cannot guarantee deletion from existing clones, forks or caches.
