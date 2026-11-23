#!/system/bin/sh
# HSynth dedicated sound module: AAudio 1 ms (48-frame) hardware burst tuning (Magisk post-fs-data.d).
# Runs before audioserver starts so AAudio MMAP opens directly at 48 frames (1000 us) instead of 96 frames (2000 us).
# Documented in docs/ANDROID_ROOT_SETUP.md
# Undo: rm /data/adb/post-fs-data.d/87-aaudio-1ms.sh + reboot (live: sh 87-aaudio-1ms.sh undo + killall audioserver)

if [ "$1" = "undo" ]; then
  resetprop aaudio.hw_burst_min_usec 2000
  exit 0
fi

resetprop aaudio.hw_burst_min_usec 1000
