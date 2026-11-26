"""Schemathesis erzeugt aus docs/iot-specs/openapi.yaml gültige und ungültige Anfragen und prüft
Statuscode, Content-Type und Antwortschema jeder Antwort gegen die Spezifikation."""

from hypothesis import settings

import schemathesis

schema = schemathesis.pytest.from_fixture("api_schema")


@schema.parametrize()
@settings(max_examples=25, deadline=None)  # wenige Fälle, das ESP32 ist langsam
def test_api_matches_openapi(case):
    case.call_and_validate()
