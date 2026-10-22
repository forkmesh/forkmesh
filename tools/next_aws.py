#!/usr/bin/env python3
from __future__ import annotations

import base64
import datetime
import hashlib
import hmac
import ipaddress
import json
import re
import secrets as _secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_cloud  # noqa: E402
import next_common as common  # noqa: E402
import next_fleet  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

ENV_FILE = "aws.env"
DEFAULT_REGION = "us-east-1"
EC2_VERSION = "2016-11-15"
STS_VERSION = "2011-06-15"
TAG_KEY = next_fleet.TAG
TAG_VALUE = "1"
KEY_NAME = next_fleet.SSH_KEY_NAME
DEBIAN_OWNER = "136693071363"
DEBIAN_PARAMETER = "/aws/service/debian/release/13/latest/%s"
FLEET_ARCH = "x86_64"
DEBIAN_ARCH = {"x86_64": "amd64", "arm64": "arm64"}
ROOT_VOLUME_GB = 20
IPV4_MONTHLY = 0.005 * 730
GP3_GB_MONTHLY = 0.08
HOURS_PER_MONTH = 730
LIVE_STATES = ("pending", "running", "stopping", "stopped")
INSTANCE_ID_RE = re.compile(r"^i-[0-9a-f]{8,17}$")
GROUP_ID_RE = re.compile(r"^sg-[0-9a-f]{8,17}$")
IMAGE_ID_RE = re.compile(r"^ami-[0-9a-f]{8,17}$")
REGION_RE = re.compile(r"^[a-z]{2}(-gov)?-[a-z]+-[0-9]{1,2}$")
TYPE_RE = re.compile(r"^[a-z][a-z0-9-]*\.[a-z0-9]+$")
ACCESS_KEY_RE = re.compile(r"^(AKIA|ASIA)[A-Z0-9]{16}$")
SECRET_KEY_RE = re.compile(r"^[A-Za-z0-9/+=]{30,128}$")
SECURITY_GROUPS = {"core": "fmnext-core", "runner": "fmnext-runner"}
CORE_KINDS = ("core", "auto")
RUNNER_KINDS = ("run", "burst")
INSTANCE_TYPES = {
    "t4g.nano": {"vcpu": 2, "ram": 512, "hourly": 0.0042, "arch": "arm64"},
    "t4g.micro": {"vcpu": 2, "ram": 1024, "hourly": 0.0084, "arch": "arm64"},
    "t4g.small": {"vcpu": 2, "ram": 2048, "hourly": 0.0168, "arch": "arm64"},
    "t4g.medium": {"vcpu": 2, "ram": 4096, "hourly": 0.0336, "arch": "arm64"},
    "t3a.nano": {"vcpu": 2, "ram": 512, "hourly": 0.0047, "arch": "x86_64"},
    "t3a.micro": {"vcpu": 2, "ram": 1024, "hourly": 0.0094, "arch": "x86_64"},
    "t3a.small": {"vcpu": 2, "ram": 2048, "hourly": 0.0188, "arch": "x86_64"},
    "t3a.medium": {"vcpu": 2, "ram": 4096, "hourly": 0.0376, "arch": "x86_64"},
    "t3.nano": {"vcpu": 2, "ram": 512, "hourly": 0.0052, "arch": "x86_64"},
    "t3.micro": {"vcpu": 2, "ram": 1024, "hourly": 0.0104, "arch": "x86_64"},
    "t3.small": {"vcpu": 2, "ram": 2048, "hourly": 0.0208, "arch": "x86_64"},
    "t3.medium": {"vcpu": 2, "ram": 4096, "hourly": 0.0416, "arch": "x86_64"},
}
REGIONS = (
    "us-east-1", "us-east-2", "us-west-1", "us-west-2", "ca-central-1",
    "eu-west-1", "eu-west-2", "eu-west-3", "eu-central-1", "eu-north-1",
    "eu-south-1", "ap-south-1", "ap-northeast-1", "ap-northeast-2",
    "ap-northeast-3", "ap-southeast-1", "ap-southeast-2", "sa-east-1",
)
PRICE_NOTE = ("AWS prices are us-east-1 on-demand estimates: instance + public "
              "IPv4 + %d GB gp3; other regions cost more" % ROOT_VOLUME_GB)
