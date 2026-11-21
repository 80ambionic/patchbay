// Objective-C++ (compile with -fobjc-arc). Core Audio process taps, macOS 14.2+.
// Flow: find the app's audio processes -> CATapDescription -> AudioHardwareCreateProcessTap -> private aggregate
// device that contains the tap -> IOProc on the aggregate device -> CaptureStream::write().
#include "AppCapture.h"
#include "AppGrouping.h"

#include <TargetConditionals.h>
#include <algorithm>
#include <atomic>
#include <map>

#if TARGET_OS_OSX && defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 140200
 #define PB_HAS_PROCESS_TAPS 1
#else
 #define PB_HAS_PROCESS_TAPS 0
#endif

#if PB_HAS_PROCESS_TAPS
 #if ! __has_feature(objc_arc)
  #error "AppCapture.mm must be compiled with -fobjc-arc"
 #endif
 #import <AppKit/AppKit.h>
 #import <CoreAudio/CoreAudio.h>
 #import <CoreAudio/AudioHardware.h>
 #import <CoreAudio/AudioHardwareTapping.h>
 #import <CoreAudio/CATapDescription.h>
 #import <Foundation/Foundation.h>
#endif

namespace pb
{
#if PB_HAS_PROCESS_TAPS
namespace
{
struct ProcInfo { AudioObjectID obj = 0; pid_t pid = 0; std::string bundleId; bool outputting = false; };

std::string toStd (CFStringRef s)
{
    if (s == nullptr) return {};
    char buf[512];
    return CFStringGetCString (s, buf, sizeof buf, kCFStringEncodingUTF8) ? std::string (buf) : std::string();
}

template <typename T>
bool getProp (AudioObjectID obj, AudioObjectPropertySelector sel, T& out)
{
    AudioObjectPropertyAddress a { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof (T);
    return AudioObjectGetPropertyData (obj, &a, 0, nullptr, &size, &out) == noErr && size == sizeof (T);
}

std::string ownGroupId()
{
    NSString* b = [[NSBundle mainBundle] bundleIdentifier];
    return b != nil ? groupIdForBundleId ([b UTF8String]) : std::string();
}

std::vector<ProcInfo> enumerateProcesses()
{
    std::vector<ProcInfo> result;
    AudioObjectPropertyAddress a { kAudioHardwarePropertyProcessObjectList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr || size == 0) return result;

    std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));
    size = (UInt32) (ids.size() * sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return result;
    ids.resize (size / sizeof (AudioObjectID));

    for (const auto id : ids)
    {
        ProcInfo p;
        p.obj = id;
        CFStringRef bundle = nullptr;
        if (getProp (id, kAudioProcessPropertyBundleID, bundle) && bundle != nullptr)
        {
            p.bundleId = toStd (bundle);
            CFRelease (bundle);                                        // the property follows the "create" rule
        }
        if (p.bundleId.empty()) continue;                              // command-line tools etc.: nothing to show the user
        getProp (id, kAudioProcessPropertyPID, p.pid);
        UInt32 running = 0;
        if (getProp (id, kAudioProcessPropertyIsRunningOutput, running)) p.outputting = running != 0;
        result.push_back (std::move (p));
    }
    return result;
}

std::string displayName (const std::string& groupId)
{
    NSArray<NSRunningApplication*>* apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:
                                                [NSString stringWithUTF8String:groupId.c_str()]];
    if (apps.count > 0 && apps[0].localizedName != nil) return std::string ([apps[0].localizedName UTF8String]);
    return fallbackAppName (groupId);
}
}

bool appCaptureSupported()
{
    if (@available (macOS 14.2, *)) return true;
    return false;
}

