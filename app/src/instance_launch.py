import asyncio
import base64
import binascii
import hashlib
import ipaddress
import json
import re

import instance_directory
import instance_theme
import runner_leases as leases
import runner_ssh_setup as ssh_setup

ID_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,37}[a-z0-9])?$")
PROJECT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._&+-]{0,39}$")
DOMAIN_RE = re.compile(
    r"^(?=.{4,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$")
EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]{1,64}@"
    r"(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$")
API_KEY_RE = re.compile(r"^[A-Za-z0-9._:+/=-]{8,400}$")
FINGERPRINT_RE = re.compile(r"^SHA256:[A-Za-z0-9+/]{43}$")
MAIL_PROVIDERS = ("none", "mailtrap", "sendgrid", "mailgun")
RESERVED_NAMES = frozenset({
    "admin", "api", "app", "assets", "dashboard", "login", "signup", "start",
    "world", "www", "root", "forkmesh", "forkmesh-instance",
})
MIN_ADMIN_PASSWORD = 10
MAX_OWNERS = 20
MAX_SECRET = 1024
MAX_LOGO_BYTES = 200 * 1024
MAX_BODY_BYTES = 512 * 1024
LOGO_RE = re.compile(
    r"^data:image/(png|jpeg|webp|svg\+xml);base64,([A-Za-z0-9+/]+={0,2})$")
LOGO_UNSAFE_RE = re.compile(
    rb"<script|<foreignobject|<iframe|<embed|<object|\son[a-z]+\s*=|"
    rb"javascript:|<!entity", re.IGNORECASE)
MAX_LAUNCHES_PER_HOUR = 4
MAX_RUNNING = 5
HOUR_MS = 60 * 60 * 1000
KEEP_MS = 7 * 24 * 60 * 60 * 1000
RUN_STALE_MS = 2 * 60 * 1000
FLUSH_MS = 700
HEARTBEAT_MS = 5000
LOG_LINES = 400
LOG_CHARS = 48000
SETUP_UNIT = "forkmesh-instance-setup"
LOST_MESSAGE = (
    "ForkMesh lost the connection to your server. The setup keeps running "
    "there: start again with the same details to follow it from where it "
    "is, or run journalctl -u %s -f on the server." % SETUP_UNIT)
DEFAULT_INSTALL_URL = "https://forkmesh.com/install-instance.sh"
DEFAULT_SOURCE_URL = "https://app.forkmesh.com/forkmesh/forkmesh"
SOURCE_URL_RE = re.compile(r"^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/[A-Za-z0-9._/-]+$")
SOURCE_REF_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,120}$")
PROGRESS_PREFIX = "FMI "
STEPS = (
    ("connect", "Connect to your server"),
    ("system", "Check the server"),
    ("packages", "Install system packages"),
    ("source", "Download the ForkMesh source"),
    ("build", "Build the app"),
    ("configure", "Save your settings"),
    ("database", "Create the database"),
    ("services", "Start the services"),
    ("https", "Set up HTTPS"),
    ("admin", "Create your admin account"),
    ("repo", "Create your forkmesh repository"),
    ("done", "Open your instance"),
)
STEP_IDS = tuple(step[0] for step in STEPS)
STEP_STATES = ("pending", "running", "done", "failed", "skipped")


def match(action):
    parts = [part for part in str(action or "").split("/") if part]
    if parts == ["launch"]:
        return ("launch",)
    if len(parts) == 2 and parts[0] == "launches":
        return ("status", parts[1])
    if parts == ["instances"]:
        return ("instances",)
    if len(parts) == 3 and parts[0] == "instances" and parts[2] == "logo":
        return ("logo", parts[1])
    return None


def _error(runtime, code, status, message="", field=""):
    body = {"ok": False, "error": code}
    if message:
        body["message"] = message
    if field:
        body["field"] = field
    return runtime.response(body, status)


def sslip_domain(address):
    address = ipaddress.ip_address(str(address))
    if address.version == 4:
        return str(address).replace(".", "-") + ".sslip.io"
    return address.exploded.replace(":", "-") + ".sslip.io"


def _text(data, key, limit=400):
    value = data.get(key)
    return value.strip()[:limit] if isinstance(value, str) else ""


