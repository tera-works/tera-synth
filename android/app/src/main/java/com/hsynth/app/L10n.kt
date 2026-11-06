package com.hsynth.app

import java.util.Locale

/**
 * UI language. The source strings are English; Japanese replaces them when the
 * device language is Japanese (or when chosen in the Monitor tab).
 *
 * Terms that Japanese musicians normally read in English stay English:
 * parameter names (Cutoff, Attack, ...), effect names (Reverb, Delay, ...),
 * VA / FM / PCM / WT / LFO / FX, MIDI, MPE, DSP, BPM, SFZ, WAV, IR.
 *
 * Ui.text / Ui.button translate their text automatically (exact match);
 * texts with values use f() with a "%s" template; log() messages are matched
 * against the patterns in [logPatterns].
 */
object L10n {
    var ja = false
        private set

    /** mode: "auto" (device language), "ja" or "en". Call before building the UI. */
    fun init(mode: String) {
        ja = when (mode) {
            "ja" -> true
            "en" -> false
            else -> Locale.getDefault().language == "ja"
        }
    }

    fun t(en: String): String {
        if (!ja) return en
        dict[en]?.let { return it }
        return en
    }

    fun f(en: String, vararg args: Any?): String = String.format(t(en), *args)

    /** Log line in the UI language (engine warnings that match nothing stay English). */
    fun log(msg: String): String {
        if (!ja) return msg
        dict[msg]?.let { return it }
        for ((re, ja) in logPatterns) {
            val m = re.matchEntire(msg) ?: continue
            var out = ja
            for (i in m.groupValues.indices.reversed()) out = out.replace("$$i", m.groupValues[i])
            return out
        }
        return msg
    }

    // Only what needs to be read as a sentence is translated: hints, dialogs,
    // settings descriptions, messages. Panel labels and buttons stay English,
    // as on a hardware synth (Hold, Octave, Filter, Save ...).
    private val dict = mapOf(
        // play
        "Quick controls" to "Sound", "Panic" to "All Off",
        // edit
        "%s engine: %s" to "%s の音源: %s",
        "  (layer %s is currently '%s' — set Type below to '%s' to hear these)" to "（%s は現在「%s」です。Type を「%s」にすると、この設定が鳴ります）",
        "out += source × via × amount   (via 'none' = 1)" to "出力 += source × via × amount（via が none なら 1）",
        "The vocoder needs an audio input (microphone). Input capture is not wired up in the Android app yet; it works in the desktop build." to
            "ボコーダーには音声入力（マイク）が必要です。Android 版はまだ入力に対応していません（デスクトップ版では使えます）。",
        "(none)" to "（なし）",
        "Sound for part %d" to "パート %d の音色", "Store scene %d" to "シーン %d に保存",
        // multi
        "Multi mode: 16 parts, layered by MIDI channel and split by key range. The on-screen keyboard plays channel 1." to
            "マルチモードでは 16 パートを鳴らせます。同じ MIDI チャンネルのパートは重なり、鍵域を分けるとスプリットになります。画面の鍵盤は ch 1 です。",
        "Tap a scene to recall it. Store, then a scene, saves the current multi there." to
            "シーンをタップすると呼び出します。Store を押してからシーンをタップすると、今のマルチを保存します。",
        // library
        "user · long-press to delete" to "User · 長押しで削除", "  · long-press to delete" to "  · 長押しで削除",
        "(no song loaded)" to "（曲が読み込まれていません）", "(no MIDI files)\n%s" to "（MIDI ファイルがありません）\n%s",
        "(no samples yet)" to "（サンプルはまだありません）",
        "Load %s into…" to "%s の読み込み先", "Samples for layer %s" to "レイヤー %s のサンプル", "Delete %s?" to "%s を削除しますか？",
        "The .sfz file is removed; its sample folder is kept." to ".sfz ファイルを削除します。サンプルのフォルダーは残ります。",
        "This removes the file from the app library." to "アプリのライブラリからこのファイルを削除します。",
        "Delete" to "削除", "Cancel" to "キャンセル", "Save preset" to "プリセットを保存", "Save" to "保存",
        "SFZ support" to "SFZ の対応状況",
        "Supported headers: <control> <global> <master> <group> <region>, #define, #include.\n\nSupported opcodes:\n%s\n\nAll other opcodes are ignored and listed as warnings in the message log when an instrument is loaded. Samples must be WAV (8/16/24/32-bit PCM or float). FLAC is planned." to
            "対応ヘッダー: <control> <global> <master> <group> <region>、#define、#include。\n\n対応オペコード:\n%s\n\nそれ以外のオペコードは無視され、音源を読み込んだときにメッセージ欄へ警告として表示されます。サンプルは WAV（8/16/24/32 ビット PCM または浮動小数点）に対応しています。FLAC は対応予定です。",
        // system: settings descriptions
        "Low (SBC / power saving)" to "Low（省電力）", "Auto (latency tuner)" to "Auto（遅延を自動調整）",
        "Audio output (applies on restart)" to "Audio Output（再起動で反映）",
        "Exclusive mode (lower latency when supported)" to "Exclusive（対応機種では遅延が小さくなります）",
        "System default" to "システムの既定", "Bluetooth (high latency)" to "Bluetooth（遅延大）",
        "MPE (channel 1 = master, 2-16 = per-note)" to "MPE（ch 1 = マスター、ch 2〜16 = ノートごと）",
        "Program Change selects the n-th preset in the library list. MIDI 2.0 UMP devices are opened in UMP mode on Android 13+ (per-note pitch bend, per-note controllers)." to
            "プログラムチェンジで、ライブラリの一覧の n 番目のプリセットを選びます。MIDI 2.0（UMP）機器は Android 13 以降で UMP モードで開きます（ノートごとのピッチベンド・コントローラーに対応）。",
        "MIDI service not available" to "MIDI サービスを利用できません",
        "No MIDI devices connected (USB MIDI hot-plug is detected automatically)" to "MIDI 機器が接続されていません（USB MIDI は接続すると自動で認識します）",
        "Auto (device)" to "Auto（端末の設定）",
        // player
        "(no songs)\nAdd some with \"Open Folder\" / \"Open Songs\"\n%s" to "（曲がありません）\n「Open Folder」「Open Songs」で追加できます\n%s",
        "Loading sounds…" to "音色を読み込んでいます…", "Cannot load: %s" to "読み込めません: %s",
    )

