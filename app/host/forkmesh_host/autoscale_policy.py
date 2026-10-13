from __future__ import annotations

import calendar
import copy
import json
import re
import time

from . import ids_records

SETTINGS_KIND = "autoscale-settings"
COMMAND_KIND = "autoscale-command"
STATUS_KIND = "autoscale-status"
POOLS = ("app", "runner")
HOURS_PER_MONTH = 730.0
MAX_NODES = 4
MIN_COOLDOWN_MINUTES = 5
MAX_CAP_CENTS = 10000
MAX_ACTIONS_PER_DAY = 12
BURST_JOBS_READY = False
APP_SIGNALS = ("p95Ms", "loopLagMs", "loadPerCpu", "rps")


class PolicyError(ValueError):
    pass


PLAN_RE = re.compile(r"^[a-z0-9-]{3,40}$")
REGION_RE = re.compile(r"^[a-z]{3,4}$")
SNAPSHOT_RE = re.compile(
    r"^([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})?$")


DEFAULTS = {
    "version": 1,
    "killSwitch": False,
    "app": {
        "enabled": False, "dryRun": True, "min": 0, "max": 2,
        "scaleUp": {"p95Ms": 500, "loopLagMs": 250, "loadPerCpu": 0.8,
                    "rps": 0, "forMinutes": 5},
        "scaleDown": {"fraction": 0.5, "forMinutes": 30},
        "cooldownMinutes": 10, "plan": "vc2-1c-1gb", "region": "ewr",
        "monthlyCapCents": 1500, "snapshotId": "",
    },
    "runner": {
        "enabled": False, "dryRun": True, "min": 0, "max": 2,
        "scaleUp": {"queueWaitMinutes": 2, "queueDepth": 1},
        "scaleDown": {"idleMinutes": 10},
        "cooldownMinutes": 10, "plan": "vc2-1c-1gb", "region": "ewr",
        "monthlyCapCents": 1000, "snapshotId": "",
    },
}
def _number(value, name, low, high, integer=True):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise PolicyError("%s must be a number" % name)
    if integer and int(value) != value:
        raise PolicyError("%s must be a whole number" % name)
    if not low <= value <= high:
        raise PolicyError("%s must be between %s and %s" % (name, low, high))
    return int(value) if integer else float(value)


def _flag(value, name):
    if not isinstance(value, bool):
        raise PolicyError("%s must be true or false" % name)
    return value


