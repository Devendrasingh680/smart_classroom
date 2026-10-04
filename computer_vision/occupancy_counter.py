import logging
import os
import time

import cv2
import numpy as np
import requests
from dotenv import load_dotenv
from ultralytics import YOLO


load_dotenv()

CAMERA_STREAM_URL = os.getenv("CAMERA_STREAM_URL", "").strip()
BACKEND_URL = os.getenv("BACKEND_URL", "https://smart-classroom-87fd.onrender.com").rstrip("/")
API_KEY = os.getenv("API_KEY", "").strip()
MODEL_NAME = os.getenv("YOLO_MODEL", "yolo11n.pt")
POST_INTERVAL_SECONDS = float(os.getenv("POST_INTERVAL_SECONDS", "5"))
INFERENCE_INTERVAL_SECONDS = float(os.getenv("INFERENCE_INTERVAL_SECONDS", "1"))
OCCUPANCY_URL = BACKEND_URL + "/api/occupancy"

STREAM_RETRY_SECONDS = 3
MAX_JPEG_BYTES = 2 * 1024 * 1024


def camera_frames():
    while True:
        try:
            with requests.get(CAMERA_STREAM_URL, stream=True, timeout=(5, 30)) as response:
                response.raise_for_status()
                buffer = bytearray()

                for chunk in response.iter_content(chunk_size=4096):
                    if not chunk:
                        continue

                    buffer.extend(chunk)
                    while True:
                        start = buffer.find(b"\xff\xd8")
                        if start < 0:
                            if len(buffer) > 1:
                                del buffer[:-1]
                            break

                        end = buffer.find(b"\xff\xd9", start + 2)
                        if end < 0:
                            if start > 0:
                                del buffer[:start]
                            if len(buffer) > MAX_JPEG_BYTES:
                                logging.warning("Discarding oversized incomplete camera frame")
                                buffer.clear()
                            break

                        jpeg = bytes(buffer[start : end + 2])
                        del buffer[: end + 2]
                        frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
                        if frame is not None:
                            yield frame

        except requests.RequestException as error:
            logging.warning("Camera stream unavailable: %s; retrying", error)
            time.sleep(STREAM_RETRY_SECONDS)


def post_count(session, count):
    try:
        response = session.post(
            OCCUPANCY_URL,
            json={"count": count},
            headers={"x-api-key": API_KEY},
            timeout=15,
        )
        if response.status_code == 201:
            logging.info("Occupancy %d posted successfully", count)
        else:
            logging.error("Occupancy upload returned HTTP %d: %s", response.status_code, response.text[:200])
    except requests.RequestException as error:
        logging.error("Occupancy upload failed: %s", error)


def main():
    if not CAMERA_STREAM_URL:
        raise SystemExit("Set CAMERA_STREAM_URL in computer_vision/.env")
    if not API_KEY or API_KEY == "YOUR_API_KEY":
        raise SystemExit("Set the Render API_KEY in computer_vision/.env")

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.info("Loading model %s; the first run may download model weights", MODEL_NAME)
    model = YOLO(MODEL_NAME)
    session = requests.Session()

    latest_count = None
    last_inference = 0.0
    last_post = 0.0

    try:
        for frame in camera_frames():
            now = time.monotonic()
            if now - last_inference >= INFERENCE_INTERVAL_SECONDS:
                results = model.track(
                    frame,
                    persist=True,
                    classes=[0],
                    tracker="bytetrack.yaml",
                    conf=0.35,
                    verbose=False,
                )
                boxes = results[0].boxes
                latest_count = 0 if boxes is None else len(boxes)
                last_inference = now
                logging.info("People visible: %d", latest_count)

            if latest_count is not None and now - last_post >= POST_INTERVAL_SECONDS:
                post_count(session, latest_count)
                last_post = now
    except KeyboardInterrupt:
        logging.info("Occupancy counter stopped")


if __name__ == "__main__":
    main()