// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
#include "ahrwwv/timecode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ahrwwv {

namespace {

// Bit positions (seconds) of each field, least significant bit first.
const int kYearUnits[] = {4, 5, 6, 7};
const int kMinuteUnits[] = {10, 11, 12, 13};
const int kMinuteTens[] = {15, 16, 17};
const int kHourUnits[] = {20, 21, 22, 23};
const int kHourTens[] = {25, 26};
const int kDoyUnits[] = {30, 31, 32, 33};
const int kDoyTens[] = {35, 36, 37, 38};
const int kDoyHundreds[] = {40, 41};
const int kYearTens[] = {51, 52, 53, 54};
const int kDut1[] = {56, 57, 58};
const int kDst2 = 2;
const int kLsw = 3;
const int kUt1Sign = 50;
const int kDst1 = 55;

bool isMarkerSecond(int s) { return s % 10 == 9; }

// Seconds 1, 8, 14, 18, 24, 27, 28, 34 and 42-48 are listed as unused by
// NIST SP 250-67. WWV transmits 0 there, but WWVH was observed sending 1 at
// second 47 in every minute (15 MHz, Palmer AK, 2026-11-04), so the decoders
// accept either value there and take no data from them.

template <size_t N>
void putBcd(Frame& f, const int (&pos)[N], int value)
{
    for (size_t i = 0; i < N; i++)
        f[pos[i]] = ((value >> i) & 1) ? Symbol::One : Symbol::Zero;
}

template <size_t N>
int getBcd(const Frame& f, const int (&pos)[N], bool& bad)
{
    int v = 0;
    for (size_t i = 0; i < N; i++) {
        Symbol s = f[pos[i]];
        if (s == Symbol::One) v |= (1 << i);
        else if (s != Symbol::Zero) bad = true;
    }
    return v;
}

} // namespace

char symbolChar(Symbol s)
{
    switch (s) {
    case Symbol::None: return '_';
    case Symbol::Zero: return '0';
    case Symbol::One: return '1';
    case Symbol::Marker: return 'M';
    default: return '?';
    }
}

DstState dstState(const TimeCode& tc)
{
    if (tc.dst1 && tc.dst2) return DstState::Daylight;
    if (!tc.dst1 && !tc.dst2) return DstState::Standard;
    if (tc.dst1) return DstState::BeginsToday;
    return DstState::EndsToday;
}

const char* dstStateName(DstState s)
{
    switch (s) {
    case DstState::Standard: return "Standard time";
    case DstState::Daylight: return "Daylight time";
    case DstState::BeginsToday: return "DST begins today";
    case DstState::EndsToday: return "DST ends today";
    }
    return "?";
}

Frame encodeFrame(const TimeCode& tc)
{
    Frame f;
    f.fill(Symbol::Zero);
    f[0] = Symbol::None;
    for (int s = 9; s < 60; s += 10) f[s] = Symbol::Marker;
    int y2 = tc.year % 100;
    putBcd(f, kYearUnits, y2 % 10);
    putBcd(f, kYearTens, y2 / 10);
    putBcd(f, kMinuteUnits, tc.minute % 10);
    putBcd(f, kMinuteTens, tc.minute / 10);
    putBcd(f, kHourUnits, tc.hour % 10);
    putBcd(f, kHourTens, tc.hour / 10);
    putBcd(f, kDoyUnits, tc.dayOfYear % 10);
    putBcd(f, kDoyTens, (tc.dayOfYear / 10) % 10);
    putBcd(f, kDoyHundreds, tc.dayOfYear / 100);
    putBcd(f, kDut1, std::abs(tc.dut1Tenths));
    f[kUt1Sign] = tc.dut1Tenths >= 0 ? Symbol::One : Symbol::Zero;
    f[kLsw] = tc.leapSecondWarning ? Symbol::One : Symbol::Zero;
    f[kDst1] = tc.dst1 ? Symbol::One : Symbol::Zero;
    f[kDst2] = tc.dst2 ? Symbol::One : Symbol::Zero;
    return f;
}

