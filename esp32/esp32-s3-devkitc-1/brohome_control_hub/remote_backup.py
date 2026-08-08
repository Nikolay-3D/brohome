#!/usr/bin/env python3
"""Backup/restore BroHome control-hub commands and upload feedback WAV files."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit("Install dependency first: py -m pip install pyserial") from exc


BAUD = 115200


def open_port(name: str) -> serial.Serial:
    port = serial.Serial(name, BAUD, timeout=0.4, write_timeout=3)
    time.sleep(1.8)
    port.reset_input_buffer()
    return port


def write_line(port: serial.Serial, line: str) -> None:
    port.write((line + "\n").encode("ascii"))
    port.flush()


def wait_for(port: serial.Serial, prefixes: tuple[str, ...], timeout: float = 8.0) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        print(line)
        if line.startswith(prefixes):
            return line
    raise TimeoutError(f"ESP32 did not answer within {timeout:g} seconds")


def backup(port: serial.Serial, output: pathlib.Path) -> None:
    write_line(port, "export")
    wait_for(port, ("EXPORT_BEGIN",))
    commands: list[dict[str, object]] = []
    while True:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line == "EXPORT_END":
            break
        if not line.startswith("EXPORT "):
            print(line)
            continue
        parts = line.split(" ", 8)
        if len(parts) != 9:
            raise ValueError(f"Malformed export record: {line[:120]}")
        _, kind, name, carrier, start_level, repeats, frequency, count, pulse_text = parts
        pulses = [int(value) for value in pulse_text.split(",")]
        if len(pulses) != int(count):
            raise ValueError(f"Pulse count mismatch for {kind}/{name}")
        commands.append(
            {
                "type": kind,
                "name": name,
                "carrier_khz": int(carrier),
                "start_level": int(start_level),
                "repeats": int(repeats),
                "frequency_hz": int(frequency),
                "pulses_us": pulses,
            }
        )
    document = {"format": "brohome-remote-backup", "version": 1, "commands": commands}
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output)
    print(f"Saved {len(commands)} command(s) to {output}")


def restore(port: serial.Serial, source: pathlib.Path) -> None:
    document = json.loads(source.read_text(encoding="utf-8"))
    if document.get("format") != "brohome-remote-backup" or document.get("version") != 1:
        raise ValueError("Unsupported backup format")
    commands = document.get("commands", [])
    for item in commands:
        pulses = item["pulses_us"]
        pulse_text = ",".join(str(int(value)) for value in pulses)
        line = (
            f"import {item['type']} {item['name']} {int(item['carrier_khz'])} "
            f"{int(item['start_level'])} {int(item['repeats'])} "
            f"{int(item['frequency_hz'])} {len(pulses)} {pulse_text}"
        )
        write_line(port, line)
        answer = wait_for(port, ("OK imported", "ERR "), timeout=12)
        if not answer.startswith("OK"):
            raise RuntimeError(f"Restore failed for {item['type']}/{item['name']}")
    print(f"Restored {len(commands)} command(s) from {source}")


def upload_audio(port: serial.Serial, name: str, source: pathlib.Path) -> None:
    if not name or any(not (char.isascii() and (char.isalnum() or char in "_-")) for char in name):
        raise ValueError("Audio name may contain only ASCII letters, digits, '_' and '-'")
    payload = source.read_bytes()
    write_line(port, f"audio_begin {name} {len(payload)}")
    if not wait_for(port, ("OK audio ready", "ERR ")).startswith("OK"):
        raise RuntimeError("ESP32 rejected audio_begin")
    for offset in range(0, len(payload), 256):
        write_line(port, "audio_chunk " + payload[offset : offset + 256].hex())
        if not wait_for(port, ("OK audio chunk", "ERR ")).startswith("OK"):
            raise RuntimeError(f"Audio upload failed at byte {offset}")
    write_line(port, "audio_end")
    if not wait_for(port, ("OK audio saved", "ERR "), timeout=15).startswith("OK"):
        raise RuntimeError("Audio upload was not committed")
    print(f"Uploaded {source} as {name}.wav")


def run_command(port: serial.Serial, text: str) -> None:
    write_line(port, text)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line:
            print(line)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="ESP32 serial port, for example COM7")
    sub = parser.add_subparsers(dest="action", required=True)
    backup_parser = sub.add_parser("backup")
    backup_parser.add_argument("file", type=pathlib.Path)
    restore_parser = sub.add_parser("restore")
    restore_parser.add_argument("file", type=pathlib.Path)
    audio_parser = sub.add_parser("audio-put")
    audio_parser.add_argument("name")
    audio_parser.add_argument("file", type=pathlib.Path)
    command_parser = sub.add_parser("command")
    command_parser.add_argument("text", help='Firmware command, e.g. "list"')
    args = parser.parse_args()

    with open_port(args.port) as port:
        if args.action == "backup":
            backup(port, args.file)
        elif args.action == "restore":
            restore(port, args.file)
        elif args.action == "audio-put":
            upload_audio(port, args.name, args.file)
        elif args.action == "command":
            run_command(port, args.text)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
