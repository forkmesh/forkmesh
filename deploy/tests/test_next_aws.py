import base64
import io
import json
import os
import urllib.error
import urllib.parse
from pathlib import Path

import pytest

import next_aws
import next_cloud
import next_common as common
import next_fleet
from fakes import FakeHttp, instance
from next_common import GuardError, Runner, ToolError

REPO = Path(__file__).resolve().parents[2]
EXAMPLE = REPO / "deploy" / "fleet.example.toml"
NS = "http://ec2.amazonaws.com/doc/2016-11-15/"
VECTOR_KEY = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
VECTOR_DATE = "20150830T123600Z"
ACCESS = "AKIA" + "ABCDEFGHIJKLMNOP"
SECRET = "abcdEFGHijklMNOPqrstUVWXyz0123456789+/AB"
PUBKEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFleetKeyForTests fmnext-fleet"
AMI = "ami-0123456789abcdef0"


def vector_signature(method, url, headers, body=b"", service="service"):
    credentials = next_aws.Credentials("AKIDEXAMPLE", VECTOR_KEY)
    signed = next_aws.sign(method, url, headers, body, credentials, "us-east-1",
                           service, VECTOR_DATE)
    return signed["Authorization"]


@pytest.mark.parametrize("method,url,headers,body,expected", [
    ("GET", "https://example.amazonaws.com/",
     {"Host": "example.amazonaws.com", "X-Amz-Date": VECTOR_DATE}, b"",
     "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"),
    ("POST", "https://example.amazonaws.com/",
     {"Host": "example.amazonaws.com", "X-Amz-Date": VECTOR_DATE}, b"",
     "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b"),
    ("GET", "https://example.amazonaws.com/?Param2=value2&Param1=value1",
     {"Host": "example.amazonaws.com", "X-Amz-Date": VECTOR_DATE}, b"",
     "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500"),
    ("POST", "https://example.amazonaws.com/",
     {"Content-Type": "application/x-www-form-urlencoded",
      "Host": "example.amazonaws.com", "X-Amz-Date": VECTOR_DATE}, b"Param1=value1",
     "ff11897932ad3f4e8b18135d722051e5ac45fc38421b1da7b9d196a0fe09473a"),
], ids=["get-vanilla", "post-vanilla", "get-vanilla-query-order-key-case",
        "post-x-www-form-urlencoded"])
def test_sigv4_matches_the_aws_test_suite(method, url, headers, body, expected):
    authorization = vector_signature(method, url, headers, body)
    assert authorization.startswith(
        "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, ")
    assert authorization.endswith("Signature=" + expected)


def test_sigv4_matches_the_documented_iam_example():
    authorization = vector_signature(
        "GET", "https://iam.amazonaws.com/?Action=ListUsers&Version=2010-05-08",
        {"Host": "iam.amazonaws.com",
         "Content-Type": "application/x-www-form-urlencoded; charset=utf-8",
         "X-Amz-Date": VECTOR_DATE}, service="iam")
    assert "SignedHeaders=content-type;host;x-amz-date" in authorization
    assert authorization.endswith(
        "Signature=5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7")


def test_signing_key_matches_the_documented_derivation():
    key = next_aws.signing_key(VECTOR_KEY, "20120215", "us-east-1", "iam")
    assert key.hex() == "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d"


def test_session_token_is_signed():
    credentials = next_aws.Credentials("AKIDEXAMPLE", VECTOR_KEY, "token-1")
    signed = next_aws.sign("POST", "https://sts.us-east-1.amazonaws.com/", {}, b"",
                           credentials, "us-east-1", "sts", VECTOR_DATE)
    assert signed["X-Amz-Security-Token"] == "token-1"
    assert "x-amz-security-token" in signed["Authorization"]


# --------------------------------------------------------------------------
# credentials


def write_env(sandbox, text):
    path = sandbox / "aws.env"
    path.write_text(text)
    path.chmod(0o600)
    return path


def test_credentials_load_from_aws_env_and_never_print_the_secret(sandbox):
    write_env(sandbox, "# fleet\nAWS_ACCESS_KEY_ID=%s\nexport AWS_SECRET_ACCESS_KEY='%s'\n"
              "AWS_REGION=eu-west-1\n" % (ACCESS, SECRET))
    credentials, region = next_aws.load_credentials()
    assert region == "eu-west-1" and credentials.access_key_id == ACCESS
    assert SECRET not in repr(credentials) and ACCESS not in repr(credentials)
    client = next_aws.AwsClient(credentials, region)
    assert SECRET not in repr(client)
    assert SECRET not in repr(next_aws.Aws(client))


