#include "StateFile.h"
#include <algorithm>
#include <cmath>

namespace pb
{
static constexpr int kFormatVersion = 1;
static constexpr int kMaxEntries = 10000;      // a real setup has dozens; this just bounds a corrupt/hostile file

static bool readInt32 (const juce::var& v, int32_t& out)
{
    if (! (v.isInt() || v.isInt64())) return false;           // rejects strings, bools, doubles, null
    const auto i = (juce::int64) v;
    if (i < INT32_MIN || i > INT32_MAX) return false;
    out = (int32_t) i;
    return true;
}

static bool readFiniteNumber (const juce::var& v, double& out)
{
    if (! (v.isDouble() || v.isInt() || v.isInt64())) return false;      // rejects strings ("NaN"), bools, null
    const double d = (double) v;
    if (! std::isfinite (d)) return false;
    out = d;
    return true;
}

static bool readBool (const juce::var& v, bool& out)
{
    if (! v.isBool()) return false;
    out = (bool) v;
    return true;
}

static const char* sourceName (SourceType t)
{
    switch (t) { case SourceType::net: return "net"; case SourceType::app: return "app"; case SourceType::input: return "input"; default: return "off"; }
}

static bool readSourceType (const juce::var& v, SourceType& out)
{
    if (! v.isString()) return false;
    const auto n = v.toString();
    if (n == "off") out = SourceType::off; else if (n == "net") out = SourceType::net;
    else if (n == "app") out = SourceType::app; else if (n == "input") out = SourceType::input;
    else return false;
    return true;
}

static bool readOutType (const juce::var& v, OutType& out)
{
    if (! v.isString()) return false;
    const auto n = v.toString();
    if (n == "audio") out = OutType::audio; else if (n == "net") out = OutType::net; else return false;
    return true;
}

static juce::var destToVar (const NetDest& d)
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("host", juce::String (d.host));
    o->setProperty ("port", d.port);
    return juce::var (o.release());
}

static void destFromVar (const juce::var& v, NetDest& d)
{
    if (! v.isObject()) return;
    const auto host = v.getProperty ("host", {});
    if (host.isString() && isValidHost (host.toString().toStdString())) d.host = host.toString().toStdString();
    int32_t port;
    if (readInt32 (v.getProperty ("port", {}), port) && port >= 1 && port <= 65535) d.port = port;
}

static constexpr int kMixerLayout = 2;       // v1 mixer files had 24 mono strips; they are not migrated (defaults load instead)

juce::var mixerToVar (const MixerSettings& in)
{
    MixerSettings m = in;
    m.sanitize();                                  // never write a value the reader would reject
    auto mo = std::make_unique<juce::DynamicObject>();
    mo->setProperty ("layout", kMixerLayout);
    mo->setProperty ("masterDb", (double) m.masterDb);
    mo->setProperty ("masterMute", m.masterMute);
    mo->setProperty ("masterType", m.masterType == OutType::net ? "net" : "audio");
    mo->setProperty ("masterOut", m.masterOut);
    mo->setProperty ("masterDest", destToVar (m.masterDest));

    juce::Array<juce::var> strips;
    for (const auto& s : m.strips)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("source", sourceName (s.src.type));
        o->setProperty ("index", s.src.index);
        o->setProperty ("gainDb", (double) s.gainDb);
        o->setProperty ("balance", (double) s.balance);
        o->setProperty ("mute", s.mute);
        o->setProperty ("solo", s.solo);
        o->setProperty ("out", s.out);
        o->setProperty ("name", juce::String::fromUTF8 (s.name.c_str()));
        strips.add (juce::var (o.release()));
    }
    mo->setProperty ("strips", strips);

    juce::Array<juce::var> buses;
    for (const auto& b : m.buses)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("gainDb", (double) b.gainDb);
        o->setProperty ("mute", b.mute);
        o->setProperty ("type", b.type == OutType::net ? "net" : "audio");
        o->setProperty ("outFirst", b.outFirst);
        o->setProperty ("dest", destToVar (b.dest));
        o->setProperty ("name", juce::String::fromUTF8 (b.name.c_str()));
        buses.add (juce::var (o.release()));
    }
    mo->setProperty ("buses", buses);

    juce::Array<juce::var> apps;
    for (const auto& a : m.apps)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("bundleId", juce::String (a.bundleId));
        o->setProperty ("name", juce::String::fromUTF8 (a.name.c_str()));
        o->setProperty ("silenceOriginal", a.silenceOriginal);
        apps.add (juce::var (o.release()));
    }
    mo->setProperty ("apps", apps);
    return juce::var (mo.release());
}

