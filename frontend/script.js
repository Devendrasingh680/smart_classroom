// =====================================================
// CONFIGURATION: change ONLY the URL below to your Render backend URL
// =====================================================
const API_BASE = "https://YOUR-BACKEND-NAME.onrender.com";
const CAMERA_STREAM_URL = ""; // HTTPS MJPEG stream URL provided by your camera/streaming service

const POLL_INTERVAL_MS    = 1000;   // ask the backend every 1 second
const HISTORY_INTERVAL_MS = 5000;   // refresh the noise chart every 5 seconds
const PHOTO_INTERVAL_MS   = 60000;  // refresh the photo archive every minute
const REQUEST_TIMEOUT_MS  = 8000;   // Render can be slow when waking up
const NOISE_ALERT_DB      = 70;     // HIGH NOISE alert above this
const OCCUPANCY_LIMIT     = 30;     // OCCUPANCY alert above this (set your classroom capacity)
// =====================================================

const BASE = API_BASE.replace(/\/+$/, "");
const $ = (id) => document.getElementById(id);

let connected = false;
let activeAlerts = new Set();

function setCard(id, cls) { $(id).className = "card " + cls; }
function timeStr(d) { return d.toLocaleTimeString(); }

// ---------- clock ----------
setInterval(() => { $("clock").textContent = timeStr(new Date()); }, 1000);
$("clock").textContent = timeStr(new Date());

// ---------- notice banner ----------
function showNotice(text) {
  const n = $("notice");
  if (text) { n.textContent = text; n.classList.remove("hidden"); }
  else { n.classList.add("hidden"); }
}

// ---------- log ----------
function logEvent(text) {
  const ul = $("log");
  if (ul.firstElementChild && ul.firstElementChild.classList.contains("muted")) ul.innerHTML = "";
  const li = document.createElement("li");
  li.textContent = timeStr(new Date()) + "  -  " + text;
  ul.insertBefore(li, ul.firstChild);
  while (ul.children.length > 30) ul.removeChild(ul.lastChild);
}

// ---------- connection state ----------
function setConnected(state, reason) {
  if (state !== connected) {
    logEvent(state ? "ESP32 CONNECTED" : "ESP32 DISCONNECTED");
  }
  connected = state;
  $("connPill").textContent = state ? "ESP32 CONNECTED" : "ESP32 DISCONNECTED";
  $("connPill").className = "pill " + (state ? "ok" : "off");
  $("infoConn").textContent = state ? "CONNECTED" : "DISCONNECTED";
  $("espValue").textContent = state ? "ONLINE" : "OFFLINE";
  $("espStatus").textContent = state ? "Sending data" : (reason || "No data");
  setCard("cardEsp", state ? "ok" : "danger");
}

// ---------- render ----------
function renderDisconnected() {
  $("occValue").textContent = "--";  $("occLimit").textContent = "";
  $("noiseValue").textContent = "--"; $("noiseBar").style.width = "0";
  $("radarValue").textContent = "--"; $("camValue").textContent = "--";
  ["occStatus", "noiseStatus", "radarStatus", "camStatus"].forEach((id) => ($(id).textContent = "No data"));
  ["cardOcc", "cardNoise", "cardRadar", "cardCam"].forEach((id) => setCard(id, "off"));
  renderAlerts([]);
}

function render(d) {
  const occ = (d.occupancy === null || d.occupancy === undefined) ? null : Number(d.occupancy);
  const noise = Number(d.noise);
  const radar = Number(d.radar) === 1;
  const cam = Number(d.camera) === 1;

  // Occupancy
  if (occ === null) {
    $("occValue").textContent = "--"; $("occLimit").textContent = "";
    $("occStatus").textContent = "Waiting for camera count";
    setCard("cardOcc", "off");
  } else {
    $("occValue").textContent = occ;
    $("occLimit").textContent = "/ " + OCCUPANCY_LIMIT;
    const over = occ > OCCUPANCY_LIMIT;
    $("occStatus").textContent = over ? "Over capacity" : (occ === 0 ? "Room empty" : "Within capacity");
    setCard("cardOcc", over ? "danger" : "ok");
  }

  // Noise
  $("noiseValue").textContent = noise.toFixed(1);
  const pct = Math.max(0, Math.min(100, ((noise - 30) / 70) * 100));
  const loud = noise > NOISE_ALERT_DB;
  const rising = noise > NOISE_ALERT_DB - 10;
  $("noiseBar").style.width = pct + "%";
  $("noiseBar").style.background = loud ? "var(--danger)" : (rising ? "var(--warn)" : "var(--ok)");
  $("noiseStatus").textContent = loud ? "Too loud" : (rising ? "Getting loud" : "Normal");
  setCard("cardNoise", loud ? "danger" : (rising ? "warn" : "ok"));

  // Radar
  $("radarValue").textContent = radar ? "PRESENT" : "CLEAR";
  $("radarStatus").textContent = radar ? "Presence detected" : "No presence";
  setCard("cardRadar", radar ? "ok" : "off");

  // Camera
  $("camValue").textContent = cam ? "ONLINE" : "NO DATA";
  $("camStatus").textContent = cam ? "Sending counts" : "No recent count received";
  setCard("cardCam", cam ? "ok" : "warn");

  // Alerts
  const alerts = [];
  if (loud) alerts.push({ text: "HIGH NOISE DETECTED", level: "danger" });
  if (occ !== null && ((radar && occ === 0) || (!radar && occ > 0))) {
    alerts.push({ text: "ABNORMAL PRESENCE/MOVEMENT (camera and radar disagree)", level: "warn" });
  }
  if (occ !== null && occ > OCCUPANCY_LIMIT) alerts.push({ text: "OCCUPANCY ALERT", level: "danger" });
  renderAlerts(alerts);
}

