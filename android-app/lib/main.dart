import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_bluetooth_serial/flutter_bluetooth_serial.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:intl/intl.dart';

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
        scaffoldBackgroundColor: const Color(0xFF0B0F19),
        primaryColor: const Color(0xFF10B981),
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFF10B981),
          secondary: Color(0xFF38BDF8),
          surface: Color(0xFF161F30),
          error: Color(0xFFEF4444),
        ),
        cardTheme: const CardThemeData(
          color: Color(0xFF161F30),
          elevation: 4,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.all(Radius.circular(16)),
            side: BorderSide(color: Color(0xFF223049), width: 1),
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
    r'Netz:\s*\[(.*?)\]\s*\|\s*Netzteil:\s*([0-9.]+)\s*V\s*\|\s*Akku:\s*([0-9]+)\s*%\s*\(([0-9.]+)\s*V\)',
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

  // Event Log
  final List<StatusLogEntry> _eventLogs = [];

  // Animation controller for pulsing red outage tile
  late AnimationController _pulseController;
  late Animation<double> _pulseAnimation;

  // Simulation timer for testing
  Timer? _simulationTimer;
  bool _isSimulationActive = false;

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
    _loadPairedDevices();

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

  Future<void> _loadPairedDevices() async {
    try {
      final List<BluetoothDevice> devices =
          await FlutterBluetoothSerial.instance.getBondedDevices();
      setState(() {
        _bondedDevices = devices;
        // Auto-select "LilyGO-Netzwaechter" if present
        for (final device in devices) {
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

  Future<void> _connectToDevice([BluetoothDevice? deviceToConnect]) async {
    final targetDevice = deviceToConnect ?? _selectedDevice;
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
    setState(() {
      _connectionStatus = ConnectionStatus.disconnected;
      _statusMessage = msg;
      _powerStatus = PowerStatus.unknown;
    });
    _pulseController.stop();

    _addLog(
      title: 'Verbindung getrennt',
      details: msg,
      icon: Icons.bluetooth_disabled,
      color: Colors.grey,
    );
  }

  // ---------------------------------------------------------------------------
  // Regex Parsing & Status Logic
  // ---------------------------------------------------------------------------

  void _parseSerialLine(String line) {
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
      _parseSerialLine('Netz: [ONLINE] | Netzteil: 5.08V | Akku: 85% (3.95V)');

      int tick = 0;
      _simulationTimer = Timer.periodic(const Duration(seconds: 4), (timer) {
        tick++;
        if (tick % 4 == 2) {
          // Simulate outage
          _parseSerialLine('Netz: [STROMAUSFALL] | Netzteil: 0.12V | Akku: 84% (3.93V)');
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
        title: Row(
          children: [
            Container(
              padding: const EdgeInsets.all(6),
              decoration: BoxDecoration(
                color: const Color(0xFF10B981).withValues(alpha: 0.2),
                borderRadius: BorderRadius.circular(8),
              ),
              child: const Icon(
                Icons.security_rounded,
                color: Color(0xFF10B981),
                size: 22,
              ),
            ),
            const SizedBox(width: 12),
            const Text(
              'LilyGO-Netzwächter',
              style: TextStyle(fontWeight: FontWeight.bold),
            ),
          ],
        ),
        actions: [
          IconButton(
            tooltip: _isSimulationActive ? 'Demo beenden' : 'Demo-Modus starten',
            icon: Icon(
              _isSimulationActive ? Icons.pause_circle : Icons.play_circle_fill,
              color: _isSimulationActive ? const Color(0xFFF59E0B) : Colors.white60,
            ),
            onPressed: _toggleSimulation,
          ),
          IconButton(
            tooltip: 'Bluetooth-Geräte verwalten',
            icon: const Icon(Icons.bluetooth),
            onPressed: _showDevicePickerModal,
          ),
        ],
      ),
      body: SafeArea(
        child: RefreshIndicator(
          onRefresh: _loadPairedDevices,
          color: const Color(0xFF10B981),
          child: SingleChildScrollView(
            physics: const AlwaysScrollableScrollPhysics(),
            padding: const EdgeInsets.all(16.0),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _buildConnectionBar(),
                const SizedBox(height: 16),
                _buildPowerStatusTile(),
                const SizedBox(height: 16),
                Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Expanded(child: _buildBatteryTile()),
                    const SizedBox(width: 12),
                    Expanded(child: _buildPowerSupplyTile()),
                  ],
                ),
                const SizedBox(height: 20),
                _buildEventLogSection(),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildConnectionBar() {
    Color statusColor;
    String statusLabel;
    IconData statusIcon;

    switch (_connectionStatus) {
      case ConnectionStatus.connected:
        statusColor = const Color(0xFF10B981);
        statusLabel = _isSimulationActive ? 'DEMO-MODUS AKTIV' : 'VERBUNDEN';
        statusIcon = Icons.bluetooth_connected;
        break;
      case ConnectionStatus.connecting:
        statusColor = const Color(0xFFF59E0B);
        statusLabel = 'VERBINDE...';
        statusIcon = Icons.bluetooth_searching;
        break;
      case ConnectionStatus.error:
        statusColor = const Color(0xFFEF4444);
        statusLabel = 'FEHLER';
        statusIcon = Icons.error_outline;
        break;
      case ConnectionStatus.disconnected:
        statusColor = Colors.grey;
        statusLabel = 'GETRENNT';
        statusIcon = Icons.bluetooth_disabled;
        break;
    }

    final String deviceName = _selectedDevice?.name ?? 'Kein Gerät gewählt';

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: const Color(0xFF161F30),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: const Color(0xFF223049)),
      ),
      child: Row(
        children: [
          Icon(statusIcon, color: statusColor, size: 20),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  deviceName,
                  style: const TextStyle(
                    fontSize: 14,
                    fontWeight: FontWeight.bold,
                    color: Colors.white,
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
                Text(
                  _statusMessage,
                  style: TextStyle(
                    fontSize: 12,
                    color: Colors.white.withValues(alpha: 0.6),
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
              ],
            ),
          ),
          const SizedBox(width: 8),
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            decoration: BoxDecoration(
              color: statusColor.withValues(alpha: 0.15),
              borderRadius: BorderRadius.circular(6),
              border: Border.all(color: statusColor.withValues(alpha: 0.4)),
            ),
            child: Text(
              statusLabel,
              style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.bold,
                color: statusColor,
                letterSpacing: 0.5,
              ),
            ),
          ),
          const SizedBox(width: 8),
          if (_connectionStatus == ConnectionStatus.connected)
            IconButton(
              icon: const Icon(Icons.link_off, color: Colors.white70, size: 20),
              tooltip: 'Trennen',
              onPressed: _disconnect,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(),
            )
          else
            IconButton(
              icon: const Icon(Icons.link, color: Color(0xFF10B981), size: 20),
              tooltip: 'Verbinden',
              onPressed: () => _connectToDevice(),
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(),
            ),
        ],
      ),
    );
  }

  Widget _buildPowerStatusTile() {
    final bool isOutage = _powerStatus == PowerStatus.outage;
    final bool isOnline = _powerStatus == PowerStatus.online;

    return AnimatedBuilder(
      animation: _pulseAnimation,
      builder: (context, child) {
        final double pulseVal = isOutage ? _pulseAnimation.value : 0.0;

        Color borderColor;
        Color backgroundColor;
        List<BoxShadow> shadows;

        if (isOutage) {
          borderColor = Color.lerp(
            const Color(0xFFB91C1C),
            const Color(0xFFEF4444),
            pulseVal,
          )!;
          backgroundColor = Color.lerp(
            const Color(0xFF450A0A),
            const Color(0xFF7F1D1D),
            pulseVal * 0.4,
          )!;
          shadows = [
            BoxShadow(
              color: const Color(0xFFEF4444).withValues(alpha: 0.35 * pulseVal),
              blurRadius: 20 * pulseVal,
              spreadRadius: 2 * pulseVal,
            ),
          ];
        } else if (isOnline) {
          borderColor = const Color(0xFF10B981);
          backgroundColor = const Color(0xFF064E3B).withValues(alpha: 0.35);
          shadows = [
            BoxShadow(
              color: const Color(0xFF10B981).withValues(alpha: 0.15),
              blurRadius: 12,
              spreadRadius: 1,
            ),
          ];
        } else {
          borderColor = const Color(0xFF334155);
          backgroundColor = const Color(0xFF161F30);
          shadows = [];
        }

        return Container(
          padding: const EdgeInsets.all(22),
          decoration: BoxDecoration(
            color: backgroundColor,
            borderRadius: BorderRadius.circular(20),
            border: Border.all(color: borderColor, width: isOutage ? 2.5 : 1.5),
            boxShadow: shadows,
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Row(
                    children: [
                      Container(
                        padding: const EdgeInsets.all(12),
                        decoration: BoxDecoration(
                          color: isOutage
                              ? const Color(0xFFEF4444).withValues(alpha: 0.2)
                              : isOnline
                                  ? const Color(0xFF10B981).withValues(alpha: 0.2)
                                  : Colors.white10,
                          shape: BoxShape.circle,
                        ),
                        child: Icon(
                          isOutage
                              ? Icons.warning_amber_rounded
                              : isOnline
                                  ? Icons.bolt_rounded
                                  : Icons.help_outline_rounded,
                          size: 32,
                          color: isOutage
                              ? const Color(0xFFEF4444)
                              : isOnline
                                  ? const Color(0xFF10B981)
                                  : Colors.white54,
                        ),
                      ),
                      const SizedBox(width: 14),
                      Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            'NETZVERSORGUNG',
                            style: TextStyle(
                              fontSize: 12,
                              fontWeight: FontWeight.w600,
                              letterSpacing: 1.2,
                              color: Colors.white.withValues(alpha: 0.6),
                            ),
                          ),
                          const SizedBox(height: 2),
                          Text(
                            isOutage
                                ? 'STROMAUSFALL AKTIV'
                                : isOnline
                                    ? 'NETZSTROM NORMAL'
                                    : 'STATUS UNBEKANNT',
                            style: TextStyle(
                              fontSize: 22,
                              fontWeight: FontWeight.w900,
                              letterSpacing: 0.5,
                              color: isOutage
                                  ? const Color(0xFFFCA5A5)
                                  : isOnline
                                      ? const Color(0xFF6EE7B7)
                                      : Colors.white70,
                            ),
                          ),
                        ],
                      ),
                    ],
                  ),
                  Container(
                    padding:
                        const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                    decoration: BoxDecoration(
                      color: isOutage
                          ? const Color(0xFFEF4444).withValues(alpha: 0.25)
                          : isOnline
                              ? const Color(0xFF10B981).withValues(alpha: 0.25)
                              : Colors.white10,
                      borderRadius: BorderRadius.circular(8),
                      border: Border.all(
                        color: isOutage
                            ? const Color(0xFFEF4444)
                            : isOnline
                                ? const Color(0xFF10B981)
                                : Colors.white24,
                      ),
                    ),
                    child: Text(
                      isOutage
                          ? 'ALARM'
                          : isOnline
                              ? 'ONLINE'
                              : 'KEINE DATEN',
                      style: TextStyle(
                        fontSize: 12,
                        fontWeight: FontWeight.bold,
                        color: isOutage
                            ? const Color(0xFFEF4444)
                            : isOnline
                                ? const Color(0xFF10B981)
                                : Colors.white60,
                      ),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 16),
              Container(
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: Colors.black.withValues(alpha: 0.25),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Row(
                  children: [
                    Icon(
                      isOutage ? Icons.crisis_alert : Icons.info_outline,
                      size: 16,
                      color: isOutage ? const Color(0xFFEF4444) : Colors.white54,
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(
                        isOutage
                            ? 'Achtung: 230V Primärstrom ausgefallen! ESP32 läuft auf Akku.'
                            : isOnline
                                ? 'Primäre 230V-Netzteilversorgung aktiv und stabil.'
                                : 'Verbinde mit LilyGO-Netzwächter, um Live-Daten zu empfangen.',
                        style: TextStyle(
                          fontSize: 12,
                          color: isOutage
                              ? const Color(0xFFFCA5A5)
                              : Colors.white.withValues(alpha: 0.8),
                        ),
                      ),
                    ),
                  ],
                ),
              ),
              if (_lastUpdate != null) ...[
                const SizedBox(height: 8),
                Align(
                  alignment: Alignment.centerRight,
                  child: Text(
                    'Letztes Signal: ${DateFormat('HH:mm:ss').format(_lastUpdate!)} Uhr',
                    style: TextStyle(
                      fontSize: 11,
                      color: Colors.white.withValues(alpha: 0.4),
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
    Color battColor;
    IconData battIcon;

    if (_batteryPercent > 60) {
      battColor = const Color(0xFF10B981);
      battIcon = Icons.battery_full_rounded;
    } else if (_batteryPercent > 25) {
      battColor = const Color(0xFFF59E0B);
      battIcon = Icons.battery_5_bar_rounded;
    } else {
      battColor = const Color(0xFFEF4444);
      battIcon = Icons.battery_alert_rounded;
    }

    final double progress = (_batteryPercent / 100.0).clamp(0.0, 1.0);

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF161F30),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFF223049)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                'AKKU',
                style: TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 1.1,
                  color: Colors.white.withValues(alpha: 0.6),
                ),
              ),
              Icon(battIcon, color: battColor, size: 22),
            ],
          ),
          const SizedBox(height: 10),
          Text(
            '$_batteryPercent %',
            style: TextStyle(
              fontSize: 28,
              fontWeight: FontWeight.w900,
              color: battColor,
            ),
          ),
          const SizedBox(height: 6),
          ClipRRect(
            borderRadius: BorderRadius.circular(6),
            child: LinearProgressIndicator(
              value: progress,
              minHeight: 8,
              backgroundColor: const Color(0xFF0F172A),
              valueColor: AlwaysStoppedAnimation<Color>(battColor),
            ),
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              const Icon(Icons.speed, size: 14, color: Colors.white54),
              const SizedBox(width: 4),
              Text(
                '${_batteryVoltage.toStringAsFixed(2)} V',
                style: const TextStyle(
                  fontSize: 13,
                  fontWeight: FontWeight.w600,
                  color: Colors.white,
                ),
              ),
              const Spacer(),
              Text(
                _batteryVoltage > 3.7 ? 'Geladen' : 'Entladen',
                style: TextStyle(
                  fontSize: 11,
                  color: Colors.white.withValues(alpha: 0.5),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildPowerSupplyTile() {
    final bool psuActive = _psuVoltage > 4.5;
    final Color psuColor = psuActive ? const Color(0xFF38BDF8) : const Color(0xFFEF4444);

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF161F30),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFF223049)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                'NETZTEIL (JST 5V)',
                style: TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 1.1,
                  color: Colors.white.withValues(alpha: 0.6),
                ),
              ),
              Icon(
                psuActive ? Icons.power_rounded : Icons.power_off_rounded,
                color: psuColor,
                size: 22,
              ),
            ],
          ),
          const SizedBox(height: 10),
          Text(
            '${_psuVoltage.toStringAsFixed(2)} V',
            style: TextStyle(
              fontSize: 28,
              fontWeight: FontWeight.w900,
              color: psuColor,
            ),
          ),
          const SizedBox(height: 6),
          ClipRRect(
            borderRadius: BorderRadius.circular(6),
            child: LinearProgressIndicator(
              value: (_psuVoltage / 5.5).clamp(0.0, 1.0),
              minHeight: 8,
              backgroundColor: const Color(0xFF0F172A),
              valueColor: AlwaysStoppedAnimation<Color>(psuColor),
            ),
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              Container(
                width: 8,
                height: 8,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: psuColor,
                ),
              ),
              const SizedBox(width: 6),
              Text(
                psuActive ? 'Spannung stabil' : 'Keine Eingangsspannung',
                style: TextStyle(
                  fontSize: 11,
                  color: Colors.white.withValues(alpha: 0.6),
                ),
              ),
            ],
          ),
        ],
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
              color: const Color(0xFF161F30),
              borderRadius: BorderRadius.circular(14),
              border: Border.all(color: const Color(0xFF223049)),
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
                  color: const Color(0xFF161F30),
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: const Color(0xFF223049)),
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
      backgroundColor: const Color(0xFF161F30),
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
                  const Divider(color: Color(0xFF223049)),
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
