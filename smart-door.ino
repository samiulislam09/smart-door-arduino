#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_http_server.h"

// --- WiFi + server ---
const char* ssid     = "Redmi";
const char* password = "1234567890";
const String serverURL = "http://10.235.156.154:8080/verify";

// --- Pins ---
#define BUZZER_PIN 14
#define FLASH_LED_PIN 4   // AI-Thinker ESP32-CAM built-in white flash LED (active HIGH)

// --- Camera Pins (AI Thinker ESP32-CAM) ---
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// --- Verification timing / cooldowns ---
#define IDLE_INTERVAL_MS    1500   // normal polling cadence when nobody is recognized
#define GRANT_COOLDOWN_MS  10000   // pause after a match so we don't re-arm every 1.5s
#define DENY_COOLDOWN_MS    8000   // back off after a stranger so the buzzer doesn't spam
#define LOWLIGHT_RECHECK_MS  300   // after lighting up, re-capture quickly to recognize

// Decision returned by verifyFace()
#define DOOR_GRANT   1   // face matched           -> server arms the unlock
#define DOOR_DENY    0   // face present, no match -> buzzer
#define DOOR_IDLE   (-1) // no face / error        -> stay silent
#define DOOR_LOWLIGHT 2  // frame too dark: LED just turned on, re-capture the lit frame

unsigned long nextCheckTime = 0;
bool ledOn = false;   // mirrors the physical GPIO 4 state; sent to the server each poll
httpd_handle_t stream_httpd = NULL;

// ---- MJPEG stream boundary ----
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ---- Stream handler ----
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Stream: capture failed");
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf(part_buf, 64, STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
    if (res != ESP_OK) break;
  }
  return res;
}

// ---- Start the streaming server ----
void startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;   // stream on port 81

  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Stream server started on port 81");
  }
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  esp_camera_init(&config);
}

void triggerBuzzer() {
  Serial.println("Access denied! Buzzer ON.");
  for (int i = 0; i < 5; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(200);
    digitalWrite(BUZZER_PIN, LOW);  delay(200);
  }
}

// Drive GPIO 4 from the server's "led" field (tolerate an optional space after the colon).
void applyLed(const String& response) {
  if (response.indexOf("\"led\":\"on\"") >= 0 ||
      response.indexOf("\"led\": \"on\"") >= 0) {
    digitalWrite(FLASH_LED_PIN, HIGH);
    ledOn = true;
  } else if (response.indexOf("\"led\":\"off\"") >= 0 ||
             response.indexOf("\"led\": \"off\"") >= 0) {
    digitalWrite(FLASH_LED_PIN, LOW);
    ledOn = false;
  }
}

// Sends the frame to the Flask server and maps the JSON response to a door decision.
int verifyFace(camera_fb_t* fb) {
  HTTPClient http;
  http.begin(serverURL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-LED-State", ledOn ? "on" : "off");
  int code = http.POST(fb->buf, fb->len);

  if (code != 200) {
    Serial.println("Server error: " + String(code));
    http.end();
    digitalWrite(FLASH_LED_PIN, LOW);
    ledOn = false;
    return DOOR_IDLE;
  }

  String response = http.getString();
  Serial.println("Server response: " + response);
  http.end();

  applyLed(response);

  if (response.indexOf("\"verified\":true") >= 0 ||
      response.indexOf("\"verified\": true") >= 0) {
    return DOOR_GRANT;
  }
  if (response.indexOf("low_light") >= 0) {
    return DOOR_LOWLIGHT;
  }
  if (response.indexOf("no_face") >= 0) {
    return DOOR_IDLE;
  }
  return DOOR_DENY;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);   // start dark

  initCamera();

  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Raw camera stream:  http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/stream");

  startStreamServer();
  Serial.println("System ready.");
}

void loop() {
  if (millis() < nextCheckTime) {
    delay(50);
    return;
  }

  Serial.println("Capturing for verification...");
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed!");
    nextCheckTime = millis() + IDLE_INTERVAL_MS;
    return;
  }

  int decision = verifyFace(fb);
  esp_camera_fb_return(fb);

  switch (decision) {
    case DOOR_GRANT:
      Serial.println("Face matched - server arms the unlock; lock board opens.");
      nextCheckTime = millis() + GRANT_COOLDOWN_MS;
      break;
    case DOOR_DENY:
      triggerBuzzer();
      nextCheckTime = millis() + DENY_COOLDOWN_MS;
      break;
    case DOOR_LOWLIGHT:
      nextCheckTime = millis() + LOWLIGHT_RECHECK_MS;
      break;
    default: // DOOR_IDLE
      nextCheckTime = millis() + IDLE_INTERVAL_MS;
      break;
  }
}
