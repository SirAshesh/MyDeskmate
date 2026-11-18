import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:device_apps/device_apps.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:built_collection/built_collection.dart';
import 'package:http/http.dart' as http;
import 'package:collection/collection.dart';

import 'notification_mqtt_rule.dart';
import 'notification.dart' as DataModel;
import 'console_output.dart';
import 'mqtt_client_payload_builder_string.dart';

enum SharedPreferenceKey { rulesURL, mqttServerURL, mqttUsername, mqttPassword, defaultTopic }

class DataManager with ChangeNotifier {
  static const String kDefaultBroker = 'broker.emqx.io';
  static const String kDefaultTopic  = 'mydeskmate/sirashesh/notif';

  List<NotificationMQTTRule> rules = [];
  List<ConsoleOutput> consoleOutput = [];
  Set<String> consoleOutputPackageNameFilter = {};

  String? _rulesURL;
  String? _mqttServerURL = kDefaultBroker;
  String? _mqttUsername;
  String? _mqttPassword;
  String  _defaultTopic  = kDefaultTopic;

  final http.Client _client = http.Client();
  static const EventChannel _notifEventChannel = EventChannel('mydeskmate/notif_events');
  static const MethodChannel _notifMethodChannel = MethodChannel('mydeskmate/notif_methods');
  StreamSubscription<dynamic>? _subscription;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _mqttSub;
  final List<DataModel.ProcessedNotification> _processedNotifications = List.empty(growable: true);
  MqttServerClient? _mqttServerClient;
  List<ApplicationWithIcon> _apps = List.empty();

  DataManager() {
    _loadSettings();
    _loadApps();
    _startMQTTServerClient();
    _startRemoteNotificationListener();

    Timer.periodic(const Duration(hours: 1), (timer) {
      reloadRules();
    });
  }

  Future<void> _loadSettings() async {
    SharedPreferences prefs = await SharedPreferences.getInstance();

    final loadedMQTTServerURL = prefs.getString(SharedPreferenceKey.mqttServerURL.toString());
    if (loadedMQTTServerURL != null && loadedMQTTServerURL.isNotEmpty) {
      _mqttServerURL = loadedMQTTServerURL;
    } else {
      _mqttServerURL = kDefaultBroker;
    }

    final loadedDefaultTopic = prefs.getString(SharedPreferenceKey.defaultTopic.toString());
    if (loadedDefaultTopic != null && loadedDefaultTopic.isNotEmpty) {
      _defaultTopic = loadedDefaultTopic;
    } else {
      _defaultTopic = kDefaultTopic;
    }

    final loadedMQTTUsername = prefs.getString(SharedPreferenceKey.mqttUsername.toString());
    if (loadedMQTTUsername != null) {
      _mqttUsername = loadedMQTTUsername;
    }

    final loadedMQTTPassword = prefs.getString(SharedPreferenceKey.mqttPassword.toString());
    if (loadedMQTTPassword != null) {
      _mqttPassword = loadedMQTTPassword;
    }

    final loadedRulesURL = prefs.getString(SharedPreferenceKey.rulesURL.toString());
    if (loadedRulesURL != null) {
      _rulesURL = loadedRulesURL;
      reloadRules();
    }
    notifyListeners();
  }

  Future<void> _loadApps() async {
    List<Application> apps = await DeviceApps.getInstalledApplications(
        onlyAppsWithLaunchIntent: true, includeAppIcons: true, includeSystemApps: true);
    List<ApplicationWithIcon> appsWithIcons =
        apps.map<ApplicationWithIcon>((app) => app as ApplicationWithIcon).toList();
    _apps = appsWithIcons;
    notifyListeners();
  }

  // Getters/Setters

  String? get rulesURL {
    return _rulesURL;
  }

  set rulesURL(String? rulesURL) {
    _rulesURL = rulesURL;
    _setPreference(SharedPreferenceKey.rulesURL.toString(), rulesURL);
  }

  String? get mqttServerURL {
    return _mqttServerURL;
  }

  set mqttServerURL(String? mqttServerURL) {
    String clean = (mqttServerURL ?? '').trim();
    clean = clean.replaceFirst(RegExp(r'^(mqtts?|ssl|tcp)://'), '');
    if (clean.contains(':')) {
      clean = clean.split(':').first;
    }
    _mqttServerURL = clean.isEmpty ? kDefaultBroker : clean;
    _setPreference(SharedPreferenceKey.mqttServerURL.toString(), _mqttServerURL);
    _mqttServerClient?.disconnect();
    _mqttServerClient = null;
    _startMQTTServerClient();
  }

