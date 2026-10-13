"""Organization CRM contacts, relationship history, and email sends.

The Worker adapter owns sessions, encryption, D1, audit logging, and outbound
Mailtrap HTTP.  This module owns the portable contract shared by the web and
desktop plugin surfaces.
"""

import base64
import csv
import hashlib
import io
import json
import re
from datetime import datetime, timezone
from urllib.parse import quote, urlencode


PREFIX = "/api/crm"
# Desktop request proof for this plugin's API. The Qt bridge signs with the
# matching kCrmRequestProof constant; both must stay byte-for-byte identical.
REQUEST_PROOF = "forkmesh-crm-request-v1"
MAX_BODY_BYTES = 2 * 1024 * 1024
MAX_CONTACTS = 2000
MAX_EMAILS = 1000
MAX_MAIL_MESSAGES = 2000
MAX_ZOHO_IMPORT = 200
MAX_IMPORT_ROWS = 2000
DAY_MS = 24 * 60 * 60 * 1000
ACTIVITY_DAYS = 30
ACTIVITY_MAX_OFFSET_MINUTES = 14 * 60
ACTIVITY_KINDS = ("emails", "outreach", "contacts")
LIST_ACTIVITY_LIMIT = 200
LIST_ACTIVITY_KEEP_MS = 90 * DAY_MS
LIST_VIEW_GAP_MS = 30 * 60 * 1000
LIST_ACTIVITY_NAMES = 5
ACTIVITY_QUERIES = (
    ("emails",
     "SELECT sender_name AS name,(created_at+?)/? AS day,COUNT(*) AS n "
     "FROM crm_email_sends WHERE org_bi=? AND created_at>=? AND status<>'draft' "
     "GROUP BY 1,2"),
    ("outreach",
     "SELECT sender_name AS name,(ts+?)/? AS day,COUNT(*) AS n "
     "FROM crm_outreach_log WHERE org_bi=? AND ts>=? AND ok=1 GROUP BY 1,2"),
    ("contacts",
     "SELECT created_by_name AS name,(created_at+?)/? AS day,COUNT(*) AS n "
     "FROM crm_contacts WHERE org_bi=? AND created_at>=? GROUP BY 1,2"),
)
# A bulk move carries membership only - which pipeline, which stage, which
# lists - so nothing in it can collide with another contact's email or needs
# the one-contact form's checks.
BULK_CONTACT_FIELDS = ("flow", "stage", "views", "setViews", "removeViews",
                       "owner", "lastContactedAt")
BULK_TAG_FIELDS = ("addTags", "removeTags")
# Sealed rows are packed into one JSON bind per UPDATE; D1 caps a bound string
# at 2 MB, so each write stays well under it.
BULK_WRITE_BYTES = 900_000
# A CSV import may arrive in parts so the page can show real progress; every
# part of one import carries the same client-chosen id.
IMPORT_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{7,63}$")
CSV_IMPORT_DELIMITERS = (",", ";", "\t", "|")
CSV_IMPORT_PING_ERRORS = 3
CSV_IMPORT_ROW_MESSAGES = {
    "name_or_email_required": "needs a name, an email or a company",
    "invalid_email": "has an email address that is not valid",
}
# How many data rows an import preview reads to guess each column's field and
# show the member a few sample values before they confirm the mapping.
CSV_PREVIEW_ROWS = 20
CSV_PREVIEW_SAMPLES = 3
CSV_IMPORT_FAILURES = {
    "csv_required": "the file is empty",
    "invalid_csv": "the file is not a readable CSV",
    "csv_header_required": "the file has no header row",
    "too_many_csv_rows": "the file has more than %d rows" % MAX_IMPORT_ROWS,
}
MAX_VIEWS = 60
MAX_VIEW_COLUMNS = 120
MAX_CUSTOM_FIELDS = 80
MAX_CUSTOM_FIELD_BYTES = 4000
CONTACT_ID_RE = re.compile(r"^[0-9a-f]{32}$")
FIELD_KEY_RE = re.compile(r"^[a-z0-9][a-z0-9_]{0,63}$")
ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$"
)
HTTPS_RE = re.compile(r"^https://[^\s]{1,990}$", re.IGNORECASE)
SLUG_RE = re.compile(r"^[a-z][a-z0-9-]{0,39}$")
DEFAULT_FLOWS = (
    {"id": "investment", "name": "Investment", "icon": "investors", "stages": (
        "sourced", "contacted", "meeting", "diligence", "committed", "passed")},
    {"id": "applicants", "name": "Applicant tracking", "icon": "hiring", "stages": (
        "applied", "screening", "interview", "offer", "hired", "rejected")},
    {"id": "partnerships", "name": "Partnerships", "icon": "partners", "stages": (
        "prospect", "contacted", "discovery", "negotiation", "active", "closed")},
    {"id": "customers", "name": "Customers", "icon": "customers", "stages": (
        "lead", "qualified", "trial", "customer", "renewal", "inactive")},
)
# The kind of pipeline a flow is, shown as an icon on its tab and in every
# pipeline picker. "pipeline" is the plain funnel a flow gets when none is set.
FLOW_ICONS = ("pipeline", "investors", "hiring", "partners", "customers",
              "sales", "support", "projects", "events")
DEFAULT_MAIL_BOXES = ("Inbox", "Follow-up", "Important", "Archive")
# The contact columns every surface shows, in their canonical order, with the
# CSV header spellings that land on each one. A header outside this table is
# not thrown away any more: it is kept verbatim as a custom field so an import
# really does carry every column of the file.
CONTACT_COLUMNS = (
    ("name", "Name", ("name", "full_name", "contact_name", "display_name")),
    ("email", "Email", ("email", "email_address", "e_mail", "e_mail_address",
                        "primary_email", "work_email")),
    ("company", "Company", ("company", "organization", "account_name")),
    ("title", "Title", ("title", "job_title", "designation")),
    ("phone", "Phone", ("phone", "phone_number", "mobile", "telephone")),
    ("twitter", "Twitter", ("twitter", "x", "twitter_handle")),
    ("mastodon", "Mastodon", ("mastodon", "mastodon_handle", "fediverse")),
    ("bluesky", "Bluesky", ("bluesky", "bsky", "bluesky_handle", "at_protocol")),
    ("linkedin", "LinkedIn", ("linkedin", "linked_in", "linkedin_url",
                              "linkedin_profile")),
    ("github", "GitHub", ("github", "git_hub", "github_handle",
                          "github_username")),
    ("website", "Website", ("website", "web_site", "url", "homepage",
                            "company_website")),
    ("location", "Location", ("location", "city", "country", "region")),
    ("flow", "Flow", ("flow", "pipeline")),
    ("stage", "Stage", ("stage", "status")),
    ("value", "Deal value", ("value", "deal_value", "amount", "deal_amount",
                             "revenue", "mrr", "arr")),
    ("nextStep", "Next step", ("next_step", "next_steps", "next_action",
                               "follow_up", "followup")),
    ("owner", "Owner", ("owner", "assigned_to", "contact_owner")),
    ("source", "Source", ("source",)),
    ("tags", "Tags", ("tags",)),
    ("notes", "Notes", ("notes",)),
    ("lastContactedAt", "Last contacted", ("last_contacted_at",)),
)
CONTACT_COLUMN_FIELDS = frozenset(field for field, _, _ in CONTACT_COLUMNS)
CONTACT_COLUMN_LABELS = {field: label for field, label, _ in CONTACT_COLUMNS}
DELETABLE_COLUMN_FIELDS = frozenset(
    CONTACT_COLUMN_FIELDS - {"name", "email", "flow", "stage", "lastContactedAt"})
CSV_ALIAS_KEYS = frozenset(
    alias for _, _, aliases in CONTACT_COLUMNS for alias in aliases)
NAME_PART_KEYS = ("first_name", "given_name", "last_name", "family_name")
# What a CSV column may be imported as: a contact column, one half of a split
# name, its own custom column ("custom"), or nothing at all ("").
CSV_NAME_PART_TARGETS = ("first_name", "last_name")
CSV_MAPPING_TARGETS = CONTACT_COLUMN_FIELDS | frozenset(
    CSV_NAME_PART_TARGETS + ("custom", ""))
# Header words that point at a contact column when no exact spelling matched,
# checked in this order so "Company name" is a company and "Last contacted" is
# not a surname. Each guess claims a column only once.
CSV_GUESS_WORDS = (
    ("email", ("email", "emails", "mail", "e_mail")),
    ("linkedin", ("linkedin",)),
    ("twitter", ("twitter", "tweet")),
    ("mastodon", ("mastodon", "fediverse", "fedi")),
    ("bluesky", ("bluesky", "bsky")),
    ("github", ("github",)),
    ("owner", ("owner", "assigned", "assignee")),
    ("company", ("company", "organization", "organisation", "org", "employer",
                 "business", "firm", "account", "publication", "publisher",
                 "outlet", "newspaper", "magazine")),
    ("first_name", ("first", "given", "forename")),
    ("last_name", ("surname", "family")),
    ("title", ("title", "position", "role", "job", "occupation")),
    ("phone", ("phone", "mobile", "cell", "tel", "telephone", "whatsapp")),
    ("website", ("website", "site", "web", "url", "domain", "homepage")),
    ("location", ("location", "city", "country", "region", "state", "address",
                  "town")),
    ("stage", ("stage", "status")),
    ("flow", ("pipeline", "flow")),
    ("value", ("value", "amount", "revenue", "mrr", "arr", "budget", "worth")),
    ("nextStep", ("next", "followup", "follow")),
    ("source", ("source", "origin", "referral")),
    ("tags", ("tag", "tags", "label", "labels", "category", "categories")),
    ("notes", ("note", "notes", "comment", "comments", "description", "memo")),
    ("name", ("name", "contact", "person", "full", "lead", "founder")),
)
# Header words also found inside a longer run-together header ("EmailAddress",
# "CompanyWebsite"); the rest must stand as a word of their own, so "state" is
# not found in "Statement".
CSV_GUESS_SUBSTRINGS = frozenset({
    "email", "linkedin", "twitter", "mastodon", "bluesky", "bsky", "github",
    "company", "website", "phone"})
# A cell that reads like a person's name: two to four words of letters, with
# no digits, handles, slashes or dashes between phrases. Every word starts with
# a capital or is a lowercase particle ("Ludwig van Beethoven").
PERSON_NAME_RE = re.compile(r"^[^\W\d_][\w'’.-]*(?:\s+[^\W\d_][\w'’.-]*){1,3}$")
NAME_PARTICLES = frozenset({
    "al", "bin", "da", "de", "del", "della", "der", "di", "du", "el", "la",
    "le", "st", "ten", "ter", "van", "von"})
VIEW_VISIBILITIES = frozenset({"organization", "private"})
SHARE_ACCESS = ("view", "edit")
SHARED_EDIT_FIELDS = frozenset({
    "name", "email", "company", "title", "phone", "twitter", "mastodon",
    "bluesky", "linkedin", "github", "website", "location", "value",
    "nextStep", "notes", "tags"})
EMAIL_PROVIDERS = frozenset({"gmail", "zoho", "mastodon", "other"})
# A fediverse address, kept as "user@host": "@grace@mastodon.social", the
# bare "grace@mastodon.social", or the profile URL a Mastodon page links to.
HANDLE_RE = re.compile(
    r"@?([A-Za-z0-9_.-]{1,64})@([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+)")
HANDLE_URL_RE = re.compile(
    r"https://([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+)/@([A-Za-z0-9_.-]{1,64})"
    r"(?:@([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+))?")
# The same two shapes found anywhere in a stored field (_handles_in).
HANDLE_URL_ANY_RE = re.compile(
    r"(?:https?://)?([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+)/(?:@|users/)"
    r"([A-Za-z0-9_.-]{1,64})(?:@([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+))?",
    re.IGNORECASE)
HANDLE_ANY_RE = re.compile(
    r"(?<![A-Za-z0-9_./-])@?([A-Za-z0-9_.-]{1,64})\s*@\s*"
    r"([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+)")
EMAIL_DIRECTIONS = frozenset({"inbound", "outbound"})
EMAIL_TRACKED_CELEBRATION = "crm-email-tracked"
SEND_STATUSES = frozenset({
    "draft", "external", "sending", "accepted", "delivery", "delivered",
    "open", "click", "failed", "reject", "bounce", "soft bounce",
    "suspension", "unsubscribe", "spam",
})
MANAGER_ROLES = frozenset({"owner", "admin"})
PLUGIN_ID = "crm"

# Founders outreach. Organization owners and admins, plus the members they put
# on the organization's outreach roster, send one-off asks (sponsorship,
# investment, partnership) from the organization's configured sender through
# the host's Mailtrap capability. Every send lands in crm_outreach_log with
# the recipient and subject sealed at rest, and per-sender daily volume is
# capped so a leaked session cannot quietly turn the shared address into a
# spam cannon.
OUTREACH_FROM_EMAIL = "founders@forkmesh.com"
OUTREACH_FROM_NAME = "ForkMesh Founders"
OUTREACH_DAILY_LIMIT = 50
OUTREACH_MAX_SUBJECT = 200
OUTREACH_MAX_BODY = 10000
OUTREACH_LOG_LIST_LIMIT = 50
OUTREACH_TEMPLATE_KEY_RE = re.compile(r"^[a-z][a-z0-9-]{0,39}$")
# Investor deck links. One token per contact; the App renders the deck at
# DECK_SITE_ORIGIN/pitch-deck/<token> (pitch_deck_runtime.py) and reports
# page views under the same token into the tables below.
DECK_SITE_ORIGIN = "https://forkmesh.com"
DECK_ID = "seed-2026"
DECK_TOKEN_RE = re.compile(r"^[a-z0-9]{10,40}$")
DECK_TOKEN_CHARS = 12
DECK_SESSION_LIST_LIMIT = 50
DECK_LINK_MARK = "/pitch-deck/"
INVESTOR_FLOW_IDS = frozenset({"investors", "investment"})
DECK_PAGE_TITLES = (
    "Cover", "Problem", "Why now", "Product", "Proof", "Roadmap",
    "Customer & go-to-market", "Market", "Business model", "Competition",
    "Team", "The round", "Sources",
)
# Starting points the compose form offers; [bracketed] placeholders are filled
# in by the sender before sending and {fromEmail} becomes the organization's
# sender address. Plain text only: outreach mail should read like a person
# wrote it, not like a marketing blast. There are five usable drafts:
# sponsorship, investment, partnership, strategic support, and follow-up.
OUTREACH_TEMPLATES = (
    {
        "key": "blank",
        "label": "Blank email",
        "subject": "",
        "body": "",
    },
    {
        "key": "sponsorship",
        "label": "Sponsorship ask",
        "subject": "Sponsoring ForkMesh — decentralized code hosting",
        "body": (
            "Hi [name],\n\n"
            "I'm [your name], one of the founders of ForkMesh "
            "(https://forkmesh.com) — an open-source, peer-to-peer network "
            "for hosting and mirroring Git repositories. Eligible mirrors may "
            "receive small, transparent, community-funded incentives, but "
            "selection and payment are never guaranteed.\n\n"
            "We're looking for sponsors to help fund the relay "
            "infrastructure and node payouts, and I thought [company] would "
            "be a great fit because [reason].\n\n"
            "Sponsors get their name and logo on forkmesh.com, a mention in "
            "our release notes and blog, and a direct line to the founding "
            "team as the network grows.\n\n"
            "Would you be open to a short call in the next couple of weeks?\n\n"
            "Best,\n[your name]\nForkMesh — {fromEmail}"
        ),
    },
    {
        "key": "investor",
        "label": "Investor outreach",
        "subject": "ForkMesh — decentralized code hosting (investor intro)",
        "body": (
            "Hi [name],\n\n"
            "I'm [your name], one of the founders of ForkMesh "
            "(https://forkmesh.com) — an open-source, peer-to-peer network for "
            "hosting and mirroring Git repositories. The protocol is MIT-licensed "
            "and independent operators can run their own nodes; any community "
            "incentives are optional, non-custodial, and never guaranteed.\n\n"
            "We're raising [round] to grow the relay infrastructure and node "
            "payout network, and I'm reaching out because [why this investor]. "
            "[Traction so far — nodes online, repos mirrored, revenue, etc.]\n\n"
            "Could I send over our deck, or grab 20 minutes in the next week "
            "or two to walk you through where the network is heading?\n\n"
            "Best,\n[your name]\nForkMesh — {fromEmail}"
        ),
    },
    {
        "key": "partnership",
        "label": "Partnership / integration",
        "subject": "Partnering with ForkMesh",
        "body": (
            "Hi [name],\n\n"
            "I'm [your name], one of the founders of ForkMesh "
            "(https://forkmesh.com) — an open-source, peer-to-peer network "
            "for hosting and mirroring Git repositories.\n\n"
            "We think [company] and ForkMesh could work well together: "
            "[what the partnership or integration would look like].\n\n"
            "Happy to share more about the network and where it's heading — "
            "would a short intro call work for you?\n\n"
            "Best,\n[your name]\nForkMesh — {fromEmail}"
        ),
    },
    {
        "key": "strategic-support",
        "label": "Strategic company support",
        "subject": "A practical way for [company] to support independent Git infrastructure",
        "body": (
            "Hi [name],\n\n"
            "I'm [your name], one of the founders of ForkMesh "
            "(https://forkmesh.com) — an open-source Git-compatible network "
            "for routing, mirroring, and discussing repositories without "
            "making one hosting provider the only place a project can live.\n\n"
            "We're speaking with a small number of companies that care about "
            "[open source / developer infrastructure / software preservation] "
            "and exploring practical ways to support the project: "
            "[relay capacity, mirror operations, compatibility work, or a "
            "defined sponsorship]. We would keep the protocol and self-hosting "
            "path open; this is not an investment, an exclusivity request, or "
            "a promise of network rewards.\n\n"
            "[Company] felt relevant because [specific reason]. Would you be "
            "open to a 20-minute conversation about whether one of those paths "
            "fits your goals?\n\n"
            "Best,\n[your name]\nForkMesh — {fromEmail}"
        ),
    },
    {
        "key": "followup",
        "label": "Follow-up nudge",
        "subject": "Re: ForkMesh",
        "body": (
            "Hi [name],\n\n"
            "Just floating this back to the top of your inbox — I reached "
            "out about [topic] and would still love to hear your thoughts. "
            "No worries if the timing isn't right; happy to reconnect "
            "whenever works.\n\n"
            "Best,\n[your name]\nForkMesh — {fromEmail}"
        ),
    },
)

