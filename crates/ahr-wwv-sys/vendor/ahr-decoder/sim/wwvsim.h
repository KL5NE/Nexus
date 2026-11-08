// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
// Synthetic WWV/WWVH signal generator for testing (not part of the plugin).
#pragma once
#include "ahrwwv/timecode.h"
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace ahrwwv {

struct SimStation {
    bool enabled = true;
    double delay = 0.010;        // propagation delay (s)
    double amplitude = 1.0;      // carrier amplitude
    double tickHz = 1000.0;      // 1000 = WWV, 1200 = WWVH
    double carrierPhase = 0.0;
    double fadeDepth = 0.0;      // 0 = steady, 1 = full Rayleigh
    double fadeRateHz = 0.2;
    double subcarrierMod = 0.5;  // 100 Hz time code modulation index
    double toneMod = 0.5;        // 500/600 Hz tones
    double voiceMod = 0.4;       // voice-like noise, seconds 45-58
    // Default voice-band model; use 150 Hz and 100 Hz swing to retain the
    // original deliberately harsh in-band interference stress scenario.
    double voiceCentreHz = 700.0;
    double voiceSwingHz = 400.0;
    double tickMod = 1.0;
};

struct SimConfig {
    SimConfig() { st[1].enabled = false; st[1].tickHz = 1200.0; st[1].delay = 0.013; st[1].carrierPhase = 1.0; }
    double sampleRate = 12000.0;
    double startUnix = 0.0;      // true UTC at the first sample
    double ppm = 0.0;            // receiver sample clock error
    double snrDb = 40.0;         // carrier / noise in 3 kHz
    SimStation st[2];            // [0]=WWV, [1]=WWVH (disabled by default)
    std::vector<std::pair<double, double>> dropouts; // (start,end) seconds from start: signal off
    // Impulsive interference (static crashes, local switching noise): Poisson
    // bursts of broadband noise, impulseLen seconds long (uniform 0.3..1x), at
    // impulseAmp times the WWV carrier amplitude. Live high-latitude captures
    // show isolated seconds whose levels read 2-3x a normal pulse.
    double impulseRate = 0.0;    // bursts per second
    double impulseAmp = 3.0;
    double impulseLen = 0.25;
    // time code flags
    int dut1Tenths = 0;
    // Optional DUT1 overrides by UTC minute (Unix seconds at :00): emulate a
    // one-minute misread (one entry) or a real DUT1 step (entries onward).
    std::vector<std::pair<int64_t, int>> dut1ByMinute;
    bool lsw = false, dst1 = false, dst2 = false;
    unsigned seed = 1;
};

class WwvSim {
public:
    explicit WwvSim(const SimConfig& c);
    // Generate the next n complex samples.
    void generate(std::vector<std::complex<float>>& out, size_t n);
    // True UTC at a sample index.
    double trueUtc(uint64_t sampleIndex) const;
    uint64_t samplesGenerated() const { return m_n; }

private:
    double modulation(int s, double t);
    double fade(int s, double t) const;
    SimConfig m_c;
    uint64_t m_n = 0;
    std::mt19937 m_rng;
    std::normal_distribution<double> m_norm{0.0, 1.0};
    double m_noiseStd = 0;
    double m_fadeF[2][8], m_fadeP[2][8][2];
    // voice-like noise: band-limited state
    double m_v1[2] = {0, 0}, m_v2[2] = {0, 0};
    int64_t m_cacheMinute[2] = {INT64_MIN, INT64_MIN};
    Frame m_cacheFrame[2];
    uint64_t m_burstEnd = 0;
    std::uniform_real_distribution<double> m_uni{0.0, 1.0};
};

} // namespace ahrwwv
