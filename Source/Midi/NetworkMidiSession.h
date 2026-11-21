#pragma once
// RTP-MIDI (network MIDI) via Apple's MIDINetworkSession: the standard AppleMIDI/RTP-MIDI protocol, so it
// interoperates with Logic, Audio MIDI Setup, and Windows rtpMIDI. While enabled, the session appears as ordinary
// CoreMIDI endpoints, so the patchbay shows it as a device and cables work with no further plumbing.
//
// Message thread only. Header is plain C++ (the implementation is Objective-C++ in NetworkMidiSession.mm).
#include <memory>
#include <string>
#include <vector>
#include "NetMidiTypes.h"

namespace pb
{
class NetworkMidiSession
{
public:
    struct Connection { std::string name, address; int port = 0; };

    NetworkMidiSession();
    ~NetworkMidiSession();

    bool isEnabled() const;
    void setEnabled (bool);

    MidiNetPolicy getPolicy() const;
    void setPolicy (MidiNetPolicy);

    int getListenPort() const;                       // UDP control port other devices connect to (0 if unavailable)
    std::string getSessionName() const;              // Bonjour name other devices see

    /** Open a session to `host` and add it to the contact list (so it can reconnect when policy is "contacts only"). */
    bool connect (const MidiNetHost& host, std::string& error);

    /** Close the session to that host and remove it from the contact list. */
    void disconnect (const std::string& address, int port);

    /** Sessions currently open, whether we initiated them or the peer did. */
    std::vector<Connection> getConnections() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