    private val logPatterns: List<Pair<Regex, String>> = listOf(
        "HSynth (.*) starting" to "HSynth $1 を起動しています",
        "factory content install failed: (.*)" to "内蔵コンテンツのインストールに失敗しました: $1",
        "audio output could not be started \\(see Monitor tab\\)" to "オーディオ出力を開始できませんでした（モニタータブを確認してください）",
        "loading preset (.*)…" to "プリセット $1 を読み込んでいます…",
        "preset load failed: (.*)" to "プリセットを読み込めませんでした: $1",
        "preset \"(.*)\" loaded" to "プリセット「$1」を読み込みました",
        "save failed: (.*)" to "保存できませんでした: $1",
        "preset saved: (.*)" to "プリセットを保存しました: $1",
        "loading (.*) into layer (.)…" to "$1 をレイヤー $2 に読み込んでいます…",
        "layer (.): (.*) loaded in (\\d+) ms" to "レイヤー $1: $2 を読み込みました（$3 ms）",
        "load failed: (.*)" to "読み込めませんでした: $1",
        "MIDI program change (\\d+)" to "MIDI プログラムチェンジ $1",
        "no samples in the library yet — import WAV or SFZ on the Library tab" to "ライブラリにサンプルがありません。ライブラリタブで WAV か SFZ を読み込んでください",
        "deleted (.*)" to "$1 を削除しました",
        "could not delete (.*)" to "$1 を削除できませんでした",
        "wavetable loaded: (.*)" to "ウェーブテーブルを読み込みました: $1",
        "wavetable load failed: (.*)" to "ウェーブテーブルを読み込めませんでした: $1",
        "wavetable import failed: (.*)" to "ウェーブテーブルを取り込めませんでした: $1",
        "IR loaded: (.*) \\((\\d+) samples\\)" to "IR を読み込みました: $1（$2 サンプル）",
        "IR load failed: (.*)" to "IR を読み込めませんでした: $1",
        "IR import failed: (.*)" to "IR を取り込めませんでした: $1",
        "imported preset (.*)" to "プリセット $1 を取り込みました",
        "imported (.*)" to "$1 を取り込みました",
        "import failed: (.*)" to "取り込めませんでした: $1",
        "importing folder…" to "フォルダーを取り込んでいます…",
        "no \\.sfz file found in the selected folder" to "選んだフォルダーに .sfz ファイルがありません",
        "(.*): (\\d+) regions, (\\d+) samples, (\\d+) MB" to "$1: リージョン $2、サンプル $3、$4 MB",
        "folder import failed: (.*)" to "フォルダーを取り込めませんでした: $1",
        "preset import failed: (.*)" to "プリセットを取り込めませんでした: $1",
        "preset exported \\(sample paths are relative to the app library\\)" to "プリセットを書き出しました（サンプルのパスはアプリのライブラリからの相対パスです）",
        "export failed: (.*)" to "書き出せませんでした: $1",
        "copied (\\d+) files…" to "$1 個のファイルをコピーしました…",
        "copied (\\d+) files from (.*)" to "$2 から $1 個のファイルをコピーしました",
        "song failed: (.*)" to "曲を読み込めませんでした: $1",
        "playing (.*) \\((.*)\\)" to "再生中: $1（$2）",
        "adding songs failed: (.*)" to "曲を追加できませんでした: $1",
        "added (\\d+) song\\(s\\)" to "$1 曲を追加しました",
    ).map { (re, ja) -> Regex(re) to ja }
}
