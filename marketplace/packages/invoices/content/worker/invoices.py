from datetime import datetime, timezone
from html import escape
import hmac
import re


PREFIX = "/api/invoices"
PLUGIN_ID = "invoices"
CRM_PLUGIN_ID = "crm"
REQUEST_PROOF = "forkmesh-invoices-request-v1"
PAGE_PATH = "/dashboard/invoices"
MAX_BODY_BYTES = 256 * 1024
MAX_INVOICES = 10000
MAX_LISTED = 2000
MAX_ITEMS = 100
MAX_EVENTS_PER_INVOICE = 300
MAX_CONTACTS = 5000
MAX_AMOUNT = 10 ** 12
MANAGER_ROLES = frozenset({"owner", "admin"})
STATUSES = ("draft", "sent", "paid", "void")
EDITABLE = frozenset({"draft", "sent"})
DELETABLE = frozenset({"draft", "void"})
EVENT_KINDS = (
    "created", "sent", "reminder", "failed", "opened", "viewed", "clicked",
    "paid", "reopened", "voided", "overdue",
)
TRACKED_KINDS = frozenset({"opened", "viewed", "clicked"})
ACTIVITY_KINDS = ("sent", "reminder", "opened", "viewed", "clicked", "paid", "overdue")
TRACK_ORIGIN = "https://app.forkmesh.com"
TRACK_LOGO = "https://app.forkmesh.com/assets/logo-96.png"
TRACK_QUIET_MS = 30 * 1000
DAY_MS = 24 * 60 * 60 * 1000
ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
TOKEN_RE = re.compile(r"^([0-9a-f]{32})\.([0-9a-f]{24})$")
CURRENCY_RE = re.compile(r"^[A-Z]{3}$")
ORIGIN_RE = re.compile(r"^https://[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?(?::[0-9]{1,5})?$")
HTTPS_RE = re.compile(r"^https://[^\s<>\"']{1,990}$", re.IGNORECASE)
EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$"
)
MAIL_CLIENTS = (
    ("googleimageproxy", "Gmail"),
    ("yahoomailproxy", "Yahoo Mail"),
    ("outlook", "Outlook"),
    ("microsoft office", "Outlook"),
    ("thunderbird", "Thunderbird"),
    ("iphone", "iPhone"),
    ("ipad", "iPad"),
    ("android", "Android"),
    ("macintosh", "Mac"),
    ("windows", "Windows"),
    ("linux", "Linux"),
)
DEFAULT_SETTINGS = {
    "businessName": "",
    "businessAddress": "",
    "fromName": "",
    "fromEmail": "",
    "currency": "USD",
    "dueDays": 30,
    "taxRate": 0,
    "prefix": "INV-",
    "nextNumber": 1,
    "paymentUrl": "",
    "paymentInstructions": "",
    "footer": "",
}


DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS invoice_records (
        invoice_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        number TEXT NOT NULL,
        status TEXT NOT NULL CHECK(status IN ('draft','sent','paid','void')),
        contact_id TEXT NOT NULL DEFAULT '',
        currency TEXT NOT NULL,
        total INTEGER NOT NULL DEFAULT 0 CHECK(total >= 0),
        paid_amount INTEGER NOT NULL DEFAULT 0 CHECK(paid_amount >= 0),
        issued_at INTEGER NOT NULL DEFAULT 0 CHECK(issued_at >= 0),
        due_at INTEGER NOT NULL DEFAULT 0 CHECK(due_at >= 0),
        sent_at INTEGER NOT NULL DEFAULT 0 CHECK(sent_at >= 0),
        last_sent_at INTEGER NOT NULL DEFAULT 0 CHECK(last_sent_at >= 0),
        paid_at INTEGER NOT NULL DEFAULT 0 CHECK(paid_at >= 0),
        opens INTEGER NOT NULL DEFAULT 0 CHECK(opens >= 0),
        views INTEGER NOT NULL DEFAULT 0 CHECK(views >= 0),
        clicks INTEGER NOT NULL DEFAULT 0 CHECK(clicks >= 0),
        first_opened_at INTEGER NOT NULL DEFAULT 0 CHECK(first_opened_at >= 0),
        first_viewed_at INTEGER NOT NULL DEFAULT 0 CHECK(first_viewed_at >= 0),
        last_activity_at INTEGER NOT NULL DEFAULT 0 CHECK(last_activity_at >= 0),
        overdue_notified_at INTEGER NOT NULL DEFAULT 0 CHECK(overdue_notified_at >= 0),
        created_by_bi TEXT NOT NULL,
        created_by_name TEXT NOT NULL,
        updated_by_bi TEXT NOT NULL,
        updated_by_name TEXT NOT NULL,
        sent_by_name TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_invoice_records_org_created "
    "ON invoice_records(org_bi,created_at DESC,invoice_id)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_invoice_records_org_number "
    "ON invoice_records(org_bi,number)",
    "CREATE INDEX IF NOT EXISTS idx_invoice_records_org_contact "
    "ON invoice_records(org_bi,contact_id,created_at DESC) WHERE contact_id<>''",
    "CREATE INDEX IF NOT EXISTS idx_invoice_records_overdue "
    "ON invoice_records(status,overdue_notified_at,due_at)",
    """CREATE TRIGGER IF NOT EXISTS trg_invoice_records_limit
        BEFORE INSERT ON invoice_records
        WHEN (SELECT COUNT(*) FROM invoice_records WHERE org_bi=NEW.org_bi)>=10000
        BEGIN SELECT RAISE(ABORT,'invoice_catalog_full'); END""",
    """CREATE TABLE IF NOT EXISTS invoice_events (
        event_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        invoice_id TEXT NOT NULL REFERENCES invoice_records(invoice_id) ON DELETE CASCADE,
        kind TEXT NOT NULL CHECK(kind IN ('created','sent','reminder','failed','opened',
            'viewed','clicked','paid','reopened','voided','overdue')),
        at INTEGER NOT NULL CHECK(at >= 0),
        actor TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_invoice_events_invoice "
    "ON invoice_events(invoice_id,at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_invoice_events_org "
    "ON invoice_events(org_bi,at DESC)",
    """CREATE TABLE IF NOT EXISTS invoice_settings (
        org_bi TEXT PRIMARY KEY,
        updated_by_name TEXT NOT NULL,
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
]
DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS invoice_events",
    "DROP TABLE IF EXISTS invoice_settings",
    "DROP TABLE IF EXISTS invoice_records",
]


RECORD_COLUMNS = (
    "invoice_id,org_name,number,status,contact_id,currency,total,paid_amount,"
    "issued_at,due_at,sent_at,last_sent_at,paid_at,opens,views,clicks,"
    "first_opened_at,first_viewed_at,last_activity_at,created_by_name,"
    "updated_by_name,sent_by_name,created_at,updated_at,data"
)


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _redirect(runtime, location):
    return runtime.response(
        {"ok": True, "location": location}, status=302,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"location": location, "x-content-type-options": "nosniff",
                       "referrer-policy": "no-referrer"})


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _text(value, limit):
    return str(value or "").replace("\x00", "").replace("\r\n", "\n")[:limit].strip()


