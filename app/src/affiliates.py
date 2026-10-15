import re
from urllib.parse import urlsplit


PREFIX = "/api/affiliates"
JOIN_PATH = PREFIX + "/join"
LEADS_PATH = PREFIX + "/leads"
ACTIVITY_PART = "activity"
ADMIN_PATH = "/api/admin/affiliates"
BODY_MAX_BYTES = 16 * 1024
DAY_MS = 24 * 60 * 60 * 1000

COMMISSION_PERCENT = 50
ACTIVITY_DAYS = 7
FINAL_AFTER_DAYS = 30
MAX_OPEN_LEADS = 10
MAX_ACTIVITY = 40
PAYMENT_LINK = "https://forkmesh.com/#support-forkmesh"
PIPELINE_FLOW = "customers"
PIPELINE_STAGE = "lead"

AFFILIATE_STATUSES = ("active", "paused", "removed")
LEAD_STATUSES = ("new", "known", "contacted", "trial", "won", "paid", "lost",
                 "rejected", "expired")
OPEN_LEAD_STATUSES = ("new", "contacted", "trial", "won")
ADMIN_LEAD_STATUSES = ("contacted", "trial", "won", "paid", "lost", "rejected")

EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]{1,64}@"
    r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
    r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$")
HOST_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$")
LEAD_ID_RE = re.compile(r"^[a-f0-9-]{8,64}$")
USERNAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,63}$")


def program():
    return {
        "commissionPercent": COMMISSION_PERCENT,
        "activityDays": ACTIVITY_DAYS,
        "finalAfterDays": FINAL_AFTER_DAYS,
        "maxOpenLeads": MAX_OPEN_LEADS,
        "paymentLink": PAYMENT_LINK,
        "terms": [
            "You earn %d%% of each eligible customer's first month."
            % COMMISSION_PERCENT,
            "You may claim a maximum of %d companies at a time." % MAX_OPEN_LEADS,
            "Claim a company here before any outreach. The first valid claim "
            "gets the company.",
            "A claim expires after %d days without meaningful outreach or "
            "activity. Log what you did to keep it." % ACTIVITY_DAYS,
            "Customers must be new ForkMesh paid customers and not already in "
            "an active ForkMesh sales conversation.",
            "One customer counts as one referral. Related accounts or entities "
            "may be treated as one customer.",
            "A referral counts after successful payment through Patreon, with "
            "the customer's own payment method. Refunds, chargebacks, "
            "fraudulent payments or reversals do not count.",
            "Commissions are final once the subscription has stayed paid and "
            "in good standing for %d days." % FINAL_AFTER_DAYS,
            "No self-referrals, fake accounts, duplicate customers, artificial "
            "upgrades, or coordinated purchases.",
            "ForkMesh may verify referrals and resolve disputes, duplicate "
            "claims, or suspected abuse, and may clarify these rules; changes "
            "are announced publicly.",
        ],
    }


def text(value, maximum):
    clean = "".join(
        character if character.isprintable() or character in "\n\t" else " "
        for character in str(value or ""))
    return clean.strip()[:maximum]


def line(value, maximum):
    return text(" ".join(str(value or "").split()), maximum)


def website_domain(value):
    raw = line(value, 300)
    if not raw:
        return ""
    if "://" not in raw:
        raw = "https://" + raw
    try:
        parsed = urlsplit(raw)
        host = str(parsed.hostname or "").strip().lower().rstrip(".")
    except ValueError:
        return ""
    if parsed.scheme not in ("http", "https") or parsed.username or parsed.password:
        return ""
    if host.startswith("www."):
        host = host[4:]
    if not HOST_RE.fullmatch(host) or host.replace(".", "").isdigit():
        return ""
    if host == "forkmesh.com" or host.endswith(".forkmesh.com"):
        return ""
    return host


def clean_email(value):
    email = line(value, 254).lower()
    return email if EMAIL_RE.fullmatch(email) else ""