def test_credentials_default_region_and_refusals(sandbox):
    with pytest.raises(ToolError, match="missing"):
        next_aws.load_credentials()
    write_env(sandbox, "AWS_ACCESS_KEY_ID=%s\nAWS_SECRET_ACCESS_KEY=%s\n" % (ACCESS, SECRET))
    assert next_aws.load_credentials()[1] == "us-east-1"
    write_env(sandbox, "AWS_ACCESS_KEY_ID=nope\nAWS_SECRET_ACCESS_KEY=%s\n" % SECRET)
    with pytest.raises(ToolError, match="access key id"):
        next_aws.load_credentials()
    write_env(sandbox, "AWS_ACCESS_KEY_ID=%s\nAWS_SECRET_ACCESS_KEY=%s\n"
              % ("ASIA" + "ABCDEFGHIJKLMNOP", SECRET))
    with pytest.raises(ToolError, match="SESSION_TOKEN"):
        next_aws.load_credentials()
    write_env(sandbox, "AWS_ACCESS_KEY_ID=%s\nAWS_SECRET_ACCESS_KEY=%s\nAWS_REGION=../x\n"
              % (ACCESS, SECRET))
    with pytest.raises(ToolError, match="region"):
        next_aws.load_credentials()


# --------------------------------------------------------------------------
# fake EC2 / SSM / STS transport


def xml(action, inner, ns=NS):
    return ('<?xml version="1.0" encoding="UTF-8"?><%sResponse xmlns="%s">'
            "<requestId>r-1</requestId>%s</%sResponse>" % (action, ns, inner, action)
            ).encode()


def tag_set(tags):
    return "<tagSet>%s</tagSet>" % "".join(
        "<item><key>%s</key><value>%s</value></item>" % kv for kv in tags.items())


def instance_xml(instance_id, name, tags=None, ip="198.51.100.40", state="running",
                 instance_type="t3a.micro"):
    tags = {"Name": name, "forkmesh-next": "1"} if tags is None else tags
    return ("<item><instanceId>%s</instanceId><imageId>%s</imageId>"
            "<instanceState><code>16</code><name>%s</name></instanceState>"
            "<instanceType>%s</instanceType><ipAddress>%s</ipAddress>"
            "<placement><availabilityZone>us-east-1a</availabilityZone></placement>"
            "%s<networkInterfaceSet><item><ipv6AddressesSet/></item>"
            "</networkInterfaceSet></item>" % (
                instance_id, AMI, state, instance_type, ip, tag_set(tags)))


