// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
// AHR-Decoder plain C API (see capi/include/ahr_wwv.h). GPL-3.0-or-later; see LICENSE.
//
// SSB audio path: the rig's USB audio carries WWV's carrier as a tone at
// carrier_hz with the upper sideband above it. It is mixed down so the
// carrier sits near 0 Hz, low-pass filtered (linear-phase FIR, passing the
// 100 Hz time code and the 1000/1200 Hz station ticks) and decimated to
// about 8 kHz complex baseband, which is the decoder core's IQ input. The
// decoder works on the envelope, so a rig tuning error of tens of Hz is
// harmless. The FIR's group delay is removed from the capture timestamps so
// the PC clock offset is not biased by it.
#include "ahr_wwv.h"

#include "ahrwwv/decoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct SsbFrontEnd {
    double fsIn = 48000.0, fsOut = 8000.0;
    int decim = 6;
    std::vector<float> taps;          // real low-pass, odd length
    std::vector<std::complex<float>> hist; // circular history of mixed samples
    size_t pos = 0;
    int phaseCount = 0;               // input samples since the last output
    double nco = 0.0, ncoStep = 0.0;  // mixer phase (radians)
    double groupDelaySamples = 0.0;

    void init(double fs, double carrierHz)
    {
        fsIn = fs;
        decim = std::max(1, int(std::floor(fs / 8000.0)));
        fsOut = fs / decim;
        // Pass +/-1400 Hz (100 Hz code, 1000/1200 Hz ticks), stop by ~0.45 fsOut.
        const double fc = 1400.0, stop = std::max(fc + 400.0, 0.45 * fsOut);
        const double tw = (stop - fc) / fs;
        int n = int(std::ceil(4.0 / tw)) | 1;
        n = std::clamp(n, 31, 2047) | 1;
        taps.assign(size_t(n), 0.0f);
        const double cut = 0.5 * (fc + stop) / fs; // normalised cut-off (cycles/sample)
        double sum = 0;
        for (int i = 0; i < n; i++) {
            double m = i - (n - 1) / 2.0;
            double sinc = m == 0 ? 2 * cut : std::sin(2 * kPi * cut * m) / (kPi * m);
            double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1)); // Blackman
            taps[size_t(i)] = float(sinc * w);
            sum += sinc * w;
        }
        for (auto& t : taps) t = float(t / sum);
        hist.assign(taps.size(), {0.0f, 0.0f});
        pos = 0;
        phaseCount = 0;
        nco = 0.0;
        ncoStep = -2 * kPi * carrierHz / fs;
        groupDelaySamples = (n - 1) / 2.0;
    }

    // Mix, filter and decimate. out receives complex baseband at fsOut;
    // outInputIndex receives, for each output, the index (within this block)
    // of the input sample it was computed at.
    void process(const float* x, size_t count, std::vector<std::complex<float>>& out, std::vector<size_t>& outInputIndex)
    {
        out.clear();
        outInputIndex.clear();
        const size_t n = taps.size();
        for (size_t i = 0; i < count; i++) {
            hist[pos] = std::complex<float>(float(x[i] * std::cos(nco)), float(x[i] * std::sin(nco)));
            nco += ncoStep;
            if (nco < -kPi) nco += 2 * kPi;
            pos = (pos + 1) % n;
            if (++phaseCount >= decim) {
                phaseCount = 0;
                std::complex<float> acc(0.0f, 0.0f);
                size_t p = pos; // oldest sample
                for (size_t k = 0; k < n; k++) {
                    acc += hist[p] * taps[k];
                    if (++p == n) p = 0;
                }
                out.push_back(acc);
                outInputIndex.push_back(i);
            }
        }
    }
};

uint8_t symCode(ahrwwv::Symbol s)
{
    switch (s) {
    case ahrwwv::Symbol::None: return AHR_WWV_SYM_NONE;
    case ahrwwv::Symbol::Zero: return AHR_WWV_SYM_ZERO;
    case ahrwwv::Symbol::One: return AHR_WWV_SYM_ONE;
    case ahrwwv::Symbol::Marker: return AHR_WWV_SYM_MARKER;
    default: return AHR_WWV_SYM_UNKNOWN;
    }
}

int32_t stateCode(ahrwwv::DecoderState s)
{
    switch (s) {
    case ahrwwv::DecoderState::Searching: return AHR_WWV_SEARCHING;
    case ahrwwv::DecoderState::SecondLock: return AHR_WWV_SECOND_LOCK;
    case ahrwwv::DecoderState::FrameLock: return AHR_WWV_FRAME_LOCK;
    case ahrwwv::DecoderState::Decoded: return AHR_WWV_DECODED;
    case ahrwwv::DecoderState::Verified: return AHR_WWV_VERIFIED;
    case ahrwwv::DecoderState::Holdover: return AHR_WWV_HOLDOVER;
    }
    return AHR_WWV_SEARCHING;
}

