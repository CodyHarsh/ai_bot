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
    0 = full voice bot   1 = face test only   2 = microphone test   3 = speaker test
  In mode 0 the board also reports to the live dashboard: http://YOUR-MAC-IP:8000/dashboard

  Serial Monitor (115200) commands:
    n / p = next / previous emotion   h = happy   l = listening   t = thinking
    k = toggle "talking mouth" demo   1..4 = switch mode (Buddy, Study, Mood, Sleep)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP_I2S.h>

// ================= CONFIG: edit these =================
const char* WIFI_SSID   = "YOUR_WIFI_NAME";
const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";
const char* SERVER_HOST = "192.168.1.50";   // your laptop's IP, where server.py runs
const int   SERVER_PORT = 8000;
#define RUN_MODE 0           // 0 = full voice bot, 1 = face test, 2 = microphone test, 3 = speaker test
#define SPEAKER_VOLUME 200   // percent: 100 = as received, 200 = twice as loud, 300 = three times (a soft limiter avoids harsh clipping)
#define MIC_SHIFT 12         // mic gain: lower = louder. 14 = quiet, 12 = 4x louder, 11 = 8x, 10 = 16x
// ======================================================

// ---------- pins (from your Cirkit circuit) ----------
#define PIN_I2S_BCLK 14
#define PIN_I2S_WS   25
#define PIN_MIC_SD   33   // INMP441 SD  -> ESP32 (data in)
#define PIN_AMP_DIN  22   // ESP32 -> MAX98357A DIN (data out)
#define OLED_SDA 21
#define OLED_SCL 19
#define OLED_ADDR 0x3C

#define SAMPLE_RATE 16000
#define FRAME 320                 // 20 ms of audio
#define PRE_FRAMES 4              // 80 ms of audio kept from before speech was detected
#define MAX_FRAMES 400            // max 8 s per utterance
#define SILENCE_FRAMES 38         // ~760 ms of quiet ends the utterance

Adafruit_SSD1306 display(128, 64, &Wire, -1);
I2SClass i2s;

// ---------- emotions ----------
enum Emotion { HAPPY, EXCITED, LOVE, WINK, JOY, SILLY, SLEEPY, SAD, CRY, POUT,
               SURPRISED, THINKING, LISTENING, SHY, STARRY, CURIOUS, EMO_COUNT };
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

enum Mouth { M_SMILE, M_CAT, M_D, M_FROWN, M_O, M_SMALLO, M_FLAT, M_WAVY, M_TONGUE, M_WAIL, M_PUFF };
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
// Reads n mono samples (left channel of the 32-bit stereo I2S stream). INMP441 data is in the top 24 bits.
size_t readMono(int16_t* out, size_t n) {
  static int32_t tmp[2 * 160];
  size_t done = 0;
  while (done < n) {
    size_t want = min((size_t)160, n - done);
    size_t got = i2s.readBytes((char*)tmp, want * 8) / 8;
    if (!got) break;
    for (size_t i = 0; i < got; i++) {
      int32_t v = tmp[2 * i] >> MIC_SHIFT;
      out[done + i] = (int16_t)constrain(v, -32768, 32767);
    }
    done += got;
  }
  return done;
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

void calibrate() {
  message("Listening to the room", "stay quiet...");
  long total = 0; int cnt = 0;
  for (int i = 0; i < 50; i++) {
    size_t n = readMono(frameBuf, FRAME);
    if (i >= 10) { total += rmsOf(frameBuf, n); cnt++; }
  }
  int noise = cnt ? total / cnt : 50;
  threshold = max(noise * 3, noise + 150);
  Serial.printf("Noise floor %d, speech threshold %d\n", noise, threshold);
}

void sendChunk(WiFiClient& c, const int16_t* data, size_t samples) {
  c.printf("%x\r\n", (unsigned)(samples * 2));
  c.write((const uint8_t*)data, samples * 2);
  c.print("\r\n");
}

void playSilence(int ms) {
  static int32_t z[2 * 160];
  memset(z, 0, sizeof(z));
  for (int i = 0; i < ms / 10; i++) i2s.write((uint8_t*)z, sizeof(z));
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
    i2s.write((uint8_t*)out, sizeof(out));
  }
}

