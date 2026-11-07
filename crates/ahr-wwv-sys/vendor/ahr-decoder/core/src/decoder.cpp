// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
#include "ahrwwv/decoder.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <limits>

namespace ahrwwv {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPulseStart = 0.030; // pulse begins 30 ms after the second
constexpr int kBins = 1000;           // 1 ms fold bins
constexpr int kKernelAfter = 120;     // ms of "pulse on" after the edge
constexpr int kKernelBefore = 180;    // ms of "pulse off" before the edge

// Symbol detection windows, seconds after the boundary.
constexpr double kWinLow[2] = {0.84, 0.99};
constexpr double kWinA[2] = {0.06, 0.18}; // on for 0, 1, M
constexpr double kWinB[2] = {0.24, 0.48}; // on for 1, M
constexpr double kWinC[2] = {0.54, 0.78}; // on for M

const double kExpected[4][3] = {
    {0, 0, 0}, // None
    {1, 0, 0}, // Zero
    {1, 1, 0}, // One
    {1, 1, 1}, // Marker
};
const Symbol kHyp[4] = {Symbol::None, Symbol::Zero, Symbol::One, Symbol::Marker};

bool isMarkerPos(int p) { return p % 10 == 9; }
// NIST "unused" seconds: 0 from WWV, but WWVH sends 1 at second 47.
bool isUnusedPos(int p)
{
    switch (p) {
    case 1: case 8: case 14: case 18: case 24: case 27: case 28: case 34:
    case 42: case 43: case 44: case 45: case 46: case 47: case 48:
        return true;
    default:
        return false;
    }
}

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void lowpass(double fc, double fs, double q)
    {
        double w0 = 2 * kPi * fc / fs, c = std::cos(w0), al = std::sin(w0) / (2 * q);
        double a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
        a1 = -2 * c / a0; a2 = (1 - al) / a0;
        z1 = z2 = 0;
    }
    double step(double x)
    {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1 = z2 = 0; }
};

// AM envelope -> 100 Hz subcarrier envelope at ~1 kHz.
class FrontEnd {
public:
    void init(double fs, double lpHz)
    {
        m_fs = fs;
        m_n = std::max(1, int(std::lround(fs / 1000.0)));
        m_dphi = 2 * kPi * 100.0 / fs;
        double fd = fs / m_n;
        const double q[2] = {0.54119610, 1.30656296};
        for (int i = 0; i < 2; i++) {
            m_re[i].lowpass(lpHz, fd, q[i]);
            m_im[i].lowpass(lpHz, fd, q[i]);
            m_c[i].lowpass(lpHz, fd, q[i]);
        }
        m_dcK = 1.0 / (fs * 0.05);
        reset();
    }
    void reset()
    {
        m_phi = 0; m_acc = 0; m_cacc = 0; m_cnt = 0; m_dc = 0; m_dcInit = false; m_first = 0;
        for (int i = 0; i < 2; i++) { m_re[i].clear(); m_im[i].clear(); m_c[i].clear(); }
        m_cInit = false;
    }
    // Returns true and sets (t, v) when a decimated output is produced.
    // xAc receives the DC-removed input (for tick analysis).
    bool push(uint64_t index, double x, bool normalize, double& t, std::complex<double>& v, double& xAc, double& carrier)
    {
        if (!m_dcInit) { m_dc = x; m_dcInit = true; }
        m_dc += (x - m_dc) * m_dcK;
        xAc = x - m_dc;
        if (m_cnt == 0) m_first = index;
        m_acc += std::complex<double>(xAc * std::cos(m_phi), -xAc * std::sin(m_phi));
        m_cacc += x;
        m_phi += m_dphi;
        if (m_phi > 2 * kPi) m_phi -= 2 * kPi;
        if (++m_cnt < m_n) return false;
        std::complex<double> z = m_acc / double(m_n);
        double c = m_cacc / double(m_n);
        m_acc = 0; m_cacc = 0; m_cnt = 0;
        // Carrier level through the same low-pass, so flat fading divides out
        // with matching timing.
        if (!m_cInit) {
            for (int i = 0; i < 2; i++) { m_c[i].z1 = c * (1 - m_c[i].b0); m_c[i].z2 = c * (m_c[i].b2 - m_c[i].a2); }
            m_cInit = true;
        }
        for (int i = 0; i < 2; i++) c = m_c[i].step(c);
        double re = z.real(), im = z.imag();
        for (int i = 0; i < 2; i++) { re = m_re[i].step(re); im = m_im[i].step(im); }
        // 2*z has magnitude equal to the amplitude of the 100 Hz tone
        std::complex<double> env(2.0 * re, 2.0 * im);
        carrier = c;
        if (normalize) env /= std::max(std::fabs(c), 1e-12);
        v = env;
        t = (double(m_first) + (m_n - 1) / 2.0) / m_fs;
        return true;
    }

private:
    double m_fs = 48000, m_dphi = 0, m_phi = 0, m_dc = 0, m_dcK = 0;
    bool m_dcInit = false, m_cInit = false;
    double m_cacc = 0;
    int m_n = 48, m_cnt = 0;
    uint64_t m_first = 0;
    std::complex<double> m_acc = 0;
    Biquad m_re[2], m_im[2], m_c[2];
};

// Exponentially weighted average of the envelope folded modulo one second.
class SecondFold {
public:
    void init(double tau) { m_alpha = std::exp(-1.0 / tau); reset(); }
    void reset()
    {
        std::fill(m_sum, m_sum + kBins, 0.0);
        std::fill(m_wt, m_wt + kBins, 0.0);
    }
    void add(double t, double v)
    {
        double ph = t - std::floor(t);
        int b = std::min(kBins - 1, int(ph * kBins));
        m_sum[b] = m_sum[b] * m_alpha + v;
        m_wt[b] = m_wt[b] * m_alpha + 1.0;
    }
    // Locates the pulse leading edge. Returns edge phase in [0,1).
    double estimate(double& hi, double& lo) const
    {
        double f[kBins];
        for (int i = 0; i < kBins; i++) f[i] = m_wt[i] > 0 ? m_sum[i] / m_wt[i] : 0.0;
        // circular prefix sums
        double ps[3 * kBins + 1];
        ps[0] = 0;
        for (int i = 0; i < 3 * kBins; i++) ps[i + 1] = ps[i] + f[i % kBins];
        auto mean = [&](int a, int b) { // [a,b) with a may be negative (offset by kBins)
            return (ps[b + kBins] - ps[a + kBins]) / double(b - a);
        };
        double k[kBins];
        int best = 0;
        for (int p = 0; p < kBins; p++) {
            k[p] = mean(p, p + kKernelAfter) - mean(p - kKernelBefore, p);
            if (k[p] > k[best]) best = p;
        }
        double km = k[(best + kBins - 1) % kBins], k0 = k[best], kp = k[(best + 1) % kBins];
        double den = km - 2 * k0 + kp, delta = 0;
        if (den < 0) delta = std::clamp(0.5 * (km - kp) / den, -0.5, 0.5);
        hi = mean(best, best + kKernelAfter);
        lo = mean(best - kKernelBefore, best);
        double e = (best + delta) / kBins;
        return e - std::floor(e);
    }

private:
    double m_alpha = 0.85;
    double m_sum[kBins], m_wt[kBins];
};

