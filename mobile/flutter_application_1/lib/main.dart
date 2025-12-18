// Import thư viện cần thiết
import 'package:flutter/material.dart';
import 'package:flutter/cupertino.dart'; 
import 'dart:convert';
import 'dart:async';

// THƯ VIỆN BỔ SUNG
import 'package:http/http.dart' as http;
import 'package:intl/intl.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({Key? key}) : super(key: key);
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'IoT Health Monitor',
      theme: ThemeData(
        primarySwatch: Colors.teal, 
        scaffoldBackgroundColor: Colors.white, 
        fontFamily: 'Inter', 
        useMaterial3: true
      ),
      home: const MyHomePage(),
      debugShowCheckedModeBanner: false,
    );
  }
}

class MyHomePage extends StatefulWidget {
  const MyHomePage({Key? key}) : super(key: key);
  @override
  State<MyHomePage> createState() => _MyHomePageState();
}

class _MyHomePageState extends State<MyHomePage> with TickerProviderStateMixin {
  
  // === CẤU HÌNH API CLOUD (RENDER) ===
  final String _renderBaseUrl = 'https://iot-health-backend.onrender.com';
  
  // === CẤU HÌNH HIVEMQ CLOUD ===
  final String _broker = '7ea4531d69e74f51b70c14213c7980e4.s1.eu.hivemq.cloud';
  final String _port = '8883'; 
  final String _mqttUser = 'esp32c3_tunglam'; 
  final String _mqttPass = 'Tunglam.03';

  // Animation Controllers
  late AnimationController _heartBeatController;
  late Animation<double> _heartScaleAnimation;
  late AnimationController _waterFloatController;
  late Animation<Offset> _waterSlideAnimation;
  late AnimationController _breathingController;
  late Animation<double> _breathingAnimation;
  late AnimationController _alertPulseController;
  late Animation<Color?> _alertColorAnimation;

  // === TRẠNG THÁI KẾT NỐI ===
  // 0: Mất kết nối Server (Đỏ) -> KHÓA NÚT
  // 1: Đã vào Server, Đang tìm ESP32 (Cam) -> KHÓA NÚT (Strict Mode)
  // 2: Thiết bị online ổn định (Xanh) -> MỞ NÚT
  int _connectionState = 0; 
  String _statusText = "Đang kết nối Server...";
  
  // Biến cờ quan trọng: Chặn tin nhắn cũ (Retained Message) trong 4s đầu
  bool _isFilteringInitialMessages = true; 

  // LOGIC KHÓA NÚT CHẶT: Chỉ khi connectionState == 2 (Màu Xanh) thì mới cho điều khiển
  bool get _canControl => _connectionState == 2;

  Timer? _deviceWatchdogTimer; 

  int? _latestHR;
  int? _latestSpO2;
  Map<String, dynamic>? _fallAlertData;
  DateTime? _fallTime;

  MqttServerClient? client;
  Timer? _fallLatchTimer;
  bool _isLoadingHistory = false;

  String _timeString = "";
  bool _showColon = true; 
  Timer? _clockTimer;
  final Map<String, Timer> _scheduledTimers = {}; 

  final String _topicFromEsp32Data = 'wearable/dev001/data';
  final String _topicFromEsp32Event = 'wearable/dev001/event';
  final String _topicToEsp32 = 'wearable/dev001/cmd';

  @override
  void initState() {
    super.initState();
    
    // Khởi tạo trạng thái ĐỎ & Bật chế độ lọc tin cũ
    _connectionState = 0;
    _statusText = "Đang kết nối Server...";
    _isFilteringInitialMessages = true;

    _initAnimations();
    _connectMqtt();
    
    _updateTime();
    _clockTimer = Timer.periodic(const Duration(seconds: 1), (Timer t) => _updateTime());

    // SAU 4 GIÂY: Tắt bộ lọc, bắt đầu chấp nhận tin nhắn thật
    Timer(const Duration(seconds: 4), () {
      if (mounted) {
        setState(() {
          _isFilteringInitialMessages = false;
          print("APP:: Đã xong giai đoạn lọc tin cũ. Sẵn sàng nhận tin mới.");
        });
      }
    });
  }

