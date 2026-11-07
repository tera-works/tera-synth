# Bluetooth MIDI（BLE MIDI）

HSynth はケーブルなしで Bluetooth LE MIDI 機器とつながります。つながった機器は USB MIDI と同じ扱いで、演奏（ノート・CC・ベンド・MPE）も [SysEx リモート制御](SYSEX.md) もそのまま使えます。

接続のしかたは 2 通りあります。

| | セントラル（HSynth から機器へ接続） | ペリフェラル（相手から HSynth へ接続） |
|---|---|---|
| 用途 | BLE MIDI のキーボード・コントローラーを使う | PC・Mac・タブレットから HSynth を弾く／リモート操作する |
| Android | Monitor 画面 → MIDI input →「Scan」→ 見つかった機器の「Connect …」 | 「Peripheral: a PC / tablet can connect to this phone」をオン。端末名で公開されます |
| iPhone / iPad | System 画面 →「Bluetooth MIDI」（iOS 標準の接続画面） | System 画面 →「Advertise」（画面を開いている間、公開します） |
| macOS デスクトップ版 | 「Audio MIDI 設定」→ MIDI スタジオ → Bluetooth で接続 | — |
| Windows デスクトップ版 | 非対応（Windows の旧 MIDI API（winmm）は BLE MIDI を扱えません） | — |

- Android で接続した機器は覚えておき、次回の起動時に自動で再接続します。
- iOS / macOS は CoreMIDI の機器構成が変わると自動で入力を開き直すので、接続後に「Refresh」を押す必要はありません。
- 初回は Bluetooth の使用許可を求められます（Android 11 以前は位置情報の許可が必要です。Android の仕様によるもので、位置情報は使いません）。

## PC から Bluetooth で操作する

`hsynth_remote.py` は Bluetooth に直接つなげます（MIDI ドライバー不要。Windows でも動きます）。`bleak` が必要です。

```bash
pip install bleak
```
```bash
python desktop/tools/hsynth_remote.py ble-scan
```
```bash
python desktop/tools/hsynth_remote.py --ble Galaxy status
```

`--ble` には、`ble-scan` に出てきた名前の一部かアドレスを指定します。相手は「ペリフェラル」をオンにした Android、または「Advertise」画面を開いている iPhone です。そのほかのコマンドは USB MIDI と同じです（[SYSEX.md](SYSEX.md) の 4 章）。

## 仕組み（開発者向け）

- BLE MIDI のパケット形式（ヘッダー・タイムスタンプ・ランニングステータス・パケットをまたぐ SysEx）は、エンジンの `engine/include/hsynth/ble_midi.h`（`ble::Decoder`, `ble::encode`）で変換します。テストは `engine/tests/test_ble.cpp` にあります。
- Android のセントラルは `MidiManager.openBluetoothDevice` を使い、以後は普通の MIDI 機器です。ペリフェラルは標準の MIDI サービス（`03B80E5A-…`、特性 `7772E5DB-…`、Read / Write without response / Notify）を持つ GATT サーバーです。受け取ったパケットはネイティブでデコードし、USB と同じ経路で処理します。返信は MTU に合わせて分割し、1 つずつ通知します（`BleMidi.kt`）。
- iOS は CoreAudioKit の `CABTMIDICentralViewController` / `CABTMIDILocalPeripheralViewController`（`ios/HSynth/BluetoothMidi.swift`）を使います。接続後の通信は CoreMIDI が行います。
- PC ツールの BLE 部分（`Ble` クラス）は、同じパケット形式を Python で実装しています。

## 制限

- BLE MIDI の遅延は通常 7.5〜15 ms 程度（接続間隔による）で、USB より大きくなります。
- 転送速度は数 KB/s 程度です。`get_state` のような大きな SysEx 応答には数秒かかることがあります。
- 実機（Galaxy S23・iPhone・BLE MIDI 機器）での接続確認はまだです。iOS 部分は Mac でのビルド確認もまだです。
