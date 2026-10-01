/*
 ============================================================
 SMART CLASSROOM MONITORING SYSTEM
 ============================================================

 Hardware:
   ESP32 DevKit V1
   OV7670
   INMP441
   IR sensor
   SIM800L

 FUNCTIONS:
   1. OV7670 camera initialization
   2. OV7670 frame capture test
   3. IR presence detection
   4. INMP441 noise monitoring
   5. SIM800L GSM communication
   6. SMS alert when excessive noise is detected
   7. Classroom status monitoring

 ============================================================
*/


#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "driver/i2s.h"


// ============================================================
//                     PIN CONFIGURATION
// ============================================================


// ---------------- OV7670 ----------------

#define CAM_SIOC   22
#define CAM_SIOD   21

#define CAM_VSYNC  27
#define CAM_HREF   14
#define CAM_PCLK   25
#define CAM_XCLK   32

#define CAM_D0     12
#define CAM_D1     13
#define CAM_D2     15
#define CAM_D3     5
#define CAM_D4     4
#define CAM_D5     16
#define CAM_D6     17
#define CAM_D7     33

// ---------------- INMP441 ----------------

#define I2S_SCK    18
#define I2S_WS     19
#define I2S_SD     26

#define I2S_PORT   I2S_NUM_1


// ---------------- IR SENSOR ----------------

#define IR_PIN     35


// ---------------- SIM800L ----------------

// SIM800L TX -> ESP32 GPIO34
// SIM800L RX <- ESP32 GPIO23

#define SIM800_RX  34
#define SIM800_TX  23

HardwareSerial SIM800(2);


// ============================================================
//                  PROJECT SETTINGS
// ============================================================


// Noise threshold.
// This is NOT calibrated dB.
// It is an initial raw microphone amplitude threshold.

#define NOISE_THRESHOLD 1200


// Time between monitoring cycles

#define MONITOR_INTERVAL 1000


// Prevent repeated SMS messages

#define SMS_COOLDOWN 60000


// Change this to the phone number that should receive alerts

const char PHONE_NUMBER[] = "+91XXXXXXXXXX";

// Replace these placeholders before uploading the sketch.
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* PHOTO_UPLOAD_URL = "https://YOUR-BACKEND-NAME.onrender.com/api/photos";
const char* API_KEY = "YOUR_API_KEY";

const unsigned long PHOTO_INTERVAL_MS = 30UL * 60UL * 1000UL;
const unsigned long PHOTO_RETRY_MS = 60000;
const unsigned long WIFI_RETRY_MS = 10000;


// ============================================================
//                    GLOBAL VARIABLES
// ============================================================

unsigned long lastMonitorTime = 0;

unsigned long lastSMS = 0;

bool cameraReady = false;

bool sim800Ready = false;

bool noisyState = false;

unsigned long lastPhotoSuccess = 0;
unsigned long lastPhotoAttempt = 0;
unsigned long lastWiFiRetry = 0;


// ============================================================
//                   CAMERA INITIALIZATION
// ============================================================

bool initializeCamera()
{
  Serial.println();
  Serial.println("Initializing OV7670...");

  camera_config_t config = {};
  config.pin_pwdn = -1;
  config.pin_reset = -1;
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
  config.xclk_freq_hz = 10000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QQVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK)
  {
    Serial.printf("OV7670 initialization failed: 0x%x\n", result);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor == nullptr || sensor->id.PID != OV7670_PID)
  {
    Serial.println("Camera initialized, but OV7670 was not detected.");
    esp_camera_deinit();
    return false;
  }

  Serial.println("OV7670 ready at QQVGA.");
  return true;
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println();
    Serial.println("Wi-Fi connection failed; will retry.");
  }
}

bool captureAndUploadPhoto()
{
  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == nullptr)
  {
    Serial.println("Photo capture failed.");
    return false;
  }

  uint8_t* jpeg = nullptr;
  size_t jpegLength = 0;
  bool encoded = frame2jpg(frame, 80, &jpeg, &jpegLength);
  esp_camera_fb_return(frame);

  if (!encoded || jpeg == nullptr || jpegLength == 0 || jpegLength > 1024 * 1024)
  {
    Serial.println("JPEG conversion failed or image exceeds 1 MB.");
    free(jpeg);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);

  if (!http.begin(client, PHOTO_UPLOAD_URL))
  {
    Serial.println("Could not start photo upload request.");
    free(jpeg);
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("x-api-key", API_KEY);
  int status = http.POST(jpeg, jpegLength);
  http.end();
  free(jpeg);

  Serial.printf("Photo upload HTTP status: %d\n", status);
  return status == 201;
}


