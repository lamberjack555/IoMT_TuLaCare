// --- KHAI BÁO THƯ VIỆN ---
const express = require('express');
const mongoose = require('mongoose');
const mqtt = require('mqtt');
const cors = require('cors');

// --- CẤU HÌNH (BẠN ĐIỀN THÔNG TIN VÀO ĐÂY) ---

// 1. MongoDB Connection String (Lấy từ bước trước)
// Thay <password> bằng mật khẩu bạn đã tạo (Tunglam.03)
const MONGO_URI = "mongodb+srv://esp32c3_db:Tunglam.03@cluster0.fhyqz3t.mongodb.net/?appName=Cluster0"; 

// 2. HiveMQ Cloud (Copy y nguyên từ code Flutter/Arduino sang)
const MQTT_HOST = "7ea4531d69e74f51b70c14213c7980e4.s1.eu.hivemq.cloud"; 
const MQTT_PORT = 8883;
const MQTT_USER = "esp32c3_tunglam";
const MQTT_PASS = "Tunglam.03";

// ---------------------------------------------------------

const app = express();
app.use(cors()); // Cho phép App gọi API từ mọi nơi
app.use(express.json());

// --- KẾT NỐI MONGODB ---
mongoose.connect(MONGO_URI)
  .then(() => console.log('✅ Đã kết nối MongoDB Atlas thành công!'))
  .catch(err => console.error('❌ Lỗi kết nối MongoDB:', err));

// Định nghĩa cấu trúc dữ liệu (Schema)
const VitalSchema = new mongoose.Schema({
  device: String,
  hr: Number,
  spo2: Number,
  timestamp: { type: Date, default: Date.now }
});
const EventSchema = new mongoose.Schema({
  device: String,
  type: String,
  severity: Number,
  alert: String,
  timestamp: { type: Date, default: Date.now }
});

const Vital = mongoose.model('Vital', VitalSchema);
const Event = mongoose.model('Event', EventSchema);

// --- KẾT NỐI MQTT (HIVEMQ) ---
const client = mqtt.connect(`mqtts://${MQTT_HOST}:${MQTT_PORT}`, {
  username: MQTT_USER,
  password: MQTT_PASS,
  rejectUnauthorized: false // Để đơn giản hóa kết nối SSL
});

client.on('connect', () => {
  console.log('✅ Đã kết nối HiveMQ Cloud!');
  client.subscribe('wearable/+/data');
  client.subscribe('wearable/+/event');
});

client.on('message', async (topic, message) => {
  try {
    const payload = JSON.parse(message.toString());
    const deviceId = topic.split('/')[1]; // Lấy dev001 từ topic

    if (topic.includes('/data')) {
      // Lưu dữ liệu HR/SpO2
      if (payload.hr && payload.spo2) {
        const newData = new Vital({
          device: deviceId,
          hr: payload.hr,
          spo2: payload.spo2
        });
        await newData.save();
        console.log(`[DATA] Đã lưu: HR=${payload.hr}, SpO2=${payload.spo2}`);
      }
    } else if (topic.includes('/event')) {
      // Lưu sự kiện Ngã
      if (payload.type === 'fall') {
        const newEvent = new Event({
          device: deviceId,
          type: 'fall',
          severity: payload.severity || 2,
          alert: `CẢNH BÁO NGÃ (Mức ${payload.severity || 2})`
        });
        await newEvent.save();
        console.log(`[EVENT] ⚠️ Đã lưu cảnh báo NGÃ!`);
      }
    }
  } catch (e) {
    console.error('Lỗi xử lý tin nhắn MQTT:', e);
  }
});

// --- API CHO FLUTTER APP GỌI ---

// 1. API Lấy lịch sử Sức khỏe (6 giờ qua)
app.get('/get_history', async (req, res) => {
  try {
    const sixHoursAgo = new Date(Date.now() - 6 * 60 * 60 * 1000);
    const data = await Vital.find({ timestamp: { $gte: sixHoursAgo } })
      .sort({ timestamp: -1 }) // Mới nhất lên đầu
      .limit(100); // Lấy tối đa 100 điểm để nhẹ mạng

    // Format lại cho giống InfluxDB để App Flutter không phải sửa nhiều
    const formattedData = data.map(item => ({
      _time: item.timestamp,
      hr: item.hr,
      spo2: item.spo2
    }));

    res.json(formattedData);
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// 2. API Lấy lịch sử Ngã (30 ngày qua)
app.get('/get_falls', async (req, res) => {
  try {
    const thirtyDaysAgo = new Date(Date.now() - 30 * 24 * 60 * 60 * 1000);
    const data = await Event.find({ 
      type: 'fall', 
      timestamp: { $gte: thirtyDaysAgo } 
    }).sort({ timestamp: -1 });

    const formattedData = data.map(item => ({
      _time: item.timestamp,
      severity: item.severity,
      type: 'fall'
    }));

    res.json(formattedData);
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// --- CHẠY SERVER ---
const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`🚀 Backend đang chạy tại cổng ${PORT}`);
});