# The CRM owns its schema: plugin_data runs DATA_SCHEMA_UP when the first
# organization installs the plugin and DATA_SCHEMA_DOWN once the last install
# is gone, fully reversing the plugin's migration. Contact PII, message
# content, sender defaults, and provider links live in encrypted envelopes;
# the blind email index is organization-scoped so imports deduplicate without
# correlating a contact between organizations.
DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS crm_contacts (
        contact_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        email_bi TEXT NOT NULL DEFAULT '',
        created_by_bi TEXT NOT NULL,
        created_by_name TEXT NOT NULL,
        updated_by_bi TEXT NOT NULL,
        updated_by_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_crm_contacts_org_updated "
    "ON crm_contacts(org_bi,updated_at DESC,contact_id)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_crm_contacts_org_email "
    "ON crm_contacts(org_bi,email_bi) WHERE email_bi<>''",
    """CREATE TRIGGER IF NOT EXISTS trg_crm_contact_limit
        BEFORE INSERT ON crm_contacts
        WHEN (SELECT COUNT(*) FROM crm_contacts WHERE org_bi=NEW.org_bi)>=2000
        BEGIN SELECT RAISE(ABORT,'crm_contact_catalog_full'); END""",
    """CREATE TABLE IF NOT EXISTS crm_email_sends (
        send_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        contact_id TEXT REFERENCES crm_contacts(contact_id) ON DELETE SET NULL,
        sender_bi TEXT NOT NULL,
        sender_name TEXT NOT NULL,
        transport TEXT NOT NULL CHECK (transport IN ('mailtrap','external')),
        status TEXT NOT NULL CHECK (status IN
            ('draft','external','sending','accepted','delivery','delivered',
             'open','click','failed','reject','bounce','soft bounce',
             'suspension','unsubscribe','spam')),
        external_id TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        sent_at INTEGER NOT NULL DEFAULT 0 CHECK (sent_at >= 0),
        status_at INTEGER NOT NULL DEFAULT 0 CHECK (status_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_crm_email_sends_org_created "
    "ON crm_email_sends(org_bi,created_at DESC,send_id)",
    "CREATE INDEX IF NOT EXISTS idx_crm_email_sends_contact "
    "ON crm_email_sends(org_bi,contact_id,created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_crm_email_sends_external "
    "ON crm_email_sends(org_bi,external_id) WHERE external_id<>''",
    """CREATE TRIGGER IF NOT EXISTS trg_crm_email_send_limit
        BEFORE INSERT ON crm_email_sends
        WHEN (SELECT COUNT(*) FROM crm_email_sends WHERE org_bi=NEW.org_bi)>=20000
        BEGIN SELECT RAISE(ABORT,'crm_email_catalog_full'); END""",
    """CREATE TABLE IF NOT EXISTS crm_settings (
        org_bi TEXT PRIMARY KEY,
        updated_by_bi TEXT NOT NULL,
        updated_by_name TEXT NOT NULL,
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS crm_views (
        view_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_crm_views_org_created "
    "ON crm_views(org_bi,created_at,view_id)",
    """CREATE TRIGGER IF NOT EXISTS trg_crm_view_limit
        BEFORE INSERT ON crm_views
        WHEN (SELECT COUNT(*) FROM crm_views WHERE org_bi=NEW.org_bi)>=60
        BEGIN SELECT RAISE(ABORT,'crm_view_catalog_full'); END""",
    # Who a sheet is shared with, keyed by a blind index of the ForkMesh user
    # name so "sheets shared with me" is one indexed read across every
    # organization without opening a single sealed view. The names themselves
    # stay inside the sealed view data.
    """CREATE TABLE IF NOT EXISTS crm_view_shares (
        view_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        shared_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        PRIMARY KEY(view_id,shared_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_crm_view_shares_user "
    "ON crm_view_shares(shared_bi,created_at,view_id)",
    """CREATE TABLE IF NOT EXISTS crm_mail_messages (
        message_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        provider TEXT NOT NULL CHECK (provider IN ('zoho')),
        provider_account TEXT NOT NULL,
        provider_message_id TEXT NOT NULL,
        box TEXT NOT NULL,
        group_key TEXT NOT NULL,
        received_at INTEGER NOT NULL CHECK (received_at >= 0),
        imported_at INTEGER NOT NULL CHECK (imported_at >= 0),
        data TEXT NOT NULL,
        UNIQUE(org_bi,provider,provider_account,provider_message_id))""",
    "CREATE INDEX IF NOT EXISTS idx_crm_mail_org_box_received "
    "ON crm_mail_messages(org_bi,box,received_at DESC,message_id)",
    "CREATE INDEX IF NOT EXISTS idx_crm_mail_org_group "
    "ON crm_mail_messages(org_bi,group_key,received_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_crm_mail_limit
        BEFORE INSERT ON crm_mail_messages
        WHEN (SELECT COUNT(*) FROM crm_mail_messages WHERE org_bi=NEW.org_bi)>=20000
        BEGIN SELECT RAISE(ABORT,'crm_mail_catalog_full'); END""",
    # Founders outreach: the per-organization roster of members allowed to
    # send, and the send log that drives the daily cap and the tracker. The
    # roster keeps public account names in the clear (the same trust level as
    # org_members); the log seals recipient, subject, and provider error.
    """CREATE TABLE IF NOT EXISTS crm_outreach_team (
        org_bi TEXT NOT NULL,
        member_bi TEXT NOT NULL,
        member_name TEXT NOT NULL,
        added_by TEXT NOT NULL,
        added_at INTEGER NOT NULL CHECK (added_at >= 0),
        PRIMARY KEY (org_bi, member_bi))""",
    """CREATE TABLE IF NOT EXISTS crm_outreach_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        org_bi TEXT NOT NULL,
        ts INTEGER NOT NULL CHECK (ts >= 0),
        sender_bi TEXT NOT NULL,
        sender_name TEXT NOT NULL,
        ok INTEGER NOT NULL DEFAULT 1,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_crm_outreach_log_org_ts "
    "ON crm_outreach_log(org_bi,ts DESC,id DESC)",
    "CREATE INDEX IF NOT EXISTS idx_crm_outreach_log_sender_ts "
    "ON crm_outreach_log(org_bi,sender_bi,ts)",
    # Investor deck links: one token per contact, the sessions each token
    # was opened in, and the seconds each session spent on each page. The
    # link row seals the reader's name and email; the sessions keep only
    # what the request itself carried (country, browser agent).
    """CREATE TABLE IF NOT EXISTS crm_deck_links (
        token TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        contact_id TEXT NOT NULL REFERENCES crm_contacts(contact_id) ON DELETE CASCADE,
        deck TEXT NOT NULL DEFAULT 'seed-2026',
        created_by_bi TEXT NOT NULL,
        created_by_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        revoked_at INTEGER NOT NULL DEFAULT 0 CHECK (revoked_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_crm_deck_links_org_contact "
    "ON crm_deck_links(org_bi,contact_id,created_at DESC)",
    """CREATE TABLE IF NOT EXISTS crm_deck_sessions (
        session_id TEXT PRIMARY KEY,
        token TEXT NOT NULL REFERENCES crm_deck_links(token) ON DELETE CASCADE,
        started_at INTEGER NOT NULL CHECK (started_at >= 0),
        last_at INTEGER NOT NULL CHECK (last_at >= 0),
        seconds INTEGER NOT NULL DEFAULT 0 CHECK (seconds >= 0),
        pages INTEGER NOT NULL DEFAULT 0 CHECK (pages >= 0),
        country TEXT NOT NULL DEFAULT '',
        agent TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_crm_deck_sessions_token "
    "ON crm_deck_sessions(token,started_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_crm_deck_session_limit
        BEFORE INSERT ON crm_deck_sessions
        WHEN (SELECT COUNT(*) FROM crm_deck_sessions WHERE token=NEW.token)>=1000
        BEGIN SELECT RAISE(ABORT,'crm_deck_sessions_full'); END""",
    """CREATE TABLE IF NOT EXISTS crm_deck_page_views (
        session_id TEXT NOT NULL REFERENCES crm_deck_sessions(session_id) ON DELETE CASCADE,
        token TEXT NOT NULL,
        page INTEGER NOT NULL CHECK (page >= 1),
        seconds INTEGER NOT NULL DEFAULT 0 CHECK (seconds >= 0),
        first_at INTEGER NOT NULL CHECK (first_at >= 0),
        last_at INTEGER NOT NULL CHECK (last_at >= 0),
        PRIMARY KEY (session_id, page))""",
    "CREATE INDEX IF NOT EXISTS idx_crm_deck_page_views_token_page "
    "ON crm_deck_page_views(token,page)",
    """CREATE TABLE IF NOT EXISTS crm_list_activity (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        org_bi TEXT NOT NULL,
        view_id TEXT NOT NULL,
        ts INTEGER NOT NULL CHECK (ts >= 0),
        actor_bi TEXT NOT NULL,
        actor_name TEXT NOT NULL,
        action TEXT NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_crm_list_activity_view_ts "
    "ON crm_list_activity(org_bi,view_id,ts DESC,id DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_crm_list_activity_keep
        AFTER INSERT ON crm_list_activity
        BEGIN DELETE FROM crm_list_activity WHERE org_bi=NEW.org_bi
            AND view_id=NEW.view_id AND ts<NEW.ts-%d; END""" % LIST_ACTIVITY_KEEP_MS,
]
# Indexes and triggers drop with their tables; children drop before contacts.
DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS crm_list_activity",
    "DROP TABLE IF EXISTS crm_deck_page_views",
    "DROP TABLE IF EXISTS crm_deck_sessions",
    "DROP TABLE IF EXISTS crm_deck_links",
    "DROP TABLE IF EXISTS crm_outreach_log",
    "DROP TABLE IF EXISTS crm_outreach_team",
    "DROP TABLE IF EXISTS crm_view_shares",
    "DROP TABLE IF EXISTS crm_mail_messages",
    "DROP TABLE IF EXISTS crm_views",
    "DROP TABLE IF EXISTS crm_email_sends",
    "DROP TABLE IF EXISTS crm_contacts",
    "DROP TABLE IF EXISTS crm_settings",
]
EXPORT_EMAIL_LIMIT = 20000


# Organizations whose schema this isolate has already replayed. The replay is
# idempotent but it is also thirty D1 statements, and running all of them ahead
# of every read made one contact fetch cost far more than the contact: the
# Worker answered 1102 (resource limits exceeded) on plain GETs. A fresh
# isolate still replays once, which is what upgrades an organization that
# installed an earlier release, so a deploy cannot skip a migration.
_SCHEMA_REPLAYED = set()


async def _ensure_data_schema(runtime, org_bi):
    """Replay the plugin's idempotent schema once per isolate per org."""
    if org_bi in _SCHEMA_REPLAYED:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    if len(_SCHEMA_REPLAYED) < 10000:
        _SCHEMA_REPLAYED.add(org_bi)


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


def _text(value, limit):
    return str(value or "").replace("\x00", "")[:limit]


def _clean(value, limit):
    return " ".join(_text(value, limit * 2).split())[:limit]


def _email(value, limit=254):
    value = _clean(value, min(254, int(limit or 254))).lower()
    return value if EMAIL_RE.fullmatch(value) else ""


def _handle(value, limit=320):
    """A fediverse handle as "user@host", lowercase; "" when it is not one.
    A profile URL names the account too: the host in the path wins over the
    site serving it, so a remote profile keeps its own instance."""
    value = _clean(value, limit)
    if not value:
        return ""
    url = HANDLE_URL_RE.fullmatch(value.rstrip("/"))
    if url:
        return ("%s@%s" % (url.group(2), url.group(3) or url.group(1))).lower()
    match = HANDLE_RE.fullmatch(value)
    return ("%s@%s" % (match.group(1), match.group(2))).lower() if match else ""


def _https(value):
    value = _clean(value, 1000)
    return value if not value or HTTPS_RE.fullmatch(value) else ""


def _integer(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _tags(value):
    source = value if isinstance(value, list) else re.split(r"[,;]", str(value or ""))
    result = []
    for item in source:
        tag = _clean(item, 40).lower()
        if tag and tag not in result:
            result.append(tag)
    return result[:20]


def _custom_fields(value, existing=None):
    """Merge supplied custom columns into the stored ones. A key the caller did
    not send keeps its value, so a narrow CSV never blanks a wider record."""
    result = dict(existing if isinstance(existing, dict) else {})
    source = value if isinstance(value, dict) else {}
    for key, item in source.items():
        key = _csv_key(key)
        if not FIELD_KEY_RE.fullmatch(key or "") or key in CONTACT_COLUMN_FIELDS:
            continue
        cleaned = _text(item, MAX_CUSTOM_FIELD_BYTES)
        if cleaned.strip():
            result[key] = cleaned
        else:
            result.pop(key, None)
        if len(result) >= MAX_CUSTOM_FIELDS:
            break
    return dict(list(result.items())[:MAX_CUSTOM_FIELDS])


def _view_ids(value, existing=None):
    """The sheets a contact belongs to, newest membership last."""
    result = [item for item in (existing or []) if CONTACT_ID_RE.fullmatch(str(item))]
    source = value if isinstance(value, list) else [value]
    for item in source:
        item = str(item or "")
        if CONTACT_ID_RE.fullmatch(item) and item not in result:
            result.append(item)
    return result[-MAX_VIEWS:]


def _slug(value, fallback=""):
    value = re.sub(r"[^a-z0-9]+", "-", _clean(value, 80).lower()).strip("-")[:40]
    return value if SLUG_RE.fullmatch(value) else fallback


def _deal_value(value):
    if isinstance(value, bool):
        return 0
    if isinstance(value, (int, float)):
        return int(value)
    digits = re.sub(r"[^0-9.]", "", str(value or "")).split(".")
    if not digits[0]:
        return 0
    try:
        return int(digits[0])
    except ValueError:
        return 0


def _stage(value):
    return _slug(value, "lead")


def _flow_placement(contact, flows):
    flow = next((item for item in flows if item["id"] == contact.get("flow")),
                flows[0])
    stage = contact.get("stage")
    return dict(contact, flow=flow["id"],
                stage=stage if stage in flow["stages"] else flow["stages"][0])


def _pipeline_moved(data):
    return isinstance(data, dict) and ("flow" in data or "stage" in data)


async def _org_flows(runtime, org_bi):
    return (await _settings(runtime, org_bi))["flows"]


def _default_flows():
    return [{"id": flow["id"], "name": flow["name"], "icon": flow["icon"],
             "stages": list(flow["stages"])} for flow in DEFAULT_FLOWS]


def _flow_icon(value, flow_id, existing=None):
    """A flow's icon: the one asked for, else the one it already had (a
    client that does not know icons saves flows without them), else the
    starter flow's, else the plain funnel."""
    icon = _clean(value, 20).lower()
    if icon in FLOW_ICONS:
        return icon
    for item in existing if isinstance(existing, list) else []:
        if isinstance(item, dict) and item.get("id") == flow_id:
            kept = _clean(item.get("icon"), 20).lower()
            if kept in FLOW_ICONS:
                return kept
    for flow in DEFAULT_FLOWS:
        if flow["id"] == flow_id:
            return flow["icon"]
    return "pipeline"


def _flows(value, existing=None):
    if not isinstance(value, list):
        return _default_flows()
    result = []
    seen = set()
    for index, item in enumerate(value[:12]):
        item = item if isinstance(item, dict) else {}
        flow_id = _slug(item.get("id") or item.get("name"), "")
        if not flow_id or flow_id in seen:
            continue
        stages = []
        source = item.get("stages") if isinstance(item.get("stages"), list) else []
        for stage in source[:30]:
            clean = _slug(stage, "")
            if clean and clean not in stages:
                stages.append(clean)
        if not stages:
            stages = ["lead"]
        seen.add(flow_id)
        result.append({
            "id": flow_id,
            "name": _clean(item.get("name"), 80) or flow_id.replace("-", " ").title(),
            "icon": _flow_icon(item.get("icon"), flow_id, existing),
            "stages": stages,
        })
    return result or _default_flows()


def _removed_columns(value):
    source = value if isinstance(value, list) else []
    return [field for index, field in enumerate(source)
            if field in DELETABLE_COLUMN_FIELDS and field not in source[:index]]


TABLE_VIEW_BUILTINS = ("default", "all")
TABLE_VIEW_COLUMN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
MAX_TABLE_VIEWS = 40


def _table_views(value):
    views = []
    seen = set(TABLE_VIEW_BUILTINS)
    for item in (value if isinstance(value, list) else [])[:MAX_TABLE_VIEWS]:
        if not isinstance(item, dict):
            continue
        view_id = _slug(item.get("id"))
        name = _clean(item.get("name"), 60)
        if not view_id or not name or view_id in seen:
            continue
        seen.add(view_id)
        columns = []
        source = item.get("columns") if isinstance(item.get("columns"), list) else []
        for field in source[:MAX_VIEW_COLUMNS]:
            field = str(field or "")
            if TABLE_VIEW_COLUMN_RE.fullmatch(field) and field not in columns:
                columns.append(field)
        views.append({"id": view_id, "name": name, "columns": columns})
    return views


def _mail_boxes(value):
    source = value if isinstance(value, list) else DEFAULT_MAIL_BOXES
    result = []
    for item in source[:20]:
        box = _clean(item, 60)
        if box and box.lower() not in {entry.lower() for entry in result}:
            result.append(box)
    return result or list(DEFAULT_MAIL_BOXES)


ZOHO_REGIONS = {
    "com": ("https://accounts.zoho.com", "https://mail.zoho.com"),
    "eu": ("https://accounts.zoho.eu", "https://mail.zoho.eu"),
    "in": ("https://accounts.zoho.in", "https://mail.zoho.in"),
    "com.au": ("https://accounts.zoho.com.au", "https://mail.zoho.com.au"),
    "jp": ("https://accounts.zoho.jp", "https://mail.zoho.jp"),
    "ca": ("https://accounts.zohocloud.ca", "https://mail.zohocloud.ca"),
}
ZOHO_CONNECTION_KEYS = ("accountId", "clientId", "clientSecret", "refreshToken")
ZOHO_SCOPES = "ZohoMail.messages.READ,ZohoMail.folders.READ,ZohoMail.accounts.READ"
ZOHO_CALLBACK_PATH = "/api/crm/-/zoho/callback"
ZOHO_STATE_TTL_MS = 10 * 60 * 1000
ZOHO_SENT_FOLDERS = frozenset({"sent", "sent items", "sent mail", "sent messages"})
CLOSED_DEAL_STAGES = frozenset({
    "won", "closed-won", "customer", "renewal", "committed", "signed", "paid",
    "active"})
UNSENT_STATUSES = ("draft", "sending", "failed", "reject")
ENDED_STAGES = frozenset({
    "passed", "rejected", "closed", "inactive", "hired", "lost", "closed-lost",
    "churned", "declined", "unsubscribed", "do-not-contact"})
EMAIL_SUGGESTION_LIMIT = 12
EMAIL_SUGGESTION_REPLY_DAYS = 60
EMAIL_SUGGESTION_FOLLOW_UP_DAYS = 14
EMAIL_SUGGESTION_INTRO_TEMPLATES = {"investment": "investor", "partnerships": "partnership"}
EMAIL_SUGGESTION_REPLY_BODY = (
    "Hi [name],\n\n"
    "Thanks for getting back to me. [your answer]\n\n"
    "Best,\n[your name]\nForkMesh — {fromEmail}"
)
CRM_PAGE_PATH = "/dashboard/crm"
CALLBACK_ORIGIN_RE = re.compile(
    r"^(?:https://[a-z0-9.-]{1,253}(?::[0-9]{1,5})?"
    r"|http://(?:localhost|127\.0\.0\.1)(?::[0-9]{1,5})?)$")


def _zoho_settings(value, existing=None):
    source = value if isinstance(value, dict) else {}
    result = dict(existing if isinstance(existing, dict) else {})
    for key, limit in (("accountId", 80), ("clientId", 200),
                       ("clientSecret", 500), ("refreshToken", 1000)):
        if key in source and source.get(key):
            result[key] = _clean(source.get(key), limit)
    if source.get("clientSecret"):
        result.pop("managed", None)
    elif source.get("managed") is True:
        result["managed"] = True
        result.pop("clientSecret", None)
    region = _clean(source.get("region", result.get("region", "com")), 20).lower()
    if region not in ZOHO_REGIONS:
        region = "com"
    result["region"] = region
    result["accountsServer"], result["mailServer"] = ZOHO_REGIONS[region]
    return result


def _zoho_sent_folder(folder):
    folder = folder if isinstance(folder, dict) else {}
    return (str(folder.get("folderType") or "").lower() == "sent"
            or str(folder.get("folderName") or folder.get("displayName") or "")
            .strip().lower() in ZOHO_SENT_FOLDERS)


def _zoho_connected(zoho):
    zoho = zoho if isinstance(zoho, dict) else {}
    keys = ("accountId", "refreshToken") if zoho.get("managed") else ZOHO_CONNECTION_KEYS
    return all(zoho.get(key) for key in keys)


def _zoho_client(runtime):
    secret = getattr(runtime, "package_secret", None)
    if secret is None:
        return None
    client_id = _clean(secret("ZOHO_OAUTH_CLIENT_ID"), 200)
    client_secret = _clean(secret("ZOHO_OAUTH_CLIENT_SECRET"), 500)
    redirect_uri = _clean(secret("ZOHO_OAUTH_REDIRECT_URI"), 300)
    if not (redirect_uri.endswith(ZOHO_CALLBACK_PATH) and CALLBACK_ORIGIN_RE.fullmatch(
            redirect_uri[:-len(ZOHO_CALLBACK_PATH)].lower())):
        redirect_uri = ""
    return ({"clientId": client_id, "clientSecret": client_secret,
             "redirectUri": redirect_uri}
            if client_id and client_secret else None)


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return "organizations", "", "", ""
    # "-" can never be an organization name, so it carries the surfaces that
    # span organizations: the sheets other ForkMesh users shared with me.
    shared = re.fullmatch(r"/api/crm/-/shared(?:/([0-9a-f]{32}))?", clean)
    if shared:
        return "shared", "", "shared", shared.group(1) or ""
    edits = re.fullmatch(
        r"/api/crm/-/shared/([0-9a-f]{32})/contacts(?:/([0-9a-f]{32}))?", clean)
    if edits:
        return "shared-contacts", "", edits.group(1), edits.group(2) or ""
    if clean == ZOHO_CALLBACK_PATH:
        return "zoho-callback", "", "zoho", "callback"
    opened = re.fullmatch(r"/api/crm/-/open/([0-9a-f]{32})", clean)
    if opened:
        return "open", "", "open", opened.group(1)
    match = re.fullmatch(
        r"/api/crm/([a-z][a-z0-9-]{0,62})"
        r"(?:/(contacts|emails|mail|settings|views|outreach|decks|zoho)"
        r"(?:/([0-9a-f]{32}|import|export|send|lookup|link|copy|order|team|bulk|merge"
        r"|duplicates|evidence|activity"
        r"|connect|exchange"
        r"|delete-column"
        r"|[a-z0-9]{10,40}))?)?",
        clean,
    )
    if not match:
        return None
    return "organization", match.group(1), match.group(2) or "", match.group(3) or ""


def _contact_data(data, existing=None):
    result = dict(existing or {})
    fields = (
        ("name", 160, _clean),
        ("email", 254, _email),
        ("company", 160, _clean),
        ("title", 160, _clean),
        ("phone", 80, _clean),
        ("twitter", 320, _clean),
        ("mastodon", 320, _clean),
        ("bluesky", 320, _clean),
        ("linkedin", 320, _clean),
        ("github", 160, _clean),
        ("website", 320, _clean),
        ("location", 160, _clean),
        ("owner", 64, _clean),
        ("source", 120, _clean),
        ("notes", 20000, _text),
    )
    for key, limit, cleaner in fields:
        if key in data:
            result[key] = cleaner(data.get(key), limit)
    if "value" in data or not existing:
        result["value"] = max(0, _integer(_deal_value(data.get("value"))))
    if "nextStep" in data or not existing:
        result["nextStep"] = _clean(data.get("nextStep"), 200)
    if "stage" in data or not existing:
        result["stage"] = _stage(data.get("stage"))
    if "flow" in data or not existing:
        result["flow"] = _slug(data.get("flow"), "customers")
    if "tags" in data or not existing:
        result["tags"] = _tags(data.get("tags"))
    if "lastContactedAt" in data or not existing:
        result["lastContactedAt"] = max(0, _integer(data.get("lastContactedAt")))
    if "fields" in data or not existing:
        result["fields"] = _custom_fields(data.get("fields"), result.get("fields"))
    # `views` only ever adds, so a caller that means "these and no others"
    # says so with `setViews`, and one dropping a membership says
    # `removeViews`. Moving a contact between lists is both at once.
    if "setViews" in data:
        result["views"] = _view_ids(data.get("setViews"), [])
    elif "views" in data:
        result["views"] = _view_ids(data.get("views"), result.get("views"))
    elif not existing:
        result["views"] = []
    if "removeViews" in data:
        dropped = set(_view_ids(data.get("removeViews"), []))
        result["views"] = [item for item in result.get("views") or []
                           if item not in dropped]
    if "addTags" in data:
        result["tags"] = _tags(_tags(result.get("tags")) + _tags(data.get("addTags")))
    if "removeTags" in data:
        dropped = set(_tags(data.get("removeTags")))
        result["tags"] = [tag for tag in _tags(result.get("tags")) if tag not in dropped]
    return result


# Where a click on a contact's email address opens a compose window. Zoho is
# the organization default; Gmail is the other web mailbox the browser
# extension already understands.
MAIL_PROVIDERS = ("zoho", "gmail")


def _mail_provider(value):
    value = _clean(value, 16).lower()
    return value if value in MAIL_PROVIDERS else MAIL_PROVIDERS[0]


def _settings_data(data, existing=None):
    result = dict(existing or {})
    for key, limit in (
        ("providerName", 80),
        ("providerUrlTemplate", 1000),
        ("fromName", 120),
        ("fromEmail", 254),
    ):
        if key in data:
            result[key] = _clean(data.get(key), limit)
    result["providerUrlTemplate"] = _https(result.get("providerUrlTemplate"))
    result["fromEmail"] = _email(result.get("fromEmail"))
    if "mailProvider" in data or "mailProvider" not in result:
        result["mailProvider"] = _mail_provider(
            data.get("mailProvider", result.get("mailProvider")))
    if "flows" in data or "flows" not in result:
        result["flows"] = _flows(data.get("flows"), result.get("flows"))
    else:
        result["flows"] = _flows(result.get("flows"))
    if "mailBoxes" in data or "mailBoxes" not in result:
        result["mailBoxes"] = _mail_boxes(data.get("mailBoxes"))
    else:
        result["mailBoxes"] = _mail_boxes(result.get("mailBoxes"))
    result["removedColumns"] = _removed_columns(
        data.get("removedColumns", result.get("removedColumns")))
    result["tableViews"] = _table_views(
        data.get("tableViews", result.get("tableViews")))
    if data.get("zohoDisconnect"):
        result.pop("zoho", None)
    elif "zoho" in data:
        result["zoho"] = _zoho_settings(data.get("zoho"), result.get("zoho"))
    return result


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    actor = _clean((record or {}).get("name"), 64).lower()
    return str(account_bi or ""), actor


async def _organizations(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT o.name,m.role,p.plugin AS installed FROM org_members m "
        "JOIN orgs o ON o.org_bi=m.org_bi "
        "LEFT JOIN org_plugin_installs p ON p.org_bi=o.org_bi AND p.plugin=? "
        "WHERE m.member_bi=? ORDER BY o.name COLLATE NOCASE LIMIT 200",
        PLUGIN_ID, account_bi,
    )
    return [{
        "name": str(row.get("name") or ""),
        "role": str(row.get("role") or "member"),
        "installed": bool(row.get("installed")),
    } for row in rows or []]


async def _contact_row(runtime, org_bi, contact_id):
    return await runtime.d1_first(
        "SELECT contact_id,org_bi,org_name,created_by_name,updated_by_name,"
        "created_at,updated_at,data FROM crm_contacts "
        "WHERE org_bi=? AND contact_id=?",
        org_bi, contact_id,
    )


async def _contact_project(runtime, row):
    return _contact_fields(row, await runtime.open(row.get("data")))


def _contact_fields(row, data):
    data = data if isinstance(data, dict) else {}
    return {
        "id": str(row.get("contact_id") or ""),
        "organization": str(row.get("org_name") or ""),
        "name": str(data.get("name") or ""),
        "email": str(data.get("email") or ""),
        "company": str(data.get("company") or ""),
        "title": str(data.get("title") or ""),
        "phone": str(data.get("phone") or ""),
        "twitter": str(data.get("twitter") or ""),
        "mastodon": str(data.get("mastodon") or ""),
        "bluesky": str(data.get("bluesky") or ""),
        "linkedin": str(data.get("linkedin") or ""),
        "github": str(data.get("github") or ""),
        "website": str(data.get("website") or ""),
        "location": str(data.get("location") or ""),
        "flow": _slug(data.get("flow"), "customers"),
        "stage": _stage(data.get("stage")),
        "value": max(0, _integer(data.get("value"))),
        "nextStep": str(data.get("nextStep") or ""),
        "tags": _tags(data.get("tags")),
        "owner": str(data.get("owner") or ""),
        "source": str(data.get("source") or ""),
        "notes": str(data.get("notes") or ""),
        "lastContactedAt": _integer(data.get("lastContactedAt")),
        "fields": _custom_fields(data.get("fields")),
        "views": _view_ids(data.get("views")),
        "account": data.get("account") if isinstance(data.get("account"), dict) else {},
        "createdBy": str(row.get("created_by_name") or ""),
        "updatedBy": str(row.get("updated_by_name") or ""),
        "createdAt": _integer(row.get("created_at")),
        "updatedAt": _integer(row.get("updated_at")),
    }


async def _contacts(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT contact_id,org_bi,org_name,created_by_name,updated_by_name,"
        "created_at,updated_at,data FROM crm_contacts WHERE org_bi=? "
        "ORDER BY updated_at DESC,contact_id LIMIT ?",
        org_bi, MAX_CONTACTS,
    )
    return [await _contact_project(runtime, row) for row in rows or []]


async def _email_row(runtime, org_bi, send_id):
    return await runtime.d1_first(
        "SELECT send_id,org_bi,org_name,contact_id,sender_name,transport,status,"
        "external_id,created_at,sent_at,status_at,data FROM crm_email_sends "
        "WHERE org_bi=? AND send_id=?",
        org_bi, send_id,
    )


async def _email_project(runtime, row, include_content=True):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    result = {
        "id": str(row.get("send_id") or ""),
        "organization": str(row.get("org_name") or ""),
        "contactId": str(row.get("contact_id") or ""),
        "sender": str(row.get("sender_name") or ""),
        "transport": str(row.get("transport") or "external"),
        "status": str(row.get("status") or "external"),
        "externalId": str(row.get("external_id") or ""),
        "to": str(data.get("to") or ""),
        "fromEmail": str(data.get("fromEmail") or ""),
        "fromName": str(data.get("fromName") or ""),
        "subject": str(data.get("subject") or ""),
        "providerUrl": str(data.get("providerUrl") or ""),
        "provider": _email_provider(data.get("provider")),
        "direction": _email_direction(data.get("direction")),
        "threadId": str(data.get("threadId") or ""),
        "mailbox": str(data.get("mailbox") or ""),
        "handle": str(data.get("handle") or ""),
        "account": str(data.get("account") or ""),
        "error": str(data.get("error") or ""),
        "createdAt": _integer(row.get("created_at")),
        "sentAt": _integer(row.get("sent_at")),
        "statusAt": _integer(row.get("status_at")),
    }
    if include_content:
        result.update({
            "text": str(data.get("text") or ""),
            "html": str(data.get("html") or ""),
        })
    return result


async def _emails(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT send_id,org_bi,org_name,contact_id,sender_name,transport,status,"
        "external_id,created_at,sent_at,status_at,data FROM crm_email_sends "
        "WHERE org_bi=? ORDER BY created_at DESC,send_id LIMIT ?",
        org_bi, MAX_EMAILS,
    )
    return [await _email_project(runtime, row, include_content=False)
            for row in rows or []]


async def _opened_settings(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT data FROM crm_settings WHERE org_bi=?", org_bi)
    data = await runtime.open(row.get("data")) if row else {}
    return _settings_data(data if isinstance(data, dict) else {})


async def _mail_message_project(runtime, row):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    return {
        "id": str(row.get("message_id") or ""),
        "provider": str(row.get("provider") or "zoho"),
        "providerMessageId": str(row.get("provider_message_id") or ""),
        "box": str(row.get("box") or "Inbox"),
        "groupKey": str(row.get("group_key") or ""),
        "subject": str(data.get("subject") or ""),
        "summary": str(data.get("summary") or ""),
        "sender": str(data.get("sender") or ""),
        "from": str(data.get("from") or ""),
        "to": str(data.get("to") or ""),
        "sourceFolder": str(data.get("sourceFolder") or ""),
        "threadId": str(data.get("threadId") or ""),
        "flag": str(data.get("flag") or ""),
        "hasAttachment": bool(data.get("hasAttachment")),
        "providerUrl": str(data.get("providerUrl") or ""),
        "receivedAt": _integer(row.get("received_at")),
        "importedAt": _integer(row.get("imported_at")),
    }


async def _mail_messages(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT message_id,provider,provider_message_id,box,group_key,"
        "received_at,imported_at,data FROM crm_mail_messages WHERE org_bi=? "
        "ORDER BY received_at DESC,message_id LIMIT ?",
        org_bi, MAX_MAIL_MESSAGES,
    )
    return [await _mail_message_project(runtime, row) for row in rows or []]


async def _mail_message_row(runtime, org_bi, message_id):
    return await runtime.d1_first(
        "SELECT message_id,provider,provider_message_id,box,group_key,"
        "received_at,imported_at,data FROM crm_mail_messages "
        "WHERE org_bi=? AND message_id=?", org_bi, message_id)


async def _move_mail(runtime, row, org_bi, actor, data):
    settings = await _opened_settings(runtime, org_bi)
    boxes = _mail_boxes(settings.get("mailBoxes"))
    requested = _clean(data.get("box"), 60)
    box = next((item for item in boxes if item.lower() == requested.lower()), "")
    if not box:
        return _response(runtime, {"error": "invalid_mail_box"}, 400)
    await runtime.d1_run(
        "UPDATE crm_mail_messages SET box=? WHERE org_bi=? AND message_id=?",
        box, org_bi, row["message_id"])
    await runtime.audit(actor, "crm.mail.move", "crm_mail_message", row["message_id"])
    return _response(runtime, {
        "ok": True,
        "message": await _mail_message_project(
            runtime, await _mail_message_row(runtime, org_bi, row["message_id"])),
    })


def _mail_box(message):
    flag = _clean(message.get("flag"), 40).lower()
    folder = _clean(message.get("sourceFolder"), 80).lower()
    if "follow" in flag:
        return "Follow-up"
    if "important" in flag:
        return "Important"
    if "archive" in folder:
        return "Archive"
    return "Inbox"


def _similar_mail_signal(message):
    thread_id = _clean(message.get("threadId"), 100)
    if thread_id:
        return "thread:" + thread_id
    subject = _clean(message.get("subject"), 300).lower()
    subject = re.sub(r"^(?:(?:re|fw|fwd)\s*:\s*)+", "", subject)
    addresses = sorted(filter(None, (
        _email(message.get("from")), _email(message.get("to")))))
    return "similar:" + subject + "\n" + "\n".join(addresses)


async def _zoho_mail_import(runtime, connection, limit):
    """Own the complete Zoho OAuth and Mail API protocol inside the plugin."""
    accounts_server = str(connection.get("accountsServer") or "").rstrip("/")
    mail_server = str(connection.get("mailServer") or "").rstrip("/")
    account_id = _clean(connection.get("accountId"), 80)
    if not accounts_server or not mail_server or not account_id:
        return {"ok": False, "error": "zoho_connection_incomplete"}
    token = await runtime.outbound_json(
        accounts_server + "/oauth/v2/token", "POST",
        {"content-type": "application/x-www-form-urlencoded"},
        urlencode({
            "client_id": str(connection.get("clientId") or ""),
            "client_secret": str(connection.get("clientSecret") or ""),
            "refresh_token": str(connection.get("refreshToken") or ""),
            "grant_type": "refresh_token",
        }))
    token_data = (token or {}).get("data")
    token_data = token_data if isinstance(token_data, dict) else {}
    access_token = str(token_data.get("access_token") or "")
    if (not (token or {}).get("ok") or not 200 <= _integer(
            (token or {}).get("status")) < 300 or not access_token):
        return {"ok": False, "error": "zoho_token_rejected"}
    headers = {"accept": "application/json",
               "authorization": "Zoho-oauthtoken " + access_token}
    folders_result = await runtime.outbound_json(
        mail_server + "/api/accounts/" + quote(account_id, safe="") + "/folders",
        "GET", headers)
    folders_data = (folders_result or {}).get("data")
    folders_data = folders_data if isinstance(folders_data, dict) else {}
    if (not (folders_result or {}).get("ok") or not 200 <= _integer(
            (folders_result or {}).get("status")) < 300):
        return {"ok": False, "error": "zoho_folders_rejected"}
    folders = [folder if isinstance(folder, dict) else {}
               for folder in list(folders_data.get("data") or [])[:20]]
    sent = [folder for folder in folders if _zoho_sent_folder(folder)]
    folders = [folder for folder in folders if not _zoho_sent_folder(folder)] + sent
    messages = []
    wanted = max(1, min(MAX_ZOHO_IMPORT, _integer(limit, 100)))
    received_budget = wanted - (wanted // 2 if sent and wanted > 1 else 0)
    for folder in folders:
        budget = wanted if _zoho_sent_folder(folder) else received_budget
        if len(messages) >= budget:
            continue
        folder_id = _clean(folder.get("folderId") or folder.get("folder_id"), 100)
        if not folder_id:
            continue
        query = urlencode({
            "folderId": folder_id, "start": 1,
            "limit": min(200, budget - len(messages)), "includeto": "true",
            "sortBy": "date", "sortorder": "false",
        })
        result = await runtime.outbound_json(
            mail_server + "/api/accounts/" + quote(account_id, safe="")
            + "/messages/view?" + query, "GET", headers)
        payload = (result or {}).get("data")
        payload = payload if isinstance(payload, dict) else {}
        if (not (result or {}).get("ok") or not 200 <= _integer(
                (result or {}).get("status")) < 300):
            continue
        folder_name = str(folder.get("folderName") or folder.get("displayName")
                          or folder.get("folderType") or "")
        for item in list(payload.get("data") or []):
            item = item if isinstance(item, dict) else {}
            message_id = _clean(item.get("messageId"), 120)
            if not message_id:
                continue
            messages.append({
                "providerMessageId": message_id,
                "threadId": str(item.get("threadId") or ""),
                "subject": str(item.get("subject") or ""),
                "summary": str(item.get("summary") or ""),
                "sender": str(item.get("sender") or ""),
                "from": str(item.get("fromAddress") or ""),
                "to": str(item.get("toAddress") or ""),
                "sourceFolder": folder_name,
                "sent": _zoho_sent_folder(folder),
                "flag": str(item.get("flagid") or ""),
                "hasAttachment": str(item.get("hasAttachment") or "0").lower()
                    not in ("", "0", "false", "none"),
                "receivedAt": _integer(item.get("receivedTime")
                                       or item.get("sentDateInGMT")),
                "providerUrl": mail_server + "/zm/#mail/folder/"
                    + quote(folder_id, safe="") + "/p/"
                    + quote(message_id, safe=""),
            })
            if len(messages) >= budget:
                break
    return {"ok": True, "messages": messages}


async def _import_zoho(runtime, org_bi, org_name, account_bi, actor, data):
    settings = await _opened_settings(runtime, org_bi)
    zoho = settings.get("zoho") if isinstance(settings.get("zoho"), dict) else {}
    if not _zoho_connected(zoho):
        return _response(runtime, {"error": "zoho_not_connected"}, 409)
    connection = dict(zoho)
    if zoho.get("managed"):
        client = _zoho_client(runtime)
        if not client:
            return _response(runtime, {"error": "zoho_one_click_unavailable"}, 409)
        connection.update(clientId=client["clientId"], clientSecret=client["clientSecret"])
    limit = max(1, min(MAX_ZOHO_IMPORT, _integer(data.get("limit"), 100)))
    result = await _zoho_mail_import(runtime, connection, limit)
    if not (result or {}).get("ok"):
        return _response(runtime, {
            "error": _clean((result or {}).get("error"), 120) or "zoho_unavailable",
        }, 502)
    created = 0
    updated = 0
    now = runtime.now()
    for source in list((result or {}).get("messages") or [])[:limit]:
        source = source if isinstance(source, dict) else {}
        provider_id = _clean(source.get("providerMessageId"), 120)
        if not provider_id:
            continue
        existing = await runtime.d1_first(
            "SELECT message_id FROM crm_mail_messages WHERE org_bi=? AND "
            "provider='zoho' AND provider_account=? AND provider_message_id=?",
            org_bi, zoho["accountId"], provider_id,
        )
        message_id = str((existing or {}).get("message_id") or runtime.new_id())
        group_key = await runtime.blind(
            "crm-mail-group-v1\n" + org_bi + "\n" + _similar_mail_signal(source))
        envelope = {
            "subject": _clean(source.get("subject"), 300),
            "summary": _text(source.get("summary"), 4000),
            "sender": _clean(source.get("sender"), 160),
            "from": _clean(source.get("from"), 500),
            "to": _clean(source.get("to"), 1000),
            "sourceFolder": _clean(source.get("sourceFolder"), 120),
            "sent": bool(source.get("sent")),
            "threadId": _clean(source.get("threadId"), 120),
            "flag": _clean(source.get("flag"), 40),
            "hasAttachment": bool(source.get("hasAttachment")),
            "providerUrl": _https(source.get("providerUrl")),
        }
        received_at = max(0, _integer(source.get("receivedAt")))
        await runtime.d1_run(
            "INSERT INTO crm_mail_messages(message_id,org_bi,org_name,provider,"
            "provider_account,provider_message_id,box,group_key,received_at,"
            "imported_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT("
            "org_bi,provider,provider_account,provider_message_id) DO UPDATE SET "
            "group_key=excluded.group_key,received_at=excluded.received_at,"
            "imported_at=excluded.imported_at,data=excluded.data",
            message_id, org_bi, org_name, "zoho", zoho["accountId"], provider_id,
            _mail_box(source), group_key, received_at, now,
            await runtime.seal(envelope),
        )
        updated += 1 if existing else 0
        created += 0 if existing else 1
    zoho["lastSyncAt"] = now
    settings["zoho"] = zoho
    await runtime.d1_run(
        "UPDATE crm_settings SET updated_by_bi=?,updated_by_name=?,updated_at=?,"
        "data=? WHERE org_bi=?",
        account_bi, actor, now, await runtime.seal(settings), org_bi,
    )
    await runtime.audit(
        actor, "crm.zoho.import", "organization", org_name,
        details={"created": created, "updated": updated},
    )
    return _response(runtime, {
        "ok": True, "created": created, "updated": updated,
        "mail": await _mail_messages(runtime, org_bi),
        "settings": await _settings(runtime, org_bi),
    })


async def email_channel_report(runtime, org_bi, since):
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return {"status": "not_installed"}
    since = max(0, _integer(since))
    zoho = (await _opened_settings(runtime, org_bi)).get("zoho")
    zoho = zoho if isinstance(zoho, dict) else {}
    sends = await runtime.d1_all(
        "SELECT external_id,data FROM crm_email_sends WHERE org_bi=? AND "
        "CASE WHEN sent_at>0 THEN sent_at ELSE created_at END>=? AND status NOT IN "
        "(" + ",".join("?" * len(UNSENT_STATUSES)) + ") LIMIT 20000",
        org_bi, since, *UNSENT_STATUSES)
    counted = set()
    crm_outbound = 0
    replies = 0
    for row in sends or []:
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        if _email_direction(data.get("direction")) == "inbound":
            replies += 1
            continue
        crm_outbound += 1
        if row.get("external_id"):
            counted.add(str(row["external_id"]))
    outreach = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM crm_outreach_log WHERE org_bi=? AND ts>=? AND ok=1",
        org_bi, since)
    crm_outbound += _integer((outreach or {}).get("n"))
    zoho_outbound = 0
    mail = await runtime.d1_all(
        "SELECT provider_message_id,data FROM crm_mail_messages WHERE org_bi=? AND "
        "provider='zoho' AND received_at>=? LIMIT 20000", org_bi, since)
    for row in mail or []:
        if str(row.get("provider_message_id") or "") in counted:
            continue
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        if data.get("sent") or _zoho_sent_folder({"folderName": data.get("sourceFolder")}):
            zoho_outbound += 1
    closed = 0
    closed_value = 0
    contacts = await runtime.d1_all(
        "SELECT data FROM crm_contacts WHERE org_bi=? LIMIT 2000", org_bi)
    for row in contacts or []:
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        if _slug(data.get("stage"), "") in CLOSED_DEAL_STAGES:
            closed += 1
            closed_value += max(0, _integer(data.get("value")))
    return {
        "status": "ok",
        "outbound": crm_outbound + zoho_outbound,
        "outboundCrm": crm_outbound,
        "outboundZoho": zoho_outbound,
        "replies": replies,
        "closedDeals": closed,
        "closedValue": closed_value,
        "closedStages": sorted(CLOSED_DEAL_STAGES),
        "zoho": {
            "connected": _zoho_connected(zoho),
            "lastSyncAt": _integer(zoho.get("lastSyncAt")),
            "mailServer": _https(zoho.get("mailServer")),
        },
    }


def _suggestion_compose_url(settings, address, subject, body):
    if settings.get("mailProvider") == "gmail":
        return "https://mail.google.com/mail/?" + urlencode(
            {"view": "cm", "fs": "1", "to": address, "su": subject, "body": body},
            quote_via=quote)
    zoho = settings.get("zoho") if isinstance(settings.get("zoho"), dict) else {}
    host = ZOHO_REGIONS.get(str(zoho.get("region") or ""), ZOHO_REGIONS["com"])[1]
    mailto = "mailto:" + quote(address, safe="@") + "?" + urlencode(
        {"subject": subject, "body": body}, quote_via=quote)
    return host + "/zm/comp.do?ct=" + quote(mailto, safe="")


def _suggestion_text(value, contact, sender_name, from_email):
    name = str(contact.get("name") or "").strip()
    first = name.split()[0] if name else ""
    company = str(contact.get("company") or "").strip()
    value = value.replace("{fromEmail}", from_email)
    for placeholder, filled in (("[name]", first), ("[company]", company),
                                ("[your name]", sender_name)):
        if filled:
            value = value.replace(placeholder, filled)
    return value


async def email_suggestions(runtime, org_bi, now, limit=EMAIL_SUGGESTION_LIMIT):
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return {"status": "not_installed"}
    now = max(0, _integer(now))
    settings = await _opened_settings(runtime, org_bi)
    from_email, _ = _outreach_sender(settings)
    sender_name = _clean(settings.get("fromName"), 120)
    templates = {template["key"]: template for template in OUTREACH_TEMPLATES}
    inbound = {}
    outbound = {}
    sends = await runtime.d1_all(
        "SELECT contact_id,sent_at,created_at,data FROM crm_email_sends WHERE org_bi=? "
        "AND contact_id IS NOT NULL AND contact_id<>'' AND "
        "CASE WHEN sent_at>0 THEN sent_at ELSE created_at END>=? AND status NOT IN "
        "(" + ",".join("?" * len(UNSENT_STATUSES)) + ") LIMIT 20000",
        org_bi, now - EMAIL_SUGGESTION_REPLY_DAYS * DAY_MS, *UNSENT_STATUSES)
    for row in sends or []:
        contact_id = str(row.get("contact_id") or "")
        at = _integer(row.get("sent_at")) or _integer(row.get("created_at"))
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        if _email_direction(data.get("direction")) == "inbound":
            if at > inbound.get(contact_id, (0, ""))[0]:
                inbound[contact_id] = (at, str(data.get("subject") or ""))
        else:
            outbound[contact_id] = max(outbound.get(contact_id, 0), at)
    contacts = await runtime.d1_all(
        "SELECT contact_id,created_at,data FROM crm_contacts WHERE org_bi=? LIMIT 2000",
        org_bi)
    ranked = []
    for row in contacts or []:
        contact_id = str(row.get("contact_id") or "")
        contact = await runtime.open(row.get("data"))
        contact = contact if isinstance(contact, dict) else {}
        address = _email(contact.get("email"))
        if not address:
            continue
        stage = _stage(contact.get("stage"))
        flow = _slug(contact.get("flow"), "customers")
        last = _integer(contact.get("lastContactedAt"))
        next_step = _clean(contact.get("nextStep"), 200)
        replied_at, replied_subject = inbound.get(contact_id, (0, ""))
        if replied_at and replied_at > outbound.get(contact_id, 0) and replied_at >= last:
            subject = replied_subject or "ForkMesh"
            if not subject.lower().startswith("re:"):
                subject = "Re: " + subject
            kind, rank, at, body = "reply", 0, replied_at, EMAIL_SUGGESTION_REPLY_BODY
            reason = "Replied, not answered yet"
        elif stage in ENDED_STAGES or stage in CLOSED_DEAL_STAGES:
            continue
        elif last and now - last >= EMAIL_SUGGESTION_FOLLOW_UP_DAYS * DAY_MS:
            template = templates["followup"]
            kind, rank, at = "follow_up", 1, last
            subject, body = template["subject"], template["body"]
            reason = "No contact for {} days".format((now - last) // DAY_MS)
        elif not last and flow in EMAIL_SUGGESTION_INTRO_TEMPLATES:
            template = templates[EMAIL_SUGGESTION_INTRO_TEMPLATES[flow]]
            kind, rank, at = "intro", 2, _integer(row.get("created_at"))
            subject, body = template["subject"], template["body"]
            reason = "Never emailed"
        else:
            continue
        if next_step:
            reason += " · next step: " + next_step
        subject = _suggestion_text(subject, contact, sender_name, from_email)
        body = _suggestion_text(body, contact, sender_name, from_email)
        ranked.append(((rank, 0 if next_step else 1, at if rank else -at), {
            "contactId": contact_id,
            "name": str(contact.get("name") or ""),
            "email": address,
            "company": str(contact.get("company") or ""),
            "flow": flow,
            "stage": stage,
            "nextStep": next_step,
            "kind": kind,
            "reason": reason,
            "at": at,
            "subject": subject,
            "body": body,
            "composeUrl": _suggestion_compose_url(settings, address, subject, body),
        }))
    ranked.sort(key=lambda item: item[0])
    return {
        "status": "ok",
        "provider": settings.get("mailProvider") or MAIL_PROVIDERS[0],
        "zohoConnected": _zoho_connected(settings.get("zoho")),
        "total": len(ranked),
        "suggestions": [item for _, item in ranked[:max(1, _integer(limit))]],
    }


def _redirect(runtime, location):
    return runtime.response(
        {"ok": True, "location": location}, status=302,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"location": location, "x-content-type-options": "nosniff",
                       "referrer-policy": "no-referrer"})


async def _zoho_connect(runtime, org_name, account_bi, actor):
    client = _zoho_client(runtime)
    if not client:
        return _response(runtime, {"error": "zoho_one_click_unavailable"}, 409)
    origin = str(runtime.request_origin() or "").rstrip("/").lower()
    redirect_uri = client["redirectUri"] or (
        origin + ZOHO_CALLBACK_PATH if CALLBACK_ORIGIN_RE.fullmatch(origin) else "")
    if not redirect_uri:
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    sealed = await runtime.seal({
        "org": org_name, "account": account_bi, "actor": actor,
        "redirect": redirect_uri, "nonce": runtime.new_id(),
        "expires": runtime.now() + ZOHO_STATE_TTL_MS,
    })
    state = base64.urlsafe_b64encode(str(sealed).encode("utf-8")).decode("ascii").rstrip("=")
    await runtime.audit(actor, "crm.zoho.connect_start", "organization", org_name)
    return _response(runtime, {
        "ok": True,
        "authorizationUrl": ZOHO_REGIONS["com"][0] + "/oauth/v2/auth?" + urlencode({
            "scope": ZOHO_SCOPES, "client_id": client["clientId"],
            "response_type": "code", "access_type": "offline",
            "prompt": "consent", "redirect_uri": redirect_uri, "state": state,
        }),
    })


async def _zoho_state(runtime, value):
    text = str(value or "").strip()
    if not text or len(text) > 8000:
        return None
    try:
        raw = base64.urlsafe_b64decode(text + "=" * (-len(text) % 4)).decode("utf-8")
        state = await runtime.open(raw)
    except Exception:
        return None
    if not isinstance(state, dict) or _integer(state.get("expires")) < runtime.now():
        return None
    if not ORG_RE.fullmatch(str(state.get("org") or "")) or not state.get("account"):
        return None
    return state


def _zoho_region_of(accounts_server):
    origin = str(accounts_server or "").strip().rstrip("/").lower()
    if not origin:
        return "com"
    return next((region for region, (accounts, _) in ZOHO_REGIONS.items()
                 if accounts == origin), "")


async def _zoho_redeem(runtime, accounts_server, fields):
    token = await runtime.outbound_json(
        accounts_server + "/oauth/v2/token", "POST",
        {"content-type": "application/x-www-form-urlencoded"}, urlencode(fields))
    token_data = (token or {}).get("data")
    token_data = token_data if isinstance(token_data, dict) else {}
    access_token = _clean(token_data.get("access_token"), 2000)
    refresh_token = _clean(token_data.get("refresh_token"), 1000)
    if (not (token or {}).get("ok") or not 200 <= _integer((token or {}).get("status")) < 300
            or not access_token or not refresh_token):
        return None, _clean(token_data.get("error"), 80) or "token_rejected"
    return (access_token, refresh_token), ""


async def _zoho_mail_account(runtime, mail_server, access_token):
    listing = await runtime.outbound_json(
        mail_server + "/api/accounts", "GET",
        {"accept": "application/json", "authorization": "Zoho-oauthtoken " + access_token})
    if not (listing or {}).get("ok") or not 200 <= _integer((listing or {}).get("status")) < 300:
        return ""
    listing_data = (listing or {}).get("data")
    accounts = listing_data.get("data") if isinstance(listing_data, dict) else None
    return next((_clean(item.get("accountId"), 80) for item in accounts or []
                 if isinstance(item, dict) and _clean(item.get("accountId"), 80)), "")


async def _zoho_exchange(runtime, org_bi, org_name, account_bi, actor, data):
    current = await runtime.d1_first(
        "SELECT data FROM crm_settings WHERE org_bi=?", org_bi)
    opened = await runtime.open(current.get("data")) if current else {}
    stored = opened.get("zoho") if isinstance(opened, dict) else None
    stored = stored if isinstance(stored, dict) and not stored.get("managed") else {}
    client_id = _clean(data.get("clientId"), 200) or _clean(stored.get("clientId"), 200)
    client_secret = _clean(data.get("clientSecret"), 500)
    if not client_secret and client_id == stored.get("clientId"):
        client_secret = _clean(stored.get("clientSecret"), 500)
    code = _clean(data.get("code"), 2000)
    if not (client_id and client_secret and code):
        return _response(runtime, {"error": "zoho_code_incomplete"}, 400)
    preferred = _clean(data.get("region"), 20).lower()
    regions = ([preferred] if preferred in ZOHO_REGIONS else []) + [
        region for region in ZOHO_REGIONS if region != preferred]
    first_reason = ""
    for region in regions:
        tokens, reason = await _zoho_redeem(runtime, ZOHO_REGIONS[region][0], {
            "grant_type": "authorization_code", "code": code,
            "client_id": client_id, "client_secret": client_secret,
        })
        if tokens:
            break
        first_reason = first_reason or reason
    else:
        return _response(runtime, {
            "error": "zoho_code_rejected", "zohoError": first_reason}, 400)
    await _ensure_data_schema(runtime, org_bi)
    account_id = await _zoho_mail_account(runtime, ZOHO_REGIONS[region][1], tokens[0])
    if not account_id:
        return _response(runtime, {"error": "zoho_account_missing", "region": region}, 400)
    saved = await _save_settings(runtime, org_bi, account_bi, actor, {
        "zoho": {"region": region, "accountId": account_id, "clientId": client_id,
                 "clientSecret": client_secret, "refreshToken": tokens[1]}})
    await runtime.audit(actor, "crm.zoho.connect", "organization", org_name,
                        details={"region": region, "selfClient": True})
    return saved


async def _zoho_callback(runtime):
    params = runtime.query_params()
    state = await _zoho_state(runtime, params.get("state"))
    if not state:
        return _redirect(runtime, CRM_PAGE_PATH + "?zoho=expired")
    org_name = state["org"]
    page = CRM_PAGE_PATH + "?" + urlencode({"org": org_name})
    code = _clean(params.get("code"), 2000)
    if params.get("error") or not code:
        return _redirect(runtime, page + "&zoho=denied")
    org_bi, role = await runtime.organization(org_name, state["account"])
    if (not org_bi or role not in MANAGER_ROLES
            or not await runtime.plugin_installed(org_bi, PLUGIN_ID)):
        return _redirect(runtime, page + "&zoho=forbidden")
    region = _zoho_region_of(params.get("accounts-server"))
    client = _zoho_client(runtime)
    if not region or not client:
        return _redirect(runtime, page + "&zoho=unavailable")
    await _ensure_data_schema(runtime, org_bi)
    accounts_server, mail_server = ZOHO_REGIONS[region]
    tokens, _ = await _zoho_redeem(runtime, accounts_server, {
        "grant_type": "authorization_code", "code": code,
        "client_id": client["clientId"], "client_secret": client["clientSecret"],
        "redirect_uri": str(state.get("redirect") or ""),
    })
    if not tokens:
        return _redirect(runtime, page + "&zoho=token_rejected")
    account_id = await _zoho_mail_account(runtime, mail_server, tokens[0])
    if not account_id:
        return _redirect(runtime, page + "&zoho=account_missing")
    await _save_settings(runtime, org_bi, state["account"], str(state.get("actor") or ""), {
        "zoho": {"region": region, "accountId": account_id,
                 "clientId": client["clientId"], "refreshToken": tokens[1],
                 "managed": True}})
    await runtime.audit(str(state.get("actor") or ""), "crm.zoho.connect",
                        "organization", org_name, details={"region": region})
    return _redirect(runtime, page + "&zoho=connected")


async def _settings(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT updated_by_name,updated_at,data FROM crm_settings WHERE org_bi=?",
        org_bi,
    )
    if not row:
        return {
            "providerName": "", "providerUrlTemplate": "", "fromName": "",
            "fromEmail": "", "mailProvider": MAIL_PROVIDERS[0],
            "updatedBy": "", "updatedAt": 0,
            "flows": _default_flows(), "mailBoxes": list(DEFAULT_MAIL_BOXES),
            "removedColumns": [], "tableViews": [],
            "zoho": {"connected": False, "region": "com", "accountId": "",
                     "oneClick": bool(_zoho_client(runtime)), "managed": False},
            "mailtrapConfigured": bool(runtime.mailtrap_configured()),
        }
    data = await runtime.open(row.get("data"))
    data = _settings_data(data if isinstance(data, dict) else {})
    zoho = data.pop("zoho", {})
    zoho = zoho if isinstance(zoho, dict) else {}
    data["zoho"] = {
        "connected": _zoho_connected(zoho),
        "region": str(zoho.get("region") or "com"),
        "accountId": str(zoho.get("accountId") or ""),
        "lastSyncAt": _integer(zoho.get("lastSyncAt")),
        "oneClick": bool(_zoho_client(runtime)),
        "managed": bool(zoho.get("managed")),
    }
    data.update({
        "updatedBy": str(row.get("updated_by_name") or ""),
        "updatedAt": _integer(row.get("updated_at")),
        "mailtrapConfigured": bool(runtime.mailtrap_configured()),
    })
    return data


def _activity_offset(value):
    offset = _integer(value)
    return max(-ACTIVITY_MAX_OFFSET_MINUTES, min(ACTIVITY_MAX_OFFSET_MINUTES, offset))


async def _activity(runtime, org_bi, members):
    offset = _activity_offset(runtime.query_params().get("tz"))
    shift = offset * 60 * 1000
    today = (runtime.now() + shift) // DAY_MS
    first = today - ACTIVITY_DAYS + 1
    since = first * DAY_MS - shift
    table = {}

    def row_for(name):
        return table.setdefault(
            name, {kind: [0] * ACTIVITY_DAYS for kind in ACTIVITY_KINDS})

    for name in members:
        row_for(name)
    for kind, sql in ACTIVITY_QUERIES:
        for row in await runtime.d1_all(sql, shift, DAY_MS, org_bi, since) or []:
            name = _clean(row.get("name"), 64).lower()
            index = _integer(row.get("day")) - first
            if name and 0 <= index < ACTIVITY_DAYS:
                row_for(name)[kind][index] += max(0, _integer(row.get("n")))
    people = [dict(counts, name=name) for name, counts in table.items()]
    people.sort(key=lambda item: (
        -sum(item["emails"]) - sum(item["outreach"]),
        -sum(item["contacts"]), item["name"]))
    return {
        "tzOffset": offset,
        "days": [datetime.fromtimestamp((first + index) * DAY_MS / 1000,
                                        timezone.utc).strftime("%Y-%m-%d")
                 for index in range(ACTIVITY_DAYS)],
        "kinds": list(ACTIVITY_KINDS),
        "members": people,
    }


async def _summary(runtime, org_bi, org_name, account_bi, actor, role):
    contacts = await _contacts(runtime, org_bi)
    emails = await _emails(runtime, org_bi)
    mail = await _mail_messages(runtime, org_bi)
    settings = await _settings(runtime, org_bi)
    members = await _org_member_names(runtime, org_bi)
    payload = {
        "ok": True,
        "organization": org_name,
        "role": role,
        "canManageSettings": role in MANAGER_ROLES,
        "contacts": contacts,
        "emails": emails,
        "mail": mail,
        "settings": settings,
        "outreach": await _outreach_state(
            runtime, org_bi, account_bi, actor, role, settings),
        "views": await _views(runtime, org_bi, account_bi, actor, role),
        "sharedViews": await _shared_views(runtime, account_bi, actor),
        "members": members,
        "activity": await _activity(runtime, org_bi, members),
        "decks": await _deck_links(runtime, org_bi),
        "summary": {
            "contacts": len(contacts),
            "customers": sum(1 for item in contacts if item["stage"] == "customer"),
            "sent": len(emails),
            "failed": sum(1 for item in emails if item["status"] in (
                "failed", "reject", "bounce", "soft bounce", "suspension")),
            "mail": len(mail),
            "mailGroups": len({item["groupKey"] for item in mail}),
        },
    }
    # This summary carries every contact, send and mail row, and a client only
    # asks for it when the page opens or its Refresh is pressed. It echoes the
    # revision it last applied as ?since=, so a Refresh on an unchanged
    # organization costs a few bytes on the wire and no re-render instead of
    # the whole CRM again.
    revision = hashlib.sha256(json.dumps(
        payload, sort_keys=True, separators=(",", ":")).encode()).hexdigest()[:32]
    if runtime.query_params().get("since") == revision:
        return _response(runtime, {"ok": True, "unchanged": True, "revision": revision})
    payload["revision"] = revision
    return _response(runtime, payload)


async def _unique_email_bi(runtime, org_bi, email):
    if not email:
        return ""
    return await runtime.blind("crm-contact-v1\n" + org_bi + "\n" + email)


async def _create_contact(runtime, org_bi, org_name, account_bi, actor, data):
    contact = _flow_placement(_contact_data(data), await _org_flows(runtime, org_bi))
    if not contact.get("name") and not contact.get("email"):
        return _response(runtime, {"error": "name_or_email_required"}, 400)
    if data.get("email") and not contact.get("email"):
        return _response(runtime, {"error": "invalid_email"}, 400)
    email_bi = await _unique_email_bi(runtime, org_bi, contact.get("email"))
    if email_bi and await runtime.d1_first(
            "SELECT contact_id FROM crm_contacts WHERE org_bi=? AND email_bi=?",
            org_bi, email_bi):
        return _response(runtime, {"error": "contact_email_exists"}, 409)
    contact_id = runtime.new_id()
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO crm_contacts(contact_id,org_bi,org_name,email_bi,"
        "created_by_bi,created_by_name,updated_by_bi,updated_by_name,created_at,"
        "updated_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        contact_id, org_bi, org_name, email_bi, account_bi, actor, account_bi,
        actor, now, now, await runtime.seal(contact),
    )
    await runtime.audit(actor, "crm.contact.create", "crm_contact", contact_id)
    await _record_contact_changes(runtime, org_bi, account_bi, actor,
                                  [(None, contact)])
    return _response(runtime, {
        "ok": True,
        "contact": await _contact_project(
            runtime, await _contact_row(runtime, org_bi, contact_id)),
    }, 201)


async def _update_contact(runtime, row, org_bi, account_bi, actor, data):
    existing = await runtime.open(row.get("data"))
    existing = existing if isinstance(existing, dict) else {}
    contact = _contact_data(data, existing)
    if _pipeline_moved(data):
        contact = _flow_placement(contact, await _org_flows(runtime, org_bi))
    if not contact.get("name") and not contact.get("email"):
        return _response(runtime, {"error": "name_or_email_required"}, 400)
    if "email" in data and data.get("email") and not contact.get("email"):
        return _response(runtime, {"error": "invalid_email"}, 400)
    email_bi = await _unique_email_bi(runtime, org_bi, contact.get("email"))
    duplicate = await runtime.d1_first(
        "SELECT contact_id FROM crm_contacts WHERE org_bi=? AND email_bi=? "
        "AND contact_id<>?",
        org_bi, email_bi, row["contact_id"],
    ) if email_bi else None
    if duplicate:
        return _response(runtime, {"error": "contact_email_exists"}, 409)
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE crm_contacts SET email_bi=?,updated_by_bi=?,updated_by_name=?,"
        "updated_at=?,data=? WHERE org_bi=? AND contact_id=?",
        email_bi, account_bi, actor, now, await runtime.seal(contact), org_bi,
        row["contact_id"],
    )
    await runtime.audit(actor, "crm.contact.update", "crm_contact", row["contact_id"])
    await _record_contact_changes(runtime, org_bi, account_bi, actor,
                                  [(existing, contact)])
    return _response(runtime, {
        "ok": True,
        "contact": await _contact_project(
            runtime, await _contact_row(runtime, org_bi, row["contact_id"])),
    })


async def _bulk_update_contacts(runtime, org_bi, org_name, account_bi, actor, data):
    """Move many contacts in one request: into a pipeline, along its stages, or
    on and off lists. The desktop applies the move to its own rows first and
    sends every edit here together; one PATCH per contact turned a large
    selection into a burst the edge rate-limited."""
    source = data.get("edits")
    if not isinstance(source, list) or not source:
        return _response(runtime, {"error": "edits_required"}, 400)
    if len(source) > MAX_CONTACTS:
        return _response(runtime, {"error": "too_many_edits"}, 413)
    edits = {}
    for item in source:
        item = item if isinstance(item, dict) else {}
        contact_id = str(item.get("id") or "")
        if not CONTACT_ID_RE.fullmatch(contact_id):
            return _response(runtime, {"error": "invalid_contact_id"}, 400)
        edits[contact_id] = {key: item[key]
                             for key in BULK_CONTACT_FIELDS + BULK_TAG_FIELDS
                             if key in item}
    rows = await runtime.d1_all(
        "SELECT contact_id,org_name,created_by_name,created_at,data "
        "FROM crm_contacts WHERE org_bi=? AND contact_id IN "
        "(SELECT value FROM json_each(?))",
        org_bi, json.dumps(list(edits)),
    )
    now = runtime.now()
    sealed, contacts, changes = {}, [], []
    flows = await _org_flows(runtime, org_bi)
    order = {contact_id: index for index, contact_id in enumerate(edits)}
    for row in sorted(rows or [], key=lambda row: order.get(row["contact_id"], 0)):
        existing = await runtime.open(row.get("data"))
        existing = existing if isinstance(existing, dict) else {}
        contact = _contact_data(edits[row["contact_id"]], existing)
        if _pipeline_moved(edits[row["contact_id"]]):
            contact = _flow_placement(contact, flows)
        changes.append((existing, contact))
        sealed[row["contact_id"]] = await runtime.seal(contact)
        contacts.append(_contact_fields(
            dict(row, updated_by_name=actor, updated_at=now), contact))
    await _write_sealed_contacts(runtime, org_bi, account_bi, actor, now, sealed)
    if contacts:
        await runtime.audit(actor, "crm.contacts.bulk_update", "organization",
                            org_name, details={"contacts": len(contacts)})
        await _record_contact_changes(runtime, org_bi, account_bi, actor, changes)
    found = {contact["id"] for contact in contacts}
    return _response(runtime, {
        "ok": True, "contacts": contacts,
        # Deleted by someone else since the page loaded.
        "missing": [contact_id for contact_id in edits if contact_id not in found],
    })


async def _write_sealed_contacts(runtime, org_bi, account_bi, actor, now, sealed):
    """Store many resealed contacts. One UPDATE per chunk rather than per
    contact: the chunk's sealed rows are a JSON object keyed by contact id,
    and each row picks out its own."""
    chunk, size = {}, 0
    for index, (contact_id, value) in enumerate(sealed.items()):
        chunk[contact_id] = value
        size += len(value) + 40
        if size < BULK_WRITE_BYTES and index < len(sealed) - 1:
            continue
        await runtime.d1_run(
            "UPDATE crm_contacts SET updated_by_bi=?1,updated_by_name=?2,"
            "updated_at=?3,data=json_extract(?4,'$.\"'||contact_id||'\"') "
            "WHERE org_bi=?5 AND contact_id IN (SELECT key FROM json_each(?4))",
            account_bi, actor, now, json.dumps(chunk), org_bi,
        )
        chunk, size = {}, 0


async def _delete_custom_column(runtime, org_bi, org_name, account_bi, actor, data):
    """Delete a custom column outright: its value leaves every contact that
    carries it and the column leaves every sheet's saved layout, so it stops
    showing for the whole organization. Built-in columns are part of every
    record and can only be hidden."""
    key = _csv_key(data.get("field"))
    if not FIELD_KEY_RE.fullmatch(key or "") or key in CONTACT_COLUMN_FIELDS:
        return _response(runtime, {"error": "invalid_column"}, 400)
    now = runtime.now()
    sealed = {}
    for row in await runtime.d1_all(
            "SELECT contact_id,data FROM crm_contacts WHERE org_bi=?", org_bi) or []:
        contact = await runtime.open(row.get("data"))
        if not isinstance(contact, dict):
            continue
        fields = contact.get("fields")
        if not isinstance(fields, dict) or key not in fields:
            continue
        contact["fields"] = {name: value for name, value in fields.items()
                             if name != key}
        sealed[row["contact_id"]] = await runtime.seal(contact)
    await _write_sealed_contacts(runtime, org_bi, account_bi, actor, now, sealed)
    views = 0
    for row in await runtime.d1_all(
            "SELECT view_id,data FROM crm_views WHERE org_bi=?", org_bi) or []:
        view = await runtime.open(row.get("data"))
        if not isinstance(view, dict):
            continue
        view = _view_data(view)
        columns = [column for column in view["columns"]
                   if not (column["custom"] and column["field"] == key)]
        if len(columns) == len(view["columns"]):
            continue
        view["columns"] = columns
        await runtime.d1_run(
            "UPDATE crm_views SET updated_at=?,data=? WHERE org_bi=? AND view_id=?",
            now, await runtime.seal(view), org_bi, row["view_id"],
        )
        views += 1
    await runtime.audit(actor, "crm.column.delete", "organization", org_name,
                        details={"field": key, "contacts": len(sealed),
                                 "views": views})
    return _response(runtime, {
        "ok": True, "field": key, "contacts": len(sealed), "views": views,
    })


async def _delete_builtin_column(runtime, org_bi, org_name, account_bi, actor, role, data):
    key = str(data.get("field") or "")
    if key not in DELETABLE_COLUMN_FIELDS:
        return _response(runtime, {"error": "invalid_column"}, 400)
    if role not in MANAGER_ROLES:
        return _response(runtime, {"error": "organization_admin_required"}, 403)
    blank = [] if key == "tags" else 0 if key == "value" else ""
    now = runtime.now()
    sealed = {}
    for row in await runtime.d1_all(
            "SELECT contact_id,data FROM crm_contacts WHERE org_bi=?", org_bi) or []:
        contact = await runtime.open(row.get("data"))
        if not isinstance(contact, dict) or not contact.get(key):
            continue
        contact[key] = blank
        sealed[row["contact_id"]] = await runtime.seal(contact)
    await _write_sealed_contacts(runtime, org_bi, account_bi, actor, now, sealed)
    views = 0
    for row in await runtime.d1_all(
            "SELECT view_id,data FROM crm_views WHERE org_bi=?", org_bi) or []:
        view = await runtime.open(row.get("data"))
        if not isinstance(view, dict):
            continue
        view = _view_data(view)
        columns = [dict(column, hidden=True) if column["field"] == key else column
                   for column in view["columns"]]
        if not any(column["field"] == key for column in columns):
            columns.append({"field": key, "label": CONTACT_COLUMN_LABELS[key],
                            "custom": False, "hidden": True})
        if columns == view["columns"]:
            continue
        view["columns"] = columns
        await runtime.d1_run(
            "UPDATE crm_views SET updated_at=?,data=? WHERE org_bi=? AND view_id=?",
            now, await runtime.seal(view), org_bi, row["view_id"],
        )
        views += 1
    current = await runtime.d1_first(
        "SELECT data FROM crm_settings WHERE org_bi=?", org_bi)
    opened = await runtime.open(current.get("data")) if current else {}
    opened = opened if isinstance(opened, dict) else {}
    settings = _settings_data(
        {"removedColumns": list(opened.get("removedColumns") or []) + [key]}, opened)
    await runtime.d1_run(
        "INSERT INTO crm_settings(org_bi,updated_by_bi,updated_by_name,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(org_bi) DO UPDATE SET "
        "updated_by_bi=excluded.updated_by_bi,updated_by_name=excluded.updated_by_name,"
        "updated_at=excluded.updated_at,data=excluded.data",
        org_bi, account_bi, actor, now, await runtime.seal(settings),
    )
    await runtime.audit(actor, "crm.column.delete", "organization", org_name,
                        details={"field": key, "contacts": len(sealed),
                                 "views": views, "builtin": True})
    return _response(runtime, {
        "ok": True, "field": key, "contacts": len(sealed), "views": views,
        "removedColumns": settings["removedColumns"],
    })


DUPLICATE_SIGNALS = ("email", "profile", "handle", "name")
ACCOUNT_EVIDENCE_CHUNK = 500
ACCOUNT_EVIDENCE_KEYS = ("account", "signedUpAt", "paid", "promotion", "trial")
# The contact columns a merge fills from the duplicate only where the kept
# record is blank; everything the kept record already says stays.
MERGE_FILL_FIELDS = ("name", "email", "company", "title", "phone", "twitter",
                     "mastodon", "bluesky", "linkedin", "github", "website",
                     "location", "owner", "source", "nextStep")


def _merged_contact_data(kept, duplicate):
    """One record out of two boxes made for the same person. The kept box wins
    every field it has filled; the duplicate fills its blanks, adds its tags,
    lists and custom columns, and its notes follow the kept notes. A second
    address stays findable in an "other_emails" column."""
    result = dict(kept)
    for key in MERGE_FILL_FIELDS:
        if not str(result.get(key) or "").strip() and str(duplicate.get(key) or "").strip():
            result[key] = duplicate[key]
    notes = [str(item.get("notes") or "").strip() for item in (kept, duplicate)]
    result["notes"] = _text("\n\n".join(
        note for index, note in enumerate(notes)
        if note and note not in notes[:index]), 20000)
    result["tags"] = _tags(_tags(kept.get("tags")) + _tags(duplicate.get("tags")))
    if not isinstance(kept.get("account"), dict) and isinstance(duplicate.get("account"), dict):
        result["account"] = duplicate["account"]
    result["views"] = _view_ids(duplicate.get("views"), _view_ids(kept.get("views")))
    result["value"] = max(_integer(kept.get("value")), _integer(duplicate.get("value")))
    result["lastContactedAt"] = max(_integer(kept.get("lastContactedAt")),
                                    _integer(duplicate.get("lastContactedAt")))
    fields = _custom_fields(kept.get("fields"))
    for key, value in _custom_fields(duplicate.get("fields")).items():
        fields.setdefault(key, value)
    other = _email(duplicate.get("email"))
    if other and other != _email(result.get("email")) and other not in _box_emails(
            dict(result, fields=fields)):
        known = fields.get("other_emails", "").strip()
        fields["other_emails"] = f"{known}, {other}" if known else other
    result["fields"] = _custom_fields(fields)
    return result


async def _merge_contacts(runtime, org_bi, account_bi, actor, data):
    """Fold a contact made by mistake into the one to keep: its details fill
    the kept record's blanks, its email history and deck links move over, and
    then it is deleted."""
    source_id = _clean(data.get("from"), 64).lower()
    target_id = _clean(data.get("into"), 64).lower()
    if not CONTACT_ID_RE.fullmatch(source_id) or not CONTACT_ID_RE.fullmatch(target_id):
        return _response(runtime, {"error": "invalid_contact_id"}, 400)
    if source_id == target_id:
        return _response(runtime, {"error": "merge_needs_two_contacts"}, 400)
    source = await _contact_row(runtime, org_bi, source_id)
    target = await _contact_row(runtime, org_bi, target_id)
    if not source or not target:
        return _response(runtime, {"error": "contact_not_found"}, 404)
    duplicate = await runtime.open(source.get("data"))
    kept = await runtime.open(target.get("data"))
    contact = _merged_contact_data(kept if isinstance(kept, dict) else {},
                                   duplicate if isinstance(duplicate, dict) else {})
    email_bi = await _unique_email_bi(runtime, org_bi, contact.get("email"))
    # The duplicate lets go of its address before the kept record may take it
    # (the org email index is unique), and is deleted only once everything that
    # pointed at it points at the kept record: a failure part way loses nothing.
    await runtime.d1_run(
        "UPDATE crm_contacts SET email_bi='' WHERE org_bi=? AND contact_id=?",
        org_bi, source_id)
    for table in ("crm_email_sends", "crm_deck_links"):
        await runtime.d1_run(
            f"UPDATE {table} SET contact_id=? WHERE org_bi=? AND contact_id=?",
            target_id, org_bi, source_id)
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE crm_contacts SET email_bi=?,updated_by_bi=?,updated_by_name=?,"
        "updated_at=?,data=?,created_at=MIN(created_at,?) "
        "WHERE org_bi=? AND contact_id=?",
        email_bi, account_bi, actor, now, await runtime.seal(contact),
        _integer(source.get("created_at")), org_bi, target_id,
    )
    await runtime.d1_run(
        "DELETE FROM crm_contacts WHERE org_bi=? AND contact_id=?", org_bi, source_id)
    await runtime.audit(actor, "crm.contact.merge", "crm_contact", target_id,
                        details={"merged": source_id})
    views = await _org_list_views(runtime, org_bi)
    pipeline_lists = _pipeline_lists(views)
    await _record_list_activity(
        runtime, org_bi,
        _contact_lists(contact, views, pipeline_lists)
        + _contact_lists(duplicate, views, pipeline_lists),
        account_bi, actor, "contact.merged",
        {"count": 1, "contacts": [_contact_label(contact)],
         "merged": _contact_label(duplicate)})
    return _response(runtime, {
        "ok": True, "merged": source_id,
        "contact": await _contact_project(
            runtime, await _contact_row(runtime, org_bi, target_id)),
    })


def _duplicate_keys(data):
    identity = _box_identity(data)
    keys = {("email", _email(data.get("email")))} - {("email", "")}
    keys |= {("profile", item) for item in identity["profiles"]}
    keys |= {("handle", item) for item in identity["handles"]}
    if len(identity["name"]) >= 2:
        keys.add(("name", " ".join(identity["name"])))
    return keys


async def _duplicate_groups(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? "
        "ORDER BY created_at,contact_id", org_bi)
    parent, holders = {}, {}

    def root(item):
        while parent[item] != item:
            parent[item] = parent[parent[item]]
            item = parent[item]
        return item

    for row in rows or []:
        contact_id = str(row.get("contact_id") or "")
        parent[contact_id] = contact_id
        data = await runtime.open(row.get("data"))
        for key in _duplicate_keys(data if isinstance(data, dict) else {}):
            holders.setdefault(key, []).append(contact_id)
    shared = [(key, ids) for key, ids in holders.items() if len(ids) > 1]
    for _, ids in shared:
        for other in ids[1:]:
            parent[root(other)] = root(ids[0])
    groups = {}
    for (signal, _), ids in shared:
        group = groups.setdefault(root(ids[0]), {"contacts": [], "signals": set()})
        group["signals"].add(signal)
    for contact_id in parent:
        group = groups.get(root(contact_id))
        if group is not None:
            group["contacts"].append(contact_id)
    return sorted(({"contacts": group["contacts"],
                    "signals": [item for item in DUPLICATE_SIGNALS
                                if item in group["signals"]]}
                   for group in groups.values()),
                  key=lambda group: (-len(group["contacts"]), group["contacts"][0]))


def _evidence_stage(flows, contact, evidence):
    flow_id = _slug(contact.get("flow"), "customers")
    stages = next((flow["stages"] for flow in flows if flow["id"] == flow_id), [])
    if "customer" in stages and (evidence.get("paid") or {}).get("active"):
        target = "customer"
    elif "trial" in stages and any((evidence.get(key) or {}).get("active")
                                   for key in ("trial", "promotion")):
        target = "trial"
    else:
        return ""
    stage = _stage(contact.get("stage"))
    if stage in stages[:-1] and stages.index(stage) >= stages.index(target):
        return ""
    return target


async def _record_account_evidence(runtime, org_bi, org_name, account_bi, actor, role, data):
    if role not in MANAGER_ROLES or not hasattr(runtime, "account_evidence"):
        return _response(runtime, {"error": "forkmesh_admin_required"}, 403)
    wanted = data.get("ids")
    wanted = ({str(item) for item in wanted if CONTACT_ID_RE.fullmatch(str(item))}
              if isinstance(wanted, list) else None)
    rows = await runtime.d1_all(
        "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? ORDER BY created_at",
        org_bi)
    contacts = {}
    for row in rows or []:
        if wanted is not None and row["contact_id"] not in wanted:
            continue
        contact = await runtime.open(row.get("data"))
        contact = contact if isinstance(contact, dict) else {}
        primary = _email(contact.get("email"))
        contacts[row["contact_id"]] = (contact, [primary] * bool(primary) + sorted(
            _box_emails(contact) - {primary}))
    addresses = list(dict.fromkeys(
        email for _, emails in contacts.values() for email in emails))
    found = {}
    for start in range(0, len(addresses), ACCOUNT_EVIDENCE_CHUNK):
        part = await runtime.account_evidence(
            actor, addresses[start:start + ACCOUNT_EVIDENCE_CHUNK])
        if part is None:
            return _response(runtime, {"error": "forkmesh_admin_required"}, 403)
        found.update(part if isinstance(part, dict) else {})
    flows = await _org_flows(runtime, org_bi)
    now = runtime.now()
    sealed, totals = {}, {"checked": 0, "accounts": 0, "paying": 0, "trial": 0, "ahead": 0}
    for contact_id, (contact, emails) in contacts.items():
        if not emails:
            continue
        totals["checked"] += 1
        email = next((item for item in emails if item in found), "")
        match = found.get(email) or {}
        evidence = {key: match.get(key) for key in ACCOUNT_EVIDENCE_KEYS}
        evidence["account"] = _clean(evidence["account"], 64)
        evidence["signedUpAt"] = _integer(evidence["signedUpAt"])
        evidence["email"] = email
        evidence["suggestedStage"] = _evidence_stage(flows, contact, evidence)
        totals["accounts"] += bool(evidence["account"])
        totals["paying"] += bool((evidence["paid"] or {}).get("active"))
        totals["trial"] += any((evidence[key] or {}).get("active")
                               for key in ("trial", "promotion"))
        totals["ahead"] += bool(evidence["suggestedStage"])
        previous = contact.get("account") if isinstance(contact.get("account"), dict) else {}
        if {key: value for key, value in previous.items() if key != "recordedAt"} == evidence:
            continue
        contact["account"] = dict(evidence, recordedAt=now)
        sealed[contact_id] = await runtime.seal(contact)
    await _write_sealed_contacts(runtime, org_bi, account_bi, actor, now, sealed)
    await runtime.audit(actor, "crm.contacts.account_evidence", "organization", org_name,
                        details=dict(totals, changed=len(sealed)))
    return _response(runtime, dict(totals, ok=True, changed=len(sealed)))


# --- sheets ---------------------------------------------------------------
# A sheet (the dashboard calls them tabs) is a saved column layout over the
# organization's contacts. Importing a CSV creates one named after the file and
# ordered like the file; members can rename it, hide or reorder its columns, and
# share it with the whole organization or with named teammates only.


def _view_columns(value, existing=None):
    columns = []
    seen = set()
    source = value if isinstance(value, list) else (existing or [])
    for item in source[:MAX_VIEW_COLUMNS]:
        item = item if isinstance(item, dict) else {"field": item}
        field = str(item.get("field") or "")
        custom = field not in CONTACT_COLUMN_FIELDS
        if custom:
            field = _csv_key(field)
            if not FIELD_KEY_RE.fullmatch(field or ""):
                continue
        if field in seen:
            continue
        seen.add(field)
        columns.append({
            "field": field,
            "label": (_clean(item.get("label"), 60)
                      or CONTACT_COLUMN_LABELS.get(field)
                      or field.replace("_", " ").title()),
            "custom": custom,
            "hidden": bool(item.get("hidden")),
        })
    return columns


def _default_view_columns(leading=()):
    """Every canonical column, with the ones a sheet leads with kept in their
    own order and up front, and the rest available but hidden. A sheet that
    leads with nothing - an empty tab, or a pipeline - opens showing them all
    instead: a tab with every column hidden has no heading left to right-click,
    so its columns could never be brought back."""
    columns = list(leading)
    known = {column["field"] for column in columns}
    for field, label, _ in CONTACT_COLUMNS:
        if field not in known:
            columns.append({"field": field, "label": label,
                            "custom": False, "hidden": bool(leading)})
    return _view_columns(columns)


def _shared_with(value):
    names = []
    source = value if isinstance(value, list) else []
    for item in source[:100]:
        name = _clean(item, 64).lower()
        if name and name not in names:
            names.append(name)
    return names


def _view_data(data, existing=None):
    result = dict(existing or {})
    if "name" in data or not existing:
        result["name"] = _clean(data.get("name"), 60) or "Untitled sheet"
    if "columns" in data or not existing:
        result["columns"] = _view_columns(data.get("columns"), result.get("columns"))
    if "visibility" in data or not existing:
        visibility = _clean(data.get("visibility"), 20).lower()
        result["visibility"] = (visibility if visibility in VIEW_VISIBILITIES
                                else "organization")
    if "sharedWith" in data or not existing:
        result["sharedWith"] = _shared_with(data.get("sharedWith"))
    if "shareAccess" in data or "shareAccess" not in result:
        access = _clean(data.get("shareAccess"), 10).lower()
        result["shareAccess"] = access if access in SHARE_ACCESS else "view"
    # Where the tab sits in the strip. Stored per sheet so the order a member
    # drags into is the order the whole organization then opens.
    if "position" in data or not existing:
        result["position"] = max(0, min(_integer(data.get("position")), MAX_VIEWS))
    if "source" in data or not existing:
        result["source"] = _slug(data.get("source"), "manual")
    if "fileName" in data or not existing:
        result["fileName"] = _clean(data.get("fileName"), 160)
    if "flow" in data or not existing:
        result["flow"] = _slug(data.get("flow"), "")
    return result


async def _view_row(runtime, org_bi, view_id):
    return await runtime.d1_first(
        "SELECT view_id,org_bi,org_name,owner_bi,owner_name,created_at,"
        "updated_at,data FROM crm_views WHERE org_bi=? AND view_id=?",
        org_bi, view_id,
    )


async def _view_project(runtime, row, account_bi="", role=""):
    data = await runtime.open(row.get("data"))
    data = _view_data(data if isinstance(data, dict) else {})
    owner_bi = str(row.get("owner_bi") or "")
    return {
        "id": str(row.get("view_id") or ""),
        "name": data["name"],
        "columns": data["columns"],
        "visibility": data["visibility"],
        "sharedWith": data["sharedWith"],
        "shareAccess": data["shareAccess"],
        "position": data["position"],
        "source": data["source"],
        "fileName": data["fileName"],
        "flow": data["flow"],
        "owner": str(row.get("owner_name") or ""),
        "isOwner": bool(account_bi) and owner_bi == account_bi,
        "canManage": (bool(account_bi) and owner_bi == account_bi
                      or role in MANAGER_ROLES),
        "createdAt": _integer(row.get("created_at")),
        "updatedAt": _integer(row.get("updated_at")),
    }


def _view_readable(view, owner_bi, account_bi, actor, role):
    # The share list is stored lowercased, so the reader's name is folded the
    # same way rather than being matched on the spelling the session carries.
    return (owner_bi == account_bi or role in MANAGER_ROLES
            or view.get("visibility") == "organization"
            or str(actor or "").lower() in view.get("sharedWith", []))


async def _views(runtime, org_bi, account_bi, actor, role):
    rows = await runtime.d1_all(
        "SELECT view_id,org_bi,org_name,owner_bi,owner_name,created_at,"
        "updated_at,data FROM crm_views WHERE org_bi=? "
        "ORDER BY created_at,view_id LIMIT ?",
        org_bi, MAX_VIEWS,
    )
    visible = []
    for row in rows or []:
        view = await _view_project(runtime, row, account_bi, role)
        if _view_readable(view, str(row.get("owner_bi") or ""), account_bi,
                          actor, role):
            visible.append(view)
    # The tab strip follows the order members dragged it into; sheets that
    # never moved keep their creation order behind the ones that did.
    visible.sort(key=lambda view: (view["position"], view["createdAt"], view["id"]))
    return visible


def _pipeline_lists(views):
    lists = {}
    for view in views:
        if view.get("flow"):
            lists.setdefault(view["flow"], []).append(view["id"])
    return lists


def _contact_in_view(contact, view, pipeline_lists):
    memberships = list(contact.get("views") or [])
    flow = view.get("flow") or ""
    if not flow:
        return view["id"] in memberships
    if (contact.get("flow") or "") != flow:
        return False
    lists = pipeline_lists.get(flow) or [view["id"]]
    mine = [item for item in lists if item in memberships]
    return (mine[0] if mine else lists[0]) == view["id"]


async def _org_list_views(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT view_id,org_bi,org_name,owner_bi,owner_name,created_at,"
        "updated_at,data FROM crm_views WHERE org_bi=? "
        "ORDER BY created_at,view_id LIMIT ?",
        org_bi, MAX_VIEWS,
    )
    views = [await _view_project(runtime, row) for row in rows or []]
    views.sort(key=lambda view: (view["position"], view["createdAt"], view["id"]))
    return views


async def _org_pipeline_lists(runtime, org_bi):
    return _pipeline_lists(await _org_list_views(runtime, org_bi))


def _contact_lists(contact, views, pipeline_lists):
    if not isinstance(contact, dict):
        return []
    return [view["id"] for view in views
            if _contact_in_view(contact, view, pipeline_lists)]


def _contact_label(contact):
    contact = contact if isinstance(contact, dict) else {}
    return (_clean(contact.get("name"), 160) or _email(contact.get("email"))
            or "a contact")


def _changed_fields(before, after):
    before = before if isinstance(before, dict) else {}
    after = after if isinstance(after, dict) else {}
    changed = []
    for field, label, _ in CONTACT_COLUMNS:
        if before.get(field) != after.get(field) and (
                before.get(field) or after.get(field)):
            changed.append(label)
    old_fields = before.get("fields") if isinstance(before.get("fields"), dict) else {}
    new_fields = after.get("fields") if isinstance(after.get("fields"), dict) else {}
    for key in sorted(set(old_fields) | set(new_fields)):
        if old_fields.get(key) != new_fields.get(key) and (
                old_fields.get(key) or new_fields.get(key)):
            changed.append(key)
    return changed


async def _record_list_activity(runtime, org_bi, view_ids, account_bi, actor,
                                action, details=None):
    view_ids = [str(item) for item in dict.fromkeys(view_ids or [])
                if CONTACT_ID_RE.fullmatch(str(item or ""))]
    if not view_ids:
        return
    now = runtime.now()
    sealed = await runtime.seal(details or {})
    for view_id in view_ids:
        await runtime.d1_run(
            "INSERT INTO crm_list_activity(org_bi,view_id,ts,actor_bi,"
            "actor_name,action,data) VALUES(?,?,?,?,?,?,?)",
            org_bi, view_id, now, account_bi, _clean(actor, 64).lower(), action,
            sealed)


async def _record_contact_changes(runtime, org_bi, account_bi, actor, changes,
                                  views=None):
    if views is None:
        views = await _org_list_views(runtime, org_bi)
    pipeline_lists = _pipeline_lists(views)
    grouped = {}
    for before, after in changes:
        was = set(_contact_lists(before, views, pipeline_lists))
        now_in = set(_contact_lists(after, views, pipeline_lists))
        fields = _changed_fields(before, after) if before and after else []
        name = _contact_label(after or before)
        for view in views:
            view_id = view["id"]
            if view_id in now_in and view_id not in was:
                action = "contact.added"
            elif view_id in was and view_id not in now_in:
                action = "contact.deleted" if after is None else "contact.removed"
            elif view_id in now_in and fields:
                action = "contact.updated"
            else:
                continue
            entry = grouped.setdefault((view_id, action), {
                "count": 0, "contacts": [], "fields": []})
            entry["count"] += 1
            if len(entry["contacts"]) < LIST_ACTIVITY_NAMES:
                entry["contacts"].append(name)
            for field in fields:
                if field not in entry["fields"]:
                    entry["fields"].append(field)
    for (view_id, action), details in grouped.items():
        await _record_list_activity(runtime, org_bi, [view_id], account_bi, actor,
                                    action, details)


async def _record_contact_event(runtime, org_bi, account_bi, actor, contact,
                                action, details=None):
    views = await _org_list_views(runtime, org_bi)
    await _record_list_activity(
        runtime, org_bi, _contact_lists(contact, views, _pipeline_lists(views)),
        account_bi, actor, action,
        dict(details or {}, count=1, contacts=[_contact_label(contact)]))


def _list_change_details(before, after):
    changes = []
    if before.get("name") != after.get("name"):
        changes.append("name")
    if before.get("columns") != after.get("columns"):
        changes.append("columns")
    if (before.get("visibility") != after.get("visibility")
            or before.get("sharedWith") != after.get("sharedWith")
            or before.get("shareAccess") != after.get("shareAccess")):
        changes.append("sharing")
    if before.get("flow") != after.get("flow"):
        changes.append("pipeline")
    return {"changes": changes, "from": before.get("name") or "",
            "to": after.get("name") or ""}


def _names_phrase(details, noun="contact"):
    count = max(1, _integer(details.get("count"), 1))
    names = [_clean(item, 160) for item in details.get("contacts") or []
             if _clean(item, 160)]
    if count == 1 and names:
        return names[0]
    phrase = "%d %ss" % (count, noun)
    if names:
        phrase += ": " + ", ".join(names[:LIST_ACTIVITY_NAMES])
        if count > len(names[:LIST_ACTIVITY_NAMES]):
            phrase += ", \u2026"
    return phrase


def _list_activity_text(action, details):
    details = details if isinstance(details, dict) else {}
    if action == "list.viewed":
        return "opened the list"
    if action == "list.created":
        return "created the list"
    if action == "list.updated":
        parts = []
        changes = details.get("changes") or []
        if "name" in changes:
            parts.append("renamed it from \u201c%s\u201d to \u201c%s\u201d" % (
                _clean(details.get("from"), 120), _clean(details.get("to"), 120)))
        if "columns" in changes:
            parts.append("changed its columns")
        if "sharing" in changes:
            parts.append("changed who it is shared with")
        if "pipeline" in changes:
            parts.append("changed its pipeline type")
        return "; ".join(parts) or "changed the list"
    if action == "contact.added":
        return "added " + _names_phrase(details)
    if action == "contact.removed":
        return "took " + _names_phrase(details) + " off the list"
    if action == "contact.deleted":
        return "deleted " + _names_phrase(details)
    if action == "contact.updated":
        fields = [_clean(item, 64) for item in details.get("fields") or []]
        text = "edited " + _names_phrase(details)
        return text + (" (" + ", ".join(fields[:8]) + ")" if fields else "")
    if action == "contact.merged":
        return "merged %s into %s" % (_clean(details.get("merged"), 160) or "a contact",
                                      _names_phrase(details))
    if action == "contact.emailed":
        subject = _clean(details.get("subject"), 200)
        return "emailed " + _names_phrase(details) + (
            " \u201c" + subject + "\u201d" if subject else "")
    if action == "contacts.imported":
        text = "imported %d row%s from %s" % (
            _integer(details.get("rows")),
            "" if _integer(details.get("rows")) == 1 else "s",
            _clean(details.get("fileName"), 160) or "a CSV")
        return text + " (%d new, %d updated)" % (
            _integer(details.get("created")), _integer(details.get("updated")))
    return action


async def _list_activity(runtime, org_bi, view_id):
    before = _integer(runtime.query_params().get("before"))
    rows = await runtime.d1_all(
        "SELECT id,ts,actor_name,action,data FROM crm_list_activity "
        "WHERE org_bi=? AND view_id=? AND (?=0 OR ts<?) "
        "ORDER BY ts DESC,id DESC LIMIT ?",
        org_bi, view_id, before, before, LIST_ACTIVITY_LIMIT + 1,
    )
    entries = []
    for row in (rows or [])[:LIST_ACTIVITY_LIMIT]:
        details = await runtime.open(row.get("data"))
        details = details if isinstance(details, dict) else {}
        action = _clean(row.get("action"), 40)
        entries.append({
            "at": _integer(row.get("ts")),
            "actor": _clean(row.get("actor_name"), 64),
            "action": action,
            "text": _list_activity_text(action, details),
            "count": max(1, _integer(details.get("count"), 1)),
        })
    return {"ok": True, "view": view_id, "entries": entries,
            "more": len(rows or []) > LIST_ACTIVITY_LIMIT}


async def _record_list_view(runtime, org_bi, view_id, account_bi, actor):
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO crm_list_activity(org_bi,view_id,ts,actor_bi,actor_name,"
        "action,data) SELECT ?,?,?,?,?,'list.viewed',? WHERE NOT EXISTS ("
        "SELECT 1 FROM crm_list_activity WHERE org_bi=? AND view_id=? "
        "AND ts>? AND actor_bi=? AND action='list.viewed')",
        org_bi, view_id, now, account_bi, _clean(actor, 64).lower(),
        await runtime.seal({}), org_bi, view_id, now - LIST_VIEW_GAP_MS,
        account_bi,
    )


async def _org_member_names(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT name FROM org_members WHERE org_bi=? "
        "ORDER BY name COLLATE NOCASE LIMIT 500",
        org_bi,
    )
    names = []
    for row in rows or []:
        name = _clean(row.get("name"), 64).lower()
        if name and name not in names:
            names.append(name)
    return names


async def _share_bi(runtime, name):
    """Blind key for "sheets shared with this ForkMesh user", global rather
    than organization-scoped because that is exactly the question it answers."""
    return await runtime.blind("crm-view-share-v1\n" + _clean(name, 64).lower())


async def _known_share_names(runtime, names):
    """Keep only names the host can vouch for as user accounts. A host without
    the capability (the package tests) accepts the list as given."""
    checker = getattr(runtime, "account_exists", None)
    if checker is None:
        return list(names)
    result = []
    for name in names:
        if await checker(name):
            result.append(name)
    return result


async def _sync_view_shares(runtime, org_bi, view_id, names):
    now = runtime.now()
    await runtime.d1_run(
        "DELETE FROM crm_view_shares WHERE view_id=?", view_id)
    for name in list(names or [])[:100]:
        await runtime.d1_run(
            "INSERT OR IGNORE INTO crm_view_shares(view_id,org_bi,shared_bi,"
            "created_at) VALUES(?,?,?,?)",
            view_id, org_bi, await _share_bi(runtime, name), now)


async def _store_view(runtime, org_bi, org_name, owner_bi, owner_name, view,
                      view_id="", created_at=0, sync_shares=False):
    now = runtime.now()
    if view_id:
        await runtime.d1_run(
            "UPDATE crm_views SET updated_at=?,data=? WHERE org_bi=? AND view_id=?",
            now, await runtime.seal(view), org_bi, view_id,
        )
    else:
        view_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO crm_views(view_id,org_bi,org_name,owner_bi,owner_name,"
            "created_at,updated_at,data) VALUES(?,?,?,?,?,?,?,?)",
            view_id, org_bi, org_name, owner_bi, owner_name, created_at or now,
            now, await runtime.seal(view),
        )
    if sync_shares:
        await _sync_view_shares(runtime, org_bi, view_id, view.get("sharedWith"))
    return view_id


async def _delete_view(runtime, org_bi, view_id):
    await runtime.d1_run(
        "DELETE FROM crm_view_shares WHERE view_id=?", view_id)
    await runtime.d1_run(
        "DELETE FROM crm_list_activity WHERE org_bi=? AND view_id=?",
        org_bi, view_id)
    await runtime.d1_run(
        "DELETE FROM crm_views WHERE org_bi=? AND view_id=?", org_bi, view_id)


async def _create_view(runtime, org_bi, org_name, account_bi, actor, role, data):
    view = _view_data(data)
    if not view["columns"]:
        view["columns"] = _default_view_columns()
    # A new tab joins the end of the strip, behind any the members have moved.
    view["position"] = MAX_VIEWS
    view["sharedWith"] = await _known_share_names(runtime, view["sharedWith"])
    view_id = await _store_view(runtime, org_bi, org_name, account_bi, actor, view,
                                sync_shares=True)
    await runtime.audit(actor, "crm.view.create", "crm_view", view_id)
    await _record_list_activity(runtime, org_bi, [view_id], account_bi, actor,
                                "list.created")
    return _response(runtime, {
        "ok": True,
        "view": await _view_project(
            runtime, await _view_row(runtime, org_bi, view_id), account_bi, role),
    }, 201)


async def _reorder_views(runtime, org_bi, account_bi, actor, role, data):
    """Move the tabs. Everyone who can open a sheet can also move it, the same
    way everyone it reaches can rename it or shape its columns; a sheet this
    member cannot see keeps its place behind the ones they ordered."""
    wanted = []
    source = data.get("order") if isinstance(data.get("order"), list) else []
    for item in source[:MAX_VIEWS]:
        item = _clean(item, 32).lower()
        if CONTACT_ID_RE.fullmatch(item) and item not in wanted:
            wanted.append(item)
    rows = await runtime.d1_all(
        "SELECT view_id,org_bi,org_name,owner_bi,owner_name,created_at,"
        "updated_at,data FROM crm_views WHERE org_bi=? "
        "ORDER BY created_at,view_id LIMIT ?",
        org_bi, MAX_VIEWS,
    )
    readable = []
    for row in rows or []:
        stored = await runtime.open(row.get("data"))
        stored = _view_data(stored if isinstance(stored, dict) else {})
        if _view_readable(stored, str(row.get("owner_bi") or ""), account_bi,
                          actor, role):
            readable.append((row, stored))
    readable.sort(key=lambda pair: (pair[1]["position"],
                                    _integer(pair[0].get("created_at")),
                                    str(pair[0].get("view_id") or "")))
    # A stable sort keeps the sheets the request did not name in the order they
    # already had, behind the ones it did.
    ordered = sorted(readable, key=lambda pair: wanted.index(
        str(pair[0].get("view_id") or "")) if str(pair[0].get("view_id") or "")
        in wanted else len(wanted))
    for position, (row, stored) in enumerate(ordered):
        if stored["position"] == position:
            continue
        await _store_view(runtime, org_bi, str(row.get("org_name") or ""),
                          str(row.get("owner_bi") or ""),
                          str(row.get("owner_name") or ""),
                          dict(stored, position=position),
                          str(row.get("view_id") or ""))
    await runtime.audit(actor, "crm.view.order", "crm_view", "")
    return _response(runtime, {
        "ok": True,
        "views": await _views(runtime, org_bi, account_bi, actor, role),
    })


async def _update_view(runtime, row, org_bi, account_bi, actor, role, data):
    existing = await runtime.open(row.get("data"))
    existing = _view_data(existing if isinstance(existing, dict) else {})
    manages = str(row.get("owner_bi") or "") == account_bi or role in MANAGER_ROLES
    # Anyone the sheet is shared with keeps it usable - renaming it and moving
    # or hiding its columns. Who else may see it stays with its owner and the
    # organization's owners and admins.
    if not manages:
        data = {key: value for key, value in data.items()
                if key in ("name", "columns")}
    view = _view_data(data, existing)
    if "sharedWith" in data:
        view["sharedWith"] = await _known_share_names(runtime, view["sharedWith"])
    await _store_view(runtime, org_bi, str(row.get("org_name") or ""),
                      account_bi, actor, view, str(row.get("view_id") or ""),
                      sync_shares="sharedWith" in data)
    await runtime.audit(actor, "crm.view.update", "crm_view", row.get("view_id"))
    change = _list_change_details(existing, view)
    if change["changes"]:
        await _record_list_activity(runtime, org_bi, [row.get("view_id")],
                                    account_bi, actor, "list.updated", change)
    return _response(runtime, {
        "ok": True,
        "view": await _view_project(
            runtime, await _view_row(runtime, org_bi, row.get("view_id")),
            account_bi, role),
    })


def _csv_value(value):
    value = str(value or "")
    if value.lstrip(" \t\r\n").startswith(("=", "+", "-", "@")):
        return "'" + value
    return value


async def _export_contacts(runtime, org_bi, org_name):
    output = io.StringIO(newline="")
    fields = [
        "name", "email", "company", "title", "phone", "twitter", "mastodon",
        "bluesky", "linkedin", "github", "website", "location", "flow", "stage",
        "value", "next_step", "tags", "owner", "source", "notes",
        "last_contacted_at",
    ]
    contacts = await _contacts(runtime, org_bi)
    # Imported columns the CRM has no built-in home for still belong to the
    # organization, so an export round-trips them instead of dropping them.
    custom = []
    for contact in contacts:
        for key in contact["fields"]:
            if key not in custom and key not in fields:
                custom.append(key)
    fields = fields + sorted(custom)[:MAX_CUSTOM_FIELDS]
    writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\r\n",
                            extrasaction="ignore")
    writer.writeheader()
    for contact in contacts:
        writer.writerow({
            **{key: _csv_value(contact["fields"].get(key)) for key in fields},
            "name": _csv_value(contact["name"]),
            "email": _csv_value(contact["email"]),
            "company": _csv_value(contact["company"]),
            "title": _csv_value(contact["title"]),
            "phone": _csv_value(contact["phone"]),
            "twitter": _csv_value(contact["twitter"]),
            "mastodon": _csv_value(contact["mastodon"]),
            "bluesky": _csv_value(contact["bluesky"]),
            "linkedin": _csv_value(contact["linkedin"]),
            "github": _csv_value(contact["github"]),
            "website": _csv_value(contact["website"]),
            "location": _csv_value(contact["location"]),
            "flow": contact["flow"],
            "stage": contact["stage"],
            "value": contact["value"] or "",
            "next_step": _csv_value(contact["nextStep"]),
            "tags": _csv_value(";".join(contact["tags"])),
            "owner": _csv_value(contact["owner"]),
            "source": _csv_value(contact["source"]),
            "notes": _csv_value(contact["notes"]),
            "last_contacted_at": contact["lastContactedAt"] or "",
        })
    return _response(runtime, {
        "ok": True,
        "filename": "forkmesh-crm-" + org_name + ".csv",
        "csv": output.getvalue(),
    })


def _csv_key(header):
    return re.sub(r"[^a-z0-9]+", "_",
                  str(header or "").lstrip("\ufeff").strip().lower()).strip("_")


def _csv_column_plan(fieldnames, mapping):
    """Ordered sheet columns for a CSV: the contact columns the mapping sends
    the file's headers to, in the file's own order, then every column kept as
    its own custom field keyed by its header."""
    columns = []
    seen = set()
    for header, target in list(zip(fieldnames or [], mapping))[:MAX_VIEW_COLUMNS]:
        if not target:
            continue
        custom = target == "custom"
        field = (_csv_custom_key(header) if custom
                 else "name" if target in CSV_NAME_PART_TARGETS else target)
        if not field or field in seen:
            continue
        seen.add(field)
        columns.append({
            "field": field,
            "label": (_clean(header, 60) if custom
                      else CONTACT_COLUMN_LABELS[field]),
            "custom": custom,
            "hidden": False,
        })
    return columns


def _csv_field_for_key(key):
    if key in ("first_name", "given_name"):
        return "first_name"
    if key in ("last_name", "family_name"):
        return "last_name"
    for field, _, aliases in CONTACT_COLUMNS:
        if key in aliases:
            return field
    return None


def _csv_custom_key(header):
    """The custom field a column kept as its own lands on. A header that spells
    a contact column the member chose not to map there gets a prefix, so it
    cannot be read back as that column."""
    key = _csv_key(header)
    if key in CSV_ALIAS_KEYS or key in NAME_PART_KEYS or key in CONTACT_COLUMN_FIELDS:
        key = "custom_" + key
    key = key[:64].rstrip("_")
    return key if FIELD_KEY_RE.fullmatch(key) else ""


def _csv_guess_from_samples(samples):
    """A contact column told apart by what its cells hold, for headers that
    say nothing useful ("Contact", "Column 3")."""
    values = [str(value or "").strip() for value in samples]
    values = [value for value in values if value]
    if not values:
        return None
    lower = [value.lower() for value in values]
    for field, test in (
            ("email", lambda value: EMAIL_RE.fullmatch(value) is not None),
            ("linkedin", lambda value: "linkedin.com/" in value),
            ("github", lambda value: "github.com/" in value),
            ("twitter", lambda value: re.search(r"(^|/)(twitter|x)\.com/", value)),
            ("bluesky", lambda value: value.endswith(".bsky.social")
             or "bsky.app/" in value),
            ("website", lambda value: re.match(r"^(https?://|www\.)", value))):
        if all(test(value) for value in lower):
            return field
    return None


def _csv_looks_like_names(samples):
    """True when every non-empty cell reads like a person's name, so a column
    headed "Journalist" or "Speaker" is still recognised as the contact."""
    values = [str(value or "").strip() for value in samples]
    values = [value for value in values if value]
    return bool(values) and all(
        PERSON_NAME_RE.fullmatch(value)
        and all(word[0].isupper() or word.lower() in NAME_PARTICLES
                for word in value.split())
        for value in values)


def _csv_guess_mapping(fieldnames, rows=(), loose=True):
    """A first best guess at what each CSV column is: exact header spellings
    first, then - when ``loose`` - what the sample cells look like and the
    words in the header. Anything left over is kept as its own column. A loose
    guess never takes a contact column another column claimed."""
    headers = list(fieldnames or [])
    mapping = [None] * len(headers)
    claimed = set()
    for index, header in enumerate(headers):
        key = _csv_key(header)
        field = _csv_field_for_key(key) if key else ""
        mapping[index] = field
        if field:
            claimed.add(field)
    for index, header in enumerate(headers):
        if mapping[index] is not None:
            continue
        if not loose:
            mapping[index] = "custom"
            continue
        key = _csv_key(header)
        words = set(key.split("_"))
        guesses = []
        if rows:
            guesses.append(_csv_guess_from_samples(
                (row or {}).get(header) for row in rows))
        for field, hints in CSV_GUESS_WORDS:
            if any(hint in words or (hint in CSV_GUESS_SUBSTRINGS and hint in key)
                   for hint in hints):
                # "First contacted" is not a given name.
                if field != "first_name" or "name" in words:
                    guesses.append(field)
        # Cells that read like people's names are the weakest hint, tried last
        # so a "Company name" column of "Analytical Engines" stays a company.
        if rows and _csv_looks_like_names((row or {}).get(header) for row in rows):
            guesses.append("name")
        field = next((guess for guess in guesses
                      if guess and guess not in claimed), "custom")
        mapping[index] = field
        claimed.add(field)
    return mapping


def _csv_mapping(fieldnames, data):
    """What each column of this import is imported as: the mapping the member
    confirmed on the import step, one target per header in file order. A
    caller that shows no import step gets exact header spellings only: a loose
    guess ("Owner region" as the owner) is only safe once a member has seen it.
    Every part of a split upload reads the same header, so each part agrees."""
    guessed = _csv_guess_mapping(fieldnames, loose=False)
    chosen = data.get("mapping")
    if not isinstance(chosen, list):
        return guessed
    mapping = []
    for index, fallback in enumerate(guessed):
        target = chosen[index] if index < len(chosen) else fallback
        target = target if isinstance(target, str) else ""
        mapping.append(target if target in CSV_MAPPING_TARGETS else "custom")
    return mapping


def _normalized_csv_row(row, fieldnames, mapping):
    result = {field: "" for field, _, _ in CONTACT_COLUMNS}
    parts = dict.fromkeys(CSV_NAME_PART_TARGETS, "")
    fields = {}
    for header, target in zip(fieldnames or [], mapping):
        value = (row or {}).get(header)
        if not target or not str(value or "").strip():
            continue
        if target == "custom":
            # Everything not mapped onto a contact column is carried through
            # as a custom field rather than dropped on the floor.
            key = _csv_custom_key(header)
            if key and key not in fields:
                fields[key] = value
        elif target in parts:
            parts[target] = parts[target] or value
        elif not str(result[target]).strip():
            result[target] = value
    if not str(result["name"]).strip():
        result["name"] = " ".join(
            str(parts[part]).strip() for part in CSV_NAME_PART_TARGETS
            if str(parts[part]).strip())
    # A list without email addresses still imports: a row with neither a name
    # nor an email is known by its company.
    if not str(result["name"]).strip() and not str(result["email"]).strip():
        result["name"] = result["company"]
    result["lastContactedAt"] = result["lastContactedAt"] or 0
    result["fields"] = fields
    return result


def _sheet_name(file_name):
    name = re.sub(r"\.[A-Za-z0-9]{1,8}$", "", _clean(file_name, 160).split("/")[-1])
    name = _clean(re.sub(r"[_\-]+", " ", name), 60)
    return name or "Imported contacts"


async def _import_sheet(runtime, org_bi, org_name, account_bi, actor, data,
                        fieldnames, mapping, import_id=""):
    """The tab a CSV import lands in: the sheet named by the caller when they
    re-import into an existing tab, otherwise a new one - named what the member
    typed on the import step, or after the file - ordered exactly like the
    file's own header row. ``fresh`` is true when this
    import opened the sheet - in this request, or in an earlier part of the
    same split upload."""
    plan = _csv_column_plan(fieldnames, mapping)
    file_name = _clean(data.get("fileName") or data.get("name"), 160)
    view_id = _clean(data.get("viewId"), 32).lower()
    row = (await _view_row(runtime, org_bi, view_id)
           if CONTACT_ID_RE.fullmatch(view_id) else None)
    if row:
        raw = await runtime.open(row.get("data"))
        raw = raw if isinstance(raw, dict) else {}
        stored = _view_data(raw)
        known = {column["field"] for column in stored["columns"]}
        view = _view_data({"columns": stored["columns"] + [
            column for column in plan if column["field"] not in known]}, stored)
        fresh = bool(import_id) and raw.get("importId") == import_id
        if raw.get("importId"):
            view["importId"] = raw["importId"]
    else:
        view_id = ""
        view = _view_data({
            "name": _clean(data.get("sheetName"), 60) or _sheet_name(file_name),
            "columns": _default_view_columns(plan),
            "visibility": "organization",
            "source": "csv",
            "fileName": file_name,
            "position": MAX_VIEWS,
        })
        if import_id:
            view["importId"] = import_id
        fresh = True
    view_id = await _store_view(runtime, org_bi, org_name, account_bi, actor,
                                view, view_id)
    return view_id, await _view_project(
        runtime, await _view_row(runtime, org_bi, view_id), account_bi), fresh


def _import_batch(data):
    """The progress envelope of a CSV uploaded in parts: which import this is,
    which part of how many, how many data rows came before this part, and the
    totals the earlier parts already applied. A request without one is a
    whole-file import - its own first and last part - so older clients still
    get both pings."""
    batch = data.get("import")
    batch = batch if isinstance(batch, dict) else {}
    import_id = _clean(batch.get("id"), 64).lower()
    parts = max(1, min(_integer(batch.get("parts"), 1), MAX_IMPORT_ROWS))
    delimiter = str(batch.get("delimiter") or "")
    earlier = []
    for item in (batch.get("errors") if isinstance(batch.get("errors"), list)
                 else [])[:CSV_IMPORT_PING_ERRORS]:
        item = item if isinstance(item, dict) else {}
        if item.get("error") in CSV_IMPORT_ROW_MESSAGES:
            earlier.append({"row": max(0, _integer(item.get("row"))),
                            "error": item["error"]})
    return {
        "id": import_id if IMPORT_ID_RE.fullmatch(import_id) else "",
        "part": max(0, min(_integer(batch.get("part")), parts - 1)),
        "parts": parts,
        "offset": max(0, min(_integer(batch.get("offset")), MAX_IMPORT_ROWS)),
        "rows": max(0, min(_integer(batch.get("rows")), MAX_IMPORT_ROWS)),
        "delimiter": delimiter if delimiter in CSV_IMPORT_DELIMITERS else "",
        "failed": _clean(batch.get("failed"), 200),
        "created": max(0, min(_integer(batch.get("created")), MAX_IMPORT_ROWS)),
        "updated": max(0, min(_integer(batch.get("updated")), MAX_IMPORT_ROWS)),
        "skipped": max(0, min(_integer(batch.get("skipped")), MAX_IMPORT_ROWS)),
        "errors": earlier,
    }


def _import_row_error(row, error):
    return {"row": row, "error": error,
            "message": "Row %d %s." % (row, CSV_IMPORT_ROW_MESSAGES.get(
                error, "could not be imported"))}


def _read_csv_rows(raw, delimiter=""):
    """(header, rows, delimiter) for one CSV body. A later part of a split
    upload names the delimiter the first part detected, so every part of one
    file is read the same way even when a slice would sniff differently."""
    def read(**dialect):
        reader = csv.DictReader(io.StringIO(raw), **dialect)
        rows = []
        for row in reader:
            rows.append(row)
            if len(rows) > MAX_IMPORT_ROWS:
                break
        return reader.fieldnames, rows
    if delimiter:
        return (*read(delimiter=delimiter), delimiter)
    try:
        # Excel/Zoho exports commonly carry a UTF-8 BOM or use semicolons in
        # locales where a comma is the decimal separator. Accept those files
        # instead of silently treating the complete header as one field.
        dialect = csv.Sniffer().sniff(raw[:8192], delimiters=",;\t|")
        return (*read(dialect=dialect), dialect.delimiter)
    except (csv.Error, UnicodeError):
        return (*read(), ",")


async def _import_ping(runtime, actor, title, body, dedupe):
    """A ping to the member running the import. Hosts without pings (the
    package tests' bare runtime) simply import without them."""
    notify = getattr(runtime, "notify", None)
    if notify is None:
        return
    await notify(actor, "plugin_activity", title, body=body, href="/dashboard/crm",
                 dedupe=dedupe)


async def _celebrate_tracked_email(runtime, actor, send_id, subject, contact):
    notify = getattr(runtime, "notify", None)
    if notify is None or not actor or not send_id:
        return
    label = _contact_label(contact)
    body = ("“%s” with %s" % (subject, label)) if subject else ("With " + label)
    await notify(actor, "plugin_activity", "🎉 Email tracked in the CRM",
                 body=body, href="/dashboard/crm",
                 dedupe="crm-email-tracked:" + send_id,
                 meta={"celebrate": EMAIL_TRACKED_CELEBRATION})


def _import_totals_copy(created, updated, skipped):
    return "%d created, %d updated, %d skipped" % (created, updated, skipped)


async def _import_view_owned(runtime, org_bi, view_id, import_id):
    """True when the sheet was opened by this very import, so an import that
    ends without a single accepted row may take its empty tab away again."""
    if not import_id or not CONTACT_ID_RE.fullmatch(view_id or ""):
        return False
    row = await _view_row(runtime, org_bi, view_id)
    stored = await runtime.open(row.get("data")) if row else None
    return isinstance(stored, dict) and stored.get("importId") == import_id


async def _import_failed(runtime, org_bi, org_name, actor, data, batch, reason,
                         status=400, error=""):
    file_name = _clean(data.get("fileName"), 160) or "contacts.csv"
    applied = batch["created"] + batch["updated"]
    body = reason[:1].upper() + reason[1:] + "."
    if batch["part"]:
        body += " Before it stopped: " + _import_totals_copy(
            batch["created"], batch["updated"], batch["skipped"]) + "."
    view_id = _clean(data.get("viewId"), 32).lower()
    if not applied and await _import_view_owned(
            runtime, org_bi, view_id, batch["id"]):
        await _delete_view(runtime, org_bi, view_id)
    await _import_ping(
        runtime, actor, "CRM import failed: " + file_name, body,
        "crm-import:%s:%s:end" % (org_name, batch["id"] or runtime.now()))
    if error:
        return _response(runtime, {"error": error}, status)
    return _response(runtime, {"ok": True, "failed": True})


def _import_preview(runtime, raw):
    """The import step's column mapping, before anything is written: every
    header with a few sample cells and the best guess at its contact column."""
    failure = ""
    if not raw.strip():
        failure = "csv_required"
    else:
        try:
            fieldnames, rows, delimiter = _read_csv_rows(raw)
        except (csv.Error, UnicodeError):
            failure = "invalid_csv"
        else:
            if not fieldnames:
                failure = "csv_header_required"
    if failure:
        return _response(runtime, {"error": failure}, 400)
    rows = rows[:CSV_PREVIEW_ROWS]
    columns = []
    for index, (header, field) in enumerate(
            zip(fieldnames, _csv_guess_mapping(fieldnames, rows))):
        samples = []
        for row in rows:
            value = _clean((row or {}).get(header), 80)
            if value and value not in samples:
                samples.append(value)
            if len(samples) == CSV_PREVIEW_SAMPLES:
                break
        columns.append({"index": index, "header": _clean(header, 120),
                        "field": field, "samples": samples})
    return _response(runtime, {"ok": True, "delimiter": delimiter,
                               "columns": columns})


async def _import_contacts(runtime, org_bi, org_name, account_bi, actor, data):
    batch = _import_batch(data)
    raw = _text(data.get("csv"), MAX_BODY_BYTES).lstrip("\ufeff")
    if data.get("preview"):
        return _import_preview(runtime, raw)
    if batch["id"] and batch["failed"]:
        # The client gave up part-way (a dropped connection, a failed part):
        # close the import with a failure ping instead of leaving "started"
        # as the last word.
        return await _import_failed(runtime, org_bi, org_name, actor, data,
                                    batch, batch["failed"])
    failure = ""
    if not raw.strip():
        failure = "csv_required"
    else:
        try:
            fieldnames, rows, delimiter = _read_csv_rows(raw, batch["delimiter"])
        except (csv.Error, UnicodeError):
            failure = "invalid_csv"
        else:
            if not fieldnames:
                failure = "csv_header_required"
            elif batch["offset"] + len(rows) > MAX_IMPORT_ROWS:
                failure = "too_many_csv_rows"
    if failure:
        return await _import_failed(
            runtime, org_bi, org_name, actor, data, batch,
            CSV_IMPORT_FAILURES[failure],
            413 if failure == "too_many_csv_rows" else 400, failure)
    mapping = _csv_mapping(fieldnames, data)
    view_id, view, fresh = await _import_sheet(
        runtime, org_bi, org_name, account_bi, actor, data, fieldnames,
        mapping, batch["id"])
    file_name = _clean(data.get("fileName"), 160) or "contacts.csv"
    ping_key = "crm-import:%s:%s" % (org_name, batch["id"] or runtime.now())
    if batch["part"] == 0:
        total_rows = batch["rows"] or len(rows)
        await _import_ping(
            runtime, actor, "CRM import started: " + file_name,
            "%d row%s importing into %s in %s." % (
                total_rows, "" if total_rows == 1 else "s", view["name"],
                org_name),
            ping_key + ":start")
    created = updated = skipped = 0
    errors = []
    now = runtime.now()
    handles = {}
    flows = (await _settings(runtime, org_bi))["flows"]
    for index, source in enumerate(rows, start=2 + batch["offset"]):
        incoming = _normalized_csv_row(source, fieldnames, mapping)
        # A CSV that leaves a column out must not erase what the CRM already
        # knows about the contact, so only values the row actually carries are
        # applied; the rest of the record survives the import untouched.
        supplied = {key: value for key, value in incoming.items()
                    if str(value or "").strip()}
        outcome, error = await _upsert_contact(
            runtime, org_bi, org_name, account_bi, actor, supplied, view_id,
            now, "csv", flows, handles)
        if outcome == "created":
            created += 1
        elif outcome == "updated":
            updated += 1
        else:
            skipped += 1
            if len(errors) < 50:
                errors.append(_import_row_error(index, error))
    last = batch["part"] == batch["parts"] - 1
    totals = {"created": batch["created"] + created,
              "updated": batch["updated"] + updated,
              "skipped": batch["skipped"] + skipped}
    sheet = view["name"]
    if last and fresh and not totals["created"] and not totals["updated"]:
        # A file whose every row was rejected leaves no tab behind to tidy up.
        await _delete_view(runtime, org_bi, view_id)
        view_id, view = "", None
    await runtime.audit(
        actor, "crm.contacts.import", "organization", org_name,
        details={"created": created, "updated": updated, "skipped": skipped,
                 "view": view_id},
    )
    if last and view_id:
        await _record_list_activity(
            runtime, org_bi, [view_id], account_bi, actor, "contacts.imported",
            dict(totals, fileName=file_name,
                 rows=totals["created"] + totals["updated"] + totals["skipped"]))
    if last:
        body = ("%s: %s." % (sheet, _import_totals_copy(**totals)) if view
                else "Nothing was imported: %d row%s skipped." % (
                    totals["skipped"], "" if totals["skipped"] == 1 else "s"))
        problems = (batch["errors"] + errors)[:CSV_IMPORT_PING_ERRORS]
        if problems:
            body += " " + " ".join(
                _import_row_error(item["row"], item["error"])["message"]
                for item in problems)
        await _import_ping(
            runtime, actor,
            ("CRM import finished with %d skipped row%s: %s" % (
                totals["skipped"], "" if totals["skipped"] == 1 else "s",
                file_name) if totals["skipped"]
             else "CRM import finished: " + file_name),
            body, ping_key + ":end")
    return _response(runtime, {
        "ok": True, "created": created, "updated": updated,
        "skipped": skipped, "errors": errors, "view": view,
        "viewId": view_id, "delimiter": delimiter,
        "import": dict(totals, id=batch["id"], part=batch["part"],
                       parts=batch["parts"], done=last),
    })


async def _contact_handle_index(runtime, org_bi):
    """Every fediverse handle the organization's contacts carry, mapped to the
    most recently updated contact that names it."""
    rows = await runtime.d1_all(
        "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? "
        "ORDER BY updated_at DESC,contact_id LIMIT ?",
        org_bi, MAX_CONTACTS,
    )
    index = {}
    for row in rows or []:
        stored = await runtime.open(row.get("data"))
        for handle in _contact_handles(stored if isinstance(stored, dict) else {}):
            index.setdefault(handle, row["contact_id"])
    return index


async def _upsert_contact(runtime, org_bi, org_name, account_bi, actor, supplied,
                          view_id, now, default_source, flows, handles=None):
    """Create or merge one contact row into a sheet. Only the values supplied
    are applied, so a narrow row never blanks a wider stored record.

    A row without an email is matched by its Mastodon handle instead when the
    caller passes ``handles`` - a dict the import shares across its rows, whose
    ``"index"`` is read on first need - so importing a follower list again
    updates the followers it brought in before rather than adding them twice."""
    contact = _flow_placement(_contact_data(dict(
        supplied, views=[view_id],
        source=supplied.get("source") or default_source)), flows)
    if not contact.get("name") and not contact.get("email"):
        return "skipped", "name_or_email_required"
    if supplied.get("email") and not contact.get("email"):
        return "skipped", "invalid_email"
    email_bi = await _unique_email_bi(runtime, org_bi, contact.get("email"))
    prior = await runtime.d1_first(
        "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? AND email_bi=?",
        org_bi, email_bi,
    ) if email_bi else None
    handle = (_handle(contact.get("mastodon"))
              if handles is not None and not email_bi else "")
    if handle:
        if handles.get("index") is None:
            handles["index"] = await _contact_handle_index(runtime, org_bi)
        known = handles["index"].get(handle)
        prior = await runtime.d1_first(
            "SELECT contact_id,data FROM crm_contacts WHERE org_bi=? "
            "AND contact_id=?", org_bi, known,
        ) if known else None
    if prior:
        existing = await runtime.open(prior.get("data"))
        merged = _flow_placement(
            _contact_data(dict(supplied, views=[view_id]),
                          existing if isinstance(existing, dict) else {}), flows)
        await runtime.d1_run(
            "UPDATE crm_contacts SET updated_by_bi=?,updated_by_name=?,"
            "updated_at=?,data=? WHERE org_bi=? AND contact_id=?",
            account_bi, actor, now, await runtime.seal(merged), org_bi,
            prior["contact_id"],
        )
        return "updated", ""
    contact_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO crm_contacts(contact_id,org_bi,org_name,email_bi,"
        "created_by_bi,created_by_name,updated_by_bi,updated_by_name,created_at,"
        "updated_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        contact_id, org_bi, org_name, email_bi, account_bi, actor,
        account_bi, actor, now, now, await runtime.seal(contact),
    )
    if handle:
        handles["index"][handle] = contact_id
    return "created", ""


# --- sheets shared across organizations ------------------------------------
# A sheet's owner may share it with any ForkMesh user by name, not only with
# teammates. Outside the owning organization the sheet is read-only, shows only
# its visible columns, and can be copied into the reader's own CRM as a new tab.


def _shared_view_public(view, row, stages=()):
    """The projection a reader outside the organization gets: visible columns
    only, no management rights, and not the rest of the share list."""
    return {
        "canEdit": view["shareAccess"] == "edit",
        "stages": list(stages),
        "id": view["id"],
        "name": view["name"],
        "organization": str(row.get("org_name") or ""),
        "columns": [column for column in view["columns"] if not column["hidden"]],
        "visibility": view["visibility"],
        "sharedWith": [],
        "source": view["source"],
        "fileName": view["fileName"],
        "flow": view["flow"],
        "owner": view["owner"],
        "isOwner": False,
        "canManage": False,
        "shared": True,
        "createdAt": view["createdAt"],
        "updatedAt": view["updatedAt"],
    }


def _shared_contact(contact, columns, placed=False):
    """Only the columns the sheet shows leave the organization."""
    result = {"id": contact["id"], "views": list(contact.get("views") or []),
              "fields": {}, "tags": []}
    if placed:
        result["flow"], result["stage"] = contact.get("flow"), contact.get("stage")
    for column in columns:
        field = column["field"]
        if column.get("custom"):
            value = str((contact.get("fields") or {}).get(field) or "")
            if value:
                result["fields"][field] = value
        elif field in contact:
            result[field] = contact[field]
    return result


async def _shared_view_access(runtime, account_bi, actor, view_id):
    if not CONTACT_ID_RE.fullmatch(str(view_id or "")):
        return None
    share = await runtime.d1_first(
        "SELECT org_bi FROM crm_view_shares WHERE view_id=? AND shared_bi=?",
        view_id, await _share_bi(runtime, actor))
    if not share:
        return None
    org_bi = str(share.get("org_bi") or "")
    row = await _view_row(runtime, org_bi, view_id)
    if not row or not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return None
    view = await _view_project(runtime, row, "", "")
    # The sealed list is authoritative; the index row only finds it.
    if str(actor or "").lower() not in view["sharedWith"]:
        return None
    flows = await _org_flows(runtime, org_bi)
    stages = next((flow["stages"] for flow in flows if flow["id"] == view["flow"]), [])
    return org_bi, row, view, _shared_view_public(view, row, stages)


async def _member_org(runtime, org_name, org_bi, account_bi):
    member_bi, role = await runtime.organization(org_name, account_bi)
    if not role or member_bi != org_bi:
        return ""
    return role if await runtime.plugin_installed(org_bi, PLUGIN_ID) else ""


async def _open_link(runtime, account_bi, actor, item):
    view = await runtime.d1_first(
        "SELECT view_id,org_bi,org_name,owner_bi FROM crm_views WHERE view_id=?", item)
    if view:
        org_bi, org_name = str(view.get("org_bi") or ""), str(view.get("org_name") or "")
        role = await _member_org(runtime, org_name, org_bi, account_bi)
        row = await _view_row(runtime, org_bi, item) if role else None
        project = await _view_project(runtime, row, account_bi, role) if row else None
        if project and _view_readable(
                project, str(row.get("owner_bi") or ""), account_bi, actor, role):
            return _response(runtime, {"ok": True, "kind": "list", "organization": org_name})
        if await _shared_view_access(runtime, account_bi, actor, item):
            return _response(runtime, {"ok": True, "kind": "list", "shared": True})
        return _response(runtime, {"error": "link_not_found"}, 404)
    contact = await runtime.d1_first(
        "SELECT org_bi,org_name FROM crm_contacts WHERE contact_id=?", item)
    if contact and await _member_org(
            runtime, str(contact.get("org_name") or ""),
            str(contact.get("org_bi") or ""), account_bi):
        return _response(runtime, {"ok": True, "kind": "contact",
                                   "organization": str(contact.get("org_name") or "")})
    return _response(runtime, {"error": "link_not_found"}, 404)


async def _shared_view_bundle(runtime, account_bi, actor, view_id):
    """The sheet ``view_id`` as shared with ``actor``, with its contacts, or
    None when no such share reaches this user."""
    access = await _shared_view_access(runtime, account_bi, actor, view_id)
    if access is None:
        return None
    org_bi, _, view, public = access
    pipeline_lists = await _org_pipeline_lists(runtime, org_bi)
    contacts = [_shared_contact(contact, public["columns"], public["canEdit"])
                for contact in await _contacts(runtime, org_bi)
                if _contact_in_view(contact, view, pipeline_lists)]
    return org_bi, public, contacts


def _shared_edit(data, public):
    columns = {column["field"]: column for column in public["columns"]}
    edit = {key: data[key] for key in SHARED_EDIT_FIELDS
            if key in data and key in columns and not columns[key].get("custom")}
    fields = data.get("fields") if isinstance(data.get("fields"), dict) else {}
    custom = {key: value for key, value in fields.items()
              if key in columns and columns[key].get("custom")}
    if custom:
        edit["fields"] = custom
    return edit


async def _shared_contact_write(runtime, account_bi, actor, view_id, contact_id, data):
    access = await _shared_view_access(runtime, account_bi, actor, view_id)
    if access is None:
        return _response(runtime, {"error": "view_not_found"}, 404)
    org_bi, row, view, public = access
    if not public["canEdit"]:
        return _response(runtime, {"error": "view_read_only"}, 403)
    org_name = str(row.get("org_name") or "")
    edit = _shared_edit(data, public)
    stage = _slug(data.get("stage"), "") if "stage" in data else ""
    if "stage" in data and (not view["flow"] or stage not in public["stages"]):
        return _response(runtime, {"error": "invalid_stage"}, 400)
    now = runtime.now()
    if not contact_id:
        supplied = dict(edit, views=[view_id], owner=actor,
                        source=f"shared sheet ({actor})")
        if view["flow"]:
            supplied.update(flow=view["flow"], stage=stage or public["stages"][0])
        contact = _flow_placement(_contact_data(supplied), await _org_flows(runtime, org_bi))
        if not contact.get("name") and not contact.get("email"):
            return _response(runtime, {"error": "name_or_email_required"}, 400)
        if data.get("email") and not contact.get("email"):
            return _response(runtime, {"error": "invalid_email"}, 400)
        email_bi = await _unique_email_bi(runtime, org_bi, contact.get("email"))
        if email_bi and await runtime.d1_first(
                "SELECT contact_id FROM crm_contacts WHERE org_bi=? AND email_bi=?",
                org_bi, email_bi):
            return _response(runtime, {"error": "contact_exists"}, 409)
        contact_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO crm_contacts(contact_id,org_bi,org_name,email_bi,"
            "created_by_bi,created_by_name,updated_by_bi,updated_by_name,created_at,"
            "updated_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            contact_id, org_bi, org_name, email_bi, account_bi, actor, account_bi,
            actor, now, now, await runtime.seal(contact),
        )
        await runtime.audit(actor, "crm.shared.contact.create", "crm_contact",
                            contact_id, details={"view": view_id})
        await _record_contact_changes(runtime, org_bi, account_bi, actor,
                                      [(None, contact)])
        status = 201
    else:
        stored = await _contact_row(runtime, org_bi, contact_id)
        existing = await runtime.open(stored.get("data")) if stored else None
        pipeline_lists = await _org_pipeline_lists(runtime, org_bi)
        if not isinstance(existing, dict) or not _contact_in_view(
                dict(existing, id=contact_id), view, pipeline_lists):
            return _response(runtime, {"error": "contact_not_found"}, 404)
        if stage:
            edit["stage"] = stage
        contact = _contact_data(edit, existing)
        if not contact.get("name") and not contact.get("email"):
            return _response(runtime, {"error": "name_or_email_required"}, 400)
        if "email" in data and data.get("email") and not contact.get("email"):
            return _response(runtime, {"error": "invalid_email"}, 400)
        email_bi = await _unique_email_bi(runtime, org_bi, contact.get("email"))
        if email_bi and await runtime.d1_first(
                "SELECT contact_id FROM crm_contacts WHERE org_bi=? AND email_bi=? "
                "AND contact_id<>?", org_bi, email_bi, contact_id):
            return _response(runtime, {"error": "contact_exists"}, 409)
        await runtime.d1_run(
            "UPDATE crm_contacts SET email_bi=?,updated_by_bi=?,updated_by_name=?,"
            "updated_at=?,data=? WHERE org_bi=? AND contact_id=?",
            email_bi, account_bi, actor, now, await runtime.seal(contact), org_bi,
            contact_id,
        )
        await runtime.audit(actor, "crm.shared.contact.update", "crm_contact",
                            contact_id, details={"view": view_id})
        await _record_contact_changes(runtime, org_bi, account_bi, actor,
                                      [(existing, contact)])
        status = 200
    projected = await _contact_project(runtime, await _contact_row(runtime, org_bi, contact_id))
    return _response(runtime, {
        "ok": True, "contact": _shared_contact(projected, public["columns"], True),
    }, status)


async def _shared_views(runtime, account_bi, actor):
    """Sheets other organizations shared with this user. Sheets shared inside
    an organization the user belongs to already sit in that CRM's tab strip."""
    rows = await runtime.d1_all(
        "SELECT v.view_id,v.org_bi,v.org_name,v.owner_bi,v.owner_name,"
        "v.created_at,v.updated_at,v.data FROM crm_view_shares s "
        "JOIN crm_views v ON v.view_id=s.view_id AND v.org_bi=s.org_bi "
        "WHERE s.shared_bi=? ORDER BY s.created_at,v.view_id LIMIT ?",
        await _share_bi(runtime, actor), MAX_VIEWS,
    )
    reachable = {}
    result = []
    for row in rows or []:
        org_bi = str(row.get("org_bi") or "")
        if org_bi not in reachable:
            _, role = await runtime.organization(
                str(row.get("org_name") or ""), account_bi)
            reachable[org_bi] = (not role
                                 and await runtime.plugin_installed(org_bi, PLUGIN_ID))
        if not reachable[org_bi]:
            continue
        view = await _view_project(runtime, row, "", "")
        if str(actor or "").lower() not in view["sharedWith"]:
            continue
        result.append(_shared_view_public(view, row))
    return result


async def account_shared_access(runtime, account_bi, actor):
    return bool(await _shared_views(runtime, account_bi, actor))


async def _copy_shared_view(runtime, org_bi, org_name, account_bi, actor, role,
                            data):
    """Copy a sheet another organization shared with me into this one: a new
    tab with the same visible columns, and its rows upserted by email."""
    bundle = await _shared_view_bundle(
        runtime, account_bi, actor, _clean(data.get("viewId"), 32).lower())
    if bundle is None:
        return _response(runtime, {"error": "view_not_found"}, 404)
    source_org_bi, source, contacts = bundle
    if source_org_bi == org_bi:
        return _response(runtime, {"error": "view_already_in_organization"}, 409)
    view = _view_data({
        "name": _clean(data.get("name"), 60) or source["name"],
        "columns": _default_view_columns(_view_columns(source["columns"])),
        "visibility": "organization",
        "source": "shared",
        "fileName": "",
        "position": MAX_VIEWS,
    })
    view_id = await _store_view(runtime, org_bi, org_name, account_bi, actor, view,
                                sync_shares=True)
    created = updated = skipped = 0
    now = runtime.now()
    flows = (await _settings(runtime, org_bi))["flows"]
    for contact in contacts:
        supplied = {key: value for key, value in contact.items()
                    if key not in ("id", "views", "fields", "tags")
                    and str(value or "").strip()}
        if contact.get("tags"):
            supplied["tags"] = list(contact["tags"])
        if contact.get("fields"):
            supplied["fields"] = dict(contact["fields"])
        outcome, _ = await _upsert_contact(
            runtime, org_bi, org_name, account_bi, actor, supplied, view_id,
            now, "shared", flows)
        created += outcome == "created"
        updated += outcome == "updated"
        skipped += outcome == "skipped"
    await runtime.audit(
        actor, "crm.view.copy", "crm_view", view_id,
        details={"from": source["organization"], "source": source["id"],
                 "created": created, "updated": updated, "skipped": skipped},
    )
    return _response(runtime, {
        "ok": True, "created": created, "updated": updated, "skipped": skipped,
        "viewId": view_id,
        "view": await _view_project(
            runtime, await _view_row(runtime, org_bi, view_id), account_bi, role),
    }, 201)


# --- emails tied to contacts from the mail client ---------------------------
# The browser extension sends the Gmail or Zoho Mail message it has open. The
# CRM keeps the provider's message id on the contact's email history and the
# permalink that reopens the message in the mailbox it came from.


def _email_provider(value):
    value = _clean(value, 20).lower()
    return value if value in EMAIL_PROVIDERS else "other"


def _email_direction(value):
    value = _clean(value, 20).lower()
    return value if value in EMAIL_DIRECTIONS else "outbound"


def _gmail_url(message_id, account_index=0):
    """The Gmail permalink for a message or thread id read from the page."""
    message_id = _clean(message_id, 200)
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,200}", message_id):
        return ""
    return "https://mail.google.com/mail/u/%d/#all/%s" % (
        max(0, min(9, _integer(account_index))), message_id)


def _email_link_fields(data, existing=None):
    """The provider identity every recorded email may carry. A re-link that
    leaves a field out keeps what the row already knows."""
    existing = existing if isinstance(existing, dict) else {}
    return {
        "provider": _email_provider(data.get("provider", existing.get("provider"))),
        "direction": _email_direction(
            data.get("direction", existing.get("direction"))),
        "threadId": _clean(data.get("threadId", existing.get("threadId")), 200),
        "mailbox": _email(data.get("mailbox", existing.get("mailbox"))),
        # A Mastodon conversation names its people by handle, not by address.
        "handle": _handle(data.get("handle", existing.get("handle"))),
        "account": _handle(data.get("account", existing.get("account"))),
    }


async def _contact_emails(runtime, org_bi, contact_id):
    rows = await runtime.d1_all(
        "SELECT send_id,org_bi,org_name,contact_id,sender_name,transport,status,"
        "external_id,created_at,sent_at,status_at,data FROM crm_email_sends "
        "WHERE org_bi=? AND contact_id=? ORDER BY created_at DESC,send_id LIMIT 50",
        org_bi, contact_id,
    )
    return [await _email_project(runtime, row, include_content=False)
            for row in rows or []]


async def _contact_row_by_email(runtime, org_bi, email):
    email_bi = await _unique_email_bi(runtime, org_bi, email)
    if not email_bi:
        return None
    row = await runtime.d1_first(
        "SELECT contact_id FROM crm_contacts WHERE org_bi=? AND email_bi=?",
        org_bi, email_bi)
    if not row:
        return None
    return await _contact_row(runtime, org_bi, str(row.get("contact_id") or ""))


# --- boxes: one record per person ------------------------------------------
# A box is the one record the CRM keeps for a person, named after them. A
# linked email, a Mastodon thread, and the extension's lookup all find the
# person's box before making another: by an address anywhere on it, a
# fediverse handle, a LinkedIn, GitHub or X profile, a phone number, or the
# name itself when the address or the company backs it up. A name that fits
# two boxes equally well matches neither, so two different "Alex"es never
# merge; a name that contradicts the box's ("Robyn Moss" on "Robin Moss")
# never matches on the weaker signals.
BOX_STRONG_SCORE = 80
BOX_MATCH_SCORE = 50
BOX_EMAIL_ANY_RE = re.compile(
    r"[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,62}\.)+"
    r"[A-Za-z]{2,63}")
BOX_PROFILE_RES = (
    ("linkedin", re.compile(
        r"linkedin\.com/(?:in|pub)/([A-Za-z0-9_%-]{2,100})", re.IGNORECASE)),
    ("github", re.compile(
        r"(?:^|[^A-Za-z0-9.-])(?:www\.)?github\.com/([A-Za-z0-9-]{1,39})"
        r"(?![A-Za-z0-9-])", re.IGNORECASE)),
    ("twitter", re.compile(
        r"(?:^|[^A-Za-z0-9.-])(?:www\.|mobile\.)?(?:twitter|x)\.com/"
        r"([A-Za-z0-9_]{1,15})(?![A-Za-z0-9_])", re.IGNORECASE)),
)
# What a bare GitHub or X field holds when it is not a URL: the username.
BOX_BARE_PROFILE_RES = {
    "github": re.compile(r"@?([A-Za-z0-9-]{1,39})"),
    "twitter": re.compile(r"@?([A-Za-z0-9_]{1,15})"),
}
BOX_PROFILE_PATHS = frozenset({
    "home", "i", "intent", "share", "search", "explore", "orgs", "sponsors",
    "settings", "login", "notifications", "messages"})
# Mail hosts that say nothing about where someone works.
BOX_FREE_MAIL = frozenset({
    "aol.com", "fastmail.com", "gmail.com", "gmx.com", "gmx.de", "gmx.net",
    "googlemail.com", "hey.com", "hotmail.com", "icloud.com", "live.com",
    "mac.com", "me.com", "msn.com", "outlook.com", "pm.me", "proton.me",
    "protonmail.com", "qq.com", "tutanota.com", "web.de", "yahoo.com",
    "yandex.com", "zoho.com", "zohomail.com"})


def _box_squash(value):
    return re.sub(r"[^a-z0-9]+", "", str(value or "").lower())


def _box_name_tokens(value):
    """A person's name as lowercase words: addresses, handles and links in
    it are dropped, and so are initials."""
    value = re.sub(r"\S*[@/]\S*", " ", _clean(value, 160))
    return [word for word in re.findall(r"[^\W\d_]+", value.casefold())
            if len(word) > 1]


def _box_emails(data):
    """Every address a box carries: its email, and any address kept in a
    custom column ("Work email", "Other emails", ...)."""
    found = set()
    email = _email(data.get("email"))
    if email:
        found.add(email)
    fields = data.get("fields")
    for value in (fields.values() if isinstance(fields, dict) else ()):
        for match in BOX_EMAIL_ANY_RE.finditer(_text(value, MAX_CUSTOM_FIELD_BYTES)):
            email = _email(match.group(0))
            if email:
                found.add(email)
    return found


def _box_profiles(data):
    """The LinkedIn, GitHub and X accounts a box names, as "site:user"."""
    found = set()
    for site, pattern in BOX_PROFILE_RES:
        for key in (site, "website"):
            value = _text(data.get(key), 2000)
            for match in pattern.finditer(value):
                user = match.group(1).strip("/").lower()
                if user and user not in BOX_PROFILE_PATHS:
                    found.add(site + ":" + user)
        bare = BOX_BARE_PROFILE_RES.get(site)
        value = _clean(data.get(site), 80)
        match = bare.fullmatch(value) if bare and value else None
        if match and match.group(1).lower() not in BOX_PROFILE_PATHS:
            found.add(site + ":" + match.group(1).lower())
    return found


def _box_phones(value):
    """Phone numbers by their last nine digits, so "+1 (415) 555-0100" and
    "415.555.0100" are one number."""
    found = set()
    for part in re.split(r"[,;/]|\bor\b", _text(value, 200)):
        digits = re.sub(r"\D", "", part)
        if len(digits) >= 7:
            found.add(digits[-9:])
    return found


def _box_host(value):
    match = re.match(r"(?:[a-z][a-z0-9+.-]*://)?(?:www\.)?([a-z0-9.-]+\.[a-z]{2,63})",
                     _clean(value, 320).lower())
    return match.group(1) if match else ""


def _box_identity(data):
    """What a box, or a message about someone, says about who they are."""
    data = data if isinstance(data, dict) else {}
    emails = _box_emails(data)
    domains = {email.rsplit("@", 1)[1] for email in emails}
    website = _box_host(data.get("website"))
    if website:
        domains.add(website)
    return {
        "emails": emails,
        "locals": {email.rsplit("@", 1)[0] for email in emails},
        "handles": _contact_handles(data) | ({_handle(data.get("handle"))} - {""}),
        "profiles": _box_profiles(data),
        "phones": _box_phones(data.get("phone")),
        "name": _box_name_tokens(data.get("name")),
        "company": _box_squash(data.get("company")),
        "domains": {domain for domain in domains if domain not in BOX_FREE_MAIL},
    }


def _box_name_in_address(name, local):
    """Whether an address's local part spells the name: "robin.moss",
    "robinmoss", "rmoss", or the first name alone ("robin@")."""
    words = re.findall(r"[^\W\d_]+", local.casefold())
    joined = "".join(words)
    if not name or not words:
        return False
    if set(name) <= set(words) or words == name[:1]:
        return True
    return len(joined) >= 4 and joined in (
        "".join(name), name[0] + name[-1], name[0][0] + name[-1])


def _box_score(incoming, box):
    """How surely a message's person is this box's person, and why."""
    if incoming["emails"] & box["emails"]:
        return 100, "email"
    if incoming["handles"] & box["handles"]:
        return 100, "handle"
    if incoming["profiles"] & box["profiles"]:
        return 90, "profile"
    ours, theirs = set(incoming["name"]), set(box["name"])
    if ours and theirs and not (ours <= theirs or theirs <= ours):
        return 0, ""  # the names disagree, and nothing stronger said otherwise
    if incoming["phones"] & box["phones"]:
        return BOX_STRONG_SCORE, "phone"
    score = 0
    if ours and ours == theirs:
        score = 60 if len(ours) > 1 else 30
    elif ours and theirs and incoming["name"][0] == box["name"][0]:
        score = 30  # "Robin" and "Robin Moss"
    if score and (
            any(_box_name_in_address(box["name"], local) for local in incoming["locals"])
            or any(_box_name_in_address(incoming["name"], local)
                   for local in box["locals"])):
        score += 25
    if score and ((incoming["company"] and incoming["company"] == box["company"])
                  or incoming["domains"] & box["domains"]):
        score += 20
    return score, "name"


async def _matching_box(runtime, org_bi, data):
    """The box a person belongs in, and what matched it: ``(row, how)``, or
    ``(None, "")``. An exact address takes the indexed path; everything else
    reads the organization's boxes the way the dashboard list does."""
    incoming = _box_identity(data)
    for email in sorted(incoming["emails"]):
        row = await _contact_row_by_email(runtime, org_bi, email)
        if row:
            return row, "email"
    if not any(incoming[key] for key in ("emails", "handles", "profiles", "phones",
                                         "name")):
        return None, ""
    rows = await runtime.d1_all(
        "SELECT contact_id,org_bi,org_name,created_by_name,updated_by_name,"
        "created_at,updated_at,data FROM crm_contacts WHERE org_bi=? "
        "ORDER BY updated_at DESC,contact_id LIMIT ?",
        org_bi, MAX_CONTACTS,
    )
    best, best_score, best_how, tied = None, 0, "", False
    for row in rows or []:
        stored = await runtime.open(row.get("data"))
        score, how = _box_score(incoming, _box_identity(stored))
        if score > best_score:
            best, best_score, best_how, tied = row, score, how, False
        elif score and score == best_score:
            tied = True  # the most recently updated box keeps a strong tie
    if best_score < BOX_MATCH_SCORE or (tied and best_score < BOX_STRONG_SCORE):
        return None, ""
    return best, best_how


def _handles_in(value):
    """Every fediverse handle a stored field names, as "user@host". A record
    holds whatever was typed or imported, which the desktop reads loosely:
    "http://" or scheme-less profile URLs, "/users/<name>" paths, a trailing
    post id or query, spaces around the "@", or a handle inside a sentence."""
    value = _text(value, 2000)
    found = set()
    for match in HANDLE_URL_ANY_RE.finditer(value):
        found.add(("%s@%s" % (match.group(2), match.group(3) or match.group(1))).lower())
    for match in HANDLE_ANY_RE.finditer(value):
        found.add(("%s@%s" % (match.group(1), match.group(2))).lower())
    return found


def _contact_handles(data):
    """The handles a contact can be found by: its Mastodon field, then the
    places a profile link also ends up (website, Twitter, or an imported
    custom column named for Mastodon or the fediverse)."""
    found = set()
    for key in ("mastodon", "website", "twitter"):
        found |= _handles_in(data.get(key))
    fields = data.get("fields")
    for key, value in (fields.items() if isinstance(fields, dict) else ()):
        if any(word in str(key).lower() for word in ("mastodon", "fedi")):
            found |= _handles_in(value)
    return found


def _link_details(data):
    """The contact details a link or a lookup read off the message. They fill
    a new box and only the blanks of an existing one."""
    details = data.get("contact") if isinstance(data.get("contact"), dict) else {}
    details = {key: details.get(key) for key in LINK_CONTACT_FIELDS if details.get(key)}
    if data.get("contactCompany") and "company" not in details:
        details["company"] = data.get("contactCompany")
    return details


async def _lookup_contact(runtime, org_bi, data):
    """The box behind an address or a fediverse handle, with the messages
    already tied to it. A POST so neither lands in a URL or a log line. The
    name and details on the message find a box that has neither yet."""
    email = _email(data.get("email"))
    handle = _handle(data.get("handle"))
    if not email and not handle:
        return _response(runtime, {"error": "invalid_email"}, 400)
    # The organization's pipelines ride along so the extension can offer
    # "add to pipeline" without a second round trip.
    flows = (await _settings(runtime, org_bi))["flows"]
    row, matched_by = await _matching_box(runtime, org_bi, {
        **_link_details(data), "email": email, "handle": handle,
        "name": data.get("name")})
    if not row:
        return _response(runtime, {"ok": True, "contact": None, "emails": [],
                                   "flows": flows})
    contact = await _contact_project(runtime, row)
    return _response(runtime, {
        "ok": True, "contact": contact, "matchedBy": matched_by,
        "emails": await _contact_emails(runtime, org_bi, contact["id"]),
        "flows": flows,
    })


async def _linked_email_row(runtime, org_bi, provider, message_id):
    """The send row already holding this provider message, if any."""
    rows = await runtime.d1_all(
        "SELECT send_id,data FROM crm_email_sends WHERE org_bi=? AND external_id=? "
        "ORDER BY created_at DESC,send_id LIMIT 20", org_bi, message_id)
    for row in rows or []:
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        if _email_provider(data.get("provider")) == provider:
            return row, data
    return None, {}


# Contact details the extension reads off an email (display name, the
# sender's domain, a signature). They fill a new contact and only the blanks
# of an existing one: what the CRM already holds is never overwritten.
LINK_CONTACT_FIELDS = ("company", "title", "phone", "website", "linkedin",
                       "twitter", "github", "mastodon", "bluesky", "location")


def _pipeline_choice(flows, data, contact_data):
    """The flow and stage a link asked for, checked against the organization's
    flows. Returns (flow, stage, error); ("", "", "") when none was asked."""
    if not data.get("flow") and not data.get("stage"):
        return "", "", ""
    flow_id = (_slug(data.get("flow"), "") or _slug(contact_data.get("flow"), "")
               or flows[0]["id"])
    flow = next((item for item in flows if item["id"] == flow_id), None)
    if not flow:
        return "", "", "invalid_flow"
    stage = _slug(data.get("stage"), "")
    if not stage:
        current = _slug(contact_data.get("stage"), "")
        same_flow = _slug(contact_data.get("flow"), "") == flow_id
        stage = current if same_flow and current in flow["stages"] else flow["stages"][0]
    if stage not in flow["stages"]:
        return "", "", "invalid_stage"
    return flow_id, stage, ""


async def _link_email(runtime, org_bi, org_name, account_bi, actor, data):
    """Tie one open message to a contact: a Gmail or Zoho Mail email, or a
    Mastodon conversation, which names its people by handle instead of by
    address. The contact is matched by the other party's address or handle and
    created when the organization does not know it yet; linking the same
    message twice updates one row. A flow and stage on the request also put
    the contact in that pipeline."""
    provider = _email_provider(data.get("provider"))
    message_id = _clean(data.get("messageId") or data.get("externalId"), 200)
    if not message_id:
        return _response(runtime, {"error": "message_id_required"}, 400)
    requested_url = data.get("providerUrl")
    provider_url = _https(requested_url)
    if requested_url and not provider_url:
        return _response(runtime, {"error": "invalid_provider_url"}, 400)
    if not provider_url and provider == "gmail":
        provider_url = _gmail_url(message_id, data.get("accountIndex"))
    existing, envelope = await _linked_email_row(runtime, org_bi, provider,
                                                 message_id)
    link = _email_link_fields(data, envelope)
    counterpart = _email(data.get("contactEmail"))
    if data.get("contactEmail") and not counterpart:
        return _response(runtime, {"error": "invalid_email"}, 400)
    handle = link["handle"]
    if data.get("handle") and not handle:
        return _response(runtime, {"error": "invalid_handle"}, 400)
    details = _link_details(data)
    if handle and not details.get("mastodon"):
        details["mastodon"] = "@" + handle
    contact_id = _clean(data.get("contactId"), 64).lower()
    row, matched_by = None, ""
    if contact_id:
        if not CONTACT_ID_RE.fullmatch(contact_id):
            return _response(runtime, {"error": "invalid_contact_id"}, 400)
        row = await _contact_row(runtime, org_bi, contact_id)
        if not row:
            return _response(runtime, {"error": "contact_not_found"}, 404)
        matched_by = "id"
    elif counterpart or handle:
        # The person's box, however this message names them: a thread and an
        # email with the same person, or a box filed under another address or
        # only a name, all land on the one record.
        row, matched_by = await _matching_box(runtime, org_bi, {
            **details, "email": counterpart, "handle": handle,
            "name": data.get("contactName")})
    else:
        return _response(runtime, {
            "error": "contact_handle_required" if provider == "mastodon"
                     else "contact_email_required"}, 400)
    flows = (await _settings(runtime, org_bi))["flows"]
    existing_contact = await runtime.open(row.get("data")) if row else {}
    flow_id, stage, error = _pipeline_choice(
        flows, data, existing_contact if isinstance(existing_contact, dict) else {})
    if error:
        return _response(runtime, {"error": error}, 400)
    now = runtime.now()
    contact_created = False
    if not row:
        if data.get("createContact") is False:
            return _response(runtime, {"error": "contact_not_found"}, 404)
        contact = _contact_data({
            **details,
            # A handle is a name of sorts: better an "@grace@host" record than
            # a nameless one when the conversation gave nothing else.
            "name": data.get("contactName") or (("@" + handle) if handle else ""),
            "email": counterpart,
            "source": ("mastodon conversation" if provider == "mastodon"
                       else provider + " email"),
            **({"flow": flow_id, "stage": stage} if flow_id else {}),
        })
        contact_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO crm_contacts(contact_id,org_bi,org_name,email_bi,"
            "created_by_bi,created_by_name,updated_by_bi,updated_by_name,"
            "created_at,updated_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            contact_id, org_bi, org_name,
            await _unique_email_bi(runtime, org_bi, counterpart), account_bi,
            actor, account_bi, actor, now, now, await runtime.seal(contact),
        )
        await runtime.audit(actor, "crm.contact.create", "crm_contact", contact_id)
        row = await _contact_row(runtime, org_bi, contact_id)
        contact_created = True
    contact_id = str(row.get("contact_id") or "")
    contact_data = await runtime.open(row.get("data"))
    contact_data = contact_data if isinstance(contact_data, dict) else {}
    # An address the box does not carry yet joins it, so the next message
    # from it matches straight away: as its email when it has none, else
    # beside the others. One another box already holds stays there.
    email_filled = address_added = False
    if (counterpart and not contact_created
            and counterpart not in _box_emails(contact_data)
            and not await _contact_row_by_email(runtime, org_bi, counterpart)):
        if not contact_data.get("email"):
            contact_data = _contact_data({"email": counterpart}, contact_data)
            email_filled = True
        else:
            fields = contact_data.get("fields")
            others = str((fields if isinstance(fields, dict) else {})
                         .get("other_emails") or "").strip()
            contact_data = _contact_data({"fields": {"other_emails": (
                others + ", " if others else "") + counterpart}}, contact_data)
        address_added = True
    counterpart = counterpart or str(contact_data.get("email") or "")
    sent_at = max(0, _integer(data.get("sentAt"), now)) or now
    envelope = dict(envelope)
    envelope.update({
        "to": counterpart if link["direction"] == "outbound" else link["mailbox"],
        "fromEmail": link["mailbox"] if link["direction"] == "outbound"
                     else counterpart,
        "fromName": _clean(data.get("fromName"), 120) or (
            "" if link["direction"] == "outbound"
            else _clean(data.get("contactName") or contact_data.get("name"), 120)),
        "subject": _clean(data.get("subject"), 300)
                   or str(envelope.get("subject") or ""),
        "text": _text(data.get("text"), 20000) or str(envelope.get("text") or ""),
        "html": "",
        "providerUrl": provider_url or str(envelope.get("providerUrl") or ""),
        "error": "",
        **link,
    })
    if existing:
        send_id = str(existing.get("send_id") or "")
        await runtime.d1_run(
            "UPDATE crm_email_sends SET contact_id=?,sent_at=?,status_at=?,data=? "
            "WHERE org_bi=? AND send_id=?",
            contact_id, sent_at, now, await runtime.seal(envelope), org_bi, send_id,
        )
    else:
        send_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO crm_email_sends(send_id,org_bi,org_name,contact_id,"
            "sender_bi,sender_name,transport,status,external_id,created_at,"
            "sent_at,status_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            send_id, org_bi, org_name, contact_id, account_bi, actor, "external",
            "external", message_id, now, sent_at, now, await runtime.seal(envelope),
        )
    # A linked message is contact history: it moves "last contacted" forward
    # but never back.
    changed = address_added
    if sent_at > _integer(contact_data.get("lastContactedAt")):
        contact_data["lastContactedAt"] = sent_at
        changed = True
    if not contact_created:
        filled = _contact_data({
            key: value for key, value in (
                *details.items(), ("name", data.get("contactName")))
            if value and not contact_data.get(key)}, contact_data)
        if filled != contact_data:
            contact_data, changed = filled, True
    if flow_id and (contact_data.get("flow"), contact_data.get("stage")) != (flow_id, stage):
        contact_data["flow"], contact_data["stage"] = flow_id, stage
        changed = True
    if changed:
        await runtime.d1_run(
            "UPDATE crm_contacts SET updated_by_bi=?,updated_by_name=?,"
            "updated_at=?,data=? WHERE org_bi=? AND contact_id=?",
            account_bi, actor, now, await runtime.seal(contact_data), org_bi,
            contact_id,
        )
    if email_filled:
        await runtime.d1_run(
            "UPDATE crm_contacts SET email_bi=? WHERE org_bi=? AND contact_id=?",
            await _unique_email_bi(runtime, org_bi, counterpart), org_bi, contact_id)
    await runtime.audit(
        actor, "crm.email.link", "crm_email_send", send_id,
        details={"provider": provider, "contactCreated": contact_created,
                 "relinked": bool(existing), "pipeline": bool(flow_id)},
    )
    if not existing:
        await _celebrate_tracked_email(runtime, actor, send_id,
                                       envelope["subject"], contact_data)
    return _response(runtime, {
        "ok": True,
        "contactCreated": contact_created,
        "matchedBy": "" if contact_created else matched_by,
        "relinked": bool(existing),
        "flows": flows,
        "email": await _email_project(
            runtime, await _email_row(runtime, org_bi, send_id)),
        "contact": await _contact_project(
            runtime, await _contact_row(runtime, org_bi, contact_id)),
        "emails": await _contact_emails(runtime, org_bi, contact_id),
    }, 200 if existing else 201)


def _provider_url(template, external_id):
    template = _https(template)
    external_id = _clean(external_id, 200)
    if not template or not external_id:
        return ""
    rendered = (template.replace("{id}", external_id)
                if "{id}" in template
                else template.rstrip("/") + "/" + external_id)
    return _https(rendered)


async def _record_external_send(runtime, org_bi, org_name, account_bi, actor, data):
    recipient = _email(data.get("to"))
    if not recipient:
        return _response(runtime, {"error": "invalid_recipient"}, 400)
    contact_id = _clean(data.get("contactId"), 64).lower()
    if contact_id and not CONTACT_ID_RE.fullmatch(contact_id):
        return _response(runtime, {"error": "invalid_contact_id"}, 400)
    if contact_id and not await _contact_row(runtime, org_bi, contact_id):
        return _response(runtime, {"error": "contact_not_found"}, 404)
    external_id = _clean(data.get("externalId"), 200)
    if not external_id:
        return _response(runtime, {"error": "external_id_required"}, 400)
    settings = await _settings(runtime, org_bi)
    requested_provider_url = data.get("providerUrl")
    provider_url = _https(requested_provider_url)
    if requested_provider_url and not provider_url:
        return _response(runtime, {"error": "invalid_provider_url"}, 400)
    provider_url = provider_url or _provider_url(
        settings.get("providerUrlTemplate"), external_id)
    status = _clean(data.get("status"), 40).lower() or "external"
    if status not in SEND_STATUSES:
        return _response(runtime, {"error": "invalid_send_status"}, 400)
    now = runtime.now()
    send_id = runtime.new_id()
    envelope = {
        "to": recipient,
        "fromEmail": _email(data.get("fromEmail")),
        "fromName": _clean(data.get("fromName"), 120),
        "subject": _clean(data.get("subject"), 300),
        "text": _text(data.get("text"), 100000),
        "html": _text(data.get("html"), 200000),
        "providerUrl": provider_url,
        "error": "",
        **_email_link_fields(data),
    }
    await runtime.d1_run(
        "INSERT INTO crm_email_sends(send_id,org_bi,org_name,contact_id,"
        "sender_bi,sender_name,transport,status,external_id,created_at,sent_at,"
        "status_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        send_id, org_bi, org_name, contact_id, account_bi, actor, "external",
        status, external_id, now, max(0, _integer(data.get("sentAt"), now)), now,
        await runtime.seal(envelope),
    )
    await runtime.audit(actor, "crm.email.link", "crm_email_send", send_id)
    contact_data = {"email": recipient}
    if contact_id:
        contact = await _contact_row(runtime, org_bi, contact_id)
        opened = await runtime.open(contact.get("data")) if contact else None
        await _record_contact_event(
            runtime, org_bi, account_bi, actor, opened,
            "contact.emailed", {"subject": envelope["subject"]})
        if isinstance(opened, dict):
            contact_data = opened
    await _celebrate_tracked_email(runtime, actor, send_id,
                                   envelope["subject"], contact_data)
    return _response(runtime, {
        "ok": True,
        "email": await _email_project(
            runtime, await _email_row(runtime, org_bi, send_id)),
    }, 201)


async def _send_mailtrap(runtime, org_bi, org_name, account_bi, actor, data):
    contact_id = _clean(data.get("contactId"), 64).lower()
    if contact_id and not CONTACT_ID_RE.fullmatch(contact_id):
        return _response(runtime, {"error": "invalid_contact_id"}, 400)
    contact = await _contact_row(runtime, org_bi, contact_id) if contact_id else None
    if contact_id and not contact:
        return _response(runtime, {"error": "contact_not_found"}, 404)
    contact_data = await runtime.open(contact.get("data")) if contact else {}
    contact_data = contact_data if isinstance(contact_data, dict) else {}
    recipient = _email(data.get("to") or contact_data.get("email"))
    if not recipient:
        return _response(runtime, {"error": "invalid_recipient"}, 400)
    subject = _clean(data.get("subject"), 300)
    text = _text(data.get("text"), 100000).strip()
    html = _text(data.get("html"), 200000).strip()
    if not subject or (not text and not html):
        return _response(runtime, {"error": "subject_and_message_required"}, 400)
    if _carries_deck(_clean(data.get("template"), 40).lower(), text, html):
        holder = contact or await _contact_row_by_email(runtime, org_bi, recipient)
        holder_data = (contact_data if contact else await runtime.open(
            holder.get("data"))) if holder else None
        refusal = await _deck_refusal(runtime, org_bi, holder_data) if isinstance(
            holder_data, dict) else None
        if refusal:
            return refusal
    settings = await _settings(runtime, org_bi)
    from_email = _email(data.get("fromEmail") or settings.get("fromEmail"))
    from_name = _clean(data.get("fromName") or settings.get("fromName"), 120)
    if not from_email:
        return _response(runtime, {"error": "invalid_sender"}, 400)
    token = _clean(data.get("mailtrapKey"), 2048)
    now = runtime.now()
    send_id = runtime.new_id()
    envelope = {
        "to": recipient, "fromEmail": from_email, "fromName": from_name,
        "subject": subject, "text": text, "html": html,
        "providerUrl": "", "error": "",
    }
    await runtime.d1_run(
        "INSERT INTO crm_email_sends(send_id,org_bi,org_name,contact_id,"
        "sender_bi,sender_name,transport,status,external_id,created_at,sent_at,"
        "status_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        send_id, org_bi, org_name, contact_id, account_bi, actor, "mailtrap",
        "sending", "", now, 0, now, await runtime.seal(envelope),
    )
    result = await runtime.send_mailtrap(token, {
        "from": {"email": from_email, "name": from_name or org_name},
        "to": [{"email": recipient}],
        "subject": subject,
        "text": text,
        "html": html,
        "category": "CRM one-off",
        "custom_variables": {
            "forkmesh_crm_send_id": send_id,
            "forkmesh_crm_org_bi": org_bi,
            "forkmesh_sender": actor,
        },
    })
    accepted = bool((result or {}).get("accepted"))
    external_id = _clean((result or {}).get("messageId"), 200)
    envelope["error"] = _clean((result or {}).get("error"), 500)
    envelope["providerUrl"] = _provider_url(
        settings.get("providerUrlTemplate"), external_id)
    status = "accepted" if accepted else "failed"
    sent_at = runtime.now()
    await runtime.d1_run(
        "UPDATE crm_email_sends SET status=?,external_id=?,sent_at=?,status_at=?,"
        "data=? WHERE org_bi=? AND send_id=?",
        status, external_id, sent_at if accepted else 0, sent_at,
        await runtime.seal(envelope), org_bi, send_id,
    )
    if contact:
        contact_data["lastContactedAt"] = sent_at if accepted else _integer(
            contact_data.get("lastContactedAt"))
        await runtime.d1_run(
            "UPDATE crm_contacts SET updated_by_bi=?,updated_by_name=?,updated_at=?,"
            "data=? WHERE org_bi=? AND contact_id=?",
            account_bi, actor, sent_at, await runtime.seal(contact_data), org_bi,
            contact_id,
        )
    await runtime.audit(
        actor, "crm.email.send", "crm_email_send", send_id,
        outcome="success" if accepted else "failed",
        details={"transport": "mailtrap", "accepted": accepted},
    )
    if contact and accepted:
        await _record_contact_event(runtime, org_bi, account_bi, actor,
                                    contact_data, "contact.emailed",
                                    {"subject": subject})
    if accepted:
        await _celebrate_tracked_email(runtime, actor, send_id, subject,
                                       contact_data or {"email": recipient})
    projected = await _email_project(
        runtime, await _email_row(runtime, org_bi, send_id))
    return _response(
        runtime,
        {"ok": accepted, "email": projected,
         "error": "mailtrap_send_failed" if not accepted else ""},
        201 if accepted else 502,
    )


async def _outreach_member(runtime, org_bi, account_bi):
    row = await runtime.d1_first(
        "SELECT member_bi FROM crm_outreach_team WHERE org_bi=? AND member_bi=?",
        org_bi, account_bi)
    return bool(row)


async def _outreach_allowed(runtime, org_bi, account_bi, role):
    return role in MANAGER_ROLES or await _outreach_member(
        runtime, org_bi, account_bi)


async def _outreach_team_list(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT member_name,added_by,added_at FROM crm_outreach_team "
        "WHERE org_bi=? ORDER BY added_at,member_name LIMIT 500", org_bi)
    return [{
        "name": _clean(row.get("member_name"), 64).lower(),
        "addedBy": _clean(row.get("added_by"), 64).lower(),
        "addedAt": _integer(row.get("added_at")),
    } for row in rows or []]


async def _outreach_sent_today(runtime, org_bi, sender_bi):
    day_ago = runtime.now() - 24 * 60 * 60 * 1000
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM crm_outreach_log "
        "WHERE org_bi=? AND sender_bi=? AND ts>=?",
        org_bi, sender_bi, day_ago)
    return _integer((row or {}).get("n"))


