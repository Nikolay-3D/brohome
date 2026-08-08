#!/usr/bin/env python3
"""Current outdoor temperature in Moscow via the Open-Meteo API."""

import json
import urllib.error
import urllib.parse
import urllib.request

API_URL = "https://api.open-meteo.com/v1/forecast"
MOSCOW_LATITUDE = 55.7558
MOSCOW_LONGITUDE = 37.6176


def degree_word(value):
    absolute = abs(value)
    last_two = absolute % 100
    last = absolute % 10

    if 11 <= last_two <= 14:
        return "градусов"
    if last == 1:
        return "градус"
    if 2 <= last <= 4:
        return "градуса"
    return "градусов"


def temperature_text(value):
    rounded = int(round(float(value)))
    if rounded < 0:
        number = "минус {}".format(abs(rounded))
    else:
        number = str(rounded)
    return "В Москве сейчас {} {}".format(number, degree_word(rounded))


def current_temperature():
    query = urllib.parse.urlencode(
        {
            "latitude": MOSCOW_LATITUDE,
            "longitude": MOSCOW_LONGITUDE,
            "current": "temperature_2m",
            "timezone": "Europe/Moscow",
        }
    )
    request = urllib.request.Request(
        API_URL + "?" + query,
        headers={"User-Agent": "BroHome/1.0"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        payload = json.load(response)
    return payload["current"]["temperature_2m"]


def main():
    try:
        response = temperature_text(current_temperature())
    except (
        KeyError,
        TypeError,
        ValueError,
        TimeoutError,
        urllib.error.URLError,
    ) as exc:
        print("Weather error:", exc, flush=True)
        print(
            "BROHOME_RESPONSE=Не удалось узнать температуру",
            flush=True,
        )
        return 1

    print("BROHOME_RESPONSE=" + response, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