std::vector<AppChoice> listCapturableApps()
{
    std::vector<AppChoice> out;
    if (! appCaptureSupported()) return out;

    const auto self = ownGroupId();
    std::map<std::string, AppChoice> groups;
    for (const auto& p : enumerateProcesses())
    {
        const auto g = groupIdForBundleId (p.bundleId);
        if (g.empty() || g == self) continue;                          // never offer PatchBay itself: that would be a feedback loop
        auto& c = groups[g];
        c.groupId = g;
        c.outputting = c.outputting || p.outputting;
    }
    for (auto& [id, c] : groups) { c.name = displayName (id); out.push_back (c); }

    std::sort (out.begin(), out.end(), [] (const AppChoice& x, const AppChoice& y)
    {
        if (x.outputting != y.outputting) return x.outputting;
        return detail::lower (x.name) < detail::lower (y.name);
    });
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
struct AppCaptureSession::Impl
{
    explicit Impl (CaptureStream& d) : dest (d) { scratch.assign ((size_t) kChunk * 2, 0.0f); }

    static constexpr int kChunk = 1024;

    CaptureStream& dest;
    std::string groupId;
    bool silence = true;
    State state = State::off;
    std::string message;
    int retryTicks = 0;

    AudioObjectID tap = kAudioObjectUnknown, agg = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    CATapDescription* desc = nil;
    std::vector<AudioObjectID> tapped;
    UInt32 tapChannels = 2;
    bool tapInterleaved = true;
    std::atomic<uint64_t> frames { 0 };
    std::vector<float> scratch;                                        // IOProc-thread scratch for planar -> interleaved

    // ---- IOProc (realtime thread): no allocation, no locks, no Objective-C
    static OSStatus ioProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                            AudioBufferList*, const AudioTimeStamp*, void* client)
    {
        auto* s = static_cast<Impl*> (client);
        if (in == nullptr || in->mNumberBuffers == 0) return noErr;

        if (s->tapInterleaved)
        {
            const AudioBuffer& b = in->mBuffers[in->mNumberBuffers - 1];       // the tap's stream is appended after any device inputs
            const int ch = (int) std::max<UInt32> (1, b.mNumberChannels);
            const int n = (int) (b.mDataByteSize / (sizeof (float) * (size_t) ch));
            if (b.mData != nullptr && n > 0)
            {
                s->dest.write (static_cast<const float*> (b.mData), n, ch);
                s->frames.fetch_add ((uint64_t) n, std::memory_order_relaxed);
            }
        }
        else if (in->mNumberBuffers >= s->tapChannels && s->tapChannels >= 1)
        {
            const UInt32 first = in->mNumberBuffers - s->tapChannels;          // tap channels are the last buffers, one channel each
            const AudioBuffer& l = in->mBuffers[first];
            const AudioBuffer& r = in->mBuffers[s->tapChannels >= 2 ? first + 1 : first];
            const int n = (int) (std::min (l.mDataByteSize, r.mDataByteSize) / sizeof (float));
            if (l.mData != nullptr && r.mData != nullptr && n > 0)
            {
                const float* lp = static_cast<const float*> (l.mData);
                const float* rp = static_cast<const float*> (r.mData);
                for (int done = 0; done < n; done += kChunk)
                {
                    const int c = std::min (kChunk, n - done);
                    for (int i = 0; i < c; ++i) { s->scratch[(size_t) i * 2] = lp[done + i]; s->scratch[(size_t) i * 2 + 1] = rp[done + i]; }
                    s->dest.write (s->scratch.data(), c, 2);
                }
                s->frames.fetch_add ((uint64_t) n, std::memory_order_relaxed);
            }
        }
        return noErr;
    }

    std::vector<AudioObjectID> matchingObjects() const
    {
        std::vector<AudioObjectID> ids;
        for (const auto& p : enumerateProcesses())
            if (processBelongsToGroup (p.bundleId, groupId)) ids.push_back (p.obj);
        std::sort (ids.begin(), ids.end());
        return ids;
    }

    void teardown()
    {
        if (agg != kAudioObjectUnknown && proc != nullptr)
        {
            AudioDeviceStop (agg, proc);                               // returns after the IOProc has stopped running
            AudioDeviceDestroyIOProcID (agg, proc);
        }
        proc = nullptr;
        if (agg != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice (agg);
        agg = kAudioObjectUnknown;
        if (@available (macOS 14.2, *))
            if (tap != kAudioObjectUnknown) AudioHardwareDestroyProcessTap (tap);
        tap = kAudioObjectUnknown;
        desc = nil;
        tapped.clear();
    }

    static std::string statusText (const char* what, OSStatus s) { return std::string (what) + " (error " + std::to_string ((int) s) + ")"; }

    bool create (const std::vector<AudioObjectID>& objs, std::string& err)
    {
        if (@available (macOS 14.2, *))
        {
            NSMutableArray<NSNumber*>* list = [NSMutableArray array];
            for (auto o : objs) [list addObject:@(o)];

            desc = [[CATapDescription alloc] initStereoMixdownOfProcesses:list];
            [desc setName:@"PatchBay app capture"];
            [desc setPrivate:YES];
            [desc setMuteBehavior:(silence ? CATapMuted : CATapUnmuted)];

            OSStatus st = AudioHardwareCreateProcessTap (desc, &tap);
            if (st != noErr) { err = statusText ("Could not create the audio tap", st); desc = nil; tap = kAudioObjectUnknown; return false; }

            AudioStreamBasicDescription asbd {};
            AudioObjectPropertyAddress fa { kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            UInt32 sz = sizeof (asbd);
            if (AudioObjectGetPropertyData (tap, &fa, 0, nullptr, &sz, &asbd) == noErr && asbd.mSampleRate > 0.0)
            {
                dest.setSourceRate (asbd.mSampleRate);
                tapChannels = std::max<UInt32> (1, asbd.mChannelsPerFrame);
                tapInterleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
            }
            else { err = "Could not read the tap's format."; teardown(); return false; }

            NSString* tapUid = [[desc UUID] UUIDString];
            NSArray* tapList = @[ @{ @kAudioSubTapUIDKey: tapUid, @kAudioSubTapDriftCompensationKey: @YES } ];

            // First attempt: a private aggregate that contains only the tap. If the system rejects that, retry clocked by
            // the default output device (the arrangement Apple's own sample uses).
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                NSMutableDictionary* d = [@{ @kAudioAggregateDeviceNameKey: @"PatchBay capture",
                                             @kAudioAggregateDeviceUIDKey: [[NSUUID UUID] UUIDString],
                                             @kAudioAggregateDeviceIsPrivateKey: @YES,
                                             @kAudioAggregateDeviceTapAutoStartKey: @YES,
                                             @kAudioAggregateDeviceTapListKey: tapList } mutableCopy];
                if (attempt == 1)
                {
                    AudioObjectID outDev = kAudioObjectUnknown;
                    AudioObjectPropertyAddress da { kAudioHardwarePropertyDefaultSystemOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    UInt32 ds = sizeof (outDev);
                    CFStringRef uid = nullptr;
                    AudioObjectPropertyAddress ua { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                    UInt32 us = sizeof (uid);
                    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &da, 0, nullptr, &ds, &outDev) == noErr
                        && AudioObjectGetPropertyData (outDev, &ua, 0, nullptr, &us, &uid) == noErr && uid != nullptr)
                    {
                        NSString* uidStr = (__bridge_transfer NSString*) uid;      // property follows the "create" rule; ARC now owns it
                        d[@kAudioAggregateDeviceMainSubDeviceKey] = uidStr;
                        d[@kAudioAggregateDeviceSubDeviceListKey] = @[ @{ @kAudioSubDeviceUIDKey: uidStr } ];
                    }
                    else break;
                }

                st = AudioHardwareCreateAggregateDevice ((__bridge CFDictionaryRef) d, &agg);
                if (st == noErr)
                {
                    st = AudioDeviceCreateIOProcID (agg, &Impl::ioProc, this, &proc);
                    if (st == noErr) st = AudioDeviceStart (agg, proc);
                    if (st == noErr) return true;
                    err = statusText ("Could not start the capture device", st);
                    teardownAggregateOnly();
                }
                else err = statusText ("Could not create the capture device", st);
            }
            teardown();
            return false;
        }
        err = "Per-app capture needs macOS 14.2 or later.";
        return false;
    }

    void teardownAggregateOnly()
    {
        if (agg != kAudioObjectUnknown && proc != nullptr) { AudioDeviceStop (agg, proc); AudioDeviceDestroyIOProcID (agg, proc); }
        proc = nullptr;
        if (agg != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice (agg);
        agg = kAudioObjectUnknown;
    }
};

AppCaptureSession::AppCaptureSession (CaptureStream& d) : impl (new Impl (d)) {}
AppCaptureSession::~AppCaptureSession() { stop(); }

void AppCaptureSession::start (const std::string& groupId, bool silenceOriginal)
{
    stop();
    if (! appCaptureSupported())
    {
        impl->state = State::failed;
        impl->message = "Per-app capture needs macOS 14.2 or later.";
        return;
    }
    impl->groupId = groupId;
    impl->silence = silenceOriginal;
    impl->state = State::waiting;
    impl->message = "Waiting for the app to play audio...";
    impl->retryTicks = 0;
    refresh();
}

void AppCaptureSession::stop()
{
    impl->teardown();
    impl->state = State::off;
    impl->message.clear();
    impl->groupId.clear();
}

void AppCaptureSession::refresh()
{
    auto& s = *impl;
    if (s.state == State::off) return;
    if (s.state == State::failed && s.retryTicks > 0) { --s.retryTicks; return; }   // back off: failures can be a permission prompt

    const auto now = s.matchingObjects();
    if (now.empty())
    {
        if (s.tap != kAudioObjectUnknown) s.teardown();
        s.state = State::waiting;
        s.message = "Waiting for " + displayName (s.groupId) + " to play audio...";
        return;
    }
    if (s.state == State::capturing && now == s.tapped) return;

    s.teardown();
    std::string err;
    if (s.create (now, err))
    {
        s.tapped = now;
        s.state = State::capturing;
        s.message = "Capturing " + displayName (s.groupId);
    }
    else
    {
        s.state = State::failed;
        s.message = err + ". If macOS asked for permission, allow PatchBay under System Settings > Privacy & Security > "
                          "Screen & System Audio Recording.";
        s.retryTicks = 10;
    }
}

AppCaptureSession::State AppCaptureSession::getState() const { return impl->state; }
std::string AppCaptureSession::getMessage() const { return impl->message; }
std::string AppCaptureSession::getGroupId() const { return impl->groupId; }
uint64_t AppCaptureSession::getFramesReceived() const { return impl->frames.load (std::memory_order_relaxed); }

#else   // iOS / iPadOS, or an SDK older than macOS 14.2: the feature is simply unavailable.

bool appCaptureSupported() { return false; }
std::vector<AppChoice> listCapturableApps() { return {}; }

struct AppCaptureSession::Impl { State state = State::off; std::string message; };
AppCaptureSession::AppCaptureSession (CaptureStream&) : impl (new Impl) {}
AppCaptureSession::~AppCaptureSession() = default;
void AppCaptureSession::start (const std::string&, bool)
{
    impl->state = State::failed;
    impl->message = "Per-app capture is only available on macOS 14.2 or later (iOS does not allow apps to hear other apps).";
}
void AppCaptureSession::stop() { impl->state = State::off; impl->message.clear(); }
void AppCaptureSession::refresh() {}
AppCaptureSession::State AppCaptureSession::getState() const { return impl->state; }
std::string AppCaptureSession::getMessage() const { return impl->message; }
std::string AppCaptureSession::getGroupId() const { return {}; }
uint64_t AppCaptureSession::getFramesReceived() const { return 0; }
#endif
}
