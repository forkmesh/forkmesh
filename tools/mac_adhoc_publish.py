#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import html
import importlib.util
import json
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.parse
import zipfile
from email.message import EmailMessage
from email.utils import formatdate, make_msgid
from pathlib import Path

import forkmesh_paths
import forkmesh_safe_delete

LAYOUT = forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT)
PUBLIC_BASE = "https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256"
INSTALL_PAGE = "https://forkmesh.com/ios-install.html"
DEFAULT_TO = "jett@forkmesh.com"
SENDER = "founders@forkmesh.com"
BOUNCE_SENDER = "no-reply@forkmesh.com"
MAIL_LOG_SOURCE = "the Mac ad hoc build tool"
MAIL_KIND = "ios_build"
MAIL_LOG_BODY = 400
TESTERS_COMMAND = "/usr/local/sbin/fmnext-app-ops early-ios-testers all"
PING_COMMAND = "/usr/local/sbin/fmnext-app-ops ios-build-ping"
RQLITE_EXECUTE = ("curl -fsS --max-time 20 -K /etc/forkmesh-next/rqlite.curlrc "
                  "-H 'Content-Type: application/json' --data-binary @- "
                  "'http://127.0.0.1:4001/db/execute'")


def release_host():
    path = Path(__file__).resolve().with_name("forkmesh-release-host.py")
    spec = importlib.util.spec_from_file_location("forkmesh_release_host", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_finished(runs: Path | None, info: Path) -> bool:
    if runs is None:
        return True
    run = info.parents[2]
    meta = runs / run.parent.name / run.name / "meta.json"
    if not meta.is_file():
        return True
    try:
        return json.loads(meta.read_text()).get("status") == "success"
    except (OSError, ValueError):
        return False


def find_builds(artifacts: Path, runs: Path | None = None) -> list[dict]:
    builds = []
    for info in artifacts.glob("*/*/mac-adhoc/*/build.json"):
        ipas = sorted((info.parent / "ipa").glob("*.ipa"))
        if not ipas or not run_finished(runs, info):
            continue
        meta = json.loads(info.read_text())
        builds.append({"dir": info.parent, "ipa": ipas[0], "commit": meta.get("commit", ""),
                       "subject": meta.get("subject", ""),
                       "builtAt": int(meta.get("builtAt") or info.stat().st_mtime),
                       "buildSeconds": meta.get("buildSeconds"),
                       "waitSeconds": meta.get("waitSeconds")})
    return sorted(builds, key=lambda build: build["builtAt"])


def damaged_entry(ipa: Path) -> str:
    try:
        with zipfile.ZipFile(ipa) as archive:
            return archive.testzip() or ""
    except (zipfile.BadZipFile, OSError, EOFError) as error:
        return str(error) or type(error).__name__


def app_info(ipa: Path) -> dict:
    with zipfile.ZipFile(ipa) as archive:
        names = [name for name in archive.namelist()
                 if name.startswith("Payload/") and name.count("/") == 2
                 and name.endswith(".app/Info.plist")]
        if not names:
            raise SystemExit(f"{ipa} has no Payload/*.app/Info.plist")
        info = plistlib.loads(archive.read(names[0]))
    return {"bundle": info["CFBundleIdentifier"],
            "version": info.get("CFBundleShortVersionString", ""),
            "build": info.get("CFBundleVersion", ""),
            "title": info.get("CFBundleDisplayName") or info.get("CFBundleName") or "App"}


def blob_url(digest: str, filename: str) -> str:
    return f"{PUBLIC_BASE}/{digest}?filename={urllib.parse.quote(filename)}"


def install_page(manifest_digest: str, info: dict) -> str:
    query = urllib.parse.urlencode({"build": info.get("build") or "", "v": manifest_digest[:12]})
    return f"{INSTALL_PAGE}?{query}#{manifest_digest}"


def manifest_bytes(ipa_url: str, info: dict) -> bytes:
    return plistlib.dumps({"items": [{
        "assets": [{"kind": "software-package", "url": ipa_url}],
        "metadata": {"bundle-identifier": info["bundle"], "bundle-version": info["version"],
                     "kind": "software", "title": info["title"]},
    }]})


def stage(cas: Path, digest: str, source: Path) -> None:
    target = cas / "sha256" / digest[:2] / digest / "data"
    if target.is_file():
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def minutes(seconds: int) -> str:
    minute, second = divmod(int(seconds), 60)
    hour, minute = divmod(minute, 60)
    if hour:
        return f"{hour} h {minute:02d} min"
    return f"{minute} min {second:02d} s" if minute else f"{second} s"


def duration_line(build: dict) -> str:
    if build.get("buildSeconds") is None:
        return ""
    line = f"Build time: {minutes(build['buildSeconds'])} on the Mac"
    if (build.get("waitSeconds") or 0) >= 60:
        line += f", after {minutes(build['waitSeconds'])} waiting for another build"
    return line + "."


def build_email(to: str, info: dict, build: dict, page: str) -> EmailMessage:
    label = f"{info['title']} {info['version']} ({info['build']})"
    commit = build["commit"][:12]
    message = EmailMessage()
    message["From"] = f"ForkMesh Builds <{SENDER}>"
    message["To"] = to
    message["Subject"] = f"Ad hoc iPhone build: {label} at {commit}"
    message["Date"] = formatdate(localtime=True)
    message["Message-ID"] = make_msgid(domain="forkmesh.com")
    message["Auto-Submitted"] = "auto-generated"
    message["X-ForkMesh-Kind"] = MAIL_KIND
    message["Feedback-ID"] = f"{MAIL_KIND}:forkmesh"
    took = duration_line(build)
    text = (f"{label} built on the local Mac from {commit}: {build['subject']}\n"
            + (f"{took}\n" if took else "") + "\n"
            + f"Install (open on the registered iPhone):\n{page}\n\n"
            "Ad hoc builds install only on devices registered with the team.\n")
    message.set_content(text)
    message.add_alternative(
        "<p><b>%s</b> built on the local Mac from <code>%s</code>: %s</p>%s"
        "<p><a href=\"%s\" style=\"display:inline-block;padding:10px 18px;background:#18181b;"
        "color:#fff;border-radius:8px;text-decoration:none\">Install on this iPhone</a></p>"
        "<p>The button opens the install page; tap Install there.</p>"
        "<p>Ad hoc builds install only on devices registered with the team.</p>"
        % (html.escape(label), html.escape(commit), html.escape(build["subject"]),
           "<p>%s</p>" % html.escape(took) if took else "",
           html.escape(page)),
        subtype="html")
    return message


def mail_log_note(message: EmailMessage, result: str, error: str = "") -> dict:
    return {"kind": "outside_app", "via": "postfix", "source": MAIL_LOG_SOURCE,
            "result": result, "error": error, "from": SENDER, "to": str(message["To"]),
            "subject": str(message["Subject"]),
            "body": message.get_body(("plain",)).get_content()[:MAIL_LOG_BODY],
            "messageId": str(message["Message-ID"])}


def record_mail_log_note(host: tuple[str, str, list[str]], note: dict) -> None:
    name, ip, options = host
    statement = [["INSERT INTO mail_log_node_notes(created_at, node, data) VALUES(?, ?, ?)",
                  int(time.time() * 1000), name.removeprefix("fmnext-")[:64],
                  json.dumps(note, sort_keys=True)[:4000]]]
    try:
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", *options, f"root@{ip}",
             RQLITE_EXECUTE],
            input=json.dumps(statement).encode(), capture_output=True, timeout=60, check=False)
    except (OSError, subprocess.SubprocessError) as error:
        print(f"#email-log note not recorded: {type(error).__name__}")
        return
    if result.returncode != 0 or b'"error"' in result.stdout:
        print(f"#email-log note not recorded: {(result.stderr or result.stdout).decode(errors='replace')[-200:]}")


