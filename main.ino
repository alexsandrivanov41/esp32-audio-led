// ============================================================================
// ESP32-WROVER-B Audio Streamer v3.0 — FINAL OPTIMIZED
// ✅ 3-LED WS2812B | I2S Fix | WDT Fix | No mDNS | Minified HTML
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <driver/i2s.h>
#include <esp_task_wdt.h>
// #include <ESPmDNS.h>  // ⚠️ mDNS ОТКЛЮЧЕН (экономия ~18KB)
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <FastLED.h>

#define LOG(fmt, ...) Serial.printf("[%7lu] " fmt "\n", millis(), ##__VA_ARGS__)

// ==================== КОНФИГУРАЦИЯ ====================
const char* AP_SSID = "ESP32-Audio";
const char* AP_PASS = "12345678";
const char* DEFAULT_HOSTNAME = "esp32-audio";
const char* DEFAULT_SERVER_HOST = "192.168.50.54";
const int DEFAULT_SERVER_WS_PORT = 8080;
const int DEFAULT_SERVER_UDP_PORT = 5004;
const char* DEFAULT_SIGNALING_PATH = "/signaling";
const char* DEFAULT_WEB_USER = "admin";
const char* DEFAULT_WEB_PASS = "admin123";
const bool DEFAULT_SSL_ENABLED = false;

// ==================== ПИНЫ ====================
#define I2S_IN_WS      22
#define I2S_IN_SD      21
#define I2S_IN_SCK     23
#define I2S_IN_PORT    I2S_NUM_0

#define I2S_OUT_BCK    26
#define I2S_OUT_LRC    27
#define I2S_OUT_DATA   25
#define I2S_OUT_PORT   I2S_NUM_1

#define BUTTON_PIN          4
#define BUTTON_PULLUP       true
#define BUTTON_DEBOUNCE_MS  50
#define BUTTON_LONG_PRESS_MS 1000

#define LED_PIN         2
#define NUM_LEDS        3
#define LED_BRIGHTNESS  64

// LED индексы
#define LED_STATUS      0   // Режим: ⚪Idle/🟢Play/🔴Rec
#define LED_WS          1   // WS: 🟦Conn/🟡Disc
#define LED_RECORD      2   // Rec/Play: 🔴Rec/🟢Play/⚫Off

// ==================== АУДИО ====================
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512
#define RTP_SSRC    1       // ✅ SSRC для RTP и WS

// ==================== EEPROM ====================
#define EEPROM_SIZE 2048
#define EEPROM_MAGIC 0x43
#define EEPROM_HOSTNAME_ADDR 10
#define EEPROM_WIFI_SSID_ADDR 42
#define EEPROM_WIFI_PASS_ADDR 106
#define EEPROM_SERVER_HOST_ADDR 170
#define EEPROM_SERVER_WS_PORT_ADDR 234
#define EEPROM_SERVER_UDP_PORT_ADDR 236
#define EEPROM_SIGNALING_PATH_ADDR 238
#define EEPROM_WEB_USER_ADDR 270
#define EEPROM_WEB_PASS_ADDR 334
#define EEPROM_WEB_PASS_HASH_ADDR 398
#define EEPROM_SSL_ENABLED_ADDR 462
#define EEPROM_PSK_ADDR 463
#define EEPROM_PSK_HEX_ADDR 495

#define PSK_LEN 32
#define HASH_LEN 32
#define PSK_HEX_LEN (PSK_LEN * 2 + 1)

// ==================== РЕЖИМЫ ====================
enum DeviceMode { MODE_IDLE = 0, MODE_PLAYBACK };

// ==================== ГЛОБАЛЬНЫЕ ====================
CRGB leds[NUM_LEDS];
WebSocketsClient webSocket;
WiFiUDP udp;
WebServer server(80);
uint8_t* i2s_buffer = nullptr;

static uint32_t session_start = 0;
#define SESSION_TIMEOUT 3600000UL

volatile bool is_recording = false;
volatile uint32_t recording_start_time = 0;
volatile DeviceMode device_mode = MODE_IDLE;

struct LedState {
    bool ws_connected;
    bool recording;
    bool recording_blink;
    uint32_t recording_blink_time;
    uint32_t ws_blink_time;
    bool ws_blink_state;
} led_state;

volatile bool button_pressed = false;
volatile bool button_long_press = false;
bool button_state = HIGH;
uint32_t last_button_time = 0;
uint32_t button_press_start = 0;

mbedtls_aes_context dtls_aes_ctx;

struct Config {
    uint8_t magic;
    char hostname[32];
    char wifi_ssid[64];
    char wifi_pass[64];
    char server_host[64];
    int server_ws_port;
    int server_udp_port;
    char signaling_path[32];
    char web_user[32];
    char web_pass[32];
    uint8_t web_pass_hash[HASH_LEN];
    bool ssl_enabled;
    uint8_t psk[PSK_LEN];
    char psk_hex[PSK_HEX_LEN];
} config;

volatile uint32_t packets_sent = 0;
volatile uint32_t bytes_sent = 0;
volatile uint32_t encrypted_packets = 0;
uint32_t rtp_seq = 0;
uint32_t rtp_ts = 0;

bool wifi_configured = false;
bool ap_mode = false;

// ==================== I2S КОНФИГУРАЦИЯ ====================
const i2s_config_t i2s_in_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
};

const i2s_pin_config_t pin_config_in = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_IN_SCK,
    .ws_io_num = I2S_IN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_IN_SD
};

const i2s_config_t i2s_out_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = true,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
};

const i2s_pin_config_t pin_config_out = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_OUT_BCK,
    .ws_io_num = I2S_OUT_LRC,
    .data_out_num = I2S_OUT_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
};

// ==================== LED ФУНКЦИИ ====================
void initLED() {
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHTNESS);
    FastLED.clear();
    FastLED.show();
    led_state.ws_connected = false;
    led_state.recording = false;
    led_state.recording_blink = false;
    led_state.recording_blink_time = 0;
    led_state.ws_blink_time = 0;
    led_state.ws_blink_state = false;
    LOG("LED init");
}

