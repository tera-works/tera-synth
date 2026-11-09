// Standard performance controllers that work on every preset (also ones without a
// filter or a mod-matrix route, like the sampled sounds): CC1 vibrato, CC5 portamento
// time, CC11 expression, CC71 resonance, CC74 brightness.
#include <cmath>
#include <vector>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

// a plain voice like the sampled presets: no filter, nothing in the mod matrix
void plainVoice(Engine& e, VaWave wave) {
    e.prepare(48000);
    e.setParam(P::layer(0, P::VaWave1), (float)wave);
    e.setParam(P::layer(0, P::VaMix), 0.0f);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    for (int s = 0; s < P::kModSlots; ++s) e.setParam(P::mod(s, P::ModAmt), 0.0f);
    e.setParam(P::AmpSustain, 1.0f);
}

Event cc(int n, int value) { return Event::control(EventType::Controller, (float)value / 127.0f, n); }

std::vector<float> render(Engine& e, std::vector<std::pair<double, Event>> events, double seconds = 1.0) {
    RenderJob job;
    job.seconds = seconds;
    for (auto& ev : events) job.events.push_back({ev.first, ev.second});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    return L;
}

// energy of the first difference: grows with the high harmonics
double brightness(const std::vector<float>& x, size_t a, size_t b) {
    double s = 0.0;
    for (size_t i = a + 1; i < b; ++i) s += (double)(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
    return std::sqrt(s / (double)(b - a));
}

}  // namespace

TEST(controllers_expression_is_level_on_every_preset) {
    Engine a, b;
    plainVoice(a, VaWave::Sine);
    plainVoice(b, VaWave::Sine);
    auto full = render(a, {{0.0, Event::noteOn(1, 60, 1.0f)}});
    auto half = render(b, {{0.0, cc(11, 64)}, {0.0, Event::noteOn(1, 60, 1.0f)}});
    const double r = rms(half, 9600, 40000) / rms(full, 9600, 40000);
    CHECK_NEAR(r, (64.0 / 127.0) * (64.0 / 127.0), 0.03);
}

TEST(controllers_cc74_darkens_a_preset_without_filter) {
    Engine a, b, c;
    plainVoice(a, VaWave::Saw);
    plainVoice(b, VaWave::Saw);
    plainVoice(c, VaWave::Saw);
    auto open = render(a, {{0.0, Event::noteOn(1, 48, 1.0f)}});
    auto centre = render(b, {{0.0, cc(74, 64)}, {0.0, Event::noteOn(1, 48, 1.0f)}});
    auto dark = render(c, {{0.0, cc(74, 10)}, {0.0, Event::noteOn(1, 48, 1.0f)}});
    const double bo = brightness(open, 9600, 40000), bc = brightness(centre, 9600, 40000),
                 bd = brightness(dark, 9600, 40000);
    CHECK_NEAR(bc / bo, 1.0, 0.01);  // centre = the preset unchanged
    CHECK(bd < bo * 0.5);
    CHECK(allFinite(dark));
}

TEST(controllers_cc71_resonance_without_filter) {
    Engine a, b;
    plainVoice(a, VaWave::Saw);
    plainVoice(b, VaWave::Saw);
    auto plain = render(a, {{0.0, cc(74, 40)}, {0.0, Event::noteOn(1, 48, 1.0f)}});
    auto reso = render(b, {{0.0, cc(74, 40)}, {0.0, cc(71, 127)}, {0.0, Event::noteOn(1, 48, 1.0f)}});
    CHECK(allFinite(reso));
    CHECK(std::fabs(rms(reso, 9600, 40000) - rms(plain, 9600, 40000)) > 0.002);
    CHECK(peak(reso) <= 1.0);
}

