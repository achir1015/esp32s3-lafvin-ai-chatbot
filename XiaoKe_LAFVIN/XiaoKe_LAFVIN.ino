// ============================================================================
//  LAFVIN ESP32S3 AIChatBot 桌上型機器人「小柯」（OpenAI 版）
//  硬體：ESP32-S3（N16R8）+ LAFVIN AI Chatbot 擴充板 + 音訊模組（ES8311，可能另有 ES7210）
//        + 2.0" ST7789 320x240 + BOOT / UP / DOWN 按鍵
//  流程：直接說話（聲控自動偵測，或按住 BOOT）→ OpenAI 語音轉文字 → GPT 回答 → OpenAI TTS 播放
//  螢幕：日期、24 小時制時間、農曆 + 機器人表情（眨眼、東張西望、聆聽/思考/說話/想睡）+ 農民曆
//        + 病房助理工作站（班表、工作流程、工作站工具 QR 碼與語音問答）
//  移植自七吋 P4 版與 SparkBot 版小柯；開發環境 Arduino IDE 2.x + esp32 by Espressif 3.x
// ============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>
#include <vector>
#include <initializer_list>
#include "img_converters.h"
#include "qrcode.h"
#include "soc/usb_serial_jtag_reg.h"
#include "mp3dec.h"
#if __has_include("config.h")
#include "config.h"          // 自己的設定（不上傳 GitHub）
#else
#include "config.example.h"  // 範本：Wi-Fi 與 API Key 可開機後用網頁設定
#endif
#include "lafvin_hw.h"
#include "cjk_font.h"
#include "lunar_table.h"
#include "almanac.h"
#include "web_page.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // HTTPS + JSON（班表、工作流程）需要較大的堆疊

// ------------------------------------------------------------------ 顏色 ----
constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFD20
#define C_GRAY    0x8410
#define C_BG      0x0000
#define C_BAR_LINE RGB(40, 52, 72)      // 分隔線
#define C_HELMET   RGB(232, 236, 242)   // 頭盔：白
#define C_VISOR    RGB(16, 30, 50)      // 面罩：深藍
#define C_GLOW     RGB(64, 232, 255)    // 發光表情：青色
#define C_GLOW_DIM RGB(30, 110, 135)
#define C_RED2     RGB(255, 70, 100)    // 生氣
#define C_PINK     RGB(255, 110, 150)   // 愛心／腮紅
#define C_PANEL    RGB(14, 22, 38)      // 農民曆卡片底色
#define C_BTN      RGB(40, 150, 200)    // 工作站按鈕

#define SCREEN_W 320
#define SCREEN_H 240
#define BAR_H    40           // 上方日期時間列高度
#define LINE_H   17           // 中文字行高

// ------------------------------------------------------------------ 螢幕 ----
SPIClass tftSPI(HSPI);
Adafruit_ST7789 tft(&tftSPI, LF_LCD_CS, LF_LCD_DC, LF_LCD_RST);
GFXcanvas16 *cv = nullptr;    // 整個畫面先畫在 PSRAM，再一次送出，不會閃爍

static inline void hspan(int x0, int x1, int y, uint16_t c) {
  if ((unsigned)y >= SCREEN_H) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
  uint16_t *p = cv->getBuffer() + y * SCREEN_W;
  for (int x = x0; x <= x1; x++) p[x] = c;
}
static uint16_t blend565(uint16_t a, uint16_t b, float t) {         // t = 0 → a，1 → b
  if (t <= 0) return a;
  if (t >= 1) return b;
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}
// 圓角矩形（上下漸層）
static void fillRRGrad(int x, int y, int w, int h, int r, uint16_t cTop, uint16_t cBot) {
  r = min(r, min(w, h) / 2);
  for (int yy = 0; yy < h; yy++) {
    int inset = 0;
    float dy = yy < r ? r - yy - 0.5f : yy >= h - r ? yy - (h - r) + 0.5f : -1;
    if (dy >= 0) inset = r - (int)sqrtf(max(0.0f, r * r - dy * dy));
    hspan(x + inset, x + w - 1 - inset, y + yy, h > 1 ? blend565(cTop, cBot, (float)yy / (h - 1)) : cTop);
  }
}
static void fillRR(int x, int y, int w, int h, int r, uint16_t c) { fillRRGrad(x, y, w, h, r, c, c); }
static void fillEllipse(int cx, int cy, int rx, int ry, uint16_t c) {
  if (rx < 1 || ry < 1) { hspan(cx - rx, cx + rx, cy, c); return; }
  for (int dy = -ry; dy <= ry; dy++) {
    int dx = (int)(rx * sqrtf(max(0.0f, 1.0f - (float)(dy * dy) / (ry * ry))));
    hspan(cx - dx, cx + dx, cy + dy, c);
  }
}
static void fillCircleP(int cx, int cy, int r, uint16_t c) { fillEllipse(cx, cy, r, r, c); }
// 粗線（兩端圓頭）
static void strokeLine(float x0, float y0, float x1, float y1, int th, uint16_t c) {
  float len = hypotf(x1 - x0, y1 - y0);
  int n = max(1, (int)(len / max(1.0f, th / 3.0f)));
  for (int i = 0; i <= n; i++) fillCircleP(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, th / 2, c);
}
// 粗弧線：角度以度為單位，0 = 右，90 = 下（螢幕座標）
static void strokeArc(int cx, int cy, int r, float a0, float a1, int th, uint16_t c) {
  float step = max(1.0f, th / 3.0f) / max(1, r) * 57.3f;
  for (float a = a0; a <= a1 + 0.01f; a += step)
    fillCircleP(cx + r * cosf(a / 57.3f), cy + r * sinf(a / 57.3f), th / 2, c);
}
static void fillHeart(int x, int y, int r, uint16_t c) {
  int h = r * 55 / 100;
  fillCircleP(x - r / 2, y, h, c);
  fillCircleP(x + r / 2, y, h, c);
  for (int yy = 0; yy <= r * 6 / 5 - r / 6; yy++) {
    int half = (r + 1) * (r * 6 / 5 - r / 6 - yy) / max(1, r * 6 / 5 - r / 6);
    hspan(x - half, x + half, y + r / 6 + yy, c);
  }
}

// 16px 繁中點陣字（GNU Unifont，見 cjk_font.h），y 為基線，透明背景
class CjkText {
 public:
  Adafruit_GFX *g = &tft;
  void setTarget(Adafruit_GFX *t) { g = t; }
  void setForegroundColor(uint16_t c) { fg = c; }

  int getUTF8Width(const char *s) {
    int w = 0;
    while (*s) w += glyphWidth(nextCodepoint(s));
    return w;
  }

  void drawUTF8(int x, int y, const char *s) {
    y -= 14;
    while (*s) {
      uint32_t cp = nextCodepoint(s);
      int idx = findGlyph(cp);
      int w = glyphWidth(cp);
      if (x + w > g->width()) break;
      if (idx >= 0) {
        const uint8_t *b = &cjkBitmaps[idx * 32];
        for (int r = 0; r < 16; r++) {
          uint16_t bits = (b[r * 2] << 8) | b[r * 2 + 1];
          for (int c = 0; c < w; c++)
            if (bits & (0x8000 >> c)) g->drawPixel(x + c, y + r, fg);
        }
      }
      x += w;
    }
  }

  // UTF-8 自動換行：英文單字/網址/數字不拆開，標點符號不放在行首（允許超出 16px）
  std::vector<String> wrap(const String &s, int w) {
    static const char *NO_START = "，。、；：！？」』）》〉…,.!?;:)]";
    std::vector<String> lines;
    String line; int lineW = 0;
    size_t i = 0;
    while (i < s.length()) {
      uint8_t c = s[i];
      if (c == '\r') { i++; continue; }
      if (c == '\n') { lines.push_back(line); line = ""; lineW = 0; i++; continue; }
      String tok;
      if (c < 0x80 && c != ' ') {                     // 連續英數字當成一個字
        size_t j = i;
        while (j < s.length() && (uint8_t)s[j] < 0x80 && s[j] != ' ' && s[j] != '\n') j++;
        tok = s.substring(i, j); i = j;
      } else {
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        tok = s.substring(i, i + len); i += len;
      }
      int tw = getUTF8Width(tok.c_str());
      bool noStart = strstr(NO_START, tok.c_str()) != nullptr;
      if (lineW > 0 && lineW + tw > w && !(noStart && lineW + tw <= w + 16)) {
        lines.push_back(line); line = ""; lineW = 0;
        if (tok == " ") continue;
      }
      if (tw > w) {                                   // 超長英文：逐字硬切
        for (size_t k = 0; k < tok.length(); k++) {
          if (lineW + 8 > w) { lines.push_back(line); line = ""; lineW = 0; }
          line += tok[k]; lineW += 8;
        }
      } else { line += tok; lineW += tw; }
    }
    if (line.length()) lines.push_back(line);
    return lines;
  }

 private:
  uint16_t fg = 0xFFFF;

