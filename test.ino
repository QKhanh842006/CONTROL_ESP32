#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>

// --- CẤU HÌNH LED WS2812B ---
#define LED_PIN         21      // Chân DIN kết nối GPIO 21
#define NUM_LEDS        16      // Tổng 16 bóng (2 thanh 8 bóng nối tiếp)
#define LED_BRIGHTNESS  70      // Độ sáng (0 - 255)

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Định nghĩa màu sắc hiển thị
const uint32_t COLOR_NORMAL = strip.Color(200, 200, 200); // Trắng demi (chạy thẳng / lùi)
const uint32_t COLOR_TURN   = strip.Color(255, 120, 0);   // Vàng cam xi-nhan (rẽ)

// Biến điều khiển hiệu ứng LED & an toàn
String currentVehicleDir          = "";
unsigned long lastLedUpdate       = 0;
uint16_t chaseOffset              = 0;
unsigned long lastActivePacketMs  = 0;
const unsigned long FAILSAFE_MS   = 900;  // Mất tín hiệu 0.9s khi đang chạy -> tự dừng xe

// --- CẤU HÌNH WIFI ACCESS POINT ---
const char *ssid     = "XeCanBang_Control";
const char *password = "12345678";

WebServer server(80);
WebSocketsServer webSocket(81); // WebSocket chạy trên cổng 81

// Biến lưu thông số nhận từ STM32
String vehicleSpeed               = "0.0";
String vehicleAngle               = "0.0";
String serial2Buffer              = "";
unsigned long lastTelemetrySendMs = 0;

// --- QUÉT SÓNG CHỌN KÊNH (1, 6, 11) ÍT NHIỄU NHẤT THEO RSSI ---
int findBestChannel() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks(false, true); // Quét cả mạng ẩn
  const int candidates[3] = {1, 6, 11};
  long score[3] = {0, 0, 0};

  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    int rssi = WiFi.RSSI(i);
    int weight = max(1, 100 + rssi); // Sóng càng mạnh (-40dBm) gây nhiễu càng lớn

    for (int k = 0; k < 3; k++) {
      int dist = abs(ch - candidates[k]);
      if (dist == 0)      score[k] += weight * 3;
      else if (dist <= 2) score[k] += weight * 2;
      else if (dist <= 4) score[k] += weight;
    }
  }
  WiFi.scanDelete();

  // Ưu tiên kênh 11 hoặc 6 nếu điểm nhiễu bằng nhau (né kênh 1 mặc định của đa số ESP32)
  int bestIdx = 2;
  if (score[1] < score[bestIdx]) bestIdx = 1;
  if (score[0] < score[bestIdx]) bestIdx = 0;

  Serial.printf("WiFi Scan (%d mang) -> Diem nhieu [CH1:%ld, CH6:%ld, CH11:%ld] => Chon CH %d\n",
                n, score[0], score[1], score[2], candidates[bestIdx]);
  return candidates[bestIdx];
}

// --- GỬI LỆNH ĐIỀU KHIỂN XUỐNG STM32 ---
void sendDirToSTM32(const String &dir) {
  if (dir == "FL") {
    Serial2.print("F\nL\n");
  } else if (dir == "FR") {
    Serial2.print("F\nR\n");
  } else if (dir == "BL") {
    Serial2.print("B\nL\n");
  } else if (dir == "BR") {
    Serial2.print("B\nR\n");
  } else if (dir == "L") {
    Serial2.print("S\nL\n");
  } else if (dir == "R") {
    Serial2.print("S\nR\n");
  } else if (dir == "F" || dir == "B" || dir == "S") {
    Serial2.print(dir);
    Serial2.print('\n');
  }
}

// Hiệu ứng dải màu đa sắc đuổi nhau liên tục khi đứng yên
void runChaseAnimation() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t pixelHue = chaseOffset + (i * 65536L / NUM_LEDS);
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
  }
  strip.show();
  chaseOffset += 512;
}

