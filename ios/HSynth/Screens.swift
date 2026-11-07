import SwiftUI
import UniformTypeIdentifiers

// Screens and editor parts ported from the Android app: envelope / filter
// graphs, sample loading, Multi (16 parts), the preset picker and System.

// ---------------------------------------------------------------- graphs

/// ADSR of "amp" or "env2" (times on a square-root scale so short and long both show).
struct EnvGraph: View {
    @EnvironmentObject var model: SynthModel
    let prefix: String

    var body: some View {
        let e = model.engine
        let ids = ["attack", "decay", "sustain", "release"].map { e.paramId(prefix + "." + $0) }
        TimelineView(.periodic(from: .now, by: 0.1)) { _ in
            Canvas { c, size in
                guard ids.allSatisfy({ $0 >= 0 }) else { return }
                let a = Double(e.paramValue(ids[0])), d = Double(e.paramValue(ids[1]))
                let s = Double(e.paramValue(ids[2])), r = Double(e.paramValue(ids[3]))
                let wa = (a / 10).squareRoot(), wd = (d / 20).squareRoot(), ws = 0.35, wr = (r / 20).squareRoot()
                let total = max(0.001, wa + wd + ws + wr)
                let w = size.width - 8, h = size.height - 8
                let x0: CGFloat = 4, y0: CGFloat = 4
                func x(_ t: Double) -> CGFloat { x0 + CGFloat(t / total) * w }
                let sy = y0 + CGFloat(1 - s) * h
                var p = Path()
                p.move(to: CGPoint(x: x0, y: y0 + h))
                p.addLine(to: CGPoint(x: x(wa), y: y0))
                p.addLine(to: CGPoint(x: x(wa + wd), y: sy))
                p.addLine(to: CGPoint(x: x(wa + wd + ws), y: sy))
                p.addLine(to: CGPoint(x: x(total), y: y0 + h))
                var fill = p
                fill.closeSubpath()
                c.fill(fill, with: .color(Hw.lcdFg.opacity(0.15)))
                c.stroke(p, with: .color(Hw.lcdFg), lineWidth: 2)
                for t in [wa, wa + wd, wa + wd + ws] {
                    var g = Path()
                    g.move(to: CGPoint(x: x(t), y: y0))
                    g.addLine(to: CGPoint(x: x(t), y: y0 + h))
                    c.stroke(g, with: .color(Hw.lcdDim.opacity(0.5)), lineWidth: 1)
                }
            }
        }
        .frame(height: 84)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

/// Approximate response of the filter (type, cutoff, resonance), 20 Hz .. 20 kHz, -48 .. +24 dB.
struct FilterGraph: View {
    @EnvironmentObject var model: SynthModel

    var body: some View {
        let e = model.engine
        let typeId = e.paramId("filter.type"), cutId = e.paramId("filter.cutoff"), resId = e.paramId("filter.reso")
        TimelineView(.periodic(from: .now, by: 0.1)) { _ in
            Canvas { c, size in
                guard typeId >= 0, cutId >= 0, resId >= 0 else { return }
                let type = e.formatParam(typeId).uppercased()
                let fc = Double(max(20, e.paramValue(cutId)))
                let q = 0.707 + Double(e.paramValue(resId)) * 9
                let w = size.width, h = size.height
                func y(_ db: Double) -> CGFloat { CGFloat((24 - min(24, max(-48, db))) / 72) * h }
                var zero = Path()
                zero.move(to: CGPoint(x: 0, y: y(0)))
                zero.addLine(to: CGPoint(x: w, y: y(0)))
                c.stroke(zero, with: .color(Hw.lcdDim.opacity(0.5)), lineWidth: 1)
                var p = Path()
                let n = 120
                for i in 0...n {
                    let f = 20 * pow(1000.0, Double(i) / Double(n))
                    let r = f / fc
                    let den = ((1 - r * r) * (1 - r * r) + (r / q) * (r / q)).squareRoot()
                    var mag: Double
                    if type.contains("OFF") { mag = 1 }
                    else if type.contains("HP") { mag = r * r / den }
                    else if type.contains("BP") { mag = (r / q) / den }
                    else if type.contains("NOTCH") || type.contains("BR") { mag = abs(1 - r * r) / den }
                    else { mag = 1 / den }
                    if type.contains("24") && !type.contains("BP") { mag *= mag }
                    let pt = CGPoint(x: CGFloat(i) / CGFloat(n) * w, y: y(20 * log10(max(1e-6, mag))))
                    if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
                }
                c.stroke(p, with: .color(Hw.lcdFg), lineWidth: 2)
            }
        }
        .frame(height: 84)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

// ---------------------------------------------------------------- samples

/// The sample sets of the edit part's layers: load from the Files app, clear.
struct SamplesPanel: View {
    @EnvironmentObject var model: SynthModel
    @State private var importing = false
    @State private var layer: Int32 = 0
    @State private var message = ""

    private var types: [UTType] {
        var t: [UTType] = [.folder, .audio, .wav]
        if let sfz = UTType(filenameExtension: "sfz") { t.append(sfz) }
        if let flac = UTType(filenameExtension: "flac") { t.append(flac) }
        return t
    }

    private func setName(_ l: Int) -> String {
        let n = model.engine.sampleSetName(Int32(l))
        return n.isEmpty ? "(none)" : n
    }

    var body: some View {
        let e = model.engine
        Panel(title: "Samples") {
            ForEach(0..<3, id: \.self) { l in
                HStack(spacing: 8) {
                    Text(["A", "B", "C"][l]).font(.system(size: 13, weight: .bold)).foregroundColor(Hw.silk).frame(width: 16)
                    Lcd(text: setName(l), size: 13)
                    HwButton(title: "Load…") {
                        layer = Int32(l)
                        importing = true
                    }
                    HwButton(title: "Clear") {
                        model.checkpoint()
                        e.unloadSamples(Int32(l))
                        model.revision += 1
                    }
                }
            }
            if !message.isEmpty {
                Text(message).font(.system(size: 12)).foregroundColor(Hw.lcdFg)
            }
            Text("SFZ は、サンプルの入ったフォルダごと選んでください。HSynth のフォルダ（library）の中のファイルはそのまま、ほかの場所のものは library/user/imported にコピーして使います。")
                .font(.system(size: 11)).foregroundColor(Hw.engrave).fixedSize(horizontal: false, vertical: true)
        }
        .id(model.revision)
        .fileImporter(isPresented: $importing, allowedContentTypes: types, allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            message = model.importSamples(url, layer: layer)
        }
    }
}

extension SynthModel {
    /// Copies a picked file / folder into Documents/library/user/imported (unless it is in the
    /// library already) and loads it into a layer of the edit part. Returns a message for the user.
    func importSamples(_ url: URL, layer: Int32) -> String {
        let fm = FileManager.default
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let lib = URL(fileURLWithPath: engine.libraryDir).resolvingSymlinksInPath().standardizedFileURL
        var target = url.resolvingSymlinksInPath().standardizedFileURL
        if !target.path.hasPrefix(lib.path + "/") {
            let dir = lib.appendingPathComponent("user/imported", isDirectory: true)
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
            let dest = dir.appendingPathComponent(url.lastPathComponent)
            try? fm.removeItem(at: dest)
            do {
                try fm.copyItem(at: url, to: dest)
            } catch {
                return "コピーできませんでした: \(error.localizedDescription)"
            }
            target = dest
        }
        var isDir: ObjCBool = false
        if fm.fileExists(atPath: target.path, isDirectory: &isDir), isDir.boolValue {
            guard let file = Self.firstFile(in: target, ext: "sfz") ?? Self.firstFile(in: target, ext: "wav")
                    ?? Self.firstFile(in: target, ext: "flac") else {
                return "フォルダに SFZ / WAV / FLAC のファイルがありません。"
            }
            target = file
        }
        checkpoint()
        if let err = engine.loadSamples(target.path, layer: layer) {
            return "読み込めませんでした: \(err)"
        }
        revision += 1
        let warnings = engine.lastWarnings
        return warnings.isEmpty ? "\(target.lastPathComponent) を読み込みました。"
                                : "\(target.lastPathComponent) を読み込みました（注意 \(warnings.count) 件: \(warnings[0])）"
    }

    static func firstFile(in dir: URL, ext: String) -> URL? {
        guard let it = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil) else { return nil }
        while let u = it.nextObject() as? URL {
            if u.pathExtension.lowercased() == ext { return u }
        }
        return nil
    }
}

// ---------------------------------------------------------------- presets

/// Searchable preset list in a sheet.
struct PresetPicker: View {
    @EnvironmentObject var model: SynthModel
    @Environment(\.dismiss) private var dismiss
    let title: String
    let onPick: (Int) -> Void
    @State private var search = ""

    var body: some View {
        let names = model.engine.presetNames
        let cats = model.engine.presetCategories
        let shown = names.indices.filter { i in
            search.isEmpty || (names[i] + " " + cats[i]).localizedCaseInsensitiveContains(search)
        }
        VStack(spacing: 8) {
            HStack {
                Text(title).font(.system(size: 16, weight: .bold)).foregroundColor(.white)
                Spacer()
                HwButton(title: "Close") { dismiss() }
            }
            TextField("Search", text: $search)
                .textFieldStyle(.plain).font(.system(size: 15, design: .monospaced)).foregroundColor(Hw.lcdFg)
                .padding(8).background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
            List(shown, id: \.self) { i in
                Button {
                    onPick(i)
                    dismiss()
                } label: {
                    HStack {
                        Text(names[i]).foregroundColor(.white)
                        Spacer()
                        Text(cats[i]).font(.system(size: 12)).foregroundColor(Hw.engrave)
                    }
                }
                .listRowBackground(Hw.panel)
            }
            .listStyle(.plain)
        }
        .padding(12)
        .background(Hw.chassis.ignoresSafeArea())
    }
}

// ---------------------------------------------------------------- multi

/// Multi: 16 parts (sound, MIDI channel, level, pan, mute, key range, transpose)
/// and the part the editor works on. Parts on one channel layer, key ranges split.
struct MultiScreen: View {
    @EnvironmentObject var model: SynthModel
    @State private var picking = false
    @State private var pickPart = 0

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 10) {
                HwButton(title: "Multi Mode", lit: model.multi) { model.setMulti(!model.multi) }
                Text("オンにすると 16 パートを同時に鳴らせます。同じ MIDI チャンネルのパートは重なり、鍵盤の範囲を分けるとスプリットになります。番号のボタンで、Edit 画面で編集するパートを選びます。")
                    .font(.system(size: 11)).foregroundColor(Hw.engrave).fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 8)
            ScrollView([.vertical, .horizontal]) {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        ForEach([("PART", 64), ("SOUND", 170), ("CH", 104), ("LEVEL", 96), ("PAN", 76), ("", 44),
                                 ("KEY LOW", 116), ("KEY HIGH", 116), ("TRANSPOSE", 104)], id: \.0) { t, w in
                            Text(t).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                                .frame(width: CGFloat(w), alignment: .leading)
                        }
                    }
                    ForEach(0..<16, id: \.self) { k in
                        PartRow(part: k) {
                            pickPart = k
                            picking = true
                        }
                    }
                }
                .padding(.horizontal, 8)
                .id(model.revision)
            }
        }
        .padding(.top, 8)
        .sheet(isPresented: $picking) {
            PresetPicker(title: "Part \(pickPart + 1)") { i in
                if !model.multi { model.setMulti(true) }
                model.selectPart(pickPart)
                model.load(preset: i)
            }
            .environmentObject(model)
        }
    }
}