int32_t stationCode(ahrwwv::StationId s)
{
    switch (s) {
    case ahrwwv::StationId::WWV: return AHR_WWV_STATION_WWV;
    case ahrwwv::StationId::WWVH: return AHR_WWV_STATION_WWVH;
    case ahrwwv::StationId::Both: return AHR_WWV_STATION_BOTH;
    default: return AHR_WWV_STATION_UNKNOWN;
    }
}

bool validConfig(const ahr_wwv_config& c)
{
    if (c.input != AHR_WWV_INPUT_SSB_AUDIO && c.input != AHR_WWV_INPUT_AM_AUDIO) return false;
    if (!(c.sample_rate_hz >= 8000.0 && c.sample_rate_hz <= 768000.0)) return false;
    if (c.input == AHR_WWV_INPUT_SSB_AUDIO && !(c.carrier_hz >= 300.0 && c.carrier_hz <= 3000.0)) return false;
    if (!(c.propagation_delay_s >= 0.0 && c.propagation_delay_s < 0.2)) return false;
    if (!(c.capture_latency_s >= 0.0 && c.capture_latency_s < 2.0)) return false;
    if (!(c.timestamp_uncertainty_s >= 0.0 && c.timestamp_uncertainty_s < 10.0)) return false;
    return true;
}

} // namespace

struct ahr_wwv_handle {
    ahr_wwv_config cfg{};
    SsbFrontEnd fe;
    ahrwwv::DecoderConfig dcfg;
    ahrwwv::WwvDecoder dec;
    std::vector<std::complex<float>> bb;
    std::vector<size_t> bbIdx;
    std::vector<float> am;

    explicit ahr_wwv_handle(const ahr_wwv_config& c, const ahrwwv::DecoderConfig& d) : cfg(c), dcfg(d), dec(d)
    {
        if (cfg.input == AHR_WWV_INPUT_SSB_AUDIO) fe.init(cfg.sample_rate_hz, cfg.carrier_hz);
    }
};

