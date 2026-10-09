/*
  Pip - voice AI bot with a cute OLED face
  ESP32 DevKitC + INMP441 mic + MAX98357A amp + speaker + SSD1306 OLED

  Flow: you talk -> ESP32 records (voice activity detection) and streams the audio
        to the Python server on your laptop -> speech-to-text -> LLM -> text-to-speech
        -> raw audio comes back -> ESP32 plays it on the speaker and shows the emotion.

  Libraries (Library Manager): Adafruit SSD1306, Adafruit GFX Library
  Board package: "esp32 by Espressif Systems" 3.x  (needed for ESP_I2S.h)
  Board: ESP32 Dev Module

  Wiring (matches the Cirkit diagram)
    INMP441   VDD->3V3  GND->GND  L/R->GND  SCK->GPIO14  WS->GPIO25  SD->GPIO33
    MAX98357A VIN->5V   GND->GND  BCLK->GPIO14  LRC->GPIO25  DIN->GPIO22  (speaker on + / -)
    OLED      VDD->3V3  GND->GND  SCK(SCL)->GPIO19  SDA->GPIO21

  ONE FILE does everything. Pick what it does with RUN_MODE below:
    0 = full voice bot   1 = face test only   2 = microphone test   3 = speaker test   4 = DIAGNOSTIC (finds what is broken)
  In mode 0 the board also reports to the live dashboard: http://YOUR-MAC-IP:8000/dashboard

  Serial Monitor (115200) commands:
    n / p = next / previous emotion   h = happy   l = listening   t = thinking
    k = toggle "talking mouth" demo   m = mute / unmute the microphone   1..4 = switch mode (Buddy, Study, Mood, Sleep)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP_I2S.h>
#include <esp_system.h>

// ================= CONFIG: edit these =================
const char* WIFI_SSID   = "YOUR_WIFI_NAME";
const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";
const char* SERVER_HOST = "192.168.1.50";   // your laptop's IP, where server.py runs
const int   SERVER_PORT = 8000;
#define RUN_MODE 0           // 0 = full voice bot, 1 = face test, 2 = microphone test, 3 = speaker test, 4 = DIAGNOSTIC report
#define LOG_LEVEL 3          // 0 = silent, 1 = errors only, 2 = normal, 3 = detailed. Logs go to the Serial Monitor AND the dashboard.
#define PIN_LED 2            // status LED (the small blue LED on most ESP32 boards). -1 = no LED. Patterns are listed below.
#define TRIGGER_MODE 2       // how a recording starts: 0 = by voice only, 1 = by the BOOT button only (hold it while you talk), 2 = both. Key 'r' in the Serial Monitor also records 4 s
#define VOICE_THRESHOLD 0    // 0 = automatic. Or a fixed level (try 4000): the voice must be louder than this to start a recording. Keys '>' and '<' change it live
#define DEVICE_SPEAKER 0     // 0 = the voice plays on the LAPTOP; the board does not drive the amplifier at all (cleanest microphone). 1 = play on the speaker wired to the board
#define SPEAKER_VOLUME 200   // percent: 100 = as received, 200 = twice as loud, 300 = three times (a soft limiter avoids harsh clipping)
#define MIC_SHIFT 15         // starting mic gain: lower = louder. 15 = normal (tested: speech peaks at 50-70%), 14 = 2x louder, 13 = 4x (can clip). It adjusts itself if the mic clips or is too quiet
// ======================================================

// ---------- pins (from your Cirkit circuit) ----------
#define PIN_I2S_BCLK 14
#define PIN_I2S_WS   25
#define PIN_MIC_SD   33   // INMP441 SD  -> ESP32 (data in)
#define PIN_AMP_DIN  22   // ESP32 -> MAX98357A DIN (data out)
#define OLED_SDA 21
#define OLED_SCL 19
#define OLED_ADDR 0x3C
#define PIN_BTN 0         // the BOOT button on the board (press = low)

#define SAMPLE_RATE 16000
#define FRAME 320                 // 20 ms of audio
#define PRE_FRAMES 4              // 80 ms of audio kept from before speech was detected
#define MAX_FRAMES 300            // max 6 s per utterance
#define SILENCE_FRAMES 36         // ~720 ms of quiet ends the utterance (quiet = well below your loudest moment)

Adafruit_SSD1306 display(128, 64, &Wire, -1);
I2SClass i2s;

// ---- types (kept above every function so the Arduino auto-prototypes can see them) ----
enum LedMode { LED_BOOT, LED_IDLE, LED_REC, LED_THINK, LED_SPEAK, LED_ERROR, LED_MUTED };
enum Emotion { HAPPY, EXCITED, LOVE, WINK, JOY, SILLY, SLEEPY, SAD, CRY, POUT,
               SURPRISED, THINKING, LISTENING, SHY, STARRY, CURIOUS, EMO_COUNT };
enum Mouth { M_SMILE, M_CAT, M_D, M_FROWN, M_O, M_SMALLO, M_FLAT, M_WAVY, M_TONGUE, M_WAIL, M_PUFF };
struct Chan { long n = 0, zeros = 0; int32_t mn = INT32_MAX, mx = INT32_MIN; double sum = 0, sq = 0; };

// ================= logging =================
// Every module writes tagged lines like:  [  12345] INFO MIC   calibrated: ...
//   tags: BOOT OLED WIFI NET I2S MIC VAD PLAY CMD LIVE DIAG.  Lines go to the Serial Monitor and, when the
//   server is reachable, to the dashboard log panel and the server terminal too.
String logBuf;   // log lines waiting to be sent to the dashboard
void logLine(const char* lvl, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void logLine(const char* lvl, const char* tag, const char* fmt, ...) {
  char msg[220];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
  char line[280];
  snprintf(line, sizeof(line), "[%7lu] %-4s %-5s %s", millis(), lvl, tag, msg);
  Serial.println(line);
  logBuf += line; logBuf += "\n";
  if (logBuf.length() > 1800) logBuf = logBuf.substring(logBuf.length() - 1200);   // keep the newest lines
}
#define LOGE(tag, ...) do { if (LOG_LEVEL >= 1) logLine("ERR", tag, __VA_ARGS__); } while (0)
#define LOGI(tag, ...) do { if (LOG_LEVEL >= 2) logLine("INFO", tag, __VA_ARGS__); } while (0)
#define LOGD(tag, ...) do { if (LOG_LEVEL >= 3) logLine("DBG", tag, __VA_ARGS__); } while (0)

// ================= status LED =================
//   fast blinking ........ starting up / joining WiFi
//   short blip every 2 s .. ready and listening
//   solid on ............... hearing you (recording)
//   slow blinking .......... thinking (waiting for the server)
//   very fast blinking ..... speaking
//   three blinks, pause .... something is wrong (read the log)
//   tiny blip every 3 s .... muted
LedMode ledMode = LED_BOOT;
void ledSet(LedMode m) { ledMode = m; }
void ledUpdate() {
  if (PIN_LED < 0) return;
  unsigned long t = millis();
  bool on = false;
  switch (ledMode) {
    case LED_BOOT:  on = (t / 120) % 2; break;
    case LED_IDLE:  on = (t % 2000) < 80; break;
    case LED_REC:   on = true; break;
    case LED_THINK: on = (t / 300) % 2; break;
    case LED_SPEAK: on = (t / 70) % 2; break;
    case LED_ERROR: { unsigned long m = t % 1800; on = (m < 150) || (m > 300 && m < 450) || (m > 600 && m < 750); break; }
    case LED_MUTED: on = (t % 3000) < 40; break;
  }
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}
const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "CRASH (panic)";
    case ESP_RST_INT_WDT:   return "watchdog (interrupt)";
    case ESP_RST_TASK_WDT:  return "watchdog (task stuck)";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "BROWN-OUT (power dipped: weak USB cable or port, or the speaker draws too much)";
    case ESP_RST_EXT:       return "reset button";
    default:                return "other";
  }
}

// ---------- emotions ----------
const char* EMO_NAMES[EMO_COUNT] = {"happy", "excited", "love", "wink", "joy", "silly", "sleepy", "sad",
                                    "cry", "pout", "surprised", "thinking", "listening", "shy", "starry", "curious"};

Emotion emo = HAPPY;
bool talking = false;
float talkAmp = 0;            // 0..1, drives the mouth while the bot speaks
bool blinking = false;
unsigned long nextBlink = 0, blinkEnd = 0, nextLook = 0, holdUntil = 0, lastActivity = 0;
int lookX = 0, lookY = 0, yo = 0;   // yo = vertical offset used for the gentle "breathing" bob
bool mouthDemo = false;

// ================= drawing helpers =================
#define W SSD1306_WHITE
#define K SSD1306_BLACK
void fc(int x, int y, int r, uint16_t c = W) { display.fillCircle(x, y + yo, r, c); }
void dc(int x, int y, int r) { display.drawCircle(x, y + yo, r, W); }
void rr(int x, int y, int w, int h, int r, uint16_t c = W) { display.fillRoundRect(x, y + yo, w, h, r, c); }
void ln(int x0, int y0, int x1, int y1, int t = 2) {
  for (int i = 0; i < t; i++) display.drawLine(x0, y0 + yo + i, x1, y1 + yo + i, W);
}
void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c = W) {
  display.fillTriangle(x0, y0 + yo, x1, y1 + yo, x2, y2 + yo, c);
}
// arc: angle 0 = right, 90 = up, 180 = left, 270 = down. 200..340 gives a "u", 20..160 gives a "^"
void arc(int cx, int cy, int r, int a0, int a1, int t) {
  for (int a = a0; a <= a1; a += 3) {
    float rad = a * DEG_TO_RAD;
    int x = cx + (int)(r * cos(rad)), y = cy - (int)(r * sin(rad));
    if (t <= 1) display.drawPixel(x, y + yo, W); else fc(x, y, t / 2);
  }
}

bool doBlink = false;

// glossy kawaii eye: solid white eye with two dark sparkle highlights that follow where it looks
void gl(int cx, int cy, int r, int ir, int px, int py) {
  if (doBlink) { rr(cx - r + 2, cy, 2 * r - 4, 3, 1); return; }
  fc(cx, cy, r);
  if (ir < 8) { fc(cx + px, cy + py, ir, K); fc(cx + px - 2, cy + py - 2, 2); return; }   // wide-eyed pupil
  fc(cx + px / 2 - r / 3, cy + py / 2 - r / 3, max(2, ir / 2 - 1), K);   // big highlight
  fc(cx + px / 2 + r / 3, cy + py / 2 + r / 3, 1, K);                    // small highlight
}

void heart(int cx, int cy, int s) {
  fc(cx - s, cy - s / 2, s + 1); fc(cx + s, cy - s / 2, s + 1);
  tri(cx - 2 * s - 1, cy - s / 4, cx + 2 * s + 1, cy - s / 4, cx, cy + 2 * s + 1);
}

void star(int cx, int cy, int R, int r) {
  int xs[8], ys[8];
  for (int k = 0; k < 8; k++) {
    float a = (90 + 45 * k) * DEG_TO_RAD;
    int rad = (k % 2 == 0) ? R : r;
    xs[k] = cx + (int)(rad * cos(a)); ys[k] = cy - (int)(rad * sin(a));
  }
  for (int k = 0; k < 8; k++) tri(cx, cy, xs[k], ys[k], xs[(k + 1) % 8], ys[(k + 1) % 8]);
}

void sparkle(int x, int y, int s) {
  ln(x - s, y, x + s, y, 1);
  display.drawFastVLine(x, y - s + yo, 2 * s + 1, W);
}

// dithered rosy cheeks. level 1 = light, 2 = heavy
void blush(int lvl) {
  if (!lvl) return;
  int cxs[2] = {15, 113};
  for (int c = 0; c < 2; c++)
    for (int dy = -5; dy <= 5; dy++)
      for (int dx = -9; dx <= 9; dx++)
        if (dx * dx * 25 + dy * dy * 81 <= 2025) {
          bool on = (lvl == 1) ? (((dx + dy) & 1) == 0) : ((((dx + dy) & 1) == 0) || ((dy & 1) == 0));
          if (on) display.drawPixel(cxs[c] + dx, 43 + dy + yo, W);
        }
}

void mouth(int m) {
  switch (m) {
    case M_SMILE:  arc(64, 40, 9, 225, 315, 2); break;
    case M_CAT:    arc(59, 45, 4, 180, 360, 2); arc(69, 45, 4, 180, 360, 2); break;
    case M_D:      fc(64, 44, 9); fc(64, 44, 7, K); rr(52, 34, 25, 10, 0, K); fc(64, 50, 3); break;
    case M_FROWN:  arc(64, 57, 8, 45, 135, 2); break;
    case M_O:      dc(64, 50, 5); dc(64, 50, 4); break;
    case M_SMALLO: fc(64, 50, 3); break;
    case M_FLAT:   ln(58, 50, 70, 50, 2); break;
    case M_WAVY:   for (int i = 0; i < 4; i++) ln(55 + i * 5, (i % 2) ? 48 : 51, 60 + i * 5, (i % 2) ? 51 : 48, 2); break;
    case M_TONGUE: arc(64, 40, 9, 225, 315, 2); fc(68, 51, 4); display.drawFastVLine(68, 48 + yo, 5, K); break;
    case M_WAIL:   rr(56, 44, 16, 10, 4); rr(58, 46, 12, 6, 2, K); break;
    case M_PUFF:   arc(64, 56, 5, 50, 130, 2); break;
  }
}

void drawTalkingMouth() {
  int h = 3 + (int)(talkAmp * 12);
  rr(54, 44, 20, h, min(h / 2, 5));
  if (h > 7) { rr(56, 46, 16, h - 4, 3, K); fc(64, 44 + h - 4, 3); }
}

void drawFace() {
  display.clearDisplay();
  yo = ((millis() / 700) % 2) ? 1 : 0;
  const int lx = 36, rx = 92, ey = 24;
  int px = lookX, py = lookY;
  bool tw = (millis() / 300) % 2;
  bool closedType = (emo == JOY || emo == SLEEPY || emo == CRY || emo == SILLY || emo == LOVE || emo == STARRY);
  doBlink = blinking && !closedType;
  int m = M_SMILE, bl = 0;

  switch (emo) {
    case HAPPY:
      gl(lx, ey, 13, 10, px, py); gl(rx, ey, 13, 10, px, py); m = M_CAT; bl = 1; break;
    case EXCITED:
      gl(lx, ey, 14, 10, px, py); gl(rx, ey, 14, 10, px, py); m = M_D; bl = 1;
      if (tw) { sparkle(6, 8, 3); sparkle(122, 8, 3); } else { sparkle(10, 4, 2); sparkle(118, 4, 2); }
      break;
    case LOVE:
      heart(lx, ey, tw ? 6 : 5); heart(rx, ey, tw ? 6 : 5);
      fc(lx - 5, ey - 5, 1, K); fc(rx - 5, ey - 5, 1, K);
      m = M_CAT; bl = 2;
      if (tw) { heart(114, 8, 2); }
      break;
    case WINK:
      gl(lx, ey, 13, 10, px, py); arc(rx, ey + 4, 10, 30, 150, 3); m = M_SMILE; bl = 1; break;
    case JOY:
      arc(lx, ey + 5, 10, 25, 155, 3); arc(rx, ey + 5, 10, 25, 155, 3); m = M_D; bl = 2; break;
    case SILLY:
      ln(lx - 8, ey - 8, lx + 6, ey, 3); ln(lx + 6, ey, lx - 8, ey + 8, 3);
      ln(rx + 8, ey - 8, rx - 6, ey, 3); ln(rx - 6, ey, rx + 8, ey + 8, 3);
      m = M_TONGUE; bl = 1; break;
    case SLEEPY:
      arc(lx, ey - 2, 10, 200, 340, 3); arc(rx, ey - 2, 10, 200, 340, 3);
      display.setTextSize(1); display.setTextColor(W);
      display.setCursor(104, 4 + yo); display.print("z");
      display.setCursor(112, 12 + yo); display.print("Z");
      if (tw) fc(64, 50, 3); else fc(64, 50, 2);
      m = -1; bl = 1; break;
    case SAD: {
      gl(lx, ey + 1, 13, 11, px, -3); gl(rx, ey + 1, 13, 11, px, -3);
      ln(lx - 12, ey - 13, lx + 8, ey - 18, 2); ln(rx - 8, ey - 18, rx + 12, ey - 13, 2);
      int ty = (millis() / 200) % 7;
      fc(lx + 10, ey + 18 + ty, 2); tri(lx + 8, ey + 17 + ty, lx + 12, ey + 17 + ty, lx + 10, ey + 11 + ty);
      m = M_FROWN; break; }
    case CRY: {
      arc(lx, ey, 10, 200, 340, 3); arc(rx, ey, 10, 200, 340, 3);
      int h = 8 + (millis() / 100) % 14;
      rr(lx - 9, ey + 6, 3, h, 1); rr(rx + 6, ey + 6, 3, h, 1);
      m = M_WAIL; bl = 1; break; }
    case POUT:
      gl(lx, ey, 13, 10, px, py); gl(rx, ey, 13, 10, px, py);
      tri(lx - 14, ey - 15, lx + 14, ey - 15, lx + 14, ey - 2, K);
      tri(rx - 14, ey - 15, rx + 14, ey - 15, rx - 14, ey - 2, K);
      dc(12, 44, 7); dc(116, 44, 7);
      ln(108, 4, 116, 12, 2); ln(116, 4, 108, 12, 2);
      m = M_PUFF; break;
    case SURPRISED:
      gl(lx, ey, 15, 6, px, py); gl(rx, ey, 15, 6, px, py);
      arc(lx, ey - 16, 8, 60, 120, 2); arc(rx, ey - 16, 8, 60, 120, 2);
      m = M_O; break;
    case THINKING: {
      gl(lx, ey, 13, 9, 5, -4); gl(rx, ey, 13, 9, 5, -4);
      ln(lx - 10, ey - 17, lx + 8, ey - 17, 2); ln(rx - 10, ey - 17, rx + 8, ey - 21, 2);
      int dots = (millis() / 400) % 4;
      for (int i = 0; i < dots; i++) fc(104 + i * 8, 54, 2);
      m = M_WAVY; break; }
    case LISTENING: {
      gl(lx, ey, 13, 10, px, py); gl(rx, ey, 13, 10, px, py);
      int n = (millis() / 250) % 4;
      for (int i = 0; i < n; i++) arc(100, 24, 12 + i * 5, -40, 40, 2);
      m = M_SMALLO; bl = 1; break; }
    case SHY:
      gl(lx, ey, 13, 9, -5, 5); gl(rx, ey, 13, 9, -5, 5); m = M_WAVY; bl = 2; break;
    case STARRY:
      star(lx, ey, 14, 5); star(rx, ey, 14, 5);
      if (tw) { sparkle(8, 10, 3); sparkle(120, 10, 3); }
      m = M_D; bl = 1; break;
    case CURIOUS:
      gl(lx, ey, 12, 9, px + 3, py); gl(rx, ey, 15, 10, px + 3, py);
      ln(rx - 10, ey - 19, rx + 8, ey - 23, 2);
      display.setTextSize(1); display.setTextColor(W); display.setCursor(116, 0 + yo); display.print("?");
      m = M_CAT; bl = 1; break;
    default: break;
  }

  blush(bl);
  if (talking && emo != SLEEPY) drawTalkingMouth();
  else if (m >= 0) mouth(m);
  display.display();
}

void message(const char* l1, const char* l2 = "") {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(W);
  display.setCursor(0, 20); display.println(l1);
  display.setCursor(0, 34); display.println(l2);
  display.display();
}

void setFace(Emotion e, unsigned long holdMs = 0) {
  emo = e;
  holdUntil = holdMs ? millis() + holdMs : 0;
}

void animate() {
  unsigned long now = millis();
  if (!blinking && now > nextBlink) { blinking = true; blinkEnd = now + 130; nextBlink = now + random(2500, 5000); }
  if (blinking && now > blinkEnd) blinking = false;
  if (now > nextLook) { lookX = random(-3, 4); lookY = random(-2, 3); nextLook = now + random(1500, 3500); }
}

// ================= audio =================
int micShift = MIC_SHIFT;     // the gain actually in use (changes by itself, see setMicShift)
// The wiring actually in use. The firmware tests the common wiring mistakes at start-up and adopts the combination
// that gives a clean microphone signal (see autoWire), so a swapped BCLK/WS pair or L/R on 3V3 still works.
int pinBclk = PIN_I2S_BCLK, pinWs = PIN_I2S_WS, pinDin = PIN_MIC_SD, pinDout = PIN_AMP_DIN;
int micChannel = 0;           // 0 = left slot (mic L/R pin on GND), 1 = right slot (L/R pin on 3V3)
// Reads n mono samples (left channel of the 32-bit stereo I2S stream). INMP441 data is in the top 24 bits.
float hpX = 0, hpY = 0;       // state of the high-pass filter
size_t readMono(int16_t* out, size_t n) {
  static int32_t tmp[2 * 160];
  size_t done = 0;
  while (done < n) {
    size_t want = min((size_t)160, n - done);
    size_t got = i2s.readBytes((char*)tmp, want * 8) / 8;
    if (!got) break;
    for (size_t i = 0; i < got; i++) {
      float x = (float)(tmp[2 * i + micChannel] >> micShift);
      float y = x - hpX + 0.94f * hpY;          // high-pass at ~150 Hz: removes the mic's DC offset and low rumble, keeps speech
      hpX = x; hpY = y;
      out[done + i] = (int16_t)constrain((int32_t)y, -32768, 32767);
    }
    done += got;
  }
  return done;
}

int peakOf(const int16_t* s, size_t n) {
  int p = 0;
  for (size_t i = 0; i < n; i++) { int a = s[i] < 0 ? -s[i] : s[i]; if (a > p) p = a; }
  return p;
}

int rmsOf(const int16_t* s, size_t n) {
  if (!n) return 0;
  long long sum = 0;
  for (size_t i = 0; i < n; i++) sum += (long long)s[i] * s[i];
  return (int)sqrt((double)(sum / n));
}

int16_t frameBuf[FRAME];
int16_t preRing[PRE_FRAMES][FRAME];
int preIdx = 0;
int threshold = 400;
int lastLevel = 0;            // latest microphone level, shown live on the dashboard
bool manualThr = false;       // true when the threshold was set by hand (VOICE_THRESHOLD or keys > <)
float noiseEst = 50;          // slowly follows the background noise, so the speech threshold adapts to the room
bool listenOn = true;         // false = muted (dashboard button, or key 'm' in the Serial Monitor)
int lastCode = 0;             // HTTP code of the last conversation (200 = answered, 204 = no speech understood)
unsigned long quietUntil = 0; // pause before listening again, so noise cannot retrigger straight away

// Automatic gain: moves the gain one step (each step doubles or halves the level) and rescales the noise estimate.
void setMicShift(int target) {
  target = constrain(target, 8, 18);
  if (target == micShift) return;
  float scale = powf(2.0f, (float)(micShift - target));   // new level / old level
  LOGI("MIC", "gain changed: MIC_SHIFT %d -> %d (levels x%.2f)", micShift, target, scale);
  micShift = target;
  noiseEst *= scale;
  threshold = max((int)(noiseEst * 3), (int)noiseEst + 150);
}

void calibrate() {
  message("Listening to the room", "stay quiet...");
  int noise = 0, pk = 0;
  for (int attempt = 0; attempt < 6; attempt++) {
    int lv[32], cnt = 0; pk = 0;
    for (int i = 0; i < 40; i++) {
      size_t n = readMono(frameBuf, FRAME);
      if (i >= 8 && cnt < 32) { lv[cnt++] = rmsOf(frameBuf, n); pk = max(pk, peakOf(frameBuf, n)); }
    }
    for (int i = 1; i < cnt; i++) { int v = lv[i], j = i - 1; while (j >= 0 && lv[j] > v) { lv[j + 1] = lv[j]; j--; } lv[j + 1] = v; }
    noise = cnt ? lv[cnt / 4] : 0;   // the 25th percentile: ignores short noise bursts (WiFi, OLED) and keeps the true background
    LOGD("MIC", "calibration pass %d: background level %d (typical), loudest sample %d, gain MIC_SHIFT %d", attempt + 1, noise, pk, micShift);
    if (pk >= 32000 && noise > 8000 && micShift < 17) {   // really saturating even at the typical level: turn the gain down and measure again
      LOGE("MIC", "the background reads %d with clipping (loudest sample %d): lowering the gain", noise, pk);
      micShift++;
      continue;
    }
    break;
  }
  noiseEst = noise;
  threshold = max(max(noise * 3, noise + 150), 250);
  if (VOICE_THRESHOLD > 0) { threshold = VOICE_THRESHOLD * 10 / 13; manualThr = true; }
  LOGI("MIC", "calibrated: background level %d, gain MIC_SHIFT %d, a recording starts above %d%s", noise, micShift, threshold * 13 / 10, manualThr ? " (fixed by you)" : "");
  if (noise == 0) LOGE("MIC", "the microphone sends NOTHING (level 0). Check mic VDD=3V3, GND, SD=GPIO33, SCK=GPIO14, WS=GPIO25, L/R=GND. Run RUN_MODE 4 for the full check");
  else if (noise > 3000) LOGE("MIC", "the background is very loud (%d): loud noise or voices nearby, or interference on the mic wires (keep them short, away from the OLED and speaker wires; add a 100 nF capacitor between mic VDD and GND). Run RUN_MODE 4", noise);
}

void sendChunk(WiFiClient& c, const int16_t* data, size_t samples) {
  c.printf("%x\r\n", (unsigned)(samples * 2));
  c.write((const uint8_t*)data, samples * 2);
  c.print("\r\n");
}

size_t spkWrite(uint8_t* d, size_t n) {   // sends sound to the amplifier, or does nothing when the laptop plays it
  return DEVICE_SPEAKER ? i2s.write(d, n) : n;
}

void playSilence(int ms) {
  static int32_t z[2 * 160];
  memset(z, 0, sizeof(z));
  for (int i = 0; i < ms / 10; i++) spkWrite((uint8_t*)z, sizeof(z));
}

// Volume with a soft limiter: loud peaks are squeezed instead of cut off, so higher volume stays clean.
inline int32_t amplify(int16_t s) {
  int32_t v = ((int32_t)s * SPEAKER_VOLUME) / 100;
  const int32_t knee = 20000;
  if (v > knee) v = knee + (v - knee) / 4;
  else if (v < -knee) v = -knee + (v + knee) / 4;
  return constrain(v, -32767, 32767);
}

void playTone(float freq, int ms) {
  static int32_t out[2 * 160];
  static float phase = 0;
  int total = SAMPLE_RATE * ms / 1000;
  for (int done = 0; done < total; done += 160) {
    for (int i = 0; i < 160; i++) {
      int32_t v = amplify((int16_t)(sin(phase) * 8000));
      phase += 2 * PI * freq / SAMPLE_RATE;
      if (phase > 2 * PI) phase -= 2 * PI;
      out[2 * i] = out[2 * i + 1] = v << 16;
    }
    spkWrite((uint8_t*)out, sizeof(out));
  }
}

// RUN_MODE 2: shows the microphone level on the OLED and in the Serial Monitor
void micMeter() {
  static unsigned long last = 0;
  static int peak = 0;
  size_t n = readMono(frameBuf, FRAME);
  int level = rmsOf(frameBuf, n);
  lastLevel = level;
  peak = max(level, (int)(peak * 0.97f));
  if (millis() - last < 100) return;
  last = millis();
  Serial.printf("level %5d  peak %5d  ", level, peak);
  for (int i = 0; i < min(level / 100, 50); i++) Serial.print('#');
  Serial.println("");
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(W);
  display.setCursor(0, 0); display.print("MIC TEST: talk or clap");
  display.drawRect(2, 20, 124, 16, W);
  display.fillRect(4, 22, constrain(level / 40, 0, 120), 12, W);
  display.setCursor(0, 44); display.print(String("level ") + level);
  display.setCursor(0, 54); display.print(String("peak  ") + peak);
  display.display();
}

// RUN_MODE 3: two beeps every 2 seconds
void speakerTest() {
  message("SPEAKER TEST", "beep boop");
  LOGI("PLAY", "beep (volume %d%%). If you hear nothing, run RUN_MODE 4", SPEAKER_VOLUME);
  ledSet(LED_SPEAK); ledUpdate();
  playTone(660, 150);
  playTone(880, 250);
  playSilence(100);
  delay(2000);
}

Emotion emotionFromName(String s) {
  s.toLowerCase();
  for (int i = 0; i < EMO_COUNT; i++) if (s == EMO_NAMES[i]) return (Emotion)i;
  return HAPPY;
}

void receiveAndPlay(WiFiClient& c) {
  unsigned long t0 = millis(), lastD = 0;
  ledSet(LED_THINK);
  LOGD("NET", "waiting for the server's answer (hearing, thinking, making the voice)...");
  while (!c.available() && c.connected() && millis() - t0 < 30000) {
    if (millis() - lastD > 100) { animate(); drawFace(); lastD = millis(); }
    ledUpdate();
    delay(5);
  }
  unsigned long waited = millis() - t0;
  if (!c.available()) {
    LOGE("NET", "no answer from the server after %lu ms. Look at the server terminal: it may be busy or have crashed", waited);
    lastCode = -1; setFace(SAD, 2500); ledSet(LED_ERROR);
    return;
  }
  String status = c.readStringUntil('\n');
  int code = status.substring(9, 12).toInt();
  long remaining = -1; String emoStr = "happy", text = "", provider = "-", tone = "-";
  bool laptopPlay = false;
  String heard = "";
  while (true) {
    String l = c.readStringUntil('\n');
    if (l.length() <= 1) break;
    l.trim();
    String low = l; low.toLowerCase();
    if (low.startsWith("content-length:")) remaining = l.substring(15).toInt();
    else if (low.startsWith("x-emotion:")) { emoStr = l.substring(10); emoStr.trim(); }
    else if (low.startsWith("x-text:")) { text = l.substring(7); text.trim(); }
    else if (low.startsWith("x-provider:")) { provider = l.substring(11); provider.trim(); }
    else if (low.startsWith("x-tone:")) { tone = l.substring(7); tone.trim(); }
    else if (low.startsWith("x-heard:")) { heard = l.substring(8); heard.trim(); }
    else if (low.startsWith("x-playback:")) laptopPlay = low.indexOf("laptop") > 0;
  }
  lastCode = code;
  Serial.printf("\n>>> I HEARD YOU SAY: \"%s\"\n\n", heard.length() ? heard.c_str() : "(nothing understood)");
  LOGI("NET", "server answered HTTP %d after %lu ms: tone=%s face=%s brain=%s", code, waited, tone.c_str(), emoStr.c_str(), provider.c_str());
  if (text.length()) LOGI("NET", "reply: %s", text.c_str());
  if (code == 204) LOGI("NET", "the server understood no speech in that recording (too noisy, too quiet, or nobody spoke)");
  if (code != 200) {
    if (code != 204) { LOGE("NET", "unexpected HTTP %d. Read the server terminal for the error", code); ledSet(LED_ERROR); }
    setFace(code == 204 ? HAPPY : SAD, 2500);
    return;
  }
  if (remaining == 0) LOGE("PLAY", "the server sent an EMPTY voice (Content-Length 0), so the speaker has nothing to play. The problem is on the Mac: check ffmpeg and the voice service in the server terminal, or press Run self-test on the dashboard");
  else LOGI("PLAY", "voice incoming: %ld bytes (about %ld ms)%s", remaining, remaining / 32, laptopPlay ? ". It plays on the LAPTOP speakers; this board only shows the face" : "");

  setFace(emotionFromName(emoStr));
  talking = true;
  ledSet(LED_SPEAK);
  alignas(4) static uint8_t buf[512];
  static int32_t out[2 * 256];
  int have = 0;
  unsigned long lastData = millis(), lastDraw = 0, p0 = millis(), stalls = 0;
  unsigned long bytesIn = 0, bytesOut = 0, shortWrites = 0;
  int peakOut = 0;
  while (remaining != 0 && (c.connected() || c.available())) {
    int avail = c.available();
    if (avail <= 0) {
      if (millis() - lastData > 4000) { LOGE("PLAY", "the voice stopped arriving (4 s gap): WiFi or server problem"); break; }
      delay(2); ledUpdate(); continue;
    }
    if (millis() - lastData > 300) stalls++;
    int got = c.read(buf + have, min(avail, (int)sizeof(buf) - have));
    if (got <= 0) continue;
    lastData = millis();
    bytesIn += got;
    have += got;
    if (remaining > 0) remaining -= got;
    int ns = have / 2;
    int16_t* s = (int16_t*)buf;
    float e = 0;
    for (int i = 0; i < ns; i++) {
      int32_t v = amplify(s[i]);
      e += abs(v);
      if (abs(v) > peakOut) peakOut = abs(v);
      out[2 * i] = out[2 * i + 1] = v << 16;
    }
    size_t w = ns * 8;
    if (laptopPlay || !DEVICE_SPEAKER) delay(ns / 16);   // the laptop plays the sound: just keep the face moving in time with it
    else w = spkWrite((uint8_t*)out, ns * 8);
    bytesOut += w;
    if (w != (size_t)(ns * 8)) shortWrites++;
    talkAmp = 0.6f * talkAmp + 0.4f * min(1.0f, (e / max(ns, 1)) / (5000.0f * SPEAKER_VOLUME / 100.0f));
    if (have & 1) { buf[0] = buf[have - 1]; have = 1; } else have = 0;
    if (millis() - lastDraw > 90) { animate(); drawFace(); lastDraw = millis(); }
    ledUpdate();
  }
  playSilence(120);
  talking = false; talkAmp = 0;
  LOGI("PLAY", "played %lu voice bytes in %lu ms, %lu bytes sent to the amplifier, loudest sample %d%% of full scale", bytesIn, millis() - p0, bytesOut, peakOut * 100 / 32767);
  if (bytesIn > 0 && peakOut < 100) LOGE("PLAY", "the voice data is silent (all near zero). The server made an empty or silent voice: check the server terminal");
  if (shortWrites > 0) LOGE("PLAY", "%lu I2S writes were cut short: the speaker output is not running properly. Run RUN_MODE 4", shortWrites);
  if (stalls > 3) LOGD("PLAY", "the voice arrived in %lu bursts (weak WiFi may cause stutter)", stalls);
  if (remaining > 0) LOGE("PLAY", "the voice was cut short: %ld bytes never arrived", remaining);
  for (int i = 0; i < 25; i++) readMono(frameBuf, FRAME);   // drop mic data captured while speaking (stops it hearing itself)
  setFace(emotionFromName(emoStr), 4000);
  lastActivity = millis();
  ledSet(listenOn ? LED_IDLE : LED_MUTED);
}

bool startNow = false;        // set by key 'r': record 4 seconds right now

// Median of the last 5 frame levels. Short noise bursts (OLED refresh, WiFi) hit only 1-2 frames and are ignored; speech lasts longer.
int median5(const int* h) {
  int a[5]; memcpy(a, h, sizeof(a));
  for (int i = 1; i < 5; i++) { int v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
  return a[2];
}

void conversation() {
  lastActivity = millis();
  setFace(LISTENING); drawFace();
  ledSet(LED_REC); ledUpdate();
  bool byButton = TRIGGER_MODE >= 1 && digitalRead(PIN_BTN) == LOW;
  bool byKey = startNow; startNow = false;
  bool manual = byButton || byKey;
  unsigned long keyEnd = millis() + 4000;
  if (byButton) LOGI("VAD", "BOOT button pressed: recording while you hold it...");
  else if (byKey) LOGI("VAD", "key r: recording for 4 seconds...");
  else LOGI("VAD", "speech detected: level %d is above the threshold %d, recording...", lastLevel, threshold);
  WiFiClient c;
  c.setNoDelay(true);
  unsigned long tc = millis();
  if (!c.connect(SERVER_HOST, SERVER_PORT, 3000)) {
    LOGE("NET", "cannot connect to %s:%d after %lu ms (WiFi %s, signal %d dBm). Check SERVER_HOST is your Mac's current IP, same WiFi, firewall, and that the server is running",
         SERVER_HOST, SERVER_PORT, millis() - tc, WiFi.status() == WL_CONNECTED ? "connected" : "DISCONNECTED", WiFi.RSSI());
    setFace(CRY, 3000);
    ledSet(LED_ERROR);
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    return;
  }
  LOGD("NET", "connected to the server in %lu ms", millis() - tc);
  c.print(String("POST /talk HTTP/1.1\r\nHost: ") + SERVER_HOST +
          "\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  for (int k = 0; k < PRE_FRAMES; k++) sendChunk(c, preRing[(preIdx + k) % PRE_FRAMES], FRAME);

  static int16_t batch[FRAME * 5];
  int bn = 0, frames = PRE_FRAMES, quiet = 0, peak = threshold, peakAbs = 0;
  unsigned long lastDraw = 0;
  lastCode = 0;
  int lh[5] = {0, 0, 0, 0, 0}, lhi = 0;
  while (frames < MAX_FRAMES) {
    if (manual) {
      if (byButton) { if (digitalRead(PIN_BTN) == HIGH && frames > 20) break; }
      else if (millis() > keyEnd) break;
    } else if (quiet >= SILENCE_FRAMES) break;
    size_t n = readMono(frameBuf, FRAME);
    lh[lhi] = rmsOf(frameBuf, n); lhi = (lhi + 1) % 5;
    int lvl = median5(lh);
    peakAbs = max(peakAbs, peakOf(frameBuf, n));
    if (lvl > peak) peak = lvl;
    int endLevel = max(threshold, peak * 3 / 10);   // the end of speech = well below the loudest moment, even in a noisy room
    if (lvl > endLevel) quiet = 0; else quiet++;
    memcpy(batch + bn * FRAME, frameBuf, FRAME * 2);
    if (++bn == 5) { sendChunk(c, batch, FRAME * 5); bn = 0; }
    frames++;
    if (millis() - lastDraw > 200) { animate(); drawFace(); lastDraw = millis(); }
    ledUpdate();
  }
  if (bn) sendChunk(c, batch, FRAME * bn);
  c.print("0\r\n\r\n");
  bool hitLimit = frames >= MAX_FRAMES;
  LOGI("VAD", "recording ended (%s) after %d ms, loudest level %d, sent %d KB of audio", hitLimit ? "TIME LIMIT" : manual ? "button/key released" : "silence", frames * 20, peak, frames * FRAME * 2 / 1024);
  if (peakAbs >= 32000) {
    LOGE("MIC", "that recording CLIPPED (loudest sample %d of 32767): the gain is too high or it is very loud. Lowering the gain for next time", peakAbs);
    setMicShift(micShift + 1);
  }
  if (hitLimit && !manual) LOGE("VAD", "the recording never went quiet: constant noise or voices nearby (peak %d, quiet line %d). Mute with 'm', or move the mic away. Dashboard shows the live level", peak, max(threshold, peak * 3 / 10));

  setFace(THINKING);
  receiveAndPlay(c);
  c.stop();
  if (lastCode == 200 && peakAbs > 0 && peakAbs < 4000 && micShift > 10) {   // understood, but the voice was quiet: a bit more gain helps
    LOGI("MIC", "your voice was quiet (loudest sample %d): raising the gain a little", peakAbs);
    setMicShift(micShift - 1);
  }
}

void setServerMode(int m) {
  HTTPClient http;
  http.begin(SERVER_HOST, SERVER_PORT, String("/mode?m=") + m);
  int code = http.GET();
  LOGI("CMD", "mode %d -> server answered HTTP %d", m, code);
  http.end();
}

// Tells the dashboard on your Mac how the device is doing (mic level, face, WiFi signal, memory),
// and receives its commands in the reply: show a face, beep the speaker. Open http://YOUR-MAC-IP:8000/dashboard
unsigned long nextReport = 0, nextBeat = 0;
int reportFails = 0;
bool linkUp = false;

int probeServer() {   // one-off check that the server answers: used at start-up and in the diagnostic
  unsigned long t0 = millis();
  WiFiClient c;
  if (!c.connect(SERVER_HOST, SERVER_PORT, 2500)) {
    LOGE("NET", "cannot connect to %s:%d after %lu ms. Check SERVER_HOST (your Mac's CURRENT IP), same WiFi, firewall, and that the server is running", SERVER_HOST, SERVER_PORT, millis() - t0);
    return -1;
  }
  unsigned long tc = millis() - t0;
  c.print(String("GET /api/state HTTP/1.1\r\nHost: ") + SERVER_HOST + "\r\nConnection: close\r\n\r\n");
  unsigned long t1 = millis();
  while (!c.available() && c.connected() && millis() - t1 < 2000) delay(1);
  String status = c.readStringUntil('\n');
  c.stop();
  int code = status.substring(9, 12).toInt();
  LOGI("NET", "server %s:%d reachable: connected in %lu ms, GET /api/state -> HTTP %d", SERVER_HOST, SERVER_PORT, tc, code);
  return code;
}

void shipLogs() {   // sends pending log lines to the server so they show on the dashboard and in the server terminal
  if (logBuf.length() == 0) return;
  WiFiClient lc;
  if (!lc.connect(SERVER_HOST, SERVER_PORT, 300)) return;
  String body = logBuf;
  logBuf = "";
  lc.print(String("POST /api/devlog HTTP/1.1\r\nHost: ") + SERVER_HOST + "\r\nContent-Type: text/plain\r\nContent-Length: " +
           body.length() + "\r\nConnection: close\r\n\r\n" + body);
  unsigned long t = millis();
  while (!lc.available() && lc.connected() && millis() - t < 300) delay(1);
  lc.stop();
}

void report() {
  unsigned long now = millis();
  if (now < nextReport || WiFi.status() != WL_CONNECTED) return;
  WiFiClient c;
  if (!c.connect(SERVER_HOST, SERVER_PORT, 300)) {   // server not up: try again in 10 s
    if (linkUp || reportFails == 0) LOGE("NET", "dashboard link down: cannot reach %s:%d (is the server running? is SERVER_HOST right?)", SERVER_HOST, SERVER_PORT);
    linkUp = false; reportFails++;
    nextReport = now + 10000;
    return;
  }
  if (!linkUp) { LOGI("NET", "dashboard link up: reporting to %s:%d", SERVER_HOST, SERVER_PORT); linkUp = true; reportFails = 0; }
  c.print(String("GET /api/device?lvl=") + lastLevel + "&thr=" + threshold + "&rssi=" + WiFi.RSSI() + "&emo=" + EMO_NAMES[emo] +
          "&heap=" + ESP.getFreeHeap() + "&up=" + (now / 1000) + "&vol=" + SPEAKER_VOLUME + "&gain=" + micShift +
          " HTTP/1.1\r\nHost: " + SERVER_HOST + "\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (!c.available() && c.connected() && millis() - t0 < 500) delay(1);
  c.readStringUntil('\n');                                              // status line
  while (true) { String l = c.readStringUntil('\n'); if (l.length() <= 1) break; }   // headers
  String body = c.readStringUntil('\n');                                // e.g. "force=wink beep=0 listen=1"
  c.stop();
  nextReport = millis() + 1500;
  int f = body.indexOf("force=");
  if (f >= 0) {
    String name = body.substring(f + 6);
    int sp = name.indexOf(' ');
    if (sp >= 0) name = name.substring(0, sp);
    name.trim();
    for (int i = 0; i < EMO_COUNT; i++) if (name == EMO_NAMES[i]) { LOGI("CMD", "dashboard asked for face '%s'", name.c_str()); setFace((Emotion)i, 3500); break; }
  }
  int li = body.indexOf("listen=");
  if (li >= 0) {
    bool on = body.charAt(li + 7) != '0';
    if (on != listenOn) { listenOn = on; setFace(on ? HAPPY : SLEEPY); ledSet(on ? LED_IDLE : LED_MUTED); LOGI("CMD", "%s", on ? "listening: on" : "listening: OFF (muted from the dashboard)"); }
  }
  if (body.indexOf("beep=1") >= 0) { LOGI("CMD", "dashboard asked for a beep"); ledSet(LED_SPEAK); playTone(660, 120); playTone(880, 160); playSilence(60); ledSet(listenOn ? LED_IDLE : LED_MUTED); }
  shipLogs();
}

void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'h') setFace(HAPPY);
    else if (ch == 'l') setFace(LISTENING);
    else if (ch == 't') setFace(THINKING);
    else if (ch == 'n') setFace((Emotion)((emo + 1) % EMO_COUNT));
    else if (ch == 'p') setFace((Emotion)((emo + EMO_COUNT - 1) % EMO_COUNT));
    else if (ch == 'k') { mouthDemo = !mouthDemo; talking = mouthDemo; }
    else if (ch == 'm') { listenOn = !listenOn; setFace(listenOn ? HAPPY : SLEEPY); ledSet(listenOn ? LED_IDLE : LED_MUTED); LOGI("CMD", "%s", listenOn ? "listening: on" : "listening: OFF (muted)"); }
    else if (ch == 'r') { startNow = true; }
    else if (ch == '>' || ch == '<') { manualThr = true; threshold = ch == '>' ? threshold * 5 / 4 + 1 : max(100, threshold * 4 / 5); LOGI("MIC", "recording now starts above %d (keys > and < change it; the room is at %d)", threshold * 13 / 10, (int)noiseEst); }
    else if (ch >= '1' && ch <= '4' && RUN_MODE == 0) setServerMode(ch - '1');
    if (ch == 'h' || ch == 'l' || ch == 't' || ch == 'n' || ch == 'p') Serial.printf("face: %s\n", EMO_NAMES[emo]);
  }
}

// ================= setup / loop =================
// ================= RUN_MODE 4: guided hardware diagnostic =================
// Checks power, OLED, WiFi, the server, the microphone (raw data, both channels, reaction to sound) and the
// speaker (real-time output + you confirm you heard it). Prints one report you can copy and send for help.
int diagFails = 0;
String diagSummary;
void check(bool ok, const char* name, const char* detail) {
  LOGI("DIAG", "%s  %s%s%s", ok ? "PASS" : "FAIL", name, detail[0] ? "  ->  " : "", detail);
  diagSummary += String(ok ? "PASS  " : "FAIL  ") + name + "\n";
  if (!ok) diagFails++;
}

void chanAdd(Chan& c, int32_t raw) {
  int32_t v = raw >> 8;                       // the mic sends 24-bit samples in the top of a 32-bit slot
  c.n++;
  if (v == 0) c.zeros++;
  if (v < c.mn) c.mn = v;
  if (v > c.mx) c.mx = v;
  c.sum += v; c.sq += (double)v * v;
}
long chanAc(const Chan& c) {                  // how much the signal wiggles (noise + sound), in 24-bit units
  if (!c.n) return 0;
  double m = c.sum / c.n, var = c.sq / c.n - m * m;
  return var > 0 ? (long)sqrt(var) : 0;
}
bool chanAlive(const Chan& c) { return c.n > 0 && c.zeros < c.n * 99 / 100 && c.mx != c.mn; }

long captureRaw(Chan& L, Chan& R, int ms) {   // returns how many stereo frames arrived
  static int32_t tmp[2 * 160];
  long frames = 0, target = (long)SAMPLE_RATE * ms / 1000;
  unsigned long t0 = millis();
  int empty = 0;
  while (frames < target && millis() - t0 < (unsigned long)ms + 1500) {
    size_t got = i2s.readBytes((char*)tmp, sizeof(tmp)) / 8;
    if (!got) { if (++empty > 5) break; continue; }
    for (size_t i = 0; i < got; i++) { chanAdd(L, tmp[2 * i]); chanAdd(R, tmp[2 * i + 1]); }
    frames += got;
    ledUpdate();
  }
  return frames;
}

bool startI2S(int bclk, int ws, int dout, int din) {
  if (!DEVICE_SPEAKER) dout = -1;   // microphone only: leave the amplifier line idle, like the mic test that works
  i2s.end();
  i2s.setPins(bclk, ws, dout, din);
  return i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
}

bool cleanChan(const Chan& c) { long ac = chanAc(c); return chanAlive(c) && ac >= 100 && ac < 6000000; }

// Looks for a clean microphone signal with the wire combinations people most often get wrong:
//   1) as in the sketch  2) BCLK and WS swapped  3) mic SD and amp DIN swapped  4) both swapped.
// BCLK and WS are shared by the mic and the amplifier, so a swapped pair breaks BOTH (noise from the mic, silence from the speaker).
bool autoWire(bool verbose) {
  struct Combo { bool swapClk, swapData; const char* name; };
  const Combo combos[4] = {{false, false, "as wired in the sketch"}, {true, false, "BCLK and WS swapped"},
                           {false, true, "mic SD and amp DIN swapped"}, {true, true, "both swapped"}};
  for (int k = 0; k < 4; k++) {
    int b  = combos[k].swapClk  ? PIN_I2S_WS   : PIN_I2S_BCLK, w    = combos[k].swapClk  ? PIN_I2S_BCLK : PIN_I2S_WS;
    int di = combos[k].swapData ? PIN_AMP_DIN  : PIN_MIC_SD,   dout = combos[k].swapData ? PIN_MIC_SD    : PIN_AMP_DIN;
    if (!startI2S(b, w, dout, di)) { LOGE("I2S", "could not start I2S for: %s", combos[k].name); continue; }
    Chan L, R;
    long fr = captureRaw(L, R, 600);
    bool lc = cleanChan(L), rc = cleanChan(R);
    LOGI("I2S", "wiring test [%s] BCLK=%d WS=%d SD=%d: %ld frames | LEFT wiggle %ld %s | RIGHT wiggle %ld %s", combos[k].name, b, w, di, fr,
         chanAc(L), lc ? "CLEAN" : "no", chanAc(R), rc ? "CLEAN" : "no");
    if (lc || rc) {
      pinBclk = b; pinWs = w; pinDin = di; pinDout = dout;
      micChannel = (lc && (!rc || chanAc(L) <= chanAc(R))) ? 0 : 1;
      if (k > 0) LOGE("I2S", "FOUND IT: the microphone only works with '%s'. Your real wires differ from the sketch. Using that now. To make it permanent, swap those wires, or change the PIN_ lines at the top of the sketch", combos[k].name);
      LOGI("I2S", "microphone found on the %s slot", micChannel == 0 ? "LEFT" : "RIGHT");
      if (micChannel == 1) LOGE("I2S", "the mic answers on the RIGHT slot, so its L/R pin is probably on 3V3. Using the right slot. For the standard setup connect L/R to GND");
      return true;
    }
  }
  startI2S(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DIN, PIN_MIC_SD);   // nothing clean anywhere: go back to the sketch's own pins
  pinBclk = PIN_I2S_BCLK; pinWs = PIN_I2S_WS; pinDin = PIN_MIC_SD; pinDout = PIN_AMP_DIN; micChannel = 0;
  LOGE("I2S", "NO wire combination gives a clean microphone signal. So it is not just swapped wires. Check: mic VDD on 3V3 (not 5V), mic GND, the SD wire really on GPIO%d, L/R on GND, short wires, no loose breadboard contact", PIN_MIC_SD);
  return false;
}

void runDiagnostics() {
  diagFails = 0; diagSummary = "";
  char buf[200];
  LOGI("DIAG", "=================== PIP DIAGNOSTIC REPORT (start) ===================");
  LOGI("DIAG", "Copy everything from this line to the END line and send it for help.");
  LOGI("DIAG", "Sketch pins: BCLK=%d WS=%d mic SD=%d amp DIN=%d | OLED SDA=%d SCL=%d | server %s:%d", PIN_I2S_BCLK, PIN_I2S_WS, PIN_MIC_SD, PIN_AMP_DIN, OLED_SDA, OLED_SCL, SERVER_HOST, SERVER_PORT);
  LOGI("DIAG", "Settings: MIC_SHIFT %d, SPEAKER_VOLUME %d%%", micShift, SPEAKER_VOLUME);

  // 1. power
  LOGI("DIAG", "--- 1. Board and power ---");
  LOGI("DIAG", "chip %s rev %d, %d core(s), %d MHz, flash %u KB, free memory %u bytes", ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores(), (int)ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() / 1024), (unsigned)ESP.getFreeHeap());
  LOGI("DIAG", "last reset reason: %s", resetReasonText());
  check(esp_reset_reason() != ESP_RST_BROWNOUT, "power is stable (no brown-out)", esp_reset_reason() == ESP_RST_BROWNOUT ? "the board browned out. Use a short good USB cable in a direct port, or lower SPEAKER_VOLUME" : "");

  // 2. OLED + I2C
  LOGI("DIAG", "--- 2. OLED (I2C scan) ---");
  String addrs; int found = 0; bool has3c = false;
  for (uint8_t a = 8; a < 120; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { char ab[10]; snprintf(ab, sizeof(ab), " 0x%02X", a); addrs += ab; found++; if (a == OLED_ADDR) has3c = true; }
  }
  LOGI("DIAG", "I2C devices found: %d:%s", found, addrs.c_str());
  check(has3c, "OLED answers on I2C", has3c ? "" : "not found at the address in the sketch. Check VDD=3V3, GND, SCK=GPIO19, SDA=GPIO21");

  // 3. WiFi + server
  LOGI("DIAG", "--- 3. WiFi and server ---");
  bool wifi = WiFi.status() == WL_CONNECTED;
  check(wifi, "WiFi connected", wifi ? "" : "wrong name or password, or a 5 GHz network (the ESP32 needs 2.4 GHz)");
  if (wifi) {
    LOGI("DIAG", "network '%s': my IP %s, router %s, signal %d dBm", WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    snprintf(buf, sizeof(buf), "signal %d dBm (stronger than -80 is good)", WiFi.RSSI());
    check(WiFi.RSSI() > -80, "WiFi signal is strong enough", WiFi.RSSI() > -80 ? "" : buf);
    int code = probeServer();
    snprintf(buf, sizeof(buf), "no answer from %s:%d. Is SERVER_HOST your Mac's CURRENT IP (ipconfig getifaddr en0)? Same WiFi? Firewall? Server running?", SERVER_HOST, SERVER_PORT);
    check(code == 200, "the server answers", code == 200 ? "" : buf);
  }

  // 4. microphone, raw
  LOGI("DIAG", "--- 4. Microphone: stay QUIET for 2 seconds ---");
  ledSet(LED_THINK); ledUpdate();
  bool wiringOk = autoWire(true);       // tries the common wiring mistakes and keeps the combination that works
  check(wiringOk, "a clean microphone signal exists with the wiring in use", wiringOk ? "" : "no pin or channel combination gives a clean signal: see the I2S lines above");
  check(pinBclk == PIN_I2S_BCLK && pinWs == PIN_I2S_WS, "BCLK and WS wires match the sketch", (pinBclk == PIN_I2S_BCLK && pinWs == PIN_I2S_WS) ? "" : "SWAPPED on the board: GPIO14 and GPIO25 wires are crossed (the mic and the amp share them, so this breaks both). Swap the two wires, or the firmware keeps using the swapped pins");
  check(pinDin == PIN_MIC_SD, "mic SD and amp DIN wires match the sketch", pinDin == PIN_MIC_SD ? "" : "SWAPPED on the board: the mic SD and amp DIN wires are crossed (GPIO33 and GPIO22)");
  check(micChannel == 0, "microphone L/R pin is on GND (left slot)", micChannel == 0 ? "" : "the mic answers on the RIGHT slot: its L/R pin is on 3V3. Connect it to GND");
  Chan L, R;
  long frames = captureRaw(L, R, 2000);
  Chan& M = (micChannel == 0) ? L : R;   // the channel the microphone is really on
  LOGI("DIAG", "I2S read %ld frames (expected about %d)", frames, SAMPLE_RATE * 2);
  LOGI("DIAG", "LEFT  channel: %ld%% zeros, range %ld .. %ld, wiggle %ld", L.n ? L.zeros * 100 / L.n : 0L, (long)L.mn, (long)L.mx, chanAc(L));
  LOGI("DIAG", "RIGHT channel: %ld%% zeros, range %ld .. %ld, wiggle %ld", R.n ? R.zeros * 100 / R.n : 0L, (long)R.mn, (long)R.mx, chanAc(R));
  check(frames > SAMPLE_RATE, "I2S is receiving audio frames", frames > SAMPLE_RATE ? "" : "nothing arrives: the I2S clock is not running. Check BCLK=GPIO14 and WS=GPIO25 wires");
  bool lAlive = chanAlive(M), rAlive = chanAlive(micChannel == 0 ? R : L);
  const char* micHint = "";
  if (!lAlive && rAlive) micHint = "the data is on the RIGHT channel: connect the mic's L/R pin to GND (not 3V3)";
  else if (!lAlive) micHint = "all zeros or constant: check mic VDD=3V3, GND, SD=GPIO33, SCK=GPIO14, WS=GPIO25 and L/R=GND, and that SD is not swapped with another wire";
  check(lAlive, "microphone sends data", micHint);
  long ac = chanAc(M);
  snprintf(buf, sizeof(buf), "wiggle %ld: a working mic in a quiet room shows roughly 300 to 2000000. Below 100 = the SD wire is floating or disconnected. Above 6000000 = loud noise nearby, or interference on the SD/clock wires (keep them short, away from the speaker wires)", ac);
  check(lAlive && ac > 100 && ac < 6000000, "microphone noise level looks like a real microphone", (lAlive && ac > 100 && ac < 6000000) ? "" : buf);
  bool stuckHigh = M.n && fabs(M.sum / M.n) > 6000000.0;
  check(!stuckHigh, "microphone data is not stuck at full scale", stuckHigh ? "the SD line is stuck high: wire shorted to 3V3, or the mic has no ground" : "");

  // 5. microphone reacts to sound
  LOGI("DIAG", "--- 5. Microphone reaction: CLAP or speak LOUDLY for the next 4 seconds ---");
  ledSet(LED_REC); ledUpdate();
  int peakLvl = 0, minLvl = 32767;
  unsigned long t0 = millis();
  while (millis() - t0 < 4000) {
    size_t n = readMono(frameBuf, FRAME);
    int lv = rmsOf(frameBuf, n);
    if (lv > peakLvl) peakLvl = lv;
    if (lv < minLvl) minLvl = lv;
    ledUpdate();
  }
  LOGI("DIAG", "quietest level %d, loudest level %d (levels after MIC_SHIFT %d; healthy: quiet 50-1500, clap 3000-30000)", minLvl, peakLvl, micShift);
  if (peakLvl >= 30000) LOGE("DIAG", "the clap CLIPPED: raise MIC_SHIFT by 1 (or 2) in the sketch, the mic is too sensitive");
  else if (peakLvl < 3000) LOGI("DIAG", "the clap was weak: clap closer, or lower MIC_SHIFT by 1");
  if (minLvl > 3000) LOGE("DIAG", "even the QUIETEST moment reads %d: there is steady loud noise nearby, or the mic wires are picking up interference", minLvl);
  bool reacts = peakLvl > 200 && peakLvl > minLvl * 3;
  snprintf(buf, sizeof(buf), "quiet %d, loud %d. Clap closer to the mic. If still flat, lower MIC_SHIFT by 1 or recheck the mic wiring", minLvl, peakLvl);
  check(reacts, "microphone reacts to sound", reacts ? "" : buf);

  // 6. speaker
  LOGI("DIAG", "--- 6. Speaker: three beeps, LISTEN ---");
  ledSet(LED_SPEAK);
  bool paced = true;
  for (int k = 0; k < 3; k++) {
    unsigned long b0 = millis();
    playTone(880 - k * 140, 400);
    unsigned long dt = millis() - b0;
    LOGI("DIAG", "beep %d: 400 ms of audio went out in %lu ms", k + 1, dt);
    if (dt < 250 || dt > 1500) paced = false;
    delay(150);
  }
  playSilence(100);
  check(paced, "speaker data leaves in real time (I2S output is running)", paced ? "" : "the output finished instantly or stalled: the I2S clock is not running. Check BCLK=GPIO14 and WS=GPIO25");
  LOGI("DIAG", ">>> Did you HEAR three beeps? Type  y  or  n  in the Serial Monitor box and press Enter (15 s)");
  int heard = -1;
  unsigned long a0 = millis();
  while (millis() - a0 < 15000 && heard < 0) {
    ledUpdate();
    if (Serial.available()) { char ch = Serial.read(); if (ch == 'y' || ch == 'Y') heard = 1; else if (ch == 'n' || ch == 'N') heard = 0; }
    delay(10);
  }
  if (heard < 0) LOGI("DIAG", "no answer typed, the speaker result is unknown");
  else check(heard == 1, "you heard the beeps", heard == 1 ? "" : "no sound. Check amp VIN=5V, GND, DIN=GPIO22, BCLK=GPIO14, LRC=GPIO25, speaker wires on the amp's screw terminals (never GND), and that the amp's SD pin is not tied to GND");

  // result
  LOGI("DIAG", "--- RESULT ---");
  LOGI("DIAG", "%s", diagSummary.c_str());
  if (diagFails == 0) LOGI("DIAG", "ALL CHECKS PASSED. The hardware is fine. If the bot still misbehaves, the problem is the server or the room noise.");
  else LOGE("DIAG", "%d CHECK(S) FAILED. Fix the first FAIL above first, then run this again.", diagFails);
  LOGI("DIAG", "=================== PIP DIAGNOSTIC REPORT (END) ===================");
  ledSet(diagFails ? LED_ERROR : LED_IDLE);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  if (PIN_LED >= 0) pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  ledSet(LED_BOOT); ledUpdate();
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  LOGI("BOOT", "==============================================");
  LOGI("BOOT", "Pip starting. RUN_MODE %d, log level %d", RUN_MODE, LOG_LEVEL);
  LOGI("BOOT", "chip %s rev %d, %d core(s) at %d MHz, free memory %u bytes", ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores(), (int)ESP.getCpuFreqMHz(), (unsigned)ESP.getFreeHeap());
  LOGI("BOOT", "last reset: %s", resetReasonText());
  LOGI("BOOT", "pins: I2S BCLK=%d WS=%d | mic SD=%d | amp DIN=%d | OLED SDA=%d SCL=%d | LED=%d", PIN_I2S_BCLK, PIN_I2S_WS, PIN_MIC_SD, PIN_AMP_DIN, OLED_SDA, OLED_SCL, PIN_LED);
  LOGI("BOOT", "settings: starting mic gain MIC_SHIFT=%d, speaker volume %d%%", MIC_SHIFT, SPEAKER_VOLUME);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    LOGE("OLED", "display not found at 0x%02X. Check VDD=3V3, GND, SCK=GPIO%d, SDA=GPIO%d", OLED_ADDR, OLED_SCL, OLED_SDA);
    ledSet(LED_ERROR);
    while (true) { ledUpdate(); delay(20); }
  }
  LOGI("OLED", "display found at 0x%02X", OLED_ADDR);
  randomSeed(micros());
  setFace(STARRY, 1500); drawFace();

#if RUN_MODE == 1
  LOGI("BOOT", "FACE TEST: faces cycle automatically. n/p = next/previous, k = talking mouth.");
  ledSet(LED_IDLE);
#else
#if RUN_MODE == 0 || RUN_MODE == 4
  message("Connecting WiFi...", WIFI_SSID);
  LOGI("WIFI", "joining '%s'...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long w0 = millis(), lastNote = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(100); ledUpdate();
    if (millis() - lastNote > 5000) {
      lastNote = millis();
      LOGE("WIFI", "not connected after %lu s (status %d). Wrong name or password, a 5 GHz network (the ESP32 needs 2.4 GHz), or out of range", (millis() - w0) / 1000, (int)WiFi.status());
    }
  }
  LOGI("WIFI", "connected to '%s' in %lu ms: my IP %s, router %s, signal %d dBm", WIFI_SSID, millis() - w0, WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
  LOGI("WIFI", "the Mac must be on this same network: its IP should start like %s", WiFi.localIP().toString().c_str());
  probeServer();
#endif
  if (!startI2S(pinBclk, pinWs, pinDout, pinDin)) {
    LOGE("I2S", "I2S failed to start (BCLK=%d WS=%d mic SD=%d amp DIN=%d). Set RUN_MODE 4 for the diagnostic", PIN_I2S_BCLK, PIN_I2S_WS, PIN_MIC_SD, PIN_AMP_DIN);
    message("I2S failed", "check wiring");
    ledSet(LED_ERROR);
    while (true) { ledUpdate(); delay(20); }
  }
  LOGI("I2S", "started: %d Hz, 32-bit stereo. Clock BCLK=GPIO%d WS=GPIO%d, mic data in GPIO%d, speaker data out GPIO%d", SAMPLE_RATE, pinBclk, pinWs, pinDin, pinDout);
#if RUN_MODE == 0
  autoWire(false);
  calibrate();
  LOGI("BOOT", "TRIGGER_MODE %d: %s", TRIGGER_MODE, TRIGGER_MODE == 0 ? "voice starts a recording" : TRIGGER_MODE == 1 ? "HOLD the BOOT button while you talk (or press key r)" : "voice starts a recording, or HOLD the BOOT button (or press key r)");
  LOGI("BOOT", "READY. Say a short sentence close to the microphone. LED: short blip = listening, solid = recording, blinking = thinking");
  ledSet(LED_IDLE);
#elif RUN_MODE == 2
  autoWire(false);
  LOGI("BOOT", "MIC TEST: talk or clap, the bar on the OLED should grow.");
  ledSet(LED_IDLE);
#elif RUN_MODE == 3
  LOGI("BOOT", "SPEAKER TEST: you should hear a beep every 2 seconds.");
  ledSet(LED_IDLE);
#else
  runDiagnostics();
#endif
#endif
  lastActivity = millis();
  setFace(HAPPY);
}

void loop() {
  handleSerial();
  animate();
  ledUpdate();
  static unsigned long lastDraw = 0, lastCycle = 0;

#if RUN_MODE == 1
  if (millis() - lastCycle > 2500 && !mouthDemo) { lastCycle = millis(); setFace((Emotion)((emo + 1) % EMO_COUNT)); LOGI("OLED", "face: %s", EMO_NAMES[emo]); }
  if (mouthDemo) talkAmp = (sin(millis() / 90.0) + 1) / 2;
  drawFace();
  delay(50);
#elif RUN_MODE == 2
  micMeter();
#elif RUN_MODE == 3
  speakerTest();
#elif RUN_MODE == 4
  micMeter();      // keep a live level bar after the report, and keep the dashboard link alive
  report();
#else
  size_t n = readMono(frameBuf, FRAME);
  memcpy(preRing[preIdx], frameBuf, FRAME * 2);
  preIdx = (preIdx + 1) % PRE_FRAMES;

  static int loud = 0;
  static int lvh[5] = {0, 0, 0, 0, 0}, lvi = 0;
  lvh[lvi] = rmsOf(frameBuf, n); lvi = (lvi + 1) % 5;
  lastLevel = median5(lvh);          // short noise bursts are ignored; speech is not
  if (n == 0) LOGE("MIC", "the I2S read returned no audio. The microphone clock is not running (check BCLK GPIO14 / WS GPIO25). Run RUN_MODE 4");
  if (lastLevel > 3000 && millis() > nextBeat - 5000 && millis() > quietUntil) {   // very loud while idle: say so, now and then
    static unsigned long lastLoudWarn = 0;
    if (millis() - lastLoudWarn > 15000) { lastLoudWarn = millis(); LOGE("MIC", "the mic level is very high (%d) with nobody speaking. Steady noise or voices nearby, or interference on the mic wires. Mute with 'm' if people are talking", lastLevel); }
  }
  if (!manualThr && lastLevel < threshold) {         // follow the room's background noise slowly
    noiseEst = noiseEst * 0.99f + lastLevel * 0.01f;
    threshold = max((int)(noiseEst * 3), (int)noiseEst + 150);
  }
  if (millis() < quietUntil || !listenOn) loud = 0;
  else if (TRIGGER_MODE != 1 && lastLevel > threshold * 13 / 10) loud++; else loud = 0;

  if (millis() > nextBeat) {
    nextBeat = millis() + 10000;
    LOGD("LIVE", "alive: mic level %d (room noise %d, recording starts above %d), memory %u, WiFi %d dBm, listening %s, last answer HTTP %d",
         lastLevel, (int)noiseEst, threshold * 13 / 10, (unsigned)ESP.getFreeHeap(), WiFi.RSSI(), listenOn ? "yes" : "NO (muted)", lastCode);
    LOGD("LIVE", "mic gain MIC_SHIFT %d", micShift);
  }
  if (millis() - lastDraw > 250) {                   // the OLED refresh can disturb the microphone, so keep it gentle while listening
    lastDraw = millis();
    if (holdUntil && millis() > holdUntil) { holdUntil = 0; setFace(listenOn ? HAPPY : SLEEPY); lastActivity = millis(); }
    if (!holdUntil && emo != SLEEPY && millis() - lastActivity > 60000) setFace(SLEEPY);
    drawFace();
  }
  if (ledMode == LED_ERROR && millis() - lastActivity > 6000) ledSet(listenOn ? LED_IDLE : LED_MUTED);
  if (loud == 0) report();
  bool pressed = TRIGGER_MODE >= 1 && listenOn && millis() > quietUntil && digitalRead(PIN_BTN) == LOW;
  if (startNow || pressed || loud >= 3) {
    loud = 0;
    conversation();
    if (ledMode == LED_THINK || ledMode == LED_REC) ledSet(listenOn ? LED_IDLE : LED_MUTED);
    quietUntil = millis() + (lastCode == 200 ? 1200 : 3500);
  }
#endif
}