double wrapHalf(double x) { return x - std::floor(x + 0.5); } // to [-0.5,0.5)

} // namespace

const char* decoderStateName(DecoderState s)
{
    switch (s) {
    case DecoderState::Searching: return "Searching";
    case DecoderState::SecondLock: return "Second lock";
    case DecoderState::FrameLock: return "Frame lock";
    case DecoderState::Decoded: return "Decoded";
    case DecoderState::Verified: return "Verified";
    case DecoderState::Holdover: return "Holdover";
    }
    return "?";
}

bool consecutiveRawMinutes(const std::vector<uint64_t>& ids, const std::vector<double>& b59)
{
    if (ids.empty() || ids.size() != b59.size()) return false;
    for (size_t i = 1; i < ids.size(); i++)
        if (ids[i] != ids[i - 1] + 1 || std::fabs((b59[i] - b59[i - 1]) - 60.0) > 0.5)
            return false;
    return true;
}

bool disjointRawMinuteWindows(uint64_t firstA, uint64_t lastA,
                              uint64_t firstB, uint64_t lastB)
{
    if (!firstA || !firstB || firstA > lastA || firstB > lastB) return false;
    return lastA < firstB || lastB < firstA;
}

const char* stationName(StationId s)
{
    switch (s) {
    case StationId::WWV: return "WWV";
    case StationId::WWVH: return "WWVH";
    case StationId::Both: return "WWV+WWVH";
    default: return "Unknown";
    }
}



namespace {

struct EnvSample {
    double t;                // delay-corrected sample-clock time (s)
    std::complex<double> z;  // 100 Hz baseband phasor
    double mag;
    double carrier;          // carrier level (IQ input), 1 for audio
};

// 50 % leading-edge crossing of the pulse in the second starting at b.
// value(e) maps an envelope sample to a real level.
template <class Ring, class Fn>
bool edgeCrossing(const Ring& ring, double b, Fn value, double& tc, double& high, double& low)
{
    double sh = 0, sl = 0; int nh = 0, nl = 0;
    for (const auto& p : ring) {
        if (p.t >= b + kWinA[0] && p.t < b + kWinA[1]) { sh += value(p); nh++; }
        if (p.t >= b + kWinLow[0] - 1.0 && p.t < b + kWinLow[1] - 1.0) { sl += value(p); nl++; }
    }
    if (!nh || !nl) return false;
    high = sh / nh; low = sl / nl;
    if (high <= low) return false;
    double th = low + 0.5 * (high - low);
    bool have = false; double pt = 0, pv = 0;
    for (const auto& p : ring) {
        if (p.t < b - 0.02) continue;
        if (p.t > b + 0.10) break;
        double v = value(p);
        if (have && pv < th && v >= th) {
            tc = pt + (th - pv) / (v - pv) * (p.t - pt);
            return true;
        }
        pt = p.t; pv = v; have = true;
    }
    return false;
}

constexpr double kLockContrast = 0.20;   // fold contrast needed to declare second lock
constexpr double kUnlockContrast = 0.10;
constexpr double kMinPulseGain = 0.25;  // weakest faded pulse level modelled
constexpr double kMaxPulseGain = 1.5;
constexpr size_t kHrefPulses = 31;
constexpr double kMinSymbolConf = 0.15;  // below this a second is shown as "?"
constexpr double kEdgeAlpha = 0.05;      // fine timing loop gain (~40 s averaging)
// Frame margins are log-likelihood ratios in nats (calibrateSoftFrame).
// A first decode needs this much before it can stand as the time basis on
// its own; weaker valid decodes are kept only as candidates for confirmation.
// One second can count for at most -ln(kOutlierProb) = 4.6 nats, so a
// single-bit field (leap second, DST) from one minute reaches this only when
// its second is unambiguous.
constexpr double kStrongDecodeNats = 4.5;
constexpr double kConfidenceFullNats = 8.0;
// The sample-clock readout is shown as a number once its 1-sigma error is at
// most this; before that it is "measuring". A sound card is typically 50-200
// ppm off and settles fast; an SDR crystal within 1-2 ppm needs ~10 minutes.
constexpr double kPpmSettledSigma = 0.5; // margin shown as confidence 1.00

} // namespace

struct WwvDecoder::Impl {
    DecoderConfig cfg;
    DecoderStatus st;
    FrontEnd fe;
    SecondFold fold;
    double delay = 0;      // detector delay to the 50 % point (s)
    double kOff = 0;       // fold-kernel edge minus 50 % edge (s)
    uint64_t index = 0;
    double lastSampleTime = 0;
    bool normalize = true;

    std::deque<EnvSample> ring;

    // second lock
    long lastWhole = std::numeric_limits<long>::min();
    int foldSeconds = 0;
    double candPhase = -1;
    int stable = 0;
    bool secLocked = false;
    int badPhase = 0;
    double phase = 0;      // coarse boundary phase (mod 1)
    double contrast = 0;

    // fine timing
    double eAvg = 0; bool eInit = false; double lastEdgeT = -1e18;
    double edgeVar = 0; bool edgeVarInit = false;

    // coherent 100 Hz reference
    std::complex<double> P = 0;
    int pUpdates = 0;

    // boundary scheduler
    bool scheduling = false;
    double nextB = 0;

    double href = 0;
    std::deque<double> hrefHist; // window-A levels of recent confident pulses
    double offVar = 0; bool offInit = false;
    double noiseFloor = 0;
    double noiseMag = 0;  // tracked off-pulse 100 Hz magnitude (noise bias)
    double carrierTyp = 0;
    int edgeCount = 0;

    std::deque<std::pair<double, double>> phaseHist;
    double ppm = 0; bool ppmValid = false;
    double dispPpm = 0, dispPpmSigma = 0; bool dispPpmValid = false;

    struct Hist { SecondSymbol s; std::array<double, 4> cost; };
    std::deque<Hist> hist;
    bool frameLocked = false;
    int pos = -1;
    int frameFailures = 0;

