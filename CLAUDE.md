# Pip: ESP32 voice AI bot (project notes for a new Claude session)

Hackathon voice bot. You talk, an LLM answers aloud, a 128x64 OLED shows a cute face matching the tone.

## Hardware (ESP32 DevKitC, board "ESP32 Dev Module", esp32 core 3.x, macOS + Arduino IDE)
- INMP441 mic: VDD 3V3, GND, L/R GND, SCK GPIO14, WS GPIO25, SD GPIO33
- MAX98357A amp: VIN 5V, BCLK GPIO14, LRC GPIO25, DIN GPIO22 (shares BCLK/LRC with the mic). The board speaker does not work, so the voice plays on the laptop.
- SSD1306 OLED I2C: SCL GPIO19, SDA GPIO21, address 0x3C. Status LED GPIO2. BOOT button GPIO0 = push to talk.

## Pipeline
ESP32 records 16 kHz mono PCM -> chunked HTTP POST `/talk` -> `server/server.py` (FastAPI on the Mac):
speech to text (LiveKit Inference if LIVEKIT_* keys are set, else local faster-whisper base.en) -> LLM chain (Groq -> OpenAI -> Gemini -> built-in replies) -> edge-tts voice -> ffmpeg -> PCM.
The server plays the reply on the laptop with `afplay` (`PLAY_ON=laptop`), and replies to the board with headers `X-Emotion`, `X-Heard`, `X-Playback: laptop`. The board only animates the face.
Dashboard: http://localhost:8000/dashboard. Handy endpoints: `/api/laptop-test`, `/api/say.wav?text=hi`, `/api/last.wav` (last mic test recording), `/api/transcribe`.

## Files
- `firmware/ai_bot/ai_bot.ino`: the ONE full firmware. Config at the top: RUN_MODE (0 bot, 1 faces, 2 mic, 3 speaker, 4 diagnostic), TRIGGER_MODE (0 voice, 1 BOOT button, 2 both), VOICE_THRESHOLD, DEVICE_SPEAKER (0), MIC_SHIFT, WIFI_*, SERVER_HOST.
- `firmware/mic_transcribe_test/`: mic-only test, press `r`, prints what you said. This one works.
- `firmware/speaker_voice_test/`, `firmware/test_mic/`, `firmware/test_speaker/`: small hardware tests.
- `server/server.py`, `server/dashboard.html`, `server/.env.example` (copy to `.env`, never commit keys), `server/requirements.txt`.
- `site/`: the simulator web page. `python3 site/build.py` rebuilds `site/simulator.html` from the sources.

## Status
- Works: mic test with transcription, server, LLM chain, laptop voice, 16 emotion faces, dashboard.
- Push to talk: hold BOOT (or key `r`) -> OLED "SPEAK NOW" + beep from the laptop (`/api/ding`) -> record -> release. Speed: Whisper is warmed up at start, end-of-speech wait is 0.5 s (`SILENCE_FRAMES 25`).
- Open problem: in the full bot the idle mic level is bursty and as loud as speech (looks like OLED refresh or WiFi interference on the mic line), so voice-triggered recording is unreliable. The mic test sketch does not have this. Workarounds built: median-of-5 level, BOOT-button push to talk, key `r` (4 s), manual threshold (`VOICE_THRESHOLD`, keys `>` `<`), OLED refresh 4 Hz.
- Next ideas: confirm push to talk gives `>>> I HEARD YOU SAY`; hardware fixes (100 nF cap at mic VDD, short wires away from OLED); an optional voice-to-voice mode (Gemini Live or OpenAI Realtime) behind `PIPELINE=live` in the server, leaving the firmware unchanged.

## How to continue in a new chat
Paste this file (or run Claude Code in the repo, it is read automatically) and say where you are, e.g. "mic test works; testing push to talk in the main bot". Keep the latest `ai_bot.ino` and `server.py` from the branch `claude/peaceful-allen-18ncek` of CodyHarsh/ai_bot.

## Gotchas
- Arduino IDE: custom enums/structs must be declared above all functions (it auto-generates prototypes).
- Do not put API keys or the WiFi password in code or commits. Keys go in `server/.env` (git-ignored).
- Check firmware syntax without hardware: compile as C++ with stub headers for Arduino/ESP libs.