// RUN_MODE 2: shows the microphone level on the OLED and in the Serial Monitor
void micMeter() {
  static unsigned long last = 0;
  static int peak = 0;
  size_t n = readMono(frameBuf, FRAME);
  int level = rmsOf(frameBuf, n);
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
  Serial.println("beep");
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
  while (!c.available() && c.connected() && millis() - t0 < 30000) {
    if (millis() - lastD > 100) { animate(); drawFace(); lastD = millis(); }
    delay(5);
  }
  String status = c.readStringUntil('\n');
  int code = status.substring(9, 12).toInt();
  long remaining = -1; String emoStr = "happy", text = "";
  while (true) {
    String l = c.readStringUntil('\n');
    if (l.length() <= 1) break;
    l.trim();
    String low = l; low.toLowerCase();
    if (low.startsWith("content-length:")) remaining = l.substring(15).toInt();
    else if (low.startsWith("x-emotion:")) { emoStr = l.substring(10); emoStr.trim(); }
    else if (low.startsWith("x-text:")) { text = l.substring(7); text.trim(); }
  }
  Serial.printf("Server: HTTP %d, emotion=%s, text=%s\n", code, emoStr.c_str(), text.c_str());
  if (code != 200) { setFace(code == 204 ? HAPPY : SAD, 2500); return; }

  setFace(emotionFromName(emoStr));
  talking = true;
  alignas(4) static uint8_t buf[512];
  static int32_t out[2 * 256];
  int have = 0;
  unsigned long lastData = millis(), lastDraw = 0;
  while (remaining != 0 && (c.connected() || c.available())) {
    int avail = c.available();
    if (avail <= 0) { if (millis() - lastData > 4000) break; delay(2); continue; }
    int got = c.read(buf + have, min(avail, (int)sizeof(buf) - have));
    if (got <= 0) continue;
    lastData = millis();
    have += got;
    if (remaining > 0) remaining -= got;
    int ns = have / 2;
    int16_t* s = (int16_t*)buf;
    float e = 0;
    for (int i = 0; i < ns; i++) {
      int32_t v = amplify(s[i]);
      e += abs(v);
      out[2 * i] = out[2 * i + 1] = v << 16;
    }
    i2s.write((uint8_t*)out, ns * 8);
    talkAmp = 0.6f * talkAmp + 0.4f * min(1.0f, (e / max(ns, 1)) / (5000.0f * SPEAKER_VOLUME / 100.0f));
    if (have & 1) { buf[0] = buf[have - 1]; have = 1; } else have = 0;
    if (millis() - lastDraw > 90) { animate(); drawFace(); lastDraw = millis(); }
  }
  playSilence(120);
  talking = false; talkAmp = 0;
  for (int i = 0; i < 12; i++) readMono(frameBuf, FRAME);   // drop mic data captured while speaking
  setFace(emotionFromName(emoStr), 4000);
  lastActivity = millis();
}

