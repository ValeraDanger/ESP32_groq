/*
  esp32_i2s_ws_voice.ino
  - захват с I2S микрофона
  - отправка PCM чанков по WebSocket на ПК
  - управление записью по кнопке на GPIO15
  Требует: WebSockets, ArduinoJson
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include <GyverOLED.h>

// ======= Настройки WiFi и WS =======
const char* WIFI_SSID = "Home71";
const char* WIFI_PSWD = "10134075";

const char* WS_SERVER_IP = "192.168.0.208"; // IP ПК с сервером (замени)
const uint16_t WS_SERVER_PORT = 8765;
const char* WS_PATH = "/";

WebSocketsClient webSocket;
bool ws_connected = false;
bool is_recording = false;

// ======= OLED Display =======
GyverOLED<SSD1306_128x64, OLED_BUFFER> oled;
String current_display_text = "";
unsigned long last_scroll_time = 0;
int scroll_position = 0;
const int scroll_delay = 150; // ms между прокруткой
const int max_chars_per_line = 35; // примерно для шрифта по умолчанию
const int max_lines = 8; // для 64px высоты

// ======= I2S настройки (взято из твоего примера) =======
#define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT
#define I2S_MIC_SERIAL_CLOCK GPIO_NUM_26
#define I2S_MIC_LEFT_RIGHT_CLOCK GPIO_NUM_32
#define I2S_MIC_SERIAL_DATA GPIO_NUM_33

// I2S конфиг
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
#define I2S_READ_LEN (2048) // bytes per read
uint8_t i2s_read_buffer[I2S_READ_LEN];

// ======= Управление =======
const int LED_PIN = 18;     // инвертированный: LOW = ON, HIGH = OFF
const int BUTTON_PIN = 15;  // кнопка для старта/стопа записи

// ======= JSON / buffers =======
const size_t JSON_BUF_SIZE = 16 * 1024; // для больших ответов

// ======= OLED Display Functions =======
void oled_init() {
  oled.init();
  oled.clear();
  oled.setScale(1);
  oled.setCursor(0, 0);
  oled.print("ESP32 Voice Ready");
  oled.update();
}

void oled_show_text(String text) {
  current_display_text = text;
  scroll_position = 0;
  oled_update_display();
}

void oled_show_status(String status) {
  oled.clear();
  oled.setScale(2);
  oled.setCursor(0, 2);
  oled.print(status);
  oled.update();
  delay(2000); // Показать статус 2 секунды
}

void oled_show_led_state(bool led_on) {
  if (led_on) {
    // Заливаем экран белым - рисуем заполненный прямоугольник на весь экран
    oled.clear();
    oled.rect(0, 0, 127, 63, 1); // 1 означает заполненный
  } else {
    // Заливаем экран черным
    oled.clear();
  }
  oled.update();
  delay(2000); // Показать состояние 2 секунды
}

void oled_update_display() {
  if (current_display_text.length() == 0) return;
  
  oled.clear();
  oled.setScale(1);
  
  // Разбиваем текст на строки
  int total_chars = current_display_text.length();
  int chars_per_screen = max_chars_per_line * max_lines;
  
  if (total_chars <= chars_per_screen) {
    // Текст помещается на один экран
    int y = 0;
    int start_pos = 0;
    while (start_pos < total_chars && y < max_lines) {
      int end_pos = start_pos + max_chars_per_line;
      if (end_pos > total_chars) end_pos = total_chars;
      
      String line = current_display_text.substring(start_pos, end_pos);
      oled.setCursor(0, y);
      oled.print(line);
      
      start_pos = end_pos;
      y++;
    }
  } else {
    // Нужна прокрутка
    int start_char = scroll_position;
    int y = 0;
    
    while (y < max_lines && start_char < total_chars) {
      int end_char = start_char + max_chars_per_line;
      if (end_char > total_chars) end_char = total_chars;
      
      String line = current_display_text.substring(start_char, end_char);
      oled.setCursor(0, y);
      oled.print(line);
      
      start_char = end_char;
      y++;
    }
  }
  
  oled.update();
}

void oled_handle_scrolling() {
  if (current_display_text.length() == 0) return;
  
  int total_chars = current_display_text.length();
  int chars_per_screen = max_chars_per_line * max_lines;
  
  if (total_chars > chars_per_screen) {
    if (millis() - last_scroll_time > scroll_delay) {
      scroll_position += max_chars_per_line;
      if (scroll_position >= total_chars) {
        scroll_position = 0; // Возврат к началу
      }
      oled_update_display();
      last_scroll_time = millis();
    }
  }
}

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
      // получаем llm_raw => choices[0].message и ищем tool_calls / function_call
      JsonObject llm_raw = body["llm_raw"];
      if (!llm_raw.isNull() && llm_raw.containsKey("choices")) {
        JsonArray choices = llm_raw["choices"];
        if (choices.size() > 0) {
          JsonObject message = choices[0]["message"];
          bool has_function_call = false;
          // 1) try tool_calls
          if (message.containsKey("tool_calls")) {
            has_function_call = true;
            JsonArray tool_calls = message["tool_calls"];
            for (JsonObject tc : tool_calls) {
              const char* fname = tc["function"]["name"] | "";
              const char* argstr = tc["function"]["arguments"] | "";
              if (strcmp(fname, "set_led_state") == 0) {
                DynamicJsonDocument a(256);
                if (deserializeJson(a, argstr) == DeserializationError::Ok) {
                  const char* state = a["state"] | "";
                  if (strcmp(state, "on") == 0) {
                    oled_show_led_state(true); // Белый экран
                    Serial.println("[led] ON");
                  } else {
                    oled_show_led_state(false); // Черный экран
                    Serial.println("[led] OFF");
                  }
                }
              }
            }
          } else if (message.containsKey("function_call")) {
            has_function_call = true;
            JsonObject fc = message["function_call"];
            const char* name = fc["name"] | "";
            const char* args = fc["arguments"] | "";
            if (strcmp(name, "set_led_state") == 0) {
              DynamicJsonDocument a(256);
              if (deserializeJson(a, args) == DeserializationError::Ok) {
                const char* state = a["state"] | "";
                if (strcmp(state, "on") == 0) {
                  oled_show_led_state(true); // Белый экран
                  Serial.println("[led] ON");
                } else {
                  oled_show_led_state(false); // Черный экран
                  Serial.println("[led] OFF");
                }
              }
            }
          }
          
          // Выводим ответ LLM только если не было вызовов функций
          if (!has_function_call && message.containsKey("content")) {
            String llm_response = message["content"] | "";
            if (llm_response.length() > 0) {
              Serial.printf("[LLM Response]: %s\n", llm_response.c_str());
              oled_show_text(llm_response);
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

// ======= I2S helper =======
size_t i2s_read_and_send_chunk() {
  size_t bytes_read = 0;
  esp_err_t r = i2s_read(I2S_NUM, (void*)i2s_read_buffer, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
  if (r != ESP_OK || bytes_read == 0) return 0;

  ws_send_chunk(i2s_read_buffer, bytes_read);
  return bytes_read;
}

// ======= Recording task: блокирующий отправщик =======
void start_i2s() {
  i2s_driver_install(I2S_NUM, &i2sMemsConfigBothChannels, 0, NULL);
  i2s_set_pin(I2S_NUM, &i2s_mic_pins);
}

void stop_i2s() {
  i2s_driver_uninstall(I2S_NUM);
}

// ======= Setup & loop =======
void setup() {
  Serial.begin(115200);
  delay(500);
  
  // Инициализация OLED дисплея
  oled_init();
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // выключен (инверсия)

  pinMode(BUTTON_PIN, INPUT_PULLUP); // Настраиваем кнопку как вход с подтяжкой
  
  Serial.println("ESP32 voice -> WS client");

  // WiFi
  oled_show_status("WiFi...");
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
  oled_show_status("WS Connect");
  ws_setup();
  
  oled_show_text("Hold button to record");
  Serial.println("Hold the button to start recording.");
}

void loop() {
  webSocket.loop();
  
  // Обработка прокрутки текста на OLED
  oled_handle_scrolling();

  // --- Управление записью по кнопке ---
  int button_state = digitalRead(BUTTON_PIN);

  // Если кнопка НАЖАТА (LOW) и запись НЕ идет
  if (button_state == LOW && !is_recording) {
    if (!ws_connected) {
      Serial.println("WS not connected yet");
      oled_show_text("WS not connected");
      delay(100); // небольшая задержка перед повторной проверкой
      return;
    }
    
    // Начинаем запись
    start_i2s();
    delay(50); // Даем I2S инициализироваться
    ws_send_start(16000, 1, 2);
    is_recording = true;
    oled_show_text("Recording...");
    Serial.println("Recording started...");
  } 
  // Если кнопка ОТПУЩЕНА (HIGH) и запись ИДЕТ
  else if (button_state == HIGH && is_recording) {
    // Останавливаем запись
    is_recording = false;
    ws_send_end();
    stop_i2s();
    oled_show_text("Processing...");
    Serial.println("Recording stopped. Waiting for server result...");
  }

  // Если запись активна, читаем и отправляем данные
  if (is_recording) {
    size_t sent = i2s_read_and_send_chunk();
    if (sent == 0) {
      delay(1);
    }
  } else {
    // В режиме ожидания
    delay(10);
  }
}