DecodeResult decodeFrame(const Frame& f)
{
    DecodeResult r;
    for (Symbol s : f)
        if (s == Symbol::Unknown) r.unknownSymbols++;
    if (r.unknownSymbols)
        r.problems.push_back(std::to_string(r.unknownSymbols) + " unreadable second(s)");

    if (f[0] != Symbol::None && f[0] != Symbol::Unknown)
        r.problems.push_back("pulse present in second 0");
    for (int s = 1; s < 60; s++) {
        if (isMarkerSecond(s)) {
            if (f[s] != Symbol::Marker && f[s] != Symbol::Unknown)
                r.problems.push_back("missing position marker at second " + std::to_string(s));
        } else if (f[s] == Symbol::Marker) {
            r.problems.push_back("unexpected marker at second " + std::to_string(s));
        } else if (f[s] == Symbol::None) {
            r.problems.push_back("missing pulse at second " + std::to_string(s));
        }
    }

    bool bad = false;
    int yu = getBcd(f, kYearUnits, bad), yt = getBcd(f, kYearTens, bad);
    int mu = getBcd(f, kMinuteUnits, bad), mt = getBcd(f, kMinuteTens, bad);
    int hu = getBcd(f, kHourUnits, bad), ht = getBcd(f, kHourTens, bad);
    int du = getBcd(f, kDoyUnits, bad), dt = getBcd(f, kDoyTens, bad), dh = getBcd(f, kDoyHundreds, bad);
    int dut = getBcd(f, kDut1, bad);
    bool flagBad = false;
    auto flag = [&](int s) {
        if (f[s] == Symbol::One) return true;
        if (f[s] != Symbol::Zero) flagBad = true;
        return false;
    };
    bool sign = flag(kUt1Sign);
    r.tc.leapSecondWarning = flag(kLsw);
    r.tc.dst1 = flag(kDst1);
    r.tc.dst2 = flag(kDst2);
    if (bad || flagBad) r.problems.push_back("data bit not readable as 0/1");

    if (yu > 9 || yt > 9 || mu > 9 || hu > 9 || du > 9 || dt > 9)
        r.problems.push_back("BCD digit greater than 9");
    if (mt > 5) r.problems.push_back("minute tens > 5");
    if (ht > 2) r.problems.push_back("hour tens > 2");
    if (dh > 3) r.problems.push_back("day-of-year hundreds > 3");

    r.tc.year = 2000 + yt * 10 + yu;
    r.tc.minute = mt * 10 + mu;
    r.tc.hour = ht * 10 + hu;
    r.tc.dayOfYear = dh * 100 + dt * 10 + du;
    r.tc.dut1Tenths = sign ? dut : -dut;

    if (r.tc.minute > 59) r.problems.push_back("minute out of range");
    if (r.tc.hour > 23) r.problems.push_back("hour out of range");
    int maxDoy = isLeapYear(r.tc.year) ? 366 : 365;
    if (r.tc.dayOfYear < 1 || r.tc.dayOfYear > maxDoy) r.problems.push_back("day of year out of range");

    r.valid = r.problems.empty();
    return r;
}

SoftFrame calibrateSoftFrame(const SoftFrame& raw)
{
    // Noise scale of this frame: the true hypothesis' cost is a weighted sum
    // of three squared errors (chi-square, 3 dof: median = 2.366 sigma^2).
    double best[60];
    for (int p = 0; p < 60; p++) best[p] = *std::min_element(raw[p].begin(), raw[p].end());
    std::nth_element(best, best + 30, best + 60);
    // Floor: a perfectly clean (synthetic) frame must not divide by zero; at
    // the floor a clean second's runner-up is still worth the full outlier cap.
    const double sigma2 = std::max(best[30] / 2.366, 0.01);
    const double cap = -std::log(kOutlierProb);
    SoftFrame out;
    for (int p = 0; p < 60; p++) {
        double x[4];
        for (int h = 0; h < 4; h++) x[h] = raw[p][h] / (2.0 * sigma2);
        double m = *std::min_element(x, x + 4);
        for (int h = 0; h < 4; h++) {
            double e = x[h] - m; // >= 0
            // -ln((1-eps) exp(-e) + eps), shifted so the best hypothesis is 0.
            double v = -std::log((1.0 - kOutlierProb) * std::exp(-e) + kOutlierProb);
            out[p][h] = std::min(v, cap);
        }
    }
    return out;
}

