#pragma once
// Turns jitter-buffer fill level into a playback-rate ratio, to absorb clock drift between the
// sender's and receiver's audio clocks (typically tens of ppm; a few hundred at worst).
//
//   ratio > 1  -> consume input faster (buffer is too full)
//   ratio < 1  -> consume input slower (buffer is draining)
//
// Feed it bufferedFrames() once per audio callback, then drive an interpolating resampler with ratio().
//
// Design: the buffer fill integrates (supply - consumption), so with a PI controller the closed loop is
// the second-order system  s^2 + a*kp*s + a*ki = 0  (a = callbackFrames / targetFrames). We pick the
// gains from a natural frequency (settle time) and critical damping, so behaviour doesn't depend on
// callback size or sample rate. The fill measurement is low-pass filtered first, because it moves in
// whole-packet steps and carries network jitter.
//
// Known limit: fill is only observed in whole-packet steps (at 100 ppm drift and 128-frame packets, one
// step per ~27 s), so the instantaneous ratio wobbles around the true value (100-300 ppm peak-to-peak,
// i.e. under half a cent) while its average is exact. If that ever matters, estimate drift from packet
// arrival times against the local sample clock instead - that measurement is continuous.
// Pure C++, header-only, unit-tested.
#include <algorithm>
#include <cmath>

namespace pb::net
{
class DriftController
{
public:
    /** @param settleSeconds  loop time constant. Measured (see Tests): 30 s tracks +/-450 ppm with no underruns and
                              the time-averaged ratio within 1 ppm of the true drift. 8 s finds the same average but
                              wobbles +/-250 ppm; 60 s is too slow to follow large offsets and can underrun. */
    DriftController (double targetFrames, double sampleRate = 48000.0, double settleSeconds = 30.0, double maxPpm = 500.0)
        : target (targetFrames), sr (sampleRate), tau (settleSeconds), limit (maxPpm * 1e-6) {}

    void setTarget (double t) { target = t; }

    void update (double fillFrames, int callbackFrames)
    {
        const double dt = callbackFrames / sr;                       // seconds this callback represents
        smoothed += (1.0 - std::exp (-dt / 1.0)) * (fillFrames - smoothed);   // 1 s measurement filter

        const double a = callbackFrames / std::max (1.0, target);    // fill moves by a * (supply - rate) per callback
        const double wn = dt / tau;                                  // natural frequency, rad per callback
        const double kp = 2.0 * wn / a;                              // zeta = 1
        const double ki = wn * wn / a;

        const double err = (smoothed - target) / std::max (1.0, target);
        integral = std::clamp (integral + ki * err, -limit, limit);  // clamped: anti-windup
        ratio_ = 1.0 + std::clamp (kp * err + integral, -limit, limit);
    }

    double ratio() const noexcept { return ratio_; }
    double smoothedFill() const noexcept { return smoothed; }
    void reset (double fill) { smoothed = fill; integral = 0.0; ratio_ = 1.0; }

private:
    double target, sr, tau, limit;
    double smoothed = 0.0, integral = 0.0, ratio_ = 1.0;
};
}
