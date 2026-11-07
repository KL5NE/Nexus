//! # wwv — WWV/WWVH time code from receiver audio
//!
//! A safe wrapper over the vendored AHR-Decoder (`ahr-wwv-sys`). Feed it the
//! rig's receive audio while the rig sits in USB with the dial 1.5 kHz below a
//! WWV/WWVH frequency (2.5, 5, 10, 15, 20 or 25 MHz); poll [`Decoder::status`].
//!
//! ## Contract
//! - **RX only.** Nothing here keys PTT, emits audio or talks to a rig.
//! - **Never touches a clock.** It reports a decoded UTC and, when the caller
//!   supplies capture timestamps, how far the PC clock is from UTC. Whether to
//!   act on that is the host's decision (Nexus: `tempo_app::clocksync`, behind
//!   its own guards; the Windows clock only through W32Time).
//! - **Trust is explicit.** [`Status::utc`] and [`Status::clock_offset`] are
//!   `Some` only in [`State::Verified`] (two independent minutes agreed) or in
//!   [`State::Holdover`] of a verified time. A [`State::Decoded`] time is an
//!   unconfirmed [`Status::candidate`]: a single decode can be confidently
//!   wrong under fading (live 5 MHz, 2026-11-05: 05:04 decoded at 05:08), so it
//!   must never be shown as the time or used to set anything.
//! - **One thread.** A [`Decoder`] is `Send` but not `Sync`: call it from the
//!   thread that owns it. Each decoder is independent; there is no global state.

use std::ffi::CStr;
use std::time::{SystemTime, UNIX_EPOCH};

use ahr_wwv_sys as sys;

/// What kind of audio the decoder is fed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Input {
    /// Rig in USB: WWV's carrier is a tone at `carrier_hz` in the audio.
    Ssb,
    /// Rig in AM: the audio is already envelope demodulated.
    Am,
}

/// Decoder configuration. [`Config::default`] is a rig in USB at 48 kHz with
/// the carrier at 1500 Hz and no delay corrections.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Config {
    pub input: Input,
    /// Audio sample rate, >= 8000 Hz.
    pub sample_rate_hz: f64,
    /// SSB: the carrier's tone frequency (dial 1.5 kHz below WWV -> 1500).
    pub carrier_hz: f64,
    /// Transmitter-to-receiver path removed from the clock offset (s); about
    /// 0.013-0.016 from Fort Collins to Alaska, 0 to leave it in.
    pub propagation_delay_s: f64,
    /// Calibrated antenna-to-timestamp delay (rig DSP, audio buffering) removed
    /// from the clock offset (s).
    pub capture_latency_s: f64,
    /// 1-sigma uncertainty of the capture timestamps (s).
    pub timestamp_uncertainty_s: f64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            input: Input::Ssb,
            sample_rate_hz: 48_000.0,
            carrier_hz: 1500.0,
            propagation_delay_s: 0.0,
            capture_latency_s: 0.0,
            timestamp_uncertainty_s: 0.020,
        }
    }
}

/// Why a decoder could not be created or fed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The library was built for a different ABI than these bindings.
    AbiMismatch { library: u32, bindings: u32 },
    /// A configuration value is out of range (see [`Config`]).
    InvalidConfig,
    /// The decoder failed internally and was reset; the next push starts over.
    Internal,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::AbiMismatch { library, bindings } => {
                write!(f, "AHR-Decoder ABI {library}, bindings expect {bindings}")
            }
            Self::InvalidConfig => f.write_str("invalid WWV decoder configuration"),
            Self::Internal => f.write_str("WWV decoder internal error (decoder reset)"),
        }
    }
}

impl std::error::Error for Error {}

/// Decoder progress, from nothing to a trusted time.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Searching,
    SecondLock,
    FrameLock,
    /// One UNCONFIRMED candidate minute: never the time.
    Decoded,
    /// Confirmed by independent minutes: trusted.
    Verified,
    /// Signal lost; a verified time carried on the sample clock.
    Holdover,
}

