#!/usr/bin/env python3
import argparse
import getpass
import html as html_module
import json
import os
import re
import smtplib
import ssl
import sys
import time
from email.message import EmailMessage
from email.utils import formataddr, formatdate, make_msgid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402

CONFIG_NAME = "zoho-mail.json"
SENT_LOG_NAME = "zoho-mail-sent.jsonl"
REGIONS = {
    "com": "zoho.com", "eu": "zoho.eu", "in": "zoho.in",
    "com.au": "zoho.com.au", "jp": "zoho.jp", "ca": "zohocloud.ca",
    "sa": "zoho.sa",
}
PORT = 465
TIMEOUT = 30
MAX_RECIPIENTS = 50
MAX_SUBJECT = 998
MAX_BODY = 2 * 1024 * 1024
ADDRESS_RE = re.compile(r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+$")
NAMED_RE = re.compile(r"^\s*(.*?)\s*<([^<>]+)>\s*$")
TAG_RE = re.compile(r"<[^>]+>")
SMTP = smtplib.SMTP_SSL


class ZohoMailError(Exception):
    pass


def smtp_host(region="com", organization=True):
    region = str(region or "com").strip().lower()
    if region not in REGIONS:
        raise ZohoMailError("region must be one of " + ", ".join(REGIONS))
    return ("smtppro." if organization else "smtp.") + REGIONS[region]


def config_path(environ=None):
    environ = os.environ if environ is None else environ
    override = (environ.get("ZOHO_MAIL_CONFIG") or "").strip()
    if override:
        return Path(override)
    return forkmesh_paths.resolve(environ=dict(environ)).config / CONFIG_NAME


def sent_log_path(environ=None):
    environ = os.environ if environ is None else environ
    return forkmesh_paths.resolve(environ=dict(environ)).logs / SENT_LOG_NAME


def load_config(environ=None):
    environ = os.environ if environ is None else environ
    path = config_path(environ)
    stored = {}
    if path.is_file():
        try:
            stored = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ZohoMailError(f"cannot read {path}: {exc}") from exc
        if not isinstance(stored, dict):
            raise ZohoMailError(f"{path} is not a JSON object")
    config = {
        "address": environ.get("ZOHO_MAIL_ADDRESS") or stored.get("address") or "",
        "appPassword": environ.get("ZOHO_MAIL_APP_PASSWORD") or stored.get("appPassword") or "",
        "host": environ.get("ZOHO_MAIL_HOST") or stored.get("host") or smtp_host(),
        "port": int(environ.get("ZOHO_MAIL_PORT") or stored.get("port") or PORT),
        "fromName": environ.get("ZOHO_MAIL_FROM_NAME") or stored.get("fromName") or "",
    }
    config["address"] = config["address"].strip()
    if not config["address"] or not config["appPassword"]:
        raise ZohoMailError(
            "Zoho Mail is not set up: run `python3 tools/zoho_mail.py setup "
            f"--address you@example.com` (writes {path}) or set "
            "ZOHO_MAIL_ADDRESS and ZOHO_MAIL_APP_PASSWORD")
    clean_address(config["address"])
    return config


def save_config(config, environ=None):
    path = config_path(environ)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        json.dump(config, handle, indent=2)
        handle.write("\n")
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)
    return path


def clean_text(value, name, limit):
    text = str(value or "")
    if "\r" in text or "\n" in text:
        raise ZohoMailError(f"{name} must be a single line")
    if len(text) > limit:
        raise ZohoMailError(f"{name} is longer than {limit} characters")
    return text.strip()


def clean_address(value):
    text = clean_text(value, "address", 320)
    match = NAMED_RE.match(text)
    name, address = (match.group(1).strip().strip('"'), match.group(2).strip()) if match else ("", text)
    if not ADDRESS_RE.match(address):
        raise ZohoMailError(f"not an email address: {text!r}")
    return formataddr((name, address)) if name else address, address


def clean_addresses(value):
    if value in (None, ""):
        return []
    items = value if isinstance(value, (list, tuple)) else str(value).replace(";", ",").split(",")
    return [clean_address(item) for item in items if str(item).strip()]


