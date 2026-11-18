package com.h4wkd.push_notification_to_mqtt

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log
import io.flutter.plugin.common.EventChannel
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

class SyncNotificationListener : NotificationListenerService() {
    companion object {
        private const val TAG = "MyDeskmateSync"
        private const val CHANNEL_ID = "mydeskmate_sync_bg"
        private const val FG_NOTIF_ID = 9001
        private const val DEFAULT_BROKER = "broker.emqx.io"
        private const val DEFAULT_TOPIC = "mydeskmate/sirashesh/notif"

        var instance: SyncNotificationListener? = null
        var eventSink: EventChannel.EventSink? = null
        private val mainHandler = Handler(Looper.getMainLooper())

        fun dismissByKey(key: String) {
            if (key.isEmpty()) return
            try {
                instance?.cancelNotification(key)
            } catch (_: Exception) {
            }
        }

        fun dismissAll() {
            try {
                instance?.cancelAllNotifications()
            } catch (_: Exception) {
            }
        }

        fun reloadConfig() {
            instance?.restartMqtt()
        }
    }

    private val running = AtomicBoolean(false)
    private val txExecutor = Executors.newSingleThreadExecutor()
    private val pendingTxQueue = ConcurrentLinkedQueue<String>()
    private var workerThread: Thread? = null

    @Volatile
    private var socket: SSLSocket? = null
    @Volatile
    private var outStream: OutputStream? = null
    @Volatile
    private var mqttConnected = false

    private var brokerHost = DEFAULT_BROKER
    private var baseTopic = DEFAULT_TOPIC
    private var mqttUser = ""
    private var mqttPass = ""

    override fun onCreate() {
        super.onCreate()
        startForegroundCompat()
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        Log.i(TAG, "onListenerConnected")
        instance = this
        startForegroundCompat()
        startMqttLoop()
    }

    override fun onListenerDisconnected() {
        if (instance == this) {
            instance = null
        }
        stopMqttLoop()
        super.onListenerDisconnected()
    }

    override fun onDestroy() {
        stopMqttLoop()
        txExecutor.shutdownNow()
        super.onDestroy()
    }

