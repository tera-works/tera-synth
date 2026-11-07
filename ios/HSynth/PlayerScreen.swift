import MediaPlayer
import SwiftUI
import UniformTypeIdentifiers

/// MIDI file player state: playlist (Documents/midi), transport, repeat, meters.
final class PlayerModel: ObservableObject {
    enum Repeat: String { case off = "Repeat: off", all = "Repeat: all", one = "Repeat: one" }

    let engine: HSynthEngine
    @Published var files: [URL] = []
    @Published var current = -1
    @Published var position = 0.0
    @Published var length = 0.0
    @Published var playing = false
    @Published var bpm = 120.0
    @Published var message = ""
    @Published var repeatMode = Repeat.all
    @Published var meters = [Float](repeating: 0, count: 16 * 9)
    @Published var masks = [UInt32](repeating: 0, count: 64)
    @Published var mute = [Bool](repeating: false, count: 16)
    @Published var solo = [Bool](repeating: false, count: 16)
    var seeking = false
    private var lastEnd = -1
    private var levels = [Float](repeating: 0, count: 16)
    private var timer: Timer?
    /// The app is in the background (no meters; the song keeps playing).
    var background = false
    private var shownTitle: String?
    private var shownPlaying: Bool?
    private var shownPos = 0.0
    /// Starts the audio output if it was stopped (set by SynthModel; lock screen play).
    var ensureAudio: (() -> Void)?

    init(engine: HSynthEngine) {
        self.engine = engine
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.tick() }
        setupRemoteCommands()
    }

    var title: String? { files.indices.contains(current) ? files[current].deletingPathExtension().lastPathComponent : nil }

    /// Lock screen / Control Center / headphone buttons.
    private func setupRemoteCommands() {
        let c = MPRemoteCommandCenter.shared()
        c.playCommand.addTarget { [weak self] _ in
            guard let self else { return .commandFailed }
            self.ensureAudio?()
            if !self.engine.songPlaying { self.togglePlay() }
            return .success
        }
        c.pauseCommand.addTarget { [weak self] _ in
            guard let self else { return .commandFailed }
            if self.engine.songPlaying { self.engine.songPlay(false) }
            return .success
        }
        c.togglePlayPauseCommand.addTarget { [weak self] _ in self?.ensureAudio?(); self?.togglePlay(); return .success }
        c.nextTrackCommand.addTarget { [weak self] _ in self?.next(manual: true); return .success }
        c.previousTrackCommand.addTarget { [weak self] _ in self?.prev(); return .success }
        c.changePlaybackPositionCommand.addTarget { [weak self] e in
            guard let self, let e = e as? MPChangePlaybackPositionCommandEvent else { return .commandFailed }
            self.engine.songSeek(e.positionTime)
            return .success
        }
    }

    private func updateNowPlaying() {
        let t = title
        // iOS runs the clock itself; refresh on changes and after a jump
        let jumped = abs(position - shownPos) > 2.0
        shownPos += playing ? 1.0 / 30 : 0
        guard t != shownTitle || playing != shownPlaying || jumped else { return }
        shownTitle = t
        shownPlaying = playing
        shownPos = position
        guard let t else { MPNowPlayingInfoCenter.default().nowPlayingInfo = nil; return }
        MPNowPlayingInfoCenter.default().nowPlayingInfo = [
            MPMediaItemPropertyTitle: t,
            MPMediaItemPropertyArtist: "HSynth",
            MPMediaItemPropertyPlaybackDuration: length,
            MPNowPlayingInfoPropertyElapsedPlaybackTime: position,
            MPNowPlayingInfoPropertyPlaybackRate: playing ? 1.0 : 0.0,
        ]
    }

    var midiDir: URL { URL(fileURLWithPath: engine.midiDir) }

    func refresh() {
        let fm = FileManager.default
        let items = (try? fm.contentsOfDirectory(at: midiDir, includingPropertiesForKeys: nil)) ?? []
        let cur = files.indices.contains(current) ? files[current] : nil
        files = items.filter { ["mid", "midi"].contains($0.pathExtension.lowercased()) }
            .sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }
        current = cur.flatMap { files.firstIndex(of: $0) } ?? -1
    }

    /// Copies chosen files into Documents/midi (kept on this device only).
    func importFiles(_ urls: [URL]) {
        var added = 0
        for u in urls {
            let ok = u.startAccessingSecurityScopedResource()
            defer { if ok { u.stopAccessingSecurityScopedResource() } }
            let dest = midiDir.appendingPathComponent(u.lastPathComponent)
            try? FileManager.default.removeItem(at: dest)
            if (try? FileManager.default.copyItem(at: u, to: dest)) != nil { added += 1 }
        }
        refresh()
        message = "\(added) song(s) added"
    }

    func play(index i: Int) {
        guard files.indices.contains(i) else { return }
        current = i
        message = "Loading sounds…"
        let path = files[i].path
        let engine = self.engine
        DispatchQueue.global(qos: .userInitiated).async {
            let err = engine.loadSong(path)
            DispatchQueue.main.async {
                if let err { self.message = "Cannot load: \(err)"; return }
                self.message = ""
                self.lastEnd = engine.songEndCount
                self.applyMutes()
                engine.songPlay(true)
            }
        }
    }

    func togglePlay() {
        if engine.songLength <= 0 { play(index: max(0, current)); return }
        engine.songPlay(!engine.songPlaying)
    }
    func stop() { engine.songPlay(false); engine.songSeek(0) }
    func prev() {
        if engine.songPosition > 3 || files.isEmpty { engine.songSeek(0); return }
        play(index: current <= 0 ? files.count - 1 : current - 1)
    }
    func next(manual: Bool) {
        guard !files.isEmpty else { return }
        let n = current + 1
        if n >= files.count {
            if manual || repeatMode == .all { play(index: 0) } else { stop() }
        } else { play(index: n) }
    }
    func cycleRepeat() {
        repeatMode = repeatMode == .off ? .all : repeatMode == .all ? .one : .off
        message = repeatMode.rawValue
    }

    func applyMutes() {
        let anySolo = solo.contains(true)
        for k in 0..<16 { engine.setPartMix(Int32(k), id: 2, value: mute[k] || (anySolo && !solo[k]) ? 1 : 0) }
    }

    private func tick() {
        position = seeking ? position : engine.songPosition
        length = engine.songLength
        playing = engine.songPlaying
        updateNowPlaying()
        let ends = engine.songEndCount
        if lastEnd < 0 { lastEnd = ends }
        if ends != lastEnd {
            lastEnd = ends
            if repeatMode == .one { engine.songSeek(0); engine.songPlay(true) } else { next(manual: false) }
        }
        if background { return }  // nothing to draw
        bpm = engine.songTempo(at: position)
        let m = engine.partMeters().map { $0.floatValue }
        if m.count == 16 * 9 {
            for k in 0..<16 { levels[k] = max(m[k * 9], levels[k] * 0.82) }
            var mm = m
            for k in 0..<16 { mm[k * 9] = levels[k] }
            meters = mm
        }
        masks = engine.noteMasks().map { $0.uint32Value }
    }
}