def _secret(data, key):
    value = data.get(key)
    return value if isinstance(value, str) else ""


def logo_kind(raw):
    if raw.startswith(b"\x89PNG\r\n\x1a\n"):
        return "png"
    if raw.startswith(b"\xff\xd8\xff"):
        return "jpeg"
    if raw[:4] == b"RIFF" and raw[8:12] == b"WEBP":
        return "webp"
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return ""
    if re.search(r"<svg[\s>]", text[:4096]) and not LOGO_UNSAFE_RE.search(raw):
        return "svg+xml"
    return ""


def parse_logo(value):
    if not isinstance(value, str) or not value.strip():
        return "", None
    match = LOGO_RE.fullmatch(value.strip())
    if not match or len(match.group(2)) > (MAX_LOGO_BYTES + 2) // 3 * 4:
        return "", ("invalid_logo", "The logo must be a PNG, JPEG, WebP or "
                    "SVG image of up to 200 KB.", "logo")
    try:
        raw = base64.b64decode(match.group(2), validate=True)
    except (binascii.Error, ValueError):
        raw = b""
    kind = logo_kind(raw) if len(raw) <= MAX_LOGO_BYTES else ""
    if not kind:
        return "", ("invalid_logo", "The logo must be a PNG, JPEG, WebP or "
                    "SVG image of up to 200 KB, without scripts.", "logo")
    return "data:image/%s;base64,%s" % (
        kind, base64.b64encode(raw).decode("ascii")), None


def parse_owners(value, owner_type, admin, admin_email, org):
    if value in (None, "", []):
        return [], None
    if not isinstance(value, list) or len(value) > MAX_OWNERS:
        return [], ("invalid_owners", "List up to %d other owners, each with "
                    "a username, email and password." % MAX_OWNERS, "owners")
    if owner_type != "org":
        return [], ("owners_need_org", "Other owners share an organization: "
                    "choose An organization, or remove them.", "ownerType")
    owners, names, emails = [], {admin, org}, {admin_email}
    for item in value:
        if not isinstance(item, dict):
            return [], ("invalid_owners", "Each other owner needs a username, "
                        "email and password.", "owners")
        name = _text(item, "username", 80).lower()
        if not NAME_RE.fullmatch(name) or name in RESERVED_NAMES:
            return [], ("invalid_owner_username", "Owner usernames use "
                        "lowercase letters, numbers and dashes and start with "
                        "a letter.", "owners")
        if name in names:
            return [], ("duplicate_owner", "@%s is listed twice, or is the "
                        "admin or the organization. Give each owner a "
                        "different username." % name, "owners")
        mail = _text(item, "email", 254).lower()
        if not EMAIL_RE.fullmatch(mail) or mail in emails:
            return [], ("invalid_owner_email", "Give @%s a valid email "
                        "address of their own." % name, "owners")
        password = _secret(item, "password")
        if not MIN_ADMIN_PASSWORD <= len(password) <= 256:
            return [], ("weak_owner_password", "The password for @%s needs at "
                        "least %d characters." % (name, MIN_ADMIN_PASSWORD),
                        "owners")
        names.add(name)
        emails.add(mail)
        owners.append({"username": name, "email": mail, "password": password})
    return owners, None


