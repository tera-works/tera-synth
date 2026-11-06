# macOS / iOS

HSynth runs on macOS (desktop host, same UI as Windows) and iOS (native SwiftUI
app). Both use the same C++ engine as Android and Windows.

## Layout

| Path | What |
|---|---|
| `engine/` | DSP engine (portable C++17). Builds and tests with CMake on macOS. |
| `desktop/src/main_macos.mm` | macOS entry point: AppKit window, Metal, Dear ImGui (OSX + Metal backends). |
| `desktop/src/platform_macos.mm` | File dialogs (NSOpenPanel / NSSavePanel), Finder. |
| `desktop/src/midi_in_coremidi.cpp`, `midi_out_coremidi.cpp` | CoreMIDI input / output (also used by iOS). |
| `desktop/src/audio_out.cpp` | Audio via miniaudio (CoreAudio on macOS, AVAudioSession / RemoteIO on iOS). |
| `ios/project.yml` | XcodeGen spec of the iOS app (the `.xcodeproj` is generated, not committed). |
| `ios/HSynth/Bridge/HSynthEngine.{h,mm}` | Objective-C++ bridge: engine, audio, MIDI, presets, parameters for Swift. |
| `ios/HSynth/*.swift` | SwiftUI app: Play (preset, Sound knobs, keyboard, wheels), Edit (all parameters by page), Library (search, categories, save). |
| `.github/workflows/apple.yml` | CI on macOS runners: engine build + tests, macOS app, iOS app (simulator, unsigned). |

## Build on a Mac

Requirements: Xcode 15+ (command line tools), CMake 3.18+, Homebrew.

```bash
# engine + tests
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine -j
./build/engine/hsynth_tests

# macOS desktop app (build/desktop/HSynth.app)
cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j
open build/desktop/HSynth.app

# iOS app
brew install xcodegen
cd ios && xcodegen && open HSynth.xcodeproj
```

For the iOS app on a device, set a development team in Xcode (Signing &
Capabilities) once. The bundle id is `com.hsynth.ios`.

The macOS app reads user data from `desktop/data` and factory content from
`android/app/src/main/assets` by default (`--data` / `--factory` override,
`--autotest DIR [--mute]` runs the scripted smoke test like on Windows).

## Content rules

The sampled test sounds (GM test set, SonicCell captures) and the MIDI files
are for testing only and are never committed or bundled. The iOS app bundles
only `android/app/src/main/assets/presets` and `library/factory`.

## Status (handed over from the Windows session, 2026-11)

- Written on Windows without a Mac; the GitHub Actions macOS jobs are the first
  compile check. Fix anything they report first.
- macOS desktop: feature parity with the Windows host expected (same `App`
  code). Not yet run on a real Mac: check audio device choice, CoreMIDI
  hot-plug (ports are opened at start / on "rescan"), Retina font sizes,
  drag & drop of WAV / SFZ / presets, `--autotest` screenshots.
- iOS: first version. Missing compared with Android: Multi page, Player (MIDI
  files), graphic editors (envelope, filter, FM algorithm, step grid, MSEG),
  MIDI learn / out, undo / compare, recording, sample import (Files app),
  hardware-style knobs everywhere. The Android Kotlin UI (`android/app/src/main/java/com/hsynth/app/`)
  is the reference for porting these screens.
- iOS audio: miniaudio with a 5 ms preferred buffer; check latency and
  interruptions (phone call, route change, other apps) on a device. An
  AUv3 (Audio Unit extension) version for GarageBand / AUM would be a natural
  next step.