    bool haveTime = false;
    double anchorT = 0, anchorUtc = 0;
    double lastGoodT = -1e18;
    int verified = 0;
    TimeCode lastTc;
    // Raw minute identities are assigned from the sample stream, never from
    // the tentative decoded UTC. All joint and confirming evidence is tracked
    // by exact [firstId, lastId] ranges.
    struct Candidate { double minuteUtc; double b59; uint64_t firstId, lastId; };
    std::deque<Candidate> candidates;
    struct RecentFrame { SoftFrame soft; double b59; uint64_t id; };
    std::deque<RecentFrame> recentFrames;
    uint64_t nextRawMinuteId = 0;
    uint64_t timeBasisLastFrameId = 0;
    uint64_t confirmationPreviousLastFrameId = 0;
    uint64_t confirmationCurrentFirstFrameId = 0;
    // Station flags (DUT1, LSW, DST) of recent accepted decodes, with the raw
    // minutes each decode used; a value is confirmed by two disjoint decodes.
    struct FlagObs { int dut1; bool lsw, dst1, dst2; uint64_t firstId, lastId; };
    std::deque<FlagObs> flagObs;
    bool flagsConfirmed = false;
    FlagObs confirmedFlags{0, false, false, false, 0, 0};
    // Up to five consecutive minutes are read together. Faded minutes are
    // weak but not wrong; the date fields need more than three of them under
    // high-latitude fading (live 10 MHz, Palmer AK, 2026-11-05).
    static constexpr int kMaxJointFrames = 5;
    // Verification needs later raw minutes that share no evidence with the
    // previous basis, even when both decodes use overlapping joint windows.
    bool tickActive = false;
    double tickStart = 0, tickEnd = 0;
    double g1r = 0, g1i = 0, g2r = 0, g2i = 0, tickE = 0;
    double f1000 = 0, f1200 = 0, fNoise = 0;
    double gn[4] = {0, 0, 0, 0};

    bool havePc = false;
    double pcRef = 0, pcRefT = 0;
    double offAvg = 0; bool offAvgInit = false;

    DecoderState lastState = DecoderState::Searching;
    std::vector<DecoderEvent> events;
    Biquad chRe[2], chIm[2];
    WwvDecoder::EnvelopeTap tap = nullptr;
    void* tapUser = nullptr;

    // ---------------------------------------------------------------
    bool phasorValid() const { return secLocked && pUpdates >= 300 && std::abs(P) > 0; }
    // Non-coherent magnitude: HF multipath can rotate subcarrier phase
    // within a single pulse. Long-term phase projection loses valid pulses.
    double proj(const EnvSample& e) const
    {
        return e.mag;
    }

    bool utcAt(double t, double& utc) const
    {
        if (!haveTime) return false;
        double scale = ppmValid ? 1.0 / (1.0 + ppm * 1e-6) : 1.0;
        utc = anchorUtc + (t - anchorT) * scale;
        return true;
    }

    void processSample(double x)
    {
        uint64_t idx = index++;
        double tr = idx / cfg.sampleRate;
        lastSampleTime = tr;
        double t, xac, c;
        std::complex<double> z;
        bool out = fe.push(idx, x, normalize, t, z, xac, c);
        tickSample(tr, xac);
        if (out) onEnvelope(t - delay, z, normalize ? c : 1.0);
    }

    // Goertzel at 1000 Hz (WWV) and 1200 Hz (WWVH) over the seconds tick.
    // A 5 ms burst of either tone has a spectral null at the other frequency.
    void tickSample(double tr, double xac)
    {
        if (!tickActive) {
            if (!(secLocked || haveTime)) return;
            double nb = phase + std::ceil(tr + 0.0025 - phase);
            tickStart = nb - 0.002;
            tickEnd = nb + 0.012;
            g1r = g1i = g2r = g2i = tickE = 0;
            for (double& g : gn) g = 0;
            tickActive = true;
        }
        if (tr >= tickStart && tr < tickEnd) {
            double dt = tr - tickStart;
            double w1 = 2 * kPi * 1000.0 * dt, w2 = 2 * kPi * 1200.0 * dt;
            g1r += xac * std::cos(w1); g1i -= xac * std::sin(w1);
            g2r += xac * std::cos(w2); g2i -= xac * std::sin(w2);
            // noise references at 800 and 1400 Hz: spectral nulls of both ticks
            double w3 = 2 * kPi * 800.0 * dt, w4 = 2 * kPi * 1400.0 * dt;
            gn[0] += xac * std::cos(w3); gn[1] -= xac * std::sin(w3);
            gn[2] += xac * std::cos(w4); gn[3] -= xac * std::sin(w4);
            tickE += xac * xac;
        } else if (tr >= tickEnd) {
            double L = 0.005 * cfg.sampleRate;
            if (tickE > 0) {
                double a = 2 * (g1r * g1r + g1i * g1i) / (L * tickE);
                double b = 2 * (g2r * g2r + g2i * g2i) / (L * tickE);
                double nz = (gn[0] * gn[0] + gn[1] * gn[1] + gn[2] * gn[2] + gn[3] * gn[3]) / (L * tickE);
                f1000 = 0.9 * f1000 + 0.1 * a;
                f1200 = 0.9 * f1200 + 0.1 * b;
                fNoise = 0.9 * fNoise + 0.1 * nz;
            }
            tickActive = false;
        }
    }

    void onEnvelope(double t, std::complex<double> z, double c)
    {
        EnvSample e{t, z, std::abs(z), c};
        carrierTyp = carrierTyp <= 0 ? c : carrierTyp + (c - carrierTyp) * 0.00005; // ~20 s
        ring.push_back(e);
        while (!ring.empty() && ring.front().t < t - 3.5) ring.pop_front();
        fold.add(t, e.mag);
        if (secLocked) {
            double ph = t - phase;
            ph -= std::floor(ph);
            if (ph >= kWinA[0] && ph < kWinA[1]) {
                P += (z - P) * (1.0 / 1500.0);
                pUpdates++;
            }
        }
        if (tap) {
            double lvl = href > 0 ? e.mag / href : e.mag;
            tap(tapUser, t, float(lvl));
        }
        long w = long(std::floor(t));
        if (w != lastWhole) {
            lastWhole = w;
            onWholeSecond(t);
        }
        while (scheduling && t >= nextB + 1.0) {
            extractSecond(nextB);
            nextB += 1.0;
            if (secLocked) nextB = phase + std::round(nextB - phase);
        }
    }

