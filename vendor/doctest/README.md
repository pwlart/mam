# Vendored doctest

Upstream: https://github.com/doctest/doctest/releases/tag/v2.5.3

Pinned commit: `2d0a9359a60c51affe2a9bebb1be1dca47868151` (2.5.3).
The single header, MIT license and CMake discovery modules are unmodified
upstream files. The CMake modules carry BSD-3-Clause notices; their license
text is included as `LICENSE-CMake.txt`. `upstream.json` records exact source
URLs, byte counts and SHA256 hashes, checked by CTest without network access.

`CMakeLists.txt` is MAM's small integration wrapper. The include directory is
marked SYSTEM; production code retains strict warnings. Only test targets
link `doctest::doctest`. `BUILD_TESTING=OFF` does not load this directory.

To update: select a reviewed upstream release/commit, replace the four
upstream framework files, retain applicable licenses, update the provenance
manifest, then run default/ASan/UBSan builds and CTest. Do not edit the vendored
header or turn checksum failures into automatic downloads.