def send(host: tuple[str, str, list[str]], message: EmailMessage) -> None:
    _, ip, options = host
    try:
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", *options, f"root@{ip}",
             f"/usr/sbin/sendmail -t -f {BOUNCE_SENDER}"],
            input=message.as_bytes(), capture_output=True, timeout=120, check=False)
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f"sendmail on {ip} failed: {type(error).__name__}")
    failed = result.returncode != 0
    record_mail_log_note(host, mail_log_note(
        message, "failed" if failed else "accepted",
        f"sendmail exit {result.returncode}" if failed else ""))
    if failed:
        raise SystemExit(f"sendmail on {ip} failed: {result.stderr.decode(errors='replace')[-300:]}")


def on_any_host(hosts: list[tuple[str, str, list[str]]], call):
    failure: SystemExit | None = None
    for host in hosts:
        try:
            return call(host)
        except SystemExit as error:
            failure = error
            print(f"{host[0]}: {error}; trying the next core")
    raise failure or SystemExit("no seed hosts")


def parse_testers(output: str) -> dict:
    for line in reversed(output.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                break
    raise SystemExit(f"early iOS tester lookup gave no JSON: {output[-300:]!r}")


def tester_addresses(result: dict) -> list[str]:
    if not result.get("ok"):
        raise SystemExit(f"early iOS tester lookup failed: {result.get('error') or result}")
    addresses = []
    for tester in result.get("testers") or []:
        address = str(tester.get("email") or "").strip().lower()
        if "@" in address and not any(ch.isspace() or ch in ",;<>" for ch in address) \
                and address not in addresses:
            addresses.append(address)
    for skipped in result.get("skipped") or []:
        print(f"Early iOS tester {skipped.get('account')} skipped: {skipped.get('reason')}")
    return addresses


def early_ios_testers(host: tuple[str, str, list[str]]) -> list[str]:
    _, ip, options = host
    try:
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", *options, f"root@{ip}",
             TESTERS_COMMAND],
            capture_output=True, timeout=120, check=False)
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f"early iOS tester lookup on {ip} failed: {type(error).__name__}")
    return tester_addresses(parse_testers(result.stdout.decode(errors="replace")))