    void onWholeSecond(double t)
    {
        foldSeconds++;
        if (foldSeconds < 3) { updateState(); return; }
        double hi, lo;
        double edge = fold.estimate(hi, lo);
        contrast = (hi - lo) / (hi + lo + 1e-15);
        double bp = edge - kPulseStart - kOff;
        bp -= std::floor(bp);
        if (candPhase >= 0 && std::fabs(wrapHalf(bp - candPhase)) < 0.003) stable++;
        else stable = 0;
        candPhase = bp;

        if (!secLocked) {
            if (contrast >= kLockContrast && stable >= 3) {
                bool jump = std::fabs(wrapHalf(bp - phase)) > 0.02;
                secLocked = true;
                badPhase = 0;
                phase = bp;
                eAvg = 0; eInit = false; edgeCount = 0;
                if (href <= 0 || jump) { href = hi - lo; hrefHist.clear(); }
                if (jump) { P = 0; pUpdates = 0; }
                phaseHist.clear();
                dispPpmValid = false;
                if (!scheduling) {
                    nextB = phase + std::ceil(ring.front().t - phase);
                    scheduling = true;
                } else {
                    nextB = phase + std::round(nextB - phase);
                    if (jump) { hist.clear(); frameLocked = false; }
                }
            }
        } else {
            if (contrast < kUnlockContrast || std::fabs(wrapHalf(bp - phase)) > 0.02) {
                if (++badPhase >= 5) {
                    secLocked = false;
                    stable = 0;
                    if (!haveTime) { scheduling = false; frameLocked = false; hist.clear(); }
                }
            } else {
                badPhase = 0;
                // Without fine edge measurements for a while, follow the coarse
                // fold if it has clearly moved away.
                if (t - lastEdgeT > 30.0 && std::fabs(wrapHalf(bp - phase - eAvg)) > 0.002) {
                    phase = bp; eAvg = 0; eInit = false; edgeCount = 0;
                }
            }
        }

        updateState();
    }

