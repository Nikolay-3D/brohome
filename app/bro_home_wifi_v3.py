#!/usr/bin/env python3
"""
BroHome over Wi-Fi microphone.

Replaces the local PyAudio microphone with an ESP32 TCP audio stream.

Flow:
1. ESP32 connects and streams ADC samples at 12000 Hz.
2. BroHome converts them to signed PCM in memory.
3. Vosk continuously listens only for the wake word.
4. After "Бро":
   - says "слушаю";
   - records 4 seconds from ESP32;
   - overwrites voice_full.wav in the BroHome installation directory;
   - recognizes command with Google SpeechRecognition;
   - calls command.py;
5. No continuous WAV archive is created.
"""

from __future__ import annotations

import json
import os
import re
import socket
import subprocess
import threading
import time
import urllib.request
import wave
from dataclasses import dataclass
from typing import Optional

import speech_recognition as sr
from vosk import KaldiRecognizer, Model


BASE = os.environ.get(
    "BROHOME_DIR",
    os.path.dirname(os.path.abspath(__file__)),
)
VOSK_MODEL = BASE + "/models/vosk-ru"
WAV = BASE + "/voice_full.wav"
COMMAND = BASE + "/command.py"
LISTENING_WAV = BASE + "/leda-ru_8916357.wav"
REPEAT_WAV = BASE + "/leda-ru_povtorite.wav"
HUB_REGISTRY_FILE = BASE + "/hub_automations.json"

HOST = os.environ.get("BROHOME_HOST", "0.0.0.0")
PORT = int(os.environ.get("BROHOME_PORT", "5001"))
ALSA_DEVICE = os.environ.get("BROHOME_ALSA_DEVICE", "plughw:1,0")

COMMAND_SECONDS = 4
ONE_SHOT_ENABLED = True
ONE_SHOT_PREROLL_SECONDS = 0.5
ONE_SHOT_TAIL_SECONDS = 2.0
VOSK_RATE = 16000
ESP_READ_TIMEOUT = 5.0
ESP_AUDIO_BUFFER_SECONDS = 10.0
MIC_GAIN = max(0.1, min(4.0, float(os.environ.get("BROHOME_MIC_GAIN", "3.0"))))
MIC_PEAK_LIMIT = 30000
EXIT_UNKNOWN_COMMAND = 20
EXIT_CONNECT_FAILED = 22

# Only accept complete words.  The former substring matching also treated
# ordinary TV words such as "программа" and "транспорт" as the alias "про".
# "брок" is retained because the kitchen microphone has produced this stable
# Vosk transcription for a clearly spoken "Бро".
WAKE_WORDS = frozenset({"бро", "брок", "броу"})
WAKE_PARTIAL_CONFIRMATIONS = 2


@dataclass(frozen=True)
class StreamInfo:
    device: str
    rate: int
    bits: int
    channels: int
    encoding: str


@dataclass
class HubOutput:
    conn: socket.socket
    lock: threading.Lock


ACTIVE_HUBS: dict[str, HubOutput] = {}
ACTIVE_HUBS_LOCK = threading.Lock()
CURRENT_DEVICE = threading.local()
HUB_REGISTRY_LOCK = threading.Lock()


def sync_hub_registry(device: str, ip: str, output: HubOutput) -> None:
    """Poll the hub HTTP registry while this audio connection is current."""
    last_serialized = ""
    while True:
        with ACTIVE_HUBS_LOCK:
            if ACTIVE_HUBS.get(device) is not output:
                return
        try:
            with urllib.request.urlopen(
                f"http://{ip}/api/automations", timeout=4
            ) as response:
                payload = json.load(response)
            if not isinstance(payload.get("actions"), list):
                raise ValueError("invalid actions registry")
            serialized = json.dumps(payload, ensure_ascii=False, sort_keys=True)
            if serialized != last_serialized:
                with HUB_REGISTRY_LOCK:
                    try:
                        with open(HUB_REGISTRY_FILE, "r", encoding="utf-8") as file:
                            registry = json.load(file)
                    except (FileNotFoundError, json.JSONDecodeError):
                        registry = {"version": 1, "hubs": {}}
                    registry.setdefault("hubs", {})[device] = {
                        "ip": ip,
                        "actions": payload["actions"],
                        "updated": int(time.time()),
                    }
                    temporary = HUB_REGISTRY_FILE + ".tmp"
                    with open(temporary, "w", encoding="utf-8") as file:
                        json.dump(registry, file, ensure_ascii=False, indent=2)
                        file.write("\n")
                    os.replace(temporary, HUB_REGISTRY_FILE)
                last_serialized = serialized
                print(
                    f"Реестр {device} синхронизирован: "
                    f"{len(payload['actions'])} действий"
                )
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"Не удалось синхронизировать реестр {device}: {exc}")
        time.sleep(10)


