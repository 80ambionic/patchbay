cmake_minimum_required(VERSION 3.22)
project(PatchBay VERSION 0.1.0 LANGUAGES C CXX)

option(PATCHBAY_BUILD_TESTS "Build the platform-independent test suite" OFF)

if(APPLE)
  enable_language(OBJCXX)      # NetworkMidiSession.mm wraps Apple's MIDINetworkSession
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is only needed for the app (Apple) and the UDP loopback test. On other platforms the pure test
# suite builds without it; set PATCHBAY_WITH_JUCE=ON to include JUCE there too (needs X11/ALSA dev packages
# on Linux, which JUCE's CMake helper requires even for console apps).
option(PATCHBAY_WITH_JUCE "Fetch JUCE even when not building the Apple app" OFF)
if(APPLE OR PATCHBAY_WITH_JUCE)
  include(FetchContent)
  FetchContent_Declare(JUCE
      GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
      GIT_TAG        8.0.6
      GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
  juce_add_gui_app(PatchBay
      PRODUCT_NAME "PatchBay"
      COMPANY_NAME "YourCompany"
      BUNDLE_ID    com.yourcompany.patchbay
      VERSION      ${PROJECT_VERSION}
      # iOS / iPadOS
      BACKGROUND_AUDIO_ENABLED       TRUE   # UIBackgroundModes: audio
      BLUETOOTH_PERMISSION_ENABLED   TRUE   # BLE-MIDI
      BLUETOOTH_PERMISSION_TEXT      "PatchBay connects to Bluetooth MIDI devices."
      # iOS 14+ shows a permission prompt the first time the app receives data from the local network
      PLIST_TO_MERGE "<plist><dict><key>NSLocalNetworkUsageDescription</key><string>PatchBay receives network audio and MIDI from other devices on your network.</string><key>NSBonjourServices</key><array><string>_apple-midi._udp</string></array><key>NSAudioCaptureUsageDescription</key><string>PatchBay captures audio from the browser or music app you choose, so you can mix it.</string></dict></plist>"
      MICROPHONE_PERMISSION_ENABLED  TRUE   # needed once the mixer opens inputs
      MICROPHONE_PERMISSION_TEXT     "PatchBay needs audio input for the mixer."
      IPHONE_SCREEN_ORIENTATIONS     UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
      IPAD_SCREEN_ORIENTATIONS       UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight UIInterfaceOrientationPortrait
  )

  target_sources(PatchBay PRIVATE
      Source/Main.cpp
      Source/MainComponent.cpp
      Source/StateFile.cpp
      Source/MixerPresets.cpp
      Source/Midi/VirtualBuses.cpp
    Source/Midi/NetworkMidiSession.mm
    Source/Capture/AppCapture.mm
      Source/Net/UdpReceiver.cpp
    Source/Net/UdpSender.cpp
    Source/Net/NetworkEngine.cpp
    Source/UI/MixerPage.cpp
    Source/UI/AnalyzerPanel.cpp
    Source/UI/NetworkAudioLane.cpp
    Source/UI/NetworkMidiLane.cpp
    Source/UI/NetworkPage.cpp
      Source/Midi/MidiTopology.cpp
      Source/Midi/MidiRouter.cpp
      Source/UI/DeviceBlock.cpp
      Source/UI/PatchbayCanvas.cpp)

  target_include_directories(PatchBay PRIVATE Source)

  # AppCapture.mm owns Objective-C objects and is written for ARC (it #errors without it).
  set_source_files_properties(Source/Capture/AppCapture.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

  target_compile_definitions(PatchBay PRIVATE
      JUCE_WEB_BROWSER=0
      JUCE_USE_CURL=0
      JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PatchBay,JUCE_PRODUCT_NAME>"
      JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

  target_link_libraries(PatchBay
      PRIVATE
          juce::juce_core
          juce::juce_events
          juce::juce_graphics
          juce::juce_gui_basics
          juce::juce_audio_devices      # network audio output, AVAudioSession
          juce::juce_audio_utils        # AudioDeviceSelectorComponent (output device picker)
          "-framework CoreMIDI"
          "-framework CoreAudio"
          "-framework CoreFoundation"
      PUBLIC
          juce::juce_recommended_config_flags)
endif()

# ---------------------------------------------------------------------------------------------------
# Tests: no CoreMIDI, so they build and run on Linux/macOS/Windows.
#   cmake -S . -B build-tests -DPATCHBAY_BUILD_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests
if(PATCHBAY_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)

  add_executable(net_tests Tests/net_tests.cpp)
  target_include_directories(net_tests PRIVATE Source)
  target_compile_features(net_tests PRIVATE cxx_std_20)
  target_link_libraries(net_tests PRIVATE Threads::Threads)
  add_test(NAME net_tests COMMAND net_tests)

  add_executable(mixer_tests Tests/mixer_tests.cpp)
  target_include_directories(mixer_tests PRIVATE Source)
  target_compile_features(mixer_tests PRIVATE cxx_std_20)
  target_link_libraries(mixer_tests PRIVATE Threads::Threads)
  add_test(NAME mixer_tests COMMAND mixer_tests)

  add_executable(netout_tests Tests/netout_tests.cpp Source/Net/UdpSender.cpp)
  target_include_directories(netout_tests PRIVATE Source)
  target_compile_features(netout_tests PRIVATE cxx_std_20)
  target_link_libraries(netout_tests PRIVATE Threads::Threads)
  add_test(NAME netout_tests COMMAND netout_tests)

  add_executable(analysis_tests Tests/analysis_tests.cpp)
  target_include_directories(analysis_tests PRIVATE Source)
  target_compile_features(analysis_tests PRIVATE cxx_std_20)
  target_link_libraries(analysis_tests PRIVATE Threads::Threads)
  add_test(NAME analysis_tests COMMAND analysis_tests)

  add_executable(capture_tests Tests/capture_tests.cpp)
  target_include_directories(capture_tests PRIVATE Source)
  target_compile_features(capture_tests PRIVATE cxx_std_20)
  target_link_libraries(capture_tests PRIVATE Threads::Threads)
  add_test(NAME capture_tests COMMAND capture_tests)

  add_executable(app_grouping_tests Tests/app_grouping_tests.cpp)
  target_include_directories(app_grouping_tests PRIVATE Source)
  target_compile_features(app_grouping_tests PRIVATE cxx_std_20)
  add_test(NAME app_grouping_tests COMMAND app_grouping_tests)

  if(COMMAND juce_add_console_app)
    juce_add_console_app(udp_loopback_test PRODUCT_NAME "udp_loopback_test")
    target_sources(udp_loopback_test PRIVATE Tests/udp_loopback_test.cpp Source/Net/UdpReceiver.cpp)
    target_include_directories(udp_loopback_test PRIVATE Source)
    target_compile_features(udp_loopback_test PRIVATE cxx_std_20)
    target_compile_definitions(udp_loopback_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(udp_loopback_test PRIVATE juce::juce_core PUBLIC juce::juce_recommended_config_flags)
    add_test(NAME udp_loopback_test COMMAND udp_loopback_test)

    juce_add_console_app(state_file_test PRODUCT_NAME "state_file_test")
    target_sources(state_file_test PRIVATE Tests/state_file_test.cpp Source/StateFile.cpp Source/MixerPresets.cpp)
    target_include_directories(state_file_test PRIVATE Source)
    target_compile_features(state_file_test PRIVATE cxx_std_20)
    target_compile_definitions(state_file_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(state_file_test PRIVATE juce::juce_core PUBLIC juce::juce_recommended_config_flags)
    add_test(NAME state_file_test COMMAND state_file_test)
  endif()
endif()
