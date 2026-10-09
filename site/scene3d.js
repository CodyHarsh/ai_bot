/* 3D build of the Pip circuit. Layout, pin positions and wire colours follow the Cirkit Designer project:
   OLED top centre, ESP32 DevKitC centre (antenna up, USB down), INMP441 left, MAX98357A and speaker right.
   Units: 1 unit is about 5.5 mm of the Cirkit canvas scaled up. x = right, z = towards the viewer, y = up. */
(function () {
  const wrap = document.getElementById('threeWrap');
  const THREE = window.THREE;
  if (!THREE || !THREE.OrbitControls) { wrap.innerHTML = '<p class="sub" style="padding:20px">The 3D library did not load. Check your connection and reload.</p>'; return; }
  let renderer;
  try { renderer = new THREE.WebGLRenderer({ antialias: true }); }
  catch (e) { wrap.innerHTML = '<p class="sub" style="padding:20px">This browser cannot run WebGL, so the 3D view is unavailable.</p>'; return; }
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputEncoding = THREE.sRGBEncoding;
  wrap.appendChild(renderer.domElement);

  const $$ = s => document.querySelector(s);
  const V = (x, y, z) => new THREE.Vector3(x, y, z);
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 300);
  const controls = new THREE.OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true; controls.maxPolarAngle = Math.PI * 0.49; controls.minDistance = 6; controls.maxDistance = 70;
  const VIEWS = {
    iso: { pos: V(3, 21, 27), target: V(4, 1, -4) },
    top: { pos: V(4, 40, 0.1), target: V(4, 0, -3.5) },
    front: { pos: V(4, 7, 34), target: V(4, 2, -2) }
  };
  camera.position.copy(VIEWS.iso.pos); controls.target.copy(VIEWS.iso.target);
  let camGoal = null;
  controls.addEventListener('start', () => { camGoal = null; });

  scene.add(new THREE.HemisphereLight(0xffffff, 0x445566, 0.75));
  const sun = new THREE.DirectionalLight(0xffffff, 0.85); sun.position.set(12, 26, 16); scene.add(sun);
  const fill = new THREE.DirectionalLight(0x9bb7ff, 0.25); fill.position.set(-14, 10, -10); scene.add(fill);

  const M = (c, r = 0.6, m = 0.05) => new THREE.MeshStandardMaterial({ color: c, roughness: r, metalness: m });
  const gold = M(0xd9ab3a, 0.35, 0.9), silver = M(0xc3c8ce, 0.35, 0.85), blackM = M(0x16181b, 0.7, 0.1);
  function add(parent, geo, mat, x, y, z) { const m = new THREE.Mesh(geo, mat); m.position.set(x, y, z); parent.add(m); return m; }
  const B = (w, h, d) => new THREE.BoxGeometry(w, h, d);
  const C = (rt, rb, h, s = 32) => new THREE.CylinderGeometry(rt, rb, h, s);

  function textTexture(txt, w, h, fg, bg, font) {
    const c = document.createElement('canvas'); c.width = w; c.height = h; const x = c.getContext('2d');
    if (bg) { x.fillStyle = bg; x.fillRect(0, 0, w, h); }
    x.fillStyle = fg; x.font = font; x.textAlign = 'center'; x.textBaseline = 'middle';
    txt.split('\n').forEach((l, i, a) => x.fillText(l, w / 2, h / 2 + (i - (a.length - 1) / 2) * parseInt(font.match(/\d+/)[0]) * 1.15));
    const t = new THREE.CanvasTexture(c); t.anisotropy = 4; return t;
  }
  function label(txt, scale, fg, bg) {
    const c = document.createElement('canvas'); c.width = 256; c.height = 64; const x = c.getContext('2d');
    x.font = 'bold 30px monospace'; const w = Math.min(248, x.measureText(txt).width + 26);
    x.fillStyle = bg || 'rgba(8,16,13,.78)'; x.fillRect((256 - w) / 2, 8, w, 48);
    x.fillStyle = fg || '#fff'; x.textAlign = 'center'; x.textBaseline = 'middle'; x.fillText(txt, 128, 34);
    const sp = new THREE.Sprite(new THREE.SpriteMaterial({ map: new THREE.CanvasTexture(c), depthTest: false, transparent: true }));
    sp.scale.set(4 * scale, scale, 1); sp.renderOrder = 20; return sp;
  }
  function silk(txt, w, h, fg, font) {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ map: textTexture(txt, 256, Math.round(256 * h / w), fg, null, font), transparent: true }));
    m.rotation.x = -Math.PI / 2; return m;
  }

  /* ---------- base ---------- */
  add(scene, B(38, 0.6, 26), M(0xe7ebe8, 0.9, 0), 3.5, -0.3, -2.5);
  const grid = new THREE.GridHelper(38, 38, 0xb9c3be, 0xd3dad6); grid.position.set(3.5, 0.01, -2.5); scene.add(grid);

  const parts = {};
  function part(key, name, step, info) {
    const g = new THREE.Group(); scene.add(g);
    const p = { key, name, step, info, g, prog: 0, labels: new THREE.Group(), pinLabels: new THREE.Group() };
    g.add(p.labels, p.pinLabels); parts[key] = p; return p;
  }

  /* ---------- ESP32 DevKitC ---------- */
  const pE = part('esp', 'ESP32 DevKitC', 0, 'The brain. Runs the firmware, joins WiFi, reads the mic and drives the speaker over I2S, and draws the face on the OLED. Pins used: 3V3, 5V, GND, GPIO14, 25, 33, 22, 21, 19.');
  { const g = pE.g;
    add(g, B(4.8, 0.2, 8.45), M(0x14171a, 0.6, 0.1), 0, 0.35, 0.22);
    add(g, B(3.1, 0.2, 1.1), M(0xe9e9e4, 0.7, 0), 0, 0.35, -4.55);
    for (let i = 0; i < 4; i++) add(g, B(2.4, 0.04, 0.07), gold, 0, 0.47, -4.9 + i * 0.22);
    add(g, B(2.9, 0.5, 3.3), M(0xb9bec4, 0.35, 0.85), 0, 0.7, -2.25);
    const lg = silk('ESP32\nWROOM-32', 2.5, 1.2, '#2a2f35', 'bold 56px sans-serif'); lg.position.set(0, 0.97, -2.2); g.add(lg);
    add(g, B(1.4, 0.6, 1.2), silver, 0, 0.7, 4.15);
    add(g, B(0.9, 0.12, 0.9), blackM, 0, 0.5, 2.3);
    [-1.4, 1.4].forEach(x => { add(g, B(0.7, 0.3, 0.7), blackM, x, 0.55, 3.7); add(g, C(0.22, 0.22, 0.12, 16), M(0xdddddd, 0.5, 0.4), x, 0.75, 3.7); });
    pE.led = add(g, new THREE.SphereGeometry(0.1, 12, 12), new THREE.MeshStandardMaterial({ color: 0x551111, emissive: 0x000000 }), 1.6, 0.5, 1.2);
    const hdr = new THREE.InstancedMesh(B(0.2, 0.5, 0.2), gold, 38); const m4 = new THREE.Matrix4(); let k = 0;
    [-1, 1].forEach(s => { for (let i = 0; i < 19; i++) { m4.setPosition(s * 2.18, 0.7, -3.64 + 0.44 * i); hdr.setMatrixAt(k++, m4); } });
    g.add(hdr);
  }
  const espPin = (side, i) => V(side * 2.18, 1.0, -3.64 + 0.44 * i);
  const LPINS = ['3V3', 'EN', 'VP', 'VN', '34', '35', '32', '33', '25', '26', '27', '14', '12', 'GND', '13', 'D2', 'D3', 'CMD', '5V'];
  const RPINS = ['GND', '23', '22', 'TX', 'RX', '21', 'GND', '19', '18', '5', '17', '16', '4', '0', '2', '15', 'D1', 'D0', 'CLK'];

  /* ---------- colours: same as the wires in the Cirkit project ---------- */
  const NET = {
    '3V3': '#a0522d', '5V': '#ff7eb3', 'GND': '#ff12b4', 'BCLK': '#0e8f9c', 'WS': '#6b3a3a', 'MIC': '#00e5f5',
    'SPK': '#6ab04c', 'SDA': '#c3c3ff', 'SCL': '#ff8c00', 'VO+': '#7cfc00', 'VO-': '#9b8cf0'
  };
  const NETINFO = { '3V3': '3.3 V power', '5V': '5 V power', 'GND': 'Ground', 'BCLK': 'I2S bit clock', 'WS': 'I2S word select', 'MIC': 'Mic audio data in', 'SPK': 'Speaker audio data out', 'SDA': 'OLED data', 'SCL': 'OLED clock', 'VO+': 'Speaker +', 'VO-': 'Speaker −' };
  function cap(parent, v, net) { add(parent, new THREE.SphereGeometry(0.17, 12, 12), M(NET[net], 0.4, 0.1), v.x, v.y + 0.02, v.z); }
  function pinText(part, parentPos, txt) { const s = label(txt, 0.34); s.position.copy(parentPos); part.pinLabels.add(s); }

  // ESP32 pin caps and labels
  const USED_L = { 0: '3V3', 7: 'MIC', 8: 'WS', 11: 'BCLK', 13: 'GND', 18: '5V' }, USED_R = { 2: 'SPK', 5: 'SDA', 7: 'SCL' };
  Object.entries(USED_L).forEach(([i, n]) => { const v = espPin(-1, +i); cap(pE.g, v, n); pinText(pE, V(v.x - 1.1, 1.3, v.z), n === 'MIC' ? 'GPIO33' : n === 'WS' ? 'GPIO25' : n === 'BCLK' ? 'GPIO14' : LPINS[+i]); });
  Object.entries(USED_R).forEach(([i, n]) => { const v = espPin(1, +i); cap(pE.g, v, n); pinText(pE, V(v.x + 1.1, 1.3, v.z), 'GPIO' + RPINS[+i]); });

  /* ---------- OLED (top centre) ---------- */
  const pO = part('oled', 'OLED SSD1306', 1, 'A 128x64 I2C screen at address 0x3C. It shows Pip\'s face. Pins: VDD to 3V3, GND to GND, SCK to GPIO19, SDA to GPIO21.');
  const oledPanel = new THREE.Group();
  const screenTex = new THREE.CanvasTexture(document.getElementById('oled'));
  screenTex.minFilter = THREE.NearestFilter; screenTex.magFilter = THREE.NearestFilter;
  const screenMat = new THREE.MeshBasicMaterial({ map: screenTex, color: 0x0a0a0a });
  { const g = pO.g;
    pO.g.position.set(-0.2, 0, -9.1);
    add(g, B(4.6, 0.4, 1.3), M(0x2a2f35, 0.7, 0.1), 0, 0.2, 0.2);
    oledPanel.position.set(0, 2.9, 0); oledPanel.rotation.x = -0.3; g.add(oledPanel);
    add(oledPanel, B(4.6, 4.6, 0.2), M(0x1d4f86, 0.55, 0.1), 0, 0, 0);
    add(oledPanel, B(4.3, 2.4, 0.08), M(0x1b1b1d, 0.4, 0.1), 0, 0.33, 0.13);
    add(oledPanel, new THREE.PlaneGeometry(3.9, 1.95), screenMat, 0, 0.33, 0.18);
    add(oledPanel, B(1.8, 0.7, 0.12), blackM, 0, -1.85, 0.12);
    [[-2, 2], [2, 2], [-2, -2], [2, -2]].forEach(([x, y]) => { const t = add(oledPanel, new THREE.TorusGeometry(0.2, 0.07, 8, 20), gold, x, y, 0.1); });
    [-0.69, -0.24, 0.2, 0.65].forEach(x => add(oledPanel, C(0.09, 0.09, 0.35, 12), gold, x, 2.3, 0));
  }
  pO.g.updateMatrixWorld(true);
  const oledPin = x => oledPanel.localToWorld(V(x, 2.5, 0));

  /* ---------- INMP441 microphone (left) ---------- */
  const pM = part('mic', 'INMP441 mic', 2, 'A digital I2S microphone. L/R is tied to GND so it sends on the left channel. It listens for your voice and streams 24-bit samples to GPIO33.');
  { const g = pM.g; g.position.set(-9.7, 0, -1.4);
    add(g, C(1.35, 1.35, 0.2, 48), M(0x151515, 0.6, 0.1), 0, 0.35, 0);
    const ring = add(g, new THREE.TorusGeometry(1.3, 0.07, 8, 48), gold, 0, 0.46, 0); ring.rotation.x = Math.PI / 2;
    add(g, B(0.55, 0.18, 0.3), M(0xeeeeee, 0.4, 0.3), -0.85, 0.5, 0);
    add(g, C(0.14, 0.14, 0.2, 16), M(0x050505), 0, 0.5, 0);
    add(g, C(0.2, 0.2, 0.18, 16), M(0x888888, 0.4, 0.6), 0.9, 0.5, 0.2);
  }
  const MICP = { 'SCK': [-0.49, -0.65], 'WS': [-0.02, -0.65], 'L/R': [0.4, -0.65], 'SD': [-0.45, 0.67], 'VDD': [-0.02, 0.67], 'GND': [0.45, 0.67] };
  const micPin = n => V(-9.7 + MICP[n][0], 0.9, -1.4 + MICP[n][1]);
  Object.entries(MICP).forEach(([n, [x, z]]) => {
    add(pM.g, C(0.14, 0.14, 0.35, 12), gold, x, 0.6, z);
    pinText(pM, V(x, 1.5 + (n === 'WS' || n === 'VDD' ? 0.35 : 0), z + (z < 0 ? -0.55 : 0.55)), n);
  });

  /* ---------- MAX98357A amp (right) ---------- */
  const pA = part('amp', 'MAX98357A amp', 3, 'An I2S amplifier. It takes digital audio on DIN and drives the speaker through VO+ and VO−. Leave SD and GAIN unconnected.');
  { const g = pA.g; g.position.set(10.1, 0, -0.9);
    add(g, B(3.0, 0.2, 3.0), M(0x1840c8, 0.5, 0.1), 0, 0.35, 0);
    add(g, B(1.3, 0.9, 0.8), M(0x1e5bd8, 0.5, 0.1), 0, 0.8, -1.0);
    [-0.33, 0.29].forEach(x => { add(g, C(0.26, 0.26, 0.15, 20), M(0xc9ced4, 0.3, 0.9), x, 1.28, -1.0); });
    add(g, B(0.55, 0.14, 0.55), blackM, 0.55, 0.52, 0.15);
    [[-1.2, -1.2], [1.2, -1.2]].forEach(([x, z]) => { const t = add(g, new THREE.TorusGeometry(0.22, 0.07, 8, 20), gold, x, 0.47, z); t.rotation.x = Math.PI / 2; });
    const s = silk('MAX\n98357A\nI2S Amp', 1.5, 0.9, '#ffffff', 'bold 42px sans-serif'); s.position.set(-0.55, 0.47, 0.25); g.add(s);
  }
  const AMPX = { 'LRC': -1.33, 'BCLK': -0.89, 'DIN': -0.45, 'GAIN': 0, 'SD': 0.42, 'GND': 0.85, 'Vin': 1.3 };
  const ampPin = n => V(10.1 + AMPX[n], 0.9, -0.9 + 1.24);
  Object.entries(AMPX).forEach(([n, x], i) => {
    add(pA.g, C(0.12, 0.12, 0.35, 12), gold, x, 0.6, 1.24);
    if (!['GAIN', 'SD'].includes(n)) pinText(pA, V(x, 1.5, 1.95 + (i % 3) * 0.38), n);
  });
  const ampTerm = n => V(10.1 + (n === '+' ? -0.33 : 0.29), 1.45, -1.9);

  /* ---------- speaker (far right) ---------- */
  const pS = part('spk', 'Speaker', 4, 'A small 4 to 8 ohm speaker. It is driven by the amplifier output, so its two wires go to VO+ and VO− and never to GND.');
  let cone;
  { const g = pS.g; g.position.set(16.2, 0, 1.1);
    add(g, B(4.4, 0.4, 4.4), M(0xc9cdd2, 0.4, 0.8), 0, 0.2, 0);
    add(g, C(2.15, 2.15, 0.5, 48), M(0x2a2d31, 0.6, 0.2), 0, 0.55, 0);
    const surround = add(g, new THREE.TorusGeometry(1.95, 0.14, 10, 48), M(0x111111, 0.8), 0, 0.82, 0); surround.rotation.x = Math.PI / 2;
    cone = add(g, C(1.5, 1.5, 0.12, 48), M(0xb7bcc2, 0.35, 0.8), 0, 0.82, 0);
    [[-1.9, -1.9], [1.9, -1.9], [-1.9, 1.9], [1.9, 1.9]].forEach(([x, z]) => add(g, C(0.22, 0.22, 0.45, 16), M(0xeeeeee, 0.5, 0.2), x, 0.42, z));
    [-0.9, 0.8].forEach(x => add(g, B(0.3, 0.2, 0.5), gold, x, 0.4, 2.1));
  }
  const spkTerm = n => V(16.2 + (n === '+' ? 0.8 : -0.9), 0.75, 1.1 + 2.1);

  /* ---------- USB power ---------- */
  const usb = new THREE.Group(); scene.add(usb);
  { const cable = new THREE.Mesh(new THREE.TubeGeometry(new THREE.CatmullRomCurve3([V(0, 0.75, 5.0), V(0, 0.75, 8), V(1.5, 0.4, 11), V(6, 0.3, 13), V(14, 0.3, 14)]), 40, 0.14, 8), M(0x222222, 0.8));
    usb.add(cable); add(usb, B(1.1, 0.5, 1.3), M(0x2d3036, 0.5, 0.2), 0, 0.75, 5.6); }
  let usbProg = 0;

  /* ---------- wires ---------- */
  const WIRES = [
    { net: '3V3', a: espPin(-1, 0), b: oledPin(-0.69), step: 1, flow: 1, need: ['esp', 'oled'], text: '3V3 → OLED VDD' },
    { net: 'GND', a: espPin(-1, 13), b: oledPin(-0.24), step: 1, flow: 1, need: ['esp', 'oled'], text: 'GND → OLED GND' },
    { net: 'SCL', a: espPin(1, 7), b: oledPin(0.2), step: 1, flow: 1, need: ['esp', 'oled'], text: 'GPIO19 → OLED SCK' },
    { net: 'SDA', a: espPin(1, 5), b: oledPin(0.65), step: 1, flow: 1, need: ['esp', 'oled'], text: 'GPIO21 → OLED SDA' },
    { net: '3V3', a: espPin(-1, 0), b: micPin('VDD'), step: 2, flow: 1, need: ['esp', 'mic'], text: '3V3 → mic VDD' },
    { net: 'GND', a: espPin(-1, 13), b: micPin('GND'), step: 2, flow: 1, need: ['esp', 'mic'], text: 'GND → mic GND' },
    { net: 'GND', a: espPin(-1, 13), b: micPin('L/R'), step: 2, flow: 1, need: ['esp', 'mic'], text: 'GND → mic L/R' },
    { net: 'BCLK', a: espPin(-1, 11), b: micPin('SCK'), step: 2, flow: 1, need: ['esp', 'mic'], text: 'GPIO14 → mic SCK' },
    { net: 'WS', a: espPin(-1, 8), b: micPin('WS'), step: 2, flow: 1, need: ['esp', 'mic'], text: 'GPIO25 → mic WS' },
    { net: 'MIC', a: micPin('SD'), b: espPin(-1, 7), step: 2, flow: 1, need: ['esp', 'mic'], text: 'mic SD → GPIO33' },
    { net: '5V', a: espPin(-1, 18), b: ampPin('Vin'), step: 3, flow: 1, need: ['esp', 'amp'], text: '5V → amp Vin' },
    { net: 'GND', a: espPin(-1, 13), b: ampPin('GND'), step: 3, flow: 1, need: ['esp', 'amp'], text: 'GND → amp GND' },
    { net: 'SPK', a: espPin(1, 2), b: ampPin('DIN'), step: 3, flow: 1, need: ['esp', 'amp'], text: 'GPIO22 → amp DIN' },
    { net: 'BCLK', a: espPin(-1, 11), b: ampPin('BCLK'), step: 3, flow: 1, need: ['esp', 'amp'], text: 'GPIO14 → amp BCLK' },
    { net: 'WS', a: espPin(-1, 8), b: ampPin('LRC'), step: 3, flow: 1, need: ['esp', 'amp'], text: 'GPIO25 → amp LRC' },
    { net: 'VO+', a: ampTerm('+'), b: spkTerm('+'), step: 4, flow: 1, need: ['amp', 'spk'], text: 'amp VO+ → speaker +' },
    { net: 'VO-', a: ampTerm('-'), b: spkTerm('-'), step: 4, flow: 1, need: ['amp', 'spk'], text: 'amp VO− → speaker −' }
  ];
  const wireGroup = new THREE.Group(); scene.add(wireGroup);
  const pulseGeo = new THREE.SphereGeometry(0.17, 10, 10);
  WIRES.forEach((w, idx) => {
    const dist = w.a.distanceTo(w.b), h = Math.min(2.8 + idx * 0.26, 1.7 + dist * 0.28 + idx * 0.05);
    const lerp = t => V(w.a.x + (w.b.x - w.a.x) * t, 0, w.a.z + (w.b.z - w.a.z) * t);
    const p1 = lerp(0.2), p2 = lerp(0.5), p3 = lerp(0.8);
    const pts = [w.a, w.a.clone().add(V(0, 0.9, 0)), V(p1.x, h, p1.z), V(p2.x, h + 0.12, p2.z), V(p3.x, h, p3.z), w.b.clone().add(V(0, 0.9, 0)), w.b];
    w.curve = new THREE.CatmullRomCurve3(pts, false, 'centripetal');
    const geo = new THREE.TubeGeometry(w.curve, 80, 0.085, 8, false);
    w.mat = new THREE.MeshStandardMaterial({ color: NET[w.net], roughness: 0.45, metalness: 0.05, transparent: true, opacity: 1 });
    w.mesh = new THREE.Mesh(geo, w.mat); w.cnt = geo.index.count; w.mesh.visible = false; wireGroup.add(w.mesh);
    w.pulses = [0, 1, 2].map(() => { const m = new THREE.Mesh(pulseGeo, new THREE.MeshBasicMaterial({ color: new THREE.Color(NET[w.net]).lerp(new THREE.Color(0xffffff), 0.65) })); m.visible = false; scene.add(m); return m; });
    w.prog = 0;
  });

  /* ---------- signal effects ---------- */
  function mkRings(color, n) {
    const arr = [];
    for (let i = 0; i < n; i++) { const m = new THREE.Mesh(new THREE.RingGeometry(0.93, 1, 56), new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0, side: THREE.DoubleSide, depthWrite: false })); m.rotation.x = -Math.PI / 2; m.visible = false; scene.add(m); arr.push(m); }
    return arr;
  }
  function updRings(arr, on, center, maxR, inward, t, speed) {
    arr.forEach((m, i) => {
      if (!on) { m.visible = false; return; }
      const u = (t * speed + i / arr.length) % 1;
      m.visible = true; m.position.copy(center); m.scale.setScalar((inward ? maxR * (1 - u) : maxR * u) + 0.4);
      m.material.opacity = (inward ? u : 1 - u) * 0.85;
    });
  }
  const wifiRings = mkRings(0xffb347, 3), micRings = mkRings(0xffffff, 3), spkRings = mkRings(0x38bdf8, 3);
  const wifiTag = label('WiFi', 0.6, '#1a1005', '#ffb347'); wifiTag.position.set(0, 2.2, -6.6); scene.add(wifiTag); wifiTag.visible = false;

  /* ---------- part labels ---------- */
  const NAME_Y = { esp: 3.0, oled: 6.4, mic: 2.6, amp: 3.2, spk: 3.2 };
  Object.values(parts).forEach(p => { const s = label(p.name, 0.7); s.position.set(0, NAME_Y[p.key], p.key === 'esp' ? 0 : 0); p.labels.add(s); });
  parts.oled.labels.position.set(0, 0, 0);

  /* ---------- UI ---------- */
  const STEPS = [
    ['Step 1 · Place the ESP32', 'Put the ESP32 DevKitC in the middle with the antenna facing the back and the USB port facing you. Everything else wires to its two pin headers.', []],
    ['Step 2 · Add the OLED', 'Stand the OLED at the back. It needs 4 wires.', [0, 1, 2, 3]],
    ['Step 3 · Add the microphone', 'Put the INMP441 on the left. L/R goes to GND so the mic sends on the left channel. 6 wires.', [4, 5, 6, 7, 8, 9]],
    ['Step 4 · Add the amplifier', 'Put the MAX98357A on the right. It shares the BCLK and WS clock wires with the mic. 5 wires.', [10, 11, 12, 13, 14]],
    ['Step 5 · Connect the speaker', 'The speaker connects to the amplifier output only. Never to GND. 2 wires.', [15, 16]],
    ['Step 6 · Plug in USB', 'USB powers everything. Press Talk below to watch the signals travel along the wires.', []]
  ];
  let step = 0;
  const stepBox = $$('#steps3');
  STEPS.forEach((s, i) => { const b = document.createElement('button'); b.className = 'chip'; b.textContent = String(i + 1); b.title = s[0]; b.onclick = () => setStep(i); stepBox.appendChild(b); });
  function setStep(n) {
    step = Math.max(0, Math.min(STEPS.length - 1, n));
    [...stepBox.children].forEach((b, i) => b.setAttribute('aria-pressed', i === step));
    $$('#cap3t').textContent = STEPS[step][0]; $$('#cap3d').textContent = STEPS[step][1];
    $$('#cap3w').innerHTML = STEPS[step][2].map(i => `<span class="wchip"><i style="background:${NET[WIRES[i].net]}"></i>${WIRES[i].text}</span>`).join('');
  }
  setStep(0);
  $$('#prev3').onclick = () => { stopAuto(); setStep(step - 1); };
  $$('#next3').onclick = () => { stopAuto(); setStep(step + 1); };
  $$('#reset3').onclick = () => { stopAuto(); setStep(0); };
  let autoT = null;
  function stopAuto() { if (autoT) { clearInterval(autoT); autoT = null; $$('#auto3').textContent = 'Auto build'; } }
  $$('#auto3').onclick = () => {
    if (autoT) { stopAuto(); return; }
    setStep(0); $$('#auto3').textContent = 'Stop';
    autoT = setInterval(() => { if (step >= STEPS.length - 1) stopAuto(); else setStep(step + 1); }, 2600);
  };
  function goView(v) { camGoal = { pos: VIEWS[v].pos.clone(), target: VIEWS[v].target.clone() }; }
  $$('#v-iso').onclick = () => goView('iso'); $$('#v-top').onclick = () => goView('top'); $$('#v-front').onclick = () => goView('front');
  $$('#rot3').onchange = e => { controls.autoRotate = e.target.checked; controls.autoRotateSpeed = 1.2; };
  $$('#pins3').onchange = e => { Object.values(parts).forEach(p => p.pinLabels.visible = e.target.checked); };

  let sel = null;
  const box = new THREE.BoxHelper(parts.esp.g, 0xffb347); box.visible = false; scene.add(box);
  function focusPart(k) {
    const p = parts[k]; if (step < p.step) setStep(p.step);
    sel = k; box.setFromObject(p.g); box.visible = true;
    const c = new THREE.Box3().setFromObject(p.g).getCenter(V(0, 0, 0)); c.y = 1;
    camGoal = { pos: c.clone().add(V(0, 9, 13)), target: c };
    $$('#info3').textContent = p.name + ': ' + p.info;
    [...$$('#parts3').children].forEach(b => b.setAttribute('aria-pressed', b.dataset.k === k));
  }
  Object.values(parts).forEach(p => { const b = document.createElement('button'); b.className = 'chip'; b.textContent = p.name; b.dataset.k = p.key; b.onclick = () => focusPart(p.key); $$('#parts3').appendChild(b); });
  $$('#info3').textContent = 'Pick a part to zoom to it and read what it does.';

  let isolated = null;
  const legend = $$('#legend3');
  Object.keys(NET).forEach(n => {
    const b = document.createElement('button'); b.className = 'chip'; b.innerHTML = `<i class="dot" style="background:${NET[n]}"></i>${n} · ${NETINFO[n]}`;
    b.onclick = () => { isolated = isolated === n ? null : n; [...legend.children].forEach(c => c.setAttribute('aria-pressed', c === b && isolated !== null)); };
    legend.appendChild(b);
  });

  const QUICK3 = ['Tell me a fun fact', 'I got the job!', 'You look angry!', 'I love you Pip', 'Say goodnight'];
  QUICK3.forEach(t => { const b = document.createElement('button'); b.className = 'chip'; b.textContent = t; b.onclick = () => talk3(t); $$('#quick3').appendChild(b); });
  function talk3(text) { stopAuto(); if (step < STEPS.length - 1) setStep(STEPS.length - 1); if (!window.S || S.busy) return; setTimeout(() => converse(text), step < STEPS.length - 1 ? 1500 : 0); }
  $$('#f3').onsubmit = e => { e.preventDefault(); talk3($$('#q3').value); };

  /* ---------- sizing ---------- */
  function resize() { const w = wrap.clientWidth, h = wrap.clientHeight; if (!w || !h) return; renderer.setSize(w, h, false); camera.aspect = w / h; camera.updateProjectionMatrix(); }
  new ResizeObserver(resize).observe(wrap); resize();
  let bgCheck = 0;
  function applyBg() { const cs = getComputedStyle(document.documentElement); scene.background = new THREE.Color(cs.getPropertyValue('--stage').trim() || '#e7ede9'); }
  applyBg();

  /* ---------- animation ---------- */
  const ease = t => t * t * (3 - 2 * t);
  const STAGE_TEXT = [['mic', 'The microphone hears you'], ['up', 'Streaming your voice over WiFi'], ['stt', 'Whisper turns speech into text'], ['llm', 'The LLM picks a reply and an emotion'], ['tts', 'Making the voice'], ['down', 'Receiving the voice over WiFi'], ['amp', 'The amplifier plays it, the face shows the emotion']];
  let last = performance.now(), lastStatus = '';
  function speed(net, powered) {
    const mode = typeof waveMode !== 'undefined' ? waveMode : 'idle';
    switch (net) {
      case '3V3': case '5V': return powered ? 0.6 : 0;
      case 'SDA': case 'SCL': return powered ? (S.busy ? 1.4 : 0.5) : 0;
      case 'BCLK': case 'WS': return powered && mode !== 'idle' ? 1.8 : 0;
      case 'MIC': return mode === 'mic' ? 1.6 : 0;
      case 'SPK': return mode === 'spk' ? 1.6 : 0;
      case 'VO+': case 'VO-': return mode === 'spk' ? 2.4 : 0;
      default: return 0;
    }
  }
  function frame() {
    requestAnimationFrame(frame);
    if ($$('#p-3d').hidden) { last = performance.now(); return; }
    const now = performance.now(), dt = Math.min(0.05, (now - last) / 1000), t = now / 1000; last = now;
    if (++bgCheck % 120 === 0) applyBg();

    Object.values(parts).forEach(p => {
      const target = step >= p.step ? 1 : 0;
      p.prog += Math.sign(target - p.prog) * Math.min(Math.abs(target - p.prog), dt * 1.4);
      p.g.visible = p.prog > 0.001;
      p.g.position.y = (1 - ease(p.prog)) * 9;
      p.labels.visible = p.prog > 0.98;
    });
    const wantUsb = step >= 5 ? 1 : 0; usbProg += Math.sign(wantUsb - usbProg) * Math.min(Math.abs(wantUsb - usbProg), dt * 1.2);
    usb.visible = usbProg > 0.001; usb.position.y = (1 - ease(usbProg)) * 3; usb.position.z = (1 - ease(usbProg)) * 6;
    const powered = usbProg > 0.95;
    pE.led.material.color.set(powered ? 0xff3030 : 0x551111); pE.led.material.emissive.set(powered ? 0xff2020 : 0x000000);
    screenMat.color.set(powered ? 0xffffff : 0x0a0a0a);
    if (powered) screenTex.needsUpdate = true;

    WIRES.forEach(w => {
      const ready = w.need.every(k => parts[k].prog > 0.98);
      const target = step >= w.step && ready ? 1 : 0;
      const rate = target ? 1.1 : 3;
      w.prog += Math.sign(target - w.prog) * Math.min(Math.abs(target - w.prog), dt * rate);
      w.mesh.visible = w.prog > 0.001;
      w.mesh.geometry.setDrawRange(0, Math.floor(w.cnt * w.prog / 3) * 3);
      w.mat.opacity = isolated && isolated !== w.net ? 0.12 : 1;
      const sp = w.prog > 0.99 ? speed(w.net, powered) : 0;
      w.pulses.forEach((m, i) => {
        if (!sp || (isolated && isolated !== w.net)) { m.visible = false; return; }
        const u = (t * sp * 0.5 + i / 3) % 1; m.visible = true;
        m.position.copy(w.curve.getPointAt(w.flow > 0 ? u : 1 - u));
      });
    });

    const mode = typeof waveMode !== 'undefined' ? waveMode : 'idle', A = typeof ACT !== 'undefined' ? ACT : {};
    const wifi = !!(A.up || A.down);
    updRings(wifiRings, wifi, V(0, 1.4, -5.6), 7, !!A.down, t, 0.9); wifiTag.visible = wifi;
    updRings(micRings, mode === 'mic' && pM.prog > 0.98, V(-9.7, 1.1, -1.4), 4.5, true, t, 0.9);
    updRings(spkRings, mode === 'spk' && pS.prog > 0.98, V(16.2, 1.3, 1.1), 6, false, t, 0.9);
    const amp = (typeof S !== 'undefined' && S.talking) ? S.talkAmp : 0;
    cone.position.y = 0.82 + amp * Math.sin(now / 24) * 0.22; cone.scale.setScalar(1 + amp * 0.04);

    let status = 'Idle';
    for (const [id, txt] of STAGE_TEXT) if (A[id]) { status = txt; break; }
    if (status !== lastStatus) { $$('#status3').textContent = status; lastStatus = status; }
    $$('#talk3').disabled = typeof S !== 'undefined' && S.busy;

    if (sel) box.setFromObject(parts[sel].g);
    if (camGoal) {
      camera.position.lerp(camGoal.pos, 0.08); controls.target.lerp(camGoal.target, 0.08);
      if (camera.position.distanceTo(camGoal.pos) < 0.05) camGoal = null;
    }
    controls.update();
    renderer.render(scene, camera);
  }
  frame();

  // test hook: lets automated checks jump to a step
  window.pip3d = { setStep };
})();
