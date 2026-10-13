from __future__ import annotations

import base64
import json
import re
import urllib.error
import urllib.request

API = "https://api.vultr.com/v2"
TAG = "forkmesh-next"
LABEL_RE = re.compile(r"^fmnext-(auto|burst)-([1-9])$")
FORBIDDEN_RE = re.compile(r"^fmnext-(core|run|runner|snap)-")
ID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
PLAN_RE = re.compile(r"^[a-z0-9-]{3,40}$")
REGION_RE = re.compile(r"^[a-z]{3,4}$")
CREATOR = "autoscaler"
MAX_USER_DATA = 16384


class GuardRefused(RuntimeError):
    pass


def name_for_label(label):
    match = LABEL_RE.match(str(label or ""))
    if not match:
        return None
    return ("a" if match.group(1) == "auto" else "b") + match.group(2)


def label_for_name(name):
    match = re.fullmatch(r"([ab])([1-9])", str(name or ""))
    if not match:
        raise GuardRefused("refusing node name %r" % name)
    return "fmnext-%s-%s" % ("auto" if match.group(1) == "a" else "burst",
                             match.group(2))


def allowed_label(label):
    label = str(label or "")
    return bool(LABEL_RE.match(label)) and not FORBIDDEN_RE.match(label)


def check_record(record):
    record = record or {}
    label = str(record.get("label") or "")
    if not allowed_label(label):
        raise GuardRefused("label %r is not an autoscaled label" % label)
    if label_for_name(record.get("name")) != label:
        raise GuardRefused("record %r does not match its label %r"
                           % (record.get("name"), label))
    if record.get("created_by") != CREATOR:
        raise GuardRefused("%s was not created by the autoscaler" % label)
    return record


def check_live(instance, record):
    instance = instance or {}
    label = str(instance.get("label") or "")
    tags = [str(t) for t in (instance.get("tags") or [])]
    if instance.get("id") != record.get("instance_id"):
        raise GuardRefused("Vultr answered for id %r, not the recorded %r"
                           % (instance.get("id"), record.get("instance_id")))
    if label != record.get("label") or not allowed_label(label):
        raise GuardRefused("instance %s is labelled %r, the record says %r"
                           % (record.get("instance_id"), label,
                              record.get("label")))
    if TAG not in tags:
        raise GuardRefused("instance %s is not tagged %s" % (
            record.get("instance_id"), TAG))
    return instance


class VultrGuard:
    def __init__(self, api_key, opener=None, base=API, timeout=30):
        if not api_key:
            raise GuardRefused("no Vultr API key in the vault")
        self._key = api_key
        self.opener = opener or urllib.request.urlopen
        self.base = base
        self.timeout = timeout

    def _request(self, method, path, body=None, expect=(200, 201, 202, 204)):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data,
                                         method=method)
        request.add_header("Authorization", "Bearer " + self._key)
        request.add_header("Content-Type", "application/json")
        request.add_header("User-Agent", "fmnext-autoscaler/1")
        try:
            with self.opener(request, timeout=self.timeout) as response:
                status = getattr(response, "status", 200)
                raw = response.read(1 << 20)
        except urllib.error.HTTPError as error:
            status = error.code
            raw = error.read(4096) if hasattr(error, "read") else b""
        if status not in expect:
            text = raw.decode("utf-8", "replace")[:200].replace(self._key, "***")
            raise RuntimeError("vultr %s %s: HTTP %s %s" % (
                method, path.split("?")[0], status, text))
        return json.loads(raw) if raw else {}

    def create(self, record, plan, region, snapshot_id, user_data):
        check_record(record)
        if not PLAN_RE.match(str(plan)) or not REGION_RE.match(str(region)):
            raise GuardRefused("bad plan or region")
        if not ID_RE.match(str(snapshot_id or "")):
            raise GuardRefused("autoscaled nodes boot only from a snapshot")
        if record.get("instance_id"):
            raise GuardRefused("%s already has an instance" % record["label"])
        encoded = base64.b64encode(user_data.encode()).decode()
        if len(encoded) > MAX_USER_DATA:
            raise GuardRefused("user_data too large")
        body = {"region": region, "plan": plan, "snapshot_id": snapshot_id,
                "label": record["label"], "hostname": record["label"],
                "tags": [TAG], "user_data": encoded, "backups": "disabled",
                "enable_ipv6": False, "activation_email": False}
        reply = self._request("POST", "/instances", body)
        instance = reply.get("instance") or {}
        if instance.get("label") != record["label"] or not ID_RE.match(
                str(instance.get("id") or "")):
            raise GuardRefused("Vultr returned an unexpected instance")
        return instance

    def get(self, record):
        check_record(record)
        instance_id = str(record.get("instance_id") or "")
        if not ID_RE.match(instance_id):
            raise GuardRefused("no recorded instance id for %s" % record["label"])
        try:
            reply = self._request("GET", "/instances/%s" % instance_id)
        except RuntimeError as error:
            if "HTTP 404" in str(error):
                return None
            raise
        return check_live(reply.get("instance") or {}, record)

    def destroy(self, record):
        live = self.get(record)
        if live is None:
            return False
        self._request("DELETE", "/instances/%s" % record["instance_id"],
                      expect=(200, 202, 204))
        return True

    def plan_hourly_cents(self, plan):
        if not PLAN_RE.match(str(plan)):
            raise GuardRefused("bad plan")
        reply = self._request("GET", "/plans?per_page=500&type=vc2")
        for item in reply.get("plans") or []:
            if item.get("id") == plan:
                return round(float(item.get("monthly_cost") or 0) * 100 / 730.0, 4)
        raise GuardRefused("Vultr has no plan %r" % plan)
