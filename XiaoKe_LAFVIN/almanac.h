// ============================================================================
//  almanac.h — 農民曆（離線計算，不需上網）
//    年柱以農曆年為準；月柱以「節」交接（立春起寅月）；日柱由日期推算。
//    節氣日期用 21 世紀近似公式（少數年份可能差 1 天）。
//    宜忌採「建除十二神」簡易版，與市售農民曆的完整宜忌不一定相同，僅供參考。
// ============================================================================
#pragma once
#include <Arduino.h>
#include <time.h>
#include "lunar_table.h"

static const char *const GZ_STEM[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
static const char *const GZ_BRANCH[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
static const char *const ZODIAC[] = {"鼠", "牛", "虎", "兔", "龍", "蛇", "馬", "羊", "猴", "雞", "狗", "豬"};
static const char *const SOLAR_TERM[] = {"小寒", "大寒", "立春", "雨水", "驚蟄", "春分", "清明", "穀雨", "立夏", "小滿", "芒種", "夏至",
                                         "小暑", "大暑", "立秋", "處暑", "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"};
static const float SOLAR_TERM_C[] = {5.4055f, 20.12f, 3.87f, 18.73f, 5.63f, 20.646f, 4.81f, 20.1f, 5.52f, 21.04f, 5.678f, 21.37f,
                                     7.108f, 22.83f, 7.5f, 23.13f, 7.646f, 23.042f, 8.318f, 23.438f, 7.438f, 22.36f, 7.18f, 21.94f};
static const char *const JIANCHU[] = {"建", "除", "滿", "平", "定", "執", "破", "危", "成", "收", "開", "閉"};
static const char *const JIANCHU_YI[] = {
  "出行、上任、祈福", "掃舍、沐浴、求醫", "祭祀、祈福、開市", "修飾、塗泥、平治道路",
  "嫁娶、交易、納財、入學", "立約、捕捉、修補", "求醫、拆屋（大事勿用）", "安床、祭祀",
  "開市、嫁娶、入學、出行", "納財、收帳、捕捉", "開市、嫁娶、出行、求職", "築堤、補垣、安葬"};
static const char *const JIANCHU_JI[] = {
  "動土、開倉、嫁娶", "嫁娶、遠行", "栽種、下葬、上任", "開渠、栽種",
  "訴訟、出行", "開市、移徙、出行", "嫁娶、開市、簽約", "登高、行船、出行",
  "訴訟", "出行、安葬", "安葬、動土", "開市、出行、求醫"};
static const char *const SHA_DIR[] = {"南", "東", "北", "西", "南", "東", "北", "西", "南", "東", "北", "西"};   // 依日支：申子辰南、亥卯未西…

struct Almanac {
  bool ok = false;
  String yearGZ, zodiac, monthGZ, dayGZ;   // 丙午、馬、戊戌、丁亥
  String jianchu, yi, ji, chong, sha;      // 成、宜…、忌…、沖蛇（辛巳）、西
  String termToday;                        // 今天是節氣就填，例如「寒露」
  String nextTerm;                         // 下一個節氣與日期，例如「霜降 10/23」
};

// 距 2000-01-01 的天數（Howard Hinnant 演算法）
static long almDays(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468 - 10957;
}

// 第 k 個節氣（0 = 小寒）在 year 年的日期（月 = k/2 + 1）
static int solarTermDay(int year, int k) {
  int y = year % 100;
  int L = (k < 4) ? (y - 1) / 4 : y / 4;
  return (int)(y * 0.2422f + SOLAR_TERM_C[k]) - L;
}

static int mod(int a, int n) { return ((a % n) + n) % n; }

Almanac computeAlmanac(const struct tm &t) {
  Almanac a;
  int Y = t.tm_year + 1900, M = t.tm_mon + 1, D = t.tm_mday;
  long d = almDays(Y, M, D);

  // ---- 年柱 / 生肖：農曆年 ----
  int lunarYear = -1;
  for (int i = 0; i < LUNAR_YEARS - 1; i++)
    if (d >= LUNAR[i].cny && d < LUNAR[i + 1].cny) { lunarYear = LUNAR_FIRST_YEAR + i; break; }
  if (lunarYear < 0) return a;
  int ygz = mod(lunarYear - 4, 60);
  a.yearGZ = String(GZ_STEM[ygz % 10]) + GZ_BRANCH[ygz % 12];
  a.zodiac = ZODIAC[ygz % 12];

  // ---- 月柱：以「節」（每月第一個節氣）交接 ----
  int termIdx = M * 2 - 2;                                     // 本月的「節」
  int mBranch = mod(M, 12);                                    // 1 月 = 丑、2 月 = 寅…（節後）
  if (D < solarTermDay(Y, termIdx)) mBranch = mod(mBranch - 1, 12);
  int solarYear = Y - ((M < 2 || (M == 2 && D < solarTermDay(Y, 2))) ? 1 : 0);   // 立春換年
  int sStem = mod(solarYear - 4, 10);
  int firstStem = (sStem % 5) * 2 + 2;                         // 寅月天干：甲己丙、乙庚戊…
  int mStem = mod(firstStem + mod(mBranch - 2, 12), 10);
  a.monthGZ = String(GZ_STEM[mStem]) + GZ_BRANCH[mBranch];

  // ---- 日柱：2000-01-01 為戊午日（第 54）----
  int dgz = mod(54 + (int)(d % 60), 60);
  int dStem = dgz % 10, dBranch = dgz % 12;
  a.dayGZ = String(GZ_STEM[dStem]) + GZ_BRANCH[dBranch];

  // ---- 建除十二神、宜忌、沖煞 ----
  int jc = mod(dBranch - mBranch, 12);
  a.jianchu = JIANCHU[jc];
  a.yi = JIANCHU_YI[jc];
  a.ji = JIANCHU_JI[jc];
  int cb = (dBranch + 6) % 12, cs = (dStem + 4) % 10;
  a.chong = String("沖") + ZODIAC[cb] + "（" + GZ_STEM[cs] + GZ_BRANCH[cb] + "）";
  a.sha = SHA_DIR[dBranch];

  // ---- 節氣 ----
  for (int k = 0; k < 24; k++) {
    int tm = k / 2 + 1, td = solarTermDay(Y, k);
    if (tm == M && td == D) a.termToday = SOLAR_TERM[k];
  }
  for (int i = 0; i < 25; i++) {                               // 找今天之後最近的節氣
    int k = (M * 2 - 2 + i) % 24, yy = Y + (M * 2 - 2 + i) / 24;
    int tm = k / 2 + 1, td = solarTermDay(yy, k);
    if (almDays(yy, tm, td) > d) {
      char buf[24];
      snprintf(buf, sizeof(buf), " %d/%d", tm, td);
      a.nextTerm = String(SOLAR_TERM[k]) + buf;
      break;
    }
  }
  a.ok = true;
  return a;
}

// 給 AI 參考的一行文字
String almanacText(const struct tm &t) {
  Almanac a = computeAlmanac(t);
  if (!a.ok) return "";
  String s = "農民曆：" + a.yearGZ + "年（" + a.zodiac + "年）、" + a.monthGZ + "月、" + a.dayGZ + "日，" +
             a.jianchu + "日，宜" + a.yi + "，忌" + a.ji + "，" + a.chong + "，煞" + a.sha + "。";
  if (a.termToday.length()) s += "今天是" + a.termToday + "。";
  s += "下一個節氣：" + a.nextTerm + "。";
  return s;
}