void setLed(int idx, CRGB color) {
    if (idx >= 0 && idx < NUM_LEDS) leds[idx] = color;
}

void setAllLeds(CRGB color) {
    for (int i = 0; i < NUM_LEDS; i++) leds[i] = color;
    FastLED.show();
}

void clearAllLeds() {
    FastLED.clear();
    FastLED.show();
}

void startupAnimation() {
    for (int i = 0; i < NUM_LEDS; i++) { setLed(i, CRGB::Blue); FastLED.show(); delay(50); }
    for (int i = NUM_LEDS - 1; i >= 0; i--) { setLed(i, CRGB::White); FastLED.show(); delay(50); }
    clearAllLeds();
}

void errorBlink(int count) {
    for (int i = 0; i < count; i++) { setAllLeds(CRGB::Red); delay(100); clearAllLeds(); delay(100); }
}

void successBlink() {
    setAllLeds(CRGB::Green); FastLED.show(); delay(150); clearAllLeds();
}

void updateLEDs() {
    uint32_t now = millis();
    
    // LED[0] - Статус режима
    if (led_state.recording) {
        if (now - led_state.recording_blink_time >= 500) {
            led_state.recording_blink = !led_state.recording_blink;
            led_state.recording_blink_time = now;
        }
        setLed(LED_STATUS, led_state.recording_blink ? CRGB::Red : CRGB::Black);
    } else {
        setLed(LED_STATUS, device_mode == MODE_PLAYBACK ? CRGB::Green : CRGB::White);
    }
    
    // LED[1] - WebSocket
    if (led_state.ws_connected) {
        setLed(LED_WS, CRGB::Cyan);
    } else {
        if (now - led_state.ws_blink_time >= 400) {
            led_state.ws_blink_state = !led_state.ws_blink_state;
            led_state.ws_blink_time = now;
        }
        setLed(LED_WS, led_state.ws_blink_state ? CRGB::Yellow : CRGB::Black);
    }
    
    // LED[2] - Запись/Воспроизведение
    if (led_state.recording) {
        setLed(LED_RECORD, CRGB::Red);
    } else if (device_mode == MODE_PLAYBACK) {
        setLed(LED_RECORD, CRGB::Green);
    } else {
        setLed(LED_RECORD, CRGB::Black);
    }
    
    FastLED.show();
}

// ==================== КРИПТОГРАФИЯ ====================
void sha256_hash(const char* input, uint8_t* output) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t*)input, strlen(input));
    mbedtls_sha256_finish(&ctx, output);
    mbedtls_sha256_free(&ctx);
}

void generate_psk(uint8_t* psk, size_t len) {
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const uint8_t*)"esp", 3);
    mbedtls_ctr_drbg_random(&ctr_drbg, psk, len);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
}

void psk_to_hex(const uint8_t* psk, char* hex) {
    const char* hc = "0123456789abcdef";
    for (int i = 0; i < PSK_LEN; i++) {
        hex[i*2] = hc[(psk[i] >> 4) & 0x0F];
        hex[i*2+1] = hc[psk[i] & 0x0F];
    }
    hex[PSK_HEX_LEN-1] = '\0';
}

bool hex_to_psk(const char* hex, uint8_t* psk) {
    if (strlen(hex) != PSK_LEN * 2) return false;
    for (int i = 0; i < PSK_LEN; i++) {
        uint8_t b = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[i*2+j];
            if (c >= '0' && c <= '9') b = (b << 4) | (c - '0');
            else if (c >= 'a' && c <= 'f') b = (b << 4) | (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') b = (b << 4) | (c - 'A' + 10);
            else return false;
        }
        psk[i] = b;
    }
    return true;
}

void encrypt_audio_data(uint8_t* data, size_t len, uint8_t* output, size_t* out_len) {
    if (!config.ssl_enabled) {
        memcpy(output, data, len);
        *out_len = len;
        return;
    }
    
    mbedtls_aes_init(&dtls_aes_ctx);
    mbedtls_aes_setkey_enc(&dtls_aes_ctx, config.psk, PSK_LEN * 8);
    
    // ✅ IV в Big-Endian
    uint8_t iv[16] = {0};
    iv[0] = (rtp_seq >> 24) & 0xFF;
    iv[1] = (rtp_seq >> 16) & 0xFF;
    iv[2] = (rtp_seq >> 8) & 0xFF;
    iv[3] = rtp_seq & 0xFF;
    memcpy(iv + 4, config.psk, 12);
    
    size_t pl = ((len + 15) / 16) * 16;
    uint8_t* pd = (uint8_t*)malloc(pl);
    if (!pd) {
        memcpy(output, data, len);
        *out_len = len;
        mbedtls_aes_free(&dtls_aes_ctx);
        return;
    }
    
    memcpy(pd, data, len);
    memset(pd + len, 0, pl - len);
    mbedtls_aes_crypt_cbc(&dtls_aes_ctx, MBEDTLS_AES_ENCRYPT, pl, iv, pd, output);
    *out_len = pl;
    encrypted_packets++;
    
    free(pd);
    mbedtls_aes_free(&dtls_aes_ctx);
}

// ==================== АУТЕНТИФИКАЦИЯ ====================
bool is_authenticated() {
    if (session_start == 0) return false;
    uint32_t now = millis();
    return (now >= session_start) ? (now - session_start < SESSION_TIMEOUT) 
                                  : (UINT32_MAX - session_start + now < SESSION_TIMEOUT);
}

void start_session() { session_start = millis(); }
void end_session() { session_start = 0; }

bool require_auth() {
    if (!is_authenticated()) {
        server.sendHeader("Location", "/login", true);
        server.send(302);
        return false;
    }
    return true;
}

// ==================== ЗАПИСЬ ====================
void sendRecordingCommand(bool start) {
    if (!webSocket.isConnected()) return;
    DynamicJsonDocument doc(128);
    doc["type"] = start ? "record_start" : "record_stop";
    doc["ts"] = millis();
    doc["id"] = config.hostname;
    String msg;
    serializeJson(doc, msg);
    webSocket.sendTXT(msg);
}

