// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
// AHR WWV/WWVH Time Decoder - portable streaming decoder engine.
//
// Feed it complex baseband IQ (AM carrier near 0 Hz) or already
// AM-demodulated audio. It recovers the 100 Hz time code subcarrier, locks to
// the second, classifies each second, finds the minute frame, decodes it,
// verifies it against the next frame, and keeps time through fades (holdover).
//
// It never touches the system clock. "PC clock offset" is computed only when
// the host supplies a capture timestamp for the samples.
//
// No Qt or SDRangel dependency: C++17 standard library only. Not thread safe;
// call from one thread (the SDRangel channel sink thread).
#pragma once

#include "ahrwwv/timecode.h"

#include <array>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace ahrwwv {

struct DecoderConfig {
    double sampleRate = 48000.0;      // input rate, >= 4000 Hz
    double lowpassHz = 25.0;          // 100 Hz detector post-mix bandwidth
    double foldTimeConstant = 6.0;    // seconds, second-phase averaging
    // Propagation: transmitter-to-receiver delay to remove from the PC offset.
    double propagationDelay = 0.0;    // seconds
    double propagationUncertainty = 0.005; // seconds (1 sigma), HF skywave
    // Host timestamp uncertainty (buffering between ADC and the timestamp).
    double hostTimestampUncertainty = 0.020; // seconds (1 sigma)
    // Free-running sample clock uncertainty used to grow holdover error.
    double sampleClockPpmUncertainty = 20.0;
    double holdoverLimit = 3600.0;    // seconds of holdover before giving up
};

enum class DecoderState {
    Searching,     // no second lock
    SecondLock,    // seconds found, minute frame not yet found
    FrameLock,     // minute frame position found, waiting for a full frame
    Decoded,       // one valid frame (not yet confirmed by a second frame)
    Verified,      // two or more consecutive frames agree
    Holdover       // signal lost/unreadable; time carried by the sample clock
};
const char* decoderStateName(DecoderState s);

enum class StationId { Unknown, WWV, WWVH, Both };
const char* stationName(StationId s);

// Consecutive raw-minute observations must have unique adjacent IDs and
// boundaries 60 seconds apart. These helpers are exercised by tests and
// used in independent-confirmation logic (never inferred from decoded UTC).
bool consecutiveRawMinutes(const std::vector<uint64_t>& ids, const std::vector<double>& b59);
bool disjointRawMinuteWindows(uint64_t firstA, uint64_t lastA,
                              uint64_t firstB, uint64_t lastB);

struct SecondSymbol {
    double boundaryTime = 0.0;  // sample-clock time of the second's start (s)
    int framePosition = -1;     // 0..59 once frame-locked, else -1
    Symbol symbol = Symbol::Unknown; // hard decision
    double confidence = 0.0;    // 0..1
    std::array<double, 3> levels{}; // normalized levels in windows A,B,C
};

