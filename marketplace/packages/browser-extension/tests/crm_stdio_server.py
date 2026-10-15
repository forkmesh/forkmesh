import asyncio
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "crm" / "tests"))
from test_crm import MastodonRuntime, crm  # noqa: E402


def main():
    runtime = MastodonRuntime()
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