def ping_spec(manifest_digest: str, info: dict, commit: str) -> str:
    def token(value) -> str:
        return re.sub(r"[^0-9A-Za-z.\-]", "", str(value or ""))[:32] or "unknown"
    commit = str(commit or "").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{0,40}", commit):
        commit = ""
    return f"{manifest_digest}:{token(info.get('version'))}:{token(info.get('build'))}:{commit}"


def ping_testers(host: tuple[str, str, list[str]], spec: str) -> list[str] | None:
    _, ip, options = host
    try:
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", *options, f"root@{ip}",
             f"{PING_COMMAND} {shlex.quote(spec)}"],
            capture_output=True, timeout=120, check=False)
    except (OSError, subprocess.SubprocessError) as error:
        print(f"Tester ping on {ip} failed: {type(error).__name__}")
        return None
    output = result.stdout.decode(errors="replace")
    for line in reversed(output.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            reply = json.loads(line)
        except ValueError:
            break
        if not reply.get("ok"):
            break
        for skipped in reply.get("skipped") or []:
            print(f"Early iOS tester {skipped.get('account')} not pinged: {skipped.get('reason')}")
        return [str(name) for name in reply.get("pinged") or []]
    print(f"Tester ping on {ip} failed: {(output or result.stderr.decode(errors='replace'))[-300:]!r}")
    return None


def sent_complete(entry: dict) -> bool:
    return entry.get("complete", True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Publish the newest Mac ad hoc IPA from the Actions artifacts and email "
                    "its install link (docs/native/MAC_BUILD.md).")
    parser.add_argument("--execute", action="store_true", help="publish and send (default: dry run)")
    parser.add_argument("--to", action="append",
                        help="email these addresses instead of the early iOS testers "
                             "flagged in the admin (repeatable)")
    parser.add_argument("--artifacts", type=Path, default=LAYOUT.data_path("actions") / "artifacts")
    parser.add_argument("--runs", type=Path, default=LAYOUT.data_path("actions") / "runs")
    parser.add_argument("--state", type=Path, default=LAYOUT.state / "mac-adhoc")
    parser.add_argument("--seed-hosts-file", type=Path, default=LAYOUT.release_seed_hosts)
    parser.add_argument("--seed-ssh-key", type=Path, default=LAYOUT.fleet_ssh_key)
    parser.add_argument("--seed-known-hosts", type=Path, default=LAYOUT.fleet_config / "known_hosts")
    parser.add_argument("--remote-repository",
                        default="/var/lib/forkmesh/repositories/forkmesh-forkmesh.git")
    parser.add_argument("--ipa", type=Path, help="publish this IPA instead of the newest artifact")
    args = parser.parse_args(argv)

    published_file = args.state / "published.json"
    published = json.loads(published_file.read_text()) if published_file.is_file() else {}

    if args.ipa:
        builds = [{"dir": args.ipa.parent, "ipa": args.ipa, "commit": "", "subject": "manual",
                   "builtAt": int(time.time())}]
    else:
        builds = find_builds(args.artifacts, args.runs)
    if not builds:
        print("No Mac ad hoc builds in the Actions artifacts.")
        return 0
    build = builds[-1]
    if not args.ipa and any(entry.get("ipa") == str(build["ipa"]) and sent_complete(entry)
                            for entry in published.values()):
        print(f"Newest build {build['ipa'].name} was already sent.")
        return 0
    digest = sha256_file(build["ipa"])
    if digest in published and sent_complete(published[digest]) and not args.ipa:
        print(f"Newest build {build['ipa'].name} ({digest[:12]}) was already sent.")
        return 0

    damaged = damaged_entry(build["ipa"])
    if damaged:
        print(f"Not publishing {build['ipa']}: {damaged} fails its zip check, so iOS would "
              "refuse it as unverifiable. Build it again.", file=sys.stderr)
        return 1
    info = app_info(build["ipa"])
    ipa_name = f"{info['title']}-{info['version']}-{info['build']}-adhoc.ipa".replace(" ", "")
    ipa_url = blob_url(digest, ipa_name)
    manifest = manifest_bytes(ipa_url, info)
    manifest_digest = hashlib.sha256(manifest).hexdigest()
    manifest_url = blob_url(manifest_digest, "manifest.plist")
    install = "itms-services://?action=download-manifest&url=" + urllib.parse.quote(manifest_url, safe="")
    page = install_page(manifest_digest, info)
    print(f"Build:   {build['ipa']}")
    print(f"App:     {info['bundle']} {info['version']} ({info['build']})")
    print(f"IPA:     {ipa_url}")
    print(f"Page:    {page}")
    print(f"Install: {install}")
    if not args.execute:
        print("Dry run: would seed both files to the cores and email "
              + (", ".join(args.to) if args.to
                 else "and ping the early iOS testers flagged in the admin")
              + ".")
        return 0

    module = release_host()
    hosts = module.seed_targets(
        module.seed_hosts(args.seed_hosts_file.read_text()
                          if args.seed_hosts_file.is_file() else ""),
        args.seed_ssh_key, args.seed_known_hosts)
    if not hosts:
        raise SystemExit(f"no seed hosts in {args.seed_hosts_file}")
    recipients = list(dict.fromkeys(address.strip().lower() for address in args.to)) \
        if args.to else on_any_host(hosts, early_ios_testers)
    if not recipients:
        print(f"No early iOS testers with a verified email; sending to {DEFAULT_TO}.")
        recipients = [DEFAULT_TO]
    cas = args.state / "cas"
    manifest_path = args.state / "manifest.plist"
    args.state.mkdir(parents=True, exist_ok=True)
    manifest_path.write_bytes(manifest)
    stage(cas, digest, build["ipa"])
    stage(cas, manifest_digest, manifest_path)
    module.seed_mirrors(cas, [digest, manifest_digest], hosts, args.remote_repository)
    previous = published.get(digest, {})
    sent = [] if args.ipa or sent_complete(previous) else list(previous.get("to") or [])
    pinged: list[str] = []

    def record(complete: bool) -> None:
        published[digest] = {"ipa": str(build["ipa"]), "commit": build["commit"], "page": page,
                             "to": sent, "pinged": pinged, "complete": complete,
                             "sentAt": int(time.time())}
        published_file.write_text(json.dumps(published, indent=2))

    for address in recipients:
        if address in sent:
            continue
        message = build_email(address, info, build, page)
        on_any_host(hosts, lambda host, m=message: send(host, m))
        sent.append(address)
        record(False)
    if not args.to:
        spec = ping_spec(manifest_digest, info, build["commit"])
        pinged = next((result for result in (ping_testers(host, spec) for host in hosts)
                       if result is not None), None) or []
    record(True)
    forkmesh_safe_delete.remove_tree(cas / "sha256" / digest[:2] / digest, cas, ignore_errors=True)
    print(f"Seeded to {len(hosts)} cores and emailed {len(recipients)}: {', '.join(recipients)}.")
    if not args.to:
        print(f"Pinged {len(pinged)} early iOS testers"
              + (f": {', '.join(pinged)}." if pinged else "."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
