import 'dart:async';

import 'package:flutter/material.dart';

/// SMS-Einstellungen des Netzwächters. Liest und schreibt die Konfiguration
/// über Bluetooth (GET / SET:KEY=WERT / TEST), gespeichert wird im Flash des ESP32.
class SmsSettingsScreen extends StatefulWidget {
  const SmsSettingsScreen({
    super.key,
    required this.sendCommand,
    required this.lines,
  });

  final Future<void> Function(String command) sendCommand;
  final Stream<String> lines;

  @override
  State<SmsSettingsScreen> createState() => _SmsSettingsScreenState();
}

class _SmsSettingsScreenState extends State<SmsSettingsScreen> {
  static const int _maxNumbers = 5;
  static const int _maxSmsLength = 160;
  static const Duration _replyTimeout = Duration(seconds: 4);

  static const Color _green = Color(0xFF10B981);
  static const Color _red = Color(0xFFEF4444);
  static const Color _card = Color(0xFF131A2A);
  static const Color _border = Color(0xFF1F2A40);

  static const List<int> _offDelayPresets = [0, 30, 60, 180, 300, 600, 1800];
  static const List<int> _onDelayPresets = [0, 10, 30, 60, 300];
  static const List<int> _lowBatPresets = [0, 10, 20, 30, 50];

  final RegExp _numberRegex = RegExp(r'^\+?[0-9]{3,15}$');

  final List<TextEditingController> _numberControllers = [];
  final TextEditingController _msgOff = TextEditingController();
  final TextEditingController _msgOn = TextEditingController();
  final TextEditingController _msgLow = TextEditingController();
  int _offDelay = 180;
  int _onDelay = 10;
  bool _restoreSms = true;
  bool _smsCommands = true;
  int _lowBat = 0;

  bool _loading = true;
  bool _saving = false;
  String? _loadError;

  @override
  void initState() {
    super.initState();
    for (final c in [_msgOff, _msgOn, _msgLow]) {
      c.addListener(() => setState(() {}));
    }
    _load();
  }

  @override
  void dispose() {
    for (final c in [..._numberControllers, _msgOff, _msgOn, _msgLow]) {
      c.dispose();
    }
    super.dispose();
  }

