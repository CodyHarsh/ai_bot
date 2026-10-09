"""Builds site/simulator.html by embedding the real source files into the page."""
import html, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
page = (root / "site" / "simulator.src.html").read_text()
files = {
    "__F_MAIN__": "firmware/ai_bot/ai_bot.ino",
    "__F_MIC__": "firmware/test_mic/test_mic.ino",
    "__F_SPK__": "firmware/test_speaker/test_speaker.ino",
    "__F_SERVER__": "server/server.py",
    "__F_REQ__": "server/requirements.txt",
}
for key, path in files.items():
    page = page.replace(key, html.escape((root / path).read_text(), quote=False))
page = page.replace("/*__SCENE3D__*/", (root / "site" / "scene3d.js").read_text())
(root / "site" / "simulator.html").write_text(page)
print("built", len(page), "bytes")