/// Which station's ticks are heard.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Station {
    Unknown,
    /// Fort Collins, Colorado (1000 Hz ticks).
    Wwv,
    /// Kauai, Hawaii (1200 Hz ticks).
    Wwvh,
    Both,
}

/// One second of the minute being received.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Symbol {
    NoPulse,
    Zero,
    One,
    Marker,
    Unreadable,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Cell {
    /// `None` until this second of the current minute has been read.
    pub symbol: Option<Symbol>,
    pub confidence: f32,
}

/// A date and minute from the time code (UTC).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Minute {
    pub year: i32,
    pub day_of_year: i32,
    pub hour: i32,
    pub minute: i32,
}

/// Station flags confirmed by two independent decodes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Flags {
    /// UT1 - UTC in tenths of a second.
    pub dut1_tenths: i32,
    pub leap_second_warning: bool,
    /// DST in effect at 00:00 UTC today / at 24:00 UTC today.
    pub dst_at_start_of_day: bool,
    pub dst_at_end_of_day: bool,
}

/// PC clock minus UTC (positive = the PC is fast), with path and calibrated
/// capture delays removed.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClockOffset {
    pub seconds: f64,
    pub uncertainty_s: f64,
}

/// The audio sample clock's rate error against UTC (positive = fast).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SampleClock {
    pub ppm: f64,
    pub sigma_ppm: f64,
    /// Precise enough to show as a number (sigma <= 0.5 ppm).
    pub settled: bool,
}

/// Everything the decoder knows, at one moment.
#[derive(Clone, Debug, PartialEq)]
pub struct Status {
    pub state: State,
    pub station: Station,
    /// Trusted UTC (Unix seconds) at the newest sample, and its 1-sigma error.
    pub utc: Option<(f64, f64)>,
    /// The trusted minute (VERIFIED/HOLDOVER).
    pub minute: Option<Minute>,
    /// DECODED only: the unconfirmed candidate minute. Grey it out or hide it.
    pub candidate: Option<Minute>,
    pub clock_offset: Option<ClockOffset>,
    pub flags: Option<Flags>,
    pub holdover_age_s: f64,
    /// Second of the current minute, if known.
    pub second: Option<i32>,
    pub frames_verified: i32,
    pub subcarrier_snr_db: f64,
    pub second_lock_quality: f64,
    pub timing_jitter_s: f64,
    pub frame_confidence: f64,
    pub sample_clock: Option<SampleClock>,
    pub seconds_processed: u64,
    /// Why the last minute was rejected (English, for a tooltip/log).
    pub last_problem: String,
    pub cells: [Cell; 60],
}

