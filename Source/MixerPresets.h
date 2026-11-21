#pragma once
// Mixer presets: named snapshots of the whole mixer (every strip's source, fader, balance, mute, solo and output; every
// bus and the master with their Audio Out / Net Out choice and destination; the app-capture targets). One small JSON
// file per preset in <app data>/PatchBay/Presets/<name>.pbmix. Loading a preset never starts capture, listening, input
// or any network sending by itself: those stay opt-in.
#include <juce_core/juce_core.h>
#include <string>
#include "Mixer/MixerSettings.h"
#include "StateFile.h"

namespace pb
{
constexpr size_t kMaxPresetNameLength = 48;

/** A preset name becomes a file name, so it is held to a safe subset: letters, digits, space, '-', '_', '.', '(' and ')'
    (UTF-8 letters are kept). Leading/trailing spaces and leading dots are dropped, runs of spaces collapse to one, and
    the result is cut to kMaxPresetNameLength. Returns "" if nothing usable is left. */
inline std::string sanitizePresetName (const std::string& in)
{
    std::string out;
    bool lastSpace = true;                                   // drops leading spaces
    for (unsigned char c : in)
    {
        if (out.size() >= kMaxPresetNameLength) break;
        const bool ascii = c < 0x80;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '-' || c == '_' || c == '.' || c == '(' || c == ')' || ! ascii;
        if (c == ' ')
        {
            if (! lastSpace) out.push_back (' ');
            lastSpace = true;
        }
        else if (ok)
        {
            if (c == '.' && out.empty()) continue;           // no hidden files, no ".."
            out.push_back ((char) c);
            lastSpace = false;
        }
    }
    while (! out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

class MixerPresets
{
public:
    explicit MixerPresets (juce::File presetsFolder) : folder (std::move (presetsFolder)) {}

    static juce::File defaultFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("PatchBay").getChildFile ("Presets");
    }

    /** Names of the presets on disk, sorted (case-insensitive). Files with unusable names are skipped. */
    juce::StringArray list() const;
    bool exists (const std::string& name) const;

    /** `name` is sanitised first (the UI shows the sanitised name). Writes atomically (a crash mid-save keeps the old
        preset). exists/load/remove take a name exactly as list() returns it: anything the sanitiser would change is refused,
        so "../x" can never reach another file or be mistaken for "x". */
    bool save (const std::string& name, const MixerSettings& settings, juce::String* error = nullptr) const;
    bool load (const std::string& name, MixerSettings& out, juce::String* error = nullptr) const;
    bool remove (const std::string& name) const;

private:
    juce::File fileFor (const std::string& name, bool exact) const;
    juce::File folder;
};
}
