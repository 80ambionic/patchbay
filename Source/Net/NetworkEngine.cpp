#include "NetworkEngine.h"
#include <algorithm>
#include <cmath>

namespace pb::net
{
NetworkEngine::NetworkEngine()
{
    for (int i = 0; i < kAppSlots; ++i)
    {
        captureStreams[(size_t) i] = std::make_unique<CaptureStream>();
        captureSessions[(size_t) i] = std::make_unique<AppCaptureSession> (*captureStreams[(size_t) i]);
        captureActive[(size_t) i] = false;
        captureInUse[(size_t) i] = false;
    }
}

NetworkEngine::~NetworkEngine()
{
    for (int i = 0; i < kAppSlots; ++i) stopCapture (i);
    stop();
    if (audioRunning) { deviceManager.removeAudioCallback (this); audioRunning = false; }
}

void NetworkEngine::enableAllOutputChannels()
{
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr) return;
    const int n = std::min (dev->getOutputChannelNames().size(), kMaxDeviceOutputs);
    if (n <= 2) return;                                    // stereo device: JUCE's default already covers it

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    setup.outputChannels.setRange (0, n, true);            // exactly the channels the device has
    if (deviceManager.setAudioDeviceSetup (setup, true).isNotEmpty())
    {
        setup.useDefaultOutputChannels = true;             // could not open them all: fall back to stereo
        deviceManager.setAudioDeviceSetup (setup, true);
    }
}

juce::String NetworkEngine::prepareDevice()
{
    if (! deviceInitialised)
    {
        auto err = deviceManager.initialiseWithDefaultDevices (0, 2);     // output only: no microphone prompt
        if (err.isEmpty()) { deviceInitialised = true; enableAllOutputChannels(); }
        return err;
    }
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        deviceManager.restartLastAudioDevice();
    return {};
}

bool NetworkEngine::ensureAudioRunning (juce::String& error)
{
    if (audioRunning) return true;
    if (auto err = prepareDevice(); err.isNotEmpty()) { error = "Audio output: " + err; return false; }
    if (deviceManager.getCurrentAudioDevice() == nullptr) { error = "No audio output device is available."; return false; }
    sender.start();                             // Net Out socket + thread; harmless if nothing is routed to the network
    deviceManager.addAudioCallback (this);      // calls audioDeviceAboutToStart() synchronously for the open device
    audioRunning = true;
    return true;
}

bool NetworkEngine::anyCaptureOn() const noexcept
{
    for (bool b : captureOn) if (b) return true;
    return false;
}

void NetworkEngine::releaseAudioIfIdle()
{
    if (! audioRunning || listening || anyCaptureOn() || inputsOn) return;
    deviceManager.removeAudioCallback (this);   // waits for an in-flight callback; calls audioDeviceStopped()
    audioRunning = false;
    sender.stop();
    deviceManager.closeAudioDevice();           // release the hardware while nothing needs it
    pubPrimed = false;
    pubDeviceOpen = false;
    pubNumOutputs = 0;
    pubNumInputs = 0;
}

bool NetworkEngine::start (const NetworkSettings& s, juce::String& error)
{
    stop();

    if (auto err = prepareDevice(); err.isNotEmpty()) { error = "Audio output: " + err; return false; }
    if (deviceManager.getCurrentAudioDevice() == nullptr) { error = "No audio output device is available."; return false; }

    if (! receiver.start (s.port))
    {
        error = "Could not listen on UDP port " + juce::String (s.port) + " (is another program using it?).";
        return false;
    }

    // The callback may be running for app capture. Take it out while the network configuration changes, so it never
    // observes a half-updated state; app capture rings simply buffer (or drop) during the brief gap.
    if (audioRunning) { deviceManager.removeAudioCallback (this); audioRunning = false; }

    // No callback is registered now, so this thread is the queue's only consumer: discard stale packets.
    while (receiver.queue().peek() != nullptr) receiver.queue().release();

    streamChannels = std::clamp (s.channels, 1, kMaxChannels);
    initialTarget = std::clamp (s.bufferPackets, NetworkSettings::kMinBuffer, NetworkSettings::kMaxBuffer);
    requestedTarget = initialTarget;
    appliedTarget = initialTarget;

    const auto& c = receiver.counters();
    baseAudio = c.audioPackets.load(); baseBad = c.badPackets.load();
    baseOther = c.otherPackets.load(); baseQueueFull = c.queueFullDrops.load();
    pubMismatch = 0; pubMismatchCh = 0;
    peakL = peakR = 0.0f;

    listening = true;
    if (! ensureAudioRunning (error))
    {
        listening = false;
        receiver.stop();
        return false;
    }
    return true;
}