TEST(controllers_mod_wheel_vibrato_without_route) {
    Engine a;
    plainVoice(a, VaWave::Sine);
    auto x = render(a, {{0.0, cc(1, 127)}, {0.0, Event::noteOn(1, 69, 1.0f)}}, 2.0);
    // the pitch moves: the frequency differs between quarter periods of the 5.5 Hz vibrato
    double lo = 1e9, hi = 0.0;
    for (size_t w = 0; w < 8; ++w) {
        const size_t a0 = 9600 + w * 2200;
        const double f = zeroCrossFreq(x, 48000, a0, a0 + 2000);
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    CHECK(hi - lo > 8.0);  // +-0.5 semitone at 440 Hz is about +-13 Hz
    Engine b;
    plainVoice(b, VaWave::Sine);
    auto y = render(b, {{0.0, cc(1, 0)}, {0.0, Event::noteOn(1, 69, 1.0f)}});
    CHECK_NEAR(zeroCrossFreq(y, 48000, 9600, 40000), 440.0, 1.0);
}

TEST(controllers_cc5_portamento_time) {
    Engine a;
    plainVoice(a, VaWave::Sine);
    a.setParam(P::VoiceMode, (float)VoiceMode::Mono);
    // glide 57 -> 69 (220 -> 440 Hz) with a long CC5 time: still well below 440 Hz shortly after
    auto x = render(a, {{0.0, cc(5, 110)}, {0.0, Event::noteOn(1, 57, 1.0f)}, {0.5, Event::noteOn(2, 69, 1.0f)}});
    CHECK(zeroCrossFreq(x, 48000, 24000 + 480, 24000 + 2880) < 400.0);
    Engine b;
    plainVoice(b, VaWave::Sine);
    b.setParam(P::VoiceMode, (float)VoiceMode::Mono);
    auto y = render(b, {{0.0, Event::noteOn(1, 57, 1.0f)}, {0.5, Event::noteOn(2, 69, 1.0f)}});
    CHECK_NEAR(zeroCrossFreq(y, 48000, 24000 + 480, 24000 + 2880), 440.0, 3.0);
}

// ---------------------------------------------------------------- setup files

#include "hsynth/midi_learn.h"
#include "hsynth/setup.h"

TEST(setup_round_trip_multi_and_midi_learn) {
    Engine a;
    a.prepare(48000);
    a.setMultiTimbral(true);
    a.setPartParam(3, P::FilterCutoff, 1234.0f);
    a.setPartMix(3, Mix::Channel, 7.0f);
    MidiLearn la;
    la.setMappings({MidiMapping{-1, 74, P::FilterCutoff, 3, 100.0f, 9000.0f}});
    Json host = Json::object();
    host.set("voices", 96);
    const std::string text = makeSetupJson(a, "Band", "", &la, host);

    Engine b;
    b.prepare(48000);
    MidiLearn lb;
    SetupResult res;
    std::string err;
    CHECK(applySetupJson(b, text, "", &lb, res, err));
    CHECK(res.multi && res.hasState && res.hasMidiLearn);
    CHECK(b.multiTimbral());
    CHECK_NEAR(b.getPartParam(3, P::FilterCutoff), 1234.0, 0.5);
    CHECK_NEAR(b.getPartMix(3, Mix::Channel), 7.0, 0.01);
    CHECK(lb.mappings().size() == 1 && lb.mappings()[0].cc == 74 && lb.mappings()[0].part == 3);
    CHECK(res.host.find("voices") && (int)res.host.find("voices")->number() == 96);
    CHECK(res.name == "Band");

    // not a setup
    SetupResult r2;
    CHECK(!applySetupJson(b, "{\"format\":\"hsynth-preset\"}", "", &lb, r2, err));
}

TEST(controllers_learned_cc_replaces_the_standard_response) {
    // CC1 learned to a part parameter: that part gets no vibrato (the parameter moves instead)
    Engine a;
    plainVoice(a, VaWave::Sine);
    MidiLearn learn;
    learn.setMappings({MidiMapping{-1, 1, P::insp(fx::Tremolo, fx::tremolo::Depth), 0, 0.0f, 1.0f}});
    const uint8_t msg[3] = {0xB0, 1, 127};
    learn.onMidi(msg, 3, a);
    CHECK(a.standardCcOverridden(0, 1));
    CHECK(!a.standardCcOverridden(1, 1));
    CHECK_NEAR(a.getPartParam(0, P::insp(fx::Tremolo, fx::tremolo::Depth)), 1.0, 1e-4);
    auto x = render(a, {{0.0, cc(1, 127)}, {0.0, Event::noteOn(1, 69, 1.0f)}}, 1.0);
    CHECK_NEAR(zeroCrossFreq(x, 48000, 9600, 40000), 440.0, 1.0);
    // mapping removed: the standard response is back with the next CC1
    learn.setMappings({});
    learn.onMidi(msg, 3, a);
    CHECK(!a.standardCcOverridden(0, 1));
}
