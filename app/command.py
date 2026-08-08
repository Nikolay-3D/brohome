#!/usr/bin/env python3
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from zoneinfo import ZoneInfo

from numbers_ru import parse_spoken_number

BASE_DIR = os.environ.get(
    "BROHOME_DIR",
    os.path.dirname(os.path.abspath(__file__)),
)
DEVICES_FILE = BASE_DIR + "/devices.json"
CHANNELS_FILE = BASE_DIR + "/channels.json"
HUB_AUTOMATIONS_FILE = BASE_DIR + "/hub_automations.json"
EXIT_UNKNOWN_COMMAND = 20
EXIT_CONNECT_FAILED = 22

def load_json(path):
    with open(path, "r", encoding="utf-8") as file:
        return json.load(file)


def load_json_optional(path, default):
    try:
        return load_json(path)
    except (FileNotFoundError, json.JSONDecodeError):
        return default


def normalize(text):
    return " ".join(
        text.lower()
        .replace(",", " ")
        .replace(".", " ")
        .replace("!", " ")
        .replace("?", " ")
        .split()
    )


def russian_count_word(value, one, few, many):
    last_two = value % 100
    if 11 <= last_two <= 14:
        return many
    last = value % 10
    if last == 1:
        return one
    if 2 <= last <= 4:
        return few
    return many


def moscow_time_response():
    now = datetime.now(ZoneInfo("Europe/Moscow"))
    hour_word = russian_count_word(now.hour, "час", "часа", "часов")
    if now.minute == 0:
        return f"Сейчас {now.hour} {hour_word} ровно"
    minute_word = russian_count_word(now.minute, "минута", "минуты", "минут")
    return f"Сейчас {now.hour} {hour_word} {now.minute} {minute_word}"


def run_hub_automation(text, source_device):
    registry = load_json_optional(HUB_AUTOMATIONS_FILE, {"hubs": {}})
    hubs = registry.get("hubs", {})
    ordered_hubs = []
    if source_device in hubs:
        ordered_hubs.append((source_device, hubs[source_device]))
    ordered_hubs.extend(
        (name, hub) for name, hub in hubs.items() if name != source_device
    )

    matches = []
    for hub_name, hub in ordered_hubs:
        for action in hub.get("actions", []):
            phrases = action.get("phrases", "")
            if isinstance(phrases, str):
                phrases = re.split(r"[,;\n]+", phrases)
            for phrase in phrases:
                normalized_phrase = normalize(str(phrase))
                if len(normalized_phrase) < 3:
                    continue
                if text == normalized_phrase:
                    matches.append((10000 + len(normalized_phrase), hub_name, hub, action))
                elif normalized_phrase in text:
                    matches.append((len(normalized_phrase), hub_name, hub, action))

    if not matches:
        return None

    _, hub_name, hub, action = max(matches, key=lambda item: item[0])
    query = urllib.parse.urlencode({"id": action.get("id", "")})
    url = "http://{}/api/automation/run?{}".format(hub.get("ip", ""), query)
    print(
        "HUB ACTION:", hub_name, action.get("device", ""),
        action.get("action", ""), "->", action.get("command", ""),
        flush=True,
    )
    try:
        request = urllib.request.Request(url, data=b"", method="POST")
        with urllib.request.urlopen(request, timeout=5) as response:
            response.read()
    except (urllib.error.URLError, ValueError) as exc:
        print("Hub action error:", exc, flush=True)
        print("BROHOME_RESPONSE=хаб недоступен", flush=True)
        return EXIT_CONNECT_FAILED

    print("BROHOME_RESPONSE=готово", flush=True)
    return 0


def active_hub(source_device):
    registry = load_json_optional(HUB_AUTOMATIONS_FILE, {"hubs": {}})
    hubs = registry.get("hubs", {})
    if source_device in hubs:
        return hubs[source_device]
    return next(iter(hubs.values()), None)


