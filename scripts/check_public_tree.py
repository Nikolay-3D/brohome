#!/usr/bin/env python3
"""Fail if files intended for Git contain common private data."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
TEXT_SUFFIXES = {
    ".c", ".cpp", ".h", ".ino", ".json", ".md", ".ps1", ".py", ".sh",
    ".txt", ".yml", ".yaml",
}
FORBIDDEN_NAMES = {"secrets.h", ".env", "voice_full.wav", "hub_automations.json"}

PATTERNS = [
    ("private key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("GitHub token", re.compile(r"\b(?:ghp|github_pat)_[A-Za-z0-9_]{20,}\b")),
    ("generic API token", re.compile(r"(?i)\b(?:api[_-]?key|access[_-]?token|refresh[_-]?token)\b\s*[:=]\s*[\"'][^\"']{12,}[\"']")),
]


def git_paths() -> list[pathlib.Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [ROOT / item.decode("utf-8") for item in result.stdout.split(b"\0") if item]


def main() -> int:
    errors: list[str] = []
    for path in git_paths():
        # `git ls-files --cached` also reports tracked files deleted in the
        # working tree before the next commit.
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT).as_posix()
        if path.name.lower() in FORBIDDEN_NAMES:
            errors.append(f"private filename is publishable: {relative}")
            continue
        if path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue

        for label, pattern in PATTERNS:
            if pattern.search(text):
                errors.append(f"{label}: {relative}")

        for macro, placeholders in {
            "BROHOME_WIFI_SSID": {"YOUR_WIFI_NAME"},
            "BROHOME_WIFI_PASSWORD": {"YOUR_WIFI_PASSWORD"},
            "BROHOME_AP_PASSWORD": {"CHANGE_THIS_PASSWORD"},
        }.items():
            match = re.search(rf"#define\s+{macro}\s+\"([^\"]+)\"", text)
            if (match and not match.group(1).startswith("$(ConvertTo-CString ")
                    and match.group(1) not in placeholders):
                errors.append(f"real {macro}: {relative}")

    if errors:
        print("Public-tree check FAILED:", file=sys.stderr)
        for error in sorted(set(errors)):
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("Public-tree check: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