void NetworkEngine::stop()
{
    if (! listening) return;

    if (audioRunning) { deviceManager.removeAudioCallback (this); audioRunning = false; }   // consumer gone first
    listening = false;
    receiver.stop();

    pubPrimed = false;
    if (anyCaptureOn() || inputsOn)
    {
        juce::String ignored;
        ensureAudioRunning (ignored);           // app capture keeps playing; the network playout is simply gone
    }
    else
    {
        deviceManager.closeAudioDevice();
        pubDeviceOpen = false;
        pubNumOutputs = 0;
    }
}

// ---- audio input device -----------------------------------------------------------------------------------------
bool NetworkEngine::setDeviceInputsEnabled (bool on, juce::String& error)
{
    if (on == inputsOn) return true;

    if (auto err = prepareDevice(); err.isNotEmpty()) { error = "Audio device: " + err; return false; }
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr) { error = "No audio device type is available."; return false; }

    // Reconfigure with the callback out of the way, then bring it back if anything still needs audio.
    const bool wasRunning = audioRunning;
    if (wasRunning) { deviceManager.removeAudioCallback (this); audioRunning = false; }

    auto setup = deviceManager.getAudioDeviceSetup();
    juce::String problem;

    if (on)
    {
        if (setup.inputDeviceName.isEmpty())
        {
            const auto names = type->getDeviceNames (true);
            const int idx = type->getDefaultDeviceIndex (true);
            if (idx >= 0 && idx < names.size()) setup.inputDeviceName = names[idx];
            else problem = "No audio input device is available.";
        }

        if (problem.isEmpty())
        {
            // Ask the device how many input channels it has instead of guessing, then open exactly min(that, kMaxDeviceInputs).
            std::unique_ptr<juce::AudioIODevice> probe (type->createDevice (setup.outputDeviceName, setup.inputDeviceName));
            const int n = probe != nullptr ? std::min (probe->getInputChannelNames().size(), kMaxDeviceInputs) : 0;
            if (n <= 0) problem = "The input device has no input channels.";
            else
            {
                setup.useDefaultInputChannels = false;
                setup.inputChannels.clear();
                setup.inputChannels.setRange (0, n, true);
                problem = deviceManager.setAudioDeviceSetup (setup, true);
            }
        }
        if (problem.isNotEmpty())
        {
            error = problem;
            if (wasRunning) { deviceManager.addAudioCallback (this); audioRunning = true; }
            return false;
        }
        inputsOn = true;
    }
    else
    {
        inputsOn = false;
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputDeviceName = {};
        deviceManager.setAudioDeviceSetup (setup, true);
        pubNumInputs = 0;
    }

    if (wasRunning || on)
    {
        if (listening || anyCaptureOn() || inputsOn)
        {
            juce::String ignored;
            if (! ensureAudioRunning (ignored) && on) { inputsOn = false; error = ignored; return false; }
        }
        else { deviceManager.closeAudioDevice(); pubDeviceOpen = false; pubNumOutputs = 0; }
    }
    return true;
}

// ---- app capture ------------------------------------------------------------------------------------------------
bool NetworkEngine::startCapture (int slot, const std::string& groupId, bool silenceOriginal, juce::String& error)
{
    if (slot < 0 || slot >= kAppSlots) { error = "Invalid capture slot."; return false; }
    if (! appCaptureSupported()) { error = "Per-app capture needs macOS 14.2 or later (iOS does not allow it)."; return false; }
    if (! isValidBundleId (groupId)) { error = "Choose an app first."; return false; }

    stopCapture (slot);
    if (! ensureAudioRunning (error)) return false;

    const auto s = (size_t) slot;
    if (auto* d = deviceManager.getCurrentAudioDevice()) captureStreams[s]->prepare (d->getCurrentSampleRate());
    captureSessions[s]->start (groupId, silenceOriginal);
    if (captureSessions[s]->getState() == AppCaptureSession::State::failed)
    {
        error = captureSessions[s]->getMessage();
        captureSessions[s]->stop();
        releaseAudioIfIdle();
        return false;
    }

    captureOn[s] = true;
    captureActive[s].store (true, std::memory_order_seq_cst);
    return true;
}

