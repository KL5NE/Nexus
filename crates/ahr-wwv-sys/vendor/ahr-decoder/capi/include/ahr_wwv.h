/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors */
/* AHR-Decoder (WWV/WWVH time code) - plain C API, ABI version 1.
 *
 * For hosts that are not C++ (Nexus: Rust through a -sys crate). Wraps the
 * portable C++17 decoder core; no Qt, no SDRangel, no exceptions or C++
 * types cross this boundary. GPL-3.0-or-later; see LICENSE.
 *
 * WHAT IT DOES: feed it receiver audio, poll its status. It reports what it
 * decoded and, if you give it capture timestamps, how far the PC clock is
 * from UTC. It NEVER sets or reads-and-steers any clock itself; what to do
 * with the offset is the host's decision (Nexus: a time source behind its
 * own policy, W32Time remains the only clock writer).
 *
 * INPUT: mono float PCM (any scale), sample rate >= 8000 Hz.
 *   AHR_WWV_INPUT_SSB_AUDIO (default): a rig in USB with the dial below a
 *     WWV frequency, so the carrier appears as a tone at carrier_hz (1500 Hz
 *     for a dial 1.5 kHz low). A few tens of Hz of rig tuning error do not
 *     matter: the decoder works on the signal envelope.
 *   AHR_WWV_INPUT_AM_AUDIO: already envelope (AM) demodulated audio.
 *
 * TIME: Unix UTC seconds. Clock offset is PC minus UTC: positive = PC fast.
 *
 * TRUST: utc_valid is set ONLY in VERIFIED (two independent minutes agree)
 * or in HOLDOVER of a verified time. In DECODED, candidate_valid is set and
 * hour/minute/date describe an UNCONFIRMED candidate: show it greyed or not
 * at all, never as the time, never to set a clock.
 *
 * THREADING: one handle per thread; calls on a handle must not overlap.
 * push_audio does the decoding work synchronously (about 1-2 % of one core
 * at 48 kHz).
 */
#ifndef AHR_WWV_H
#define AHR_WWV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AHR_WWV_ABI_VERSION 1u
#define AHR_WWV_FRAME_CELLS 60
#define AHR_WWV_PROBLEM_LEN 96

typedef struct ahr_wwv_handle ahr_wwv_handle;

/* Return codes */
#define AHR_WWV_OK 0
#define AHR_WWV_ERR_NULL (-1)        /* NULL handle or pointer */
#define AHR_WWV_ERR_STRUCT_SIZE (-2) /* struct_size too small: caller built against an older header */
#define AHR_WWV_ERR_CONFIG (-3)      /* invalid configuration value */
#define AHR_WWV_ERR_INTERNAL (-4)    /* unexpected failure inside the decoder; the handle is reset */

enum ahr_wwv_state {
    AHR_WWV_SEARCHING = 0,   /* no second lock */
    AHR_WWV_SECOND_LOCK = 1, /* seconds found, minute not yet */
    AHR_WWV_FRAME_LOCK = 2,  /* minute frame aligned, reading it */
    AHR_WWV_DECODED = 3,     /* one UNCONFIRMED candidate time */
    AHR_WWV_VERIFIED = 4,    /* confirmed by independent minutes: trusted */
    AHR_WWV_HOLDOVER = 5     /* signal lost; time carried on the sample clock */
};

enum ahr_wwv_station {
    AHR_WWV_STATION_UNKNOWN = 0,
    AHR_WWV_STATION_WWV = 1,  /* Fort Collins, 1000 Hz ticks */
    AHR_WWV_STATION_WWVH = 2, /* Kauai, 1200 Hz ticks */
    AHR_WWV_STATION_BOTH = 3
};

enum ahr_wwv_symbol {
    AHR_WWV_SYM_NONE = 0,   /* no pulse (second 0) */
    AHR_WWV_SYM_ZERO = 1,   /* 200 ms pulse */
    AHR_WWV_SYM_ONE = 2,    /* 500 ms pulse */
    AHR_WWV_SYM_MARKER = 3, /* 800 ms position marker */
    AHR_WWV_SYM_UNKNOWN = 4 /* unreadable */
};

enum ahr_wwv_input {
    AHR_WWV_INPUT_SSB_AUDIO = 0,
    AHR_WWV_INPUT_AM_AUDIO = 1
};

typedef struct ahr_wwv_config {
    uint32_t struct_size;   /* = sizeof(ahr_wwv_config) */
    uint32_t input;         /* enum ahr_wwv_input */
    double sample_rate_hz;  /* >= 8000 */
    double carrier_hz;      /* SSB input: tone frequency of the carrier, default 1500 */
    /* Removed from the PC offset: transmitter-to-receiver path, about 13-16 ms
     * from Fort Collins to Alaska (0 = do not correct). */
    double propagation_delay_s;
    /* Fixed delay between the antenna and the samples' capture timestamps that
     * the host knows of (rig DSP, measured by calibration). Removed from the
     * PC offset. 0 if unknown. */
    double capture_latency_s;
    /* 1-sigma uncertainty of the host's capture timestamps (audio buffering).
     * Included in clock_offset_uncertainty_s. Default 0.020. */
    double timestamp_uncertainty_s;
} ahr_wwv_config;