void toggleRecording() {
    is_recording = !is_recording;
    led_state.recording = is_recording;
    
    if (is_recording) {
        recording_start_time = millis();
        led_state.recording_blink = true;
        led_state.recording_blink_time = millis();
        LOG("REC ON");
    } else {
        LOG("REC OFF %lus", (millis() - recording_start_time) / 1000);
    }
    
    sendRecordingCommand(is_recording);
}

String getRecDur() {
    if (!is_recording) return "-";
    uint32_t s = (millis() - recording_start_time) / 1000;
    return String(s/60) + ":" + (s%60<10?"0":"") + String(s%60);
}

// ==================== КНОПКА ====================
void initButton() {
    pinMode(BUTTON_PIN, BUTTON_PULLUP ? INPUT_PULLUP : INPUT);
    button_state = digitalRead(BUTTON_PIN);
    last_button_time = millis();
    button_pressed = false;
    button_long_press = false;
}

void checkButton() {
    bool cur = digitalRead(BUTTON_PIN);
    uint32_t now = millis();
    
    if (cur != button_state) {
        last_button_time = now;
        button_state = cur;
    }
    
    // Нажатие
    if (cur == LOW && !button_pressed && (now - last_button_time >= BUTTON_DEBOUNCE_MS)) {
        button_pressed = true;
        button_press_start = now;
        button_long_press = false;
    }
    
    // Долгое нажатие
    if (cur == LOW && button_pressed && !button_long_press) {
        if (now - button_press_start >= BUTTON_LONG_PRESS_MS) {
            button_long_press = true;
            if (!is_recording) {
                device_mode = (device_mode == MODE_IDLE) ? MODE_PLAYBACK : MODE_IDLE;
                setLed(LED_STATUS, CRGB::Blue);
                FastLED.show();
                delay(60);
                LOG("Mode:%d", device_mode);
            }
        }
    }
    
    // Отпускание
    if (cur == HIGH && button_pressed) {
        if (!button_long_press) {
            if (webSocket.isConnected()) {
                toggleRecording();
            } else {
                errorBlink(2);
            }
        }
        button_pressed = false;
        button_long_press = false;
    }
}

// ==================== I2S ИНИЦИАЛИЗАЦИЯ ====================
// ✅ FIX: i2s_driver_uninstall перед install для предотвращения конфликта
// ✅ FIX: i2s_set_clk удалён (вызывает конфликт в Core 3.x)

void initAudioInput() {
    i2s_driver_uninstall(I2S_IN_PORT);
    delay(10);
    
    if (i2s_driver_install(I2S_IN_PORT, &i2s_in_config, 0, NULL) != ESP_OK) {
        LOG("I2S IN install failed");
        return;
    }
    
    if (i2s_set_pin(I2S_IN_PORT, &pin_config_in) != ESP_OK) {
        LOG("I2S IN set_pin failed");
        i2s_driver_uninstall(I2S_IN_PORT);
        return;
    }
    
    i2s_zero_dma_buffer(I2S_IN_PORT);
    LOG("I2S IN ok");
}

void initAudioOutput() {
    i2s_driver_uninstall(I2S_OUT_PORT);
    delay(10);
    
    if (i2s_driver_install(I2S_OUT_PORT, &i2s_out_config, 0, NULL) != ESP_OK) {
        LOG("I2S OUT install failed");
        return;
    }
    
    if (i2s_set_pin(I2S_OUT_PORT, &pin_config_out) != ESP_OK) {
        LOG("I2S OUT set_pin failed");
        i2s_driver_uninstall(I2S_OUT_PORT);
        return;
    }
    
    i2s_zero_dma_buffer(I2S_OUT_PORT);
    // ❌ i2s_set_clk УДАЛЕН - вызывает конфликт драйверов
    LOG("I2S OUT ok");
}