  static uint32_t nextCodepoint(const char *&s) {
    uint8_t c = *s++;
    if (c < 0x80) return c;
    int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : 3;
    uint32_t cp = c & (0x3F >> n);
    while (n-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
    return cp;
  }
  static int findGlyph(uint32_t cp) {
    if (cp > 0xFFFF) return -1;
    int lo = 0, hi = CJK_FONT_COUNT - 1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (cjkCodes[m] == cp) return m;
      if (cjkCodes[m] < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
  }
  static int glyphWidth(uint32_t cp) {
    int i = findGlyph(cp);
    if (i < 0) return cp < 0x80 ? 8 : 16;
    return (cjkWide[i / 8] >> (i % 8)) & 1 ? 16 : 8;
  }
};
CjkText u8f;

// 超過寬度就截斷加「…」
String clipText(const String &s, int w) {
  if (u8f.getUTF8Width(s.c_str()) <= w) return s;
  String out;
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    String next = out + s.substring(i, i + len);
    if (u8f.getUTF8Width((next + "…").c_str()) > w) break;
    out = next; i += len;
  }
  return out + "…";
}

// ---------------------------------------------------------------- UI 狀態 ----
enum UiState { UI_IDLE, UI_LISTEN, UI_THINK, UI_SPEAK, UI_MSG, UI_HELP, UI_SONG, UI_QR, UI_PORTAL };
volatile UiState uiState = UI_IDLE;
String uiTitle, uiText;
uint32_t uiSince = 0;
bool uiSad = false;
SemaphoreHandle_t uiLock = nullptr;
volatile int micLevel = 0;         // 目前麥克風音量（給 UI 顯示）
volatile int speakLevel = 0;       // 目前播放音量（嘴巴開合）
volatile uint32_t lastInteraction = 0;

// 「主畫面」：對話結束後回到這裡（一般主畫面、唱歌、工作站選單、QR 碼）
UiState homeState = UI_IDLE;
String homeTitle, homeText;
uint32_t homeSince = 0;

// ---- 唱歌：YouTube 歌單 ----
struct Song { String id, title; };
std::vector<Song> songs;
SemaphoreHandle_t songLock = nullptr;
uint16_t *thumbPix = nullptr;      // 歌曲封面 320x180 RGB565（PSRAM）
volatile bool thumbOk = false;
std::vector<Mp3Song> mp3Songs;     // 雲端硬碟 MP3 歌曲
volatile bool songPlaying = false, songLoading = false;   // 播放雲端 MP3 中／下載中
volatile uint32_t songPosMs = 0, songDurMs = 0;
#define QR_MAX 57                  // QR 碼最多 version 10 = 57x57
uint8_t qrMods[QR_MAX * QR_MAX];
volatile int qrSize = 0;

bool containsAny(const String &s, std::initializer_list<const char *> words);

void setUi(UiState s, const String &title = "", const String &text = "", bool sad = false) {
  xSemaphoreTake(uiLock, portMAX_DELAY);
  uiState = s; uiTitle = title; uiText = text; uiSad = sad; uiSince = millis();
  xSemaphoreGive(uiLock);
}
void goHome(UiState s, const String &title = "", const String &text = "") {
  homeState = s; homeTitle = title; homeText = text; homeSince = millis();
  setUi(s, title, text);
}
void restoreHome() { setUi(homeState, homeTitle, homeText); }

// ------------------------------------------------------------------ 音訊 ----
// 喇叭與麥克風共用一組全雙工 I2S：24kHz / 16-bit / 立體聲
I2SClass lfI2S;
volatile int volumePct = DEFAULT_VOLUME;
bool micES7210 = false;            // 音訊模組有 ES7210 = 用它收音；沒有 = 用 ES8311 的 ADC

#define MIC_SAMPLE_RATE 16000                  // 送給 OpenAI 的錄音：24kHz 降頻成 16kHz
#define TTS_SAMPLE_RATE I2S_RATE               // OpenAI TTS pcm 固定 24kHz / 16bit / mono

#define TTS_BUF_BYTES (3 * 1024 * 1024)        // 約 65 秒語音
#define TTS_PREBUFFER (TTS_SAMPLE_RATE * 2 / 2)  // 先緩衝 0.5 秒
struct {
  uint8_t *buf = nullptr;
  volatile size_t written = 0;
  volatile size_t played = 0;
  volatile bool downloadDone = true;
  volatile bool playing = false;
} tts;
TaskHandle_t playTaskHandle;

// 錄音：44 bytes WAV header + PCM（PSRAM）
uint8_t *wavBuf = nullptr;
const size_t MAX_SAMPLES = MIC_SAMPLE_RATE * MAX_RECORD_SEC;
#define FRAME       320                        // 20ms @16kHz
#define PREROLL_FR  15                         // 保留觸發前 300ms，避免吃掉第一個字
int16_t preroll[PREROLL_FR][FRAME];
int prerollHead = 0, prerollCount = 0;
float noiseFloor = 150;
float lastRecAvgRms = 0;           // 上一段錄音的平均音量
bool lastRecHitMax = false;        // 上一段錄音是否一路沒停頓錄到上限（多半是音樂/噪音）
const float NOISE_FLOOR_MAX = 3000;

// 聽到的是噪音或音樂：把門檻提高到這個音量，避免同樣的聲音一直觸發（安靜後會自動降回）
void raiseNoiseFloor(float level) {
  float nf = min(NOISE_FLOOR_MAX, max(noiseFloor, level));
  if (nf > noiseFloor) Serial.printf("（噪音 %.0f → 門檻提高到 %d）\n", level, (int)(nf * VAD_RATIO));
  noiseFloor = nf;
}
int32_t dcState = 0;

// ------------------------------------------------------------------ 其它 ----
String histRole[MAX_HISTORY_MSGS];
String histText[MAX_HISTORY_MSGS];
int histCount = 0;
String chatContext;                // 查到的班表 / 工作流程 / 工作站資料，附在這次問 AI 的指示裡
const char *OPENAI_HOST = "https://api.openai.com";

// ---- 可由網頁修改的設定（存在 NVS；沒存過就用 config.h 的值）----
struct Settings { String ssid, pass, apiKey, playlist; } cfg;
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
volatile bool portalMode = false;
char ipText[24] = "";
#define AP_SSID "XiaoKe-Setup"
#define AP_PASS "xiaoke123"

String cleanDefault(const char *v, const char *placeholder) {
  return strstr(v, placeholder) ? String("") : String(v);
}
void loadSettings() {
  prefs.begin("robot", true);
  cfg.ssid   = prefs.isKey("ssid")   ? prefs.getString("ssid")   : cleanDefault(WIFI_SSID, "你的WiFi");
  cfg.pass   = prefs.isKey("pass")   ? prefs.getString("pass")   : cleanDefault(WIFI_PASSWORD, "你的WiFi");
  cfg.apiKey = prefs.isKey("apikey") ? prefs.getString("apikey") : cleanDefault(OPENAI_API_KEY, "請填入");
  cfg.playlist = prefs.isKey("playlist") ? prefs.getString("playlist") : String(YT_PLAYLIST_URL);
  prefs.end();
}
String authHeader() { return "Bearer " + cfg.apiKey; }

// ============================================================================
//  按鍵：BOOT（GPIO0）、擴充板 UP（GPIO20）/ DOWN（GPIO19），按下為 LOW
// ============================================================================
bool buttonPressed() { return digitalRead(PIN_BUTTON) == LOW; }

Key kUp = {PIN_KEY_UP, false, 0, false}, kDown = {PIN_KEY_DOWN, false, 0, false};

bool keysEnabled = true;          // 原生 USB 孔接著電腦時 GPIO19/20 是 USB 資料線，不能當按鍵

// 開機時判斷原生 USB 是否接著電腦：電腦每 1ms 送一個 SOF 封包，USB 控制器的框計數器會一直跳
// （不能用 digitalRead 判斷：設定 GPIO19/20 的瞬間 USB 就斷線了）
bool usbHostAttached() {
  uint32_t a = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF;
  delay(20);
  uint32_t b = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF;
  return a != b;
}

// 短按 = 放開時觸發；長按 = 按住 0.9 秒時觸發一次；狀態需穩定 40ms 才算數（防彈跳）
KeyEv pollKey(Key &k) {
  if (!keysEnabled) return K_NONE;
  bool raw = digitalRead(k.pin) == LOW;
  static uint32_t chg[2] = {0, 0};
  static bool last[2] = {false, false};
  int id = k.pin == PIN_KEY_UP ? 0 : 1;
  if (raw != last[id]) { last[id] = raw; chg[id] = millis(); }
  if (millis() - chg[id] < 40) return K_NONE;
  bool p = raw;
  uint32_t now = millis();
  if (p && !k.down) { k.down = true; k.at = now; k.longDone = false; return K_NONE; }
  if (p && k.down && !k.longDone && now - k.at >= 900) { k.longDone = true; return K_LONG; }
  if (!p && k.down) {
    k.down = false;
    if (!k.longDone && now - k.at > 30) return K_SHORT;
  }
  return K_NONE;
}

// 音量存到 NVS，重開機後保留
void saveVolume() {
  prefs.begin("robot", false);
  prefs.putInt("vol", volumePct);
  prefs.end();
}
void changeVolume(int delta) {
  volumePct = constrain(volumePct + delta, 0, 100);
  saveVolume();
  lastInteraction = millis();
  Serial.printf("音量 %d%%\n", (int)volumePct);
}

// ============================================================================
//  畫面：白色頭盔 + 深藍面罩 + 青色發光表情
// ============================================================================
enum Expr { EX_NORMAL, EX_HAPPY, EX_WINK, EX_LOVE, EX_SURPRISE, EX_SAD, EX_ANGRY, EX_SHY,
            EX_BORED, EX_SLEEPY, EX_LISTEN, EX_THINK, EX_DEAD };
volatile int speakExpr = EX_HAPPY;      // GPT 回覆帶的心情，說話時顯示

// 發光：先畫兩層淡色光暈，再畫本體
static void glowRR(int x, int y, int w, int h, int r, uint16_t c, int g) {
  fillRR(x - g, y - g, w + 2 * g, h + 2 * g, r + g, blend565(C_VISOR, c, 0.22f));
  fillRR(x - g / 2, y - g / 2, w + g, h + g, r + g / 2, blend565(C_VISOR, c, 0.45f));
  fillRR(x, y, w, h, r, c);
}
static void glowArc(int cx, int cy, int r, float a0, float a1, int th, uint16_t c) {
  strokeArc(cx, cy, r, a0, a1, th + th, blend565(C_VISOR, c, 0.25f));
  strokeArc(cx, cy, r, a0, a1, th, c);
}
static void glowLine(float x0, float y0, float x1, float y1, int th, uint16_t c) {
  strokeLine(x0, y0, x1, y1, th + th, blend565(C_VISOR, c, 0.25f));
  strokeLine(x0, y0, x1, y1, th, c);
}

// X,Y = 面罩中心；s = 縮放（1 = 頭盔寬 248）；lx,ly = 眼神方向；mouthOpen = 說話張嘴程度（0~1）
void drawRobot(int X, int Y, float s, int ex, float lx, float ly, bool blink, float mouthOpen, uint32_t now) {
  auto S = [&](float v) { return (int)(v * s + 0.5f); };
  uint16_t glow = ex == EX_ANGRY ? C_RED2 : C_GLOW;

  // ---- 耳罩（灰藍，左右兩側，在頭盔後面）----
  for (int side = -1; side <= 1; side += 2) {
    int cx = X + side * S(121);
    fillRRGrad(cx - S(19), Y - S(44), S(38), S(92), S(16), RGB(176, 190, 210), RGB(104, 120, 146));
    fillRR(cx - S(5) + side * S(6), Y - S(30), max(2, S(9)), S(56), S(5), RGB(206, 216, 232));   // 反光
  }

  // ---- 頭盔：陰影 + 白色漸層 + 左上反光 ----
  fillRR(X - S(125), Y - S(96), S(250), S(198), S(90), RGB(128, 138, 154));
  fillRRGrad(X - S(124), Y - S(99), S(248), S(194), S(88), RGB(252, 253, 255), RGB(196, 203, 214));
  fillEllipse(X - S(62), Y - S(80), S(34), S(9), RGB(255, 255, 255));

  // ---- 面罩：外框 + 深藍漸層 + 玻璃反光 ----
  fillRR(X - S(102), Y - S(71), S(204), S(142), S(56), RGB(52, 70, 96));
  fillRRGrad(X - S(98), Y - S(67), S(196), S(134), S(52), RGB(30, 52, 80), RGB(9, 18, 32));
  fillEllipse(X - S(50), Y - S(52), S(32), S(7), RGB(44, 70, 102));

  const int ey = Y - S(10), dx = S(44), my = Y + S(34);
  const int th = max(2, S(5));                                // 線條粗細
  bool talking = mouthOpen > 0.05f;
  bool smiley = ex == EX_NORMAL || ex == EX_HAPPY || ex == EX_WINK || ex == EX_LOVE || ex == EX_SHY;

  // ---- 腮紅（害羞、愛心）----
  if (ex == EX_SHY || ex == EX_LOVE)
    for (int side = -1; side <= 1; side += 2)
      fillEllipse(X + side * S(68), Y + S(20), S(15), S(6), blend565(C_VISOR, C_PINK, 0.55f));

  // ---- 嘴巴 ----
  if (talking) {
    int rx = S(15), ry = max(S(4), S(4 + 13 * mouthOpen));
    fillEllipse(X, my - S(4), rx + S(3), ry + S(3), blend565(C_VISOR, glow, 0.25f));
    fillEllipse(X, my - S(4), rx, ry, glow);
    if (ry > S(7)) fillEllipse(X, my - S(4), rx - S(5), ry - S(5), RGB(14, 60, 82));
  } else if (smiley) {
    glowArc(X, my - S(18), S(18), 35, 145, th, glow);         // 微笑 ︶
  } else if (ex == EX_SURPRISE || ex == EX_LISTEN) {
    int r = S(ex == EX_SURPRISE ? 9 : 7) + (ex == EX_LISTEN ? min(S(6), (int)(micLevel / 600)) : 0);
    fillCircleP(X, my - S(2), r + S(3), blend565(C_VISOR, glow, 0.25f));
    fillCircleP(X, my - S(2), r, glow);
    if (ex == EX_LISTEN) fillCircleP(X, my - S(2), max(1, r - th), C_VISOR);
  } else if (ex == EX_SAD || ex == EX_ANGRY) {
    glowArc(X, my + S(12), S(16), 215, 325, th, glow);        // 嘴角下垂 ︵
  } else if (ex == EX_THINK) {
    for (int i = 0; i < 3; i++)
      fillCircleP(X + (i - 1) * S(14), my - S(2), max(1, S(4)), (now / 250) % 3 == (unsigned)i ? glow : C_GLOW_DIM);
  } else if (ex == EX_SLEEPY) {
    glowArc(X, my + S(4), S(8), 200, 340, th, glow);
  } else {                                                    // BORED / DEAD：一字嘴
    glowLine(X - S(13), my - S(2), X + S(13), my - S(2), th, glow);
  }

  // ---- 眼睛 ----
  const int ew = S(22), eh = S(32);
  for (int side = -1; side <= 1; side += 2) {
    int cx = X + side * dx, x = cx + (int)lx, y = ey + (int)ly;
    switch (ex) {
      case EX_HAPPY: case EX_SHY:                             // 瞇眼笑 ∩
        glowArc(cx, ey + S(8), S(12), 200, 340, th + 1, glow); break;
      case EX_WINK:                                           // 左眼眨（一條線），右眼張開
        if (side < 0) glowLine(cx - S(11), ey, cx + S(11), ey, th + 1, glow);
        else glowRR(x - ew / 2, y - eh / 2, ew, eh, ew / 2, glow, max(1, S(5)));
        break;
      case EX_LOVE: {
        int r = S(15) + (int)(S(2) * sinf(now / 150.0f));
        fillHeart(cx, ey - S(2), r + S(3), blend565(C_VISOR, C_PINK, 0.3f));
        fillHeart(cx, ey - S(2), r, C_PINK);
        break;
      }
      case EX_SURPRISE:
        fillCircleP(cx, ey, S(14), blend565(C_VISOR, glow, 0.3f));
        fillCircleP(cx, ey, S(10), glow);
        break;
      case EX_BORED:                                          // 無聊：一條線
        glowLine(cx - S(12), ey + S(2), cx + S(12), ey + S(2), th + 1, glow); break;
      case EX_SLEEPY:                                         // 想睡：∪
        glowArc(cx, ey - S(6), S(11), 20, 160, th, glow); break;
      case EX_DEAD:
        glowLine(cx - S(9), ey - S(9), cx + S(9), ey + S(9), th, glow);
        glowLine(cx - S(9), ey + S(9), cx + S(9), ey - S(9), th, glow);
        break;
      case EX_THINK:
        glowRR(cx + S(6) - ew / 2, ey - S(8) - S(13), ew, S(26), ew / 2, glow, max(1, S(5))); break;
      default: {                                              // NORMAL / LISTEN / SAD / ANGRY
        int w = ex == EX_LISTEN ? S(25) : ew, h = ex == EX_LISTEN ? S(36) : ex == EX_ANGRY ? S(24) : ex == EX_SAD ? S(26) : eh;
        if (blink && ex != EX_LISTEN) glowLine(x - w / 2, y, x + w / 2, y, th + 1, glow);
        else glowRR(x - w / 2, y - h / 2, w, h, w / 2, glow, max(1, S(5)));
        if (ex == EX_SAD)                                     // 擔心：眉毛內側上揚
          glowLine(x - S(13), y - h / 2 - S(7) + S(5) * side, x + S(13), y - h / 2 - S(7) - S(5) * side, th, glow);
        if (ex == EX_ANGRY)                                   // 生氣：眉毛內側下壓
          glowLine(x - S(13), y - h / 2 - S(9) - S(6) * side, x + S(13), y - h / 2 - S(9) + S(6) * side, th, glow);
      }
    }
  }

  // ---- 想睡的 Zzz（畫在頭盔外右上）----
  if (ex == EX_SLEEPY) {
    int kz = (now / 500) % 3;
    cv->setTextColor(C_GLOW);
    cv->setTextSize(s >= 0.45f ? 2 : 1);
    cv->setCursor(X + (int)(92 * s), Y - (int)(110 * s) - kz * 2);
    cv->print("z");
    cv->setTextSize(s >= 0.45f ? 3 : 2);
    cv->setCursor(X + (int)(108 * s), Y - (int)(140 * s) - kz * 2);
    cv->print("Z");
  }
}

// ---- 功能說明（按鍵翻頁）----
#define DEVELOPER_CREDIT "此系統是吳玉柱先生與 Claude AI 共同開發，有任何意見請聯絡："
#define DEVELOPER_EMAIL  "achir1015@gmail.com"
struct HelpLine { String text; uint16_t color; };
std::vector<std::vector<HelpLine>> helpPages;
volatile int helpPage = 0;
const int HELP_LINES = 9;

void buildHelp() {
  struct Para { const char *t; uint16_t c; };
  static const Para P[] = {
    {"一、系統功能", C_YELLOW},
    {"【聊天與查詢】", C_GLOW},
    {"1. 聲控聊天：直接說話，停頓一下就自動送出；環境吵雜時「按住 BOOT 說話，放開送出」。", C_WHITE},
    {"2. 上網查詢：問天氣、新聞、股價、比賽結果等即時資訊，" ROBOT_NAME "會自己上網搜尋再回答。", C_WHITE},
    {"3. 班表查詢：問「我明天上什麼班」「這個月休幾天」「誰明天上大夜」，讀取病房助理與護理師月班表回答。", C_WHITE},
    {"4. 工作流程：問「白班幾點交班」「大夜班有哪些工作」「現在該做什麼」，依工作時間軸回答。", C_WHITE},
    {"5. " PORTAL_NAME "：長按 UP 鍵或說「工作站」開啟選單，用 UP/DOWN 選工具、按 BOOT 看說明與 QR 碼；技術稽核、督導手冊、學科、術科、長照組合的內容可以直接用說的問。", C_WHITE},
    {"【生活與娛樂】", C_GLOW},
    {"6. 時間與農民曆：上方顯示日期、星期、農曆與時間；右側農民曆顯示干支、生肖、宜忌、沖煞與節氣（建除十二神簡易版，僅供參考），也可以問「今天宜什麼」。", C_WHITE},
    {"7. 唱歌：說「唱首歌」隨機唱一首" SONG_COMPOSER "的創作歌曲（雲端硬碟 MP3 直接播放），也可以點歌（例如「唱千江有水千江月」）；同時顯示 YouTube QR 碼。唱歌中按 BOOT 停止、UP/DOWN 調音量。", C_WHITE},
    {"8. 表情：依心情開心、愛心、驚訝、難過、生氣、眨眼、害羞；太久沒互動會想睡覺。", C_WHITE},
    {"9. 音量與記憶：UP/DOWN 鍵或說「大聲一點／小聲一點」調音量；說「清除記憶」重新開始對話。", C_WHITE},
    {"【設定與按鍵】", C_GLOW},
    {"10. 網頁設定：開啟螢幕左下角的網址，可修改 Wi-Fi、密碼、API Key 與歌單。", C_WHITE},
    {"11. 連不上 Wi-Fi 時會開熱點 " AP_SSID "（密碼 " AP_PASS "），手機連上後開 192.168.4.1 設定。", C_WHITE},
    {"12. BOOT：短按看說明（再按下一頁）、按住說話。UP/DOWN：主畫面調音量、說明頁翻頁、選單選工具；長按 UP 開關工作站。", C_WHITE},
    {"\f", 0},
    {"二、創意開發者", C_YELLOW},
    {"", C_WHITE},
    {DEVELOPER_CREDIT, C_WHITE},
    {DEVELOPER_EMAIL, C_GLOW},
    {"", C_WHITE},
    {"謝謝你陪" ROBOT_NAME "聊天！", C_PINK},
  };
  helpPages.clear();
  std::vector<HelpLine> page;
  for (auto &p : P) {
    if (!strcmp(p.t, "\f")) { if (page.size()) helpPages.push_back(page); page.clear(); continue; }
    std::vector<String> lines = u8f.wrap(p.t, SCREEN_W - 16);
    if (lines.empty()) lines.push_back("");
    bool heading = p.c == C_GLOW && lines.size() == 1;  // 分類標題不要孤零零留在頁尾
    if (page.size() && (page.size() + lines.size() > (size_t)HELP_LINES || (heading && page.size() + 3 > (size_t)HELP_LINES))
        && lines.size() <= (size_t)HELP_LINES) {
      helpPages.push_back(page);                       // 同一項目不跨頁
      page.clear();
    }
    for (auto &l : lines) {
      if ((int)page.size() == HELP_LINES) { helpPages.push_back(page); page.clear(); }
      page.push_back({l, p.c});
    }
  }
  if (page.size()) helpPages.push_back(page);
}

void drawHelp(uint32_t now) {
  int total = helpPages.size(), pg = constrain((int)helpPage, 0, total - 1);
  bool last = pg == total - 1;
  u8f.setForegroundColor(C_GLOW);
  u8f.drawUTF8(8, 58, "功能說明");
  char num[12];
  snprintf(num, sizeof(num), "%d / %d", pg + 1, total);
  u8f.setForegroundColor(C_GRAY);
  u8f.drawUTF8(SCREEN_W - 8 - u8f.getUTF8Width(num), 58, num);
  cv->drawFastHLine(8, 63, SCREEN_W - 16, C_BAR_LINE);
  for (size_t i = 0; i < helpPages[pg].size(); i++) {
    u8f.setForegroundColor(helpPages[pg][i].color);
    u8f.drawUTF8(8, 81 + i * LINE_H, helpPages[pg][i].text.c_str());
  }
  if (last) drawRobot(SCREEN_W - 46, 196, 0.22f, (now / 1500) % 2 ? EX_HAPPY : EX_LOVE, 0, 0, false, 0, now);
  const char *foot = last ? "BOOT：回到聊天｜UP：上一頁" : "BOOT/DOWN：下一頁｜UP：上一頁";
  u8f.setForegroundColor(C_GREEN);
  u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width(foot)) / 2, 236, foot);
}