impl Status {
    fn from_raw(s: &sys::ahr_wwv_status) -> Self {
        let minute = Minute {
            year: s.year,
            day_of_year: s.day_of_year,
            hour: s.hour,
            minute: s.minute,
        };
        let state = match s.state {
            sys::AHR_WWV_SECOND_LOCK => State::SecondLock,
            sys::AHR_WWV_FRAME_LOCK => State::FrameLock,
            sys::AHR_WWV_DECODED => State::Decoded,
            sys::AHR_WWV_VERIFIED => State::Verified,
            sys::AHR_WWV_HOLDOVER => State::Holdover,
            _ => State::Searching,
        };
        let station = match s.station {
            sys::AHR_WWV_STATION_WWV => Station::Wwv,
            sys::AHR_WWV_STATION_WWVH => Station::Wwvh,
            sys::AHR_WWV_STATION_BOTH => Station::Both,
            _ => Station::Unknown,
        };
        let mut cells = [Cell {
            symbol: None,
            confidence: 0.0,
        }; 60];
        for (cell, raw) in cells.iter_mut().zip(s.cells.iter()) {
            cell.confidence = raw.confidence;
            cell.symbol = (raw.received != 0).then_some(match raw.symbol {
                sys::AHR_WWV_SYM_NONE => Symbol::NoPulse,
                sys::AHR_WWV_SYM_ZERO => Symbol::Zero,
                sys::AHR_WWV_SYM_ONE => Symbol::One,
                sys::AHR_WWV_SYM_MARKER => Symbol::Marker,
                _ => Symbol::Unreadable,
            });
        }
        // SAFETY: the library NUL-terminates last_problem within its fixed buffer.
        let last_problem = unsafe { CStr::from_ptr(s.last_problem.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        Self {
            state,
            station,
            utc: (s.utc_valid != 0).then_some((s.utc_unix_s, s.time_uncertainty_s)),
            minute: (s.utc_valid != 0).then_some(minute),
            candidate: (s.candidate_valid != 0).then_some(minute),
            clock_offset: (s.clock_offset_valid != 0).then_some(ClockOffset {
                seconds: s.clock_offset_s,
                uncertainty_s: s.clock_offset_uncertainty_s,
            }),
            flags: (s.flags_confirmed != 0).then_some(Flags {
                dut1_tenths: s.dut1_tenths,
                leap_second_warning: s.leap_second_warning != 0,
                dst_at_start_of_day: s.dst1 != 0,
                dst_at_end_of_day: s.dst2 != 0,
            }),
            holdover_age_s: s.holdover_age_s,
            second: (s.frame_position >= 0).then_some(s.frame_position),
            frames_verified: s.frames_verified,
            subcarrier_snr_db: s.subcarrier_snr_db,
            second_lock_quality: s.second_lock_quality,
            timing_jitter_s: s.timing_jitter_s,
            frame_confidence: s.frame_confidence,
            sample_clock: (s.sample_clock_ppm_sigma > 0.0).then_some(SampleClock {
                ppm: s.sample_clock_ppm,
                sigma_ppm: s.sample_clock_ppm_sigma,
                settled: s.sample_clock_settled != 0,
            }),
            seconds_processed: s.seconds_processed,
            last_problem,
            cells,
        }
    }
}

/// A WWV/WWVH decoder. Dropping it frees the native decoder.
pub struct Decoder {
    handle: std::ptr::NonNull<sys::ahr_wwv_handle>,
    // Not Sync: the native handle must not be used from two threads at once.
    _not_sync: std::marker::PhantomData<std::cell::Cell<()>>,
}

// SAFETY: the handle owns independent heap state with no thread affinity; moving
// it to another thread is fine as long as calls do not overlap, which `&mut self`
// on every mutating method and the !Sync marker guarantee.
unsafe impl Send for Decoder {}

impl Decoder {
    pub fn new(config: &Config) -> Result<Self, Error> {
        // SAFETY: pure function.
        let library = unsafe { sys::ahr_wwv_abi_version() };
        if library != sys::AHR_WWV_ABI_VERSION {
            return Err(Error::AbiMismatch {
                library,
                bindings: sys::AHR_WWV_ABI_VERSION,
            });
        }
        let raw = sys::ahr_wwv_config {
            struct_size: std::mem::size_of::<sys::ahr_wwv_config>() as u32,
            input: match config.input {
                Input::Ssb => sys::AHR_WWV_INPUT_SSB_AUDIO,
                Input::Am => sys::AHR_WWV_INPUT_AM_AUDIO,
            },
            sample_rate_hz: config.sample_rate_hz,
            carrier_hz: config.carrier_hz,
            propagation_delay_s: config.propagation_delay_s,
            capture_latency_s: config.capture_latency_s,
            timestamp_uncertainty_s: config.timestamp_uncertainty_s,
        };
        // SAFETY: raw is a fully initialised config of the declared size.
        let h = unsafe { sys::ahr_wwv_create(&raw) };
        std::ptr::NonNull::new(h)
            .map(|handle| Self {
                handle,
                _not_sync: std::marker::PhantomData,
            })
            .ok_or(Error::InvalidConfig)
    }

    /// Feed audio. `captured_at` is the PC clock time at which the FIRST of
    /// these samples was captured (not when the callback ran), or `None`; it is
    /// needed only for [`Status::clock_offset`].
    pub fn push(&mut self, samples: &[f32], captured_at: Option<SystemTime>) -> Result<(), Error> {
        let ts = captured_at
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map_or(f64::NAN, |d| d.as_secs_f64());
        self.push_unix(samples, ts)
    }

    /// As [`Self::push`], with the capture time as Unix seconds (NaN = unknown).
    pub fn push_unix(&mut self, samples: &[f32], captured_at_unix_s: f64) -> Result<(), Error> {
        // SAFETY: handle is live; the slice pointer/length pair is valid for reads.
        let rc = unsafe {
            sys::ahr_wwv_push_audio(
                self.handle.as_ptr(),
                samples.as_ptr(),
                samples.len(),
                captured_at_unix_s,
            )
        };
        match rc {
            sys::AHR_WWV_OK => Ok(()),
            _ => Err(Error::Internal),
        }
    }

    pub fn status(&mut self) -> Status {
        let mut raw = sys::ahr_wwv_status::zeroed();
        // SAFETY: handle is live; raw is a correctly sized status. poll cannot fail
        // with both pointers valid and struct_size set.
        let rc = unsafe { sys::ahr_wwv_poll(self.handle.as_ptr(), &mut raw) };
        debug_assert_eq!(rc, sys::AHR_WWV_OK);
        Status::from_raw(&raw)
    }

    /// Start over (after retuning), keeping the configuration.
    pub fn reset(&mut self) {
        // SAFETY: handle is live.
        unsafe { sys::ahr_wwv_reset(self.handle.as_ptr()) };
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        // SAFETY: handle came from ahr_wwv_create and is dropped exactly once.
        unsafe { sys::ahr_wwv_destroy(self.handle.as_ptr()) };
    }
}

impl State {
    /// Display name as the library spells it (English; the UI translates).
    pub fn name(self) -> &'static str {
        let code = match self {
            Self::Searching => sys::AHR_WWV_SEARCHING,
            Self::SecondLock => sys::AHR_WWV_SECOND_LOCK,
            Self::FrameLock => sys::AHR_WWV_FRAME_LOCK,
            Self::Decoded => sys::AHR_WWV_DECODED,
            Self::Verified => sys::AHR_WWV_VERIFIED,
            Self::Holdover => sys::AHR_WWV_HOLDOVER,
        };
        // SAFETY: returns a static NUL-terminated string.
        unsafe { CStr::from_ptr(sys::ahr_wwv_state_name(code)) }
            .to_str()
            .unwrap_or("?")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_a_sample_rate_the_decoder_cannot_use() {
        let cfg = Config {
            sample_rate_hz: 4000.0,
            ..Config::default()
        };
        assert_eq!(Decoder::new(&cfg).err(), Some(Error::InvalidConfig));
    }

    #[test]
    fn a_fresh_decoder_trusts_nothing() {
        let mut d = Decoder::new(&Config::default()).expect("default config is valid");
        let s = d.status();
        assert_eq!(s.state, State::Searching);
        assert!(s.utc.is_none() && s.candidate.is_none() && s.clock_offset.is_none());
        assert!(s.cells.iter().all(|c| c.symbol.is_none()));
    }

    #[test]
    fn silence_never_produces_a_time() {
        let mut d = Decoder::new(&Config::default()).expect("default config is valid");
        let block = vec![0.0f32; 4800];
        for _ in 0..(10 * 120) {
            d.push(&block, None).expect("push");
        }
        let s = d.status();
        assert!(s.utc.is_none() && s.candidate.is_none(), "{s:?}");
        assert_eq!(State::Verified.name(), "Verified");
    }
}
