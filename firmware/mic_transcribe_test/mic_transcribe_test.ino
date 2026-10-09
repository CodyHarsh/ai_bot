/*
  MICROPHONE TEST THAT PRINTS WHAT YOU SAID  (microphone only: no amp, no OLED needed)

  It records 5 seconds from the INMP441, sends them to your Mac server, and prints the words it understood
  in the Serial Monitor (115200). The server also keeps the recording, so you can LISTEN to what the mic heard:
      http://YOUR-MAC-IP:8000/api/last.wav

  Wiring:  INMP441  VDD -> 3V3   GND -> GND   L/R -> GND
                    SCK -> GPIO14   WS -> GPIO25   SD -> GPIO33
  Server:  python server.py is running on your Mac, same WiFi (2.4 GHz).

  Keys in the Serial Monitor:   r  record 5 s and print the words      l  live level meter (10 s)
                                + / -  mic gain       s  show settings       m  menu
*/
#include <ESP_I2S.h>
#include <WiFi.h>

// ---- settings -----------------------------------------------------------------------------
#define WIFI_SSID    "YOUR_WIFI_NAME"
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"
#define SERVER_HOST  "192.168.1.50"   // your Mac's CURRENT IP (the server prints it when it starts)
#define SERVER_PORT  8000
#define RECORD_SECONDS 5

#define PIN_BCLK 14                    // mic SCK
#define PIN_WS   25                    // mic WS
#define PIN_DIN  33                    // mic SD
#define PIN_LED  2
int micShift = 14;                     // 14 = normal, 13 = 2x louder, 12 = 4x louder. Keys + and - change it.
// ---------------------------------------------------------------------------------------------

#define SAMPLE_RATE 16000
#define FRAME 320                      // 20 ms
#define LOGI(tag, ...) do { Serial.printf("[%8lu] %-5s ", millis(), tag); Serial.printf(__VA_ARGS__); Serial.println(); } while (0)

I2SClass i2s;
int micChannel = 0;                    // 0 = left slot (L/R pin on GND), 1 = right slot (L/R on 3V3)

size_t readMono(int16_t* out, size_t n) {
  static int32_t tmp[2 * 160];
  size_t done = 0;
  while (done < n) {
    size_t want = min((size_t)160, n - done);
    size_t got = i2s.readBytes((char*)tmp, want * 8) / 8;
    if (!got) break;
    for (size_t i = 0; i < got; i++) out[done + i] = (int16_t)constrain(tmp[2 * i + micChannel] >> micShift, -32768, 32767);
    done += got;
  }
  return done;
}

int peakOf(const int16_t* s, size_t n) { int p = 0; for (size_t i = 0; i < n; i++) { int a = s[i] < 0 ? -s[i] : s[i]; if (a > p) p = a; } return p; }
int rmsOf(const int16_t* s, size_t n) {
  if (!n) return 0;
  long long sum = 0;
  for (size_t i = 0; i < n; i++) sum += (long long)s[i] * s[i];
  return (int)sqrt((double)sum / n);
}

void sendChunk(WiFiClient& c, const int16_t* data, size_t samples) {
  c.printf("%x\r\n", (unsigned)(samples * 2));
  c.write((const uint8_t*)data, samples * 2);
  c.print("\r\n");
}

String bar(int level) {                // text level meter, 0..32767 on a log-ish scale
  int n = level <= 0 ? 0 : constrain((int)(log10((double)level) * 6.0 - 6), 0, 24);
  String s = "["; for (int i = 0; i < 24; i++) s += i < n ? '#' : '.'; s += "]";
  return s;
}