def validate(settings):
    if not isinstance(settings, dict):
        raise PolicyError("settings must be an object")
    out = copy.deepcopy(DEFAULTS)
    if settings.get("version", 1) != 1:
        raise PolicyError("unknown settings version")
    for key in settings:
        if key not in out:
            raise PolicyError("unknown setting %s" % key)
    out["killSwitch"] = _flag(settings.get("killSwitch", False), "killSwitch")
    for pool in POOLS:
        given = settings.get(pool) or {}
        if not isinstance(given, dict):
            raise PolicyError("%s must be an object" % pool)
        target = out[pool]
        for key in given:
            if key not in target:
                raise PolicyError("unknown setting %s.%s" % (pool, key))
        for key in ("scaleUp", "scaleDown"):
            sub = given.get(key, {})
            if not isinstance(sub, dict):
                raise PolicyError("%s.%s must be an object" % (pool, key))
            for inner in sub:
                if inner not in target[key]:
                    raise PolicyError("unknown setting %s.%s.%s" % (pool, key, inner))
            target[key].update(sub)
        for key, value in given.items():
            if key not in ("scaleUp", "scaleDown"):
                target[key] = value
        name = pool + "."
        target["enabled"] = _flag(target["enabled"], name + "enabled")
        target["dryRun"] = _flag(target["dryRun"], name + "dryRun")
        target["min"] = _number(target["min"], name + "min", 0, MAX_NODES)
        target["max"] = _number(target["max"], name + "max", 0, MAX_NODES)
        if target["min"] > target["max"]:
            raise PolicyError("%smin must not exceed %smax" % (name, name))
        target["cooldownMinutes"] = _number(
            target["cooldownMinutes"], name + "cooldownMinutes",
            MIN_COOLDOWN_MINUTES, 24 * 60)
        target["monthlyCapCents"] = _number(
            target["monthlyCapCents"], name + "monthlyCapCents", 0, MAX_CAP_CENTS)
        if not PLAN_RE.match(str(target["plan"])):
            raise PolicyError("%splan is not a Vultr plan id" % name)
        if not REGION_RE.match(str(target["region"])):
            raise PolicyError("%sregion is not a Vultr region id" % name)
        up, down = target["scaleUp"], target["scaleDown"]
        if pool == "app":
            up["p95Ms"] = _number(up["p95Ms"], name + "scaleUp.p95Ms", 0, 60000)
            up["loopLagMs"] = _number(up["loopLagMs"], name + "scaleUp.loopLagMs",
                                      0, 60000)
            up["loadPerCpu"] = _number(up["loadPerCpu"], name + "scaleUp.loadPerCpu",
                                       0, 16, integer=False)
            up["rps"] = _number(up["rps"], name + "scaleUp.rps", 0, 100000,
                                integer=False)
            up["forMinutes"] = _number(up["forMinutes"], name + "scaleUp.forMinutes",
                                       1, 24 * 60)
            down["fraction"] = _number(down["fraction"], name + "scaleDown.fraction",
                                       0.05, 0.95, integer=False)
            down["forMinutes"] = _number(down["forMinutes"],
                                         name + "scaleDown.forMinutes", 5, 7 * 24 * 60)
            if not any(up[k] for k in APP_SIGNALS):
                raise PolicyError("app.scaleUp needs at least one threshold")
            if not SNAPSHOT_RE.match(str(target["snapshotId"] or "")):
                raise PolicyError("app.snapshotId is not a Vultr snapshot id")
            target["snapshotId"] = str(target["snapshotId"] or "")
        else:
            up["queueWaitMinutes"] = _number(up["queueWaitMinutes"],
                                             name + "scaleUp.queueWaitMinutes", 0, 24 * 60)
            up["queueDepth"] = _number(up["queueDepth"], name + "scaleUp.queueDepth",
                                       1, 1000)
            down["idleMinutes"] = _number(down["idleMinutes"],
                                          name + "scaleDown.idleMinutes", 5, 24 * 60)
            if not SNAPSHOT_RE.match(str(target["snapshotId"] or "")):
                raise PolicyError("runner.snapshotId is not a Vultr snapshot id")
            target["snapshotId"] = str(target["snapshotId"] or "")
    return out




SIGN_DOMAIN = "fmnext-autoscale-v1"
KINDS = frozenset({"autoscale-settings", "autoscale-command", "autoscale-status"})


def canonical(kind, body):
    if kind not in KINDS:
        raise PolicyError("unknown document kind %r" % (kind,))
    return ("%s\n%s\n%s" % (SIGN_DOMAIN, kind, json.dumps(
        body, sort_keys=True, separators=(",", ":"), ensure_ascii=True))).encode()


def signed_document(kind, body, private_raw):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = Ed25519PrivateKey.from_private_bytes(bytes(private_raw))
    return dict(body, signature=ids_records.b64u(key.sign(canonical(kind, body))))


def verify_document(kind, document, public_key, fields):
    if not isinstance(document, dict):
        return False
    body = {field: document.get(field) for field in fields}
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        key = Ed25519PublicKey.from_public_bytes(ids_records.unb64u(public_key))
        key.verify(ids_records.unb64u(document.get("signature") or ""),
                   canonical(kind, body))
        return True
    except (InvalidSignature, ValueError, TypeError, PolicyError):
        return False


def settings_document(settings, private_raw, signed_at):
    return signed_document(SETTINGS_KIND, {"settings": settings,
                                           "signedAt": int(signed_at)},
                           private_raw)


