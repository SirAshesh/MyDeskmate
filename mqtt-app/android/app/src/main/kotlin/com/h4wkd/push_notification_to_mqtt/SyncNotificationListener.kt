package com.h4wkd.push_notification_to_mqtt

import android.app.Notification
import android.os.Handler
import android.os.Looper
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import io.flutter.plugin.common.EventChannel

class SyncNotificationListener : NotificationListenerService() {
    companion object {
        var instance: SyncNotificationListener? = null
        var eventSink: EventChannel.EventSink? = null
        private val mainHandler = Handler(Looper.getMainLooper())

        fun dismissByKey(key: String) {
            if (key.isEmpty()) return
            instance?.cancelNotification(key)
        }

        fun dismissAll() {
            instance?.cancelAllNotifications()
        }
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        instance = this
    }

    override fun onListenerDisconnected() {
        if (instance == this) {
            instance = null
        }
        super.onListenerDisconnected()
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

        val payload = hashMapOf(
            "action" to "post",
            "key" to (sbn.key ?: ""),
            "packageName" to pkg,
            "title" to title,
            "message" to message
        )
        mainHandler.post {
            eventSink?.success(payload)
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        if (sbn == null) return
        val pkg = sbn.packageName ?: return
        if (pkg == "com.android.systemui" || pkg == packageName) return

        val key = sbn.key ?: return
        if (key.isEmpty()) return

        val payload = hashMapOf(
            "action" to "remove",
            "key" to key
        )
        mainHandler.post {
            eventSink?.success(payload)
        }
    }
}