def _outreach_sender(settings):
    """The organization's configured sender, else the package default."""
    from_email = _email((settings or {}).get("fromEmail")) or OUTREACH_FROM_EMAIL
    from_name = _clean((settings or {}).get("fromName"), 120) or OUTREACH_FROM_NAME
    return from_email, from_name


def _outreach_templates(from_email):
    return [{
        "key": template["key"],
        "label": template["label"],
        "subject": template["subject"],
        "body": template["body"].replace("{fromEmail}", from_email),
    } for template in OUTREACH_TEMPLATES]


async def _outreach_recent(runtime, org_bi, limit=OUTREACH_LOG_LIST_LIMIT):
    rows = await runtime.d1_all(
        "SELECT ts,sender_name,ok,data FROM crm_outreach_log WHERE org_bi=? "
        "ORDER BY ts DESC,id DESC LIMIT ?", org_bi, int(limit))
    recent = []
    for row in rows or []:
        record = await runtime.open(row.get("data"))
        record = record if isinstance(record, dict) else {}
        recent.append({
            "ts": _integer(row.get("ts")),
            "sender": _clean(row.get("sender_name"), 64).lower(),
            "ok": bool(_integer(row.get("ok"))),
            "to": _email(record.get("to")),
            "subject": _clean(record.get("subject"), OUTREACH_MAX_SUBJECT),
            "template": _clean(record.get("template"), 40),
            "error": _clean(record.get("error"), 200),
        })
    return recent


