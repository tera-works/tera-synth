import SwiftUI

/// Shared engine state for the views.
final class SynthModel: ObservableObject {
    let engine: HSynthEngine
    @Published var presetName = "Init"
    @Published var presetIndex = -1
    @Published var voices = 0
    @Published var cpu: Float = 0
    @Published var revision = 0          // bumps when parameters change wholesale (preset load)
    @Published var lcdOverride: String? = nil
    private var timer: Timer?
    private var lcdClear: DispatchWorkItem?

    init() {
        let factory = Bundle.main.resourcePath ?? ""
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        engine = HSynthEngine(factoryDir: factory, userDir: docs)
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.voices = Int(self.engine.activeVoices)
            self.cpu = self.engine.cpuLoad
        }
        let last = UserDefaults.standard.integer(forKey: "lastPreset")
        load(preset: engine.presetNames.indices.contains(last) ? last : 0)
    }

    func start() { _ = engine.startAudio() }
    func stop() { engine.stopAudio() }

    func load(preset i: Int) {
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
            if p == .active { model.start() } else if p == .background { model.stop() }
        }
    }
}
