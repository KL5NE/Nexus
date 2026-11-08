# AHR-Decoder in Nexus: Phase B review and integration plan

From Sean (KL5NE), with Claude. 6 Nov 2026.

Reviewed:
- Nexus fork `KL5NE/Nexus`, branch `feature/wwv-time-tab` at `5d047faa`, cut from upstream tag v1.22.0 (`c41ba30e`);
- the decoder repo at `0b0cb18`, with today's fixes and C API on top (attached as patches).

Status, updated 6 Nov evening:
- **Sean approved all three decisions** (section 4).
- **C1 is built and tested locally.** It's delivered as `nexus-C1-wwv-crates.patch`, with the details in section 3.
- **The W32Time spike is done** (`W32TIME_SPIKE.md`), and C4(b) is re-planned around it.

Every rule in the fork's `CLAUDE.md` and `docs/wwv/PHASE0_ORIENTATION.md` still applies.

---

## 1. Review of Phase 0

**The orientation note (`docs/wwv/PHASE0_ORIENTATION.md`) is accurate and well judged.** It has the right RX-only invariants, the right "never call a clock API" reading, and the right fail-safe stance. Four corrections and additions:

1. **Licence.** Nexus's workspace licence is **`GPL-3.0-only`** (`Cargo.toml`), not "or later".
   - The decoder repo ships the GPLv3 text, but its source files carry **no licence headers**.
   - Nexus's own rule for vendored code is per-file review with headers intact, plus NOTICE and README credit.
   - **Before vendoring, the decoder files need SPDX headers.** That's Sean's decision. `GPL-3.0-or-later` is the most flexible choice, and it's compatible with Nexus's `GPL-3.0-only`.
2. **Capture timestamps don't exist in Nexus today.** The cpal input callbacks ignore `InputCallbackInfo` (`crates/tempo-audio/src/device.rs`, around lines 1729 and 1787), and the RX taps carry samples only. The decoder can still *decode* without timestamps, but it needs them to measure the **PC clock offset**. See section 3, step C2.
3. **`ClockState::publish(offset_ms, servers, rate_ppm, now)`** (`crates/tempo-app/src/clocksync.rs`):
   - It is built around NTP: `servers` means "agreeing NTP servers".
   - It **refuses** offsets over `MAX_STEER_MS` = 60 s, and the code calls that guard transmit-path safety.
   - So a WWV source needs its own provenance field, not a fake server count, as the note already says.
   - Guard 3 also means a clock that is minutes out is fixed **only** through the Windows clock path, never by slot steering. That's correct and stays.
4. **Steering FT timing needs Seth's sign-off.** `CLAUDE.md` says FT-mode TX and timing changes need explicit maintainer sign-off. Feeding a WWV offset into the slot clock (requirement 4a) is exactly that kind of change. In the fork it must stay **behind a setting that defaults to off**, and it must be listed as a sign-off item when Sean offers the work to KD9TAW.

**`crates/tempo-fast-sys/build.rs` change.** It strips the Windows `\\?\` verbatim prefix and honours `CMAKE_GENERATOR`. Both are sound, small and well commented. It's a candidate fix to offer upstream separately, since it isn't WWV-specific.

**CI baseline:** I can't see the workflow results. Please report:
- pass/fail for `cargo test --workspace`, clippy, `tsc` and `vitest`;
- whether the Windows installer built;
- its size.

---

## 2. The C API: implemented (ABI 1)

ChatGPT's draft (PR #7) was a good shape. The implemented version is in `capi/` (patch 0002), with these changes from the draft:

| Draft | Implemented, and why |
|---|---|
| `utc_valid` covers DECODED | **`utc_valid` only in VERIFIED, or HOLDOVER of a verified time.** A DECODED time goes in `candidate_valid`. A single decode can be confidently wrong, so it must never reach a clock. |
| `carrier_offset_hz` (exact tone) | `carrier_hz` (nominal 1500). The decoder works on the envelope, so a rig error of tens of Hz doesn't matter. Tested at ±12 and −25 Hz. |
| Mono PCM, mixing unspecified | **SSB front end built in:** mix the carrier to baseband, a linear-phase FIR to ±1400 Hz (keeping the 100 Hz code and the 1000/1200 Hz station ticks), then decimate to about 8 kHz. Its group delay is removed from timestamps. Also `AHR_WWV_INPUT_AM_AUDIO`. |
| `capture_latency_seconds` | Kept as `capture_latency_s`, the **calibrated rig + audio delay**, removed from the offset. |
| DUT1/leap/DST raw | Reported only with **`flags_confirmed`**: two independent decodes must agree. That's the fix for today's one-minute DUT1 glitch. |
| (none) | `sample_clock_ppm` with a `sigma` and `settled` flag, `clock_offset_uncertainty_s`, `timing_jitter_s`, `last_problem` text, and the raw minute IDs of the confirmation. |
| (none) | `ahr_wwv_abi_version()`, `ahr_wwv_config_default()`, explicit error codes, and `struct_size` checks on both structs. |

**Tests:**
- **The header compiles as plain C99** with `-Wall -Wextra -pedantic`.
- **Four rig-audio scenarios** (built with a real USB filter): 48 and 44.1 kHz; rig 12 Hz high and 25 Hz low; deep fading; a sound card 150 ppm fast; PC clock +250 ms, −1.7 s and −0.4 s. All reach **VERIFIED, with the PC offset within 0.2 ms** of truth, the right DUT1, and the sound-card ppm measured to 0.1 ppm.
- **Live recordings converted to rig-style USB audio** (5 Nov):
  - 5 MHz evening and 10 MHz: both gave a **correct** DECODED candidate (05:07, 00:19) and no wrong time.
  - Neither reached VERIFIED in the recording.
  - The IQ path verified the 5 MHz recording, because USB audio keeps only one sideband, about 3 dB less time-code signal. See section 5.

**Example program:** `capi/tools/ahr_wwv_wav.c`, about 120 lines of plain C, runs a WAV file through the API.

---

## 3. Integration plan for the fork (phased, each with a gate)

### C1. Native build and Rust wrapper (no UI, no rig): DONE locally, needs CI

The original C1 plan is kept below for reference.

Delivered as `nexus-C1-wwv-crates.patch`, against `feature/wwv-time-tab` at `5d047faa`.

**What it adds:**
- **`crates/ahr-wwv-sys`:**
  - the vendored decoder subset at `vendor/ahr-decoder/` (SPDX headers, `UPSTREAM_COMMIT`, a README);
  - `build.rs` driving CMake with no new crate dependencies, linking the C++ runtime by the same rules as `tempo-fast-sys`, including a static `stdc++` for the MinGW cross build;
  - hand-written `#[repr(C)]` bindings with compile-time layout asserts;
  - a `test-synth` feature for tests only.
