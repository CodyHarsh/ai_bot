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
import time
import wave

import anthropic
import edge_tts
import httpx
import numpy as np
import uvicorn
from faster_whisper import WhisperModel
from fastapi import FastAPI, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse

# The brain is a chain of providers tried in order. If one fails (no key, rate limit, no internet, no credit),
# the next one answers, and the built-in offline replies are the last resort.
#   default chain:  gemini -> groq -> ollama -> anthropic
#   change it:      export LLM_CHAIN=gemini,ollama          (or one name, like LLM_CHAIN=groq)
# Keys (set only the ones you have):
#   GEMINI_API_KEY (free, aistudio.google.com)   GROQ_API_KEY (free, console.groq.com)
#   ANTHROPIC_API_KEY (paid)                     ollama needs no key (ollama.com, then: ollama pull llama3.2)
# Model overrides: GEMINI_MODEL, GROQ_MODEL, OLLAMA_MODEL, ANTHROPIC_MODEL.
# Any other OpenAI-style service: add "openai" to the chain with LLM_BASE_URL, LLM_API_KEY and LLM_MODEL.
CHAIN = [x.strip().lower() for x in os.getenv("LLM_CHAIN", os.getenv("LLM_PROVIDER", "gemini,groq,ollama,anthropic")).split(",") if x.strip()]
PRESETS = {  # base url, key env var(s), default model, model env var
    "gemini": ("https://generativelanguage.googleapis.com/v1beta/openai", ("GEMINI_API_KEY", "GOOGLE_API_KEY"), "gemini-2.5-flash", "GEMINI_MODEL"),
    "groq": ("https://api.groq.com/openai/v1", ("GROQ_API_KEY",), "llama-3.1-8b-instant", "GROQ_MODEL"),
    "ollama": ("http://localhost:11434/v1", (), "llama3.2", "OLLAMA_MODEL"),
    "openai": (os.getenv("LLM_BASE_URL", ""), ("LLM_API_KEY",), os.getenv("LLM_MODEL", ""), "LLM_MODEL"),
}
CLAUDE_MODEL = os.getenv("ANTHROPIC_MODEL", "claude-haiku-5-5")
COOLDOWN_SECONDS = 90        # after a provider fails, skip it for this long so replies stay fast
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

state = {"mode": 0, "history": [], "log": [], "warned": False, "provider": "-"}
cooldown = {}                # provider name -> time when it may be tried again
OFFLINE = os.getenv("LLM_MODE", "").lower() == "offline"   # set LLM_MODE=offline to skip the LLM completely

print(f"Loading speech-to-text model '{WHISPER_MODEL}' (first run downloads it)...")
stt = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
llm = None
if "anthropic" in CHAIN:
    try:
        llm = anthropic.Anthropic()  # reads ANTHROPIC_API_KEY
    except Exception:
        llm = None


def provider_key(name: str) -> str:
    keys = PRESETS.get(name, ("", (), "", ""))[1]
    return next((os.environ[k] for k in keys if os.getenv(k)), "")


def available(name: str) -> bool:
    if name == "anthropic":
        return llm is not None
    if name == "ollama":
        return True
    return name in PRESETS and bool(PRESETS[name][0]) and bool(provider_key(name))


ACTIVE = [n for n in CHAIN if available(n)]
print("LLM chain:", " -> ".join(CHAIN), "| ready:", ", ".join(ACTIVE) or "none", "| last resort: offline replies")
if OFFLINE:
    print("LLM_MODE=offline: the LLM is skipped.")
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


OFFLINE_RULES = [
    (r"angry|mad|grumpy", "pout", "Hmph! I am not grumpy. Okay, maybe a little."),
    (r"rough|bad day|sad|lonely|cry", "sad", "I am here with you. Do you want to tell me more?"),
    (r"anxious|worried|stress|scared", "shy", "Breathe in slowly. I will stay right here."),
    (r"job|won|passed|promotion|good news", "excited", "No way! That is amazing! I am so proud of you!"),
    (r"love|thank", "love", "Aww, you are my favourite human in the whole world!"),
    (r"goodnight|good night|tired|sleep", "sleepy", "Sweet dreams. Lights out in three, two, one."),
    (r"morning", "happy", "Good morning! Let us make today a good one."),
    (r"focus|distract", "thinking", "Try twenty five minutes of focus, then a five minute break."),
    (r"study|tip|exam", "thinking", "Explain it out loud in one sentence. The gaps will show."),
    (r"joke|funny", "silly", "Why did the robot go to school? To improve its byte!"),
    (r"who are you|your name", "wink", "I am Pip, a tiny robot with big feelings."),
    (r"fact", "surprised", "Octopuses have three hearts. Is that not wild?"),
    (r"hello|hi pip|hey", "happy", "Hi hi! I am so happy you are here!"),
]
OFFLINE_FALLBACK = [("happy", "Ask me anything. I am all ears!"), ("thinking", "Good question. Let us break it into small parts."),
                    ("love", "Tell me more. I am listening."), ("sleepy", "Mmm. Soft thoughts only now.")]