function renderAlerts(list) {
  const box = $("alerts");
  box.innerHTML = "";
  if (list.length === 0) {
    const ok = document.createElement("div");
    ok.className = "alert ok"; ok.textContent = "No active alerts";
    box.appendChild(ok);
  }
  list.forEach((a) => {
    const el = document.createElement("div");
    el.className = "alert " + a.level; el.textContent = a.text;
    box.appendChild(el);
  });
  const now = new Set(list.map((a) => a.text));
  now.forEach((t) => { if (!activeAlerts.has(t)) logEvent("ALERT: " + t); });
  activeAlerts = now;
}

// ---------- fetch helper with timeout ----------
async function getJson(path) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    const res = await fetch(BASE + path, { signal: controller.signal, cache: "no-store" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    return await res.json();
  } finally {
    clearTimeout(timer);
  }
}

// ---------- live polling ----------
async function poll() {
  try {
    const d = await getJson("/api/latest");
    showNotice("");
    if (d.lastUpdate) $("lastUpdate").textContent = timeStr(new Date(d.lastUpdate));
    if (Number(d.esp32) === 1) {
      setConnected(true);
      render(d);
    } else {
      setConnected(false, "No data from ESP32 in the last 10 seconds");
      renderDisconnected();
    }
  } catch (err) {
    setConnected(false, "Backend not reachable");
    renderDisconnected();
    showNotice("Cannot reach the backend. On Render's free plan the server sleeps when idle, so the first request can take about a minute. Keep this page open.");
  } finally {
    setTimeout(poll, POLL_INTERVAL_MS);
  }
}

// ---------- noise chart ----------
function drawChart(points) {
  const c = $("noiseChart");
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  c.width = w * dpr; c.height = h * dpr;
  const ctx = c.getContext("2d");
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, h);

  if (points.length < 2) {
    ctx.fillStyle = "#8fa0c2"; ctx.font = "14px sans-serif";
    ctx.fillText("Not enough history yet", 10, 24);
    return;
  }
  const minDb = 30, maxDb = 100;
  const y = (v) => h - 8 - ((Math.min(Math.max(v, minDb), maxDb) - minDb) / (maxDb - minDb)) * (h - 16);
  const x = (i) => 8 + (i / (points.length - 1)) * (w - 16);

  ctx.strokeStyle = "#ef4444"; ctx.lineWidth = 1; ctx.setLineDash([6, 4]);
  ctx.beginPath(); ctx.moveTo(0, y(NOISE_ALERT_DB)); ctx.lineTo(w, y(NOISE_ALERT_DB)); ctx.stroke();
  ctx.setLineDash([]);

  ctx.strokeStyle = "#38bdf8"; ctx.lineWidth = 2; ctx.beginPath();
  points.forEach((p, i) => {
    if (i === 0) ctx.moveTo(x(i), y(p.noise)); else ctx.lineTo(x(i), y(p.noise));
  });
  ctx.stroke();
}

async function pollHistory() {
  try {
    const rows = await getJson("/api/history?limit=60");
    drawChart(rows);
  } catch (err) {
    /* ignore, live polling already shows connection problems */
  } finally {
    setTimeout(pollHistory, HISTORY_INTERVAL_MS);
  }
}

function renderPhotos(rows) {
  const body = $("photoRows");
  body.replaceChildren();
  $("photoCount").textContent = rows.length + (rows.length === 1 ? " photo" : " photos");
  if (rows.length === 0) {
    const row = body.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 3;
    cell.className = "muted";
    cell.textContent = "No photos received yet";
    return;
  }

  rows.forEach((photo) => {
    const row = body.insertRow();
    const captured = row.insertCell();
    captured.textContent = new Date(photo.createdAt).toLocaleString();

    const preview = row.insertCell();
    const image = document.createElement("img");
    image.className = "photo-thumb";
    image.src = BASE + photo.imageUrl;
    image.alt = "Classroom captured at " + captured.textContent;
    image.loading = "lazy";
    preview.appendChild(image);

    const linkCell = row.insertCell();
    const link = document.createElement("a");
    link.href = BASE + photo.imageUrl;
    link.target = "_blank";
    link.rel = "noopener";
    link.textContent = "Open photo";
    linkCell.appendChild(link);
  });
}

async function pollPhotos() {
  try {
    renderPhotos(await getJson("/api/photos?limit=100"));
  } catch (err) {
    $("photoCount").textContent = "Photo archive unavailable";
  } finally {
    setTimeout(pollPhotos, PHOTO_INTERVAL_MS);
  }
}

function configureLiveStream() {
  const image = $("liveStream");
  const empty = $("streamEmpty");
  if (!CAMERA_STREAM_URL) return;

  $("streamStatus").textContent = "Connecting to stream...";
  image.onload = () => {
    image.classList.remove("hidden");
    empty.classList.add("hidden");
    $("streamStatus").textContent = "Live";
  };
  image.onerror = () => {
    image.classList.add("hidden");
    empty.classList.remove("hidden");
    empty.textContent = "Camera stream is unavailable.";
    $("streamStatus").textContent = "Offline";
  };
  image.src = CAMERA_STREAM_URL;
}

setConnected(false, "Connecting...");
renderDisconnected();
configureLiveStream();
poll();
pollHistory();
pollPhotos();
