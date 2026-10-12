// ============================================================================
//  lafvin_hw.h — LAFVIN ESP32S3 AIChatBot 擴充板硬體
//    腳位取自 LAFVIN 官方教學（2.hardware_connect）
//    音訊模組：ES8311（喇叭 DAC，也有一個 ADC）；模組上若另有 ES7210 麥克風 ADC 會自動改用它
//    錄音與播放共用一組全雙工 I2S：24kHz（OpenAI TTS pcm 固定 24kHz），錄音再降頻成 16kHz 上傳
// ============================================================================
#pragma once
#include <Wire.h>

// ---- 腳位（擴充板固定接線，不需要修改）----
#define LF_I2C_SDA      1
#define LF_I2C_SCL      2
#define LF_I2S_MCLK     38
#define LF_I2S_BCLK     14
#define LF_I2S_WS       13
#define LF_I2S_DOUT     45         // ESP32 → 喇叭（ES8311）；腳位名稱以 ESP32 為準，同立創開發板
#define LF_I2S_DIN      12         // 麥克風（ES7210）→ ESP32
#define LF_PA_EN        48         // 功放開關，HIGH = 開
#define LF_LCD_SCK      41
#define LF_LCD_MOSI     40
#define LF_LCD_CS       47
#define LF_LCD_DC       39
#define LF_LCD_RST      -1         // RES 接開發板 RST
#define LF_LCD_BL       42
#define PIN_BUTTON      0          // 開發板 BOOT 鍵（按下為 LOW）
#define PIN_KEY_UP      20         // 擴充板 UP 鍵（GPIO20 = USB D+，請用 UART 那個 Type-C 孔供電）
#define PIN_KEY_DOWN    19         // 擴充板 DOWN 鍵（GPIO19 = USB D-）

// 雲端 MP3 歌曲清單項目
struct Mp3Song { String id, title; };

// 按鍵狀態（主程式 pollKey 使用）
struct Key { int pin; bool down; uint32_t at; bool longDone; };
enum KeyEv { K_NONE, K_SHORT, K_LONG };

#define ES8311_ADDR    0x18
uint8_t ES7210_ADDR = 0x40;     // 立創板 / LAFVIN 為 0x41（AD0=1），開機自動尋找 0x40~0x43
#define I2S_RATE       24000

static bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static int i2cRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}
static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// 開機時列出 I2C 上所有裝置（除錯用）
static void i2cScan() {
  Serial.print("I2C 裝置：");
  for (uint8_t a = 1; a < 127; a++) if (i2cPresent(a)) Serial.printf("0x%02X ", a);
  Serial.println();
}

// ---------------------------------------------------------------- ES8311 ----
// 從屬模式，MCLK = 256fs（24kHz → 6.144MHz），I2S 16-bit；DAC 播放 + ADC 類比麥克風
bool es8311Init(uint8_t pgaGain) {
  if (!i2cPresent(ES8311_ADDR)) { Serial.println("ES8311 位址 0x18 沒有回應"); return false; }
  uint8_t pga = 0x10 | (pgaGain > 10 ? 10 : pgaGain);   // bit4 = MIC1P/MIC1N 差動輸入
  const uint8_t seq[][2] = {
    {0x44, 0x08}, {0x44, 0x08},              // 加強 I2C 抗雜訊
    {0x00, 0x1F}, {0x00, 0x00},              // 重置
    {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
    {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
    {0x00, 0x80},                            // 從屬模式、開機
    {0x01, 0x3F},                            // 時脈取自 MCLK 腳，全部開啟
    {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF}, // BCLK / LRCK 分頻（從屬模式不使用）
    {0x09, 0x0C}, {0x0A, 0x0C},              // DAC / ADC 格式：I2S 16-bit
    {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A}, // ADC 高通濾波（去直流）
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00},
    {0x14, pga},                             // 麥克風類比增益
    {0x15, 0x40},
    {0x17, 0xBF},                            // ADC 音量 0dB（預設 0x00 = 幾乎靜音）
    {0x37, 0x08}, {0x45, 0x00},
    {0x32, 0xBF},                            // DAC 音量 0dB（實際音量用軟體調）
    {0x31, 0x00},                            // 取消靜音
  };
  bool ok = true;
  for (auto &r : seq) {
    bool w = false;
    for (int k = 0; k < 3 && !w; k++) { w = i2cWrite(ES8311_ADDR, r[0], r[1]); if (!w) delay(5); }
    if (!w) { Serial.printf("ES8311 寫入失敗 reg 0x%02X\n", r[0]); ok = false; }
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  Serial.printf("ES8311 ID=%02X%02X\n", i2cRead(ES8311_ADDR, 0xFD), i2cRead(ES8311_ADDR, 0xFE));
  return ok;
}

// ---------------------------------------------------------------- ES7210 ----
// 從屬模式，MCLK = 256fs，I2S 16-bit 立體聲：左 = MIC1、右 = MIC2（設定沿用 BOX-3 實測值）
bool es7210Init(uint8_t pgaGain) {
  bool found = false;
  for (uint8_t a = 0x40; a <= 0x43 && !found; a++) if (i2cPresent(a)) { ES7210_ADDR = a; found = true; }
  if (!found) { Serial.println("找不到 ES7210（0x40~0x43）"); return false; }
  Serial.printf("ES7210 位址 0x%02X\n", ES7210_ADDR);
  uint8_t g = 0x10 | (pgaGain > 14 ? 14 : pgaGain);
  const uint8_t seq[][2] = {
    {0x00, 0xFF}, {0x00, 0x41},              // 重置
    {0x01, 0x3F},                            // 先關時脈
    {0x09, 0x30}, {0x0A, 0x30},              // 開機時序
    {0x23, 0x2A}, {0x22, 0x0A}, {0x20, 0x0A}, {0x21, 0x2A},  // 高通濾波
    {0x08, 0x10},                            // 從屬模式（bit4 必須設，否則每 8 個樣本只輸出 1 個）
    {0x40, 0x43},                            // VMID
    {0x41, 0x70}, {0x42, 0x70},              // 麥克風偏壓 2.87V
    {0x07, 0x20},                            // OSR
    {0x02, 0xC1},                            // 主時脈分頻：256fs
    {0x04, 0x01}, {0x05, 0x00},              // LRCK = MCLK / 256
    {0x11, 0x60},                            // I2S 16-bit
    {0x12, 0x00},                            // 不使用 TDM
    {0x43, g}, {0x44, g}, {0x45, 0x00}, {0x46, 0x00},        // MIC1/2 增益，MIC3/4 關閉
    {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
    {0x4B, 0x00},                            // MIC1/2 上電
    {0x4C, 0xFF},                            // MIC3/4 斷電
    {0x01, 0x34},                            // 開啟 ADC1/2 時脈
    {0x06, 0x00},                            // 上電
    {0x00, 0x71}, {0x00, 0x41},              // 啟動
  };
  bool ok = true;
  for (auto &r : seq) ok &= i2cWrite(ES7210_ADDR, r[0], r[1]);
  return ok;
}