CLOUD_CONFIG = "#cloud-config\nusers: []\ndisable_root: false\nhostname: %s\n"


class Credentials:
    def __init__(self, access_key_id, secret_access_key, session_token=""):
        self.access_key_id = str(access_key_id or "").strip()
        self._secret = str(secret_access_key or "").strip()
        self.session_token = str(session_token or "").strip()

    def check(self):
        if not ACCESS_KEY_RE.match(self.access_key_id):
            raise ToolError("AWS_ACCESS_KEY_ID is not an AKIA/ASIA access key id")
        if not SECRET_KEY_RE.match(self._secret):
            raise ToolError("AWS_SECRET_ACCESS_KEY is malformed")
        if self.access_key_id.startswith("ASIA") and not self.session_token:
            raise ToolError("an ASIA (temporary) access key needs AWS_SESSION_TOKEN")
        return self

    @property
    def secret_access_key(self):
        return self._secret

    def __repr__(self):
        return "Credentials(%s…, secret hidden)" % self.access_key_id[:4]


def check_region(region):
    value = str(region or "").strip().lower()
    if not REGION_RE.match(value):
        raise ToolError("bad AWS region %r" % region)
    return value


def load_credentials():
    path = common.config_dir() / ENV_FILE
    values = common.read_env_file(path)
    access = values.get("AWS_ACCESS_KEY_ID") or ""
    secret = values.get("AWS_SECRET_ACCESS_KEY") or ""
    if not access or not secret:
        raise ToolError("AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY are not set in %s" % path)
    region = values.get("AWS_REGION") or values.get("AWS_DEFAULT_REGION") or DEFAULT_REGION
    return (Credentials(access, secret, values.get("AWS_SESSION_TOKEN") or "").check(),
            check_region(region))


def _sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def _hmac(key, message):
    return hmac.new(key, message.encode("utf-8"), hashlib.sha256).digest()


def uri_encode(value, safe="-_.~"):
    return urllib.parse.quote(str(value), safe=safe)


def signing_key(secret_key, date, region, service):
    key = _hmac(("AWS4" + secret_key).encode("utf-8"), date)
    key = _hmac(key, region)
    key = _hmac(key, service)
    return _hmac(key, "aws4_request")


def canonical_query(query):
    pairs = urllib.parse.parse_qsl(query, keep_blank_values=True)
    encoded = sorted((uri_encode(k), uri_encode(v)) for k, v in pairs)
    return "&".join("%s=%s" % pair for pair in encoded)


def canonical_headers(headers):
    folded = {}
    for name, value in headers.items():
        key = name.strip().lower()
        clean = " ".join(str(value).strip().split())
        folded[key] = clean if key not in folded else folded[key] + "," + clean
    names = sorted(folded)
    return "".join("%s:%s\n" % (n, folded[n]) for n in names), ";".join(names)


def canonical_request(method, url, headers, body):
    parts = urllib.parse.urlsplit(url)
    path = uri_encode(parts.path or "/", safe="/-_.~")
    header_text, signed = canonical_headers(headers)
    payload = _sha256_hex(body or b"")
    text = "\n".join([method.upper(), path, canonical_query(parts.query),
                      header_text, signed, payload])
    return text, signed


def string_to_sign(amz_date, scope, request_text):
    return "\n".join(["AWS4-HMAC-SHA256", amz_date, scope,
                      _sha256_hex(request_text.encode("utf-8"))])


def sign(method, url, headers, body, credentials, region, service, amz_date):
    headers = dict(headers)
    lower = {k.lower() for k in headers}
    if "host" not in lower:
        headers["Host"] = urllib.parse.urlsplit(url).netloc
    if "x-amz-date" not in lower:
        headers["X-Amz-Date"] = amz_date
    if credentials.session_token and "x-amz-security-token" not in lower:
        headers["X-Amz-Security-Token"] = credentials.session_token
    request_text, signed = canonical_request(method, url, headers, body)
    scope = "%s/%s/%s/aws4_request" % (amz_date[:8], region, service)
    key = signing_key(credentials.secret_access_key, amz_date[:8], region, service)
    signature = hmac.new(key, string_to_sign(amz_date, scope, request_text).encode("utf-8"),
                         hashlib.sha256).hexdigest()
    headers["Authorization"] = (
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (
            credentials.access_key_id, scope, signed, signature))
    return headers


