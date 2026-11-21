# PatchBay

JUCE 8 + CMake, macOS and iOS/iPadOS.

**Page 1 - MIDI Patchbay (working):** CoreMIDI topology reader, live routing, draggable device blocks,
PatchBay's own virtual buses, and saved layout/routes.
**Page 2 - Network (working):** receive audio over UDP and play it on any output device, with a jitter
buffer, clock-drift correction, live stats and level meters. A second lane carries MIDI over the network
(RTP-MIDI / AppleMIDI, interoperable with Logic, Audio MIDI Setup and Windows rtpMIDI).
**Page 3 - Mixer (working):** a 24-strip console - 8 network channels, 4 stereo app-capture slots (browsers and
music apps as virtual inputs, macOS 14.2+) and 8 audio-input-device channels - with per-strip fader, pan, mute, solo, rename, meters, per-strip output
routing to any device output, a selectable master output pair and a clip indicator. Settings are saved.

## Build the app

Requirements: Xcode 15+, CMake 3.22+ (JUCE is fetched automatically).

    # macOS
    cmake --preset macos
    cmake --build build/macos --config Debug
    open build/macos/PatchBay.xcodeproj

    # iOS / iPadOS (set your team for device builds)
    cmake --preset ios -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOURTEAMID
    open build/ios/PatchBay.xcodeproj

Change `COMPANY_NAME` and `BUNDLE_ID` in CMakeLists.txt first.

## Installers
- **macOS .dmg:** `scripts/build_macos_dmg.sh` (universal arm64 + x86_64; ad-hoc signed unless `CODESIGN_IDENTITY` is set,
  notarised if `NOTARY_PROFILE` is set). Ad-hoc builds run on the Mac that built them; for other Macs you need a Developer ID and notarisation.
- **iOS / iPadOS:** `scripts/build_ios.sh simulator` (no account needed) or `TEAM_ID=... scripts/build_ios.sh device` for a signed .ipa.
- **No Mac?** `.github/workflows/build.yml` runs the tests and builds the .dmg and the iOS simulator app on GitHub's macOS runners.
These scripts have never been run: the first build on real Xcode may need small fixes.

## Run the tests (any OS, no Xcode needed)

    cmake -S . -B build-tests -DPATCHBAY_BUILD_TESTS=ON
    cmake --build build-tests && ctest --test-dir build-tests --output-on-failure

That builds `net_tests` (packet codec, jitter buffer, resampler/playout, drift controller, output mapping,
lock-free queue, route-loop guard) `mixer_tests` (mixer DSP, routing), `capture_tests` (resampling, drift, underrun; also run under TSan) and `app_grouping_tests`. On macOS, or elsewhere with `-DPATCHBAY_WITH_JUCE=ON`, it also builds
`udp_loopback_test` (real sockets through the whole receive chain, float32 and int16) and `state_file_test`
(persistence, corrupt-file handling). The CoreMIDI, audio-device and UI code can only be exercised by running the app.

## Using the Mixer page
- **Strips 1-8** carry network stream channels 1-8. **Strips 9-16** are four stereo app slots (App1 L/R ... App4 L/R).
  **Strips 17-24** (In 1-8) are the audio input device's channels.
  Defaults: odd strips hard left, even strips hard right, so stereo sources play as stereo.
- **Fader** -60..+12 dB (bottom = off); double-click resets to 0 dB, type a value in the box to set it exactly.
  **Pan** knob: double-click to centre. Pan law is constant power (-3 dB at centre).
  **M** mutes, **S** solos (any solo silences the non-soloed strips; mute beats solo). Double-click a name to rename it.
- **Output routing (multichannel).** Each strip's menu is *Master* or *Out N*. *Out N* sends that strip, post-fader and
  without pan, straight to device output N and takes it off the master bus - for headphone feeds, cue sends, stems to an
  interface. Several strips may share one output (they sum). The **master** menu picks which output pair the master bus
  plays on (1-2, 3-4, ...). Menus list the open device's outputs; PatchBay opens *all* of them. A saved choice the current
  device lacks stays selectable, marked "(not on this device)". "Out N" counts the outputs enabled in *Audio device...*.
  Outputs that sum past full scale are clamped and light CLIP.
- Meters are post-fader and pre-mute, so a muted strip still shows its signal. The master meter is post-clamp; **CLIP**
  lights for about 2 s when the sum exceeded full scale (the output is hard-clamped at +-1, not limited).
- Gain and routing changes ramp across the audio block (no zipper noise or clicks). Non-finite samples from a bad sender
  are replaced by silence.

