"""Ziel der Tests: DEVICE_URL gesetzt = echtes Gerät, sonst wird der Mock gestartet."""

import os
from pathlib import Path

import pytest
import requests
import schemathesis

import mock_device

SPEC = Path(__file__).parents[1] / "docs" / "iot-specs" / "openapi.yaml"


@pytest.fixture(scope="session")
def base_url():
    url = os.environ.get("DEVICE_URL", "").rstrip("/")
    if url:
        yield url
    else:
        server = mock_device.start()
        yield f"http://127.0.0.1:{server.server_port}"
        server.shutdown()


@pytest.fixture(scope="session", autouse=True)
def restore_interval(base_url):
    """Die Tests ändern das Intervall, am Ende wird der Ausgangswert wiederhergestellt."""
    original = requests.get(f"{base_url}/config", timeout=5).json()["reading_interval_seconds"]
    yield
    requests.post(f"{base_url}/config", json={"reading_interval_seconds": original}, timeout=5)


@pytest.fixture(scope="session")
def api_schema(base_url):
    schema = schemathesis.openapi.from_path(SPEC)
    # Der WebServer des ESP32 bedient nur eine Verbindung gleichzeitig
    schema.config.update(base_url=base_url, headers={"Connection": "close"}, request_timeout=5)
    return schema
