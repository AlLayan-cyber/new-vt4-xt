// Halden VT-4 — reference models MPEQ and PSEQ.
//
// In these models the tube / transformer / power-supply topology is out of
// the signal path: the plugin is a linear equaliser at the oversampled rate.
//
//   HPF ─ LOW shelf ─ BODY ─ PRESENCE ─ AIR shelf
//
// LOW and AIR take the shape of the reference unit, fitted to sweeps of it:
//   MPEQ (passive-style mastering EQ): first-order shelves,
//        LOW geometric centre = 4 · knob, AIR centre = 0.55 · knob
//   PSEQ (Prism-style mastering EQ): second-order shelves,
//        LOW f0 = 2.28 · knob, Q 0.42;  AIR f0 = 0.55 · knob, Q 0.49
// HPF, BODY and PRESENCE keep the curves of the VT-4 passive network (fitted
// to its small-signal response, within 0.1 dB), without the tubes and iron.

#pragma once

#include "Device.h"

#include <cmath>
#include <complex>

namespace vt4
{

enum Model : int { kModelVT4 = 0, kModelMPEQ, kModelPSEQ };

namespace refeq
{
    struct Coefs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    // bilinear transform of an analogue biquad (b2 s² + b1 s + b0)/(a2 s² + a1 s + a0)
    // with s normalised to w0 (pre-warped at w0)
    inline Coefs bilinear (double fs, double f0, double nb2, double nb1, double nb0, double na2, double na1, double na0)
    {
        const double k = 1.0 / std::tan (kPi * std::min (f0, 0.45 * fs) / fs);
        const double k2 = k * k;
        const double a0 = na2 * k2 + na1 * k + na0;
        Coefs c;
        c.b0 = (nb2 * k2 + nb1 * k + nb0) / a0;
        c.b1 = 2.0 * (nb0 - nb2 * k2) / a0;
        c.b2 = (nb2 * k2 - nb1 * k + nb0) / a0;
        c.a1 = 2.0 * (na0 - na2 * k2) / a0;
        c.a2 = (na2 * k2 - na1 * k + na0) / a0;
        return c;
    }

    inline Coefs peak (double fs, double f0, double q, double g)
    {
        const double A = std::pow (10.0, g / 40.0);
        return bilinear (fs, f0, 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0);
    }
    inline Coefs highPass (double fs, double f0, double q)
    {
        return bilinear (fs, f0, 1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0);
    }
    // second-order shelves (RBJ analogue prototypes)
    inline Coefs lowShelf2 (double fs, double f0, double q, double g)
    {
        const double A = std::pow (10.0, g / 40.0), r = std::sqrt (A) / q;
        return bilinear (fs, f0, A, A * r, A * A, A, r, 1.0);
    }
    inline Coefs highShelf2 (double fs, double f0, double q, double g)
    {
        const double A = std::pow (10.0, g / 40.0), r = std::sqrt (A) / q;
        return bilinear (fs, f0, A * A, A * r, A, 1.0, r, A);
    }
    // first-order shelves around the geometric centre fc (s normalised to fc)
    inline Coefs lowShelf1 (double fs, double fc, double g)
    {
        const double G = std::pow (10.0, g / 20.0), r = std::sqrt (G);
        // (s + r) / (s + 1/r)
        return bilinear (fs, fc, 0.0, 1.0, r, 0.0, 1.0, 1.0 / r);
    }
    inline Coefs highShelf1 (double fs, double fc, double g)
    {
        const double G = std::pow (10.0, g / 20.0), r = std::sqrt (G);
        // (s + 1/r) / (s + r) · G
        return bilinear (fs, fc, 0.0, G, G / r, 0.0, 1.0, r);
    }

    // Q of the VT-4 BODY / PRESENCE bands against gain (fitted)
    inline double bodyQ (double g)      { return g >= 0.0 ? 0.555 - 0.019 * g : 0.40 + 0.031 * -g; }
    inline double presWideQ (double g)  { return g >= 0.0 ? 1.04 - 0.045 * g  : 0.64 + 0.055 * -g; }
    inline double presNarrowQ (double g){ return g >= 0.0 ? 2.12 - 0.09 * g   : 1.35 + 0.112 * -g; }

    struct Biquad
    {
        Coefs c;
        double z1 = 0.0, z2 = 0.0;
        inline double process (double x) noexcept
        {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };
}

class RefEq
{
public:
    static constexpr int kCtrl = 32;

    void prepare (double oversampledRate, const DeviceParams& p, int m)
    {
        fs = oversampledRate;
        prm = p; model = m;
        for (int c = 0; c < 2; ++c) cur[c] = target (c);
        aSmooth = 1.0 - std::exp (-(double) kCtrl / (0.02 * fs));    // ≈ 20 ms
        design();
        reset();
    }

    void setParams (const DeviceParams& p) noexcept { prm = p; }

    void reset() noexcept { for (auto& ch : f) for (auto& b : ch) b.reset(); ctrl = 0; }