async def _outreach_state(runtime, org_bi, account_bi, actor, role, settings=None):
    manager = role in MANAGER_ROLES
    allowed = await _outreach_allowed(runtime, org_bi, account_bi, role)
    if settings is None:
        settings = await _settings(runtime, org_bi)
    from_email, from_name = _outreach_sender(settings)
    payload = {
        "ok": True, "name": actor, "canManage": manager, "allowed": allowed,
        "fromEmail": from_email, "fromName": from_name,
        "dailyLimit": OUTREACH_DAILY_LIMIT,
        "configured": bool(runtime.mailtrap_configured()),
    }
    if not allowed:
        # Members outside the roster learn only that they lack access, not
        # the templates, the roster, or the send history.
        return payload
    payload["templates"] = _outreach_templates(from_email)
    payload["sentToday"] = await _outreach_sent_today(runtime, org_bi, account_bi)
    payload["recent"] = await _outreach_recent(runtime, org_bi)
    if manager:
        payload["members"] = await _outreach_team_list(runtime, org_bi)
    return payload


async def _outreach_send(runtime, org_bi, org_name, account_bi, actor, data):
    recipient = _email(data.get("to"))
    subject = _clean(data.get("subject"), OUTREACH_MAX_SUBJECT)
    body = _text(data.get("body"), OUTREACH_MAX_BODY).strip()
    template = _clean(data.get("template"), 40).lower()
    if not OUTREACH_TEMPLATE_KEY_RE.fullmatch(template):
        template = "blank"
    if not recipient:
        return _response(runtime, {"error": "invalid_recipient"}, 400)
    if not subject:
        return _response(runtime, {"error": "subject_required"}, 400)
    if not body:
        return _response(runtime, {"error": "body_required"}, 400)
    if _carries_deck(template, body):
        contact = await _contact_row_by_email(runtime, org_bi, recipient)
        contact_data = await runtime.open(contact.get("data")) if contact else None
        refusal = await _deck_refusal(runtime, org_bi, contact_data) if isinstance(
            contact_data, dict) else None
        if refusal:
            return refusal
    sent_today = await _outreach_sent_today(runtime, org_bi, account_bi)
    if sent_today >= OUTREACH_DAILY_LIMIT:
        return _response(runtime, {
            "error": "daily_limit_reached", "dailyLimit": OUTREACH_DAILY_LIMIT,
        }, 429)
    from_email, from_name = _outreach_sender(await _settings(runtime, org_bi))
    # The server's Mailtrap key sends outreach: an empty request key means
    # "use the host's configured sender", exactly like transactional mail.
    result = await runtime.send_mailtrap("", {
        "from": {"email": from_email, "name": from_name},
        "to": [{"email": recipient}],
        "subject": subject,
        "text": body,
        "category": "CRM outreach",
        "custom_variables": {
            "forkmesh_crm_org_bi": org_bi,
            "forkmesh_crm_outreach_template": template,
            "forkmesh_sender": actor,
        },
    })
    accepted = bool((result or {}).get("accepted"))
    error = _clean((result or {}).get("error"), 200)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO crm_outreach_log(org_bi,ts,sender_bi,sender_name,ok,data) "
        "VALUES(?,?,?,?,?,?)",
        org_bi, now, account_bi, actor, 1 if accepted else 0,
        await runtime.seal({
            "to": recipient, "subject": subject, "template": template,
            "error": error,
        }),
    )
    if accepted:
        # A recipient who is already a contact gets their last-contacted stamp
        # so the outreach shows up in the same relationship history.
        contact = await _contact_row_by_email(runtime, org_bi, recipient)
        contact_data = await runtime.open(contact.get("data")) if contact else None
        if contact and isinstance(contact_data, dict):
            contact_data["lastContactedAt"] = now
            await runtime.d1_run(
                "UPDATE crm_contacts SET updated_by_bi=?,updated_by_name=?,"
                "updated_at=?,data=? WHERE org_bi=? AND contact_id=?",
                account_bi, actor, now, await runtime.seal(contact_data), org_bi,
                str(contact.get("contact_id") or ""),
            )
    await runtime.audit(
        actor, "crm.outreach.send", "crm_outreach", template,
        outcome="success" if accepted else "failed",
        details={"transport": "mailtrap", "accepted": accepted},
    )
    if not accepted:
        return _response(runtime, {
            "error": "send_failed",
            "detail": "The email provider rejected the send"
                      + (" (" + error + ")" if error else "") + ".",
            "sentToday": sent_today + 1, "dailyLimit": OUTREACH_DAILY_LIMIT,
        }, 502)
    return _response(runtime, {
        "ok": True, "to": recipient, "from": from_email,
        "sentToday": sent_today + 1, "dailyLimit": OUTREACH_DAILY_LIMIT,
    })


