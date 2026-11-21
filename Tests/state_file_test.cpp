// Persistence: round-trip, corrupt/hostile input, atomic save. Needs only juce_core.
#include <juce_core/juce_core.h>
#include <cstdio>
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

    // ---- mixer settings: round trip, defaults, hostile input
    {
        PatchbayState m;
        m.mixer.masterDb = -3.5f; m.mixer.masterMute = true;
        m.mixer.strips[0].gainDb = -12.25f; m.mixer.strips[0].pan = 0.5f; m.mixer.strips[0].mute = true; m.mixer.strips[0].name = "Kick";
        m.mixer.strips[7].solo = true; m.mixer.strips[7].gainDb = kMinGainDb; m.mixer.strips[7].pan = -1.0f;
        CHECK (fromJson (toJson (m)).mixer == m.mixer);
        CHECK (fromJson ("{\"version\":1}").mixer == MixerSettings());
        const auto h = fromJson (R"({"version":1,"mixer":{"masterDb":"NaN","masterMute":1,"strips":[
            {"gainDb":1e300,"pan":-9,"mute":"yes","solo":true,"name":"a-very-long-name-that-exceeds-the-cap"},
            {"gainDb":null,"pan":0.25},
            7, "x", null,
            {},{},{},{},{},{}]}})").mixer;
        CHECK (h.masterDb == 0.0f && ! h.masterMute);                          // wrong types ignored
        CHECK (h.strips[0].gainDb == kMaxGainDb && h.strips[0].pan == -1.0f);  // out-of-range clamped
        CHECK (! h.strips[0].mute && h.strips[0].solo);
        CHECK (h.strips[0].name.size() == kMaxStripNameLength);
        CHECK (h.strips[1].gainDb == 0.0f && h.strips[1].pan == 0.25f);
        CHECK (h.strips[2] == MixerSettings().strips[2]);                   // junk entries leave defaults
        // routing + app capture selections
        PatchbayState r;
        r.mixer.masterOut = 5; r.mixer.strips[3].directOut = 12; r.mixer.strips[9].directOut = kMaxDeviceOutputs;
        r.mixer.apps[0] = { "com.apple.Safari", "Safari", false };
        r.mixer.apps[2] = { "com.spotify.client", "Spotify", true };
        CHECK (fromJson (toJson (r)).mixer == r.mixer);
        const auto rh = fromJson (R"({"version":1,"mixer":{"masterOut":"3","apps":[
            {"bundleId":"bad id!","name":"x"},
            {"bundleId":"com.google.Chrome","name":7,"silenceOriginal":"no"},
            5, {"bundleId":["a"]}, {"bundleId":"com.extra.fifth"}],
            "strips":[{"directOut":99999},{"directOut":-3},{"directOut":2.5},{"directOut":"4"}]}})").mixer;
        CHECK (rh.masterOut == 1);                                              // wrong type: default
        CHECK (rh.apps[0] == AppTarget());                                      // invalid bundle id: slot stays empty
        CHECK (rh.apps[1].bundleId == "com.google.Chrome" && rh.apps[1].name.empty() && rh.apps[1].silenceOriginal);
        CHECK (rh.apps[2] == AppTarget() && rh.apps[3] == AppTarget());          // junk entries; 5th entry beyond kAppSlots ignored
        CHECK (rh.strips[0].directOut == kMaxDeviceOutputs && rh.strips[1].directOut == 0);
        CHECK (rh.strips[2].directOut == 0 && rh.strips[3].directOut == 0);     // double / string: ignored
        CHECK (fromJson (R"({"version":1,"mixer":{"masterOut":9999}})").mixer.masterOut == kMaxDeviceOutputs - 1);
        // input-device strips (17-24) round trip; a file saved before they existed leaves them at defaults
        PatchbayState in;
        in.mixer.strips[20].directOut = 7; in.mixer.strips[20].name = "Mic"; in.mixer.strips[23].gainDb = -9.5f;
        CHECK (fromJson (toJson (in)).mixer == in.mixer);
        const auto old = fromJson (R"({"version":1,"mixer":{"strips":[{"gainDb":-3},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{"gainDb":-6}]}})").mixer;
        CHECK (old.strips[0].gainDb == -3.0f && old.strips[15].gainDb == -6.0f);
        CHECK (old.strips[16] == MixerSettings().strips[16] && old.strips[23] == MixerSettings().strips[23]);
        CHECK (fromJson (R"({"version":1,"mixer":5})").mixer == MixerSettings());
        CHECK (fromJson (R"({"version":1,"mixer":{"strips":"no"}})").mixer == MixerSettings());
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
