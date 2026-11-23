#!/system/bin/sh
# HSynth dedicated sound module: system-level tuning (Magisk service.d, root).
# Documented in docs/ANDROID_ROOT_SETUP.md
# Idempotent: safe to run again at any time.
#   sh 85-audio-tune.sh          boot mode: wait for boot, apply, then follow the app (loop)
#   sh 85-audio-tune.sh once     apply static + current app settings once, then exit
#   sh 85-audio-tune.sh undo     revert everything this script changes (live, no reboot)
PKG=com.hsynth.app
LOG=/data/adb/hsynth-health.log
MEM_LOW=3221225472            # 3 GiB best-effort reclaim protection for the app (memory.low)
IDLE_CPUS="4 5 6 7"           # MID + BIG: no deep idle (exit latency 340/455 us)
CG=/sys/fs/cgroup
Q=/proc/vendor_sched/sched_qos
CHRT=/system/bin/chrt          # Toybox chrt: -f -p <pid> <prio> (avoid Magisk busybox arg order!)
PSTATE=/data/user/0/$PKG/files/power_state.json

log() { echo "$(date '+%F %T') tune: $*" >> $LOG; }
w() { [ -e "$2" ] && echo "$1" > "$2" 2>/dev/null; }

cpuidle_set() {   # $1 = 1 (disable deep idle for low latency) or 0 (allow deep sleep)
  for c in $IDLE_CPUS; do w "$1" /sys/devices/system/cpu/cpu$c/cpuidle/state1/disable; done
}

usb_autosuspend_set() {   # $1 = off (keep USB DAC awake) or auto (allow deep sleep)
  if [ "$1" = "off" ]; then
    w -1 /sys/module/usbcore/parameters/autosuspend
    for d in /sys/bus/usb/devices/*/power; do
      [ -e "$d/control" ] && w on "$d/control"
      [ -e "$d/autosuspend_delay_ms" ] && w -1 "$d/autosuspend_delay_ms"
    done
  else
    w 2 /sys/module/usbcore/parameters/autosuspend
    for d in /sys/bus/usb/devices/*/power; do
      [ -e "$d/control" ] && w auto "$d/control"
    done
  fi
}

screen_is_off() {
  for b in /sys/class/backlight/*/brightness; do
    if [ -r "$b" ]; then
      [ "$(cat "$b" 2>/dev/null)" = "0" ] && return 0 || return 1
    fi
  done
  dumpsys power 2>/dev/null | grep -q 'mWakefulness=Asleep'
}

json_str() { sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$2" 2>/dev/null | head -n 1; }
json_val() { sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\([0-9a-z]*\).*/\1/p" "$2" 2>/dev/null | head -n 1; }

# Dynamic power management driven by HSynth's power_state.json:
#   - active / screen_off : keep cpuidle state1 disabled & USB DAC awake
#   - standby             : re-enable cpuidle state1 (MID/BIG deep sleep) while BLE stays ready
#   - deep_sleep          : also allow USB autosuspend
#   - idle >= shutdownMin : safe `svc power shutdown` when screen is off (& on battery if configured)
last_pstate=""
power_check() {
  [ -r "$PSTATE" ] || return 0
  st=$(json_str state "$PSTATE")
  [ -z "$st" ] && st="active"
  if [ "$st" != "$last_pstate" ]; then
    case "$st" in
      standby)
        cpuidle_set 0
        log "power: state=standby -> cpuidle state1 enabled (MID/BIG sleep)"
        ;;
      deep_sleep)
        cpuidle_set 0
        usb_autosuspend_set auto
        log "power: state=deep_sleep -> cpuidle enabled, USB autosuspend restored"
        ;;
      *)
        cpuidle_set 1
        usb_autosuspend_set off
        log "power: state=$st -> cpuidle state1 disabled, USB kept awake"
        ;;
    esac
    last_pstate="$st"
  fi

  smin=$(json_val shutdownMin "$PSTATE")
  [ -z "$smin" ] || [ "$smin" = "0" ] && return 0
  last_act=$(json_val lastActivityEpochSec "$PSTATE")
  [ -z "$last_act" ] && return 0
  # Guard: never shut down within first 5 minutes after boot
  up=$(awk '{print int($1)}' /proc/uptime 2>/dev/null)
  [ -z "$up" ] || [ "$up" -lt 300 ] && return 0
  now=$(date +%s)
  idle=$((now - last_act))
  limit=$((smin * 60))
  [ "$idle" -lt "$limit" ] && return 0
  screen_is_off || return 0
  bonly=$(json_val shutdownBatteryOnly "$PSTATE")
  if [ "$bonly" != "false" ]; then
    bstat=$(cat /sys/class/power_supply/battery/status 2>/dev/null)
    [ "$bstat" = "Discharging" ] || return 0
  fi
  log "power: idle $((idle / 60))m >= ${smin}m (screen off, batt=${bstat:-any}) -> shutting down"
  svc power shutdown 2>/dev/null || reboot -p
  sleep 30
}

static_apply() {
  if [ "$last_pstate" = "standby" ] || [ "$last_pstate" = "deep_sleep" ]; then
    cpuidle_set 0
  else
    cpuidle_set 1
    w -1 /sys/module/usbcore/parameters/autosuspend
  fi
  alsa_perms
}
static_undo() {
  cpuidle_set 0
  usb_autosuspend_set auto
  alsa_undo
}