def _integer(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _number(value, default=0.0):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return default
    return result if result == result and abs(result) != float("inf") else default


def _email(value):
    value = _clean(value, 254)
    return value if EMAIL_RE.fullmatch(value) else ""


def _https(value):
    value = _clean(value, 1000)
    return value if HTTPS_RE.fullmatch(value) else ""


def _currency(value, default="USD"):
    value = _clean(value, 3).upper()
    return value if CURRENCY_RE.fullmatch(value) else default


def _amount(value):
    return min(MAX_AMOUNT, max(0, _integer(round(_number(value)))))


def _rate(value):
    return min(100000, max(0, _integer(round(_number(value)))))


def _stamp(value, default=0):
    stamp = _integer(value, default)
    return stamp if 0 <= stamp < 10 ** 14 else default


def _money(amount, currency):
    return "%s %s" % (currency, "{:,.2f}".format(_integer(amount) / 100))


def _date(stamp):
    if not stamp:
        return ""
    return datetime.fromtimestamp(stamp / 1000, timezone.utc).strftime("%d %b %Y").lstrip("0")


def _mail_client(agent):
    agent = str(agent or "").lower()
    for needle, label in MAIL_CLIENTS:
        if needle in agent:
            return label
    return ""


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ("organizations",)
    match = re.fullmatch(r"/api/invoices/-/t/(o|c)/([0-9a-f]{32}\.[0-9a-f]{24})(?:\.png)?", clean)
    if match:
        return ("track", match.group(1), match.group(2))
    match = re.fullmatch(r"/api/invoices/-/public/([0-9a-f]{32}\.[0-9a-f]{24})", clean)
    if match:
        return ("public", match.group(1))
    match = re.fullmatch(
        r"/api/invoices/([a-z][a-z0-9-]{0,62})"
        r"(?:/(items|contacts|settings)(?:/([0-9a-f]{32})(?:/(send|paid|void|reopen))?)?)?",
        clean,
    )
    if not match:
        return None
    return ("organization", match.group(1), match.group(2) or "", match.group(3) or "",
            match.group(4) or "")


def _settings_data(data, existing=None):
    result = dict(DEFAULT_SETTINGS)
    result.update(existing or {})
    for key, limit, cleaner in (
        ("businessName", 160, _clean),
        ("businessAddress", 1000, _text),
        ("fromName", 120, _clean),
        ("prefix", 20, _clean),
        ("paymentInstructions", 4000, _text),
        ("footer", 1000, _text),
    ):
        if key in data:
            result[key] = cleaner(data.get(key), limit)
    if "fromEmail" in data:
        result["fromEmail"] = _email(data.get("fromEmail"))
    if "paymentUrl" in data:
        result["paymentUrl"] = _https(data.get("paymentUrl"))
    if "currency" in data:
        result["currency"] = _currency(data.get("currency"), result["currency"])
    if "dueDays" in data:
        result["dueDays"] = min(365, max(0, _integer(data.get("dueDays"), 30)))
    if "taxRate" in data:
        result["taxRate"] = _rate(data.get("taxRate"))
    if "nextNumber" in data:
        result["nextNumber"] = min(10 ** 9, max(1, _integer(data.get("nextNumber"), 1)))
    return result


async def _settings(runtime, org_bi):
    row = await runtime.d1_first("SELECT data FROM invoice_settings WHERE org_bi=?", org_bi)
    data = await runtime.open(row.get("data")) if row else {}
    return _settings_data({}, data if isinstance(data, dict) else {})


async def _save_settings(runtime, org_bi, actor, settings):
    await runtime.d1_run(
        "INSERT INTO invoice_settings(org_bi,updated_by_name,updated_at,data) VALUES(?,?,?,?) "
        "ON CONFLICT(org_bi) DO UPDATE SET updated_by_name=excluded.updated_by_name,"
        "updated_at=excluded.updated_at,data=excluded.data",
        org_bi, actor, runtime.now(), await runtime.seal(settings))


async def _crm_installed(runtime, org_bi):
    return bool(await runtime.plugin_installed(org_bi, CRM_PLUGIN_ID))


async def _crm_sender(runtime, org_bi):
    try:
        row = await runtime.d1_first("SELECT data FROM crm_settings WHERE org_bi=?", org_bi)
    except Exception as error:
        if "no such table" in str(error).lower():
            return {}
        raise
    data = await runtime.open(row.get("data")) if row else {}
    data = data if isinstance(data, dict) else {}
    return {"fromEmail": _email(data.get("fromEmail")), "fromName": _clean(data.get("fromName"), 120)}


def _crm_contact_project(row, data):
    return {
        "id": str(row.get("contact_id") or ""),
        "name": _clean(data.get("name"), 160),
        "email": _email(data.get("email")),
        "company": _clean(data.get("company"), 160),
        "stage": _clean(data.get("stage"), 40),
    }


async def _crm_contact(runtime, org_bi, contact_id):
    if not contact_id or not await _crm_installed(runtime, org_bi):
        return None
    row = await runtime.d1_first(
        "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? AND contact_id=?",
        org_bi, contact_id)
    if not row:
        return None
    data = await runtime.open(row.get("data"))
    return _crm_contact_project(row, data if isinstance(data, dict) else {})


async def _crm_contacts(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? "
        "ORDER BY updated_at DESC,contact_id LIMIT ?", org_bi, MAX_CONTACTS)
    contacts = []
    for row in rows or []:
        data = await runtime.open(row.get("data"))
        contacts.append(_crm_contact_project(row, data if isinstance(data, dict) else {}))
    contacts.sort(key=lambda item: (item["name"] or item["email"]).lower())
    return contacts


def _client(value, existing=None):
    value = value if isinstance(value, dict) else {}
    result = dict(existing or {})
    for key, limit, cleaner in (
        ("name", 160, _clean),
        ("company", 160, _clean),
        ("address", 1000, _text),
    ):
        if key in value:
            result[key] = cleaner(value.get(key), limit)
    if "email" in value:
        result["email"] = _email(value.get("email"))
    return {key: result.get(key, "") for key in ("name", "email", "company", "address")}


def _items(value):
    items = []
    for raw in (value if isinstance(value, list) else [])[:MAX_ITEMS]:
        if not isinstance(raw, dict):
            continue
        description = _text(raw.get("description"), 1000)
        quantity = round(min(10 ** 6, max(0.0, _number(raw.get("quantity"), 1.0))), 3)
        unit = _amount(raw.get("unitAmount"))
        if not description and not unit:
            continue
        items.append({"description": description, "quantity": quantity, "unitAmount": unit})
    return items


def _totals(items, tax_rate):
    subtotal = sum(int(round(item["quantity"] * item["unitAmount"])) for item in items)
    subtotal = min(MAX_AMOUNT, subtotal)
    tax = int(round(subtotal * tax_rate / 10000))
    return subtotal, tax, min(MAX_AMOUNT, subtotal + tax)


async def _track_token(runtime, invoice_id):
    return "%s.%s" % (invoice_id, (await runtime.blind("invoice-view:" + invoice_id))[:24])


async def _token_invoice(runtime, token):
    match = TOKEN_RE.fullmatch(str(token or ""))
    if not match:
        return ""
    invoice_id = match.group(1)
    if not hmac.compare_digest(await _track_token(runtime, invoice_id), token):
        return ""
    return invoice_id


def _track_origin(runtime):
    secret = getattr(runtime, "package_secret", None)
    configured = str(secret("TRACK_ORIGIN") if secret else "").rstrip("/").lower()
    if ORIGIN_RE.fullmatch(configured):
        return configured
    for source in ("request_origin", "public_origin"):
        origin = str(getattr(runtime, source, lambda: "")() or "").rstrip("/").lower()
        if ORIGIN_RE.fullmatch(origin):
            return origin
    return TRACK_ORIGIN


def _view_url(origin, token):
    return "%s/invoice?t=%s" % (origin, token)


def _display_status(row, now):
    status = str(row.get("status") or "draft")
    if status == "sent" and _integer(row.get("due_at")) and _integer(row.get("due_at")) < now:
        return "overdue"
    if status == "sent" and _integer(row.get("paid_amount")):
        return "partial"
    return status


async def _row(runtime, org_bi, invoice_id):
    return await runtime.d1_first(
        "SELECT " + RECORD_COLUMNS + " FROM invoice_records WHERE org_bi=? AND invoice_id=?",
        org_bi, invoice_id)


async def _project(runtime, row, now, origin=""):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    items = _items(data.get("items"))
    tax_rate = _rate(data.get("taxRate"))
    subtotal, tax, total = _totals(items, tax_rate)
    paid = _integer(row.get("paid_amount"))
    status = str(row.get("status") or "draft")
    invoice_id = str(row.get("invoice_id") or "")
    public_url = ""
    if origin and status != "draft":
        public_url = _view_url(origin, await _track_token(runtime, invoice_id))
    return {
        "id": invoice_id,
        "organization": str(row.get("org_name") or ""),
        "number": str(row.get("number") or ""),
        "status": status,
        "displayStatus": _display_status(row, now),
        "contactId": str(row.get("contact_id") or ""),
        "client": _client(data.get("client")),
        "currency": str(row.get("currency") or "USD"),
        "items": items,
        "taxRate": tax_rate,
        "subtotal": subtotal,
        "tax": tax,
        "total": total,
        "paidAmount": paid,
        "balance": max(0, total - paid) if status not in ("void", "paid") else 0,
        "notes": str(data.get("notes") or ""),
        "terms": str(data.get("terms") or ""),
        "paymentUrl": str(data.get("paymentUrl") or ""),
        "payments": list(data.get("payments") or [])[-50:],
        "issuedAt": _integer(row.get("issued_at")),
        "dueAt": _integer(row.get("due_at")),
        "sentAt": _integer(row.get("sent_at")),
        "lastSentAt": _integer(row.get("last_sent_at")),
        "sentTo": _email(data.get("sentTo")),
        "paidAt": _integer(row.get("paid_at")),
        "opens": _integer(row.get("opens")),
        "views": _integer(row.get("views")),
        "clicks": _integer(row.get("clicks")),
        "firstOpenedAt": _integer(row.get("first_opened_at")),
        "firstViewedAt": _integer(row.get("first_viewed_at")),
        "lastActivityAt": _integer(row.get("last_activity_at")),
        "createdBy": str(row.get("created_by_name") or ""),
        "updatedBy": str(row.get("updated_by_name") or ""),
        "sentBy": str(row.get("sent_by_name") or ""),
        "createdAt": _integer(row.get("created_at")),
        "updatedAt": _integer(row.get("updated_at")),
        "publicUrl": public_url,
    }


async def _invoices(runtime, org_bi, now, origin="", contact_id=""):
    if contact_id:
        rows = await runtime.d1_all(
            "SELECT " + RECORD_COLUMNS + " FROM invoice_records WHERE org_bi=? AND contact_id=? "
            "ORDER BY created_at DESC,invoice_id LIMIT ?", org_bi, contact_id, MAX_LISTED)
    else:
        rows = await runtime.d1_all(
            "SELECT " + RECORD_COLUMNS + " FROM invoice_records WHERE org_bi=? "
            "ORDER BY created_at DESC,invoice_id LIMIT ?", org_bi, MAX_LISTED)
    return [await _project(runtime, row, now, origin) for row in rows or []]


async def _event(runtime, org_bi, invoice_id, kind, actor="", detail=None, at=0):
    if kind in TRACKED_KINDS:
        row = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM invoice_events WHERE invoice_id=?", invoice_id)
        if _integer((row or {}).get("n")) >= MAX_EVENTS_PER_INVOICE:
            return
    await runtime.d1_run(
        "INSERT INTO invoice_events(event_id,org_bi,invoice_id,kind,at,actor,data) "
        "VALUES(?,?,?,?,?,?,?)",
        runtime.new_id(), org_bi, invoice_id, kind, at or runtime.now(), _clean(actor, 64),
        await runtime.seal(dict(detail or {})))


async def _event_project(runtime, row):
    data = await runtime.open(row.get("data")) if row.get("data") else {}
    data = data if isinstance(data, dict) else {}
    return {
        "kind": str(row.get("kind") or ""),
        "at": _integer(row.get("at")),
        "actor": str(row.get("actor") or ""),
        "client": _clean(data.get("client"), 40),
        "to": _email(data.get("to")),
        "amount": _integer(data.get("amount")),
        "method": _clean(data.get("method"), 60),
        "error": _clean(data.get("error"), 200),
        "invoiceId": str(row.get("invoice_id") or ""),
    }


async def _events(runtime, invoice_id):
    rows = await runtime.d1_all(
        "SELECT invoice_id,kind,at,actor,data FROM invoice_events WHERE invoice_id=? "
        "ORDER BY at DESC,event_id LIMIT ?", invoice_id, MAX_EVENTS_PER_INVOICE)
    return [await _event_project(runtime, row) for row in rows or []]


async def _activity(runtime, org_bi, invoices):
    rows = await runtime.d1_all(
        "SELECT invoice_id,kind,at,actor,data FROM invoice_events WHERE org_bi=? "
        "AND kind IN (" + ",".join("?" * len(ACTIVITY_KINDS)) + ") "
        "ORDER BY at DESC,event_id LIMIT 30", org_bi, *ACTIVITY_KINDS)
    by_id = {item["id"]: item for item in invoices}
    activity = []
    for row in rows or []:
        event = await _event_project(runtime, row)
        invoice = by_id.get(event["invoiceId"]) or {}
        event["number"] = invoice.get("number", "")
        event["clientName"] = (invoice.get("client") or {}).get("name") or (
            invoice.get("client") or {}).get("company", "")
        event["currency"] = invoice.get("currency", "")
        activity.append(event)
    return activity


def _sum_by_currency(rows, key):
    result = {}
    for row in rows:
        amount = _integer(row.get(key))
        if amount:
            currency = str(row.get("currency") or "USD")
            result[currency] = result.get(currency, 0) + amount
    return result


async def _summary(runtime, org_bi, now):
    rows = await runtime.d1_all(
        "SELECT status,currency,total,paid_amount,due_at,sent_at,paid_at,opens,views,"
        "clicks,first_opened_at FROM invoice_records WHERE org_bi=?", org_bi)
    rows = rows or []
    unpaid = [dict(row, balance=max(0, _integer(row.get("total")) - _integer(row.get("paid_amount"))))
              for row in rows if row.get("status") == "sent"]
    overdue = [row for row in unpaid if _integer(row.get("due_at")) and _integer(row.get("due_at")) < now]
    paid_recent = [row for row in rows if row.get("status") == "paid"
                   and _integer(row.get("paid_at")) >= now - 30 * DAY_MS]
    sent = [row for row in rows if _integer(row.get("sent_at")) and row.get("status") != "draft"]
    paid = [row for row in rows if row.get("status") == "paid" and _integer(row.get("sent_at"))
            and _integer(row.get("paid_at")) >= _integer(row.get("sent_at"))]
    opened = [row for row in sent if _integer(row.get("first_opened_at"))
              >= _integer(row.get("sent_at"))]
    return {
        "counts": {
            "draft": sum(row.get("status") == "draft" for row in rows),
            "sent": len(unpaid),
            "overdue": len(overdue),
            "paid": sum(row.get("status") == "paid" for row in rows),
            "void": sum(row.get("status") == "void" for row in rows),
        },
        "outstanding": _sum_by_currency(unpaid, "balance"),
        "overdueAmount": _sum_by_currency(overdue, "balance"),
        "paidLast30Days": _sum_by_currency(paid_recent, "total"),
        "sent": len(sent),
        "opened": sum(_integer(row.get("opens")) > 0 for row in sent),
        "viewed": sum(_integer(row.get("views")) > 0 for row in sent),
        "clicked": sum(_integer(row.get("clicks")) > 0 for row in sent),
        "openRate": round(100 * sum(_integer(row.get("opens")) > 0 for row in sent) / len(sent))
        if sent else 0,
        "viewRate": round(100 * sum(_integer(row.get("views")) > 0 for row in sent) / len(sent))
        if sent else 0,
        "averageDaysToPay": round(sum(
            _integer(row.get("paid_at")) - _integer(row.get("sent_at")) for row in paid
        ) / len(paid) / DAY_MS, 1) if paid else 0,
        "averageHoursToOpen": round(sum(
            _integer(row.get("first_opened_at")) - _integer(row.get("sent_at")) for row in opened
        ) / len(opened) / (DAY_MS / 24), 1) if opened else 0,
    }


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _organizations(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT o.name,m.role,p.plugin AS installed FROM org_members m "
        "JOIN orgs o ON o.org_bi=m.org_bi LEFT JOIN org_plugin_installs p "
        "ON p.org_bi=o.org_bi AND p.plugin=? WHERE m.member_bi=? "
        "ORDER BY o.name COLLATE NOCASE LIMIT 200",
        PLUGIN_ID,
        account_bi,
    )
    return [
        {
            "name": str(row.get("name") or ""),
            "role": str(row.get("role") or "member"),
            "installed": bool(row.get("installed")),
        }
        for row in rows or []
    ]


def _public_settings(settings):
    return {key: settings.get(key) for key in DEFAULT_SETTINGS}


async def _overview(runtime, org_bi, org_name, role):
    now = runtime.now()
    invoices = await _invoices(runtime, org_bi, now, _track_origin(runtime))
    return _response(runtime, {
        "ok": True,
        "organization": org_name,
        "role": role,
        "canManagePlugin": role in MANAGER_ROLES,
        "crmInstalled": await _crm_installed(runtime, org_bi),
        "invoices": invoices,
        "summary": await _summary(runtime, org_bi, now),
        "activity": await _activity(runtime, org_bi, invoices),
        "settings": _public_settings(await _settings(runtime, org_bi)),
    })


async def _next_number(runtime, org_bi, settings):
    number = max(1, _integer(settings.get("nextNumber"), 1))
    prefix = str(settings.get("prefix") or "")
    for _ in range(50):
        candidate = "%s%04d" % (prefix, number)
        number += 1
        exists = await runtime.d1_first(
            "SELECT invoice_id FROM invoice_records WHERE org_bi=? AND number=?",
            org_bi, candidate)
        if not exists:
            settings["nextNumber"] = number
            return candidate
    settings["nextNumber"] = number
    return "%s%04d-%s" % (prefix, number, runtime.new_id()[:6])


async def _number_taken(runtime, org_bi, number, invoice_id=""):
    row = await runtime.d1_first(
        "SELECT invoice_id FROM invoice_records WHERE org_bi=? AND number=? AND invoice_id<>?",
        org_bi, number, invoice_id)
    return bool(row)


async def _apply_contact(runtime, org_bi, data, envelope):
    if "contactId" not in data:
        return "", None
    contact_id = _clean(data.get("contactId"), 64).lower()
    if not contact_id:
        return "", None
    if not ID_RE.fullmatch(contact_id):
        return None, _response(runtime, {"error": "invalid_contact_id"}, 400)
    contact = await _crm_contact(runtime, org_bi, contact_id)
    if not contact:
        return None, _response(runtime, {"error": "crm_contact_not_found"}, 404)
    client = envelope["client"]
    supplied = data.get("client") if isinstance(data.get("client"), dict) else {}
    for key in ("name", "email", "company"):
        if not supplied.get(key) and contact.get(key):
            client[key] = contact[key]
    return contact_id, None


def _invoice_envelope(data, existing=None):
    envelope = dict(existing or {})
    envelope["client"] = _client(data.get("client"), envelope.get("client"))
    if "items" in data or "items" not in envelope:
        envelope["items"] = _items(data.get("items"))
    if "taxRate" in data or "taxRate" not in envelope:
        envelope["taxRate"] = _rate(data.get("taxRate"))
    for key, limit in (("notes", 10000), ("terms", 4000)):
        if key in data or key not in envelope:
            envelope[key] = _text(data.get(key), limit)
    if "paymentUrl" in data or "paymentUrl" not in envelope:
        envelope["paymentUrl"] = _https(data.get("paymentUrl"))
    envelope.setdefault("payments", [])
    return envelope


async def _create(runtime, org_bi, org_name, account_bi, actor, data):
    settings = await _settings(runtime, org_bi)
    seeded = dict(data)
    seeded.setdefault("taxRate", settings["taxRate"])
    seeded.setdefault("paymentUrl", settings["paymentUrl"])
    envelope = _invoice_envelope(seeded)
    contact_id, error = await _apply_contact(runtime, org_bi, data, envelope)
    if error:
        return error
    if data.get("paymentUrl") and not envelope["paymentUrl"]:
        return _response(runtime, {"error": "invalid_payment_url"}, 400)
    now = runtime.now()
    number = _clean(data.get("number"), 40)
    if number and await _number_taken(runtime, org_bi, number):
        return _response(runtime, {"error": "invoice_number_taken"}, 409)
    if not number:
        number = await _next_number(runtime, org_bi, settings)
        await _save_settings(runtime, org_bi, actor, settings)
    issued = _stamp(data.get("issuedAt"), now) or now
    due = _stamp(data.get("dueAt"), issued + settings["dueDays"] * DAY_MS)
    _, _, total = _totals(envelope["items"], envelope["taxRate"])
    invoice_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO invoice_records(invoice_id,org_bi,org_name,number,status,contact_id,"
        "currency,total,paid_amount,issued_at,due_at,created_by_bi,created_by_name,"
        "updated_by_bi,updated_by_name,created_at,updated_at,data) "
        "VALUES(?,?,?,?,?,?,?,?,0,?,?,?,?,?,?,?,?,?)",
        invoice_id, org_bi, org_name, number, "draft", contact_id or "",
        _currency(data.get("currency"), settings["currency"]), total, issued, due,
        account_bi, actor, account_bi, actor, now, now, await runtime.seal(envelope))
    await _event(runtime, org_bi, invoice_id, "created", actor)
    await runtime.audit(actor, "invoice.create", "invoice", invoice_id)
    return _response(runtime, {"ok": True, "invoice": await _project(
        runtime, await _row(runtime, org_bi, invoice_id), runtime.now(),
        _track_origin(runtime))}, 201)


async def _update(runtime, row, org_bi, account_bi, actor, data):
    if row.get("status") not in EDITABLE:
        return _response(runtime, {"error": "invoice_locked"}, 409)
    existing = await runtime.open(row.get("data"))
    existing = existing if isinstance(existing, dict) else {}
    envelope = _invoice_envelope(data, existing)
    contact_id = str(row.get("contact_id") or "")
    if "contactId" in data:
        linked, error = await _apply_contact(runtime, org_bi, data, envelope)
        if error:
            return error
        contact_id = linked or ""
    if data.get("paymentUrl") and not envelope["paymentUrl"]:
        return _response(runtime, {"error": "invalid_payment_url"}, 400)
    number = _clean(data.get("number", row.get("number")), 40) or str(row.get("number"))
    if await _number_taken(runtime, org_bi, number, row["invoice_id"]):
        return _response(runtime, {"error": "invoice_number_taken"}, 409)
    _, _, total = _totals(envelope["items"], envelope["taxRate"])
    due = _stamp(data.get("dueAt"), _integer(row.get("due_at")))
    await runtime.d1_run(
        "UPDATE invoice_records SET number=?,contact_id=?,currency=?,total=?,issued_at=?,"
        "due_at=?,overdue_notified_at=CASE WHEN ?<>due_at THEN 0 ELSE overdue_notified_at END,"
        "updated_by_bi=?,updated_by_name=?,updated_at=?,data=? WHERE org_bi=? AND invoice_id=?",
        number, contact_id, _currency(data.get("currency"), str(row.get("currency") or "USD")),
        total, _stamp(data.get("issuedAt"), _integer(row.get("issued_at"))), due, due,
        account_bi, actor, runtime.now(), await runtime.seal(envelope), org_bi,
        row["invoice_id"])
    await runtime.audit(actor, "invoice.update", "invoice", row["invoice_id"])
    return await _detail(runtime, org_bi, row["invoice_id"])


async def _detail(runtime, org_bi, invoice_id, status=200, extra=None):
    row = await _row(runtime, org_bi, invoice_id)
    invoice = await _project(runtime, row, runtime.now(), _track_origin(runtime))
    invoice["events"] = await _events(runtime, invoice_id)
    if invoice["contactId"]:
        invoice["contact"] = await _crm_contact(runtime, org_bi, invoice["contactId"])
    return _response(runtime, dict({"ok": True, "invoice": invoice}, **(extra or {})), status)


def _paragraphs(text):
    return "".join(
        '<p style="margin:0 0 14px">%s</p>' % escape(block.strip()).replace("\n", "<br>")
        for block in re.split(r"\n\s*\n", text.strip()) if block.strip())


def _email_html(invoice, settings, message, view_url, pixel_url):
    sans = "-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif"
    currency = invoice["currency"]
    business = settings.get("businessName") or invoice["organization"]
    rows = "".join(
        '<tr><td style="padding:8px 0;border-bottom:1px solid #e4e4e7">%s</td>'
        '<td align="right" style="padding:8px 0;border-bottom:1px solid #e4e4e7;'
        'white-space:nowrap">%s</td></tr>' % (
            escape(item["description"] or "Item")
            + (" × %s" % escape(("%g" % item["quantity"])) if item["quantity"] != 1 else ""),
            escape(_money(round(item["quantity"] * item["unitAmount"]), currency)))
        for item in invoice["items"])
    totals = '<tr><td style="padding:8px 0;color:#52525b">Subtotal</td><td align="right">%s</td></tr>' % (
        escape(_money(invoice["subtotal"], currency)))
    if invoice["tax"]:
        totals += '<tr><td style="padding:4px 0;color:#52525b">Tax (%s%%)</td><td align="right">%s</td></tr>' % (
            escape("%g" % (invoice["taxRate"] / 100)), escape(_money(invoice["tax"], currency)))
    if invoice["paidAmount"]:
        totals += '<tr><td style="padding:4px 0;color:#52525b">Paid</td><td align="right">−%s</td></tr>' % (
            escape(_money(invoice["paidAmount"], currency)))
    totals += ('<tr><td style="padding:8px 0;font-weight:700">Amount due</td>'
               '<td align="right" style="font-weight:700">%s</td></tr>') % escape(
        _money(invoice["balance"], currency))
    instructions = settings.get("paymentInstructions") or ""
    footer = settings.get("footer") or ""
    return (
        '<!doctype html><html><body style="margin:0;background:#f4f4f6">'
        '<table role="presentation" width="100%%" cellpadding="0" cellspacing="0" '
        'style="background:#f4f4f6;padding:28px 12px"><tr><td align="center">'
        '<table role="presentation" width="100%%" cellpadding="0" cellspacing="0" '
        'style="max-width:600px;background:#ffffff;border-radius:12px;padding:28px;'
        'font:400 15px/1.6 %(sans)s;color:#18181b">'
        '<tr><td style="font:700 18px/1.3 %(sans)s">%(business)s</td>'
        '<td align="right" style="color:#52525b;font-size:13px">Invoice %(number)s</td></tr>'
        '<tr><td colspan="2" style="padding:20px 0 4px;color:#52525b;font-size:13px">Amount due</td></tr>'
        '<tr><td colspan="2" style="font:700 28px/1.2 %(sans)s">%(due)s</td></tr>'
        '<tr><td colspan="2" style="padding:4px 0 20px;color:#52525b;font-size:13px">%(when)s</td></tr>'
        '<tr><td colspan="2">%(message)s</td></tr>'
        '<tr><td colspan="2" style="padding:8px 0 20px"><a href="%(view)s" style="display:inline-block;'
        'background:#2563eb;color:#ffffff;text-decoration:none;font-weight:600;padding:10px 18px;'
        'border-radius:8px">View invoice</a></td></tr>'
        '<tr><td colspan="2"><table role="presentation" width="100%%" cellpadding="0" '
        'cellspacing="0" style="font-size:14px">%(rows)s%(totals)s</table></td></tr>'
        '%(instructions)s%(footer)s'
        '</table></td></tr></table>'
        '<img src="%(pixel)s" width="1" height="1" alt="" style="display:block;border:0;'
        'width:1px;height:1px"></body></html>'
    ) % {
        "sans": sans,
        "business": escape(business),
        "number": escape(invoice["number"]),
        "due": escape(_money(invoice["balance"], currency)),
        "when": escape(("Due " + _date(invoice["dueAt"])) if invoice["dueAt"] else ""),
        "message": _paragraphs(message),
        "view": escape(view_url, quote=True),
        "rows": rows,
        "totals": totals,
        "instructions": ('<tr><td colspan="2" style="padding:20px 0 0;font-size:13px;color:#3f3f46">'
                         '<strong>How to pay</strong><br>%s</td></tr>' % escape(instructions)
                         .replace("\n", "<br>")) if instructions else "",
        "footer": ('<tr><td colspan="2" style="padding:20px 0 0;font-size:12px;color:#71717a">%s</td></tr>'
                   % escape(footer).replace("\n", "<br>")) if footer else "",
        "pixel": escape(pixel_url, quote=True),
    }


def _email_text(invoice, settings, message, view_url):
    lines = [message.strip(), "", "View and pay invoice %s: %s" % (invoice["number"], view_url), ""]
    for item in invoice["items"]:
        lines.append("- %s: %s" % (item["description"] or "Item", _money(
            round(item["quantity"] * item["unitAmount"]), invoice["currency"])))
    lines.append("Amount due: %s" % _money(invoice["balance"], invoice["currency"]))
    if invoice["dueAt"]:
        lines.append("Due: %s" % _date(invoice["dueAt"]))
    if settings.get("paymentInstructions"):
        lines += ["", "How to pay:", settings["paymentInstructions"]]
    if settings.get("footer"):
        lines += ["", settings["footer"]]
    return "\n".join(lines).strip()


def _default_message(invoice, settings, reminder, signer):
    name = invoice["client"].get("name") or invoice["client"].get("company") or "there"
    business = settings.get("businessName") or invoice["organization"]
    amount = _money(invoice["balance"], invoice["currency"])
    due = (" due %s" % _date(invoice["dueAt"])) if invoice["dueAt"] else ""
    if reminder:
        body = "A friendly reminder that invoice %s for %s is%s." % (
            invoice["number"], amount, (" due " + _date(invoice["dueAt"])) if due else " still open")
    else:
        body = "Here is invoice %s for %s%s." % (invoice["number"], amount, due)
    return "Hi %s,\n\n%s\n\nThank you,\n%s" % (name, body, signer or business)


async def _send(runtime, row, org_bi, account_bi, actor, data):
    if row.get("status") not in EDITABLE:
        return _response(runtime, {"error": "invoice_locked"}, 409)
    now = runtime.now()
    origin = _track_origin(runtime)
    invoice = await _project(runtime, row, now, origin)
    if not invoice["items"] or not invoice["total"]:
        return _response(runtime, {"error": "invoice_has_no_amount"}, 400)
    recipient = _email(data.get("to") or invoice["client"].get("email"))
    if not recipient:
        return _response(runtime, {"error": "invalid_recipient"}, 400)
    settings = await _settings(runtime, org_bi)
    crm_sender = await _crm_sender(runtime, org_bi) if await _crm_installed(runtime, org_bi) else {}
    from_email = settings.get("fromEmail") or crm_sender.get("fromEmail", "")
    from_name = (settings.get("fromName") or crm_sender.get("fromName")
                 or settings.get("businessName") or invoice["organization"])
    if not from_email:
        return _response(runtime, {"error": "invalid_sender"}, 400)
    reminder = bool(invoice["sentAt"])
    business = settings.get("businessName") or invoice["organization"]
    subject = _clean(data.get("subject"), 300) or (
        ("Reminder: invoice %s from %s" if reminder else "Invoice %s from %s") % (
            invoice["number"], business))
    message = _text(data.get("message"), 20000) or _default_message(
        invoice, settings, reminder, from_name)
    token = await _track_token(runtime, invoice["id"])
    view_url = _view_url(origin, token)
    pixel_url = "%s/api/invoices/-/t/o/%s.png" % (origin, token)
    result = await runtime.send_mailtrap("", {
        "from": {"email": from_email, "name": from_name},
        "to": [{"email": recipient}],
        "subject": subject,
        "text": _email_text(invoice, settings, message, view_url),
        "html": _email_html(invoice, settings, message, view_url, pixel_url),
        "category": "Invoice",
        "tracked": True,
        "custom_variables": {
            "forkmesh_invoice_id": invoice["id"],
            "forkmesh_sender": actor,
        },
    })
    accepted = bool((result or {}).get("accepted"))
    sent_at = runtime.now()
    if not accepted:
        error = _clean((result or {}).get("error"), 200) or "send_failed"
        await _event(runtime, org_bi, invoice["id"], "failed", actor,
                     {"to": recipient, "error": error})
        await runtime.audit(actor, "invoice.send", "invoice", invoice["id"], outcome="failed",
                            details={"error": error})
        return _response(runtime, {"ok": False, "error": error}, 502)
    existing = await runtime.open(row.get("data"))
    existing = existing if isinstance(existing, dict) else {}
    existing["sentTo"] = recipient
    await runtime.d1_run(
        "UPDATE invoice_records SET status='sent',sent_at=CASE WHEN sent_at=0 THEN ? ELSE sent_at END,"
        "last_sent_at=?,sent_by_name=?,last_activity_at=?,updated_at=?,data=? "
        "WHERE org_bi=? AND invoice_id=?",
        sent_at, sent_at, actor, sent_at, sent_at, await runtime.seal(existing), org_bi,
        invoice["id"])
    await _event(runtime, org_bi, invoice["id"], "reminder" if reminder else "sent", actor,
                 {"to": recipient}, sent_at)
    await runtime.audit(actor, "invoice.send", "invoice", invoice["id"],
                        details={"reminder": reminder})
    return await _detail(runtime, org_bi, invoice["id"], 201,
                         {"messageId": _clean((result or {}).get("messageId"), 200)})


async def _mark_paid(runtime, row, org_bi, account_bi, actor, data):
    if row.get("status") not in EDITABLE:
        return _response(runtime, {"error": "invoice_locked"}, 409)
    total, already = _integer(row.get("total")), _integer(row.get("paid_amount"))
    balance = max(0, total - already)
    amount = _amount(data.get("amount")) if "amount" in data else balance
    if amount <= 0:
        return _response(runtime, {"error": "invalid_amount"}, 400)
    now = runtime.now()
    paid_at = _stamp(data.get("paidAt"), now) or now
    paid = min(MAX_AMOUNT, already + amount)
    existing = await runtime.open(row.get("data"))
    existing = existing if isinstance(existing, dict) else {}
    payment = {"amount": amount, "at": paid_at, "method": _clean(data.get("method"), 60),
               "note": _clean(data.get("note"), 500), "by": actor}
    existing["payments"] = (list(existing.get("payments") or []) + [payment])[-50:]
    settled = paid >= total
    await runtime.d1_run(
        "UPDATE invoice_records SET status=?,paid_amount=?,paid_at=?,last_activity_at=?,"
        "updated_by_bi=?,updated_by_name=?,updated_at=?,data=? WHERE org_bi=? AND invoice_id=?",
        "paid" if settled else row.get("status"), paid, paid_at if settled else 0, now,
        account_bi, actor, now, await runtime.seal(existing), org_bi, row["invoice_id"])
    await _event(runtime, org_bi, row["invoice_id"], "paid", actor,
                 {"amount": amount, "method": payment["method"]}, now)
    await runtime.audit(actor, "invoice.paid", "invoice", row["invoice_id"],
                        details={"settled": settled})
    return await _detail(runtime, org_bi, row["invoice_id"])


async def _set_status(runtime, row, org_bi, account_bi, actor, action):
    status = str(row.get("status") or "")
    now = runtime.now()
    if action == "void":
        if status == "void":
            return _response(runtime, {"error": "invoice_already_void"}, 409)
        target, kind = "void", "voided"
    else:
        if status not in ("paid", "void"):
            return _response(runtime, {"error": "invoice_not_closed"}, 409)
        target, kind = ("sent" if _integer(row.get("sent_at")) else "draft"), "reopened"
    paid_amount = 0 if status == "paid" and action == "reopen" else _integer(row.get("paid_amount"))
    await runtime.d1_run(
        "UPDATE invoice_records SET status=?,paid_amount=?,paid_at=CASE WHEN ?='paid' THEN paid_at "
        "ELSE 0 END,overdue_notified_at=0,updated_by_bi=?,updated_by_name=?,updated_at=? "
        "WHERE org_bi=? AND invoice_id=?",
        target, paid_amount, target, account_bi, actor, now, org_bi, row["invoice_id"])
    await _event(runtime, org_bi, row["invoice_id"], kind, actor, at=now)
    await runtime.audit(actor, "invoice." + action, "invoice", row["invoice_id"])
    return await _detail(runtime, org_bi, row["invoice_id"])


async def _delete(runtime, row, org_bi, actor):
    if row.get("status") not in DELETABLE:
        return _response(runtime, {"error": "void_before_delete"}, 409)
    await runtime.d1_run("DELETE FROM invoice_events WHERE invoice_id=?", row["invoice_id"])
    await runtime.d1_run(
        "DELETE FROM invoice_records WHERE org_bi=? AND invoice_id=?", org_bi, row["invoice_id"])
    await runtime.audit(actor, "invoice.delete", "invoice", row["invoice_id"])
    return _response(runtime, {"ok": True, "deleted": True})


async def _contact_invoices(runtime, org_bi, contact_id):
    now = runtime.now()
    invoices = await _invoices(runtime, org_bi, now, _track_origin(runtime), contact_id)
    billed, paid, outstanding = {}, {}, {}
    for item in invoices:
        if item["status"] == "void":
            continue
        currency = item["currency"]
        billed[currency] = billed.get(currency, 0) + item["total"]
        if item["paidAmount"]:
            paid[currency] = paid.get(currency, 0) + item["paidAmount"]
        if item["balance"]:
            outstanding[currency] = outstanding.get(currency, 0) + item["balance"]
    return _response(runtime, {
        "ok": True, "contactId": contact_id, "invoices": invoices,
        "totals": {"billed": billed, "paid": paid, "outstanding": outstanding},
    })


async def _notify_sender(runtime, row, title, body, dedupe):
    notify = getattr(runtime, "notify", None)
    sender = _clean(row.get("sent_by_name") or row.get("created_by_name"), 64).lower()
    if notify is None or not sender:
        return
    await notify(sender, "plugin_activity", title, body=body,
                 href="%s?org=%s&invoice=%s" % (PAGE_PATH, row.get("org_name"), row.get("invoice_id")),
                 dedupe=dedupe)


async def _client_label(runtime, row):
    data = await runtime.open(row.get("data"))
    client = _client((data if isinstance(data, dict) else {}).get("client"))
    return client.get("name") or client.get("company") or client.get("email") or "Your client"


async def _record_track(runtime, row, kind, now, visitor):
    column, first = {"opened": ("opens", "first_opened_at"), "viewed": ("views", "first_viewed_at"),
                     "clicked": ("clicks", "")}[kind]
    stamp = (",{f}=CASE WHEN {f}=0 THEN ? ELSE {f} END".format(f=first)) if first else ""
    args = [now] if first else []
    await runtime.d1_run(
        "UPDATE invoice_records SET {c}={c}+1{s},last_activity_at=? WHERE invoice_id=?".format(
            c=column, s=stamp), *args, now, row["invoice_id"])
    client = _mail_client((visitor or {}).get("agent"))
    await _event(runtime, str(row.get("org_bi") or ""), row["invoice_id"], kind,
                 detail={"client": client}, at=now)
    if _integer(row.get(column)):
        return
    label = await _client_label(runtime, row)
    title = {
        "opened": "📬 %s opened invoice %s",
        "viewed": "👀 %s viewed invoice %s",
        "clicked": "💳 %s clicked pay on invoice %s",
    }[kind] % (label, row.get("number"))
    body = ("Read in %s" % client) if client else ""
    await _notify_sender(runtime, row, title, body,
                         "invoice-%s:%s" % (kind, row.get("invoice_id")))


async def _public_row(runtime, token):
    invoice_id = await _token_invoice(runtime, token)
    if not invoice_id:
        return None
    row = await runtime.d1_first(
        "SELECT org_bi," + RECORD_COLUMNS + " FROM invoice_records WHERE invoice_id=?",
        invoice_id)
    if not row or row.get("status") == "draft":
        return None
    if not await runtime.plugin_installed(row.get("org_bi"), PLUGIN_ID):
        return None
    return row


def _visitor(runtime):
    visitor = getattr(runtime, "request_visitor", None)
    return visitor() if visitor else {}


async def _track(runtime, action, token):
    row = await _public_row(runtime, token)
    if action == "o":
        now = runtime.now()
        if row and now - _integer(row.get("last_sent_at")) >= TRACK_QUIET_MS:
            await _record_track(runtime, row, "opened", now, _visitor(runtime))
        return _redirect(runtime, TRACK_LOGO)
    if not row:
        return _response(runtime, {"error": "invoice_not_found"}, 404)
    data = await runtime.open(row.get("data"))
    target = _https((data if isinstance(data, dict) else {}).get("paymentUrl"))
    if not target or row.get("status") != "sent":
        return _redirect(runtime, _view_url(_track_origin(runtime), token))
    await _record_track(runtime, row, "clicked", runtime.now(), _visitor(runtime))
    return _redirect(runtime, target)


async def _public(runtime, token):
    row = await _public_row(runtime, token)
    if not row:
        return _response(runtime, {"error": "invoice_not_found"}, 404)
    now = runtime.now()
    settings = await _settings(runtime, row["org_bi"])
    invoice = await _project(runtime, row, now)
    if row.get("status") != "void":
        await _record_track(runtime, row, "viewed", now, _visitor(runtime))
    origin = _track_origin(runtime)
    pay = ""
    if invoice["paymentUrl"] and invoice["status"] == "sent":
        pay = "%s/api/invoices/-/t/c/%s" % (origin, token)
    return _response(runtime, {
        "ok": True,
        "invoice": {
            "number": invoice["number"],
            "status": invoice["displayStatus"],
            "currency": invoice["currency"],
            "issuedAt": invoice["issuedAt"],
            "dueAt": invoice["dueAt"],
            "paidAt": invoice["paidAt"],
            "client": {key: invoice["client"][key] for key in ("name", "company", "address")},
            "items": invoice["items"],
            "taxRate": invoice["taxRate"],
            "subtotal": invoice["subtotal"],
            "tax": invoice["tax"],
            "total": invoice["total"],
            "paidAmount": invoice["paidAmount"],
            "balance": invoice["balance"],
            "notes": invoice["notes"],
            "terms": invoice["terms"],
            "payUrl": pay,
        },
        "business": {
            "name": settings.get("businessName") or invoice["organization"],
            "address": settings.get("businessAddress") or "",
            "email": settings.get("fromEmail") or "",
            "paymentInstructions": settings.get("paymentInstructions") or "",
            "footer": settings.get("footer") or "",
        },
    })


async def deliver_reminders(runtime):
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT org_bi," + RECORD_COLUMNS + " FROM invoice_records WHERE status='sent' "
        "AND overdue_notified_at=0 AND due_at>0 AND due_at<? LIMIT 25", now)
    delivered = 0
    for row in rows or []:
        if not await runtime.plugin_installed(row.get("org_bi"), PLUGIN_ID):
            continue
        await runtime.d1_run(
            "UPDATE invoice_records SET overdue_notified_at=? WHERE invoice_id=?",
            now, row["invoice_id"])
        await _event(runtime, row["org_bi"], row["invoice_id"], "overdue", at=now)
        balance = max(0, _integer(row.get("total")) - _integer(row.get("paid_amount")))
        await _notify_sender(
            runtime, row,
            "⏰ Invoice %s to %s is overdue" % (row.get("number"), await _client_label(runtime, row)),
            "%s was due %s" % (_money(balance, row.get("currency") or "USD"),
                               _date(_integer(row.get("due_at")))),
            "invoice-overdue:%s:%s" % (row["invoice_id"], row.get("due_at")))
        delivered += 1
    return {"delivered": delivered}