void NetworkEngine::stopCapture (int slot)
{
    if (slot < 0 || slot >= kAppSlots) return;
    const auto s = (size_t) slot;
    if (! captureOn[s] && captureSessions[s]->getState() == AppCaptureSession::State::off) return;

    captureActive[s].store (false, std::memory_order_seq_cst);
    for (int i = 0; i < 200 && captureInUse[s].load (std::memory_order_seq_cst); ++i)   // let an in-flight block finish (<= ~20 ms)
        juce::Thread::sleep (1);

    captureSessions[s]->stop();
    captureOn[s] = false;
    releaseAudioIfIdle();
}

AppCaptureSession::State NetworkEngine::getCaptureState (int slot) const
{
    return slot >= 0 && slot < kAppSlots ? captureSessions[(size_t) slot]->getState() : AppCaptureSession::State::off;
}

juce::String NetworkEngine::getCaptureMessage (int slot) const
{
    return slot >= 0 && slot < kAppSlots ? juce::String::fromUTF8 (captureSessions[(size_t) slot]->getMessage().c_str()) : juce::String();
}

CaptureStream::Status NetworkEngine::getCaptureStatus (int slot) const
{
    return slot >= 0 && slot < kAppSlots ? captureStreams[(size_t) slot]->status() : CaptureStream::Status {};
}

void NetworkEngine::refreshCaptures()
{
    for (int i = 0; i < kAppSlots; ++i)
        if (captureOn[(size_t) i]) captureSessions[(size_t) i]->refresh();
}

// ---- Net Out ----------------------------------------------------------------------------------------------------
void NetworkEngine::setNetOutDestination (int stream, const std::string& host, int port)
{
    if (stream < 0 || stream >= kNetOutStreams) return;
    const bool usable = ! host.empty() && isValidHost (host) && port >= 1 && port <= 65535;
    sender.setDestination (stream, usable ? host : std::string(), usable ? port : 0);
    netDestSet[(size_t) stream].store (usable, std::memory_order_relaxed);
}

NetworkEngine::NetOutStats NetworkEngine::getNetOutStats() const
{
    NetOutStats s;
    const auto& c = sender.counters();
    s.sent = c.sent.load(); s.failed = c.failed.load(); s.unrouted = c.unrouted.load();
    s.queueDrops = pubNetDrops.load();
    s.running = sender.isRunning();
    return s;
}

// ---- audio device lifecycle -------------------------------------------------------------------------------------
void NetworkEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();

    // Built here (not on the audio thread) so the callback never allocates. Also reached when the user switches
    // device or sample rate while running: buffers restart at the new rate.
    scratch.assign ((size_t) kSliceFrames * (size_t) std::max (1, streamChannels), 0.0f);
    mixL.assign ((size_t) kSliceFrames, 0.0f);
    mixR.assign ((size_t) kSliceFrames, 0.0f);
    appBuf.assign ((size_t) kSliceFrames * (size_t) (kAppSlots * 2), 0.0f);
    capTmp.assign ((size_t) kSliceFrames * 2, 0.0f);
    devBuf.assign ((size_t) kSliceFrames * (size_t) kMaxDeviceInputs, 0.0f);
    for (size_t i = 0; i < busBufL.size(); ++i)
    {
        busBufL[i].assign ((size_t) kSliceFrames, 0.0f); busPtrL[i] = busBufL[i].data();
        busBufR[i].assign ((size_t) kSliceFrames, 0.0f); busPtrR[i] = busBufR[i].data();
    }
    packetizer.reset();
    mixer.reset();                              // first block jumps to the fader positions instead of fading in

    playout.reset();
    if (listening)
    {
        NetworkPlayout::Config c;
        c.channels = streamChannels;
        c.sampleRate = rate;
        c.targetPackets = initialTarget;
        playout = std::make_unique<NetworkPlayout> (c);
        appliedTarget = requestedTarget.load();
        playout->setTargetPackets (appliedTarget);
    }

    for (auto& cs : captureStreams) cs->prepare (rate);      // the callback is not running yet, so this is safe

    pubSampleRate = rate;
    pubNumOutputs = device->getActiveOutputChannels().countNumberOfSetBits();
    pubNumInputs = device->getActiveInputChannels().countNumberOfSetBits();
    pubDeviceOpen = true;
}

