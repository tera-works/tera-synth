// Multimode filter based on the topology-preserving-transform state variable
// filter (Zavalishin / Cytomic). Stable under fast modulation. Optional input
// drive and a soft-saturated band-pass state give a mild non-linear character.
#pragma once

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

class Svf {
public:
    void reset() { ic1_ = ic2_ = 0.0f; }

    // cutoff in Hz, res 0..1
    void setCoefs(float cutoff, float res, float sampleRate) {
        cutoff = clampf(cutoff, 10.0f, sampleRate * 0.49f);
        g_ = std::tan(kPi * cutoff / sampleRate);
        k_ = 2.0f - 1.98f * clampf(res, 0.0f, 1.0f);  // k = 1/Q, Q up to ~50
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }

    struct Out { float lp, bp, hp; };

    Out tick(float x) {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return {v2, v1, x - k_ * v1 - v2};
    }

    // Non-linear variant: saturate the integrator state (keeps resonance tame
    // when driven hard).
    Out tickNonlinear(float x) {
        Out o = tick(x);
        ic1_ = fastTanh(ic1_ * 0.5f) * 2.0f;
        return o;
    }

    float k() const { return k_; }
    void copyCoefs(const Svf& o) { g_ = o.g_; k_ = o.k_; a1_ = o.a1_; a2_ = o.a2_; a3_ = o.a3_; }

private:
    float g_ = 0, k_ = 2, a1_ = 0, a2_ = 0, a3_ = 0;
    float ic1_ = 0, ic2_ = 0;
};

// Voice filter: chooses mode, cascades two SVFs for 24 dB/oct low-pass.
class VoiceFilter {
public:
    void reset() { s1_.reset(); s2_.reset(); }

    void set(FilterType type, float cutoff, float res, float drive, float sampleRate) {
        type_ = type;
        if (type == FilterType::Off) return;
        drive_ = drive;
        driveGain_ = 1.0f + drive * 8.0f;
        driveComp_ = 1.0f / std::sqrt(driveGain_);
        if (type == FilterType::LP24) {
            // spread resonance: only the second stage resonates
            s1_.setCoefs(cutoff, 0.0f, sampleRate);
            s2_.setCoefs(cutoff, res, sampleRate);
        } else {
            s1_.setCoefs(cutoff, res, sampleRate);
        }
    }

    // Share coefficients with another filter (stereo pair) without touching state.
    void copyCoefsFrom(const VoiceFilter& o) {
        type_ = o.type_; drive_ = o.drive_; driveGain_ = o.driveGain_; driveComp_ = o.driveComp_;
        s1_.copyCoefs(o.s1_);
        s2_.copyCoefs(o.s2_);
    }

    float process(float x) {
        if (type_ == FilterType::Off) return x;
        if (drive_ > 0.001f) x = fastTanh(x * driveGain_) * driveComp_ * 1.5f;
        const bool nl = drive_ > 0.001f;
        switch (type_) {
            case FilterType::LP12: return (nl ? s1_.tickNonlinear(x) : s1_.tick(x)).lp;
            case FilterType::LP24: {
                const float a = s1_.tick(x).lp;
                return (nl ? s2_.tickNonlinear(a) : s2_.tick(a)).lp;
            }
            case FilterType::BP: {
                Svf::Out o = nl ? s1_.tickNonlinear(x) : s1_.tick(x);
                return o.bp * s1_.k();  // unity gain at centre
            }
            case FilterType::HP: return (nl ? s1_.tickNonlinear(x) : s1_.tick(x)).hp;
            case FilterType::Notch: {
                Svf::Out o = s1_.tick(x);
                return o.lp + o.hp;
            }
            default: return x;
        }
    }

private:
    Svf s1_, s2_;
    FilterType type_ = FilterType::LP24;
    float drive_ = 0.0f, driveGain_ = 1.0f, driveComp_ = 1.0f;
};

}  // namespace hsynth
