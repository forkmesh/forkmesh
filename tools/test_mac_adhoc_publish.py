import json

import pytest

import mac_adhoc_publish

INFO = {"title": "ForkMesh", "version": "1.4.0", "build": "18", "bundle": "com.forkmesh.forkmesh"}


def email(build):
    message = mac_adhoc_publish.build_email("to@example.com", INFO, build, "PAGE")
    return (message.get_body(("plain",)).get_content(), message.get_body(("html",)).get_content())


def test_email_links_only_the_install_page():
    message = mac_adhoc_publish.build_email(
        "to@example.com", INFO, {"commit": "c", "subject": "s"},
        "https://forkmesh.com/ios-install.html?build=18&v=abc#abc")
    text = message.get_body(("plain",)).get_content()
    page = message.get_body(("html",)).get_content()
    for body in (text, page):
        assert "https://forkmesh.com/ios-install.html?build=18" in body
        assert "itms-services" not in body
        assert ".ipa" not in body
    assert message["Auto-Submitted"] == "auto-generated"
    assert message["Feedback-ID"] == "ios_build:forkmesh"


def test_email_says_how_long_the_build_took():
    text, page = email({"commit": "c3f94cc775e67190", "subject": "s", "buildSeconds": 1137,
                        "waitSeconds": 5})
    assert "Build time: 18 min 57 s on the Mac." in text
    assert "Build time: 18 min 57 s on the Mac." in page
    assert "waiting" not in text


def test_email_mentions_the_wait_for_the_mac():
    text, _ = email({"commit": "c", "subject": "s", "buildSeconds": 3725, "waitSeconds": 245})
    assert "Build time: 1 h 02 min on the Mac, after 4 min 05 s waiting for another build." in text


def test_build_without_timings_has_no_build_time_line():
    text, page = email({"commit": "c", "subject": "s"})
    assert "Build time" not in text and "Build time" not in page


def test_find_builds_reads_the_timings(tmp_path):
    folder = tmp_path / "repo" / "7" / "mac-adhoc" / "c3f94cc775e6"
    (folder / "ipa").mkdir(parents=True)
    (folder / "ipa" / "ForkMesh.ipa").write_bytes(b"")
    (folder / "build.json").write_text(json.dumps(
        {"commit": "c3f94cc775e67190", "subject": "s", "builtAt": 10, "buildSeconds": 600,
         "waitSeconds": 0}))
    [build] = mac_adhoc_publish.find_builds(tmp_path)
    assert build["buildSeconds"] == 600 and build["waitSeconds"] == 0


def landed_build(tmp_path, run="7"):
    folder = tmp_path / "artifacts" / "repo" / run / "mac-adhoc" / "c3f94cc775e6"
    (folder / "ipa").mkdir(parents=True)
    (folder / "ipa" / "ForkMesh.ipa").write_bytes(b"")
    (folder / "build.json").write_text(json.dumps({"commit": "c", "subject": "s", "builtAt": 10}))
    return folder


def run_meta(tmp_path, status, run="7"):
    meta = tmp_path / "runs" / "repo" / run / "meta.json"
    meta.parent.mkdir(parents=True)
    meta.write_text(json.dumps({"id": int(run), "status": status}))


def test_find_builds_waits_for_the_run_to_finish_landing(tmp_path):
    landed_build(tmp_path)
    run_meta(tmp_path, "running")
    assert mac_adhoc_publish.find_builds(tmp_path / "artifacts", tmp_path / "runs") == []


def test_find_builds_takes_a_finished_run(tmp_path):
    landed_build(tmp_path)
    run_meta(tmp_path, "success")
    [build] = mac_adhoc_publish.find_builds(tmp_path / "artifacts", tmp_path / "runs")
    assert build["commit"] == "c"


def test_find_builds_takes_an_artifact_with_no_run_record(tmp_path):
    landed_build(tmp_path, run="manual-1")
    (tmp_path / "runs").mkdir()
    assert len(mac_adhoc_publish.find_builds(tmp_path / "artifacts", tmp_path / "runs")) == 1


def test_an_ipa_already_sent_is_not_hashed_again(tmp_path, monkeypatch):
    folder = landed_build(tmp_path)
    state = tmp_path / "state"
    state.mkdir()
    (state / "published.json").write_text(json.dumps(
        {"abc": {"ipa": str(folder / "ipa" / "ForkMesh.ipa")}}))
    monkeypatch.setattr(mac_adhoc_publish, "sha256_file", lambda path: pytest.fail("hashed"))
    assert mac_adhoc_publish.main(["--artifacts", str(tmp_path / "artifacts"),
                                   "--runs", str(tmp_path / "runs"), "--state", str(state)]) == 0