### Audio input device (microphone, interface, virtual driver)
The *Audio input device* panel (above strips In 1-8) feeds the first 8 channels of the default input device to strips 17-24.
- **Use input device** is off after every launch and is the only thing that opens an input, so the microphone permission
  prompt appears when you press it, not before. **Audio device...** lets you pick a different input and its channels.
- This is also how a virtual audio driver such as BlackHole or Loopback works as a source: set the app's (or the system's)
  output to the driver, then choose the driver as PatchBay's input.
- A microphone feeding speakers will howl: use headphones, or route those strips with *Out N* to something that is not heard.
- Input and output share one JUCE device. If they are two different physical devices, CoreAudio runs them on separate
  clocks and PatchBay does not resample between them; prefer one interface, or a macOS aggregate device.
- Sandboxed builds need the `com.apple.security.device.audio-input` entitlement; iOS asks for microphone access
  (the usage string is in CMakeLists.txt).

### Browsers and music apps as virtual inputs (macOS 14.2+)
Each *App* panel above the app strips captures one app's audio using Core Audio process taps:
1. Pick the app from the menu (apps that have touched the audio system, playing ones first; the list refreshes every few
   seconds). Press **Capture**. The first time, macOS asks for permission: allow PatchBay under
   *System Settings > Privacy & Security > Screen & System Audio Recording*.
2. **Silence app's own output** (on by default) mutes the app on your speakers while it is captured, so you hear it only
   through the mixer. Capture ending (stop, quit, crash) un-mutes it.
3. Capture is **armed**, not instant: if the app is not playing yet (a browser with no audible tab) the panel says it is
   waiting, and it starts by itself when the app's audio processes appear. It also re-attaches when a browser spawns a new
   helper process, because a tap only covers processes that existed when it was created.
4. The captured audio is resampled to the output device's rate and drift-corrected, with about 50 ms of latency.
Capture never starts by itself on launch (the app choice is saved; pressing Capture is yours to do each launch).

Limits to know about:
- **iOS/iPadOS: not possible.** Apps cannot hear each other there; the panels say so. (Network audio and MIDI still work.)
- The app list groups helper processes by bundle ID (Chromium/Electron `...helper`, WebKit -> Safari). WebKit's audio
  helpers are shared by every WebKit app, so capturing "Safari" can also pick up another WebKit app's sound.
  Other apps that play from unusual helper processes show up under their own IDs; Firefox is untested.
- Starting or stopping the **network Listen** briefly restarts the audio callback, so a captured app glitches for a moment.
- Protected (DRM) audio may arrive silent; not tested.
- A virtual audio driver (BlackHole, Loopback) works through the *Audio input device* panel instead (see above).
- If the permission is denied the tap may deliver silence rather than an error; the panel's buffer readout stays at
  "Buffering" or shows 0 ms.

## Using the MIDI-over-network lane
The lower lane of the Network tab drives Apple's `MIDINetworkSession`.
1. **Enable network MIDI.** It is always off after launch; the enabled state is never saved.
2. **Who can connect:** *No one*, *Only my contacts* (default) or *Anyone on the network*. With *contacts*,
   a peer must be added first: connecting out to it from this lane adds it. *Anyone* lets any device on the
   LAN open a session, so use it only on networks you trust.
3. To reach a peer, enter its address (IPv4, IPv6 or host name), port (5004 by default) and press **Connect**.
   Successful hosts are remembered under *Saved hosts* (double-click to reconnect, **Forget** to remove).
   *Open sessions* lists every live session, including ones the peer opened; **Disconnect** closes one.
4. While enabled, the session appears as ordinary CoreMIDI endpoints, so it shows up on the MIDI Patchbay tab
   and cables work as for any other device.

iOS needs `NSBonjourServices` (`_apple-midi._udp`) and the Local Network usage string; both are set in CMakeLists.txt.
The wrapper (`Source/Midi/NetworkMidiSession.mm`) is Objective-C++ and has only been compile-checked against stubs.

## Using the Network page (audio lane)
1. Pick the UDP port and the stream's channel count, press **Listen**. The page shows the addresses to send to.
2. Send audio. A reference sender is included:

       python3 Tools/send_test_tone.py <ip-shown-on-the-page> --port 9000 --channels 2

   Useful flags: `--format int16`, `--drift-ppm 200` (watch *Clock correction* follow it),
   `--loss 0.02` (watch *Lost (concealed)*), `--frames 256`.
3. **Buffer** sets the jitter-buffer depth in packets and takes effect live: lower is less latency,
   higher rides out more network jitter. **Audio device...** picks the output device.