  void _initAnimations() {
    _heartBeatController = AnimationController(vsync: this, duration: const Duration(milliseconds: 1000))..repeat(); 
    _heartScaleAnimation = TweenSequence<double>([
      TweenSequenceItem(tween: Tween(begin: 1.0, end: 1.2).chain(CurveTween(curve: Curves.easeOut)), weight: 10),
      TweenSequenceItem(tween: Tween(begin: 1.2, end: 1.0).chain(CurveTween(curve: Curves.easeIn)), weight: 10),
      TweenSequenceItem(tween: Tween(begin: 1.0, end: 1.0), weight: 20),
      TweenSequenceItem(tween: Tween(begin: 1.0, end: 1.15).chain(CurveTween(curve: Curves.easeOut)), weight: 10),
      TweenSequenceItem(tween: Tween(begin: 1.15, end: 1.0).chain(CurveTween(curve: Curves.easeIn)), weight: 10),
      TweenSequenceItem(tween: Tween(begin: 1.0, end: 1.0), weight: 40),
    ]).animate(_heartBeatController);

    _waterFloatController = AnimationController(vsync: this, duration: const Duration(milliseconds: 2000))..repeat(reverse: true);
    _waterSlideAnimation = Tween<Offset>(begin: const Offset(0, 0.05), end: const Offset(0, -0.05)).animate(CurvedAnimation(parent: _waterFloatController, curve: Curves.easeInOut));

    _breathingController = AnimationController(vsync: this, duration: const Duration(milliseconds: 3000))..repeat(reverse: true);
    _breathingAnimation = Tween<double>(begin: 0.6, end: 1.0).animate(_breathingController);

    _alertPulseController = AnimationController(vsync: this, duration: const Duration(milliseconds: 500))..repeat(reverse: true);
    _alertColorAnimation = ColorTween(begin: Colors.red[50], end: Colors.red[200]).animate(_alertPulseController);
  }

  @override
  void dispose() {
    _heartBeatController.dispose(); _waterFloatController.dispose();
    _breathingController.dispose(); _alertPulseController.dispose();
    _fallLatchTimer?.cancel(); _clockTimer?.cancel();
    _deviceWatchdogTimer?.cancel();
    _scheduledTimers.forEach((key, timer) => timer.cancel());
    client?.disconnect();
    super.dispose();
  }

  void _updateTime() {
    final DateTime now = DateTime.now();
    if (mounted) setState(() { _showColon = !_showColon; _timeString = DateFormat('HH:mm:ss').format(now); });
  }

  String _formatDate(DateTime dateTime) {
    return DateFormat('EEEE, dd/MM/yyyy').format(dateTime);
  }

  // =================================================================
  // MQTT CONNECTION
  // =================================================================
  void _connectMqtt() async {
    String clientId = 'flutter_app_${DateTime.now().millisecondsSinceEpoch}';

    client = MqttServerClient.withPort(_broker, clientId, int.parse(_port));
    client!.logging(on: true);
    client!.keepAlivePeriod = 60;
    client!.secure = true; 
    client!.onBadCertificate = (dynamic cert) => true; 

    client!.onDisconnected = _onMqttDisconnected;
    client!.onConnected = _onMqttConnected;
    client!.autoReconnect = true;

    // BƯỚC 1: ĐANG KẾT NỐI SERVER (ĐỎ) -> NÚT KHÓA
    setState(() {
      _connectionState = 0;
      _statusText = "Đang kết nối Server...";
    });

    try {
      await client!.connect(_mqttUser, _mqttPass);
    } catch (e) {
      client!.disconnect();
      if (mounted) setState(() {
        _connectionState = 0;
        _statusText = "Lỗi kết nối Server!";
      });
    }

    if (client!.connectionStatus!.state == MqttConnectionState.connected) {
      // BƯỚC 2: VÀO ĐƯỢC SERVER, TÌM THIẾT BỊ (CAM) -> NÚT VẪN KHÓA
      setState(() {
        _connectionState = 1; 
        _statusText = "Đang tìm thiết bị...";
      });
      
      client!.updates!.listen((List<MqttReceivedMessage<MqttMessage?>>? c) {
        final MqttPublishMessage recMess = c![0].payload as MqttPublishMessage;
        final String payload = MqttPublishPayload.bytesToStringAsString(recMess.payload.message);
        _handleNewMessage(payload);
      });
    }
  }