// --- HÀM CẬP NHẬT TRẠNG THÁI LED THEO HƯỚNG XE ---
void updateLeds(const String &dir) {
  if (dir == currentVehicleDir) return;
  currentVehicleDir = dir;

  if (dir.indexOf('L') != -1) {
    strip.clear();
    for (int i = 8; i < 16; i++) {
      strip.setPixelColor(i, COLOR_TURN);
    }
    strip.show();
  }
  else if (dir.indexOf('R') != -1) {
    strip.clear();
    for (int i = 0; i < 8; i++) {
      strip.setPixelColor(i, COLOR_TURN);
    }
    strip.show();
  }
  else if (dir == "S") {
    lastLedUpdate = 0;
  }
  else {
    strip.clear();
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, COLOR_NORMAL);
    }
    strip.show();
  }
}

// --- XỬ LÝ SỰ KIỆN WEBSOCKET ---
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      lastActivePacketMs = millis();
      Serial.printf("[WS #%u] Da ket noi\n", num);
      // Gửi ngay telemetry hiện tại khi vừa kết nối
      webSocket.sendTXT(num, "T:" + vehicleSpeed + "," + vehicleAngle);
      break;

    case WStype_DISCONNECTED:
      Serial.printf("[WS #%u] Mat ket noi\n", num);
      // Nếu không còn máy nào kết nối -> dừng xe ngay lập tức
      if (webSocket.connectedClients() == 0 && currentVehicleDir != "S") {
        sendDirToSTM32("S");
        updateLeds("S");
      }
      break;

    case WStype_TEXT: {
      lastActivePacketMs = millis();
      String msg = String((char *)payload);
      msg.trim();

      // Gói giữ nhịp (keepalive) khi đang giữ nút -> chỉ reset bộ đếm failsafe, không spam STM32
      if (msg == "K") return;

      sendDirToSTM32(msg);
      updateLeds(msg);
      break;
    }

    default:
      break;
  }
}

// --- GIAO DIỆN WEB ĐIỀU KHIỂN ---
const char HTML_CONTENT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta charset="UTF-8">
    <title>Xe Cân Bằng Control</title>
    <style>
        * {
            -webkit-touch-callout: none !important;
            -webkit-user-select: none !important;
            user-select: none !important;
            -webkit-user-drag: none !important;
            -webkit-tap-highlight-color: transparent;
            box-sizing: border-box;
        }

        html, body {
            margin: 0;
            padding: 0;
            width: 100%;
            height: 100%;
            font-family: Arial, sans-serif;
            display: flex;
            justify-content: center;
            align-items: center;
            overflow: hidden;
            position: fixed;
            touch-action: none;
            background-color: #111;
        }

        body::before {
            content: "";
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: url('/background.jpg') no-repeat center center;
            background-size: cover;
            pointer-events: none;
            z-index: -1;
        }

        .control-panel {
            text-align: center;
            z-index: 1;
        }

        .ws-badge {
            display: inline-block;
            padding: 3px 10px;
            border-radius: 10px;
            font-size: 11px;
            font-weight: bold;
            margin-bottom: 10px;
            color: #fff;
            background: rgba(235, 77, 75, 0.85);
            pointer-events: none;
        }

        .ws-badge.connected {
            background: rgba(0, 184, 148, 0.85);
        }

        .telemetry-container {
            display: flex;
            gap: 10px;
            justify-content: center;
            margin-bottom: 12px;
            pointer-events: none;
        }

        .telemetry-box {
            background: rgba(0, 0, 0, 0.6);
            color: #dcdde1;
            padding: 6px 12px;
            border-radius: 12px;
            border: 1px solid rgba(255, 255, 255, 0.35);
            font-size: 11px;
            backdrop-filter: blur(4px);
            -webkit-backdrop-filter: blur(4px);
            min-width: 95px;
            box-shadow: 0 4px 10px rgba(0,0,0,0.3);
        }

        .telemetry-value {
            display: block;
            font-size: 19px;
            font-weight: bold;
            color: #00ffcc;
            margin-top: 2px;
        }

        .angle-value {
            color: #ffbe76;
        }

        .grid-container {
            display: grid;
            grid-template-columns: repeat(3, 66px);
            grid-gap: 10px;
            justify-content: center;
        }

        .btn {
            width: 66px;
            height: 66px;
            display: flex;
            justify-content: center;
            align-items: center;
            color: white;
            background: rgba(255, 255, 255, 0.25);
            border: 1px solid rgba(255, 255, 255, 0.4);
            border-radius: 14px;
            cursor: pointer;
            backdrop-filter: blur(4px);
            -webkit-backdrop-filter: blur(4px);
            transition: transform 0.08s ease, background 0.08s ease;
            touch-action: none;
        }

        .btn-diag {
            background: rgba(255, 255, 255, 0.16);
            border: 1px solid rgba(255, 255, 255, 0.3);
        }

        .btn svg {
            width: 28px;
            height: 28px;
            fill: white;
            pointer-events: none;
        }

        .btn.pressing {
            background: rgba(0, 255, 204, 0.65);
            border-color: #00ffcc;
            transform: scale(0.90);
            box-shadow: 0 0 12px rgba(0, 255, 204, 0.6);
        }

        .btn-stop {
            background: rgba(235, 77, 75, 0.75);
        }

        .btn-stop.pressing {
            background: rgba(255, 77, 75, 1.0);
            border-color: #ff4d4b;
            box-shadow: 0 0 12px rgba(255, 77, 75, 0.8);
        }

        h3 {
            color: #ffffff;
            margin: 0 0 6px 0;
            font-size: 16px;
            text-shadow: 0 2px 4px rgba(0,0,0,0.8);
            pointer-events: none;
        }
    </style>