    inline void process (double inL, double inR, double& outL, double& outR) noexcept
    {
        if (--ctrl <= 0) { ctrl = kCtrl; controlTick(); }
        double a = inL, b = inR;
        const bool ms = prm.stereo == kMidSide;
        if (ms) { a = 0.5 * (inL + inR); b = 0.5 * (inL - inR); }
        a = chain (0, a); b = chain (1, b);
        if (ms) { outL = a + b; outR = a - b; }
        else    { outL = a; outR = b; }
    }

private:
    struct Set
    {
        double hpf = 0.0;                 // 0 = off
        double f[kNumBands] {}, g[kNumBands] {};
        double focus = 0.0;
    };

    Set target (int c) const
    {
        const auto& cp = prm.ch[(size_t) (prm.stereo == kLink ? 0 : c)];
        Set s;
        s.hpf = cp.hpfHz;
        for (int b = 0; b < kNumBands; ++b) { s.f[b] = cp.band[(size_t) b].freq; s.g[b] = cp.band[(size_t) b].gainDb; }
        s.focus = cp.band[kPres].narrow ? 1.0 : 0.0;
        return s;
    }

    inline double chain (int c, double x) noexcept
    {
        auto& k = f[c];
        if (hpfOn[c]) x = k[0].process (x);
        for (int i = 2; i < 6; ++i) x = k[i].process (x);
        return x;
    }

    void controlTick() noexcept
    {
        bool changed = false;
        for (int c = 0; c < 2; ++c)
        {
            const Set t = target (c);
            Set& s = cur[c];
            auto approachLog = [&] (double& v, double tv)
            {
                if (v == tv) return;
                if (v <= 0.0 || tv <= 0.0) { v = tv; changed = true; return; }
                v = std::exp (std::log (v) + aSmooth * (std::log (tv) - std::log (v)));
                if (std::fabs (std::log (v / tv)) < 1.0e-6) v = tv;
                changed = true;
            };
            auto approach = [&] (double& v, double tv, double eps)
            {
                if (v == tv) return;
                v += aSmooth * (tv - v);
                if (std::fabs (v - tv) < eps) v = tv;
                changed = true;
            };
            if ((s.hpf <= 0.0) != (t.hpf <= 0.0)) { s.hpf = t.hpf; changed = true; }
            else approachLog (s.hpf, t.hpf);
            for (int b = 0; b < kNumBands; ++b) { approachLog (s.f[b], t.f[b]); approach (s.g[b], t.g[b], 1.0e-5); }
            approach (s.focus, t.focus, 1.0e-4);
        }
        if (changed) design();
    }

    void design() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            const Set& s = cur[c];
            const bool on = s.hpf > 0.0;
            if (on && ! hpfOn[c]) f[c][0].reset();
            hpfOn[c] = on;
            for (int st = 0; st < 5; ++st)
                f[c][st == 0 ? 0 : st + 1].c = stage (model, fs, s.hpf, s.f, s.g, s.focus, st);
        }
    }

public:
    // Coefficients of one stage: 0 HPF, 1 LOW, 2 BODY, 3 PRESENCE, 4 AIR.
    static refeq::Coefs stage (int model, double fs, double hpf, const double* fr, const double* g, double focus, int st) noexcept
    {
        using namespace refeq;
        switch (st)
        {
            case 0:  // the VT-4 RC-RC high-pass: f0 = 0.448 · setting, Q 0.384
                return hpf > 0.0 ? highPass (fs, 0.448 * hpf, 0.384) : Coefs{};
            case 1:
                return model == kModelMPEQ ? lowShelf1 (fs, 4.0 * fr[kLow], g[kLow])
                                           : lowShelf2 (fs, 2.28 * fr[kLow], 0.42, g[kLow]);
            case 2:
                return peak (fs, fr[kBody], bodyQ (g[kBody]), g[kBody]);
            case 3:
            {
                // wide ↔ narrow morph in log Q
                const double gp = g[kPres];
                const double q = std::exp ((1.0 - focus) * std::log (presWideQ (gp)) + focus * std::log (presNarrowQ (gp)));
                return peak (fs, fr[kPres], q, gp);
            }
            default:
                return model == kModelMPEQ ? highShelf1 (fs, 0.55 * fr[kAir], g[kAir])
                                           : highShelf2 (fs, 0.55 * fr[kAir], 0.49, g[kAir]);
        }
    }

    // Response of one stage for the settings of a channel set (display).
    static std::complex<double> stageResponse (int model, double fs, const ChannelParams& cp, int st, double freq) noexcept
    {
        double fr[kNumBands], g[kNumBands];
        for (int b = 0; b < kNumBands; ++b) { fr[b] = cp.band[(size_t) b].freq; g[b] = cp.band[(size_t) b].gainDb; }
        const auto c = stage (model, fs, cp.hpfHz, fr, g, cp.band[kPres].narrow ? 1.0 : 0.0, st);
        const auto z1 = std::polar (1.0, -2.0 * kPi * freq / fs);
        const auto z2 = z1 * z1;
        return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }

    // Starts at the present settings with empty filters (switched in).
    void restart (const DeviceParams& p)
    {
        prm = p;
        for (int c = 0; c < 2; ++c) cur[c] = target (c);
        design();
        reset();
    }

private:
    double fs = 176400.0, aSmooth = 0.1;
    DeviceParams prm;
    int model = kModelMPEQ;
    Set cur[2];
    refeq::Biquad f[2][6];
    bool hpfOn[2] { false, false };
    int ctrl = 0;
};

} // namespace vt4
