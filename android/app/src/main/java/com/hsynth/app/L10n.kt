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
        // Phase 5 / 6: AI morph, stems, sound packs
        "AI 2D" to "AI 2D", "+ IMPORT" to "+ 取り込み", "⬇ PACKS" to "⬇ パック",
        "SOUND LICENSES" to "サウンドのライセンス",
        "✦ STEMS (NPU)" to "✦ ステム分離 (NPU)", "4-STEM MIXER" to "4ステム ミキサー", "SEPARATE STEMS" to "ステム分離",
        "VOCALS" to "ボーカル", "DRUMS" to "ドラム", "BASS" to "ベース", "KEYS" to "鍵盤",
        "ALL" to "すべて", "MINUS-ONE (NO KEYS)" to "マイナスワン（鍵盤なし）", "KARAOKE (NO VOX)" to "カラオケ（ボーカルなし）",
        "DRUMS+BASS" to "ドラム＋ベース",
        "Download Spleeter 4-Stems FP16 (~40 MB)" to "Spleeter 4ステム FP16 をダウンロード（約40 MB）",
        "Fast Spectral (Instant, Offline)" to "高速スペクトル（即時・オフライン）",
        "Spleeter models are not on this device yet. Pick how to split the track:" to "Spleeter のモデルはまだ端末にありません。分離の方法を選んでください:",
        "Split the backing track into vocals / drums / bass / keys" to "バッキングトラックをボーカル / ドラム / ベース / 鍵盤に分離します",
        "Load a backing track first (OPEN…)" to "先にバッキングトラックを開いてください（OPEN…）",
        "Downloading Spleeter models…" to "Spleeter モデルをダウンロード中…",
        "Neural separation…" to "ニューラル分離中…", "Fast spectral separation…" to "高速スペクトル分離中…",
        "Loading stems…" to "ステムを読み込み中…", "STEMS READY" to "ステム準備完了", "cached" to "キャッシュ",
        "cancelling…" to "キャンセル中…", "cancelled" to "キャンセルしました", "CANCEL" to "キャンセル",
        "Download failed: %s" to "ダウンロードできませんでした: %s", "Separation failed: %s" to "分離できませんでした: %s",
        "NPU status unavailable" to "NPU の状態を取得できません", "last" to "直近",
        "%d anchors, %d params, compiled" to "アンカー %d、パラメーター %d、コンパイル済み",
        "NPU not available - CPU fallback" to "NPU が使えないため CPU で動作します",
        // edit
        "%s engine: %s" to "%s の音源: %s",
        "  (layer %s is currently '%s' — set Type below to '%s' to hear these)" to "（%s は現在「%s」です。Type を「%s」にすると、この設定が鳴ります）",
        "out += source × via × amount   (via 'none' = 1)" to "出力 += source × via × amount（via が none なら 1）",
        "Turn on Audio Input in SYSTEM to use the vocoder." to
            "ボコーダーを使うには SYSTEM で Audio Input を ON にしてください。",
        "(none)" to "（なし）",
        "Sound for part %d" to "パート %d の音色", "Store scene %d" to "シーン %d に保存",
        "Audio Input (mic / USB) for vocoder and looper" to "Audio Input（マイク / USB 入力。ボコーダーとルーパー用）",
        "Audio Input (mic / USB) for vocoder, looper, tuner and sampling" to
            "Audio Input（マイク / USB 入力。ボコーダー・ルーパー・チューナー・サンプリング用）",
        "Enable Audio Input" to "Audio Input を有効にする",
        // Phrase Recorder
        "Phrase Recorder" to "フレーズレコーダー",
        "Quantize" to "クオンタイズ",
        "Export SMF" to "SMF 書き出し",
        "%d bars" to "%d 小節",
        // Scala Tuning
        "Tuning / Microtonal" to "チューニング / 微分音",
        "Load .scl…" to ".scl を読み込む…",
        "Classic Temperaments" to "古典音律",
        "Equal" to "平均律",
        "Just (5-limit)" to "純正律（5限界）",
        "Pythagorean" to "ピタゴラス音律",
        "Meantone (1/4)" to "中全音律（1/4コンマ）",
        "Werckmeister III" to "ヴェルクマイスター III",
        "Kirnberger III" to "キルンベルガー III",
        "Arabic (Rast)" to "アラビア音階（ラースト）",
        // SST
        "Seamless Switch (SST)" to "シームレス切替（SST）",
        "Transition" to "トランジション",
        "Fast" to "速い",
        "Normal" to "標準",
        "Slow" to "遅い",
        // Split Crossfade
        "Split Crossfade" to "スプリットクロスフェード",
        // Master Stage
        "Global Master EQ & Comp" to "グローバルマスター EQ & Comp",
        "Master EQ & Comp…" to "マスター EQ & Comp…",
        "Bypass" to "バイパス",
        "Flat" to "フラット",
        "Bright PA" to "ブライト PA",
        "Warm Room" to "ウォームルーム",
        "IEM" to "IEM",
        // Super Knob
        "SUPER: no destinations" to "SUPER: 割り当て先なし",
        "Search name / category / tag" to "名前・カテゴリ・タグで検索",
        "Add to favourites" to "お気に入りに追加", "Remove from favourites" to "お気に入りから外す",
        // multi
        "Multi mode: 16 parts, layered by MIDI channel and split by key range. The on-screen keyboard plays the channel of the part being edited." to
            "マルチモードでは 16 パートを鳴らせます。同じ MIDI チャンネルのパートは重なり、鍵域を分けるとスプリットになります。画面の鍵盤は、エディット中のパートのチャンネルで鳴ります。",
        "Tap a scene to recall it. Store, then a scene, saves the current multi there." to
            "シーンをタップすると呼び出します。Store を押してからシーンをタップすると、今のマルチを保存します。",
        // live screen
        "Live" to "ライブ",
        "Panel" to "パネル",
        "NO MIDI DEVICE" to "MIDI 機器なし",
        "hold to store" to "長押しで登録",
        "empty slot — long press to store the current preset" to "空きスロットです。長押しで今のプリセットを登録します",
        "Fader" to "フェーダー",
        "Category" to "カテゴリ",
        "LIVE SETS — 16 ch" to "ライブセット（16ch）",
        "Single sounds: all" to "単体の音色：すべて",
        "choose a set" to "セットを選んでください",
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
        "Sample rate" to "サンプルレート",
        "Exclusive mode (lower latency when supported)" to "Exclusive（対応機種では遅延が小さくなります）",
        "Direct ALSA (USB DAC / root)" to "Direct ALSA（USB DAC / root）",
        "Power saving" to "省電力 / スリープ",
        "Off" to "オフ",
        "%d min" to "%d 分",
        "Screen off" to "画面自動消灯",
        "Audio standby" to "音声スタンバイ",
        "Deep sleep" to "ディープスリープ",
        "Auto power off" to "自動電源 OFF",
        "Auto power off on battery only (root)" to "バッテリー駆動時のみ自動電源 OFF（root）",
        "Screen turns off first, then audio pauses while BLE MIDI stays ready to wake on any note. Tap the screen or press the power button to wake." to
            "無演奏が続くと「画面消灯 → 音声スタンバイ（BLE MIDI 接続維持・鍵盤入力で即復帰）→ ディープスリープ → 自動電源 OFF」の順に省電力移行します。消灯後は画面タップ・鍵盤入力・電源ボタンのいずれでも復帰できます。",
        "System default" to "システムの既定", "Bluetooth (high latency)" to "Bluetooth（遅延大）",
        "MPE (channel 1 = master, 2-16 = per-note)" to "MPE（ch 1 = マスター、ch 2〜16 = ノートごと）",
        "Program Change selects the n-th preset in the library list. MIDI 2.0 UMP devices are opened in UMP mode on Android 13+ (per-note pitch bend, per-note controllers)." to
            "プログラムチェンジで、ライブラリの一覧の n 番目のプリセットを選びます。MIDI 2.0（UMP）機器は Android 13 以降で UMP モードで開きます（ノートごとのピッチベンド・コントローラーに対応）。",
        "MIDI service not available" to "MIDI サービスを利用できません",
        "No MIDI devices connected (USB MIDI hot-plug is detected automatically)" to "MIDI 機器が接続されていません（USB MIDI は接続すると自動で認識します）",
        "Auto (device)" to "Auto（端末の設定）",
        "Bluetooth LE is not available on this device" to "この端末では Bluetooth LE を利用できません",
        "Scan finds BLE MIDI keyboards and controllers; tap one to connect (it is reconnected next time). " +
            "Peripheral mode lets a PC or tablet connect to this phone for playing and SysEx remote control." to
            "Scan で BLE MIDI のキーボードやコントローラーを探し、タップすると接続します（次回は自動で再接続）。" +
            "Peripheral をオンにすると、PC やタブレットからこの端末に Bluetooth で接続して、演奏や SysEx リモート操作ができます。",
        "Searching…" to "検索中…",
        "Export setup…" to "セットアップを書き出す…",
        "Import setup…" to "セットアップを読み込む…",
        "A setup file holds the parts (channel, preset, mix), effects, MIDI learn and polyphony, for this app, the desktop app and iPhone." to
            "セットアップファイルには、パート（MIDI チャンネル・プリセット・ミックス）、エフェクト、MIDI Learn、最大発音数がまとめて入ります。PC 版・iPhone 版とも共通です。",
        "setup exported" to "セットアップを書き出しました",
        "setup import failed: %s" to "セットアップを読み込めませんでした: %s",
        "setup \"%s\" imported" to "セットアップ「%s」を読み込みました",
        "Advertising, waiting for a connection" to "接続を待っています（アドバタイズ中）",
        "Connected: %s" to "接続中: %s",
        "Bluetooth permission denied" to "Bluetooth の権限が許可されませんでした",
        // player
        "(no songs)\nAdd some with \"Open Folder\" / \"Open Songs\"\n%s" to "（曲がありません）\n「Open Folder」「Open Songs」で追加できます\n%s",
        "Loading sounds…" to "音色を読み込んでいます…", "Cannot load: %s" to "読み込めません: %s",
        // Phase 2: SetList / Banks / Chain / Memo
        "Song Notes / Chords" to "曲メモ / コード進行",
        "Import .txt…" to ".txt を読み込む…",
        "Show on recall" to "呼び出し時に表示",
        "STORE CURRENT" to "現在の状態を登録",
        "CLEAR" to "クリア",
        "Slot" to "スロット",
        "Bank" to "バンク",
        "Chain" to "チェーン",
        // Phase 2: Metronome & Tuner
        "Metronome" to "メトロノーム",
        "Tuner" to "チューナー",
        "Count-in" to "カウントイン",
        "Route" to "出力先",
        "Mute Out" to "出力ミュート",
        "Hold" to "ホールド",
        "Play a note or sing into Audio Input" to "楽器を弾くか、Audio Input に向かって歌ってください",
        // Phase 2: Scope
        "Scope" to "スコープ",
        "Spectrum" to "スペクトラム",
        "Wave" to "波形",
        "Tap to freeze · Long-press to switch mode" to "タップで一時停止 · 長押しでモード切替",
        // Phase 2: Pedals & Footswitches
        "Pedals & Controllers" to "ペダル & コントローラー",
        "Sustain pedal" to "サステインペダル",
        "Inverted" to "反転",
        "Velocity curve" to "ベロシティカーブ",
        "Aftertouch" to "アフタータッチ",
        "Footswitches" to "フットスイッチ",
        "Scene Next" to "次のシーン",
        "Scene Prev" to "前のシーン",
        "SetList Next" to "次のセットリスト",
        "SetList Prev" to "前のセットリスト",
        "Rotary Fast/Slow" to "ロータリー Fast/Slow",
        "Looper Tap" to "ルーパー タップ",
        "Tap Tempo" to "タップテンポ",
        "Metronome Toggle" to "メトロノーム ON/OFF",
        "Phrase Rec/Dub" to "フレーズ 録音/オーバーダブ",
        // Phase 3: Physical Modeling
        "Physical Modeling" to "物理モデリング",
        "TEST NOTE" to "試聴ノート",
        "▶ TEST NOTE" to "▶ 試聴ノート",
        "preview" to "プレビュー",
        // Phase 3: Vintage / Spread
        "Vintage / Spread" to "ビンテージ / スプレッド",
        "drift · slop" to "ドリフト · スロップ",
        // Phase 3: Morph A<->B
        "SET A" to "A を設定",
        "CAPTURE B" to "B を記録",
        "SWAP" to "入替",
        "MORPH LEARN" to "モーフラーン",
        "CLEAR ALL" to "すべてクリア",
        "MORPH LEARN — move a knob" to "モーフラーン — ノブを動かしてください",
        "%d params assigned" to "%d 個のパラメーターを割り当て済み",
        "Touch a knob while LEARN is on to set its end-point (B). Its current value is A." to
            "LEARN が ON の状態でノブに触れると終点 (B) を設定できます。現在の値が A になります。",
        // Phase 3: Piano Realism
        "Piano Realism" to "ピアノリアリズム",
        "Piano realism" to "ピアノリアリズム",
        "Piano parameters not available" to "ピアノパラメーターを利用できません",
        "Pedal Noise" to "ペダルノイズ",
        "Soft Release" to "ソフトリリース",
        "Dynamic Comp" to "ダイナミックコンプ",
        "Lid" to "蓋（リッド）",
        "Half-Damper" to "ハーフダンパー",
        // Phase 3: Pattern Arp / Strum / Rhythm
        "Pattern Arp" to "パターンアルペジオ",
        "Strum" to "ストラム",
        "Rhythm Pattern" to "リズムパターン",
        "FILL" to "フィル",
        "PATTERNS" to "パターン",
        "RANDOM" to "ランダム",
        // Phase 4: Mutate / Init
        "Mutate" to "ミューテート",
        "Init this page" to "このページを初期化",
        "Init layer" to "レイヤーを初期化",
        "Init whole patch…" to "パッチ全体を初期化…",
        "Init whole patch" to "パッチ全体を初期化",
        "Reset every sound parameter to its default? You can undo this." to
            "すべてのサウンドパラメーターを初期値に戻しますか？取り消すこともできます。",
        "AGAIN" to "もう一度",
        "KEEP" to "キープ",
        "params" to "項目",
        // Phase 4: Backing Audio
        "Backing Track" to "バッキングトラック",
        "A-B Loop" to "A-B ループ",
        "OPEN…" to "開く…",
        "SPEED" to "再生速度",
        "PITCH" to "ピッチ",
        "DIRECT" to "ダイレクト",
        "Decoding audio…" to "オーディオをデコード中…",
        // Phase 4: 16-Pad Sampler
        "PADS" to "パッド",
        "STOP ALL" to "すべて停止",
        "ONE-SHOT" to "ワンショット",
        "LOOP" to "ループ",
        "GATE" to "ゲート",
        "CHOKE" to "チョーク",
        "VEL SENS" to "ベロシティ感度",
        "POSITION" to "再生位置",
        "AREA" to "範囲",
        "FIXED" to "固定",
        // Phase 4: Live Sampling Wizard
        "SAMPLE" to "サンプル",
        "Record…" to "録音…",
        "SOURCE" to "ソース",
        "RECORD" to "録音",
        "TRIM" to "トリム",
        "ASSIGN" to "割り当て",
        "AUDIO INPUT" to "オーディオ入力",
        "INTERNAL RESAMPLE" to "内部リサンプル",
        "ARM on signal" to "入力信号で録音開始",
        "AUTO TRIM" to "自動トリム",
        "NORMALIZE" to "ノーマライズ",
        "ZERO-X LOOP" to "ゼロクロスループ",
        "PREVIEW" to "プレビュー",
        "FINISH" to "完了",
        // Phase 4: External MIDI Zone
        "EXTERNAL MIDI" to "外部 MIDI",
        "ROUTE" to "ルート",
        "OUT PORT" to "出力ポート",
        "OUT CH" to "出力 ch",
        "BANK MSB" to "バンク MSB",
        "SEND NOW" to "今すぐ送信",
        "EXT NOTES OFF" to "外部ノートオフ",
        "External MIDI outputs" to "外部 MIDI 出力",
        "Bank / PC are sent whenever a scene or set-list slot is recalled." to
            "シーンやセットリストのスロットを呼び出すたびに Bank / PC を送信します。",
        // Phase 4: Backup & Web Editor
        "Backup / Restore" to "バックアップ / 復元",
        "Presets & settings" to "プリセットと設定",
        "+ user samples" to "+ ユーザーサンプル",
        "BACKUP NOW" to "今すぐバックアップ",
        "CHOOSE FOLDER…" to "フォルダーを選択…",
        "RESTORE…" to "復元…",
        "Replace everything" to "すべて置き換える",
        "Merge (keep newer)" to "統合（新しい方を残す）",
        "Wi-Fi Web Editor" to "Wi-Fi Web エディター",
        "Enable Web Editor (LAN only)" to "Web エディターを有効化（LAN 内のみ）",
        "NEW PIN" to "新しい PIN",
        "NO Wi-Fi NETWORK" to "Wi-Fi 未接続",
        "Already queued or installed." to "すでに順番待ち、またはインストール済みです。",
        "%s packs need about %s free (download + unpacking), but only %s is available." to
            "%s 個のパックのダウンロードと展開に約 %s の空き容量が必要ですが、%s しかありません。",
        "Not enough free space (need %s, free %s)" to "空き容量が足りません（必要 %s、空き %s）",
        "+%s queued" to "+%s 件待機",
        "Select packs first." to "先にパックを選択してください。",
        "BATCH: tap packs in the order you want them downloaded" to "一括選択: ダウンロードしたい順にパックをタップ",
    )

    private val logPatterns: List<Pair<Regex, String>> = listOf(
        "HSynth (.*) starting" to "HSynth $1 を起動しています",
        "factory content install failed: (.*)" to "内蔵コンテンツのインストールに失敗しました: $1",
        "audio output could not be started \\(see Monitor tab\\)" to "オーディオ出力を開始できませんでした（モニタータブを確認してください）",
        "Direct ALSA unavailable \\((.*)\\), falling back to AAudio" to "Direct ALSA を利用できません（$1）。AAudio にフォールバックします",
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
        "recording saved \\((.*) s\\): (.*)" to "録音を保存しました（$1 秒）: $2",
        "recording…" to "録音中…",
        "recording failed to start" to "録音を開始できませんでした",
        "audio input could not be opened" to "オーディオ入力を開けませんでした",
        "microphone permission denied" to "マイクの使用が許可されませんでした",
        "MIDI learn: move a controller for (.*)" to "MIDI ラーン: $1 に割り当てるコントローラーを動かしてください",
        "MIDI learn: (.*) = (.*)" to "MIDI ラーン: $1 = $2",
        "MIDI learn removed: (.*)" to "MIDI ラーンを解除しました: $1",
        "scene (\\d+) stored" to "シーン $1 を保存しました",
        "scene (\\d+) recalled" to "シーン $1 を呼び出しました",
    ).map { (re, ja) -> Regex(re) to ja }
}