// QR 碼：白底黑點（x,y 左上角，邊長 w）
void drawQr(int x, int y, int w) {
  cv->fillRect(x, y, w, w, C_WHITE);
  int n = qrSize;
  if (n <= 0) return;
  int m = max(1, w / (n + 4)), off = (w - m * n) / 2;
  for (int yy = 0; yy < n; yy++)
    for (int xx = 0; xx < n; xx++)
      if (qrMods[yy * n + xx]) cv->fillRect(x + off + xx * m, y + off + yy * m, m, m, C_BLACK);
}

void qrCapture(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  if (n > QR_MAX) return;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) qrMods[y * n + x] = esp_qrcode_get_module(q, x, y);
  qrSize = n;
}

bool makeQr(const String &text) {
  qrSize = 0;
  esp_qrcode_config_t qc = {};
  qc.display_func = qrCapture;
  qc.max_qrcode_version = 10;
  qc.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&qc, text.c_str());
  return qrSize > 0;
}

// ---- 唱歌畫面（320x240：左邊封面與歌名，右邊大 QR 碼方便手機掃描）----
void drawSong(const String &title, uint32_t now) {
  const int TX = 6, TY = 48, TW = 160, TH = 90;              // 封面 320x180 縮成 160x90
  uint16_t *dst = cv->getBuffer();
  if (thumbOk) {
    for (int y = 0; y < TH; y++)
      for (int x = 0; x < TW; x++)
        dst[(TY + y) * SCREEN_W + TX + x] = thumbPix[(y * 2) * 320 + x * 2];
  } else {
    cv->fillRect(TX, TY, TW, TH, C_VISOR);
    u8f.setForegroundColor(C_GLOW);
    u8f.drawUTF8(TX + 48, TY + 52, "♪ ♪ ♪");
  }
  cv->drawRect(TX - 1, TY - 1, TW + 2, TH + 2, C_GLOW_DIM);

  const int QX = 176, QY = 46, QW = 138;
  if (qrSize > 0) {
    drawQr(QX, QY, QW);
    u8f.setForegroundColor(C_GRAY);
    const char *scan = "手機掃描 YouTube";
    u8f.drawUTF8(QX + (QW - u8f.getUTF8Width(scan)) / 2, QY + QW + 17, scan);
  } else {                                                    // 沒有對應的 YouTube：大大的唱歌小柯
    float m = speakLevel > 0 ? constrain(speakLevel / 6000.0f, 0.0f, 1.0f) : 0;
    drawRobot(QX + QW / 2, QY + 66, 0.42f, EX_HAPPY, 0, 0, false, m, now);
  }
  // 播放狀態 + 進度條（右下）
  char st[40] = "";
  if (songLoading) snprintf(st, sizeof(st), "歌曲下載中…");
  else if (songPlaying) snprintf(st, sizeof(st), "%u:%02u / %u:%02u", (unsigned)(songPosMs / 60000), (unsigned)(songPosMs / 1000 % 60),
                                 (unsigned)(songDurMs / 60000), (unsigned)(songDurMs / 1000 % 60));
  if (st[0]) {
    u8f.setForegroundColor(songLoading ? C_YELLOW : C_GREEN);
    u8f.drawUTF8(QX, 216, st);
    if (songPlaying && songDurMs) {
      cv->fillRect(QX, 221, QW - 40, 3, C_BAR_LINE);
      cv->fillRect(QX, 221, (int)min((uint32_t)(QW - 40), (uint32_t)((uint64_t)songPosMs * (QW - 40) / songDurMs)), 3, C_GLOW);
    }
  }

  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8(TX, 158, "正在唱：");
  std::vector<String> lines = u8f.wrap("《" + title + "》", TW + 2);
  u8f.setForegroundColor(C_WHITE);
  for (size_t i = 0; i < lines.size() && i < 2; i++) u8f.drawUTF8(TX, 176 + i * LINE_H, lines[i].c_str());
  u8f.setForegroundColor(C_GLOW_DIM);
  u8f.drawUTF8(TX, lines.size() > 1 ? 214 : 197, "詞曲：" SONG_COMPOSER);

  float beat = (sinf(now / 160.0f) + 1) / 2;
  float mouth = speakLevel > 0 ? constrain(speakLevel / 6000.0f, 0.0f, 1.0f) : beat;
  drawRobot(296, 220, 0.14f, EX_HAPPY, 0, 0, false, mouth, now);
  u8f.setForegroundColor(C_GREEN);
  u8f.drawUTF8(TX, 236, songPlaying ? "BOOT：停止｜UP/DOWN：音量" : "按任一鍵：回到聊天");
}

// ---- 病房助理工作站：工具清單（開機時從工作站網頁解析，新增工具會自動更新）----
struct PortalTool { String name, desc, url; };
std::vector<PortalTool> portalTools;
volatile int portalSel = 0;
bool portalOpen = false;               // 選單開著（看 QR 碼時按 BOOT 會回到選單）
const int PORTAL_ROWS = 8;
int knowledgeForTool(const String &name);
extern const char *KNOW_EXAMPLE[];

void drawPortalMenu(uint32_t now) {
  xSemaphoreTake(songLock, portMAX_DELAY);
  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8(8, 58, PORTAL_NAME);
  int n = portalTools.size();
  if (n == 0) {
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(8, 100, "讀取工具清單中…（需要網路）");
    xSemaphoreGive(songLock);
  } else {
    int sel = constrain((int)portalSel, 0, n - 1);
    char pn[12]; snprintf(pn, sizeof(pn), "%d/%d", sel + 1, n);
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(SCREEN_W - 8 - u8f.getUTF8Width(pn), 58, pn);
    int top = constrain(sel - PORTAL_ROWS / 2, 0, max(0, n - PORTAL_ROWS));
    for (int k = 0; k < PORTAL_ROWS && top + k < n; k++) {
      int i = top + k, y = 66 + k * 19;
      bool on = i == sel;
      if (on) cv->fillRoundRect(4, y, SCREEN_W - 8, 18, 4, C_GLOW_DIM);
      u8f.setForegroundColor(on ? C_WHITE : knowledgeForTool(portalTools[i].name) >= 0 ? C_GLOW : RGB(200, 206, 216));
      u8f.drawUTF8(10, y + 15, clipText(portalTools[i].name, SCREEN_W - 24).c_str());
    }
    xSemaphoreGive(songLock);
  }
  u8f.setForegroundColor(C_GREEN);
  const char *foot = (now / 3000) % 2 ? "長按 UP：回主畫面" : "UP/DOWN 選擇｜BOOT 看 QR 碼";
  u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width(foot)) / 2, 236, foot);
}

// 單一工具 / 工作站：QR 碼 + 說明
void drawQrPage(const String &title, const String &text, uint32_t now) {
  drawQr(6, 46, 150);
  u8f.setForegroundColor(C_GRAY);
  u8f.drawUTF8(6 + (150 - u8f.getUTF8Width("手機掃描開啟")) / 2, 214, "手機掃描開啟");
  const int TX = 164, TW = SCREEN_W - TX - 4;
  std::vector<String> tl = u8f.wrap(title, TW);
  u8f.setForegroundColor(C_ORANGE);
  int y = 60;
  for (size_t i = 0; i < tl.size() && i < 2; i++, y += LINE_H) u8f.drawUTF8(TX, y, tl[i].c_str());
  y += 4;
  std::vector<String> ls = u8f.wrap(text, TW);
  u8f.setForegroundColor(C_WHITE);
  for (size_t i = 0; i < ls.size() && y <= 218; i++, y += LINE_H) u8f.drawUTF8(TX, y, ls[i].c_str());
  u8f.setForegroundColor(C_GREEN);
  u8f.drawUTF8(6, 236, portalOpen ? "BOOT：返回清單｜長按 UP：回主畫面" : "按任一鍵：回主畫面");
}

// ---- 農曆 ----
// 西元日期 → 距 2000-01-01 的天數（Howard Hinnant 演算法）
long daysSince2000(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468 - 10957;
}