def clean_lead(data):
    data = data if isinstance(data, dict) else {}
    lead = {
        "company": line(data.get("company"), 160),
        "domain": website_domain(data.get("website")),
        "contactName": line(data.get("contactName"), 160),
        "contactEmail": clean_email(data.get("contactEmail")),
        "contactTitle": line(data.get("contactTitle"), 160),
        "notes": text(data.get("notes"), 2000),
    }
    if not lead["company"]:
        return lead, "company_required"
    if not lead["domain"]:
        return lead, "website_required"
    if data.get("contactEmail") and not lead["contactEmail"]:
        return lead, "invalid_contact_email"
    if lead["contactEmail"] and data.get("consent") is not True:
        return lead, "consent_required"
    return lead, ""


def pipeline_lead(lead, affiliate, lead_id):
    notes = ["Claimed by affiliate @%s, who is doing the outreach." % affiliate]
    if lead.get("contactEmail"):
        notes.append("They said this contact is happy to hear from ForkMesh.")
    if lead.get("notes"):
        notes.extend(("", lead["notes"]))
    return {
        "actor": affiliate,
        "name": lead.get("contactName") or lead["company"],
        "email": lead.get("contactEmail", ""),
        "company": lead["company"],
        "title": lead.get("contactTitle", ""),
        "website": "https://" + lead["domain"],
        "source": "Affiliate: @" + affiliate,
        "notes": "\n".join(notes),
        "flow": PIPELINE_FLOW,
        "stage": PIPELINE_STAGE,
        "nextStep": "Check with @%s before contacting them" % affiliate,
        "tags": ["affiliate", "affiliate-" + affiliate[:40]],
        "fields": {"affiliate": "@" + affiliate, "affiliate_lead": lead_id},
    }


def commission_cents(first_month_cents):
    return max(0, int(first_month_cents or 0)) * COMMISSION_PERCENT // 100


def public_lead(record, row):
    record = record if isinstance(record, dict) else {}
    return {
        "id": str(row.get("lead_id") or ""),
        "company": line(record.get("company"), 160),
        "domain": line(record.get("domain"), 253),
        "contactName": line(record.get("contactName"), 160),
        "status": str(row.get("status") or ""),
        "inPipeline": str(row.get("pipeline_status") or "") in ("created", "exists"),
        "firstMonthCents": int(record.get("firstMonthCents") or 0),
        "commissionCents": int(record.get("commissionCents") or 0),
        "note": line(record.get("statusNote"), 300),
        "activity": [
            {"at": int(item.get("at") or 0), "note": line(item.get("note"), 500)}
            for item in (record.get("activity") or [])[-5:]
            if isinstance(item, dict)],
        "createdAt": int(row.get("created_at") or 0),
        "expiresAt": int(row.get("expires_at") or 0),
    }


def _response(runtime, payload, status=200):
    return runtime.response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"})


async def _body(runtime):
    if not runtime.same_origin():
        return None, _response(runtime, {"error": "origin_not_allowed"}, 403)
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime, {"error": error},
            413 if error == "payload_too_large" else 400)
    return data, None