struct PlayerScreen: View {
    @EnvironmentObject var model: SynthModel
    @ObservedObject var player: PlayerModel
    @State private var showPlaylist = false
    @State private var importing = false

    private func fmt(_ s: Double) -> String { String(format: "%d:%02d", Int(max(0, s)) / 60, Int(max(0, s)) % 60) }

    var body: some View {
        VStack(spacing: 8) {
            HStack(alignment: .top, spacing: 8) {
                // LCD
                VStack(alignment: .leading, spacing: 2) {
                    Text(player.message).font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.led).lineLimit(1)
                    Text(player.files.indices.contains(player.current) ? player.files[player.current].deletingPathExtension().lastPathComponent : "—")
                        .font(.system(size: 22, weight: .bold, design: .monospaced)).foregroundColor(Hw.lcdFg).lineLimit(1)
                    Text(String(format: "BPM %.0f   TIME %@ / %@   SONG %@   VOICES %d", player.bpm, fmt(player.position), fmt(player.length),
                                player.current >= 0 ? "\(player.current + 1)/\(player.files.count)" : "-/\(player.files.count)", model.voices))
                        .font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.lcdFg).lineLimit(1)
                }
                .padding(10)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))

                Panel(title: "Transport") {
                    HStack(spacing: 6) {
                        HwButton(title: "⏮") { player.prev() }
                        HwButton(title: player.playing ? "⏸ Pause" : "▶ Play", lit: player.playing) { player.togglePlay() }
                        HwButton(title: "⏭") { player.next(manual: true) }
                        HwButton(title: player.repeatMode == .one ? "🔂" : "🔁", lit: player.repeatMode != .off) { player.cycleRepeat() }
                        HwButton(title: "■") { player.stop() }
                    }
                    Slider(value: Binding(get: { player.length > 0 ? player.position / player.length : 0 },
                                          set: { player.position = $0 * player.length }),
                           onEditingChanged: { editing in
                               player.seeking = editing
                               if !editing { model.engine.songSeek(player.position) }
                           })
                        .tint(Hw.led)
                }
                .frame(width: 400)
            }
            .frame(height: 120)

            HStack {
                HwButton(title: "Channels", lit: !showPlaylist) { showPlaylist = false }
                HwButton(title: "Playlist (\(player.files.count))", lit: showPlaylist) { showPlaylist = true }
                Spacer()
                HwButton(title: "Reset Mix") {
                    player.mute = Array(repeating: false, count: 16)
                    player.solo = Array(repeating: false, count: 16)
                    for k in 0..<16 { model.engine.setPartMix(Int32(k), id: 0, value: 1) }
                    player.applyMutes()
                }
                HwButton(title: "Open Songs") { importing = true }
            }

            if showPlaylist { playlist } else { channels }
        }
        .padding(8)
        .onAppear { player.refresh() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType.midi, UTType(filenameExtension: "mid") ?? .data],
                      allowsMultipleSelection: true) { result in
            if case let .success(urls) = result { player.importFiles(urls) }
        }
    }

    private var playlist: some View {
        ScrollView {
            if player.files.isEmpty {
                Text("No songs yet.\nAdd .mid files with \"Open Songs\", or copy them into HSynth/midi with the Files app or Finder.")
                    .font(.system(size: 13)).foregroundColor(Hw.engrave).padding()
            }
            LazyVStack(spacing: 4) {
                ForEach(player.files.indices, id: \.self) { i in
                    Button { player.play(index: i) } label: {
                        HStack {
                            Text(String(format: "%3d", i + 1)).font(.system(size: 13, design: .monospaced)).foregroundColor(Hw.engrave)
                            Text(player.files[i].deletingPathExtension().lastPathComponent)
                                .font(.system(size: 15, weight: i == player.current ? .bold : .regular))
                                .foregroundColor(i == player.current ? Hw.lcdFg : .white)
                            Spacer()
                        }
                        .padding(10)
                        .background(RoundedRectangle(cornerRadius: 3).fill(i == player.current ? Hw.lcdBg : Hw.key))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    private var channels: some View {
        let e = model.engine
        return ScrollView {
            VStack(spacing: 2) {
                HStack(spacing: 6) {
                    ForEach([("", 26), ("CH", 26), ("Sound / GM", 170), ("Level", 70), ("Vol", 54), ("Solo", 40)], id: \.0) { t, w in
                        Text(t.uppercased()).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave).frame(width: CGFloat(w), alignment: .leading)
                    }
                    Text("KEYBOARD").font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                    Spacer()
                }
                ForEach(0..<16, id: \.self) { k in
                    let used = e.songChannelUsed(Int32(k))
                    let muted = player.mute[k] || (player.solo.contains(true) && !player.solo[k])
                    HStack(spacing: 6) {
                        Button { player.mute[k].toggle(); player.applyMutes() } label: {
                            Text(player.mute[k] ? "✕" : "♪").foregroundColor(player.mute[k] ? .red : .white).frame(width: 26)
                        }.buttonStyle(.plain)
                        Text("\(k + 1)").font(.system(size: 13, design: .monospaced)).foregroundColor(.white).frame(width: 26, alignment: .leading)
                        VStack(alignment: .leading, spacing: 0) {
                            Text(used ? e.songChannelPreset(Int32(k)).replacingOccurrences(of: #"^SC GM2 (Capital|Rhythm Sets) [0-9]+ "#, with: "", options: .regularExpression) : "(unused)")
                                .font(.system(size: 13)).foregroundColor(.white).lineLimit(1)
                            Text(e.songChannelGm(Int32(k))).font(.system(size: 10)).foregroundColor(Hw.engrave).lineLimit(1)
                        }
                        .frame(width: 170, alignment: .leading)
                        meterBar(player.meters[k * 9]).frame(width: 70, height: 8)
                        Text("\(Int(e.partMix(Int32(k), id: 0) * 100))").font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.lcdFg)
                            .frame(width: 54)
                        Button { player.solo[k].toggle(); player.applyMutes() } label: {
                            Text("S").font(.system(size: 12, weight: .bold)).foregroundColor(player.solo[k] ? .black : .white)
                                .frame(width: 32, height: 24).background(RoundedRectangle(cornerRadius: 3).fill(player.solo[k] ? Hw.led : Hw.key))
                        }.buttonStyle(.plain).frame(width: 40)
                        MiniKeys(mask: Array(player.masks[k * 4..<k * 4 + 4]), color: Color(hue: Double(k) / 16, saturation: 0.65, brightness: 0.95))
                            .frame(height: 24)
                    }
                    .padding(.vertical, 3)
                    .background(k % 2 == 0 ? Color(white: 0.09) : Color.clear)
                    .opacity(used ? (muted ? 0.45 : 1) : 0.4)
                }
            }
        }
    }

    private func meterBar(_ v: Float) -> some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2).fill(Color(white: 0.16))
                RoundedRectangle(cornerRadius: 2).fill(Hw.led).frame(width: g.size.width * CGFloat(min(1, v * 1.4)))
            }
        }
    }
}

/// C1..B7 keyboard strip lighting the keys a part plays.
struct MiniKeys: View {
    let mask: [UInt32]
    let color: Color
    var body: some View {
        Canvas { c, size in
            let first = 24, last = 107
            let whites = (first...last).filter { ![1, 3, 6, 8, 10].contains($0 % 12) }
            let ww = size.width / CGFloat(whites.count)
            func on(_ n: Int) -> Bool { (mask[n >> 5] >> UInt32(n & 31)) & 1 == 1 }
            for (i, n) in whites.enumerated() {
                c.fill(Path(CGRect(x: CGFloat(i) * ww + 0.5, y: 0, width: ww - 1, height: size.height)), with: .color(on(n) ? color : Color(white: 0.9)))
            }
            var wi = 0
            for n in first...last {
                if [1, 3, 6, 8, 10].contains(n % 12) {
                    let x = CGFloat(wi) * ww - ww * 0.3
                    c.fill(Path(CGRect(x: x, y: 0, width: ww * 0.6, height: size.height * 0.6)), with: .color(on(n) ? color : Color(white: 0.1)))
                } else { wi += 1 }
            }
        }
    }
}