async def org_has_data(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT invoice_id FROM invoice_records WHERE org_bi=? LIMIT 1", org_bi)
    if row:
        return True
    return bool(await runtime.d1_first(
        "SELECT org_bi FROM invoice_settings WHERE org_bi=?", org_bi))


async def export_org_data(runtime, org_bi, org_name):
    now = runtime.now()
    invoices = await _invoices(runtime, org_bi, now)
    for invoice in invoices:
        invoice["events"] = await _events(runtime, invoice["id"])
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-invoices-export-v1",
        "organization": org_name,
        "exportedAt": now,
        "settings": _public_settings(await _settings(runtime, org_bi)),
        "invoices": invoices,
    }


async def purge_org_data(runtime, org_bi):
    await runtime.d1_run("DELETE FROM invoice_events WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM invoice_records WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM invoice_settings WHERE org_bi=?", org_bi)


def _allowed(route):
    kind = route[0]
    if kind in ("organizations", "track", "public"):
        return ("GET",)
    _, _, resource, item, action = route
    if not resource:
        return ("GET",)
    if resource == "settings":
        return ("GET", "PATCH") if not item else ()
    if resource == "contacts":
        return ("GET",)
    if not item:
        return ("POST",) if not action else ()
    if action:
        return ("POST",)
    return ("GET", "PATCH", "DELETE")


