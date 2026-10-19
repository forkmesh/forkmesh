import asyncio
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "crm" / "tests"))
from test_crm import MastodonRuntime, crm  # noqa: E402


class ZohoConsoleRuntime(MastodonRuntime):
    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.endswith("/oauth/v2/token") and "grant_type=authorization_code" in body:
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            if not url.startswith("https://accounts.zoho.eu/"):
                return {"ok": True, "status": 400, "data": {"error": "invalid_client"}}
            if "code=1000.good" not in body:
                return {"ok": True, "status": 400, "data": {"error": "invalid_code"}}
            return {"ok": True, "status": 200, "data": {
                "access_token": "self-access", "refresh_token": "self-refresh"}}
        if url == "https://mail.zoho.eu/api/accounts":
            return {"ok": True, "status": 200, "data": {"data": [{"accountId": "4242"}]}}
        return await super().outbound_json(url, method, headers, body)


def main():
    runtime = ZohoConsoleRuntime()
    for line in sys.stdin:
        message = json.loads(line)
        if message.get("control") == "pings":
            reply = {"status": 200, "data": {"ok": True, "pings": runtime.pings}}
        else:
            runtime.use(message["method"], message["actor"], message.get("body"))
            reply = asyncio.run(crm.handle(runtime, message["path"]))
        sys.stdout.write(json.dumps({"status": reply["status"], "data": reply["data"]}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