def offline_reply(text: str) -> dict:
    """Simple built-in replies, used when the LLM is unavailable."""
    t = text.lower()
    for pattern, emotion, reply in OFFLINE_RULES:
        if re.search(pattern, t):
            return {"emotion": emotion, "text": reply}
    emotion, reply = OFFLINE_FALLBACK[state["mode"]]
    return {"emotion": emotion, "text": reply}


def call_one(name: str, system: str, messages: list) -> str:
    if name == "anthropic":
        resp = llm.messages.create(model=CLAUDE_MODEL, max_tokens=200, system=system, messages=messages, timeout=20)
        return resp.content[0].text
    base, _, default_model, model_env = PRESETS[name]
    model = os.getenv(model_env) or default_model
    r = httpx.post(
        base.rstrip("/") + "/chat/completions",
        headers={"Authorization": "Bearer " + (provider_key(name) or "none")},
        json={"model": model, "messages": [{"role": "system", "content": system}] + messages,
              "max_tokens": 200, "temperature": 0.8},
        timeout=60 if name == "ollama" else 20,
    )
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"]


def call_llm(system: str, messages: list):
    """Try each provider in the chain. Returns (reply text, provider name) or raises if all fail."""
    errors = []
    for name in ACTIVE:
        if cooldown.get(name, 0) > time.time():
            continue
        try:
            return call_one(name, system, messages), name
        except Exception as e:  # any failure moves on to the next provider
            cooldown[name] = time.time() + COOLDOWN_SECONDS
            errors.append(f"{name}: {getattr(e, 'message', e)}")
            print(f"[{name}] failed, trying the next one: {getattr(e, 'message', e)}")
    raise RuntimeError("; ".join(errors) or "no provider available")


def think(text: str) -> dict:
    if OFFLINE or not ACTIVE:
        state["provider"] = "offline"
        return {**offline_reply(text), "provider": "offline"}
    messages = state["history"][-8:] + [{"role": "user", "content": text}]
    try:
        raw, provider = call_llm(MODE_PROMPTS[state["mode"]] + RULES, messages)
    except Exception as e:  # every provider failed: keep talking with built-in replies
        print("All LLM providers failed, using offline reply:", e)
        state["provider"] = "offline"
        reply = {**offline_reply(text), "provider": "offline"}
        if not state["warned"]:
            state["warned"] = True
            reply["text"] = "My smart brain is offline right now, so I will keep it simple. " + reply["text"]
        return reply
    state["provider"] = provider
    state["warned"] = False
    try:
        data = json.loads(raw[raw.index("{"): raw.rindex("}") + 1])
        reply = {"emotion": str(data.get("emotion", "happy")).lower(), "text": str(data.get("text", "")).strip(), "provider": provider}
    except Exception:
        reply = {"emotion": "happy", "text": raw.strip()[:160], "provider": provider}
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
    print(f"[{MODES[state['mode']]} · {reply.get('provider', '-')}] you: {user!r} -> {reply['emotion']}: {reply['text']!r}")


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
        "X-Provider": reply.get("provider", "-"),
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
<form onsubmit="send(event)"><input id=q style="width:60%;padding:8px" placeholder="Type to Pip..."> <button>Send</button> <button type=button id=mic onclick="listen()">&#127908; Talk</button></form>
<p><label><input type=checkbox id=conv> Keep listening after Pip answers</label> <span id=st style="color:#888"></span></p>
<div id=log></div>
<script>
async function load(){const s=await (await fetch('/api/state')).json();
 m.innerHTML=s.modes.map(x=>'<option>'+x+'</option>').join('');m.selectedIndex=s.mode;
 log.innerHTML=s.log.slice().reverse().map(e=>'<p><b>You:</b> '+e.user+'<br><b>Pip ('+e.emotion+'):</b> '+e.bot+'</p>').join('')}
async function ask(t){if(!t.trim())return;st.textContent='thinking...';
 const r=await (await fetch('/api/chat',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({text:t})})).json();
 const a=new Audio('data:audio/wav;base64,'+r.audio);st.textContent='Pip ('+r.emotion+', via '+(r.provider||'-')+'): '+r.text;
 a.onended=()=>{if(conv.checked)listen()};a.play();load()}
function send(e){e.preventDefault();const t=q.value;q.value='';ask(t)}
function listen(){const R=window.SpeechRecognition||window.webkitSpeechRecognition;
 if(!R){st.textContent='Use Chrome or Safari for the Talk button.';return}
 const r=new R();r.lang='en-US';r.onstart=()=>{st.textContent='listening...'};
 r.onresult=e=>ask(e.results[0][0].transcript);r.onerror=e=>{st.textContent='mic: '+e.error};r.start()}
load();setInterval(load,4000);
</script></body>"""


@app.get("/", response_class=HTMLResponse)
async def index():
    return PAGE


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
