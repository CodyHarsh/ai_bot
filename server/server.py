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
import collections
import io
import json
import logging
import os
import re
import shutil
import socket
import time
import wave

import anthropic
import edge_tts
import httpx
import numpy as np
import uvicorn
from faster_whisper import WhisperModel
from fastapi import FastAPI, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse


def load_env_file(path: str = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")):
    """Optional: read KEY=value lines from a .env file next to this script (keeps secrets out of the terminal)."""
    if not os.path.exists(path):
        return
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


load_env_file()

# ---------- logging ----------
# Every module logs with its own tag: boot, stt (hearing), llm (brain), tts (voice), net (requests), esp32 (the board).
# The ESP32 sends its own log lines here too, so the whole system is in one place: this terminal and the dashboard.
RING = collections.deque(maxlen=400)


class RingHandler(logging.Handler):
    def emit(self, record):
        RING.append({"t": time.strftime("%H:%M:%S", time.localtime(record.created)), "tag": record.name,
                     "level": record.levelname, "msg": record.getMessage()})


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-5s %(name)-6s %(message)s", datefmt="%H:%M:%S")
logging.getLogger().addHandler(RingHandler())
logging.getLogger("httpx").setLevel(logging.WARNING)   # hide the library's own request lines, our logs say what matters
log = logging.getLogger("boot")
log_stt, log_llm, log_tts, log_net, log_esp = (logging.getLogger(n) for n in ("stt", "llm", "tts", "net", "esp32"))

# The brain is a chain of providers tried in order. If one fails (no key, rate limit, no internet, no credit),
# the next one answers, and the built-in offline replies are the last resort.
#   default chain:  groq -> openai -> gemini   (then the built-in offline replies)
#   change it:      export LLM_CHAIN=groq,gemini,ollama,anthropic   (any of: groq, openai, gemini, ollama, anthropic, custom)
# Keys (set only the ones you have):
#   GROQ_API_KEY (free, console.groq.com)   OPENAI_API_KEY (ChatGPT, paid)   GEMINI_API_KEY (free, aistudio.google.com)
#   ANTHROPIC_API_KEY (Claude, paid) - only used if you add it to LLM_CHAIN
#   ollama needs no key (ollama.com, then: ollama pull llama3.2)
# Keys can go in the terminal (export ...) or in a .env file next to this script (see .env.example).
# Model overrides: GEMINI_MODEL, GROQ_MODEL, OLLAMA_MODEL, ANTHROPIC_MODEL, OPENAI_MODEL.
# Any other OpenAI-style service: add "custom" to the chain with LLM_BASE_URL, LLM_API_KEY and LLM_MODEL.
# The voice does not depend on the brain: it is always TTS_VOICE (edge-tts), whichever provider answered.
CHAIN = [x.strip().lower() for x in os.getenv("LLM_CHAIN", os.getenv("LLM_PROVIDER", "groq,openai,gemini")).split(",") if x.strip()]
PRESETS = {  # base url, key env var(s), default model, model env var
    "gemini": ("https://generativelanguage.googleapis.com/v1beta/openai", ("GEMINI_API_KEY", "GOOGLE_API_KEY"), "gemini-2.5-flash", "GEMINI_MODEL"),
    "groq": ("https://api.groq.com/openai/v1", ("GROQ_API_KEY",), "llama-3.1-8b-instant", "GROQ_MODEL"),
    "ollama": ("http://localhost:11434/v1", (), "llama3.2", "OLLAMA_MODEL"),
    "openai": ("https://api.openai.com/v1", ("OPENAI_API_KEY",), "gpt-4o-mini", "OPENAI_MODEL"),
    "custom": (os.getenv("LLM_BASE_URL", ""), ("LLM_API_KEY",), os.getenv("LLM_MODEL", ""), "LLM_MODEL"),
}
CLAUDE_MODEL = os.getenv("ANTHROPIC_MODEL", "claude-haiku-5-5")
COOLDOWN_SECONDS = 90        # after a provider fails, skip it for this long so replies stay fast
VOICE = os.getenv("TTS_VOICE", "en-US-AnaNeural")      # cute child-like voice
PLAY_ON = os.getenv("PLAY_ON", "laptop").lower()      # where the voice comes out: laptop (default), device (ESP32 speaker) or both
TTS_GAIN = os.getenv("TTS_GAIN", "2.0")                # voice loudness: 1.0 = normal, 2.0 = twice as loud (a limiter prevents clipping)
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
# ---------- tone of the user's message ----------
# A quick keyword check works with no LLM at all. The LLM also judges the tone and can correct it.
TONES = ["sad", "lonely", "angry", "anxious", "tired", "excited", "loving", "playful", "happy", "curious", "neutral"]
TONE_PATTERNS = [  # checked in this order, so feelings that need care win over cheerful words
    ("sad", r"sad|unhappy|depress|down today|crying|cry|hurt|heartbroken|miss (him|her|them)|bad day|rough day|worst day|terrible|awful|upset|disappoint|failed|failure|grief|not okay|not ok"),
    ("lonely", r"lonely|alone|nobody|no one (cares|likes|understands)|no friends"),
    ("angry", r"angry|mad at|furious|annoyed|annoying|hate|irritated|fed up|so unfair|sick of"),
    ("anxious", r"anxious|worried|worry|nervous|scared|afraid|stress|panic|overwhelmed|cannot sleep|can't sleep|exam tomorrow"),
    ("tired", r"tired|sleepy|exhausted|bedtime|goodnight|good night|yawn|so sleepy"),
    ("excited", r"excited|amazing|awesome|got the job|i passed|i won|promotion|yay|can't wait|best day|celebrate|great news|good news"),
    ("loving", r"love you|miss you|best friend|you are the best|appreciate you|thank you so much"),
    ("playful", r"joke|funny|lol|haha|silly|prank"),
    ("happy", r"happy|great|good morning|wonderful|glad|thanks|thank you|nice"),
    ("curious", r"what is|what's|why |how do|how does|who is|tell me|explain|\?"),
]
# How Pip shows it has understood: it mirrors the feeling with a caring face.
TONE_TO_EMOTION = {"sad": "sad", "lonely": "sad", "angry": "pout", "anxious": "shy", "tired": "sleepy", "excited": "excited",
                   "loving": "love", "playful": "silly", "happy": "happy", "curious": "curious", "neutral": "happy"}
NEGATIVE_TONES = {"sad", "lonely", "angry", "anxious"}
CHEERFUL_FACES = {"happy", "excited", "wink", "joy", "silly", "starry", "love"}   # wrong face for someone who is hurting
VERY_SAD = r"crying|heartbroken|devastated|breaking down|want to cry|i cried"


def found(pattern: str, t: str) -> bool:
    """True if the pattern matches and is not negated ("I am not sad" does not count as sad)."""
    for m in re.finditer(pattern, t):
        if not re.search(r"(not|never|no|n't) $", t[max(0, m.start() - 6): m.start()]):
            return True
    return False


def detect_tone(text: str) -> str:
    t = text.lower()
    for tone, pattern in TONE_PATTERNS:
        if found(pattern, t):
            return tone
    if t.count("!") >= 2:
        return "excited"
    return "neutral"


def face_for(tone: str, text: str, llm_emotion: str = "") -> str:
    """Pick the final face. The LLM's choice is kept unless it clashes with the user's feelings."""
    if tone == "sad" and re.search(VERY_SAD, text.lower()):
        return "cry"
    if llm_emotion in EMOTIONS and not (tone in NEGATIVE_TONES and llm_emotion in CHEERFUL_FACES):
        return llm_emotion
    return TONE_TO_EMOTION.get(tone, "happy")


# Cues in what was said. Used when the LLM just says "happy", and to keep the faces varied.
EMOTION_CUES = [  # checked in order, first match wins; the user's words are checked before Pip's own reply
    ("starry", r"wow|amazing|awesome|incredible|so cool|impressive|fantastic|brilliant|genius"),
    ("surprised", r"fun fact|did you know|no way|whoa|unbelievable|really\?|surprising"),
    ("silly", r"joke|funny|haha|lol|silly|prank|pun\b|knock knock"),
    ("shy", r"you are (so )?(cute|smart|sweet|kind|nice|pretty|clever)|blush|flatter"),
    ("love", r"love|thank you|thanks|best friend|hug|favou?rite"),
    ("joy", r"congrat|well done|great job|proud|celebrate|you did it|nice work|hooray|yay"),
    ("wink", r"hello|hi there|hey|hi pip|good morning|nice to meet|how are you|who are you|your name|secret"),
    ("sleepy", r"goodnight|good night|bedtime|sleep well|sweet dreams|bye|goodbye|see you"),
    ("thinking", r"let me think|hmm|good question|explain|how does|how do|why is|why do|what is|what are|because"),
    ("sad", r"sorry to hear|that sounds hard|i am sorry"),
    ("curious", r"\?"),
]
VARY_POOL = ["wink", "joy", "excited", "starry", "curious", "silly", "shy", "love"]   # rotated when a face would repeat
REPEATABLE = {"happy", "curious", "thinking", "wink", "joy", "excited", "starry", "silly", "shy", "love"}


def cue_emotion(user_text: str, reply_text: str) -> str:
    for source in (user_text.lower(), reply_text.lower()):
        for emotion, pattern in EMOTION_CUES:
            if found(pattern, source):
                return emotion
    return ""


def vary(emotion: str, tone: str) -> str:
    """Never show the same cheerful face twice in a row when nothing special is happening."""
    last = state.get("last_emotion", "")
    if emotion == last and emotion in REPEATABLE and tone not in NEGATIVE_TONES:
        for _ in range(len(VARY_POOL)):
            state["rot"] = (state.get("rot", 0) + 1) % len(VARY_POOL)
            if VARY_POOL[state["rot"]] != last:
                return VARY_POOL[state["rot"]]
    return emotion


def finalize_emotion(user_text: str, reply: dict) -> dict:
    tone = reply.get("tone", "neutral")
    emotion = reply.get("emotion", "")
    if emotion in ("", "happy") and tone not in NEGATIVE_TONES:
        from_tone = TONE_TO_EMOTION.get(tone, "") if tone not in ("neutral", "happy") else ""
        emotion = cue_emotion(user_text, reply.get("text", "")) or from_tone or emotion or "happy"
    emotion = face_for(tone, user_text, emotion)      # still never cheerful for someone who is hurting
    emotion = vary(emotion, tone)
    state["last_emotion"] = emotion
    return {**reply, "emotion": emotion}


RULES = (
    " You live inside a tiny robot with an OLED face and a speaker, so your words are spoken aloud. "
    "First judge the tone of the user's last message: " + ", ".join(TONES) + ". "
    "Then show you understand it with your face: sad or lonely -> sad (or cry if they are very upset), angry -> pout, "
    "anxious -> shy, tired -> sleepy, excited -> excited, loving -> love, playful -> silly. Never use a cheerful face for someone who is hurting. "
    "Reply with ONLY a JSON object: {\"tone\": \"<one of those tones>\", \"emotion\": \"<one of: " + ", ".join(EMOTIONS) + ">\", "
    "\"text\": \"<what you say>\"}. The text must be 1 or 2 short sentences, under 160 characters, "
    "plain words with no emojis, no markdown. Comfort first when the user is sad, anxious or angry. "
    "Use the WHOLE range of faces and do not default to happy: excited = good news or big plans, joy = celebrating, "
    "wink = greetings, friendly secrets, playful teasing, silly = jokes and wordplay, surprised = amazing facts, "
    "starry = something impressive, love = thanks and affection, shy = compliments, curious = when you ask a question back, "
    "thinking = explaining or a careful answer, sleepy = goodnight and goodbye. Use plain happy only for an ordinary cheerful reply, "
    "and pick a different face from your last reply when it fits."
)

state = {"mode": 0, "history": [], "log": [], "warned": False, "provider": "-"}
cooldown = {}                # provider name -> time when it may be tried again
START_TIME = time.time()
state.update({"device": {}, "turn": {}, "cmd": {}, "errors": {}, "lvl_hist": [], "listen": True, "rec_total": 0, "rec_ignored": 0})   # live data for the dashboard
OFFLINE = os.getenv("LLM_MODE", "").lower() == "offline"   # set LLM_MODE=offline to skip the LLM completely

# Speech to text: LiveKit (cloud, needs LIVEKIT_URL / LIVEKIT_API_KEY / LIVEKIT_API_SECRET) or local faster-whisper.
LIVEKIT_READY = all(os.getenv(k) for k in ("LIVEKIT_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET"))
STT_PROVIDER = os.getenv("STT_PROVIDER", "livekit" if LIVEKIT_READY else "whisper").lower()
LIVEKIT_STT_MODEL = os.getenv("LIVEKIT_STT_MODEL", "deepgram/nova-3")   # any LiveKit Inference speech model
_whisper = {"model": None}
lk_stt = None
if STT_PROVIDER == "livekit":
    if not LIVEKIT_READY:
        log.error("STT_PROVIDER=livekit but LIVEKIT_URL / LIVEKIT_API_KEY / LIVEKIT_API_SECRET are not all set (put them in server/.env). Using local Whisper instead")
        STT_PROVIDER = "whisper"
    else:
        try:
            from livekit import rtc as lk_rtc
            from livekit.agents import inference as lk_inference
            lk_stt = lk_inference.STT(model=LIVEKIT_STT_MODEL, language="en")
            log.info("speech-to-text: LiveKit (%s). Local Whisper is only loaded if LiveKit fails", LIVEKIT_STT_MODEL)
        except Exception as e:
            log.error("could not start LiveKit speech-to-text (%s). Install it with: pip install livekit-agents. Using local Whisper instead", e)
            STT_PROVIDER = "whisper"


def get_whisper():
    if _whisper["model"] is None:
        log.info("loading the local speech-to-text model '%s' (the first run downloads it, please wait)...", WHISPER_MODEL)
        _whisper["model"] = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
    return _whisper["model"]


if STT_PROVIDER == "whisper":
    get_whisper()
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
log_llm.info("brain chain: %s | ready (have a key): %s | last resort: built-in replies", " -> ".join(CHAIN), ", ".join(ACTIVE) or "NONE (add keys to .env)")
if OFFLINE:
    log_llm.warning("LLM_MODE=offline: the brain is skipped, only built-in replies are used")
def my_ips() -> list:
    ips = set()
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))   # no data is sent, this only finds which address the Mac uses on the network
        ips.add(sock.getsockname()[0])
        sock.close()
    except Exception:
        pass
    try:
        ips.update(i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
    except Exception:
        pass
    return sorted(i for i in ips if not i.startswith("127."))


def startup_checks():
    ff = shutil.which("ffmpeg")
    if ff:
        log.info("ffmpeg found at %s (needed for the voice)", ff)
    else:
        log.error("ffmpeg NOT FOUND: the speaker will be silent. Run:  brew install ffmpeg")
    ips = my_ips()
    log.info("this computer's network address: %s", ", ".join(ips) or "unknown")
    log.info("put that address in SERVER_HOST in ai_bot.ino (it must start with the same numbers as the ESP32's IP)")
    log.info("voice: %s (loudness x%s) | hearing: %s | dashboard: http://localhost:8000/dashboard", VOICE, TTS_GAIN, "LiveKit " + LIVEKIT_STT_MODEL if lk_stt is not None else "Whisper " + WHISPER_MODEL)


startup_checks()
app = FastAPI()


def transcribe_whisper(pcm: bytes) -> str:
    audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    if peak > 0:
        audio = audio * min(8.0, 0.6 / peak)  # lift quiet mic audio
    segments, _ = get_whisper().transcribe(audio, language="en", beam_size=1, vad_filter=True,
                                 condition_on_previous_text=False, no_speech_threshold=0.6)
    return " ".join(s.text.strip() for s in segments).strip()


def prep_pcm(pcm: bytes) -> bytes:
    """Lifts quiet mic audio so the recogniser hears it (never past 8x, never clipped)."""
    a = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    peak = float(np.max(np.abs(a))) if a.size else 0.0
    if peak > 0:
        a = np.clip(a * min(8.0, 19000.0 / peak), -32767, 32767)
    return a.astype(np.int16).tobytes()


async def transcribe(pcm: bytes) -> str:
    """Words from the recording. LiveKit first (if set up), local Whisper as the fallback."""
    if lk_stt is not None:
        try:
            data = prep_pcm(pcm)
            frame = lk_rtc.AudioFrame(data=data, sample_rate=SAMPLE_RATE, num_channels=1, samples_per_channel=len(data) // 2)
            ev = await lk_stt.recognize(frame)
            text = ev.alternatives[0].text.strip() if ev.alternatives else ""
            state["stt_used"] = "livekit"
            return text
        except Exception as e:
            log_stt.error("LiveKit speech-to-text failed (%s: %s). Falling back to local Whisper for this recording", type(e).__name__, str(e)[:200])
    state["stt_used"] = "whisper"
    return await asyncio.to_thread(transcribe_whisper, pcm)


FILLERS = {"you", "uh", "um", "hmm", "mm", "mhm", "the", "so", "oh", "ah", "huh", "i"}


def junk(text: str) -> bool:
    """Speech models invent words on pure noise ("you", "thanks for watching"). Ignore those."""
    t = re.sub(r"[^a-z' ]", "", text.lower()).strip()
    return len(t) < 2 or t in FILLERS or "thanks for watching" in t or "subtitles by" in t or "amara.org" in t


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
    (r"how are you", "joy", "I am wonderful now that you are here!"),
    (r"hello|hi pip|hey|hi there", "wink", "Hi hi! I am so happy you are here!"),
    (r"you are (so )?(cute|smart|sweet|kind|nice|clever)|cute", "shy", "Oh stop it, you are making me blush!"),
    (r"wow|amazing|awesome|so cool|incredible", "starry", "Ooh, sparkly! Tell me more!"),
    (r"bye|goodbye|see you", "sleepy", "Bye bye! Come back soon."),
    (r"what is|why |how does|how do|explain", "thinking", "Hmm, good question. Let me think about that."),
]
OFFLINE_FALLBACK = [("happy", "Ask me anything. I am all ears!"), ("thinking", "Good question. Let us break it into small parts."),
                    ("love", "Tell me more. I am listening."), ("sleepy", "Mmm. Soft thoughts only now.")]


def offline_reply(text: str) -> dict:
    """Simple built-in replies, used when the LLM is unavailable."""
    t = text.lower()
    tone = detect_tone(text)
    for pattern, emotion, reply in OFFLINE_RULES:
        if found(pattern, t):
            return {"emotion": face_for(tone, text, emotion), "text": reply, "tone": tone}
    emotion, reply = OFFLINE_FALLBACK[state["mode"]]
    if tone == "neutral":
        return {"emotion": emotion, "text": reply, "tone": tone}
    return {"emotion": face_for(tone, text), "text": reply, "tone": tone}


NOT_CHAT = re.compile(r"whisper|tts|audio|speech|transcribe|realtime|embed|guard|moderation|image|imagen|veo|vision|robotics|computer-use|"
                      r"search|codex|instruct|aqa|learnlm|gemma|orpheus|safeguard|live|dall|davinci|babbage|sora|diffusion", re.I)
MODEL_PREFERENCE = {  # earlier words win; the newest matching model is picked
    "groq": ["llama-3.3-70b", "llama-3.1-8b", "llama", "gpt-oss", "qwen"],
    "gemini": ["flash", "pro"],
    "openai": ["gpt-4o-mini", "mini", "gpt-4o", "gpt"],
    "ollama": ["llama", "qwen", "gemma", "mistral", ""],
    "custom": [""],
}
found_models = {}   # provider -> model that actually worked


def discover_model(name: str):
    """Ask the provider which models exist and pick a sensible chat model. Used when the default name is gone."""
    base = PRESETS[name][0].rstrip("/")
    r = httpx.get(base + "/models", headers={"Authorization": "Bearer " + (provider_key(name) or "none")}, timeout=15)
    r.raise_for_status()
    ids = [m.get("id", "").replace("models/", "") for m in r.json().get("data", [])]
    ids = [i for i in ids if i and not NOT_CHAT.search(i)]
    stable = [i for i in ids if not re.search(r"preview|exp|beta|latest", i, re.I)] or ids
    for word in MODEL_PREFERENCE.get(name, [""]):
        hits = sorted([i for i in stable if word in i], reverse=True)
        if hits:
            return hits[0]
    return None


def post_chat(name: str, model: str, system: str, messages: list):
    base = PRESETS[name][0]
    return httpx.post(
        base.rstrip("/") + "/chat/completions",
        headers={"Authorization": "Bearer " + (provider_key(name) or "none")},
        json={"model": model, "messages": [{"role": "system", "content": system}] + messages,
              **({"max_completion_tokens": 800} if name == "openai" else {"max_tokens": 600, "temperature": 0.8})},
        timeout=60 if name == "ollama" else 20,
    )


def call_one(name: str, system: str, messages: list) -> str:
    if name == "anthropic":
        resp = llm.messages.create(model=CLAUDE_MODEL, max_tokens=300, system=system, messages=messages, timeout=20)
        return resp.content[0].text
    _, _, default_model, model_env = PRESETS[name]
    forced = os.getenv(model_env)                      # a model you set yourself is never replaced
    model = forced or found_models.get(name) or default_model
    r = post_chat(name, model, system, messages)
    if r.status_code in (400, 404) and not forced:     # the default model name is probably retired: find a current one
        try:
            better = discover_model(name)
        except Exception as e:
            better = None
            log_llm.warning("[%s] could not list models: %s", name, e)
        if better and better != model:
            log_llm.warning("[%s] model '%s' did not work, trying '%s'", name, model, better)
            r = post_chat(name, better, system, messages)
            if r.status_code < 400:
                found_models[name] = better
    if r.status_code >= 400:   # show the provider's own explanation, e.g. "insufficient_quota" or "model not found"
        raise RuntimeError(f"HTTP {r.status_code} (model {model}): {r.text[:300]}")
    return r.json()["choices"][0]["message"]["content"]


def call_llm(system: str, messages: list):
    """Try each provider in the chain. Returns (reply text, provider name) or raises if all fail."""
    errors = []
    for name in ACTIVE:
        if cooldown.get(name, 0) > time.time():
            continue
        try:
            reply_text = call_one(name, system, messages)
            state["errors"].pop(name, None)
            return reply_text, name
        except Exception as e:  # any failure moves on to the next provider
            cooldown[name] = time.time() + COOLDOWN_SECONDS
            state["errors"][name] = str(getattr(e, "message", e))[:200]
            errors.append(f"{name}: {getattr(e, 'message', e)}")
            log_llm.warning("[%s] failed, trying the next one: %s", name, getattr(e, "message", e))
    raise RuntimeError("; ".join(errors) or "no provider available")


def parse_reply(raw: str) -> dict:
    """Read the model's answer even when it is cut off or wrapped in extra text."""
    try:
        data = json.loads(raw[raw.index("{"): raw.rindex("}") + 1])
        return {k: str(data.get(k, "")).strip() for k in ("tone", "emotion", "text")}
    except Exception:
        pass

    def field(name):
        m = re.search(r'"' + name + r'"\s*:\s*"([^"]*)', raw)
        return m.group(1).strip() if m else ""

    out = {"tone": field("tone"), "emotion": field("emotion"), "text": field("text")}
    if not out["text"] and not raw.lstrip().startswith(("{", "`")):
        out["text"] = raw.strip()[:160]
    return out


def think(text: str) -> dict:
    if OFFLINE or not ACTIVE:
        state["provider"] = "offline"
        return finalize_emotion(text, {**offline_reply(text), "provider": "offline"})
    messages = state["history"][-8:] + [{"role": "user", "content": text}]
    try:
        raw, provider = call_llm(MODE_PROMPTS[state["mode"]] + RULES, messages)
    except Exception as e:  # every provider failed: keep talking with built-in replies
        log_llm.error("every brain failed, using a built-in reply: %s", e)
        state["provider"] = "offline"
        reply = finalize_emotion(text, {**offline_reply(text), "provider": "offline"})
        if not state["warned"]:
            state["warned"] = True
            reply["text"] = "My smart brain is offline right now, so I will keep it simple. " + reply["text"]
        return reply
    state["provider"] = provider
    state["warned"] = False
    parsed = parse_reply(raw)
    reply = {"emotion": parsed["emotion"].lower(), "text": parsed["text"], "tone": parsed["tone"].lower(), "provider": provider}
    if not reply["text"]:   # the answer was empty or cut off: use a built-in reply for this turn
        log_llm.warning("[%s] the answer had no usable text (%r), using a built-in reply", provider, raw[:100])
        off = offline_reply(text)
        reply["text"] = off["text"]
        reply["emotion"] = reply["emotion"] or off["emotion"]
    keyword_tone = detect_tone(text)
    if reply["tone"] not in TONES:
        reply["tone"] = keyword_tone
    elif reply["tone"] == "neutral" and keyword_tone != "neutral":   # the keyword check caught a feeling the LLM missed
        reply["tone"] = keyword_tone
    reply["emotion"] = face_for(reply["tone"], text, reply["emotion"])
    reply = finalize_emotion(text, reply)
    if not reply["text"]:
        reply["text"] = "Hmm, I got a little lost."
    state["history"] += [{"role": "user", "content": text},
                         {"role": "assistant", "content": json.dumps({"tone": reply["tone"], "emotion": reply["emotion"], "text": reply["text"]})}]
    state["history"] = state["history"][-16:]
    return reply


async def speak(text: str) -> bytes:
    """text -> mp3 (edge-tts, needs internet) -> raw 16 kHz mono s16le PCM (ffmpeg). Returns b"" if anything fails."""
    t0 = time.time()
    mp3 = bytearray()
    try:
        async for chunk in edge_tts.Communicate(text, VOICE, rate="+8%", pitch="+25Hz").stream():
            if chunk["type"] == "audio":
                mp3 += chunk["data"]
    except Exception as e:
        log_tts.error("the voice service failed: %s. edge-tts needs internet (it talks to Microsoft). Check your connection", e)
        return b""
    if not mp3:
        log_tts.error("the voice service returned NO audio for %r. Internet down or blocked?", text[:60])
        return b""
    try:
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg", "-loglevel", "error", "-i", "pipe:0", "-af", f"volume={TTS_GAIN},alimiter=limit=0.95:level=disabled", "-f", "s16le",
            "-ar", str(SAMPLE_RATE), "-ac", "1", "pipe:1",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        log_tts.error("ffmpeg is NOT installed, so the speaker will be silent. Run:  brew install ffmpeg")
        return b""
    pcm, err = await proc.communicate(bytes(mp3))
    if proc.returncode != 0 or not pcm:
        log_tts.error("ffmpeg failed (exit code %s): %s", proc.returncode, err.decode(errors="ignore")[:300])
        return b""
    log_tts.info("voice ready: %d ms of audio (%d KB) in %d ms, loudness x%s", len(pcm) // 32, len(pcm) // 1024, int((time.time() - t0) * 1000), TTS_GAIN)
    return pcm


def to_wav(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SAMPLE_RATE); w.writeframes(pcm)
    return buf.getvalue()


def record(user: str, reply: dict):
    state["log"] = (state["log"] + [{"user": user, "emotion": reply["emotion"], "bot": reply["text"]}])[-30:]
    log.info("[%s · %s] you (%s): %r -> face %s: %r", MODES[state["mode"]], reply.get("provider", "-"), reply.get("tone", "-"), user, reply["emotion"], reply["text"])


def levels(pcm: bytes) -> dict:
    """Loudness of 16-bit audio as percent of full scale, for the dashboard."""
    a = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype=np.int16).astype(np.float32) / 32768.0
    if a.size == 0:
        return {"rms": 0.0, "peak": 0.0, "ms": 0}
    return {"rms": round(float(np.sqrt(np.mean(a * a))) * 100, 1), "peak": round(float(np.max(np.abs(a))) * 100, 1),
            "ms": int(a.size / SAMPLE_RATE * 1000)}


def health() -> dict:
    return {
        "uptime": int(time.time() - START_TIME), "stt": ("LiveKit " + LIVEKIT_STT_MODEL) if lk_stt is not None else ("Whisper " + WHISPER_MODEL), "voice": VOICE, "tts_gain": TTS_GAIN,
        "ffmpeg": bool(shutil.which("ffmpeg")), "offline_forced": OFFLINE, "provider": state.get("provider", "-"),
        "chain": [{"name": n, "ready": n in ACTIVE, "cooldown": max(0, int(cooldown.get(n, 0) - time.time())),
                   "error": state["errors"].get(n, ""),
                   "model": found_models.get(n) or (CLAUDE_MODEL if n == "anthropic" else PRESETS.get(n, ("", "", "", ""))[2])}
                  for n in CHAIN],
    }


async def respond(text: str) -> dict:
    switched = maybe_switch_mode(text)
    if switched:
        reply = {"emotion": "excited", "text": f"Okay! {MODES[state['mode']]} mode on."}
    else:
        reply = await asyncio.to_thread(think, text)
    record(text, reply)
    return reply



def ascii_header(s: str) -> str:
    return (s or "").encode("ascii", "ignore").decode().replace("\n", " ").replace("\r", " ")[:160]


_player = {"proc": None}


def find_player():
    for cmd in (["afplay"], ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"], ["paplay"], ["aplay", "-q"]):
        if shutil.which(cmd[0]):
            return cmd
    return None

pl = find_player()
log.info("voice comes out of: %s (PLAY_ON=%s, player: %s). Test it: open http://localhost:8000/api/laptop-test", "the LAPTOP speakers" if PLAY_ON == "laptop" else "the ESP32 speaker" if PLAY_ON == "device" else "laptop and ESP32", PLAY_ON, pl[0] if pl else "NONE FOUND")


async def play_on_laptop(pcm: bytes):
    """Plays the voice on THIS computer's speakers (afplay on a Mac). Runs in the background; a new reply replaces an old one."""
    cmd = find_player()
    if not cmd:
        log_tts.error("no audio player found on this computer (a Mac has afplay built in; elsewhere install ffmpeg/ffplay). The voice cannot be played here")
        return
    if not pcm:
        return
    import tempfile
    f = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    f.write(to_wav(pcm))
    f.close()
    old = _player["proc"]
    if old and old.returncode is None:
        old.kill()
    try:
        proc = await asyncio.create_subprocess_exec(*cmd, f.name, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
    except Exception as e:
        log_tts.error("could not start %s: %s", cmd[0], e)
        return
    _player["proc"] = proc
    log_tts.info("playing the voice on the laptop speakers with %s (%d ms)", cmd[0], len(pcm) // 32)

    async def done():
        err = (await proc.stderr.read()).decode(errors="ignore").strip()
        await proc.wait()
        if proc.returncode not in (0, -9) or err:
            log_tts.error("%s finished with code %s: %s", cmd[0], proc.returncode, err[:200])
        try:
            os.unlink(f.name)
        except OSError:
            pass
    asyncio.create_task(done())


@app.post("/talk")
async def talk(request: Request):
    pcm = await request.body()
    who = request.client.host if request.client else "?"
    log_net.info("/talk from %s: received %d KB of audio (%d ms)", who, len(pcm) // 1024, len(pcm) // 32)
    if len(pcm) < SAMPLE_RATE * 2 * 0.4:  # under 0.4 s: ignore
        state["rec_total"] += 1
        state["rec_ignored"] += 1
        log_stt.info("ignored: the recording is shorter than 0.4 s")
        return Response(status_code=204)
    t0 = time.time()
    mic = levels(pcm)
    state["rec_total"] += 1
    if mic["peak"] >= 98:
        log_stt.warning("the microphone is CLIPPING (peak %s%%, average %s%%): too much gain or very loud noise. The board lowers its gain by itself; if this keeps happening, move away from voices and noise", mic["peak"], mic["rms"])
    elif mic["rms"] > 40:
        log_stt.warning("the recording is a constant loud signal (average %s%%): noise or interference, not speech", mic["rms"])
    elif mic["peak"] < 3:
        log_stt.warning("the microphone is very quiet (peak %s%%): speak closer, or the board will raise its gain", mic["peak"])
    text = await transcribe(pcm)
    t1 = time.time()
    log_stt.info("🎤 I HEARD: \"%s\"  [%s]", text or "(nothing)", state.get("stt_used", "?"))
    log_stt.info("heard %r from %d ms of audio (mic level %s%% average, %s%% peak) in %d ms", text, mic["ms"], mic["rms"], mic["peak"], int((t1 - t0) * 1000))
    if junk(text):
        log_stt.info("ignored: no real speech in that recording (heard %r)%s", text, ". The mic level is very low" if mic["peak"] < 3 else "")
        state["rec_ignored"] += 1
        state["turn"] = {"at": time.time(), "source": "esp32", "heard": "", "note": "nothing understood", "mic": mic}
        return Response(status_code=204, headers={"X-Heard": ascii_header(text)})
    reply = await respond(text)
    t2 = time.time()
    audio = await speak(reply["text"])
    t3 = time.time()
    if not audio:
        log_tts.error("SENDING AN EMPTY VOICE to the ESP32: the speaker will be SILENT. Fix the line above (ffmpeg or the voice service), then try again")
    else:
        log_net.info("answering: HTTP 200, %d KB voice, face %s, brain %s, total %.1f s (hearing %d ms, thinking %d ms, voice %d ms)",
                     len(audio) // 1024, reply["emotion"], reply.get("provider", "-"), t3 - t0,
                     int((t1 - t0) * 1000), int((t2 - t1) * 1000), int((t3 - t2) * 1000))
    state["turn"] = {"at": time.time(), "source": "esp32", "heard": text, "tone": reply.get("tone", "neutral"),
                     "emotion": reply["emotion"], "provider": reply.get("provider", "-"), "reply": reply["text"],
                     "mic": mic, "speaker": levels(audio),
                     "ms": {"stt": int((t1 - t0) * 1000), "llm": int((t2 - t1) * 1000), "tts": int((t3 - t2) * 1000)}}
    if audio and PLAY_ON in ("laptop", "both"):
        await play_on_laptop(audio)
    headers = {
        "X-Heard": ascii_header(text),
        "X-Playback": "laptop" if PLAY_ON == "laptop" else "device",
        "X-Emotion": reply["emotion"],
        "X-Text": reply["text"].encode("ascii", "ignore").decode()[:200],
        "X-Mode": MODES[state["mode"]],
        "X-Provider": reply.get("provider", "-"),
        "X-Tone": reply.get("tone", "neutral"),
    }
    return Response(audio, media_type="application/octet-stream", headers=headers)


@app.get("/mode")
async def set_mode(m: int = 0):
    state["mode"] = max(0, min(3, m))
    state["history"] = []
    return {"mode": MODES[state["mode"]]}


_last = {"pcm": b""}


@app.post("/api/transcribe")  # microphone test: words only, no LLM and no voice
async def api_transcribe(request: Request):
    pcm = await request.body()
    _last["pcm"] = pcm
    mic = levels(pcm)
    t0 = time.time()
    text = await transcribe(pcm)
    log_stt.info("🎤 MIC TEST HEARD: \"%s\"  [%s] (%d ms of audio, level %s%% avg, %s%% peak, %d ms)", text or "(nothing)", state.get("stt_used", "?"), mic["ms"], mic["rms"], mic["peak"], int((time.time() - t0) * 1000))
    if mic["peak"] >= 98:
        log_stt.warning("the recording is saturated (peak %s%%): the microphone wiring is wrong, or the gain is too high", mic["peak"])
    return {"text": text.replace('"', "'"), "engine": state.get("stt_used", "?"), "mic": mic}


@app.get("/api/last.wav")  # the last microphone test recording, so you can listen to what the mic really captured
async def last_wav():
    if not _last["pcm"]:
        return PlainTextResponse("no recording yet: press r in the mic test sketch first\n", status_code=404)
    return Response(to_wav(_last["pcm"]), media_type="audio/wav")


@app.post("/api/chat")  # used by the test page below, no ESP32 needed
async def chat(request: Request):
    body = await request.json()
    t1 = time.time()
    reply = await respond(body.get("text", ""))
    t2 = time.time()
    pcm = await speak(reply["text"])
    t3 = time.time()
    state["turn"] = {"at": time.time(), "source": "web", "heard": body.get("text", ""), "tone": reply.get("tone", "neutral"),
                     "emotion": reply["emotion"], "provider": reply.get("provider", "-"), "reply": reply["text"],
                     "speaker": levels(pcm), "ms": {"llm": int((t2 - t1) * 1000), "tts": int((t3 - t2) * 1000)}}
    wav = to_wav(pcm)
    return JSONResponse({**reply, "mode": MODES[state["mode"]], "audio": base64.b64encode(wav).decode()})


@app.get("/api/state")
async def get_state():
    dev = dict(state["device"])
    if dev:
        dev["age"] = round(time.time() - dev.get("seen", 0), 1)
        if dev["age"] > 8 and state.get("device_online"):
            state["device_online"] = False
            log_esp.warning("ESP32 stopped reporting (last seen %.0f s ago). Is it powered, on WiFi, and is SERVER_HOST still right?", dev["age"])
    return {"mode": state["mode"], "modes": MODES, "log": state["log"], "device": dev, "turn": state["turn"],
            "lvl_hist": state["lvl_hist"], "health": health(), "listen": state["listen"],
            "rec_total": state["rec_total"], "rec_ignored": state["rec_ignored"], "emotions": EMOTIONS + ["listening"]}


@app.get("/api/device")  # the ESP32 reports here every second or two, and receives commands in the reply
async def device(request: Request, lvl: int = 0, thr: int = 0, rssi: int = 0, emo: str = "", heap: int = 0,
                 up: int = 0, vol: int = 0, gain: int = 0):
    if not state["device"] or time.time() - state["device"].get("seen", 0) > 10:
        log_esp.info("ESP32 connected from %s (mic level %d, face %s, WiFi %d dBm)", request.client.host if request.client else "?", lvl, emo, rssi)
        state["device_online"] = True
    state["device"] = {"lvl": lvl, "thr": thr, "rssi": rssi, "emo": emo, "heap": heap, "up": up, "vol": vol, "gain": gain,
                       "ip": request.client.host if request.client else "", "seen": time.time()}
    state["lvl_hist"] = (state["lvl_hist"] + [lvl])[-60:]
    cmd, state["cmd"] = state["cmd"], {}
    return PlainTextResponse(f"force={cmd.get('face', '-')} beep={1 if cmd.get('beep') else 0} listen={1 if state['listen'] else 0}\n")


@app.post("/api/devlog")  # the ESP32 sends its own log lines here, so they show in this terminal and on the dashboard
async def devlog(request: Request):
    text = (await request.body()).decode("utf-8", errors="ignore")
    for line in text.splitlines():
        if line.strip():
            (log_esp.error if " ERR " in line else log_esp.info)(line.strip())
    return PlainTextResponse("ok\n")


@app.get("/api/ding")  # a short "go ahead" beep on the laptop: the board calls it when push-to-talk starts, so you know when to speak
async def ding():
    n = int(SAMPLE_RATE * 0.16)
    x = np.arange(n) / SAMPLE_RATE
    env = np.minimum(1.0, np.minimum(x / 0.01, (0.16 - x) / 0.04))
    wave_ = (np.sin(2 * np.pi * 988 * x) + 0.5 * np.sin(2 * np.pi * 1480 * x)) / 1.5 * env * 0.6
    await play_on_laptop((wave_ * 32767).astype(np.int16).tobytes())
    await asyncio.sleep(0.4)   # the board waits for the beep to finish before it records
    return {"ok": True}


@app.get("/api/laptop-test")  # plays a sentence on the laptop speakers: proves the Mac side works
async def laptop_test(text: str = "Hello! I am Pip. This is the laptop speaker."):
    audio = await speak(text[:200])
    if not audio:
        return PlainTextResponse("the server could not make the voice, see the server terminal\n", status_code=500)
    await play_on_laptop(audio)
    return {"playing": True, "player": (find_player() or ["none"])[0], "play_on": PLAY_ON}


@app.get("/api/say")  # raw voice for the ESP32 speaker test sketch
async def say(text: str = "Hello, I am Pip. This is a speaker test."):
    log_net.info("/api/say: %r", text[:80])
    audio = await speak(text[:200])
    if not audio:
        return PlainTextResponse("the server could not make the voice, see the server terminal (ffmpeg or internet)\n", status_code=500)
    return Response(audio, media_type="application/octet-stream", headers={"X-Text": text.encode("ascii", "ignore").decode()[:200]})


@app.get("/api/say.wav")  # the same voice as a normal sound file: open it in your browser to check the Mac makes sound
async def say_wav(text: str = "Hello, I am Pip. This is a speaker test."):
    audio = await speak(text[:200])
    if not audio:
        return PlainTextResponse("the server could not make the voice, see the server terminal (ffmpeg or internet)\n", status_code=500)
    return Response(to_wav(audio), media_type="audio/wav")


@app.get("/api/logs")
async def get_logs(n: int = 200):
    return list(RING)[-max(1, min(n, 400)):]


@app.post("/api/cmd")  # dashboard buttons: show a face on the real OLED, or beep the real speaker
async def cmd(request: Request):
    body = await request.json()
    log_esp.info("dashboard command for the device: %s", body)
    if body.get("face") in EMOTIONS + ["listening"]:
        state["cmd"]["face"] = body["face"]
    if body.get("beep"):
        state["cmd"]["beep"] = True
    if "listen" in body:
        state["listen"] = bool(body["listen"])
    return {"queued": state["cmd"]}


@app.post("/api/selftest")  # checks every part of the server side in one go
async def selftest():
    results, audio_b64 = [], ""

    def add(name, ok, detail):
        results.append({"name": name, "ok": bool(ok), "detail": detail})

    has_ffmpeg = bool(shutil.which("ffmpeg"))
    add("ffmpeg installed", has_ffmpeg, "converts the voice for the speaker" if has_ffmpeg else "run: brew install ffmpeg")
    try:
        t = time.time()
        pcm = await speak("Testing one, two, three. Pip is ready.")
        ms = int((time.time() - t) * 1000)
        lv = levels(pcm)
        add("Voice (text to speech)", lv["ms"] > 500, f"{lv['ms']} ms of audio made in {ms} ms, level {lv['rms']}% (peak {lv['peak']}%)")
        audio_b64 = base64.b64encode(to_wav(pcm)).decode()
        t = time.time()
        heard = await transcribe(pcm)
        add("Hearing (speech to text)", len(heard) > 5, f'heard "{heard}" in {int((time.time() - t) * 1000)} ms')
    except Exception as e:
        add("Voice and hearing", False, str(e)[:200])
    saved = list(state["history"])
    try:
        t = time.time()
        r = await asyncio.to_thread(think, "Hello Pip, how are you today?")
        ok = r.get("provider") not in (None, "offline")
        add("Brain (LLM)", ok, f"{r.get('provider')} answered in {int((time.time() - t) * 1000)} ms: {r['text'][:70]}"
            + ("" if ok else "  (using built-in replies, check your keys)"))
    except Exception as e:
        add("Brain (LLM)", False, str(e)[:200])
    state["history"] = saved
    dev = state["device"]
    online = bool(dev) and time.time() - dev.get("seen", 0) < 6
    add("ESP32 connected", online, f"reporting from {dev.get('ip', '?')}, mic level {dev.get('lvl', '?')}" if online
        else "not reporting. Upload the latest ai_bot.ino and check WiFi and SERVER_HOST")
    return {"results": results, "audio": audio_b64}


PAGE = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Pip server</title>
<body style="font-family:system-ui;max-width:560px;margin:auto;padding:16px">
<h2>Pip server is running</h2>
<p><a href="/dashboard">Open the live dashboard</a> (levels, faces, self-test)</p>
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
 const a=new Audio('data:audio/wav;base64,'+r.audio);st.textContent='You sounded '+(r.tone||'neutral')+'. Pip ('+r.emotion+', via '+(r.provider||'-')+'): '+r.text;
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


@app.get("/dashboard", response_class=HTMLResponse)
async def dashboard():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")
    if not os.path.exists(path):
        return "dashboard.html is missing. Download it into the same folder as server.py, then reload."
    return open(path, encoding="utf-8").read()


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000, access_log=False)