def send_hub_commands(source_device, command_type, commands):
    hub = active_hub(source_device)
    if not hub or not hub.get("ip"):
        print("BROHOME_RESPONSE=хаб недоступен", flush=True)
        return EXIT_CONNECT_FAILED
    try:
        for index, name in enumerate(commands):
            query = urllib.parse.urlencode({"type": command_type, "name": name})
            url = "http://{}/api/send?{}".format(hub["ip"], query)
            request = urllib.request.Request(url, data=b"", method="POST")
            with urllib.request.urlopen(request, timeout=5) as response:
                response.read()
            if index + 1 < len(commands):
                # Android TV boxes commonly ignore a second IR key while the
                # first key is still being debounced. Keep channel digits and
                # repeated volume presses clearly separated.
                time.sleep(0.45)
    except (urllib.error.URLError, ValueError) as exc:
        print("Hub remote error:", exc, flush=True)
        print("BROHOME_RESPONSE=хаб недоступен", flush=True)
        return EXIT_CONNECT_FAILED
    print("BROHOME_RESPONSE=готово", flush=True)
    return 0


def resolve_channel_number(text, devices):
    candidate = clean_command_text(text, devices)
    channels = load_json_optional(CHANNELS_FILE, {})
    if candidate in channels:
        return int(channels[candidate])
    if candidate.isdigit():
        return int(candidate)
    return parse_spoken_number(candidate)


def run_ir_voice_command(text, source_device, devices):
    if "убавь громкость" in text or "уменьши громкость" in text or "тише" in text:
        return send_hub_commands(source_device, "ir", ["Vol_down"] * max(1, extract_number(text, 1)))
    if "прибавь громкость" in text or "увеличь громкость" in text or "громче" in text:
        return send_hub_commands(source_device, "ir", ["Vol_up"] * max(1, extract_number(text, 1)))
    if any(phrase in text for phrase in ("выключи звук", "включи звук", "верни звук", "без звука", "mute")):
        return send_hub_commands(source_device, "ir", ["Mute"])
    if text in ("домой", "главный экран", "домашний экран"):
        return send_hub_commands(source_device, "ir", ["Home"])
    if text in ("назад", "вернись назад"):
        return send_hub_commands(source_device, "ir", ["Back"])
    if text in ("ок", "подтверди", "выбери"):
        return send_hub_commands(source_device, "ir", ["ok"])
    if any(word in text for word in ("пристав", "телевизор", "телек", "тв")) and any(
        word in text for word in ("включ", "выключ", "разбуд", "усып", "питание")
    ):
        return send_hub_commands(source_device, "ir", ["pwr"])
    if text in ("приставка", "телевизор", "телек", "тв"):
        return send_hub_commands(source_device, "ir", ["pwr"])
    if "включи" in text or "запусти" in text or "покажи" in text:
        number = resolve_channel_number(text, devices)
        if number is not None:
            return send_hub_commands(source_device, "ir", list(str(number)))
    number = resolve_channel_number(text, devices)
    if number is not None:
        return send_hub_commands(source_device, "ir", list(str(number)))
    return None


def extract_number(text, default=1):
    for word in text.split():
        if word.isdigit():
            return int(word)
    return default


def clean_command_text(text, devices):
    result = text

    words_to_remove = [
        "бро", "пожалуйста",
        "включи", "запусти", "покажи",
        "канал", "телеканал", "телевизор", "телевизоры",
        "везде", "на всех", "оба", "обоих",
    ]

    for device in devices.values():
        words_to_remove.extend(device.get("aliases", []))

    for word in sorted(words_to_remove, key=len, reverse=True):
        result = result.replace(word, " ")

    return " ".join(result.split())


def run(command):
    print("RUN:", " ".join(command), flush=True)
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
    )

    output = (result.stdout or "") + (result.stderr or "")
    if output.strip():
        print(output.rstrip(), flush=True)

    return result.returncode


def main():
    if len(sys.argv) < 2:
        print('Usage: python3 command.py "включи рбк"', flush=True)
        return 1

    text = normalize(" ".join(sys.argv[1:]))
    devices = load_json_optional(DEVICES_FILE, {})

    if text in ("время", "который час", "сколько времени", "московское время"):
        print("BROHOME_RESPONSE=" + moscow_time_response(), flush=True)
        return 0

    source_device = os.environ.get("BROHOME_SOURCE_DEVICE", "").strip()
    hub_result = run_hub_automation(text, source_device)
    if hub_result is not None:
        return hub_result

    if (
        "температур" in text
        or "сколько градусов" in text
        or text in ("погода", "какая погода")
    ):
        return run(["python3", BASE_DIR + "/weather.py"])

    ir_result = run_ir_voice_command(text, source_device, devices)
    if ir_result is not None:
        return ir_result

    print("Не понял команду:", text, flush=True)
    return EXIT_UNKNOWN_COMMAND

if __name__ == "__main__":
    raise SystemExit(main())