    private fun startForegroundCompat() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                if (nm.getNotificationChannel(CHANNEL_ID) == null) {
                    val ch = NotificationChannel(
                        CHANNEL_ID,
                        "MyDeskmate Background Sync",
                        NotificationManager.IMPORTANCE_LOW
                    )
                    ch.setShowBadge(false)
                    nm.createNotificationChannel(ch)
                }
            }
            val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                Notification.Builder(this, CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION")
                Notification.Builder(this)
            }
            val notif = builder
                .setContentTitle("MyDeskmate Sync Aktif")
                .setContentText("Sinkronisasi notifikasi ke ESP32 berjalan di latar belakang")
                .setSmallIcon(R.mipmap.ic_launcher)
                .setOngoing(true)
                .build()
            startForeground(FG_NOTIF_ID, notif)
        } catch (_: Exception) {
        }
    }

    private fun loadConfigFromPrefs() {
        val prefs = getSharedPreferences("FlutterSharedPreferences", Context.MODE_PRIVATE)
        var host = prefs.getString("flutter.SharedPreferenceKey.mqttServerURL", DEFAULT_BROKER) ?: DEFAULT_BROKER
        host = host.trim().replace(Regex("^(mqtts?|ssl|tcp)://"), "")
        if (host.contains(":")) host = host.substringBefore(":")
        brokerHost = if (host.isEmpty()) DEFAULT_BROKER else host

        val topic = prefs.getString("flutter.SharedPreferenceKey.defaultTopic", DEFAULT_TOPIC)?.trim() ?: DEFAULT_TOPIC
        baseTopic = if (topic.isEmpty()) DEFAULT_TOPIC else topic

        mqttUser = prefs.getString("flutter.SharedPreferenceKey.mqttUsername", "") ?: ""
        mqttPass = prefs.getString("flutter.SharedPreferenceKey.mqttPassword", "") ?: ""
    }

    fun restartMqtt() {
        stopMqttLoop()
        startMqttLoop()
    }

    private fun startMqttLoop() {
        if (running.getAndSet(true)) return
        loadConfigFromPrefs()
        workerThread = Thread {
            while (running.get()) {
                try {
                    connectAndReadLoop()
                } catch (e: Exception) {
                    emitUiLog("[MQTT BG] Disconnected: ${e.message}")
                } finally {
                    mqttConnected = false
                    closeSocketQuietly()
                }
                if (running.get()) {
                    try {
                        Thread.sleep(3000)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }
        }.apply {
            isDaemon = true
            start()
        }
    }

    private fun stopMqttLoop() {
        running.set(false)
        mqttConnected = false
        closeSocketQuietly()
        workerThread?.interrupt()
        workerThread = null
    }

    private fun closeSocketQuietly() {
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
        outStream = null
    }

    private fun emitUiLog(msg: String) {
        Log.i(TAG, msg)
        mainHandler.post {
            eventSink?.success(hashMapOf("action" to "log", "message" to msg))
        }
    }

    private fun encodeVarInt(value: Int): ByteArray {
        val baos = ByteArrayOutputStream()
        var v = value
        do {
            var digit = v % 128
            v /= 128
            if (v > 0) digit = digit or 0x80
            baos.write(digit)
        } while (v > 0)
        return baos.toByteArray()
    }

    private fun writeUtf8Field(baos: ByteArrayOutputStream, text: String) {
        val bytes = text.toByteArray(StandardCharsets.UTF_8)
        baos.write((bytes.size shr 8) and 0xFF)
        baos.write(bytes.size and 0xFF)
        baos.write(bytes)
    }

    private fun buildConnectPacket(clientId: String): ByteArray {
        val body = ByteArrayOutputStream()
        writeUtf8Field(body, "MQTT")
        body.write(0x04) // MQTT 3.1.1
        var flags = 0x02 // Clean session
        if (mqttUser.isNotEmpty()) flags = flags or 0x80
        if (mqttPass.isNotEmpty()) flags = flags or 0x40
        body.write(flags)
        body.write(0x00)
        body.write(20) // KeepAlive 20s

        writeUtf8Field(body, clientId)
        if (mqttUser.isNotEmpty()) writeUtf8Field(body, mqttUser)
        if (mqttPass.isNotEmpty()) writeUtf8Field(body, mqttPass)

        val payload = body.toByteArray()
        val pkt = ByteArrayOutputStream()
        pkt.write(0x10)
        pkt.write(encodeVarInt(payload.size))
        pkt.write(payload)
        return pkt.toByteArray()
    }

    private fun buildSubscribePacket(packetId: Int, topic: String): ByteArray {
        val body = ByteArrayOutputStream()
        body.write((packetId shr 8) and 0xFF)
        body.write(packetId and 0xFF)
        writeUtf8Field(body, topic)
        body.write(0x01) // QoS 1

        val payload = body.toByteArray()
        val pkt = ByteArrayOutputStream()
        pkt.write(0x82)
        pkt.write(encodeVarInt(payload.size))
        pkt.write(payload)
        return pkt.toByteArray()
    }

    private fun buildPublishPacket(topic: String, message: String): ByteArray {
        val body = ByteArrayOutputStream()
        writeUtf8Field(body, topic)
        body.write(message.toByteArray(StandardCharsets.UTF_8))

        val payload = body.toByteArray()
        val pkt = ByteArrayOutputStream()
        pkt.write(0x30) // PUBLISH QoS 0
        pkt.write(encodeVarInt(payload.size))
        pkt.write(payload)
        return pkt.toByteArray()
    }

    private fun readRemainingLength(dis: DataInputStream): Int {
        var multiplier = 1
        var value = 0
        var digit: Int
        do {
            digit = dis.readUnsignedByte()
            value += (digit and 0x7F) * multiplier
            multiplier *= 128
        } while ((digit and 0x80) != 0)
        return value
    }

    private fun connectAndReadLoop() {
        val sslSocket = SSLSocketFactory.getDefault().createSocket(brokerHost, 8883) as SSLSocket
        sslSocket.soTimeout = 60000
        sslSocket.startHandshake()
        socket = sslSocket

        val out = sslSocket.outputStream
        outStream = out
        val dis = DataInputStream(BufferedInputStream(sslSocket.inputStream))

        val clientId = "mydeskmate_bg_${System.currentTimeMillis()}"
        synchronized(out) {
            out.write(buildConnectPacket(clientId))
            out.flush()
        }

        // Read CONNACK
        val pktType = dis.readUnsignedByte()
        val remLen = readRemainingLength(dis)
        val connack = ByteArray(remLen)
        dis.readFully(connack)
        if ((pktType and 0xF0) != 0x20 || remLen < 2 || connack[1].toInt() != 0) {
            throw RuntimeException("MQTT CONNACK rejected")
        }

        val syncTopic = "$baseTopic/sync"
        synchronized(out) {
            out.write(buildSubscribePacket(1, syncTopic))
            out.flush()
        }

        mqttConnected = true
        emitUiLog("[MQTT BG TLS] Connected to $brokerHost:8883")

        // Flush antrean pesan yang tertunda saat sempat putus
        flushPendingQueue()

        // Keepalive thread
        val kaThread = Thread {
            while (running.get() && mqttConnected) {
                try {
                    Thread.sleep(15000)
                    if (!mqttConnected) break
                    val os = outStream ?: break
                    synchronized(os) {
                        os.write(byteArrayOf(0xC0.toByte(), 0x00))
                        os.flush()
                    }
                } catch (_: Exception) {
                    break
                }
            }
        }.apply {
            isDaemon = true
            start()
        }

        try {
            while (running.get()) {
                val first = dis.readUnsignedByte()
                val len = readRemainingLength(dis)
                val data = ByteArray(len)
                if (len > 0) dis.readFully(data)

                val type = first and 0xF0
                if (type == 0x30 && len >= 2) {
                    val topicLen = ((data[0].toInt() and 0xFF) shl 8) or (data[1].toInt() and 0xFF)
                    var pos = 2 + topicLen
                    if ((first and 0x06) != 0) pos += 2 // QoS > 0 has packetId
                    if (pos <= len) {
                        val payloadStr = String(data, pos, len - pos, StandardCharsets.UTF_8).trim()
                        handleIncomingSyncJson(payloadStr)
                    }
                }
            }
        } finally {
            mqttConnected = false
            kaThread.interrupt()
        }
    }

    private fun handleIncomingSyncJson(raw: String) {
        try {
            val obj = JSONObject(raw)
            val action = obj.optString("action", "")
            if (action == "remove") {
                val key = obj.optString("key", "")
                if (key.isNotEmpty()) {
                    dismissByKey(key)
                    emitUiLog("[SYNC] Dismissed on Android: $key")
                }
            } else if (action == "clear_all") {
                dismissAll()
                emitUiLog("[SYNC] Cleared all notifications on Android")
            }
        } catch (_: Exception) {
        }
    }

    private fun flushPendingQueue() {
        txExecutor.execute {
            while (mqttConnected) {
                val jsonStr = pendingTxQueue.peek() ?: break
                val os = outStream ?: break
                try {
                    synchronized(os) {
                        os.write(buildPublishPacket(baseTopic, jsonStr))
                        os.flush()
                    }
                    pendingTxQueue.poll()
                    Log.i(TAG, "Published to $baseTopic: $jsonStr")
                } catch (e: Exception) {
                    Log.e(TAG, "Flush publish error: ${e.message}")
                    closeSocketQuietly()
                    break
                }
            }
        }
    }

    private fun publishJsonAsync(obj: JSONObject) {
        val jsonStr = obj.toString()
        if (pendingTxQueue.size >= 30) {
            pendingTxQueue.poll()
        }
        pendingTxQueue.add(jsonStr)
        if (mqttConnected) {
            flushPendingQueue()
        } else {
            Log.i(TAG, "Queued while reconnecting: $jsonStr")
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn == null) return
        val pkg = sbn.packageName ?: return
        if (pkg == "com.android.systemui" || pkg == packageName) return

        val extras = sbn.notification?.extras ?: return
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        val message = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString()
            ?: extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()
            ?: ""

        if (title.isEmpty() && message.isEmpty()) return

        val key = sbn.key ?: ""
        val json = JSONObject().apply {
            put("action", "post")
            put("key", key)
            put("app", pkg)
            put("title", title)
            put("message", message)
        }
        publishJsonAsync(json)

        mainHandler.post {
            eventSink?.success(
                hashMapOf(
                    "action" to "post",
                    "key" to key,
                    "packageName" to pkg,
                    "title" to title,
                    "message" to message
                )
            )
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        if (sbn == null) return
        val pkg = sbn.packageName ?: return
        if (pkg == "com.android.systemui" || pkg == packageName) return

        val key = sbn.key ?: return
        if (key.isEmpty()) return

        val json = JSONObject().apply {
            put("action", "remove")
            put("key", key)
        }
        publishJsonAsync(json)

        mainHandler.post {
            eventSink?.success(hashMapOf("action" to "remove", "key" to key))
        }
    }
}
