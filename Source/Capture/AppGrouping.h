#pragma once
// Chooses which Core Audio "process" belongs to which app. Browsers and many apps play audio from helper
// processes (e.g. "com.google.Chrome.helper.Renderer"), so the user picks an APP and we tap every process in its group.
// Pure C++, unit-tested. The heuristics cover Chromium-family browsers, Electron apps and Safari/WebKit; anything else
// simply appears under its own bundle ID.
#include <algorithm>
#include <cctype>
#include <string>

namespace pb
{
namespace detail
{
inline std::string lower (std::string s) { for (auto& c : s) c = (char) std::tolower ((unsigned char) c); return s; }
inline bool startsWithCI (const std::string& s, const std::string& prefix) { return lower (s).rfind (lower (prefix), 0) == 0; }
}

/** "com.google.Chrome.helper.Renderer" -> "com.google.Chrome"; "com.apple.WebKit.GPU" -> "com.apple.Safari". */
inline std::string groupIdForBundleId (const std::string& bundleId)
{
    if (bundleId.empty()) return {};
    const auto low = detail::lower (bundleId);

    // WebKit's shared helper processes play Safari's audio (they also serve other WebKit apps; see README).
    if (low == "com.apple.webkit.gpu" || low == "com.apple.webkit.webcontent" || low == "com.apple.webkit.networking")
        return "com.apple.Safari";

    // Chromium / Electron helpers: "<app>.helper", "<app>.helper.Renderer", "<app>.helper.GPU", ...
    for (size_t pos = low.find (".helper"); pos != std::string::npos; pos = low.find (".helper", pos + 1))
    {
        const size_t end = pos + 7;
        if (end == low.size() || low[end] == '.') return pos > 0 ? bundleId.substr (0, pos) : bundleId;
    }
    return bundleId;
}

/** True if a process with `processBundleId` is part of the app group `groupId`. */
inline bool processBelongsToGroup (const std::string& processBundleId, const std::string& groupId)
{
    return ! groupId.empty() && groupIdForBundleId (processBundleId) == groupId;
}

/** Last component of a reverse-DNS id, used as a fallback display name ("com.spotify.client" -> "client"). */
inline std::string fallbackAppName (const std::string& groupId)
{
    const auto dot = groupId.rfind ('.');
    return dot == std::string::npos || dot + 1 >= groupId.size() ? groupId : groupId.substr (dot + 1);
}
}