  void _onMqttConnected() {
    setState(() {
      _connectionState = 1; // CAM: Vào Server rồi, đang quét -> VẪN KHÓA NÚT
      _statusText = "Đang tìm thiết bị...";
    });
    client!.subscribe(_topicFromEsp32Data, MqttQos.atLeastOnce);
    client!.subscribe(_topicFromEsp32Event, MqttQos.atLeastOnce);
  }

  void _onMqttDisconnected() {
    if (mounted) {
      setState(() {
        _connectionState = 0; // ĐỎ: Mất mạng -> Khóa nút
        _statusText = "Mất kết nối Server!";
      });
    }
  }

  void _handleNewMessage(String payload) {
    // === KHẮC PHỤC LỖI "BÁO NGÃ ẢO" ===
    // Nếu đang trong 4s đầu tiên, BỎ QUA mọi tin nhắn (vì đó là tin nhắn cũ lưu trên server)
    if (_isFilteringInitialMessages) {
       print("APP:: Bỏ qua tin nhắn cũ (Retained Message).");
       return; 
    }

    // Đã nhận tin mới thực sự -> Reset bộ đếm thời gian (Watchdog)
    _resetDeviceWatchdog();

    // CHỈ KHI NHẬN ĐƯỢC TIN NHẮN THỰC SỰ MỚI CHUYỂN SANG MÀU XANH (MỞ KHÓA NÚT)
    if (_connectionState != 2) {
      setState(() {
        _connectionState = 2; // XANH -> MỞ NÚT
        _statusText = "Đã kết nối với thiết bị đeo";
      });
    }

    try {
      final data = json.decode(payload) as Map<String, dynamic>;
      final String messageType = data['type'] ?? 'unknown';

      if (messageType == 'fall') {
        _fallLatchTimer?.cancel();
        _fallLatchTimer = Timer(const Duration(seconds: 12), () {
          if (mounted) setState(() { _fallAlertData = null; _fallTime = null; });
        });
        if (mounted) setState(() { _fallAlertData = data; _fallTime = DateTime.now(); });
      } else if (data.containsKey('hr') || data.containsKey('spo2')) {
        if (mounted) setState(() {
          if (data.containsKey('hr')) _latestHR = (data['hr'] as num?)?.toInt();
          if (data.containsKey('spo2')) _latestSpO2 = (data['spo2'] as num?)?.toInt();
        });
      }
    } catch (e) {
      print("JSON Error: $e");
    }
  }

  // Watchdog 120 giây (2 phút) - Nếu quá lâu không thấy tin nhắn mới cảnh báo
  void _resetDeviceWatchdog() {
    _deviceWatchdogTimer?.cancel();
    _deviceWatchdogTimer = Timer(const Duration(seconds: 120), () {
      if (mounted && _connectionState == 2) {
        setState(() {
          _connectionState = 1; // Về CAM -> KHÓA NÚT LẠI
          _statusText = "Mất tín hiệu thiết bị...";
        });
      }
    });
  }