struct DecoderStatus {
    DecoderState state = DecoderState::Searching;
    // Signal
    double subcarrierSnrDb = 0.0;   // 100 Hz pulse-on vs pulse-off energy
    double secondLockQuality = 0.0; // 0..1
    double secondPhase = 0.0;       // sample-clock seconds of the second boundary (mod 1)
    double timingJitter = 0.0;      // 1 sigma estimate of second-boundary jitter (s)
    double sampleClockPpm = 0.0;    // measured sample clock error vs. UTC
    bool sampleClockPpmValid = false;
    // For display: the same measurement fitted robustly (jumped edges
    // removed), with its 1-sigma error. Positive = the sample clock runs fast.
    // Show the number only when Settled; otherwise "measuring".
    double sampleClockPpmMeasured = 0.0;
    double sampleClockPpmSigma = 0.0;
    bool sampleClockPpmSettled = false;
    StationId station = StationId::Unknown;
    double tick1000 = 0.0, tick1200 = 0.0; // relative tick tone energies
    // Frame
    int framePosition = -1;          // current second within the minute
    std::array<SecondSymbol, 60> frame{}; // symbols of the frame in progress, by position
    std::array<SecondSymbol, 60> lastFrame{}; // last complete frame
    // Time
    bool haveTime = false;           // a decoded (or holdover) time is available
    TimeCode lastTimeCode;           // most recent valid frame contents
    // DUT1, leap-second warning and DST, as confirmed by two accepted decodes
    // that share no raw minute. One faded second can flip one of these bits
    // in a single decode (live 15 MHz, 2026-11-06: DUT1 shown +0.0 for one
    // minute while the station sent -0.1), so displays should use these
    // rather than lastTimeCode's copies. A change (DUT1 step, DST day, leap
    // second warning) shows once two independent decodes report it.
    bool flagsConfirmed = false;
    int confirmedDut1Tenths = 0;
    bool confirmedLsw = false, confirmedDst1 = false, confirmedDst2 = false;
    double frameConfidence = 0.0;    // 0..1 of most recent decode attempt
    int verifiedFrames = 0;          // independent agreeing frames
    // Raw minute IDs used for the most recent independent confirmation.
    // Zero means no independent confirmation yet.
    uint64_t confirmationPreviousLastFrameId = 0;
    uint64_t confirmationCurrentFirstFrameId = 0;
    std::string lastProblem;         // why the last frame was rejected
    double holdoverAge = 0.0;        // seconds since last good frame (Holdover)
    double timeUncertainty = 0.0;    // 1 sigma of UTC at receiver, excl. host (s)
    // PC clock comparison (only if host timestamps supplied)
    bool haveClockOffset = false;
    double clockOffset = 0.0;        // PC clock minus UTC (s); positive = PC fast
    double clockOffsetUncertainty = 0.0; // 1 sigma (s)
    uint64_t secondsProcessed = 0;
};

// Event emitted once per second boundary and once per frame attempt.
struct DecoderEvent {
    enum class Type { Second, FrameDecoded, FrameRejected, StateChanged } type;
    SecondSymbol second;          // for Second
    DecodeResult decode;          // for Frame*
    DecoderState state = DecoderState::Searching;
    double utcAtBoundary = 0.0;   // Unix UTC (s) of the boundary just passed, if haveTime
    bool haveUtc = false; // Only trusted VERIFIED/HOLDOVER seconds may carry UTC
};

class WwvDecoder {
public:
    explicit WwvDecoder(const DecoderConfig& cfg = DecoderConfig());
    ~WwvDecoder();

    void reset();

    // Supply the PC (wall) clock time, Unix seconds, at which the NEXT sample
    // passed to process*() was captured. Optional; enables clock offset.
    void setNextSampleTimestamp(double pcUnixSeconds);

    void processIQ(const std::complex<float>* iq, size_t n);
    void processAudio(const float* audio, size_t n);

    const DecoderStatus& status() const;
    // UTC (Unix seconds) at a given sample-clock time, if time is known.
    bool utcAt(double sampleTime, double& utc) const;
    double currentSampleTime() const;
    // Convenience: UTC at the most recent sample.
    bool currentUtc(double& utc) const { return utcAt(currentSampleTime(), utc); }

    std::vector<DecoderEvent> takeEvents();

    // Group delay of the 100 Hz detector, measured at construction (s).
    double detectorDelay() const;

    // Optional diagnostic tap, called for every ~1 ms envelope sample with the
    // sample-clock time and the 100 Hz level (coherent once locked) normalized
    // so that a pulse is ~1.0. Used by the GUI's envelope plot. Pass nullptr
    // to remove. Survives reset().
    using EnvelopeTap = void (*)(void* user, double sampleTime, float level);
    void setEnvelopeTap(EnvelopeTap fn, void* user);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Rough HF one-way delay (s) from a transmitter to the receiver, assuming an
// F2 layer at ~300 km and the minimum number of hops of <= 3500 km.
double estimateSkywaveDelay(double rxLatDeg, double rxLonDeg, double txLatDeg, double txLonDeg, int* hopsOut = nullptr);
constexpr double kWwvLat = 40.6781, kWwvLon = -105.0469;
constexpr double kWwvhLat = 21.9875, kWwvhLon = -159.7625;

} // namespace ahrwwv