DecodeResult decodeFrameSoft(const SoftFrame& raw, double minFieldMargin)
{
    DecodeResult r;
    const SoftFrame c = calibrateSoftFrame(raw);
    // Only count a clear contradiction as a structure error. Weak skywave
    // pulses can marginally favor a forbidden symbol without being reliable.
    // Structure is judged on the raw costs, exactly as before calibration.
    const double kStructMargin = 0.5;
    for (int p = 0; p < 60; p++) {
        bool allowed[4] = {false, false, false, false};
        if (p == 0) allowed[0] = true;
        else if (isMarkerSecond(p)) allowed[3] = true;
        else { allowed[1] = true; allowed[2] = true; } // data or unused bit
        double bestAllowed = 1e300, bestForbidden = 1e300;
        for (int h = 0; h < 4; h++) {
            if (p != 0 && h == 0) continue; // faded missing pulse = erasure
            if (allowed[h]) bestAllowed = std::min(bestAllowed, raw[p][h]);
            else bestForbidden = std::min(bestForbidden, raw[p][h]);
        }
        if (bestForbidden + kStructMargin < bestAllowed) r.structureErrors++;
    }
    auto bitCost = [&](int p, int bit) { return c[p][bit ? 2 : 1]; };
    // cost of writing `value` into the listed bit positions as BCD
    auto bcdCost = [&](const int* pos, int n, int value) {
        double s = 0;
        for (int i = 0; i < n; i++) s += bitCost(pos[i], (value >> i) & 1);
        return s;
    };
    r.minMargin = 1e300;
    auto pick = [&](const char* name, int lo, int hi, auto costFn) {
        double best = 1e300, second = 1e300;
        int bv = lo;
        for (int v = lo; v <= hi; v++) {
            double x = costFn(v);
            if (x < best) { second = best; best = x; bv = v; }
            else if (x < second) second = x;
        }
        double m = second - best;
        if (m < r.minMargin) { r.minMargin = m; r.weakestField = name; }
        return bv;
    };
    int y2 = pick("year", 0, 99, [&](int v) {
        return bcdCost(kYearUnits, 4, v % 10) + bcdCost(kYearTens, 4, v / 10);
    });
    r.tc.year = 2000 + y2;
    r.tc.minute = pick("minute", 0, 59, [&](int v) {
        return bcdCost(kMinuteUnits, 4, v % 10) + bcdCost(kMinuteTens, 3, v / 10);
    });
    r.tc.hour = pick("hour", 0, 23, [&](int v) {
        return bcdCost(kHourUnits, 4, v % 10) + bcdCost(kHourTens, 2, v / 10);
    });
    r.tc.dayOfYear = pick("day of year", 1, isLeapYear(r.tc.year) ? 366 : 365, [&](int v) {
        return bcdCost(kDoyUnits, 4, v % 10) + bcdCost(kDoyTens, 4, (v / 10) % 10) + bcdCost(kDoyHundreds, 2, v / 100);
    });
    r.tc.dut1Tenths = pick("DUT1", -7, 7, [&](int v) {
        double plus = bitCost(kUt1Sign, 1) + bcdCost(kDut1, 3, v);
        double minus = bitCost(kUt1Sign, 0) + bcdCost(kDut1, 3, -v);
        if (v > 0) return plus;
        if (v < 0) return minus;
        return std::min(plus, minus);
    });
    r.tc.leapSecondWarning = pick("leap second warning", 0, 1, [&](int v) { return bitCost(kLsw, v); });
    r.tc.dst1 = pick("DST1", 0, 1, [&](int v) { return bitCost(kDst1, v); });
    r.tc.dst2 = pick("DST2", 0, 1, [&](int v) { return bitCost(kDst2, v); });

    if (r.structureErrors > 3)
        r.problems.push_back(std::to_string(r.structureErrors) + " seconds break the frame structure");
    // Margins are log-likelihood ratios: this frame's noise is already in them.
    if (r.minMargin < minFieldMargin)
        r.problems.push_back(r.weakestField + " too weak to read");
    r.valid = r.problems.empty();
    return r;
}

