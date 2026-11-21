#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <array>
#include <memory>
#include <string>
#include "NetworkPlayout.h"
#include "../Capture/AppCapture.h"
#include "../Capture/CaptureStream.h"
#include "../Mixer/DeviceRouter.h"
#include "../Mixer/MixerCore.h"
#include "UdpReceiver.h"
#include "../StateFile.h"

namespace pb::net
{
/** The audio engine: owns the output device and the audio callback, and feeds the mixer from two kinds of source.
      network:  UDP thread (UdpReceiver) --SpscQueue--> NetworkPlayout ------------\
      app:      Core Audio tap thread --> CaptureStream (resample + drift fix) ----+--> MixerCore --> DeviceRouter --> outputs
    The device is open while the network is listening OR any app capture slot is on; closing the last one releases it.
    Lifecycle calls (start/stop/setBufferPackets/snapshot) are message-thread only; the audio callback only
    touches atomics shared with them, never locks or allocates. */
class NetworkEngine : private juce::AudioIODeviceCallback
{
public:
    struct Snapshot
    {
        bool listening = false, deviceOpen = false, playing = false;
        uint64_t packets = 0, rejected = 0, queueOverflows = 0, channelMismatches = 0;
        uint64_t lost = 0, late = 0, duplicates = 0, underruns = 0, trimmed = 0, resyncs = 0;
        int lastMismatchChannels = 0;
        int packetFrames = 0;
        double sampleRate = 0, fillMs = 0, targetMs = 0, ratioPpm = 0;
        float peakL = 0, peakR = 0;      // peak since the previous snapshot() call
    };

    NetworkEngine();
    ~NetworkEngine() override;

    /** Opens the output device (first time) and starts listening on the network. On failure returns false and fills `error`. */
    bool start (const NetworkSettings& settings, juce::String& error);
    void stop();
    bool isListening() const noexcept { return listening; }
    bool isAudioRunning() const noexcept { return audioRunning; }
    int  getNumOutputs() const noexcept { return pubNumOutputs.load(); }      // output channels the open device provides (0 = closed)

    // ---- audio input device (opt-in: opening inputs is what triggers the microphone permission prompt) ----------------
    /** Opens the default audio input device's first kInputStrips channels (or closes inputs) and feeds them to strips 17-24.
        Never called automatically. Choose a different input in the Audio device dialog. */
    bool setDeviceInputsEnabled (bool on, juce::String& error);
    bool areDeviceInputsEnabled() const noexcept { return inputsOn; }
    int  getNumInputs() const noexcept { return pubNumInputs.load(); }       // input channels the open device provides

    // ---- app capture (macOS 14.2+; elsewhere startCapture reports "unsupported") ---------------------------------
    /** Arms capture of an app into slot 0..kAppSlots-1 and opens the audio device if needed. Not blocked by the app being silent. */
    bool startCapture (int slot, const std::string& groupId, bool silenceOriginal, juce::String& error);
    void stopCapture (int slot);
    bool isCaptureOn (int slot) const noexcept { return slot >= 0 && slot < kAppSlots && captureOn[(size_t) slot]; }
    AppCaptureSession::State getCaptureState (int slot) const;
    juce::String getCaptureMessage (int slot) const;
    CaptureStream::Status getCaptureStatus (int slot) const;
    void refreshCaptures();                      // ~1 Hz from the message thread: follows apps whose audio processes change

    void setBufferPackets (int n) { requestedTarget = std::clamp (n, NetworkSettings::kMinBuffer, NetworkSettings::kMaxBuffer); }

    /** Make sure the device manager has been initialised (so a device-selector UI has something to show). */
    juce::String prepareDevice();

    Snapshot snapshot();
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    /** The mixer stage. Its setters are safe from the message thread while audio runs. */
    MixerCore& getMixer() noexcept { return mixer; }
    juce::String describeDevice() const;

private:
    static constexpr int kSliceFrames = 1024;     // audio callbacks larger than this are rendered in slices

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    static void raisePeak (std::atomic<float>& peak, float v) noexcept
    {
        float cur = peak.load (std::memory_order_relaxed);
        while (v > cur && ! peak.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }

    bool ensureAudioRunning (juce::String& error);
    void releaseAudioIfIdle();
    bool anyCaptureOn() const noexcept;
    void enableAllOutputChannels();

    juce::AudioDeviceManager deviceManager;
    MixerCore mixer;
    UdpReceiver receiver;
    bool deviceInitialised = false, listening = false, audioRunning = false, inputsOn = false;

    // Written by the message thread before the callback is registered; read-only for the callback afterwards.
    int streamChannels = 2;
    int initialTarget = 4;
    std::atomic<int> requestedTarget { 4 };

    // Owned by the audio device lifecycle: created in audioDeviceAboutToStart, destroyed in audioDeviceStopped.
    std::unique_ptr<NetworkPlayout> playout;
    std::vector<float> scratch, mixL, mixR, appBuf, capTmp, devBuf;
    std::array<std::vector<float>, kMixerStrips> directBuf;
    std::array<float*, kMixerStrips> directPtrs {};
    int appliedTarget = 4;

    // Receiver counters keep running across sessions; we report deltas since start().
    uint64_t baseAudio = 0, baseBad = 0, baseOther = 0, baseQueueFull = 0;

    // Published by the audio thread, read by snapshot().
    std::atomic<bool> pubPrimed { false }, pubDeviceOpen { false };
    std::atomic<uint64_t> pubLost { 0 }, pubLate { 0 }, pubDup { 0 }, pubUnder { 0 }, pubTrim { 0 }, pubResync { 0 }, pubMismatch { 0 };
    std::atomic<int> pubPacketFrames { 0 }, pubMismatchCh { 0 };
    std::atomic<double> pubFrames { 0 }, pubTargetFrames { 0 }, pubRatioPpm { 0 }, pubSampleRate { 0 };
    std::atomic<float> peakL { 0 }, peakR { 0 };
    std::atomic<int> pubNumOutputs { 0 }, pubNumInputs { 0 };

    // App capture. Streams live as long as the engine; a slot is only read by the audio thread while captureActive is set.
    std::array<std::unique_ptr<CaptureStream>, kAppSlots> captureStreams;
    std::array<std::unique_ptr<AppCaptureSession>, kAppSlots> captureSessions;
    std::array<bool, kAppSlots> captureOn {};                          // message-thread view
    std::array<std::atomic<bool>, kAppSlots> captureActive {}, captureInUse {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkEngine)
};
}