</head>
<body>
    <div class="control-panel">
        <h3>ĐIỀU KHIỂN XE</h3>
        <div id="ws-status" class="ws-badge">ĐANG KẾT NỐI...</div>

        <div class="telemetry-container">
            <div class="telemetry-box">
                VẬN TỐC
                <span id="speed-val" class="telemetry-value">0.0</span>
            </div>
            <div class="telemetry-box">
                GÓC NGHIÊNG
                <span class="telemetry-value angle-value"><span id="angle-val">0.0</span>°</span>
            </div>
        </div>

        <div class="grid-container">
            <!-- Hàng 1: Tiến-Trái (FL), Tiến (F), Tiến-Phải (FR) -->
            <div class="btn btn-diag move-btn" data-cmd="FL">
                <svg viewBox="0 0 24 24" style="transform: rotate(-45deg)"><path d="M12 4l-10 14h20z"/></svg>
            </div>
            <div class="btn move-btn" data-cmd="F">
                <svg viewBox="0 0 24 24"><path d="M12 4l-10 14h20z"/></svg>
            </div>
            <div class="btn btn-diag move-btn" data-cmd="FR">
                <svg viewBox="0 0 24 24" style="transform: rotate(45deg)"><path d="M12 4l-10 14h20z"/></svg>
            </div>

            <!-- Hàng 2: Rẽ Trái (L), Dừng (STOP), Rẽ Phải (R) -->
            <div class="btn move-btn" data-cmd="L">
                <svg viewBox="0 0 24 24"><path d="M4 12l14-10v20z"/></svg>
            </div>
            <div class="btn btn-stop" id="stop-btn">
                <svg viewBox="0 0 24 24"><rect x="5" y="5" width="14" height="14" rx="2"/></svg>
            </div>
            <div class="btn move-btn" data-cmd="R">
                <svg viewBox="0 0 24 24"><path d="M20 12l-14 10v-20z"/></svg>
            </div>

            <!-- Hàng 3: Lùi-Trái (BL), Lùi (B), Lùi-Phải (BR) -->
            <div class="btn btn-diag move-btn" data-cmd="BL">
                <svg viewBox="0 0 24 24" style="transform: rotate(-135deg)"><path d="M12 4l-10 14h20z"/></svg>
            </div>
            <div class="btn move-btn" data-cmd="B">
                <svg viewBox="0 0 24 24"><path d="M12 20l10-14h-20z"/></svg>
            </div>
            <div class="btn btn-diag move-btn" data-cmd="BR">
                <svg viewBox="0 0 24 24" style="transform: rotate(135deg)"><path d="M12 4l-10 14h20z"/></svg>
            </div>
        </div>
    </div>

    <script>
        let ws = null;
        let wsReady = false;
        let reconnectTimer = null;
        let desiredCmd = 'S';
        let lastSentCmd = 'S';
        let isTouchDevice = false;

        const statusBadge = document.getElementById('ws-status');

        window.addEventListener('contextmenu', e => e.preventDefault(), { passive: false });
        document.addEventListener('selectstart', e => e.preventDefault(), { passive: false });
        document.addEventListener('touchmove', e => {
            if (e.cancelable) e.preventDefault();
        }, { passive: false });

        function setStatus(connected) {
            wsReady = connected;
            if (connected) {
                statusBadge.innerText = 'WS SẴN SÀNG';
                statusBadge.classList.add('connected');
            } else {
                statusBadge.innerText = 'MẤT KẾT NỐI - ĐANG THỬ LẠI...';
                statusBadge.classList.remove('connected');
            }
        }

        function connectWS() {
            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
                reconnectTimer = null;
            }
            const host = window.location.hostname || '192.168.4.1';
            ws = new WebSocket('ws://' + host + ':81/');

            ws.onopen = () => {
                setStatus(true);
                lastSentCmd = '';
                sendCmd(true);
            };

            ws.onmessage = (e) => {
                if (typeof e.data === 'string' && e.data.startsWith('T:')) {
                    const parts = e.data.substring(2).split(',');
                    if (parts.length === 2) {
                        document.getElementById('speed-val').innerText = parts[0];
                        document.getElementById('angle-val').innerText = parts[1];
                    }
                }
            };

            ws.onclose = () => {
                setStatus(false);
                reconnectTimer = setTimeout(connectWS, 600);
            };

            ws.onerror = () => {
                try { ws.close(); } catch (err) {}
            };
        }

        function computeDesiredCmd() {
            let d = '';
            let t = '';
            document.querySelectorAll('.move-btn.pressing').forEach(btn => {
                const c = btn.getAttribute('data-cmd');
                if (c.indexOf('F') !== -1) d = 'F';
                else if (c.indexOf('B') !== -1) d = 'B';

                if (c.indexOf('L') !== -1) t = 'L';
                else if (c.indexOf('R') !== -1) t = 'R';
            });
            const combined = d + t;
            return combined === '' ? 'S' : combined;
        }

        function sendCmd(force = false) {
            if (!wsReady || !ws || ws.readyState !== WebSocket.OPEN) return;
            if (!force && desiredCmd === lastSentCmd) return;

            ws.send(desiredCmd);
            lastSentCmd = desiredCmd;
        }

        function updateMotion() {
            desiredCmd = computeDesiredCmd();
            sendCmd(false);
        }

        function stopAll() {
            document.querySelectorAll('.move-btn').forEach(b => b.classList.remove('pressing'));
            desiredCmd = 'S';
            sendCmd(true);
        }

        // Gửi gói giữ nhịp 'K' mỗi 300ms khi đang giữ nút để tránh kích hoạt failsafe
        setInterval(() => {
            if (wsReady && ws && ws.readyState === WebSocket.OPEN && desiredCmd !== 'S') {
                ws.send('K');
            }
        }, 300);

        document.querySelectorAll('.move-btn').forEach(btn => {
            btn.addEventListener('touchstart', (e) => {
                isTouchDevice = true;
                if (e.cancelable) e.preventDefault();
                e.stopPropagation();
                btn.classList.add('pressing');
                updateMotion();
            }, { passive: false });

            const handleTouchEnd = (e) => {
                if (e.cancelable) e.preventDefault();
                e.stopPropagation();
                if (e.targetTouches && e.targetTouches.length > 0) return;
                if (!btn.classList.contains('pressing')) return;
                btn.classList.remove('pressing');
                updateMotion();
            };

            btn.addEventListener('touchend', handleTouchEnd, { passive: false });
            btn.addEventListener('touchcancel', handleTouchEnd, { passive: false });

            btn.addEventListener('mousedown', (e) => {
                if (isTouchDevice) return;
                e.preventDefault();
                btn.classList.add('pressing');
                updateMotion();
            });

            const handleMouseRelease = () => {
                if (isTouchDevice) return;
                if (!btn.classList.contains('pressing')) return;
                btn.classList.remove('pressing');
                updateMotion();
            };

            btn.addEventListener('mouseup', handleMouseRelease);
            btn.addEventListener('mouseleave', handleMouseRelease);
        });

        const stopBtn = document.getElementById('stop-btn');
        const emergencyStop = (e) => {
            if (e.cancelable) e.preventDefault();
            stopBtn.classList.add('pressing');
            stopAll();
        };
        stopBtn.addEventListener('touchstart', emergencyStop, { passive: false });
        stopBtn.addEventListener('touchend', (e) => {
            if (e.cancelable) e.preventDefault();
            stopBtn.classList.remove('pressing');
        }, { passive: false });
        stopBtn.addEventListener('mousedown', (e) => {
            if (isTouchDevice) return;
            emergencyStop(e);
        });
        stopBtn.addEventListener('mouseup', () => stopBtn.classList.remove('pressing'));

        document.addEventListener('visibilitychange', () => {
            if (document.hidden && desiredCmd !== 'S') stopAll();
        });

        connectWS();
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", HTML_CONTENT);
}