namespace {

int structureErrorsOf(const SoftFrame& c)
{
    const double kStructMargin = 0.5;
    int n = 0;
    for (int p = 0; p < 60; p++) {
        bool allowed[4] = {false, false, false, false};
        if (p == 0) allowed[0] = true;
        else if (isMarkerSecond(p)) allowed[3] = true;
        else { allowed[1] = true; allowed[2] = true; }
        double bestAllowed = 1e300, bestForbidden = 1e300;
        for (int h = 0; h < 4; h++) {
            if (p != 0 && h == 0) continue; // erasure, not structure error
            if (allowed[h]) bestAllowed = std::min(bestAllowed, c[p][h]);
            else bestForbidden = std::min(bestForbidden, c[p][h]);
        }
        if (bestForbidden + kStructMargin < bestAllowed) n++;
    }
    return n;
}

template <size_t N>
double bcdCostOf(const SoftFrame& c, const int (&pos)[N], int value)
{
    double s = 0;
    for (size_t i = 0; i < N; i++) s += c[pos[i]][((value >> i) & 1) ? 2 : 1];
    return s;
}

} // namespace

DecodeResult decodeFramesSoft(const std::vector<SoftFrame>& raw, double minFieldMargin)
{
    if (raw.size() == 1) return decodeFrameSoft(raw[0], minFieldMargin);
    DecodeResult r;
    const int K = int(raw.size());
    if (K == 0) { r.problems.push_back("no frames"); return r; }
    r.framesUsed = K;
    int worstStructure = 0;
    for (auto& c : raw) worstStructure = std::max(worstStructure, structureErrorsOf(c));
    // Each frame is calibrated on its own noise, so a noisy minute adds less
    // than a clean one and log-likelihoods simply add across frames.
    std::vector<SoftFrame> f;
    f.reserve(raw.size());
    for (auto& c : raw) f.push_back(calibrateSoftFrame(c));
    r.structureErrors = worstStructure;
    r.minMargin = 1e300;
    auto note = [&](const char* name, double best, double second) {
        double m = second - best;
        if (m < r.minMargin) { r.minMargin = m; r.weakestField = name; }
    };

    // Time of day of the newest frame, 0..1439 minutes; frame k is K-1-k
    // minutes earlier.
    double bestT = 1e300, secondT = 1e300;
    int mod = 0;
    for (int m = 0; m < 1440; m++) {
        double s = 0;
        for (int k = 0; k < K; k++) {
            int mk = ((m - (K - 1 - k)) % 1440 + 1440) % 1440;
            int hh = mk / 60, mm = mk % 60;
            s += bcdCostOf(f[k], kMinuteUnits, mm % 10) + bcdCostOf(f[k], kMinuteTens, mm / 10)
               + bcdCostOf(f[k], kHourUnits, hh % 10) + bcdCostOf(f[k], kHourTens, hh / 10);
        }
        if (s < bestT) { secondT = bestT; bestT = s; mod = m; }
        else if (s < secondT) secondT = s;
    }
    note("time of day", bestT, secondT);
    r.tc.hour = mod / 60;
    r.tc.minute = mod % 60;
    // Frames before midnight (for the chosen time of day) carry the day before.
    int framesBefore = std::max(0, (K - 1) - mod); // frames k < framesBefore are on the previous day

    // Date of the newest frame: every day 2000-01-01 .. 2099-12-31.
    const int64_t d0 = daysFromCivil(2000, 1, 1), d1 = daysFromCivil(2099, 12, 31);
    auto dateCost = [&](const SoftFrame& c, int year, int doy) {
        int y2 = year % 100;
        return bcdCostOf(c, kYearUnits, y2 % 10) + bcdCostOf(c, kYearTens, y2 / 10)
             + bcdCostOf(c, kDoyUnits, doy % 10) + bcdCostOf(c, kDoyTens, (doy / 10) % 10)
             + bcdCostOf(c, kDoyHundreds, doy / 100);
    };
    double bestD = 1e300, secondD = 1e300;
    int bestYear = 2000, bestDoy = 1;
    int py = 1999, pdoy = 365; // the day before d (1999-12-31 for the first)
    int y = 2000, doy = 1;
    for (int64_t d = d0; d <= d1; d++) {
        double s = 0;
        for (int k = 0; k < K; k++)
            s += k < framesBefore ? dateCost(f[k], py, pdoy) : dateCost(f[k], y, doy);
        if (s < bestD) { secondD = bestD; bestD = s; bestYear = y; bestDoy = doy; }
        else if (s < secondD) secondD = s;
        py = y; pdoy = doy;
        if (++doy > (isLeapYear(y) ? 366 : 365)) { doy = 1; y++; }
    }
    note("date", bestD, secondD);
    r.tc.year = bestYear;
    r.tc.dayOfYear = bestDoy;

    // Flags and DUT1: same in every frame.
    auto sumBit = [&](int pos, int v) { double s = 0; for (auto& c : f) s += c[pos][v ? 2 : 1]; return s; };
    {
        double best = 1e300, second = 1e300;
        int bv = 0;
        for (int v = -7; v <= 7; v++) {
            double plus = 0, minus = 0;
            for (auto& c : f) {
                plus += c[kUt1Sign][2] + bcdCostOf(c, kDut1, std::abs(v));
                minus += c[kUt1Sign][1] + bcdCostOf(c, kDut1, std::abs(v));
            }
            double x = v > 0 ? plus : v < 0 ? minus : std::min(plus, minus);
            if (x < best) { second = best; best = x; bv = v; }
            else if (x < second) second = x;
        }
        note("DUT1", best, second);
        r.tc.dut1Tenths = bv;
    }
    auto flag = [&](const char* name, int pos) {
        double c0 = sumBit(pos, 0), c1 = sumBit(pos, 1);
        note(name, std::min(c0, c1), std::max(c0, c1));
        return c1 < c0;
    };
    r.tc.leapSecondWarning = flag("leap second warning", kLsw);
    r.tc.dst1 = flag("DST1", kDst1);
    r.tc.dst2 = flag("DST2", kDst2);

    if (worstStructure > 3)
        r.problems.push_back(std::to_string(worstStructure) + " seconds break the frame structure");
    if (r.minMargin < minFieldMargin)
        r.problems.push_back(r.weakestField + " too weak to read");
    r.valid = r.problems.empty();
    return r;
}

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

