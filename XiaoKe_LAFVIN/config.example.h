// ============================================================================
//  config.example.h — 設定範本（複製成 config.h 再填入自己的值；沒有 config.h 時自動使用本檔）
//  ⚠ config.h 含 API Key，請勿上傳到 GitHub 或分享給他人
//  Wi-Fi 與 API Key 也可以開機後用手機開啟螢幕左下角的網址修改
// ============================================================================
#pragma once

// ---------------------------------------------------------------- Wi-Fi ----
#define WIFI_SSID       "你的WiFi名稱"
#define WIFI_PASSWORD   "你的WiFi密碼"

// --------------------------------------------------------------- OpenAI ----
#define OPENAI_API_KEY  "sk-請填入你的OpenAI金鑰"

#define STT_MODEL       "gpt-4o-mini-transcribe"   // 或 "whisper-1"
#define CHAT_MODEL      "gpt-4o-mini"
#define TTS_MODEL       "gpt-4o-mini-tts"          // 或 "tts-1"
#define TTS_VOICE       "nova"                     // alloy / ash / coral / echo / fable / nova / onyx / sage / shimmer
#define TTS_INSTRUCTIONS "用可愛、活潑、溫暖的台灣口音中文說話，語氣像貼心的小朋友，語速稍快。"

#define ROBOT_NAME      "小柯"
#define MAX_HISTORY_MSGS 10        // 記住最近幾則對話（user+assistant 各算 1 則）
#define CHAT_MAX_TOKENS  300

// ------------------------------------------------------------ 上網搜尋 ----
// 1 = 問天氣、新聞、股價等即時資訊時，AI 會自己決定上網搜尋再回答（OpenAI web_search，搜尋會另外計費）
#define ENABLE_WEB_SEARCH 1
#define SEARCH_CITY       "Taipei"   // 搜尋預設地區（英文城市名）
#define SEARCH_CITY_ZH    "台北"     // 沒講地點時以這裡為準

// ------------------------------------------------------- 班表查詢（Google Firebase Firestore）----
// 問「我明天上什麼班」「這個月休幾天」時，讀取 Firestore 文件 schedules/<類別>-<年>-<月> 交給 AI 回答
#define FIREBASE_PROJECT  "monthlyschedule-a5dea"
#define FIREBASE_API_KEY  ""          // Firestore 規則需要金鑰時才填
#define SCHEDULE_CATEGORIES { "assistant", "nurse" }
#define SCHEDULE_LABELS     { "病房助理", "護理師" }
#define SCHEDULE_MY_NAME  "吳玉柱"    // 使用者說「我」指的是誰
#define SCHEDULE_LEGEND   "（與月班表系統相同規則）代號含 D 或「白值」= 白班（如 D1、DD、D/F、D/W、D/洗、D/學）；含 E = 小夜班；含 N 或「夜8」= 大夜班；含 OFF、國、休 = 休假；例 = 例假；其他代號（如傷照）照原代號說"

// ------------------------------------------------- 工作流程查詢（Firebase：線上專案進度時間軸表）----
#define TIMELINE_PROJECT  "project-timeline-d2188"
#define TIMELINE_DESC_MAX 150         // 每個任務說明最多保留幾個字

// ------------------------------------------------------------ 病房助理工作站 ----
// 主畫面長按 UP 鍵或說「工作站」開啟選單；工具清單開機時從工作站網頁讀取
#define PORTAL_NAME       "病房助理工作站"
#define PORTAL_URL        "https://achir1015.github.io/ward-assistant-portal/"
#define PORTAL_SHOW_SEC   120        // 選單 / QR 碼畫面多久沒操作自動回主畫面

// ------------------------------------------------------------ 唱歌（YouTube 歌單）----
// 說「唱首歌」會從這個播放清單隨機選一首，顯示封面、歌名與 QR 碼（手機掃描在 YouTube 播放）
#define YT_PLAYLIST_URL   "https://www.youtube.com/playlist?list=PLMiXt5EIkXI0"
#define SONG_COMPOSER     "吳玉柱"   // 詞曲創作者（版權所有）
#define SONG_SHOW_SEC     180        // 唱歌畫面停留秒數（按鍵可提早回到聊天）
// 雲端硬碟 MP3 資料夾（自己創作的歌，小柯會直接唱出來）：資料夾設成「知道連結的任何人都能檢視」；留空 = 只顯示 QR 碼
// MP3 建議：單聲道、24kHz、80kbps（ffmpeg -ac 1 -ar 24000 -b:a 80k），檔名 = 歌名.mp3（與 YouTube 歌名相同就會顯示封面與 QR 碼）
#define SONGS_MP3_FOLDER  ""

// ------------------------------------------------------------ 聲控（免按鍵）----
#define HW_TEST_MODE      0        // 1 = 開機只跑硬體測試（螢幕/喇叭/麥克風/按鍵），先確認硬體用
#define VOICE_ACTIVATION  1        // 1 = 直接說話就會開始聆聽；0 = 只用按鍵（按住 BOOT 說話）
#define VAD_MIN_RMS       250      // 觸發錄音的最低音量（環境吵、常誤觸就調高；喊很大聲才有反應就調低）
#define VAD_RATIO         3.0      // 音量需高於背景噪音幾倍才觸發
#define VAD_SILENCE_MS    900      // 停頓多久視為說完
#define VAD_MIN_SPEECH_MS 400      // 少於這個長度的聲音視為雜音
#define SLEEPY_AFTER_SEC  90       // 多久沒互動表情變想睡

// ------------------------------------------------------------------ 音訊 ----
#define MAX_RECORD_SEC   12
#define DEFAULT_VOLUME   60        // 0~100
#define MIC_PGA_GAIN     8         // 麥克風類比增益（ES8311 0~10、ES7210 0~14，x3dB）；聲音太小調高、破音調低
#define MIC_SOFT_GAIN    2         // 數位增益倍數（錄音太小聲調大）

// ------------------------------------------------------------------ 螢幕 ----
#define TFT_ROTATION     1         // 1 或 3 = 橫向 320x240；畫面上下左右顛倒就改另一個
#define TFT_INVERT       1         // IPS 面板要反相；顏色黑白顛倒（黑底變白底）就改 0
#define TFT_BGR          0         // 紅色和藍色對調就改 1
#define LCD_BRIGHTNESS   100       // 背光亮度 10~100