def validate(data):
    project = " ".join(_text(data, "projectName", 80).split())
    if not PROJECT_RE.fullmatch(project):
        return None, ("invalid_project_name", "Give your project a name: "
                      "letters, numbers and spaces, up to 40 characters.",
                      "projectName")
    host = _text(data, "host", 260).lower().rstrip(".")
    if not host or not (ssh_setup.HOST_RE.fullmatch(host)
                        or ssh_setup.public_address(host)):
        return None, ("invalid_host", "Enter your server's public IP address "
                      "or host name.", "host")
    try:
        port = int(data.get("port") or 22)
    except (TypeError, ValueError):
        port = 0
    if not 0 < port < 65536:
        return None, ("invalid_port", "The SSH port is a number from 1 to "
                      "65535.", "port")
    user = _text(data, "sshUser", 40) or "root"
    if not ssh_setup.USER_RE.fullmatch(user):
        return None, ("invalid_ssh_user", "Enter the SSH user name, for "
                      "example root.", "sshUser")
    password = _secret(data, "sshPassword")
    private_key = _secret(data, "sshPrivateKey").replace("\r\n", "\n")
    passphrase = _secret(data, "sshPassphrase")
    if bool(password) == bool(private_key):
        return None, ("invalid_credentials", "Enter the server's SSH password "
                      "or upload its private key (one of the two).",
                      "sshPassword")
    if len(password) > MAX_SECRET or len(passphrase) > MAX_SECRET or (
            private_key and not ssh_setup.PRIVATE_KEY_RE.fullmatch(
                private_key)):
        return None, ("invalid_credentials", "That password or private key "
                      "is not in a form ForkMesh accepts. Keys look like "
                      "-----BEGIN OPENSSH PRIVATE KEY-----.", "sshPrivateKey")  # forkmesh-secret-scan:ignore-line
    fingerprint = _text(data, "hostKeyFingerprint", 80)
    if fingerprint and not FINGERPRINT_RE.fullmatch(fingerprint):
        return None, ("invalid_fingerprint", "A host key fingerprint looks "
                      "like SHA256: followed by 43 characters.",
                      "hostKeyFingerprint")
    domain = _text(data, "domain", 260).lower().rstrip(".")
    for prefix in ("https://", "http://"):
        if domain.startswith(prefix):
            domain = domain[len(prefix):].split("/", 1)[0]
    if domain and (not DOMAIN_RE.fullmatch(domain)
                   or domain.endswith("forkmesh.com")):
        return None, ("invalid_domain", "Enter a domain like "
                      "code.example.com, or leave it empty.", "domain")
    admin = _text(data, "adminUsername", 80).lower()
    if not NAME_RE.fullmatch(admin) or admin in RESERVED_NAMES:
        return None, ("invalid_admin_username", "Usernames use lowercase "
                      "letters, numbers and dashes and start with a letter.",
                      "adminUsername")
    email = _text(data, "adminEmail", 254).lower()
    if not EMAIL_RE.fullmatch(email):
        return None, ("invalid_admin_email", "Enter a valid email address "
                      "for the admin.", "adminEmail")
    admin_password = _secret(data, "adminPassword")
    if not MIN_ADMIN_PASSWORD <= len(admin_password) <= 256:
        return None, ("weak_admin_password", "The admin password needs at "
                      "least %d characters." % MIN_ADMIN_PASSWORD,
                      "adminPassword")
    owner_type = _text(data, "ownerType", 10).lower() or "org"
    if owner_type not in ("org", "user"):
        return None, ("invalid_owner_type", "Choose an organization or a "
                      "user account to own the repository.", "ownerType")
    owner = _text(data, "ownerName", 80).lower() if owner_type == "org" else admin
    if owner_type == "org" and (not NAME_RE.fullmatch(owner)
                                or owner in RESERVED_NAMES):
        return None, ("invalid_owner_name", "Organization names use lowercase "
                      "letters, numbers and dashes.", "ownerName")
    if owner_type == "org" and owner == admin:
        return None, ("owner_is_admin", "Pick an organization name that is "
                      "different from the admin username, or choose User.",
                      "ownerName")
    owners, problem = parse_owners(data.get("owners"), owner_type, admin,
                                   email, owner)
    if problem:
        return None, problem
    logo, problem = parse_logo(data.get("logo"))
    if problem:
        return None, problem
    theme = _text(data, "theme", 41).lower()
    if theme and theme not in instance_theme.PRESETS:
        return None, ("invalid_theme", "Choose one of the colour themes "
                      "listed, or the default.", "theme")
    provider = _text(data, "mailProvider", 20).lower() or "none"
    if provider not in MAIL_PROVIDERS:
        return None, ("invalid_mail_provider", "Choose Mailtrap, SendGrid, "
                      "Mailgun or no email.", "mailProvider")
    mail_key = _secret(data, "mailApiKey").strip()
    mail_from = _text(data, "mailFrom", 254).lower()
    mail_domain = _text(data, "mailgunDomain", 253).lower().rstrip(".")
    mail_region = _text(data, "mailgunRegion", 4).lower() or "us"
    if provider != "none":
        if not API_KEY_RE.fullmatch(mail_key):
            return None, ("invalid_mail_key", "Paste the API key from your "
                          "email provider.", "mailApiKey")
        if mail_from and not EMAIL_RE.fullmatch(mail_from):
            return None, ("invalid_mail_from", "The sender must be an email "
                          "address, like no-reply@example.com.", "mailFrom")
        if provider == "mailgun" and not DOMAIN_RE.fullmatch(mail_domain):
            return None, ("invalid_mailgun_domain", "Mailgun needs the "
                          "sending domain you verified there, like "
                          "mg.example.com.", "mailgunDomain")
        if mail_region not in ("us", "eu"):
            return None, ("invalid_mailgun_region", "Mailgun's region is US "
                          "or EU.", "mailgunRegion")
    else:
        mail_key = mail_from = mail_domain = ""
    return {
        "projectName": project,
        "host": host,
        "port": port,
        "sshUser": user,
        "sshPassword": password,
        "sshPrivateKey": private_key,
        "sshPassphrase": passphrase,
        "hostKeyFingerprint": fingerprint,
        "domain": domain,
        "adminUsername": admin,
        "adminEmail": email,
        "adminPassword": admin_password,
        "ownerType": owner_type,
        "ownerName": owner,
        "owners": owners,
        "mailProvider": provider,
        "mailApiKey": mail_key,
        "mailFrom": mail_from,
        "mailgunDomain": mail_domain if provider == "mailgun" else "",
        "mailgunRegion": mail_region if provider == "mailgun" else "",
        "logo": logo,
        "theme": theme,
        "ownWorld": instance_directory.flag(data, "ownWorld"),
        "listOnHomepage": instance_directory.flag(data, "listOnHomepage"),
    }, None


