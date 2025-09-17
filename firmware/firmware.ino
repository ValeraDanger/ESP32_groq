#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* ssid = "";
const char* password = "";

const char* serverUrl = "http://192.168.0.208:5000/chat";

#define LED_PIN 22

String userPrompt = "";

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // ИНВЕРСИЯ: HIGH = выключено, LOW = включено

  Serial.println("Подключаюсь к WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi подключен!");
  Serial.println("Введите запрос для LLM:");
}

void loop() {
  // Проверяем, есть ли данные в Serial
  if (Serial.available()) {
    userPrompt = Serial.readStringUntil('\n');
    userPrompt.trim();

    if (userPrompt.length() > 0) {
      sendRequest(userPrompt);
      Serial.println("\nВведите следующий запрос:");
    }
  }
}

void sendRequest(String prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi не подключен!");
    return;
  }

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"prompt\":\"" + prompt + "\"}";
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Ответ сервера:");
    Serial.println(response);

    // Парсим JSON
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      // Проверка на tool_calls
      if (doc.containsKey("tool_calls")) {
        JsonArray tools = doc["tool_calls"];
        for (JsonObject tool : tools) {
          const char* name = tool["function"]["name"];
          const char* argsStr = tool["function"]["arguments"];
          if (strcmp(name, "set_led_state") == 0) {
            StaticJsonDocument<256> args;
            deserializeJson(args, argsStr);
            const char* state = args["state"];
            if (strcmp(state, "on") == 0) {
              digitalWrite(LED_PIN, LOW);  // ВКЛ
              Serial.println("💡 LED ВКЛЮЧЕН");
            } else {
              digitalWrite(LED_PIN, HIGH); // ВЫКЛ
              Serial.println("💡 LED ВЫКЛЮЧЕН");
            }
          }
        }
      }

      // Вывод текстового ответа модели (если есть)
      if (doc.containsKey("response") && !doc["response"].isNull()) {
        Serial.print("Ответ модели: ");
        Serial.println(doc["response"].as<String>());
      }

    } else {
      Serial.println("Ошибка парсинга JSON");
      Serial.println(error.c_str());
    }
  } else {
    Serial.printf("Ошибка HTTP: %d\n", httpResponseCode);
  }

  http.end();
}