    // Sample clock error from the trend of raw (unfiltered) edge times.
    void recordEdge(double tc)
    {
        double frac = tc - std::floor(tc);
        double u = frac;
        if (!phaseHist.empty()) u = phaseHist.back().second + wrapHalf(frac - phaseHist.back().second);
        phaseHist.emplace_back(tc, u);
        while (phaseHist.size() > 400 || (phaseHist.size() > 2 && tc - phaseHist.front().first > 600.0))
            phaseHist.pop_front();
        if (phaseHist.size() < 30 || phaseHist.back().first - phaseHist.front().first < 90.0) return;
        double n = double(phaseHist.size()), sx = 0, sy = 0, sxx = 0, sxy = 0;
        double t0 = phaseHist.front().first, y0 = phaseHist.front().second;
        for (auto& p : phaseHist) {
            double x = p.first - t0, y = p.second - y0;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double den = n * sxx - sx * sx;
        if (den > 0) { ppm = (n * sxy - sx * sy) / den * 1e6; ppmValid = true; }
        robustPpmFit();
    }

    // The value shown to people: the same trend, refitted without edges that
    // jumped (fading path changes, impulses), with an honest 1-sigma error.
    // Edge residuals are correlated over seconds (ionospheric path wander),
    // so the error is inflated by the lag-1 autocorrelation of the residuals.
    void robustPpmFit()
    {
        const size_t n = phaseHist.size();
        if (n < 30) return;
        const double t0 = phaseHist.front().first, y0 = phaseHist.front().second;
        std::vector<double> xs(n), ys(n);
        for (size_t i = 0; i < n; i++) { xs[i] = phaseHist[i].first - t0; ys[i] = phaseHist[i].second - y0; }
        std::vector<char> use(n, 1);
        double a = 0, b = 0, sxxc = 0;
        int m = 0;
        for (int pass = 0; pass < 2; pass++) {
            double sx = 0, sy = 0; m = 0;
            for (size_t i = 0; i < n; i++) if (use[i]) { sx += xs[i]; sy += ys[i]; m++; }
            if (m < 20) return;
            double mx = sx / m, my = sy / m, sxy = 0;
            sxxc = 0;
            for (size_t i = 0; i < n; i++) if (use[i]) { sxxc += (xs[i] - mx) * (xs[i] - mx); sxy += (xs[i] - mx) * (ys[i] - my); }
            if (sxxc <= 0) return;
            b = sxy / sxxc; a = my - b * mx;
            std::vector<double> ar;
            for (size_t i = 0; i < n; i++) ar.push_back(std::fabs(ys[i] - (a + b * xs[i])));
            std::vector<double> sorted = ar;
            std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
            double mad = 1.4826 * sorted[n / 2];
            double lim = std::max(3.5 * mad, 0.0005);
            if (pass == 0) for (size_t i = 0; i < n; i++) use[i] = ar[i] <= lim;
        }
        double ss = 0, lag = 0, prev = 0; bool havePrev = false; int nl = 0;
        for (size_t i = 0; i < n; i++) if (use[i]) {
            double r = ys[i] - (a + b * xs[i]);
            ss += r * r;
            if (havePrev) { lag += r * prev; nl++; }
            prev = r; havePrev = true;
        }
        double var = ss / std::max(1, m - 2);
        double rho = (nl > 0 && ss > 0) ? std::clamp((lag / nl) / (ss / m), 0.0, 0.95) : 0.0;
        double inflate = std::sqrt((1.0 + rho) / (1.0 - rho));
        dispPpm = b * 1e6;
        dispPpmSigma = std::sqrt(var / sxxc) * inflate * 1e6;
        dispPpmValid = true;
    }

    double minCarrier(double a, double b) const
    {
        double m = 1e300;
        for (auto& p : ring) if (p.t >= a && p.t < b) m = std::min(m, p.carrier);
        return m == 1e300 ? 0.0 : m;
    }

    // Mean power of the component in quadrature with the 100 Hz reference.
    double quadPower(double a, double b) const
    {
        double s = 0; int n = 0;
        double m = std::abs(P);
        for (auto& p : ring) {
            if (p.t >= a && p.t < b) { double q = (p.z * std::conj(P)).imag() / m; s += q * q; n++; }
        }
        return n ? s / n : 0.0;
    }

    double windowMean(double a, double b, double* var = nullptr) const
    {
        double s = 0, s2 = 0; int n = 0;
        for (auto& p : ring) {
            if (p.t >= a && p.t < b) { double v = proj(p); s += v; s2 += v * v; n++; }
        }
        if (!n) return 0;
        double m = s / n;
        if (var) *var = std::max(0.0, s2 / n - m * m);
        return m;
    }

    void extractSecond(double b)
    {
        // Phase-independent 100 Hz envelope, with tracked off-pulse bias.
        // Real multi-hop skywave can swing phase during the pulse itself.
        double var = 0;
        double lowMag = windowMean(b + kWinLow[0], b + kWinLow[1], &var);
        double varPre = 0;
        double preMag = windowMean(b - 0.16, b - 0.01, &varPre);
        double offMag = std::min(lowMag, preMag);
        if (noiseMag <= 0) noiseMag = offMag;
        else noiseMag += (offMag < noiseMag ? 0.3 : 0.02) * (offMag - noiseMag);
        double A = windowMean(b + kWinA[0], b + kWinA[1]) - noiseMag;
        double B = windowMean(b + kWinB[0], b + kWinB[1]) - noiseMag;
        double C = windowMean(b + kWinC[0], b + kWinC[1]) - noiseMag;
        double r = std::max(href, 1e-15);
        double lv[3] = {A / r, B / r, C / r};
        // Noise floor: follows quiet seconds quickly, noisy (voice) seconds slowly.
        double sn = std::sqrt(std::max(var, varPre));
        if (noiseFloor <= 0) noiseFloor = sn;
        else noiseFloor += (sn < noiseFloor ? 0.3 : 0.01) * (sn - noiseFloor);
        // Weight pulse decisions by local envelope fluctuations rather than
        // the quadrature component, which can contain rotated real skywave
        // signal. Unreliable seconds must not outvote clear seconds.
        const double* wins[3] = {kWinA, kWinB, kWinC};
        double wgt[3];
        for (int i = 0; i < 3; i++) {
            double len = wins[i][1] - wins[i][0];
            double neff = std::max(1.0, len * 2.0 * 1.03 * cfg.lowpassHz);
            double sl2 = sn * sn / (r * r) / neff;
            wgt[i] = std::min(1.0, (0.12 * 0.12) / std::max(sl2, 1e-12));
        }
        // Fading-aware templates: on HF the whole pulse fades together within
        // a second, so a "1" received at 30 % strength reads A~0.3, B~0.3, not
        // A~0.3, B~1 (live WWVH 15 MHz, Palmer AK, 2026-11-04). The A window is
        // on for every pulse symbol, so its level is a symbol-independent
        // estimate of this second's pulse gain; the B/C templates are scaled
        // by it. The A term itself keeps the fixed template, so the decision
        // between "no pulse" and "pulse" is unchanged.
        double g = std::clamp(lv[0], kMinPulseGain, kMaxPulseGain);
        std::array<double, 4> cost;
        for (int h = 0; h < 4; h++) {
            cost[h] = 0;
            for (int i = 0; i < 3; i++) {
                double scale = (i == 0 || h == 0) ? 1.0 : g;
                double e = lv[i] - scale * kExpected[h][i];
                cost[h] += wgt[i] * e * e;
            }
        }
        int best = 0, second = -1;
        for (int h = 1; h < 4; h++) if (cost[h] < cost[best]) best = h;
        for (int h = 0; h < 4; h++) if (h != best && (second < 0 || cost[h] < cost[second])) second = h;
        double conf = std::clamp(cost[second] - cost[best], 0.0, 1.0);

        // Use a median instead of following carrier-fade spikes with an EMA.
        if (best != 0 && conf > 0.3 && A > 0) {
            hrefHist.push_back(A);
            while (hrefHist.size() > kHrefPulses) hrefHist.pop_front();
            if (hrefHist.size() >= 5) {
                std::vector<double> v(hrefHist.begin(), hrefHist.end());
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                href = v[v.size() / 2];
            } else {
                href = 0.8 * href + 0.2 * A;
            }
        }
        if (!offInit) { offVar = var; offInit = true; } else offVar = 0.9 * offVar + 0.1 * var;

        // Fine timing from this second's leading edge.
        // only time edges from seconds without extra noise/voice around them
        bool quiet = sn < 0.2 * r && sn < 2.0 * noiseFloor;
        // frequency-aided tracking: advance by the measured sample clock drift
        if (ppmValid && eInit) eAvg += ppm * 1e-6;
        if (best != 0 && conf > 0.5 && quiet) {
            double tc, hi, lo;
            if (edgeCrossing(ring, b, [this](const EnvSample& e) { return proj(e); }, tc, hi, lo)
                && hi - lo > 0.4 * r) {
                double e = tc - (b + kPulseStart);
                if (std::fabs(e) < 0.02) {
                    double dev = eInit ? e - eAvg : 0.0;
                    if (!edgeVarInit) { edgeVar = 0; edgeVarInit = true; }
                    else edgeVar = 0.95 * edgeVar + 0.05 * std::min(dev * dev, 1e-4);
                    // Huber-style clamp so one bad second (voice, static crash)
                    // cannot drag the timing.
                    double lim = std::max(2.5 * std::sqrt(edgeVar), 0.0005);
                    // average the first edges equally, then settle to the slow loop
                    double g = std::max(kEdgeAlpha, 1.0 / (edgeCount + 1));
                    eAvg = eInit ? eAvg + g * std::clamp(dev, -lim, lim) : e;
                    eInit = true;
                    edgeCount++;
                    lastEdgeT = b;
                    recordEdge(tc);
                    if (std::fabs(eAvg) > 0.002) { phase += eAvg; phase -= std::floor(phase); b += eAvg; eAvg = 0; }
                }
            }
        }

        SecondSymbol ss;
        ss.boundaryTime = b + eAvg;
        ss.symbol = conf < kMinSymbolConf ? Symbol::Unknown : kHyp[best];
        ss.confidence = conf;
        ss.levels = {lv[0], lv[1], lv[2]};
        hist.push_back(Hist{ss, cost});
        while (hist.size() > 130) hist.pop_front();
        st.secondsProcessed++;

        if (frameLocked) pos = (pos + 1) % 60;
        alignFrame();
        if (frameLocked) {
            hist.back().s.framePosition = pos;
            if (pos == 0) st.frame = std::array<SecondSymbol, 60>();
            st.frame[pos] = hist.back().s;
            if (pos == 59) tryDecode(0);
        }

        DecoderEvent ev{};
        ev.type = DecoderEvent::Type::Second;
        ev.second = hist.back().s;
        double utc;
        if (utcAt(ss.boundaryTime, utc)) {
            // DECODED can be a confidently wrong hypothesis. Never label
            // a second as UTC or compare it to the PC clock until VERIFIED.
            if (verified >= 2) {
                ev.utcAtBoundary = std::round(utc);
                ev.haveUtc = true;
                updateClockOffset(ss.boundaryTime, ev.utcAtBoundary);
            }
        }
        events.push_back(ev);
        updateState();
    }

    double posCost(const Hist& h, int p) const
    {
        if (p == 0) return h.cost[0];
        if (isMarkerPos(p)) return h.cost[3];
        return std::min(h.cost[1], h.cost[2]);
    }

    void alignFrame()
    {
        int n = int(hist.size());
        int m = std::min(n, 70);
        if (m < 20) return;
        double c[60];
        for (int j = 0; j < 60; j++) {
            double s = 0;
            for (int k = 0; k < m; k++) s += posCost(hist[n - 1 - k], ((j - k) % 60 + 60) % 60);
            c[j] = s;
        }
        int j1 = 0;
        for (int j = 1; j < 60; j++) if (c[j] < c[j1]) j1 = j;
        double c2 = 1e300;
        for (int j = 0; j < 60; j++) if (j != j1) c2 = std::min(c2, c[j]);
        double margin = c2 - c[j1];
        bool sawZero = j1 < m;
        bool good = sawZero && margin >= 1.5 && c[j1] / m < 0.3;

        if (!frameLocked) {
            if (good) {
                frameLocked = true;
                pos = j1;
                frameFailures = 0;
                int age59 = j1 + 1; // most recent complete frame already in history?
                if (pos != 59 && n >= age59 + 60) tryDecode(age59);
            }
        } else if (j1 != pos && good && c[pos] - c[j1] >= 2.0) {
            pos = j1;
            frameFailures = 0;
        }
    }

    void tryDecode(int age59)
    {
        int n = int(hist.size());
        if (n < age59 + 60) return;
        SoftFrame soft;
        std::array<SecondSymbol, 60> syms;
        for (int p = 0; p < 60; p++) {
            const Hist& h = hist[n - 1 - (age59 + 59 - p)];
            soft[p] = h.cost;
            syms[p] = h.s;
            syms[p].framePosition = p;
        }
        double b0 = syms[0].boundaryTime, b59 = syms[59].boundaryTime;
        if (std::fabs((b59 - b0) - 59.0) > 0.1) return;

        // Keep the recent frames: a re-decode of the same frame after
        // re-alignment replaces it; a gap in the minute sequence starts over.
        uint64_t rawId = 0;
        if (!recentFrames.empty() && std::fabs(b59 - recentFrames.back().b59) < 0.5) {
            rawId = recentFrames.back().id; // a re-decode, NOT a new raw minute
            recentFrames.pop_back();
        }
        if (!recentFrames.empty() && std::fabs((b59 - recentFrames.back().b59) - 60.0) > 0.5) {
            // A discontinuity invalidates any previous candidate for
            // independent confirmation. A verified clock may hold over, but
            // an unconfirmed hypothesis must not bridge missing raw minutes.
            recentFrames.clear();
            candidates.clear();
            if (verified < 2) {
                haveTime = false;
                verified = 0;
                timeBasisLastFrameId = 0;
                offAvgInit = false;
            }
        }
        if (!rawId) rawId = ++nextRawMinuteId;
        recentFrames.push_back(RecentFrame{soft, b59, rawId});
        while (int(recentFrames.size()) > kMaxJointFrames) recentFrames.pop_front();

        DecodeResult r = decodeFrameSoft(soft);
        uint64_t firstId = recentFrames.back().id;
        uint64_t lastId = recentFrames.back().id;
        // A joint window may only contain consecutive, distinct raw minutes
        // aligned 60 sample-clock seconds apart.
        for (int k = 2; !r.valid && k <= int(recentFrames.size()); k++) {
            std::vector<SoftFrame> fs;
            std::vector<uint64_t> ids;
            std::vector<double> boundaries;
            for (size_t i = recentFrames.size() - k; i < recentFrames.size(); i++) {
                fs.push_back(recentFrames[i].soft);
                ids.push_back(recentFrames[i].id);
                boundaries.push_back(recentFrames[i].b59);
            }
            if (!consecutiveRawMinutes(ids, boundaries)) continue;
            DecodeResult rj = decodeFramesSoft(fs);
            if (rj.valid) { r = rj; firstId = ids.front(); }
        }
        st.frameConfidence = std::clamp(r.minMargin / kConfidenceFullNats, 0.0, 1.0);
        st.lastFrame = syms;

        double predMinute = 0;
        bool havePred = false;
        if (haveTime) {
            double u;
            utcAt(b0, u);
            predMinute = std::round(u);
            havePred = std::fmod(predMinute, 60.0) == 0.0;
        }

        DecoderEvent ev{};
        ev.decode = r;
        bool accept = r.valid;
        if (r.valid) {
            double M = double(timeCodeToUnix(r.tc));
            bool agree = havePred && std::fabs(M - predMinute) < 0.5;
            // Is this frame consistent with an earlier, different decoded frame
            // (same UTC offset from the sample clock)? Two independent frames
            // that agree confirm each other even if frames in between failed.
            bool confirmedByOld = false;
            uint64_t oldLastId = 0;
            for (const auto& c : candidates)
                if (std::fabs(M - c.minuteUtc) > 0.5
                    && std::fabs((M - c.minuteUtc) - (b59 - c.b59)) < 0.5
                    && c.lastId < firstId
                    && disjointRawMinuteWindows(c.firstId, c.lastId, firstId, lastId)) {
                    confirmedByOld = true;
                    oldLastId = c.lastId;
                    break;
                }
            while (!candidates.empty() && b59 - candidates.front().b59 > 900.0) candidates.pop_front();
            if (r.minMargin >= kStrongDecodeNats) candidates.push_back(Candidate{M, b59, firstId, lastId});

            if (agree) {
                if (timeBasisLastFrameId && timeBasisLastFrameId < firstId) {
                    if (verified == 1) {
                        confirmationPreviousLastFrameId = timeBasisLastFrameId;
                        confirmationCurrentFirstFrameId = firstId;
                    }
                    verified++;
                    timeBasisLastFrameId = lastId;
                    st.lastProblem = "";
                } else {
                    st.lastProblem = "agrees; waiting for a disjoint raw-minute window";
                }
            } else if (haveTime && verified >= 2) {
                // Never replace an established VERIFIED clock with contradictory
                // time, even if two later joint candidates agree with each
                // other. Flag the disagreement and retain the trusted anchor;
                // reacquisition requires an explicit reset or loss of lock.
                accept = false;
                st.lastProblem = "candidate contradicts VERIFIED time; manual review/reacquisition needed";
            } else if (confirmedByOld) {
                verified = std::max(verified + 1, 2);
                confirmationPreviousLastFrameId = oldLastId;
                confirmationCurrentFirstFrameId = firstId;
                timeBasisLastFrameId = lastId;
                st.lastProblem = haveTime ? "time changed: confirmed by two disjoint raw-minute windows" : "";
            } else if (haveTime) {
                // Conflicts with an unverified time: trust neither until confirmed.
                accept = false;
                haveTime = false;
                verified = 0;
                timeBasisLastFrameId = 0;
                offAvgInit = false;
                st.lastProblem = "frame disagrees with unverified time; waiting for confirmation";
            } else if (r.minMargin < kStrongDecodeNats) {
                accept = false; // weak first decode: keep as candidate only
                st.lastProblem = "weak decode; waiting for confirmation";
            } else {
                verified = 1;
                timeBasisLastFrameId = lastId;
                st.lastProblem = "";
            }
        }
        if (accept) {
            double M = double(timeCodeToUnix(r.tc));
            anchorT = b59;
            anchorUtc = M + 59;
            haveTime = true;
            lastGoodT = b59;
            lastTc = r.tc;
            frameFailures = 0;
            noteFlags(r.tc, firstId, lastId);
            ev.type = DecoderEvent::Type::FrameDecoded;
            if (verified >= 2) {
                ev.utcAtBoundary = M + 59;
                ev.haveUtc = true;
            }
        } else if (!r.valid) {
            st.lastProblem = r.problems.empty() ? "invalid frame" : r.problems.front();
            // A weak frame still refreshes holdover if it fits the expected time well.
            bool consistent = false;
            if (havePred) {
                TimeCode pt = unixToTimeCode(int64_t(predMinute));
                pt.dut1Tenths = flagsConfirmed ? confirmedFlags.dut1 : lastTc.dut1Tenths;
                pt.leapSecondWarning = flagsConfirmed ? confirmedFlags.lsw : lastTc.leapSecondWarning;
                pt.dst1 = flagsConfirmed ? confirmedFlags.dst1 : lastTc.dst1;
                pt.dst2 = flagsConfirmed ? confirmedFlags.dst2 : lastTc.dst2;
                Frame pf = encodeFrame(pt);
                // excess: how much worse the expected frame fits than the best
                // per-second guesses; support: how strongly the seconds back the
                // expected symbol over their best alternative (max 1 each).
                double excess = 0, support = 0;
                for (int p = 0; p < 60; p++) {
                    if (isUnusedPos(p)) continue;
                    int e = int(pf[p]) - int(Symbol::None);
                    double mn = *std::min_element(soft[p].begin(), soft[p].end());
                    double alt = 1e300;
                    for (int h = 0; h < 4; h++) if (h != e) alt = std::min(alt, soft[p][h]);
                    excess += soft[p][e] - mn;
                    support += std::clamp(alt - soft[p][e], -1.0, 1.0);
                }
                consistent = excess <= 1.5 && support >= 30.0 && r.structureErrors <= 3;
                // An unverified time that this frame clearly contradicts is dropped.
                if (!consistent && verified < 2 && excess > 2.0) {
                    haveTime = false;
                    verified = 0;
                    timeBasisLastFrameId = 0;
                    offAvgInit = false;
                    st.lastProblem += "; unverified time dropped";
                }
                if (consistent) {
                    anchorT = b59;
                    anchorUtc = predMinute + 59;
                    lastGoodT = b59;
                    st.lastProblem += " (fits expected time)";
                }
            }
            // Retain lock for aligned but weak fields until subsequent frames can
            // contribute to a joint decode.
            bool alignedButWeak = !havePred && r.structureErrors <= 3;
            if (!consistent && !alignedButWeak) {
                frameFailures++;
                if (frameFailures >= (haveTime ? 3 : 2)) frameLocked = false;
            }
            ev.type = DecoderEvent::Type::FrameRejected;
        }
        if (r.valid && !accept) {
            ev.type = DecoderEvent::Type::FrameRejected;
            ev.decode.problems.push_back(st.lastProblem);
        }
        ev.decode.valid = accept;
        events.push_back(ev);
    }

    // Record an accepted decode's station flags; confirm them when an older
    // accepted decode over disjoint raw minutes carried the same values.
    void noteFlags(const TimeCode& tc, uint64_t firstId, uint64_t lastId)
    {
        FlagObs o{tc.dut1Tenths, tc.leapSecondWarning, tc.dst1, tc.dst2, firstId, lastId};
        auto same = [](const FlagObs& a, const FlagObs& b) {
            return a.dut1 == b.dut1 && a.lsw == b.lsw && a.dst1 == b.dst1 && a.dst2 == b.dst2;
        };
        for (const auto& old : flagObs)
            if (same(old, o) && disjointRawMinuteWindows(old.firstId, old.lastId, firstId, lastId)) {
                confirmedFlags = o;
                flagsConfirmed = true;
                break;
            }
        flagObs.push_back(o);
        while (flagObs.size() > 12) flagObs.pop_front();
    }

    void updateClockOffset(double b, double utcB)
    {
        if (!havePc) return;
        if (verified < 2 || lastState != DecoderState::Verified) return;
        double pcB = pcRef + (b - pcRefT);
        double off = pcB - (utcB + cfg.propagationDelay);
        if (!offAvgInit || std::fabs(off - offAvg) > 0.5) { offAvg = off; offAvgInit = true; }
        else offAvg = 0.8 * offAvg + 0.2 * off;
    }

    void updateState()
    {
        double age = lastSampleTime - lastGoodT;
        // Only verified time is carried through holdover; an unverified decode
        // that is not confirmed by the next frames is dropped.
        if (haveTime && (age > cfg.holdoverLimit || (verified < 2 && age > 130.0))) {
            haveTime = false;
            verified = 0;
            timeBasisLastFrameId = 0;
            offAvgInit = false;
            if (!secLocked) { scheduling = false; frameLocked = false; hist.clear(); }
        }
        DecoderState s;
        if (haveTime) {
            if (secLocked && frameLocked && age < 125.0)
                s = verified >= 2 ? DecoderState::Verified : DecoderState::Decoded;
            else
                s = DecoderState::Holdover;
        } else if (frameLocked) s = DecoderState::FrameLock;
        else if (secLocked) s = DecoderState::SecondLock;
        else s = DecoderState::Searching;

        st.state = s;
        double noise = std::sqrt(std::max(offVar, 1e-30));
        st.subcarrierSnrDb = href > 0 ? 20.0 * std::log10(href / noise) : 0.0;
        st.secondLockQuality = secLocked ? std::clamp(contrast / 0.6, 0.0, 1.0) : 0.0;
        st.secondPhase = std::fmod(phase + eAvg + 1.0, 1.0);
        st.timingJitter = std::sqrt(edgeVar);
        st.sampleClockPpm = ppm;
        st.sampleClockPpmValid = ppmValid;
        st.sampleClockPpmMeasured = dispPpmValid ? dispPpm : 0.0;
        st.sampleClockPpmSigma = dispPpmValid ? dispPpmSigma : 0.0;
        st.sampleClockPpmSettled = dispPpmValid && dispPpmSigma <= kPpmSettledSigma;
        st.tick1000 = f1000;
        st.tick1200 = f1200;
        // Tick energy above the noise reference; a station counts as heard when
        // its tick stands clearly above noise and is not a small fraction of the other.
        double s1 = std::max(0.0, f1000 - fNoise), s2 = std::max(0.0, f1200 - fNoise);
        double floorT = std::max(0.05, 3.0 * fNoise);
        bool h1 = s1 > floorT && s1 > 0.15 * s2;
        bool h2 = s2 > floorT && s2 > 0.15 * s1;
        if (!(secLocked || haveTime)) st.station = StationId::Unknown;
        else st.station = h1 && h2 ? StationId::Both : h1 ? StationId::WWV : h2 ? StationId::WWVH : StationId::Unknown;
        st.haveTime = haveTime;
        st.lastTimeCode = lastTc;
        st.flagsConfirmed = flagsConfirmed;
        st.confirmedDut1Tenths = confirmedFlags.dut1;
        st.confirmedLsw = confirmedFlags.lsw;
        st.confirmedDst1 = confirmedFlags.dst1;
        st.confirmedDst2 = confirmedFlags.dst2;
        st.verifiedFrames = verified;
        st.confirmationPreviousLastFrameId = confirmationPreviousLastFrameId;
        st.confirmationCurrentFirstFrameId = confirmationCurrentFirstFrameId;
        st.holdoverAge = haveTime ? std::max(0.0, age) : 0.0;
        if (frameLocked) st.framePosition = pos;
        else if (haveTime) {
            double u; utcAt(lastSampleTime, u);
            st.framePosition = int(std::fmod(std::floor(u), 60.0));
        } else st.framePosition = -1;
        // 1 sigma uncertainty of UTC at the receiver
        double sb = st.timingJitter / std::sqrt(2.0 / kEdgeAlpha - 1.0); // EMA averaging gain
        double unc = std::sqrt(sb * sb + cfg.propagationUncertainty * cfg.propagationUncertainty);
        if (s == DecoderState::Holdover) unc += cfg.sampleClockPpmUncertainty * 1e-6 * st.holdoverAge;
        st.timeUncertainty = haveTime ? unc : 0.0;
        st.haveClockOffset = havePc && offAvgInit && haveTime && verified >= 2;
        st.clockOffset = offAvg;
        st.clockOffsetUncertainty = std::sqrt(unc * unc + cfg.hostTimestampUncertainty * cfg.hostTimestampUncertainty);

        if (s != lastState) {
            lastState = s;
            DecoderEvent ev{};
            ev.type = DecoderEvent::Type::StateChanged;
            ev.state = s;
            events.push_back(ev);
        }
    }
};

WwvDecoder::WwvDecoder(const DecoderConfig& cfg) : d(new Impl)
{
    d->cfg = cfg;
    if (d->cfg.sampleRate < 4000) d->cfg.sampleRate = 4000;

    // Calibrate the detector delay: run a clean synthetic code through an
    // identical front end, then measure (1) where the fold kernel puts the
    // edge and (2) where the 50 % crossing is.
    FrontEnd fe;
    fe.init(d->cfg.sampleRate, d->cfg.lowpassHz);
    SecondFold f;
    f.init(2.0);
    std::deque<EnvSample> ring;
    const double fs = d->cfg.sampleRate;
    const uint64_t n = uint64_t(9 * fs);
    for (uint64_t i = 0; i < n; i++) {
        double t = i / fs, ph = t - std::floor(t);
        double on = (ph >= kPulseStart && ph < kPulseStart + 0.17) ? 1.0 : 0.0;
        double x = 1.0 + 0.5 * on * std::sin(2 * kPi * 100.0 * (ph - kPulseStart));
        double tt, xac, c;
        std::complex<double> z;
        if (fe.push(i, x, true, tt, z, xac, c) && t > 2.0) {
            f.add(tt, std::abs(z));
            ring.push_back(EnvSample{tt, z, std::abs(z), c});
        }
    }
    double hi, lo;
    double dK = wrapHalf(f.estimate(hi, lo) - kPulseStart);
    double sum = 0; int cnt = 0;
    for (int k = 4; k <= 7; k++) {
        // shift ring times so the windows line up with this second
        std::deque<EnvSample> sh;
        for (auto e : ring) { e.t -= dK; sh.push_back(e); }
        double tc, h, l;
        if (edgeCrossing(sh, double(k), [](const EnvSample& e) { return e.mag; }, tc, h, l)) {
            sum += tc + dK - (k + kPulseStart);
            cnt++;
        }
    }
    d->delay = cnt ? sum / cnt : dK;
    d->kOff = dK - d->delay;
    reset();
}

WwvDecoder::~WwvDecoder() = default;

void WwvDecoder::reset()
{
    DecoderConfig cfg = d->cfg;
    double delay = d->delay, kOff = d->kOff;
    WwvDecoder::EnvelopeTap tap = d->tap;
    void* tapUser = d->tapUser;
    d.reset(new Impl);
    d->tap = tap;
    d->tapUser = tapUser;
    d->cfg = cfg;
    d->delay = delay;
    d->kOff = kOff;
    d->fe.init(cfg.sampleRate, cfg.lowpassHz);
    const double q[2] = {0.54119610, 1.30656296};
    double fc = std::min(1600.0, 0.4 * cfg.sampleRate);
    for (int k = 0; k < 2; k++) { d->chRe[k].lowpass(fc, cfg.sampleRate, q[k]); d->chIm[k].lowpass(fc, cfg.sampleRate, q[k]); }
    d->fold.init(cfg.foldTimeConstant);
}

const DecoderStatus& WwvDecoder::status() const { return d->st; }

void WwvDecoder::setEnvelopeTap(EnvelopeTap fn, void* user)
{
    d->tap = fn;
    d->tapUser = user;
}
double WwvDecoder::detectorDelay() const { return d->delay; }
double WwvDecoder::currentSampleTime() const { return d->lastSampleTime; }
// The public UTC API must not expose a provisional DECODED hypothesis.
// Internal Impl::utcAt remains available solely for comparing candidate
// frames, never for a trusted clock, PC offset or time-labelled events.
bool WwvDecoder::utcAt(double t, double& utc) const { return d->verified >= 2 && d->utcAt(t, utc); }

void WwvDecoder::setNextSampleTimestamp(double pc)
{
    d->havePc = true;
    d->pcRef = pc;
    d->pcRefT = double(d->index) / d->cfg.sampleRate;
}

std::vector<DecoderEvent> WwvDecoder::takeEvents()
{
    std::vector<DecoderEvent> e;
    e.swap(d->events);
    return e;
}

void WwvDecoder::processIQ(const std::complex<float>* iq, size_t n)
{
    d->normalize = true;
    for (size_t i = 0; i < n; i++) {
        // +/-1.6 kHz channel filter: passes the 100 Hz code and the 1000/1200 Hz
        // ticks, keeps wideband noise out of the envelope detector.
        double re = iq[i].real(), im = iq[i].imag();
        for (int k = 0; k < 2; k++) { re = d->chRe[k].step(re); im = d->chIm[k].step(im); }
        d->processSample(std::hypot(re, im));
    }
}

void WwvDecoder::processAudio(const float* a, size_t n)
{
    d->normalize = false;
    for (size_t i = 0; i < n; i++) d->processSample(a[i]);
}

double estimateSkywaveDelay(double rxLat, double rxLon, double txLat, double txLon, int* hopsOut)
{
    const double R = 6371.0, h = 300.0, c = 299792.458, rad = kPi / 180.0;
    double p1 = rxLat * rad, p2 = txLat * rad, dl = (txLon - rxLon) * rad;
    double a = std::sin((p2 - p1) / 2), b = std::sin(dl / 2);
    double ang = 2 * std::asin(std::sqrt(a * a + std::cos(p1) * std::cos(p2) * b * b));
    double ground = R * ang;
    int hops = std::max(1, int(std::ceil(ground / 3500.0)));
    double half = ang / (2 * hops);
    double s = std::sqrt(R * R + (R + h) * (R + h) - 2 * R * (R + h) * std::cos(half));
    if (hopsOut) *hopsOut = hops;
    return 2 * hops * s / c;
}

} // namespace ahrwwv