def initial_steps():
    return [{"id": step_id, "title": title, "state": "pending"}
            for step_id, title in STEPS]


def apply_progress(steps, event):
    step_id = str(event.get("step") or "")
    state = str(event.get("state") or "")
    if step_id not in STEP_IDS or state not in STEP_STATES:
        return False
    for step in steps:
        if step["id"] == step_id:
            step["state"] = state
            title = event.get("title")
            if isinstance(title, str) and title.strip():
                step["title"] = ssh_setup.scrub(title)[:80]
        elif state == "running" and step["state"] == "running":
            step["state"] = "done"
    return True


def parse_progress(line):
    if not line.startswith(PROGRESS_PREFIX):
        return None
    try:
        event = json.loads(line[len(PROGRESS_PREFIX):])
    except ValueError:
        return None
    return event if isinstance(event, dict) else None


def append_log(log, line):
    lines = (log.split("\n") if log else []) + [line[:500]]
    return "\n".join(lines[-LOG_LINES:])[-LOG_CHARS:]


def instance_url(domain, owner):
    return "https://%s/%s/forkmesh" % (domain, owner)


def safe_redirect(url, domain):
    prefix = "https://%s/" % domain
    if isinstance(url, str) and url.startswith(prefix) and len(url) < 400 \
            and re.fullmatch(r"https://[a-z0-9.-]+/[A-Za-z0-9._/-]*", url):
        return url
    return ""


async def handle(runtime, route):
    kind = route[0]
    method = runtime.method()
    want = "POST" if kind == "launch" else "GET"
    if method != want:
        return _error(runtime, "method_not_allowed", 405)
    if kind == "instances":
        return await instance_directory.instances(runtime)
    if kind == "logo":
        found = await instance_directory.logo(runtime, route[1])
        return found if found is not None else _error(
            runtime, "not_found", 404)
    if kind == "status":
        launch_id = route[1]
        if not ID_RE.fullmatch(launch_id):
            return _error(runtime, "not_found", 404)
        return await _status(runtime, launch_id)
    raw = await runtime.body_bytes(MAX_BODY_BYTES)
    if raw is None:
        return _error(runtime, "request_too_large", 413)
    data = leases._json(raw)
    raw = b""
    if data is None:
        return _error(runtime, "invalid_json", 400)
    try:
        return await _launch(runtime, data)
    finally:
        data.clear()


