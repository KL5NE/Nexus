// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
// TEST ONLY - see ahr_wwv_test_synth.h.
#include "ahr_wwv_test_synth.h"
#include "wwvsim.h"

#include <cmath>
#include <complex>
#include <deque>
#include <vector>

using namespace ahrwwv;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct UsbRig {
    double fsSim, fsAudio, carrier;
    std::vector<double> h;
    std::vector<std::complex<double>> hist;
    size_t pos = 0;
    double ph1 = 0, ph2 = 0, t = 0;
    double resampAcc = 0; // simple linear resampler state
    std::complex<double> prev{0, 0};
    bool havePrev = false;

    UsbRig(double fsSim_, double fsAudio_, double carrier_) : fsSim(fsSim_), fsAudio(fsAudio_), carrier(carrier_)
    {
        const int n = 401;
        const double cut = 1350.0 / fsSim;
        double sum = 0;
        for (int i = 0; i < n; i++) {
            double m = i - (n - 1) / 2.0;
            double v = (m == 0 ? 2 * cut : std::sin(2 * kPi * cut * m) / (kPi * m)) * (0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1)));
            h.push_back(v); sum += v;
        }
        for (auto& v : h) v /= sum;
        hist.assign(h.size(), 0.0);
    }

    double groupDelay() const { return (double(h.size()) - 1.0) / 2.0 / fsSim; }

    // Upper sideband of one simulator sample (complex baseband, carrier at 0).
    std::complex<double> usb(std::complex<double> z)
    {
        z *= std::polar(1.0, -ph1);
        ph1 += 2 * kPi * 1350.0 / fsSim;
        hist[pos] = z; pos = (pos + 1) % h.size();
        std::complex<double> acc = 0;
        size_t p = pos;
        for (size_t k = 0; k < h.size(); k++) { acc += hist[p] * h[k]; if (++p == h.size()) p = 0; }
        return acc * std::polar(1.0, ph1 - 2 * kPi * 1350.0 / fsSim); // back up
    }

    // Convert a block of simulator IQ into real audio at fsAudio with the
    // carrier at `carrier` Hz.
    void block(const std::vector<std::complex<float>>& iq, std::vector<float>& audio)
    {
        audio.clear();
        const double ratio = fsAudio / fsSim;
        for (auto& s : iq) {
            std::complex<double> u = usb(std::complex<double>(s.real(), s.imag()));
            if (!havePrev) { prev = u; havePrev = true; }
            // emit audio samples falling between prev and u
            while (resampAcc < 1.0) {
                std::complex<double> v = prev + (u - prev) * resampAcc;
                audio.push_back(float(std::real(v * std::polar(1.0, ph2))));
                ph2 += 2 * kPi * carrier / fsAudio;
                if (ph2 > kPi) ph2 -= 2 * kPi;
                resampAcc += 1.0 / ratio;
            }
            resampAcc -= 1.0;
            prev = u;
        }
    }
};

} // namespace

struct ahr_wwv_test_synth {
    SimConfig sc;
    WwvSim sim;
    UsbRig rig;
    std::vector<std::complex<float>> iq;
    std::vector<float> audio;
    std::deque<float> q;
    ahr_wwv_test_synth(const SimConfig& c, double fs, double carrier) : sc(c), sim(c), rig(c.sampleRate, fs, carrier) {}
};

extern "C" {

ahr_wwv_test_synth* ahr_wwv_test_synth_create(double fs, double start, double snr, double carrier, int32_t dut1,
                                              double fading, uint32_t seed)
{
    if (!(fs >= 8000.0) || !(carrier > 0.0)) return nullptr;
    SimConfig sc;
    sc.sampleRate = 12000;
    sc.startUnix = start;
    sc.snrDb = snr;
    sc.seed = seed ? seed : 1;
    sc.dut1Tenths = dut1;
    sc.st[0].delay = 0.0134;
    if (fading > 0) { sc.st[0].fadeDepth = std::min(1.0, fading); sc.st[0].fadeRateHz = 0.2; }
    try { return new ahr_wwv_test_synth(sc, fs, carrier); } catch (...) { return nullptr; }
}

void ahr_wwv_test_synth_generate(ahr_wwv_test_synth* s, float* out, size_t count)
{
    if (!s || !out) return;
    while (s->q.size() < count) {
        s->sim.generate(s->iq, 1200);
        s->rig.block(s->iq, s->audio);
        s->q.insert(s->q.end(), s->audio.begin(), s->audio.end());
    }
    for (size_t i = 0; i < count; i++) { out[i] = s->q.front(); s->q.pop_front(); }
}

double ahr_wwv_test_synth_rig_latency_s(const ahr_wwv_test_synth* s) { return s ? s->rig.groupDelay() : 0.0; }
double ahr_wwv_test_synth_path_delay_s(const ahr_wwv_test_synth* s) { return s ? s->sc.st[0].delay : 0.0; }
void ahr_wwv_test_synth_destroy(ahr_wwv_test_synth* s) { delete s; }

} // extern "C"
