# Android Root セットアップガイド（専用ハードウェア音源モジュール化）

HSynth は **非 root の通常の Android 端末** でもそのまま動作します（AAudio 低遅延出力、USB / BLE MIDI、Tensor G2 Edge TPU 推論、全 7 音源フォーマット、アプリ内音源パックダウンローダーはすべて root 不要です）。

一方、**Magisk による root 環境** を組み合わせると、Android スマートフォンを **1 ms バースト・XRun 0・自動起動・省電力連動の「専用ハードウェア音源モジュール」** として極限まで最適化できます。

> [!WARNING]
> - 以下の手順は **Magisk（root）導入済み・ブートローダーアンロック済み端末** 向けです。
> - すべての変更は **再起動で元に戻せる起動時スクリプト**（`/data/adb/post-fs-data.d/` および `/data/adb/service.d/`）のみで完結し、`/system` や `/vendor` パーティションの改変、およびサーマル（熱保護）機能の無効化は一切行いません。
> - 主な検証環境は **Google Pixel 7（panther / Tensor G2 / Android 16 / Magisk）** です。他機種では `cpuidle` の CPU 番号や `/proc/vendor_sched` の有無が異なる場合があります（非対応パスは自動で安全にスキップされます）。

---

## 1. 非 Root vs Root（専用音源化）の比較

| 機能・指標 | 非 Root（通常利用） | Root（Magisk チューニング適用） |
|---|---|---|
| AAudio MMAP exclusive 低遅延出力 | 対応（通常 96f / 2 ms バースト） | **48f / 1 ms バースト**（Pixel 7 実測遅延 **3.9〜4.8 ms**, `xruns 0`） |
| Tensor G2 Edge TPU (`AI 2D Morph`) | 対応（`google-edgetpu`, `0.59 ms`） | 対応（同左） |
| 4-Stem AI 分離（ONNX / NNAPI） | 対応 | 対応 |
| SF2 / SF3 / DLS / GIG / DS / DX7 / WT | 対応 | 対応 |
| アプリ内 音源パックダウンローダー | 対応 | 対応 |
| 音声スレッド RT 優先度 & `cpuidle` 抑止 | OS 標準 | **`SCHED_FIFO` + MID/BIG `state1` 無効化**（起床遅延 p99 `550→49 µs`） |
| サンプルメモリの zram スワップ防止 | OS 標準 | cgroup v2 `memory.swap.max=0`, `memory.low=3G` |
| BLE MIDI 接続間隔 | 端末既定（15〜30 ms） | **`interval=6`（7.5 ms 固定）** |
| Direct ALSA（USB DAC 直接出力） | 不可（SELinux 制限） | **対応**（`magiskpolicy --live` + USB オートサスペンド抑止） |
| 無演奏時の段階省電力 & 自動シャットダウン | 画面消灯・音声スタンバイ | **4 段階連動**（画面消灯 → `cpuidle` 復帰 → USB 省電力 → 自動電源 OFF） |
| 起動時自動立ち上げ & ウォッチドッグ | 任意 | **対応**（`90-hsynth.sh` / 任意でホームアプリ化） |

---

## 2. 同梱スクリプト一覧（`android/root-tuning/`）

### `post-fs-data.d/`（システムサービス起動前に適用）
| スクリプト | 役割 | 元に戻す方法 |
|---|---|---|
| `85-bt-lowlat.sh` | Bluetooth 起動前に `resetprop` で BLE 接続間隔を `6`（7.5 ms）へ設定 | `rm /data/adb/post-fs-data.d/85-bt-lowlat.sh` + 再起動 |
| `86-alsa-direct.sh` | USB DAC 向け Direct ALSA 用の最小 SELinux 許可（`card 0` 除外）＆ USB autosuspend 無効化 | `rm /data/adb/post-fs-data.d/86-alsa-direct.sh` + 再起動 |
| `87-aaudio-1ms.sh` | `audioserver` 起動前に `aaudio.hw_burst_min_usec=1000`（48f / 1 ms）を設定 | `rm /data/adb/post-fs-data.d/87-aaudio-1ms.sh` + 再起動 |