extern "C" {

uint32_t ahr_wwv_abi_version(void) { return AHR_WWV_ABI_VERSION; }

void ahr_wwv_config_default(ahr_wwv_config* c)
{
    if (!c) return;
    std::memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c;
    c->input = AHR_WWV_INPUT_SSB_AUDIO;
    c->sample_rate_hz = 48000.0;
    c->carrier_hz = 1500.0;
    c->propagation_delay_s = 0.0;
    c->capture_latency_s = 0.0;
    c->timestamp_uncertainty_s = 0.020;
}

ahr_wwv_handle* ahr_wwv_create(const ahr_wwv_config* config)
{
    ahr_wwv_config c;
    ahr_wwv_config_default(&c);
    if (config) {
        if (config->struct_size < sizeof(ahr_wwv_config)) return nullptr;
        c = *config;
        c.struct_size = sizeof c;
    }
    if (!validConfig(c)) return nullptr;
    ahrwwv::DecoderConfig d;
    d.propagationDelay = c.propagation_delay_s;
    d.hostTimestampUncertainty = c.timestamp_uncertainty_s;
    if (c.input == AHR_WWV_INPUT_SSB_AUDIO) {
        SsbFrontEnd probe;
        probe.init(c.sample_rate_hz, c.carrier_hz);
        d.sampleRate = probe.fsOut;
        // A sound card is far less stable than an SDR crystal (50-200 ppm).
        d.sampleClockPpmUncertainty = 200.0;
    } else {
        d.sampleRate = c.sample_rate_hz;
        d.sampleClockPpmUncertainty = 200.0;
    }
    try {
        return new ahr_wwv_handle(c, d);
    } catch (...) {
        return nullptr;
    }
}

void ahr_wwv_destroy(ahr_wwv_handle* h) { delete h; }

int32_t ahr_wwv_push_audio(ahr_wwv_handle* h, const float* samples, size_t count, double pc0)
{
    if (!h || (!samples && count)) return AHR_WWV_ERR_NULL;
    if (!count) return AHR_WWV_OK;
    try {
        const bool haveTs = std::isfinite(pc0);
        const double fs = h->cfg.sample_rate_hz;
        if (h->cfg.input == AHR_WWV_INPUT_AM_AUDIO) {
            if (haveTs) h->dec.setNextSampleTimestamp(pc0 - h->cfg.capture_latency_s);
            h->dec.processAudio(samples, count);
        } else {
            h->fe.process(samples, count, h->bb, h->bbIdx);
            if (!h->bb.empty()) {
                if (haveTs) {
                    // The output computed at input index i describes the input
                    // around i - groupDelay.
                    double ts = pc0 + (double(h->bbIdx.front()) - h->fe.groupDelaySamples) / fs - h->cfg.capture_latency_s;
                    h->dec.setNextSampleTimestamp(ts);
                }
                h->dec.processIQ(h->bb.data(), h->bb.size());
            }
        }
        h->dec.takeEvents(); // events are not exposed through the C API; drop them
        return AHR_WWV_OK;
    } catch (...) {
        h->dec.reset();
        return AHR_WWV_ERR_INTERNAL;
    }
}

int32_t ahr_wwv_poll(ahr_wwv_handle* h, ahr_wwv_status* out)
{
    if (!h || !out) return AHR_WWV_ERR_NULL;
    if (out->struct_size < sizeof(ahr_wwv_status)) return AHR_WWV_ERR_STRUCT_SIZE;
    ahr_wwv_status s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.abi_version = AHR_WWV_ABI_VERSION;
    const ahrwwv::DecoderStatus& st = h->dec.status();
    s.state = stateCode(st.state);
    s.station = stationCode(st.station);
    const bool trusted = st.state == ahrwwv::DecoderState::Verified
        || (st.state == ahrwwv::DecoderState::Holdover && st.verifiedFrames >= 2);
    double utc = 0;
    s.utc_valid = trusted && h->dec.currentUtc(utc) ? 1 : 0;
    s.utc_unix_s = s.utc_valid ? utc : 0.0;
    s.candidate_valid = (st.state == ahrwwv::DecoderState::Decoded && st.haveTime) ? 1 : 0;
    s.clock_offset_valid = (trusted && st.haveClockOffset) ? 1 : 0;
    s.clock_offset_s = s.clock_offset_valid ? st.clockOffset : 0.0;
    s.clock_offset_uncertainty_s = s.clock_offset_valid ? st.clockOffsetUncertainty : 0.0;
    s.time_uncertainty_s = s.utc_valid ? st.timeUncertainty : 0.0;
    s.holdover_age_s = st.holdoverAge;
    if (s.utc_valid || s.candidate_valid) {
        s.year = st.lastTimeCode.year;
        s.day_of_year = st.lastTimeCode.dayOfYear;
        s.hour = st.lastTimeCode.hour;
        s.minute = st.lastTimeCode.minute;
    }
    s.flags_confirmed = st.flagsConfirmed ? 1 : 0;
    if (st.flagsConfirmed) {
        s.dut1_tenths = st.confirmedDut1Tenths;
        s.leap_second_warning = st.confirmedLsw;
        s.dst1 = st.confirmedDst1;
        s.dst2 = st.confirmedDst2;
    }
    s.frame_position = st.framePosition;
    s.frames_verified = st.verifiedFrames;
    s.subcarrier_snr_db = st.subcarrierSnrDb;
    s.second_lock_quality = st.secondLockQuality;
    s.timing_jitter_s = st.timingJitter;
    s.frame_confidence = st.frameConfidence;
    s.sample_clock_settled = st.sampleClockPpmSettled ? 1 : 0;
    s.sample_clock_ppm = st.sampleClockPpmMeasured;
    s.sample_clock_ppm_sigma = st.sampleClockPpmSigma;
    s.seconds_processed = st.secondsProcessed;
    s.confirmation_previous_last_id = st.confirmationPreviousLastFrameId;
    s.confirmation_current_first_id = st.confirmationCurrentFirstFrameId;
    std::strncpy(s.last_problem, st.lastProblem.c_str(), AHR_WWV_PROBLEM_LEN - 1);
    for (int p = 0; p < AHR_WWV_FRAME_CELLS; p++) {
        const ahrwwv::SecondSymbol& c = st.frame[size_t(p)];
        const bool got = c.boundaryTime > 0.0; // cleared at the start of each minute
        s.cells[p].received = got ? 1 : 0;
        s.cells[p].symbol = got ? symCode(c.symbol) : uint8_t(AHR_WWV_SYM_UNKNOWN);
        s.cells[p].confidence = float(c.confidence);
    }
    std::memcpy(out, &s, sizeof s);
    return AHR_WWV_OK;
}

int32_t ahr_wwv_reset(ahr_wwv_handle* h)
{
    if (!h) return AHR_WWV_ERR_NULL;
    h->dec.reset();
    if (h->cfg.input == AHR_WWV_INPUT_SSB_AUDIO) h->fe.init(h->cfg.sample_rate_hz, h->cfg.carrier_hz);
    return AHR_WWV_OK;
}

const char* ahr_wwv_state_name(int32_t s)
{
    switch (s) {
    case AHR_WWV_SEARCHING: return "Searching";
    case AHR_WWV_SECOND_LOCK: return "Second lock";
    case AHR_WWV_FRAME_LOCK: return "Frame lock";
    case AHR_WWV_DECODED: return "Decoded (unconfirmed)";
    case AHR_WWV_VERIFIED: return "Verified";
    case AHR_WWV_HOLDOVER: return "Holdover";
    }
    return "?";
}

const char* ahr_wwv_station_name(int32_t s)
{
    switch (s) {
    case AHR_WWV_STATION_WWV: return "WWV";
    case AHR_WWV_STATION_WWVH: return "WWVH";
    case AHR_WWV_STATION_BOTH: return "WWV+WWVH";
    default: return "Unknown";
    }
}

} // extern "C"