def amz_date(now):
    return datetime.datetime.fromtimestamp(now, datetime.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ")


def _local(tag):
    return tag.rsplit("}", 1)[-1]


def xml_value(element):
    children = list(element)
    if not children:
        return (element.text or "").strip()
    if all(_local(c.tag) in ("item", "member") for c in children):
        return [xml_value(c) for c in children]
    out = {}
    for child in children:
        out[_local(child.tag)] = xml_value(child)
    return out


def parse_xml(raw):
    try:
        root = ElementTree.fromstring(raw)
    except ElementTree.ParseError as error:
        raise ToolError("AWS returned malformed XML: %s" % error) from None
    return _local(root.tag), xml_value(root)


def as_list(value):
    if isinstance(value, list):
        return value
    if value in (None, ""):
        return []
    return [value]


def tag_dict(tag_set):
    out = {}
    for item in as_list(tag_set):
        if isinstance(item, dict) and item.get("key"):
            out[str(item["key"])] = str(item.get("value") or "")
    return out


def error_text(raw):
    try:
        _, body = parse_xml(raw)
    except ToolError:
        return raw[:300].decode("utf-8", "replace")
    found = {}

    def walk(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key in ("Code", "Message") and isinstance(value, str):
                    found.setdefault(key, value)
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(body)
    return ("%s: %s" % (found.get("Code", "error"), found.get("Message", ""))).strip()


class AwsClient:
    def __init__(self, credentials, region, opener=None, clock=time.time):
        self.credentials = credentials
        self.region = check_region(region)
        self.opener = opener or urllib.request.urlopen
        self.clock = clock

    def endpoint(self, service):
        return "https://%s.%s.amazonaws.com/" % (service, self.region)

    def _send(self, service, body, headers, label):
        url = self.endpoint(service)
        signed = sign("POST", url, headers, body, self.credentials, self.region,
                      service, amz_date(self.clock()))
        request = urllib.request.Request(url, data=body, method="POST", headers=signed)
        try:
            with self.opener(request, timeout=30) as response:
                status = response.status
                raw = response.read()
        except urllib.error.HTTPError as error:
            status = error.code
            raw = error.read() or b""
        if status != 200:
            raise ToolError("AWS %s -> HTTP %d %s" % (label, status, error_text(raw)))
        return raw

    def query(self, service, action, params=None, version=EC2_VERSION):
        fields = {"Action": action, "Version": version}
        fields.update({k: str(v) for k, v in (params or {}).items()})
        body = urllib.parse.urlencode(sorted(fields.items()),
                                      quote_via=urllib.parse.quote).encode()
        raw = self._send(service, body, {
            "Content-Type": "application/x-www-form-urlencoded; charset=utf-8",
            "Accept": "application/xml", "User-Agent": "fmnext-tools/1"},
            "%s %s" % (service, action))
        return parse_xml(raw)[1]

    def json_call(self, service, target, payload):
        body = json.dumps(payload, sort_keys=True).encode()
        raw = self._send(service, body, {
            "Content-Type": "application/x-amz-json-1.1", "X-Amz-Target": target,
            "User-Agent": "fmnext-tools/1"}, target)
        return json.loads(raw) if raw.strip() else {}

    def __repr__(self):
        return "AwsClient(%s)" % self.region


def caller_identity(client):
    result = client.query("sts", "GetCallerIdentity", version=STS_VERSION)
    found = result.get("GetCallerIdentityResult") or {}
    return {"account": found.get("Account", ""), "arn": found.get("Arn", ""),
            "user_id": found.get("UserId", "")}


def plan_monthly(instance_type, volume_gb=ROOT_VOLUME_GB):
    info = INSTANCE_TYPES.get(instance_type)
    if not info:
        return None
    return round(info["hourly"] * HOURS_PER_MONTH + IPV4_MONTHLY
                 + GP3_GB_MONTHLY * volume_gb, 2)


def plans(region=None, arch=FLEET_ARCH):
    locations = [region] if region else list(REGIONS)
    out = []
    for name, info in sorted(INSTANCE_TYPES.items()):
        if arch and info["arch"] != arch:
            continue
        out.append({"id": name, "ram": info["ram"], "vcpu_count": info["vcpu"],
                    "monthly_cost": plan_monthly(name), "disk": ROOT_VOLUME_GB,
                    "type": info["arch"], "locations": locations})
    return out


def cheapest_type(arch=FLEET_ARCH, min_ram=next_fleet.MIN_RAM_MB):
    best = None
    for name, info in INSTANCE_TYPES.items():
        if info["arch"] != arch or info["ram"] < min_ram:
            continue
        key = (info["hourly"], -info["ram"], name)
        if best is None or key < best[0]:
            best = (key, name)
    return None if best is None else best[1]


def role_for_label(label):
    match = next_fleet.LABEL_RE.match(str(label or ""))
    if not match:
        raise GuardError("refusing %r: not an fmnext-core/run/auto/burst-N label" % label)
    kind = match.group(1)
    if kind in CORE_KINDS:
        return "core"
    if kind in RUNNER_KINDS:
        return "runner"
    raise GuardError("refusing %s: %s builders are Vultr-only" % (label, kind))


def fleet_ports(fleet=None):
    import render
    wg_port, tcp, udp, relay = 51820, [], [], None
    if fleet:
        wg_port = int((fleet.get("wireguard") or {}).get("port", 51820))
        if render.turn_enabled(fleet):
            turn_tcp, turn_udp, relay_text = render.turn_ports(fleet)
            tcp, udp = list(turn_tcp), list(turn_udp)
            low, _, high = relay_text.partition("-")
            relay = (int(low), int(high))
        udp += render.calls_ports(fleet)[1]
    else:
        defaults = render.TURN_DEFAULTS
        tcp, udp = [defaults["port"]], [defaults["port"],
                                        render.CALLS_DEFAULTS["udp_port"]]
        relay = (defaults["relay_min_port"], defaults["relay_max_port"])
    return wg_port, tcp, udp, relay


def ingress_rules(role, fleet=None, ssh_sources=("0.0.0.0/0",)):
    rules = set()
    for cidr in ssh_sources:
        try:
            network = ipaddress.IPv4Network(str(cidr), strict=False)
        except ValueError:
            raise ToolError("--ssh-from %r is not an IPv4 address or CIDR" % cidr) from None
        rules.add(("tcp", 22, 22, str(network)))
    if role == "core":
        wg_port, turn_tcp, turn_udp, relay = fleet_ports(fleet)
        for port in [80, 443] + turn_tcp:
            rules.add(("tcp", int(port), int(port), "0.0.0.0/0"))
        for port in [443, wg_port] + turn_udp:
            rules.add(("udp", int(port), int(port), "0.0.0.0/0"))
        if relay:
            rules.add(("udp", relay[0], relay[1], "0.0.0.0/0"))
        for icmp_type in (3, 8, 11):
            rules.add(("icmp", icmp_type, -1, "0.0.0.0/0"))
    return rules


def rule_text(rule):
    proto, low, high, cidr = rule
    if proto == "icmp":
        return "icmp type %s from %s" % (low, cidr)
    if proto == "-1":
        return "all traffic from %s" % cidr
    ports = str(low) if low == high else "%s-%s" % (low, high)
    return "%s %s from %s" % (proto, ports, cidr)


def permissions_of(group):
    found = set()
    for perm in as_list(group.get("ipPermissions")):
        proto = str(perm.get("ipProtocol") or "")
        low = int(perm.get("fromPort") or -1) if proto != "-1" else -1
        high = int(perm.get("toPort") or -1) if proto != "-1" else -1
        for item in as_list(perm.get("ipRanges")):
            found.add((proto, low, high, str(item.get("cidrIp") or "")))
        for item in as_list(perm.get("ipv6Ranges")):
            found.add((proto, low, high, str(item.get("cidrIpv6") or "")))
        for item in as_list(perm.get("groups")):
            found.add((proto, low, high, "group:" + str(item.get("groupId") or "")))
        for item in as_list(perm.get("prefixListIds")):
            found.add((proto, low, high, "prefix:" + str(item.get("prefixListId") or "")))
    return found


def ingress_params(rules):
    params = {}
    for index, (proto, low, high, cidr) in enumerate(sorted(rules), start=1):
        prefix = "IpPermissions.%d." % index
        params[prefix + "IpProtocol"] = proto
        params[prefix + "FromPort"] = low
        params[prefix + "ToPort"] = high
        params[prefix + "IpRanges.1.CidrIp"] = cidr
        params[prefix + "IpRanges.1.Description"] = "fmnext nftables mirror"
    return params


def tag_spec_params(index, resource_type, label=""):
    prefix = "TagSpecification.%d." % index
    params = {prefix + "ResourceType": resource_type,
              prefix + "Tag.1.Key": TAG_KEY, prefix + "Tag.1.Value": TAG_VALUE}
    if label:
        params[prefix + "Tag.2.Key"] = "Name"
        params[prefix + "Tag.2.Value"] = label
    return params


def run_instances_params(label, image_id, instance_type, group_id, root_device,
                         volume_gb=ROOT_VOLUME_GB, termination_protection=False,
                         client_token=None):
    next_fleet.guard_instance({"label": label, "tags": [TAG_KEY]})
    if not IMAGE_ID_RE.match(str(image_id)):
        raise GuardError("refusing malformed image id %r" % image_id)
    if not GROUP_ID_RE.match(str(group_id)):
        raise GuardError("refusing malformed security group id %r" % group_id)
    if not TYPE_RE.match(str(instance_type)):
        raise ToolError("bad instance type %r" % instance_type)
    if not re.fullmatch(r"/dev/[a-z0-9]+", str(root_device)):
        raise ToolError("bad root device name %r" % root_device)
    params = {
        "ImageId": image_id, "InstanceType": instance_type,
        "MinCount": 1, "MaxCount": 1, "KeyName": KEY_NAME,
        "ClientToken": client_token or "%s-%s" % (label, _secrets.token_hex(8)),
        "NetworkInterface.1.DeviceIndex": 0,
        "NetworkInterface.1.AssociatePublicIpAddress": "true",
        "NetworkInterface.1.DeleteOnTermination": "true",
        "NetworkInterface.1.SecurityGroupId.1": group_id,
        "MetadataOptions.HttpTokens": "required",
        "MetadataOptions.HttpEndpoint": "enabled",
        "MetadataOptions.HttpPutResponseHopLimit": 1,
        "MetadataOptions.InstanceMetadataTags": "disabled",
        "BlockDeviceMapping.1.DeviceName": root_device,
        "BlockDeviceMapping.1.Ebs.VolumeType": "gp3",
        "BlockDeviceMapping.1.Ebs.VolumeSize": int(volume_gb),
        "BlockDeviceMapping.1.Ebs.Encrypted": "true",
        "BlockDeviceMapping.1.Ebs.DeleteOnTermination": "true",
        "InstanceInitiatedShutdownBehavior": "stop",
        "DisableApiTermination": "true" if termination_protection else "false",
        "UserData": base64.b64encode((CLOUD_CONFIG % label).encode()).decode(),
    }
    if instance_type.split(".")[0] in ("t2", "t3", "t3a", "t4g"):
        params["CreditSpecification.CpuCredits"] = "standard"
    params.update(tag_spec_params(1, "instance", label))
    params.update(tag_spec_params(2, "volume", label))
    params.update(tag_spec_params(3, "network-interface", label))
    return params


def normalize_instance(raw, region):
    tags = tag_dict(raw.get("tagSet"))
    ipv6 = ""
    for nic in as_list(raw.get("networkInterfaceSet")):
        for address in as_list((nic or {}).get("ipv6AddressesSet")):
            if isinstance(address, dict) and address.get("ipv6Address"):
                ipv6 = ipv6 or address["ipv6Address"]
    state = raw.get("instanceState") or {}
    return {
        "id": str(raw.get("instanceId") or ""),
        "label": tags.get("Name", ""),
        "tags": [TAG_KEY] if tags.get(TAG_KEY) == TAG_VALUE else [],
        "aws_tags": tags,
        "main_ip": str(raw.get("ipAddress") or ""),
        "v6_main_ip": ipv6,
        "region": region,
        "zone": str((raw.get("placement") or {}).get("availabilityZone") or ""),
        "plan": str(raw.get("instanceType") or ""),
        "status": str(state.get("name") or "") if isinstance(state, dict) else "",
        "image": str(raw.get("imageId") or ""),
        "provider": "aws",
    }


def guard_aws_instance(instance, region=None):
    instance = instance or {}
    if instance.get("provider") != "aws":
        raise GuardError("refusing %r: not an AWS instance record" % instance.get("label"))
    if not INSTANCE_ID_RE.match(str(instance.get("id") or "")):
        raise GuardError("refusing malformed instance id %r" % instance.get("id"))
    tags = instance.get("aws_tags") or {}
    if tags.get(TAG_KEY) != TAG_VALUE:
        raise GuardError("refusing instance %s: not tagged %s=%s (tags: %s)" % (
            instance.get("id"), TAG_KEY, TAG_VALUE, ", ".join(sorted(tags)) or "none"))
    if tags.get("Name", "") != instance.get("label"):
        raise GuardError("refusing instance %s: Name tag does not match its label"
                         % instance.get("id"))
    next_fleet.guard_instance(instance)
    if region and instance.get("region") != region:
        raise GuardError("refusing %s: in %s, not %s" % (
            instance.get("label"), instance.get("region"), region))
    return instance


def public_key_body(text):
    fields = str(text or "").split()
    if len(fields) < 2 or not fields[0].startswith(("ssh-ed25519", "ssh-rsa", "ecdsa-")):
        raise ToolError("the fleet public key is not an OpenSSH public key")
    return fields[0], fields[1]


class Aws(next_cloud.Provider):
    name = "aws"

    def __init__(self, client, offline=False):
        self.client = client
        self.offline = bool(offline)
        self.region = client.region if client is not None else DEFAULT_REGION

    @classmethod
    def from_config(cls, region=None, opener=None):
        credentials, configured = load_credentials()
        return cls(AwsClient(credentials, check_region(region or configured), opener))

    @classmethod
    def offline_plan(cls, region=None):
        provider = cls(None, offline=True)
        provider.region = check_region(region or DEFAULT_REGION)
        return provider

    def ec2(self, action, params=None):
        if self.client is None:
            raise ToolError("no AWS credentials: create %s" % (common.config_dir() / ENV_FILE))
        return self.client.query("ec2", action, params)

    def guard(self, instance):
        return guard_aws_instance(instance, self.region)

    def _describe(self, params):
        found, token = [], ""
        for _ in range(50):
            page = dict(params)
            if token:
                page["NextToken"] = token
            result = self.ec2("DescribeInstances", page)
            for reservation in as_list(result.get("reservationSet")):
                for raw in as_list((reservation or {}).get("instancesSet")):
                    if isinstance(raw, dict):
                        found.append(normalize_instance(raw, self.region))
            token = str(result.get("nextToken") or "")
            if not token:
                break
        return found

    def fleet_instances(self):
        if self.offline:
            return []
        params = {"Filter.1.Name": "tag:%s" % TAG_KEY, "Filter.1.Value.1": TAG_VALUE,
                  "Filter.2.Name": "instance-state-name"}
        for index, state in enumerate(LIVE_STATES, start=1):
            params["Filter.2.Value.%d" % index] = state
        out = []
        for instance in self._describe(params):
            try:
                out.append(self.guard(instance))
            except GuardError:
                continue
        return out

    def instance(self, instance_id):
        if not INSTANCE_ID_RE.match(str(instance_id)):
            raise GuardError("refusing malformed instance id %r" % instance_id)
        found = self._describe({"InstanceId.1": instance_id})
        if len(found) != 1 or found[0]["id"] != instance_id:
            raise ToolError("AWS has no instance %s in %s" % (instance_id, self.region))
        return found[0]

    def create_instance(self, params):
        label = params.get("TagSpecification.1.Tag.2.Value")
        if (params.get("TagSpecification.1.ResourceType") != "instance"
                or params.get("TagSpecification.1.Tag.1.Key") != TAG_KEY
                or params.get("TagSpecification.1.Tag.1.Value") != TAG_VALUE
                or params.get("TagSpecification.1.Tag.2.Key") != "Name"):
            raise GuardError("refusing RunInstances without the %s=%s and Name tags"
                             % (TAG_KEY, TAG_VALUE))
        next_fleet.guard_instance({"label": label, "tags": [TAG_KEY]})
        if str(params.get("MinCount")) != "1" or str(params.get("MaxCount")) != "1":
            raise GuardError("refusing RunInstances for more than one instance")
        if params.get("MetadataOptions.HttpTokens") != "required":
            raise GuardError("refusing RunInstances without IMDSv2")
        if params.get("BlockDeviceMapping.1.Ebs.Encrypted") != "true":
            raise GuardError("refusing RunInstances with an unencrypted root volume")
        result = self.ec2("RunInstances", params)
        created = [normalize_instance(raw, self.region)
                   for raw in as_list(result.get("instancesSet")) if isinstance(raw, dict)]
        if len(created) != 1:
            raise ToolError("RunInstances returned %d instances" % len(created))
        instance = created[0]
        if not instance["aws_tags"]:
            tags = {"Name": label, TAG_KEY: TAG_VALUE}
            instance.update(label=label, aws_tags=tags, tags=[TAG_KEY])
        return self.guard(instance)

    def delete_instance(self, instance_id):
        self.guard(self.instance(instance_id))
        result = self.ec2("TerminateInstances", {"InstanceId.1": instance_id})
        changed = [i for i in as_list(result.get("instancesSet")) if isinstance(i, dict)]
        if [i.get("instanceId") for i in changed] != [instance_id]:
            raise ToolError("TerminateInstances did not confirm %s" % instance_id)

    def plans(self):
        return plans(self.region)

    def debian_image(self, arch=FLEET_ARCH):
        if self.offline:
            return {"id": "(Debian 13 %s AMI from SSM, resolved on --execute)"
                    % DEBIAN_ARCH[arch], "root_device": "/dev/xvda"}
        name = DEBIAN_PARAMETER % DEBIAN_ARCH[arch]
        answer = self.client.json_call("ssm", "AmazonSSM.GetParameter", {"Name": name})
        image_id = str((answer.get("Parameter") or {}).get("Value") or "")
        if not IMAGE_ID_RE.match(image_id):
            raise ToolError("SSM %s returned %r, not an AMI id" % (name, image_id))
        result = self.ec2("DescribeImages", {"ImageId.1": image_id,
                                             "Owner.1": DEBIAN_OWNER})
        images = [i for i in as_list(result.get("imagesSet")) if isinstance(i, dict)]
        if len(images) != 1:
            raise GuardError("%s is not an image owned by Debian (%s)" % (
                image_id, DEBIAN_OWNER))
        image = images[0]
        if image.get("imageOwnerId") != DEBIAN_OWNER:
            raise GuardError("%s is owned by %s, not Debian" % (
                image_id, image.get("imageOwnerId")))
        if image.get("architecture") != arch or image.get("imageState") != "available":
            raise GuardError("%s is %s/%s, not an available %s image" % (
                image_id, image.get("architecture"), image.get("imageState"), arch))
        return {"id": image_id, "root_device": image.get("rootDeviceName") or "/dev/xvda",
                "name": image.get("name", "")}

    def ensure_key_pair(self, runner, public_key):
        key_type, key_body = public_key_body(public_key)
        if self.offline:
            runner.step("import SSH key pair %s into AWS %s if it is missing (tag %s=%s)"
                        % (KEY_NAME, self.region, TAG_KEY, TAG_VALUE))
            return KEY_NAME
        result = self.ec2("DescribeKeyPairs", {"Filter.1.Name": "key-name",
                                               "Filter.1.Value.1": KEY_NAME,
                                               "IncludePublicKey": "true"})
        pairs = [p for p in as_list(result.get("keySet")) if isinstance(p, dict)]
        if pairs:
            existing = str(pairs[0].get("publicKey") or "").split()
            if existing[:2] != [key_type, key_body]:
                raise GuardError("AWS key pair %s in %s is a different key than %s"
                                 % (KEY_NAME, self.region, common.ssh_key_path()))
            return KEY_NAME
        params = {"KeyName": KEY_NAME, "PublicKeyMaterial": base64.b64encode(
            ("%s %s" % (key_type, key_body)).encode()).decode()}
        params.update(tag_spec_params(1, "key-pair"))
        runner.step("import SSH key pair %s into AWS %s (tag %s=%s)" % (
            KEY_NAME, self.region, TAG_KEY, TAG_VALUE),
            lambda: self.ec2("ImportKeyPair", params))
        return KEY_NAME

    def ensure_security_group(self, runner, role, rules):
        name = SECURITY_GROUPS[role]
        listing = "\n".join("    " + rule_text(r) for r in sorted(rules))
        if self.offline:
            runner.step("create or reuse security group %s in AWS %s (tag %s=%s), "
                        "ingress:\n%s" % (name, self.region, TAG_KEY, TAG_VALUE, listing))
            return "sg-00000000"
        result = self.ec2("DescribeSecurityGroups", {"Filter.1.Name": "group-name",
                                                     "Filter.1.Value.1": name})
        groups = [g for g in as_list(result.get("securityGroupInfo")) if isinstance(g, dict)]
        if len(groups) > 1:
            raise GuardError("more than one security group is named %s in %s" % (
                name, self.region))
        if groups:
            group = groups[0]
            if tag_dict(group.get("tagSet")).get(TAG_KEY) != TAG_VALUE:
                raise GuardError("refusing security group %s (%s): not tagged %s=%s" % (
                    name, group.get("groupId"), TAG_KEY, TAG_VALUE))
            group_id = str(group.get("groupId") or "")
            if not GROUP_ID_RE.match(group_id):
                raise GuardError("refusing malformed security group id %r" % group_id)
            present = permissions_of(group)
            extra = present - set(rules)
            if extra:
                raise GuardError("security group %s allows %s, which the nftables "
                                 "render does not; remove it in the AWS console" % (
                                     name, ", ".join(rule_text(r) for r in sorted(extra))))
            missing = set(rules) - present
            if missing:
                runner.step("open %d missing port rule(s) on %s" % (len(missing), name),
                            lambda: self.ec2("AuthorizeSecurityGroupIngress", dict(
                                ingress_params(missing), GroupId=group_id)))
            return group_id
        params = {"GroupName": name,
                  "GroupDescription": "ForkMesh Next %s (mirrors the node nftables)" % role}
        params.update(tag_spec_params(1, "security-group"))
        holder = {}

        def create():
            created = self.ec2("CreateSecurityGroup", params)
            group_id = str(created.get("groupId") or "")
            if not GROUP_ID_RE.match(group_id):
                raise ToolError("CreateSecurityGroup returned %r" % group_id)
            self.ec2("AuthorizeSecurityGroupIngress",
                     dict(ingress_params(rules), GroupId=group_id))
            holder["id"] = group_id
            return group_id
        runner.step("create security group %s in AWS %s (tag %s=%s), ingress:\n%s" % (
            name, self.region, TAG_KEY, TAG_VALUE, listing), create)
        return holder.get("id", "sg-00000000")

    def create_labelled(self, label, runner, plan_id=None, region=None, fleet=None,
                        public_key="", ssh_sources=("0.0.0.0/0",),
                        termination_protection=False, volume_gb=ROOT_VOLUME_GB):
        if region and check_region(region) != self.region:
            raise ToolError("the AWS client is for %s, not %s (set AWS_REGION or "
                            "--region)" % (self.region, region))
        role = role_for_label(label)
        instance_type = plan_id or cheapest_type(FLEET_ARCH)
        if not TYPE_RE.match(str(instance_type)):
            raise ToolError("bad instance type %r" % instance_type)
        info = INSTANCE_TYPES.get(instance_type)
        if info and info["arch"] != FLEET_ARCH:
            raise ToolError("%s is %s; the fleet installs %s binaries" % (
                instance_type, info["arch"], FLEET_ARCH))
        if info and info["ram"] < next_fleet.MIN_RAM_MB:
            runner.say("note: %s has %d MB RAM, below the %d MB a fleet node needs"
                       % (instance_type, info["ram"], next_fleet.MIN_RAM_MB))
        image = self.debian_image(FLEET_ARCH)
        self.ensure_key_pair(runner, public_key)
        group_id = self.ensure_security_group(
            runner, role, ingress_rules(role, fleet, ssh_sources))
        cost = plan_monthly(instance_type, volume_gb)
        description = ("create AWS instance %s (%s, %s, %s, %d GB encrypted gp3, "
                       "IMDSv2 only, tags Name=%s %s=%s%s, about %s/month)" % (
                           label, instance_type, self.region, image["id"], volume_gb,
                           label, TAG_KEY, TAG_VALUE,
                           ", termination protection" if termination_protection else "",
                           "$%.2f" % cost if cost is not None else "an unknown price"))
        if self.offline:
            runner.step(description)
            return None
        params = run_instances_params(label, image["id"], instance_type, group_id,
                                      image["root_device"], volume_gb,
                                      termination_protection)
        return runner.step(description, lambda: self.create_instance(params))
