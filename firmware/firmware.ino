/*
  esp32_i2s_ws_voice.ino
  - захват с I2S микрофона
  - отправка PCM чанков по WebSocket на ПК
  - управление записью через Serial: "rec" / "stop"
  Требует: WebSockets, ArduinoJson
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"

// ======= Настройки WiFi и WS =======
const char* WIFI_SSID = "Home71";
const char* WIFI_PSWD = "10134075";

const char* WS_SERVER_IP = "192.168.0.208"; // IP ПК с сервером (замени)
const uint16_t WS_SERVER_PORT = 8765;
const char* WS_PATH = "/";

WebSocketsClient webSocket;
bool ws_connected = false;
bool is_recording = false;

// ======= I2S настройки (взято из твоего примера) =======
#define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT
#define I2S_MIC_SERIAL_CLOCK GPIO_NUM_26
#define I2S_MIC_LEFT_RIGHT_CLOCK GPIO_NUM_32
#define I2S_MIC_SERIAL_DATA GPIO_NUM_33

// I2S конфиг — используем 16k, но читаем 32-bit и приводим к 16-bit
i2s_config_t i2sMemsConfigBothChannels = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = 16000,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S),
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 4,
  .dma_buf_len = 64,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

i2s_pin_config_t i2s_mic_pins = {
  .bck_io_num = I2S_MIC_SERIAL_CLOCK,
  .ws_io_num = I2S_MIC_LEFT_RIGHT_CLOCK,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_MIC_SERIAL_DATA
};

#define I2S_NUM      (I2S_NUM_0)
#define I2S_READ_LEN (2048) // bytes per read (must be even)
uint8_t i2s_read_buffer[I2S_READ_LEN];

// ======= LED =======
const int LED_PIN = 18; // инвертированный: LOW = ON, HIGH = OFF

// ======= JSON / buffers =======
const size_t JSON_BUF_SIZE = 16 * 1024; // для больших ответов

// ======= Функции WS =======
void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    ws_connected = true;
    Serial.println("[ws] connected");
  } else if (type == WStype_DISCONNECTED) {
    ws_connected = false;
    Serial.println("[ws] disconnected");
  } else if (type == WStype_TEXT) {
    String s = String((char*)payload);
    Serial.printf("[ws] TXT: %s\n", s.c_str());
    // обрабатываем JSON ответ от сервера
    DynamicJsonDocument doc(JSON_BUF_SIZE);
    DeserializationError err = deserializeJson(doc, s);
    if (err) {
      Serial.printf("[ws] JSON parse error: %s\n", err.c_str());
      return;
    }
    String cmd = doc["cmd"] | "";
    if (cmd == "result") {
      JsonObject body = doc["body"];
      String transcription = body["transcription"] | "";
      Serial.printf("[ws] Transcription: %s\n", transcription.c_str());
      // получаем llm_raw => choices[0].message и ищем tool_calls / function_call
      JsonObject llm_raw = body["llm_raw"];
      if (!llm_raw.isNull() && llm_raw.containsKey("choices")) {
        JsonArray choices = llm_raw["choices"];
        if (choices.size() > 0) {
          JsonObject message = choices[0]["message"];
          // 1) try tool_calls
          if (message.containsKey("tool_calls")) {
            JsonArray tool_calls = message["tool_calls"];
            for (JsonObject tc : tool_calls) {
              const char* fname = tc["function"]["name"] | "";
              const char* argstr = tc["function"]["arguments"] | "";
              if (strcmp(fname, "set_led_state") == 0) {
                DynamicJsonDocument a(256);
                if (deserializeJson(a, argstr) == DeserializationError::Ok) {
                  const char* state = a["state"] | "";
                  if (strcmp(state, "on") == 0) {
                    digitalWrite(LED_PIN, LOW); Serial.println("[led] ON");
                  } else {
                    digitalWrite(LED_PIN, HIGH); Serial.println("[led] OFF");
                  }
                }
              }
            }
          } else if (message.containsKey("function_call")) {
            JsonObject fc = message["function_call"];
            const char* name = fc["name"] | "";
            const char* args = fc["arguments"] | "";
            if (strcmp(name, "set_led_state") == 0) {
              DynamicJsonDocument a(256);
              if (deserializeJson(a, args) == DeserializationError::Ok) {
                const char* state = a["state"] | "";
                if (strcmp(state, "on") == 0) {
                  digitalWrite(LED_PIN, LOW); Serial.println("[led] ON");
                } else {
                  digitalWrite(LED_PIN, HIGH); Serial.println("[led] OFF");
                }
              }
            }
          }
        }
      }
    } else if (cmd == "ack") {
      Serial.println("[ws] server ack");
    } else if (cmd == "error") {
      Serial.print("[ws] error: "); Serial.println(doc["msg"].as<const char*>());
    }
  } else if (type == WStype_BIN) {
    // не ожидаем бинарных сообщений от сервера
  }
}

void ws_setup() {
  webSocket.begin(WS_SERVER_IP, WS_SERVER_PORT, WS_PATH);
  webSocket.onEvent(wsEvent);
  webSocket.setReconnectInterval(5000);
  Serial.println("[ws] connecting...");
}

// ======= Отправка команд управления =======
void ws_send_start(uint32_t rate = 16000, uint8_t channels = 1, uint8_t sampwidth = 2) {
  DynamicJsonDocument doc(256);
  doc["cmd"] = "start";
  doc["rate"] = rate;
  doc["channels"] = channels;
  doc["sampwidth"] = sampwidth;
  String out; serializeJson(doc, out);
  webSocket.sendTXT(out);
}

void ws_send_end() {
  DynamicJsonDocument doc(64);
  doc["cmd"] = "end";
  String out; serializeJson(doc, out);
  webSocket.sendTXT(out);
}

void ws_send_chunk(const uint8_t* data, size_t len) {
  if (!ws_connected) return;
  webSocket.sendBIN(data, len);
}

// ======= I2S helper: read raw & convert 32->16 =======
size_t i2s_read_and_send_chunk() {
  size_t bytes_read = 0;
  esp_err_t r = i2s_read(I2S_NUM, (void*)i2s_read_buffer, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
  if (r != ESP_OK || bytes_read == 0) return 0;
  // i2s configured as 32bit per sample -> convert to int16 (take high 16 bits)
  // bytes_read is multiple of 4 (32-bit samples). We'll produce bytes_read/2 bytes (16-bit samples).
  // const int32_t* in32 = (const int32_t*)i2s_read_buffer;
  // size_t samples32 = bytes_read / 4;
  // // prepare out buffer (static to avoid stack issues)
  // static uint8_t outbuf[I2S_READ_LEN]; // safe: out size <= in size
  // size_t out_idx = 0;
  // for (size_t i = 0; i < samples32; ++i) {
  //   int32_t s32 = in32[i];
  //   int16_t s16 = (int16_t)(s32 >> 16); // take top 16 bits
  //   outbuf[out_idx++] = (uint8_t)(s16 & 0xFF);
  //   outbuf[out_idx++] = (uint8_t)((s16 >> 8) & 0xFF);
  // }
  // // отправляем outbuf длиной out_idx
  // ws_send_chunk(outbuf, out_idx);
  // return out_idx;

  ws_send_chunk(i2s_read_buffer, bytes_read);
  return bytes_read;
}

// ======= Recording task: блокирующий отправщик =======
void start_i2s() {
  // init i2s
  i2s_driver_install(I2S_NUM, &i2sMemsConfigBothChannels, 0, NULL);
  i2s_set_pin(I2S_NUM, &i2s_mic_pins);
  // enable built-in ADC? Not needed for I2S MEMS
}

void stop_i2s() {
  // stop and uninstall
  i2s_driver_uninstall(I2S_NUM);
}

// ======= Setup & loop =======
void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // выключен (инверсия)
  Serial.println("ESP32 voice -> WS client");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PSWD);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // websocket
  ws_setup();
  // prepare i2s (will be installed on start)
  // note: we will install i2s when start recording
  Serial.println("Type 'rec' to start, 'stop' to finish recording.");
}

void loop() {
  webSocket.loop();

  // Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "rec") {
      if (!ws_connected) {
        Serial.println("WS not connected yet");
        return;
      }
      if (is_recording) {
        Serial.println("Already recording");
        return;
      }
      // start i2s driver & send start
      start_i2s();
      delay(50);
      ws_send_start(16000, 1, 2);
      is_recording = true;
      Serial.println("Recording started. Type 'stop' to finish.");
    } else if (cmd == "stop") {
      if (!is_recording) {
        Serial.println("Not recording");
        return;
      }
      // stop sending chunks and send end
      is_recording = false;
      ws_send_end();
      stop_i2s();
      Serial.println("Recording stopped. Waiting for server result...");
    } else {
      Serial.printf("Unknown cmd: %s\n", cmd.c_str());
    }
  }

  // If recording, read from I2S and send chunks
  if (is_recording) {
    // try to read and send; this will block until data available
    size_t sent = i2s_read_and_send_chunk();
    // small yield
    if (sent == 0) delay(1);
  } else {
    // idle
    delay(10);
  }
}