# --- investor deck links -------------------------------------------------------

def _deck_token(runtime):
    """Twelve lowercase base-36 characters from the runtime's random id."""
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    number = int(re.sub(r"[^0-9a-f]", "", str(runtime.new_id())) or "0", 16)
    out = ""
    while number and len(out) < DECK_TOKEN_CHARS:
        number, rem = divmod(number, 36)
        out += digits[rem]
    return out.ljust(DECK_TOKEN_CHARS, "0")


def _deck_url(token):
    return "%s/pitch-deck/%s" % (DECK_SITE_ORIGIN, token)


def _investor_flow(flows, flow_id):
    flow = next((item for item in flows if item.get("id") == flow_id), {})
    return flow.get("icon") == "investors" or flow_id in INVESTOR_FLOW_IDS


def _carries_deck(template, *texts):
    return template == "investor" or any(DECK_LINK_MARK in str(text or "") for text in texts)


async def _deck_refusal(runtime, org_bi, contact):
    contact = contact if isinstance(contact, dict) else {}
    flow_id = _slug(contact.get("flow"), "customers")
    flows = await _org_flows(runtime, org_bi)
    if _investor_flow(flows, flow_id):
        return None
    name = next((item.get("name") for item in flows if item.get("id") == flow_id), flow_id)
    return _response(runtime, {
        "error": "deck_needs_investor_pipeline", "flow": flow_id,
        "detail": "%s is in %s, not an investor pipeline. Move them to an investor "
                  "pipeline first if they should see the deck." % (
                      _clean(contact.get("name"), 120) or "This contact", name),
    }, 409)


