# WWV Time integration — Phase 0 architecture orientation
Baseline: upstream `kd9taw/Nexus` tag **v1.22.0**, commit `c41ba30e95495268c0fc54dc33ce0597d05b6554`.
Status: documentation / planning only. No WWV feature code. No upstream outreach.

## Source map
| Concern | Current Nexus source | Integration observation |
|---|---|---|
| Rig audio | `crates/tempo-audio` (`service.rs`, backend/device code), `cpal 0.18` behind `device` feature | Reuse captured mono PCM through a receive-only consumer; confirm actual sample format/rate and existing resampler before writing adapters. |
| CAT and PTT | `crates/tempo-audio/src/service.rs`, `rig`, `slot.rs`; Hamlib/`rigctld` broker | A WWV session must first obtain an exclusive RX-only lease; prohibit any transmit/PTT path, verify PTT is idle, and make Stop/exit restore radio settings. This is a *design requirement*, not a guard implemented in this branch. |
| Mode | `crates/modes` and `crates/tempo-app/src/engine.rs` | Follow the existing receive-only WSPR/APRS pattern, without modifying transmission modes. |
| Tauri | `src-tauri/src/lib.rs` command handlers, exposed through `ui/src/api.ts` | Commands for start/stop/status; no new commands yet. |
| UI | `ui/src` React components and left-side navigation | Future `WWV Time` tab with Start/Stop, station, UTC, 60 cells, progress and SNR. Exact component insertion point to confirm during POC implementation. |
| Native DSP | `libtempo/CMakeLists.txt`, C ABI shims and `crates/*-sys` | Vendored AHR core plus a plain C interface, after peer review of `KL5NE/AHR-WWV-Decoder` PR #7. Keep source attribution and GPL obligations. |

## Clock rules: important safeguards
- `crates/tempo-net/src/sntp.rs` `measure()` gathers multiple SNTP samples. `MIN_AGREEING = 2`, `AGREE_WINDOW_MS = 200`; returns the median of an agreeing cluster. Its sign convention: **PC clock minus UTC** is positive when PC is fast.
- `crates/tempo-app/src/clocksync.rs` keeps a measured offset on a **monotonic-clock** expiry, detects OS clock steps, and refuses steering if |offset| exceeds `MAX_STEER_MS = 60000`.
- `crates/tempo-audio/src/service.rs` uses the steered slot clock for timed transmit/decode. A future WWV source needs an explicit provenance/quality policy, not a fake SNTP server count.
- `crates/tempo-audio/src/clockdiag.rs` explicitly says **“Nexus never calls a clock API.”** Windows clock repair uses `sc` and `w32tm`; only after operator action / elevation, and stands down when third-party time software owns the clock. The source comments warn that bare-Windows behavior still needs bench testing.
- Proposed Windows-clock path: VERIFIED WWV -> uncertainty/age gate -> separately operated local NTP server -> **operator-confirmed**, elevated W32Time configuration; save and restore previous peers/settings. **Not built or proven.** Validate whether W32Time supports and accepts the local loopback peer, timing/discipline behavior, permissions, and rollback before treating this as a solution. Never call SetSystemTime directly.
- Proposed internal Nexus path: VERIFIED WWV offset could steer the slot clock when offline with bounded uncertainty and freshness, independently of the OS clock. Reject DECODED, HOLDOVER beyond limits, stale/uncertain measurements, impossible jumps, and concurrent conflicting sources.
- A 15 MHz USB audio test on the FTDX101D reportedly decoded correctly in SDRangel with carrier at ~1512 Hz (operator's live test). Nexus resampling/detection is **not** yet verified.

## Rig safety — invariants for Phase 1
1. **RX-only**: the mode must never call keying/PTT/transmit commands; reject start when TX is active and isolate / gate TX while its lease is held.
2. Save original frequency, mode, filter and any other modified rig settings; restore on clean Stop and error unwind. On a process crash restoration is not inherently guaranteed; use best-effort recovery on next launch and fail safe with PTT inhibited.
3. Tune to selected broadcast frequency minus **1500 Hz dial offset** in USB, with suitable SSB filter width; detector must use measured carrier-offset calibration rather than assuming exactly 1500 Hz.
4. Signal path is **receive audio**, not IQ. AHR decoder core needs reviewed audio-carrier mixing/resampling strategy before an adapter is coded.
5. Explicitly test no-TX on mode switches, UI crashes, CAT failures, audio device loss, STOP and application exit using a mock rig before any HAMBEAST bench operation.

## Tests / CI baseline plan
- Workspace: `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace`.
- UI: from `ui/`, `npm ci`, `npx tsc -b`, `npx vitest run`.
- Native Windows release baseline: follow upstream `scripts/build-windows-cross.sh` from Linux/WSL2, or native MSYS2 UCRT64 setup. It requires toolchain and native codec/FFTW dependencies. Run on an actual runner; **not validated by this note**.
- Record separate: test totals and failures; OS and toolchain versions; start/end timestamps; Nexus installer/output size; smoke launch on HAMBEAST and confirmed CAT/audio/PTT behavior.
- No testing or change in upstream KD9TAW repo; all experimental work stays under KL5NE fork.

## Remaining Phase 0 gates
- Confirm unmodified Windows build and artifact, not merely successful Rust/TS checks.
- Confirm runtime on HAMBEAST with the FTDX101D while TX remains unkeyed.
- Review proposed `ahr_wwv_c_api_draft.h` in decoder PR #7 with Claude, then add ABI implementation only in a later phase.