  String? get mqttUsername {
    return _mqttUsername;
  }

  set mqttUsername(String? mqttUsername) {
    _mqttUsername = mqttUsername;
    _setPreference(SharedPreferenceKey.mqttUsername.toString(), mqttUsername);
  }

  String? get mqttPassword {
    return _mqttPassword;
  }

  set mqttPassword(String? mqttPassword) {
    _mqttPassword = mqttPassword;
    _setPreference(SharedPreferenceKey.mqttPassword.toString(), mqttPassword);
  }

  String get defaultTopic => _defaultTopic;

  set defaultTopic(String topic) {
    _defaultTopic = topic.trim().isEmpty ? kDefaultTopic : topic.trim();
    _setPreference(SharedPreferenceKey.defaultTopic.toString(), _defaultTopic);
  }

  Future<void> _setPreference(String key, String? value) async {
    final prefs = await SharedPreferences.getInstance();
    if (value != null) {
      prefs.setString(key, value);
    } else {
      prefs.remove(key);
    }
  }

  List<ConsoleOutput> get filteredConsoleOutput {
    if (consoleOutputPackageNameFilter.isEmpty) {
      return consoleOutput;
    }
    return consoleOutput
        .where((consoleOutput) =>
            consoleOutputPackageNameFilter.any((filter) => filter == consoleOutput.notification?.packageName))
        .toList();
  }

  // Reload rules

  Future<List<NotificationMQTTRule>> reloadRules() async {
    if (rulesURL == null) {
      logConsoleOutput(null, "reload rules failed: no rules URL detected");
      return [];
    }
    final response = await _client.get(Uri.parse(rulesURL!));
    final List<dynamic> parsed = jsonDecode(response.body);
    final List<NotificationMQTTRule> loadedRules = parsed
        .map<NotificationMQTTRule?>((item) {
          final jsonString = jsonEncode(item);
          return NotificationMQTTRule.fromJson(jsonString);
        })
        .whereNotNull()
        .toList();
    if (loadedRules.isNotEmpty) {
      rules = loadedRules;
      _notifyReload();
      notifyListeners();
    } else {
      logConsoleOutput(null, "reload rules failed: loaded rules are empty");
    }
    return rules;
  }

  Future<void> _notifyReload() async {
    // sent MQTT message after successful reload
    final builder = MqttClientPayloadBuilder();
    builder.addString('${rules.length}');
    _mqttServerClient?.publishMessage('notification-mqtt-rules-loaded', MqttQos.exactlyOnce, builder.payload!);
    logConsoleOutput(null, '${rules.length} rules reloaded');
  }

  // MQTT

  Future<void> _startMQTTServerClient() async {
    if (mqttServerURL == null || mqttServerURL!.isEmpty) {
      print('[MQTT] server url undefined');
      return;
    }

    // Gunakan TLS (port 8883) untuk koneksi terenkripsi ke public MQTT broker
    final client = MqttServerClient.withPort(mqttServerURL!, '', 8883);
    client.secure = true;
    client.securityContext = SecurityContext.defaultContext;

    client.logging(on: false);
    client.setProtocolV311();
    client.keepAlivePeriod = 20;
    client.onDisconnected = _onMQTTDisconnected;

    final clientId = 'mydeskmate_android_${DateTime.now().millisecondsSinceEpoch}';
    final connMess = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean();
    client.connectionMessage = connMess;

    try {
      if (mqttUsername != null && mqttUsername!.isNotEmpty && mqttPassword != null && mqttPassword!.isNotEmpty) {
        await client.connect(mqttUsername, mqttPassword);
      } else {
        await client.connect();
      }
    } on Exception catch (e) {
      print('[MQTT] client exception - $e');
      logConsoleOutput(null, '[MQTT TLS] Error: $e');
      client.disconnect();
      return;
    }

    /// Check we are connected
    if (client.connectionStatus!.state == MqttConnectionState.connected) {
      print('[MQTT] Mosquitto client connected');
      logConsoleOutput(null, '[MQTT TLS] Connected to $mqttServerURL:8883');
      final syncTopic = '$_defaultTopic/sync';
      client.subscribe(syncTopic, MqttQos.atLeastOnce);
      _mqttSub?.cancel();
      _mqttSub = client.updates?.listen(_onMqttSyncMessage);
    } else {
      print(
          '[MQTT] ERROR Mosquitto client connection failed - disconnecting, state is ${client.connectionStatus!.state}');
      logConsoleOutput(null, '[MQTT TLS] Connect failed: ${client.connectionStatus!.state}');
      client.disconnect();
      return;
    }

    _mqttServerClient = client;
  }