async def _public_schema_ready(runtime):
    if not await runtime.d1_first(
            "SELECT plugin FROM org_plugin_installs WHERE plugin=? LIMIT 1", PLUGIN_ID):
        return False
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    return True


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    allowed = _allowed(route)
    if method == "HEAD" and "GET" in allowed:
        method = "GET"
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    await runtime.ensure_schema()
    if route[0] == "track":
        if not await _public_schema_ready(runtime):
            return _redirect(runtime, TRACK_LOGO) if route[1] == "o" else _response(
                runtime, {"error": "invoice_not_found"}, 404)
        return await _track(runtime, route[1], route[2])
    if route[0] == "public":
        if not await _public_schema_ready(runtime):
            return _response(runtime, {"error": "invoice_not_found"}, 404)
        return await _public(runtime, route[1])
    if method != "GET" and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method != "GET":
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error}, 413 if error == "payload_too_large" else 400)
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if route[0] == "organizations":
        return _response(runtime, {"ok": True, "actor": actor,
                                   "organizations": await _organizations(runtime, account_bi)})
    _, org_name, resource, item, action = route
    if not ORG_RE.fullmatch(org_name):
        return _response(runtime, {"error": "organization_not_found"}, 404)
    org_bi, role = await runtime.organization(org_name, account_bi)
    if not org_bi or not role:
        return _response(runtime, {"error": "organization_not_found"}, 404)
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return _response(runtime, {"error": "plugin_not_installed", "organization": org_name,
                                   "canInstall": role in MANAGER_ROLES}, 409)
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    if not resource:
        return await _overview(runtime, org_bi, org_name, role)
    if resource == "settings":
        if method == "GET":
            return _response(runtime, {"ok": True, "settings": _public_settings(
                await _settings(runtime, org_bi))})
        if role not in MANAGER_ROLES:
            return _response(runtime, {"error": "settings_require_admin"}, 403)
        settings = _settings_data(data, await _settings(runtime, org_bi))
        if data.get("paymentUrl") and not settings["paymentUrl"]:
            return _response(runtime, {"error": "invalid_payment_url"}, 400)
        if data.get("fromEmail") and not settings["fromEmail"]:
            return _response(runtime, {"error": "invalid_sender"}, 400)
        await _save_settings(runtime, org_bi, actor, settings)
        await runtime.audit(actor, "invoice.settings", "invoice_settings", org_name)
        return _response(runtime, {"ok": True, "settings": _public_settings(settings)})
    if resource == "contacts":
        if not await _crm_installed(runtime, org_bi):
            return _response(runtime, {"error": "crm_not_installed"}, 409)
        if item:
            return await _contact_invoices(runtime, org_bi, item)
        return _response(runtime, {"ok": True, "contacts": await _crm_contacts(runtime, org_bi)})
    if not item:
        return await _create(runtime, org_bi, org_name, account_bi, actor, data)
    row = await _row(runtime, org_bi, item)
    if not row:
        return _response(runtime, {"error": "invoice_not_found"}, 404)
    if action == "send":
        return await _send(runtime, row, org_bi, account_bi, actor, data)
    if action == "paid":
        return await _mark_paid(runtime, row, org_bi, account_bi, actor, data)
    if action in ("void", "reopen"):
        return await _set_status(runtime, row, org_bi, account_bi, actor, action)
    if method == "GET":
        return await _detail(runtime, org_bi, item)
    if method == "DELETE":
        return await _delete(runtime, row, org_bi, actor)
    return await _update(runtime, row, org_bi, account_bi, actor, data)
