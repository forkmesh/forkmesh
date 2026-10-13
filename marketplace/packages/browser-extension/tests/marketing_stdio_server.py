import asyncio
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "marketing" / "tests"))
from test_marketing import Runtime, marketing  # noqa: E402


def main():
    runtime = Runtime()
    for line in sys.stdin:
        message = json.loads(line)
        if message.get("control") == "threads":
            rows = runtime.db.execute(
                "SELECT status,data FROM marketing_reddit_threads ORDER BY created_at").fetchall()
            reply = {"status": 200, "data": {"ok": True, "rows": [
                {"status": row["status"], **json.loads(row["data"].removeprefix("sealed:"))}
                for row in rows]}}
        elif message.get("control") == "outbound":
            runtime.outbound[message["url"]] = (message.get("status", 200), json.dumps(message["data"]))
            reply = {"status": 200, "data": {"ok": True}}
        else:
            runtime.use(message["method"], message["actor"], message.get("body"))
            reply = asyncio.run(marketing.handle(runtime, message["path"]))
        sys.stdout.write(json.dumps({"status": reply["status"], "data": reply["data"]}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
