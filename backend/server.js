require("dotenv").config();
const express = require("express");
const cors = require("cors");
const mongoose = require("mongoose");

// ---------- settings ----------
const PORT = process.env.PORT || 3000;
const MONGODB_URI = process.env.MONGODB_URI;
const API_KEY = process.env.API_KEY;
const FRONTEND_ORIGIN = process.env.FRONTEND_ORIGIN || "*";

const ESP32_ONLINE_MS = 10000;          // ESP32 counts as online if it sent data in the last 10 s
const CAMERA_ONLINE_MS = 15000;         // camera counts as online if a count arrived in the last 15 s
const KEEP_SECONDS = 60 * 60 * 24 * 7;  // MongoDB deletes readings older than 7 days automatically

if (!MONGODB_URI || !API_KEY) {
  console.error("Missing MONGODB_URI or API_KEY environment variable.");
  process.exit(1);
}

// ---------- database models ----------
const readingSchema = new mongoose.Schema({
  noise: { type: Number, required: true },
  radar: { type: Number, enum: [0, 1], required: true },
  createdAt: { type: Date, default: Date.now, expires: KEEP_SECONDS },
});
const occupancySchema = new mongoose.Schema({
  count: { type: Number, required: true, min: 0, max: 500 },
  createdAt: { type: Date, default: Date.now, expires: KEEP_SECONDS },
});
const photoSchema = new mongoose.Schema({
  image: { type: Buffer, required: true },
  contentType: { type: String, enum: ["image/jpeg", "image/png"], required: true },
  createdAt: { type: Date, default: Date.now, expires: KEEP_SECONDS },
});
const Reading = mongoose.model("Reading", readingSchema);
const Occupancy = mongoose.model("Occupancy", occupancySchema);
const Photo = mongoose.model("Photo", photoSchema);

// ---------- app ----------
const app = express();
app.use(express.json({ limit: "10kb" }));

const origins = FRONTEND_ORIGIN.split(",").map((s) => s.trim());
app.use(cors({ origin: origins.includes("*") ? "*" : origins }));

function requireKey(req, res, next) {
  if (req.get("x-api-key") !== API_KEY) {
    return res.status(401).json({ error: "unauthorized" });
  }
  next();
}

app.get("/", (req, res) => res.send("Smart Classroom API is running"));
app.get("/health", (req, res) => res.json({ ok: true }));

// ESP32 sends noise + radar here
app.post("/api/data", requireKey, async (req, res) => {
  const noise = Number(req.body.noise);
  const radar = Number(req.body.radar);
  if (!Number.isFinite(noise) || (radar !== 0 && radar !== 1)) {
    return res.status(400).json({ error: "noise (number) and radar (0 or 1) are required" });
  }
  try {
    await Reading.create({ noise, radar });
    res.status(201).json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

// The camera / people-counting program sends the count here
app.post("/api/occupancy", requireKey, async (req, res) => {
  const count = Number(req.body.count);
  if (!Number.isInteger(count) || count < 0 || count > 500) {
    return res.status(400).json({ error: "count must be an integer from 0 to 500" });
  }
  try {
    await Occupancy.create({ count });
    res.status(201).json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

// Camera process uploads one compressed JPEG/PNG at each capture interval.
app.post("/api/photos", requireKey, express.raw({
  type: ["image/jpeg", "image/png"],
  limit: "1mb",
}), async (req, res) => {
  if (!Buffer.isBuffer(req.body) || req.body.length === 0) {
    return res.status(400).json({ error: "send a JPEG or PNG image as the request body" });
  }
  try {
    const photo = await Photo.create({ image: req.body, contentType: req.get("content-type") });
    res.status(201).json({ ok: true, id: photo.id, createdAt: photo.createdAt });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

app.get("/api/photos", async (req, res) => {
  const limit = Math.min(Math.max(parseInt(req.query.limit, 10) || 50, 1), 100);
  try {
    const rows = await Photo.find().sort({ createdAt: -1 }).limit(limit).select("createdAt contentType").lean();
    res.set("Cache-Control", "no-store");
    res.json(rows.map((photo) => ({
      id: photo._id,
      createdAt: photo.createdAt,
      contentType: photo.contentType,
      imageUrl: `/api/photos/${photo._id}/image`,
    })));
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

app.get("/api/photos/:id/image", async (req, res) => {
  if (!mongoose.isValidObjectId(req.params.id)) return res.status(404).end();
  try {
    const photo = await Photo.findById(req.params.id).select("image contentType").lean();
    if (!photo) return res.status(404).end();
    res.set("Content-Type", photo.contentType);
    res.set("Cache-Control", "public, max-age=3600");
    res.send(photo.image);
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

// Dashboard reads the newest values here
app.get("/api/latest", async (req, res) => {
  try {
    const [reading, occ] = await Promise.all([
      Reading.findOne().sort({ createdAt: -1 }).lean(),
      Occupancy.findOne().sort({ createdAt: -1 }).lean(),
    ]);
    const now = Date.now();
    const espOnline = !!reading && now - reading.createdAt.getTime() < ESP32_ONLINE_MS;
    const camOnline = !!occ && now - occ.createdAt.getTime() < CAMERA_ONLINE_MS;

    res.set("Cache-Control", "no-store");
    res.json({
      esp32: espOnline ? 1 : 0,
      lastUpdate: reading ? reading.createdAt : null,
      noise: reading ? reading.noise : null,
      radar: reading ? reading.radar : null,
      camera: camOnline ? 1 : 0,
      occupancy: camOnline ? occ.count : null,
    });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

// Recent noise history for the chart
app.get("/api/history", async (req, res) => {
  const limit = Math.min(Math.max(parseInt(req.query.limit, 10) || 60, 1), 300);
  try {
    const rows = await Reading.find().sort({ createdAt: -1 }).limit(limit).lean();
    res.set("Cache-Control", "no-store");
    res.json(rows.reverse().map((r) => ({ t: r.createdAt, noise: r.noise, radar: r.radar })));
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "database error" });
  }
});

// ---------- start ----------
mongoose
  .connect(MONGODB_URI)
  .then(() => {
    console.log("MongoDB connected");
    app.listen(PORT, () => console.log("Server listening on port " + PORT));
  })
  .catch((err) => {
    console.error("MongoDB connection failed:", err.message);
    process.exit(1);
  });
