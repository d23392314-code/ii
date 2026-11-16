/*
 * Xiaozhi-Qwen untuk ESP32-S3 CAM
 *  - Dengar   : mic INMP441  -> Qwen Omni Flash (paham suara + foto, jawab teks)
 *  - Bicara   : ElevenLabs TTS (teks -> suara PCM) -> speaker MAX98357A
 *  - Tampilan : wajah emoji di LCD ST7789 240x240, mulut ikut gerak saat bicara
 *  - Setup    : WiFi "Xiaozhi-Setup" -> buka 192.168.4.1
 *               (isi WiFi, password, API key Qwen, API key ElevenLabs, Voice ID)
 *
 * Library: Adafruit GFX, Adafruit BusIO, Adafruit ST7735 and ST7789 Library, ArduinoJson (v7)
 * Board  : ESP32S3 Dev Module, Arduino-ESP32 core 3.x, PSRAM enabled
 *
 * Pemakaian:
 *   - Bicara ke mic -> AI menjawab dengan suara
 *   - Tekan BOOT sebentar -> foto, boleh tanya lewat suara (5 dtk), AI menjelaskan foto
 *   - Tahan BOOT 3 detik  -> reset WiFi, masuk mode setup lagi
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <vector>
#include "esp_camera.h"
#include "driver/i2s_std.h"
#include "mbedtls/base64.h"

// ================== PIN ==================
// LCD
#define LCD_SCK   19   // SCL
#define LCD_MOSI  20   // SDA
#define LCD_RST   21   // RES
#define LCD_DC    47
#define LCD_BLK   38
// Mic INMP441
#define MIC_WS    1
#define MIC_SCK   2
#define MIC_SD    42   // L/R ke GND
// Speaker MAX98357A
#define SPK_LRC   41
#define SPK_BCLK  40
#define SPK_DIN   39
// Tombol
#define BTN_PIN   0

// Kamera (default: Freenove ESP32-S3 WROOM CAM). Ganti kalau boardmu beda.
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_Y9    16
#define CAM_Y8    17
#define CAM_Y7    18
#define CAM_Y6    12
#define CAM_Y5    10
#define CAM_Y4    8
#define CAM_Y3    9
#define CAM_Y2    11
#define CAM_VSYNC 6
#define CAM_HREF  7
#define CAM_PCLK  13

// ================== SETTING ==================
#define DEFAULT_HOST   "dashscope-intl.aliyuncs.com"   // key region Singapore. Key China: dashscope.aliyuncs.com
#define DEFAULT_VOICE  "21m00Tcm4TlvDq8ikWAM"          // voice ID ElevenLabs (Rachel)
#define DEFAULT_VOL    80
#define CHAT_MODEL     "qwen3.8-omni-flash"
#define TTS_MODEL      "eleven_flash_v2_5"             // mendukung Bahasa Indonesia
#define TTS_HOST       "api.elevenlabs.io"

#define AP_NAME        "Xiaozhi-Setup"
#define SAMPLE_RATE    16000
#define CH             512
#define MAX_SECONDS    6
#define MAX_SAMPLES    (SAMPLE_RATE * MAX_SECONDS)
#define MIN_SAMPLES    12000
#define SILENCE_CH     31
#define PRE_CH         4
#define GAIN_SHIFT     14
#define VAD_MIN        250.0f
#define TTS_MAX_BYTES  (2048 * 1024)

const char* SYSTEM_PROMPT =
  "Kamu asisten suara kecil di robot kecil. Jawab dengan bahasa yang dipakai pengguna (default Indonesia). "
  "Jawaban SANGAT singkat, maksimal 2 kalimat atau 200 karakter, gaya ramah dan santai seperti ngobrol. "
  "Tanpa emoji, tanpa markdown, tanpa simbol aneh, tanpa daftar, hanya kalimat biasa yang enak diucapkan.";

// ================== GLOBAL ==================
Adafruit_ST7789 tft(&SPI, -1, LCD_DC, LCD_RST);
GFXcanvas16* mouthCv = NULL;
Preferences prefs;
WebServer server(80);
DNSServer dns;
i2s_chan_handle_t rxChan = NULL;
i2s_chan_handle_t txChan = NULL;

String cfgSsid, cfgPass, cfgKey, cfgXKey, cfgHost, cfgVoice;
int cfgVol = DEFAULT_VOL;
String lastErr;

uint8_t* pcmBuf = NULL;
int16_t* pcm = NULL;
float noiseFloor = 60.0f;
bool camOK = false;

// ================== WAJAH ==================
enum FaceMode { F_LISTEN, F_REC, F_THINK, F_SPEAK };
FaceMode faceMode = F_LISTEN;
uint32_t nextBlink = 0, blinkUntil = 0, lastThinkMove = 0;
int thinkStep = 0, mouthH = -1;
bool eyesClosed = false;
#define FACE_COL ST77XX_CYAN

void drawEyes(bool closed, int dx, int dy, bool tall = false) {
  tft.fillRect(0, 36, 240, 108, ST77XX_BLACK);
  int cy = 90 + dy;
  int xs[2] = {72 + dx, 168 + dx};
  int h = tall ? 62 : 52;
  for (int i = 0; i < 2; i++) {
    if (closed) tft.fillRoundRect(xs[i] - 20, cy - 3, 40, 7, 3, FACE_COL);
    else tft.fillRoundRect(xs[i] - 20, cy - h / 2, 40, h, 18, FACE_COL);
  }
}

// type: 0 senyum, 1 datar, 2 terbuka (h = tinggi), 3 bulat kecil
void drawMouth(int type, int h) {
  GFXcanvas16& c = *mouthCv;
  c.fillScreen(0);
  switch (type) {
    case 0:
      for (int i = -42; i <= 42; i++) c.fillCircle(60 + i, 34 - (i * i) / 55, 3, FACE_COL);
      break;
    case 1:
      c.fillRoundRect(25, 28, 70, 7, 3, FACE_COL);
      break;
    case 2: {
      int hh = constrain(h, 6, 56);
      int w = 56 + hh / 2;
      int r = hh / 2 > 16 ? 16 : hh / 2;
      c.fillRoundRect(60 - w / 2, 30 - hh / 2, w, hh, r, FACE_COL);
      break;
    }
    case 3:
      c.fillCircle(60, 30, 13, FACE_COL);
      c.fillCircle(60, 30, 8, 0);
      break;
  }
  tft.drawRGBBitmap(60, 150, c.getBuffer(), 120, 60);
}

void setFace(FaceMode m, const char* label, uint16_t col) {
  faceMode = m;
  mouthH = -1;
  eyesClosed = false;
  nextBlink = millis() + random(2000, 4500);
  tft.fillRect(0, 214, 240, 26, ST77XX_BLACK);
  tft.setTextColor(col);
  tft.setTextSize(2);
  tft.setCursor(6, 218);
  tft.print(label);
  switch (m) {
    case F_LISTEN: drawEyes(false, 0, 0); drawMouth(0, 0); break;
    case F_REC:    drawEyes(false, 0, 0, true); drawMouth(3, 0); break;
    case F_THINK:  drawEyes(false, 14, -10); drawMouth(1, 0); thinkStep = 0; lastThinkMove = millis(); break;
    case F_SPEAK:  drawEyes(false, 0, 0); drawMouth(2, 8); break;
  }
}

// dipanggil berkala saat menunggu: kedip & mata bergerak
void faceTick() {
  uint32_t now = millis();
  if (faceMode == F_THINK) {
    if (now - lastThinkMove > 600) {
      lastThinkMove = now;
      thinkStep = (thinkStep + 1) % 4;
      static const int dxs[4] = {-14, 0, 14, 0};
      drawEyes(false, dxs[thinkStep], -10);
    }
    return;
  }
  if (faceMode == F_REC) return;
  if (!eyesClosed && now > nextBlink) {
    eyesClosed = true; blinkUntil = now + 130; drawEyes(true, 0, 0);
  } else if (eyesClosed && now > blinkUntil) {
    eyesClosed = false; nextBlink = now + random(2500, 5500); drawEyes(false, 0, 0);
  }
}

// ================== LCD TEKS (setup & error) ==================
void showStatus(const char* s, uint16_t color) {
  tft.fillRect(0, 0, 240, 26, color);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(6, 5);
  tft.print(s);
}

void showBody(const String& t, uint16_t color = ST77XX_WHITE) {
  tft.fillRect(0, 28, 240, 212, ST77XX_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(0, 34);
  tft.print(t);
}

void showAnswer(String t) {
  String clean;
  for (size_t i = 0; i < t.length(); i++) {
    unsigned char c = t[i];
    if (c == '\n' || c == '\r' || c == '\t') clean += ' ';
    else if (c >= 32 && c < 127) clean += (char)c;
  }
  const int COLS = 20, ROWS = 12;
  std::vector<String> lines;
  String cur;
  int i = 0, L = clean.length();
  while (i < L) {
    int sp = clean.indexOf(' ', i);
    String w = (sp < 0) ? clean.substring(i) : clean.substring(i, sp);
    i = (sp < 0) ? L : sp + 1;
    if (w.length() == 0) continue;
    while ((int)w.length() > COLS) {
      if (cur.length()) { lines.push_back(cur); cur = ""; }
      lines.push_back(w.substring(0, COLS));
      w = w.substring(COLS);
    }
    if ((int)(cur.length() + (cur.length() ? 1 : 0) + w.length()) > COLS) { lines.push_back(cur); cur = w; }
    else { if (cur.length()) cur += ' '; cur += w; }
  }
  if (cur.length()) lines.push_back(cur);
  if (lines.empty()) lines.push_back("(kosong)");

  int pages = (lines.size() + ROWS - 1) / ROWS;
  if (pages > 3) pages = 3;
  for (int p = 0; p < pages; p++) {
    tft.fillRect(0, 28, 240, 212, ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    for (int r = 0; r < ROWS; r++) {
      int idx = p * ROWS + r;
      if (idx >= (int)lines.size()) break;
      tft.setCursor(0, 34 + r * 16);
      tft.print(lines[idx]);
    }
    delay(4500);
  }
}

// ================== PORTAL WIFI ==================
String htmlEsc(String s) {
  s.replace("&", "&amp;"); s.replace("\"", "&quot;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  return s;
}

String portalPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Xiaozhi Setup</title><style>"
               "body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}"
               ".c{max-width:420px;margin:auto;background:#1e293b;padding:20px;border-radius:14px}"
               "h2{margin-top:0}label{display:block;margin:14px 0 4px;font-size:14px;color:#94a3b8}"
               "input{width:100%;box-sizing:border-box;padding:12px;border-radius:8px;border:1px solid #334155;"
               "background:#0f172a;color:#fff;font-size:16px}"
               "button{width:100%;margin-top:20px;padding:14px;border:0;border-radius:8px;background:#22c55e;"
               "color:#000;font-size:17px;font-weight:bold}a{color:#38bdf8}small{color:#64748b}</style></head><body><div class='c'>"
               "<h2>Setup Xiaozhi-Qwen</h2><form method='POST' action='/save'>"
               "<label>Nama WiFi (SSID)</label><input name='ssid' list='ssids' required value='");
  h += htmlEsc(cfgSsid);
  h += F("'><datalist id='ssids'>");
  int n = WiFi.scanComplete();
  for (int i = 0; i < n; i++) h += "<option value='" + htmlEsc(WiFi.SSID(i)) + "'>";
  h += F("</datalist><label>Password WiFi</label><input name='pass' type='password' value='");
  h += htmlEsc(cfgPass);
  h += F("'><label>API Key Qwen (DashScope)</label><input name='key' placeholder='tempel API key Qwen' value='");
  h += htmlEsc(cfgKey);
  h += F("'><small>Untuk mendengar, berpikir, dan melihat foto.</small>"
         "<label>API Key ElevenLabs</label><input name='xkey' placeholder='tempel API key ElevenLabs' value='");
  h += htmlEsc(cfgXKey);
  h += F("'><small>Untuk suara (TTS).</small>"
         "<label>Host API Qwen</label><input name='host' value='");
  h += htmlEsc(cfgHost);
  h += F("'><small>Key Singapore/Internasional: dashscope-intl.aliyuncs.com<br>Key China: dashscope.aliyuncs.com</small>"
         "<label>Voice ID ElevenLabs</label><input name='voice' value='");
  h += htmlEsc(cfgVoice);
  h += F("'><small>Salin Voice ID dari ElevenLabs. Default: Rachel</small>"
         "<label>Volume (10-100)</label><input name='vol' type='number' min='10' max='100' value='");
  h += String(cfgVol);
  h += F("'><button type='submit'>Simpan &amp; Sambungkan</button></form>"
         "<p><a href='/'>Refresh daftar WiFi</a></p></div></body></html>");
  return h;
}

void runPortal() {
  showStatus("Setup WiFi", ST77XX_ORANGE);
  showBody("1. Sambung HP ke WiFi:\n\n  " AP_NAME "\n\n2. Buka browser:\n\n  192.168.4.1");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.scanNetworks(true);
  WiFi.softAP(AP_NAME);
  delay(300);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", portalPage()); });
  server.on("/save", HTTP_POST, []() {
    String k = server.arg("key"); k.trim();
    String xk = server.arg("xkey"); xk.trim();
    String hst = server.arg("host"); hst.trim();
    hst.replace("https://", ""); hst.replace("http://", "");
    while (hst.endsWith("/")) hst.remove(hst.length() - 1);
    if (hst.length() == 0) hst = DEFAULT_HOST;
    String v = server.arg("voice"); v.trim();
    if (v.length() == 0) v = DEFAULT_VOICE;
    int vol = server.arg("vol").toInt();
    if (vol < 10 || vol > 100) vol = DEFAULT_VOL;
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    prefs.putString("qkey", k);
    prefs.putString("xkey", xk);
    prefs.putString("qhost", hst);
    prefs.putString("qvoice", v);
    prefs.putInt("vol", vol);
    server.send(200, "text/html",
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<body style='font-family:sans-serif;background:#0f172a;color:#fff;padding:30px'>"
      "<h2>Tersimpan!</h2><p>Perangkat restart & menyambung ke WiFi. Kamu bisa tutup halaman ini.</p>");
    delay(1500);
    ESP.restart();
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  while (true) {
    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }
}

bool connectWiFi() {
  showStatus("Sambung WiFi", ST77XX_YELLOW);
  showBody(cfgSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// ================== TOMBOL ==================
int checkButton() {
  if (digitalRead(BTN_PIN) == HIGH) return 0;
  uint32_t t = millis();
  while (digitalRead(BTN_PIN) == LOW) {
    if (millis() - t > 3000) {
      showStatus("Reset WiFi...", ST77XX_RED);
      prefs.remove("ssid"); prefs.remove("pass");
      delay(800);
      ESP.restart();
    }
    delay(10);
  }
  return (millis() - t > 30) ? 1 : 0;
}

// ================== MIC ==================
// Isi config I2S manual (macro bawaan IDF 5.3 error di C++: urutan designator)
static i2s_std_clk_config_t makeClk(uint32_t rate) {
  i2s_std_clk_config_t c;
  memset(&c, 0, sizeof(c));
  c.sample_rate_hz = rate;
  c.clk_src = I2S_CLK_SRC_DEFAULT;
  c.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  return c;
}

static void makeStd(i2s_std_config_t& s, uint32_t rate, i2s_data_bit_width_t bw,
                    i2s_slot_mode_t mode, i2s_std_slot_mask_t mask,
                    int bclk, int ws, int dout, int din) {
  memset(&s, 0, sizeof(s));
  s.clk_cfg = makeClk(rate);
  s.slot_cfg.data_bit_width = bw;
  s.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  s.slot_cfg.slot_mode = mode;
  s.slot_cfg.slot_mask = mask;
  s.slot_cfg.ws_width = (uint32_t)bw;
  s.slot_cfg.ws_pol = false;
  s.slot_cfg.bit_shift = true;
  s.slot_cfg.left_align = true;
  s.slot_cfg.big_endian = false;
  s.slot_cfg.bit_order_lsb = false;
  s.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  s.gpio_cfg.bclk = (gpio_num_t)bclk;
  s.gpio_cfg.ws = (gpio_num_t)ws;
  s.gpio_cfg.dout = (gpio_num_t)dout;
  s.gpio_cfg.din = (gpio_num_t)din;
}

void micInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = 256;
  i2s_new_channel(&cc, NULL, &rxChan);
  i2s_std_config_t sc;
  makeStd(sc, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT,
          MIC_SCK, MIC_WS, I2S_GPIO_UNUSED, MIC_SD);
  i2s_channel_init_std_mode(rxChan, &sc);
  i2s_channel_enable(rxChan);
}

// ================== SPEAKER ==================
void spkInit() {
  i2s_chan_config_t tc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  tc.dma_desc_num = 6;
  tc.dma_frame_num = 256;
  tc.auto_clear = true;
  i2s_new_channel(&tc, &txChan, NULL);
  i2s_std_config_t ts;
  makeStd(ts, 24000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH,
          SPK_BCLK, SPK_LRC, SPK_DIN, I2S_GPIO_UNUSED);
  i2s_channel_init_std_mode(txChan, &ts);   // belum di-enable; di-enable saat memutar
}

// Putar PCM 16-bit sambil menggerakkan mulut sesuai kerasnya suara
void playPcm(const uint8_t* data, size_t dlen, uint32_t rate, uint16_t chn) {
  if (chn == 0) return;
  i2s_std_clk_config_t clk = makeClk(rate);
  i2s_channel_reconfig_std_clock(txChan, &clk);
  i2s_channel_enable(txChan);

  const int16_t* s = (const int16_t*)data;
  size_t frames = dlen / 2 / chn;
  float vol = cfgVol / 100.0f;
  static int16_t st[256 * 2];
  int smoothH = 8, blk = 0;

  for (size_t i = 0; i < frames; i += 256) {
    size_t n = frames - i;
    if (n > 256) n = 256;
    uint32_t acc = 0;
    for (size_t j = 0; j < n; j++) {
      int32_t v = (int32_t)(s[(i + j) * chn] * vol);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      st[j * 2] = st[j * 2 + 1] = (int16_t)v;
      acc += abs(v);
    }
    size_t w = 0;
    i2s_channel_write(txChan, st, n * 4, &w, 2000);

    int lvl = acc / (n ? n : 1);
    int target = 6 + min(50, lvl / 80);
    smoothH = (smoothH * 2 + target) / 3;
    blk++;
    if (blk % 3 == 0 && abs(smoothH - mouthH) >= 3) { mouthH = smoothH; drawMouth(2, smoothH); }
    if (blk % 12 == 0) faceTick();
  }
  delay(120);
  i2s_channel_disable(txChan);
  drawMouth(0, 0);
}

// ================== REKAM SUARA ==================
float readChunk(int16_t* out) {
  static int32_t raw[CH];
  static float dc = 0;
  size_t br = 0;
  if (i2s_channel_read(rxChan, raw, sizeof(raw), &br, 1000) != ESP_OK || br == 0) return -1;
  int n = br / 4;
  float sum = 0;
  for (int i = 0; i < n; i++) {
    float v = (float)(raw[i] >> GAIN_SHIFT);
    dc += (v - dc) * 0.002f;
    v -= dc;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
    sum += fabsf(v);
  }
  for (int i = n; i < CH; i++) out[i] = 0;
  return sum / (n ? n : 1);
}

void flushMic(int count = 12) {
  int16_t tmp[CH];
  for (int i = 0; i < count; i++) readChunk(tmp);
}

// return: jumlah sample, 0 = tidak ada suara, -1 = tombol BOOT ditekan
int recordSpeech(uint32_t maxWaitMs, bool pollBtn) {
  static int16_t pre[PRE_CH][CH];
  int16_t chunk[CH];
  int preIdx = 0, preCount = 0, hot = 0;
  uint32_t t0 = millis(), lastLog = 0;
  float thr = 0;

  while (true) {
    if (pollBtn && checkButton() == 1) return -1;
    if (maxWaitMs && millis() - t0 > maxWaitMs) return 0;
    faceTick();
    float lvl = readChunk(chunk);
    if (lvl < 0) continue;
    memcpy(pre[preIdx], chunk, sizeof(chunk));
    preIdx = (preIdx + 1) % PRE_CH;
    if (preCount < PRE_CH) preCount++;
    thr = noiseFloor * 3.0f + VAD_MIN;
    if (lvl > thr) hot++;
    else { hot = 0; noiseFloor = noiseFloor * 0.98f + lvl * 0.02f; }
    if (millis() - lastLog > 2000) {
      lastLog = millis();
      Serial.printf("level=%.0f noise=%.0f thr=%.0f\n", lvl, noiseFloor, thr);
    }
    if (hot >= 3) break;
  }

  setFace(F_REC, "Merekam...", ST77XX_RED);
  size_t n = 0;
  int start = (preIdx - preCount + PRE_CH) % PRE_CH;
  for (int i = 0; i < preCount; i++) {
    memcpy(pcm + n, pre[(start + i) % PRE_CH], CH * 2);
    n += CH;
  }
  int quiet = 0;
  while (n + CH <= MAX_SAMPLES) {
    float lvl = readChunk(chunk);
    if (lvl < 0) continue;
    memcpy(pcm + n, chunk, CH * 2);
    n += CH;
    if (lvl > thr * 0.7f) quiet = 0; else quiet++;
    if (quiet > SILENCE_CH) break;
  }
  return (n < MIN_SAMPLES) ? 0 : (int)n;
}

void writeWavHeader(uint8_t* b, uint32_t dataLen) {
  uint32_t v32; uint16_t v16;
  memcpy(b, "RIFF", 4);  v32 = 36 + dataLen; memcpy(b + 4, &v32, 4);
  memcpy(b + 8, "WAVEfmt ", 8);
  v32 = 16;              memcpy(b + 16, &v32, 4);
  v16 = 1;               memcpy(b + 20, &v16, 2);
  v16 = 1;               memcpy(b + 22, &v16, 2);
  v32 = SAMPLE_RATE;     memcpy(b + 24, &v32, 4);
  v32 = SAMPLE_RATE * 2; memcpy(b + 28, &v32, 4);
  v16 = 2;               memcpy(b + 32, &v16, 2);
  v16 = 16;              memcpy(b + 34, &v16, 2);
  memcpy(b + 36, "data", 4);
  v32 = dataLen;         memcpy(b + 40, &v32, 4);
}

// ================== KAMERA ==================
bool camInit() {
  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
  c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK;
  c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN; c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

// ================== HTTPS ==================
String dechunk(const String& s) {
  String o;
  int i = 0, L = s.length();
  while (i < L) {
    int e = s.indexOf("\r\n", i);
    if (e < 0) break;
    long sz = strtol(s.substring(i, e).c_str(), NULL, 16);
    if (sz <= 0) break;
    o.concat(s.c_str() + e + 2, (unsigned int)sz);
    i = e + 2 + sz + 2;
  }
  return o;
}

// POST lewat TLS, body dikirim per 1 KB. return kode HTTP, -1 gagal konek, -2 gagal kirim, -3 timeout
int httpsPost(const char* host, const String& path, const uint8_t* body, size_t len, String& out) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);
  if (!client.connect(host, 443)) return -1;

  String hdr = String("POST ") + path + " HTTP/1.0\r\n" +
               "Host: " + host + "\r\n" +
               "Authorization: Bearer " + cfgKey + "\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + String((unsigned)len) + "\r\n" +
               "Connection: close\r\n\r\n";
  client.print(hdr);

  size_t sent = 0;
  int fails = 0, cnt = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > 1024) n = 1024;
    size_t w = client.write(body + sent, n);
    if (w == 0) {
      if (++fails > 150 || !client.connected()) { client.stop(); return -2; }
      delay(20);
      continue;
    }
    fails = 0;
    sent += w;
    if (++cnt % 24 == 0) faceTick();
    delay(1);
  }

  String raw;
  uint8_t buf[512];
  uint32_t last = millis();
  while ((client.connected() || client.available()) && millis() - last < 60000) {
    int n = client.read(buf, sizeof(buf));
    if (n > 0) { raw.concat((const char*)buf, n); last = millis(); }
    else { faceTick(); delay(10); }
  }
  client.stop();
  if (raw.length() < 12) return -3;

  int code = raw.substring(9, 12).toInt();
  int b = raw.indexOf("\r\n\r\n");
  String headers = (b >= 0) ? raw.substring(0, b) : raw;
  headers.toLowerCase();
  out = (b >= 0) ? raw.substring(b + 4) : String("");
  if (headers.indexOf("transfer-encoding: chunked") >= 0) out = dechunk(out);
  return code;
}

// ================== QWEN: CHAT (dengar + foto) ==================
static size_t b64Len(size_t n) { return 4 * ((n + 2) / 3); }

static size_t appendStr(uint8_t* dst, size_t pos, const String& s) {
  memcpy(dst + pos, s.c_str(), s.length());
  return pos + s.length();
}

static size_t appendB64(uint8_t* dst, size_t pos, const uint8_t* src, size_t len) {
  size_t olen = 0;
  mbedtls_base64_encode(dst + pos, b64Len(len) + 1, &olen, src, len);
  return pos + olen;
}

String jsonEsc(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n' || c == '\r' || c == '\t') o += ' ';
    else if ((uint8_t)c < 32) { }
    else o += c;
  }
  return o;
}

// Ambil teks dari balasan streaming (SSE) Qwen
String parseSSE(const String& sse) {
  String out;
  int i = 0, L = sse.length();
  while (i < L) {
    int e = sse.indexOf('\n', i);
    if (e < 0) e = L;
    String line = sse.substring(i, e);
    i = e + 1;
    line.trim();
    if (!line.startsWith("data:")) continue;
    line = line.substring(5);
    line.trim();
    if (line.length() == 0 || line == "[DONE]") continue;
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    const char* c = d["choices"][0]["delta"]["content"] | "";
    out += c;
  }
  if (out.length() == 0) {                    // jaga-jaga kalau balasannya JSON biasa
    JsonDocument d;
    if (!deserializeJson(d, sse)) {
      const char* c = d["choices"][0]["message"]["content"] | "";
      out = c;
    }
  }
  return out;
}

bool askLLM(const uint8_t* wav, size_t wavLen, const uint8_t* jpg, size_t jpgLen, const char* prompt, String& result) {
  if (cfgKey.length() < 10) { result = "API key belum diisi. Tahan BOOT 3 dtk untuk setup."; return false; }

  String head = String("{\"model\":\"") + CHAT_MODEL + "\",\"stream\":true,\"reasoning_effort\":\"none\","
                "\"modalities\":[\"text\"],\"max_tokens\":300,\"messages\":["
                "{\"role\":\"system\",\"content\":\"" + SYSTEM_PROMPT + "\"},"
                "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"" + prompt + "\"}";
  String imgHead = ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
  String imgTail = "\"}}";
  String audHead = ",{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:;base64,";
  String audTail = "\",\"format\":\"wav\"}}";
  String tail = "]}]}";

  size_t total = head.length() + tail.length() + 16;
  if (jpg) total += imgHead.length() + b64Len(jpgLen) + imgTail.length();
  if (wav) total += audHead.length() + b64Len(wavLen) + audTail.length();

  uint8_t* body = (uint8_t*)ps_malloc(total + 8);
  if (!body) { result = "Memori tidak cukup (PSRAM aktif?)"; return false; }

  size_t p = 0;
  p = appendStr(body, p, head);
  if (jpg) { p = appendStr(body, p, imgHead); p = appendB64(body, p, jpg, jpgLen); p = appendStr(body, p, imgTail); }
  if (wav) { p = appendStr(body, p, audHead); p = appendB64(body, p, wav, wavLen); p = appendStr(body, p, audTail); }
  p = appendStr(body, p, tail);

  String resp;
  int code = httpsPost(cfgHost.c_str(), "/compatible-mode/v1/chat/completions", body, p, resp);
  free(body);
  Serial.printf("CHAT HTTP %d, %u byte\n", code, (unsigned)resp.length());

  if (code == -1) { result = "Gagal konek ke server Qwen (cek internet/host)"; return false; }
  if (code == -2) { result = "Gagal kirim data (koneksi lambat). Bicara lebih singkat."; return false; }
  if (code == -3) { result = "Server tidak merespon (timeout)"; return false; }

  if (code != 200) {
    JsonDocument d;
    String m = String("Error ") + code;
    if (!deserializeJson(d, resp)) {
      const char* em = d["error"]["message"] | (const char*)(d["message"] | "");
      if (strlen(em)) m += String(": ") + em;
    }
    result = m;
    return false;
  }
  result = parseSSE(resp);
  result.trim();
  if (result.length() == 0) { result = "Jawaban kosong dari AI"; return false; }
  return true;
}

// ================== ELEVENLABS: TTS ==================
String cleanForSpeech(const String& t) {
  String o;
  for (size_t i = 0; i < t.length(); i++) {
    char c = t[i];
    if (c == '*' || c == '#' || c == '`') continue;
    o += c;
  }
  o.trim();
  if (o.length() > 560) {
    o = o.substring(0, 560);
    int sp = o.lastIndexOf(' ');
    if (sp > 400) o = o.substring(0, sp);
  }
  return o;
}

bool ttsSpeak(const String& text) {
  String t = cleanForSpeech(text);
  String body = String("{\"text\":\"") + jsonEsc(t) + "\",\"model_id\":\"" + TTS_MODEL + "\"}";

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(30);
  if (!c.connect(TTS_HOST, 443)) { lastErr = "Gagal konek ElevenLabs"; return false; }
  c.print(String("POST /v1/text-to-speech/") + cfgVoice + "?output_format=pcm_24000 HTTP/1.0\r\n" +
          "Host: " + TTS_HOST + "\r\n" +
          "xi-api-key: " + cfgXKey + "\r\n" +
          "Content-Type: application/json\r\n" +
          "Content-Length: " + String((unsigned)body.length()) + "\r\n" +
          "Connection: close\r\n\r\n");
  c.print(body);

  int code = 0;
  bool chunked = false, hdrDone = false;
  uint32_t t0 = millis();
  while ((c.connected() || c.available()) && millis() - t0 < 30000) {
    String line = c.readStringUntil('\n');
    line.trim();
    if (line.startsWith("HTTP/")) code = line.substring(9, 12).toInt();
    else if (line.length() == 0) { hdrDone = true; break; }
    else { line.toLowerCase(); if (line.indexOf("transfer-encoding: chunked") >= 0) chunked = true; }
  }
  Serial.printf("TTS HTTP %d\n", code);
  if (!hdrDone || code != 200) {
    String err = c.readString();
    c.stop();
    lastErr = String("ElevenLabs ") + code;
    JsonDocument d;
    if (!deserializeJson(d, err)) {
      const char* em = d["detail"]["message"] | (const char*)(d["detail"]["status"] | "");
      if (strlen(em)) lastErr += String(": ") + em;
    }
    return false;
  }

  uint8_t* buf = (uint8_t*)ps_malloc(TTS_MAX_BYTES);
  if (!buf) { c.stop(); lastErr = "Memori audio kurang"; return false; }
  size_t n = 0;
  uint32_t last = millis();

  if (chunked) {
    while (n < TTS_MAX_BYTES && millis() - last < 20000) {
      String ln = c.readStringUntil('\n');
      long sz = strtol(ln.c_str(), NULL, 16);
      if (sz <= 0) break;
      long got = 0;
      while (got < sz && n < TTS_MAX_BYTES && millis() - last < 20000) {
        size_t want = sz - got;
        if (want > TTS_MAX_BYTES - n) want = TTS_MAX_BYTES - n;
        int r = c.read(buf + n, want);
        if (r > 0) { n += r; got += r; last = millis(); }
        else { faceTick(); delay(5); }
      }
      c.read(); c.read();   // CRLF penutup chunk
    }
  } else {
    while ((c.connected() || c.available()) && n < TTS_MAX_BYTES && millis() - last < 20000) {
      size_t want = TTS_MAX_BYTES - n;
      if (want > 2048) want = 2048;
      int r = c.read(buf + n, want);
      if (r > 0) { n += r; last = millis(); }
      else { faceTick(); delay(5); }
    }
  }
  c.stop();
  n &= ~(size_t)1;
  if (n < 2000) { free(buf); lastErr = "Audio ElevenLabs kosong"; return false; }

  playPcm(buf, n, 24000, 1);   // pcm_24000 = 16-bit mono 24 kHz
  free(buf);
  return true;
}

// ================== ALUR UTAMA ==================
void converse(const uint8_t* wav, size_t wavLen, const uint8_t* jpg, size_t jpgLen, const char* prompt) {
  setFace(F_THINK, "Berpikir...", ST77XX_YELLOW);
  String r;
  bool ok = askLLM(wav, wavLen, jpg, jpgLen, prompt, r);
  if (!ok) {
    showStatus("Error", ST77XX_RED);
    showAnswer(r);
    return;
  }
  Serial.println("AI: " + r);
  setFace(F_SPEAK, "Berbicara", ST77XX_CYAN);
  if (!ttsSpeak(r)) {
    showStatus("TTS gagal", ST77XX_ORANGE);
    showAnswer(lastErr + ". Jawaban: " + r);
  }
  delay(300);
}

void doPhoto() {
  if (!camOK) { showStatus("Kamera error", ST77XX_RED); showBody("Kamera tidak terdeteksi.\nCek pin kamera."); delay(2500); return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (!fb) { showStatus("Foto gagal", ST77XX_RED); delay(2000); return; }

  tft.fillScreen(ST77XX_WHITE);            // efek blitz
  delay(70);
  tft.fillScreen(ST77XX_BLACK);
  setFace(F_LISTEN, "Tanya? (5 dtk)", ST77XX_GREEN);
  flushMic();
  int n = recordSpeech(5000, false);

  if (n > 0) {
    writeWavHeader(pcmBuf, n * 2);
    converse(pcmBuf, 44 + n * 2, fb->buf, fb->len,
             "Ini foto dari kamera. Audio berikut adalah pertanyaan pengguna tentang foto ini. Jawab pertanyaannya.");
  } else {
    converse(NULL, 0, fb->buf, fb->len, "Jelaskan apa yang terlihat di foto ini secara singkat.");
  }
  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LCD_BLK, OUTPUT);
  digitalWrite(LCD_BLK, HIGH);

  SPI.begin(LCD_SCK, -1, LCD_MOSI, -1);
  tft.init(240, 240, SPI_MODE3);
  tft.setSPISpeed(40000000);
  tft.setRotation(0);                       // coba 1/2/3 kalau layar terbalik
  tft.fillScreen(ST77XX_BLACK);
  mouthCv = new GFXcanvas16(120, 60);
  showStatus("Booting...", ST77XX_WHITE);

  if (!psramFound()) { showBody("PSRAM tidak aktif!\nTools > PSRAM > Enabled", ST77XX_RED); while (1) delay(1000); }
  pcmBuf = (uint8_t*)ps_malloc(44 + MAX_SAMPLES * 2);
  pcm = (int16_t*)(pcmBuf + 44);

  camOK = camInit();
  Serial.println(camOK ? "Kamera OK" : "Kamera GAGAL");

  prefs.begin("cfg", false);
  cfgSsid  = prefs.getString("ssid", "");
  cfgPass  = prefs.getString("pass", "");
  cfgKey   = prefs.getString("qkey", "");
  cfgXKey  = prefs.getString("xkey", "");
  cfgHost  = prefs.getString("qhost", DEFAULT_HOST);
  cfgVoice = prefs.getString("qvoice", DEFAULT_VOICE);
  if (cfgVoice.length() < 15) cfgVoice = DEFAULT_VOICE;   // buang voice lama ("Cherry")
  cfgVol   = prefs.getInt("vol", DEFAULT_VOL);

  if (cfgSsid.length() == 0 || !connectWiFi() || cfgKey.length() < 10 || cfgXKey.length() < 10) {
    runPortal();                            // restart setelah disimpan
  }

  micInit();
  spkInit();
  randomSeed(micros());
  tft.fillScreen(ST77XX_BLACK);
  setFace(F_LISTEN, "Siap!", ST77XX_GREEN);
  delay(1200);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi putus", ST77XX_RED);
    WiFi.reconnect();
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
    return;
  }

  setFace(F_LISTEN, "Mendengarkan", ST77XX_GREEN);
  flushMic();
  int n = recordSpeech(0, true);

  if (n == -1) { doPhoto(); return; }
  if (n > 0) {
    writeWavHeader(pcmBuf, n * 2);
    converse(pcmBuf, 44 + n * 2, NULL, 0,
             "Dengarkan audio berikut (suara pengguna) lalu jawab langsung pertanyaan atau permintaannya.");
  }
}
