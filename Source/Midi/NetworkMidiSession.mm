// Objective-C++: Apple's MIDINetworkSession is an Objective-C class. This file avoids retain/release and
// __bridge so it compiles identically with or without ARC (objects are only used within a single call).
#include "NetworkMidiSession.h"

#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

namespace pb
{
struct NetworkMidiSession::Impl
{
    MIDINetworkSession* session = nil;      // process-wide singleton owned by CoreMIDI; not retained here
};

namespace
{
std::string toStd (NSString* s) { return s != nil ? std::string ([s UTF8String]) : std::string(); }
NSString* toNS (const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

bool sameHost (MIDINetworkHost* h, const std::string& address, int port)
{
    return h != nil && toStd ([h address]) == address && (int) [h port] == port;
}
}

NetworkMidiSession::NetworkMidiSession() : impl (new Impl)
{
    impl->session = [MIDINetworkSession defaultSession];
}

NetworkMidiSession::~NetworkMidiSession() = default;

bool NetworkMidiSession::isEnabled() const { return impl->session != nil && [impl->session isEnabled]; }

void NetworkMidiSession::setEnabled (bool on)
{
    if (impl->session != nil) [impl->session setEnabled:(on ? YES : NO)];
}

MidiNetPolicy NetworkMidiSession::getPolicy() const
{
    if (impl->session == nil) return MidiNetPolicy::contactsOnly;
    switch ([impl->session connectionPolicy])
    {
        case MIDINetworkConnectionPolicy_NoOne:             return MidiNetPolicy::noOne;
        case MIDINetworkConnectionPolicy_Anyone:            return MidiNetPolicy::anyone;
        case MIDINetworkConnectionPolicy_HostsInContactList:
        default:                                            return MidiNetPolicy::contactsOnly;
    }
}

void NetworkMidiSession::setPolicy (MidiNetPolicy p)
{
    if (impl->session == nil) return;
    switch (p)
    {
        case MidiNetPolicy::noOne:        [impl->session setConnectionPolicy:MIDINetworkConnectionPolicy_NoOne]; break;
        case MidiNetPolicy::anyone:       [impl->session setConnectionPolicy:MIDINetworkConnectionPolicy_Anyone]; break;
        case MidiNetPolicy::contactsOnly: [impl->session setConnectionPolicy:MIDINetworkConnectionPolicy_HostsInContactList]; break;
    }
}

int NetworkMidiSession::getListenPort() const
{
    return impl->session != nil ? (int) [impl->session networkPort] : 0;
}

std::string NetworkMidiSession::getSessionName() const
{
    return impl->session != nil ? toStd ([impl->session networkName]) : std::string();
}

bool NetworkMidiSession::connect (const MidiNetHost& h, std::string& error)
{
    if (impl->session == nil) { error = "Network MIDI is not available on this system."; return false; }
    if (! isValidHostAddress (h.address) || ! isValidPort (h.port)) { error = "That address or port is not valid."; return false; }
    if (! [impl->session isEnabled]) { error = "Enable network MIDI first."; return false; }

    // Already connected? Treat as success rather than opening a second session.
    for (MIDINetworkConnection* c in [impl->session connections])
        if (sameHost ([c host], h.address, h.port)) return true;

    const std::string name = h.name.empty() ? h.address : h.name;
    MIDINetworkHost* host = [MIDINetworkHost hostWithName:toNS (name) address:toNS (h.address) port:(NSUInteger) h.port];
    if (host == nil) { error = "Could not create a host entry for that address."; return false; }

    [impl->session addContact:host];            // lets this peer reconnect under the "contacts only" policy
    MIDINetworkConnection* connection = [MIDINetworkConnection connectionWithHost:host];
    if (connection == nil || ! [impl->session addConnection:connection])
    {
        error = "The connection could not be opened.";
        return false;
    }
    return true;
}

void NetworkMidiSession::disconnect (const std::string& address, int port)
{
    if (impl->session == nil) return;

    for (MIDINetworkConnection* c in [[impl->session connections] allObjects])      // copy: we mutate the set while iterating
        if (sameHost ([c host], address, port))
            [impl->session removeConnection:c];

    for (MIDINetworkHost* h in [[impl->session contacts] allObjects])
        if (sameHost (h, address, port))
            [impl->session removeContact:h];
}

std::vector<NetworkMidiSession::Connection> NetworkMidiSession::getConnections() const
{
    std::vector<Connection> out;
    if (impl->session == nil) return out;

    for (MIDINetworkConnection* c in [impl->session connections])
    {
        MIDINetworkHost* h = [c host];
        if (h == nil) continue;
        out.push_back ({ toStd ([h name]), toStd ([h address]), (int) [h port] });
    }
    return out;
}
}
