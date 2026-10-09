"""
Pip brain server. Runs on your laptop; the ESP32 talks to it over WiFi.

  ESP32 mic audio (16 kHz mono 16-bit PCM)
    -> speech-to-text (faster-whisper, runs locally)
    -> LLM (Claude)  -> {"emotion": "...", "text": "..."}
    -> text-to-speech (edge-tts, cute child-like voice)
    -> raw 16 kHz mono 16-bit PCM back to the ESP32, emotion in the X-Emotion header

Run:  python server.py        (needs ANTHROPIC_API_KEY and ffmpeg, see the Build steps tab)
Test without hardware: open http://localhost:8000 and type to the bot.
"""
import asyncio
import base64
import io
import json
import os
import re
import wave

import anthropic
import edge_tts
import numpy as np
import uvicorn
from faster_whisper import WhisperModel
from fastapi import FastAPI, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse

LLM_MODEL = os.getenv("LLM_MODEL", "claude-haiku-5-5")
VOICE = os.getenv("TTS_VOICE", "en-US-AnaNeural")      # cute child-like voice
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "base.en")  # tiny.en is faster, small.en is more accurate
SAMPLE_RATE = 16000

EMOTIONS = ["happy", "excited", "love", "wink", "joy", "silly", "sleepy", "sad",
            "cry", "pout", "surprised", "thinking", "shy", "starry", "curious"]

MODES = ["Buddy", "Study", "Mood", "Sleep"]
MODE_PROMPTS = [
    "You are Pip, a cheerful, curious little desk-buddy robot. Chat, joke, and share fun facts.",
    "You are Pip in Study mode: a calm, encouraging study coach. Give one short, practical tip or a clear answer.",
    "You are Pip in Mood mode: an empathetic pet. React warmly to the user's feelings and ask a gentle follow-up.",
    "You are Pip in Sleep mode: a sleepy bedtime bot. Speak softly, keep it very short, help the user wind down.",
]
RULES = (
    " You live inside a tiny robot with an OLED face and a speaker, so your words are spoken aloud. "
    "Reply with ONLY a JSON object: {\"emotion\": \"<one of: " + ", ".join(EMOTIONS) + ">\", "
    "\"text\": \"<what you say>\"}. The text must be 1 or 2 short sentences, under 160 characters, "
    "plain words with no emojis, no markdown. Pick the emotion that matches your reply."
)

state = {"mode": 0, "history": [], "log": []}

print(f"Loading speech-to-text model '{WHISPER_MODEL}' (first run downloads it)...")
stt = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
llm = anthropic.Anthropic()  # reads ANTHROPIC_API_KEY
app = FastAPI()


def transcribe(pcm: bytes) -> str:
    audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    if peak > 0:
        audio = audio * min(8.0, 0.6 / peak)  # lift quiet mic audio
    segments, _ = stt.transcribe(audio, language="en", beam_size=1, vad_filter=True)
    return " ".join(s.text.strip() for s in segments).strip()


def maybe_switch_mode(text: str) -> bool:
    m = re.search(r"\b(buddy|study|mood|sleep) mode\b", text.lower())
    if m:
        state["mode"] = [x.lower() for x in MODES].index(m.group(1))
        return True
    return False


def think(text: str) -> dict:
    messages = state["history"][-8:] + [{"role": "user", "content": text}]
    resp = llm.messages.create(
        model=LLM_MODEL,
        max_tokens=200,
        system=MODE_PROMPTS[state["mode"]] + RULES,
        messages=messages,
    )
    raw = resp.content[0].text
    try:
        data = json.loads(raw[raw.index("{"): raw.rindex("}") + 1])
        reply = {"emotion": str(data.get("emotion", "happy")).lower(), "text": str(data.get("text", "")).strip()}
    except Exception:
        reply = {"emotion": "happy", "text": raw.strip()[:160]}
    if reply["emotion"] not in EMOTIONS:
        reply["emotion"] = "happy"
    if not reply["text"]:
        reply["text"] = "Hmm, I got a little lost."
    state["history"] += [{"role": "user", "content": text}, {"role": "assistant", "content": raw}]
    state["history"] = state["history"][-16:]
    return reply