// 回傳例如「八月二十」「閏六月初一」；超出表格範圍回傳空字串
String lunarString(const struct tm &t) {
  long d = daysSince2000(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  int yi = -1;
  for (int i = 0; i < LUNAR_YEARS - 1; i++)
    if (d >= LUNAR[i].cny && d < LUNAR[i + 1].cny) { yi = i; break; }
  if (yi < 0) return "";
  long off = d - LUNAR[yi].cny;
  int leap = LUNAR[yi].leap, n = leap ? 13 : 12, month = 1, day = 1;
  bool isLeap = false;
  for (int i = 0; i < n; i++) {
    int len = (LUNAR[yi].bits >> i) & 1 ? 30 : 29;
    if (off < len) {
      if (!leap || i < leap) month = i + 1;
      else if (i == leap) { month = leap; isLeap = true; }
      else month = i;
      day = off + 1;
      break;
    }
    off -= len;
  }
  static const char *MN[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "臘"};
  static const char *DG[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};
  static const char *TP[] = {"初", "十", "廿"};
  String ds = day == 10 ? "初十" : day == 20 ? "二十" : day == 30 ? "三十" : String(TP[day / 10]) + DG[day % 10 - 1];
  return String(isLeap ? "閏" : "") + MN[month - 1] + "月" + ds;
}

// 上方日期時間列：日期、星期、農曆（左）＋ 24 小時制時間（中）＋ Wi-Fi、音量（右）
void drawTopBar() {
  struct tm t;
  bool ok = getLocalTime(&t, 0);
  static const char *WD[] = {"週日", "週一", "週二", "週三", "週四", "週五", "週六"};
  char dateStr[16] = "--/--", timeStr[12] = "--:--:--";
  if (ok) {
    snprintf(dateStr, sizeof(dateStr), "%d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &t);
  }
  cv->fillRect(0, 0, SCREEN_W, BAR_H, C_BG);
  cv->drawFastHLine(0, BAR_H - 1, SCREEN_W, C_BAR_LINE);
  static int lunarDay = -1;
  static String lunar;
  if (ok && t.tm_yday != lunarDay) {            // 一天只算一次
    lunarDay = t.tm_yday;
    String l = lunarString(t);
    lunar = l.startsWith("閏") || l.isEmpty() ? l : "農曆" + l;
  }
  u8f.setForegroundColor(C_GLOW);
  u8f.drawUTF8(4, 17, (String(dateStr) + " " + (ok ? WD[t.tm_wday] : "")).c_str());
  u8f.setForegroundColor(C_ORANGE);
  u8f.drawUTF8(4, 36, ok ? lunar.c_str() : "");
  cv->setTextSize(3);
  cv->setTextColor(C_WHITE);
  cv->setCursor(128, 9);
  cv->print(timeStr);
  char vol[8];
  snprintf(vol, sizeof(vol), "%d%%", (int)volumePct);
  cv->fillCircle(282, 11, 3, WiFi.status() == WL_CONNECTED ? C_GREEN : C_RED);   // Wi-Fi 燈號
  u8f.setForegroundColor(C_YELLOW);
  u8f.drawUTF8(SCREEN_W - 2 - 32, 17, "音量");
  u8f.drawUTF8(SCREEN_W - 2 - u8f.getUTF8Width(vol), 36, vol);
}

// 右側農民曆卡片
void drawAlmanac(int x0, int y0, int w, int h) {
  static Almanac alm;
  static int almDay = -1;
  struct tm t;
  bool ok = getLocalTime(&t, 0);
  if (ok && t.tm_yday != almDay) { almDay = t.tm_yday; alm = computeAlmanac(t); }
  cv->fillRoundRect(x0, y0, w, h, 8, C_PANEL);
  cv->drawRoundRect(x0, y0, w, h, 8, C_GLOW_DIM);
  int x = x0 + 8, y = y0 + 19, tw = w - 16;
  u8f.setForegroundColor(C_YELLOW);
  u8f.drawUTF8(x, y, "農民曆");
  if (!ok || !alm.ok) {
    u8f.setForegroundColor(C_GRAY);
    u8f.drawUTF8(x, y + 26, "等待網路校時…");
    return;
  }
  u8f.setForegroundColor(C_ORANGE);
  String yz = alm.yearGZ + "年【" + alm.zodiac + "】";
  u8f.drawUTF8(x0 + w - 8 - u8f.getUTF8Width(yz.c_str()), y, yz.c_str());
  cv->drawFastHLine(x, y + 5, tw, C_BAR_LINE);
  y += 23;
  u8f.setForegroundColor(C_WHITE);
  u8f.drawUTF8(x, y, (alm.monthGZ + "月 " + alm.dayGZ + "日 " + alm.jianchu + "日").c_str());
  y += LINE_H + 1;
  u8f.setForegroundColor(C_GLOW);
  u8f.drawUTF8(x, y, (alm.chong + " 煞" + alm.sha).c_str());
  y += LINE_H + 5;
  auto block = [&](const char *label, uint16_t lc, const String &txt) {
    cv->fillRoundRect(x, y - 14, 18, 18, 4, lc);
    u8f.setForegroundColor(C_BLACK);
    u8f.drawUTF8(x + 1, y + 1, label);
    std::vector<String> ls = u8f.wrap(txt, tw - 22);
    u8f.setForegroundColor(C_WHITE);
    for (size_t i = 0; i < ls.size() && i < 2; i++) u8f.drawUTF8(x + 22, y + i * LINE_H, ls[i].c_str());
    y += max((size_t)1, min(ls.size(), (size_t)2)) * LINE_H + 4;
  };
  block("宜", C_GREEN, alm.yi);
  block("忌", C_RED2, alm.ji);
  u8f.setForegroundColor(C_ORANGE);
  if (alm.termToday.length()) u8f.drawUTF8(x, y + 1, ("今日節氣 " + alm.termToday).c_str());
  else u8f.drawUTF8(x, y + 1, ("下個節氣 " + alm.nextTerm).c_str());
}

// 麥克風音量條（讓人知道它一直在聽）
void drawMicBars(int x, int y, bool listening) {
  int lvl = constrain(map(micLevel, 0, 3000, 0, 100), 0, 100);
  for (int i = 0; i < 4; i++) {
    int h = 3 + (lvl * (4 - abs(i * 2 - 3)) / 3) * 12 / 100;
    cv->fillRoundRect(x + i * 6, y - h, 4, h, 2, listening ? C_GREEN : C_GRAY);
  }
}

void flushScreen() { tft.drawRGBBitmap(0, 0, cv->getBuffer(), SCREEN_W, SCREEN_H); }

// UI task：重畫整個畫面
void uiTask(void *) {
  uint32_t nextBlink = millis() + 3000, nextLook = 0, nextIdle = millis() + 8000, idleUntil = 0;
  int idleExpr = EX_NORMAL;
  float lx = 0, ly = 0, tx = 0, ty = 0;
  for (;;) {
    uint32_t now = millis();
    UiState st; String title, text; uint32_t since; bool sad;
    xSemaphoreTake(uiLock, portMAX_DELAY);
    st = uiState; title = uiTitle; text = uiText; since = uiSince; sad = uiSad;
    xSemaphoreGive(uiLock);

    // 眼神：待機時東張西望
    if (now > nextLook) {
      if (st == UI_IDLE) { tx = random(-12, 13); ty = random(-6, 7); }
      else { tx = 0; ty = 0; }
      nextLook = now + random(1200, 3500);
    }
    lx += (tx - lx) * 0.35f; ly += (ty - ly) * 0.35f;

    // 眨眼（偶爾連眨兩下）
    bool blink = false;
    if (now >= nextBlink) {
      uint32_t d = now - nextBlink;
      if (d < 130 || (d > 260 && d < 390 && (nextBlink & 1))) blink = true;
      else if (d >= 400) nextBlink = now + random(2500, 6000);
    }

    // 待機時的隨機小表情
    if (st == UI_IDLE && now > nextIdle) {
      int r = random(100);
      idleExpr = r < 28 ? EX_HAPPY : r < 48 ? EX_WINK : r < 60 ? EX_LOVE : r < 72 ? EX_SURPRISE : r < 86 ? EX_BORED : EX_SHY;
      idleUntil = now + (idleExpr == EX_WINK ? 800 : idleExpr == EX_SURPRISE ? 900 : 1800);
      nextIdle = now + random(6000, 14000);
    }

    bool sleepy = st == UI_IDLE && now - lastInteraction > SLEEPY_AFTER_SEC * 1000UL;
    int ex = EX_NORMAL;
    switch (st) {
      case UI_LISTEN: ex = EX_LISTEN; break;
      case UI_THINK:  ex = EX_THINK;  break;
      case UI_SPEAK:  ex = speakExpr; break;
      case UI_MSG:    ex = sad ? EX_DEAD : EX_THINK; break;
      default:        ex = sleepy ? EX_SLEEPY : now < idleUntil ? idleExpr : EX_NORMAL;
    }
    float mouth = st == UI_SPEAK ? constrain(speakLevel / 6000.0f, 0.0f, 1.0f) : 0;

    cv->fillScreen(C_BG);
    if (st == UI_SONG || st == UI_QR || st == UI_PORTAL || (st == UI_HELP && helpPages.size())) {
      if (st == UI_SONG) drawSong(title, now);
      else if (st == UI_QR) drawQrPage(title, text, now);
      else if (st == UI_PORTAL) drawPortalMenu(now);
      else drawHelp(now);
      drawTopBar();
      flushScreen();
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    bool compact = (st == UI_THINK || st == UI_SPEAK || st == UI_MSG) && (title.length() || text.length());
    if (!compact) {
      drawRobot(72, 124, 0.5f, ex, lx * 0.5f, ly * 0.5f, blink, mouth, now);
      drawAlmanac(146, 44, 170, 168);
      drawTopBar();
      const char *hint = st == UI_LISTEN ? "聆聽中…" : sleepy ? "想睡…" : VOICE_ACTIVATION ? "待命中" : "按住BOOT說話";
      u8f.setForegroundColor(st == UI_LISTEN ? C_GREEN : C_GRAY);
      u8f.drawUTF8(4, 213, hint);
      drawMicBars(112, 214, st == UI_LISTEN);
      u8f.setForegroundColor(C_GLOW_DIM);
      u8f.drawUTF8(4, 233, ipText);                                  // 設定網頁位址
      cv->fillRoundRect(178, 216, 138, 22, 8, C_BTN);                // 長按 UP 開啟工作站
      u8f.setForegroundColor(C_WHITE);
      u8f.drawUTF8(178 + (138 - u8f.getUTF8Width(PORTAL_NAME)) / 2, 233, PORTAL_NAME);
    } else {
      // 左邊小機器人，右邊標題 + 文字（字太多時自動翻頁）
      drawRobot(54, 140, 0.36f, ex, lx * 0.36f, ly * 0.36f, blink, mouth, now);
      drawTopBar();
      const int TX = 112, TW = SCREEN_W - TX - 4, PER = 9;
      u8f.setForegroundColor(st == UI_MSG ? (sad ? C_RED : C_YELLOW) : st == UI_SPEAK ? C_ORANGE : C_GLOW);
      u8f.drawUTF8(TX, 60, clipText(title, TW).c_str());
      std::vector<String> lines = u8f.wrap(text, TW);
      int pages = max(1, (int)((lines.size() + PER - 1) / PER));
      int page = (int)((now - since) / 4500) % pages;
      u8f.setForegroundColor(C_WHITE);
      for (int i = 0; i < PER && page * PER + i < (int)lines.size(); i++)
        u8f.drawUTF8(TX, 84 + i * LINE_H, lines[page * PER + i].c_str());
      if (pages > 1) {
        char pn[12]; snprintf(pn, sizeof(pn), "%d/%d", page + 1, pages);
        u8f.setForegroundColor(C_GRAY);
        u8f.drawUTF8(4, 236, pn);
      }
    }
    flushScreen();
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ============================================================================
//  音訊：錄音 / 播放
// ============================================================================
void writeWavHeader(uint8_t *h, uint32_t dataBytes) {
  uint32_t sr = MIC_SAMPLE_RATE, byteRate = sr * 2, chunk = 36 + dataBytes;
  memcpy(h, "RIFF", 4);      memcpy(h + 4, &chunk, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmtLen = 16;      memcpy(h + 16, &fmtLen, 4);
  uint16_t fmt = 1, ch = 1;  memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sr, 4);    memcpy(h + 28, &byteRate, 4);
  uint16_t align = 2, bits = 16; memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataBytes, 4);
}

// 讀 20ms 麥克風資料（24kHz 立體聲 → 單聲道 → 16kHz），回傳 RMS
// 麥克風不一定在左聲道：每一段取能量較大的聲道
int readFrame(int16_t *out) {
  static int16_t raw[FRAME * 3 / 2 * 2];        // 480 個立體聲樣本
  size_t got = lfI2S.readBytes((char *)raw, sizeof(raw)) / 2;
  int64_t eL = 0, eR = 0;
  for (size_t i = 0; i + 1 < got; i += 2) { eL += abs(raw[i]); eR += abs(raw[i + 1]); }
  int ch = eR > eL ? 1 : 0;
  int32_t mono[FRAME * 3 / 2];
  for (int i = 0; i < FRAME * 3 / 2; i++) {
    int32_t s = (size_t)(i * 2 + ch) < got ? (int32_t)raw[i * 2 + ch] * MIC_SOFT_GAIN : 0;
    dcState += (s - dcState) >> 6;              // 簡易去直流
    mono[i] = s - dcState;
  }
  int64_t sum = 0;
  for (int k = 0; k < FRAME / 2; k++) {        // 每 3 個 24kHz 樣本 → 2 個 16kHz 樣本
    const int32_t *m = &mono[k * 3];
    int32_t a = (m[0] * 2 + m[1]) / 3, b = (m[1] + m[2] * 2) / 3;
    out[k * 2] = constrain(a, -32768, 32767);
    out[k * 2 + 1] = constrain(b, -32768, 32767);
    sum += (int64_t)out[k * 2] * out[k * 2] + (int64_t)out[k * 2 + 1] * out[k * 2 + 1];
  }
  return (int)sqrtf((float)(sum / FRAME));
}

void flushMic(int ms) {
  int16_t f[FRAME];
  for (int i = 0; i < ms / 20; i++) readFrame(f);
  prerollCount = 0;
}

int vadThreshold() { return max((int)VAD_MIN_RMS, (int)(noiseFloor * VAD_RATIO)); }

// 錄一句話：byButton = 按鍵模式（放開結束）；否則偵測停頓結束。回傳樣本數（0 = 視為雜音）
size_t recordUtterance(bool byButton) {
  int16_t *pcm = (int16_t *)(wavBuf + 44);
  size_t n = 0;
  for (int k = 0; k < prerollCount; k++) {      // 先放入觸發前的預錄
    int idx = (prerollHead - prerollCount + k + PREROLL_FR) % PREROLL_FR;
    memcpy(pcm + n, preroll[idx], FRAME * 2);
    n += FRAME;
  }
  prerollCount = 0;

  setUi(UI_LISTEN);
  lastInteraction = millis();
  int thr = vadThreshold();
  int silentMs = 0, voicedMs = 0;
  double rmsSum = 0; int frames = 0;
  lastRecHitMax = true;
  while (n + FRAME <= MAX_SAMPLES) {
    int rms = readFrame(pcm + n);
    n += FRAME;
    micLevel = rms;
    rmsSum += rms; frames++;
    if (byButton) {
      if (!buttonPressed()) break;
      voicedMs += 20;
    } else {
      if (rms > thr * 0.7f) { silentMs = 0; voicedMs += 20; }
      else silentMs += 20;
      if (silentMs >= VAD_SILENCE_MS) { lastRecHitMax = false; break; }
    }
  }
  if (byButton) lastRecHitMax = false;
  lastRecAvgRms = frames ? rmsSum / frames : 0;
  micLevel = 0;
  if (!byButton) {
    size_t trim = (size_t)max(0, silentMs - 200) * MIC_SAMPLE_RATE / 1000;   // 去掉尾端靜音（保留 200ms）
    n = n > trim ? n - trim : 0;
    if (voicedMs < VAD_MIN_SPEECH_MS) return 0;
  } else if (n < MIC_SAMPLE_RATE / 2) {
    return 0;
  }
  return n;
}

// 播放 task：從 tts.buf 取資料送 I2S（單聲道 → 立體聲），套用音量，並計算嘴巴開合
void playTask(void *) {
  int16_t out[512];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!tts.downloadDone && tts.written < TTS_PREBUFFER) vTaskDelay(5);
    for (;;) {
      size_t avail = (tts.written - tts.played) & ~(size_t)1;
      if (avail == 0) {
        if (tts.downloadDone) break;
        vTaskDelay(2);
        continue;
      }
      size_t bytes = min(avail, sizeof(out) / 2);
      const int16_t *src = (const int16_t *)(tts.buf + tts.played);
      int vol = volumePct;
      int64_t acc = 0;
      for (size_t i = 0; i < bytes / 2; i++) {
        int16_t s = (int32_t)src[i] * vol / 100;
        out[i * 2] = out[i * 2 + 1] = s;
        acc += abs(src[i]);
      }
      speakLevel = (int)(acc / (int64_t)max((size_t)1, bytes / 2));
      lfI2S.write((uint8_t *)out, bytes * 2);
      tts.played += bytes;
    }
    speakLevel = 0;
    memset(out, 0, sizeof(out));             // 補靜音，避免 DMA 殘留造成嗡嗡聲
    for (int i = 0; i < 8; i++) lfI2S.write((uint8_t *)out, sizeof(out));
    tts.playing = false;
  }
}

// 讓 HTTPClient::writeToStream 直接把 TTS 音訊寫進 PSRAM 緩衝
class TtsSink : public Stream {
 public:
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    size_t space = TTS_BUF_BYTES - tts.written;
    size_t n = min(len, space);
    memcpy(tts.buf + tts.written, data, n);
    tts.written += n;
    return len;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// 播放中：UP/DOWN 可以調音量
void waitPlaybackDone() {
  while (tts.playing) {
    if (pollKey(kUp) == K_SHORT) changeVolume(+10);
    if (pollKey(kDown) == K_SHORT) changeVolume(-10);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================================
//  OpenAI API
// ============================================================================
String lastApiError;

String speechToText(size_t samples) {
  uint32_t dataBytes = samples * 2;
  writeWavHeader(wavBuf, dataBytes);
  size_t wavLen = 44 + dataBytes;

  const String B = "----ESP32S3RobotBoundary";
  String head =
      "--" + B + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n" +
      "--" + B + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nzh\r\n" +
      "--" + B + "\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n請用台灣繁體中文轉寫。\r\n" +
      "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
      "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + B + "--\r\n";

  size_t total = head.length() + wavLen + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body) { lastApiError = "記憶體不足"; return ""; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), wavBuf, wavLen);
  memcpy(body + head.length() + wavLen, tail.c_str(), tail.length());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, String(OPENAI_HOST) + "/v1/audio/transcriptions");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + B);
  int code = http.POST(body, total);
  String resp = http.getString();
  http.end();
  free(body);

  if (code != 200) {
    lastApiError = "STT " + String(code) + " " + resp.substring(0, 120);
    Serial.println(lastApiError);
    return "";
  }
  JsonDocument doc;
  deserializeJson(doc, resp);
  String text = doc["text"] | "";
  text.trim();
  return text;
}

String nowString() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return "未知";
  static const char *W[] = {"日", "一", "二", "三", "四", "五", "六"};
  char buf[48];
  strftime(buf, sizeof(buf), "%Y年%m月%d日 %H:%M", &t);
  return String(buf) + " 星期" + W[t.tm_wday] + "（農曆" + lunarString(t) + "）。" + almanacText(t);
}

void pushHistory(const char *role, const String &text) {
  if (histCount == MAX_HISTORY_MSGS) {
    for (int i = 1; i < MAX_HISTORY_MSGS; i++) { histRole[i - 1] = histRole[i]; histText[i - 1] = histText[i]; }
    histCount--;
  }
  histRole[histCount] = role;
  histText[histCount] = text;
  histCount++;
}

// 取出回覆開頭的心情標籤，例如「[love]好喜歡你！」→ EX_LOVE，並從文字中移除
int parseEmotion(String &reply) {
  int e = EX_HAPPY;
  if (reply.startsWith("[")) {
    int k = reply.indexOf(']');
    if (k > 0 && k < 16) {
      String tag = reply.substring(1, k);
      tag.toLowerCase();
      if (tag == "love") e = EX_LOVE;
      else if (tag == "surprise") e = EX_SURPRISE;
      else if (tag == "sad") e = EX_SAD;
      else if (tag == "angry") e = EX_ANGRY;
      else if (tag == "wink") e = EX_WINK;
      else if (tag == "shy") e = EX_SHY;
      reply = reply.substring(k + 1);
      reply.trim();
    }
  }
  return e;
}

// 網路搜尋的回答會附上來源連結，例如「 ([nownews.com](https://…))」，語音播放前拿掉
String stripLinks(String t) {
  for (;;) {                                           // Markdown 連結 [文字](網址)
    int a = t.indexOf('[');
    if (a < 0) break;
    int m = t.indexOf("](", a);
    int b = m < 0 ? -1 : t.indexOf(')', m);
    if (b < 0) break;
    int from = a, to = b + 1;
    if (from > 0 && t[from - 1] == '(' && to < (int)t.length() && t[to] == ')') { from--; to++; }   // 外層括號
    while (from > 0 && t[from - 1] == ' ') from--;
    t.remove(from, to - from);
  }
  for (int h; (h = t.indexOf("http")) >= 0;) {         // 剩下的裸網址
    int e = h;
    while (e < (int)t.length() && (uint8_t)t[e] > ' ' && (uint8_t)t[e] < 0x80) e++;
    t.remove(h, e - h);
  }
  t.replace("\n\n", "\n");
  return t;
}

// 問題看起來需要即時資訊（只用來顯示「上網查詢中」，要不要搜尋由 AI 自己決定）
bool looksLikeSearch(const String &t) {
  return containsAny(t, {"天氣", "氣溫", "下雨", "颱風", "新聞", "最新", "股價", "股票", "匯率", "比賽", "比數", "賽程",
                         "查一下", "查查", "搜尋", "上網", "油價", "地震", "空氣品質", "營業時間"});
}

