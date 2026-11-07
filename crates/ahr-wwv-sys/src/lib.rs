//! Raw FFI bindings to the vendored AHR-Decoder plain C API (ABI 1).
//!
//! `vendor/ahr-decoder/capi/include/ahr_wwv.h` is the authoritative contract;
//! these declarations mirror it by hand (no bindgen, so no libclang build
//! dependency). Use the safe wrapper in the `wwv` crate rather than these.
//!
//! # What the decoder is, and is not, allowed to do
//! It turns receiver audio into a decoded WWV/WWVH time and, given capture
//! timestamps, a measured PC-clock offset. It **never sets, steers or reads-and-
//! adjusts any clock**: that stays with the host's own policy (in Nexus,
//! `tempo_app::clocksync` and W32Time). It holds no global state: each handle
//! is independent, so unlike `tempo-fast-sys` there is no process-wide lock.
//!
//! # ABI checks
//! The struct layouts are asserted at compile time below, and the wrapper must
//! compare [`ahr_wwv_abi_version`] with [`AHR_WWV_ABI_VERSION`] at run time, so
//! a vendored header update that changes a struct cannot link silently.
#![allow(non_camel_case_types)]

use std::os::raw::c_char;

pub const AHR_WWV_ABI_VERSION: u32 = 1;
pub const AHR_WWV_FRAME_CELLS: usize = 60;
pub const AHR_WWV_PROBLEM_LEN: usize = 96;

pub const AHR_WWV_OK: i32 = 0;
pub const AHR_WWV_ERR_NULL: i32 = -1;
pub const AHR_WWV_ERR_STRUCT_SIZE: i32 = -2;
pub const AHR_WWV_ERR_CONFIG: i32 = -3;
pub const AHR_WWV_ERR_INTERNAL: i32 = -4;

pub const AHR_WWV_SEARCHING: i32 = 0;
pub const AHR_WWV_SECOND_LOCK: i32 = 1;
pub const AHR_WWV_FRAME_LOCK: i32 = 2;
pub const AHR_WWV_DECODED: i32 = 3;
pub const AHR_WWV_VERIFIED: i32 = 4;
pub const AHR_WWV_HOLDOVER: i32 = 5;

pub const AHR_WWV_STATION_UNKNOWN: i32 = 0;
pub const AHR_WWV_STATION_WWV: i32 = 1;
pub const AHR_WWV_STATION_WWVH: i32 = 2;
pub const AHR_WWV_STATION_BOTH: i32 = 3;

pub const AHR_WWV_SYM_NONE: u8 = 0;
pub const AHR_WWV_SYM_ZERO: u8 = 1;
pub const AHR_WWV_SYM_ONE: u8 = 2;
pub const AHR_WWV_SYM_MARKER: u8 = 3;
pub const AHR_WWV_SYM_UNKNOWN: u8 = 4;

pub const AHR_WWV_INPUT_SSB_AUDIO: u32 = 0;
pub const AHR_WWV_INPUT_AM_AUDIO: u32 = 1;

