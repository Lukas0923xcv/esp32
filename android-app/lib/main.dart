import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_bluetooth_serial/flutter_bluetooth_serial.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:intl/intl.dart';
import 'package:shared_preferences/shared_preferences.dart';

// Bump on every change; the build number is injected by CI (--dart-define).
const String kAppVersion = '1.4.1';
const String kBuildNumber = String.fromEnvironment('BUILD_NUMBER', defaultValue: 'dev');

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const LilyGoNetzwaechterApp());
}

class LilyGoNetzwaechterApp extends StatelessWidget {
  const LilyGoNetzwaechterApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'LilyGO-Netzwächter',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF0A0F1C),
        primaryColor: const Color(0xFF10B981),
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFF10B981),
          secondary: Color(0xFF38BDF8),
          surface: Color(0xFF131A2A),
          error: Color(0xFFEF4444),
        ),
        cardTheme: const CardThemeData(
          color: Color(0xFF131A2A),
          elevation: 4,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.all(Radius.circular(16)),
            side: BorderSide(color: Color(0xFF1F2A40), width: 1),
          ),
        ),
        appBarTheme: const AppBarTheme(
          backgroundColor: Color(0xFF0F172A),
          elevation: 0,
          centerTitle: false,
          titleTextStyle: TextStyle(
            fontSize: 20,
            fontWeight: FontWeight.bold,
            letterSpacing: 0.5,
          ),
        ),
      ),
      home: const DashboardScreen(),
    );
  }
}

enum PowerStatus {
  online,
  outage,
  unknown,
}

enum ConnectionStatus {
  disconnected,
  connecting,
  connected,
  error,
}

class StatusLogEntry {
  final DateTime timestamp;
  final String title;
  final String details;
  final IconData icon;
  final Color color;
  final PowerStatus? status;

