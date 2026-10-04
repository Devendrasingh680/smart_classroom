#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"

// Pin map for the common AI-Thinker ESP32-CAM with OV2640.
#define CAM_PWDN  32
#define CAM_RESET -1
#define CAM_XCLK   0
#define CAM_SIOD  26
#define CAM_SIOC  27
#define CAM_D0     5
#define CAM_D1    18
#define CAM_D2    19
#define CAM_D3    21
#define CAM_D4    36
#define CAM_D5    39
#define CAM_D6    34
#define CAM_D7    35
#define CAM_VSYNC 25
#define CAM_HREF  23
#define CAM_PCLK  22

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* PHOTO_UPLOAD_URL = "https://smart-classroom-z77j.onrender.com/api/photos";
const char* API_KEY = "YOUR_API_KEY";

const unsigned long PHOTO_INTERVAL_MS = 30UL * 60UL * 1000UL;
const unsigned long PHOTO_RETRY_MS = 60000;
const unsigned long WIFI_RETRY_MS = 10000;
const size_t MAX_PHOTO_BYTES = 1024 * 1024;

static const char* STREAM_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char* STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

SemaphoreHandle_t cameraMutex = nullptr;
httpd_handle_t cameraServer = nullptr;
bool cameraReady = false;
unsigned long lastPhotoSuccess = 0;
unsigned long lastPhotoAttempt = 0;
unsigned long lastWiFiRetry = 0;

bool initializeCamera()
{
  camera_config_t config = {};
  config.pin_pwdn = CAM_PWDN;
  config.pin_reset = CAM_RESET;
  config.pin_xclk = CAM_XCLK;
  config.pin_sccb_sda = CAM_SIOD;
  config.pin_sccb_scl = CAM_SIOC;
  config.pin_d0 = CAM_D0;
  config.pin_d1 = CAM_D1;
  config.pin_d2 = CAM_D2;
  config.pin_d3 = CAM_D3;
  config.pin_d4 = CAM_D4;
  config.pin_d5 = CAM_D5;
  config.pin_d6 = CAM_D6;
  config.pin_d7 = CAM_D7;
  config.pin_vsync = CAM_VSYNC;
  config.pin_href = CAM_HREF;
  config.pin_pclk = CAM_PCLK;
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK)
  {
    Serial.printf("Camera initialization failed: 0x%x\n", result);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor == nullptr || sensor->id.PID != OV2640_PID)
  {
    Serial.println("OV2640 was not detected.");
    esp_camera_deinit();
    return false;
  }

  Serial.printf("OV2640 ready (%s).\n", psramFound() ? "VGA with PSRAM" : "QVGA without PSRAM");
  return true;
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.print("Wi-Fi connected. Camera stream: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/stream");
  }
  else
  {
    Serial.println("\nWi-Fi connection failed; will retry.");
  }
}

esp_err_t indexHandler(httpd_req_t* request)
{
  static const char page[] = "<!doctype html><html><body style='margin:0;background:#111'><img src='/stream' style='width:100%;height:auto'></body></html>";
  httpd_resp_set_type(request, "text/html");
  return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t streamHandler(httpd_req_t* request)
{
  httpd_resp_set_type(request, STREAM_TYPE);
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");

  esp_err_t result = ESP_OK;
  while (result == ESP_OK)
  {
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr)
    {
      xSemaphoreGive(cameraMutex);
      result = ESP_FAIL;
      break;
    }

    char partHeader[64];
    const int headerLength = snprintf(partHeader, sizeof(partHeader), STREAM_PART, static_cast<unsigned>(frame->len));
    result = httpd_resp_send_chunk(request, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (result == ESP_OK && headerLength > 0 && static_cast<size_t>(headerLength) < sizeof(partHeader))
    {
      result = httpd_resp_send_chunk(request, partHeader, headerLength);
    }
    if (result == ESP_OK)
    {
      result = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(frame->buf), frame->len);
    }

    esp_camera_fb_return(frame);
    xSemaphoreGive(cameraMutex);
    if (result == ESP_OK) vTaskDelay(pdMS_TO_TICKS(80));
  }

  httpd_resp_send_chunk(request, nullptr, 0);
  return result;
}

bool startCameraServer()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 4;
  if (httpd_start(&cameraServer, &config) != ESP_OK)
  {
    Serial.println("Could not start camera web server.");
    return false;
  }

  httpd_uri_t root = {};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = indexHandler;
  httpd_register_uri_handler(cameraServer, &root);

  httpd_uri_t stream = {};
  stream.uri = "/stream";
  stream.method = HTTP_GET;
  stream.handler = streamHandler;
  httpd_register_uri_handler(cameraServer, &stream);
  return true;
}

bool captureAndUploadPhoto()
{
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) return false;

  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == nullptr)
  {
    xSemaphoreGive(cameraMutex);
    Serial.println("Photo capture failed.");
    return false;
  }

  if (frame->format != PIXFORMAT_JPEG || frame->len == 0 || frame->len > MAX_PHOTO_BYTES)
  {
    Serial.printf("Photo is not JPEG or exceeds 1 MB (%u bytes).\n", static_cast<unsigned>(frame->len));
    esp_camera_fb_return(frame);
    xSemaphoreGive(cameraMutex);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, PHOTO_UPLOAD_URL))
  {
    esp_camera_fb_return(frame);
    xSemaphoreGive(cameraMutex);
    Serial.println("Could not start photo upload.");
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("x-api-key", API_KEY);
  const int status = http.POST(frame->buf, frame->len);
  http.end();
  esp_camera_fb_return(frame);
  xSemaphoreGive(cameraMutex);

  Serial.printf("Photo upload HTTP status: %d\n", status);
  return status == 201;
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  cameraReady = initializeCamera();
  if (!cameraReady) return;

  cameraMutex = xSemaphoreCreateMutex();
  if (cameraMutex == nullptr)
  {
    Serial.println("Could not create camera lock.");
    cameraReady = false;
    return;
  }

  connectWiFi();
  lastWiFiRetry = millis();
  if (WiFi.status() == WL_CONNECTED) startCameraServer();
}

void loop()
{
  const unsigned long now = millis();
  if (!cameraReady) return;

  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetry >= WIFI_RETRY_MS)
  {
    lastWiFiRetry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (WiFi.status() == WL_CONNECTED && cameraServer == nullptr)
  {
    startCameraServer();
  }

  if (WiFi.status() == WL_CONNECTED &&
      (lastPhotoSuccess == 0 || now - lastPhotoSuccess >= PHOTO_INTERVAL_MS) &&
      (lastPhotoAttempt == 0 || now - lastPhotoAttempt >= PHOTO_RETRY_MS))
  {
    lastPhotoAttempt = now;
    if (captureAndUploadPhoto()) lastPhotoSuccess = millis();
  }

  delay(20);
}