#!/system/bin/sh
# HSynth dedicated sound module: zero-latency-impact system power settings (Magisk service.d).
# Locks OLED refresh rate to 60 Hz, sets screen_off_timeout to 30 s (HSynth holds
# FLAG_KEEP_SCREEN_ON while playing / within user's Screen-off timeout), disables AOD
# and background Wi-Fi/BLE scanning (Bluetooth LE MIDI stays enabled).
# Usage:
#   sh 88-power-save.sh          boot mode (waits for boot_completed)
#   sh 88-power-save.sh once     apply immediately
#   sh 88-power-save.sh undo     restore stock defaults (90 Hz, 2h timeout, scans)
LOG=/data/adb/hsynth-health.log
log() { echo "$(date '+%F %T') power: $*" >> $LOG; }

if [ "$1" = "undo" ]; then
  settings delete system peak_refresh_rate 2>/dev/null
  settings delete system min_refresh_rate 2>/dev/null
  settings put system screen_off_timeout 7200000 2>/dev/null
  settings put secure doze_always_on 1 2>/dev/null
  settings put global wifi_scan_always_enabled 1 2>/dev/null
  settings put global ble_scan_always_enabled 1 2>/dev/null
  log "undo done (restored 90 Hz / default timeouts)"
  exit 0
fi

if [ "$1" != "once" ]; then
  until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
  sleep 25
fi

# Adaptive 60<->90 Hz (90 Hz on touch/drag, drops to 60 Hz when idle)
settings put system peak_refresh_rate 90.0 2>/dev/null
settings delete system min_refresh_rate 2>/dev/null
# 30 s system timeout so OLED blanks shortly after HSynth clears FLAG_KEEP_SCREEN_ON
settings put system screen_off_timeout 30000 2>/dev/null
# Disable Always-On Display and pick-up wake, but keep tap-to-wake enabled
# so tapping the blank OLED wakes the screen cleanly.
settings put secure doze_always_on 0 2>/dev/null
settings put secure doze_enabled 1 2>/dev/null
settings put secure doze_tap_gesture 1 2>/dev/null
settings put secure double_tap_to_wake 1 2>/dev/null
settings put secure doze_pulse_on_pick_up 0 2>/dev/null
settings put secure wake_gesture_enabled 0 2>/dev/null
settings put global wifi_scan_always_enabled 0 2>/dev/null
settings put global ble_scan_always_enabled 0 2>/dev/null
settings put global stay_on_while_plugged_in 0 2>/dev/null

log "applied (60-90 Hz adaptive, 30s screen_off_timeout, tap-to-wake on, AOD/bg-scans off)"
exit 0
