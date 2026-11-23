package com.hsynth.app

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.PowerManager
import android.os.Process
import android.os.SystemClock
import org.json.JSONException
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.MessageDigest
import java.security.SecureRandom
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadFactory
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/*
 * B6 — lightweight Wi-Fi Web Editor (Phase 4 Wave 1B): plain `java.net.ServerSocket`, zero external deps, HTTP (not
 * encrypted) on the LAN only, opt-in, PIN protected. Standalone: the engine bridge and the set list are injected as lambdas,
 * so nothing here depends on NativeEngine / LivePanel. Wave 2 wiring:
 *
 *   WebEditorServer(a,
 *       remoteHandle = { NativeEngine.nativeRemoteHandle(it) },        // synchronous RemoteControl::handle (mutex inside)
 *       remoteChanges = { NativeEngine.nativeRemoteChanges() },
 *       setlistJsonGet = { <SetlistStore as JSON string> },            // same shape as files/setlist.json
 *       setlistJsonSet = { <parse + apply + save, true when accepted> })
 *   + manifest: INTERNET, ACCESS_NETWORK_STATE, ACCESS_WIFI_STATE; MainActivity.onStop / PowerState != ACTIVE -> stop().
 *
 * [setlistJsonGet] / [setlistJsonSet] are always invoked on the UI thread (the store is UI-thread only);
 * [remoteHandle] / [remoteChanges] are invoked on the server's worker threads (the engine side is thread-safe).
 *
 * Threads: 1 acceptor `hsynth-web-accept` (daemon, BACKGROUND, also the 1 s housekeeping tick) + 4 workers `hsynth-web-N`.
 * `GET /events` keeps one worker per open stream (at most [MAX_SSE]); the page falls back to polling when refused.
 */