// 使用 OpenAI Responses API；ENABLE_WEB_SEARCH = 1 時 AI 可以自己決定上網搜尋（天氣、新聞…）
String chatWithGPT(const String &userText) {
  JsonDocument doc;
  doc["model"] = CHAT_MODEL;
  doc["max_output_tokens"] = CHAT_MAX_TOKENS;
#if ENABLE_WEB_SEARCH
  if (chatContext.isEmpty()) {                        // 查班表、工作站資料時不需要上網
    JsonObject ws = doc["tools"].to<JsonArray>().add<JsonObject>();
    ws["type"] = "web_search";
    ws["search_context_size"] = "medium";
    ws["user_location"]["type"] = "approximate";
    ws["user_location"]["country"] = "TW";
    ws["user_location"]["city"] = SEARCH_CITY;
  }
#endif
  String ins = String("你是一台可愛的桌上型 AI 語音機器人，名字叫「" ROBOT_NAME "」，個性活潑、貼心又有點俏皮。") +
               "一律使用台灣繁體中文回答，口語化、簡短（100 字以內），因為回答會用語音播放，"
               "不要使用 Markdown、條列符號、表情符號或網址。"
               "你沒有鏡頭，被要求看東西時，請可愛地說明自己沒有眼睛鏡頭。"
#if ENABLE_WEB_SEARCH
               "遇到天氣、新聞、股價、比賽結果等需要即時資訊的問題，請先上網搜尋，再用口語簡短摘要重點，"
               "搜尋到的數字（溫度、價格、比數等）要直接講出來，不要叫使用者自己去查；"
               "不要唸出網址或來源網站名稱；沒指定地點時以" SEARCH_CITY_ZH "為準。"
#endif
               "語音辨識偶爾會聽錯字，請依上下文推測使用者的意思。"
               "你的創意開發者是吳玉柱先生，他與 Claude AI 共同開發了你；"
               "被問到是誰做的、開發者、作者或設計者時，要熱情地介紹吳玉柱先生，"
               "並說有任何意見可以寫信到 achir1015@gmail.com。"
               "每次回答的最開頭，加上一個代表你當下心情的標籤，只能是以下其中一個："
               "[happy] [love] [surprise] [sad] [angry] [wink] [shy]，標籤後面直接接回答內容。"
               "現在時間：" + nowString() + "。";
  if (chatContext.length())
    ins += "以下是資料庫查到的資料（月班表、工作流程時間軸、工作站資料），請根據資料回答，資料沒有的不要亂編；"
           "使用者說「我」指的是" SCHEDULE_MY_NAME "；工作流程的時間是 24 小時制，問「現在該做什麼」時請對照現在時間。"
           "班別代號：" SCHEDULE_LEGEND "。回答時把代號說成中文班別。\n" + chatContext;
  doc["instructions"] = ins;
  ins = String();

  JsonArray msgs = doc["input"].to<JsonArray>();
  for (int i = 0; i < histCount; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"] = histRole[i];
    m["content"] = histText[i];
  }
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user";
  u["content"] = userText;

  String req;
  serializeJson(doc, req);
  doc.clear();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(45000);
  http.begin(client, String(OPENAI_HOST) + "/v1/responses");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(req);
  req = String();
  String resp = http.getString();
  http.end();

  if (code != 200) {
    lastApiError = "Chat " + String(code) + " " + resp.substring(0, 120);
    Serial.println(lastApiError);
    return "";
  }
  // output 可能是 [web_search_call, message]，取 message 的文字
  JsonDocument filter;
  filter["output"][0]["type"] = true;
  filter["output"][0]["content"][0]["text"] = true;
  JsonDocument r;
  deserializeJson(r, resp, DeserializationOption::Filter(filter));
  resp = String();
  String reply;
  bool searched = false;
  for (JsonObject o : r["output"].as<JsonArray>()) {
    String type = o["type"] | "";
    if (type == "web_search_call") searched = true;
    if (type == "message")
      for (JsonObject c : o["content"].as<JsonArray>()) reply += c["text"] | "";
  }
  if (searched) Serial.println("（已上網搜尋）");
  reply.trim();
  speakExpr = parseEmotion(reply);                     // 先取出開頭的心情標籤，再去掉網址
  reply = stripLinks(reply);
  reply.replace("*", "");
  reply.replace("#", "");
  reply.trim();

  pushHistory("user", userText);
  pushHistory("assistant", reply);
  return reply;
}

bool speak(const String &text) {
  if (text.isEmpty()) return false;
  JsonDocument doc;
  doc["model"] = TTS_MODEL;
  doc["voice"] = TTS_VOICE;
  doc["input"] = text;
  doc["response_format"] = "pcm";
  doc["instructions"] = TTS_INSTRUCTIONS;   // tts-1 會忽略此欄位
  String req;
  serializeJson(doc, req);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, String(OPENAI_HOST) + "/v1/audio/speech");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(req);
  if (code != 200) {
    lastApiError = "TTS " + String(code) + " " + http.getString().substring(0, 120);
    Serial.println(lastApiError);
    http.end();
    return false;
  }

  tts.written = 0;
  tts.played = 0;
  tts.downloadDone = false;
  tts.playing = true;
  xTaskNotifyGive(playTaskHandle);

  TtsSink sink;
  http.writeToStream(&sink);
  tts.downloadDone = true;
  http.end();
  return true;
}

// ============================================================================
//  唱歌：從 YouTube 播放清單隨機選歌
// ============================================================================
// 邊下載邊解析播放清單網頁（約 1MB，不整頁存下來）：找 lockupViewModel → videoId → title
class PlaylistParser : public Stream {
 public:
  std::vector<Song> out;
  size_t write(uint8_t b) override { feed(b); return 1; }
  size_t write(const uint8_t *d, size_t n) override { for (size_t i = 0; i < n; i++) feed(d[i]); return n; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
 private:
  int state = 0, mi = 0;
  bool esc = false;
  String buf, vid;
  void feed(uint8_t c) {
    static const char *M[] = {"\"lockupViewModel\":{\"contentImage\"", "\"videoId\":\"", "\"title\":{\"content\":\""};
    if (out.size() >= 80) return;
    if (state == 0 || state == 1 || state == 3) {
      const char *pat = M[state == 0 ? 0 : state == 1 ? 1 : 2];
      if (c == (uint8_t)pat[mi]) mi++;
      else mi = (c == (uint8_t)pat[0]) ? 1 : 0;
      if (pat[mi] == 0) { mi = 0; buf = ""; esc = false; state = state == 0 ? 1 : state == 1 ? 2 : 4; }
    } else if (state == 2) {
      buf += (char)c;
      if (buf.length() == 11) { vid = buf; state = 3; }
    } else if (state == 4) {
      if (c == '"' && !esc) { out.push_back({vid, unescape(buf)}); state = 0; }
      else { esc = (c == '\\' && !esc); buf += (char)c; }
      if (buf.length() > 300) state = 0;
    }
  }
  static String unescape(const String &s) {
    String r;
    for (size_t i = 0; i < s.length(); i++) {
      if (s[i] == '\\' && i + 1 < s.length()) {
        char n = s[++i];
        if (n == 'u' && i + 4 < s.length()) {                    // \uXXXX → UTF-8
          uint32_t cp = strtoul(s.substring(i + 1, i + 5).c_str(), nullptr, 16);
          i += 4;
          if (cp < 0x80) r += (char)cp;
          else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
          else { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
        } else r += n == 'n' ? ' ' : n;
      } else r += s[i];
    }
    return r;
  }
};

void loadSongsFromNvs() {
  prefs.begin("robot", true);
  String raw = prefs.getString("songs", "");
  prefs.end();
  std::vector<Song> v;
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    int tab = line.indexOf('\t');
    if (tab == 11) v.push_back({line.substring(0, 11), line.substring(12)});
    start = nl + 1;
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  songs = v;
  xSemaphoreGive(songLock);
}

// 下載播放清單並存進 NVS；回傳歌曲數（失敗 -1）
int refreshPlaylist() {
  if (cfg.playlist.indexOf("list=") < 0) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, cfg.playlist);
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126 Safari/537.36");
  http.addHeader("Accept-Language", "zh-TW,zh;q=0.9");
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("歌單下載失敗 %d\n", code); return -1; }
  PlaylistParser parser;
  http.writeToStream(&parser);
  http.end();
  if (parser.out.empty()) { Serial.println("歌單解析不到歌曲"); return -1; }
  String raw;
  for (auto &x : parser.out) raw += x.id + "\t" + x.title + "\n";
  prefs.begin("robot", false);
  prefs.putString("songs", raw);
  prefs.end();
  xSemaphoreTake(songLock, portMAX_DELAY);
  songs = parser.out;
  xSemaphoreGive(songLock);
  Serial.printf("歌單更新：%d 首\n", (int)parser.out.size());
  return parser.out.size();
}

// 下載 YouTube 封面（mqdefault 320x180）並解碼
bool fetchThumb(const String &id) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg");
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    const size_t CAP = 64 * 1024;
    uint8_t *jpg = (uint8_t *)ps_malloc(CAP);
    size_t len = 0;
    WiFiClient *st = http.getStreamPtr();
    uint32_t t0 = millis();
    int total = http.getSize();
    while (jpg && http.connected() && len < CAP && (total < 0 || (int)len < total) && millis() - t0 < 8000) {
      size_t a = st->available();
      if (a) len += st->readBytes(jpg + len, min(a, CAP - len));
      else delay(5);
    }
    if (jpg && len > 0 && jpg2rgb565(jpg, len, (uint8_t *)thumbPix, JPG_SCALE_NONE)) ok = true;
    free(jpg);
  }
  http.end();
  return ok;
}

void speakAndShow(const String &reply);
uint32_t songSince = 0;
int lastSong = -1;

// ---- 唱歌：直接播放 Google 雲端硬碟資料夾裡的 MP3（自己創作的歌）----
//  資料夾設成「知道連結的任何人都能檢視」，小柯讀資料夾的公開清單頁，再下載 MP3 到 PSRAM 解碼播放

// 比對歌名用：去掉副檔名、空白與標點、複本尾碼「(1)」「_3」
String normTitle(String s) {
  if (s.endsWith(".mp3") || s.endsWith(".mp4")) s.remove(s.length() - 4);
  static const char *DROP[] = {" ", "　", "，", "。", "、", "：", "！", "？", "(", ")", "（", "）", "《", "》", "「", "」", "_", "-", ".", ",", ":", "!", "?"};
  for (auto d : DROP) s.replace(d, "");
  while (s.length() && isdigit((uint8_t)s[s.length() - 1])) s.remove(s.length() - 1);
  return s;
}

String songsFolderId() {
  String f = SONGS_MP3_FOLDER;
  int k = f.indexOf("folders/");
  if (k >= 0) { f = f.substring(k + 8); int q = f.indexOf('?'); if (q >= 0) f = f.substring(0, q); }
  return f;
}

// 讀雲端資料夾清單（embeddedfolderview 公開頁），更新 mp3Songs；回傳歌曲數（失敗 -1）
int fetchMp3List() {
  String folder = songsFolderId();
  if (folder.isEmpty()) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.google.com/embeddedfolderview?id=" + folder);
  int code = http.GET();
  String html = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) { Serial.printf("MP3 歌單讀取失敗 %d\n", code); return -1; }
  std::vector<Mp3Song> v;
  int pos = 0;
  for (;;) {
    int a = html.indexOf("id=\"entry-", pos);
    if (a < 0) break;
    a += 10;
    int b = html.indexOf('"', a);
    int t = html.indexOf("flip-entry-title\">", b);
    if (b < 0 || t < 0) break;
    t += 18;
    int e = html.indexOf('<', t);
    String id = html.substring(a, b), title = html.substring(t, e);
    title.replace("&amp;", "&"); title.replace("&#39;", "'"); title.replace("&quot;", "\"");
    if (title.endsWith(".mp3") || title.endsWith(".MP3")) v.push_back({id, title.substring(0, title.length() - 4)});
    pos = e;
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  mp3Songs = v;
  xSemaphoreGive(songLock);
  Serial.printf("MP3 歌單：%d 首\n", (int)v.size());
  return v.size();
}

// 下載整首 MP3 到 PSRAM（80kbps 一首約 2~3MB）；回傳長度，失敗 0
size_t downloadMp3(const String &id, uint8_t **out) {
  *out = nullptr;
  size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  size_t CAP = room > 512 * 1024 ? room - 256 * 1024 : 0;            // 留一點 PSRAM 給其他工作
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.usercontent.google.com/download?id=" + id + "&export=download&confirm=t");
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("MP3 下載失敗 %d\n", code); return 0; }
  int total = http.getSize();
  if (total > 0 && (size_t)total > CAP) {
    http.end();
    Serial.printf("MP3 太大 %d bytes（可用 %u）\n", total, (unsigned)CAP);
    return 0;
  }
  size_t cap = total > 0 ? (size_t)total : CAP;
  uint8_t *buf = cap ? (uint8_t *)ps_malloc(cap) : nullptr;
  size_t len = 0;
  if (buf) {
    WiFiClient *st = http.getStreamPtr();
    uint32_t last = millis();
    while (len < cap && (total < 0 || (int)len < total) && millis() - last < 10000) {
      size_t a = st->available();
      if (a) { len += st->readBytes(buf + len, min(a, cap - len)); last = millis(); }
      else if (!http.connected()) break;
      else delay(2);
    }
  }
  http.end();
  Serial.printf("MP3 下載 %u bytes\n", (unsigned)len);
  if (len < 4096) { free(buf); return 0; }
  *out = buf;
  return len;
}

// 解碼並播放（阻塞到唱完或按 BOOT）；UP/DOWN 可調音量。回傳 true = 被按鍵停止
bool playMp3(uint8_t *data, size_t len) {
  HMP3Decoder h = MP3InitDecoder();
  if (!h) return false;
  static int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
  static int16_t out[MAX_NGRAN * MAX_NSAMP * 2 * 2];
  uint8_t *p = data;
  int left = len;
  uint32_t ramp = 0, samples = 0;
  bool stopped = false, wasPressed = buttonPressed();
  songPosMs = 0;
  songPlaying = true;
  while (left > 0) {
    bool b = buttonPressed();
    if (b && !wasPressed) { stopped = true; break; }                // 按 BOOT：停止
    wasPressed = b;
    if (pollKey(kUp) == K_SHORT) changeVolume(+10);
    if (pollKey(kDown) == K_SHORT) changeVolume(-10);
    int off = MP3FindSyncWord(p, left);
    if (off < 0) break;
    p += off; left -= off;
    int err = MP3Decode(h, &p, &left, pcm, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW) break;
    if (err) { if (left > 0) { p++; left--; } continue; }        // 壞掉的影格：跳過
    MP3FrameInfo fi;
    MP3GetLastFrameInfo(h, &fi);
    int n = fi.outputSamps / max(1, fi.nChans);
    if (songDurMs == 0 && fi.bitrate > 0) songDurMs = (uint32_t)((uint64_t)len * 8000 / fi.bitrate);
    // 轉成 24kHz 立體聲（轉檔時已是 24kHz；其他取樣率用最近鄰換算）
    int outN = fi.samprate == I2S_RATE ? n : (int)((int64_t)n * I2S_RATE / max(1, fi.samprate));
    outN = min(outN, (int)(sizeof(out) / 4));
    int vol = volumePct;
    int64_t acc = 0;
    for (int i = 0; i < outN; i++) {
      int si = fi.samprate == I2S_RATE ? i : (int)((int64_t)i * fi.samprate / I2S_RATE);
      int32_t s = fi.nChans == 2 ? ((int32_t)pcm[si * 2] + pcm[si * 2 + 1]) / 2 : pcm[si];
      s = s * vol / 100;
      const uint32_t RAMP = I2S_RATE / 5;                          // 開頭 0.2 秒淡入，避免爆音
      if (ramp < RAMP) s = s * (int32_t)ramp++ / (int32_t)RAMP;
      s = constrain(s, -32767, 32767);
      out[i * 2] = out[i * 2 + 1] = s;
      acc += abs(s);
    }
    speakLevel = (int)(acc / max(1, outN)) * 2;                    // 嘴巴跟著節奏開合
    lfI2S.write((uint8_t *)out, outN * 4);
    samples += outN;
    songPosMs = (uint32_t)((uint64_t)samples * 1000 / I2S_RATE);
  }
  memset(out, 0, sizeof(out));
  for (int i = 0; i < 4; i++) lfI2S.write((uint8_t *)out, sizeof(out));
  speakLevel = 0;
  songPlaying = false;
  MP3FreeDecoder(h);
  while (buttonPressed()) delay(10);                               // 等放開按鍵，避免接著打開說明
  return stopped;
}