def _deck_link_project(row):
    token = str(row.get("token") or "")
    revoked_at = int(row.get("revoked_at") or 0)
    return {
        "token": token,
        "url": _deck_url(token),
        "contactId": str(row.get("contact_id") or ""),
        "deck": str(row.get("deck") or DECK_ID),
        "createdAt": int(row.get("created_at") or 0),
        "createdBy": str(row.get("created_by_name") or ""),
        "revokedAt": revoked_at,
        "active": revoked_at == 0,
        "views": int(row.get("views") or 0),
        "seconds": int(row.get("seconds") or 0),
        "lastViewedAt": int(row.get("last_viewed_at") or 0),
        "pagesViewed": int(row.get("pages_viewed") or 0),
        "pageCount": len(DECK_PAGE_TITLES),
    }


async def _deck_links(runtime, org_bi):
    """Every link the organization minted, with its view totals."""
    rows = await runtime.d1_all(
        "SELECT l.token,l.contact_id,l.deck,l.created_at,l.created_by_name,"
        "l.revoked_at,"
        "(SELECT COUNT(*) FROM crm_deck_sessions s WHERE s.token=l.token) AS views,"
        "(SELECT COALESCE(SUM(s.seconds),0) FROM crm_deck_sessions s "
        "WHERE s.token=l.token) AS seconds,"
        "(SELECT COALESCE(MAX(s.last_at),0) FROM crm_deck_sessions s "
        "WHERE s.token=l.token) AS last_viewed_at,"
        "(SELECT COUNT(DISTINCT p.page) FROM crm_deck_page_views p "
        "WHERE p.token=l.token) AS pages_viewed "
        "FROM crm_deck_links l WHERE l.org_bi=? "
        "ORDER BY l.created_at DESC,l.token",
        org_bi,
    )
    return [_deck_link_project(row) for row in rows]