void setup() {
  Serial.begin(115200);

  // Khởi động LED WS2812B
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  updateLeds("S");

  // Khởi tạo LittleFS để phục vụ file ảnh nền /background.jpg trên Web
  if (!LittleFS.begin(true)) {
    Serial.println("Loi khoi tao LittleFS!");
  } else {
    Serial.println("Khoi tao LittleFS thanh cong!");
  }

  // Cấu hình Serial2 kết nối STM32 cho ESP32-WROOM-32 (RX: GPIO 26, TX: GPIO 25)
  Serial2.begin(115200, SERIAL_8N1, 26, 25);

  // Quét chọn kênh ít nhiễu nhất -> bật SoftAP tối ưu chống nhiễu
  int bestCh = findBestChannel();
  WiFi.mode(WIFI_AP);
  // Tối đa 2 kết nối để khi điện thoại rớt mạng kết nối lại không bị kẹt phiên cũ
  WiFi.softAP(ssid, password, bestCh, false, 2);

  // Ép băng thông 20MHz và đẩy công suất phát tối đa của WROOM-32 (78 * 0.25 = 19.5 dBm)
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  esp_wifi_set_max_tx_power(78);
  WiFi.setSleep(false);

  Serial.printf("AP Started on CH %d - IP: %s\n", bestCh, WiFi.softAPIP().toString().c_str());

  server.serveStatic("/background.jpg", LittleFS, "/background.jpg");
  server.on("/", handleRoot);
  server.begin();

  // Khởi động WebSocket và bật heartbeat kiểm tra kết nối ngầm
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  webSocket.enableHeartbeat(2000, 1000, 2);
}