// 找使用者點名的歌：「唱千江有水千江月」→ 對應的 MP3 索引，找不到 -1
int findRequestedMp3(const String &userText) {
  String u = normTitle(userText);
  int best = -1, bestLen = 0;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (size_t i = 0; i < mp3Songs.size(); i++) {
    String t = normTitle(mp3Songs[i].title);
    if (t.length() >= 6 && u.indexOf(t) >= 0 && (int)t.length() > bestLen) { best = i; bestLen = t.length(); }
  }
  xSemaphoreGive(songLock);
  return best;
}

// 用 MP3 歌名找 YouTube 播放清單裡同一首（封面與 QR 碼用）
bool findYoutube(const String &title, String &ytId) {
  String t = normTitle(title);
  bool ok = false;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (auto &s : songs) {
    String y = normTitle(s.title);
    if (y.length() && (y.indexOf(t) >= 0 || t.indexOf(y) >= 0)) { ytId = s.id; ok = true; break; }
  }
  xSemaphoreGive(songLock);
  return ok;
}

void backToMain();

// 唱歌：有雲端 MP3 就直接唱出來（userText 有點歌名就唱那首）；沒有 MP3 才只顯示 YouTube QR 碼
void singSong(const String &userText) {
  setUi(UI_THINK, "選歌中…", "");
  fetchMp3List();                                                  // 每次重讀，雲端新增的歌馬上就能唱
  xSemaphoreTake(songLock, portMAX_DELAY);
  int n = mp3Songs.size();
  xSemaphoreGive(songLock);
  if (!n) { singYoutubeOnly(); return; }

  int idx = findRequestedMp3(userText);
  if (idx < 0) {
    idx = random(n);
    if (n > 1) while (idx == lastSong) idx = random(n);
  }
  lastSong = idx;
  xSemaphoreTake(songLock, portMAX_DELAY);
  Mp3Song m = mp3Songs[idx];
  xSemaphoreGive(songLock);
  Serial.println("唱歌（MP3）：" + m.title);

  String ytId;
  bool hasYt = findYoutube(m.title, ytId);
  thumbOk = hasYt && fetchThumb(ytId);
  if (hasYt) makeQr("https://youtu.be/" + ytId); else qrSize = 0;
  songDurMs = 0; songPosMs = 0;
  songLoading = true;
  portalOpen = false;
  goHome(UI_SONG, m.title, hasYt ? ytId : String(""));
  songSince = millis();

  uint8_t *data = nullptr;                                         // 先下載（和介紹詞同時進行會搶網路，依序做）
  size_t len = downloadMp3(m.id, &data);
  songLoading = false;
  if (!len) {
    speakExpr = EX_SAD;
    goHome(UI_IDLE);
    speakAndShow("這首歌下載失敗，請確認雲端資料夾已經設成知道連結的人都能檢視。");
    backToMain();
    return;
  }
  if (speak("我來唱《" + m.title + "》給你聽！這是" SONG_COMPOSER "創作的歌。")) waitPlaybackDone();
  bool stopped = playMp3(data, len);
  free(data);
  Serial.println(stopped ? "（唱歌：按鍵停止）" : "（唱歌：唱完了）");
  backToMain();
}

// 只有 YouTube 歌單時：介紹歌曲 + QR 碼（手機播放）
void singYoutubeOnly() {
  xSemaphoreTake(songLock, portMAX_DELAY);
  int n = songs.size();
  Song s;
  if (n) {
    int i = random(n);
    if (n > 1) while (i == lastSong) i = random(n);
    lastSong = i;
    s = songs[i];
  }
  xSemaphoreGive(songLock);
  if (!n) {
    speakExpr = EX_SAD;
    speakAndShow("我的歌單還是空的，請到設定網頁填入 YouTube 播放清單網址，再說「更新歌單」喔。");
    return;
  }
  Serial.println("唱歌：" + s.title + " https://youtu.be/" + s.id);
  setUi(UI_THINK, "選歌中…", "《" + s.title + "》");
  thumbOk = fetchThumb(s.id);
  makeQr("https://youtu.be/" + s.id);
  portalOpen = false;
  goHome(UI_SONG, s.title, s.id);
  songSince = millis();
  if (speak("我來唱《" + s.title + "》給你聽！這是" SONG_COMPOSER "創作的歌，用手機掃描 QR 碼，就能在 YouTube 聽完整版喔！"))
    waitPlaybackDone();
  songSince = millis();
  lastInteraction = millis();
}

void backToMain() {
  portalOpen = false;
  goHome(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
}

// ============================================================================
//  工作站語音問答：下載網站資料 → 板子上用關鍵字找最相關段落 → 交給 AI 回答
// ============================================================================
static const char *KNOW_NAME[] = {"照顧服務員技術稽核", "居家服務督導員工作手冊", "照服員單一級學科測驗", "照服員術科練習本", "長照組合計算系統"};
static const char *KNOW_URL[] = {
  "https://achir1015.github.io/caregiver-skills-audit/data.js",
  "https://achir1015.github.io/home-care-supervisor-manual/data.js",
  "https://docs.google.com/spreadsheets/d/1DHrXPAgagusv1DiVVf9hjIGy_IKOCTok2zkfMobNMPY/gviz/tq?tqx=out:csv&sheet=%E5%B7%A5%E4%BD%9C%E8%A1%A81",
  "https://achir1015.github.io/care-exam-practice/",
  "https://achir1015.github.io/Long-term-care-combined-calculation-system/"};
const char *KNOW_EXAMPLE[] = {"「冰枕使用技術的稽核項目有哪些？」", "「居督員家訪要注意什麼？」",
                              "「預防泌尿道感染的考題答案是什麼？」", "「術科洗手的步驟是什麼？」", "「BA01 基本身體清潔一次多少錢？」"};
static const std::initializer_list<const char *> KNOW_KEYS[] = {
  {"稽核"},
  {"督導", "居督", "工作手冊", "家訪", "個案管理"},
  {"學科", "題庫", "考題", "選擇題", "模擬考", "答案是"},
  {"術科", "考場", "操作步驟", "扣分", "易錯"},
  {"組合", "給付", "額度", "碼別", "自付", "部分負擔", "支付價格", "BA0", "BA1", "AA0", "GA0", "SC0"}};
const int KNOW_N = sizeof(KNOW_NAME) / sizeof(KNOW_NAME[0]);
static char *knowText[5] = {nullptr};
static size_t knowLen[5] = {0};
static uint32_t knowAt[5] = {0};

int knowledgeForTool(const String &name) {
  for (int i = 0; i < KNOW_N; i++) if (name == KNOW_NAME[i]) return i;
  return -1;
}

int matchKnowledge(const String &q) {
  String u = q;                                                   // 只把英文字母轉大寫（String::toUpperCase 會弄壞中文）
  for (size_t i = 0; i < u.length(); i++) if (u[i] >= 'a' && u[i] <= 'z') u.setCharAt(i, u[i] - 32);
  for (int i = 0; i < KNOW_N; i++)
    for (auto k : KNOW_KEYS[i]) if (u.indexOf(k) >= 0) return i;
  return -1;
}

bool loadKnowledge(int i) {
  if (knowText[i] && millis() - knowAt[i] < 6UL * 3600 * 1000) return true;   // 6 小時內用快取
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, KNOW_URL[i]);
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("資料下載失敗 %s %d\n", KNOW_NAME[i], code); return knowText[i] != nullptr; }
  const size_t CAP = 1024 * 1024;
  char *buf = (char *)ps_malloc(CAP + 1);
  size_t len = 0;
  if (buf) {
    WiFiClient *st = http.getStreamPtr();
    int total = http.getSize();
    uint32_t last = millis();
    while (len < CAP && (total < 0 || (int)len < total) && millis() - last < 8000) {
      size_t a = st->available();
      if (a) { len += st->readBytes(buf + len, min(a, CAP - len)); last = millis(); }
      else if (!http.connected()) break;
      else delay(2);
    }
    buf[len] = 0;
  }
  http.end();
  if (!buf || len < 100) { free(buf); return knowText[i] != nullptr; }
  free(knowText[i]);
  knowText[i] = buf; knowLen[i] = len; knowAt[i] = millis();
  Serial.printf("資料載入 %s %u bytes\n", KNOW_NAME[i], (unsigned)len);
  return true;
}

// 找出和問題最相關的段落（以中文兩字詞比對），最多約 3500 bytes
String knowledgeContext(int i, const String &question) {
  if (!loadKnowledge(i)) return "";
  std::vector<String> grams;
  const char *q = question.c_str();
  int ql = question.length();
  for (int p = 0; p + 6 <= ql;) {                                 // 中文字 UTF-8 每字 3 bytes：取相鄰兩字
    uint8_t c = q[p];
    if ((c >> 4) == 14 && ((uint8_t)q[p + 3] >> 4) == 14) {
      String g = question.substring(p, p + 6);
      if (g != "什麼" && g != "哪些" && g != "多少" && g != "是不" && g != "可以" && g != "請問" && g != "怎麼" && g != "如何")
        grams.push_back(g);
      p += 3;
    } else p += (c < 0x80) ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
  }
  for (int p = 0; p < ql;) {                                      // 英數字詞（例如 BA01）
    if (isalnum((uint8_t)q[p])) { int e = p; while (e < ql && isalnum((uint8_t)q[e])) e++; if (e - p >= 2) grams.push_back(question.substring(p, e)); p = e; }
    else p++;
  }
  const char *t = knowText[i];
  size_t len = knowLen[i];
  const size_t CH = 700;
  struct Hit { size_t pos; int score; };
  std::vector<Hit> hits;
  for (size_t pos = 0; pos < len; pos += CH / 2) {                // 段落重疊一半，避免答案剛好被切開
    size_t end = min(len, pos + CH);
    int score = 0;
    for (auto &g : grams)
      if (std::search(t + pos, t + end, g.c_str(), g.c_str() + g.length()) != t + end) score += g.length() > 6 ? 3 : 2;
    if (score > 0) hits.push_back({pos, score});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
  String out = String("以下是「") + KNOW_NAME[i] + "」的資料片段（原始格式，可能含程式碼符號），請根據內容回答，資料沒有的不要亂編：\n";
  std::vector<size_t> used;
  for (auto &h : hits) {
    if (out.length() > 3500) break;
    bool dup = false;
    for (auto u : used) if ((h.pos > u ? h.pos - u : u - h.pos) < CH) dup = true;
    if (dup) continue;
    used.push_back(h.pos);
    size_t s = h.pos, e = min(len, h.pos + CH);
    while (s > 0 && ((uint8_t)t[s] & 0xC0) == 0x80) s--;          // 對齊 UTF-8 字元邊界
    while (e < len && ((uint8_t)t[e] & 0xC0) == 0x80) e++;
    out += "…" + String(t + s).substring(0, e - s) + "…\n";
  }
  Serial.printf("[問答] %s：%d 個關鍵詞、命中 %d 段，送出 %u bytes\n", KNOW_NAME[i], (int)grams.size(), (int)hits.size(), (unsigned)out.length());
  return hits.empty() ? String("「") + KNOW_NAME[i] + "」的資料裡找不到和問題相關的內容。\n" : out;
}

// ============================================================================
//  病房助理工作站：工具清單（從工作站網頁解析）
// ============================================================================
// 取出 key:'值' 或 key:"值"
String jsField(const String &s, const char *key) {
  String k = String(key) + ":";
  int a = s.indexOf(k);
  if (a < 0) return "";
  a += k.length();
  while (a < (int)s.length() && s[a] == ' ') a++;
  if (a >= (int)s.length()) return "";
  char q = s[a];
  if (q != '\'' && q != '"') return "";
  int b = s.indexOf(q, a + 1);
  return b > a ? s.substring(a + 1, b) : String("");
}

int fetchPortalTools() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, PORTAL_URL);
  int code = http.GET();
  String html = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) { Serial.printf("工作站清單讀取失敗 %d\n", code); return -1; }
  std::vector<PortalTool> tools;
  tools.push_back({PORTAL_NAME "首頁", "工作站入口，所有工具都在這裡。", PORTAL_URL});
  for (int p = 0; (p = html.indexOf("new Tool({", p)) >= 0; p++) {
    int e = html.indexOf("})", p);
    String s = html.substring(p, e);
    PortalTool t{jsField(s, "name"), jsField(s, "desc"), jsField(s, "url")};
    if (t.name.length() && t.url.length()) tools.push_back(t);
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  portalTools = tools;
  xSemaphoreGive(songLock);
  Serial.printf("工作站：%d 個工具\n", (int)tools.size() - 1);
  return tools.size();
}

void openPortalMenu() {
  portalOpen = true;
  portalSel = 0;
  if (portalTools.empty()) fetchPortalTools();
  goHome(UI_PORTAL);
  Serial.println("（開啟" PORTAL_NAME "選單）");
}

// 顯示選中工具的 QR 碼與說明
void showToolQr(int i) {
  xSemaphoreTake(songLock, portMAX_DELAY);
  bool ok = i >= 0 && i < (int)portalTools.size();
  PortalTool t = ok ? portalTools[i] : PortalTool{};
  xSemaphoreGive(songLock);
  if (!ok) return;
  makeQr(t.url);
  int k = knowledgeForTool(t.name);
  String text = t.desc + "\n" + (k >= 0 ? String("可以直接用說的問我，例如：") + KNOW_EXAMPLE[k] : String("請用手機掃描 QR 碼開啟。"));
  goHome(UI_QR, t.name, text);
}

// ============================================================================
//  班表查詢：Google Firebase Firestore（REST API）
// ============================================================================
bool looksLikeSchedule(const String &t) {
  return containsAny(t, {"班表", "上班", "上什麼班", "值班", "排班", "輪班", "休假", "休幾天", "放假", "休息幾天",
                         "大夜", "小夜", "白班", "夜班", "上幾天班", "哪天休", "什麼班"});
}