class ContinuousAudioReceiver:
    """Continuously drain ESP TCP audio into a bounded in-memory buffer."""

    def __init__(
        self,
        conn: socket.socket,
        initial: bytes,
        max_buffer_bytes: int,
    ) -> None:
        self._conn = conn
        self._buffer = bytearray(initial)
        self._max_buffer_bytes = max(2, max_buffer_bytes)
        self._condition = threading.Condition()
        self._error: Optional[Exception] = None
        self._dropped_since_discard = 0

        self._trim_oldest_locked()

        self._thread = threading.Thread(
            target=self._receive_loop,
            name="esp-audio-receiver",
            daemon=True,
        )

    def start(self) -> None:
        self._thread.start()

    def _trim_oldest_locked(self) -> None:
        overflow = len(self._buffer) - self._max_buffer_bytes
        if overflow <= 0:
            return

        # Audio is uint16 little-endian. Always discard whole samples so the
        # remaining stream stays aligned even if recv() split a sample.
        drop_bytes = overflow + (overflow % 2)
        del self._buffer[:drop_bytes]
        self._dropped_since_discard += drop_bytes

    def _receive_loop(self) -> None:
        try:
            while True:
                chunk = self._conn.recv(65536)
                if not chunk:
                    raise ConnectionError("ESP32 закрыла соединение")

                with self._condition:
                    self._buffer.extend(chunk)
                    self._trim_oldest_locked()
                    self._condition.notify_all()
        except Exception as exc:
            with self._condition:
                self._error = exc
                self._condition.notify_all()

    def read_exact(self, count: int) -> bytes:
        with self._condition:
            while len(self._buffer) < count and self._error is None:
                self._condition.wait()

            # A receive timeout means the live TCP stream is dead. Do not
            # process buffered stale audio before reconnecting.
            if self._error is not None:
                raise self._error

            result = bytes(self._buffer[:count])
            del self._buffer[:count]
            return result

    def discard(self) -> int:
        with self._condition:
            if self._error is not None:
                raise self._error

            discarded = len(self._buffer) + self._dropped_since_discard
            self._buffer.clear()
            self._dropped_since_discard = 0
            return discarded


def has_wake_word(text: str) -> bool:
    words = re.findall(r"[0-9a-zа-яё]+", text.lower())
    return any(word in WAKE_WORDS for word in words)