bool mixerFromVar (const juce::var& mx, MixerSettings& out)
{
    out = MixerSettings();
    int32_t layout = 0;
    if (! mx.isObject() || ! readInt32 (mx.getProperty ("layout", {}), layout) || layout != kMixerLayout) return false;

    auto num = [] (const juce::var& v, float& dest, float (*clamp) (float))
    {
        double d;
        if (readFiniteNumber (v, d)) dest = clamp ((float) std::clamp (d, -1000.0, 1000.0));
    };
    bool b = false;
    int32_t i32 = 0;

    num (mx.getProperty ("masterDb", {}), out.masterDb, clampGainDb);
    if (readBool (mx.getProperty ("masterMute", {}), b)) out.masterMute = b;
    readOutType (mx.getProperty ("masterType", {}), out.masterType);
    if (readInt32 (mx.getProperty ("masterOut", {}), i32)) out.masterOut = clampOutFirst (i32);
    destFromVar (mx.getProperty ("masterDest", {}), out.masterDest);

    if (auto* apps = mx.getProperty ("apps", {}).getArray())
        for (int i = 0; i < (int) apps->size() && i < kAppSlots; ++i)
        {
            const auto& e = apps->getReference (i);
            const auto id = e.getProperty ("bundleId", {});
            if (! id.isString() || ! isValidBundleId (id.toString().toStdString())) continue;   // bad slot: stays empty
            auto& a = out.apps[(size_t) i];
            a.bundleId = id.toString().toStdString();
            const auto name = e.getProperty ("name", {});
            if (name.isString()) a.name = sanitizeText (name.toString().toStdString(), kMaxAppNameLength);
            if (readBool (e.getProperty ("silenceOriginal", {}), b)) a.silenceOriginal = b;
        }

    if (auto* arr = mx.getProperty ("strips", {}).getArray())
        for (int i = 0; i < (int) arr->size() && i < kStrips; ++i)             // extra entries are ignored
        {
            const auto& e = arr->getReference (i);
            if (! e.isObject()) continue;
            auto& strip = out.strips[(size_t) i];
            SourceType t;
            if (readSourceType (e.getProperty ("source", {}), t))
            {
                strip.src.type = t;
                strip.src.index = readInt32 (e.getProperty ("index", {}), i32) ? i32 : 0;
                strip.src.sanitize();
            }
            num (e.getProperty ("gainDb", {}), strip.gainDb, clampGainDb);
            num (e.getProperty ("balance", {}), strip.balance, clampBalance);
            if (readBool (e.getProperty ("mute", {}), b)) strip.mute = b;
            if (readBool (e.getProperty ("solo", {}), b)) strip.solo = b;
            if (readInt32 (e.getProperty ("out", {}), i32)) strip.out = clampStripOut (i32);
            const auto name = e.getProperty ("name", {});
            if (name.isString()) strip.name = sanitizeStripName (name.toString().toStdString());
        }

    if (auto* arr = mx.getProperty ("buses", {}).getArray())
        for (int i = 0; i < (int) arr->size() && i < kBuses; ++i)
        {
            const auto& e = arr->getReference (i);
            if (! e.isObject()) continue;
            auto& bus = out.buses[(size_t) i];
            num (e.getProperty ("gainDb", {}), bus.gainDb, clampGainDb);
            if (readBool (e.getProperty ("mute", {}), b)) bus.mute = b;
            readOutType (e.getProperty ("type", {}), bus.type);
            if (readInt32 (e.getProperty ("outFirst", {}), i32)) bus.outFirst = clampOutFirst (i32);
            destFromVar (e.getProperty ("dest", {}), bus.dest);
            const auto name = e.getProperty ("name", {});
            if (name.isString()) bus.name = sanitizeStripName (name.toString().toStdString());
        }

    out.sanitize();
    return true;
}