// 從問題判斷要查哪幾個月（預設本月；月底順便查下個月）
std::vector<std::pair<int, int>> scheduleMonths(const String &t) {
  struct tm now;
  getLocalTime(&now, 10);
  int y = now.tm_year + 1900, m = now.tm_mon + 1;
  std::vector<std::pair<int, int>> v;
  auto add = [&](int yy, int mm) {
    while (mm > 12) { mm -= 12; yy++; }
    while (mm < 1) { mm += 12; yy--; }
    for (auto &x : v) if (x.first == yy && x.second == mm) return;
    v.push_back({yy, mm});
  };
  static const char *CNUM[] = {"十二", "十一", "十", "九", "八", "七", "六", "五", "四", "三", "二", "一"};
  static const int CVAL[] = {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  for (int pos = t.indexOf("月"); pos >= 0; pos = t.indexOf("月", pos + 1)) {
    int mm = 0, k = pos;
    while (k > 0 && isdigit((uint8_t)t[k - 1])) k--;
    if (k < pos) mm = t.substring(k, pos).toInt();
    else
      for (int i = 0; i < 12 && !mm; i++) {
        int L = strlen(CNUM[i]);
        if (pos >= L && t.substring(pos - L, pos) == CNUM[i]) mm = CVAL[i];
      }
    if (mm >= 1 && mm <= 12 && t.substring(max(0, pos - 3), pos) != "個") add(mm < m - 6 ? y + 1 : y, mm);
  }
  if (t.indexOf("下個月") >= 0 || t.indexOf("下月") >= 0) add(y, m + 1);
  if (t.indexOf("上個月") >= 0 || t.indexOf("上月") >= 0) add(y, m - 1);
  if (v.empty()) {
    add(y, m);
    if (now.tm_mday >= 24) add(y, m + 1);           // 月底問「下週」可能跨月
  }
  return v;
}

// 讀取一個月的班表，轉成精簡文字（10 分鐘快取）
String fetchScheduleMonth(const char *cat, const char *label, int y, int m) {
  static String cKey[8], cVal[8];
  static uint32_t cAt[8] = {0};
  char doc[48];
  snprintf(doc, sizeof(doc), "%s-%04d-%02d", cat, y, m);
  for (int i = 0; i < 8; i++)
    if (cKey[i] == doc && millis() - cAt[i] < 600000UL) return cVal[i];

  String url = String("https://firestore.googleapis.com/v1/projects/" FIREBASE_PROJECT "/databases/(default)/documents/schedules/") + doc;
  if (strlen(FIREBASE_API_KEY)) url += "?key=" FIREBASE_API_KEY;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, url);
  int code = http.GET();
  String out;
  if (code == 404) {
    out = String(y) + "年" + m + "月" + label + "班表：尚未建立。\n";
  } else if (code != 200) {
    Serial.printf("班表讀取失敗 %d\n", code);
    http.end();
    return String(y) + "年" + m + "月" + label + "班表：讀取失敗（" + code + "）。\n";
  } else {
    JsonDocument filter;
    JsonObject f = filter["fields"]["employees"]["arrayValue"]["values"][0]["mapValue"]["fields"].to<JsonObject>();
    f["name"]["stringValue"] = true;
    f["shifts"]["mapValue"]["fields"] = true;
    JsonDocument d;
    DeserializationError err = deserializeJson(d, http.getString(), DeserializationOption::Filter(filter),
                                               DeserializationOption::NestingLimit(24));   // Firestore 巢狀約 12 層
    if (err) { Serial.printf("班表 JSON 錯誤：%s\n", err.c_str()); http.end(); return String(y) + "年" + m + "月" + label + "班表：格式錯誤。\n"; }
    static const char *W[] = {"日", "一", "二", "三", "四", "五", "六"};
    int days = daysSince2000(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - daysSince2000(y, m, 1);
    out = String(y) + "年" + m + "月" + label + "班表（星期：";
    for (int dd = 1; dd <= days; dd++) out += String(dd) + W[(daysSince2000(y, m, dd) + 6) % 7] + (dd < days ? " " : "）\n");
    for (JsonObject e : d["fields"]["employees"]["arrayValue"]["values"].as<JsonArray>()) {
      JsonObject ef = e["mapValue"]["fields"];
      String name = ef["name"]["stringValue"] | "";
      out += name + (name == SCHEDULE_MY_NAME ? "（我）" : "") + "：";
      JsonObject sh = ef["shifts"]["mapValue"]["fields"];
      for (int dd = 1; dd <= days; dd++) {
        const char *v = sh[String(dd)]["stringValue"] | "";
        if (*v) out += String(dd) + "=" + v + " ";
      }
      out += "\n";
    }
  }
  http.end();
  int slot = 0;
  for (int i = 1; i < 8; i++) if (cAt[i] < cAt[slot]) slot = i;
  cKey[slot] = doc; cVal[slot] = out; cAt[slot] = millis();
  Serial.printf("班表 %s：%d bytes\n", doc, out.length());
  return out;
}

// ============================================================================
//  工作流程查詢：Firebase「線上專案進度時間軸表」
// ============================================================================
bool looksLikeWorkflow(const String &t) {
  return containsAny(t, {"流程", "工作", "幾點", "要做什麼", "該做什麼", "做什麼事", "交班", "接班", "庫存", "專案",
                         "任務", "時間軸", "注意什麼", "要注意", "洗澡班", "教學班"});
}

String utf8Prefix(const String &s, int maxChars) {     // 依字數（不是位元組）截斷
  int chars = 0;
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (++chars > maxChars) return s.substring(0, i) + "…";
    i += len;
  }
  return s;
}

String hhmm(long mins) {
  mins = ((mins % 1440) + 1440) % 1440;
  char b[8];
  snprintf(b, sizeof(b), "%02ld:%02ld", mins / 60, mins % 60);
  return b;
}

String firestoreGet(const String &path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, "https://firestore.googleapis.com/v1/projects/" TIMELINE_PROJECT "/databases/(default)/documents/" + path + "?pageSize=100");
  int code = http.GET();
  String body = code == 200 ? http.getString() : String("");
  if (code != 200) Serial.printf("時間軸讀取失敗 %s：%d\n", path.c_str(), code);
  http.end();
  return body;
}

// 讀取所有專案的班別與任務，轉成精簡文字（10 分鐘快取）
String fetchTimeline() {
  static String cache;
  static uint32_t cacheAt = 0;
  if (cache.length() && millis() - cacheAt < 600000UL) return cache;

  JsonDocument pf;
  JsonObject pff = pf["documents"][0].to<JsonObject>();
  pff["name"] = true;
  pff["fields"]["name"]["stringValue"] = true;
  pff["fields"]["legacy"]["booleanValue"] = true;
  JsonDocument pd;
  if (deserializeJson(pd, firestoreGet("projects"), DeserializationOption::Filter(pf), DeserializationOption::NestingLimit(24)))
    return "工作流程資料讀取失敗。\n";

  String out;
  for (JsonObject pj : pd["documents"].as<JsonArray>()) {
    String id = pj["name"] | "";
    id = id.substring(id.lastIndexOf('/') + 1);
    String pname = pj["fields"]["name"]["stringValue"] | "（未命名專案）";
    bool legacy = pj["fields"]["legacy"]["booleanValue"] | false;
    out += "【工作流程：" + pname + "】\n";

    JsonDocument sf;
    JsonObject sff = sf["documents"][0]["fields"].to<JsonObject>();
    sff["name"]["stringValue"] = true;
    JsonObject tf = sff["tasks"]["arrayValue"]["values"][0]["mapValue"]["fields"].to<JsonObject>();
    tf["name"]["stringValue"] = true;
    tf["desc"]["stringValue"] = true;
    tf["startMin"] = true;
    tf["endMin"] = true;
    JsonDocument sd;
    String path = legacy ? String("shifts") : "projects/" + id + "/shifts";
    if (deserializeJson(sd, firestoreGet(path), DeserializationOption::Filter(sf), DeserializationOption::NestingLimit(24))) {
      out += "（讀取失敗）\n";
      continue;
    }
    for (JsonObject sh : sd["documents"].as<JsonArray>()) {
      JsonObject f = sh["fields"];
      out += String(f["name"]["stringValue"] | "班別") + "：\n";
      struct T { long st, en; String name, desc; };
      std::vector<T> tasks;
      for (JsonObject t : f["tasks"]["arrayValue"]["values"].as<JsonArray>()) {
        JsonObject tf2 = t["mapValue"]["fields"];
        auto num = [](JsonObject v) -> long {
          if (v["integerValue"].is<const char *>()) return atol(v["integerValue"]);
          if (v["doubleValue"].is<double>()) return (long)v["doubleValue"].as<double>();
          if (v["stringValue"].is<const char *>()) return atol(v["stringValue"]);
          return 0;
        };
        tasks.push_back({num(tf2["startMin"]), num(tf2["endMin"]), tf2["name"]["stringValue"] | "",
                         tf2["desc"]["stringValue"] | ""});
      }
      std::sort(tasks.begin(), tasks.end(), [](const T &a, const T &b) { return a.st < b.st; });
      for (auto &t : tasks) {
        String d = t.desc;
        d.replace("\n", " ");
        out += " " + hhmm(t.st) + "-" + hhmm(t.en) + " " + t.name + (d.length() ? "：" + utf8Prefix(d, TIMELINE_DESC_MAX) : "") + "\n";
      }
    }
  }
  cache = out;
  cacheAt = millis();
  Serial.printf("工作流程：%d bytes\n", out.length());
  return out;
}

// ============================================================================
//  一次完整對話
// ============================================================================
bool containsAny(const String &s, std::initializer_list<const char *> words) {
  for (auto w : words) if (s.indexOf(w) >= 0) return true;
  return false;
}

// 語音辨識在雜音/靜音時常出現的幻覺字句
bool isNoiseTranscript(const String &t) {
  if (t.length() < 3) return true;                       // 少於一個中文字
  return containsAny(t, {"字幕", "訂閱", "觀看", "點贊", "點讚", "Amara", "明鏡", "請用台灣繁體中文轉寫", "謝謝大家"});
}

void showError(const String &msg) {
  setUi(UI_MSG, "哎呀，出了點問題", msg, true);
  delay(4000);
}

void speakAndShow(const String &reply) {
  setUi(UI_SPEAK, ROBOT_NAME "：", reply);
  if (!speak(reply)) { showError(lastApiError); return; }
  waitPlaybackDone();
  delay(300);
}

void handleUtterance(size_t samples) {
  // 1) 語音轉文字
  setUi(UI_THINK, "聽聽看你說了什麼…", "");
  lastApiError = "";
  String userText = speechToText(samples);
  if (userText.isEmpty() || isNoiseTranscript(userText)) {
    if (lastApiError.length()) showError(lastApiError);
    else raiseNoiseFloor(lastRecAvgRms * (lastRecHitMax ? 0.9f : 0.7f));   // 聽不出內容 = 噪音，提高門檻
    Serial.println("（忽略：" + userText + "）");
    return;
  }
  Serial.println("你：" + userText);
  lastInteraction = millis();
  homeSince = millis();

  // 2) 本地指令
  if ((homeState == UI_PORTAL || homeState == UI_QR) &&
      containsAny(userText, {"關閉工作站", "離開工作站", "回主畫面", "回到主畫面", "關掉"})) {
    backToMain();
    return;
  }
  if ((containsAny(userText, {"工作站", "病房助理"}) && !containsAny(userText, {"關閉", "離開", "回主畫面"})) ||
      (matchKnowledge(userText) < 0 &&
       containsAny(userText, {"長期照顧", "長照", "照服員", "照顧服務員", "居服員", "居家服務", "居家督導",
                              "督導員", "長照2.0", "長照3.0", "長照二點零", "長照三點零"}))) {
    openPortalMenu();
    speakExpr = EX_HAPPY;
    speakAndShow("這是" PORTAL_NAME "，用上下鍵選工具，按 BOOT 看說明和 QR 碼；技術稽核、督導手冊、學科、術科和長照組合的內容，也可以直接用說的問我喔！");
    return;
  }
  if (containsAny(userText, {"唱歌", "唱首歌", "唱一首", "唱個歌", "來首歌", "來一首", "放歌", "聽歌", "播歌", "放首歌", "播首歌", "播一首", "放一首",
                             "聽首歌", "聽一首", "放音樂", "播音樂", "聽音樂", "來點音樂"}) ||
      (containsAny(userText, {"唱", "播", "放", "聽"}) && findRequestedMp3(userText) >= 0)) {   // 點歌：「唱千江有水千江月」
    singSong(userText);
    return;
  }
  String reply;
  if (containsAny(userText, {"更新歌單"})) {
    setUi(UI_THINK, "更新歌單中…", "");
    int cnt = refreshPlaylist();
    reply = cnt > 0 ? "歌單更新好了，一共有" + String(cnt) + "首歌！" : "歌單更新失敗，請檢查播放清單網址。";
  } else if (containsAny(userText, {"大聲", "音量調大", "音量大一點"})) {
    volumePct = min(100, volumePct + 20);
    saveVolume();
    reply = "好的，音量調到百分之" + String(volumePct) + "。";
  } else if (containsAny(userText, {"小聲", "音量調小", "音量小一點"})) {
    volumePct = max(10, volumePct - 20);
    saveVolume();
    reply = "好的，音量調到百分之" + String(volumePct) + "。";
  } else if (containsAny(userText, {"清除記憶", "忘記剛剛", "重新開始"})) {
    histCount = 0;
    reply = "好的，我已經把剛剛的對話忘掉了。";
  }

  speakExpr = EX_HAPPY;

  // 3) 問 GPT（需要時先查班表 / 工作流程 / 工作站資料）
  if (reply.isEmpty()) {
    bool wantSchedule = looksLikeSchedule(userText);
    bool wantWorkflow = looksLikeWorkflow(userText);
    int know = (!wantSchedule && !wantWorkflow) ? matchKnowledge(userText) : -1;
    setUi(UI_THINK, wantSchedule ? "查詢班表中…" : wantWorkflow ? "查詢工作流程中…"
                    : know >= 0 ? String("查詢《") + KNOW_NAME[know] + "》中…"
                    : (ENABLE_WEB_SEARCH && looksLikeSearch(userText)) ? "上網查詢中…" : "思考中…",
          "你：" + userText);
    chatContext = "";
    if (wantSchedule)
      for (auto &ym : scheduleMonths(userText)) {
        static const char *CATS[] = SCHEDULE_CATEGORIES, *LABELS[] = SCHEDULE_LABELS;
        for (size_t c = 0; c < sizeof(CATS) / sizeof(CATS[0]); c++)
          chatContext += fetchScheduleMonth(CATS[c], LABELS[c], ym.first, ym.second);
      }
    if (wantWorkflow) chatContext += fetchTimeline();
    if (know >= 0) chatContext += knowledgeContext(know, userText);
    reply = chatWithGPT(userText);
    chatContext = "";
    if (reply.isEmpty()) {
      showError(lastApiError.length() ? lastApiError : "AI 沒有回應");
      return;
    }
  }
  Serial.println(String(ROBOT_NAME "：") + reply);

  // 4) 說出回答
  speakAndShow(reply);
  lastInteraction = millis();
}

// ============================================================================
//  初始化
// ============================================================================
void initDisplay() {
  analogWrite(LF_LCD_BL, constrain(LCD_BRIGHTNESS, 10, 100) * 255 / 100);   // 背光
  tftSPI.begin(LF_LCD_SCK, -1, LF_LCD_MOSI, LF_LCD_CS);
  tft.init(240, 320, SPI_MODE0);
  tft.setSPISpeed(40000000);
  tft.setRotation(TFT_ROTATION);
  if (TFT_BGR) {                                  // 紅藍對調：MADCTL 加 BGR 位元
    static const uint8_t MAD[] = {0x00, 0x60, 0xC0, 0xA0};
    uint8_t m = MAD[TFT_ROTATION & 3] | 0x08;
    tft.sendCommand(ST77XX_MADCTL, &m, 1);
  }
  tft.invertDisplay(TFT_INVERT);
  tft.fillScreen(C_BG);
}

