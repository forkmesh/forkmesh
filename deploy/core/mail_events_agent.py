#!/usr/bin/env python3
import collections
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

SPOOL = os.environ.get("FMNEXT_MAIL_SPOOL", "/var/lib/fmnext-mail-spool")
CURSOR = os.environ.get("FMNEXT_MAIL_CURSOR", "/var/lib/fmnext-mail-spool/.cursor")
KEEP_SECONDS = 3 * 24 * 3600
IDENTIFIERS = ("postfix/smtp", "postfix/cleanup", "postfix/error",
               "postfix/bounce", "postfix/qmgr")
MESSAGE_ID = re.compile(r"^([0-9A-Za-z]+): message-id=<([^>]{1,200})>")
DELIVERY = re.compile(r"^([0-9A-Za-z]+): to=<([^>]{0,254})>,.*?dsn=(\d\.\d{1,3}\.\d{1,3}), "
                      r"status=(sent|deferred|bounced) \((.{0,600})\)$")
SENDER = re.compile(r"^([0-9A-Za-z]+): from=<([^>]{0,254})>, size=")
EXPIRED = re.compile(r"^([0-9A-Za-z]+): from=<[^>]*>, status=expired")
EVENTS = {"sent": "delivery", "deferred": "soft bounce", "bounced": "bounce"}
MAX_SPOOL = 5000


def event_for(line, ids, deferred, node, senders=None):
    senders = {} if senders is None else senders
    match = MESSAGE_ID.match(line)
    if match:
        ids[match.group(1)] = match.group(2)
        while len(ids) > 20000:
            ids.popitem(last=False)
        return None
    match = SENDER.match(line)
    if match:
        senders[match.group(1)] = match.group(2) or "<>"
        while len(senders) > 20000:
            senders.popitem(last=False)
        return None
    match = DELIVERY.match(line)
    if not match:
        return None
    queue_id, email, dsn, status, reason = match.groups()
    if status == "deferred":
        key = queue_id + email
        if key in deferred:
            return None
        deferred[key] = True
        while len(deferred) > 20000:
            deferred.popitem(last=False)
    code = re.search(r"\b([245]\d\d)\b", reason)
    digest = hashlib.sha256(("%s|%s|%s|%s|%s" % (
        node, queue_id, email, status, dsn)).encode()).hexdigest()[:40]
    return {"event_id": "smtp:" + digest, "event": EVENTS[status],
            "message_id": ids.get(queue_id, ""), "email": email,
            "reason": "" if status == "sent" else reason[:300],
            "response_code": int(code.group(1)) if code else 0,
            "dsn": dsn, "node": node, "sender": senders.get(queue_id, "")}


def prune():
    cutoff = time.time_ns() - KEEP_SECONDS * 10**9
    for name in os.listdir(SPOOL):
        try:
            if name.endswith(".json") and int(name.split("-", 1)[0]) < cutoff:
                os.unlink(os.path.join(SPOOL, name))
        except (ValueError, OSError):
            continue


def write(event):
    try:
        if len(os.listdir(SPOOL)) >= MAX_SPOOL:
            prune()
            if len(os.listdir(SPOOL)) >= MAX_SPOOL:
                return
    except OSError:
        return
    name = "%020d-%s.json" % (time.time_ns(), event["event_id"].split(":")[-1])
    path = os.path.join(SPOOL, name)
    tmp = os.path.join(SPOOL, "." + name + ".tmp")
    with open(tmp, "w") as handle:
        json.dump(event, handle)
    os.chmod(tmp, 0o640)
    os.replace(tmp, path)


def main():
    node = socket.gethostname().split(".")[0]
    args = ["journalctl", "-f", "-o", "json", "--no-tail",
            "--cursor-file=" + CURSOR]
    for identifier in IDENTIFIERS:
        args += ["-t", identifier]
    ids = collections.OrderedDict()
    deferred = collections.OrderedDict()
    senders = collections.OrderedDict()
    pruned = 0.0
    with subprocess.Popen(args, stdout=subprocess.PIPE, text=True) as proc:
        for raw in proc.stdout:
            try:
                line = str(json.loads(raw).get("MESSAGE") or "")
            except ValueError:
                continue
            event = event_for(line, ids, deferred, node, senders)
            if event:
                write(event)
            if time.time() - pruned > 3600:
                prune()
                pruned = time.time()
    return proc.returncode or 1


if __name__ == "__main__":
    sys.exit(main())
