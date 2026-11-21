#pragma once
// Settings model for RTP-MIDI (network MIDI) with strict validation. Pure C++ (std only) so it is unit-tested
// on any platform; the Apple-specific session lives in NetworkMidiSession.mm.
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pb
{
/** Who may open a session with us. (Connections WE initiate always work.) */
enum class MidiNetPolicy : int { noOne = 0, contactsOnly = 1, anyone = 2 };

inline const char* toString (MidiNetPolicy p) noexcept
{
    switch (p) { case MidiNetPolicy::noOne: return "noOne"; case MidiNetPolicy::anyone: return "anyone"; default: return "contacts"; }
}
inline bool policyFromString (const std::string& s, MidiNetPolicy& out) noexcept
{
    if (s == "noOne")    { out = MidiNetPolicy::noOne; return true; }
    if (s == "contacts") { out = MidiNetPolicy::contactsOnly; return true; }
    if (s == "anyone")   { out = MidiNetPolicy::anyone; return true; }
    return false;
}

constexpr int kDefaultRtpMidiPort = 5004;     // the conventional RTP-MIDI control port
constexpr int kMaxMidiNetHosts = 32;
constexpr size_t kMaxHostNameLength = 64;

struct MidiNetHost
{
    std::string name;                         // display name only
    std::string address;                      // IPv4, IPv6 literal, or hostname
    int port = kDefaultRtpMidiPort;
    bool operator== (const MidiNetHost& o) const noexcept { return address == o.address && port == o.port && name == o.name; }
};

inline bool isValidPort (int p) noexcept { return p >= 1 && p <= 65535; }

namespace detail
{
inline bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }
inline bool isHex (char c) noexcept { return isDigit (c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline bool isAlnum (char c) noexcept { return isDigit (c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline bool isIPv4 (const std::string& s)
{
    int parts = 0;
    size_t i = 0;
    while (i <= s.size())
    {
        size_t j = i;
        int value = 0;
        while (j < s.size() && isDigit (s[j])) { value = value * 10 + (s[j] - '0'); if (value > 255) return false; ++j; }
        const size_t len = j - i;
        if (len == 0 || len > 3 || (len > 1 && s[i] == '0')) return false;      // no empty parts, no leading zeros (octal ambiguity)
        ++parts;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
    }
    return parts == 4;
}

inline bool isIPv6Literal (const std::string& s)
{
    if (s.size() < 2 || s.size() > 39) return false;
    int colons = 0, run = 0, groupLen = 0;
    bool doubleColon = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == ':')
        {
            ++colons; ++run; groupLen = 0;
            if (run == 2) { if (doubleColon) return false; doubleColon = true; }
            if (run > 2) return false;
        }
        else if (isHex (c)) { run = 0; if (++groupLen > 4) return false; }
        else return false;
    }
    return colons >= 2 && colons <= 7;
}

inline bool isHostname (const std::string& s)
{
    if (s.empty() || s.size() > 253) return false;
    size_t labelStart = 0;
    for (size_t i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == '.')
        {
            const size_t len = i - labelStart;
            if (len == 0 || len > 63) return false;
            if (s[labelStart] == '-' || s[i - 1] == '-') return false;
            labelStart = i + 1;
        }
        else if (! (isAlnum (s[i]) || s[i] == '-')) return false;
    }
    return true;
}
}

/** True for a dotted-quad IPv4 address, an IPv6 literal (no brackets, no zone), or an RFC 1123 hostname. */
inline bool isValidHostAddress (const std::string& s)
{
    if (s.empty() || s.size() > 253) return false;
    if (s.find (':') != std::string::npos) return detail::isIPv6Literal (s);

    bool digitsAndDotsOnly = true;
    for (char c : s) if (! (detail::isDigit (c) || c == '.')) { digitsAndDotsOnly = false; break; }
    if (digitsAndDotsOnly) return detail::isIPv4 (s);        // "1.2.3" or "256.1.1.1" must not slip through as a hostname

    return detail::isHostname (s);
}

/** Trim, drop control characters, cap the length. */
inline std::string sanitizeHostName (const std::string& in)
{
    std::string out;
    for (unsigned char c : in) if (c >= 0x20 && c != 0x7f) out.push_back ((char) c);
    const auto first = out.find_first_not_of (' ');
    if (first == std::string::npos) return {};
    out = out.substr (first, out.find_last_not_of (' ') - first + 1);
    if (out.size() > kMaxHostNameLength) out.resize (kMaxHostNameLength);
    return out;
}

/** Keeps only valid entries, one per (address, port), at most kMaxMidiNetHosts; a blank name defaults to the address. */
inline std::vector<MidiNetHost> normaliseHosts (const std::vector<MidiNetHost>& in)
{
    std::vector<MidiNetHost> out;
    std::set<std::pair<std::string, int>> seen;
    for (auto h : in)
    {
        if ((int) out.size() >= kMaxMidiNetHosts) break;
        if (! isValidHostAddress (h.address) || ! isValidPort (h.port)) continue;
        if (! seen.insert ({ h.address, h.port }).second) continue;
        h.name = sanitizeHostName (h.name);
        if (h.name.empty()) h.name = h.address;
        out.push_back (std::move (h));
    }
    return out;
}

struct MidiNetSettings
{
    MidiNetPolicy policy = MidiNetPolicy::contactsOnly;      // safer default; "anyone" must be chosen deliberately
    std::vector<MidiNetHost> hosts;                           // saved peers, reconnected on demand
    bool operator== (const MidiNetSettings& o) const noexcept { return policy == o.policy && hosts == o.hosts; }
};
}