- **`crates/wwv`:** the safe wrapper, with a `//!` contract header. `Status::utc` and `clock_offset` are `Some` only when trusted, and a DECODED time is `Status::candidate`.
- **NOTICE entry and README credit; the workspace members list; `Cargo.lock`.**

**Tested here on Linux, Rust 1.97:**
- **`cargo test -p ahr-wwv-sys -p wwv`:** 9 tests.
  - ABI and layout checks.
  - A fresh decoder trusts nothing, and silence never gives a time.
  - Synthetic rig audio reaches VERIFIED with the PC offset within 2 ms, at 48 kHz (+250 ms) and 44.1 kHz (rig off tune, −1.7 s).
  - Deep fading never shows a wrong time, with a positive control that the run did lock.
- `cargo fmt`; `cargo clippy --all-targets -D warnings` clean.

**Still to do in CI (it can't run here):**
- the pinned 1.93.1 toolchain;
- `scripts/gates`;
- `windows-cross` (no MinGW here);
- macOS.


**Native crate:**
- Vendor the decoder at `third_party/ahr-decoder/` (`core/`, `capi/`, `LICENSE`, CMake), with per-file headers, a NOTICE entry and README credit.
- New crate **`crates/ahr-wwv-sys`**, following the `tempo-fast-sys` pattern:
  - its `build.rs` uses `cmake` with `-DAHRWWV_BUILD_TESTS=OFF` and links `ahrwwv_capi` and `ahrwwv_core` statically, plus the C++ runtime (`stdc++` on MinGW/Linux, `c++` on macOS);
  - bindings are hand-written: `#[repr(C)]` structs matching ABI 1, with a const assert on `size_of` and a runtime check of `ahr_wwv_abi_version()`.

**Safe wrapper:**
- New crate **`crates/wwv`**: `Decoder::new(config)`, `push(&[f32], Option<SystemTime>)` and `status() -> WwvStatus`, all owned and safe.
- Add a `//!` contract header saying: RX-only, never touches a clock, and `utc` is only `Some` when trusted.

**Tests:**
- Add a **test-only synthesiser export** to the decoder (`ahr_wwv_test_synth_*`, built only with a CMake option), so Rust tests generate on-air-like audio without large fixtures. Alternatively, a small 12 kHz fixture under `crates/wwv/tests/fixtures/`.
- **Gate:** `cargo test -p wwv` reaches VERIFIED on synthetic audio. Then clippy, fmt, and the Windows GNU cross-build all pass.

### C2. Audio in, and capture time tags

**Decode-only first:** use the engine's 12 kHz modem audio with the **same armed-tap pattern as SSTV RX** (`sstvrx.rs`). That means `wwv_armed`, `take_wwv_audio()`, and a drain thread every 100 ms that runs the decoder off-lock and publishes a `WwvHealth` and status to the engine. That's enough for a working tab showing the time, VERIFIED, the grid and the SNR.

**Then the PC clock offset:**
- In the cpal input callback, record a **time tag**: the cumulative sample count and `SystemTime::now()` for each callback. Write it with a single atomic or SPSC push, so nothing in the realtime path can block.
- The WWV consumer reads the device-rate tee (`RxTap`, like `rxdsp.rs`), with no CAT or rig handle, the same type-level safety argument as `rxtap.rs`. It fits sample count → wall clock by linear regression over the tags to smooth callback jitter, and passes `first_sample_pc_unix_s` to the decoder.
- The constant part, buffering plus rig DSP, is the per-rig **calibration** (`capture_latency_s`).

**Gate:**
- With a mock audio source and a known PC skew, the offset is recovered to ±2 ms.
- The callback adds no allocations and no locks (review plus a test).

### C3. The WWV Time tab, with no clock action

**Tauri commands:** `wwv_start(freq_mhz)`, `wwv_stop()`, `wwv_status()`.

**UI:** a left-rail **WWV Time** tab, following the UI layout contract in `CLAUDE.md`, with all strings through `t()`. It shows:
- Start/Stop;
- UTC and the state chip, with the DECODED candidate greyed;
- WWV or WWVH;
- SNR (0–45 dB, coloured);
- the 60-cell grid with hover text (reuse the SDRangel tooltip wording);
- DUT1, leap second and DST with "checking…";
- the waiting timer and progress line;
- the sample-clock "measuring… / ±".

**Rig control:**
- **On Start:**
  - **refuse** if TX is enabled or PTT is active;
  - save frequency, mode and filter;
  - set USB, the dial at WWV − 1.5 kHz, and the widest SSB filter;
  - **hold the TX-enable latch off** for the session.
- **On Stop, an error or app exit:** restore the saved settings.
- **After a crash:** restore on the next launch from a small "pending restore" record.

**Gate: mock-rig tests prove no PTT call and no TX-enable on every path:**
- Start;
- Stop;
- switching mode while running;
- a CAT failure;
- losing the audio device;
- an app exit;
- a UI reload.

Then a supervised HAMBEAST run with the FTDX101D, logging that TX stayed unkeyed.

### C4. Time to the PC

- **(a) Nexus slot clock, internal.** Add a provenance-aware source to `ClockState`, for example a `ClockSource::Wwv { station, uncertainty_ms }` beside NTP.
  - Publish only VERIFIED offsets, fresh, with uncertainty ≤ 50 ms, and within guard 3.
  - When NTP and WWV are both fresh and disagree by more than 100 ms, show both and **steer by neither**.
  - **Setting default off; Seth's sign-off item.**
- **(b) The Windows clock, via W32Time: re-planned after the spike** (`W32TIME_SPIKE.md`).
  - **The loopback NTP server is dropped.** W32Time's client must use UDP 123 as its *source* port, and its client and server share 123, so a Nexus NTP server on 127.0.0.1:123 is a binding gamble.
  - **The new design is a W32Time input time provider.**
    - A small `nexus-wwv-timeprov.dll` (`TimeProvOpen`/`Command`/`Close`), registered under `W32Time\TimeProviders`.
    - It reads Nexus's latest VERIFIED offset over a local named pipe, and offers W32Time a sample: stratum 1, refid `WWV`/`WWVH`, dispersion ≥ our uncertainty.
    - It offers nothing when Nexus isn't VERIFIED or the sample is stale.
    - W32Time stays the only clock writer.
  - **Install and undo:** an operator-confirmed, elevated "Use WWV as this PC's time source" action does both, saving and restoring the previous settings.
  - **Known limit:** standalone Windows refuses corrections over **15 h** (`MaxPos/NegPhaseCorrection` = 54,000 s). A PC days or years out shows the WWV date and asks the operator to set it once. Nexus never sets it.
- **Gate:** the mock comparison shows the PC clock converging. Then the skeptic test plan below, against the GPS stratum 1 Pi as truth.

### C5. Calibration, docs, and the POC package

- **Per-rig calibration wizard:** with the internet or the GPS Pi as truth, measure the total delay and store it as a profile, for example "FTDX101D + HAMBEAST".
- **A comparison against JTSync and NTP**, with raw logs.
- **A short screen recording.**
- **Tester build** as `1.22.0-wwv1`, following the fork's prerelease-suffix rule. It stays in Sean's fork; Sean contacts KD9TAW himself.

---

## 4. Decisions (Sean approved all three on 6 Nov 2026)

1. **Licence headers: `GPL-3.0-or-later`, approved.** Applied to every decoder source file (decoder patch 0003).
2. **C1–C3 approved** to proceed in the fork. C4(a) stays behind a default-off setting pending Seth.
3. **C4(b) W32Time spike approved: done, read-only.** Nothing ran on HAMBEAST. See `W32TIME_SPIKE.md`.

## 5. A finding for the rig setup

- **USB audio costs about 3 dB** of time-code signal against SDR IQ, because the rig passes only the upper sideband. On strong signals it doesn't matter: you decoded at 28–34 dB on 5 Nov. In deep fading it slows VERIFIED.
- **Worth one live test:** the FTDX101D in **AM mode** with a wide filter, feeding the API's `AM_AUDIO` input. The rig then passes both sidebands.
- **The catch:** the 100 Hz time code then sits at 100 Hz in the rig's audio. Many rigs roll off audio below about 200–300 Hz, which could lose more than AM gains.
- So test it rather than assume it. If it helps, the tab can offer USB or AM.