def plain_from_html(markup):
    text = re.sub(r"(?i)<br\s*/?>|</p>|</div>|</li>|</h[1-6]>", "\n", markup)
    return html_module.unescape(TAG_RE.sub("", text)).strip() + "\n"


def build_message(config, to, subject, body, cc=(), reply_to="", html=False):
    sender = formataddr((config["fromName"], config["address"])) if config["fromName"] else config["address"]
    message = EmailMessage()
    message["From"] = sender
    message["To"] = ", ".join(header for header, _ in to)
    if cc:
        message["Cc"] = ", ".join(header for header, _ in cc)
    if reply_to:
        message["Reply-To"] = reply_to[0][0]
    message["Subject"] = subject
    message["Date"] = formatdate(localtime=True)
    message["Message-ID"] = make_msgid(domain=config["address"].rsplit("@", 1)[1])
    if html:
        message.set_content(plain_from_html(body))
        message.add_alternative(body, subtype="html")
    else:
        message.set_content(body if body.endswith("\n") else body + "\n")
    return message


def record_sent(entry, environ=None):
    try:
        path = sent_log_path(environ)
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        with os.fdopen(descriptor, "a", encoding="utf-8") as handle:
            handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass


def open_smtp(config):
    try:
        client = SMTP(config["host"], config["port"], timeout=TIMEOUT,
                      context=ssl.create_default_context())
    except (OSError, smtplib.SMTPException) as exc:
        raise ZohoMailError(f"cannot reach {config['host']}:{config['port']}: {exc}") from exc
    try:
        client.login(config["address"], config["appPassword"])
    except smtplib.SMTPAuthenticationError as exc:
        client.close()
        raise ZohoMailError(
            "Zoho rejected the login for " + config["address"] +
            ": check the app password and the SMTP host (smtppro.* for "
            "organization accounts, smtp.* for personal ones)") from exc
    except (OSError, smtplib.SMTPException) as exc:
        client.close()
        raise ZohoMailError(f"login to {config['host']} failed: {exc}") from exc
    return client


def check(environ=None):
    config = load_config(environ)
    client = open_smtp(config)
    try:
        client.quit()
    except (OSError, smtplib.SMTPException):
        client.close()
    return {"ok": True, "address": config["address"], "host": config["host"]}


def send(to, subject, body, cc=None, bcc=None, reply_to="", html=False, environ=None):
    to_list, cc_list, bcc_list = clean_addresses(to), clean_addresses(cc), clean_addresses(bcc)
    reply_list = clean_addresses(reply_to)
    if not to_list:
        raise ZohoMailError("at least one recipient (to) is required")
    if len(to_list) + len(cc_list) + len(bcc_list) > MAX_RECIPIENTS:
        raise ZohoMailError(f"at most {MAX_RECIPIENTS} recipients per message")
    if len(reply_list) > 1:
        raise ZohoMailError("reply_to takes one address")
    subject = clean_text(subject, "subject", MAX_SUBJECT)
    if not subject:
        raise ZohoMailError("subject is required")
    body = str(body or "")
    if not body.strip():
        raise ZohoMailError("body is required")
    if len(body.encode("utf-8")) > MAX_BODY:
        raise ZohoMailError(f"body is larger than {MAX_BODY} bytes")
    config = load_config(environ)
    message = build_message(config, to_list, subject, body, cc_list, reply_list, bool(html))
    recipients = [address for _, address in to_list + cc_list + bcc_list]
    client = open_smtp(config)
    try:
        refused = client.send_message(message, from_addr=config["address"], to_addrs=recipients)
    except smtplib.SMTPRecipientsRefused as exc:
        raise ZohoMailError("Zoho refused every recipient: " + ", ".join(exc.recipients)) from exc
    except (OSError, smtplib.SMTPException) as exc:
        raise ZohoMailError(f"Zoho did not accept the message: {exc}") from exc
    finally:
        try:
            client.quit()
        except (OSError, smtplib.SMTPException):
            client.close()
    refused = sorted(refused or {})
    result = {
        "sent": True,
        "messageId": message["Message-ID"],
        "from": config["address"],
        "to": [address for _, address in to_list],
        "cc": [address for _, address in cc_list],
        "bcc": [address for _, address in bcc_list],
        "refused": refused,
        "subject": subject,
        "at": int(time.time() * 1000),
    }
    record_sent(result, environ)
    return result