void loop() {
  server.handleClient();
  webSocket.loop();

  unsigned long currentMillis = millis();

  // Failsafe: Nếu xe đang chạy mà mất gói keepalive quá 0.9s -> tự dừng xe
  if (currentVehicleDir != "S" && (currentMillis - lastActivePacketMs > FAILSAFE_MS)) {
    Serial.println("[FAILSAFE] Mat tin hieu -> Dung xe!");
    sendDirToSTM32("S");
    updateLeds("S");
  }

  // Hiệu ứng LED đuổi khi đứng yên
  if (currentVehicleDir == "S") {
    if (currentMillis - lastLedUpdate >= 30) {
      lastLedUpdate = currentMillis;
      runChaseAnimation();
    }
  }

  // Đọc telemetry từ STM32
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      serial2Buffer.trim();
      if (serial2Buffer.startsWith("D:")) {
        String payload = serial2Buffer.substring(2);
        int commaIndex = payload.indexOf(',');
        if (commaIndex != -1) {
          vehicleSpeed = payload.substring(0, commaIndex);
          vehicleAngle = payload.substring(commaIndex + 1);
        }
      }
      serial2Buffer = "";
    } else if (c != '\r') {
      serial2Buffer += c;
      if (serial2Buffer.length() > 32) {
        serial2Buffer = "";
      }
    }
  }

  // Đẩy telemetry lên Web qua WebSocket mỗi 250ms (chỉ khi có điện thoại kết nối)
  if (webSocket.connectedClients() > 0 && (currentMillis - lastTelemetrySendMs >= 250)) {
    lastTelemetrySendMs = currentMillis;
    webSocket.broadcastTXT("T:" + vehicleSpeed + "," + vehicleAngle);
  }
}