class Response:
    def __init__(self, status, body):
        self.status = status
        self.body = body

    def read(self):
        return self.body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FakeAws:
    def __init__(self, instances=(), key_pairs=(), groups=(), image_owner=next_aws.DEBIAN_OWNER,
                 image_arch="x86_64"):
        self.instances = list(instances)
        self.key_pairs = list(key_pairs)
        self.groups = list(groups)
        self.image_owner = image_owner
        self.image_arch = image_arch
        self.calls = []

    def actions(self):
        return [c[1] for c in self.calls]

    def mutations(self):
        return [c for c in self.calls if not c[1].startswith(("Describe", "Get",
                                                              "AmazonSSM.Get"))]

    def __call__(self, request, timeout=None):
        host = urllib.parse.urlsplit(request.full_url).netloc
        headers = {k.lower(): v for k, v in request.header_items()}
        assert headers["authorization"].startswith("AWS4-HMAC-SHA256 Credential=" + ACCESS)
        assert SECRET not in json.dumps(headers)
        if "x-amz-target" in headers:
            target = headers["x-amz-target"]
            payload = json.loads(request.data)
            self.calls.append((host, target, payload))
            assert target == "AmazonSSM.GetParameter"
            assert payload == {"Name": "/aws/service/debian/release/13/latest/amd64"}
            return Response(200, json.dumps({"Parameter": {"Value": AMI}}).encode())
        fields = dict(urllib.parse.parse_qsl(request.data.decode()))
        action = fields.pop("Action")
        fields.pop("Version")
        self.calls.append((host, action, fields))
        handler = getattr(self, "do_" + action, None)
        if handler is None:
            raise AssertionError("unexpected AWS action %s" % action)
        return handler(fields)

    def do_GetCallerIdentity(self, fields):
        return Response(200, xml("GetCallerIdentity",
                                 "<GetCallerIdentityResult><Arn>arn:aws:iam::123456789012:"
                                 "user/fleet</Arn><UserId>AIDA1</UserId><Account>123456789012"
                                 "</Account></GetCallerIdentityResult>",
                                 "https://sts.amazonaws.com/doc/2011-06-15/"))

    def do_DescribeInstances(self, fields):
        wanted = fields.get("InstanceId.1")
        items = [i for i in self.instances if not wanted or ("<instanceId>%s<" % wanted) in i]
        return Response(200, xml("DescribeInstances",
                                 "<reservationSet><item><instancesSet>%s</instancesSet>"
                                 "</item></reservationSet>" % "".join(items)))

    def do_TerminateInstances(self, fields):
        return Response(200, xml("TerminateInstances",
                                 "<instancesSet><item><instanceId>%s</instanceId></item>"
                                 "</instancesSet>" % fields["InstanceId.1"]))

    def do_DescribeImages(self, fields):
        assert fields == {"ImageId.1": AMI, "Owner.1": next_aws.DEBIAN_OWNER}
        return Response(200, xml("DescribeImages",
                                 "<imagesSet><item><imageId>%s</imageId><imageState>available"
                                 "</imageState><imageOwnerId>%s</imageOwnerId><architecture>%s"
                                 "</architecture><rootDeviceName>/dev/xvda</rootDeviceName>"
                                 "<name>debian-13-amd64</name></item></imagesSet>" % (
                                     AMI, self.image_owner, self.image_arch)))

    def do_DescribeKeyPairs(self, fields):
        return Response(200, xml("DescribeKeyPairs", "<keySet>%s</keySet>" % "".join(
            "<item><keyName>fmnext-fleet</keyName><publicKey>%s</publicKey></item>" % k
            for k in self.key_pairs)))

    def do_ImportKeyPair(self, fields):
        self.key_pairs.append(base64.b64decode(fields["PublicKeyMaterial"]).decode())
        return Response(200, xml("ImportKeyPair", "<keyName>fmnext-fleet</keyName>"))

    def do_DescribeSecurityGroups(self, fields):
        return Response(200, xml("DescribeSecurityGroups", "<securityGroupInfo>%s"
                                 "</securityGroupInfo>" % "".join(self.groups)))

    def do_CreateSecurityGroup(self, fields):
        return Response(200, xml("CreateSecurityGroup",
                                 "<return>true</return><groupId>sg-0aaaabbbbccccdddd</groupId>"))

    def do_AuthorizeSecurityGroupIngress(self, fields):
        return Response(200, xml("AuthorizeSecurityGroupIngress", "<return>true</return>"))

    def do_RunInstances(self, fields):
        name = fields["TagSpecification.1.Tag.2.Value"]
        return Response(200, xml("RunInstances", "<instancesSet>%s</instancesSet>" % (
            instance_xml("i-0123456789abcdef9", name, ip="", state="pending"))))


def aws_with(fake, region="us-east-1"):
    credentials = next_aws.Credentials(ACCESS, SECRET)
    client = next_aws.AwsClient(credentials, region, opener=fake, clock=lambda: 1.7e9)
    return next_aws.Aws(client)


def group_xml(name="fmnext-core", tags=None, permissions=""):
    tags = {"forkmesh-next": "1"} if tags is None else tags
    return ("<item><groupId>sg-0123456789abcdef0</groupId><groupName>%s</groupName>"
            "%s<ipPermissions>%s</ipPermissions></item>" % (name, tag_set(tags), permissions))


def permission_xml(proto, low, high, cidr):
    return ("<item><ipProtocol>%s</ipProtocol><fromPort>%d</fromPort><toPort>%d</toPort>"
            "<ipRanges><item><cidrIp>%s</cidrIp></item></ipRanges></item>"
            % (proto, low, high, cidr))


def fleet_key(tmp_path, monkeypatch):
    key = tmp_path / "fleet_key"
    key.write_text("private")
    Path(str(key) + ".pub").write_text(PUBKEY + "\n")
    monkeypatch.setenv("FMNEXT_SSH_KEY", str(key))


# --------------------------------------------------------------------------
# requests and guards


