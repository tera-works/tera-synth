package com.hsynth.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper

/**
 * Keeps song playback (Player tab) running while the app is in the background:
 * a media-playback foreground service with a notification and a media session
 * (lock screen, headset buttons). It only holds the process; the audio stream,
 * the engine and the playlist stay in MainActivity / PlayerPanel.
 *
 * MainActivity starts it when a song starts playing and stops it when nothing
 * plays any more (see MainActivity.updateBackgroundPlayback).
 */
class PlaybackService : Service() {
    companion object {
        @Volatile var running = false
            private set
        @Volatile var standbyMode = false
        private const val CHANNEL = "playback"
        private const val ID = 1
        private const val ACTION = "com.hsynth.app.PLAYBACK"

        fun start(ctx: Context) {
            standbyMode = false
            val i = Intent(ctx, PlaybackService::class.java)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
        }

        fun startStandby(ctx: Context) {
            standbyMode = true
            val i = Intent(ctx, PlaybackService::class.java).putExtra("standby", true)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
        }

        fun stop(ctx: Context) {
            standbyMode = false
            ctx.stopService(Intent(ctx, PlaybackService::class.java))
        }
    }

    private val ui = Handler(Looper.getMainLooper())
    private lateinit var session: MediaSession
    private var shownTitle: String? = null
    private var shownPlaying: Boolean? = null

    private val player: PlayerPanel? get() = MainActivity.instance?.let { if (it.isPlayerReady) it.player else null }

    private val noisy = object : BroadcastReceiver() {
        // headphones unplugged / Bluetooth gone: pause instead of playing out of the speaker
        override fun onReceive(c: Context, i: Intent) { player?.let { if (it.isPlaying) it.togglePlay() } }
    }

    private val tick = object : Runnable {
        override fun run() {
            refresh()
            ui.postDelayed(this, 500)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        running = true
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26)
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Playback", NotificationManager.IMPORTANCE_LOW).apply {
                description = "曲の再生中に表示します"
                setShowBadge(false)
            })
        session = MediaSession(this, "HSynth").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() { command("toggle", wantPlaying = true) }
                override fun onPause() { command("toggle", wantPlaying = false) }
                override fun onSkipToNext() { command("next") }
                override fun onSkipToPrevious() { command("prev") }
                override fun onStop() { command("close") }
            })
            isActive = true
        }
        registerReceiver(noisy, IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY))
        val n = if (standbyMode) buildStandby() else build("HSynth", true)
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        else startForeground(ID, n)
        ui.post(tick)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        intent?.getStringExtra("cmd")?.let { command(it) }
        return START_NOT_STICKY  // the engine lives in the activity's process state; never restart empty
    }

    override fun onDestroy() {
        ui.removeCallbacks(tick)
        runCatching { unregisterReceiver(noisy) }
        session.isActive = false
        session.release()
        running = false
        super.onDestroy()
    }

    private fun command(cmd: String, wantPlaying: Boolean? = null) {
        val p = player
        if (p == null) { stopSelf(); return }
        when (cmd) {
            "toggle" -> if (wantPlaying == null || wantPlaying != p.isPlaying) p.togglePlay()
            "next" -> p.next(manual = true)
            "prev" -> p.prev()
            "close" -> {
                p.stop()
                MainActivity.instance?.stopBackgroundPlayback()
                return
            }
        }
        ui.postDelayed({ refresh() }, 100)
    }

    private fun refresh() {
        val p = player
        val playing = p?.isPlaying == true
        if (!playing && standbyMode) {
            if (shownTitle == "__STANDBY__" && shownPlaying == false) return
            shownTitle = "__STANDBY__"
            shownPlaying = false
            getSystemService(NotificationManager::class.java).notify(ID, buildStandby())
            return
        }
        if (p == null) return
        val title = p.title ?: "HSynth"
        if (title == shownTitle && playing == shownPlaying) return
        shownTitle = title
        shownPlaying = playing
        session.setMetadata(MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, "HSynth")
            .putLong(MediaMetadata.METADATA_KEY_DURATION, (p.lengthSec() * 1000).toLong())
            .build())
        session.setPlaybackState(PlaybackState.Builder()
            .setActions(PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE or
                        PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS or PlaybackState.ACTION_STOP)
            .setState(if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED,
                      (p.positionSec() * 1000).toLong(), if (playing) 1f else 0f)
            .build())
        getSystemService(NotificationManager::class.java).notify(ID, build(title, playing))
    }

    private fun action(cmd: String, code: Int): PendingIntent =
        PendingIntent.getService(this, code, Intent(this, PlaybackService::class.java).setAction("$ACTION.$cmd").putExtra("cmd", cmd),
                                 PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)

    private fun act(icon: Int, label: String, cmd: String, code: Int) =
        Notification.Action.Builder(Icon.createWithResource(this, icon), label, action(cmd, code)).build()

    @Suppress("DEPRECATION")
    private fun buildStandby(): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP), PendingIntent.FLAG_IMMUTABLE)
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL) else Notification.Builder(this)
        return b.setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("HSynth Standby")
            .setContentText("BLE MIDI wake ready")
            .setContentIntent(open)
            .setOngoing(true)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .build()
    }

    @Suppress("DEPRECATION")
    private fun build(title: String, playing: Boolean): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP), PendingIntent.FLAG_IMMUTABLE)
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL) else Notification.Builder(this)
        return b.setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(title)
            .setContentText(if (playing) "Playing" else "Paused")
            .setContentIntent(open)
            .setOngoing(playing)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .addAction(act(android.R.drawable.ic_media_previous, "Prev", "prev", 1))
            .addAction(act(if (playing) android.R.drawable.ic_media_pause else android.R.drawable.ic_media_play, if (playing) "Pause" else "Play", "toggle", 2))
            .addAction(act(android.R.drawable.ic_media_next, "Next", "next", 3))
            .addAction(act(android.R.drawable.ic_menu_close_clear_cancel, "Close", "close", 4))
            .setStyle(Notification.MediaStyle().setMediaSession(session.sessionToken).setShowActionsInCompactView(0, 1, 2))
            .build()
    }
}
