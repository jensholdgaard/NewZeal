#!/usr/bin/env python3
"""Validate OTLP/JSON payloads against the OFFICIAL OpenTelemetry protobuf schema.

The hand-rolled exporter builds OTLP JSON by hand, so the spec is the contract. This checks our
payload shapes against opentelemetry-proto itself — the same definitions the collector uses — which
catches the whole class of "valid JSON, invalid OTLP" bugs (e.g. a single attribute serialized as
an object instead of a one-element array) *before* they ship, offline and in milliseconds.

    pip install opentelemetry-proto
    python ci/otlp_validate.py ci/fixtures/*.json        # or any captured payload

File names decide the signal: *metrics*.json, *logs*.json, *traces*.json.
"""
import glob
import sys

from google.protobuf import json_format
from opentelemetry.proto.collector.logs.v1 import logs_service_pb2
from opentelemetry.proto.collector.metrics.v1 import metrics_service_pb2
from opentelemetry.proto.collector.trace.v1 import trace_service_pb2

SIGNALS = {
    "metrics": metrics_service_pb2.ExportMetricsServiceRequest,
    "logs": logs_service_pb2.ExportLogsServiceRequest,
    "traces": trace_service_pb2.ExportTraceServiceRequest,
}


def signal_for(path: str):
    for name, msg in SIGNALS.items():
        if name in path:
            return name, msg
    return None, None


def main(paths):
    files = [f for p in paths for f in (glob.glob(p) or [p])]
    if not files:
        print("no payloads to validate", file=sys.stderr)
        return 1
    failures = 0
    for path in sorted(files):
        name, msg = signal_for(path)
        if not msg:
            print(f"SKIP  {path} (name must contain metrics/logs/traces)")
            continue
        try:
            json_format.Parse(open(path).read(), msg())
            print(f"OK    {path} ({name})")
        except Exception as exc:  # noqa: BLE001 - report any schema violation verbatim
            failures += 1
            print(f"FAIL  {path} ({name})\n      {str(exc).splitlines()[0]}")
    print(f"\n{len(files) - failures}/{len(files)} payloads valid per official OTLP schema")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["ci/fixtures/*.json"]))