def test_query_posts_a_signed_form_to_the_regional_endpoint():
    fake = FakeAws()
    aws = aws_with(fake, "eu-west-1")
    assert next_aws.caller_identity(aws.client) == {
        "account": "123456789012", "arn": "arn:aws:iam::123456789012:user/fleet",
        "user_id": "AIDA1"}
    assert fake.calls == [("sts.eu-west-1.amazonaws.com", "GetCallerIdentity", {})]


def test_http_errors_carry_the_aws_code_but_no_secret():
    def opener(request, timeout=None):
        raise urllib.error.HTTPError(
            request.full_url, 403, "Forbidden", {}, io.BytesIO(
                b"<Response><Errors><Error><Code>UnauthorizedOperation</Code><Message>"
                b"You are not authorized</Message></Error></Errors></Response>"))
    aws = aws_with(opener)
    with pytest.raises(ToolError) as error:
        aws.fleet_instances()
    assert "UnauthorizedOperation" in str(error.value) and SECRET not in str(error.value)


def test_fleet_instances_keeps_only_tagged_fmnext_labels():
    fake = FakeAws(instances=[
        instance_xml("i-0000000000000000a", "fmnext-core-5"),
        instance_xml("i-0000000000000000b", "fmnext-core-6",
                     tags={"Name": "fmnext-core-6", "forkmesh-next": "true"}),
        instance_xml("i-0000000000000000c", "mirror35",
                     tags={"Name": "mirror35", "forkmesh-next": "1"}),
        instance_xml("i-0000000000000000d", "fmnext-run-3",
                     tags={"Name": "fmnext-run-3"}),
    ])
    aws = aws_with(fake)
    found = aws.fleet_instances()
    assert [i["label"] for i in found] == ["fmnext-core-5"]
    first = found[0]
    assert first["provider"] == "aws" and first["main_ip"] == "198.51.100.40"
    assert first["tags"] == ["forkmesh-next"] and first["plan"] == "t3a.micro"
    filters = fake.calls[0][2]
    assert filters["Filter.1.Name"] == "tag:forkmesh-next" and filters["Filter.1.Value.1"] == "1"
    assert "terminated" not in filters.values()
    assert aws.find_by_label("fmnext-core-5")["id"] == "i-0000000000000000a"
    assert aws.find_by_label("fmnext-run-3") is None


@pytest.mark.parametrize("tags", [
    {"Name": "fmnext-core-5"},
    {"Name": "fmnext-core-5", "forkmesh-next": "0"},
    {"Name": "fmnext-core-5", "forkmesh-next": ""},
    {"forkmesh-next": "1"},
    {"Name": "web-1", "forkmesh-next": "1"},
])
def test_destroy_refuses_an_untagged_or_foreign_instance(tags):
    fake = FakeAws(instances=[instance_xml("i-0000000000000000a", "fmnext-core-5", tags=tags)])
    aws = aws_with(fake)
    with pytest.raises(GuardError):
        aws.delete_instance("i-0000000000000000a")
    assert fake.mutations() == []


def test_destroy_terminates_a_guarded_instance_after_a_live_read():
    fake = FakeAws(instances=[instance_xml("i-0000000000000000a", "fmnext-core-5")])
    aws = aws_with(fake)
    aws.delete_instance("i-0000000000000000a")
    assert fake.actions() == ["DescribeInstances", "TerminateInstances"]
    with pytest.raises(GuardError):
        aws.delete_instance("i-0000000000000000a; rm")


def test_guard_refuses_other_regions_and_vultr_records():
    record = next_aws.normalize_instance(
        {"instanceId": "i-0000000000000000a", "tagSet": [
            {"key": "Name", "value": "fmnext-core-5"}, {"key": "forkmesh-next", "value": "1"}]},
        "us-east-1")
    assert next_aws.guard_aws_instance(record, "us-east-1") is record
    with pytest.raises(GuardError):
        next_aws.guard_aws_instance(record, "eu-west-1")
    with pytest.raises(GuardError):
        next_aws.guard_aws_instance(instance("fmnext-core-5"))


