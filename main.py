#!/usr/bin/env python3
# audio_ws_server.py
import asyncio
import websockets
import wave
import os
import json
import requests
import uuid
from pathlib import Path

# === НАСТРОЙКИ ===
GROQ_API_KEY = "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"
WHISPER_URL = "https://api.groq.com/openai/v1/audio/transcriptions"
CHAT_URL = "https://api.groq.com/openai/v1/chat/completions"

MODEL_WHISPER = "whisper-large-v3"
MODEL_CHAT = "openai/gpt-oss-20b"


# если нужен прокси, заполните словарь ниже

PROXIES = {"http": "modeler_xioeB8:cycr02pu3myV@185.4.182.66:10152", "https": "modeler_xioeB8:cycr02pu3myV@185.4.182.66:10152"}

OUTPUT_DIR = Path("received_audio")
OUTPUT_DIR.mkdir(exist_ok=True)

WS_HOST = "0.0.0.0"
WS_PORT = 8765

# === вспомогательные ===
def call_llm_with_transcript(transcript_text):
    payload = {
        "model": MODEL_CHAT,
        "messages": [
            {"role": "user", "content": f"Транскрипт пользователя: {transcript_text}"}
        ],
        "tools": [
            {
                "type": "function",
                "function": {
                    "name": "set_led_state",
                    "description": "Включает или выключает свет",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "state": {
                                "type": "string",
                                "enum": ["on", "off"],
                                "description": "on или off"
                            }
                        },
                        "required": ["state"]
                    }
                }
            }
        ]
    }
    headers = {"Authorization": f"Bearer {GROQ_API_KEY}", "Content-Type": "application/json"}
    r = requests.post(CHAT_URL, headers=headers, json=payload, proxies=PROXIES, timeout=30)
    r.raise_for_status()
    return r.json()

async def process_finished_file(ws, meta):
    filepath = meta["filepath"]
    print(f"[server] Отправка {filepath} в Whisper...")
    headers = {"Authorization": f"Bearer {GROQ_API_KEY}"}
    with open(filepath, "rb") as f:
        files = {"file": f}
        data = {"model": MODEL_WHISPER, "temperature": "0", "response_format": "verbose_json"}
        r = requests.post(WHISPER_URL, headers=headers, files=files, data=data, proxies=PROXIES, timeout=60)
    r.raise_for_status()
    whisper_json = r.json()
    transcript = whisper_json.get("text", "")
    print(f"[server] Whisper: {transcript[:200]}")

    # вызов LLM
    llm_json = call_llm_with_transcript(transcript)
    # prepare reply
    reply = {
        "transcription": transcript,
        "whisper_raw": whisper_json,
        "llm_raw": llm_json
    }
    await ws.send(json.dumps({"cmd": "result", "body": reply}, ensure_ascii=False))

async def handler(ws):
    client_id = str(uuid.uuid4())[:8]
    print(f"[server] Клиент подключен: {client_id}")
    wf = None
    meta = {}
    try:
        async for message in ws:
            # бинарные сообщения = PCM chunk
            if isinstance(message, bytes):
                if wf is None:
                    # нет открытого файла — пропускаем
                    print("[server] Получен PCM-чанк, но запись не начата — игнорируем")
                    continue
                wf.writeframes(message)
            else:
                # текстовое управление (JSON)
                try:
                    obj = json.loads(message)
                except Exception as e:
                    print("[server] Некорректный JSON:", message)
                    continue
                cmd = obj.get("cmd")
                if cmd == "start":
                    rate = int(obj.get("rate", 16000))
                    channels = int(obj.get("channels", 1))
                    sampwidth = int(obj.get("sampwidth", 2))
                    fname = OUTPUT_DIR / f"rec_{client_id}.wav"
                    meta = {"filepath": str(fname), "rate": rate, "channels": channels, "sampwidth": sampwidth}
                    wf = wave.open(str(fname), "wb")
                    wf.setnchannels(channels)
                    wf.setsampwidth(sampwidth)
                    wf.setframerate(rate)
                    print(f"[server] Начата запись {fname} ({rate}Hz, {channels}ch, {sampwidth*8}bit)")
                    await ws.send(json.dumps({"cmd": "ack", "msg": "started"}, ensure_ascii=False))
                elif cmd == "end":
                    if wf:
                        wf.close()
                        wf = None
                        print(f"[server] Закончена запись, файл сохранён: {meta['filepath']}")
                        await ws.send(json.dumps({"cmd": "ack", "msg": "ended"}, ensure_ascii=False))
                        await process_finished_file(ws, meta)
                    else:
                        await ws.send(json.dumps({"cmd": "error", "msg": "no recording"}, ensure_ascii=False))
                else:
                    await ws.send(json.dumps({"cmd": "error", "msg": f"unknown cmd {cmd}"}, ensure_ascii=False))
    except websockets.exceptions.ConnectionClosed:
        print(f"[server] Клиент отключился: {client_id}")
    finally:
        if wf:
            wf.close()

async def main():
    print(f"[server] WS слушает ws://{WS_HOST}:{WS_PORT}")
    async with websockets.serve(handler, WS_HOST, WS_PORT, max_size=2**20):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
