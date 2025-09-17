import os
import requests
import json
from flask import Flask, request, Response

PROXY_HOST = '185.4.182.66'
PROXY_PORT = 10152
PROXY_USER = ''
PROXY_PASS = ''

proxy_url = f"http://{PROXY_USER}:{PROXY_PASS}@{PROXY_HOST}:{PROXY_PORT}"
proxies = {"http": proxy_url, "https": proxy_url}

api_key = ''
if not api_key:
    raise ValueError("API ключ не задан!")

app = Flask(__name__)

@app.route("/chat", methods=["POST"])
def chat():
    try:
        data = request.get_json()
        if not data or "prompt" not in data:
            return Response(
                json.dumps({"error": "Отсутствует поле 'prompt'"}, ensure_ascii=False),
                status=400,
                content_type="application/json; charset=utf-8"
            )

        user_prompt = data["prompt"]
        print(f"Запрос от ESP32: {user_prompt}")

        payload = {
            "model": "openai/gpt-oss-20b",
            "messages": [{"role": "user", "content": user_prompt}],
            "tools": [
                {
                    "type": "function",
                    "function": {
                        "name": "set_led_state",
                        "description": "Включает или выключает светодиод на пине 18 (ESP32)",
                        "parameters": {
                            "type": "object",
                            "properties": {
                                "state": {
                                    "type": "string",
                                    "enum": ["on", "off"],
                                    "description": "Состояние светодиода (on=горит, off=выключен)"
                                }
                            },
                            "required": ["state"]
                        }
                    }
                }
            ]
        }

        url = "https://api.groq.com/openai/v1/chat/completions"
        headers = {
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json"
        }

        response = requests.post(url, headers=headers, json=payload, proxies=proxies)
        response.raise_for_status()

        result = response.json()
        choice = result["choices"][0]["message"]

        answer_text = choice.get("content")
        tool_calls = choice.get("tool_calls")

        final_response = {
            "response": answer_text,
            "tool_calls": tool_calls
        }

        print(f"Ответ модели: {final_response}")

        return Response(
            json.dumps(final_response, ensure_ascii=False),
            status=200,
            content_type="application/json; charset=utf-8"
        )

    except requests.exceptions.RequestException as e:
        return Response(
            json.dumps({"error": f"Ошибка запроса API: {str(e)}"}, ensure_ascii=False),
            status=500,
            content_type="application/json; charset=utf-8"
        )

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