def test_run_instances_params_are_hardened():
    params = next_aws.run_instances_params("fmnext-core-5", AMI, "t3a.micro",
                                           "sg-0123456789abcdef0", "/dev/xvda",
                                           client_token="fmnext-core-5-x")
    assert params["MetadataOptions.HttpTokens"] == "required"
    assert params["MetadataOptions.HttpPutResponseHopLimit"] == 1
    assert params["BlockDeviceMapping.1.Ebs.Encrypted"] == "true"
    assert params["BlockDeviceMapping.1.Ebs.VolumeType"] == "gp3"
    assert params["CreditSpecification.CpuCredits"] == "standard"
    assert (params["MinCount"], params["MaxCount"]) == (1, 1)
    assert params["DisableApiTermination"] == "false"
    assert params["KeyName"] == "fmnext-fleet"
    for index, kind in ((1, "instance"), (2, "volume"), (3, "network-interface")):
        prefix = "TagSpecification.%d." % index
        assert params[prefix + "ResourceType"] == kind
        assert (params[prefix + "Tag.1.Key"], params[prefix + "Tag.1.Value"]) == (
            "forkmesh-next", "1")
        assert (params[prefix + "Tag.2.Key"], params[prefix + "Tag.2.Value"]) == (
            "Name", "fmnext-core-5")
    user_data = base64.b64decode(params["UserData"]).decode()
    assert "disable_root: false" in user_data and "users: []" in user_data
    protected = next_aws.run_instances_params("fmnext-core-5", AMI, "t3a.micro",
                                              "sg-0123456789abcdef0", "/dev/xvda",
                                              termination_protection=True)
    assert protected["DisableApiTermination"] == "true"
    with pytest.raises(GuardError):
        next_aws.run_instances_params("mirror35", AMI, "t3a.micro",
                                      "sg-0123456789abcdef0", "/dev/xvda")
    with pytest.raises(GuardError):
        next_aws.run_instances_params("fmnext-core-5", "ami-x; y", "t3a.micro",
                                      "sg-0123456789abcdef0", "/dev/xvda")


@pytest.mark.parametrize("change", [
    {"TagSpecification.1.Tag.1.Value": "0"},
    {"TagSpecification.1.Tag.2.Value": "mirror35"},
    {"MaxCount": 2},
    {"MetadataOptions.HttpTokens": "optional"},
    {"BlockDeviceMapping.1.Ebs.Encrypted": "false"},
])
def test_create_instance_refuses_an_unguarded_request(change):
    fake = FakeAws()
    aws = aws_with(fake)
    params = next_aws.run_instances_params("fmnext-core-5", AMI, "t3a.micro",
                                           "sg-0123456789abcdef0", "/dev/xvda")
    params.update(change)
    with pytest.raises(GuardError):
        aws.create_instance(params)
    assert fake.calls == []


def test_cheapest_type_follows_the_fleet_arch_and_ram_floor():
    assert next_aws.FLEET_ARCH == "x86_64"
    assert next_aws.cheapest_type() == "t3a.micro"
    assert next_aws.cheapest_type(min_ram=512) == "t3a.nano"
    assert next_aws.cheapest_type("arm64", 512) == "t4g.nano"
    assert next_aws.plan_monthly("t3a.micro") == round(0.0094 * 730 + 3.65 + 1.6, 2)
    assert {p["type"] for p in next_aws.plans()} == {"x86_64"}


def test_ingress_rules_mirror_the_rendered_nftables(tmp_path):
    import render
    fleet = render.load_fleet(EXAMPLE, state_path=tmp_path / "none.json")
    rules = next_aws.ingress_rules("core", fleet)
    wg_port = int(fleet["wireguard"]["port"])
    nft = render.render_core(fleet, "n1")[0]["/etc/nftables.conf"]
    nft = nft[0] if isinstance(nft, tuple) else nft
    udp = []
    for line in nft.splitlines():
        line = line.strip()
        if line.startswith("tcp dport {"):
            tcp = {int(p) for p in line.split("{")[1].split("}")[0].split(",")}
        if line.startswith("udp dport {"):
            udp = line.split("{")[1].split("}")[0].replace(" ", "").split(",")
    assert {(r[1]) for r in rules if r[0] == "tcp"} == tcp | {22}
    singles = {int(p) for p in udp if "-" not in p}
    ranges = {tuple(int(x) for x in p.split("-")) for p in udp if "-" in p}
    assert {r[1] for r in rules if r[0] == "udp" and r[1] == r[2]} == singles
    assert {(r[1], r[2]) for r in rules if r[0] == "udp" and r[1] != r[2]} == ranges
    assert wg_port in singles
    assert next_aws.ingress_rules("runner") == {("tcp", 22, 22, "0.0.0.0/0"),
                                                ("tcp", 822, 822, "0.0.0.0/0")}
    assert ("tcp", 822, 822, "203.0.113.0/24") in next_aws.ingress_rules(
        "runner", ssh_sources=["203.0.113.7/24"])
    with pytest.raises(ToolError):
        next_aws.ingress_rules("runner", ssh_sources=["::/0"])


