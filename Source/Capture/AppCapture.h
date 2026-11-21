#pragma once
// Per-app audio capture: makes a running app (a browser, Music, Spotify, ...) behave like a virtual audio input.
//
// macOS 14.2+ only: uses Core Audio process taps. There is no equivalent on iOS/iPadOS (apps cannot hear each other),
// where appCaptureSupported() is false. Implementation: AppCapture.mm. All calls: message thread only.
#include <memory>
#include <string>
#include <vector>
#include "CaptureStream.h"

namespace pb
{
bool appCaptureSupported();                       // true on macOS >= 14.2

struct AppChoice
{
    std::string groupId;                          // stable id saved in settings (e.g. "com.google.Chrome")
    std::string name;                             // display name ("Google Chrome")
    bool outputting = false;                      // currently producing sound
};

/** Apps that Core Audio currently knows about (processes that have touched the audio system), grouped per app,
    playing ones first. Empty when unsupported. */
std::vector<AppChoice> listCapturableApps();

/** Feeds one CaptureStream from one app. While armed it retries whenever the app's set of audio processes changes
    (a new browser tab's helper process starts, the app is launched later), because a tap only covers the processes that
    existed when it was created. Call refresh() from a ~1 Hz timer. */
class AppCaptureSession
{
public:
    enum class State { off, waiting, capturing, failed };

    explicit AppCaptureSession (CaptureStream& destination);
    ~AppCaptureSession();

    /** Arms capture for `groupId`. Never blocks on the app being present: state becomes `waiting` until it plays audio. */
    void start (const std::string& groupId, bool silenceOriginal);
    void stop();
    void refresh();                               // (re)creates the tap if the app's processes changed

    State getState() const;
    std::string getMessage() const;               // human-readable detail for the UI (error, or "waiting for ...")
    std::string getGroupId() const;
    uint64_t getFramesReceived() const;           // grows while audio is actually arriving

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