def summary(result):
    line = "Sent to " + ", ".join(result["to"] + result["cc"] + result["bcc"])
    line += f": {result['subject']} ({result['messageId']})"
    if result["refused"]:
        line += "; refused: " + ", ".join(result["refused"])
    return line


def read_body(args):
    if args.body is not None:
        return args.body
    if args.body_file in (None, "-"):
        return sys.stdin.read()
    return Path(args.body_file).read_text(encoding="utf-8")


def command_setup(args):
    address = clean_address(args.address)[1]
    if args.password_stdin:
        password = sys.stdin.readline().strip()
    else:
        password = getpass.getpass(f"Zoho app password for {address}: ").strip()
    if not password:
        raise ZohoMailError("no app password given")
    config = {
        "address": address,
        "appPassword": password,
        "host": args.host or smtp_host(args.region, not args.personal),
        "port": args.port,
        "fromName": clean_text(args.from_name, "from name", 200),
    }
    path = save_config(config)
    print(f"Saved {path}")
    if not args.no_check:
        result = check()
        print(f"Signed in to {result['host']} as {result['address']}")
    return 0


def command_check(args):
    result = check()
    print(json.dumps(result) if args.json else f"Signed in to {result['host']} as {result['address']}")
    return 0


def command_send(args):
    result = send(args.to, args.subject, read_body(args), cc=args.cc, bcc=args.bcc,
                  reply_to=args.reply_to, html=args.html)
    print(json.dumps(result) if args.json else summary(result))
    return 0


def parser():
    root = argparse.ArgumentParser(
        prog="zoho_mail.py",
        description="Send email through a Zoho Mail account over SMTP, with no prompts once set up.")
    commands = root.add_subparsers(dest="command", required=True)

    setup = commands.add_parser("setup", help="save the Zoho address and app password (once)")
    setup.add_argument("--address", required=True, help="the Zoho mailbox to send from")
    setup.add_argument("--region", default="com", choices=sorted(REGIONS),
                       help="Zoho data centre of the account (default com)")
    setup.add_argument("--personal", action="store_true",
                       help="personal @zoho.com account (smtp.*) instead of an organization domain (smtppro.*)")
    setup.add_argument("--host", default="", help="SMTP host, overriding --region and --personal")
    setup.add_argument("--port", type=int, default=PORT)
    setup.add_argument("--from-name", default="", help="display name on the From line")
    setup.add_argument("--password-stdin", action="store_true",
                       help="read the app password from the first line of stdin")
    setup.add_argument("--no-check", action="store_true", help="save without signing in")
    setup.set_defaults(run=command_setup)

    probe = commands.add_parser("check", help="sign in to Zoho without sending anything")
    probe.add_argument("--json", action="store_true")
    probe.set_defaults(run=command_check)

    send_parser = commands.add_parser("send", help="send one email")
    send_parser.add_argument("--to", action="append", required=True,
                             help="recipient; repeat or separate with commas")
    send_parser.add_argument("--cc", action="append", default=[])
    send_parser.add_argument("--bcc", action="append", default=[])
    send_parser.add_argument("--reply-to", default="")
    send_parser.add_argument("--subject", required=True)
    body = send_parser.add_mutually_exclusive_group()
    body.add_argument("--body", help="message text")
    body.add_argument("--body-file", help="read the message from a file, or - for stdin (the default)")
    send_parser.add_argument("--html", action="store_true", help="the body is HTML")
    send_parser.add_argument("--json", action="store_true", help="print the result as JSON")
    send_parser.set_defaults(run=command_send)
    return root


def flatten(values):
    return [part for value in values or [] for part in str(value).replace(";", ",").split(",") if part.strip()]


def main(argv=None):
    args = parser().parse_args(argv)
    for name in ("to", "cc", "bcc"):
        if hasattr(args, name):
            setattr(args, name, flatten(getattr(args, name)))
    try:
        return args.run(args)
    except ZohoMailError as exc:
        print(f"zoho_mail: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