void NetworkEngine::audioDeviceStopped()
{
    playout.reset();
    pubPrimed = false;
    pubDeviceOpen = false;
}

void NetworkEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                      int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], numSamples);

    auto* pl = playout.get();

    if (pl != nullptr)
    {
        // 1) Hand packets from the network thread to the jitter buffer. A stream with the wrong channel count can't be
        //    mixed into the playout, so it is counted and dropped (the UI tells the user what the sender is sending).
        auto& q = receiver.queue();
        while (const auto* p = q.peek())
        {
            if (p->channels == streamChannels) pl->push (p->seq, p->samples, p->frames);
            else { pubMismatch.fetch_add (1, std::memory_order_relaxed); pubMismatchCh.store (p->channels, std::memory_order_relaxed); }
            q.release();
        }

        // 2) Live buffer-size changes from the UI.
        if (const int t = requestedTarget.load (std::memory_order_relaxed); t != appliedTarget)
        {
            pl->setTargetPackets (t);
            appliedTarget = t;
        }
    }

    // 3) Render: sources -> mixer -> device router.
    float pkL = 0.0f, pkR = 0.0f;
    for (int done = 0; done < numSamples;)
    {
        const int n = std::min (kSliceFrames, numSamples - done);

        MixerCore::Input netIn, appIn;
        if (pl != nullptr)
        {
            pl->process (scratch.data(), n);
            netIn = { scratch.data(), streamChannels };
        }

        bool anyApp = false;
        for (int s = 0; s < kAppSlots; ++s)
        {
            // captureActive/captureInUse form a handshake with stopCapture(): once it clears `active` and sees `inUse` drop,
            // nothing here touches that slot's stream again until it is re-armed.
            if (! captureActive[(size_t) s].load (std::memory_order_seq_cst)) continue;
            captureInUse[(size_t) s].store (true, std::memory_order_seq_cst);
            if (captureActive[(size_t) s].load (std::memory_order_seq_cst))
            {
                if (! anyApp) { std::fill (appBuf.begin(), appBuf.begin() + (size_t) n * (size_t) (kAppSlots * 2), 0.0f); anyApp = true; }
                captureStreams[(size_t) s]->process (capTmp.data(), n);
                for (int i = 0; i < n; ++i)
                {
                    appBuf[(size_t) i * (size_t) (kAppSlots * 2) + (size_t) s * 2]     = capTmp[(size_t) i * 2];
                    appBuf[(size_t) i * (size_t) (kAppSlots * 2) + (size_t) s * 2 + 1] = capTmp[(size_t) i * 2 + 1];
                }
            }
            captureInUse[(size_t) s].store (false, std::memory_order_seq_cst);
        }
        if (anyApp) appIn = { appBuf.data(), kAppSlots * 2 };

        MixerCore::Input devIn;
        if (numIn > 0 && in != nullptr)
        {
            const int ch = std::min (numIn, kMaxDeviceInputs);
            for (int c = 0; c < ch; ++c)
            {
                if (in[c] != nullptr)
                    for (int i = 0; i < n; ++i) devBuf[(size_t) i * (size_t) ch + (size_t) c] = in[c][done + i];
                else
                    for (int i = 0; i < n; ++i) devBuf[(size_t) i * (size_t) ch + (size_t) c] = 0.0f;
            }
            devIn = { devBuf.data(), ch };
        }

        MixerCore::Output mo;
        mo.masterL = mixL.data(); mo.masterR = mixR.data();
        mo.busL = busPtrL.data(); mo.busR = busPtrR.data();
        MixerCore::Routing routing;
        mixer.process (netIn, appIn, devIn, n, mo, &routing);

        for (int i = 0; i < n; ++i)
        {
            pkL = std::max (pkL, std::abs (mixL[(size_t) i]));
            pkR = std::max (pkR, std::abs (mixR[(size_t) i]));
        }

        // Each stereo output goes to its destination: device channels (Audio Out) or a network stream (Net Out).
        std::array<StereoOut, kBuses + 1> toDevice;
        int numToDevice = 0;
        int drops = 0;
        auto sendNet = [&] (int stream, const float* l, const float* r)
        {
            if (netDestSet[(size_t) stream].load (std::memory_order_relaxed)) drops += packetizer.push (stream, l, r, n);
            else packetizer.discard (stream);
        };

        if (routing.masterType == OutType::audio) { toDevice[(size_t) numToDevice++] = { mixL.data(), mixR.data(), routing.masterFirst }; packetizer.discard (0); }
        else sendNet (0, mixL.data(), mixR.data());

        for (int b = 0; b < kBuses; ++b)
        {
            const auto k = (size_t) b;
            if (! routing.busActive[k]) { packetizer.discard (b + 1); continue; }
            if (routing.busType[k] == OutType::audio) { toDevice[(size_t) numToDevice++] = { busPtrL[k], busPtrR[k], routing.busFirst[k] }; packetizer.discard (b + 1); }
            else sendNet (b + 1, busPtrL[k], busPtrR[k]);
        }
        if (drops > 0) pubNetDrops.fetch_add ((uint64_t) drops, std::memory_order_relaxed);

        if (routeToDevice (toDevice.data(), numToDevice, n, out, numOut, done))
            mixer.noteClip();
        done += n;
    }
    raisePeak (peakL, pkL);
    raisePeak (peakR, pkR);

    // 4) Publish status for the UI (relaxed atomics; the UI only displays them).
    if (pl != nullptr)
    {
        const auto st = pl->status();
        pubPrimed.store (st.primed, std::memory_order_relaxed);
        pubPacketFrames.store (st.packetFrames, std::memory_order_relaxed);
        pubFrames.store (st.fillFrames, std::memory_order_relaxed);
        pubTargetFrames.store (st.targetFrames, std::memory_order_relaxed);
        pubRatioPpm.store (st.ratioPpm, std::memory_order_relaxed);
        pubLost.store (st.jitter.lost, std::memory_order_relaxed);
        pubLate.store (st.jitter.late, std::memory_order_relaxed);
        pubDup.store (st.jitter.duplicates, std::memory_order_relaxed);
        pubUnder.store (st.jitter.underruns, std::memory_order_relaxed);
        pubTrim.store (st.jitter.droppedForLatency, std::memory_order_relaxed);
        pubResync.store (st.jitter.resyncs, std::memory_order_relaxed);
    }
}

