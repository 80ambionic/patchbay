// Persistence: round-trip, corrupt/hostile input, atomic save. Needs only juce_core.
#include <juce_core/juce_core.h>
#include <cstdio>
#include "MixerPresets.h"
#include "StateFile.h"

using namespace pb;
static int failures = 0;
#define CHECK(c) do { if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    // ---- round trip, including extreme uids and negative positions
    PatchbayState st;
    st.routes = { { 100, 200 }, { kBusOutBase, 300 }, { INT32_MIN + 5, INT32_MAX } };
    st.positions[100] = BlockPos { 20, 30 };
    st.positions[kBusGroupUid] = BlockPos { -50, 4000 };
    st.positions[INT32_MIN] = BlockPos { 0, 0 };
    st.network = { 12345, 6, 11 };
    const auto back = fromJson (toJson (st));
    CHECK (back.routes == st.routes);
    CHECK (back.positions == st.positions);
    CHECK (back.network == st.network);

    // ---- mixer settings (layout 2): round trip, defaults, hostile input
    {
        const MixerSettings defaults;
        PatchbayState m;
        m.mixer.masterDb = -3.5f; m.mixer.masterMute = true; m.mixer.masterType = OutType::net; m.mixer.masterOut = 5;
        m.mixer.masterDest = { "studio-mac.local", 9100 };
        m.mixer.strips[0].src = { SourceType::input, 17 }; m.mixer.strips[0].gainDb = -12.25f; m.mixer.strips[0].balance = 0.5f;
        m.mixer.strips[0].mute = true; m.mixer.strips[0].name = "Kick"; m.mixer.strips[0].out = 12;
        m.mixer.strips[7].solo = true; m.mixer.strips[7].gainDb = kMinGainDb; m.mixer.strips[7].balance = -1.0f; m.mixer.strips[7].src = { SourceType::off, 0 };
        m.mixer.strips[31].src = { SourceType::app, 3 }; m.mixer.strips[31].out = kBuses;
        m.mixer.buses[0].type = OutType::net; m.mixer.buses[0].dest = { "192.168.1.20", 9001 }; m.mixer.buses[0].gainDb = -7.0f; m.mixer.buses[0].name = "Cue";
        m.mixer.buses[31].mute = true; m.mixer.buses[31].outFirst = kMaxDeviceOutputs - 1;
        m.mixer.apps[0] = { "com.apple.Safari", "Safari", false };
        m.mixer.apps[2] = { "com.spotify.client", "Spotify", true };
        CHECK (fromJson (toJson (m)).mixer == m.mixer);
        CHECK (fromJson ("{\"version\":1}").mixer == defaults);
        CHECK (fromJson (toJson (PatchbayState())).mixer == defaults);

        // an old (layout-less) mixer section is ignored, not misread
        CHECK (fromJson (R"({"version":1,"mixer":{"masterDb":-6,"strips":[{"gainDb":-3,"pan":1,"directOut":4}]}})").mixer == defaults);
        CHECK (fromJson (R"({"version":1,"mixer":{"layout":1,"masterDb":-6}})").mixer == defaults);
        CHECK (fromJson (R"({"version":1,"mixer":{"layout":"2","masterDb":-6}})").mixer == defaults);
        CHECK (fromJson (R"({"version":1,"mixer":5})").mixer == defaults);

        // wrong types and out-of-range values
        const auto h = fromJson (R"({"version":1,"mixer":{"layout":2,"masterDb":"NaN","masterMute":1,"masterType":"warp","masterOut":"3",
            "masterDest":{"host":"bad host!","port":99999},
            "strips":[
              {"source":"input","index":9999,"gainDb":1e300,"balance":-9,"mute":"yes","solo":true,"out":77,"name":"a-very-long-name-that-exceeds-the-cap"},
              {"source":"net","index":-4,"gainDb":null,"balance":0.25,"out":-2},
              {"source":"banana","index":2}, {"source":3}, 7, "x", null, {} ],
            "buses":[
              {"gainDb":-1e9,"type":"net","outFirst":0,"dest":{"host":"10.0.0.7","port":"9000"},"mute":"no"},
              {"type":5,"outFirst":9999,"dest":{"host":["a"],"port":2.5}}, 3, null ]}})").mixer;
        CHECK (h.masterDb == 0.0f && ! h.masterMute && h.masterType == OutType::audio && h.masterOut == 1);   // wrong types ignored
        CHECK (h.masterDest.host.empty() && h.masterDest.port == kDefaultNetOutPort);
        CHECK (h.strips[0].src == (Source { SourceType::input, kDevicePairs - 1 }) && h.strips[0].gainDb == kMaxGainDb && h.strips[0].balance == -1.0f);
        CHECK (! h.strips[0].mute && h.strips[0].solo && h.strips[0].out == kBuses);
        CHECK (h.strips[0].name.size() == kMaxStripNameLength);
        CHECK (h.strips[1].src == (Source { SourceType::net, 0 }) && h.strips[1].gainDb == 0.0f && h.strips[1].balance == 0.25f && h.strips[1].out == 0);
        CHECK (h.strips[2] == defaults.strips[2] && h.strips[3] == defaults.strips[3]);       // unknown source name / wrong type: untouched
        CHECK (h.strips[4] == defaults.strips[4] && h.strips[7] == defaults.strips[7]);       // junk entries leave defaults
        CHECK (h.strips[31] == defaults.strips[31]);                                          // missing entries too
        CHECK (h.buses[0].gainDb == kMinGainDb && h.buses[0].type == OutType::net && h.buses[0].outFirst == 1 && ! h.buses[0].mute);
        CHECK (h.buses[0].dest.host == "10.0.0.7" && h.buses[0].dest.port == kDefaultNetOutPort);   // string port: ignored
        CHECK (h.buses[1].type == OutType::audio && h.buses[1].outFirst == kMaxDeviceOutputs - 1 && h.buses[1].dest == NetDest());
        CHECK (h.buses[2] == defaults.buses[2] && h.buses[3] == defaults.buses[3]);

        // apps
        const auto rh = fromJson (R"({"version":1,"mixer":{"layout":2,"apps":[
            {"bundleId":"bad id!","name":"x"},
            {"bundleId":"com.google.Chrome","name":7,"silenceOriginal":"no"},
            5, {"bundleId":["a"]}, {"bundleId":"com.extra.fifth"}]}})").mixer;
        CHECK (rh.apps[0] == AppTarget());                                      // invalid bundle id: slot stays empty
        CHECK (rh.apps[1].bundleId == "com.google.Chrome" && rh.apps[1].name.empty() && rh.apps[1].silenceOriginal);
        CHECK (rh.apps[2] == AppTarget() && rh.apps[3] == AppTarget());          // junk entries; 5th entry beyond kAppSlots ignored

        CHECK (fromJson (R"({"version":1,"mixer":{"layout":2,"masterOut":9999}})").mixer.masterOut == kMaxDeviceOutputs - 1);
        CHECK (fromJson (R"({"version":1,"mixer":{"layout":2,"strips":"no","buses":{"a":1}}})").mixer == defaults);

        // more entries than strips/buses: the extras are ignored, not a crash
        juce::String many = R"({"version":1,"mixer":{"layout":2,"strips":[)";
        for (int i = 0; i < 500; ++i) many << (i ? "," : "") << R"({"gainDb":-5})";
        many << R"(],"buses":[)";
        for (int i = 0; i < 500; ++i) many << (i ? "," : "") << R"({"gainDb":-5})";
        many << "]}}";
        const auto big = fromJson (many).mixer;
        CHECK (big.strips[kStrips - 1].gainDb == -5.0f && big.buses[kBuses - 1].gainDb == -5.0f);
    }

    // ---- preset names and the preset library
    {
        CHECK (sanitizePresetName ("  Live  Set (v2).final ") == "Live Set (v2).final");
        CHECK (sanitizePresetName ("../../etc/passwd") == "etcpasswd");
        CHECK (sanitizePresetName ("...hidden") == "hidden" && sanitizePresetName ("..") == "" && sanitizePresetName ("   ") == "" && sanitizePresetName ("") == "");
        CHECK (sanitizePresetName ("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij");
        CHECK (sanitizePresetName (std::string (200, 'x')).size() == kMaxPresetNameLength);
        CHECK (sanitizePresetName ("Caf\xc3\xa9 mix") == "Caf\xc3\xa9 mix");                           // UTF-8 letters survive
        CHECK (sanitizePresetName (std::string ("a\0b", 3)) == "ab");
        CHECK (sanitizePresetName ("trailing dots...") == "trailing dots");

        auto pdir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("patchbay_presets_test_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
        MixerPresets lib (pdir);
        CHECK (lib.list().isEmpty() && ! lib.exists ("A"));

        MixerSettings a;
        a.masterDb = -4.0f; a.strips[3].gainDb = -9.0f; a.strips[3].out = 7; a.buses[6].type = OutType::net; a.buses[6].dest = { "10.1.1.9", 9200 };
        a.apps[1] = { "com.google.Chrome", "Chrome", true };
        juce::String err;
        CHECK (lib.save ("Podcast", a, &err) && err.isEmpty());
        MixerSettings b = a; b.masterDb = 2.0f; b.strips[0].mute = true;
        CHECK (lib.save ("zebra mix", b) && lib.save ("  Alpha ", MixerSettings()));   // saving sanitises the name
        CHECK (lib.list() == juce::StringArray ({ "Alpha", "Podcast", "zebra mix" }));
        CHECK (lib.exists ("Podcast") && ! lib.exists ("  Podcast  ") && ! lib.exists ("../Podcast"));   // lookups are exact

        MixerSettings got; got.masterDb = 99.0f;
        CHECK (lib.load ("Podcast", got, &err) && got == a);
        CHECK (lib.load ("zebra mix", got) && got == b);

        CHECK (lib.save ("Podcast", b));                                                         // overwrite
        CHECK (lib.load ("Podcast", got) && got == b);
        CHECK (pdir.findChildFiles (juce::File::findFiles, false, "*").size() == 3);             // no temp files left

        CHECK (! lib.save ("", a, &err) && err.isNotEmpty());
        CHECK (! lib.save ("...", a));
        CHECK (! lib.load ("missing", got, &err) && err.isNotEmpty());
        CHECK (! lib.load ("../x", got));

        // a hand-made file: wrong shape, wrong version, an old (layout-less) mixer, oversized, not JSON
        pdir.getChildFile ("bad1.pbmix").replaceWithText ("not json");
        pdir.getChildFile ("bad2.pbmix").replaceWithText (R"({"patchbayMixerPreset":2,"mixer":{"layout":2}})");
        pdir.getChildFile ("bad3.pbmix").replaceWithText (R"({"patchbayMixerPreset":1,"mixer":{"masterDb":-3}})");
        pdir.getChildFile ("bad4.pbmix").replaceWithText (R"({"patchbayMixerPreset":1})");
        pdir.getChildFile ("bad5.pbmix").replaceWithText (juce::String (std::string (2 * 1024 * 1024, ' ')));
        pdir.getChildFile ("not a preset.txt").replaceWithText ("x");
        pdir.getChildFile ("we!rd.pbmix").replaceWithText ("{}");                                // listed names must be loadable names
        for (const char* n : { "bad1", "bad2", "bad3", "bad4", "bad5" })
        {
            MixerSettings keep = a; keep.masterDb = 5.0f;
            CHECK (! lib.load (n, keep, &err) && err.isNotEmpty());
            CHECK (keep.masterDb == 5.0f);                                                       // a failed load never changes the caller's settings
        }
        const auto names = lib.list();
        CHECK (names.contains ("Alpha") && names.contains ("bad1") && ! names.contains ("we!rd") && ! names.contains ("not a preset"));

        CHECK (lib.remove ("bad1") && ! lib.exists ("bad1") && ! lib.remove ("bad1") && ! lib.remove ("../Alpha"));
        CHECK (lib.exists ("Alpha"));
        pdir.deleteRecursively();
        CHECK (lib.list().isEmpty());
    }

    // ---- RTP-MIDI settings: round trip, validation on load, defaults
    st.midiNet.policy = MidiNetPolicy::anyone;
    st.midiNet.hosts = { { "Studio Mac", "192.168.1.20", 5004 }, { "Laptop", "my-laptop.local", 5006 } };
    CHECK (fromJson (toJson (st)).midiNet == st.midiNet);
    CHECK (fromJson ("{\"version\":1}").midiNet == MidiNetSettings());
    CHECK (fromJson ("{\"version\":1,\"midiNet\":{\"policy\":\"everyone\"}}").midiNet.policy == MidiNetPolicy::contactsOnly);   // unknown policy: safe default
    CHECK (fromJson ("{\"version\":1,\"midiNet\":{\"policy\":3}}").midiNet.policy == MidiNetPolicy::contactsOnly);
    const auto hand = fromJson (R"({"version":1,"midiNet":{"policy":"noOne","hosts":[
        {"name":"ok","address":"10.1.2.3","port":5004},
        {"name":"x","address":"evil host; rm","port":5004},
        {"name":"x","address":"10.1.2.4","port":"5004"},
        {"name":"x","address":7,"port":5004},
        {"address":"10.1.2.5","port":70000},
        {"address":"10.1.2.6","port":5004},
        {"name":"dup","address":"10.1.2.3","port":5004},
        5, null, "str"]}})").midiNet;
    CHECK (hand.policy == MidiNetPolicy::noOne);
    CHECK (hand.hosts.size() == 2 && hand.hosts[0].address == "10.1.2.3" && hand.hosts[1].address == "10.1.2.6" && hand.hosts[1].name == "10.1.2.6");

    // ---- network settings: missing -> defaults; each bad field falls back individually; good fields survive
    const NetworkSettings defaults;
    CHECK (fromJson ("{\"version\":1}").network == defaults);
    CHECK (fromJson ("{\"version\":1,\"network\":5}").network == defaults);
    CHECK (fromJson ("{\"version\":1,\"network\":{\"port\":80,\"channels\":0,\"bufferPackets\":999}}").network == defaults);  // all out of range
    CHECK (fromJson ("{\"version\":1,\"network\":{\"port\":\"9000\",\"channels\":true,\"bufferPackets\":2.5}}").network == defaults);
    const auto partial = fromJson ("{\"version\":1,\"network\":{\"port\":70000,\"channels\":4,\"bufferPackets\":8}}").network;
    CHECK (partial.port == defaults.port && partial.channels == 4 && partial.bufferPackets == 8);

    // ---- garbage in, empty (or valid-subset) out; never a crash or fabricated entries
    for (const char* bad : { "", "   ", "not json", "{", "[", "null", "42", "\"str\"", "[1,2,3]", "{}", "{\"version\":1}",
                             "{\"version\":2,\"routes\":[{\"src\":1,\"dst\":2}]}",          // future format: don't guess
                             "{\"version\":\"1\",\"routes\":[{\"src\":1,\"dst\":2}]}",      // version of the wrong type
                             "{\"version\":1,\"routes\":\"oops\",\"devices\":7}",
                             "{\"version\":1,\"routes\":[1,\"a\",null,[],{}]}" })
    {
        const auto s = fromJson (bad);
        CHECK (s.routes.empty() && s.positions.empty());
    }

    // ---- type confusion must not become route 0->0 or position 0,0
    const auto mixed = fromJson (R"({"version":1,
        "routes":[{"src":"abc","dst":2},{"src":1,"dst":true},{"src":1.5,"dst":2},{"src":9999999999,"dst":2},{"src":7,"dst":8}],
        "devices":[{"uid":1,"x":"5","y":6},{"uid":2,"x":3},{"uid":3,"x":10,"y":20}]})");
    CHECK (mixed.routes.size() == 1 && mixed.routes[0] == (Route { 7, 8 }));
    CHECK (mixed.positions.size() == 1 && mixed.positions.at (3) == (BlockPos { 10, 20 }));

    // ---- unknown extra fields are ignored (forward-compatible within a version)
    const auto extra = fromJson (R"({"version":1,"futureThing":{"a":1},"routes":[{"src":1,"dst":2,"color":"red"}],"devices":[]})");
    CHECK (extra.routes.size() == 1);

    // ---- truncated file (crash mid-write of a NON-atomic writer) is survivable
    const auto full = toJson (st);
    for (int cut = 0; cut < full.length(); cut += 7)
        (void) fromJson (full.substring (0, cut));       // must not crash; result is whatever parses (usually empty)

    // ---- size cap on a hostile file
    juce::String huge = "{\"version\":1,\"routes\":[";
    for (int i = 0; i < 30000; ++i) huge << (i ? "," : "") << "{\"src\":" << i << ",\"dst\":1}";
    huge << "]}";
    CHECK (fromJson (huge).routes.size() == 10000);

    // ---- file level: atomic save, no temp leftovers, overwrite, missing file, unreadable file
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("patchbay_state_test_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
    auto file = dir.getChildFile ("nested").getChildFile ("patchbay.json");
    CHECK (loadFile (file).routes.empty());                       // missing file -> empty, no throw
    CHECK (saveAtomically (file, st));                            // creates parent dirs
    CHECK (loadFile (file).routes == st.routes);

    PatchbayState second; second.routes = { { 1, 2 } };
    CHECK (saveAtomically (file, second));                        // overwrite
    CHECK (loadFile (file).routes == second.routes && loadFile (file).positions.empty());
    CHECK (file.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*").size() == 1);   // no temp file left behind

    file.replaceWithText ("{\"version\":1,\"routes\":[{\"src\":1,\"dst\":2},");   // simulate a truncated file on disk
    CHECK (loadFile (file).routes.empty());

    dir.deleteRecursively();
    std::printf (failures == 0 ? "ALL OK\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