def test_the_email_leaves_a_mail_log_note_with_its_body(monkeypatch):
    calls = []

    class Done:
        returncode = 0
        stdout = b'{"results":[{"rows_affected":1}]}'
        stderr = b""

    def run(argv, input=None, **_kwargs):
        calls.append((argv[-1], input))
        return Done()

    monkeypatch.setattr(mac_adhoc_publish.subprocess, "run", run)
    monkeypatch.setattr(mac_adhoc_publish.time, "time", lambda: 1790664203.0)
    message = mac_adhoc_publish.build_email(
        "jett@forkmesh.com", INFO, {"commit": "c3f94cc775e67190", "subject": "s"}, "PAGE")
    mac_adhoc_publish.send(("fmnext-n1", "192.0.2.1", []), message)
    assert calls[0][0].startswith("/usr/sbin/sendmail ")
    assert calls[1][0] == mac_adhoc_publish.RQLITE_EXECUTE
    [[sql, at, node, data]] = json.loads(calls[1][1])
    assert sql.startswith("INSERT INTO mail_log_node_notes")
    assert (at, node) == (1790664203000, "n1")
    note = json.loads(data)
    assert note["kind"] == "outside_app" and note["via"] == "postfix"
    assert note["result"] == "accepted" and note["to"] == "jett@forkmesh.com"
    assert note["subject"] == "Ad hoc iPhone build: ForkMesh 1.4.0 (18) at c3f94cc775e6"
    assert note["body"].startswith("ForkMesh 1.4.0 (18) built on the local Mac")
    assert note["messageId"] == message["Message-ID"]
    assert note["source"] == "the Mac ad hoc build tool"


def ipa_build(tmp_path):
    import plistlib
    import zipfile
    folder = landed_build(tmp_path)
    with zipfile.ZipFile(folder / "ipa" / "ForkMesh.ipa", "w") as archive:
        archive.writestr("Payload/ForkMesh.app/Info.plist", plistlib.dumps({
            "CFBundleIdentifier": "com.forkmesh.forkmesh", "CFBundleShortVersionString": "1.4.0",
            "CFBundleVersion": "21", "CFBundleDisplayName": "ForkMesh"}))
    return folder


class FakeReleaseHost:
    hosts = [("fmnext-n1", "192.0.2.1")]

    def parse_hosts(self, text):
        return list(self.hosts)

    def seed_hosts(self, text):
        return self.parse_hosts(text)

    def seed_targets(self, hosts, *_args):
        return [(name, ip, []) for name, ip in hosts]

    def seed_mirrors(self, *_args):
        pass


def fake_core(monkeypatch, testers, fail_to=(), pings=None):
    sent = []
    pings = pings if pings is not None else []

    class Result:
        def __init__(self, stdout=b"", returncode=0):
            self.stdout, self.returncode, self.stderr = stdout, returncode, b""

    def run(argv, input=None, **_kwargs):
        if argv[-1] == mac_adhoc_publish.TESTERS_COMMAND:
            return Result(b"starting\n" + json.dumps(testers).encode() + b"\n")
        if argv[-1].startswith(mac_adhoc_publish.PING_COMMAND + " "):
            pings.append(argv[-1].split()[-1])
            names = [tester["account"] for tester in testers.get("testers") or []]
            return Result(json.dumps({"ok": True, "pinged": names, "skipped": []}).encode())
        if argv[-1].startswith("/usr/sbin/sendmail"):
            to = next(line for line in input.decode().splitlines() if line.startswith("To: "))[4:]
            if to in fail_to:
                return Result(returncode=75)
            sent.append(to)
        return Result(b'{"results":[{"rows_affected":1}]}')

    monkeypatch.setattr(mac_adhoc_publish.subprocess, "run", run)
    monkeypatch.setattr(mac_adhoc_publish, "release_host", FakeReleaseHost)
    return sent


def publish_args(tmp_path, *extra):
    hosts = tmp_path / "hosts"
    hosts.write_text("fmnext-n1 192.0.2.1\n")
    return ["--execute", "--artifacts", str(tmp_path / "artifacts"), "--runs", str(tmp_path / "runs"),
            "--state", str(tmp_path / "state"), "--seed-hosts-file", str(hosts), *extra]


