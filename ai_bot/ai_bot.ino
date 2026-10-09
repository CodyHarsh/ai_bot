/*
  AI Bot - ESP32 + SSD1306 OLED face + LLM (hackathon build)

  Libraries (Library Manager):
    - Adafruit SSD1306
    - Adafruit GFX Library
    - ArduinoJson (version 7.x)
  Board: "ESP32 Dev Module"

  Wiring:
    OLED  VCC->3V3  GND->GND  SDA->GPIO21  SCL->GPIO22
    BTN1 (next mode)  GPIO18 -> GND
    BTN2 (poke)       GPIO19 -> GND
    BTN3 (ask/demo)   GPIO23 -> GND
    Buzzer (+)        GPIO25, (-) GND
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- CONFIG: edit these ----------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* API_KEY   = "YOUR_ANTHROPIC_API_KEY";   // never commit a real key
const char* MODEL     = "claude-haiku-5-5";         // fast + cheap, good for a bot
// ----------------------------------------

#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C
#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BTN_MODE 18
#define PIN_BTN_POKE 19
#define PIN_BTN_ASK  23
#define PIN_BUZZER   25

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer server(80);

enum Emotion { HAPPY, SAD, ANGRY, SURPRISED, SLEEPY, LOVE, THINKING, WINK };
const char* EMO_NAMES[] = {"happy", "sad", "angry", "surprised", "sleepy", "love", "thinking", "wink"};

enum Mode { BUDDY, STUDY, MOOD, SLEEP };
const char* MODE_NAMES[] = {"Buddy", "Study", "Mood", "Sleep"};
const char* MODE_PROMPTS[] = {
  "You are a cheerful, curious desk buddy robot.",
  "You are a calm study coach. Give one short, practical tip or answer.",
  "You are an empathetic mood pet. React to the user's feelings warmly.",
  "You are a sleepy bedtime bot. Speak softly and help the user wind down."
};

Mode mode = BUDDY;
Emotion emotion = HAPPY;
String replyText = "";
unsigned long showTextUntil = 0;
unsigned long lastBlink = 0, blinkUntil = 0, modeBannerUntil = 0;
int lookX = 0;

// ---------- buzzer ----------
void beep(int freq, int ms) {
  ledcAttach(PIN_BUZZER, freq, 8);   // core 3.x API
  ledcWriteTone(PIN_BUZZER, freq);
  delay(ms);
  ledcWriteTone(PIN_BUZZER, 0);
}

void emotionSound(Emotion e) {
  switch (e) {
    case HAPPY:     beep(880, 80); beep(1175, 120); break;
    case SAD:       beep(440, 150); beep(330, 250); break;
    case ANGRY:     beep(200, 250); break;
    case SURPRISED: beep(1400, 100); break;
    case LOVE:      beep(660, 100); beep(880, 100); beep(1040, 150); break;
    default: break;
  }
}

// ---------- face drawing ----------
void heart(int x, int y) {
  display.fillCircle(x - 5, y, 6, WHITE);
  display.fillCircle(x + 5, y, 6, WHITE);
  display.fillTriangle(x - 11, y + 2, x + 11, y + 2, x, y + 14, WHITE);
}

void eyeBox(int cx, int cy, int h) {
  display.fillRoundRect(cx - 12, cy - h / 2, 24, h, 7, WHITE);
}

void drawFace(Emotion e, bool blink) {
  display.clearDisplay();
  int lx = 34 + lookX, rx = 94 + lookX, ey = 24;

  if (blink && e != SLEEPY && e != LOVE) {
    display.fillRoundRect(lx - 12, ey - 1, 24, 3, 1, WHITE);
    display.fillRoundRect(rx - 12, ey - 1, 24, 3, 1, WHITE);
  } else switch (e) {
    case HAPPY:
      display.fillCircle(lx, ey + 3, 11, WHITE); display.fillCircle(lx, ey + 10, 11, BLACK);
      display.fillCircle(rx, ey + 3, 11, WHITE); display.fillCircle(rx, ey + 10, 11, BLACK);
      break;
    case SAD:
      eyeBox(lx, ey, 26); eyeBox(rx, ey, 26);
      display.fillTriangle(lx - 14, ey - 14, lx + 14, ey - 14, lx - 14, ey - 1, BLACK);
      display.fillTriangle(rx - 14, ey - 14, rx + 14, ey - 14, rx + 14, ey - 1, BLACK);
      break;
    case ANGRY:
      eyeBox(lx, ey, 26); eyeBox(rx, ey, 26);
      display.fillTriangle(lx - 14, ey - 14, lx + 14, ey - 14, lx + 14, ey - 1, BLACK);
      display.fillTriangle(rx - 14, ey - 14, rx + 14, ey - 14, rx - 14, ey - 1, BLACK);
      break;
    case SURPRISED:
      display.fillCircle(lx, ey, 14, WHITE); display.fillCircle(rx, ey, 14, WHITE);
      display.fillCircle(lx, ey, 5, BLACK);  display.fillCircle(rx, ey, 5, BLACK);
      break;
    case SLEEPY:
      display.fillRoundRect(lx - 12, ey + 2, 24, 3, 1, WHITE);
      display.fillRoundRect(rx - 12, ey + 2, 24, 3, 1, WHITE);
      display.setTextSize(1); display.setTextColor(WHITE);
      display.setCursor(108, 4);  display.print("z");
      display.setCursor(114, 12); display.print("Z");
      break;
    case LOVE:
      heart(lx, ey - 2); heart(rx, ey - 2);
      break;
    case THINKING:
      eyeBox(lx, ey - 2, 22); eyeBox(rx, ey - 2, 22);
      display.fillCircle(lx + 4, ey - 8, 4, BLACK);
      display.fillCircle(rx + 4, ey - 8, 4, BLACK);
      break;
    case WINK:
      display.fillCircle(lx, ey + 3, 11, WHITE); display.fillCircle(lx, ey + 10, 11, BLACK);
      eyeBox(rx, ey, 26);
      break;
  }

  // mouth
  switch (e) {
    case HAPPY: case WINK: case LOVE:
      display.fillCircle(64, 46, 12, WHITE); display.fillCircle(64, 41, 12, BLACK); break;
    case SAD:
      display.fillCircle(64, 58, 12, WHITE); display.fillCircle(64, 63, 12, BLACK); break;
    case ANGRY:
      display.fillRect(52, 52, 24, 3, WHITE); break;
    case SURPRISED:
      display.drawCircle(64, 52, 6, WHITE); break;
    case THINKING:
      display.fillCircle(58, 52, 2, WHITE); display.fillCircle(66, 52, 2, WHITE);
      display.fillCircle(74, 52, 2, WHITE); break;
    case SLEEPY:
      display.drawFastHLine(58, 52, 12, WHITE); break;
  }

  // mode banner (top-left, shows for 1.5 s after a mode change)
  if (millis() < modeBannerUntil) {
    display.fillRect(0, 0, 60, 10, BLACK);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(0, 1); display.print(MODE_NAMES[mode]);
  }
  display.display();
}

void drawText(const String& t) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print(t);
  display.display();
}

// ---------- LLM ----------
bool askLLM(const String& question) {
  WiFiClientSecure client;
  client.setInsecure();                       // hackathon shortcut; pin the cert for production
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, "https://api.anthropic.com/v1/messages")) return false;
  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", API_KEY);
  http.addHeader("anthropic-version", "2023-06-01");

  String sys = String(MODE_PROMPTS[mode]) +
    " You live inside a tiny robot with a 128x64 OLED face. Reply ONLY with JSON: "
    "{\"emotion\":\"<happy|sad|angry|surprised|sleepy|love|thinking|wink>\",\"text\":\"<max 80 chars>\"}";

  JsonDocument req;
  req["model"] = MODEL;
  req["max_tokens"] = 150;
  req["system"] = sys;
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject m = msgs.add<JsonObject>();
  m["role"] = "user";
  m["content"] = question;
  String body; serializeJson(req, body);

  int code = http.POST(body);
  Serial.printf("LLM HTTP %d\n", code);
  if (code != 200) { http.end(); return false; }

  JsonDocument res;
  DeserializationError err = deserializeJson(res, http.getString());
  http.end();
  if (err) return false;

  String raw = res["content"][0]["text"] | "";
  int a = raw.indexOf('{'), b = raw.lastIndexOf('}');
  if (a < 0 || b < a) { replyText = raw.substring(0, 80); emotion = HAPPY; return true; }

  JsonDocument out;
  if (deserializeJson(out, raw.substring(a, b + 1))) return false;
  replyText = String((const char*)(out["text"] | "..."));
  String emo = out["emotion"] | "happy";
  emotion = HAPPY;
  for (int i = 0; i < 8; i++) if (emo == EMO_NAMES[i]) emotion = (Emotion)i;
  return true;
}

void respond(const String& q) {
  drawFace(THINKING, false);
  if (askLLM(q)) {
    drawFace(emotion, false);
    emotionSound(emotion);
    delay(1500);
    drawText(replyText);
    showTextUntil = millis() + 5000;
  } else {
    emotion = SAD; replyText = "Oops, network error";
    drawFace(SAD, false);
    showTextUntil = 0;
  }
}

// ---------- web UI (open http://<esp32-ip>/ on your phone) ----------
const char PAGE[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<body style="font-family:sans-serif;max-width:480px;margin:auto;padding:16px">
<h2>AI Bot</h2>
<select id=m><option>Buddy<option>Study<option>Mood<option>Sleep</select>
<input id=q style="width:100%;padding:10px;margin:8px 0" placeholder="Say something...">
<button onclick=ask()>Send</button> <button onclick=mic()>Mic</button>
<p id=a></p>
<script>
async function ask(){
  const q=document.getElementById('q').value, m=document.getElementById('m').selectedIndex;
  document.getElementById('a').textContent='thinking...';
  const r=await fetch('/ask?m='+m+'&q='+encodeURIComponent(q)); const j=await r.json();
  document.getElementById('a').textContent=j.emotion+': '+j.text;
}
function mic(){
  const R=window.SpeechRecognition||window.webkitSpeechRecognition; if(!R){alert('Use Chrome');return}
  const s=new R(); s.onresult=e=>{document.getElementById('q').value=e.results[0][0].transcript;ask()}; s.start();
}
</script></body>
)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleAsk() {
  int m = server.arg("m").toInt();
  if (m >= 0 && m < 4) mode = (Mode)m;
  respond(server.arg("q"));
  JsonDocument d;
  d["emotion"] = EMO_NAMES[emotion];
  d["text"] = replyText;
  String s; serializeJson(d, s);
  server.send(200, "application/json", s);
}

// ---------- setup / loop ----------
bool pressed(int pin) {
  if (digitalRead(pin) == LOW) { delay(30); while (digitalRead(pin) == LOW) delay(5); return true; }
  return false;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_BTN_POKE, INPUT_PULLUP);
  pinMode(PIN_BTN_ASK, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found - check wiring / address 0x3C");
    while (true) delay(1000);
  }
  drawText("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\nIP: " + WiFi.localIP().toString());
  drawText("Open in browser:\n" + WiFi.localIP().toString());
  delay(3000);
  server.on("/", handleRoot);
  server.on("/ask", handleAsk);
  server.begin();
  modeBannerUntil = millis() + 1500;
}

void loop() {
  server.handleClient();

  if (pressed(PIN_BTN_MODE)) {
    mode = (Mode)((mode + 1) % 4);
    emotion = (mode == SLEEP) ? SLEEPY : (mode == STUDY ? THINKING : HAPPY);
    modeBannerUntil = millis() + 1500;
    showTextUntil = 0;
    beep(1000, 40);
  }
  if (pressed(PIN_BTN_POKE)) {
    emotion = (Emotion)random(0, 8);
    emotionSound(emotion);
    showTextUntil = 0;
  }
  if (pressed(PIN_BTN_ASK)) {
    const char* demo[] = {"Tell me a fun fact", "Give me a study tip", "I had a rough day", "Say goodnight"};
    respond(demo[mode]);
  }

  if (millis() < showTextUntil) return;   // keep reply text on screen

  // idle animation: blink + look around
  unsigned long now = millis();
  if (now - lastBlink > 3200) { lastBlink = now; blinkUntil = now + 140; lookX = random(-6, 7); }
  drawFace(emotion, now < blinkUntil);
  delay(40);
}
