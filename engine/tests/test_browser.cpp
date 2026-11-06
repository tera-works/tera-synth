// Host support: preset browser index (search, categories, tags, favorites)
// and MIDI learn.
#include <filesystem>

#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/midi_learn.h"
#include "hsynth/preset.h"
#include "hsynth/preset_index.h"
#include "test.h"

using namespace hsynth;
namespace fs = std::filesystem;

namespace {

std::string tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.generic_string();
}

void writePreset(const std::string& path, const std::string& name, const std::string& cat, std::vector<std::string> tags,
                 LayerType b = LayerType::Off, const std::string& sample = "") {
    Preset p;
    p.name = name;
    p.category = cat;
    p.tags = std::move(tags);
    p.params[(size_t)P::layer(1, P::LType)] = (float)b;
    p.samples[0] = sample;
    std::string err;
    savePresetFile(path, p, err);
}

}  // namespace

TEST(preset_tags_round_trip) {
    Preset p;
    p.name = "Warm Pad";
    p.tags = {"warm", "pad", "日本語タグ"};
    Preset q;
    std::string err;
    CHECK(presetFromJson(presetToJson(p), q, err));
    CHECK(q.tags.size() == 3 && q.tags[2] == "日本語タグ");
    CHECK(PresetIndex::splitTags(" a, b ,, a;c ") == std::vector<std::string>({"a", "b", "c"}));
    CHECK(PresetIndex::joinTags({"a", "b"}) == "a, b");
}

TEST(preset_index_search_and_favorites) {
    const std::string f = tempDir("index_factory"), u = tempDir("index_user");
    fs::create_directories(utf8Path(f + "/sub"));
    writePreset(f + "/01.json", "Brass Stab", "VA", {"bright", "stab"});
    writePreset(f + "/sub/02.json", "Glass Piano", "PCM", {"soft"}, LayerType::FM, "factory/piano/piano.sfz");
    writePreset(u + "/mine.json", "My Bright Pad", "Pad", {"warm"});
    {  // a multi in the same folder is not a preset
        MultiPreset m;
        std::string err;
        saveMultiFile(f + "/multi.json", m, err);
    }
    PresetIndex idx;
    idx.setFolders({{f, false}, {u, true}});
    idx.setFavoritesFile(u + "/favorites.json");
    idx.rescan();
    CHECK(idx.all().size() == 3);

    PresetQuery q;
    q.text = "bright";
    CHECK(idx.query(q).size() == 2);         // tag of one, name of the other
    q.text = "#bright";
    CHECK(idx.query(q).size() == 1);         // tags only
    q.text = "#fm";
    CHECK(idx.query(q).size() == 1);         // automatic layer-type tag
    q.text = "#sampled piano";
    CHECK(idx.query(q).size() == 1);
    q.text = "";
    q.category = "PCM";
    CHECK(idx.query(q).size() == 1);
    q.category.clear();
    q.userOnly = true;
    CHECK(idx.query(q).size() == 1);
    CHECK(idx.categories().size() == 3);

    const int pad = idx.find(u + "/mine.json");
    CHECK(pad >= 0);
    idx.setFavorite(pad, true);
    std::string err;
    CHECK(idx.updateMetadata(pad, "Pads", {"warm", "evolving"}, err));
    CHECK(!idx.updateMetadata(idx.find(f + "/01.json"), "X", {}, err));  // factory: read-only

    PresetIndex again;  // favorites and metadata survive a rescan
    again.setFolders({{f, false}, {u, true}});
    again.setFavoritesFile(u + "/favorites.json");
    again.rescan();
    PresetQuery fav;
    fav.favoritesOnly = true;
    const auto r = again.query(fav);
    CHECK(r.size() == 1 && again.all()[(size_t)r[0]].category == "Pads");
    fav.favoritesOnly = false;
    fav.text = "#evolving";
    CHECK(again.query(fav).size() == 1);
}

TEST(midi_learn_assigns_and_scales) {
    Engine e;
    e.prepare(48000);
    MidiLearn learn;
    learn.arm(P::FilterCutoff);
    const uint8_t cc74max[3] = {0xB0, 74, 127};
    CHECK(learn.onMidi(cc74max, 3, e));
    CHECK(learn.armedParam() == -1 && learn.lastLearned() == P::FilterCutoff);
    const ParamInfo& info = paramTable()[(size_t)P::FilterCutoff];
    CHECK(std::fabs(e.getParam(P::FilterCutoff) - info.max) < info.max * 1e-4f);
    const uint8_t cc74mid[3] = {0xB3, 74, 64};  // other channel: mapping is omni
    CHECK(learn.onMidi(cc74mid, 3, e));
    const float mid = e.getParam(P::FilterCutoff);
    CHECK(mid > info.min && mid < info.max);
    if (info.curve == ParamCurve::Exp) CHECK(mid < 0.5f * (info.min + info.max));  // log taper
    const uint8_t other[3] = {0xB0, 75, 10};
    CHECK(!learn.onMidi(other, 3, e));
    const uint8_t allOff[3] = {0xB0, 123, 0};
    CHECK(!learn.onMidi(allOff, 3, e));       // channel mode messages are never learned
    CHECK(learn.describe(P::FilterCutoff) == "CC 74");

    // part parameter in multi mode: goes to the selected part
    e.setMultiTimbral(true);
    e.selectPart(3);
    learn.arm(P::AmpAttack);
    const uint8_t cc20[3] = {0xB0, 20, 0};
    learn.onMidi(cc20, 3, e);
    CHECK(e.getPartParam(3, P::AmpAttack) == paramTable()[(size_t)P::AmpAttack].min);

    MidiLearn copy;
    std::string err;
    CHECK(copy.fromJson(learn.toJson(), err));
    CHECK(copy.mappings().size() == 2);
    copy.removeParam(P::FilterCutoff);
    CHECK(copy.mappings().size() == 1 && copy.describe(P::FilterCutoff).empty());
}
