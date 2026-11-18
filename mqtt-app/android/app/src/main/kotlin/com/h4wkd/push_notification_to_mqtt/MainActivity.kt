package com.h4wkd.push_notification_to_mqtt

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        EventChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "mydeskmate/notif_events"
        ).setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                SyncNotificationListener.eventSink = events
            }

            override fun onCancel(arguments: Any?) {
                SyncNotificationListener.eventSink = null
            }
        })

        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "mydeskmate/notif_methods"
        ).setMethodCallHandler { call, result ->
            when (call.method) {
                "dismiss" -> {
                    val key = call.argument<String>("key") ?: ""
                    SyncNotificationListener.dismissByKey(key)
                    result.success(true)
                }
                "dismissAll" -> {
                    SyncNotificationListener.dismissAll()
                    result.success(true)
                }
                else -> result.notImplemented()
            }
        }
    }
}
