//! End-to-end: synthetic WWV as a rig in USB would deliver it, through the
//! safe wrapper, with capture timestamps from a PC clock that is deliberately
//! wrong. Uses the TEST-ONLY synthesiser (`ahr-wwv-sys/test-synth`).

use ahr_wwv_sys::test_synth as ts;
use wwv::{Config, Decoder, State, Station};

struct Synth(*mut ts::ahr_wwv_test_synth);

impl Synth {
    fn new(
        rate: f64,
        start_utc: f64,
        snr_db: f64,
        carrier_hz: f64,
        dut1: i32,
        fading: f64,
        seed: u32,
    ) -> Self {
        // SAFETY: plain values in; checked for NULL below.
        let p = unsafe {
            ts::ahr_wwv_test_synth_create(rate, start_utc, snr_db, carrier_hz, dut1, fading, seed)
        };
        assert!(!p.is_null());
        Self(p)
    }
    fn fill(&mut self, out: &mut [f32]) {
        // SAFETY: live synth; out is a valid writable slice.
        unsafe { ts::ahr_wwv_test_synth_generate(self.0, out.as_mut_ptr(), out.len()) }
    }
    fn rig_latency(&self) -> f64 {
        // SAFETY: live synth.
        unsafe { ts::ahr_wwv_test_synth_rig_latency_s(self.0) }
    }
    fn path_delay(&self) -> f64 {
        // SAFETY: live synth.
        unsafe { ts::ahr_wwv_test_synth_path_delay_s(self.0) }
    }
}

impl Drop for Synth {
    fn drop(&mut self) {
        // SAFETY: created by ahr_wwv_test_synth_create, freed once.
        unsafe { ts::ahr_wwv_test_synth_destroy(self.0) }
    }
}

struct Run {
    verified_after_s: Option<f64>,
    wrong_utc_polls: u32,
    untrusted_time_polls: u32,
    last: wwv::Status,
}

/// Run `seconds` of rig audio. The PC clock reads `pc_error_s` ahead of true time.
fn run(rate: f64, carrier_tone: f64, snr: f64, fading: f64, pc_error_s: f64, seconds: f64) -> Run {
    // 2026-11-06 22:22:13.7 UTC, the time of the first live VERIFIED of this build.
    let start = 1_793_923_200.0 + 22.0 * 3600.0 + 22.0 * 60.0 + 13.7;
    let mut synth = Synth::new(rate, start, snr, carrier_tone, -1, fading, 7);
    let cfg = Config {
        sample_rate_hz: rate,
        propagation_delay_s: synth.path_delay(),
        // What a calibration against a good clock would have measured for this rig.
        capture_latency_s: synth.rig_latency(),
        ..Config::default()
    };
    let mut d = Decoder::new(&cfg).expect("valid config");
    let block = (rate / 10.0) as usize; // 100 ms, a typical capture callback
    let mut buf = vec![0.0f32; block];
    let mut done = 0usize;
    let mut out = Run {
        verified_after_s: None,
        wrong_utc_polls: 0,
        untrusted_time_polls: 0,
        last: d.status(),
    };
    while (done as f64) < seconds * rate {
        synth.fill(&mut buf);
        // The PC's (wrong) reading of the moment the first sample was captured.
        // The rig's own delay is IN the audio (that sample carries what the
        // antenna heard rig_latency earlier); the config removes it.
        let t_capture = start + done as f64 / rate;
        d.push_unix(&buf, t_capture + pc_error_s).expect("push");
        done += block;
        let s = d.status();
        // A candidate frame must never expose a trusted UTC or a clock offset.
        // This is a safety contract for future Nexus clock integrations.
        if !matches!(s.state, State::Verified | State::Holdover)
            && (s.utc.is_some() || s.clock_offset.is_some())
        {
            out.untrusted_time_polls += 1;
        }
        let t_tx_now = start + done as f64 / rate - synth.path_delay();
        if let Some((utc, _)) = s.utc {
            if (utc - t_tx_now).abs() > 0.5 {
                out.wrong_utc_polls += 1;
            }
        }
        if out.verified_after_s.is_none() && s.state == State::Verified {
            out.verified_after_s = Some(done as f64 / rate);
        }
        out.last = s;
    }
    out
}

#[test]
fn verified_time_and_pc_offset_from_rig_audio() {
    let r = run(48_000.0, 1500.0, 30.0, 0.0, 0.250, 420.0);
    let s = &r.last;
    assert!(
        r.verified_after_s.is_some_and(|t| t < 240.0),
        "verified after {:?}",
        r.verified_after_s
    );
    assert_eq!(r.wrong_utc_polls, 0);
    assert_eq!(r.untrusted_time_polls, 0);
    assert_eq!(s.station, Station::Wwv);
    let m = s.minute.expect("trusted minute");
    assert_eq!((m.year, m.day_of_year), (2026, 310));
    let off = s.clock_offset.expect("clock offset");
    assert!((off.seconds - 0.250).abs() < 0.002, "offset {off:?}");
    let f = s.flags.expect("confirmed flags");
    assert_eq!(f.dut1_tenths, -1);
    assert!(s.candidate.is_none(), "a verified time is not a candidate");
}

#[test]
fn rig_off_tune_and_slow_pc_clock_at_44k1() {
    let r = run(44_100.0, 1512.0, 25.0, 0.0, -1.700, 420.0);
    assert!(
        r.verified_after_s.is_some(),
        "never verified: {:?}",
        r.last.state
    );
    assert_eq!(r.wrong_utc_polls, 0);
    assert_eq!(r.untrusted_time_polls, 0);
    let off = r.last.clock_offset.expect("clock offset");
    assert!((off.seconds + 1.700).abs() < 0.002, "offset {off:?}");
}

#[test]
fn deep_fading_never_shows_a_wrong_time() {
    let r = run(48_000.0, 1475.0, 15.0, 0.95, 0.080, 600.0);
    assert_eq!(r.wrong_utc_polls, 0);
    assert_eq!(r.untrusted_time_polls, 0);
    // Positive control: the run must have got far enough for a wrong time to
    // be possible at all, or "no wrong time" proves nothing.
    assert!(
        r.verified_after_s.is_some() || r.last.state != State::Searching,
        "deep-fading run never locked: {:?}",
        r.last.state
    );
}
