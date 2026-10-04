#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "driver/i2s.h"

// ESP32 DevKit V1: INMP441, IR sensor, and SIM800L.
const int I2S_SCK = 18;
const int I2S_WS = 19;
const int I2S_SD = 26;
const int IR_PIN = 35;
const int SIM800_RX = 34;
const int SIM800_TX = 23;
const i2s_port_t I2S_PORT = I2S_NUM_0;

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* PHOTO_UPLOAD_URL = "https://smart-classroom-z77j.onrender.com/api/photos";
const char* API_KEY = "YOUR_API_KEY";
const char PHONE_NUMBER[] = "+91XXXXXXXXXX";

const long NOISE_THRESHOLD = 1200; // Raw INMP441 amplitude; tune after reading Serial output.
const unsigned long MONITOR_INTERVAL_MS = 1000;
const unsigned long DATA_INTERVAL_MS = 3000;
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long SMS_COOLDOWN_MS = 60000;

HardwareSerial SIM800(2);
unsigned long lastMonitor = 0;
unsigned long lastDataPost = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastSMS = 0;
bool sim800Ready = false;
bool noisyState = false;

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
    Serial.print("Wi-Fi connected: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("\nWi-Fi connection failed; will retry.");
  }
}

bool setupMicrophone()
{
  const i2s_config_t config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  const i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  esp_err_t result = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
  if (result != ESP_OK)
  {
    Serial.printf("I2S install failed: %d\n", result);
    return false;
  }
  result = i2s_set_pin(I2S_PORT, &pins);
  if (result != ESP_OK)
  {
    Serial.printf("I2S pin setup failed: %d\n", result);
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("INMP441 ready.");
  return true;
}

long readMicrophone()
{
  int32_t samples[64];
  size_t bytesRead = 0;
  const esp_err_t result = i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(100));
  if (result != ESP_OK || bytesRead < sizeof(int32_t)) return -1;

  const int count = bytesRead / sizeof(int32_t);
  int64_t total = 0;
  for (int i = 0; i < count; ++i)
  {
    const int32_t sample = samples[i] >> 14;
    total += llabs(static_cast<long long>(sample));
  }
  return total / count;
}

bool postSensorData(long noise, bool presence)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, DATA_UPLOAD_URL))
  {
    Serial.println("Could not start sensor upload.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", API_KEY);
  const String body = "{\"noise\":" + String(noise) + ",\"radar\":" + String(presence ? 1 : 0) + "}";
  const int status = http.POST(body);
  http.end();
  Serial.printf("Sensor upload HTTP status: %d\n", status);
  return status == 201;
}

bool initializeSIM800()
{
  SIM800.begin(9600, SERIAL_8N1, SIM800_RX, SIM800_TX);
  delay(2000);
  SIM800.println("AT");
  delay(1000);
  String response;
  while (SIM800.available()) response += static_cast<char>(SIM800.read());
  sim800Ready = response.indexOf("OK") >= 0;
  Serial.println(sim800Ready ? "SIM800L ready." : "SIM800L did not respond.");
  return sim800Ready;
}

bool sendSMS(const char* message)
{
  if (!sim800Ready) return false;
  SIM800.println("AT+CMGF=1");
  delay(500);
  SIM800.print("AT+CMGS=\"");
  SIM800.print(PHONE_NUMBER);
  SIM800.println("\"");
  delay(1000);
  SIM800.print(message);
  SIM800.write(26);
  delay(5000);

  String response;
  while (SIM800.available()) response += static_cast<char>(SIM800.read());
  Serial.println(response.indexOf("OK") >= 0 ? "SMS sent." : "SMS sending failed.");
  return response.indexOf("OK") >= 0;
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  pinMode(IR_PIN, INPUT);

  setupMicrophone();
  initializeSIM800();
  connectWiFi();
  lastWiFiRetry = millis();
  Serial.println("DevKit monitoring started.");
}

void loop()
{
  const unsigned long now = millis();
  if (now - lastMonitor < MONITOR_INTERVAL_MS) return;
  lastMonitor = now;

  const bool presence = digitalRead(IR_PIN) == LOW;
  long noise = readMicrophone();
  if (noise < 0) noise = 0;

  Serial.printf("IR: %s | Mic raw amplitude: %ld\n", presence ? "PRESENT" : "CLEAR", noise);

  const bool excessiveNoise = noise >= NOISE_THRESHOLD;
  if (excessiveNoise && !noisyState)
  {
    Serial.println("High microphone level detected.");
    if (lastSMS == 0 || now - lastSMS >= SMS_COOLDOWN_MS)
    {
      sendSMS("Smart Classroom Alert: Excessive noise detected.");
      lastSMS = now;
    }
    noisyState = true;
  }
  if (!excessiveNoise) noisyState = false;

  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetry >= WIFI_RETRY_MS)
  {
    lastWiFiRetry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (now - lastDataPost >= DATA_INTERVAL_MS)
  {
    lastDataPost = now;
    if (WiFi.status() == WL_CONNECTED) postSensorData(noise, presence);
    else Serial.println("Sensor data not sent: Wi-Fi is disconnected.");
  }
}