NetworkEngine::Snapshot NetworkEngine::snapshot()
{
    Snapshot s;
    s.listening = listening;
    s.deviceOpen = pubDeviceOpen.load();
    s.playing = pubPrimed.load();

    const auto& c = receiver.counters();
    if (listening)
    {
        s.packets = c.audioPackets.load() - baseAudio;
        s.rejected = (c.badPackets.load() - baseBad) + (c.otherPackets.load() - baseOther);
        s.queueOverflows = c.queueFullDrops.load() - baseQueueFull;
    }
    s.channelMismatches = pubMismatch.load();
    s.lastMismatchChannels = pubMismatchCh.load();
    s.lost = pubLost.load(); s.late = pubLate.load(); s.duplicates = pubDup.load();
    s.underruns = pubUnder.load(); s.trimmed = pubTrim.load(); s.resyncs = pubResync.load();

    s.packetFrames = pubPacketFrames.load();
    s.sampleRate = pubSampleRate.load();
    if (s.sampleRate > 0)
    {
        s.fillMs = pubFrames.load() / s.sampleRate * 1000.0;
        s.targetMs = pubTargetFrames.load() / s.sampleRate * 1000.0;
    }
    s.ratioPpm = pubRatioPpm.load();
    s.peakL = peakL.exchange (0.0f);
    s.peakR = peakR.exchange (0.0f);
    return s;
}

juce::String NetworkEngine::describeDevice() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getName() + "  -  " + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz, "
               + juce::String (d->getCurrentBufferSizeSamples()) + " samples";
    return "No output device open";
}
}
