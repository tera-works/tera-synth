import AVFoundation
import SwiftUI

/// Shared engine state for the views.
final class SynthModel: ObservableObject {
    let engine: HSynthEngine
    let player: PlayerModel
    @Published var presetName = "Init"
    @Published var presetIndex = -1
    @Published var voices = 0
    @Published var cpu: Float = 0
    @Published var revision = 0          // bumps when parameters change wholesale (preset load)
    @Published var lcdOverride: String? = nil
    private var timer: Timer?
    private var lcdClear: DispatchWorkItem?
    private var audioOn = false
    private var background = false
    private var idleSince: Date?

    init() {
        let factory = Bundle.main.resourcePath ?? ""
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        engine = HSynthEngine(factoryDir: factory, userDir: docs)
        player = PlayerModel(engine: engine)
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.voices = Int(self.engine.activeVoices)
            self.cpu = self.engine.cpuLoad
            self.checkBackgroundIdle()
        }
        player.ensureAudio = { [weak self] in self?.start() }
        // a phone call or another app took the audio: pause the song, restart the audio afterwards
        NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
            guard let self, let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
            if type == .began {
                self.engine.songPlay(false)
            } else if !self.background || self.audioOn {
                self.engine.stopAudio()
                self.audioOn = self.engine.startAudio()
            }
        }
        let last = UserDefaults.standard.integer(forKey: "lastPreset")
        load(preset: engine.presetNames.indices.contains(last) ? last : 0)
    }

    private var autotestStarted = false

    func start() {
        if !audioOn { audioOn = engine.startAudio() }
        if (Self.autotestRequested || Self.libtestRequested) && !autotestStarted {
            autotestStarted = true
            if Self.libtestRequested { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.runLibtest() } }
            if Self.autotestRequested { runAutotest() }
        }
    }
    func stop() {
        engine.stopAudio()
        audioOn = false
    }

    /// Scene phase: a playing song keeps going in the background (UIBackgroundModes audio);
    /// otherwise the audio stops as before.
    func enterBackground() {
        background = true
        player.background = true
        idleSince = nil
        if !engine.songPlaying { stop() }
    }
    func enterForeground() {
        background = false
        player.background = false
        start()
    }

    /// Paused in the background for 10 minutes: stop the audio (iOS then suspends the app).
    private func checkBackgroundIdle() {
        guard background, audioOn else { return }
        if engine.songPlaying { idleSince = nil; return }
        if idleSince == nil { idleSince = Date() }
        else if Date().timeIntervalSince(idleSince!) > 600 { stop() }
    }

    func load(preset i: Int) {
        engine.songPlay(false)  // a preset replaces the song's sounds
        guard engine.loadPreset(i) else { return }
        presetIndex = i
        presetName = engine.currentPresetName
        revision += 1
        UserDefaults.standard.set(i, forKey: "lastPreset")
    }

    func step(_ d: Int) {
        let n = engine.presetNames.count
        guard n > 0 else { return }
        load(preset: ((presetIndex < 0 ? 0 : presetIndex + d) % n + n) % n)
    }

    /// The header LCD shows "Label value" for a moment while a control moves.
    func show(_ text: String) {
        lcdOverride = text
        lcdClear?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.lcdOverride = nil }
        lcdClear = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: w)
    }
}

@main
struct HSynthApp: App {
    @StateObject private var model = SynthModel()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
                .statusBarHidden(true)
                .persistentSystemOverlays(.hidden)
        }
        .onChange(of: phase) { p in
            if p == .active { model.enterForeground() } else if p == .background { model.enterBackground() }
        }
    }
}