/// Opaque decoder handle.
#[repr(C)]
pub struct ahr_wwv_handle {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ahr_wwv_config {
    pub struct_size: u32,
    pub input: u32,
    pub sample_rate_hz: f64,
    pub carrier_hz: f64,
    pub propagation_delay_s: f64,
    pub capture_latency_s: f64,
    pub timestamp_uncertainty_s: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ahr_wwv_cell {
    pub symbol: u8,
    pub received: u8,
    pub reserved: u16,
    pub confidence: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ahr_wwv_status {
    pub struct_size: u32,
    pub abi_version: u32,
    pub state: i32,
    pub station: i32,

    pub utc_valid: u8,
    pub candidate_valid: u8,
    pub clock_offset_valid: u8,
    pub flags_confirmed: u8,
    pub leap_second_warning: u8,
    pub dst1: u8,
    pub dst2: u8,
    pub sample_clock_settled: u8,

    pub utc_unix_s: f64,
    pub time_uncertainty_s: f64,
    pub clock_offset_s: f64,
    pub clock_offset_uncertainty_s: f64,
    pub holdover_age_s: f64,

    pub year: i32,
    pub day_of_year: i32,
    pub hour: i32,
    pub minute: i32,
    pub dut1_tenths: i32,
    pub frame_position: i32,
    pub frames_verified: i32,
    pub reserved0: i32,

    pub subcarrier_snr_db: f64,
    pub second_lock_quality: f64,
    pub timing_jitter_s: f64,
    pub frame_confidence: f64,
    pub sample_clock_ppm: f64,
    pub sample_clock_ppm_sigma: f64,

    pub seconds_processed: u64,
    pub confirmation_previous_last_id: u64,
    pub confirmation_current_first_id: u64,

    pub last_problem: [c_char; AHR_WWV_PROBLEM_LEN],
    pub cells: [ahr_wwv_cell; AHR_WWV_FRAME_CELLS],
}

impl ahr_wwv_status {
    /// A zeroed status with `struct_size` set, ready for [`ahr_wwv_poll`].
    pub fn zeroed() -> Self {
        // SAFETY: every field is a plain integer, float or array of them; all-zero is valid.
        let mut s: Self = unsafe { std::mem::zeroed() };
        s.struct_size = std::mem::size_of::<Self>() as u32;
        s
    }
}

// Layout guards: these sizes are what the C header produces on every supported
// target (all fields naturally aligned, no implementation-defined padding).
const _: () = assert!(std::mem::size_of::<ahr_wwv_config>() == 48);
const _: () = assert!(std::mem::size_of::<ahr_wwv_cell>() == 8);
const _: () = assert!(
    std::mem::size_of::<ahr_wwv_status>()
        == 16 + 8 + 5 * 8 + 8 * 4 + 6 * 8 + 3 * 8 + AHR_WWV_PROBLEM_LEN + 8 * AHR_WWV_FRAME_CELLS
);

extern "C" {
    pub fn ahr_wwv_abi_version() -> u32;
    pub fn ahr_wwv_config_default(config: *mut ahr_wwv_config);
    pub fn ahr_wwv_create(config: *const ahr_wwv_config) -> *mut ahr_wwv_handle;
    pub fn ahr_wwv_destroy(handle: *mut ahr_wwv_handle);
    pub fn ahr_wwv_push_audio(
        handle: *mut ahr_wwv_handle,
        samples: *const f32,
        count: usize,
        first_sample_pc_unix_s: f64,
    ) -> i32;
    pub fn ahr_wwv_poll(handle: *mut ahr_wwv_handle, status: *mut ahr_wwv_status) -> i32;
    pub fn ahr_wwv_reset(handle: *mut ahr_wwv_handle) -> i32;
    pub fn ahr_wwv_state_name(state: i32) -> *const c_char;
    pub fn ahr_wwv_station_name(station: i32) -> *const c_char;
}

/// TEST ONLY: synthetic WWV received by a rig in USB (`test-synth` feature).
#[cfg(feature = "test-synth")]
pub mod test_synth {
    #[repr(C)]
    pub struct ahr_wwv_test_synth {
        _private: [u8; 0],
    }
    extern "C" {
        pub fn ahr_wwv_test_synth_create(
            sample_rate_hz: f64,
            start_utc_unix_s: f64,
            snr_db: f64,
            carrier_hz: f64,
            dut1_tenths: i32,
            fading: f64,
            seed: u32,
        ) -> *mut ahr_wwv_test_synth;
        pub fn ahr_wwv_test_synth_generate(s: *mut ahr_wwv_test_synth, out: *mut f32, count: usize);
        pub fn ahr_wwv_test_synth_rig_latency_s(s: *const ahr_wwv_test_synth) -> f64;
        pub fn ahr_wwv_test_synth_path_delay_s(s: *const ahr_wwv_test_synth) -> f64;
        pub fn ahr_wwv_test_synth_destroy(s: *mut ahr_wwv_test_synth);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn library_reports_the_abi_these_bindings_were_written_for() {
        // SAFETY: a pure function with no arguments.
        assert_eq!(unsafe { ahr_wwv_abi_version() }, AHR_WWV_ABI_VERSION);
    }

    #[test]
    fn default_config_matches_the_c_struct_size() {
        let mut c = std::mem::MaybeUninit::<ahr_wwv_config>::uninit();
        // SAFETY: the callee fully initialises the struct it is given.
        let c = unsafe {
            ahr_wwv_config_default(c.as_mut_ptr());
            c.assume_init()
        };
        assert_eq!(
            c.struct_size as usize,
            std::mem::size_of::<ahr_wwv_config>()
        );
        assert_eq!(c.input, AHR_WWV_INPUT_SSB_AUDIO);
    }

    #[test]
    fn poll_accepts_this_status_layout() {
        // SAFETY: NULL config means defaults; the handle is destroyed below.
        let h = unsafe { ahr_wwv_create(std::ptr::null()) };
        assert!(!h.is_null());
        let mut s = ahr_wwv_status::zeroed();
        // SAFETY: h is live and s is a correctly sized status.
        let rc = unsafe { ahr_wwv_poll(h, &mut s) };
        // SAFETY: h came from ahr_wwv_create and is not used again.
        unsafe { ahr_wwv_destroy(h) };
        assert_eq!(rc, AHR_WWV_OK);
        assert_eq!(s.abi_version, AHR_WWV_ABI_VERSION);
        assert_eq!(s.state, AHR_WWV_SEARCHING);
        assert_eq!(s.utc_valid, 0);
    }
}