def test_snapshot_builders_are_vultr_only():
    with pytest.raises(GuardError):
        next_aws.role_for_label("fmnext-snap-1")
    assert next_aws.role_for_label("fmnext-auto-2") == "core"
    assert next_aws.role_for_label("fmnext-burst-2") == "runner"


# --------------------------------------------------------------------------
# create


def test_create_dry_run_only_reads(tmp_path, sandbox, monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws()
    runner = Runner(execute=False, out=io.StringIO())
    assert next_fleet.create("core", 5, aws_with(fake), runner) is None
    assert fake.mutations() == []
    assert fake.actions() == ["DescribeInstances", "AmazonSSM.GetParameter",
                              "DescribeImages", "DescribeKeyPairs",
                              "DescribeSecurityGroups"]
    assert any("fmnext-core-5" in s and "t3a.micro" in s and AMI in s
               for s in runner.skipped)
    assert not (sandbox / "fleet-instances.json").exists()


def test_create_executes_key_group_and_instance_then_records_aws(tmp_path, sandbox,
                                                                 monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws()
    created = next_fleet.create("run", 4, aws_with(fake), Runner(execute=True,
                                                                 out=io.StringIO()))
    assert created["id"] == "i-0123456789abcdef9" and created["label"] == "fmnext-run-4"
    assert [c[1] for c in fake.mutations()] == [
        "ImportKeyPair", "CreateSecurityGroup", "AuthorizeSecurityGroupIngress",
        "RunInstances"]
    imported = fake.mutations()[0][2]
    assert base64.b64decode(imported["PublicKeyMaterial"]).decode() == " ".join(
        PUBKEY.split()[:2])
    group = fake.mutations()[1][2]
    assert group["GroupName"] == "fmnext-runner"
    assert group["TagSpecification.1.Tag.1.Key"] == "forkmesh-next"
    ingress = fake.mutations()[2][2]
    assert {ingress["IpPermissions.1.FromPort"],
            ingress["IpPermissions.2.FromPort"]} == {"22", "822"}
    assert "IpPermissions.3.IpProtocol" not in ingress
    run = fake.mutations()[3][2]
    assert run["NetworkInterface.1.SecurityGroupId.1"] == "sg-0aaaabbbbccccdddd"
    assert run["MetadataOptions.HttpTokens"] == "required"
    recorded = json.loads((sandbox / "fleet-instances.json").read_text())
    assert recorded["fmnext-run-4"]["provider"] == "aws"
    assert recorded["fmnext-run-4"]["id"] == "i-0123456789abcdef9"
    assert recorded["fmnext-run-4"]["region"] == "us-east-1"


def test_create_records_an_instance_whose_launch_answer_has_no_tags(tmp_path, sandbox,
                                                                     monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws()
    fake.do_RunInstances = lambda fields: Response(200, xml("RunInstances", (
        "<instancesSet><item><instanceId>i-0123456789abcdef9</instanceId>"
        "<instanceState><name>pending</name></instanceState>"
        "<instanceType>t3a.micro</instanceType></item></instancesSet>")))
    created = next_fleet.create("core", 6, aws_with(fake), Runner(execute=True,
                                                                  out=io.StringIO()))
    assert created["label"] == "fmnext-core-6" and created["id"] == "i-0123456789abcdef9"
    recorded = json.loads((sandbox / "fleet-instances.json").read_text())
    assert recorded["fmnext-core-6"]["provider"] == "aws"


def test_create_adopts_an_existing_tagged_instance(tmp_path, sandbox, monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws(instances=[instance_xml("i-0000000000000000a", "fmnext-core-5")])
    adopted = next_fleet.create("core", 5, aws_with(fake), Runner(execute=True,
                                                                  out=io.StringIO()))
    assert adopted["id"] == "i-0000000000000000a" and fake.mutations() == []
    recorded = json.loads((sandbox / "fleet-instances.json").read_text())
    assert recorded["fmnext-core-5"] == {
        "id": "i-0000000000000000a", "ip": "198.51.100.40", "plan": "t3a.micro",
        "provider": "aws", "region": "us-east-1"}


def test_create_refuses_a_foreign_key_pair_and_untagged_group(tmp_path, monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws(key_pairs=["ssh-ed25519 AAAAsomeoneelse x"])
    with pytest.raises(GuardError, match="different key"):
        next_fleet.create("core", 5, aws_with(fake), Runner(execute=True, out=io.StringIO()))
    fake = FakeAws(key_pairs=[PUBKEY], groups=[group_xml(tags={})])
    with pytest.raises(GuardError, match="not tagged"):
        next_fleet.create("core", 5, aws_with(fake), Runner(execute=True, out=io.StringIO()))
    assert fake.mutations() == []


def test_reused_group_with_extra_rules_is_refused_and_missing_rules_added(tmp_path,
                                                                          monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    wide = permission_xml("tcp", 0, 65535, "0.0.0.0/0")
    fake = FakeAws(key_pairs=[PUBKEY], groups=[group_xml("fmnext-runner", permissions=wide)])
    with pytest.raises(GuardError, match="allows tcp 0-65535"):
        next_fleet.create("run", 4, aws_with(fake), Runner(execute=True, out=io.StringIO()))
    assert fake.mutations() == []
    fake = FakeAws(key_pairs=[PUBKEY], groups=[group_xml("fmnext-runner")])
    next_fleet.create("run", 4, aws_with(fake), Runner(execute=True, out=io.StringIO()))
    assert [c[1] for c in fake.mutations()] == ["AuthorizeSecurityGroupIngress",
                                                "RunInstances"]
    assert fake.mutations()[0][2]["GroupId"] == "sg-0123456789abcdef0"


def test_create_refuses_an_image_not_owned_by_debian(tmp_path, monkeypatch):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws(image_owner="999999999999")
    with pytest.raises(GuardError, match="Debian"):
        next_fleet.create("core", 5, aws_with(fake), Runner(execute=True, out=io.StringIO()))
    fake = FakeAws(image_arch="arm64")
    with pytest.raises(GuardError):
        next_fleet.create("core", 5, aws_with(fake), Runner(execute=True, out=io.StringIO()))
    assert fake.mutations() == []


def test_create_command_plans_offline_without_credentials(tmp_path, sandbox, monkeypatch,
                                                          capsys):
    fleet_key(tmp_path, monkeypatch)
    assert next_fleet.main(["create", "n9", "--provider", "aws", "--region",
                            "eu-west-1"]) == 0
    out = capsys.readouterr().out
    assert "planning offline (no AWS calls)" in out
    assert "create AWS instance fmnext-core-9 (t3a.micro, eu-west-1" in out
    assert "tcp 22 from 0.0.0.0/0" in out and "udp 51820 from 0.0.0.0/0" in out
    assert "(dry run: nothing was changed" in out
    assert not (sandbox / "fleet-instances.json").exists()
    assert next_fleet.main(["create", "n9", "--provider", "aws", "--execute"]) == 1


def test_create_command_uses_the_injected_aws_client(tmp_path, sandbox, monkeypatch,
                                                     capsys):
    fleet_key(tmp_path, monkeypatch)
    fake = FakeAws()
    assert next_fleet.main(["create", "r4", "--provider", "aws"], vultr=aws_with(fake)) == 0
    assert fake.mutations() == []
    assert next_fleet.main(["create", "r4", "--provider", "aws", "--execute"],
                           vultr=aws_with(fake)) == 0
    assert [c[1] for c in fake.mutations()][-1] == "RunInstances"


# --------------------------------------------------------------------------
# fleet-instances.json provider field


def seed_instances(sandbox, data):
    (sandbox / "fleet-instances.json").write_text(json.dumps(data))


def test_missing_provider_means_vultr(sandbox):
    seed_instances(sandbox, {"fmnext-core-1": {"id": "x", "ip": "198.51.100.1"},
                             "fmnext-core-5": {"id": "i-0000000000000000a",
                                               "ip": "198.51.100.40", "provider": "aws",
                                               "region": "us-east-1"}})
    assert next_cloud.recorded_provider("fmnext-core-1") == "vultr"
    assert next_cloud.recorded_provider("fmnext-core-5") == "aws"
    assert next_cloud.recorded_provider("fmnext-core-7") == "vultr"
    with pytest.raises(ToolError):
        next_cloud.entry_provider({"provider": "gcp"})


def test_vultr_records_now_carry_their_provider(sandbox):
    next_fleet.record_instance(instance("fmnext-core-2", instance_id="a" * 8 + "-0000-0000-0000-"
                                        + "0" * 12))
    recorded = json.loads((sandbox / "fleet-instances.json").read_text())
    assert recorded["fmnext-core-2"]["provider"] == "vultr"


def test_verify_target_instance_dispatches_by_provider(sandbox, monkeypatch):
    seed_instances(sandbox, {"fmnext-core-5": {"id": "i-0000000000000000a",
                                               "ip": "198.51.100.40", "provider": "aws",
                                               "region": "us-east-1"}})
    fake = FakeAws(instances=[instance_xml("i-0000000000000000a", "fmnext-core-5")])
    monkeypatch.setattr(next_cloud, "from_config",
                        lambda name, region=None: aws_with(fake, region))
    vultr = next_fleet.Vultr(FakeHttp({}))
    found = next_fleet.verify_target_instance(vultr, "n5", "198.51.100.40")
    assert found["provider"] == "aws"
    with pytest.raises(GuardError, match="on AWS"):
        next_fleet.verify_target_instance(vultr, "n5", "198.51.100.41")
    fake.instances = [instance_xml("i-0000000000000000a", "fmnext-core-5",
                                   tags={"Name": "fmnext-core-5"})]
    with pytest.raises(GuardError, match="no fmnext-core-5 instance tagged"):
        next_fleet.verify_target_instance(vultr, "n5", "198.51.100.40")


def test_destroy_dispatches_to_aws_and_keeps_the_guards(tmp_path, sandbox, monkeypatch):
    seed_instances(sandbox, {"fmnext-run-4": {"id": "i-0000000000000000a",
                                              "ip": "198.51.100.40", "provider": "aws",
                                              "region": "us-east-1"}})
    fake = FakeAws(instances=[instance_xml("i-0000000000000000a", "fmnext-run-4")])
    monkeypatch.setattr(next_cloud, "from_config",
                        lambda name, region=None: aws_with(fake, region))
    vultr = next_fleet.Vultr(FakeHttp({}))
    runner = Runner(execute=False, out=io.StringIO())
    next_fleet.destroy({}, "r4", vultr, None, runner, yes=True)
    assert fake.mutations() == []
    assert any(s.startswith("DESTROY AWS instance fmnext-run-4") for s in runner.skipped)
    next_fleet.destroy({}, "r4", vultr, None, Runner(execute=True, out=io.StringIO()),
                       yes=True)
    assert [c[1] for c in fake.mutations()] == ["TerminateInstances"]


def test_enable_ipv6_is_vultr_only(sandbox):
    seed_instances(sandbox, {"fmnext-core-5": {"id": "i-0000000000000000a",
                                               "ip": "198.51.100.40", "provider": "aws"}})
    with pytest.raises(ToolError, match="Vultr-only"):
        next_fleet.enable_ipv6({}, "n5", None, None, Runner())


# --------------------------------------------------------------------------
# add-core


def test_add_core_options_list_aws_plans(tmp_path, sandbox):
    import next_add_core

    class Backend:
        def plan(self, job, provider=None):
            assert provider == "aws"
            return {"name": "n4", "voter": False, "voter_reason": "r", "rev": "abc"}
    job = next_add_core.Job(next_add_core.State(Path(os.devnull), {}),
                            Runner(out=io.StringIO()), out=io.StringIO(), events=False)
    result = next_add_core.options(Backend(), job, provider="aws")
    assert result["provider"] == "aws" and result["providers"] == ["vultr", "aws"]
    assert result["plan"] == "t3a.micro" and result["region"] == "us-east-1"
    assert "us-east-1" in result["regions"]
    assert {p["id"] for p in result["plans"]} >= {"t3a.micro", "t3.micro", "t3a.small"}
    assert "t3a.nano" not in {p["id"] for p in result["plans"]}
    assert "aws.env" in result["plans_error"]
    write_env(sandbox, "AWS_ACCESS_KEY_ID=%s\nAWS_SECRET_ACCESS_KEY=%s\nAWS_REGION=eu-west-2\n"
              % (ACCESS, SECRET))
    result = next_add_core.options(Backend(), job, provider="aws")
    assert result["region"] == "eu-west-2" and "plans_error" not in result


def test_vultr_placement_ignores_aws_cores():
    import next_add_core
    instances = {"fmnext-core-1": {"region": "ewr", "plan": "vc2-1c-1gb"},
                 "fmnext-core-5": {"region": "us-east-1", "plan": "t3a.micro",
                                   "provider": "aws"}}
    assert next_add_core.default_placement(instances) == ("ewr", "vc2-1c-1gb")
    assert next_add_core.aws_placement(instances) == ("us-east-1", "t3a.micro")