async def _deck_link_row(runtime, org_bi, token):
    if not DECK_TOKEN_RE.fullmatch(str(token or "")):
        return None
    return await runtime.d1_first(
        "SELECT token,org_bi,org_name,contact_id,deck,created_by_bi,"
        "created_by_name,created_at,revoked_at,data FROM crm_deck_links "
        "WHERE org_bi=? AND token=?",
        org_bi, token,
    )


async def _create_deck_link(runtime, org_bi, org_name, account_bi, actor, data):
    """Mint the contact's link, or hand back the active one it already has."""
    contact_id = _clean(data.get("contactId"), 32).lower()
    if not CONTACT_ID_RE.fullmatch(contact_id):
        return _response(runtime, {"error": "contact_required"}, 400)
    row = await _contact_row(runtime, org_bi, contact_id)
    if not row:
        return _response(runtime, {"error": "contact_not_found"}, 404)
    existing = await runtime.d1_first(
        "SELECT token FROM crm_deck_links WHERE org_bi=? AND contact_id=? "
        "AND revoked_at=0 ORDER BY created_at DESC LIMIT 1",
        org_bi, contact_id,
    )
    if existing:
        links = await _deck_links(runtime, org_bi)
        link = next((item for item in links
                     if item["token"] == str(existing.get("token") or "")), None)
        return _response(runtime, {"ok": True, "link": link, "created": False})
    contact = await runtime.open(row.get("data")) or {}
    refusal = await _deck_refusal(runtime, org_bi, contact)
    if refusal:
        await runtime.audit(actor, "crm.deck.link", "crm_contact", contact_id,
                            outcome="denied", details={"reason": "not_investor_pipeline"})
        return refusal
    token = _deck_token(runtime)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO crm_deck_links(token,org_bi,org_name,contact_id,deck,"
        "created_by_bi,created_by_name,created_at,revoked_at,data) "
        "VALUES(?,?,?,?,?,?,?,?,0,?)",
        token, org_bi, org_name, contact_id, DECK_ID, account_bi, actor, now,
        await runtime.seal({
            "viewer": _clean(contact.get("name"), 120),
            "email": _email(contact.get("email")),
        }),
    )
    await runtime.audit(actor, "crm.deck.link", "crm_contact", contact_id)
    return _response(runtime, {
        "ok": True, "created": True,
        "link": _deck_link_project({
            "token": token, "contact_id": contact_id, "deck": DECK_ID,
            "created_at": now, "created_by_name": actor, "revoked_at": 0,
        }),
    }, 201)


