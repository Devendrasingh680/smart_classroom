# Smart Classroom Monitor (cloud version)

ESP32 -> Render (Node/Express API) -> MongoDB Atlas -> Netlify dashboard

- `backend/`  Node.js API (deploy on Render, Root Directory = backend)
- `frontend/` Static dashboard (deploy on Netlify, Base directory = frontend)
- `esp32/`    Arduino sketch for classroom sensors, SMS alerts, and OV7670 photo uploads

Setup order: MongoDB Atlas -> Render -> edit API_BASE in frontend/script.js -> Netlify -> configure and flash ESP32.
The current sketch reports noise and IR status over Serial, sends SMS alerts, and uploads camera photos; it does not POST noise/radar readings.

## Camera photos and live video

The dashboard photo archive displays the latest 100 images and refreshes every minute. The
ESP32 sketch captures a low-resolution OV7670 frame, converts it to JPEG, and uploads it
immediately after startup and every 30 minutes. Replace the Wi-Fi SSID/password, backend URL,
API key, and SMS recipient placeholders in the sketch before uploading it. Do not commit real
credentials. A failed upload is retried after one minute. The backend accepts JPEG/PNG uploads
up to 1 MB and retains them for seven days.

To test the backend independently, upload an image with the same API key:

```sh
curl -X POST "https://YOUR-BACKEND-NAME.onrender.com/api/photos" \
	-H "x-api-key: YOUR_API_KEY" \
	-H "Content-Type: image/jpeg" \
	--data-binary @classroom.jpg
```

Images must be smaller than 1 MB. The dashboard requires the backend `API_BASE` to be
configured as usual.

Live video is not provided by the sketch or backend yet. The dashboard needs a publicly
reachable HTTPS MJPEG stream URL; a regular single-image URL is not a live stream.
