// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
#include "wwvsim.h"
#include <cmath>

namespace ahrwwv {

static const double kPi = 3.14159265358979323846;

WwvSim::WwvSim(const SimConfig& c) : m_c(c), m_rng(c.seed)
{
    // Noise per complex sample such that carrier/noise in 3 kHz = snrDb.
    double pn3k = 1.0 / std::pow(10.0, c.snrDb / 10.0);
    double pn = pn3k * c.sampleRate / 3000.0;
    m_noiseStd = std::sqrt(pn / 2.0);
    std::uniform_real_distribution<double> u(0, 1);
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 8; k++) {
            m_fadeF[s][k] = c.st[s].fadeRateHz * std::cos(2 * kPi * u(m_rng));
            m_fadeP[s][k][0] = 2 * kPi * u(m_rng);
            m_fadeP[s][k][1] = 2 * kPi * u(m_rng);
        }
}

double WwvSim::trueUtc(uint64_t i) const
{
    return m_c.startUnix + double(i) / (m_c.sampleRate * (1.0 + m_c.ppm * 1e-6));
}

double WwvSim::fade(int s, double t) const
{
    const SimStation& st = m_c.st[s];
    if (st.fadeDepth <= 0) return 1.0;
    double re = 0, im = 0;
    for (int k = 0; k < 8; k++) {
        re += std::cos(2 * kPi * m_fadeF[s][k] * t + m_fadeP[s][k][0]);
        im += std::cos(2 * kPi * m_fadeF[s][k] * t + m_fadeP[s][k][1]);
    }
    double r = std::sqrt((re * re + im * im) / 8.0); // unit-power Rayleigh-like
    return (1.0 - st.fadeDepth) + st.fadeDepth * r;
}

double WwvSim::modulation(int s, double utc)
{
    const SimStation& st = m_c.st[s];
    double ts = utc - st.delay; // transmitter time
    double secF = std::floor(ts);
    double ph = ts - secF;
    int64_t sec = int64_t(secF);
    int64_t minute = (sec >= 0 ? sec / 60 : (sec - 59) / 60) * 60;
    int pos = int(sec - minute);
    if (minute != m_cacheMinute[s]) {
        TimeCode tc = unixToTimeCode(minute);
        tc.dut1Tenths = m_c.dut1Tenths;
        for (auto& o : m_c.dut1ByMinute) if (o.first == minute) tc.dut1Tenths = o.second;
        tc.leapSecondWarning = m_c.lsw;
        tc.dst1 = m_c.dst1;
        tc.dst2 = m_c.dst2;
        m_cacheFrame[s] = encodeFrame(tc);
        m_cacheMinute[s] = minute;
    }
    Symbol sym = m_cacheFrame[s][pos];
    double m = 0;
    // 100 Hz time code
    double dur = sym == Symbol::Zero ? 0.17 : sym == Symbol::One ? 0.47 : sym == Symbol::Marker ? 0.77 : 0.0;
    if (dur > 0 && ph >= 0.03 && ph < 0.03 + dur)
        m += st.subcarrierMod * std::sin(2 * kPi * 100.0 * (ph - 0.03));
    // ticks and minute/hour marker
    int minOfHour = int((minute / 60) % 60);
    if (pos == 0) {
        if (ph < 0.8) {
            double f = minOfHour == 0 ? 1500.0 : st.tickHz;
            m += st.tickMod * std::sin(2 * kPi * f * ph);
        }
    } else if (pos != 29 && pos != 59 && ph < 0.005) {
        m += st.tickMod * std::sin(2 * kPi * st.tickHz * ph);
    }
    bool prot = ph < 0.030 || ph >= 0.990;
    if (!prot && pos >= 1 && pos <= 44) {
        double f = (minOfHour % 2) ? 600.0 : 500.0;
        m += st.toneMod * std::sin(2 * kPi * f * ts);
    }
    if (!prot && pos >= 45 && pos <= 58 && st.voiceMod > 0) {
        // crude voice: noise through a wandering resonator, bursty
        double w = m_norm(m_rng);
        double f0 = st.voiceCentreHz + st.voiceSwingHz * std::sin(2 * kPi * 0.7 * ts);
        double r = 0.995, th = 2 * kPi * f0 / m_c.sampleRate;
        double y = w * 0.05 + 2 * r * std::cos(th) * m_v1[s] - r * r * m_v2[s];
        m_v2[s] = m_v1[s]; m_v1[s] = y;
        double gate = std::sin(2 * kPi * 3.0 * ts) > -0.3 ? 1.0 : 0.0;
        m += st.voiceMod * gate * std::tanh(y);
    }
    return m;
}

void WwvSim::generate(std::vector<std::complex<float>>& out, size_t n)
{
    out.resize(n);
    for (size_t i = 0; i < n; i++, m_n++) {
        double utc = trueUtc(m_n);
        double rel = double(m_n) / m_c.sampleRate;
        bool off = false;
        for (auto& d : m_c.dropouts) if (rel >= d.first && rel < d.second) off = true;
        std::complex<double> z = 0;
        if (!off) {
            for (int s = 0; s < 2; s++) {
                const SimStation& st = m_c.st[s];
                if (!st.enabled) continue;
                double env = std::max(0.0, 1.0 + modulation(s, utc));
                double a = st.amplitude * fade(s, rel) * env;
                z += std::polar(a, st.carrierPhase);
            }
        }
        z += std::complex<double>(m_noiseStd * m_norm(m_rng), m_noiseStd * m_norm(m_rng));
        if (m_c.impulseRate > 0) {
            if (m_n >= m_burstEnd && m_uni(m_rng) < m_c.impulseRate / m_c.sampleRate)
                m_burstEnd = m_n + uint64_t(m_c.sampleRate * m_c.impulseLen * (0.3 + 0.7 * m_uni(m_rng)));
            if (m_n < m_burstEnd) {
                double a = m_c.impulseAmp * m_c.st[0].amplitude / std::sqrt(2.0);
                z += std::complex<double>(a * m_norm(m_rng), a * m_norm(m_rng));
            }
        }
        out[i] = std::complex<float>(float(z.real()), float(z.imag()));
    }
}

} // namespace ahrwwv
