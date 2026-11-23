#!/system/bin/sh
# HSynth dedicated sound module: tuning + watchdog + health log (root, Magisk service.d)
PKG=com.hsynth.app
LOG=/data/adb/hsynth-health.log
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
sleep 15

# (removed 2026-11-20: 'echo 1 > /dev/cpuctl/top-app/cpu.uclamp.latency_sensitive' was a no-op --
#  on this Pixel kernel apps are not in /dev/cpuctl/top-app (scheduling uses /proc/vendor_sched groups).
#  Real tuning lives in 85-audio-tune.sh; see docs/ANDROID_ROOT_SETUP.md)

input keyevent KEYCODE_WAKEUP
am start -n $PKG/.MainActivity >/dev/null 2>&1
echo "$(date '+%F %T') boot: started $PKG" >> $LOG

zt() { for z in /sys/class/thermal/thermal_zone*; do [ "$(cat $z/type)" = "$1" ] && echo $(($(cat $z/temp)/1000)); done; }
n=0
while true; do
  sleep 30
  if [ -z "$(pidof $PKG)" ]; then
    echo "$(date '+%F %T') watchdog: $PKG not running, restarting" >> $LOG
    am start -n $PKG/.MainActivity >/dev/null 2>&1
  fi
  n=$((n+1))
  if [ $((n % 10)) = 0 ]; then   # every 5 min
    B=/sys/class/power_supply/battery
    echo "$(date '+%F %T') batt=$(cat $B/capacity)% $(cat $B/status) batt_t=$(($(cat $B/temp)/10))C case=$(zt neutral_therm)C cpu_big=$(zt BIG)C" >> $LOG
    [ $(wc -l < $LOG) -gt 5000 ] && tail -n 2500 $LOG > $LOG.tmp && mv $LOG.tmp $LOG
  fi
done