async def _account(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    name = line((record or {}).get("name"), 64).lower()
    if not account_bi or not name:
        return "", "", None
    return str(account_bi), name, record


def _verified(record):
    return bool(record) and bool(record.get("email_verified")) and str(
        record.get("status") or "active") == "active"


async def _affiliate(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_bi,username,status,data,created_at FROM affiliates "
        "WHERE account_bi=?", account_bi)


async def _expire_leads(runtime, now):
    await runtime.d1_run(
        "UPDATE affiliate_leads SET status='expired',updated_at=? "
        "WHERE status IN ('new','contacted','trial') AND expires_at>0 "
        "AND expires_at<?", now, now)


async def _leads_for(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT lead_id,status,pipeline_status,data,created_at,expires_at "
        "FROM affiliate_leads WHERE affiliate_bi=? "
        "ORDER BY created_at DESC LIMIT 100", account_bi)
    leads = []
    for row in rows or []:
        leads.append(public_lead(await runtime.open(row.get("data")), row))
    return leads


async def _me(runtime, account_bi, name, record):
    row = await _affiliate(runtime, account_bi)
    return {
        "account": name,
        "emailVerified": _verified(record),
        "affiliate": {
            "status": str(row.get("status") or ""),
            "joinedAt": int(row.get("created_at") or 0),
        } if row else None,
        "leads": await _leads_for(runtime, account_bi) if row else [],
    }


async def _join(runtime):
    data, error = await _body(runtime)
    if error:
        return error
    account_bi, name, record = await _account(runtime, data)
    if not account_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    if not _verified(record):
        return _response(runtime, {"error": "email_not_verified"}, 403)
    if data.get("acceptTerms") is not True:
        return _response(runtime, {"error": "terms_required"}, 400)
    channel = text(data.get("channel"), 500)
    if not channel:
        return _response(runtime, {"error": "channel_required"}, 400)
    existing = await _affiliate(runtime, account_bi)
    if existing and existing.get("status") == "removed":
        return _response(runtime, {"error": "affiliate_removed"}, 403)
    now = int(runtime.now())
    sealed = await runtime.seal({
        "username": name, "channel": channel, "acceptedTermsAt": now})
    if existing:
        await runtime.d1_run(
            "UPDATE affiliates SET data=?,updated_at=? WHERE account_bi=?",
            sealed, now, account_bi)
    else:
        await runtime.d1_run(
            "INSERT INTO affiliates (account_bi,username,status,data,"
            "created_at,updated_at) VALUES (?,?,'active',?,?,?)",
            account_bi, name, sealed, now, now)
        await runtime.audit(name, "affiliate.join", "affiliate", name, {})
    return _response(runtime, {
        "ok": True, "me": await _me(runtime, account_bi, name, record)},
        200 if existing else 201)


async def _quota(runtime, account_bi, now):
    open_row = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM affiliate_leads WHERE affiliate_bi=? "
        "AND status IN ('new','contacted','trial')", account_bi)
    if int((open_row or {}).get("count") or 0) >= MAX_OPEN_LEADS:
        return "open_lead_limit"
    return ""


async def _add_lead(runtime):
    data, error = await _body(runtime)
    if error:
        return error
    account_bi, name, record = await _account(runtime, data)
    if not account_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    if not _verified(record):
        return _response(runtime, {"error": "email_not_verified"}, 403)
    affiliate = await _affiliate(runtime, account_bi)
    if not affiliate:
        return _response(runtime, {"error": "join_first"}, 403)
    if affiliate.get("status") != "active":
        return _response(runtime, {"error": "affiliate_not_active"}, 403)
    lead, invalid = clean_lead(data)
    if invalid:
        return _response(runtime, {"error": invalid}, 400)
    own_email = clean_email((record or {}).get("email"))
    if own_email and (own_email == lead["contactEmail"] or own_email.split(
            "@")[-1] == lead["domain"]):
        return _response(runtime, {"error": "self_referral"}, 400)
    now = int(runtime.now())
    await _expire_leads(runtime, now)
    quota = await _quota(runtime, account_bi, now)
    if quota:
        return _response(runtime, {"error": quota, "program": program()}, 429)
    domain_bi = await runtime.blind("affiliate-domain:" + lead["domain"])
    taken = await runtime.d1_first(
        "SELECT lead_id FROM affiliate_leads WHERE domain_bi=? "
        "AND status IN ('new','contacted','trial','won','paid')", domain_bi)
    if taken:
        return _response(runtime, {"error": "already_claimed"}, 409)
    lead_id = str(runtime.new_id())
    delivered = await runtime.deliver_lead(pipeline_lead(lead, name, lead_id))
    pipeline_status = line((delivered or {}).get("status"), 40)
    status = "known" if pipeline_status == "exists" else "new"
    expires_at = now + ACTIVITY_DAYS * DAY_MS if status == "new" else 0
    sealed = await runtime.seal(dict(
        lead, affiliate=name, activity=[], contactId=line(
            (delivered or {}).get("contactId"), 64)))
    try:
        await runtime.d1_run(
            "INSERT INTO affiliate_leads (lead_id,affiliate_bi,domain_bi,status,"
            "pipeline_status,data,created_at,updated_at,expires_at) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            lead_id, account_bi, domain_bi, status, pipeline_status, sealed, now,
            now, expires_at)
    except Exception:
        return _response(runtime, {"error": "already_claimed"}, 409)
    await runtime.audit(name, "affiliate.lead", "affiliate_lead", lead_id,
                        {"status": status, "pipeline": pipeline_status})
    row = {"lead_id": lead_id, "status": status, "pipeline_status": pipeline_status,
           "created_at": now, "expires_at": expires_at}
    return _response(runtime, {
        "ok": True, "lead": public_lead(lead, row),
        "known": status == "known"}, 201)


