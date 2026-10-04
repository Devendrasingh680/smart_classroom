# Smart Classroom Monitor (cloud version)

ESP32 -> Render (Node/Express API) -> MongoDB Atlas -> Netlify dashboard

- `backend/`  Node.js API (deploy on Render, Root Directory = backend)
- `frontend/` Static dashboard (deploy on Netlify, Base directory = frontend)
- `esp32/esp32_devkit_30pin/` Arduino sketch for the 30-pin DevKit, INMP441, IR sensor, and SIM800L
- `esp32/esp32_cam_ov2640/`   Arduino sketch for an AI-Thinker-style ESP32-CAM with OV2640

Setup order: MongoDB Atlas -> Render -> edit API_BASE in frontend/script.js -> Netlify -> configure and flash both ESP32 boards.
The DevKit posts microphone amplitude and IR presence to `/api/data` and sends SMS alerts. The ESP32-CAM uploads photos to `/api/photos`.

## Camera photos and live video

The dashboard photo archive displays the latest 100 images and refreshes every minute. The
ESP32-CAM sketch uses the AI-Thinker pin map, captures OV2640 JPEGs, uploads the first photo
after startup, then captures every 30 minutes. Failed uploads are retried after one minute.
For a board without PSRAM it falls back to QVGA; with PSRAM it uses VGA. The backend accepts
photos up to 1 MB and retains them for seven days.

Replace the Wi-Fi SSID/password, Render backend URL, and API key placeholders in both sketches.
Replace the SMS recipient placeholder in the DevKit sketch. Do not commit real credentials.
The mic reports raw amplitude, not calibrated dB; tune `NOISE_THRESHOLD` in the DevKit sketch
and `NOISE_ALERT_LEVEL` in `frontend/script.js` for your sensor and room.
In Arduino IDE, select the matching 30-pin ESP32 Dev Module for the sensor sketch and AI Thinker
ESP32-CAM for the camera sketch; enable PSRAM for the camera board if that option is available.

To test the backend independently, upload an image with the same API key:

```sh
curl -X POST "https://YOUR-BACKEND-NAME.onrender.com/api/photos" \
	-H "x-api-key: YOUR_API_KEY" \
	-H "Content-Type: image/jpeg" \
	--data-binary @classroom.jpg
```

Images must be smaller than 1 MB. The dashboard requires the backend `API_BASE` to be
configured as usual.

The ESP32-CAM serves an MJPEG stream at `http://<camera-ip>/stream` on the same Wi-Fi network.
Open the printed URL in a local browser to test it. A hosted HTTPS dashboard cannot directly
load that local HTTP URL; remote viewing needs a publicly reachable HTTPS reverse proxy or
streaming service. The ESP32-CAM stream is not proxied through this backend.