alsa_perms() {
  for dev in /dev/snd/pcmC[1-9]*D*p; do
    if [ -e "$dev" ]; then
      chmod 0666 "$dev" 2>/dev/null
      # Keep attached USB host devices awake unless in deep_sleep
      if [ "$last_pstate" != "deep_sleep" ]; then
        usb_autosuspend_set off
      fi
    fi
  done
}
alsa_undo() {
  for dev in /dev/snd/pcmC[1-9]*D*p; do
    if [ -e "$dev" ]; then chmod 0660 "$dev" 2>/dev/null; fi
  done
}

# memory: keep the app's sample memory out of zram and protect it from reclaim
app_mem() {   # $1 = pid
  rel=$(awk -F: '/^0::/{print $3}' /proc/$1/cgroup 2>/dev/null)   # /apps/uid_X/pid_Y
  [ -z "$rel" ] && return 1
  uidd=$CG${rel%/pid_*}; pidd=$CG$rel
  [ -d "$pidd" ] || return 1
  w $MEM_LOW $CG/apps/memory.low      # parent must carry protection (memory_recursiveprot)
  for d in $uidd $pidd; do w 0 $d/memory.swap.max; w $MEM_LOW $d/memory.low; done
  return 0
}

# threads:
#   - hsynth-w0..w6 (engine render workers): SCHED_FIFO prio 1 (strictly below hsynth-audio prio 2)
#   - hsynth-audio: SCHED_FIFO prio 2 if not already FIFO (e.g. Direct ALSA started unprivileged)
#   - hsynth-midi / hsynth-ble: CFS vendor sched_qos "sensitive_extreme" (Binder-safe)
app_threads() {   # $1 = pid ; prints "fifo_w/fifo_a/qos_ble"
  fw=0; fa=0; mq=0
  for t in /proc/$1/task/*; do
    c=$(cat $t/comm 2>/dev/null) || continue
    tid=${t##*/}
    pol=$(awk '{print $41}' $t/stat 2>/dev/null)   # 0 = SCHED_OTHER, 1 = SCHED_FIFO
    case "$c" in
      hsynth-w[0-9]*)
        if [ "$pol" != 1 ]; then $CHRT -f -p $tid 1 >/dev/null 2>&1 && pol=1; fi
        [ "$pol" = 1 ] && fw=$((fw+1)) ;;
      hsynth-audio)
        if [ "$pol" != 1 ]; then $CHRT -f -p $tid 2 >/dev/null 2>&1 && pol=1; fi
        [ "$pol" = 1 ] && fa=$((fa+1)) ;;
      hsynth-midi|hsynth-ble)
        if [ "$pol" = 0 ]; then w $tid $Q/sched_qos_sensitive_extreme && mq=$((mq+1)); fi ;;
    esac
  done
  echo "$fw/$fa/$mq"
}

app_undo() {
  w 0 $CG/apps/memory.low
  for p in $(pidof $PKG); do
    rel=$(awk -F: '/^0::/{print $3}' /proc/$p/cgroup 2>/dev/null)
    if [ -n "$rel" ]; then
      for x in $CG${rel%/pid_*} $CG$rel; do w max $x/memory.swap.max; w 0 $x/memory.low; done
    fi
    for t in /proc/$p/task/*; do
      tid=${t##*/}
      case "$(cat $t/comm 2>/dev/null)" in
        hsynth-w[0-9]*) $CHRT -o -p $tid 0 >/dev/null 2>&1; w $tid $Q/sched_qos_none ;;
        hsynth-midi|hsynth-ble) w $tid $Q/sched_qos_none ;;
      esac
    done
  done
}

case "$1" in
  undo) F=/data/adb/85-audio-tune.pid
        [ -f $F ] && grep -q 85-audio-tune /proc/$(cat $F)/cmdline 2>/dev/null && kill $(cat $F)
        static_undo; app_undo; log "undo done"; exit 0 ;;
  once) static_apply; P=$(pidof $PKG) && { app_mem $P; app_threads $P >/dev/null; }; log "applied once (pid ${P:-none})"; exit 0 ;;
esac

# single instance of the loop
PIDF=/data/adb/85-audio-tune.pid
if [ -f $PIDF ] && grep -q 85-audio-tune /proc/$(cat $PIDF)/cmdline 2>/dev/null; then exit 0; fi
echo $$ > $PIDF

until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
sleep 20
static_apply
log "static applied (cpuidle state1 off on cpu $IDLE_CPUS)"

last=""; lastn=""; i=0
while true; do
  power_check
  alsa_perms
  P=$(pidof $PKG)
  if [ -n "$P" ]; then
    if [ "$P" != "$last" ]; then
      if app_mem $P; then last=$P; lastn=""; log "pid $P: swap.max=0 memory.low=$MEM_LOW"; fi
    fi
    n=$(app_threads $P)                      # cheap; re-applies to new/renamed threads
    [ "$n" != "$lastn" ] && { log "pid $P: threads fifo_w/fifo_a/qos_ble=$n"; lastn=$n; }
  fi
  i=$((i+1)); [ $((i % 60)) = 0 ] && static_apply   # every ~10 min, in case something reset it
  # Check USB DAC device nodes, power state, and newly spawned RT threads every 2 s
  for _t in 1 2 3 4 5; do
    sleep 2
    power_check
    alsa_perms
    if [ -n "$P" ] && [ -d "/proc/$P" ]; then
      n=$(app_threads $P)
      [ "$n" != "$lastn" ] && { log "pid $P: threads fifo_w/fifo_a/qos_ble=$n"; lastn=$n; }
    fi
  done
done