// ==================== HTML МИНИФИЦИРОВАННЫЙ ====================
const char HTML_CONFIG[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 Audio</title><style>body{font-family:Arial,sans-serif;max-width:800px;margin:0 auto;padding:20px;background:#f0f2f5}h1{color:#1a73e8;text-align:center}.card{background:#fff;padding:20px;border-radius:10px;margin:15px 0;box-shadow:0 2px 6px rgba(0,0,0,.1)}label{display:block;margin:10px 0 5px;font-weight:600}input{width:100%;padding:10px;border:2px solid #e0e0e0;border-radius:6px;box-sizing:border-box}input:focus{border-color:#1a73e8;outline:0}button{background:#1a73e8;color:#fff;padding:12px 20px;border:0;border-radius:6px;cursor:pointer;width:100%;font-size:16px;margin-top:10px}button:hover{background:#1557b0}button.danger{background:#dc3545}button.warning{background:#ffc107;color:#333}button.rec{background:#dc3545;animation:pulse 1.5s infinite}button.idle{background:#28a745}@keyframes pulse{0%,100%{box-shadow:0 0 0 0 rgba(220,53,69,.7)}50%{box-shadow:0 0 0 10px rgba(220,53,69,0)}}.info{background:#e7f3ff;padding:15px;border-radius:8px;margin:15px 0;border-left:4px solid #1a73e8}.row{display:flex;gap:10px}.row .col{flex:1}.rec-panel{background:linear-gradient(135deg,#28a745,#218838);color:#fff;padding:20px;border-radius:10px;text-align:center;margin:15px 0}.rec-panel.rec{background:linear-gradient(135deg,#dc3545,#c82333)}.rec-timer{font-size:28px;font-weight:700;margin:10px 0;font-family:monospace}.tabs{display:flex;gap:8px;margin-bottom:15px;border-bottom:2px solid #e0e0e0}.tab{padding:8px 16px;cursor:pointer;background:0 0;border:0;font-size:14px;color:#666}.tab.active{color:#1a73e8;border-bottom:2px solid #1a73e8}.tab-content{display:none}.tab-content.active{display:block}.leds{display:flex;justify-content:center;gap:15px;margin:15px 0}.led-box{background:#333;padding:10px;border-radius:6px;color:#fff;text-align:center;min-width:70px}.led-dot{width:16px;height:16px;border-radius:50%;margin:0 auto 6px}.led-lbl{font-size:10px;color:#aaa}.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.stat{background:#f8f9fa;padding:10px;border-radius:6px;text-align:center}.stat-val{font-size:18px;font-weight:700;color:#1a73e8}.stat-lbl{font-size:10px;color:#666}</style><script>function showTab(t,e){document.querySelectorAll('.tab-content').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.getElementById(t).classList.add('active');e.classList.add('active')}function toggleRec(){fetch('/api/rec/toggle',{method:'POST'}).then(r=>r.json()).then(d=>updRec(d.rec,d.dur)).catch(()=>{})}function updRec(r,d){const p=document.getElementById('recPanel'),b=document.getElementById('recBtn'),t=document.getElementById('recTimer'),s=document.getElementById('recStatus');if(r){p.classList.add('rec');b.classList.remove('idle');b.classList.add('rec');b.innerHTML='⏹️ Stop';s.innerHTML='🔴 REC';t.style.display='block'}else{p.classList.remove('rec');b.classList.remove('rec');b.classList.add('idle');b.innerHTML='🔴 Rec';s.innerHTML='⏹️ Idle';t.style.display='none'}if(t&&d)t.textContent=d}setInterval(()=>{fetch('/api/rec/status').then(r=>r.json()).then(d=>{updRec(d.rec,d.dur);updLeds(d.ls,d.lw,d.lr)}).catch(()=>{})},2000);function updLeds(s,w,r){document.getElementById('ls').style.background=s;document.getElementById('lw').style.background=w;document.getElementById('lr').style.background=r}</script></head><body><h1>🎵 ESP32 Audio v3.0</h1><div class="leds"><div class="led-box"><div id="ls" class="led-dot" style="background:%LS%"></div><div class="led-lbl">Mode</div></div><div class="led-box"><div id="lw" class="led-dot" style="background:%LW%"></div><div class="led-lbl">WS</div></div><div class="led-box"><div id="lr" class="led-dot" style="background:%LR%"></div><div class="led-lbl">Rec</div></div></div><div id="recPanel" class="rec-panel %RC%"><h3>🎙️ Recording</h3><p id="recStatus">%RS%</p><div id="recTimer" class="rec-timer" style="display:%TD%">%RD%</div><button id="recBtn" class="%RB%" onclick="toggleRec()">%RT%</button></div><div class="info"><p><b>Host:</b> %HN%</p><p><b>IP:</b> %IP%</p><p><b>WS:</b> %WS%</p><p><b>SSL:</b> %SS%</p><p><b>Mode:</b> %DM%</p></div><div class="tabs"><button class="tab active" onclick="showTab('net',this)">🌐 Net</button><button class="tab" onclick="showTab('sec',this)">🔒 Sec</button><button class="tab" onclick="showTab('st',this)">📊 Stat</button></div><div id="net" class="tab-content active"><form method="POST" action="/save"><div class="card"><h3>Device</h3><label>Hostname:</label><input name="hostname" value="%HN%" required maxlength="31"></div><div class="card"><h3>WiFi</h3><label>SSID:</label><input name="ssid" value="%SSID%" required><label>Pass:</label><input type="password" name="pass" value="%PASS%" placeholder="***"></div><div class="card"><h3>Server</h3><label>Host:</label><input name="server_host" value="%SH%" required><div class="row"><div class="col"><label>WS Port:</label><input type="number" name="ws_port" value="%WP%" required></div><div class="col"><label>UDP Port:</label><input type="number" name="udp_port" value="%UP%" required></div></div><label>Path:</label><input name="sig_path" value="%SP%" required><label style="margin-top:10px"><input type="checkbox" name="ssl" value="1" %SC% style="width:auto"> SSL/TLS</label></div><button type="submit">💾 Save & Reboot</button></form></div><div id="sec" class="tab-content"><form method="POST" action="/pwd"><div class="card"><h3>Password</h3><label>Current:</label><input type="password" name="cur" required><label>New:</label><input type="password" name="new" required minlength="6"><label>Confirm:</label><input type="password" name="conf" required><button type="submit">🔄 Change</button></div></form><form method="POST" action="/psk"><div class="card"><h3>PSK Key</h3><label>PSK (64 hex):</label><input name="psk_hex" value="%PH%" pattern="[0-9a-fA-F]{64}" maxlength="64" required><p style="font-size:11px;color:#888">Current: %PM%</p><button type="submit" class="warning">🔐 Apply</button></div></form></div><div id="st" class="tab-content"><div class="card"><h3>Stats</h3><div class="stats"><div class="stat"><div class="stat-val">%PK%</div><div class="stat-lbl">Pkts</div></div><div class="stat"><div class="stat-val">%BY%</div><div class="stat-lbl">Bytes</div></div><div class="stat"><div class="stat-val">%EN%</div><div class="stat-lbl">Enc</div></div><div class="stat"><div class="stat-val">%UT%s</div><div class="stat-lbl">Uptime</div></div><div class="stat"><div class="stat-val">%HP%K</div><div class="stat-lbl">Heap</div></div><div class="stat"><div class="stat-val">%RS%dB</div><div class="stat-lbl">RSSI</div></div></div></div></div><form method="POST" action="/reset"><button type="submit" class="danger">⚠️ Factory Reset</button></form><div style="text-align:center;margin-top:20px;font-size:12px;color:#999"><a href="/logout">Logout</a> | <a href="/api/status">API</a></div></body></html>
)rawliteral";

const char HTML_LOGIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Login</title><style>body{font-family:Arial;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;margin:0;display:flex;align-items:center;justify-content:center}.box{background:#fff;padding:30px;border-radius:10px;box-shadow:0 8px 30px rgba(0,0,0,.2);width:100%;max-width:350px}h1{color:#1a73e8;text-align:center}label{display:block;margin:12px 0 6px;font-weight:600}input{width:100%;padding:10px;border:2px solid #e0e0e0;border-radius:6px;box-sizing:border-box}button{background:#1a73e8;color:#fff;padding:12px;border:0;border-radius:6px;cursor:pointer;width:100%;font-size:16px;margin-top:15px}.err{background:#fee;color:#c33;padding:8px;border-radius:6px;margin-top:10px;text-align:center}</style></head><body><div class="box"><h1>🎵 ESP32 Audio</h1><form method="POST" action="/login"><label>User:</label><input name="user" required autofocus><label>Pass:</label><input type="password" name="pass" required><button type="submit">Login</button></form>%ERR%</div></body></html>
)rawliteral";

// ==================== ОБРАБОТЧИКИ ====================
void handleFavicon() { server.send(204); }

void handleNotFound() {
    if (!is_authenticated()) {
        server.sendHeader("Location", "/login", true);
        server.send(302);
        return;
    }
    server.send(404, "text/plain", "NF");
}

void handleLoginGet() {
    if (is_authenticated()) {
        server.sendHeader("Location", "/", true);
        server.send(302);
        return;
    }
    String html = FPSTR(HTML_LOGIN);
    html.replace("%ERR%", server.hasArg("e") ? "<div class='err'>❌ Invalid</div>" : "");
    server.send(200, "text/html", html);
}

void handleLoginPost() {
    if (server.hasArg("user") && server.hasArg("pass")) {
        uint8_t hash[HASH_LEN];
        sha256_hash(server.arg("pass").c_str(), hash);
        if (server.arg("user") == config.web_user && memcmp(hash, config.web_pass_hash, HASH_LEN) == 0) {
            start_session();
            server.sendHeader("Location", "/", true);
            server.send(302);
            return;
        }
    }
    server.sendHeader("Location", "/login?e=1", true);
    server.send(302);
}

void handleLogout() {
    end_session();
    server.sendHeader("Location", "/login", true);
    server.send(302);
}

void handleRecToggle() {
    if (!require_auth()) {
        server.send(401, "application/json", "{\"err\":\"Auth\"}");
        return;
    }
    if (!webSocket.isConnected()) {
        server.send(503, "application/json", "{\"err\":\"WS\"}");
        return;
    }
    toggleRecording();
    DynamicJsonDocument doc(64);
    doc["ok"] = true;
    doc["rec"] = is_recording;
    doc["dur"] = getRecDur();
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleRecStatus() {
    if (!require_auth()) {
        server.send(401);
        return;
    }
    DynamicJsonDocument doc(128);
    doc["rec"] = is_recording;
    doc["dur"] = getRecDur();
    doc["con"] = webSocket.isConnected();
    
    String ls = "#666";
    if (is_recording) ls = led_state.recording_blink ? "#dc3545" : "#000";
    else if (device_mode == MODE_PLAYBACK) ls = "#28a745";
    else ls = "#fff";
    
    doc["ls"] = ls;
    doc["lw"] = led_state.ws_connected ? "#17a2b8" : "#ffc107";
    doc["lr"] = is_recording ? "#dc3545" : (device_mode == MODE_PLAYBACK ? "#28a745" : "#000");
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handlePSK() {
    if (!require_auth()) return;
    if (server.hasArg("psk_hex")) {
        String hex = server.arg("psk_hex");
        if (hex.length() != 64) {
            server.send(400, "text/plain", "64 hex");
            return;
        }
        if (!hex_to_psk(hex.c_str(), config.psk)) {
            server.send(400, "text/plain", "Invalid");
            return;
        }
        hex.toCharArray(config.psk_hex, PSK_HEX_LEN);
        config.ssl_enabled = true;
        saveConfig();
        server.send(200, "text/html", "<meta http-equiv='refresh' content='2;url=/'><body style='font-family:Arial;text-align:center;padding:50px'><h1 style='color:#28a745'>OK</h1></body>");
        return;
    }
    server.send(400, "text/plain", "Missing");
}

void handlePwd() {
    if (!require_auth()) return;
    if (server.hasArg("cur") && server.hasArg("new") && server.hasArg("conf")) {
        String cur = server.arg("cur"), nw = server.arg("new"), cf = server.arg("conf");
        if (nw != cf) {
            server.send(400, "text/plain", "Mismatch");
            return;
        }
        if (nw.length() < 6) {
            server.send(400, "text/plain", "Min 6");
            return;
        }
        uint8_t h[HASH_LEN];
        sha256_hash(cur.c_str(), h);
        if (memcmp(h, config.web_pass_hash, HASH_LEN) != 0) {
            server.send(400, "text/plain", "Wrong");
            return;
        }
        strcpy(config.web_pass, nw.c_str());
        sha256_hash(nw.c_str(), config.web_pass_hash);
        saveConfig();
        end_session();
        server.send(200, "text/html", "<meta http-equiv='refresh' content='2;url=/login'><body style='font-family:Arial;text-align:center;padding:50px'><h1 style='color:#28a745'>OK</h1></body>");
        return;
    }
    server.send(400, "text/plain", "Missing");
}

void handleConfig() {
    if (!require_auth()) return;
    if (ESP.getFreeHeap() < 40000) {
        server.send(503, "text/plain", "LowMem");
        return;
    }
    
    String html = FPSTR(HTML_CONFIG);
    String ip = ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String ws = webSocket.isConnected() ? "OK" : "Wait";
    String ss = config.ssl_enabled ? "On" : "Off";
    String sc = config.ssl_enabled ? "checked" : "";
    String rs = is_recording ? "🔴 REC" : "⏹️ Idle";
    String rd = getRecDur();
    String rt = is_recording ? "⏹️ Stop" : "🔴 Rec";
    String rb = is_recording ? "rec" : "idle";
    String rc = is_recording ? "rec" : "";
    String td = is_recording ? "block" : "none";
    String ph = String(config.psk_hex);
    String pm = config.ssl_enabled ? "●●●●..." : "-";
    String dm = (device_mode == MODE_PLAYBACK) ? "PLAY" : "IDLE";
    
    String ls = "#666";
    if (is_recording) ls = led_state.recording_blink ? "#dc3545" : "#000";
    else if (device_mode == MODE_PLAYBACK) ls = "#28a745";
    else ls = "#fff";
    
    String lw = led_state.ws_connected ? "#17a2b8" : "#ffc107";
    String lr = is_recording ? "#dc3545" : (device_mode == MODE_PLAYBACK ? "#28a745" : "#000");
    
    html.replace("%HN%", config.hostname);
    html.replace("%IP%", ip);
    html.replace("%WS%", ws);
    html.replace("%SS%", ss);
    html.replace("%SC%", sc);
    html.replace("%SSID%", config.wifi_ssid);
    html.replace("%PASS%", "********");
    html.replace("%SH%", config.server_host);
    html.replace("%WP%", String(config.server_ws_port));
    html.replace("%UP%", String(config.server_udp_port));
    html.replace("%SP%", config.signaling_path);
    html.replace("%RS%", rs);
    html.replace("%RD%", rd);
    html.replace("%RT%", rt);
    html.replace("%RB%", rb);
    html.replace("%RC%", rc);
    html.replace("%TD%", td);
    html.replace("%PH%", ph);
    html.replace("%PM%", pm);
    html.replace("%DM%", dm);
    html.replace("%LS%", ls);
    html.replace("%LW%", lw);
    html.replace("%LR%", lr);
    html.replace("%PK%", String(packets_sent));
    html.replace("%BY%", String(bytes_sent));
    html.replace("%EN%", String(encrypted_packets));
    html.replace("%UT%", String(millis() / 1000));
    html.replace("%HP%", String(ESP.getFreeHeap() / 1024));
    html.replace("%RS%", String(WiFi.RSSI()));
    
    server.send(200, "text/html", html);
}

void handleSave() {
    if (!require_auth()) return;
    if (server.hasArg("ssid")) {
        server.arg("hostname").toCharArray(config.hostname, 32);
        server.arg("ssid").toCharArray(config.wifi_ssid, 64);
        String p = server.arg("pass");
        if (p.length() > 0 && p != "********") p.toCharArray(config.wifi_pass, 64);
        server.arg("server_host").toCharArray(config.server_host, 64);
        config.server_ws_port = server.arg("ws_port").toInt();
        config.server_udp_port = server.arg("udp_port").toInt();
        server.arg("sig_path").toCharArray(config.signaling_path, 32);
        config.ssl_enabled = server.hasArg("ssl");
        saveConfig();
        server.send(200, "text/html", "<meta http-equiv='refresh' content='3;url=/'><body style='font-family:Arial;text-align:center;padding:50px'><h1 style='color:#28a745'>Saved</h1><p>Reboot...</p></body>");
        delay(800);
        ESP.restart();
    }
}

void handleReset() {
    if (!require_auth()) return;
    resetConfig();
    server.send(200, "text/html", "<meta http-equiv='refresh' content='3;url=/'><body style='font-family:Arial;text-align:center;padding:50px'><h1 style='color:#dc3545'>Reset</h1><p>Reboot...</p></body>");
    delay(800);
    ESP.restart();
}

void handleStatus() {
    if (!require_auth()) return;
    DynamicJsonDocument doc(256);
    doc["hn"] = config.hostname;
    doc["ip"] = ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["ws"] = webSocket.isConnected();
    doc["ssl"] = config.ssl_enabled;
    doc["pk"] = packets_sent;
    doc["by"] = bytes_sent;
    doc["hp"] = ESP.getFreeHeap();
    doc["rec"] = is_recording;
    doc["mode"] = device_mode;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// ==================== КОНФИГУРАЦИЯ ====================
void loadConfig() {
    EEPROM.begin(EEPROM_SIZE);
    config.magic = EEPROM.read(0);
    
    if (config.magic == EEPROM_MAGIC) {
        for (int i = 0; i < 32; i++) config.hostname[i] = EEPROM.read(EEPROM_HOSTNAME_ADDR + i);
        for (int i = 0; i < 64; i++) config.wifi_ssid[i] = EEPROM.read(EEPROM_WIFI_SSID_ADDR + i);
        for (int i = 0; i < 64; i++) config.wifi_pass[i] = EEPROM.read(EEPROM_WIFI_PASS_ADDR + i);
        for (int i = 0; i < 64; i++) config.server_host[i] = EEPROM.read(EEPROM_SERVER_HOST_ADDR + i);
        config.server_ws_port = EEPROM.read(EEPROM_SERVER_WS_PORT_ADDR) | (EEPROM.read(EEPROM_SERVER_WS_PORT_ADDR+1) << 8);
        config.server_udp_port = EEPROM.read(EEPROM_SERVER_UDP_PORT_ADDR) | (EEPROM.read(EEPROM_SERVER_UDP_PORT_ADDR+1) << 8);
        for (int i = 0; i < 32; i++) config.signaling_path[i] = EEPROM.read(EEPROM_SIGNALING_PATH_ADDR + i);
        for (int i = 0; i < 32; i++) config.web_user[i] = EEPROM.read(EEPROM_WEB_USER_ADDR + i);
        for (int i = 0; i < 32; i++) config.web_pass[i] = EEPROM.read(EEPROM_WEB_PASS_ADDR + i);
        for (int i = 0; i < HASH_LEN; i++) config.web_pass_hash[i] = EEPROM.read(EEPROM_WEB_PASS_HASH_ADDR + i);
        config.ssl_enabled = EEPROM.read(EEPROM_SSL_ENABLED_ADDR);
        for (int i = 0; i < PSK_LEN; i++) config.psk[i] = EEPROM.read(EEPROM_PSK_ADDR + i);
        for (int i = 0; i < PSK_HEX_LEN && EEPROM_PSK_HEX_ADDR+i<EEPROM_SIZE; i++)
            config.psk_hex[i] = EEPROM.read(EEPROM_PSK_HEX_ADDR + i);
        
        if (!strlen(config.hostname)) strcpy(config.hostname, DEFAULT_HOSTNAME);
        if (!strlen(config.web_user)) strcpy(config.web_user, DEFAULT_WEB_USER);
        
        bool hv = false;
        for (int i = 0; i < HASH_LEN; i++) if (config.web_pass_hash[i]) { hv = true; break; }
        if (!hv) {
            if (strlen(config.web_pass)) sha256_hash(config.web_pass, config.web_pass_hash);
            else { strcpy(config.web_pass, DEFAULT_WEB_PASS); sha256_hash(DEFAULT_WEB_PASS, config.web_pass_hash); }
        }
        
        wifi_configured = strlen(config.wifi_ssid) > 0 && strcmp(config.wifi_ssid, "YOUR_WIFI_SSID") != 0;
        if (config.server_ws_port < 1) config.server_ws_port = DEFAULT_SERVER_WS_PORT;
        if (config.server_udp_port < 1) config.server_udp_port = DEFAULT_SERVER_UDP_PORT;
        if (!strlen(config.signaling_path)) strcpy(config.signaling_path, DEFAULT_SIGNALING_PATH);
        
        bool pv = false;
        for (int i = 0; i < PSK_LEN; i++) if (config.psk[i]) { pv = true; break; }
        if (!pv) { generate_psk(config.psk, PSK_LEN); psk_to_hex(config.psk, config.psk_hex); }
        if (!strlen(config.psk_hex)) psk_to_hex(config.psk, config.psk_hex);
        
        LOG("Cfg loaded");
    } else {
        strcpy(config.hostname, DEFAULT_HOSTNAME);
        strcpy(config.wifi_ssid, "YOUR_WIFI_SSID");
        strcpy(config.wifi_pass, "YOUR_WIFI_PASSWORD");
        strcpy(config.server_host, DEFAULT_SERVER_HOST);
        config.server_ws_port = DEFAULT_SERVER_WS_PORT;
        config.server_udp_port = DEFAULT_SERVER_UDP_PORT;
        strcpy(config.signaling_path, DEFAULT_SIGNALING_PATH);
        strcpy(config.web_user, DEFAULT_WEB_USER);
        strcpy(config.web_pass, DEFAULT_WEB_PASS);
        sha256_hash(DEFAULT_WEB_PASS, config.web_pass_hash);
        config.ssl_enabled = DEFAULT_SSL_ENABLED;
        generate_psk(config.psk, PSK_LEN);
        psk_to_hex(config.psk, config.psk_hex);
        wifi_configured = false;
        LOG("Default cfg");
    }
    EEPROM.end();
}

void saveConfig() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(0, EEPROM_MAGIC);
    for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_HOSTNAME_ADDR + i, config.hostname[i]);
    for (int i = 0; i < 64; i++) EEPROM.write(EEPROM_WIFI_SSID_ADDR + i, config.wifi_ssid[i]);
    for (int i = 0; i < 64; i++) EEPROM.write(EEPROM_WIFI_PASS_ADDR + i, config.wifi_pass[i]);
    for (int i = 0; i < 64; i++) EEPROM.write(EEPROM_SERVER_HOST_ADDR + i, config.server_host[i]);
    EEPROM.write(EEPROM_SERVER_WS_PORT_ADDR, config.server_ws_port & 0xFF);
    EEPROM.write(EEPROM_SERVER_WS_PORT_ADDR+1, (config.server_ws_port >> 8) & 0xFF);
    EEPROM.write(EEPROM_SERVER_UDP_PORT_ADDR, config.server_udp_port & 0xFF);
    EEPROM.write(EEPROM_SERVER_UDP_PORT_ADDR+1, (config.server_udp_port >> 8) & 0xFF);
    for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_SIGNALING_PATH_ADDR + i, config.signaling_path[i]);
    for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_WEB_USER_ADDR + i, config.web_user[i]);
    for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_WEB_PASS_ADDR + i, config.web_pass[i]);
    for (int i = 0; i < HASH_LEN; i++) EEPROM.write(EEPROM_WEB_PASS_HASH_ADDR + i, config.web_pass_hash[i]);
    EEPROM.write(EEPROM_SSL_ENABLED_ADDR, config.ssl_enabled ? 1 : 0);
    for (int i = 0; i < PSK_LEN; i++) EEPROM.write(EEPROM_PSK_ADDR + i, config.psk[i]);
    for (int i = 0; i < PSK_HEX_LEN && EEPROM_PSK_HEX_ADDR+i<EEPROM_SIZE; i++)
        EEPROM.write(EEPROM_PSK_HEX_ADDR + i, config.psk_hex[i]);
    EEPROM.commit();
    EEPROM.end();
    LOG("Cfg saved");
}