async def _deck_detail(runtime, link):
    """One link with its per-page totals and its most recent sessions."""
    token = str(link.get("token") or "")
    totals = {int(row.get("page") or 0): row for row in await runtime.d1_all(
        "SELECT page,COALESCE(SUM(seconds),0) AS seconds,"
        "COUNT(DISTINCT session_id) AS viewers FROM crm_deck_page_views "
        "WHERE token=? GROUP BY page", token)}
    pages = []
    for index, title in enumerate(DECK_PAGE_TITLES, start=1):
        row = totals.get(index) or {}
        pages.append({
            "page": index, "title": title,
            "seconds": int(row.get("seconds") or 0),
            "viewers": int(row.get("viewers") or 0),
        })
    sessions = []
    for row in await runtime.d1_all(
            "SELECT session_id,started_at,last_at,seconds,pages,country,agent "
            "FROM crm_deck_sessions WHERE token=? "
            "ORDER BY started_at DESC LIMIT ?", token, DECK_SESSION_LIST_LIMIT):
        sessions.append({
            "id": str(row.get("session_id") or ""),
            "startedAt": int(row.get("started_at") or 0),
            "lastAt": int(row.get("last_at") or 0),
            "seconds": int(row.get("seconds") or 0),
            "pages": int(row.get("pages") or 0),
            "country": str(row.get("country") or ""),
            "agent": _agent_label(str(row.get("agent") or "")),
        })
    summary = await runtime.d1_first(
        "SELECT COUNT(*) AS views,COALESCE(SUM(seconds),0) AS seconds,"
        "COALESCE(MAX(last_at),0) AS last_viewed_at FROM crm_deck_sessions "
        "WHERE token=?", token) or {}
    projected = _deck_link_project(dict(link, **{
        "views": summary.get("views"), "seconds": summary.get("seconds"),
        "last_viewed_at": summary.get("last_viewed_at"),
        "pages_viewed": sum(1 for item in pages if item["viewers"]),
    }))
    projected["pages"] = pages
    projected["sessions"] = sessions
    return projected


def _agent_label(agent):
    """A short device/browser label; the raw user agent never leaves."""
    text = agent.lower()
    device = "Phone" if ("iphone" in text or "android" in text and "mobile" in text) \
        else "Tablet" if ("ipad" in text or "tablet" in text) else "Desktop"
    browser = ("Edge" if "edg/" in text else "Chrome" if "chrome/" in text
               else "Safari" if "safari/" in text else "Firefox"
               if "firefox/" in text else "Browser")
    return "%s · %s" % (device, browser) if agent else ""


async def _revoke_deck_link(runtime, link, org_bi, actor):
    token = str(link.get("token") or "")
    if int(link.get("revoked_at") or 0) == 0:
        await runtime.d1_run(
            "UPDATE crm_deck_links SET revoked_at=? WHERE org_bi=? AND token=?",
            runtime.now(), org_bi, token)
        await runtime.audit(actor, "crm.deck.revoke", "crm_contact",
                            str(link.get("contact_id") or ""))
    return _response(runtime, {"ok": True, "revoked": True, "token": token})


async def _outreach_team_update(runtime, org_bi, account_bi, actor, role, data):
    action = _clean(data.get("action"), 16).lower()
    target = _clean(data.get("name"), 64).lower()
    audit_action = (
        "crm.outreach_team_grant" if action == "add"
        else "crm.outreach_team_revoke" if action == "remove"
        else "crm.outreach_team_change")
    if role not in MANAGER_ROLES:
        await runtime.audit(
            actor, audit_action, "crm_outreach_member", target,
            outcome="denied", details={"reason": "organization_admin_required"})
        return _response(runtime, {"error": "organization_admin_required"}, 403)
    if action not in ("add", "remove"):
        return _response(runtime, {"error": "invalid_action"}, 400)
    member = await runtime.d1_first(
        "SELECT member_bi,name FROM org_members WHERE org_bi=? AND lower(name)=?",
        org_bi, target) if target else None
    member_bi = str((member or {}).get("member_bi") or "")
    if not member_bi:
        await runtime.audit(
            actor, audit_action, "crm_outreach_member", target,
            outcome="denied", details={"reason": "member_not_found"})
        return _response(runtime, {"error": "member_not_found"}, 404)
    try:
        if action == "add":
            await runtime.d1_run(
                "INSERT INTO crm_outreach_team(org_bi,member_bi,member_name,"
                "added_by,added_at) VALUES(?,?,?,?,?) "
                "ON CONFLICT(org_bi,member_bi) DO NOTHING",
                org_bi, member_bi, _clean(member.get("name"), 64).lower(),
                actor, runtime.now())
        else:
            await runtime.d1_run(
                "DELETE FROM crm_outreach_team WHERE org_bi=? AND member_bi=?",
                org_bi, member_bi)
    except Exception:
        await runtime.audit(
            actor, audit_action, "crm_outreach_member", target,
            outcome="failed", details={"reason": "mutation_error"})
        raise
    await runtime.audit(actor, audit_action, "crm_outreach_member", target)
    return _response(runtime, {
        "ok": True, "members": await _outreach_team_list(runtime, org_bi)})


async def _update_email(runtime, row, org_bi, actor, data):
    status = _clean(data.get("status", row.get("status")), 40).lower()
    if status not in SEND_STATUSES:
        return _response(runtime, {"error": "invalid_send_status"}, 400)
    external_id = _clean(data.get("externalId", row.get("external_id")), 200)
    envelope = await runtime.open(row.get("data"))
    envelope = dict(envelope if isinstance(envelope, dict) else {})
    if "providerUrl" in data:
        provider_url = _https(data.get("providerUrl"))
        if data.get("providerUrl") and not provider_url:
            return _response(runtime, {"error": "invalid_provider_url"}, 400)
        envelope["providerUrl"] = provider_url
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE crm_email_sends SET status=?,external_id=?,status_at=?,data=? "
        "WHERE org_bi=? AND send_id=?",
        status, external_id, now, await runtime.seal(envelope), org_bi,
        row["send_id"],
    )
    await runtime.audit(actor, "crm.email.update", "crm_email_send", row["send_id"])
    return _response(runtime, {
        "ok": True,
        "email": await _email_project(
            runtime, await _email_row(runtime, org_bi, row["send_id"])),
    })


async def _save_settings(runtime, org_bi, actor_bi, actor, data):
    current = await runtime.d1_first(
        "SELECT data FROM crm_settings WHERE org_bi=?", org_bi)
    opened = await runtime.open(current.get("data")) if current else {}
    settings = _settings_data(data, opened if isinstance(opened, dict) else {})
    if data.get("providerUrlTemplate") and not settings.get("providerUrlTemplate"):
        return _response(runtime, {"error": "invalid_provider_url"}, 400)
    if data.get("fromEmail") and not settings.get("fromEmail"):
        return _response(runtime, {"error": "invalid_sender"}, 400)
    if "flows" in data and not isinstance(data.get("flows"), list):
        return _response(runtime, {"error": "invalid_flows"}, 400)
    if "zoho" in data:
        zoho = settings.get("zoho") if isinstance(settings.get("zoho"), dict) else {}
        supplied = data.get("zoho") if isinstance(data.get("zoho"), dict) else {}
        if supplied and not _zoho_connected(zoho):
            return _response(runtime, {"error": "incomplete_zoho_connection"}, 400)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO crm_settings(org_bi,updated_by_bi,updated_by_name,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(org_bi) DO UPDATE SET "
        "updated_by_bi=excluded.updated_by_bi,updated_by_name=excluded.updated_by_name,"
        "updated_at=excluded.updated_at,data=excluded.data",
        org_bi, actor_bi, actor, now, await runtime.seal(settings),
    )
    await runtime.audit(actor, "crm.settings.update", "organization", org_bi)
    return _response(runtime, {"ok": True, "settings": await _settings(runtime, org_bi)})




async def org_has_data(runtime, org_bi):
    """True when uninstalling this organization would destroy CRM records."""
    for table in ("crm_contacts", "crm_email_sends", "crm_mail_messages",
                  "crm_settings", "crm_views", "crm_outreach_team",
                  "crm_outreach_log", "crm_deck_links", "crm_list_activity"):
        row = await runtime.d1_first(
            "SELECT 1 AS present FROM " + table + " WHERE org_bi=? LIMIT 1",
            org_bi)
        if row:
            return True
    return False


async def export_org_data(runtime, org_bi, org_name):
    """Full decrypted export handed to the organization on uninstall."""
    contacts = await _contacts(runtime, org_bi)
    rows = await runtime.d1_all(
        "SELECT send_id,org_bi,org_name,contact_id,sender_name,transport,status,"
        "external_id,created_at,sent_at,status_at,data FROM crm_email_sends "
        "WHERE org_bi=? ORDER BY created_at DESC,send_id LIMIT ?",
        org_bi, EXPORT_EMAIL_LIMIT,
    )
    emails = [await _email_project(runtime, row) for row in rows or []]
    mail = await _mail_messages(runtime, org_bi)
    views = await _views(runtime, org_bi, "", "", "owner")
    settings = await _settings(runtime, org_bi)
    settings.pop("mailtrapConfigured", None)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-crm-export-v1",
        "organization": org_name,
        "exportedAt": runtime.now(),
        "contacts": contacts,
        "emails": emails,
        "mail": mail,
        "views": views,
        "settings": settings,
        "outreach": {
            "team": await _outreach_team_list(runtime, org_bi),
            "sent": await _outreach_recent(runtime, org_bi, EXPORT_EMAIL_LIMIT),
        },
        "decks": [await _deck_detail(runtime, row) for row in await runtime.d1_all(
            "SELECT token,org_bi,org_name,contact_id,deck,created_by_bi,"
            "created_by_name,created_at,revoked_at,data FROM crm_deck_links "
            "WHERE org_bi=? ORDER BY created_at DESC,token", org_bi)],
    }


async def purge_org_data(runtime, org_bi):
    """Remove every CRM row for the organization (children before contacts)."""
    await runtime.d1_run(
        "DELETE FROM crm_deck_page_views WHERE token IN "
        "(SELECT token FROM crm_deck_links WHERE org_bi=?)", org_bi)
    await runtime.d1_run(
        "DELETE FROM crm_deck_sessions WHERE token IN "
        "(SELECT token FROM crm_deck_links WHERE org_bi=?)", org_bi)
    await runtime.d1_run("DELETE FROM crm_deck_links WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_outreach_log WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_outreach_team WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_mail_messages WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_email_sends WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_list_activity WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_view_shares WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_views WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_contacts WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM crm_settings WHERE org_bi=?", org_bi)


async def mailtrap_webhook_event(runtime, event, status, now):
    """Link an authenticated Mailtrap lifecycle event to its CRM send."""
    variables = event.get("custom_variables")
    variables = variables if isinstance(variables, dict) else {}
    send_id = _clean(variables.get("forkmesh_crm_send_id"), 64).lower()
    org_bi = _clean(variables.get("forkmesh_crm_org_bi"), 128)
    if not send_id or not org_bi or status not in SEND_STATUSES:
        return
    await runtime.d1_run(
        "UPDATE crm_email_sends SET status=?,status_at=?,"
        "external_id=CASE WHEN external_id='' THEN ? ELSE external_id END "
        "WHERE send_id=? AND org_bi=? AND NOT (status IN ('open','click') "
        "AND ? IN ('accepted','delivery','delivered','soft bounce')) "
        "AND NOT (status='click' AND ?='open')",
        status, now, _clean(event.get("message_id"), 200), send_id, org_bi,
        status, status,
    )


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    _, org_name, resource, item = route
    allowed = {
        ("organizations", ""): ("GET",),
        ("organization", ""): ("GET",),
        ("organization", "contacts"): ("GET", "POST"),
        ("organization", "emails"): ("GET", "POST"),
        ("organization", "mail"): ("GET",),
        ("organization", "settings"): ("GET", "PUT"),
        ("organization", "views"): ("GET", "POST"),
        ("organization", "outreach"): ("GET",),
        ("organization", "decks"): ("GET", "POST"),
        ("shared", "shared"): ("GET",),
        ("zoho-callback", "zoho"): ("GET",),
    }.get((route[0], resource), ())
    if route[0] == "open":
        allowed = ("GET",)
    elif route[0] == "shared-contacts":
        allowed = ("PATCH",) if item else ("POST",)
    elif resource == "contacts" and item in ("import", "lookup", "bulk", "merge",
                                             "delete-column", "evidence"):
        allowed = ("POST",)
    elif resource == "contacts" and item in ("export", "duplicates"):
        allowed = ("GET",)
    elif resource == "emails" and item in ("send", "link"):
        allowed = ("POST",)
    elif resource == "mail" and item == "import":
        allowed = ("POST",)
    elif resource == "zoho":
        allowed = (("GET",) if route[0] == "zoho-callback"
                   else ("POST",) if item in ("connect", "exchange") else ())
    elif resource == "views" and item in ("copy", "order"):
        allowed = ("POST",)
    elif resource == "views" and item == "activity":
        allowed = ("GET", "POST")
    elif resource == "outreach" and item in ("send", "team"):
        allowed = ("POST",)
    elif resource == "decks" and item:
        allowed = ("GET", "DELETE") if DECK_TOKEN_RE.fullmatch(item) else ()
    elif route[0] == "shared":
        allowed = ("GET",) if not item or CONTACT_ID_RE.fullmatch(item) else ()
    elif (resource in ("contacts", "emails", "mail", "views") and item
          and CONTACT_ID_RE.fullmatch(item)):
        allowed = ("GET", "PATCH", "DELETE")
    elif item:
        allowed = ()
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(
                runtime, {"error": error},
                413 if error == "payload_too_large" else 400,
            )
    await runtime.ensure_schema()
    if route[0] == "zoho-callback":
        return await _zoho_callback(runtime)
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if route[0] == "organizations":
        return _response(runtime, {
            "ok": True, "actor": actor,
            "organizations": await _organizations(runtime, account_bi),
        })
    if route[0] == "shared":
        # No organization is in the path to gate on, so the tables are only
        # touched once some organization has installed the plugin; the replay
        # then upgrades installs that predate the share index.
        if not await runtime.d1_first(
                "SELECT plugin FROM org_plugin_installs WHERE plugin=? LIMIT 1",
                PLUGIN_ID):
            return _response(runtime, {"error": "view_not_found"} if item
                             else {"ok": True, "views": []}, 404 if item else 200)
        await _ensure_data_schema(runtime, "")
        if not item:
            return _response(runtime, {
                "ok": True, "views": await _shared_views(runtime, account_bi, actor),
            })
        bundle = await _shared_view_bundle(runtime, account_bi, actor, item)
        if bundle is None:
            return _response(runtime, {"error": "view_not_found"}, 404)
        org_bi, view, contacts = bundle
        await _record_list_view(runtime, org_bi, item, account_bi, actor)
        return _response(runtime, {"ok": True, "view": view, "contacts": contacts})
    if route[0] == "open":
        if not await runtime.d1_first(
                "SELECT plugin FROM org_plugin_installs WHERE plugin=? LIMIT 1",
                PLUGIN_ID):
            return _response(runtime, {"error": "link_not_found"}, 404)
        await _ensure_data_schema(runtime, "")
        return await _open_link(runtime, account_bi, actor, item)
    if route[0] == "shared-contacts":
        if not await runtime.d1_first(
                "SELECT plugin FROM org_plugin_installs WHERE plugin=? LIMIT 1",
                PLUGIN_ID):
            return _response(runtime, {"error": "view_not_found"}, 404)
        await _ensure_data_schema(runtime, "")
        return await _shared_contact_write(
            runtime, account_bi, actor, resource, item, data)
    if not ORG_RE.fullmatch(org_name):
        return _response(runtime, {"error": "organization_not_found"}, 404)
    org_bi, role = await runtime.organization(org_name, account_bi)
    if not org_bi or not role:
        return _response(runtime, {"error": "organization_not_found"}, 404)
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return _response(runtime, {
            "error": "plugin_not_installed",
            "organization": org_name,
            "canInstall": role in MANAGER_ROLES,
        }, 409)
    # Plugin schemas are normally created by installation. Replaying the
    # idempotent statements after the install gate also upgrades organizations
    # that installed an earlier CRM release without leaving tables for an
    # organization that never opted in.
    await _ensure_data_schema(runtime, org_bi)
    if not resource:
        return await _summary(runtime, org_bi, org_name, account_bi, actor, role)
    if resource == "views":
        if not item:
            if method == "GET":
                return _response(runtime, {
                    "ok": True,
                    "views": await _views(runtime, org_bi, account_bi, actor, role),
                })
            return await _create_view(
                runtime, org_bi, org_name, account_bi, actor, role, data)
        if item == "copy":
            return await _copy_shared_view(
                runtime, org_bi, org_name, account_bi, actor, role, data)
        if item == "order":
            return await _reorder_views(
                runtime, org_bi, account_bi, actor, role, data)
        if item == "activity":
            view_id = _clean(runtime.query_params().get("view") if method == "GET"
                             else data.get("view"), 32).lower()
            row = (await _view_row(runtime, org_bi, view_id)
                   if CONTACT_ID_RE.fullmatch(view_id) else None)
            view = await _view_project(runtime, row, account_bi, role) if row else None
            if not row or not _view_readable(
                    view, str(row.get("owner_bi") or ""), account_bi, actor, role):
                return _response(runtime, {"error": "view_not_found"}, 404)
            if method == "GET":
                return _response(runtime, await _list_activity(
                    runtime, org_bi, view_id))
            await _record_list_view(runtime, org_bi, view_id, account_bi, actor)
            return _response(runtime, {"ok": True})
        row = await _view_row(runtime, org_bi, item)
        view = await _view_project(runtime, row, account_bi, role) if row else None
        if not row or not _view_readable(
                view, str(row.get("owner_bi") or ""), account_bi, actor, role):
            return _response(runtime, {"error": "view_not_found"}, 404)
        if method == "GET":
            return _response(runtime, {"ok": True, "view": view})
        if method == "PATCH":
            return await _update_view(
                runtime, row, org_bi, account_bi, actor, role, data)
        if not view["canManage"]:
            return _response(runtime, {"error": "view_owner_required"}, 403)
        await _delete_view(runtime, org_bi, item)
        await runtime.audit(actor, "crm.view.delete", "crm_view", item)
        return _response(runtime, {"ok": True, "deleted": True})
    if resource == "outreach":
        if not item:
            return _response(runtime, await _outreach_state(
                runtime, org_bi, account_bi, actor, role))
        if item == "team":
            return await _outreach_team_update(
                runtime, org_bi, account_bi, actor, role, data)
        if not await _outreach_allowed(runtime, org_bi, account_bi, role):
            return _response(runtime, {"error": "outreach_forbidden"}, 403)
        return await _outreach_send(
            runtime, org_bi, org_name, account_bi, actor, data)
    if resource == "decks":
        if not item:
            if method == "GET":
                return _response(runtime, {
                    "ok": True, "decks": await _deck_links(runtime, org_bi)})
            return await _create_deck_link(
                runtime, org_bi, org_name, account_bi, actor, data)
        link = await _deck_link_row(runtime, org_bi, item)
        if not link:
            return _response(runtime, {"error": "deck_link_not_found"}, 404)
        if method == "GET":
            return _response(runtime, {
                "ok": True, "deck": await _deck_detail(runtime, link)})
        return await _revoke_deck_link(runtime, link, org_bi, actor)
    if resource == "zoho":
        if role not in MANAGER_ROLES:
            return _response(runtime, {"error": "organization_admin_required"}, 403)
        if item == "exchange":
            return await _zoho_exchange(runtime, org_bi, org_name, account_bi, actor, data)
        return await _zoho_connect(runtime, org_name, account_bi, actor)
    if resource == "settings":
        if method == "GET":
            return _response(runtime, {
                "ok": True, "settings": await _settings(runtime, org_bi),
                "canManageSettings": role in MANAGER_ROLES,
            })
        if role not in MANAGER_ROLES:
            if "tableViews" not in data:
                return _response(runtime, {"error": "organization_admin_required"}, 403)
            data = {"tableViews": data.get("tableViews")}
        return await _save_settings(runtime, org_bi, account_bi, actor, data)
    if resource == "contacts":
        if not item:
            if method == "GET":
                return _response(runtime, {"ok": True, "contacts": await _contacts(runtime, org_bi)})
            return await _create_contact(runtime, org_bi, org_name, account_bi, actor, data)
        if item == "import":
            return await _import_contacts(runtime, org_bi, org_name, account_bi, actor, data)
        if item == "export":
            return await _export_contacts(runtime, org_bi, org_name)
        if item == "duplicates":
            return _response(runtime, {
                "ok": True, "groups": await _duplicate_groups(runtime, org_bi)})
        if item == "evidence":
            return await _record_account_evidence(
                runtime, org_bi, org_name, account_bi, actor, role, data)
        if item == "lookup":
            return await _lookup_contact(runtime, org_bi, data)
        if item == "bulk":
            return await _bulk_update_contacts(
                runtime, org_bi, org_name, account_bi, actor, data)
        if item == "merge":
            return await _merge_contacts(runtime, org_bi, account_bi, actor, data)
        if item == "delete-column":
            if str(data.get("field") or "") in CONTACT_COLUMN_FIELDS:
                return await _delete_builtin_column(
                    runtime, org_bi, org_name, account_bi, actor, role, data)
            return await _delete_custom_column(
                runtime, org_bi, org_name, account_bi, actor, data)
        row = await _contact_row(runtime, org_bi, item)
        if not row:
            return _response(runtime, {"error": "contact_not_found"}, 404)
        if method == "GET":
            return _response(runtime, {"ok": True, "contact": await _contact_project(runtime, row)})
        if method == "PATCH":
            return await _update_contact(runtime, row, org_bi, account_bi, actor, data)
        removed = await runtime.open(row.get("data"))
        await runtime.d1_run(
            "DELETE FROM crm_contacts WHERE org_bi=? AND contact_id=?", org_bi, item)
        await runtime.audit(actor, "crm.contact.delete", "crm_contact", item)
        if isinstance(removed, dict):
            await _record_contact_changes(runtime, org_bi, account_bi, actor,
                                          [(removed, None)])
        return _response(runtime, {"ok": True, "deleted": True})
    if resource == "mail":
        if item == "import":
            # The owner/admin controls the shared encrypted OAuth connection;
            # once connected, every organization member may explicitly refresh
            # the same shared mail boxes. Restricting refresh itself made CRM
            # state diverge by role and prevented teammates from collaborating.
            return await _import_zoho(
                runtime, org_bi, org_name, account_bi, actor, data)
        if not item:
            return _response(runtime, {
                "ok": True, "mail": await _mail_messages(runtime, org_bi)})
        row = await _mail_message_row(runtime, org_bi, item)
        if not row:
            return _response(runtime, {"error": "mail_message_not_found"}, 404)
        if method == "GET":
            return _response(runtime, {
                "ok": True, "message": await _mail_message_project(runtime, row)})
        if method == "DELETE":
            await runtime.d1_run(
                "DELETE FROM crm_mail_messages WHERE org_bi=? AND message_id=?",
                org_bi, item)
            await runtime.audit(actor, "crm.mail.delete", "crm_mail_message", item)
            return _response(runtime, {"ok": True, "deleted": True})
        return await _move_mail(runtime, row, org_bi, actor, data)
    if not item:
        if method == "GET":
            return _response(runtime, {"ok": True, "emails": await _emails(runtime, org_bi)})
        return await _record_external_send(runtime, org_bi, org_name, account_bi, actor, data)
    if item == "send":
        return await _send_mailtrap(runtime, org_bi, org_name, account_bi, actor, data)
    if item == "link":
        return await _link_email(runtime, org_bi, org_name, account_bi, actor, data)
    row = await _email_row(runtime, org_bi, item)
    if not row:
        return _response(runtime, {"error": "email_send_not_found"}, 404)
    if method == "GET":
        return _response(runtime, {"ok": True, "email": await _email_project(runtime, row)})
    if method == "DELETE":
        await runtime.d1_run(
            "DELETE FROM crm_email_sends WHERE org_bi=? AND send_id=?", org_bi, item)
        await runtime.audit(actor, "crm.email.delete", "crm_email_send", item)
        return _response(runtime, {"ok": True, "deleted": True})
    return await _update_email(runtime, row, org_bi, actor, data)