async def _launch(runtime, data):
    helper = ssh_setup._helper(runtime)
    if helper is None:
        return _error(runtime, "launch_unavailable", 503,
                      "Instance setup is not available on this server right "
                      "now. Try again in a few minutes.")
    form, problem = validate(data)
    if problem:
        return _error(runtime, problem[0], 400, problem[1], problem[2])
    now = runtime.now()
    await runtime.d1_run(
        "DELETE FROM instance_launches WHERE updated_at<?", now - KEEP_MS)
    client = await runtime.client_key()
    recent = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM instance_launches WHERE client_key=? AND "
        "created_at>=?", client, now - HOUR_MS)
    if int((recent or {}).get("n") or 0) >= MAX_LAUNCHES_PER_HOUR:
        return _error(runtime, "rate_limited", 429, "That is a lot of launches "
                      "in one hour. Try again a little later.")
    running = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM instance_launches WHERE state='running' "
        "AND updated_at>=?", now - RUN_STALE_MS)
    if int((running or {}).get("n") or 0) >= MAX_RUNNING:
        return _error(runtime, "busy", 503, "Several instances are being set "
                      "up right now. Try again in a few minutes.")
    addresses = await runtime.resolve_host(form["host"])
    checked = [ssh_setup.public_address(item) for item in addresses or []]
    if not checked or any(item is None for item in checked):
        return _error(runtime, "address_not_public", 400,
                      "That server does not have a public address ForkMesh "
                      "can reach. Use its public IP.", "host")
    address = str(sorted(checked, key=lambda item: item.version)[0])
    keys, scan_error = [], ""
    async for message in helper({"op": "keyscan", "address": address,
                                 "port": form["port"]}):
        if message.get("error"):
            scan_error = str(message["error"])[:200]
        for item in message.get("keys") or []:
            key_type = str((item or {}).get("type") or "")
            blob = str((item or {}).get("key") or "")
            print_ = ssh_setup.fingerprint(blob)
            if key_type in ssh_setup.KEY_TYPES and \
                    ssh_setup.KEY_BLOB_RE.fullmatch(blob) and print_:
                keys.append({"type": key_type, "key": blob,
                             "fingerprint": print_})
    if not keys:
        return _error(runtime, "ssh_unreachable", 502,
                      "No SSH server answered at %s port %d. Check the IP, "
                      "the port and the server's firewall. %s" % (
                          address, form["port"], ssh_setup.scrub(scan_error)),
                      "host")
    keys.sort(key=lambda item: ssh_setup.KEY_TYPES.index(item["type"]))
    if form["hostKeyFingerprint"]:
        key = next((item for item in keys if item["fingerprint"]
                    == form["hostKeyFingerprint"]), None)
        if key is None:
            return _error(runtime, "fingerprint_mismatch", 400,
                          "The server answered with a different host key "
                          "than the fingerprint you entered.",
                          "hostKeyFingerprint")
    else:
        key = keys[0]
    domain = form["domain"] or sslip_domain(address)
    owner = form["ownerName"]
    actor = ""
    try:
        actor = await leases._account(runtime, {})
    except Exception:
        actor = ""
    launch_id = runtime.new_id()
    steps = initial_steps()
    steps[0]["state"] = "running"
    await runtime.d1_run(
        "INSERT INTO instance_launches (id,client_key,account,project,domain,"
        "owner,owner_type,admin,address,port,host_key,state,steps,message,log,"
        "url,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,'running',?,"
        "?,'','',?,?)", launch_id, client, actor, form["projectName"], domain,
        owner, form["ownerType"], form["adminUsername"], address, form["port"],
        json.dumps({"type": key["type"], "fingerprint": key["fingerprint"]}),
        json.dumps(steps), "Connecting to %s" % address, now, now)
    config = {
        "launchId": launch_id,
        "projectName": form["projectName"],
        "domain": domain,
        "domainProvided": bool(form["domain"]),
        "publicAddress": address,
        "adminUsername": form["adminUsername"],
        "adminEmail": form["adminEmail"],
        "adminPassword": form["adminPassword"],
        "ownerType": form["ownerType"],
        "ownerName": owner,
        "owners": form["owners"],
        "mailProvider": form["mailProvider"],
        "mailApiKey": form["mailApiKey"],
        "mailFrom": form["mailFrom"] or (
            "no-reply@" + domain if form["mailProvider"] != "none" else ""),
        "mailgunDomain": form["mailgunDomain"],
        "mailgunRegion": form["mailgunRegion"],
        "logo": form["logo"],
        "theme": form["theme"],
        "sourceUrl": runtime.instance_source_url(),
        "sourceRef": runtime.instance_source_ref(),
        "plugins": [],
    }
    helper_request = {
        "op": "instance",
        "address": address,
        "port": form["port"],
        "hostKey": {"type": key["type"], "key": key["key"]},
        "user": form["sshUser"],
        "password": form["sshPassword"],
        "privateKey": form["sshPrivateKey"],
        "passphrase": form["sshPassphrase"],
        "installUrl": runtime.instance_install_url(),
        "config": config,
    }
    secrets = [form["sshPassword"], form["sshPassphrase"],
               form["adminPassword"], form["mailApiKey"]] + [
                   item["password"] for item in form["owners"]]
    entry = instance_directory.listing(form)
    form.clear()
    await runtime.audit(actor or "anonymous", "instance.launch.start", domain)
    events = helper(helper_request)
    runtime.spawn(run_launch(runtime, launch_id, domain, owner, events,
                             secrets, entry))
    return runtime.response({
        "ok": True,
        "id": launch_id,
        "state": "running",
        "domain": domain,
        "address": address,
        "hostKey": {"type": key["type"], "fingerprint": key["fingerprint"]},
        "steps": steps,
        "url": instance_url(domain, owner),
    }, 202)