// Tells which of the two I2S slots carries the microphone (the one that wiggles), so L/R wired to 3V3 also works.
void pickChannel() {
  static int32_t tmp[2 * 160];
  double mean[2] = {0, 0}; long n = 0;
  for (int k = 0; k < 8; k++) {
    size_t got = i2s.readBytes((char*)tmp, sizeof(tmp)) / 8;
    for (size_t i = 0; i < got; i++) for (int ch = 0; ch < 2; ch++) { mean[ch] += tmp[2 * i + ch] >> 8; n++; }
  }
  n /= 2; if (!n) { LOGI("MIC", "no data from the I2S port at all"); return; }
  mean[0] /= n; mean[1] /= n;
  long long dev[2] = {0, 0}; long m = 0;
  for (int k = 0; k < 8; k++) {
    size_t got = i2s.readBytes((char*)tmp, sizeof(tmp)) / 8;
    for (size_t i = 0; i < got; i++) { for (int ch = 0; ch < 2; ch++) { double d = (tmp[2 * i + ch] >> 8) - mean[ch]; dev[ch] += (long long)(d * d / 256); } m++; }
  }
  micChannel = dev[1] > dev[0] * 4 ? 1 : 0;
  LOGI("MIC", "signal in left slot %lld, right slot %lld -> using the %s slot", dev[0], dev[1], micChannel ? "RIGHT (L/R pin is on 3V3?)" : "LEFT");
}