  StatusLogEntry({
    required this.timestamp,
    required this.title,
    required this.details,
    required this.icon,
    required this.color,
    this.status,
  });
}

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen>
    with SingleTickerProviderStateMixin {
  // Regex parser matching ESP32 serial format:
  // "Netz: [ONLINE] | Netzteil: 5.08V | Akku: 85% (3.95V)"
  // "Netz: [STROMAUSFALL] | Netzteil: 0.12V | Akku: 84% (3.93V)"
  final RegExp _serialRegex = RegExp(
    r'Netz:\s*\[(.*?)\]\s*\|\s*Netzteil(?:-In)?:\s*([0-9.]+)\s*V\s*\|\s*Akku:\s*([0-9]+)\s*%\s*\(([0-9.]+)\s*V\)',
    caseSensitive: false,
  );

  // Notifications
  final FlutterLocalNotificationsPlugin _notificationsPlugin =
      FlutterLocalNotificationsPlugin();

  // Bluetooth state
  BluetoothConnection? _connection;
  ConnectionStatus _connectionStatus = ConnectionStatus.disconnected;
  String _statusMessage = 'Nicht verbunden';
  BluetoothDevice? _selectedDevice;
  List<BluetoothDevice> _bondedDevices = [];
  String _serialBuffer = '';
  StreamSubscription<Uint8List>? _streamSubscription;

  // Measurement State
  PowerStatus _powerStatus = PowerStatus.unknown;
  double _psuVoltage = 0.0;
  int _batteryPercent = 0;
  double _batteryVoltage = 0.0;
  DateTime? _lastUpdate;
  String _lastRawLine = '';

  // Outage debounce: a single (or short burst of) low PSU readings is ignored
  // so brief voltage dips don't trigger a false Stromausfall alarm.
  static const Duration _outageConfirmDuration = Duration.zero;
  static const int _outageConfirmReadings = 1;
  DateTime? _outageSince;
  int _outageReadings = 0;

  // Event Log
  final List<StatusLogEntry> _eventLogs = [];

  // Animation controller for pulsing red outage tile
  late AnimationController _pulseController;
  late Animation<double> _pulseAnimation;

  // Simulation timer for testing
  Timer? _simulationTimer;
  bool _isSimulationActive = false;

  // Remembered device + automatic reconnect
  static const String _prefsDeviceKey = 'last_device_address';
  static const Duration _reconnectDelay = Duration(seconds: 5);
  String? _savedAddress;
  Timer? _reconnectTimer;
  bool _userDisconnected = false;
  int _reconnectAttempts = 0;

  @override
  void initState() {
    super.initState();

    _pulseController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 900),
    );

    _pulseAnimation = Tween<double>(begin: 0.4, end: 1.0).animate(
      CurvedAnimation(parent: _pulseController, curve: Curves.easeInOut),
    );

    _pulseController.addStatusListener((status) {
      if (status == AnimationStatus.completed) {
        _pulseController.reverse();
      } else if (status == AnimationStatus.dismissed) {
        _pulseController.forward();
      }
    });

    _initNotifications();
    _requestAppPermissions();
    _initDeviceAndAutoConnect();

    _addLog(
      title: 'App gestartet',
      details: 'Warte auf Verbindung zu LilyGO-Netzwächter',
      icon: Icons.power_settings_new,
      color: Colors.blueAccent,
    );
  }

  @override
  void dispose() {
    _pulseController.dispose();
    _streamSubscription?.cancel();
    _connection?.dispose();
    _simulationTimer?.cancel();
    _reconnectTimer?.cancel();
    super.dispose();
  }

  // ---------------------------------------------------------------------------
  // Notifications & Permissions
  // ---------------------------------------------------------------------------

  Future<void> _initNotifications() async {
    const AndroidInitializationSettings androidSettings =
        AndroidInitializationSettings('@mipmap/ic_launcher');

    const InitializationSettings initSettings =
        InitializationSettings(android: androidSettings);

    await _notificationsPlugin.initialize(initSettings);
  }

  Future<void> _requestAppPermissions() async {
    try {
      await [
        Permission.bluetoothConnect,
        Permission.bluetoothScan,
        Permission.location,
        Permission.notification,
      ].request();
    } catch (e) {
      debugPrint('Fehler bei Berechtigungsanfrage: $e');
    }
  }

  Future<void> _triggerPowerOutageNotification(
      double psuVolt, int battPct, double battVolt) async {
    // Vibration pattern: [delay, vibrate, pause, vibrate, pause, vibrate]
    final Int64List vibrationPattern =
        Int64List.fromList([0, 800, 300, 800, 300, 1200]);

    final AndroidNotificationDetails androidDetails = AndroidNotificationDetails(
      'lilygo_power_outage_channel',
      'LilyGO Stromausfall-Alarm',
      channelDescription: 'Sofortige Alarmierung bei Netzstromausfall',
      importance: Importance.max,
      priority: Priority.high,
      vibrationPattern: vibrationPattern,
      enableVibration: true,
      playSound: true,
      ticker: 'Stromausfall erkannt!',
      color: const Color(0xFFEF4444),
      ledColor: const Color(0xFFEF4444),
      ledOnMs: 1000,
      ledOffMs: 500,
    );

    final NotificationDetails notificationDetails =
        NotificationDetails(android: androidDetails);

    try {
      await _notificationsPlugin.show(
        1001,
        '⚠️ STROMAUSFALL AKTIV!',
        'Netzteil: ${psuVolt.toStringAsFixed(2)}V (Ausfall) | Akku: $battPct% (${battVolt.toStringAsFixed(2)}V)',
        notificationDetails,
      );
      HapticFeedback.heavyImpact();
    } catch (e) {
      debugPrint('Fehler beim Anzeigen der Benachrichtigung: $e');
    }
  }

  Future<void> _triggerPowerRestoredNotification(double psuVolt) async {
    const AndroidNotificationDetails androidDetails =
        AndroidNotificationDetails(
      'lilygo_power_outage_channel',
      'LilyGO Stromausfall-Alarm',
      channelDescription: 'Benachrichtigung bei Wiederkehr des Netzstroms',
      importance: Importance.high,
      priority: Priority.high,
      enableVibration: true,
      playSound: true,
      color: Color(0xFF10B981),
    );

    const NotificationDetails notificationDetails =
        NotificationDetails(android: androidDetails);

    try {
      await _notificationsPlugin.show(
        1002,
        '✅ NETZSTROM WIEDERHERGESTELLT',
        'Das 230V-Netzteil liefert wieder ${psuVolt.toStringAsFixed(2)}V.',
        notificationDetails,
      );
    } catch (e) {
      debugPrint('Fehler bei Entwarnungs-Notification: $e');
    }
  }

  // ---------------------------------------------------------------------------
  // Bluetooth Connection & Data Processing
  // ---------------------------------------------------------------------------

  Future<void> _initDeviceAndAutoConnect() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      _savedAddress = prefs.getString(_prefsDeviceKey);
    } catch (e) {
      debugPrint('Fehler beim Lesen des gespeicherten Geräts: $e');
    }
    await _loadPairedDevices();
    if (!mounted || _selectedDevice == null || _isSimulationActive) return;
    // Wait for the Bluetooth permission prompt before the first connect attempt.
    await Future<void>.delayed(const Duration(seconds: 1));
    if (mounted && _connectionStatus == ConnectionStatus.disconnected) {
      _connectToDevice(_selectedDevice);
    }
  }

  Future<void> _rememberDevice(String address) async {
    _savedAddress = address;
    try {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString(_prefsDeviceKey, address);
    } catch (e) {
      debugPrint('Fehler beim Speichern des Geräts: $e');
    }
  }

  void _scheduleReconnect() {
    _reconnectTimer?.cancel();
    if (!mounted || _userDisconnected || _isSimulationActive || _selectedDevice == null) {
      return;
    }
    _reconnectTimer = Timer(_reconnectDelay, () {
      if (mounted &&
          !_userDisconnected &&
          !_isSimulationActive &&
          _connectionStatus == ConnectionStatus.disconnected) {
        _connectToDevice(_selectedDevice, true);
      }
    });
  }

  Future<void> _userDisconnect() async {
    _userDisconnected = true;
    _reconnectTimer?.cancel();
    await _disconnect();
    _handleDisconnect('Manuell getrennt');
  }

  Future<void> _loadPairedDevices() async {
    try {
      final List<BluetoothDevice> devices =
          await FlutterBluetoothSerial.instance.getBondedDevices();
      setState(() {
        _bondedDevices = devices;
        // Prefer the remembered device, then "LilyGO-Netzwaechter"
        for (final device in devices) {
          if (_savedAddress != null && device.address == _savedAddress) {
            _selectedDevice = device;
            break;
          }
        }
        for (final device in devices) {
          if (_selectedDevice != null) break;
          final name = device.name ?? '';
          if (name.contains('LilyGO') || name.contains('Netzwaechter')) {
            _selectedDevice = device;
            break;
          }
        }
        if (_selectedDevice == null && devices.isNotEmpty) {
          _selectedDevice = devices.first;
        }
      });
    } catch (e) {
      debugPrint('Fehler beim Laden gekoppelter Bluetooth-Geräte: $e');
    }
  }

  Future<void> _connectToDevice([BluetoothDevice? deviceToConnect, bool auto = false]) async {
    final targetDevice = deviceToConnect ?? _selectedDevice;
    if (!auto) {
      _userDisconnected = false;
      _reconnectAttempts = 0;
    }
    _reconnectTimer?.cancel();
    if (targetDevice == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Kein Bluetooth-Gerät ausgewählt. Bitte koppeln oder wählen.'),
          backgroundColor: Color(0xFFEF4444),
        ),
      );
      return;
    }

    await _disconnect();

    setState(() {
      _connectionStatus = ConnectionStatus.connecting;
      _statusMessage = 'Verbinde mit ${targetDevice.name ?? targetDevice.address}...';
    });

    try {
      final connection = await BluetoothConnection.toAddress(targetDevice.address);
      setState(() {
        _connection = connection;
        _connectionStatus = ConnectionStatus.connected;
        _statusMessage = 'Verbunden mit ${targetDevice.name ?? targetDevice.address}';
      });
      _reconnectAttempts = 0;
      _rememberDevice(targetDevice.address);

      _addLog(
        title: 'Bluetooth verbunden',
        details: '${targetDevice.name ?? "Unbekannt"} (${targetDevice.address})',
        icon: Icons.bluetooth_connected,
        color: const Color(0xFF38BDF8),
      );

      _serialBuffer = '';
      _streamSubscription = connection.input?.listen(
        (Uint8List data) {
          _serialBuffer += utf8.decode(data, allowMalformed: true);
          while (_serialBuffer.contains('\n')) {
            final int newlineIndex = _serialBuffer.indexOf('\n');
            final String rawLine = _serialBuffer.substring(0, newlineIndex).trim();
            _serialBuffer = _serialBuffer.substring(newlineIndex + 1);

            if (rawLine.isNotEmpty) {
              _parseSerialLine(rawLine);
            }
          }
        },
        onDone: () {
          _handleDisconnect('Verbindung vom Gerät beendet');
        },
        onError: (dynamic error) {
          _handleDisconnect('Bluetooth Stream-Fehler: $error');
        },
      );
    } catch (e) {
      _handleDisconnect('Verbindungsfehler: $e');
    }
  }

  Future<void> _disconnect() async {
    _streamSubscription?.cancel();
    _streamSubscription = null;
    await _connection?.close();
    _connection?.dispose();
    _connection = null;
  }

  void _handleDisconnect([String? reason]) {
    final msg = reason ?? 'Verbindung getrennt';
    _outageSince = null;
    _outageReadings = 0;
    setState(() {
      _connectionStatus = ConnectionStatus.disconnected;
      _statusMessage = msg;
      _powerStatus = PowerStatus.unknown;
    });
    _pulseController.stop();

    // Only log the first failure of a reconnect series, not every retry.
    if (_reconnectAttempts == 0) {
      _addLog(
        title: 'Verbindung getrennt',
        details: msg,
        icon: Icons.bluetooth_disabled,
        color: Colors.grey,
      );
    }
    if (!_userDisconnected) {
      _reconnectAttempts++;
      _scheduleReconnect();
    }
  }

  // ---------------------------------------------------------------------------
  // Regex Parsing & Status Logic
  // ---------------------------------------------------------------------------

  void _parseSerialLine(String line, {bool debounce = true}) {
    setState(() => _lastRawLine = line);
    final match = _serialRegex.firstMatch(line);
    if (match == null) return;

    final String statusStr = match.group(1)?.trim().toUpperCase() ?? '';
    final double psuVolt = double.tryParse(match.group(2) ?? '') ?? 0.0;
    final int battPct = int.tryParse(match.group(3) ?? '') ?? 0;
    final double battVolt = double.tryParse(match.group(4) ?? '') ?? 0.0;

    PowerStatus newStatus = PowerStatus.unknown;
    if (statusStr.contains('STROMAUSFALL')) {
      newStatus = PowerStatus.outage;
    } else if (statusStr.contains('ONLINE')) {
      newStatus = PowerStatus.online;
    }

    if (debounce && newStatus == PowerStatus.outage) {
      _outageSince ??= DateTime.now();
      _outageReadings++;
      final bool confirmed = _outageReadings >= _outageConfirmReadings &&
          DateTime.now().difference(_outageSince!) >= _outageConfirmDuration;
      if (!confirmed && _powerStatus != PowerStatus.outage) {
        // Not yet confirmed: keep the previous status, but refresh the readings.
        setState(() {
          _psuVoltage = psuVolt;
          _batteryPercent = battPct.clamp(0, 100);
          _batteryVoltage = battVolt;
          _lastUpdate = DateTime.now();
        });
        return;
      }
    } else {
      _outageSince = null;
      _outageReadings = 0;
    }

    _applyNewStatus(
      newStatus: newStatus,
      psuVolt: psuVolt,
      battPct: battPct,
      battVolt: battVolt,
    );
  }

  void _applyNewStatus({
    required PowerStatus newStatus,
    required double psuVolt,
    required int battPct,
    required double battVolt,
  }) {
    final PowerStatus oldStatus = _powerStatus;

    setState(() {
      _powerStatus = newStatus;
      _psuVoltage = psuVolt;
      _batteryPercent = battPct.clamp(0, 100);
      _batteryVoltage = battVolt;
      _lastUpdate = DateTime.now();
    });

    // Control pulsing animation
    if (newStatus == PowerStatus.outage) {
      if (!_pulseController.isAnimating) {
        _pulseController.forward();
      }
    } else {
      if (_pulseController.isAnimating) {
        _pulseController.stop();
        _pulseController.reset();
      }
    }

    // Status transition detection
    if (oldStatus != newStatus) {
      if (newStatus == PowerStatus.outage) {
        _triggerPowerOutageNotification(psuVolt, battPct, battVolt);
        _addLog(
          title: 'STROMAUSFALL ERKANNT',
          details: 'Netzteil: ${psuVolt.toStringAsFixed(2)}V | Akku: $battPct% (${battVolt.toStringAsFixed(2)}V)',
          icon: Icons.warning_rounded,
          color: const Color(0xFFEF4444),
          status: PowerStatus.outage,
        );
      } else if (newStatus == PowerStatus.online) {
        if (oldStatus == PowerStatus.outage) {
          _triggerPowerRestoredNotification(psuVolt);
        }
        _addLog(
          title: 'NETZSTROM NORMAL',
          details: 'Netzteil: ${psuVolt.toStringAsFixed(2)}V | Akku: $battPct% (${battVolt.toStringAsFixed(2)}V)',
          icon: Icons.check_circle_outline,
          color: const Color(0xFF10B981),
          status: PowerStatus.online,
        );
      }
    }
  }

  void _addLog({
    required String title,
    required String details,
    required IconData icon,
    required Color color,
    PowerStatus? status,
  }) {
    setState(() {
      _eventLogs.insert(
        0,
        StatusLogEntry(
          timestamp: DateTime.now(),
          title: title,
          details: details,
          icon: icon,
          color: color,
          status: status,
        ),
      );
      // Keep recent 100 entries
      if (_eventLogs.length > 100) {
        _eventLogs.removeLast();
      }
    });
  }

  // ---------------------------------------------------------------------------
  // Demo / Simulation Mode (for testing without physical hardware)
  // ---------------------------------------------------------------------------

  void _toggleSimulation() {
    if (_isSimulationActive) {
      _simulationTimer?.cancel();
      _simulationTimer = null;
      setState(() {
        _isSimulationActive = false;
        _connectionStatus = ConnectionStatus.disconnected;
        _statusMessage = 'Simulation beendet';
        _powerStatus = PowerStatus.unknown;
      });
      _pulseController.stop();
      _pulseController.reset();
    } else {
      setState(() {
        _isSimulationActive = true;
        _connectionStatus = ConnectionStatus.connected;
        _statusMessage = 'Demo-Simulation aktiv';
      });

      _addLog(
        title: 'Demo-Modus gestartet',
        details: 'Simuliert Datenstrom des LilyGO-Netzwächters',
        icon: Icons.play_circle_outline,
        color: const Color(0xFFF59E0B),
      );

      // Initial normal status
      _parseSerialLine('Netz: [ONLINE] | Netzteil: 5.08V | Akku: 85% (3.95V)',
          debounce: false);

      int tick = 0;
      _simulationTimer = Timer.periodic(const Duration(seconds: 4), (timer) {
        tick++;
        if (tick % 4 == 2) {
          // Simulate outage
          _parseSerialLine('Netz: [STROMAUSFALL] | Netzteil: 0.12V | Akku: 84% (3.93V)', debounce: false);
        } else if (tick % 4 == 0) {
          // Simulate recovery
          _parseSerialLine('Netz: [ONLINE] | Netzteil: 5.05V | Akku: 85% (3.97V)');
        }
      });
    }
  }

  // ---------------------------------------------------------------------------
  // UI Building
  // ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        backgroundColor: Colors.transparent,
        scrolledUnderElevation: 0,
        titleSpacing: 20,
        title: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              'Netzwächter',
              style: TextStyle(
                fontSize: 22,
                fontWeight: FontWeight.w800,
                letterSpacing: -0.3,
              ),
            ),
            Text(
              'v$kAppVersion · Build $kBuildNumber',
              style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w500,
                color: Colors.white.withValues(alpha: 0.45),
              ),
            ),
          ],
        ),
        actions: [
          IconButton(
            tooltip: _isSimulationActive ? 'Demo beenden' : 'Demo-Modus starten',
            icon: Icon(
              _isSimulationActive ? Icons.stop_circle_outlined : Icons.science_outlined,
              color: _isSimulationActive ? const Color(0xFFF59E0B) : Colors.white54,
            ),
            onPressed: _toggleSimulation,
          ),
          IconButton(
            tooltip: 'Bluetooth-Geräte verwalten',
            icon: const Icon(Icons.bluetooth_rounded, color: Colors.white70),
            onPressed: _showDevicePickerModal,
          ),
          const SizedBox(width: 8),
        ],
      ),
      body: SafeArea(
        child: RefreshIndicator(
          onRefresh: _loadPairedDevices,
          color: const Color(0xFF10B981),
          child: SingleChildScrollView(
            physics: const AlwaysScrollableScrollPhysics(),
            padding: const EdgeInsets.fromLTRB(20, 4, 20, 24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _buildConnectionBar(),
                const SizedBox(height: 16),
                _buildPowerStatusTile(),
                const SizedBox(height: 16),
                IntrinsicHeight(
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Expanded(child: _buildBatteryTile()),
                      const SizedBox(width: 14),
                      Expanded(child: _buildPowerSupplyTile()),
                    ],
                  ),
                ),
                const SizedBox(height: 16),
                _buildDiagnosticsTile(),
                const SizedBox(height: 24),
                _buildEventLogSection(),
              ],
            ),
          ),
        ),
      ),
    );
  }

  BoxDecoration _cardDecoration({Color? border}) => BoxDecoration(
        color: const Color(0xFF131A2A),
        borderRadius: BorderRadius.circular(24),
        border: Border.all(color: border ?? const Color(0xFF1F2A40)),
      );

  Widget _cardLabel(String text, IconData icon, Color color) {
    return Row(
      children: [
        Icon(icon, size: 16, color: color),
        const SizedBox(width: 6),
        Expanded(
          child: Text(
            text,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w700,
              letterSpacing: 1.0,
              color: Colors.white.withValues(alpha: 0.55),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildConnectionBar() {
    Color statusColor;
    String statusLabel;

    switch (_connectionStatus) {
      case ConnectionStatus.connected:
        statusColor = const Color(0xFF10B981);
        statusLabel = _isSimulationActive ? 'Demo' : 'Verbunden';
        break;
      case ConnectionStatus.connecting:
        statusColor = const Color(0xFFF59E0B);
        statusLabel = 'Verbinde …';
        break;
      case ConnectionStatus.error:
        statusColor = const Color(0xFFEF4444);
        statusLabel = 'Fehler';
        break;
      case ConnectionStatus.disconnected:
        statusColor = Colors.grey;
        statusLabel = 'Getrennt';
        break;
    }

    final String deviceName = _selectedDevice?.name ?? 'Kein Gerät gewählt';
    final bool connected = _connectionStatus == ConnectionStatus.connected;

    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 8, 12),
      decoration: _cardDecoration(),
      child: Row(
        children: [
          Container(
            width: 10,
            height: 10,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: statusColor,
              boxShadow: [
                BoxShadow(
                  color: statusColor.withValues(alpha: 0.6),
                  blurRadius: 8,
                ),
              ],
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  deviceName,
                  style: const TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.w700,
                    color: Colors.white,
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
                Text(
                  '$statusLabel · $_statusMessage',
                  style: TextStyle(
                    fontSize: 12,
                    color: Colors.white.withValues(alpha: 0.55),
                  ),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
          ),
          IconButton(
            icon: Icon(
              connected ? Icons.link_off_rounded : Icons.link_rounded,
              color: connected ? Colors.white70 : const Color(0xFF10B981),
            ),
            tooltip: connected ? 'Trennen' : 'Verbinden',
            onPressed: connected ? _userDisconnect : () => _connectToDevice(),
          ),
        ],
      ),
    );
  }

  Widget _buildPowerStatusTile() {
    final bool isOutage = _powerStatus == PowerStatus.outage;
    final bool isOnline = _powerStatus == PowerStatus.online;

    final Color accent = isOutage
        ? const Color(0xFFEF4444)
        : isOnline
            ? const Color(0xFF10B981)
            : const Color(0xFF64748B);
    final String title = isOutage
        ? 'Stromausfall'
        : isOnline
            ? 'Netzstrom normal'
            : 'Keine Daten';
    final String subtitle = isOutage
        ? '230 V ausgefallen – das Gerät läuft auf Akku.'
        : isOnline
            ? 'Die Netzversorgung ist aktiv und stabil.'
            : 'Verbinde dich mit dem Netzwächter, um Live-Daten zu sehen.';
    final IconData icon = isOutage
        ? Icons.power_off_rounded
        : isOnline
            ? Icons.bolt_rounded
            : Icons.sensors_off_rounded;

    return AnimatedBuilder(
      animation: _pulseAnimation,
      builder: (context, child) {
        final double pulse = isOutage ? _pulseAnimation.value : 0.0;

        return Container(
          padding: const EdgeInsets.fromLTRB(24, 28, 24, 22),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(28),
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: [
                accent.withValues(alpha: isOutage ? 0.25 + 0.2 * pulse : 0.22),
                const Color(0xFF131A2A),
              ],
            ),
            border: Border.all(
              color: accent.withValues(alpha: isOutage ? 0.5 + 0.5 * pulse : 0.45),
              width: isOutage ? 2 : 1.2,
            ),
            boxShadow: [
              if (isOutage || isOnline)
                BoxShadow(
                  color: accent.withValues(alpha: isOutage ? 0.35 * pulse : 0.12),
                  blurRadius: isOutage ? 28 * pulse : 20,
                  spreadRadius: isOutage ? 2 * pulse : 0,
                ),
            ],
          ),
          child: Column(
            children: [
              Container(
                width: 92,
                height: 92,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: accent.withValues(alpha: 0.18),
                  border: Border.all(color: accent.withValues(alpha: 0.6), width: 2),
                ),
                child: Icon(icon, size: 46, color: accent),
              ),
              const SizedBox(height: 18),
              Text(
                title,
                textAlign: TextAlign.center,
                style: TextStyle(
                  fontSize: 28,
                  fontWeight: FontWeight.w800,
                  letterSpacing: -0.5,
                  color: isOutage ? const Color(0xFFFCA5A5) : Colors.white,
                ),
              ),
              const SizedBox(height: 6),
              Text(
                subtitle,
                textAlign: TextAlign.center,
                style: TextStyle(
                  fontSize: 14,
                  height: 1.35,
                  color: Colors.white.withValues(alpha: 0.7),
                ),
              ),
              if (_lastUpdate != null) ...[
                const SizedBox(height: 16),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                  decoration: BoxDecoration(
                    color: Colors.black.withValues(alpha: 0.25),
                    borderRadius: BorderRadius.circular(20),
                  ),
                  child: Text(
                    'Letztes Signal ${DateFormat('HH:mm:ss').format(_lastUpdate!)}',
                    style: TextStyle(
                      fontSize: 12,
                      fontWeight: FontWeight.w500,
                      color: Colors.white.withValues(alpha: 0.6),
                    ),
                  ),
                ),
              ],
            ],
          ),
        );
      },
    );
  }

  Widget _buildBatteryTile() {
    final Color battColor = _batteryPercent > 60
        ? const Color(0xFF10B981)
        : _batteryPercent > 25
            ? const Color(0xFFF59E0B)
            : const Color(0xFFEF4444);
    final double progress = (_batteryPercent / 100.0).clamp(0.0, 1.0);

    return Container(
      padding: const EdgeInsets.all(18),
      decoration: _cardDecoration(),
      child: Column(
        children: [
          _cardLabel('AKKU', Icons.battery_full_rounded, battColor),
          const SizedBox(height: 14),
          SizedBox(
            width: 104,
            height: 104,
            child: CustomPaint(
              painter: _RingPainter(progress: progress, color: battColor),
              child: Center(
                child: Text(
                  '$_batteryPercent%',
                  style: const TextStyle(
                    fontSize: 26,
                    fontWeight: FontWeight.w800,
                    letterSpacing: -0.5,
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(height: 14),
          Text(
            '${_batteryVoltage.toStringAsFixed(2)} V',
            style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w700),
          ),
          const SizedBox(height: 2),
          Text(
            _batteryVoltage > 3.7 ? 'Gut geladen' : 'Entladen',
            style: TextStyle(
              fontSize: 12,
              color: Colors.white.withValues(alpha: 0.5),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildPowerSupplyTile() {
    final bool psuActive = _psuVoltage > 3.0;
    final Color psuColor = psuActive ? const Color(0xFF38BDF8) : const Color(0xFFEF4444);
    final double progress = (_psuVoltage / 5.5).clamp(0.0, 1.0);

    return Container(
      padding: const EdgeInsets.all(18),
      decoration: _cardDecoration(),
      child: Column(
        children: [
          _cardLabel(
            'NETZTEIL',
            psuActive ? Icons.power_rounded : Icons.power_off_rounded,
            psuColor,
          ),
          const SizedBox(height: 14),
          SizedBox(
            width: 104,
            height: 104,
            child: CustomPaint(
              painter: _RingPainter(progress: progress, color: psuColor),
              child: Center(
                child: Text(
                  _psuVoltage.toStringAsFixed(1),
                  style: const TextStyle(
                    fontSize: 26,
                    fontWeight: FontWeight.w800,
                    letterSpacing: -0.5,
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(height: 14),
          Text(
            '${_psuVoltage.toStringAsFixed(2)} V',
            style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w700),
          ),
          const SizedBox(height: 2),
          Text(
            psuActive ? 'Spannung stabil' : 'Keine Spannung',
            style: TextStyle(
              fontSize: 12,
              color: Colors.white.withValues(alpha: 0.5),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildDiagnosticsTile() {
    return Container(
      decoration: _cardDecoration(),
      child: Theme(
        data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
        child: ExpansionTile(
          tilePadding: const EdgeInsets.symmetric(horizontal: 18),
          childrenPadding: const EdgeInsets.fromLTRB(18, 0, 18, 16),
          leading: const Icon(Icons.terminal_rounded, size: 20, color: Colors.white54),
          title: const Text(
            'Diagnose',
            style: TextStyle(fontSize: 14, fontWeight: FontWeight.w600),
          ),
          children: [
            Align(
              alignment: Alignment.centerLeft,
              child: SelectableText(
                _lastRawLine.isEmpty
                    ? 'Noch keine Daten empfangen.'
                    : _lastRawLine,
                style: TextStyle(
                  fontSize: 12,
                  fontFamily: 'monospace',
                  color: Colors.white.withValues(alpha: 0.65),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildEventLogSection() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Row(
              children: [
                const Icon(Icons.history_rounded, size: 20, color: Colors.white70),
                const SizedBox(width: 8),
                const Text(
                  'Event-Log (Statuswechsel)',
                  style: TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                    color: Colors.white,
                  ),
                ),
                const SizedBox(width: 8),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  decoration: BoxDecoration(
                    color: Colors.white10,
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Text(
                    '${_eventLogs.length}',
                    style: const TextStyle(fontSize: 11, color: Colors.white70),
                  ),
                ),
              ],
            ),
            if (_eventLogs.isNotEmpty)
              TextButton.icon(
                icon: const Icon(Icons.delete_sweep_outlined, size: 16, color: Colors.white54),
                label: const Text(
                  'Leeren',
                  style: TextStyle(fontSize: 12, color: Colors.white54),
                ),
                onPressed: () {
                  setState(() => _eventLogs.clear());
                },
              ),
          ],
        ),
        const SizedBox(height: 10),
        if (_eventLogs.isEmpty)
          Container(
            padding: const EdgeInsets.all(24),
            decoration: BoxDecoration(
              color: const Color(0xFF131A2A),
              borderRadius: BorderRadius.circular(18),
              border: Border.all(color: const Color(0xFF1F2A40)),
            ),
            child: Center(
              child: Text(
                'Noch keine Ereignisse aufgezeichnet.',
                style: TextStyle(color: Colors.white.withValues(alpha: 0.5)),
              ),
            ),
          )
        else
          ListView.separated(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            itemCount: _eventLogs.length,
            separatorBuilder: (_, __) => const SizedBox(height: 8),
            itemBuilder: (context, index) {
              final entry = _eventLogs[index];
              return Container(
                padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
                decoration: BoxDecoration(
                  color: const Color(0xFF131A2A),
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: const Color(0xFF1F2A40)),
                ),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Container(
                      padding: const EdgeInsets.all(8),
                      decoration: BoxDecoration(
                        color: entry.color.withValues(alpha: 0.15),
                        borderRadius: BorderRadius.circular(8),
                      ),
                      child: Icon(entry.icon, color: entry.color, size: 18),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Text(
                                entry.title,
                                style: const TextStyle(
                                  fontSize: 13,
                                  fontWeight: FontWeight.bold,
                                  color: Colors.white,
                                ),
                              ),
                              Text(
                                DateFormat('HH:mm:ss').format(entry.timestamp),
                                style: TextStyle(
                                  fontSize: 11,
                                  color: Colors.white.withValues(alpha: 0.4),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 2),
                          Text(
                            entry.details,
                            style: TextStyle(
                              fontSize: 12,
                              color: Colors.white.withValues(alpha: 0.7),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              );
            },
          ),
      ],
    );
  }

  void _showDevicePickerModal() {
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: const Color(0xFF131A2A),
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
      ),
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setModalState) {
            return Padding(
              padding: const EdgeInsets.all(16.0),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceBetween,
                    children: [
                      const Text(
                        'Bluetooth-Geräte (SPP)',
                        style: TextStyle(
                          fontSize: 18,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                      IconButton(
                        icon: const Icon(Icons.refresh),
                        onPressed: () async {
                          await _loadPairedDevices();
                          setModalState(() {});
                        },
                      ),
                    ],
                  ),
                  const Divider(color: Color(0xFF1F2A40)),
                  if (_bondedDevices.isEmpty)
                    Padding(
                      padding: const EdgeInsets.symmetric(vertical: 24),
                      child: Center(
                        child: Text(
                          'Keine gekoppelten Geräte gefunden.\nBitte zuerst in den Android Bluetooth-Einstellungen koppeln.',
                          textAlign: TextAlign.center,
                          style: TextStyle(color: Colors.white.withValues(alpha: 0.6)),
                        ),
                      ),
                    )
                  else
                    Flexible(
                      child: ListView.builder(
                        shrinkWrap: true,
                        itemCount: _bondedDevices.length,
                        itemBuilder: (context, index) {
                          final device = _bondedDevices[index];
                          final isSelected =
                              _selectedDevice?.address == device.address;
                          final isLilyGo = (device.name ?? '')
                                  .contains('LilyGO') ||
                              (device.name ?? '').contains('Netzwaechter');

                          return ListTile(
                            leading: Icon(
                              Icons.bluetooth,
                              color: isLilyGo
                                  ? const Color(0xFF10B981)
                                  : Colors.white70,
                            ),
                            title: Text(
                              device.name ?? 'Unbenanntes Gerät',
                              style: TextStyle(
                                fontWeight: isSelected
                                    ? FontWeight.bold
                                    : FontWeight.normal,
                                color: isLilyGo
                                    ? const Color(0xFF6EE7B7)
                                    : Colors.white,
                              ),
                            ),
                            subtitle: Text(
                              device.address,
                              style: TextStyle(
                                fontSize: 11,
                                color: Colors.white.withValues(alpha: 0.5),
                              ),
                            ),
                            trailing: isSelected
                                ? const Icon(
                                    Icons.check_circle,
                                    color: Color(0xFF10B981),
                                  )
                                : null,
                            onTap: () {
                              setState(() {
                                _selectedDevice = device;
                              });
                              Navigator.pop(context);
                              _connectToDevice(device);
                            },
                          );
                        },
                      ),
                    ),
                ],
              ),
            );
          },
        );
      },
    );
  }
}

class _RingPainter extends CustomPainter {
  _RingPainter({required this.progress, required this.color});

  final double progress;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    const double stroke = 10;
    final Rect rect = Offset.zero & size;
    final Rect arcRect = rect.deflate(stroke / 2);

    final Paint track = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..color = Colors.white.withValues(alpha: 0.08);
    canvas.drawArc(arcRect, 0, 6.2831853, false, track);

    final Paint arc = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round
      ..color = color;
    canvas.drawArc(arcRect, -1.5707963, 6.2831853 * progress, false, arc);
  }

  @override
  bool shouldRepaint(_RingPainter old) =>
      old.progress != progress || old.color != color;
}