### `service.d/`（ブート完了後にバックグラウンド実行）
| スクリプト | 役割 | 元に戻す方法 |
|---|---|---|
| `85-audio-tune.sh` | `cpu4..7` の深い `cpuidle` 無効化、cgroup スワップ抑止、レンダースレッド `SCHED_FIFO` 付与、HSynth の `power_state.json` 連動省電力＆安全自動電源 OFF | `su -c "sh /data/adb/service.d/85-audio-tune.sh undo"` |
| `88-power-save.sh` | アダプティブ 60↔90 Hz リフレッシュレート、`screen_off_timeout=30s`、AOD・常時スキャン OFF、タップ復帰 ON | `su -c "sh /data/adb/service.d/88-power-save.sh undo"` |
| `90-hsynth.sh` | **（任意・専用機向け）** 起動時に HSynth を自動起動し、終了時に復帰＋バッテリー／温度ヘルスログ記録 | `rm /data/adb/service.d/90-hsynth.sh` |

> [!NOTE]
> `90-hsynth.sh` は **HSynth 専用機として使う場合のみ** 導入してください。普段使いのスマホに導入すると、アプリを閉じても 30 秒以内に自動再起動されます。

---

## 3. セットアップ手順

### 前提条件
- HSynth APK が端末にインストール済みであること（`adb install -r app-debug.apk` または `app-release.apk`）
- Magisk（root）が有効で、PC から `adb shell su -c id` が `uid=0(root)` を返すこと

### Step 1: スクリプトを端末へ転送して実行権限を付与

#### Windows (PowerShell)
```powershell
cd android\root-tuning

# 一時領域へ転送
adb push post-fs-data.d/85-bt-lowlat.sh  /data/local/tmp/
adb push post-fs-data.d/86-alsa-direct.sh /data/local/tmp/
adb push post-fs-data.d/87-aaudio-1ms.sh  /data/local/tmp/
adb push service.d/85-audio-tune.sh       /data/local/tmp/
adb push service.d/88-power-save.sh       /data/local/tmp/
# 専用機として自動起動したい場合のみ:
# adb push service.d/90-hsynth.sh         /data/local/tmp/

# Magisk 領域へ配置（念のため CRLF→LF 正規化 + 実行権限付与）
adb shell "su -c '
  mkdir -p /data/adb/post-fs-data.d /data/adb/service.d
  for f in 85-bt-lowlat.sh 86-alsa-direct.sh 87-aaudio-1ms.sh; do
    sed s/\r$// /data/local/tmp/$f > /data/adb/post-fs-data.d/$f
    chmod 755 /data/adb/post-fs-data.d/$f
    rm -f /data/local/tmp/$f
  done
  for f in 85-audio-tune.sh 88-power-save.sh; do
    sed s/\r$// /data/local/tmp/$f > /data/adb/service.d/$f
    chmod 755 /data/adb/service.d/$f
    rm -f /data/local/tmp/$f
  done
  if [ -f /data/local/tmp/90-hsynth.sh ]; then
    sed s/\r$// /data/local/tmp/90-hsynth.sh > /data/adb/service.d/90-hsynth.sh
    chmod 755 /data/adb/service.d/90-hsynth.sh
    rm -f /data/local/tmp/90-hsynth.sh
  fi
'"
```

#### macOS / Linux (bash / zsh)
```bash
cd android/root-tuning
for f in 85-bt-lowlat.sh 86-alsa-direct.sh 87-aaudio-1ms.sh; do
  adb push "post-fs-data.d/$f" "/data/local/tmp/$f"
  adb shell "su -c 'tr -d \"\r\" < /data/local/tmp/$f > /data/adb/post-fs-data.d/$f && chmod 755 /data/adb/post-fs-data.d/$f && rm -f /data/local/tmp/$f'"
done
for f in 85-audio-tune.sh 88-power-save.sh; do
  adb push "service.d/$f" "/data/local/tmp/$f"
  adb shell "su -c 'tr -d \"\r\" < /data/local/tmp/$f > /data/adb/service.d/$f && chmod 755 /data/adb/service.d/$f && rm -f /data/local/tmp/$f'"
done
# 専用機として自動起動したい場合のみ:
# adb push service.d/90-hsynth.sh /data/local/tmp/90-hsynth.sh
# adb shell "su -c 'tr -d \"\r\" < /data/local/tmp/90-hsynth.sh > /data/adb/service.d/90-hsynth.sh && chmod 755 /data/adb/service.d/90-hsynth.sh && rm -f /data/local/tmp/90-hsynth.sh'"
```