void resetConfig() {
    EEPROM.begin(EEPROM_SIZE);
    for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
    EEPROM.commit();
    EEPROM.end();
    LOG("Cfg reset");
}

// ==================== АУДИО И WS ====================
void sendRtpAudio(uint8_t* data, size_t len) {
    if (!udp.beginPacket(config.server_host, config.server_udp_port)) return;
    
    // ✅ RTP заголовок с SSRC
    uint8_t hdr[12] = {
        0x80, 0x60,
        (uint8_t)(rtp_seq >> 8), (uint8_t)(rtp_seq & 0xFF),
        (uint8_t)(rtp_ts >> 24), (uint8_t)(rtp_ts >> 16),
        (uint8_t)(rtp_ts >> 8), (uint8_t)(rtp_ts & 0xFF),
        (uint8_t)(RTP_SSRC >> 24), (uint8_t)(RTP_SSRC >> 16),
        (uint8_t)(RTP_SSRC >> 8), (uint8_t)(RTP_SSRC & 0xFF)
    };
    
    uint8_t* enc = (uint8_t*)malloc(len + 32);
    if (!enc) {
        udp.endPacket();
        return;
    }
    
    size_t el;
    encrypt_audio_data(data, len, enc, &el);
    
    udp.write(hdr, 12);
    udp.write(enc, el);
    udp.endPacket();
    
    rtp_seq++;
    rtp_ts += len / 2;
    packets_sent++;
    bytes_sent += el + 12;
    
    free(enc);
}

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            LOG("WS con");
            led_state.ws_connected = true;
            successBlink();
            {
                DynamicJsonDocument doc(128);
                doc["type"] = "register";
                doc["id"] = config.hostname;
                doc["mac"] = WiFi.macAddress();
                doc["ssl"] = config.ssl_enabled;
                // ✅ SSRC отправляется серверу
                doc["ssrc"] = RTP_SSRC;
                String msg;
                serializeJson(doc, msg);
                webSocket.sendTXT(msg);
            }
            break;
        case WStype_DISCONNECTED:
            LOG("WS dis");
            led_state.ws_connected = false;
            if (is_recording) {
                is_recording = false;
                led_state.recording = false;
            }
            break;
        case WStype_TEXT: {
            DynamicJsonDocument doc(128);
            if (!deserializeJson(doc, payload)) {
                String t = doc["type"];
                if (t == "registered") {
                    int p = doc["udp_port"];
                    if (p > 0 && p != config.server_udp_port) {
                        config.server_udp_port = p;
                        saveConfig();
                    }
                } else if (t == "record_ack") {
                    bool r = doc["recording"];
                    if (r != is_recording) {
                        is_recording = r;
                        led_state.recording = r;
                        if (r) {
                            recording_start_time = millis();
                            led_state.recording_blink = true;
                            led_state.recording_blink_time = millis();
                        }
                    }
                }
            }
            break;
        }
        default: break;
    }
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    delay(100);
    
    LOG("ESP32 Audio v3.0");
    
    // ✅ FIX: Очистка I2S перед инициализацией
    i2s_driver_uninstall(I2S_IN_PORT);
    i2s_driver_uninstall(I2S_OUT_PORT);
    delay(50);
    
    initLED();
    startupAnimation();
    loadConfig();
    
    if (psramFound()) i2s_buffer = (uint8_t*)ps_malloc(BUFFER_SIZE);
    else i2s_buffer = (uint8_t*)malloc(BUFFER_SIZE);
    
    if (!i2s_buffer) {
        LOG("Mem fail");
        errorBlink(5);
        while(1) delay(1000);
    }
    
    initAudioInput();
    initButton();
    initAudioOutput();
    
    if (wifi_configured) {
        LOG("WiFi:%s", config.wifi_ssid);
        WiFi.setHostname(config.hostname);
        WiFi.begin(config.wifi_ssid, config.wifi_pass);
        WiFi.setSleep(false);
        setLed(LED_STATUS, CRGB::Blue);
        FastLED.show();
        
        int att = 0;
        while (WiFi.status() != WL_CONNECTED && att < 40) {
            delay(500);
            att++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            LOG("WiFi OK %s", WiFi.localIP().toString().c_str());
            ap_mode = false;
        } else {
            LOG("WiFi fail, AP");
            ap_mode = true;
        }
    } else {
        LOG("No cfg, AP");
        ap_mode = true;
    }
    
    if (ap_mode) {
        WiFi.softAP(AP_SSID, AP_PASS);
        LOG("AP:%s", WiFi.softAPIP().toString().c_str());
    }
    
    // mDNS отключён
    
    udp.begin(ap_mode ? WiFi.softAPIP() : WiFi.localIP(), 0);
    
    if (!ap_mode) {
        LOG("WS:%s:%d%s", config.server_host, config.server_ws_port, config.signaling_path);
        webSocket.begin(config.server_host, config.server_ws_port, config.signaling_path);
        webSocket.onEvent(wsEvent);
        webSocket.setReconnectInterval(3000);
        
        for (int i = 0; i < 15 && !webSocket.isConnected(); i++) {
            webSocket.loop();
            delay(100);
        }
    }
    
    server.on("/favicon.ico", handleFavicon);
    server.on("/", handleConfig);
    server.on("/login", HTTP_GET, handleLoginGet);
    server.on("/login", HTTP_POST, handleLoginPost);
    server.on("/logout", handleLogout);
    server.on("/api/rec/toggle", HTTP_POST, handleRecToggle);
    server.on("/api/rec/status", HTTP_GET, handleRecStatus);
    server.on("/psk", HTTP_POST, handlePSK);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/pwd", HTTP_POST, handlePwd);
    server.on("/api/status", handleStatus);
    server.onNotFound(handleNotFound);
    
    server.begin();
    LOG("Web:%s", (ap_mode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
    
    // ✅ FIX: WDT инициализация для Core 2.0.17
    esp_task_wdt_deinit();
    esp_task_wdt_init(10000, true);  // timeout=10sec, panic=true
    esp_task_wdt_add(NULL);
    
    device_mode = MODE_IDLE;
    updateLEDs();
    
    LOG("Setup OK");
}

// ==================== LOOP ====================
void loop() {
    esp_task_wdt_reset();
    
    webSocket.loop();
    server.handleClient();
    checkButton();
    updateLEDs();
    
    if (!ap_mode && webSocket.isConnected()) {
        size_t br = 0;
        if (i2s_read(I2S_IN_PORT, (char*)i2s_buffer, BUFFER_SIZE, &br, pdMS_TO_TICKS(100)) == ESP_OK && br > 0) {
            sendRtpAudio(i2s_buffer, br);
            
            size_t bw = 0;
            i2s_write(I2S_OUT_PORT, i2s_buffer, br, &bw, pdMS_TO_TICKS(100));
        }
    } else {
        delay(10);
    }
    
    static uint32_t ls = 0;
    if (millis() - ls > 5000) {
        LOG("pk=%d enc=%d heap=%dK mode=%d rec=%d ws=%d",
            packets_sent, encrypted_packets, ESP.getFreeHeap()/1024,
            device_mode, led_state.recording, led_state.ws_connected);
        packets_sent = 0;
        bytes_sent = 0;
        encrypted_packets = 0;
        ls = millis();
    }
}