// ============================================================
//                       I2S SETUP
// ============================================================

void setupMicrophone()
{
  Serial.println("Initializing INMP441...");


  i2s_config_t i2s_config =
  {
    .mode =
      (i2s_mode_t)
      (
        I2S_MODE_MASTER |
        I2S_MODE_RX
      ),

    .sample_rate = 16000,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 8,

    .dma_buf_len = 64,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t pin_config =
  {
    .bck_io_num = I2S_SCK,

    .ws_io_num = I2S_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num =
      I2S_SD
  };


  i2s_driver_install(
    I2S_PORT,
    &i2s_config,
    0,
    NULL
  );


  i2s_set_pin(
    I2S_PORT,
    &pin_config
  );


  i2s_zero_dma_buffer(I2S_PORT);


  Serial.println("INMP441 ready.");
}


// ============================================================
//                    READ MICROPHONE
// ============================================================

long readMicrophone()
{
  int32_t samples[64];

  size_t bytesRead = 0;


  esp_err_t result =
    i2s_read(
      I2S_PORT,
      samples,
      sizeof(samples),
      &bytesRead,
      100
    );


  if (
    result != ESP_OK ||
    bytesRead == 0
  )
  {
    return -1;
  }


  int count =
    bytesRead /
    sizeof(int32_t);


  long long total = 0;


  for (
    int i = 0;
    i < count;
    i++
  )
  {
    int32_t sample =
      samples[i] >> 14;


    total +=
      abs(sample);
  }


  return total / count;
}


// ============================================================
//                     IR SENSOR
// ============================================================

bool readIR()
{
  int state =
    digitalRead(IR_PIN);


  /*
     Most common IR modules:

     LOW  = object detected
     HIGH = no object

     If your sensor behaves opposite,
     change LOW to HIGH below.
  */


  return state == LOW;
}


// ============================================================
//                     SIM800L
// ============================================================

void sendATCommand(
  const char *command
)
{
  SIM800.println(command);

  delay(500);


  while (SIM800.available())
  {
    Serial.write(
      SIM800.read()
    );
  }
}


bool initializeSIM800()
{
  Serial.println();
  Serial.println("Initializing SIM800L...");


  SIM800.begin(
    9600,
    SERIAL_8N1,
    SIM800_RX,
    SIM800_TX
  );


  delay(2000);


  SIM800.println("AT");

  delay(1000);


  String response = "";


  while (SIM800.available())
  {
    char c =
      SIM800.read();

    response += c;

    Serial.write(c);
  }


  if (
    response.indexOf("OK")
    >= 0
  )
  {
    Serial.println();
    Serial.println("SIM800L detected.");

    sim800Ready = true;

    return true;
  }


  Serial.println();
  Serial.println(
    "SIM800L did not respond."
  );


  return false;
}


// ============================================================
//                     SEND SMS
// ============================================================

bool sendSMS(
  const char *message
)
{
  if (!sim800Ready)
  {
    return false;
  }


  Serial.println();
  Serial.println("Sending SMS...");


  SIM800.println(
    "AT+CMGF=1"
  );

  delay(500);


  SIM800.print(
    "AT+CMGS=\""
  );

  SIM800.print(
    PHONE_NUMBER
  );

  SIM800.println("\"");

  delay(1000);


  SIM800.print(
    message
  );


  SIM800.write(26);


  delay(5000);


  String response = "";


  while (SIM800.available())
  {
    char c =
      SIM800.read();

    response += c;

    Serial.write(c);
  }


  Serial.println();


  if (
    response.indexOf("OK")
    >= 0
  )
  {
    Serial.println(
      "SMS sent."
    );

    return true;
  }


  Serial.println(
    "SMS sending failed."
  );


  return false;
}


// ============================================================
//                 CLASSROOM STATUS
// ============================================================

void printClassroomStatus(
  bool presence,
  long noise
)
{
  Serial.println();
  Serial.println(
    "========== CLASSROOM STATUS =========="
  );


  Serial.print(
    "IR Presence: "
  );


  if (presence)
  {
    Serial.println(
      "DETECTED"
    );
  }
  else
  {
    Serial.println(
      "NO PRESENCE"
    );
  }


  Serial.print(
    "Noise Level: "
  );


  Serial.println(
    noise
  );


  Serial.print(
    "Noise Status: "
  );


  if (
    noise >= NOISE_THRESHOLD
  )
  {
    Serial.println(
      "HIGH"
    );
  }
  else
  {
    Serial.println(
      "NORMAL"
    );
  }


  Serial.print(
    "Camera: "
  );


  if (cameraReady)
  {
    Serial.println(
      "READY"
    );
  }
  else
  {
    Serial.println(
      "NOT READY"
    );
  }


  Serial.println(
    "======================================"
  );
}


// ============================================================
//                       SETUP
// ============================================================

void setup()
{
  Serial.begin(
    115200
  );


  delay(2000);


  Serial.println();
  Serial.println();
  Serial.println(
    "========================================"
  );
  Serial.println(
    " SMART CLASSROOM MONITORING SYSTEM"
  );
  Serial.println(
    "========================================"
  );


  // ----------------------------------------------------------
  // IR
  // ----------------------------------------------------------

  pinMode(
    IR_PIN,
    INPUT
  );


  // ----------------------------------------------------------
  // CAMERA
  // ----------------------------------------------------------

  cameraReady =
    initializeCamera();


  // ----------------------------------------------------------
  // MICROPHONE
  // ----------------------------------------------------------

  setupMicrophone();


  // ----------------------------------------------------------
  // SIM800L
  // ----------------------------------------------------------

  initializeSIM800();


  // ----------------------------------------------------------
  // WI-FI FOR PHOTO UPLOADS
  // ----------------------------------------------------------

  connectWiFi();
  lastWiFiRetry = millis();


  Serial.println();
  Serial.println(
    "SYSTEM INITIALIZATION COMPLETE"
  );


  Serial.println();
  Serial.println(
    "Monitoring classroom..."
  );
}


// ============================================================
//                       LOOP
// ============================================================

void loop()
{
  unsigned long now =
    millis();


  if (
    now - lastMonitorTime
    < MONITOR_INTERVAL
  )
  {
    return;
  }


  lastMonitorTime =
    now;


  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetry >= WIFI_RETRY_MS)
  {
    lastWiFiRetry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }


  if (
    cameraReady &&
    WiFi.status() == WL_CONNECTED &&
    (lastPhotoSuccess == 0 || now - lastPhotoSuccess >= PHOTO_INTERVAL_MS) &&
    (lastPhotoAttempt == 0 || now - lastPhotoAttempt >= PHOTO_RETRY_MS)
  )
  {
    lastPhotoAttempt = now;
    if (captureAndUploadPhoto())
    {
      lastPhotoSuccess = millis();
    }
  }


  // ----------------------------------------------------------
  // READ IR
  // ----------------------------------------------------------

  bool presence =
    readIR();


  // ----------------------------------------------------------
  // READ MICROPHONE
  // ----------------------------------------------------------

  long noise =
    readMicrophone();


  if (noise < 0)
  {
    noise = 0;
  }


  // ----------------------------------------------------------
  // PRINT STATUS
  // ----------------------------------------------------------

  printClassroomStatus(
    presence,
    noise
  );


  // ----------------------------------------------------------
  // NOISE DETECTION
  // ----------------------------------------------------------

  bool excessiveNoise =
    noise >= NOISE_THRESHOLD;


  // Detect transition:
  // NORMAL → HIGH NOISE

  if (
    excessiveNoise &&
    !noisyState
  )
  {
    Serial.println();
    Serial.println(
      "WARNING: HIGH NOISE DETECTED!"
    );


    // Prevent repeated alerts

    if (
      now - lastSMS
      >= SMS_COOLDOWN
    )
    {
      sendSMS(
        "Smart Classroom Alert: Excessive noise detected."
      );


      lastSMS =
        now;
    }


    noisyState = true;
  }


  // HIGH → NORMAL

  if (!excessiveNoise)
  {
    noisyState = false;
  }
}