### Step 2: 端末を再起動
`post-fs-data.d/` の設定（AAudio 1 ms バースト・BLE 7.5 ms・Direct ALSA SELinux）はシステムサービス起動前に読み込まれるため、配置後に **1 回再起動** してください。

```bash
adb reboot
```

### Step 3: （任意）HSynth をホームアプリ（ランチャー）にする
完全な専用音源モジュール化を行う場合のみ、マニフェスト内の `HomeAlias` を有効化するとホーム画面として HSynth を選択できます：

```bash
# 有効化
adb shell "su -c 'pm enable com.hsynth.app/.HomeAlias'"

# 元に戻す
adb shell "su -c 'pm disable com.hsynth.app/.HomeAlias'"
```

---

## 4. 動作確認コマンド

再起動して HSynth を起動した後、以下で全チューニングが効いていることを確認できます：

```bash
# 1. AAudio 1 ms バースト設定確認（1000 が返れば OK）
adb shell getprop aaudio.hw_burst_min_usec

# 2. BLE MIDI 低遅延設定確認（6 が返れば OK）
adb shell getprop bluetooth.core.le.min_connection_interval

# 3. ヘルスログ & チューニング適用ログ確認
adb shell "su -c 'tail -n 30 /data/adb/hsynth-health.log'"

# 4. CPU4-7 の cpuidle state1 無効化状態確認（演奏中・active 時に 1）
adb shell "su -c 'cat /sys/devices/system/cpu/cpu4/cpuidle/state1/disable'"
```

また、アプリ内の **`SYS > STATUS`** 画面で以下が表示されていれば完璧です：
- `stream`: `running AAudio exclusive low-latency`
- `buffer`: `burst 48 size 48 (1.0 ms)`
- `latency`: `3.9–4.8 ms` / `xruns 0`
- `NPU / EDGE TPU`: `google-edgetpu · uint8`（`AI 2D` でアンカー作成後）

---

## 5. トラブルシューティング & 安全なロールバック

### Wi-Fi が「制限付き接続」になる / 音源パックがダウンロードできない
長期間オフラインだった専用端末では本体時刻が大きくずれ、HTTPS 証明書の期限検証エラー（`ERR_CERT_DATE_INVALID`）で Wi-Fi 検証やダウンロードが失敗することがあります。自動時刻同期を有効にしてください：

```bash
adb shell "settings put global auto_time 1"
adb shell "date"
```

### チューニングを一時的にライブ解除する（再起動不要）
```bash
adb shell "su -c 'sh /data/adb/service.d/85-audio-tune.sh undo'"
adb shell "su -c 'sh /data/adb/service.d/88-power-save.sh undo'"
```

### すべての root チューニングを完全に削除して標準状態へ戻す
```bash
adb shell "su -c '
  sh /data/adb/service.d/85-audio-tune.sh undo 2>/dev/null
  sh /data/adb/service.d/88-power-save.sh undo 2>/dev/null
  rm -f /data/adb/post-fs-data.d/85-bt-lowlat.sh \
        /data/adb/post-fs-data.d/86-alsa-direct.sh \
        /data/adb/post-fs-data.d/87-aaudio-1ms.sh \
        /data/adb/service.d/85-audio-tune.sh \
        /data/adb/service.d/88-power-save.sh \
        /data/adb/service.d/90-hsynth.sh \
        /data/adb/85-audio-tune.pid
'"
adb reboot
```

---

## 6. 実測ベンチマーク（Google Pixel 7 / Tensor G2 / Android 16）

| 項目 | 実測値 |
|---|---|
| AAudio MMAP exclusive 構成 | `48000 Hz / float32 / burst 48 / buffer 48 (1.0 ms)` |
| 出力推定遅延 (`latency`) | **3.9〜4.8 ms** |
| オーディオコールバック時間 (`hsynth-audio`) | 平均 **7〜12 µs**（無演奏時）、演奏クライマックス時でも `xruns 0` |
| 音声スレッド起床遅延 (`hsbench` 20,023 回計測) | `p50 = 15 µs`, `p99 = 49 µs`, `>500 µs = 0` |
| Edge TPU 2D パッチモーフィング (`google-edgetpu`, `uint8`) | **591 µs（0.59 ms）** |
| 高速スペクトル 4-Stem 分離（3 分ステレオ曲） | **1.19 秒** |
| 画面 ON 無演奏時 ODPM 消費電力（silent-idle + 省電力） | **737 mW**（`CPU_BIG + CPU_MID = 80 mW`） |