  // ---------------------------------------------------------------------------
  // Kommunikation
  // ---------------------------------------------------------------------------

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _loadError = null;
    });

    final Map<String, String> cfg = {};
    final completer = Completer<void>();
    final sub = widget.lines.listen((line) {
      if (line == 'CFG:END') {
        if (!completer.isCompleted) completer.complete();
      } else if (line.startsWith('CFG:')) {
        final int eq = line.indexOf('=');
        if (eq > 4) cfg[line.substring(4, eq)] = line.substring(eq + 1);
      }
    });

    try {
      await widget.sendCommand('GET');
      await completer.future.timeout(_replyTimeout);
      if (!mounted) return;
      setState(() {
        for (final c in _numberControllers) {
          c.dispose();
        }
        _numberControllers
          ..clear()
          ..addAll((cfg['NUMS'] ?? '')
              .split(',')
              .where((n) => n.trim().isNotEmpty)
              .map((n) => TextEditingController(text: n.trim())));
        if (_numberControllers.isEmpty) {
          _numberControllers.add(TextEditingController());
        }
        _msgOff.text = cfg['MSGOFF'] ?? '';
        _msgOn.text = cfg['MSGON'] ?? '';
        _msgLow.text = cfg['MSGLOW'] ?? '';
        _offDelay = int.tryParse(cfg['OFFDELAY'] ?? '') ?? 180;
        _onDelay = int.tryParse(cfg['ONDELAY'] ?? '') ?? 10;
        _restoreSms = cfg['RESTORE'] != '0';
        _smsCommands = cfg['SMSCMD'] != '0';
        _lowBat = int.tryParse(cfg['LOWBAT'] ?? '') ?? 0;
        _loading = false;
      });
    } on TimeoutException {
      if (!mounted) return;
      setState(() {
        _loading = false;
        _loadError =
            'Keine Antwort vom Netzwächter.\nIst die neue Firmware (mit SMS-Einstellungen) aufgespielt?';
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _loading = false;
        _loadError = 'Senden fehlgeschlagen: $e';
      });
    } finally {
      await sub.cancel();
    }
  }

  /// Sendet SET:KEY=WERT und wartet auf OK:KEY bzw. ERR:KEY.
  Future<bool> _set(String key, String value) async {
    final reply = widget.lines
        .firstWhere((l) => l == 'OK:$key' || l == 'ERR:$key')
        .timeout(_replyTimeout);
    try {
      await widget.sendCommand('SET:$key=${value.replaceAll(RegExp(r'[\r\n]+'), ' ').trim()}');
    } catch (_) {
      reply.ignore();
      rethrow;
    }
    return (await reply) == 'OK:$key';
  }

  List<String> get _numbers => _numberControllers
      .map((c) => c.text.replaceAll(RegExp(r'[\s\-/()]'), ''))
      .where((n) => n.isNotEmpty)
      .toList();

  Future<void> _save() async {
    final numbers = _numbers;
    final invalid = numbers.where((n) => !_numberRegex.hasMatch(n)).toList();
    if (invalid.isNotEmpty) {
      _snack('Ungültige Nummer: ${invalid.join(', ')}', _red);
      return;
    }
    if (numbers.isEmpty) {
      _snack('Mindestens einen Empfänger eintragen.', _red);
      return;
    }
    if ([_msgOff, _msgOn, _msgLow].any((c) => c.text.trim().isEmpty)) {
      _snack('SMS-Texte dürfen nicht leer sein.', _red);
      return;
    }

    setState(() => _saving = true);
    final settings = <String, String>{
      'NUMS': numbers.join(','),
      'MSGOFF': _msgOff.text,
      'MSGON': _msgOn.text,
      'MSGLOW': _msgLow.text,
      'OFFDELAY': '$_offDelay',
      'ONDELAY': '$_onDelay',
      'RESTORE': _restoreSms ? '1' : '0',
      'SMSCMD': _smsCommands ? '1' : '0',
      'LOWBAT': '$_lowBat',
    };

    final failed = <String>[];
    try {
      for (final entry in settings.entries) {
        if (!await _set(entry.key, entry.value)) failed.add(entry.key);
      }
      if (failed.isEmpty) {
        _snack('Einstellungen auf dem Netzwächter gespeichert.', _green);
      } else {
        _snack('Nicht gespeichert: ${failed.join(', ')}', _red);
      }
    } on TimeoutException {
      _snack('Keine Antwort vom Netzwächter – nicht alles gespeichert.', _red);
    } catch (e) {
      _snack('Speichern fehlgeschlagen: $e', _red);
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  Future<void> _sendTest() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        backgroundColor: _card,
        title: const Text('Test-SMS senden?'),
        content: const Text(
          'Es wird eine Test-SMS an alle gespeicherten Empfänger gesendet. '
          'Ungespeicherte Änderungen bitte vorher speichern.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Abbrechen'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Senden'),
          ),
        ],
      ),
    );
    if (confirmed != true) return;
    try {
      await widget.sendCommand('TEST');
      _snack('Test-SMS angefordert – Ergebnis erscheint im Event-Log.', _green);
    } catch (e) {
      _snack('Senden fehlgeschlagen: $e', _red);
    }
  }

  void _snack(String text, Color color) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), backgroundColor: color),
    );
  }

  // ---------------------------------------------------------------------------
  // Hilfsfunktionen
  // ---------------------------------------------------------------------------

  static String _formatDelay(int s) {
    if (s == 0) return 'Sofort';
    if (s < 60) return '$s s';
    if (s < 3600) return s % 60 == 0 ? '${s ~/ 60} min' : '${s ~/ 60} min ${s % 60} s';
    return '${s ~/ 3600} h';
  }

  /// Vorschau mit Beispielwerten, so wie der ESP32 die SMS zusammenbaut.
  static String _preview(String template, String duration) {
    String t = template
        .replaceAll('{akku}', '84')
        .replaceAll('{volt}', '3.93')
        .replaceAll('{dauer}', duration)
        .replaceAll('ä', 'ae')
        .replaceAll('ö', 'oe')
        .replaceAll('ü', 'ue')
        .replaceAll('Ä', 'Ae')
        .replaceAll('Ö', 'Oe')
        .replaceAll('Ü', 'Ue')
        .replaceAll('ß', 'ss')
        .replaceAll(RegExp(r'[^\x20-\x7E]'), '');
    return t.length > _maxSmsLength ? t.substring(0, _maxSmsLength) : t;
  }

  // ---------------------------------------------------------------------------
  // UI
  // ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('SMS-Alarm'),
        actions: [
          if (!_loading && _loadError == null)
            Padding(
              padding: const EdgeInsets.only(right: 12),
              child: FilledButton.icon(
                onPressed: _saving ? null : _save,
                icon: _saving
                    ? const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.save_rounded, size: 18),
                label: const Text('Speichern'),
              ),
            ),
        ],
      ),
      body: SafeArea(child: _buildBody()),
    );
  }

  Widget _buildBody() {
    if (_loading) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_loadError != null) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.sms_failed_outlined, size: 48, color: Colors.white38),
              const SizedBox(height: 12),
              Text(
                _loadError!,
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white.withValues(alpha: 0.7)),
              ),
              const SizedBox(height: 16),
              OutlinedButton.icon(
                onPressed: _load,
                icon: const Icon(Icons.refresh),
                label: const Text('Erneut versuchen'),
              ),
            ],
          ),
        ),
      );
    }

    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
      children: [
        _section(
          icon: Icons.contacts_rounded,
          title: 'Empfänger',
          subtitle: 'Bis zu $_maxNumbers Nummern im internationalen Format, z. B. +41791234567',
          children: [
            for (int i = 0; i < _numberControllers.length; i++)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Row(
                  children: [
                    Expanded(
                      child: TextField(
                        controller: _numberControllers[i],
                        keyboardType: TextInputType.phone,
                        decoration: _inputDecoration('Nummer ${i + 1}'),
                      ),
                    ),
                    IconButton(
                      tooltip: 'Entfernen',
                      icon: const Icon(Icons.remove_circle_outline, color: Colors.white54),
                      onPressed: _numberControllers.length <= 1
                          ? null
                          : () => setState(() {
                                _numberControllers.removeAt(i).dispose();
                              }),
                    ),
                  ],
                ),
              ),
            if (_numberControllers.length < _maxNumbers)
              Align(
                alignment: Alignment.centerLeft,
                child: TextButton.icon(
                  onPressed: () => setState(
                      () => _numberControllers.add(TextEditingController())),
                  icon: const Icon(Icons.person_add_alt_1_rounded, size: 18),
                  label: const Text('Empfänger hinzufügen'),
                ),
              ),
          ],
        ),
        _section(
          icon: Icons.power_off_rounded,
          iconColor: _red,
          title: 'Stromausfall',
          subtitle: 'Die SMS kommt erst, wenn der Strom so lange ohne Unterbrechung weg ist. '
              'Kurze Aussetzer lösen keinen Alarm aus.',
          children: [
            _label('Alarm nach'),
            _delayChips(_offDelayPresets, _offDelay, (v) => setState(() => _offDelay = v)),
            const SizedBox(height: 14),
            _messageField(_msgOff, 'Alarm-Text', _offDelay == 0 ? '0 s' : _formatDelay(_offDelay)),
          ],
        ),
        _section(
          icon: Icons.bolt_rounded,
          iconColor: _green,
          title: 'Strom wieder da',
          subtitle: 'Wird nur gesendet, wenn vorher ein Alarm verschickt wurde.',
          children: [
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              value: _restoreSms,
              onChanged: (v) => setState(() => _restoreSms = v),
              title: const Text('Entwarnungs-SMS senden'),
            ),
            if (_restoreSms) ...[
              _label('Strom muss stabil da sein für'),
              _delayChips(_onDelayPresets, _onDelay, (v) => setState(() => _onDelay = v)),
              const SizedBox(height: 14),
              _messageField(_msgOn, 'Entwarnungs-Text', '1 h 12 min'),
            ],
          ],
        ),
        _section(
          icon: Icons.battery_alert_rounded,
          iconColor: const Color(0xFFF59E0B),
          title: 'Akku-Warnung',
          subtitle: 'Zusätzliche SMS während eines Stromausfalls, wenn der USV-Akku unter die Schwelle fällt '
              '(einmal pro Ausfall).',
          children: [
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                for (final p in _withCurrent(_lowBatPresets, _lowBat))
                  ChoiceChip(
                    label: Text(p == 0 ? 'Aus' : 'unter $p %'),
                    selected: _lowBat == p,
                    onSelected: (_) => setState(() => _lowBat = p),
                  ),
              ],
            ),
            if (_lowBat > 0) ...[
              const SizedBox(height: 14),
              _messageField(_msgLow, 'Warn-Text', '2 h 5 min'),
            ],
          ],
        ),
        _section(
          icon: Icons.settings_remote_rounded,
          iconColor: const Color(0xFF38BDF8),
          title: 'Fernsteuerung per SMS',
          subtitle: 'SMS, die mit "/" beginnen, sind Befehle. Angenommen werden sie nur von den '
              'Empfänger-Nummern oben. Der Netzwächter antwortet per SMS.',
          children: [
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              value: _smsCommands,
              onChanged: (v) => setState(() => _smsCommands = v),
              title: const Text('SMS-Befehle erlauben'),
            ),
            if (_smsCommands)
              Text(
                '/status  →  aktueller Zustand\n'
                '/einstellungen  →  alle Einstellungen\n'
                '/alarm 5m  →  Alarm nach 5 Minuten\n'
                '/entwarnung an · aus · 30s\n'
                '/akku 20 · aus  →  Akku-Warnung\n'
                '/text alarm <Text>  →  SMS-Text ändern\n'
                '/nummer liste · neu <Nr> · weg <Nr>\n'
                '/pause 2h · aus  →  Alarme stummschalten\n'
                '/test  ·  /neustart  ·  /hilfe',
                style: TextStyle(
                  fontSize: 12,
                  height: 1.5,
                  fontFamily: 'monospace',
                  color: Colors.white.withValues(alpha: 0.7),
                ),
              ),
          ],
        ),
        _section(
          icon: Icons.info_outline_rounded,
          title: 'Platzhalter',
          children: [
            Text(
              '{akku}  →  Akkustand in %\n'
              '{volt}  →  Akkuspannung\n'
              '{dauer}  →  Dauer des Stromausfalls\n\n'
              'Umlaute werden automatisch umgeschrieben (ä → ae). '
              'Eine SMS hat max. $_maxSmsLength Zeichen, längere Texte werden gekürzt.',
              style: TextStyle(
                fontSize: 13,
                height: 1.4,
                color: Colors.white.withValues(alpha: 0.7),
              ),
            ),
          ],
        ),
        const SizedBox(height: 4),
        OutlinedButton.icon(
          onPressed: _sendTest,
          icon: const Icon(Icons.send_rounded),
          label: const Text('Test-SMS an gespeicherte Empfänger'),
          style: OutlinedButton.styleFrom(
            padding: const EdgeInsets.symmetric(vertical: 14),
          ),
        ),
      ],
    );
  }

  List<int> _withCurrent(List<int> presets, int current) =>
      presets.contains(current) ? presets : ([...presets, current]..sort());

  Widget _delayChips(List<int> presets, int value, ValueChanged<int> onChanged) {
    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        for (final p in _withCurrent(presets, value))
          ChoiceChip(
            label: Text(_formatDelay(p)),
            selected: value == p,
            onSelected: (_) => onChanged(p),
          ),
      ],
    );
  }

  Widget _messageField(TextEditingController controller, String label, String sampleDuration) {
    final String preview = _preview(controller.text, sampleDuration);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        TextField(
          controller: controller,
          minLines: 2,
          maxLines: 4,
          maxLength: _maxSmsLength,
          keyboardType: TextInputType.text,
          decoration: _inputDecoration(label),
        ),
        Container(
          width: double.infinity,
          padding: const EdgeInsets.all(10),
          decoration: BoxDecoration(
            color: Colors.black.withValues(alpha: 0.25),
            borderRadius: BorderRadius.circular(10),
          ),
          child: Text(
            'Vorschau (${preview.length} Zeichen): $preview',
            style: TextStyle(
              fontSize: 12,
              fontFamily: 'monospace',
              color: Colors.white.withValues(alpha: 0.6),
            ),
          ),
        ),
      ],
    );
  }

  InputDecoration _inputDecoration(String label) => InputDecoration(
        labelText: label,
        filled: true,
        fillColor: Colors.black.withValues(alpha: 0.2),
        border: OutlineInputBorder(
          borderRadius: BorderRadius.circular(12),
          borderSide: const BorderSide(color: _border),
        ),
        enabledBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(12),
          borderSide: const BorderSide(color: _border),
        ),
      );

  Widget _label(String text) => Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: Text(
          text,
          style: TextStyle(
            fontSize: 12,
            fontWeight: FontWeight.w600,
            color: Colors.white.withValues(alpha: 0.6),
          ),
        ),
      );

  Widget _section({
    required IconData icon,
    required String title,
    String? subtitle,
    Color iconColor = Colors.white70,
    required List<Widget> children,
  }) {
    return Container(
      margin: const EdgeInsets.only(bottom: 14),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: _card,
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: _border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, size: 20, color: iconColor),
              const SizedBox(width: 8),
              Text(title, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w700)),
            ],
          ),
          if (subtitle != null) ...[
            const SizedBox(height: 4),
            Text(
              subtitle,
              style: TextStyle(fontSize: 12, height: 1.35, color: Colors.white.withValues(alpha: 0.5)),
            ),
          ],
          const SizedBox(height: 12),
          ...children,
        ],
      ),
    );
  }
}
