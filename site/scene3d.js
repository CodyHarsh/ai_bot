/* Pip parts bench. Same idea as the Bedside Guardian bench: every part is modelled in centimetres, laid out
   in manifest order, and can travel into the assembled circuit. The assembled layout, pin positions and wire
   colours follow the Cirkit Designer project (OLED back, ESP32 centre with antenna up and USB down,
   INMP441 left, MAX98357A and speaker right). */
(function () {
  "use strict";
  var $$ = function (s) { return document.querySelector(s); };
  var RM = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var viewEl = $$("#threeWrap"), labelsEl = $$("#labels3"), rowsEl = $$("#rows3");
  var THREE = window.THREE;
  var LIVE = {};   // filled in once WebGL is up

  /* ---------- the list: everything on the page is derived from it ---------- */
  var NET = {
    "3V3": "#a0522d", "5V": "#ff7eb3", "GND": "#ff12b4", "BCLK": "#0e8f9c", "WS": "#6b3a3a", "MIC": "#00e5f5",
    "SPK": "#6ab04c", "SDA": "#c3c3ff", "SCL": "#ff8c00", "VO+": "#9b8cf0", "VO-": "#7cfc00"
  };
  var NETINFO = { "3V3": "3.3 V power", "5V": "5 V power", "GND": "Ground", "BCLK": "I2S bit clock", "WS": "I2S word select", "MIC": "Mic audio in", "SPK": "Speaker audio out", "SDA": "OLED data", "SCL": "OLED clock", "VO+": "Speaker +", "VO-": "Speaker −" };
  var PIN_NET = { "3V3": "3V3", "GPIO33": "MIC", "GPIO25": "WS", "GPIO14": "BCLK", "GND": "GND", "5V": "5V", "GPIO22": "SPK", "GPIO21": "SDA", "GPIO19": "SCL" };
  var PI = Math.PI;

  var PARTS = [
    { id: "esp32", ref: "U1", name: "ESP32 DevKitC", stage: "Brain", qty: "1 board", pieces: 1, pins: "9 pins",
      note: "Runs the firmware, joins WiFi, reads the microphone and drives the speaker over I2S, and draws the face on the OLED. Uses 3V3, 5V, GND and GPIO14, 25, 33, 22, 21, 19.",
      tags: ["38 pin, micro-USB", "WiFi + I2S"],
      m: { p: [-17, 0, -2], s: 1.35 }, a: { p: [0, 0, 0], r: [0, PI / 2, 0], s: 1 } },
    { id: "oled", ref: "DS1", name: "OLED SSD1306 128×64", stage: "Face", qty: "1 module", pieces: 1, pins: "4 pins",
      note: "I2C screen at address 0x3C that shows Pip's face. VDD to 3V3, GND to GND, SCK to GPIO19, SDA to GPIO21.",
      tags: ["I2C 0x3C", "128 × 64"],
      m: { p: [-8.5, 0, -2], s: 1.55 }, a: { p: [0, 0, -7], s: 1 } },
    { id: "mic", ref: "MK1", name: "INMP441 microphone", stage: "Listen", qty: "1 breakout", pieces: 1, pins: "6 pins",
      note: "Digital I2S microphone. L/R is tied to GND so it sends on the left channel. SD carries the audio to GPIO33, SCK and WS are the shared I2S clocks.",
      tags: ["I2S, 24-bit", "Left channel"],
      m: { p: [-2.5, 0, -2], s: 1.9 }, a: { p: [-7.4, 0, -1.1], s: 1 } },
    { id: "amp", ref: "U2", name: "MAX98357A amplifier", stage: "Speak", qty: "1 breakout", pieces: 1, pins: "5 pins + 2 terminals",
      note: "I2S amplifier. It takes digital audio on DIN and drives the speaker through its screw terminals. Leave SD and GAIN unconnected.",
      tags: ["I2S in", "3 W class-D"],
      m: { p: [4.2, 0, -2], s: 1.7 }, a: { p: [7.7, 0, -0.7], s: 1 } },
    { id: "spk", ref: "LS1", name: "Speaker", stage: "Speak", qty: "1 driver", pieces: 1, pins: "2 terminals",
      note: "A small 4 to 8 ohm speaker. It connects to the amplifier terminals only, never to GND.",
      tags: ["4–8 Ω", "Driven by VO+ / VO−"],
      m: { p: [12.5, 0, -2], s: 1.15 }, a: { p: [12.6, 0, 0.8], s: 1 } },
    { id: "jump", ref: "W1–W17", name: "Jumper wires", stage: "Wiring", qty: "17 wires", pieces: 17, pins: "11 signals",
      note: "Seventeen colour-coded wires, the same colours as the Cirkit project. Four share GND on the ESP32, and two each share 3V3, GPIO14 and GPIO25.",
      tags: ["Male to female", "Cirkit colours"],
      m: { p: [-6, 0, 7.5], s: 1.2 }, a: { p: [-6, 0, 7.5], s: 0.001 } },
    { id: "usb", ref: "J1", name: "USB cable", stage: "Power", qty: "1 cable", pieces: 1, pins: "5 V + serial",
      note: "Powers the board and carries the Serial Monitor to your Mac. The ESP32's 5V pin feeds the amplifier from it.",
      tags: ["Micro-USB", "Data, not charge-only"],
      m: { p: [6, 0, 7.5], s: 1.2 }, a: { p: [6, 0, 7.5], s: 0.001 } }
  ];
  var PIECES = PARTS.reduce(function (a, p) { return a + p.pieces; }, 0);
  $$("#s-lines3").textContent = PARTS.length;
  $$("#s-pieces3").textContent = PIECES;
  $$("#s-nets3").textContent = Object.keys(NET).length;
  $$("#rail-count3").textContent = PARTS.length + " lines · " + PIECES + " pieces";

  PARTS.forEach(function (p) {
    var b = document.createElement("button");
    b.className = "pb-row"; b.type = "button"; b.setAttribute("aria-pressed", "false"); b.dataset.id = p.id;
    b.innerHTML = '<div class="pb-ref">' + p.ref.split("–")[0] + '</div><div><div class="pb-nm"></div><div class="pb-meta"></div></div><div class="pb-pins"></div>';
    b.querySelector(".pb-nm").textContent = p.name;
    b.querySelector(".pb-meta").textContent = p.stage + " · " + p.qty;
    b.querySelector(".pb-pins").textContent = p.pins;
    b.addEventListener("click", function () { if (LIVE.select) LIVE.select(p.id); });
    rowsEl.appendChild(b);
  });

  function noweb(msg) {
    var d = document.createElement("div"); d.className = "pb-noweb"; d.textContent = msg; viewEl.appendChild(d);
  }
  if (!THREE) { noweb("The 3D library did not load. The parts list beside this panel is complete on its own."); return; }
  var renderer;
  try { renderer = new THREE.WebGLRenderer({ antialias: true }); }
  catch (e) { noweb("This browser can't run WebGL, so the 3D bench can't draw. The parts list beside this panel is complete on its own."); return; }

  var tok = function (n) { return getComputedStyle(document.documentElement).getPropertyValue(n).trim(); };
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputEncoding = THREE.sRGBEncoding;
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFSoftShadowMap;
  viewEl.insertBefore(renderer.domElement, labelsEl);

  var scene = new THREE.Scene();
  var camera = new THREE.PerspectiveCamera(36, 1, 0.3, 300);

  scene.add(new THREE.HemisphereLight(0xdfe6ee, 0x30363d, 0.78));
  var key = new THREE.DirectionalLight(0xfff0dc, 1.15);
  key.position.set(16, 28, 20); key.castShadow = true; key.shadow.mapSize.set(1024, 1024);
  key.shadow.camera.near = 6; key.shadow.camera.far = 90;
  key.shadow.camera.left = -32; key.shadow.camera.right = 32; key.shadow.camera.top = 32; key.shadow.camera.bottom = -32;
  key.shadow.bias = -0.0012; scene.add(key);
  var fill = new THREE.DirectionalLight(0xc8d8ea, 0.42); fill.position.set(-18, 14, -20); scene.add(fill);
  var rim = new THREE.DirectionalLight(0xffffff, 0.25); rim.position.set(0, 10, -26); scene.add(rim);

  var mat = new THREE.Mesh(new THREE.PlaneGeometry(300, 300), new THREE.MeshStandardMaterial({ color: 0x000000, roughness: 0.96, metalness: 0 }));
  mat.rotation.x = -PI / 2; mat.position.y = -0.02; mat.receiveShadow = true; scene.add(mat);
  var grid = new THREE.GridHelper(200, 200, 0x000000, 0x000000);
  grid.material.transparent = true; grid.material.opacity = 0.5; grid.position.y = 0.005; scene.add(grid);

  /* ---------- geometry helpers (centimetres) ---------- */
  var C = {
    pcbBlack: 0x15171C, pcbBlue: 0x1840C8, oledBlue: 0x1E4FA0, gold: 0xC9A227, silver: 0xB7BEC6, chrome: 0xD3D8DE,
    white: 0xE9E7E2, dark: 0x24282E, slate: 0x3A4149, glass: 0x0B1116, amber: 0xEDA23F, green: 0x46C08A, cone: 0x8E949B
  };
  function M(c, o) {
    var d = { color: new THREE.Color(c), roughness: 0.62, metalness: 0.04 };
    if (o) for (var k in o) d[k] = o[k];
    return new THREE.MeshStandardMaterial(d);
  }
  function mesh(g, m) { var x = new THREE.Mesh(g, m); x.castShadow = true; x.receiveShadow = true; return x; }
  function box(w, h, d, m) { return mesh(new THREE.BoxGeometry(w, h, d), m); }
  function cyl(rt, rb, h, s, m, open) { return mesh(new THREE.CylinderGeometry(rt, rb, h, s || 20, 1, !!open), m); }
  function sph(r, m, ph) { return mesh(new THREE.SphereGeometry(r, 26, 18, 0, PI * 2, 0, ph || PI), m); }
  function put(parent, o, x, y, z, rx, ry, rz) { o.position.set(x, y, z); if (rx || ry || rz) o.rotation.set(rx || 0, ry || 0, rz || 0); parent.add(o); return o; }
  var goldM = function () { return M(C.gold, { metalness: 0.85, roughness: 0.32 }); };
  function newPart() { var g = new THREE.Group(); g.userData.pins = {}; return g; }
  function anchor(root, parent, name, x, y, z) { var o = new THREE.Object3D(); o.position.set(x, y, z); parent.add(o); root.userData.pins[name] = o; return o; }
  function pinPost(parent, x, z, y0) { return put(parent, cyl(0.045, 0.045, 0.45, 8, goldM()), x, y0 || 0.3, z); }
  function marker(parent, net, x, y, z) { return put(parent, sph(0.085, M(NET[net], { roughness: 0.35 })), x, y, z); }
  function texPlane(txt, w, h, fg, font, bg) {
    var cv = document.createElement("canvas"); cv.width = 256; cv.height = Math.round(256 * h / w);
    var x = cv.getContext("2d"); if (bg) { x.fillStyle = bg; x.fillRect(0, 0, cv.width, cv.height); }
    x.fillStyle = fg; x.font = font; x.textAlign = "center"; x.textBaseline = "middle";
    var lines = txt.split("\n"), fs = parseInt(font.match(/\d+/)[0], 10);
    lines.forEach(function (l, i) { x.fillText(l, cv.width / 2, cv.height / 2 + (i - (lines.length - 1) / 2) * fs * 1.18); });
    var m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ map: new THREE.CanvasTexture(cv), transparent: true }));
    m.rotation.x = -PI / 2; return m;
  }

  var BUILD = {};

  /* ESP32 DevKitC: long axis is local x, USB at -x, antenna at +x. Left header = local z -1.22. */
  BUILD.esp32 = function () {
    var g = newPart();
    put(g, box(4.95, 0.16, 2.8, M(C.pcbBlack, { roughness: 0.52 })), -0.075, 0.14, 0);
    put(g, box(0.63, 0.16, 1.8, M(C.white, { roughness: 0.6 })), 2.715, 0.14, 0);              // antenna plate
    for (var k = 0; k < 6; k++) put(g, box(0.5, 0.03, 0.07, M(C.gold, { metalness: 0.7, roughness: 0.4 })), 2.7, 0.23, -0.55 + k * 0.22);
    var pins = new THREE.InstancedMesh(new THREE.CylinderGeometry(0.045, 0.045, 0.52, 6), goldM(), 38);
    pins.castShadow = true;
    var d = new THREE.Object3D(), i = 0;
    for (var r = 0; r < 2; r++) for (var c = 0; c < 19; c++) { d.position.set((9 - c) * 0.254, 0.46, r ? 1.22 : -1.22); d.updateMatrix(); pins.setMatrixAt(i++, d.matrix); }
    g.add(pins);
    put(g, box(4.85, 0.2, 0.26, M(C.dark)), 0, 0.32, -1.22);
    put(g, box(4.85, 0.2, 0.26, M(C.dark)), 0, 0.32, 1.22);
    put(g, box(1.85, 0.26, 1.5, M(C.silver, { metalness: 0.88, roughness: 0.3 })), 1.2, 0.35, 0);   // radio can
    put(g, box(1.65, 0.02, 1.3, M(C.chrome, { metalness: 0.9, roughness: 0.45 })), 1.2, 0.49, 0);
    put(g, texPlane("ESP32\nWROOM-32", 1.5, 0.8, "#2a2f35", "bold 54px sans-serif"), 1.2, 0.51, 0);
    put(g, box(0.78, 0.34, 0.62, M(C.chrome, { metalness: 0.9, roughness: 0.22 })), -2.6, 0.39, 0);  // micro-USB
    put(g, box(0.55, 0.1, 0.55, M(C.dark)), -0.9, 0.25, 0);
    put(g, box(0.36, 0.2, 0.36, M(C.dark)), -2.0, 0.32, -0.85);
    put(g, box(0.36, 0.2, 0.36, M(C.dark)), -2.0, 0.32, 0.85);
    g.userData.led = put(g, box(0.2, 0.08, 0.12, M(0x441111, { emissive: new THREE.Color(0x000000) })), -0.1, 0.26, 0.9);
    var L = { "3V3": 0, "GPIO33": 7, "GPIO25": 8, "GPIO14": 11, "GND": 13, "5V": 18 }, R = { "GPIO22": 2, "GPIO21": 5, "GPIO19": 7 };
    Object.keys(L).forEach(function (n) { var x = (9 - L[n]) * 0.254; anchor(g, g, n, x, 0.85, -1.22); marker(g, PIN_NET[n], x, 0.78, -1.22); });
    Object.keys(R).forEach(function (n) { var x = (9 - R[n]) * 0.254; anchor(g, g, n, x, 0.85, 1.22); marker(g, PIN_NET[n], x, 0.78, 1.22); });
    anchor(g, g, "USB", -3.05, 0.4, 0); anchor(g, g, "ANT", 2.8, 0.6, 0);
    return g;
  };

  /* OLED on a small foot, tilted back so the face reads from the front */
  BUILD.oled = function () {
    var g = newPart();
    put(g, box(2.9, 0.25, 0.9, M(C.dark)), 0, 0.12, 0.35);
    var panel = new THREE.Group(); panel.position.set(0, 1.75, 0); panel.rotation.x = -0.3; g.add(panel);
    put(panel, box(2.7, 2.8, 0.12, M(C.oledBlue, { roughness: 0.55 })), 0, 0, 0);
    put(panel, box(2.3, 1.35, 0.05, M(0x1b1b1d, { roughness: 0.4 })), 0, 0.2, 0.085);
    var screenMat = new THREE.MeshBasicMaterial({ map: LIVE.tex || null, color: 0x0a0a0a });
    put(panel, mesh(new THREE.PlaneGeometry(2.08, 1.04), screenMat), 0, 0.2, 0.115);
    put(panel, box(1.1, 0.45, 0.07, M(C.pcbBlack)), 0, -1.1, 0.085);
    [[-1.15, 1.2], [1.15, 1.2], [-1.15, -1.2], [1.15, -1.2]].forEach(function (p) {
      var t = put(panel, mesh(new THREE.TorusGeometry(0.12, 0.045, 8, 20), goldM()), p[0], p[1], 0.08); t.castShadow = false;
    });
    var X = { VDD: -0.39, GND: -0.13, SCK: 0.11, SDA: 0.37 }, NETS = { VDD: "3V3", GND: "GND", SCK: "SCL", SDA: "SDA" };
    Object.keys(X).forEach(function (n) {
      put(panel, cyl(0.045, 0.045, 0.34, 8, goldM()), X[n], 1.5, 0);
      anchor(g, panel, n, X[n], 1.72, 0); marker(panel, NETS[n], X[n], 1.68, 0);
    });
    g.userData.screen = screenMat;
    return g;
  };

  BUILD.mic = function () {
    var g = newPart();
    put(g, cyl(0.75, 0.75, 0.12, 48, M(0x151515, { roughness: 0.6 })), 0, 0.1, 0);
    var ring = put(g, mesh(new THREE.TorusGeometry(0.72, 0.03, 8, 48), goldM()), 0, 0.17, 0); ring.rotation.x = PI / 2;
    put(g, box(0.3, 0.1, 0.17, M(0xeeeeee, { metalness: 0.3, roughness: 0.4 })), -0.3, 0.2, 0);
    put(g, cyl(0.07, 0.07, 0.06, 16, M(0x050505)), 0, 0.2, 0);
    put(g, cyl(0.1, 0.1, 0.1, 16, M(0x888888, { metalness: 0.6, roughness: 0.4 })), 0.4, 0.2, 0.1);
    var P = { "SCK": [-0.28, -0.37, "BCLK"], "WS": [-0.02, -0.37, "WS"], "L/R": [0.23, -0.37, "GND"], "SD": [-0.26, 0.38, "MIC"], "VDD": [-0.01, 0.38, "3V3"], "GND": [0.24, 0.38, "GND"] };
    Object.keys(P).forEach(function (n) {
      pinPost(g, P[n][0], P[n][1], 0.3); anchor(g, g, n, P[n][0], 0.58, P[n][1]); marker(g, P[n][2], P[n][0], 0.52, P[n][1]);
    });
    anchor(g, g, "TOP", 0, 0.3, 0);
    return g;
  };

  BUILD.amp = function () {
    var g = newPart();
    put(g, box(1.8, 0.12, 1.8, M(C.pcbBlue, { roughness: 0.5 })), 0, 0.1, 0);
    put(g, box(0.85, 0.55, 0.5, M(0x1e5bd8, { roughness: 0.5 })), 0, 0.42, -0.55);
    [-0.19, 0.16].forEach(function (x) { put(g, cyl(0.13, 0.13, 0.08, 20, M(C.chrome, { metalness: 0.9, roughness: 0.3 })), x, 0.72, -0.55); });
    put(g, box(0.3, 0.07, 0.3, M(C.dark)), 0, 0.2, 0.2);
    put(g, box(0.18, 0.08, 0.1, M(C.dark)), 0.5, 0.2, -0.05);
    [[-0.78, -0.78], [0.78, -0.78]].forEach(function (p) { var t = put(g, mesh(new THREE.TorusGeometry(0.11, 0.04, 8, 20), goldM()), p[0], 0.17, p[1]); t.rotation.x = PI / 2; t.castShadow = false; });
    put(g, texPlane("MAX 98357A\nI2S Amp", 1.0, 0.5, "#ffffff", "bold 40px sans-serif"), -0.3, 0.17, 0.05);
    var X = { LRC: -0.75, BCLK: -0.5, DIN: -0.26, GAIN: 0, SD: 0.24, GND: 0.48, Vin: 0.74 }, NETS = { LRC: "WS", BCLK: "BCLK", DIN: "SPK", GND: "GND", Vin: "5V" };
    Object.keys(X).forEach(function (n) {
      pinPost(g, X[n], 0.72, 0.3);
      if (NETS[n]) { anchor(g, g, n, X[n], 0.58, 0.72); marker(g, NETS[n], X[n], 0.52, 0.72); }
    });
    anchor(g, g, "VO-", -0.19, 0.82, -0.55); anchor(g, g, "VO+", 0.16, 0.82, -0.55);
    marker(g, "VO-", -0.19, 0.8, -0.35); marker(g, "VO+", 0.16, 0.8, -0.35);
    return g;
  };

  BUILD.spk = function () {
    var g = newPart();
    put(g, box(4.0, 0.3, 4.0, M(C.chrome, { metalness: 0.85, roughness: 0.35 })), 0, 0.15, 0);
    put(g, cyl(1.78, 1.78, 0.25, 48, M(C.slate, { metalness: 0.55, roughness: 0.4 })), 0, 0.4, 0);
    var sur = put(g, mesh(new THREE.TorusGeometry(1.5, 0.13, 10, 48), M(0x16181b, { roughness: 0.85 })), 0, 0.55, 0); sur.rotation.x = PI / 2;
    g.userData.cone = put(g, cyl(1.35, 1.2, 0.1, 48, M(C.cone, { metalness: 0.7, roughness: 0.35 })), 0, 0.56, 0);
    put(g, cyl(0.6, 0.6, 0.06, 32, M(0xaeb4bb, { metalness: 0.8, roughness: 0.3 })), 0, 0.64, 0);
    [[-1.65, -1.65], [1.65, -1.65], [-1.65, 1.65], [1.65, 1.65]].forEach(function (p) { put(g, cyl(0.2, 0.2, 0.32, 16, M(0x101214)), p[0], 0.15, p[1]); });
    put(g, box(0.3, 0.15, 0.45, goldM()), -0.5, 0.3, 2.1); put(g, box(0.3, 0.15, 0.45, goldM()), 0.5, 0.3, 2.1);
    anchor(g, g, "-", -0.5, 0.55, 2.15); anchor(g, g, "+", 0.5, 0.55, 2.15);
    marker(g, "VO-", -0.5, 0.45, 1.85); marker(g, "VO+", 0.5, 0.45, 1.85);
    anchor(g, g, "CONE", 0, 0.7, 0);
    return g;
  };

  BUILD.jump = function () {
    var g = newPart();
    var cols = ["3V3", "GND", "SCL", "SDA", "BCLK", "WS", "MIC", "SPK"].map(function (n) { return NET[n]; });
    cols.forEach(function (c, i) {
      var z = -1.7 + i * 0.5;
      var curve = new THREE.QuadraticBezierCurve3(new THREE.Vector3(-3.4, 0.1, z), new THREE.Vector3(0, 1.2 + (i % 3) * 0.3, z + (i % 2 ? 0.7 : -0.7)), new THREE.Vector3(3.4, 0.1, z + 0.2));
      g.add(mesh(new THREE.TubeGeometry(curve, 30, 0.07, 8, false), M(c, { roughness: 0.5 })));
      [-3.4, 3.4].forEach(function (x, j) {
        put(g, box(0.42, 0.2, 0.2, M(0x1a1c20)), x, 0.12, z + (j ? 0.2 : 0));
        put(g, cyl(0.04, 0.04, 0.5, 6, M(C.silver, { metalness: 0.8, roughness: 0.3 })), x + (x < 0 ? -0.45 : 0.45), 0.12, z + (j ? 0.2 : 0), 0, 0, PI / 2);
      });
    });
    return g;
  };

  BUILD.usb = function () {
    var g = newPart();
    var pts = [];
    for (var t = 0; t <= 40; t++) { var u = t / 40; pts.push(new THREE.Vector3(-2.2 + u * 4.4, 0.3 + Math.sin(u * PI * 3) * 0.35 + 0.25, Math.cos(u * PI * 3) * 1.1)); }
    g.add(mesh(new THREE.TubeGeometry(new THREE.CatmullRomCurve3(pts), 80, 0.15, 8, false), M(C.dark, { roughness: 0.82 })));
    put(g, box(0.9, 0.5, 0.6, M(0x2d3036, { roughness: 0.5 })), -2.9, 0.52, pts[0].z);
    put(g, box(0.5, 0.2, 0.4, M(C.chrome, { metalness: 0.9, roughness: 0.25 })), -3.55, 0.52, pts[0].z);
    put(g, box(1.3, 0.55, 0.55, M(0x2d3036, { roughness: 0.5 })), 2.9, 0.52, pts[40].z);
    put(g, box(1.0, 0.45, 0.4, M(C.chrome, { metalness: 0.9, roughness: 0.25 })), 3.9, 0.52, pts[40].z);
    return g;
  };

  /* ---------- instantiate, place, keep both poses ---------- */
  var world = new THREE.Group(); scene.add(world);
  var built = {};
  LIVE.tex = new THREE.CanvasTexture(document.getElementById("oled"));
  LIVE.tex.minFilter = THREE.NearestFilter; LIVE.tex.magFilter = THREE.NearestFilter;
  var pinLabels = [];
  PARTS.forEach(function (p) {
    var g = BUILD[p.id]();
    g.userData.part = p.id;
    var m = p.m, a = p.a;
    g.userData.poses = {
      manifest: { p: new THREE.Vector3(m.p[0], m.p[1], m.p[2]), r: new THREE.Euler().fromArray(m.r || [0, 0, 0]), s: m.s || 1 },
      assembled: { p: new THREE.Vector3(a.p[0], a.p[1], a.p[2]), r: new THREE.Euler().fromArray(a.r || [0, 0, 0]), s: a.s || 1 }
    };
    var lb = new THREE.Box3().setFromObject(g);
    g.userData.lcenter = lb.getCenter(new THREE.Vector3());
    g.userData.ltop = new THREE.Vector3(g.userData.lcenter.x, lb.max.y, g.userData.lcenter.z);
    g.userData.lspan = lb.getSize(new THREE.Vector3()).length();
    var pose = g.userData.poses.manifest;
    g.position.copy(pose.p); g.rotation.copy(pose.r); g.scale.setScalar(pose.s);
    built[p.id] = g; world.add(g);

    var lab = document.createElement("div"); lab.className = "pb-lab";
    lab.innerHTML = "<b>" + p.ref.split("–")[0] + "</b>&nbsp; " + p.name;
    lab.dataset.lift = (PARTS.indexOf(p) % 2) ? 20 : 34;
    labelsEl.appendChild(lab); g.userData.label = lab;

    Object.keys(g.userData.pins).forEach(function (n) {
      if (n === "TOP" || n === "ANT" || n === "CONE" || n === "USB") return;
      var el = document.createElement("div"); el.className = "pb-pin"; el.textContent = n; el.style.opacity = 0;
      labelsEl.appendChild(el); pinLabels.push({ el: el, obj: g.userData.pins[n], part: p.id });
    });
  });

  function setPose(g, name) {
    var pose = g.userData.poses[name];
    g.position.copy(pose.p); g.rotation.copy(pose.r); g.scale.setScalar(pose.s); g.updateMatrixWorld(true);
  }
  function asmPin(partId, pin) {
    var g = built[partId]; setPose(g, "assembled");
    var v = g.userData.pins[pin].getWorldPosition(new THREE.Vector3());
    setPose(g, "manifest"); return v;
  }

  /* ---------- wires, in the Cirkit colours ---------- */
  var WIRES = [
    ["3V3", "esp32", "3V3", "oled", "VDD", "3V3 → OLED VDD"], ["GND", "esp32", "GND", "oled", "GND", "GND → OLED GND"],
    ["SCL", "esp32", "GPIO19", "oled", "SCK", "GPIO19 → OLED SCK"], ["SDA", "esp32", "GPIO21", "oled", "SDA", "GPIO21 → OLED SDA"],
    ["3V3", "esp32", "3V3", "mic", "VDD", "3V3 → mic VDD"], ["GND", "esp32", "GND", "mic", "GND", "GND → mic GND"],
    ["GND", "esp32", "GND", "mic", "L/R", "GND → mic L/R"], ["BCLK", "esp32", "GPIO14", "mic", "SCK", "GPIO14 → mic SCK"],
    ["WS", "esp32", "GPIO25", "mic", "WS", "GPIO25 → mic WS"], ["MIC", "mic", "SD", "esp32", "GPIO33", "mic SD → GPIO33"],
    ["5V", "esp32", "5V", "amp", "Vin", "5V → amp Vin"], ["GND", "esp32", "GND", "amp", "GND", "GND → amp GND"],
    ["SPK", "esp32", "GPIO22", "amp", "DIN", "GPIO22 → amp DIN"], ["BCLK", "esp32", "GPIO14", "amp", "BCLK", "GPIO14 → amp BCLK"],
    ["WS", "esp32", "GPIO25", "amp", "LRC", "GPIO25 → amp LRC"],
    ["VO+", "amp", "VO+", "spk", "+", "amp VO+ → speaker +"], ["VO-", "amp", "VO-", "spk", "-", "amp VO− → speaker −"]
  ];
  var GROUP_ORDER = { oled: 0, mic: 1, amp: 2, spk: 3 };
  var wireGroup = new THREE.Group(); scene.add(wireGroup);
  var pulseGeo = new THREE.SphereGeometry(0.1, 10, 10);
  var wires = WIRES.map(function (d, idx) {
    var a = asmPin(d[1], d[2]), b = asmPin(d[3], d[4]);
    var dist = a.distanceTo(b), h = Math.min(1.7 + idx * 0.14, 1.0 + dist * 0.27 + idx * 0.03);
    var at = function (t) { return new THREE.Vector3(a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t); };
    var p1 = at(0.2), p2 = at(0.5), p3 = at(0.8);
    var pts = [a, a.clone().add(new THREE.Vector3(0, 0.55, 0)), new THREE.Vector3(p1.x, h, p1.z), new THREE.Vector3(p2.x, h + 0.08, p2.z), new THREE.Vector3(p3.x, h, p3.z), b.clone().add(new THREE.Vector3(0, 0.55, 0)), b];
    var curve = new THREE.CatmullRomCurve3(pts, false, "centripetal");
    var geo = new THREE.TubeGeometry(curve, 80, 0.055, 8, false);
    var wm = new THREE.MeshStandardMaterial({ color: NET[d[0]], roughness: 0.45, metalness: 0.05, transparent: true, opacity: 1 });
    var me = new THREE.Mesh(geo, wm); me.castShadow = true; me.visible = false; wireGroup.add(me);
    var grp = d[3] === "esp32" ? d[1] : (d[3] === "spk" ? "spk" : d[3]);
    if (d[1] === "amp" && d[3] === "spk") grp = "spk";
    var pulses = [0, 1, 2].map(function () {
      var pm = new THREE.Mesh(pulseGeo, new THREE.MeshBasicMaterial({ color: new THREE.Color(NET[d[0]]).lerp(new THREE.Color(0xffffff), 0.65) }));
      pm.visible = false; scene.add(pm); return pm;
    });
    var order = GROUP_ORDER[grp] || 0;
    return { net: d[0], text: d[5], curve: curve, mesh: me, mat: wm, cnt: geo.index.count, prog: 0, pulses: pulses, delay: 0.25 + order * 0.9 + (idx % 6) * 0.1 };
  });

  /* USB plug-in cable: from the ESP32 USB socket out to the bench edge */
  var usbDyn = new THREE.Group(); scene.add(usbDyn);
  (function () {
    var s = asmPin("esp32", "USB");
    var curve = new THREE.CatmullRomCurve3([s.clone().add(new THREE.Vector3(0, 0, 0.5)), s.clone().add(new THREE.Vector3(0, -0.1, 3)), new THREE.Vector3(s.x + 1, 0.2, 7), new THREE.Vector3(s.x + 5, 0.2, 10)]);
    usbDyn.add(mesh(new THREE.TubeGeometry(curve, 40, 0.14, 8, false), M(C.dark, { roughness: 0.82 })));
    put(usbDyn, box(0.6, 0.55, 0.9, M(0x2d3036, { roughness: 0.5 })), s.x, s.y - 0.3, s.z + 0.5);
    put(usbDyn, box(0.4, 0.2, 0.5, M(C.chrome, { metalness: 0.9, roughness: 0.25 })), s.x, s.y - 0.3, s.z - 0.1);
  })();
  usbDyn.visible = false;
  var usbProg = 0;

  /* ---------- signal effects ---------- */
  function mkRings(color) {
    var arr = [];
    for (var i = 0; i < 3; i++) {
      var m = new THREE.Mesh(new THREE.RingGeometry(0.93, 1, 56), new THREE.MeshBasicMaterial({ color: color, transparent: true, opacity: 0, side: THREE.DoubleSide, depthWrite: false }));
      m.rotation.x = -PI / 2; m.visible = false; scene.add(m); arr.push(m);
    }
    return arr;
  }
  function updRings(arr, on, center, maxR, inward, t, speed) {
    arr.forEach(function (m, i) {
      if (!on) { m.visible = false; return; }
      var u = (t * speed + i / arr.length) % 1;
      m.visible = true; m.position.copy(center); m.scale.setScalar((inward ? maxR * (1 - u) : maxR * u) + 0.25);
      m.material.opacity = (inward ? u : 1 - u) * 0.85;
    });
  }
  var wifiRings = mkRings(0xffb347), micRings = mkRings(0xffffff), spkRings = mkRings(0x38bdf8);

  /* ---------- theme ---------- */
  function paint() {
    var ground = new THREE.Color(tok("--mat"));
    var dark = (ground.r * 0.299 + ground.g * 0.587 + ground.b * 0.114) < 0.45;
    renderer.setClearColor(ground, 1);
    scene.fog = new THREE.Fog(ground, 60, 150);
    mat.material.color.copy(ground).offsetHSL(0, 0, dark ? 0.02 : -0.024);
    grid.material.color.setRGB(1, 1, 1);
    var gc = new THREE.Color(tok("--grid")), arr = grid.geometry.attributes.color.array;
    for (var i = 0; i < arr.length; i += 3) { arr[i] = gc.r; arr[i + 1] = gc.g; arr[i + 2] = gc.b; }
    grid.geometry.attributes.color.needsUpdate = true;
  }
  paint();
  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", paint);
  new MutationObserver(paint).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

  /* ---------- camera: hand-rolled orbit ---------- */
  var HOMES = {
    manifest: { az: 0.3, pol: 0.95, rad: 36, tgt: new THREE.Vector3(-1.5, 1.2, 0.5) },
    assembled: { az: 0.18, pol: 0.9, rad: 26, tgt: new THREE.Vector3(3.6, 0.8, -2.8) }
  };
  var cam = { az: HOMES.manifest.az, pol: HOMES.manifest.pol, rad: HOMES.manifest.rad, tgt: HOMES.manifest.tgt.clone() };
  var want = { az: cam.az, pol: cam.pol, rad: cam.rad, tgt: cam.tgt.clone() };
  function fitRad(base) { return base * Math.max(1, 1.5 / (camera.aspect || 1.5)); }
  function applyCamera() {
    var sp = Math.sin(cam.pol), cp = Math.cos(cam.pol);
    camera.position.set(cam.tgt.x + cam.rad * sp * Math.sin(cam.az), cam.tgt.y + cam.rad * cp, cam.tgt.z + cam.rad * sp * Math.cos(cam.az));
    camera.lookAt(cam.tgt);
  }
  var dragging = false, lastX = 0, lastY = 0, userMoved = false;
  viewEl.addEventListener("pointerdown", function (e) {
    if (e.pointerType === "mouse" && e.button !== 0) return;
    dragging = true; userMoved = false; lastX = e.clientX; lastY = e.clientY;
    viewEl.classList.add("grabbing"); viewEl.setPointerCapture(e.pointerId);
  });
  viewEl.addEventListener("pointermove", function (e) {
    if (!dragging) return;
    var dx = e.clientX - lastX, dy = e.clientY - lastY;
    if (Math.abs(dx) + Math.abs(dy) > 3) userMoved = true;
    lastX = e.clientX; lastY = e.clientY;
    want.az -= dx * 0.0062; want.pol = Math.max(0.16, Math.min(1.46, want.pol - dy * 0.0055));
  });
  function endDrag(e) {
    if (!dragging) return; dragging = false; viewEl.classList.remove("grabbing");
    try { viewEl.releasePointerCapture(e.pointerId); } catch (err) { }
    if (!userMoved) pick(e);
  }
  viewEl.addEventListener("pointerup", endDrag); viewEl.addEventListener("pointercancel", endDrag);
  viewEl.addEventListener("wheel", function (e) {
    e.preventDefault(); want.rad = Math.max(6, Math.min(90, want.rad * (1 + Math.sign(e.deltaY) * 0.1)));
  }, { passive: false });

  /* ---------- picking, selection, modes ---------- */
  var selected = null, mode = "manifest", spin = !RM, showPins = false;
  var ray = new THREE.Raycaster(), ndc = new THREE.Vector2();
  function pick(e) {
    var r = renderer.domElement.getBoundingClientRect();
    ndc.x = ((e.clientX - r.left) / r.width) * 2 - 1; ndc.y = -((e.clientY - r.top) / r.height) * 2 + 1;
    ray.setFromCamera(ndc, camera);
    var hits = ray.intersectObjects(world.children, true);
    if (!hits.length) { select(null); return; }
    var o = hits[0].object; while (o && !o.userData.part) o = o.parent;
    select(o ? o.userData.part : null);
  }
  var tween = { from: "manifest", to: "manifest", t: 1, dur: RM ? 0.001 : 0.95 };
  var asmT = 0;
  function homeFor(m) { var h = HOMES[m]; want.tgt.copy(h.tgt); want.rad = fitRad(h.rad); }
  function setMode(next) {
    if (next === mode) return;
    tween.from = mode; tween.to = next; tween.t = 0; mode = next; asmT = 0;
    $$("#b-manifest").setAttribute("aria-pressed", String(next === "manifest"));
    $$("#b-assembled").setAttribute("aria-pressed", String(next === "assembled"));
    $$("#legend-wrap").hidden = next !== "assembled";
    if (!selected) homeFor(next); else frame(selected);
  }
  var _m4 = new THREE.Matrix4(), _q = new THREE.Quaternion(), _sv = new THREE.Vector3();
  function frame(id) {
    var g = built[id], pose = g.userData.poses[mode];
    _q.setFromEuler(pose.r); _sv.setScalar(pose.s); _m4.compose(pose.p, _q, _sv);
    want.tgt.copy(g.userData.lcenter).applyMatrix4(_m4);
    want.rad = Math.max(7, g.userData.lspan * pose.s * 1.9);
  }
  function select(id) {
    if (id && mode === "assembled" && built[id].userData.poses.assembled.s < 0.01) setMode("manifest");
    selected = id;
    Array.prototype.forEach.call(rowsEl.children, function (b) { b.setAttribute("aria-pressed", String(b.dataset.id === id)); });
    var chipEl = $$("#cap-chips3"); chipEl.innerHTML = "";
    if (!id) {
      $$("#cap-name3").textContent = "All seven parts";
      $$("#cap-note3").innerHTML = "Laid out on the bench in manifest order. Switch to <b>Assembled</b> to watch them travel into the Cirkit layout and get wired, or pick a line from the list to inspect one part.";
      $$("#cap-pins3").textContent = "17 wires"; $$("#cap-pinslabel3").textContent = "11 signals";
      homeFor(mode); return;
    }
    var p = PARTS.filter(function (x) { return x.id === id; })[0];
    $$("#cap-name3").textContent = p.name; $$("#cap-note3").textContent = p.note;
    var sc = document.createElement("span"); sc.className = "pb-chip pb-stage-tag"; sc.textContent = p.stage; chipEl.appendChild(sc);
    [p.ref, p.qty].concat(p.tags).forEach(function (t) { var c = document.createElement("span"); c.className = "pb-chip"; c.textContent = t; chipEl.appendChild(c); });
    $$("#cap-pins3").textContent = p.pins; $$("#cap-pinslabel3").textContent = p.ref.split("–")[0] + " · pins";
    frame(id);
  }
  $$("#b-manifest").addEventListener("click", function () { setMode("manifest"); });
  $$("#b-assembled").addEventListener("click", function () { setMode("assembled"); });
  $$("#b-reset").addEventListener("click", function () { var h = HOMES[mode]; want.az = h.az; want.pol = h.pol; select(null); });
  var spinBtn = $$("#b-spin"), pinBtn = $$("#b-pins");
  function paintSpin() { spinBtn.setAttribute("aria-pressed", String(spin)); spinBtn.innerHTML = "Spin ·&nbsp;" + (spin ? "on" : "off"); }
  paintSpin(); spinBtn.addEventListener("click", function () { spin = !spin; paintSpin(); });
  pinBtn.addEventListener("click", function () { showPins = !showPins; pinBtn.setAttribute("aria-pressed", String(showPins)); pinBtn.innerHTML = "Pin names ·&nbsp;" + (showPins ? "on" : "off"); });

  /* wire legend: click one to isolate it */
  var isolated = null, legend = $$("#legend3");
  Object.keys(NET).forEach(function (n) {
    var b = document.createElement("button"); b.className = "chip"; b.type = "button";
    b.innerHTML = '<i class="dot" style="background:' + NET[n] + '"></i>' + n + " · " + NETINFO[n];
    b.onclick = function () { isolated = isolated === n ? null : n; Array.prototype.forEach.call(legend.children, function (c) { c.setAttribute("aria-pressed", String(c === b && isolated !== null)); }); };
    legend.appendChild(b);
  });
  $$("#legend-wrap").hidden = true;

  /* talk panel */
  var pending = null;
  ["Tell me a fun fact", "I got the job!", "You look angry!", "I love you Pip", "Say goodnight"].forEach(function (t) {
    var b = document.createElement("button"); b.className = "chip"; b.type = "button"; b.textContent = t; b.onclick = function () { talk3(t); }; $$("#quick3").appendChild(b);
  });
  function talk3(text) { if (!text || !text.trim()) return; if (mode !== "assembled") setMode("assembled"); pending = text; }
  $$("#f3").onsubmit = function (e) { e.preventDefault(); talk3($$("#q3").value); };

  /* ---------- sizing ---------- */
  function resize() {
    var w = viewEl.clientWidth, h = viewEl.clientHeight; if (!w || !h) return;
    renderer.setSize(w, h, false); camera.aspect = w / h; camera.updateProjectionMatrix();
    if (!selected) want.rad = fitRad(HOMES[mode].rad);
  }
  new ResizeObserver(resize).observe(viewEl); resize();

  /* ---------- loop ---------- */
  var ease = function (t) { return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2; };
  var proj = new THREE.Vector3(), clock = new THREE.Clock(), tmp = new THREE.Vector3(), lastStatus = "";
  var STAGE_TEXT = [["mic", "The microphone hears you"], ["up", "Streaming your voice over WiFi"], ["stt", "Whisper turns speech into text"], ["llm", "The LLM picks a reply and an emotion"], ["tts", "Making the voice"], ["down", "Receiving the voice over WiFi"], ["amp", "The amplifier plays it, the face shows the emotion"]];
  function speed(net, powered) {
    var m = typeof waveMode !== "undefined" ? waveMode : "idle", busy = typeof S !== "undefined" && S.busy;
    switch (net) {
      case "3V3": case "5V": return powered ? 0.6 : 0;
      case "SDA": case "SCL": return powered ? (busy ? 1.4 : 0.5) : 0;
      case "BCLK": case "WS": return powered && m !== "idle" ? 1.8 : 0;
      case "MIC": return m === "mic" ? 1.6 : 0;
      case "SPK": return m === "spk" ? 1.6 : 0;
      case "VO+": case "VO-": return m === "spk" ? 2.4 : 0;
      default: return 0;
    }
  }
  var simClock = performance.now();
  function step(dt) {
    var now = (simClock += dt * 1000), t = now / 1000;

    if (tween.t < 1) {
      tween.t = Math.min(1, tween.t + dt / tween.dur);
      var k = ease(tween.t);
      PARTS.forEach(function (p) {
        var g = built[p.id], A = g.userData.poses[tween.from], B = g.userData.poses[tween.to];
        g.position.lerpVectors(A.p, B.p, k);
        g.rotation.set(A.r.x + (B.r.x - A.r.x) * k, A.r.y + (B.r.y - A.r.y) * k, A.r.z + (B.r.z - A.r.z) * k);
        g.scale.setScalar(A.s + (B.s - A.s) * k);
      });
    }
    var asm = mode === "assembled" && tween.t >= 1;
    asmT = asm ? asmT + dt : 0;
    var wantUsb = asmT > 4.8 ? 1 : 0;
    usbProg += Math.sign(wantUsb - usbProg) * Math.min(Math.abs(wantUsb - usbProg), dt * (wantUsb ? 1.1 : 3));
    usbDyn.visible = usbProg > 0.001;
    usbDyn.scale.set(1, 1, 1); usbDyn.position.z = (1 - ease(usbProg)) * 4;
    var powered = usbProg > 0.95;
    var led = built.esp32.userData.led;
    led.material.color.set(powered ? 0xff3030 : 0x441111); led.material.emissive.set(powered ? 0xff2020 : 0x000000);
    var scr = built.oled.userData.screen;
    scr.color.set(powered ? 0xffffff : 0x0a0a0a); if (powered) LIVE.tex.needsUpdate = true;

    var A = typeof ACT !== "undefined" ? ACT : {}, wm = typeof waveMode !== "undefined" ? waveMode : "idle";
    wires.forEach(function (w) {
      var target = asmT > w.delay ? 1 : 0;
      w.prog += Math.sign(target - w.prog) * Math.min(Math.abs(target - w.prog), dt * (target ? 1.4 : 3));
      w.mesh.visible = w.prog > 0.001;
      w.mesh.geometry.setDrawRange(0, Math.floor(w.cnt * w.prog / 3) * 3);
      var dim = isolated && isolated !== w.net;
      w.mat.opacity = dim ? 0.12 : 1;
      var sp = w.prog > 0.99 && !dim ? speed(w.net, powered) : 0;
      w.pulses.forEach(function (m, i) {
        if (!sp) { m.visible = false; return; }
        m.visible = true; m.position.copy(w.curve.getPointAt((t * sp * 0.45 + i / 3) % 1));
      });
    });
    var wifi = !!(A.up || A.down) && powered;
    built.esp32.userData.pins.ANT.getWorldPosition(tmp);
    updRings(wifiRings, wifi, tmp.clone().setY(0.5), 4, !!A.down, t, 0.9);
    built.mic.userData.pins.TOP.getWorldPosition(tmp);
    updRings(micRings, wm === "mic" && asm, tmp.clone().setY(0.4), 2.6, true, t, 0.9);
    built.spk.userData.pins.CONE.getWorldPosition(tmp);
    updRings(spkRings, wm === "spk" && asm, tmp.clone().setY(0.5), 4.2, false, t, 0.9);
    var amp = (typeof S !== "undefined" && S.talking) ? S.talkAmp : 0, cone = built.spk.userData.cone;
    cone.position.y = 0.56 + amp * Math.sin(now / 24) * 0.12;

    if (pending && powered && typeof S !== "undefined" && !S.busy) { var tx = pending; pending = null; converse(tx); }
    var status = "Idle";
    if (pending) status = "Wiring it up first";
    for (var si = 0; si < STAGE_TEXT.length; si++) if (A[STAGE_TEXT[si][0]]) { status = STAGE_TEXT[si][1]; break; }
    if (status !== lastStatus) { $$("#status3").textContent = status; lastStatus = status; }
    $$("#talk3").disabled = typeof S !== "undefined" && S.busy;

    if (spin && !dragging) want.az += dt * 0.07;
    var s = 1 - Math.pow(0.0015, dt);
    cam.az += (want.az - cam.az) * s; cam.pol += (want.pol - cam.pol) * s; cam.rad += (want.rad - cam.rad) * s; cam.tgt.lerp(want.tgt, s);
    applyCamera();
  }
  function paintFrame() {
    renderer.render(scene, camera);
    var asm = mode === "assembled" && tween.t >= 1;
    /* labels ride on the parts */
    var r = renderer.domElement.getBoundingClientRect();
    PARTS.forEach(function (p) {
      var g = built[p.id], lab = g.userData.label;
      var gone = mode === "assembled" && g.userData.poses.assembled.s < 0.01;
      var show = !gone && ((mode === "manifest" && !selected) || selected === p.id);
      if (!show) { lab.style.opacity = 0; return; }
      proj.copy(g.userData.ltop).applyMatrix4(g.matrixWorld).project(camera);
      if (proj.z > 1) { lab.style.opacity = 0; return; }
      lab.style.opacity = 1;
      if (!lab._hw) lab._hw = lab.offsetWidth / 2 + 6;
      var lx = (proj.x * 0.5 + 0.5) * r.width;
      lab.style.left = Math.max(lab._hw, Math.min(r.width - lab._hw, lx)) + "px";
      lab.style.top = ((-proj.y * 0.5 + 0.5) * r.height - lab.dataset.lift) + "px";
    });
    pinLabels.forEach(function (pl) {
      var on = (showPins && mode === "assembled" && asm) || selected === pl.part;
      if (!on) { pl.el.style.opacity = 0; return; }
      pl.obj.getWorldPosition(tmp).project(camera);
      if (tmp.z > 1) { pl.el.style.opacity = 0; return; }
      pl.el.style.opacity = 1;
      pl.el.style.left = ((tmp.x * 0.5 + 0.5) * r.width) + "px";
      pl.el.style.top = ((-tmp.y * 0.5 + 0.5) * r.height - 18) + "px";
    });
  }
  function tick() {
    requestAnimationFrame(tick);
    if ($$("#p-3d").hidden) { clock.getDelta(); return; }
    step(Math.min(clock.getDelta(), 0.05));
    paintFrame();
  }
  LIVE.select = select;
  select(null);
  tick();
  // advance(seconds) runs the simulation ahead without drawing, used by automated checks
  window.pip3d = { setMode: setMode, select: select, advance: function (sec) { for (var i = 0; i < sec * 30; i++) step(1 / 30); } };
})();