async def _log_activity(runtime, lead_id):
    data, error = await _body(runtime)
    if error:
        return error
    account_bi, name, _record = await _account(runtime, data)
    if not account_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    note = text(data.get("note"), 500)
    if not note:
        return _response(runtime, {"error": "note_required"}, 400)
    now = int(runtime.now())
    await _expire_leads(runtime, now)
    row = await runtime.d1_first(
        "SELECT lead_id,affiliate_bi,status,pipeline_status,data,created_at,"
        "expires_at FROM affiliate_leads WHERE lead_id=?", lead_id)
    if not row or str(row.get("affiliate_bi") or "") != account_bi:
        return _response(runtime, {"error": "lead_not_found"}, 404)
    if row.get("status") == "expired":
        return _response(runtime, {"error": "claim_expired"}, 409)
    if row.get("status") not in ("new", "contacted", "trial"):
        return _response(runtime, {"error": "claim_closed"}, 409)
    record = await runtime.open(row.get("data"))
    record = record if isinstance(record, dict) else {}
    activity = [item for item in record.get("activity") or [] if isinstance(item, dict)]
    record["activity"] = (activity + [{"at": now, "note": note}])[-MAX_ACTIVITY:]
    expires_at = now + ACTIVITY_DAYS * DAY_MS
    await runtime.d1_run(
        "UPDATE affiliate_leads SET data=?,updated_at=?,expires_at=? "
        "WHERE lead_id=? AND status IN ('new','contacted','trial')",
        await runtime.seal(record), now, expires_at, lead_id)
    await runtime.audit(name, "affiliate.activity", "affiliate_lead", lead_id, {})
    return _response(runtime, {"ok": True, "lead": public_lead(
        record, dict(row, expires_at=expires_at))})


async def _admin(runtime):
    method = runtime.method()
    data = {}
    if method == "POST":
        data, error = await _body(runtime)
        if error:
            return error
    account_bi, name, _record = await _account(runtime, data)
    if not account_bi or not await runtime.is_admin(name):
        return _response(runtime, {"error": "admin_required"}, 403)
    if method == "GET":
        return _response(runtime, await _admin_listing(runtime))
    lead_id = line(data.get("lead"), 64).lower()
    affiliate = line(data.get("affiliate"), 64).lower()
    now = int(runtime.now())
    if lead_id:
        return await _admin_lead(runtime, name, lead_id, data, now)
    if affiliate:
        status = line(data.get("status"), 20).lower()
        if not USERNAME_RE.fullmatch(affiliate) or status not in AFFILIATE_STATUSES:
            return _response(runtime, {"error": "invalid_request"}, 400)
        await runtime.d1_run(
            "UPDATE affiliates SET status=?,updated_at=? WHERE username=?",
            status, now, affiliate)
        await runtime.audit(name, "affiliate.status", "affiliate", affiliate,
                            {"status": status})
        return _response(runtime, {"ok": True})
    return _response(runtime, {"error": "invalid_request"}, 400)