void conversation() {
  lastActivity = millis();
  setFace(LISTENING); drawFace();
  WiFiClient c;
  c.setNoDelay(true);
  if (!c.connect(SERVER_HOST, SERVER_PORT, 3000)) {
    Serial.println("Cannot reach server: check SERVER_HOST, same WiFi, firewall");
    setFace(CRY, 3000);
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    return;
  }
  c.print(String("POST /talk HTTP/1.1\r\nHost: ") + SERVER_HOST +
          "\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  for (int k = 0; k < PRE_FRAMES; k++) sendChunk(c, preRing[(preIdx + k) % PRE_FRAMES], FRAME);

  static int16_t batch[FRAME * 5];
  int bn = 0, frames = PRE_FRAMES, quiet = 0;
  unsigned long lastDraw = 0;
  while (frames < MAX_FRAMES && quiet < SILENCE_FRAMES) {
    size_t n = readMono(frameBuf, FRAME);
    if (rmsOf(frameBuf, n) > threshold) quiet = 0; else quiet++;
    memcpy(batch + bn * FRAME, frameBuf, FRAME * 2);
    if (++bn == 5) { sendChunk(c, batch, FRAME * 5); bn = 0; }
    frames++;
    if (millis() - lastDraw > 200) { animate(); drawFace(); lastDraw = millis(); }
  }
  if (bn) sendChunk(c, batch, FRAME * bn);
  c.print("0\r\n\r\n");
  Serial.printf("Sent %d ms of audio\n", frames * 20);

  setFace(THINKING);
  receiveAndPlay(c);
  c.stop();
}

void setServerMode(int m) {
  HTTPClient http;
  http.begin(SERVER_HOST, SERVER_PORT, String("/mode?m=") + m);
  int code = http.GET();
  Serial.printf("mode %d -> HTTP %d\n", m, code);
  http.end();
}

// Tells the dashboard on your Mac how the device is doing (mic level, face, WiFi signal, memory),
// and receives its commands in the reply: show a face, beep the speaker. Open http://YOUR-MAC-IP:8000/dashboard
unsigned long nextReport = 0;
void report() {
  unsigned long now = millis();
  if (now < nextReport || WiFi.status() != WL_CONNECTED) return;
  WiFiClient c;
  if (!c.connect(SERVER_HOST, SERVER_PORT, 300)) { nextReport = now + 10000; return; }   // server not up: try again in 10 s
  c.print(String("GET /api/device?lvl=") + lastLevel + "&thr=" + threshold + "&rssi=" + WiFi.RSSI() + "&emo=" + EMO_NAMES[emo] +
          "&heap=" + ESP.getFreeHeap() + "&up=" + (now / 1000) + "&vol=" + SPEAKER_VOLUME + "&gain=" + MIC_SHIFT +
          " HTTP/1.1\r\nHost: " + SERVER_HOST + "\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (!c.available() && c.connected() && millis() - t0 < 500) delay(1);
  c.readStringUntil('\n');                                              // status line
  while (true) { String l = c.readStringUntil('\n'); if (l.length() <= 1) break; }   // headers
  String body = c.readStringUntil('\n');                                // e.g. "force=wink beep=0"
  c.stop();
  nextReport = millis() + 1500;
  int f = body.indexOf("force=");
  if (f >= 0) {
    String name = body.substring(f + 6);
    int sp = name.indexOf(' ');
    if (sp >= 0) name = name.substring(0, sp);
    name.trim();
    for (int i = 0; i < EMO_COUNT; i++) if (name == EMO_NAMES[i]) { setFace((Emotion)i, 3500); break; }
  }
  if (body.indexOf("beep=1") >= 0) { playTone(660, 120); playTone(880, 160); playSilence(60); }
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
    else if (ch >= '1' && ch <= '4' && RUN_MODE == 0) setServerMode(ch - '1');
    if (ch == 'h' || ch == 'l' || ch == 't' || ch == 'n' || ch == 'p') Serial.printf("face: %s\n", EMO_NAMES[emo]);
  }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed: check power, SDA/SCL, controller and I2C address.");
    while (true) delay(1000);
  }
  randomSeed(micros());
  setFace(STARRY, 1500); drawFace();

#if RUN_MODE == 1
  Serial.println("FACE TEST: faces cycle automatically. n/p = next/previous, k = talking mouth.");
#else
#if RUN_MODE == 0
  message("Connecting WiFi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\nESP32 IP: " + WiFi.localIP().toString());
#endif
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DIN, PIN_MIC_SD);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S init failed");
    message("I2S failed", "check wiring");
    while (true) delay(1000);
  }
#if RUN_MODE == 0
  calibrate();
#elif RUN_MODE == 2
  Serial.println("MIC TEST: talk or clap, the bar should grow.");
#else
  Serial.println("SPEAKER TEST: you should hear a beep every 2 seconds.");
#endif
#endif
  lastActivity = millis();
  setFace(HAPPY);
}

void loop() {
  handleSerial();
  animate();
  static unsigned long lastDraw = 0, lastCycle = 0;

#if RUN_MODE == 1
  if (millis() - lastCycle > 2500 && !mouthDemo) { lastCycle = millis(); setFace((Emotion)((emo + 1) % EMO_COUNT)); Serial.printf("face: %s\n", EMO_NAMES[emo]); }
  if (mouthDemo) talkAmp = (sin(millis() / 90.0) + 1) / 2;
  drawFace();
  delay(50);
#elif RUN_MODE == 2
  micMeter();
#elif RUN_MODE == 3
  speakerTest();
#else
  size_t n = readMono(frameBuf, FRAME);
  memcpy(preRing[preIdx], frameBuf, FRAME * 2);
  preIdx = (preIdx + 1) % PRE_FRAMES;

  static int loud = 0;
  lastLevel = rmsOf(frameBuf, n);
  if (lastLevel > threshold) loud++; else loud = 0;

  if (millis() - lastDraw > 100) {
    lastDraw = millis();
    if (holdUntil && millis() > holdUntil) { holdUntil = 0; setFace(HAPPY); lastActivity = millis(); }
    if (!holdUntil && emo != SLEEPY && millis() - lastActivity > 60000) setFace(SLEEPY);
    drawFace();
  }
  if (loud == 0) report();
  if (loud >= 3) { loud = 0; conversation(); }
#endif
}
