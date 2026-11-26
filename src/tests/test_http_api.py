"""Verhalten, das Schemathesis aus openapi.yaml allein nicht prüfen kann."""

import time
from datetime import datetime

import pytest
import requests


def get(base_url, path):
    return requests.get(base_url + path, timeout=5).json()


def post_interval(base_url, body):
    return requests.post(f"{base_url}/config", json=body, timeout=5)


def test_post_config_changes_interval(base_url):
    response = post_interval(base_url, {"reading_interval_seconds": 7})
    assert response.status_code == 200
    assert get(base_url, "/config")["reading_interval_seconds"] == 7


def test_post_config_does_not_change_device_id(base_url):
    before = get(base_url, "/config")["device_id"]
    post_interval(base_url, {"reading_interval_seconds": 30, "device_id": "gehackt"})
    assert get(base_url, "/config")["device_id"] == before


def test_last_measurement_is_plausible(base_url):
    status = get(base_url, "/status")
    if status["last_telemetry"] is None:
        pytest.skip("noch keine Messung gesendet")
    datetime.strptime(status["last_telemetry"]["timestamp"], "%Y-%m-%dT%H:%M:%SZ")  # UTC, ISO 8601
    assert status["last_telemetry"]["unit"] == "°C"
    assert 0 <= status["last_telemetry"]["value"] <= 50  # Messbereich DHT11
    assert 0 <= status["last_humidity_percent"] <= 100


def test_new_interval_produces_new_measurement(base_url):
    status = get(base_url, "/status")
    if not status["mqtt_connected"]:
        pytest.skip("keine MQTT-Verbindung, last_telemetry wird erst nach dem Senden aktualisiert")
    before = status["last_telemetry"]
    post_interval(base_url, {"reading_interval_seconds": 3})
    for _ in range(15):
        time.sleep(1)
        if get(base_url, "/status")["last_telemetry"] != before:
            return
    pytest.fail("innerhalb von 15 s keine neue Messung")
