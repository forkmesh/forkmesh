#!/usr/bin/env python3
"""Automation and moderation: a signed webhook receiver that never double-acts.

Receives forkmesh-webhook-v1 deliveries (from ``forkmesh_integration.py
deliver``), verifies the HMAC signature and timestamp, and feeds an
EventLedger so duplicate, delayed, missing, and out-of-order deliveries still
produce exactly one action per state change. Repositories whose state carries
termsFlagged=true produce a moderation.flagged line.

    export FORKMESH_WEBHOOK_SECRET=...
    python3 webhook_moderation.py --port 8787 --state ledger.json

The first stdout line announces the bound URL; every later line is one action.
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sys
import threading

import _client

fm = _client.load()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--state", default="", help="ledger persistence file")
    parser.add_argument("--secret-env", default="FORKMESH_WEBHOOK_SECRET")
    parser.add_argument("--gap-timeout", type=float, default=fm.GAP_TIMEOUT_SECONDS)
    parser.add_argument("--stop-after", type=int, default=0,
                        help="exit after this many verified deliveries (for tests)")
    args = parser.parse_args(argv)

    secrets = [value for value in os.environ.get(args.secret_env, "").split(",") if value]
    if not secrets:
        print("set $%s (comma-separate old,new during rotation)" % args.secret_env,
              file=sys.stderr)
        return fm.EXIT_USAGE
    state_path = Path(args.state) if args.state else None
    options = {"gap_timeout": args.gap_timeout}
    if state_path and state_path.is_file():
        ledger = fm.EventLedger.from_dict(
            json.loads(state_path.read_text(encoding="utf-8")), **options)
    else:
        ledger = fm.EventLedger(**options)
    lock = threading.Lock()
    verified = {"count": 0}
    done = threading.Event()

    def act(actions):
        for action, payload in actions:
            if action == "apply":
                line = {"action": "apply", "type": payload["type"],
                        "subject": payload["subject"], "sequence": payload["sequence"]}
                fm.emit(dict(line, schema=fm.CLI_SCHEMA, command="example.webhook"))
                data = payload.get("data") or {}
                if data.get("termsFlagged"):
                    fm.emit({"schema": fm.CLI_SCHEMA, "command": "example.webhook",
                             "action": "moderation.flagged",
                             "subject": payload["subject"]})
            elif action == "resync":
                fm.emit(dict(payload, schema=fm.CLI_SCHEMA,
                             command="example.webhook", action="resync"))
            else:
                fm.emit({"schema": fm.CLI_SCHEMA, "command": "example.webhook",
                         "action": action, "id": payload["id"]})

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            return

        def do_POST(self):  # noqa: N802 - http.server API
            body = self.rfile.read(int(self.headers.get("content-length") or 0))
            try:
                event = fm.verify_webhook(secrets, self.headers.items(), body)
            except fm.WebhookVerificationError as exc:
                fm.emit({"schema": fm.CLI_SCHEMA, "command": "example.webhook",
                         "action": "rejected", "reason": str(exc)})
                return self._answer(401, {"error": str(exc)})
            with lock:
                act(ledger.accept(event))
                act(ledger.expire())
                if state_path:
                    temporary = state_path.with_name(state_path.name + ".tmp")
                    temporary.write_text(json.dumps(ledger.to_dict()), encoding="utf-8")
                    os.replace(temporary, state_path)
                verified["count"] += 1
                finished = bool(args.stop_after) and verified["count"] >= args.stop_after
            # Duplicates and stale events are acknowledged too, so the sender
            # stops retrying them.
            self._answer(200, {"ok": True})
            if finished:
                done.set()

        def _answer(self, status, payload):
            data = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    host, port = server.server_address[:2]
    fm.emit({"schema": fm.CLI_SCHEMA, "command": "example.webhook",
             "url": "http://%s:%d/forkmesh" % (host, port)})
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        while not done.wait(1.0):
            with lock:
                act(ledger.expire())
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()
    with lock:
        fm.emit({"schema": fm.CLI_SCHEMA, "command": "example.webhook",
                 "action": "final", "state": ledger.snapshot()})
    return fm.EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
