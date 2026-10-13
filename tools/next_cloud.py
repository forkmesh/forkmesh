#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

DEFAULT_PROVIDER = "vultr"
PROVIDERS = ("vultr", "aws")
DISPLAY = {"vultr": "Vultr", "aws": "AWS"}


class Provider:
    name = ""

    def guard(self, instance):
        raise NotImplementedError

    def fleet_instances(self):
        raise NotImplementedError

    def find_by_label(self, label):
        matches = [i for i in self.fleet_instances() if i.get("label") == label]
        if len(matches) > 1:
            raise GuardError("more than one instance is labelled %s" % label)
        return matches[0] if matches else None

    def instance(self, instance_id):
        raise NotImplementedError

    def create_instance(self, body):
        raise NotImplementedError

    def delete_instance(self, instance_id):
        raise NotImplementedError


def check_name(name):
    value = str(name or DEFAULT_PROVIDER).strip().lower()
    if value not in PROVIDERS:
        raise ToolError("unknown provider %r (known: %s)" % (name, ", ".join(PROVIDERS)))
    return value


def display(name):
    return DISPLAY[check_name(name)]


def provider_name(client):
    return check_name(getattr(client, "name", "") or DEFAULT_PROVIDER)


def instances_file():
    return common.config_dir() / "fleet-instances.json"


def load_instances():
    return common.load_json(instances_file(), {}) or {}


def entry_provider(entry):
    return check_name((entry or {}).get("provider") or DEFAULT_PROVIDER)


def recorded(label, instances=None):
    data = load_instances() if instances is None else instances
    entry = data.get(label) if isinstance(data, dict) else None
    return entry if isinstance(entry, dict) else {}


def recorded_provider(label, instances=None):
    return entry_provider(recorded(label, instances))


def from_config(name, region=None):
    name = check_name(name)
    if name == "aws":
        import next_aws
        return next_aws.Aws.from_config(region=region or None)
    import next_fleet
    return next_fleet.Vultr.from_config()


def for_label(label, given=None, instances=None):
    entry = recorded(label, instances)
    name = entry_provider(entry)
    if given is None:
        return None
    if provider_name(given) == name:
        return given
    return from_config(name, region=entry.get("region"))