async def _pump(events, queue):
    try:
        async for event in events:
            await queue.put(event)
    except Exception as exc:
        await queue.put({"lost": type(exc).__name__})
    finally:
        await queue.put(None)


async def run_launch(runtime, launch_id, domain, owner, events, secrets,
                     entry=None):
    log = ""
    steps = initial_steps()
    steps[0]["state"] = "running"
    state = "failed"
    message = ""
    url = ""
    last = 0
    dirty = False
    progressed = False
    finished = False
    queue = asyncio.Queue()
    reader = asyncio.ensure_future(_pump(events, queue))
    try:
        while True:
            try:
                event = await asyncio.wait_for(queue.get(), FLUSH_MS / 1000)
            except asyncio.TimeoutError:
                event = {}
            if event is None:
                break
            if "out" in event:
                line = ssh_setup.scrub(event.get("out"), secrets).rstrip()
                progress = parse_progress(line)
                if progress is not None:
                    progressed = True
                    if progress.get("done"):
                        url = safe_redirect(progress.get("url"), domain) or \
                            instance_url(domain, owner)
                    elif apply_progress(steps, progress):
                        if progress.get("state") == "running":
                            message = steps[STEP_IDS.index(
                                progress["step"])]["title"]
                    dirty = True
                elif line:
                    if steps[0]["state"] == "running" and line.startswith(
                            "Connected to "):
                        apply_progress(steps, {"step": "system",
                                               "state": "running"})
                        message = steps[1]["title"]
                    log = append_log(log, line)
                    dirty = True
            if "exit" in event:
                finished = True
                code = event.get("exit")
                if code == 0 and url:
                    state = "succeeded"
                    message = "Your instance is ready."
                    for step in steps:
                        if step["state"] in ("running", "pending"):
                            step["state"] = "done"
                elif progressed and code in (255, -1):
                    message = LOST_MESSAGE
                else:
                    message = ssh_setup.scrub(event.get("error") or (
                        "The installer stopped with status %s. The log above "
                        "shows the last thing it was doing." % code),
                        secrets)[:300]
                break
            if "lost" in event:
                break
            now = runtime.now()
            if (dirty and now - last >= FLUSH_MS) or \
                    now - last >= HEARTBEAT_MS:
                last = now
                dirty = False
                await runtime.d1_run(
                    "UPDATE instance_launches SET log=?,steps=?,message=?,"
                    "updated_at=? WHERE id=? AND state='running'", log,
                    json.dumps(steps), message, now, launch_id)
    except Exception as exc:
        finished = True
        message = ssh_setup.scrub("The setup failed: %s" % type(exc).__name__)
    finally:
        secrets = None
        reader.cancel()
    if not finished:
        message = LOST_MESSAGE if progressed else \
            "The setup stopped without an answer."
    if state != "succeeded":
        for step in steps:
            if step["state"] == "running":
                step["state"] = "failed"
    await runtime.d1_run(
        "UPDATE instance_launches SET state=?,message=?,log=?,steps=?,url=?,"
        "updated_at=? WHERE id=? AND state='running'", state, message, log,
        json.dumps(steps), url, runtime.now(), launch_id)
    if state == "succeeded" and entry is not None:
        try:
            await instance_directory.publish(runtime, launch_id, domain, entry)
        except Exception:
            pass