struct PartRow: View {
    @EnvironmentObject var model: SynthModel
    let part: Int
    let pick: () -> Void
    @State private var tick = 0

    private static let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    private static func note(_ v: Float) -> String {
        let n = Int(v.rounded())
        return noteNames[((n % 12) + 12) % 12] + "\(n / 12 - 1)"
    }

    var body: some View {
        let e = model.engine
        let k = Int32(part)
        let muted = e.partMix(k, id: 2) > 0.5
        HStack(spacing: 6) {
            HwButton(title: "\(part + 1)", lit: model.editPart == part) { model.selectPart(part) }
                .frame(width: 64, alignment: .leading)
            Button(action: pick) {
                Lcd(text: model.partNames[part].isEmpty ? "Init" : model.partNames[part], size: 13)
            }
            .buttonStyle(.plain)
            .frame(width: 170)
            stepper(id: 3, lo: 1, hi: 16, width: 104) { "\(Int($0))" }
            Slider(value: Binding(get: { Double(e.partMix(k, id: 0)) },
                                  set: { e.setPartMix(k, id: 0, value: Float($0)); tick += 1 }), in: 0...1)
                .tint(Hw.led).frame(width: 96)
            Slider(value: Binding(get: { Double(e.partMix(k, id: 1)) },
                                  set: { e.setPartMix(k, id: 1, value: Float($0)); tick += 1 }), in: -1...1)
                .tint(Hw.led).frame(width: 76)
            HwButton(title: "M", lit: muted) {
                e.setPartMix(k, id: 2, value: muted ? 0 : 1)
                tick += 1
            }
            .frame(width: 44)
            stepper(id: 4, lo: 0, hi: 127, width: 116) { Self.note($0) }
            stepper(id: 5, lo: 0, hi: 127, width: 116) { Self.note($0) }
            stepper(id: 6, lo: -48, hi: 48, width: 104) { v in v > 0 ? "+\(Int(v))" : "\(Int(v))" }
        }
        .padding(.vertical, 2)
        .background(part % 2 == 0 ? Color(white: 0.09) : Color.clear)
        .opacity(model.multi || part == 0 ? 1 : 0.45)
        .id(tick)
    }