The stream now goes through the mixer: stream channel *i* feeds strip *i*, and the master bus plays on device outputs 1 and 2
(a mono stream feeds strips 1 and 2; a mono device gets the sum; outputs 3+ are silent). Settings are saved, but
the app never starts listening by itself: you press Listen every launch.

Wire format (header big-endian, payload little-endian; details in `Source/Net/Packet.h`):

    16-byte header: magic "PBNT", version, type, seq(16), timestamp(32), payloadBytes(16), channels, format
    payload: interleaved float32 or int16 frames; keep header+payload under ~1400 bytes (128 frames x 2 ch x float32 = 1040)

Limitations you should know about:
- **The wire format carries no sample rate.** Sender and output device must run at the same rate, or the
  audio plays at the wrong pitch. (Add a rate field before building real senders.)
- **One sender per port.** Packets from two senders to one port interleave.
- **No encryption or authentication:** anyone who can reach the port can send audio to it. Use it on a
  trusted network. If you enable the macOS App Sandbox you need the `com.apple.security.network.server` entitlement.
- **iOS** asks for Local Network permission the first time data arrives (the usage string is set in
  CMakeLists.txt). Background audio keeps playback running with the screen off.
- The clock-correction loop's measured behaviour is in `Source/Net/DriftController.h`.

## Using the patchbay
- Drag a device by its header. Positions are saved.
- Drag from an orange OUT dot to a green IN dot to connect; MIDI is forwarded live. Right-click or
  double-click a cable to remove it. Routes are saved and revive when a device is replugged.
- **PatchBay Buses** (purple block): four pass-through virtual ports. Other apps see "PatchBay Bus 1..4"
  as both a MIDI input and output. Whatever an app sends into a bus input comes out of that bus's output,
  where you can cable it anywhere. The patchbay refuses cables that would feed a bus back into itself.
- State lives in `~/Library/Application Support/PatchBay/patchbay.json` (sandbox container on iOS).

## Layout
    Source/Midi/RouteGraph.h       routing rules + bus IDs + loop guard (pure C++, tested)
    Source/Midi/MidiTopology.*     CoreMIDI device/entity/endpoint scan + hot-plug notifications
    Source/Midi/MidiRouter.*       UID-based route table, forwards MIDI between endpoints
    Source/Midi/VirtualBuses.*     the four pass-through buses
    Source/StateFile.*             JSON persistence (defensive parser, atomic writes)
    Source/StateStore.h            debounced auto-save
    Source/UI/                     DeviceBlock, PatchbayCanvas
    Source/Net/Packet.h            wire format + codec (pure C++)
    Source/Net/SpscQueue.h         wait-free network-thread -> audio-thread queue
    Source/Net/JitterBuffer.h      reorder/loss/underrun/wrap handling
    Source/Net/DriftController.h   clock-drift -> playback ratio (PI loop)
    Source/Net/NetworkPlayout.h    jitter buffer + Catmull-Rom resampler + drift control (pure C++)
    Source/Net/OutputMapper.h      stream channels -> device outputs (pure C++)
    Source/Net/UdpReceiver.*       UDP thread: validate, decode, enqueue
    Source/Net/NetworkEngine.*     audio-device callback tying the above together, published stats
    Source/Midi/NetMidiTypes.h     RTP-MIDI settings model + host validation (pure C++)
    Source/Midi/NetworkMidiSession.*  MIDINetworkSession wrapper (C++ header, Objective-C++ implementation)
    Source/Mixer/MixerSettings.h   mixer settings model + clamping (pure C++)
    Source/Mixer/MixerCore.h       mixer DSP: gain, pan, mute/solo, direct outs, master, meters (pure C++, lock-free)
    Source/Mixer/DeviceRouter.h    places master + direct outs on device output channels (pure C++)
    Source/Capture/CaptureStream.h rate-converting, drift-correcting ring for captured audio (pure C++)
    Source/Capture/AppGrouping.h   which audio process belongs to which app (pure C++)
    Source/Capture/AppCapture.*    Core Audio process-tap wrapper (C++ header, Objective-C++ / ARC implementation)
    Source/UI/MixerPage.*          the Mixer tab
    Source/UI/NetworkAudioLane.*   audio lane of the Network tab
    Source/UI/NetworkMidiLane.*    MIDI lane of the Network tab
    Source/UI/NetworkPage.*        the Network tab (scrolling container for both lanes)
    Tools/send_test_tone.py        reference sender for the wire format
    Tests/                         see above