def verify_settings(document, public_key):
    if not verify_document(SETTINGS_KIND, document, public_key,
                           ("settings", "signedAt")):
        raise PolicyError("the settings signature does not verify against "
                          "the operator key")
    return validate(document["settings"])


def empty_pool_state():
    return {"breachSince": 0, "calmSince": 0, "lastActionMs": 0,
            "actions": [], "month": "", "spentCents": 0.0, "lastAccrualMs": 0,
            "decision": {"action": "none", "reason": "", "atMs": 0},
            "signals": {}, "idleSince": {}}


def month_of(ms):
    return time.strftime("%Y-%m", time.gmtime(ms / 1000))


def accrue(pool_state, nodes, now):
    month = month_of(now)
    spent = float(pool_state.get("spentCents") or 0.0)
    last = int(pool_state.get("lastAccrualMs") or 0)
    if pool_state.get("month") != month:
        spent = 0.0
        start = calendar.timegm(time.strptime(month + "-01", "%Y-%m-%d")) * 1000
        last = max(last, start)
    for node in nodes:
        since = max(last, int(node.get("createdMs") or 0))
        if since and now > since:
            spent += (now - since) / 3_600_000 * float(node.get("hourlyCents") or 0)
    pool_state["month"] = month
    pool_state["spentCents"] = round(spent, 4)
    pool_state["lastAccrualMs"] = now
    return pool_state


def aggregate_app_signals(samples):
    present = [s for s in samples if s]

    def values(key):
        return [float(s[key]) for s in present if s.get(key) is not None]
    out = {}
    for key, how in (("p95Ms", max), ("loopLagMs", max)):
        found = values(key)
        out[key] = round(how(found), 1) if found else None
    for key in ("loadPerCpu", "rps"):
        found = values(key)
        out[key] = round(sum(found) / len(found), 3) if found else None
    out["nodesReporting"] = len(present)
    return out


def _cooldown_left(pool_state, settings, now):
    last = int(pool_state.get("lastActionMs") or 0)
    wait = settings["cooldownMinutes"] * 60_000
    return max(0, last + wait - now)


def _rate_limited(pool_state, now):
    recent = [t for t in pool_state.get("actions") or [] if now - t < 86_400_000]
    pool_state["actions"] = recent
    return len(recent) >= MAX_ACTIONS_PER_DAY


def _spend_ok(pool_state, settings, count, hourly):
    projected = float(pool_state.get("spentCents") or 0.0) + (count + 1) * hourly
    return projected <= settings["monthlyCapCents"]


def decide_app(settings, pool_state, signals, count, hourly, now):
    up_rules, down_rules = settings["scaleUp"], settings["scaleDown"]
    active = {k: up_rules[k] for k in APP_SIGNALS if up_rules.get(k)}
    measured = {k: signals.get(k) for k in active if signals.get(k) is not None}
    breach = [k for k, v in measured.items() if v > active[k]]
    calm = bool(measured) and all(
        v < active[k] * down_rules["fraction"] for k, v in measured.items())
    pool_state["breachSince"] = (pool_state.get("breachSince") or now) if breach else 0
    pool_state["calmSince"] = (pool_state.get("calmSince") or now) if calm else 0
    pool_state["signals"] = dict(signals)
    over_cap = float(pool_state.get("spentCents") or 0.0) >= settings["monthlyCapCents"]
    return _decide(settings, pool_state, count, hourly, now, over_cap,
                   wants_up=bool(breach) and now - pool_state["breachSince"]
                   >= up_rules["forMinutes"] * 60_000,
                   wants_down=calm and now - pool_state["calmSince"]
                   >= down_rules["forMinutes"] * 60_000,
                   up_reason="%s over threshold for %d min" % (
                       ", ".join("%s %s > %s" % (k, measured[k], active[k])
                                 for k in breach), up_rules["forMinutes"]),
                   down_reason="every signal under %d%% of its threshold for "
                   "%d min" % (down_rules["fraction"] * 100,
                               down_rules["forMinutes"]),
                   hold_reason=("no metrics from the nodes yet" if not measured
                                else "signals within thresholds"))