    private func stepper(id: Int32, lo: Float, hi: Float, width: CGFloat, format: @escaping (Float) -> String) -> some View {
        let e = model.engine
        let k = Int32(part)
        let v = e.partMix(k, id: id)
        return HStack(spacing: 2) {
            Button {
                e.setPartMix(k, id: id, value: max(lo, v - 1))
                tick += 1
            } label: {
                Text("◀").foregroundColor(.white).frame(width: 26, height: 34)
            }
            .buttonStyle(.plain)
            Text(format(v)).font(.system(size: 13, design: .monospaced)).foregroundColor(Hw.lcdFg)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
            Button {
                e.setPartMix(k, id: id, value: min(hi, v + 1))
                tick += 1
            } label: {
                Text("▶").foregroundColor(.white).frame(width: 26, height: 34)
            }
            .buttonStyle(.plain)
        }
        .frame(width: width)
    }
}

// ---------------------------------------------------------------- system

/// Audio status and settings, recording, MIDI in / out.
struct SystemScreen: View {
    @EnvironmentObject var model: SynthModel
    @AppStorage("maxVoices") private var maxVoices = 128
    @AppStorage("quality") private var quality = 1
    @State private var tick = 0
    @State private var recSeconds = -1.0
    @State private var lastRecording = ""
    private let timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        let e = model.engine
        let _ = tick  // the status below is read again every half second
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Panel(title: "Audio") {
                    Lcd(text: e.audioDevice, size: 12)
                    Text(String(format: "VOICES %d (PEAK %d)   DSP %d%% (PEAK %d%%)   CALLBACK %d%%   LATE %d",
                                e.activeVoices, e.peakVoices, Int(e.cpuLoad * 100), Int(e.cpuLoadPeak * 100),
                                Int(e.callbackLoadMax * 100), e.lateCallbacks))
                        .font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.lcdFg)
                    HStack {
                        HwButton(title: "Reset Peaks") { e.resetPeaks() }
                        HwButton(title: "Audio Input", lit: e.audioInputOn) {
                            e.setAudioInput(!e.audioInputOn)
                            tick += 1
                        }
                    }
                    Text("Audio Input をオンにすると、マイクの音をボコーダーやルーパーに使えます（最初に許可を求めます）。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "Recording") {
                    HStack {
                        HwButton(title: recSeconds >= 0 ? "■ Stop" : "● Rec", lit: recSeconds >= 0) { toggleRecording() }
                        Lcd(text: recSeconds >= 0 ? String(format: "REC %.1f s", recSeconds)
                                                  : (lastRecording.isEmpty ? "—" : lastRecording), size: 13)
                    }
                    Text("出力（エフェクトのあと）を最大 5 分まで録音し、「ファイル」アプリの HSynth > recordings に WAV で保存します。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "Settings") {
                    HStack(spacing: 16) {
                        HStack(spacing: 4) {
                            Text("POLYPHONY").font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                            HwButton(title: "◀") { maxVoices = max(8, maxVoices - 8) }
                            Lcd(text: "\(maxVoices)", size: 14).frame(width: 56)
                            HwButton(title: "▶") { maxVoices = min(128, maxVoices + 8) }
                        }
                        Picker("Quality", selection: $quality) {
                            Text("Low").tag(0)
                            Text("Normal").tag(1)
                            Text("High").tag(2)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 240)
                    }
                    Text("同時発音数を減らすと、重い音色や曲でも音切れしにくくなります。Quality は Low ほど軽く、High ほどきれいです。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "MIDI") {
                    Lcd(text: "IN: " + (e.midiInputs.isEmpty ? "(none)" : e.midiInputs.joined(separator: ", ")), size: 13)
                    HStack {
                        HwButton(title: "Refresh") {
                            e.refreshMidi()
                            tick += 1
                        }
                        Menu {
                            Button("Off") {
                                _ = e.openMidiOutput("")
                                tick += 1
                            }
                            ForEach(e.midiOutputs, id: \.self) { n in
                                Button(n) {
                                    _ = e.openMidiOutput(n)
                                    tick += 1
                                }
                            }
                        } label: {
                            Lcd(text: "OUT: " + (e.midiOutput.isEmpty ? "Off" : e.midiOutput), size: 13).frame(width: 280)
                        }
                    }
                    Text("MIDI OUT には、画面の鍵盤で弾いた音符を送ります。MIDI Learn は、Edit 画面の Learn をオンにしてつまみをタップし、コントローラーを動かすと割り当てられます。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
            }
            .padding(8)
        }
        .onReceive(timer) { _ in
            recSeconds = e.recordSeconds
            tick += 1
        }
        .onChange(of: maxVoices) { v in e.maxVoices = Int32(v) }
        .onChange(of: quality) { q in e.setQuality(Int32(q)) }
    }

    private func toggleRecording() {
        let e = model.engine
        if e.recordSeconds >= 0 {
            if let path = e.stopRecording() {
                lastRecording = (path as NSString).lastPathComponent
            } else {
                lastRecording = "(nothing recorded)"
            }
            recSeconds = -1
        } else {
            model.start()
            e.startRecording()
            recSeconds = 0
        }
    }
}
