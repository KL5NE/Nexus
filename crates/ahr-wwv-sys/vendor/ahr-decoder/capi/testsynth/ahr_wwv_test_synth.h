/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors */
/* TEST ONLY: synthetic WWV received by a rig in USB, as real audio. For host
 * test suites (Nexus' Rust crates) that need on-air-like input without large
 * fixtures. Built only with -DAHRWWV_CAPI_TEST_SYNTH=ON; never ship it. */
#ifndef AHR_WWV_TEST_SYNTH_H
#define AHR_WWV_TEST_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ahr_wwv_test_synth ahr_wwv_test_synth;

/* start_utc_unix_s: true UTC at the antenna for the first sample.
 * snr_db: carrier to noise in 3 kHz. carrier_hz: tone of the carrier in the
 * audio (1500 for a dial 1.5 kHz low). fading: 0 steady, up to 1 deep. */
ahr_wwv_test_synth *ahr_wwv_test_synth_create(double sample_rate_hz, double start_utc_unix_s, double snr_db,
                                              double carrier_hz, int32_t dut1_tenths, double fading, uint32_t seed);
/* Fill out[0..count) with the next audio samples. */
void ahr_wwv_test_synth_generate(ahr_wwv_test_synth *s, float *out, size_t count);
/* The synthetic rig's own delay (its USB filter) plus the WWV-to-receiver path
 * built into the synthesis: what a calibration would measure. */
double ahr_wwv_test_synth_rig_latency_s(const ahr_wwv_test_synth *s);
double ahr_wwv_test_synth_path_delay_s(const ahr_wwv_test_synth *s);
void ahr_wwv_test_synth_destroy(ahr_wwv_test_synth *s);

#ifdef __cplusplus
}
#endif
#endif