/* Fill a config with defaults (SSB audio, 48 kHz, carrier 1500 Hz). */
void ahr_wwv_config_default(ahr_wwv_config *config);

typedef struct ahr_wwv_cell {
    uint8_t symbol;   /* enum ahr_wwv_symbol */
    uint8_t received; /* 1 once this second of the current minute was read */
    uint16_t reserved;
    float confidence; /* 0..1 */
} ahr_wwv_cell;

typedef struct ahr_wwv_status {
    uint32_t struct_size; /* caller sets = sizeof(ahr_wwv_status) before poll */
    uint32_t abi_version; /* filled: AHR_WWV_ABI_VERSION */
    int32_t state;        /* enum ahr_wwv_state */
    int32_t station;      /* enum ahr_wwv_station */

    /* Trust flags (see TRUST above) */
    uint8_t utc_valid;          /* utc_unix_s is trusted */
    uint8_t candidate_valid;    /* DECODED: date/time fields hold an unconfirmed candidate */
    uint8_t clock_offset_valid; /* clock_offset_s is trusted (needs timestamps + VERIFIED) */
    uint8_t flags_confirmed;    /* dut1/leap/dst confirmed by two independent decodes */
    uint8_t leap_second_warning;
    uint8_t dst1;               /* DST in effect at 00:00 UTC today */
    uint8_t dst2;               /* DST in effect at 24:00 UTC today */
    uint8_t sample_clock_settled; /* sample_clock_ppm is shown-worthy (sigma <= 0.5 ppm) */

    double utc_unix_s;                 /* UTC at the newest sample pushed */
    double time_uncertainty_s;         /* 1 sigma of utc_unix_s, excl. host timestamps */
    double clock_offset_s;             /* PC minus UTC, path and capture latency removed */
    double clock_offset_uncertainty_s; /* 1 sigma */
    double holdover_age_s;             /* seconds since the last good minute (HOLDOVER) */

    /* Date/time of the most recent accepted minute (VERIFIED, HOLDOVER or a
     * DECODED candidate). Meaningful only if utc_valid or candidate_valid. */
    int32_t year;
    int32_t day_of_year;
    int32_t hour;
    int32_t minute;
    int32_t dut1_tenths;  /* UT1-UTC in 0.1 s; meaningful if flags_confirmed */
    int32_t frame_position; /* second of the current minute, -1 if unknown */
    int32_t frames_verified;
    int32_t reserved0;

    /* Signal */
    double subcarrier_snr_db; /* 100 Hz time code SNR; > 15 dB is good */
    double second_lock_quality; /* 0..1 */
    double timing_jitter_s;     /* 1 sigma of tick timing */
    double frame_confidence;    /* 0..1, last decode attempt */
    double sample_clock_ppm;    /* audio/sample clock error vs UTC, + = fast */
    double sample_clock_ppm_sigma;

    uint64_t seconds_processed;
    /* Raw minute IDs of the independent confirmation (0 = none yet). */
    uint64_t confirmation_previous_last_id;
    uint64_t confirmation_current_first_id;

    char last_problem[AHR_WWV_PROBLEM_LEN]; /* why the last minute was rejected, NUL-terminated */
    ahr_wwv_cell cells[AHR_WWV_FRAME_CELLS]; /* the minute being received, by second */
} ahr_wwv_status;

/* Library ABI version, for a runtime check against AHR_WWV_ABI_VERSION. */
uint32_t ahr_wwv_abi_version(void);

/* NULL on invalid config or allocation failure. config may be NULL (defaults). */
ahr_wwv_handle *ahr_wwv_create(const ahr_wwv_config *config);
void ahr_wwv_destroy(ahr_wwv_handle *handle); /* NULL is ignored */

/* Push count samples. first_sample_pc_unix_s: the PC clock time at which the
 * first of these samples was captured (callback time minus buffered audio),
 * or NaN if unknown; needed only for the clock offset. */
int32_t ahr_wwv_push_audio(ahr_wwv_handle *handle, const float *samples, size_t count,
                           double first_sample_pc_unix_s);

/* Fill *status (set status->struct_size first). */
int32_t ahr_wwv_poll(ahr_wwv_handle *handle, ahr_wwv_status *status);

/* Start over (e.g. after retuning); keeps the configuration. */
int32_t ahr_wwv_reset(ahr_wwv_handle *handle);

/* Static, NUL-terminated names for display/logging. */
const char *ahr_wwv_state_name(int32_t state);
const char *ahr_wwv_station_name(int32_t station);

#ifdef __cplusplus
}
#endif

#endif /* AHR_WWV_H */