async def speak(text: str) -> bytes:
    """text -> mp3 (edge-tts) -> raw 16 kHz mono s16le PCM (ffmpeg)."""
    mp3 = bytearray()
    async for chunk in edge_tts.Communicate(text, VOICE, rate="+8%", pitch="+25Hz").stream():
        if chunk["type"] == "audio":
            mp3 += chunk["data"]
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-loglevel", "error", "-i", "pipe:0", "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1", "pipe:1",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
    )
    pcm, _ = await proc.communicate(bytes(mp3))
    return pcm


def to_wav(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SAMPLE_RATE); w.writeframes(pcm)
    return buf.getvalue()


def record(user: str, reply: dict):
    state["log"] = (state["log"] + [{"user": user, "emotion": reply["emotion"], "bot": reply["text"]}])[-30:]
    print(f"[{MODES[state['mode']]}] you: {user!r} -> {reply['emotion']}: {reply['text']!r}")


async def respond(text: str) -> dict:
    switched = maybe_switch_mode(text)
    if switched:
        reply = {"emotion": "excited", "text": f"Okay! {MODES[state['mode']]} mode on."}
    else:
        reply = await asyncio.to_thread(think, text)
    record(text, reply)
    return reply


@app.post("/talk")
async def talk(request: Request):
    pcm = await request.body()
    if len(pcm) < SAMPLE_RATE * 2 * 0.4:  # under 0.4 s: ignore
        return Response(status_code=204)
    text = await asyncio.to_thread(transcribe, pcm)
    print("heard:", repr(text))
    if len(text) < 2:
        return Response(status_code=204)
    reply = await respond(text)
    audio = await speak(reply["text"])
    headers = {
        "X-Emotion": reply["emotion"],
        "X-Text": reply["text"].encode("ascii", "ignore").decode()[:200],
        "X-Mode": MODES[state["mode"]],
    }
    return Response(audio, media_type="application/octet-stream", headers=headers)


@app.get("/mode")
async def set_mode(m: int = 0):
    state["mode"] = max(0, min(3, m))
    state["history"] = []
    return {"mode": MODES[state["mode"]]}


@app.post("/api/chat")  # used by the test page below, no ESP32 needed
async def chat(request: Request):
    body = await request.json()
    reply = await respond(body.get("text", ""))
    wav = to_wav(await speak(reply["text"]))
    return JSONResponse({**reply, "mode": MODES[state["mode"]], "audio": base64.b64encode(wav).decode()})


@app.get("/api/state")
async def get_state():
    return {"mode": state["mode"], "modes": MODES, "log": state["log"]}


PAGE = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Pip server</title>
<body style="font-family:system-ui;max-width:560px;margin:auto;padding:16px">
<h2>Pip server is running</h2>
<p>Mode: <select id=m onchange="fetch('/mode?m='+this.selectedIndex)"></select></p>
<form onsubmit="send(event)"><input id=q style="width:70%;padding:8px" placeholder="Type to Pip..."> <button>Send</button></form>
<div id=log></div>
<script>
async function load(){const s=await (await fetch('/api/state')).json();
 m.innerHTML=s.modes.map(x=>'<option>'+x+'</option>').join('');m.selectedIndex=s.mode;
 log.innerHTML=s.log.slice().reverse().map(e=>'<p><b>You:</b> '+e.user+'<br><b>Pip ('+e.emotion+'):</b> '+e.bot+'</p>').join('')}
async function send(e){e.preventDefault();const t=q.value;q.value='';
 const r=await (await fetch('/api/chat',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({text:t})})).json();
 new Audio('data:audio/wav;base64,'+r.audio).play();load()}
load();setInterval(load,4000);
</script></body>"""


@app.get("/", response_class=HTMLResponse)
async def index():
    return PAGE


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
