// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
// AHR WWV/WWVH Time Decoder - portable time code layer (no Qt, no SDRangel).
// Frame format per NIST SP 250-67 (WWV/WWVH 100 Hz IRIG-H style time code).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ahrwwv {

// One second of the time code.
enum class Symbol : uint8_t {
    None = 0,    // second 0: no 100 Hz pulse (frame reference)
    Zero = 1,    // 170 ms pulse
    One = 2,     // 470 ms pulse
    Marker = 3,  // 770 ms pulse (seconds 9,19,29,39,49,59)
    Unknown = 4  // not received / undecidable
};

char symbolChar(Symbol s); // ' ', '0', '1', 'M', '?'

// Content of one 60-second frame. The frame describes the UTC minute that
// begins at second 0 of the frame.
struct TimeCode {
    int year = 0;          // full year, 2000-2099
    int dayOfYear = 1;     // 1..366
    int hour = 0;          // 0..23 UTC
    int minute = 0;        // 0..59
    int dut1Tenths = 0;    // UT1-UTC in tenths of a second, -7..+7
    bool leapSecondWarning = false;
    bool dst1 = false;     // bit 55: set at 0000 UTC on the day DST starts
    bool dst2 = false;     // bit 2:  set 24 h after dst1
};

enum class DstState { Standard, Daylight, BeginsToday, EndsToday };
DstState dstState(const TimeCode& tc);
const char* dstStateName(DstState s);

using Frame = std::array<Symbol, 60>;

// Encode a time code into the 60 symbols that WWV/WWVH transmit.
Frame encodeFrame(const TimeCode& tc);

struct DecodeResult {
    bool valid = false;
    TimeCode tc;
    std::vector<std::string> problems; // human readable reasons when invalid
    int unknownSymbols = 0;            // number of Unknown symbols in the frame
    // Soft decoding only:
    double minMargin = 0.0;            // weakest field: cost gap to next-best value
    std::string weakestField;
    int structureErrors = 0;           // seconds whose best guess breaks the frame structure
    int framesUsed = 1;                // consecutive frames read together (decodeFramesSoft)
};

// Per-second hypothesis costs, index 0=None 1=Zero 2=One 3=Marker (lower = better;
// a clean second has cost 0 for the true symbol and >= 1 for the others).
using SoftFrame = std::array<std::array<double, 4>, 60>;

// Maximum-likelihood decode: each field is chosen among its legal values only
// (minute 0-59, hour 0-23, day 1-365/366, ...). The frame is valid when the
// marker/second-0/unused-bit structure holds (<= 3 wrong-kind pulses;
// a faded missing pulse is treated as an erasure) and every
// field beats its runner-up by at least minFieldMargin.
//
// Field choices and margins use calibrated log-likelihoods (nats, see
// calibrateSoftFrame), so minFieldMargin is a log-likelihood ratio: 4 nats
// means the chosen value is about 55 times likelier than the runner-up.
constexpr double kDefaultFieldMarginNats = 4.0;
DecodeResult decodeFrameSoft(const SoftFrame& cost, double minFieldMargin = kDefaultFieldMarginNats);

// Turn one frame's raw per-second costs into calibrated negative
// log-likelihoods (nats), lowest hypothesis 0 in every second:
//  - scale: raw costs are squared errors; dividing by 2 sigma^2, with sigma^2
//    estimated from this frame's own median best cost, puts frames with
//    different noise on one scale, so a joint decode weighs each minute by
//    how clean it was and log-likelihoods simply add across minutes. (The
//    old rule asked for a margin of 2 sigma in raw units, which is 1/sigma in
//    nats: noisier minutes needed LESS evidence.)
//  - outliers: on live high-latitude recordings about 1 data bit in 80 was
//    wrong with a raw ratio far beyond what Gaussian noise allows (static
//    crashes, carrier-fade spikes), so no single second may count for more
//    than -ln(kOutlierProb), about 4.6 nats.
// Checked on 1248 data bits from 24 live frames (Palmer AK, 5 and 10 MHz,
// 2026-11-05) against the known transmitted time code: after scaling, the
// raw ratio needs no further gain (fitted slope 1.03). A 0-versus-1 bias
// term also fitted on those frames (raw evidence favoured "1") but was not
// stable between recordings and caused wrong hours in simulated flutter, so
// it is deliberately not applied.
SoftFrame calibrateSoftFrame(const SoftFrame& raw);
constexpr double kOutlierProb = 0.01;

// Joint maximum-likelihood decode of 2 or more consecutive frames (oldest
// first) that carry consecutive UTC minutes; the result is the newest frame's
// time code. On HF a fade can wipe out a few bits of one minute; the same
// fields in the neighbouring minutes are then read together, so one faded
// minute no longer blocks the decode. Time of day and date are each chosen
// over all frames (with the hour/day/year carries in between); DUT1, leap
// second warning and DST bits are assumed unchanged over the span. Validity
// rules are the same as decodeFrameSoft, applied per frame for structure.
DecodeResult decodeFramesSoft(const std::vector<SoftFrame>& frames, double minFieldMargin = kDefaultFieldMarginNats);

// Decode a full frame. Rejects frames with bad markers, a pulse in second 0,
// non-zero unused bits, impossible BCD digits, or out-of-range fields.
DecodeResult decodeFrame(const Frame& f);

// Calendar helpers (proleptic Gregorian, UTC, no leap seconds).
bool isLeapYear(int year);
void dayOfYearToDate(int year, int doy, int& month, int& day);
int dateToDayOfYear(int year, int month, int day);
int64_t daysFromCivil(int y, unsigned m, unsigned d);
// UTC seconds since 1970-01-01 for the start of the frame's minute.
int64_t timeCodeToUnix(const TimeCode& tc);
TimeCode unixToTimeCode(int64_t unixSeconds); // flags/DUT1 left at defaults
std::string formatUnix(double unixSeconds, bool withMillis = true); // "2026-11-02 15:17:30.123"

} // namespace ahrwwv