def decide_runner(settings, pool_state, observation, nodes, hourly, now):
    queued = int(observation.get("queued") or 0)
    wait_s = int(observation.get("oldestWaitS") or 0)
    idle = pool_state.setdefault("idleSince", {})
    busy = observation.get("busy") or {}
    idle_victim = None
    for node in sorted(nodes, key=lambda n: n["name"]):
        name = node["name"]
        if busy.get(name) or node.get("state") != "serving":
            idle.pop(name, None)
            continue
        idle[name] = idle.get(name) or now
        if now - idle[name] >= settings["scaleDown"]["idleMinutes"] * 60_000 \
                and idle_victim is None:
            idle_victim = name
    for name in list(idle):
        if name not in {n["name"] for n in nodes}:
            idle.pop(name, None)
    pool_state["signals"] = {"queued": queued, "oldestWaitS": wait_s}
    up_rules = settings["scaleUp"]
    wants_up = (queued >= up_rules["queueDepth"]
                and wait_s >= up_rules["queueWaitMinutes"] * 60
                and not observation.get("alwaysOnFree"))
    over_cap = float(pool_state.get("spentCents") or 0.0) >= settings["monthlyCapCents"]
    decision = _decide(settings, pool_state, len(nodes), hourly, now, over_cap,
                       wants_up=wants_up and BURST_JOBS_READY,
                       wants_down=idle_victim is not None and not wants_up,
                       up_reason="%d queued, oldest waited %d s" % (queued, wait_s),
                       down_reason="%s idle for %d min" % (
                           idle_victim, settings["scaleDown"]["idleMinutes"]),
                       hold_reason=("queued work, but burst runners hold no "
                                    "provider credentials yet (RUNNERS.md mode B)"
                                    if wants_up and not BURST_JOBS_READY
                                    else "no queued work waiting"))
    if decision["action"] == "down" and idle_victim:
        decision["node"] = idle_victim
    return decision


def _decide(settings, pool_state, count, hourly, now, over_cap, wants_up,
            wants_down, up_reason, down_reason, hold_reason):
    def result(action, reason):
        decision = {"action": action, "reason": reason, "atMs": now}
        pool_state["decision"] = decision
        return dict(decision)
    if over_cap and count > 0:
        return result("down", "monthly spend cap reached (%.1f of %d cents)" % (
            pool_state.get("spentCents") or 0.0, settings["monthlyCapCents"]))
    if count > settings["max"]:
        return result("down", "%d nodes above the maximum %d" % (count, settings["max"]))
    left = _cooldown_left(pool_state, settings, now)
    limited = _rate_limited(pool_state, now)
    if count < settings["min"]:
        if not _spend_ok(pool_state, settings, count, hourly):
            return result("none", "below the minimum, but one more node would pass "
                          "the monthly spend cap")
        if left or limited:
            return result("none", "below the minimum; waiting for the cooldown")
        return result("up", "%d nodes below the minimum %d" % (count, settings["min"]))
    if wants_up:
        if count >= settings["max"]:
            return result("none", up_reason + "; already at the maximum %d"
                          % settings["max"])
        if not _spend_ok(pool_state, settings, count, hourly):
            return result("none", up_reason + "; one more node would pass the "
                          "monthly spend cap")
        if left:
            return result("none", up_reason + "; cooldown %d s left" % (left // 1000))
        if limited:
            return result("none", up_reason + "; %d changes in 24 h already"
                          % MAX_ACTIONS_PER_DAY)
        return result("up", up_reason)
    if wants_down and count > settings["min"]:
        if left:
            return result("none", down_reason + "; cooldown %d s left" % (left // 1000))
        if limited:
            return result("none", down_reason + "; %d changes in 24 h already"
                          % MAX_ACTIONS_PER_DAY)
        return result("down", down_reason)
    return result("none", hold_reason)