class WebEditorServer(
    private val a: MainActivity,
    private val remoteHandle: (String) -> String,
    private val remoteChanges: () -> Int,
    private val setlistJsonGet: () -> String,
    private val setlistJsonSet: (String) -> Boolean,
) {
    companion object {
        const val PORT_FIRST = 8080
        const val PORT_LAST = 8099
        private const val BACKLOG = 8
        private const val WORKERS = 4
        private const val SO_TIMEOUT_MS = 5000
        private const val MAX_REQUEST_LINE = 2048
        private const val MAX_HEADER_BYTES = 8192
        private const val MAX_HEADERS = 64
        private const val MAX_BODY = 2 * 1024 * 1024
        private const val REQUEST_DEADLINE_MS = 15_000L
        private const val SESSION_MS = 8L * 60 * 60 * 1000
        private const val MAX_SESSIONS = 32
        private const val IDLE_MS = 30L * 60 * 1000
        private const val LOCK_MS = 60_000L
        private const val IP_FAILS = 5
        private const val GLOBAL_FAILS = 20
        private const val MAX_SSE = 2
        private const val SSE_POLL_MS = 250L
        private const val HEARTBEAT_MS = 15_000L
        private const val ADDR_CHECK_MS = 10_000L
        private const val COOKIE = "hs"
        private const val PAGE_ASSET = "web/editor.html"

        /**
         * RemoteLink ops reachable from the web. Denied on purpose: `setup_set` (writes the MIDI map + host settings),
         * `subscribe` / `unsubscribe` (SSE replaces them), `tuner` / `pedal_config` (device configuration), any `host.*`
         * op and everything not listed (restore, file upload, ...). `app.setlist_get` / `app.setlist_set` are handled here.
         */
        private val OPS = setOf(
            "hello", "param_info", "mix_info", "status", "meters", "get", "set", "get_state", "set_state", "setup_get",
            "mode", "select_part", "mix_get", "mix_set", "presets", "load_preset", "save_preset", "samples", "impulse",
            "files", "note", "event", "panic", "transport", "songs", "song_load", "song", "phrase", "looper", "settings",
            "learn", "master_stage_get", "master_stage_set", "sst", "tuning", "metronome", "perf_offsets", "morph",
            "arp_pattern", "rhythm",
        )
        private val HOST_RE = Regex("^(\\d{1,3}(\\.\\d{1,3}){3}|localhost)(:\\d{1,5})?$")

        private const val CSP = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; " +
            "connect-src 'self'; img-src data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
    }

    // ============================================================================================ observable state

    /** True between a successful [start] and [stop] (or an automatic stop). */
    @Volatile var running = false
        private set

    /** `http://192.168.1.23:8080` while running, else "". */
    @Volatile var url = ""
        private set

    /** The 6-digit PIN while running, else "". */
    @Volatile var pin = ""
        private set

    /** Why [start] failed (`"NO Wi-Fi NETWORK"`, ...) — null after a successful start. */
    @Volatile var error: String? = null
        private set

    /** Why the server last stopped by itself ("idle 30 min", "screen off", "Wi-Fi lost", ...); null after a manual stop. */
    @Volatile var stopReason: String? = null
        private set

    /** Browsers that were active in the last 30 s or hold an event stream. */
    val clientCount: Int
        get() {
            val now = SystemClock.elapsedRealtime()
            return sessions.values.count { it.sse.get() > 0 || now - it.seen < 30_000L }
        }

    /** Called on the UI thread whenever running / url / pin / clientCount / error change. */
    @Volatile var onStateChanged: (() -> Unit)? = null

    // ============================================================================================ internals

    private class Session(val token: String, val ip: String, val expires: Long) {
        @Volatile var seen = SystemClock.elapsedRealtime()
        val sse = AtomicInteger()
    }

    private class Fail { var count = 0; var lockUntil = 0L }
    private class HttpError(val status: Int, msg: String) : Exception(msg)
    private class Request(val method: String, val path: String, val headers: Map<String, String>, val body: ByteArray)
    private class Lan(val addr: Inet4Address, val network: Network?)

    private val rnd = SecureRandom()
    private val sessions = ConcurrentHashMap<String, Session>()
    private val fails = HashMap<String, Fail>()
    private val globalFails = AtomicInteger()
    private val conns: MutableSet<Socket> = ConcurrentHashMap.newKeySet()
    private val sseOpen = AtomicInteger()
    private val backupBusy = AtomicBoolean(false)

    @Volatile private var pinHash = ByteArray(32)
    @Volatile private var server: ServerSocket? = null
    @Volatile private var pool: ThreadPoolExecutor? = null
    @Volatile private var bindAddr: InetAddress? = null
    @Volatile private var netCallback: ConnectivityManager.NetworkCallback? = null
    @Volatile private var lastActivity = 0L
    private var lastAddrCheck = 0L                         // acceptor thread only
    @Volatile private var page: ByteArray? = null

    // ============================================================================================ lifecycle

    /** Start listening. False when there is no Wi-Fi / LAN or no free port in 8080..8099 ([error] says which). */
    @Synchronized fun start(): Boolean {
        if (running) return true
        error = null
        stopReason = null
        val lan = try { findLan() } catch (e: SecurityException) { error = "network permission missing"; changed(); return false }
        if (lan == null) { error = "NO Wi-Fi NETWORK"; changed(); return false }

        var ss: ServerSocket? = null
        var port = 0
        for (p in PORT_FIRST..PORT_LAST) {
            val s = ServerSocket()
            try {
                s.reuseAddress = true
                s.bind(InetSocketAddress(lan.addr, p), BACKLOG)
                s.soTimeout = 1000                      // accept() wakes every second for the housekeeping tick
                ss = s
                port = p
                break
            } catch (_: IOException) {
                try { s.close() } catch (_: IOException) {}
            }
        }
        if (ss == null) { error = "no free port ${PORT_FIRST}-${PORT_LAST}"; changed(); return false }

        sessions.clear()
        synchronized(fails) { fails.clear() }
        globalFails.set(0)
        newPin()
        lastActivity = SystemClock.elapsedRealtime()
        lastAddrCheck = lastActivity
        bindAddr = lan.addr
        server = ss
        url = "http://${lan.addr.hostAddress}:$port"
        pool = ThreadPoolExecutor(WORKERS, WORKERS, 0L, TimeUnit.MILLISECONDS, ArrayBlockingQueue(16), object : ThreadFactory {
            private val n = AtomicInteger()
            override fun newThread(r: Runnable) = Thread({
                Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND)
                r.run()
            }, "hsynth-web-${n.incrementAndGet()}").apply { isDaemon = true }
        })
        running = true
        watchNetwork(lan)
        val srv = ss
        Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND)
            acceptLoop(srv)
        }, "hsynth-web-accept").apply { isDaemon = true; start() }
        changed()
        return true
    }

    /** Stop now (user toggle, onStop, ...). Safe to call when not running. */
    fun stop() = stopInternal(null)

    /** A fresh PIN; every browser has to log in again. No-op while stopped. */
    fun regeneratePin() {
        synchronized(this) {
            if (!running) return
            newPin()
            sessions.clear()
            synchronized(fails) { fails.clear() }
            globalFails.set(0)
        }
        changed()
    }

    private fun newPin() {
        pin = String.format(Locale.US, "%06d", rnd.nextInt(1_000_000))
        pinHash = sha256(pin.toByteArray(Charsets.UTF_8))
    }

    private fun stopInternal(reason: String?) {
        synchronized(this) {
            if (!running) return
            running = false
            stopReason = reason
            try { server?.close() } catch (_: IOException) {}
            server = null
            pool?.shutdownNow()
            pool = null
            for (s in conns.toList()) closeQuiet(s)
            conns.clear()
            netCallback?.let {
                try { (a.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager).unregisterNetworkCallback(it) } catch (_: Exception) {}
            }
            netCallback = null
            sessions.clear()
            pin = ""
            url = ""
            bindAddr = null
        }
        changed()
    }

    private fun changed() {
        a.ui.post { onStateChanged?.invoke() }
    }

    // ============================================================================================ network

    @Suppress("DEPRECATION")
    private fun findLan(): Lan? {
        val cm = a.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        var best: Lan? = null
        var bestRank = Int.MAX_VALUE
        val active = try { cm.activeNetwork } catch (_: Exception) { null }
        for (n in cm.allNetworks) {
            val caps = cm.getNetworkCapabilities(n) ?: continue
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) continue
            var rank = when {
                caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> 0
                caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> 2
                else -> continue
            }
            if (n != active) rank += 1
            val lp = cm.getLinkProperties(n) ?: continue
            val addr = lp.linkAddresses.map { it.address }.filterIsInstance<Inet4Address>().firstOrNull { it.isSiteLocalAddress } ?: continue
            if (rank < bestRank) { best = Lan(addr, n); bestRank = rank }
        }
        if (best != null) return best
        // phone is the hotspot / tethering: there is no Wi-Fi *network*, only an access-point interface
        try {
            for (i in NetworkInterface.getNetworkInterfaces()) {
                val nm = i.name
                if (!i.isUp || i.isLoopback || !(nm.startsWith("wlan") || nm.startsWith("ap") || nm.startsWith("swlan") || nm.startsWith("softap"))) continue
                for (ad in i.inetAddresses) if (ad is Inet4Address && ad.isSiteLocalAddress) return Lan(ad, null)
            }
        } catch (_: Exception) {}
        return null
    }

    private fun watchNetwork(lan: Lan) {
        val bound = lan.network ?: return
        try {
            val cm = a.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            val cb = object : ConnectivityManager.NetworkCallback() {
                override fun onLost(network: Network) {
                    if (network == bound) stopInternal("Wi-Fi lost")
                }
                override fun onLinkPropertiesChanged(network: Network, lp: LinkProperties) {
                    if (network == bound && lp.linkAddresses.none { it.address == lan.addr }) stopInternal("Wi-Fi address changed")
                }
            }
            cm.registerNetworkCallback(NetworkRequest.Builder().build(), cb)
            netCallback = cb
        } catch (_: Exception) {
            // the 10 s address check in the housekeeping tick still catches a lost network
        }
    }

    private fun addressStillPresent(): Boolean {
        val want = bindAddr ?: return false
        return try {
            for (i in NetworkInterface.getNetworkInterfaces()) for (ad in i.inetAddresses) if (ad == want) return true
            false
        } catch (_: Exception) { true }
    }

    private fun allowedClient(c: InetAddress): Boolean = c.isSiteLocalAddress || c.isLinkLocalAddress || c.isLoopbackAddress

    // ============================================================================================ acceptor

    private fun acceptLoop(srv: ServerSocket) {
        var lastTick = 0L
        while (running) {
            val s = try {
                srv.accept()
            } catch (_: SocketTimeoutException) {
                null
            } catch (_: IOException) {
                break
            }
            val now = SystemClock.elapsedRealtime()
            if (now - lastTick >= 1000L) { lastTick = now; if (tick(now)) break }
            if (s == null) continue
            if (!allowedClient(s.inetAddress)) { closeQuiet(s); continue }
            try {
                s.soTimeout = SO_TIMEOUT_MS
                s.tcpNoDelay = true
                conns.add(s)
                (pool ?: throw RejectedExecutionException()).execute { handle(s) }
            } catch (_: Exception) {
                conns.remove(s)
                closeQuiet(s)
            }
        }
    }

    /** 1 Hz housekeeping on the acceptor thread. @return true when the server stopped. */
    private fun tick(now: Long): Boolean {
        if (now - lastActivity > IDLE_MS) { stopInternal("idle 30 min"); return true }
        try {
            val pm = a.getSystemService(Context.POWER_SERVICE) as PowerManager
            if (!pm.isInteractive) { stopInternal("screen off"); return true }
        } catch (_: Exception) {}
        if (now - lastAddrCheck >= ADDR_CHECK_MS) {
            lastAddrCheck = now
            if (!addressStillPresent()) { stopInternal("Wi-Fi lost"); return true }
        }
        val it = sessions.values.iterator()
        var removed = false
        while (it.hasNext()) if (it.next().expires < now) { it.remove(); removed = true }
        if (removed) changed()
        return false
    }

    // ============================================================================================ connection

    private fun handle(s: Socket) {
        try {
            val raw = s.getOutputStream()
            val req = try {
                readRequest(s.getInputStream(), raw)
            } catch (e: HttpError) {
                sendText(raw, e.status, e.message ?: "error")
                return
            }
            route(s, raw, req)
        } catch (_: IOException) {
            // client went away / timed out
        } catch (_: Exception) {
            // never let a worker die on a malformed request
        } finally {
            conns.remove(s)
            closeQuiet(s)
        }
    }

    private fun route(s: Socket, raw: OutputStream, req: Request) {
        val ip = s.inetAddress.hostAddress ?: ""
        val host = req.headers["host"] ?: return sendText(raw, 400, "Host header required")
        if (!HOST_RE.matches(host)) return sendText(raw, 400, "bad Host")
        req.headers["origin"]?.let { if (it.removePrefix("http://") != host) return sendText(raw, 403, "bad Origin") }

        if (req.path == "/") {
            if (req.method != "GET") return sendText(raw, 405, "GET only")
            val p = pageBytes() ?: return sendText(raw, 500, "editor page missing")
            return send(raw, 200, "text/html; charset=utf-8", p, "Content-Security-Policy: $CSP\r\n")
        }
        if (req.path == "/favicon.ico") return send(raw, 204, "image/x-icon", ByteArray(0))
        if (req.path == "/login") {
            if (req.method != "POST") return sendText(raw, 405, "POST only")
            return login(raw, ip, req)
        }

        val session = authenticate(req, ip) ?: return sendJson(raw, 401, "{\"ok\":false,\"error\":\"auth\"}")
        session.seen = SystemClock.elapsedRealtime()
        lastActivity = session.seen
        when (req.path) {
            "/api" -> if (req.method == "POST") api(raw, req) else sendText(raw, 405, "POST only")
            "/api/ping" -> if (req.method == "GET") sendJson(raw, 200, "{\"ok\":true,\"changes\":${remoteChanges()}}") else sendText(raw, 405, "GET only")
            "/events" -> if (req.method == "GET") events(s, raw, session) else sendText(raw, 405, "GET only")
            "/backup.zip" -> if (req.method == "GET") backup(raw) else sendText(raw, 405, "GET only")
            else -> sendText(raw, 404, "not found")
        }
    }

    // ============================================================================================ HTTP parsing

    private fun readLine(i: InputStream, max: Int, deadline: Long, status: Int): String? {
        val sb = StringBuilder()
        while (true) {
            if (System.nanoTime() > deadline) throw HttpError(408, "request timeout")
            val b = i.read()
            if (b < 0) { if (sb.isEmpty()) return null; throw HttpError(400, "truncated request") }
            if (b == '\n'.code) return sb.toString()
            if (b != '\r'.code) {
                sb.append(b.toChar())
                if (sb.length > max) throw HttpError(status, "line too long")
            }
        }
    }

    private fun readRequest(inp: InputStream, out: OutputStream): Request {
        val deadline = System.nanoTime() + REQUEST_DEADLINE_MS * 1_000_000L
        val i = java.io.BufferedInputStream(inp, 4096)
        val line = readLine(i, MAX_REQUEST_LINE, deadline, 414) ?: throw HttpError(400, "empty request")
        val parts = line.split(' ')
        if (parts.size != 3 || !parts[2].startsWith("HTTP/1.")) throw HttpError(400, "bad request line")
        val method = parts[0]
        if (method != "GET" && method != "POST") throw HttpError(405, "GET and POST only")
        val target = parts[1]
        if (!target.startsWith("/")) throw HttpError(400, "bad target")
        val path = target.substringBefore('?').substringBefore('#')

        val headers = HashMap<String, String>()
        var total = 0
        while (true) {
            val h = readLine(i, MAX_HEADER_BYTES, deadline, 431) ?: throw HttpError(400, "truncated headers")
            if (h.isEmpty()) break
            total += h.length + 2
            if (total > MAX_HEADER_BYTES || headers.size >= MAX_HEADERS) throw HttpError(431, "headers too large")
            val c = h.indexOf(':')
            if (c <= 0) throw HttpError(400, "bad header")
            val name = h.substring(0, c).trim().lowercase(Locale.US)
            val value = h.substring(c + 1).trim()
            if (headers.containsKey(name) && (name == "content-length" || name == "host")) throw HttpError(400, "duplicate $name")
            headers.putIfAbsent(name, value)
        }
        if (headers.containsKey("transfer-encoding")) throw HttpError(501, "chunked bodies are not supported")

        var length = 0
        headers["content-length"]?.let {
            if (it.isEmpty() || it.length > 9 || !it.all { ch -> ch in '0'..'9' }) throw HttpError(400, "bad Content-Length")
            val n = it.toLong()
            if (n > MAX_BODY) throw HttpError(413, "body larger than 2 MB")
            length = n.toInt()
        }
        if (method == "POST" && length > 0 && headers["expect"]?.equals("100-continue", ignoreCase = true) == true) {
            out.write("HTTP/1.1 100 Continue\r\n\r\n".toByteArray(Charsets.US_ASCII))
            out.flush()
        }
        val body = ByteArray(if (method == "POST") length else 0)
        var got = 0
        while (got < body.size) {
            if (System.nanoTime() > deadline) throw HttpError(408, "request timeout")
            val n = i.read(body, got, body.size - got)
            if (n < 0) throw HttpError(400, "truncated body")
            got += n
        }
        return Request(method, path, headers, body)
    }

    // ============================================================================================ HTTP responses

    private fun reason(code: Int) = when (code) {
        200 -> "OK"; 204 -> "No Content"; 400 -> "Bad Request"; 401 -> "Unauthorized"; 403 -> "Forbidden"
        404 -> "Not Found"; 405 -> "Method Not Allowed"; 408 -> "Request Timeout"; 411 -> "Length Required"
        413 -> "Payload Too Large"; 414 -> "URI Too Long"; 429 -> "Too Many Requests"; 431 -> "Request Header Fields Too Large"
        500 -> "Internal Server Error"; 501 -> "Not Implemented"; 503 -> "Service Unavailable"; else -> "Error"
    }

    private fun head(code: Int, ctype: String, length: Long, extra: String): String =
        "HTTP/1.1 $code ${reason(code)}\r\nContent-Type: $ctype\r\n" +
            (if (length >= 0) "Content-Length: $length\r\n" else "") +
            "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n" +
            "Referrer-Policy: no-referrer\r\n$extra\r\n"

    private fun send(out: OutputStream, code: Int, ctype: String, body: ByteArray, extra: String = "") {
        val h = head(code, ctype, body.size.toLong(), extra).toByteArray(Charsets.ISO_8859_1)
        val all = ByteArray(h.size + body.size)
        System.arraycopy(h, 0, all, 0, h.size)
        System.arraycopy(body, 0, all, h.size, body.size)
        out.write(all)
        out.flush()
    }

    private fun sendText(out: OutputStream, code: Int, msg: String) =
        try { send(out, code, "text/plain; charset=utf-8", msg.toByteArray(Charsets.UTF_8)) } catch (_: IOException) {}

    private fun sendJson(out: OutputStream, code: Int, json: String, extra: String = "") =
        send(out, code, "application/json; charset=utf-8", json.toByteArray(Charsets.UTF_8), extra)

    private fun pageBytes(): ByteArray? {
        page?.let { return it }
        return try {
            a.assets.open(PAGE_ASSET).use { it.readBytes() }.also { page = it }
        } catch (_: IOException) { null }
    }

    // ============================================================================================ auth

    private fun login(out: OutputStream, ip: String, req: Request) {
        val now = SystemClock.elapsedRealtime()
        val left = synchronized(fails) { fails[ip]?.lockUntil?.minus(now) ?: 0L }
        if (left > 0) {
            return sendJson(out, 429, "{\"ok\":false,\"error\":\"locked\",\"retry\":${(left + 999) / 1000}}",
                "Retry-After: ${(left + 999) / 1000}\r\n")
        }
        val given = try {
            val o = JSONObject(String(req.body, Charsets.UTF_8))
            o.opt("pin")?.toString() ?: ""
        } catch (_: JSONException) { "" }
        // compare fixed-length digests in constant time (never short-circuits on the first wrong digit)
        val ok = MessageDigest.isEqual(sha256(given.toByteArray(Charsets.UTF_8)), pinHash)
        if (ok) {
            synchronized(fails) { fails.remove(ip) }
            val tok = ByteArray(16).also { rnd.nextBytes(it) }.joinToString("") { String.format("%02x", it) }
            if (sessions.size >= MAX_SESSIONS) sessions.values.minByOrNull { it.seen }?.let { sessions.remove(it.token) }
            sessions[tok] = Session(tok, ip, now + SESSION_MS)
            lastActivity = now
            changed()
            return sendJson(out, 200, "{\"ok\":true}",
                "Set-Cookie: $COOKIE=$tok; Path=/; HttpOnly; SameSite=Strict; Max-Age=${SESSION_MS / 1000}\r\n")
        }
        var remaining = IP_FAILS
        synchronized(fails) {
            if (fails.size > 256) fails.entries.removeAll { it.value.lockUntil < now }
            val f = fails.getOrPut(ip) { Fail() }
            f.count++
            if (f.count >= IP_FAILS) { f.count = 0; f.lockUntil = now + LOCK_MS }
            remaining = IP_FAILS - f.count
        }
        if (globalFails.incrementAndGet() >= GLOBAL_FAILS) {
            sendJson(out, 403, "{\"ok\":false,\"error\":\"too many failed attempts, server stopped\"}")
            stopInternal("too many failed PIN attempts")
            return
        }
        sendJson(out, 401, "{\"ok\":false,\"error\":\"wrong PIN\",\"left\":$remaining}")
    }

    private fun authenticate(req: Request, ip: String): Session? {
        val cookie = req.headers["cookie"] ?: return null
        var tok: String? = null
        for (part in cookie.split(';')) {
            val p = part.trim()
            if (p.startsWith("$COOKIE=")) { tok = p.substring(COOKIE.length + 1); break }
        }
        val s = sessions[tok ?: return null] ?: return null
        if (s.expires < SystemClock.elapsedRealtime()) { sessions.remove(s.token); return null }
        if (s.ip != ip) return null                       // a stolen cookie is useless from another address
        return s
    }

    private fun sha256(b: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(b)

    // ============================================================================================ endpoints

    private fun api(out: OutputStream, req: Request) {
        val o = try { JSONObject(String(req.body, Charsets.UTF_8)) } catch (_: JSONException) {
            return sendJson(out, 400, "{\"ok\":false,\"error\":\"body must be one JSON object\"}")
        }
        val op = o.optString("op", "")
        if (op == "app.setlist_get" || op == "app.setlist_set") return sendJson(out, 200, appOp(op, o))
        if (op !in OPS) {
            return sendJson(out, 403, JSONObject().put("ok", false).put("op", op).put("error", "op not allowed over the web").toString())
        }
        // forward the re-serialised object the allow-list just looked at (no second parser can see a different "op")
        val reply = try { remoteHandle(o.toString()) } catch (e: Throwable) {
            JSONObject().put("ok", false).put("op", op).put("error", e.message ?: "engine error").toString()
        }
        sendJson(out, 200, reply)
    }

    private fun appOp(op: String, req: JSONObject): String {
        fun err(m: String) = JSONObject().put("ok", false).put("op", op).put("error", m).toString()
        return try {
            if (op == "app.setlist_get") {
                val text = onUi { setlistJsonGet() }
                JSONObject().put("ok", true).put("op", op).put("setlist", JSONObject(text)).toString()
            } else {
                val sl = req.optJSONObject("setlist") ?: return err("'setlist' must be an object")
                val slots = sl.optJSONArray("slots") ?: return err("'setlist.slots' must be an array")
                if (slots.length() > 64) return err("at most 64 slots")
                val ok = onUi { setlistJsonSet(sl.toString()) }
                if (ok) JSONObject().put("ok", true).put("op", op).toString() else err("set list rejected")
            }
        } catch (e: Exception) {
            err(e.message ?: "failed")
        }
    }

    /** Run [block] on the UI thread and wait (3 s) for the result. */
    private fun <T> onUi(block: () -> T): T {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return block()
        val latch = CountDownLatch(1)
        var result: Any? = null
        var failure: Throwable? = null
        a.ui.post {
            try { result = block() } catch (t: Throwable) { failure = t } finally { latch.countDown() }
        }
        if (!latch.await(3, TimeUnit.SECONDS)) throw IOException("app is busy")
        failure?.let { throw it }
        @Suppress("UNCHECKED_CAST")
        return result as T
    }

    private fun events(s: Socket, out: OutputStream, session: Session) {
        if (sseOpen.incrementAndGet() > MAX_SSE) {
            sseOpen.decrementAndGet()
            return sendText(out, 503, "too many event streams")
        }
        session.sse.incrementAndGet()
        changed()
        try {
            s.soTimeout = 0
            val h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n" +
                "X-Accel-Buffering: no\r\nX-Content-Type-Options: nosniff\r\n\r\nretry: 3000\n\n"
            out.write(h.toByteArray(Charsets.ISO_8859_1))
            var last = remoteChanges()
            out.write("data: {\"changes\":$last}\n\n".toByteArray(Charsets.US_ASCII))
            out.flush()
            var beat = SystemClock.elapsedRealtime()
            while (running && !s.isClosed && sessions.containsKey(session.token)) {
                Thread.sleep(SSE_POLL_MS)
                val c = remoteChanges()
                val now = SystemClock.elapsedRealtime()
                if (c != last) {
                    last = c
                    beat = now
                    out.write("data: {\"changes\":$c}\n\n".toByteArray(Charsets.US_ASCII))
                    out.flush()
                } else if (now - beat >= HEARTBEAT_MS) {
                    beat = now
                    out.write(": hb\n\n".toByteArray(Charsets.US_ASCII))
                    out.flush()
                }
            }
        } catch (_: IOException) {
        } catch (_: InterruptedException) {
        } finally {
            sseOpen.decrementAndGet()
            session.sse.decrementAndGet()
            if (running) changed()
        }
    }

    private fun backup(raw: OutputStream) {
        if (!backupBusy.compareAndSet(false, true)) return sendText(raw, 503, "a backup is already being sent")
        try {
            val name = "HSynth_Backup_" + SimpleDateFormat("yyyyMMdd_HHmm", Locale.US).format(Date()) + ".zip"
            raw.write(head(200, "application/zip", -1L, "Content-Disposition: attachment; filename=\"$name\"\r\n")
                .toByteArray(Charsets.ISO_8859_1))
            val out = BufferedOutputStream(raw, 32 * 1024)
            BackupManager(a).createBackup(out, false, { _, _ -> }, { !running })
            out.flush()
        } finally {
            backupBusy.set(false)
        }
    }

    private fun closeQuiet(s: Socket) {
        try { s.close() } catch (_: IOException) {}
    }
}