  void _publishCommand(String command, int beeps) {
    // KIỂM TRA CHẶT: Chỉ gửi khi _canControl == true (tức là State == 2 - XANH)
    if (!_canControl || client?.connectionStatus?.state != MqttConnectionState.connected) {
       ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Chưa kết nối thiết bị! Vui lòng đợi màu Xanh...'), backgroundColor: Colors.orange));
       return;
    }

    String payload;
    String friendlyName;
    if (command == "eat") { payload = '{"btn":1}'; friendlyName = "Báo giờ ăn"; }
    else if (command == "sleep") { payload = '{"btn":2}'; friendlyName = "Báo giờ ngủ"; }
    else if (command == "gather") { payload = '{"btn":2}'; friendlyName = "Thông báo tập trung"; }
    else { payload = '{"command":"$command", "beeps":$beeps}'; friendlyName = command; }

    final builder = MqttClientPayloadBuilder();
    builder.addString(payload);
    client!.publishMessage(_topicToEsp32, MqttQos.atLeastOnce, builder.payload!);

    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Đã gửi lệnh: $friendlyName'), backgroundColor: Colors.blueAccent));
  }

  // =================================================================
  // TIMER & ALARM
  // =================================================================
  Future<void> _selectTimeAndSchedule(BuildContext context) async {
    // KIỂM TRA CHẶT: Phải XANH mới chạy
    if (!_canControl) return; 

    DateTime now = DateTime.now();
    DateTime tempPickedDate = now;

    showCupertinoModalPopup(
      context: context,
      builder: (BuildContext context) {
        return Container(
          height: 300,
          color: Colors.white,
          child: Column(
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  CupertinoButton(child: const Text('Hủy', style: TextStyle(color: Colors.red)), onPressed: () => Navigator.of(context).pop()),
                  const Text("Chọn giờ báo thức", style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                  CupertinoButton(child: const Text('Tiếp tục', style: TextStyle(color: Colors.teal)), onPressed: () {
                    Navigator.of(context).pop();
                    _showAlarmTypeDialog(tempPickedDate);
                  }),
                ],
              ),
              Expanded(
                child: CupertinoDatePicker(
                  mode: CupertinoDatePickerMode.time,
                  initialDateTime: now,
                  use24hFormat: true,
                  onDateTimeChanged: (DateTime newDateTime) => tempPickedDate = newDateTime,
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  void _showAlarmTypeDialog(DateTime pickedTime) {
    showDialog(
      context: context,
      builder: (BuildContext context) {
        return SimpleDialog(
          title: const Text('Chọn loại nhắc nhở'),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(15)),
          children: <Widget>[
            SimpleDialogOption(
              onPressed: () { _scheduleNotification(pickedTime, "eat"); Navigator.pop(context); },
              child: const Padding(padding: EdgeInsets.symmetric(vertical: 10), child: Row(children: [Icon(Icons.restaurant, color: Colors.orange), SizedBox(width: 15), Text('Giờ ăn (3 bíp)')])),
            ),
            const Divider(),
            SimpleDialogOption(
              onPressed: () { _scheduleNotification(pickedTime, "sleep"); Navigator.pop(context); },
              child: const Padding(padding: EdgeInsets.symmetric(vertical: 10), child: Row(children: [Icon(Icons.nightlight_round, color: Colors.purple), SizedBox(width: 15), Text('Giờ ngủ (5 bíp)')])),
            ),
          ],
        );
      },
    );
  }

  void _scheduleNotification(DateTime pickedTime, String type) {
    final now = DateTime.now();
    var scheduledTime = DateTime(now.year, now.month, now.day, pickedTime.hour, pickedTime.minute);
    if (scheduledTime.isBefore(now)) scheduledTime = scheduledTime.add(const Duration(days: 1));

    final duration = scheduledTime.difference(now);
    final String timeStr = "${pickedTime.hour.toString().padLeft(2, '0')}:${pickedTime.minute.toString().padLeft(2, '0')}";
    String typeName = type == "eat" ? "Ăn" : "Ngủ";
    String key = "$timeStr - $typeName"; 

    if (_scheduledTimers.containsKey(key)) _scheduledTimers[key]?.cancel();

    Timer timer = Timer(duration, () {
      if (_canControl) {
         _publishCommand(type, 0); 
      }
      setState(() => _scheduledTimers.remove(key));
    });

    setState(() => _scheduledTimers[key] = timer);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Đã hẹn giờ $timeStr')));
  }

  void _cancelAlarm(String key) {
    if (_scheduledTimers.containsKey(key)) {
      _scheduledTimers[key]?.cancel();
      setState(() => _scheduledTimers.remove(key));
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Đã hủy báo thức')));
    }
  }

  // =================================================================
  // API & HISTORY (KHÔNG LIÊN QUAN ĐẾN MQTT - LẤY TỪ RENDER)
  // =================================================================
  Future<void> _fetchAllHistory() async {
    if (_isLoadingHistory) return;
    setState(() => _isLoadingHistory = true);
    List<dynamic> combinedHistory = [];
    
    try {
      final responses = await Future.wait([
        http.get(Uri.parse('$_renderBaseUrl/get_history')).timeout(const Duration(seconds: 10)),
        http.get(Uri.parse('$_renderBaseUrl/get_falls')).timeout(const Duration(seconds: 10)),
      ]);

      if (responses[0].statusCode == 200) {
        List<dynamic> vitals = json.decode(responses[0].body);
        for (var item in vitals) item['dataType'] = 'vital';
        combinedHistory.addAll(vitals);
      }
      if (responses[1].statusCode == 200) {
        List<dynamic> falls = json.decode(responses[1].body);
        for (var item in falls) item['dataType'] = 'fall';
        combinedHistory.addAll(falls);
      }
      combinedHistory.sort((a, b) => DateTime.parse(b['_time']).compareTo(DateTime.parse(a['_time'])));
      if (mounted) _showCombinedHistoryModal(combinedHistory);
    } catch (e) {
      if (mounted) _showErrorSnackbar('Lỗi kết nối API: $e');
    }
    setState(() => _isLoadingHistory = false);
  }

  void _showCombinedHistoryModal(List<dynamic> data) {
    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (context) => DraggableScrollableSheet(
        expand: false, initialChildSize: 0.7, maxChildSize: 0.9,
        builder: (context, scrollController) => Container(
          padding: const EdgeInsets.all(16),
          child: Column(
            children: [
              Container(width: 40, height: 5, decoration: BoxDecoration(color: Colors.grey[300], borderRadius: BorderRadius.circular(10))),
              const SizedBox(height: 16),
              Text('Nhật Ký Hoạt Động', style: Theme.of(context).textTheme.headlineSmall),
              const SizedBox(height: 10),
              Expanded(
                child: data.isEmpty
                    ? const Center(child: Text('Chưa có dữ liệu.'))
                    : ListView.builder(
                        controller: scrollController,
                        itemCount: data.length,
                        itemBuilder: (context, index) {
                          final item = data[index] as Map<String, dynamic>;
                          final formattedTime = DateFormat('HH:mm - dd/MM/yy').format(DateTime.parse(item['_time']).toLocal());
                          final String type = item['dataType'] ?? 'vital';
                          if (type == 'fall') {
                             return Card(color: Colors.red[50], child: ListTile(leading: const Icon(Icons.warning_amber_rounded, color: Colors.red, size: 32), title: Text(formattedTime, style: const TextStyle(fontWeight: FontWeight.bold, color: Colors.red)), subtitle: Text('PHÁT HIỆN NGÃ (Cấp độ ${item['severity'] ?? 2})')));
                          } else {
                             return Card(child: ListTile(leading: const Icon(Icons.favorite_border, color: Colors.teal), title: Text(formattedTime), subtitle: Text('HR: ${item['hr'] ?? '--'} bpm  |  SpO2: ${item['spo2'] ?? '--'}%')));
                          }
                        },
                      ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  void _showErrorSnackbar(String message) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message), backgroundColor: Colors.red));
  }

  // =================================================================
  // GIAO DIỆN CHÍNH
  // =================================================================
  @override
  Widget build(BuildContext context) {
    final bool isFallDetected = _fallAlertData != null;
    
    // MÀU SẮC TRẠNG THÁI:
    Color statusColor = _connectionState == 2 ? Colors.green : (_connectionState == 1 ? Colors.orange[800]! : Colors.red);
    Color statusBgColor = _connectionState == 2 ? Colors.green[50]! : (_connectionState == 1 ? Colors.orange[50]! : Colors.red[50]!);
    IconData statusIcon = _connectionState == 2 ? Icons.link : (_connectionState == 1 ? Icons.sync : Icons.link_off);

    return Scaffold(
      extendBodyBehindAppBar: true, 
      appBar: AppBar(
        title: const Text('IoT Sức Khỏe'),
        backgroundColor: Colors.transparent, 
        elevation: 0,
        centerTitle: true,
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 16.0),
            child: Icon(Icons.wifi, color: statusColor),
          )
        ],
      ),
      body: Container(
        decoration: BoxDecoration(
          image: DecorationImage(image: const AssetImage('assets/nen_app.png'), fit: BoxFit.cover, colorFilter: ColorFilter.mode(Colors.white.withOpacity(0.85), BlendMode.lighten)),
        ),
        child: SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(20.0),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                // 1. CLOCK & STATUS
                Column(
                  children: [
                    Text(_timeString, style: const TextStyle(fontSize: 48, fontWeight: FontWeight.bold, color: Colors.teal)),
                    Text(_formatDate(DateTime.now()), style: TextStyle(fontSize: 16, color: Colors.grey[700])),
                    const SizedBox(height: 8),
                    
                    Container(
                      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                      decoration: BoxDecoration(
                        color: statusBgColor,
                        borderRadius: BorderRadius.circular(30),
                        border: Border.all(color: statusColor, width: 1.5),
                      ),
                      child: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          _connectionState == 1
                              ? SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: statusColor))
                              : Icon(statusIcon, color: statusColor, size: 20),
                          const SizedBox(width: 8),
                          Text(
                            _statusText,
                            style: TextStyle(fontSize: 14, fontWeight: FontWeight.bold, color: statusColor),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 20),

                // 2. MAIN CONTENT
                Expanded(
                  child: SingleChildScrollView(
                    child: Column(
                      children: [
                        _buildAnimatedStatusCard(isFallDetected),
                        const SizedBox(height: 20),
                        
                        if (_scheduledTimers.isNotEmpty) ...[
                          const Align(alignment: Alignment.centerLeft, child: Text("Báo thức sắp tới:", style: TextStyle(fontWeight: FontWeight.bold))),
                          ..._scheduledTimers.keys.map((key) => Card(color: Colors.teal[50], child: ListTile(leading: const Icon(Icons.alarm, color: Colors.teal), title: Text(key), trailing: IconButton(icon: const Icon(Icons.delete_outline, color: Colors.red), onPressed: () => _cancelAlarm(key))))),
                          const SizedBox(height: 20),
                        ],

                        Row(
                          children: [
                            Expanded(
                              child: ElevatedButton.icon(
                                // LOGIC KHÓA NÚT CHẶT: State == 2 (Xanh) mới mở
                                onPressed: _canControl ? () => _selectTimeAndSchedule(context) : null,
                                
                                icon: const Icon(Icons.alarm_add),
                                label: const Text("Hẹn Giờ"),
                                style: ElevatedButton.styleFrom(
                                  padding: const EdgeInsets.symmetric(vertical: 15),
                                  backgroundColor: Colors.white,
                                  foregroundColor: Colors.teal,
                                  elevation: 2,
                                  disabledBackgroundColor: Colors.grey[200],
                                  disabledForegroundColor: Colors.grey[500],
                                ),
                              ),
                            ),
                            const SizedBox(width: 15),
                            Expanded(
                              child: ElevatedButton.icon(
                                onPressed: _isLoadingHistory ? null : _fetchAllHistory,
                                icon: const Icon(Icons.history),
                                label: Text(_isLoadingHistory ? "Đang tải..." : "Xem Lịch Sử"),
                                style: ElevatedButton.styleFrom(padding: const EdgeInsets.symmetric(vertical: 15), backgroundColor: Colors.white, foregroundColor: Colors.teal, elevation: 2),
                              ),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),
                ),

                // 3. NÚT TẬP TRUNG
                const SizedBox(height: 20),
                SizedBox(
                  width: double.infinity,
                  height: 60,
                  child: ElevatedButton.icon(
                    // LOGIC KHÓA NÚT CHẶT: State == 2 (Xanh) mới mở
                    onPressed: _canControl ? () => _publishCommand("gather", 0) : null,
                    
                    icon: const Icon(Icons.campaign, size: 30), 
                    label: const Text("Tập trung", style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.orange[800], 
                      foregroundColor: Colors.white,
                      elevation: 5,
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(15)),
                      disabledBackgroundColor: Colors.grey[300],
                      disabledForegroundColor: Colors.grey[600],
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildAnimatedStatusCard(bool isFall) {
    return AnimatedBuilder(
      animation: _alertPulseController,
      builder: (context, child) {
        return Container(
          padding: const EdgeInsets.all(20),
          decoration: BoxDecoration(
            color: isFall ? _alertColorAnimation.value : Colors.green[50], 
            borderRadius: BorderRadius.circular(20),
            border: Border.all(color: isFall ? Colors.red : Colors.green.withOpacity(_breathingAnimation.value), width: isFall ? 3 : 2),
            boxShadow: [BoxShadow(color: isFall ? Colors.red.withOpacity(0.3) : Colors.green.withOpacity(0.1), blurRadius: 10 * _breathingAnimation.value, offset: const Offset(0, 5))],
          ),
          child: Column(
            children: [
              Text(isFall ? "CẢNH BÁO NGÃ!" : "BÌNH THƯỜNG", style: TextStyle(fontSize: 28, fontWeight: FontWeight.bold, color: isFall ? Colors.red : Colors.green)),
              if (isFall) ...[
                const SizedBox(height: 5),
                Text("Mức độ: ${_fallAlertData?['severity'] ?? '--'}", style: const TextStyle(fontSize: 18, color: Colors.red, fontWeight: FontWeight.bold)),
                Text("Lúc: ${_fallTime != null ? DateFormat('HH:mm:ss').format(_fallTime!) : '--'}", style: const TextStyle(color: Colors.grey)),
                const SizedBox(height: 15),
                ElevatedButton.icon(icon: const Icon(Icons.notifications_off), label: const Text("TẮT BÁO ĐỘNG"), onPressed: () { setState(() { _fallAlertData = null; _fallLatchTimer?.cancel(); }); }, style: ElevatedButton.styleFrom(backgroundColor: Colors.red, foregroundColor: Colors.white))
              ] else ...[
                const SizedBox(height: 20),
                Row(mainAxisAlignment: MainAxisAlignment.spaceAround, children: [_buildAnimatedHeartSensor(), _buildAnimatedWaterSensor()]),
              ]
            ],
          ),
        );
      },
    );
  }

  Widget _buildAnimatedHeartSensor() {
    String valStr = _latestHR?.toString() ?? "--";
    return Column(children: [ScaleTransition(scale: _heartScaleAnimation, child: const Icon(Icons.favorite, color: Colors.red, size: 38)), const SizedBox(height: 8), Text("Nhịp tim", style: TextStyle(color: Colors.grey[600], fontSize: 14)), Text("$valStr bpm", style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold))]);
  }

  Widget _buildAnimatedWaterSensor() {
    String valStr = _latestSpO2?.toString() ?? "--";
    return Column(children: [SlideTransition(position: _waterSlideAnimation, child: const Icon(Icons.water_drop, color: Colors.blue, size: 38)), const SizedBox(height: 8), Text("SpO2", style: TextStyle(color: Colors.grey[600], fontSize: 14)), Text("$valStr %", style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold))]);
  }
}