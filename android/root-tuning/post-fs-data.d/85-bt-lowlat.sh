#!/system/bin/sh
# HSynth dedicated sound module: BLE MIDI 7.5 ms (interval=6) tuning (Magisk post-fs-data.d).
# Runs before system_server / com.android.bt starts so Bluetooth boots directly with 7.5 ms LE intervals.
# Documented in docs/ANDROID_ROOT_SETUP.md
# Undo: rm /data/adb/post-fs-data.d/85-bt-lowlat.sh (or run `sh 85-bt-lowlat.sh undo` + BT toggle)

PROPS="
bluetooth.core.gap.le.conn.min.limit
bluetooth.core.le.min_connection_interval
bluetooth.core.le.max_connection_interval
bluetooth.core.le.min_connection_interval_aggressive
bluetooth.core.le.max_connection_interval_aggressive
bluetooth.core.le.min_connection_interval_relaxed
bluetooth.core.le.max_connection_interval_relaxed
bluetooth.gatt.high_priority.min_interval
bluetooth.gatt.high_priority.max_interval
"

if [ "$1" = "undo" ]; then
  for k in $PROPS; do resetprop --delete "$k" 2>/dev/null; done
  exit 0
fi

for k in $PROPS; do
  resetprop "$k" 6
done