bool initAudio() {
  pinMode(LF_PA_EN, OUTPUT);
  digitalWrite(LF_PA_EN, LOW);
  Wire.begin(LF_I2C_SDA, LF_I2C_SCL, 100000);
  lfI2S.setPins(LF_I2S_BCLK, LF_I2S_WS, LF_I2S_DOUT, LF_I2S_DIN, LF_I2S_MCLK);
  bool okI2S = lfI2S.begin(I2S_MODE_STD, I2S_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  delay(20);                                    // 等 MCLK 穩定再設定音訊晶片
  i2cScan();
  bool okDac = es8311Init(MIC_PGA_GAIN);
  micES7210 = es7210Init(MIC_PGA_GAIN);
  digitalWrite(LF_PA_EN, HIGH);
  Serial.printf("I2S: %s, ES8311: %s, 麥克風: %s\n", okI2S ? "OK" : "FAIL", okDac ? "OK" : "FAIL",
                micES7210 ? "ES7210" : "ES8311 ADC");
  return okI2S && okDac;
}

// ============================================================================
//  設定網頁
// ============================================================================
String maskKey(const String &k) {
  if (k.length() < 12) return k.length() ? "已設定" : "";
  return k.substring(0, 7) + "…" + k.substring(k.length() - 4);
}

void handleStatus() {
  JsonDocument d;
  d["name"] = ROBOT_NAME;
  d["mode"] = portalMode ? "ap" : "sta";
  d["ssid"] = cfg.ssid;
  d["hasPass"] = cfg.pass.length() > 0;
  d["apiKey"] = maskKey(cfg.apiKey);
  d["playlist"] = cfg.playlist;
  xSemaphoreTake(songLock, portMAX_DELAY);
  d["songs"] = (int)songs.size();
  xSemaphoreGive(songLock);
  d["ip"] = portalMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["rssi"] = portalMode ? 0 : WiFi.RSSI();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

struct Net { String ssid; int rssi; bool secure; };

void handleScan() {
  int n = WiFi.scanNetworks();
  std::vector<Net> nets;
  for (int i = 0; i < n; i++) {                       // 同名只留訊號最強的
    String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    auto it = std::find_if(nets.begin(), nets.end(), [&](const Net &x) { return x.ssid == ss; });
    if (it != nets.end()) { it->rssi = max(it->rssi, (int)WiFi.RSSI(i)); continue; }
    nets.push_back({ss, (int)WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (auto &x : nets) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = x.ssid;
    o["rssi"] = x.rssi;
    o["secure"] = x.secure;
  }
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleSave() {
  String ssid = server.arg("ssid"), pass = server.arg("pass"), key = server.arg("apikey"), pl = server.arg("playlist");
  ssid.trim(); key.trim(); pl.trim();
  if (ssid.isEmpty()) { server.send(400, "text/plain; charset=utf-8", "請選擇 Wi-Fi"); return; }
  prefs.begin("robot", false);
  // 密碼空白：同一個網路 = 不變更；換了網路 = 無密碼網路
  prefs.putString("pass", pass.length() || ssid != cfg.ssid ? pass : cfg.pass);
  prefs.putString("ssid", ssid);
  prefs.putString("apikey", key.length() ? key : cfg.apiKey);
  if (pl != cfg.playlist) { prefs.putString("playlist", pl); prefs.remove("songs"); }
  prefs.end();
  server.send(200, "text/plain; charset=utf-8", "ok");
  Serial.println("設定已儲存，重新啟動");
  setUi(UI_MSG, "設定已儲存", "重新啟動中…");
  delay(1200);
  ESP.restart();
}

void webTask(void *) {
  for (;;) {
    if (portalMode) dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void startWebServer() {
  static bool started = false;
  if (started) return;
  started = true;
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_POST, [] {
    int n = refreshPlaylist();
    server.send(n > 0 ? 200 : 500, "text/plain; charset=utf-8", n > 0 ? String(n) : String("更新失敗"));
  });
  server.onNotFound([] {                               // 設定模式下所有網址都導向設定頁（手機會自動跳出）
    if (portalMode) { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 16384, nullptr, 1, nullptr, 0);
}

// 連不上 Wi-Fi 或尚未設定：開熱點 + 設定網頁，停在這裡等使用者儲存（儲存後會重新啟動）
void runSetupPortal(const String &reason) {
  portalMode = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  dnsServer.start(53, "*", WiFi.softAPIP());
  snprintf(ipText, sizeof(ipText), "http://%s", WiFi.softAPIP().toString().c_str());
  startWebServer();
  Serial.printf("設定模式：%s，熱點 %s / %s，http://%s\n", reason.c_str(), AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  setUi(UI_MSG, "設定模式", reason + "\n1. 手機連 Wi-Fi「" AP_SSID "」密碼 " AP_PASS "\n2. 開啟 http://192.168.4.1\n3. 選 Wi-Fi、填密碼與 API Key");
  while (true) delay(1000);
}

// allowPortal = 開機時連不上就進設定模式；執行中斷線則重新啟動
void connectWiFi(bool allowPortal) {
  if (cfg.ssid.isEmpty()) runSetupPortal("尚未設定 Wi-Fi");
  setUi(UI_MSG, "連線 Wi-Fi 中…", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    if (millis() - t0 > 20000) {
      if (allowPortal) runSetupPortal("連不上「" + cfg.ssid + "」");
      setUi(UI_MSG, "Wi-Fi 斷線", "重新啟動中…", true);
      delay(3000);
      ESP.restart();
    }
  }
  snprintf(ipText, sizeof(ipText), "http://%s", WiFi.localIP().toString().c_str());
  Serial.printf("WiFi IP: %s（設定網頁 %s 或 http://xiaoke.local）\n", WiFi.localIP().toString().c_str(), ipText);
  MDNS.begin("xiaoke");
  MDNS.addService("http", "tcp", 80);
  startWebServer();
  configTzTime("CST-8", "time.stdtime.gov.tw", "pool.ntp.org");
  restoreHome();
}

// 背景：更新歌單與工作站工具清單
void bgFetchTask(void *) {
  refreshPlaylist();
  fetchMp3List();
  fetchPortalTools();
  vTaskDelete(nullptr);
}

// ============================================================================
//  硬體測試模式（HW_TEST_MODE = 1）
// ============================================================================
void hardwareTestLoop() {
  uint16_t colors[] = {C_RED, C_GREEN, C_BLUE, C_WHITE};
  for (auto col : colors) { tft.fillScreen(col); delay(400); }
  tft.fillScreen(C_BG);
  u8f.setTarget(&tft);
  u8f.setForegroundColor(C_WHITE);
  u8f.drawUTF8(4, 20, "硬體測試：螢幕 OK（紅綠藍白）");
  u8f.setForegroundColor(C_RED);   u8f.drawUTF8(4, 40, "紅");
  u8f.setForegroundColor(C_GREEN); u8f.drawUTF8(28, 40, "綠");
  u8f.setForegroundColor(C_BLUE);  u8f.drawUTF8(52, 40, "藍");
  u8f.setForegroundColor(C_WHITE);
  u8f.drawUTF8(4, 60, "喇叭：播放嗶聲…");
  int16_t st[480];                               // 1kHz 立體聲
  for (int i = 0; i < 240; i++) st[i * 2] = st[i * 2 + 1] = (int16_t)(8000 * sin(2 * PI * i / 24.0));
  for (int k = 0; k < 100; k++) lfI2S.write((uint8_t *)st, sizeof(st));
  memset(st, 0, sizeof(st));
  for (int k = 0; k < 8; k++) lfI2S.write((uint8_t *)st, sizeof(st));

  u8f.drawUTF8(4, 80, micES7210 ? "麥克風（ES7210）：說話看音量條" : "麥克風（ES8311）：說話看音量條");
  u8f.drawUTF8(4, 150, "按鍵：BOOT=橘 UP=綠 DOWN=藍");
  int16_t f[FRAME];
  uint32_t t0 = millis();
  while (millis() - t0 < 6000) {
    int rms = readFrame(f);
    int w = constrain(map(rms, 0, 3000, 0, 300), 0, 300);
    tft.fillRect(10, 100, w, 20, C_GREEN);
    tft.fillRect(10 + w, 100, 300 - w, 20, C_GRAY);
    tft.fillRect(10, 160, 60, 20, buttonPressed() ? C_ORANGE : C_BG);
    tft.fillRect(80, 160, 60, 20, keysEnabled && digitalRead(PIN_KEY_UP) == LOW ? C_GREEN : C_BG);
    tft.fillRect(150, 160, 60, 20, keysEnabled && digitalRead(PIN_KEY_DOWN) == LOW ? C_BLUE : C_BG);
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 200) {
      lastLog = millis();
      Serial.printf("mic rms=%d boot=%d up=%d down=%d\n", rms, buttonPressed(), digitalRead(PIN_KEY_UP) == LOW, digitalRead(PIN_KEY_DOWN) == LOW);
    }
  }
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) delay(10);   // USB 序列埠：等電腦連上再印開機紀錄
  Serial.println("\n=== LAFVIN ESP32S3 AIChatBot「" ROBOT_NAME "」OpenAI 版 ===");

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  if (usbHostAttached()) {
    keysEnabled = false;
    Serial.println("原生 USB 接著電腦：停用 UP/DOWN 鍵（請改用 UART 孔供電才能用按鍵）");
  } else {
    pinMode(PIN_KEY_UP, INPUT_PULLUP);
    pinMode(PIN_KEY_DOWN, INPUT_PULLUP);
  }
  prefs.begin("robot", true);
  volumePct = prefs.getInt("vol", DEFAULT_VOLUME);
  prefs.end();
  initDisplay();

  if (!psramFound()) {
    u8f.drawUTF8(4, 40, "找不到 PSRAM：請選 OPI PSRAM");
    Serial.println("找不到 PSRAM");
    while (true) delay(1000);
  }
  wavBuf = (uint8_t *)ps_malloc(44 + MAX_SAMPLES * 2);
  tts.buf = (uint8_t *)ps_malloc(TTS_BUF_BYTES);
  thumbPix = (uint16_t *)ps_malloc(320 * 180 * 2);
  songLock = xSemaphoreCreateMutex();

  bool audioOK = initAudio();
  xTaskCreatePinnedToCore(playTask, "play", 4096, nullptr, 5, &playTaskHandle, 1);

#if HW_TEST_MODE
  (void)audioOK;
  return;
#endif

  // 畫面改由 UI task 以 PSRAM 畫布繪製
  cv = new GFXcanvas16(SCREEN_W, SCREEN_H);
  u8f.setTarget(cv);
  uiLock = xSemaphoreCreateMutex();
  lastInteraction = millis();
  buildHelp();
  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 1, nullptr, 0);

  if (!audioOK) showError("音訊初始化失敗（ES8311 沒有回應）");
  loadSettings();
  loadSongsFromNvs();
  connectWiFi(true);
  xTaskCreatePinnedToCore(bgFetchTask, "bgfetch", 16384, nullptr, 1, nullptr, 1);
  if (cfg.apiKey.isEmpty()) {
    setUi(UI_MSG, "尚未設定 API Key", String("請用手機或電腦開啟\n") + ipText + "\n輸入 OpenAI API Key");
    while (true) delay(1000);
  }

  // 校正背景噪音
  int16_t f[FRAME];
  float acc = 0;
  flushMic(200);
  for (int i = 0; i < 50; i++) acc += readFrame(f);
  noiseFloor = acc / 50;
  Serial.printf("背景噪音 rms=%.0f，觸發門檻=%d\n", noiseFloor, vadThreshold());

  speakAndShow(VOICE_ACTIVATION ? "嗨，我是" ROBOT_NAME "！隨時可以直接跟我說話喔。"
                                : "嗨，我是" ROBOT_NAME "！按住 BOOT 鍵跟我說話吧。");
  flushMic(300);
  goHome(UI_IDLE);
}

// ---- 功能說明：BOOT/DOWN 下一頁、UP 上一頁，最後一頁再按回到聊天 ----
uint32_t helpSince = 0;
bool helpSpoken = false;

void exitHelp() {
  restoreHome();
  flushMic(200);
  lastInteraction = millis();
}

void helpStep(int dir) {
  helpSince = millis();
  lastInteraction = millis();
  if (uiState != UI_HELP) {
    helpPage = 0;
    helpSpoken = false;
    setUi(UI_HELP);
    return;
  }
  if (dir < 0) { if (helpPage > 0) helpPage = helpPage - 1; setUi(UI_HELP); return; }
  if (helpPage + 1 >= (int)helpPages.size()) { exitHelp(); return; }
  helpPage = helpPage + 1;
  setUi(UI_HELP);
  if (helpPage == (int)helpPages.size() - 1 && !helpSpoken) {     // 開發者頁：小柯親口介紹
    helpSpoken = true;
    if (speak("這個系統是吳玉柱先生和 Claude AI 共同開發的，有任何意見，歡迎寫信給吳先生喔！")) waitPlaybackDone();
    helpSince = millis();
  }
}

// 按住 BOOT 說話
void pushToTalk() {
  Serial.println("（按住說話）");
  size_t n = recordUtterance(true);
  if (n) handleUtterance(n);
  flushMic(300);
  if (uiState != UI_SONG) restoreHome();
}

void loop() {
#if HW_TEST_MODE
  hardwareTestLoop();
  return;
#endif
  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);

  // ---- 按鍵 ----
  KeyEv up = pollKey(kUp), dn = pollKey(kDown);
  bool bootClick = false, bootHold = false;
  static bool btnPrev = false;
  bool btn = buttonPressed();
  if (btn && !btnPrev) {
    delay(30);
    if (buttonPressed()) {
      uint32_t t0 = millis();                              // 短按 = 功能 / 下一頁；按住 = 說話
      while (buttonPressed() && millis() - t0 < 450) delay(10);
      if (buttonPressed()) bootHold = true; else bootClick = true;
    }
  }
  btnPrev = buttonPressed();
  if (up != K_NONE || dn != K_NONE || bootClick || bootHold) homeSince = millis();

  UiState st = uiState;
  if (st == UI_HELP) {                                     // 看說明時暫停聲控，90 秒沒動作自動回到聊天
    if (bootClick || dn == K_SHORT) helpStep(+1);
    else if (up == K_SHORT) helpStep(-1);
    else if (bootHold || up == K_LONG) exitHelp();
    else if (millis() - helpSince > 90000) exitHelp();
    delay(20);
    return;
  }
  if (st == UI_SONG) {                                     // 唱歌畫面：暫停聲控（手機正在放歌），時間到回到聊天
    if (bootClick || bootHold || up != K_NONE || dn != K_NONE || millis() - songSince > SONG_SHOW_SEC * 1000UL) backToMain();
    delay(20);
    return;
  }
  if (st == UI_PORTAL) {
    int n = portalTools.size();
    if (up == K_SHORT && n) portalSel = (portalSel + n - 1) % n;
    else if (dn == K_SHORT && n) portalSel = (portalSel + 1) % n;
    else if (bootClick) showToolQr(portalSel);
    else if (up == K_LONG) { backToMain(); return; }
    else if (bootHold) { pushToTalk(); return; }
    if (millis() - homeSince > PORTAL_SHOW_SEC * 1000UL) backToMain();
  } else if (st == UI_QR) {
    if (up == K_LONG || (!portalOpen && (bootClick || up == K_SHORT || dn == K_SHORT))) { backToMain(); return; }
    if (bootClick || up == K_SHORT || dn == K_SHORT) { goHome(UI_PORTAL); return; }
    if (bootHold) { pushToTalk(); return; }
    if (millis() - homeSince > PORTAL_SHOW_SEC * 1000UL) backToMain();
  } else if (st == UI_IDLE) {
    if (up == K_SHORT) changeVolume(+10);
    else if (dn == K_SHORT) changeVolume(-10);
    else if (up == K_LONG) {
      openPortalMenu();
      return;
    } else if (bootClick) { helpStep(+1); return; }
    else if (bootHold) { pushToTalk(); return; }
  }

  // ---- 持續聆聽：每次讀 20ms，存進預錄環狀緩衝 ----
  int rms = readFrame(preroll[prerollHead]);
  prerollHead = (prerollHead + 1) % PREROLL_FR;
  if (prerollCount < PREROLL_FR) prerollCount++;
  micLevel = rms;

  static int loudFrames = 0;
  int thr = vadThreshold();
  bool trigger = false;
  if (VOICE_ACTIVATION) {
    loudFrames = rms > thr ? loudFrames + 1 : 0;
    if (loudFrames >= 3) trigger = true;                          // 連續 60ms 大於門檻
    else if (rms < thr) noiseFloor += (rms - noiseFloor) * 0.01f;  // 慢慢追蹤背景噪音
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 5000) {
    lastLog = millis();
    Serial.printf("[聆聽] rms=%d 噪音=%.0f 門檻=%d\n", rms, noiseFloor, thr);
  }

  if (trigger) {
    loudFrames = 0;
    size_t n = recordUtterance(false);
    if (n && lastRecHitMax) {
      Serial.println("（一直沒有停頓，可能是音樂或噪音，不送出）");
      raiseNoiseFloor(lastRecAvgRms * 0.9f);
      n = 0;
    }
    if (n) handleUtterance(n);
    else Serial.println("（太短，視為雜音）");
    flushMic(300);
    if (uiState != UI_SONG) restoreHome();
  }
}