void dayOfYearToDate(int year, int doy, int& month, int& day)
{
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = doy;
    for (int m = 0; m < 12; m++) {
        int n = mdays[m] + (m == 1 && isLeapYear(year) ? 1 : 0);
        if (d <= n) { month = m + 1; day = d; return; }
        d -= n;
    }
    month = 12; day = 31;
}

int dateToDayOfYear(int year, int month, int day)
{
    return int(daysFromCivil(year, month, day) - daysFromCivil(year, 1, 1)) + 1;
}

// Howard Hinnant's days_from_civil.
int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

int64_t timeCodeToUnix(const TimeCode& tc)
{
    int64_t days = daysFromCivil(tc.year, 1, 1) + (tc.dayOfYear - 1);
    return days * 86400 + tc.hour * 3600 + tc.minute * 60;
}

TimeCode unixToTimeCode(int64_t t)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t sod = t - days * 86400;
    // civil_from_days
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = int(yoe + era * 400);
    const unsigned doyM = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doyM + 2) / 153;
    const unsigned d = doyM - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    TimeCode tc;
    tc.year = y;
    tc.dayOfYear = dateToDayOfYear(y, int(m), int(d));
    tc.hour = int(sod / 3600);
    tc.minute = int((sod / 60) % 60);
    return tc;
}

std::string formatUnix(double t, bool withMillis)
{
    double fl = std::floor(t);
    int64_t s = int64_t(fl);
    int ms = int(std::lround((t - fl) * 1000.0));
    if (ms >= 1000) { ms -= 1000; s += 1; }
    TimeCode tc = unixToTimeCode(s);
    int mon, day;
    dayOfYearToDate(tc.year, tc.dayOfYear, mon, day);
    int sec = int(((s % 60) + 60) % 60);
    char buf[64];
    if (withMillis)
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tc.year, mon, day, tc.hour, tc.minute, sec, ms);
    else
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", tc.year, mon, day, tc.hour, tc.minute, sec);
    return buf;
}

} // namespace ahrwwv
