// Multi-core voice rendering (Engine::setRenderThreads): the parallel path must
// sound like the serial one (only the summation order of voices differs).
#include <algorithm>
#include <cmath>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

// Two layers (VA + FM) with LFO2 modulating the filter, so the per-control-
// block LFO values handed to the render threads are exercised too.
void heavyPatch(Engine& e, int part) {
    e.setPartParam(part, P::layer(1, P::LType), (float)LayerType::FM);
    e.setPartParam(part, P::FilterType, (float)FilterType::LP24);
    e.setPartParam(part, P::FilterCutoff, 1500.0f);
    e.setPartParam(part, P::Lfo2Rate, 3.0f);
    e.setPartParam(part, P::mod(1, P::ModSrc), (float)ModSource::Lfo2);
    e.setPartParam(part, P::mod(1, P::ModDst), (float)ModDest::Cutoff);
    e.setPartParam(part, P::mod(1, P::ModAmt), 0.5f);
}

void render(int threads, bool multi, std::vector<float>& L, std::vector<float>& R, int blockSize = 128) {
    Engine e;
    e.prepare(48000);
    e.setRenderThreads(threads);
    e.setMultiTimbral(multi);
    for (int k = 0; k < (multi ? 4 : 1); ++k) heavyPatch(e, k);
    RenderJob job;
    job.seconds = 2.0;
    job.blockSize = blockSize;
    uint32_t id = 1;
    for (int i = 0; i < 48; ++i) {
        Event on = Event::noteOn(id, 36 + i, 0.4f + 0.01f * (float)i);
        Event off = Event::noteOff(id, 36 + i);
        ++id;
        const int ch = multi ? i % 4 : 0;
        on.channel = off.channel = (uint8_t)ch;
        const double t = 0.01 * (double)(i % 12);
        job.events.push_back({t, on});
        job.events.push_back({1.0 + 0.005 * (double)i, off});
    }
    std::stable_sort(job.events.begin(), job.events.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
    renderOffline(e, job, L, R);
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, (double)std::fabs(a[i] - b[i]));
    return d;
}

}  // namespace

TEST(parallel_render_matches_serial_single_part) {
    std::vector<float> l1, r1, l4, r4;
    render(1, false, l1, r1);
    render(4, false, l4, r4);
    const double d = std::max(maxDiff(l1, l4), maxDiff(r1, r4));
    INFO("single part, 48 voices: peak %.3f, max |serial - 4 threads| = %.2e\n", peak(l1), d);
    CHECK(rms(l1, 4800, 48000) > 0.01);
    CHECK(d < 1e-4);
}

TEST(parallel_render_matches_serial_multi_part) {
    std::vector<float> l1, r1, l4, r4;
    render(1, true, l1, r1);
    render(4, true, l4, r4);
    const double d = std::max(maxDiff(l1, l4), maxDiff(r1, r4));
    INFO("4 parts x 12 voices: peak %.3f, max |serial - 4 threads| = %.2e\n", peak(l1), d);
    CHECK(rms(l1, 4800, 48000) > 0.01);
    CHECK(d < 1e-4);
}

TEST(parallel_render_odd_block_sizes_and_thread_changes) {
    // block sizes that are not a multiple of the control block, and a thread
    // count changed while rendering (the pool is created on first use)
    std::vector<float> l1, r1, l3, r3;
    render(1, false, l1, r1, 100);
    render(3, false, l3, r3, 100);
    CHECK(std::max(maxDiff(l1, l3), maxDiff(r1, r3)) < 1e-4);

    Engine e;
    e.prepare(48000);
    heavyPatch(e, 0);
    for (int i = 0; i < 32; ++i) e.sendEvent(Event::noteOn((uint32_t)(i + 1), 40 + i, 0.5f));
    std::vector<float> L(256), R(256);
    const int counts[] = {1, 2, 8, 0, 3, 1, 5};
    for (int c : counts) {
        e.setRenderThreads(c);
        for (int b = 0; b < 20; ++b) e.process(L.data(), R.data(), 256);
        CHECK(allFinite(L) && allFinite(R));
    }
    CHECK(e.renderThreads() == 5);
    CHECK(e.stats().activeVoices == 32);
}