  void _onMQTTDisconnected() {
    _mqttSub?.cancel();
    _mqttSub = null;
    _mqttServerClient = null;
    print('[MQTT] Mosquitto client disconnected');
  }

  void _onMqttSyncMessage(List<MqttReceivedMessage<MqttMessage>> events) {
    for (final event in events) {
      final recMess = event.payload as MqttPublishMessage;
      final raw = MqttPublishPayload.bytesToStringAsString(recMess.payload.message).trim();
      try {
        final data = jsonDecode(raw);
        if (data is Map<String, dynamic>) {
          final action = data['action']?.toString() ?? '';
          if (action == 'remove') {
            final key = data['key']?.toString() ?? '';
            if (key.isNotEmpty) {
              _notifMethodChannel.invokeMethod('dismiss', {'key': key});
              logConsoleOutput(null, '[SYNC] Dismissed on Android: $key');
            }
          } else if (action == 'clear_all') {
            _notifMethodChannel.invokeMethod('dismissAll');
            logConsoleOutput(null, '[SYNC] Cleared all notifications on Android');
          }
        }
      } catch (e) {
        print('[SYNC] Invalid sync JSON: $e');
      }
    }
  }

  Future<void> _publishMQTTMessage(DataModel.Notification notification, NotificationMQTTRule rule) async {
    if (_mqttServerClient == null) {
      await _startMQTTServerClient();
      if (_mqttServerClient == null) {
        print('[MQTT] unable to start MQTT client');
        return;
      }
    }

    Map<String, String> payload = {"title": notification.text, "message": notification.message};
    List<String> dataParameters = [];

    if (rule.titleRegex != null) {
      final regex = RegExp(rule.titleRegex!);
      final match = regex.firstMatch(notification.text);
      if (match != null && match.groupCount > 0) {
        for (var i = 0; i < match.groupCount; i++) {
          final groupValue = match.group(i + 1);
          if (groupValue != null) {
            print('title match \'${rule.titleRegex}\' group $i value: $groupValue');
            payload["titleMatch"] = groupValue;
          }
        }
      }
    }

    if (rule.messageRegex != null) {
      final regex = RegExp(rule.messageRegex!);
      final match = regex.firstMatch(notification.message);
      if (match != null && match.groupCount > 0) {
        for (var i = 0; i < match.groupCount; i++) {
          final groupValue = match.group(i + 1);
          if (groupValue != null) {
            print('message match \'${rule.messageRegex}\' group $i value: $groupValue');
            payload["messageMatch"] = groupValue;
          }
        }
      }
    }

    final builder = MqttClientPayloadBuilder();
    builder.addString(json.encode(payload));

    print('[MQTT] publish to ${rule.publishTopic}: ${payload.toString()}');
    logConsoleOutput(notification, 'MQTT ${rule.publishTopic}: ${payload.toString()}');
    _mqttServerClient?.publishMessage(rule.publishTopic, MqttQos.exactlyOnce, builder.payload!);
  }

  // Remote notifications

  void _startRemoteNotificationListener() {
    _subscription = _notifEventChannel.receiveBroadcastStream().listen(
      _onRemoteNotificationEvent,
      onError: (err) => print('[SYNC] EventChannel error: $err'),
    );
  }

  void _stopRemoteNotificationListening() {
    _subscription?.cancel();
  }

  Future<void> _publishSyncPayload(Map<String, dynamic> payload) async {
    if (_mqttServerClient == null) {
      await _startMQTTServerClient();
      if (_mqttServerClient == null) {
        print('[MQTT] unable to start MQTT client');
        return;
      }
    }
    final builder = MqttClientPayloadBuilder();
    builder.addString(json.encode(payload));
    _mqttServerClient?.publishMessage(_defaultTopic, MqttQos.atLeastOnce, builder.payload!);
  }

  void _onRemoteNotificationEvent(dynamic rawEvent) {
    if (rawEvent is! Map) return;
    final action = rawEvent['action']?.toString() ?? 'post';
    final key    = rawEvent['key']?.toString() ?? '';

    if (action == 'remove') {
      if (key.isNotEmpty) {
        _publishSyncPayload({"action": "remove", "key": key});
        logConsoleOutput(null, '[SYNC] Removed on Android -> ESP32: $key');
      }
      return;
    }

    final pkg   = rawEvent['packageName']?.toString() ?? '';
    final title = rawEvent['title']?.toString() ?? '';
    final msg   = rawEvent['message']?.toString() ?? '';
    if (pkg == "com.android.systemui") return;

    final notification = DataModel.Notification((b) => b
      ..packageName = pkg
      ..message = msg
      ..text = title
      ..timestamp = DateTime.now());

    logConsoleOutput(notification,
        'Notification received for $pkg:\nText: $title\nMessage: $msg');

    if (rules.isEmpty) {
      _publishSyncPayload({
        "action": "post",
        "key": key,
        "app": pkg,
        "title": title,
        "message": msg,
      });
      logConsoleOutput(notification, 'MQTT TLS $_defaultTopic: $title');
      final processedNotification = DataModel.ProcessedNotification((b) => b
        ..notification = notification.toBuilder()
        ..regexMatches = BuiltList<NotificationMQTTRule>([]).toBuilder());
      _processedNotifications.insert(0, processedNotification);
      return;
    }

    processRemoteNotification(notification);
  }