def clean_text(text: str) -> str:
    text = text.lower()
    text = text.replace("слушаю", "")
    text = text.replace("слушай", "")

    replacements = {
        "убив громкость": "убавь громкость",
        "убай громкость": "убавь громкость",
        "убай фронкость": "убавь громкость",
        "убей громкость": "убавь громкость",
        "убери громкость": "убавь громкость",
        "убавить громкость": "убавь громкость",
        "прибавить громкость": "прибавь громкость",
        "добавь громкость": "прибавь громкость",
        "на один": "на 1",
        "на два": "на 2",
        "на три": "на 3",
        "на четыре": "на 4",
        "на пять": "на 5",
        "на шесть": "на 6",
        "на семь": "на 7",
        "на восемь": "на 8",
        "на девять": "на 9",
        "на десять": "на 10",
        "рыбака": "рбк",
        "рыбка": "рбк",
        "рыбак": "рбк",
        "маскувают 24": "москва 24",
        "москву 24": "москва 24",
        "sts": "стс",
        "эс тэ эс": "стс",
        "эстээс": "стс",
    }

    for old, new in replacements.items():
        text = text.replace(old, new)

    text = re.sub(r"[.,!?]", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def command_after_wake_word(text: str) -> str:
    words = re.findall(r"[0-9a-zа-яё]+", text.lower())
    for index, word in enumerate(words):
        if word in WAKE_WORDS:
            return " ".join(words[index + 1:]).strip()
    return ""


def execute_command(
    text: str,
    source_device: str,
    speak_on_unknown: bool = True,
) -> int:
    if not text:
        print("Пустая команда")
        if speak_on_unknown:
            speak("повторите")
        return EXIT_UNKNOWN_COMMAND

    print("Распознано:", text)

    env = os.environ.copy()
    env["BROHOME_SOURCE_DEVICE"] = source_device

    result = subprocess.run(
        ["python3", COMMAND, text],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )

    output = (result.stdout or "") + (result.stderr or "")
    if output.strip():
        print(output.rstrip())

    response = ""
    for line in output.splitlines():
        if line.startswith("BROHOME_RESPONSE="):
            response = line.split("=", 1)[1].strip()

    if response:
        speak(response)
    elif result.returncode == EXIT_CONNECT_FAILED:
        speak("не подключена")
    elif result.returncode == EXIT_UNKNOWN_COMMAND:
        if speak_on_unknown:
            speak("повторите")
    elif result.returncode != 0:
        speak("повторите")

    return result.returncode


def read_wav_for_hub(path: str) -> tuple[int, bytes]:
    with wave.open(path, "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise ValueError("Hub playback requires mono 16-bit PCM WAV")
        return source.getframerate(), source.readframes(source.getnframes())


def send_wav_to_hub(device: str, path: str) -> float:
    if not device:
        return 0.0

    with ACTIVE_HUBS_LOCK:
        output = ACTIVE_HUBS.get(device)
    if output is None:
        return 0.0

    rate, pcm = read_wav_for_hub(path)
    header = f"BROHOME_PLAY/1 rate={rate} length={len(pcm)}\n".encode("ascii")
    try:
        with output.lock:
            output.conn.sendall(header)
            output.conn.sendall(pcm)
    except OSError:
        with ACTIVE_HUBS_LOCK:
            if ACTIVE_HUBS.get(device) is output:
                ACTIVE_HUBS.pop(device, None)
        return 0.0

    return len(pcm) / (rate * 2)


def speak(text: str) -> None:
    if text.strip().lower() == "повторите":
        wav = REPEAT_WAV
    else:
        wav = "/tmp/bro_speak.wav"
        subprocess.run(
            ["espeak-ng", "-v", "ru", "-s", "90", "-w", wav, text],
            check=False,
        )
    device = getattr(CURRENT_DEVICE, "name", "")
    if send_wav_to_hub(device, wav) > 0:
        print(f"Ответ отправлен на динамик ESP32: {device}")
        return

    subprocess.run(
        ["aplay", "-D", ALSA_DEVICE, wav],
        check=False,
    )


def prepare_speech_wav(text: str) -> str:
    wav = "/tmp/bro_speak.wav"
    subprocess.run(
        ["espeak-ng", "-v", "ru", "-s", "90", "-w", wav, text],
        check=False,
    )
    return wav


def speak_while_discarding(
    text: str,
    receiver: ContinuousAudioReceiver,
) -> int:
    """Play a prompt while consuming ESP audio in real time."""
    # wav = prepare_speech_wav(text)  # Старый синтез речи, оставлен для отката.
    wav = LISTENING_WAV
    discarded = receiver.discard()

    device = getattr(CURRENT_DEVICE, "name", "")
    playback_seconds = send_wav_to_hub(device, wav)
    if playback_seconds > 0:
        deadline = time.monotonic() + playback_seconds
        while time.monotonic() < deadline:
            discarded += receiver.discard()
            time.sleep(0.02)
        discarded += receiver.discard()
        return discarded

    player = subprocess.Popen(
        ["aplay", "-D", ALSA_DEVICE, wav],
        stdout=subprocess.DEVNULL,
        stderr=None,
    )

    while player.poll() is None:
        discarded += receiver.discard()
        time.sleep(0.02)

    discarded += receiver.discard()
    return discarded


def google_recognize() -> str:
    print("Распознаю через Google...")
    recognizer = sr.Recognizer()

    try:
        with sr.AudioFile(WAV) as source:
            audio_data = recognizer.record(source)

        result = recognizer.recognize_google(
            audio_data,
            language="ru-RU",
        )
        return clean_text(result)

    except sr.UnknownValueError:
        print("Google STT: не удалось распознать речь")
        return ""
    except sr.RequestError as exc:
        print("Google STT error:", exc)
        return ""
    except Exception as exc:
        print("Google STT unexpected error:", exc)
        return ""


def receive_header(conn: socket.socket) -> tuple[StreamInfo, bytearray]:
    buffer = bytearray()

    while b"\n" not in buffer:
        chunk = conn.recv(256)
        if not chunk:
            raise ConnectionError("ESP32 отключилась до заголовка")
        buffer.extend(chunk)

        if len(buffer) > 512:
            raise ValueError("Слишком длинный заголовок")

    line, trailing = bytes(buffer).split(b"\n", 1)
    parts = line.decode("ascii", errors="strict").strip().split()

    if not parts or parts[0] != "BROHOME_AUDIO/1":
        raise ValueError("Неизвестный протокол")

    values: dict[str, str] = {}

    for part in parts[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            values[key] = value

    info = StreamInfo(
        device=values["device"],
        rate=int(values["rate"]),
        bits=int(values["bits"]),
        channels=int(values["channels"]),
        encoding=values["encoding"],
    )

    if info.bits != 16 or info.channels != 1:
        raise ValueError("Поддерживается только mono 16-bit")
    if info.encoding != "adc_u16le":
        raise ValueError("Неподдерживаемая кодировка")

    return info, bytearray(trailing)


def recv_exact(
    receiver: ContinuousAudioReceiver,
    count: int,
) -> bytes:
    return receiver.read_exact(count)


def discard_buffered_audio(
    receiver: ContinuousAudioReceiver,
) -> int:
    """Discard stale audio already drained by the background receiver."""
    return receiver.discard()


def adc_raw_to_pcm(raw: bytes) -> bytes:
    if len(raw) % 2:
        raw = raw[:-1]

    samples = [
        raw[index] | (raw[index + 1] << 8)
        for index in range(0, len(raw), 2)
    ]

    if not samples:
        return b""

    center = sum(samples) / len(samples)
    centered = [sample - center for sample in samples]
    peak = max(abs(sample) for sample in centered)
    # Restore far-field sensitivity while preventing the hard clipping that
    # previously distorted loud speech and TV audio at a fixed 3x gain.
    applied_gain = min(MIC_GAIN, MIC_PEAK_LIMIT / peak) if peak else MIC_GAIN
    pcm = bytearray()

    for sample in centered:
        value = int(sample * applied_gain)
        value = max(-32768, min(32767, value))
        pcm.extend(value.to_bytes(2, "little", signed=True))

    return bytes(pcm)


def resample_pcm16_mono(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """Simple linear resampling for mono signed 16-bit PCM."""
    if src_rate == dst_rate:
        return pcm

    if len(pcm) < 4:
        return pcm

    samples = [
        int.from_bytes(pcm[i:i + 2], "little", signed=True)
        for i in range(0, len(pcm) - 1, 2)
    ]

    if len(samples) < 2:
        return pcm

    out_count = max(1, round(len(samples) * dst_rate / src_rate))
    result = bytearray()

    for out_index in range(out_count):
        src_pos = out_index * src_rate / dst_rate
        left = int(src_pos)

        if left >= len(samples) - 1:
            value = samples[-1]
        else:
            fraction = src_pos - left
            value = round(
                samples[left] * (1.0 - fraction)
                + samples[left + 1] * fraction
            )

        value = max(-32768, min(32767, value))
        result.extend(value.to_bytes(2, "little", signed=True))

    return bytes(result)


def save_command_wav(pcm: bytes, rate: int) -> None:
    with wave.open(WAV, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(pcm)


def process_client(conn: socket.socket, address: tuple[str, int], model: Model) -> None:
    # ESP streams audio continuously. If no bytes arrive for several seconds,
    # the old TCP session is considered dead so a rebooted ESP can reconnect.
    conn.settimeout(ESP_READ_TIMEOUT)

    conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    if hasattr(socket, "TCP_KEEPIDLE"):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 10)
    if hasattr(socket, "TCP_KEEPINTVL"):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 3)
    if hasattr(socket, "TCP_KEEPCNT"):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)

    info, initial_audio = receive_header(conn)
    CURRENT_DEVICE.name = info.device
    output = HubOutput(conn, threading.Lock())
    with ACTIVE_HUBS_LOCK:
        ACTIVE_HUBS[info.device] = output
    threading.Thread(
        target=sync_hub_registry,
        args=(info.device, address[0], output),
        name=f"hub-registry-{info.device}",
        daemon=True,
    ).start()

    max_buffer_bytes = round(
        info.rate * info.channels * (info.bits // 8) * ESP_AUDIO_BUFFER_SECONDS
    )
    receiver = ContinuousAudioReceiver(
        conn,
        initial_audio,
        max_buffer_bytes,
    )
    receiver.start()

    print(
        f"ESP32 подключена: {info.device}, "
        f"{address[0]}:{address[1]}, {info.rate} Гц"
    )

    # About 0.25 sec per Vosk chunk.
    chunk_samples = max(1, info.rate // 4)
    chunk_bytes = chunk_samples * 2
    pre_roll_limit = round(info.rate * ONE_SHOT_PREROLL_SECONDS) * 2
    pre_roll_pcm = bytearray()

    recognizer = KaldiRecognizer(model, VOSK_RATE)
    recognizer.SetWords(False)

    print(f"BroHome слушает слово 'Бро'... Vosk: {VOSK_RATE} Гц")
    last_partial = ""
    wake_partial_hits = 0

    while True:
        raw = recv_exact(receiver, chunk_bytes)
        pcm = adc_raw_to_pcm(raw)
        pre_roll_pcm.extend(pcm)
        if len(pre_roll_pcm) > pre_roll_limit:
            del pre_roll_pcm[:-pre_roll_limit]

        vosk_pcm = resample_pcm16_mono(pcm, info.rate, VOSK_RATE)

        accepted = recognizer.AcceptWaveform(vosk_pcm)

        partial = json.loads(
            recognizer.PartialResult()
        ).get("partial", "").lower().strip()

        if partial and partial != last_partial:
            print("Vosk partial:", partial)
            last_partial = partial

        if not partial:
            last_partial = ""

        final_text = ""

        if accepted:
            final_text = json.loads(
                recognizer.Result()
            ).get("text", "").lower().strip()

            if final_text:
                print("Vosk:", final_text)

        if has_wake_word(partial):
            wake_partial_hits += 1
        else:
            wake_partial_hits = 0

        wake_confirmed = (
            has_wake_word(final_text)
            or wake_partial_hits >= WAKE_PARTIAL_CONFIRMATIONS
        )
        if not wake_confirmed:
            continue

        print("Бро услышал")

        recognizer = KaldiRecognizer(model, VOSK_RATE)
        recognizer.SetWords(False)
        last_partial = ""
        wake_partial_hits = 0

        if ONE_SHOT_ENABLED:
            print("Проверяю команду в одной фразе...")
            tail_bytes = round(info.rate * ONE_SHOT_TAIL_SECONDS) * 2
            tail_raw = recv_exact(receiver, tail_bytes)
            tail_pcm = adc_raw_to_pcm(tail_raw)
            save_command_wav(bytes(pre_roll_pcm) + tail_pcm, info.rate)

            one_shot_text = google_recognize()
            one_shot_command = command_after_wake_word(one_shot_text)
            if (
                not one_shot_command
                and one_shot_text
                and not has_wake_word(one_shot_text)
            ):
                # Google sometimes omits the already detected wake word.
                one_shot_command = one_shot_text
            if one_shot_command:
                print("Команда после 'Бро':", one_shot_command)
                code = execute_command(
                    one_shot_command,
                    info.device,
                    speak_on_unknown=False,
                )
                if code != EXIT_UNKNOWN_COMMAND:
                    discarded_after = discard_buffered_audio(receiver)
                    print(
                        "One-shot выполнен, буфер очищен:",
                        discarded_after,
                        "байт",
                    )
                    recognizer = KaldiRecognizer(model, VOSK_RATE)
                    recognizer.SetWords(False)
                    last_partial = ""
                    wake_partial_hits = 0
                    pre_roll_pcm.clear()
                    print("Снова слушаю слово 'Бро'...")
                    continue

            print("One-shot команда не найдена, включаю обычный режим")

        # While BroHome says "слушаю", consume incoming ESP audio continuously.
        # Recording starts immediately after playback ends.
        discarded_prompt = speak_while_discarding("слушаю", receiver)
        print(f"Во время подсказки отброшено: {discarded_prompt} байт")

        print(f"Пишу команду {COMMAND_SECONDS} секунды...")
        command_bytes = round(info.rate * COMMAND_SECONDS) * 2
        command_raw = recv_exact(receiver, command_bytes)
        command_pcm = adc_raw_to_pcm(command_raw)
        save_command_wav(command_pcm, info.rate)

        recognized = google_recognize()
        execute_command(recognized, info.device)

        # Audio accumulated while Google, command.py and voice responses were
        # running is no longer useful. Clear it only now.
        discarded_after = discard_buffered_audio(receiver)
        print(f"Буфер после команды очищен: {discarded_after} байт")

        recognizer = KaldiRecognizer(model, VOSK_RATE)
        recognizer.SetWords(False)
        last_partial = ""
        wake_partial_hits = 0
        pre_roll_pcm.clear()
        print("Снова слушаю слово 'Бро'...")


def main() -> int:
    print("Загружаю Vosk...")
    model = Model(VOSK_MODEL)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((HOST, PORT))
        server.listen(5)

        print(f"BroHome Wi-Fi microphone server: {HOST}:{PORT}")

        while True:
            conn, address = server.accept()

            with conn:
                try:
                    process_client(conn, address, model)
                except socket.timeout:
                    print(
                        "ESP32 перестала передавать звук: "
                        f"нет данных более {ESP_READ_TIMEOUT:g} секунд"
                    )
                except (
                    ConnectionError,
                    OSError,
                    ValueError,
                ) as exc:
                    print("ESP32 отключена/ошибка:", exc)

            print("Ожидаю повторное подключение ESP32...")


if __name__ == "__main__":
    raise SystemExit(main())
