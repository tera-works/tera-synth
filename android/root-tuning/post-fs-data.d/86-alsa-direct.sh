#!/system/bin/sh
# HSynth dedicated sound module: Direct ALSA USB-DAC SELinux policy (Magisk post-fs-data.d).
# Allows untrusted_app (com.hsynth.app) to discover /proc/asound/* and open/ioctl
# external USB DAC nodes under /dev/snd/pcmC[1-9]*D*p (chmod 0666 handled in 85-audio-tune.sh).
# Documented in docs/ANDROID_ROOT_SETUP.md
# Undo: rm /data/adb/post-fs-data.d/86-alsa-direct.sh + reboot (live: sh 85-audio-tune.sh undo)

if [ "$1" = "undo" ]; then
  echo 2 > /sys/module/usbcore/parameters/autosuspend 2>/dev/null
  for dev in /dev/snd/pcmC[1-9]*D*p; do
    [ -e "$dev" ] && chmod 0660 "$dev" 2>/dev/null
  done
  exit 0
fi

# Prevent xhci-exynos / usbcore from autosuspending USB DACs after 2 s during Direct ALSA playback
echo -1 > /sys/module/usbcore/parameters/autosuspend 2>/dev/null

magiskpolicy --live \
  "typeattribute audio_device mlstrustedobject" \
  "allow untrusted_app proc_asound dir { open read getattr search }" \
  "allow untrusted_app proc_asound file { open read getattr }" \
  "allow untrusted_app audio_device dir { open read getattr search }" \
  "allow untrusted_app audio_device chr_file { open read write getattr ioctl map }" \
  "allowxperm untrusted_app audio_device chr_file ioctl 0x4100-0x41ff"

for dev in /dev/snd/pcmC[1-9]*D*p; do
  if [ -e "$dev" ]; then chmod 0666 "$dev" 2>/dev/null; fi
done
exit 0
