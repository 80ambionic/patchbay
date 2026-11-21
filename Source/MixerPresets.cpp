#include "MixerPresets.h"

namespace pb
{
namespace
{
constexpr const char* kExt = ".pbmix";
constexpr juce::int64 kMaxPresetBytes = 1024 * 1024;       // a real preset is ~20 KB; this just bounds a hostile file
constexpr int kPresetVersion = 1;
}

juce::File MixerPresets::fileFor (const std::string& name, bool exact) const
{
    const auto n = sanitizePresetName (name);
    if (n.empty() || (exact && n != name)) return {};         // lookups must name a file exactly; "../x" is not "x"
    return folder.getChildFile (juce::String::fromUTF8 (n.c_str()) + kExt);
}

juce::StringArray MixerPresets::list() const
{
    juce::StringArray names;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExt))
    {
        const auto base = f.getFileNameWithoutExtension().toStdString();
        if (sanitizePresetName (base) == base && ! base.empty()) names.add (juce::String::fromUTF8 (base.c_str()));
    }
    names.sortNatural();
    return names;
}

bool MixerPresets::exists (const std::string& name) const
{
    const auto f = fileFor (name, true);
    return f != juce::File() && f.existsAsFile();
}

bool MixerPresets::save (const std::string& name, const MixerSettings& settings, juce::String* error) const
{
    const auto file = fileFor (name, false);
    if (file == juce::File()) { if (error != nullptr) *error = "Give the preset a name."; return false; }
    if (! folder.createDirectory()) { if (error != nullptr) *error = "Could not create the presets folder."; return false; }

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("patchbayMixerPreset", kPresetVersion);
    root->setProperty ("name", file.getFileNameWithoutExtension());
    root->setProperty ("mixer", mixerToVar (settings));

    juce::TemporaryFile tmp (file);
    if (! tmp.getFile().replaceWithText (juce::JSON::toString (juce::var (root.release()))) || ! tmp.overwriteTargetFileWithTemporary())
    {
        if (error != nullptr) *error = "Could not write the preset file.";
        return false;
    }
    return true;
}

bool MixerPresets::load (const std::string& name, MixerSettings& out, juce::String* error) const
{
    const auto file = fileFor (name, true);
    if (file == juce::File() || ! file.existsAsFile()) { if (error != nullptr) *error = "That preset no longer exists."; return false; }
    if (file.getSize() > kMaxPresetBytes) { if (error != nullptr) *error = "That preset file is too large to be a mixer preset."; return false; }

    const auto root = juce::JSON::parse (file.loadFileAsString());
    juce::var version = root.isObject() ? root.getProperty ("patchbayMixerPreset", {}) : juce::var();
    MixerSettings m;
    if (! (version.isInt() && (int) version == kPresetVersion) || ! mixerFromVar (root.getProperty ("mixer", {}), m))
    {
        if (error != nullptr) *error = "That file is not a mixer preset this version of PatchBay can read.";
        return false;
    }
    out = m;
    return true;
}

bool MixerPresets::remove (const std::string& name) const
{
    const auto f = fileFor (name, true);
    return f != juce::File() && f.existsAsFile() && f.deleteFile();
}
}