bool startI2S() {
  i2s.setPins(PIN_BCLK, PIN_WS, -1, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    LOGI("I2S", "FAILED to start the microphone port");
    return false;
  }
  LOGI("I2S", "microphone started: 16 kHz, SCK=GPIO%d WS=GPIO%d SD=GPIO%d", PIN_BCLK, PIN_WS, PIN_DIN);
  return true;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  LOGI("WIFI", "connecting to \"%s\"...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  if (WiFi.status() == WL_CONNECTED) LOGI("WIFI", "connected, my IP %s, signal %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else LOGI("WIFI", "NOT connected after 15 s. Check WIFI_SSID / WIFI_PASS (must be a 2.4 GHz network)");
}

void liveMeter() {
  LOGI("METER", "10 seconds of live level. Clap or talk: the bar must jump. Silence should be nearly empty");
  unsigned long t0 = millis(); int16_t f[FRAME];
  int hi = 0;
  while (millis() - t0 < 10000) {
    size_t n = readMono(f, FRAME);
    int pk = peakOf(f, n), r = rmsOf(f, n);
    hi = max(hi, pk);
    static unsigned long last = 0;
    if (millis() - last > 150) { last = millis(); Serial.printf("  %s level %5d  peak %5d\n", bar(r).c_str(), r, pk); }
  }
  LOGI("METER", "loudest peak %d of 32767 (%d%%)", hi, hi * 100 / 32767);
}

void recordAndTranscribe() {
  connectWifi();
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient c;
  c.setNoDelay(true);
  unsigned long t0 = millis();
  if (!c.connect(SERVER_HOST, SERVER_PORT, 3000)) {
    LOGI("NET", "cannot connect to %s:%d after %lu ms. Check SERVER_HOST (your Mac's CURRENT IP), same WiFi, firewall, server running", SERVER_HOST, SERVER_PORT, millis() - t0);
    return;
  }
  c.print(String("POST /api/transcribe HTTP/1.1\r\nHost: ") + SERVER_HOST +
          "\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  Serial.println();
  Serial.println("  >>> SPEAK NOW (5 seconds) <<<");
  digitalWrite(PIN_LED, HIGH);
  int16_t f[FRAME];
  int total = RECORD_SECONDS * SAMPLE_RATE / FRAME, peakAll = 0, zeroFrames = 0;
  long long sumSq = 0; long cnt = 0;
  unsigned long lastPrint = 0;
  for (int i = 0; i < total; i++) {
    size_t n = readMono(f, FRAME);
    if (n < FRAME) { LOGI("MIC", "the microphone port stopped delivering data (got %u of %d samples)", (unsigned)n, FRAME); break; }
    int pk = peakOf(f, n), r = rmsOf(f, n);
    peakAll = max(peakAll, pk);
    if (pk == 0) zeroFrames++;
    for (size_t k = 0; k < n; k++) sumSq += (long long)f[k] * f[k];
    cnt += n;
    sendChunk(c, f, n);
    if (millis() - lastPrint > 250) { lastPrint = millis(); Serial.printf("  %s %d.%ds\n", bar(r).c_str(), (i * 20) / 1000, ((i * 20) % 1000) / 100); }
  }
  c.print("0\r\n\r\n");
  digitalWrite(PIN_LED, LOW);
  int avg = cnt ? (int)sqrt((double)sumSq / cnt) : 0;
  LOGI("MIC", "recorded %d s: average level %d, loudest sample %d (%d%% of full scale)", RECORD_SECONDS, avg, peakAll, peakAll * 100 / 32767);
  if (peakAll == 0 || zeroFrames > total * 9 / 10) LOGI("MIC", "ALL ZEROS: no signal at all. Check mic 3V3 + GND, the SD wire on GPIO33 and SCK/WS wires");
  else if (peakAll >= 32000) LOGI("MIC", "CLIPPING: too loud / saturated. Press - to lower the gain. If it is saturated even in a quiet room, SCK and WS are probably swapped or L/R is floating");
  else if (peakAll < 1500) LOGI("MIC", "very quiet. Speak close to the mic or press + for more gain");
  LOGI("NET", "sent, waiting for the words (a few seconds)...");

  unsigned long w0 = millis();
  while (!c.available() && c.connected() && millis() - w0 < 30000) delay(10);
  String status = c.readStringUntil('\n');
  int code = status.substring(9, 12).toInt();
  while (true) { String l = c.readStringUntil('\n'); if (l.length() <= 1) break; }
  String body = c.readString();
  c.stop();
  int p = body.indexOf("\"text\"");
  String words = "";
  if (p >= 0) { int a = body.indexOf('"', body.indexOf(':', p) + 1), b = body.indexOf('"', a + 1); if (a >= 0 && b > a) words = body.substring(a + 1, b); }
  Serial.println();
  if (code != 200) { LOGI("NET", "server answered HTTP %d. Read the server terminal. (404 = you need the new server.py with /api/transcribe)", code); return; }
  if (words.length()) Serial.printf("  >>> YOU SAID: \"%s\"\n", words.c_str());
  else Serial.println("  >>> NOTHING UNDERSTOOD (no words in that recording)");
  Serial.printf("  Listen to what the mic captured:  http://%s:%d/api/last.wav\n\n", SERVER_HOST, SERVER_PORT);
  if (peakAll > 0 && peakAll < 4000 && micShift > 10) { micShift--; LOGI("MIC", "quiet voice: raised the gain (shift %d)", micShift); }
  if (peakAll >= 32000 && micShift < 16) { micShift++; LOGI("MIC", "clipping: lowered the gain (shift %d)", micShift); }
}

void menu() {
  Serial.println();
  Serial.printf("keys:  r record %d s and print the words   l live level meter   + / - gain (shift %d)   s settings   m menu\n", RECORD_SECONDS, micShift);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  delay(800);
  Serial.println();
  Serial.println("=== MICROPHONE TEST: prints what you say ===");
  if (!startI2S()) { while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(100); } }
  delay(300);
  pickChannel();
  connectWifi();
  menu();
  Serial.println("Recording starts in 3 seconds...");
  delay(3000);
  recordAndTranscribe();
  menu();
}

void loop() {
  if (!Serial.available()) { delay(10); return; }
  char k = Serial.read();
  if (k == 'r') recordAndTranscribe();
  else if (k == 'l') liveMeter();
  else if (k == '+' || k == '=') { micShift = max(micShift - 1, 8); LOGI("GAIN", "louder: shift %d", micShift); }
  else if (k == '-') { micShift = min(micShift + 1, 18); LOGI("GAIN", "quieter: shift %d", micShift); }
  else if (k == 's') LOGI("SET", "server %s:%d, gain shift %d, slot %s, WiFi %s", SERVER_HOST, SERVER_PORT, micShift, micChannel ? "right" : "left", WiFi.status() == WL_CONNECTED ? "connected" : "DOWN");
  else if (k == 'm') menu();
}
