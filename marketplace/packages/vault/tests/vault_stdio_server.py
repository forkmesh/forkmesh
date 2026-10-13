"""Serve the vault Worker module over stdin/stdout, one JSON object a line.

The JavaScript end-to-end test starts this so the real client and the real
crypto talk to the real module. Request: {"actor","method","path","body"} or
a control message {"control": "join"|"leave"|"delete_account", ...}.
"""

import asyncio
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vault_runtime import Runtime, bi, vault  # noqa: E402


def main():
    runtime = Runtime()
    for line in sys.stdin:
        message = json.loads(line)
        control = message.get("control")
        if control == "join":
            runtime.join(message["organization"], message["member"])
            reply = {"status": 200, "data": {"ok": True}}
        elif control == "leave":
            runtime.leave(message["organization"], message["member"])
            reply = {"status": 200, "data": {"ok": True}}
        elif control == "delete_account":
            asyncio.run(vault.purge_account_data(runtime, bi(message["member"]), message["member"]))
            runtime.accounts.discard(message["member"])
            reply = {"status": 200, "data": {"ok": True}}
        elif control == "set_bundle":
            # What a hostile server would do: publish other keys under a name.
            row = runtime.db.execute(
                "SELECT data FROM vault_accounts WHERE account_bi=?",
                (bi(message["member"]),)).fetchone()
            data = json.loads(row["data"].removeprefix("sealed:"))
            data["bundle"] = message["bundle"]
            runtime.db.execute(
                "UPDATE vault_accounts SET data=? WHERE account_bi=?",
                ("sealed:" + json.dumps(data, sort_keys=True), bi(message["member"])))
            runtime.db.commit()
            reply = {"status": 200, "data": {"ok": True}}
        elif control == "pings":
            reply = {"status": 200, "data": {"ok": True, "pings": runtime.pings}}
        elif control == "dump":
            rows = []
            for table in ("vault_accounts", "vault_items", "vault_shares", "vault_grants"):
                for row in runtime.db.execute("SELECT * FROM " + table).fetchall():
                    rows.append({"table": table, **dict(row)})
            reply = {"status": 200, "data": {"ok": True, "rows": rows}}
        else:
            runtime.use(message["method"], message["actor"], message.get("body"))
            reply = asyncio.run(vault.handle(runtime, message["path"]))
        sys.stdout.write(json.dumps({"status": reply["status"], "data": reply["data"]}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
