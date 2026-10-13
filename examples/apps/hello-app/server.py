#!/usr/bin/env python3
import base64
import hashlib
import hmac
import json
import os
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("HELLO_PORT", "8787"))
PUBLIC_URL = os.environ.get("HELLO_PUBLIC_URL", "http://127.0.0.1:%d" % PORT)
FORKMESH = os.environ.get("FORKMESH_ORIGIN", "https://app.forkmesh.com")
CLIENT_ID = os.environ["FORKMESH_CLIENT_ID"]
CLIENT_SECRET = os.environ["FORKMESH_CLIENT_SECRET"]
SIGNING_SECRET = os.environ["FORKMESH_SIGNING_SECRET"]
STATE_FILE = os.environ.get("HELLO_STATE_FILE", "")
SCOPES = os.environ.get(
    "HELLO_SCOPES",
    "repos:read chat:write pings:read pings:write commands")
SCHEME = "forkmesh-webhook-v1"
TOLERANCE = 300
MAX_BODY = 256 * 1024

LOCK = threading.Lock()
PENDING = {}
STATE = {"installs": {}, "events": [], "commands": [], "actions": [],
         "seen": []}


def save():
    if not STATE_FILE:
        return
    public = {"installs": {
        key: {k: v for k, v in value.items() if not k.endswith("token")}
        for key, value in STATE["installs"].items()}}
    with open(STATE_FILE, "w", encoding="utf-8") as handle:
        json.dump(public, handle)


def verify(headers, body):
    request_id = headers.get("ForkMesh-Webhook-Id", "")
    timestamp = headers.get("ForkMesh-Webhook-Timestamp", "")
    signature = headers.get("ForkMesh-Webhook-Signature", "")
    try:
        stamp = int(timestamp)
    except ValueError:
        return False
    if abs(time.time() - stamp) > TOLERANCE:
        return False
    message = ("%s\n%d\n%s\n" % (SCHEME, stamp, request_id)).encode() + body
    expected = "v1=" + hmac.new(SIGNING_SECRET.encode(), message,
                                hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, signature):
        return False
    with LOCK:
        if request_id in STATE["seen"]:
            return False
        STATE["seen"] = (STATE["seen"] + [request_id])[-500:]
    return True


def forkmesh(path, form=None, token=None, payload=None):
    headers = {"accept": "application/json"}
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["content-type"] = "application/x-www-form-urlencoded"
        basic = base64.b64encode(
            ("%s:%s" % (CLIENT_ID, CLIENT_SECRET)).encode()).decode()
        headers["authorization"] = "Basic " + basic
    else:
        data = json.dumps(payload or {}).encode()
        headers["content-type"] = "application/json"
        headers["authorization"] = "Bearer " + token
    request = urllib.request.Request(FORKMESH + path, data=data,
                                     headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"{}")


def remember_tokens(body):
    install = body["install"]
    record = {
        "installId": install["id"],
        "target": install["target"],
        "name": install["name"],
        "scopes": body["scope"].split(),
        "bot": body.get("bot"),
        "access_token": body["access_token"],
        "refresh_token": body["refresh_token"],
        "expires": time.time() + int(body["expires_in"]) - 60,
    }
    with LOCK:
        STATE["installs"][install["id"]] = record
    save()
    return record


def token_for(install_id):
    with LOCK:
        record = STATE["installs"].get(install_id)
    if record is None:
        return None
    if time.time() < record["expires"]:
        return record["access_token"]
    status, body = forkmesh("/oauth/token", form={
        "grant_type": "refresh_token",
        "refresh_token": record["refresh_token"]})
    if status != 200:
        return None
    return remember_tokens(body)["access_token"]


def web_api(install_id, method, payload):
    token = token_for(install_id)
    if token is None:
        return 401, {"error": "not_installed"}
    return forkmesh("/api/apps/v1/" + method, token=token, payload=payload)


def greeting_blocks(user):
    return [
        {"type": "section", "text": "Hello, @%s!" % user},
        {"type": "actions", "elements": [
            {"type": "button", "action_id": "wave", "text": "Wave back",
             "value": "wave", "style": "primary"},
            {"type": "static_select", "action_id": "mood",
             "placeholder": "How are you?",
             "options": [{"text": "Great", "value": "great"},
                         {"text": "Fine", "value": "fine"}]},
        ]},
    ]