async def _admin_lead(runtime, actor, lead_id, data, now):
    status = line(data.get("status"), 20).lower()
    if not LEAD_ID_RE.fullmatch(lead_id) or status not in ADMIN_LEAD_STATUSES:
        return _response(runtime, {"error": "invalid_request"}, 400)
    row = await runtime.d1_first(
        "SELECT lead_id,status,pipeline_status,data,created_at,expires_at "
        "FROM affiliate_leads WHERE lead_id=?", lead_id)
    if not row:
        return _response(runtime, {"error": "lead_not_found"}, 404)
    record = await runtime.open(row.get("data"))
    record = record if isinstance(record, dict) else {}
    if status in ("won", "paid"):
        try:
            cents = int(data.get("firstMonthCents", record.get(
                "firstMonthCents") or 0))
        except (TypeError, ValueError):
            cents = -1
        if cents <= 0:
            return _response(runtime, {"error": "first_month_required"}, 400)
        record["firstMonthCents"] = cents
        record["commissionCents"] = commission_cents(cents)
    record["statusNote"] = line(data.get("note"), 300)
    expires_at = 0 if status in ("won", "paid", "lost", "rejected") else (
        now + ACTIVITY_DAYS * DAY_MS)
    await runtime.d1_run(
        "UPDATE affiliate_leads SET status=?,data=?,updated_at=?,expires_at=? "
        "WHERE lead_id=?", status, await runtime.seal(record), now, expires_at,
        lead_id)
    await runtime.audit(actor, "affiliate.lead_status", "affiliate_lead",
                        lead_id, {"status": status})
    return _response(runtime, {"ok": True, "lead": public_lead(
        record, dict(row, status=status, expires_at=expires_at))})


async def _admin_listing(runtime):
    affiliates = []
    for row in await runtime.d1_all(
            "SELECT username,status,data,created_at FROM affiliates "
            "ORDER BY created_at DESC LIMIT 500") or []:
        record = await runtime.open(row.get("data"))
        affiliates.append({
            "username": str(row.get("username") or ""),
            "status": str(row.get("status") or ""),
            "channel": text((record or {}).get("channel"), 500),
            "joinedAt": int(row.get("created_at") or 0),
        })
    leads = []
    for row in await runtime.d1_all(
            "SELECT lead_id,status,pipeline_status,data,created_at,expires_at "
            "FROM affiliate_leads ORDER BY created_at DESC LIMIT 500") or []:
        record = await runtime.open(row.get("data"))
        record = record if isinstance(record, dict) else {}
        lead = public_lead(record, row)
        lead.update({
            "affiliate": line(record.get("affiliate"), 64),
            "contactEmail": clean_email(record.get("contactEmail")),
            "pipelineStatus": str(row.get("pipeline_status") or ""),
        })
        leads.append(lead)
    return {"ok": True, "program": program(), "affiliates": affiliates,
            "leads": leads}


async def handle(runtime, path):
    normalized = str(path or "").rstrip("/")
    method = runtime.method()
    await runtime.ensure_schema()
    if normalized == ADMIN_PATH:
        if method not in ("GET", "POST"):
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        return await _admin(runtime)
    if normalized == PREFIX:
        if method != "GET":
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        account_bi, name, record = await _account(runtime)
        payload = {"ok": True, "program": program(), "me": None}
        if account_bi:
            payload["me"] = await _me(runtime, account_bi, name, record)
        return _response(runtime, payload)
    if normalized in (JOIN_PATH, LEADS_PATH):
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        return await (_join(runtime) if normalized == JOIN_PATH
                      else _add_lead(runtime))
    parts = normalized[len(LEADS_PATH) + 1:].split("/") if normalized.startswith(
        LEADS_PATH + "/") else []
    if len(parts) == 2 and parts[1] == ACTIVITY_PART and LEAD_ID_RE.fullmatch(parts[0]):
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        return await _log_activity(runtime, parts[0])
    return _response(runtime, {"error": "not_found"}, 404)
