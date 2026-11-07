# AHR-Decoder (vendored subset)

Upstream: <https://github.com/KL5NE/AHR-WWV-Decoder>, commit in `UPSTREAM_COMMIT`.
License: GPL-3.0-or-later (per-file SPDX headers; full text in `LICENSE`).

What is here, unmodified from upstream except `CMakeLists.txt` (a minimal
build of just these targets):

| Path | What | Shipped |
|---|---|---|
| `core/` | portable C++17 decoder engine | yes |
| `capi/include`, `capi/src` | plain C API, ABI 1 (`ahr_wwv.h` is the contract) | yes |
| `capi/testsynth/`, `sim/` | synthetic rig audio for tests | **no** (`test-synth` feature only) |

To update: copy the same paths from the new upstream commit, write its hash to
`UPSTREAM_COMMIT`, and if `AHR_WWV_ABI_VERSION` changed, update the bindings in
`../../src/lib.rs` (their layout asserts will fail the build otherwise).