juce::String toJson (const PatchbayState& st)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("version", kFormatVersion);

    juce::Array<juce::var> routes;
    for (const auto& r : st.routes)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("src", (int) r.srcUid);
        o->setProperty ("dst", (int) r.dstUid);
        routes.add (juce::var (o.release()));
    }
    root->setProperty ("routes", routes);

    juce::Array<juce::var> devices;
    for (const auto& [uid, pos] : st.positions)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("uid", (int) uid);
        o->setProperty ("x", pos.x);
        o->setProperty ("y", pos.y);
        devices.add (juce::var (o.release()));
    }
    root->setProperty ("devices", devices);

    auto net = std::make_unique<juce::DynamicObject>();
    net->setProperty ("port", st.network.port);
    net->setProperty ("channels", st.network.channels);
    net->setProperty ("bufferPackets", st.network.bufferPackets);
    root->setProperty ("network", juce::var (net.release()));

    auto mn = std::make_unique<juce::DynamicObject>();
    mn->setProperty ("policy", juce::String (toString (st.midiNet.policy)));
    juce::Array<juce::var> hosts;
    for (const auto& h : st.midiNet.hosts)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("name", juce::String (h.name));
        o->setProperty ("address", juce::String (h.address));
        o->setProperty ("port", h.port);
        hosts.add (juce::var (o.release()));
    }
    mn->setProperty ("hosts", hosts);
    root->setProperty ("midiNet", juce::var (mn.release()));

    root->setProperty ("mixer", mixerToVar (st.mixer));

    return juce::JSON::toString (juce::var (root.release()));
}

PatchbayState fromJson (const juce::String& text)
{
    PatchbayState st;
    const auto root = juce::JSON::parse (text);
    if (! root.isObject()) return st;

    int32_t version = 0;
    if (! readInt32 (root.getProperty ("version", {}), version) || version != kFormatVersion)
        return st;                                  // unknown/future format: start fresh rather than misread it

    if (auto* routes = root.getProperty ("routes", {}).getArray())
        for (const auto& r : *routes)
        {
            if (st.routes.size() >= (size_t) kMaxEntries) break;
            int32_t src, dst;
            if (readInt32 (r.getProperty ("src", {}), src) && readInt32 (r.getProperty ("dst", {}), dst))
                st.routes.push_back ({ src, dst });
        }

    if (auto* devices = root.getProperty ("devices", {}).getArray())
        for (const auto& d : *devices)
        {
            if (st.positions.size() >= (size_t) kMaxEntries) break;
            int32_t uid, x, y;
            if (readInt32 (d.getProperty ("uid", {}), uid) && readInt32 (d.getProperty ("x", {}), x) && readInt32 (d.getProperty ("y", {}), y))
                st.positions[uid] = BlockPos { x, y };
        }

    // Each field is validated on its own; anything missing or out of range keeps its default.
    const auto net = root.getProperty ("network", {});
    auto readRanged = [&] (const char* key, int lo, int hi, int& dest)
    {
        int32_t v;
        if (readInt32 (net.getProperty (key, {}), v) && v >= lo && v <= hi) dest = v;
    };
    if (net.isObject())
    {
        readRanged ("port", NetworkSettings::kMinPort, NetworkSettings::kMaxPort, st.network.port);
        readRanged ("channels", 1, NetworkSettings::kMaxChannels, st.network.channels);
        readRanged ("bufferPackets", NetworkSettings::kMinBuffer, NetworkSettings::kMaxBuffer, st.network.bufferPackets);
    }

    const auto mn = root.getProperty ("midiNet", {});
    if (mn.isObject())
    {
        const auto policy = mn.getProperty ("policy", {});
        if (policy.isString()) policyFromString (policy.toString().toStdString(), st.midiNet.policy);

        std::vector<MidiNetHost> raw;
        if (auto* hosts = mn.getProperty ("hosts", {}).getArray())
            for (const auto& h : *hosts)
            {
                const auto addr = h.getProperty ("address", {});
                int32_t port;
                if (! addr.isString() || ! readInt32 (h.getProperty ("port", {}), port)) continue;
                const auto name = h.getProperty ("name", {});
                raw.push_back ({ name.isString() ? name.toString().toStdString() : std::string(), addr.toString().toStdString(), port });
            }
        st.midiNet.hosts = normaliseHosts (raw);          // same validation whether the file is ours or hand-edited
    }

    mixerFromVar (root.getProperty ("mixer", {}), st.mixer);

    return st;
}

bool saveAtomically (const juce::File& file, const PatchbayState& st)
{
    if (! file.getParentDirectory().createDirectory()) return false;
    juce::TemporaryFile tmp (file);
    return tmp.getFile().replaceWithText (toJson (st)) && tmp.overwriteTargetFileWithTemporary();
}

PatchbayState loadFile (const juce::File& file)
{
    return file.existsAsFile() ? fromJson (file.loadFileAsString()) : PatchbayState {};
}
}