  void processRemoteNotification(DataModel.Notification notification) {
    // check for reload notification
    if (notification.packageName == 'io.homeassistant.companion.android' &&
        notification.text == 'Notification MQTT' &&
        notification.message == 'reload') {
      reloadRules();
    }

    // Jika tidak ada rules.json, langsung forward semua notifikasi ke defaultTopic
    if (rules.isEmpty) {
      _publishSyncPayload({
        "action": "post",
        "key": "",
        "app": notification.packageName,
        "title": notification.text,
        "message": notification.message,
      });
      final processedNotification = DataModel.ProcessedNotification((b) => b
        ..notification = notification.toBuilder()
        ..regexMatches = BuiltList<NotificationMQTTRule>([]).toBuilder());
      _processedNotifications.insert(0, processedNotification);
      return;
    }

    var matches = List<NotificationMQTTRule>.empty(growable: true);

    for (var notificationRule in rules) {
      if (notificationRule.packageName != notification.packageName) {
        // only match if packageNames match
        continue;
      }
      bool? hasTitleRegexMatch;
      bool? hasMessageRegexMatch;
      if (notificationRule.titleRegex != null) {
        final regex = RegExp(notificationRule.titleRegex!);
        final hasMatch = regex.hasMatch(notification.text);
        if (hasMatch) {
          logConsoleOutput(
              notification, 'regex match in title: ${notification.text}\nregex: ${notificationRule.titleRegex}');
          hasTitleRegexMatch = true;
        } else {
          hasTitleRegexMatch = false;
        }
      }
      if (notificationRule.messageRegex != null) {
        final regex = RegExp(notificationRule.messageRegex!);
        final hasMatch = regex.hasMatch(notification.message);
        if (hasMatch) {
          logConsoleOutput(
              notification, 'regex match in message: ${notification.message}\nregex: ${notificationRule.messageRegex}');
          hasMessageRegexMatch = true;
        } else {
          hasMessageRegexMatch = false;
        }
      }

      // check if there's a valid regex match
      bool hasRegexMatch;
      if (hasTitleRegexMatch != null && hasMessageRegexMatch != null) {
        // if both title and message regex is provided, both must match
        hasRegexMatch = hasTitleRegexMatch == true && hasMessageRegexMatch == true;
      } else {
        hasRegexMatch = hasTitleRegexMatch == true || hasMessageRegexMatch == true;
      }

      if (hasRegexMatch) {
        matches.add(notificationRule);
        // publish MQTT message
        _publishMQTTMessage(notification, notificationRule);
      }
    }

    final processedNotification = DataModel.ProcessedNotification((b) => b
      ..notification = notification.toBuilder()
      ..regexMatches = BuiltList<NotificationMQTTRule>(matches).toBuilder());

    _processedNotifications.insert(0, processedNotification);
  }

  // Console output

  void logConsoleOutput(DataModel.Notification? notification, String message) {
    final app = _apps.firstWhereOrNull((app) => app.packageName == notification?.packageName);
    final output = ConsoleOutput((b) => b
      ..timestamp = DateTime.now()
      ..message = message
      ..notification = notification?.toBuilder()
      ..icon = app?.icon);
    consoleOutput.insert(0, output);
    notifyListeners();
  }

  void clearConsoleLogs() {
    consoleOutput.clear();
    notifyListeners();
  }

  void toggleConsoleOutputForApp(String packageName) {
    if (consoleOutputPackageNameFilter.any((filter) => filter == packageName)) {
      consoleOutputPackageNameFilter.remove(packageName);
    } else {
      consoleOutputPackageNameFilter.add(packageName);
    }
    notifyListeners();
  }

  List<ApplicationWithIcon> receivedNotificationApplications() {
    return _apps
        .where(
            (app) => consoleOutput.any((consoleOutput) => consoleOutput.notification?.packageName == app.packageName))
        .toList();
  }
}