def test_the_build_is_emailed_to_each_early_ios_tester(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    sent = fake_core(monkeypatch, {"ok": True, "testers": [
        {"account": "ada", "email": "Ada@Example.com"}, {"account": "bob", "email": "bob@example.com"},
        {"account": "dup", "email": "ada@example.com"}],
        "skipped": [{"account": "cy", "reason": "email_not_verified"}]})
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    assert sent == ["ada@example.com", "bob@example.com"]
    [entry] = json.loads((tmp_path / "state" / "published.json").read_text()).values()
    assert entry["to"] == ["ada@example.com", "bob@example.com"] and entry["complete"] is True
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    assert len(sent) == 2


def test_the_testers_are_pinged_once_with_the_install_link(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    pings = []
    fake_core(monkeypatch, {"ok": True, "testers": [
        {"account": "ada", "email": "ada@example.com"}]}, pings=pings)
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    [spec] = pings
    digest, version, build, commit = spec.split(":")
    assert len(digest) == 64 and (version, build, commit) == ("1.4.0", "21", "c")
    [entry] = json.loads((tmp_path / "state" / "published.json").read_text()).values()
    assert entry["pinged"] == ["ada"]
    assert entry["page"] == ("https://forkmesh.com/ios-install.html?build=21&v=%s#%s"
                             % (digest[:12], digest))
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    assert len(pings) == 1


def test_to_sends_no_ping(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    pings = []
    fake_core(monkeypatch, {"ok": True, "testers": []}, pings=pings)
    assert mac_adhoc_publish.main(publish_args(tmp_path, "--to", "x@example.com")) == 0
    assert pings == []


def test_a_failed_ping_still_completes_the_send(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    sent = fake_core(monkeypatch, {"ok": True, "testers": [
        {"account": "ada", "email": "ada@example.com"}]})
    real_run = mac_adhoc_publish.subprocess.run

    def run(argv, input=None, **kwargs):
        if argv[-1].startswith(mac_adhoc_publish.PING_COMMAND):
            raise mac_adhoc_publish.subprocess.TimeoutExpired(argv, 120)
        return real_run(argv, input=input, **kwargs)

    monkeypatch.setattr(mac_adhoc_publish.subprocess, "run", run)
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    [entry] = json.loads((tmp_path / "state" / "published.json").read_text()).values()
    assert sent == ["ada@example.com"] and entry["complete"] is True and entry["pinged"] == []


def test_each_build_gets_its_own_install_page_url():
    first = mac_adhoc_publish.install_page("ab" * 32, INFO)
    second = mac_adhoc_publish.install_page("cd" * 32, dict(INFO, build="19"))
    assert first == "https://forkmesh.com/ios-install.html?build=18&v=abababababab#" + "ab" * 32
    assert first.split("#")[0] != second.split("#")[0]


def test_the_ping_spec_drops_unsafe_characters():
    spec = mac_adhoc_publish.ping_spec("a" * 64, {"version": "1.4 beta;rm", "build": ""},
                                       "C3F94CC775E67190")
    assert spec == "a" * 64 + ":1.4betarm:unknown:c3f94cc775e67190"
    assert mac_adhoc_publish.ping_spec("a" * 64, INFO, "not-a-sha").endswith(":1.4.0:18:")


def test_no_flagged_tester_falls_back_to_the_operator(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    sent = fake_core(monkeypatch, {"ok": True, "testers": [], "skipped": []})
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    assert sent == [mac_adhoc_publish.DEFAULT_TO]


def test_to_overrides_the_tester_lookup(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    sent = fake_core(monkeypatch, {"ok": False, "error": "sealed"})
    assert mac_adhoc_publish.main(publish_args(tmp_path, "--to", "x@example.com")) == 0
    assert sent == ["x@example.com"]


def test_a_failed_lookup_sends_nothing(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    sent = fake_core(monkeypatch, {"ok": False, "error": "sealed"})
    with pytest.raises(SystemExit, match="sealed"):
        mac_adhoc_publish.main(publish_args(tmp_path))
    assert sent == []
    assert not (tmp_path / "state" / "published.json").exists()


def test_a_failed_send_resumes_without_emailing_anyone_twice(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    testers = {"ok": True, "testers": [{"account": "ada", "email": "ada@example.com"},
                                       {"account": "bob", "email": "bob@example.com"}]}
    sent = fake_core(monkeypatch, testers, fail_to=("bob@example.com",))
    with pytest.raises(SystemExit, match="sendmail"):
        mac_adhoc_publish.main(publish_args(tmp_path))
    [entry] = json.loads((tmp_path / "state" / "published.json").read_text()).values()
    assert entry["to"] == ["ada@example.com"] and entry["complete"] is False
    sent_again = fake_core(monkeypatch, testers)
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    assert sent == ["ada@example.com"] and sent_again == ["bob@example.com"]


def test_a_damaged_ipa_is_found(tmp_path):
    import zipfile
    ipa = tmp_path / "a.ipa"
    with zipfile.ZipFile(ipa, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Payload/A.app/A", b"x" * 4096)
    assert mac_adhoc_publish.damaged_entry(ipa) == ""
    data = bytearray(ipa.read_bytes())
    data[60] ^= 0xFF
    ipa.write_bytes(bytes(data))
    assert mac_adhoc_publish.damaged_entry(ipa) == "Payload/A.app/A"


def test_a_dead_first_core_hands_the_lookup_and_mail_to_the_next(tmp_path, monkeypatch):
    ipa_build(tmp_path)
    sent = fake_core(monkeypatch, {"ok": True, "testers": [
        {"account": "ada", "email": "ada@example.com"}]})
    inner = mac_adhoc_publish.subprocess.run
    used = []

    def run(argv, input=None, **kwargs):
        target = next((a for a in argv if str(a).startswith("root@")), "")
        if target == "root@192.0.2.1":
            raise mac_adhoc_publish.subprocess.TimeoutExpired(argv, 15)
        used.append(target)
        return inner(argv, input=input, **kwargs)
    monkeypatch.setattr(mac_adhoc_publish.subprocess, "run", run)

    class TwoCores(FakeReleaseHost):
        hosts = [("fmnext-n1", "192.0.2.1"), ("fmnext-n2", "192.0.2.2")]
    monkeypatch.setattr(mac_adhoc_publish, "release_host", TwoCores)
    assert mac_adhoc_publish.main(publish_args(tmp_path)) == 0
    assert sent == ["ada@example.com"] and set(used) == {"root@192.0.2.2"}
