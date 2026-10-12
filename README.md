# esp32s3-lafvin-ai-chatbot
ESP32 AI 桌上型機器人 — LAFVIN ESP32S3 AIChatBot「小柯」
<img width="1186" height="930" alt="image" src="https://github.com/user-attachments/assets/533c252c-d5bb-40f1-ad9d-3bae761e3a28" />
https://github.com/user-attachments/assets/9bedfdc5-0a82-4b4e-84f8-4c37b05b11ce
把 LAFVIN ESP32S3 AIChatBot 原廠的「小智」韌體換成 **小柯 OpenAI 版**：直接說話 → OpenAI 語音轉文字 → GPT 回答 → OpenAI TTS 從喇叭講出來。2.0 吋螢幕顯示可愛表情、日期時間、農曆與農民曆；會查班表、工作流程、病房助理工作站，還會唱歌（雲端硬碟 MP3 直接播放）。

---

## 一、系統安裝

### 1. 硬體

| 元件 | 說明 |
|---|---|
| 主控 | ESP32-S3 N16R8（16MB Flash、8MB OPI PSRAM）|
| 擴充板 | LAFVIN AI Chatbot Shield（UP / DOWN 兩個按鍵）|
| 螢幕 | 2.0" ST7789 320×240（橫向）|
| 音訊模組 | ES8311（喇叭）+ ES7210（麥克風）+ 功放，架構同立創開發板 |

腳位寫在 `XiaoKe_LAFVIN/lafvin_hw.h`，是擴充板固定的接線，不需要修改：

| 元件 | 腳位 |
|---|---|
| ST7789 | SCK=41、MOSI=40、CS=47、DC=39、BLK=42、RES 接開發板 RST |
| 音訊 I2C | SDA=1、SCL=2（ES8311 = 0x18、ES7210 = 0x41）|
| 音訊 I2S | MCLK=38、BCLK=14、WS=13、喇叭（ESP 輸出）=45、麥克風（ESP 輸入）=12 |
| 功放 | PA_EN=48 |
| 按鍵 | BOOT=GPIO0、UP=GPIO20、DOWN=GPIO19 |

> ⚠ LAFVIN 文件寫的 `DOUT-45`、`DIN-12` 是以 **ESP32** 的角度命名（DOUT = ESP 送出給喇叭），不要接反。
>
> ⚠ UP / DOWN 鍵（GPIO19/20）就是開發板**原生 USB** 孔的 D-/D+。插原生 USB 孔時程式會自動停用這兩個鍵（否則 USB 會斷線）；要使用按鍵請把線插到 **UART（CP210x）** 孔。

### 2. 開發環境

- Arduino IDE 2.x
- 開發板：**esp32 by Espressif 3.x**（測試版本 3.3.12，內建 Helix MP3 解碼器）
- 函式庫（程式庫管理員安裝）：**Adafruit GFX**、**Adafruit ST7735 and ST7789**、**ArduinoJson 7.x**

### 3. 設定 `config.h`

1. 把 `XiaoKe_LAFVIN/config.example.h` 複製成 `XiaoKe_LAFVIN/config.h`
2. 填入 Wi-Fi 名稱、密碼（**只支援 2.4GHz**）與 OpenAI API Key（https://platform.openai.com/api-keys）
3. 要直接唱 MP3：把 Google 雲端硬碟資料夾（設成「知道連結的任何人都能檢視」）網址填到 `SONGS_MP3_FOLDER`

也可以先不填：開機連不上 Wi-Fi 時，機器人會開熱點 `XiaoKe-Setup`（密碼 `xiaoke123`），用手機連上後開 `http://192.168.4.1`，選擇 Wi-Fi 並輸入 API Key。

### 4. 編譯與上傳

用 Arduino IDE 開啟 `XiaoKe_LAFVIN/XiaoKe_LAFVIN.ino`，工具選單設定：

| 工具選單 | 設定 |
|---|---|
| 開發板 | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** |
| USB Mode | Hardware CDC and JTAG |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | **OPI PSRAM** |

首次編譯約需 10 分鐘（繁中字型很大）。若板子跑起來後原生 USB 連不上，按住 **BOOT** 再按一下 **RST** 進入下載模式後上傳。

---

## 二、使用說明

### 語音

| 說 | 功能 |
|---|---|
| 直接說話 | 聊天（停頓一下自動送出）；天氣、新聞、股價會自己上網查 |
| 「我明天上什麼班」「這個月休幾天」 | 查病房助理 / 護理師月班表（Firebase）|
| 「白班幾點交班」「現在該做什麼」 | 查工作流程時間軸（Firebase）|
| 「工作站」「長照」 | 開啟病房助理工作站選單；稽核、督導手冊、學科、術科、長照組合可以直接問 |
| 「唱首歌」「唱千江有水千江月」 | 播放雲端硬碟 MP3（同時顯示 YouTube 封面與 QR 碼）|
| 「大聲一點」「小聲一點」 | 調音量 |
| 「今天宜什麼」「今天農曆幾號」 | 農民曆 |
| 「清除記憶」 | 忘掉剛剛的對話 |

### 按鍵

| 畫面 | BOOT | UP | DOWN |
|---|---|---|---|
| 主畫面 | 短按：功能說明；按住：說話 | 音量 +10%；長按：開啟工作站 | 音量 -10% |
| 功能說明 | 下一頁 | 上一頁 | 下一頁 |
| 工作站選單 | 看 QR 碼；按住：說話 | 上一個；長按：回主畫面 | 下一個 |
| 唱歌中 | 停止 | 音量 + | 音量 - |

### 設定網頁

螢幕左下角顯示的網址（例如 `http://192.168.6.19` 或 `http://xiaoke.local`）可修改 Wi-Fi、API Key 與 YouTube 歌單。

---

## 三、創意開發者

此系統是吳玉柱先生與 Claude AI 共同開發，有任何意見請聯絡：achir1015@gmail.com