class Handler(BaseHTTPRequestHandler):
    server_version = "hello-app/1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, payload=None, location=None, text=None):
        body = b""
        if payload is not None:
            body = json.dumps(payload).encode()
        elif text is not None:
            body = text.encode()
        self.send_response(status)
        if location:
            self.send_header("location", location)
        if payload is not None:
            self.send_header("content-type", "application/json")
        elif text is not None:
            self.send_header("content-type", "text/plain; charset=utf-8")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        if url.path == "/health":
            return self.reply(200, {"ok": True})
        if url.path == "/install":
            verifier = secrets.token_urlsafe(48)
            state = secrets.token_urlsafe(16)
            with LOCK:
                PENDING[state] = verifier
            challenge = base64.urlsafe_b64encode(
                hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
            location = FORKMESH + "/oauth/authorize?" + urllib.parse.urlencode({
                "response_type": "code", "client_id": CLIENT_ID,
                "redirect_uri": PUBLIC_URL + "/oauth/callback",
                "scope": SCOPES, "state": state,
                "code_challenge": challenge, "code_challenge_method": "S256"})
            return self.reply(302, location=location)
        if url.path == "/oauth/callback":
            with LOCK:
                verifier = PENDING.pop(query.get("state", ""), None)
            if verifier is None or "code" not in query:
                return self.reply(400, {"error": query.get("error", "state")})
            status, body = forkmesh("/oauth/token", form={
                "grant_type": "authorization_code", "code": query["code"],
                "redirect_uri": PUBLIC_URL + "/oauth/callback",
                "code_verifier": verifier})
            if status != 200:
                return self.reply(502, {"error": body.get("error")})
            record = remember_tokens(body)
            web_api(record["installId"], "pings.send", {
                "title": "Hello App is installed",
                "body": "Type /hello in a chat channel."})
            return self.reply(200, text="Hello App is installed. "
                                        "You can close this tab.")
        if url.path == "/state":
            with LOCK:
                installs = list(STATE["installs"].values())
                latest = installs[-1] if installs else {}
                return self.reply(200, {
                    "installed": bool(installs),
                    "installIds": [i["installId"] for i in installs],
                    "scopes": latest.get("scopes", []),
                    "bot": latest.get("bot"),
                    "events": STATE["events"],
                    "commands": STATE["commands"],
                    "actions": STATE["actions"],
                })
        if url.path == "/post":
            with LOCK:
                installs = list(STATE["installs"])
            if not installs:
                return self.reply(409, {"error": "not_installed"})
            status, body = web_api(installs[-1], "chat.postMessage", {
                "channel": query.get("channel", ""),
                "text": query.get("text", "Hello from Hello App"),
                "blocks": greeting_blocks("everyone")})
            return self.reply(status, body)
        return self.reply(404, {"error": "not_found"})

    def do_POST(self):
        length = int(self.headers.get("content-length") or 0)
        if length > MAX_BODY:
            return self.reply(413, {"error": "too_large"})
        body = self.rfile.read(length)
        if not verify(self.headers, body):
            return self.reply(401, {"error": "bad_signature"})
        payload = json.loads(body or b"{}")
        if self.path == "/events":
            if payload.get("type") == "url_verification":
                return self.reply(200, {"challenge": payload.get("challenge")})
            with LOCK:
                STATE["events"].append(payload.get("event", {}))
            return self.reply(200, {"ok": True})
        if self.path == "/commands":
            user = payload.get("user", {}).get("name", "friend")
            with LOCK:
                STATE["commands"].append({"command": payload.get("command"),
                                          "text": payload.get("text"),
                                          "user": user})
            if payload.get("text") == "announce":
                return self.reply(200, {
                    "responseType": "in_channel",
                    "text": "Hello everyone, from @%s via Hello App." % user})
            return self.reply(200, {
                "responseType": "ephemeral",
                "text": "Hello, @%s!" % user,
                "blocks": greeting_blocks(user)})
        if self.path == "/interactive":
            action = payload.get("action", {})
            user = payload.get("user", {}).get("name", "friend")
            with LOCK:
                STATE["actions"].append({"actionId": action.get("actionId"),
                                         "value": action.get("value"),
                                         "user": user})
            if action.get("actionId") == "wave":
                return self.reply(200, {"responseType": "in_channel",
                                        "text": "@%s waved back." % user})
            return self.reply(200, {"responseType": "ephemeral",
                                    "text": "Noted: %s." % action.get("value")})
        return self.reply(404, {"error": "not_found"})


def main():
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("hello-app listening on %s" % PUBLIC_URL, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