async def _status(runtime, launch_id):
    row = await runtime.d1_first(
        "SELECT * FROM instance_launches WHERE id=?", launch_id)
    if not row:
        return _error(runtime, "not_found", 404)
    now = runtime.now()
    if row.get("state") == "running" and int(
            row.get("updated_at") or 0) < now - RUN_STALE_MS:
        await runtime.d1_run(
            "UPDATE instance_launches SET state='failed',updated_at=?,"
            "message=? WHERE id=? AND state='running'", now, LOST_MESSAGE,
            launch_id)
        row = await runtime.d1_first(
            "SELECT * FROM instance_launches WHERE id=?", launch_id)
    try:
        steps = json.loads(row.get("steps") or "[]")
    except ValueError:
        steps = []
    try:
        host_key = json.loads(row.get("host_key") or "{}")
    except ValueError:
        host_key = {}
    return runtime.response({
        "ok": True,
        "launch": {
            "id": row["id"],
            "state": row.get("state") or "",
            "projectName": row.get("project") or "",
            "domain": row.get("domain") or "",
            "address": row.get("address") or "",
            "port": int(row.get("port") or 22),
            "owner": row.get("owner") or "",
            "ownerType": row.get("owner_type") or "",
            "admin": row.get("admin") or "",
            "hostKey": host_key if isinstance(host_key, dict) else {},
            "steps": steps if isinstance(steps, list) else [],
            "message": row.get("message") or "",
            "log": row.get("log") or "",
            "url": row.get("url") or "",
            "createdAt": int(row.get("created_at") or 0),
            "updatedAt": int(row.get("updated_at") or 0),
        },
    })


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class LaunchRuntime(leases.WorkerRuntime):
    def public_response(self, data, max_age):
        return json_response(  # noqa: F821
            data, status=200, cache_control="public, max-age=%d" % max_age)

    def image_response(self, raw, media_type, max_age):
        return JsResponse.new(Uint8Array.new(_to_js(bytes(raw))), to_js({  # noqa: F821
            "status": 200,
            "headers": {
                "content-type": media_type,
                "cache-control": "public, max-age=%d" % max_age,
                "x-content-type-options": "nosniff",
                "content-security-policy": "default-src 'none'; "
                "style-src 'unsafe-inline'; sandbox",
                "access-control-allow-origin": "*",
            },
        }))

    async def client_key(self):
        address = _transient_client_address(self.request)  # noqa: F821
        seed = str(getattr(self.env, "ADMIN_PASS", "") or "instance-launch")
        return hashlib.sha256(
            ("instance-launch\n%s\n%s" % (seed, address)).encode("utf-8")
        ).hexdigest()

    def instance_install_url(self):
        return str(getattr(self.env, "INSTANCE_INSTALL_URL", "")
                   or DEFAULT_INSTALL_URL).strip()

    def instance_source_url(self):
        value = str(getattr(self.env, "INSTANCE_SOURCE_URL", "")
                    or DEFAULT_SOURCE_URL).strip()
        return value if SOURCE_URL_RE.fullmatch(value) else DEFAULT_SOURCE_URL

    def instance_source_ref(self):
        value = str(getattr(self.env, "INSTANCE_SOURCE_REF", "")
                    or "main").strip()
        return value if SOURCE_REF_RE.fullmatch(value) else "main"


async def route(env, request, path):
    action = str(path or "")[len("/api/start/"):]
    found = match(action)
    runtime = LaunchRuntime(env, request)
    if found is None:
        return _error(runtime, "not_found", 404)
    await runtime.ensure_schema()
    return await handle(runtime, found)

