"""Watch a Discord bug channel and turn real reports into ForkMesh issues.

Loaded on demand (entry.py holds a ``_LazyModule`` proxy) so Cloudflare's
startup-memory validation does not pay for this domain on every isolate.

The pipeline for one new channel message is ordered so the cheapest gate runs
first, and so the channel only ever hears from the bot about messages that
were actually reports:

1. **Is it a bug report at all?**  One Workers AI call classifies the message
   and, in the same answer, drafts the issue title and body.  Greetings,
   how-do-I questions, replies to somebody else's report and ordinary chatter
   classify as *not a report*, get no issue, and — deliberately — get no
   reply.  A bug channel whose bot answers every message is a bug channel
   nobody posts in.
2. **Has it already been reported?**  The duplicate reviewer from
   ``microjobs`` (the same prompt, schema and verdict parser that gates paid
   issue reports) reads the candidate next to the repository's existing issues
   and a code search for the report's distinctive words.
3. **Who gets the credit?**  The reporter's Discord snowflake resolves to a
   ForkMesh username through the ``users.discord_bi`` link, and that username
   rides the opening event and inbox item's ``meta.reporter``. An unlinked
   reporter is credited by their Discord handle in the body and nothing is
   guessed.
4. **File it, attach the evidence, then say what happened** in the channel.

Working while the source of truth is offline
--------------------------------------------
A repository's issues live on the owner's desktop node, which is frequently
asleep.  Losing a bug report to that is not acceptable, so nothing on the
filing path needs the node:

* Filing is an ``issue_inbox`` insert — the same queue the web composer writes
  to.  The owner's node drains it whenever it next syncs.
* The duplicate reviewer runs against whatever comparison corpus is reachable.
  Two of its three sources are plain D1 reads that always work: this Worker's
  own undrained ``issue_inbox`` backlog, and the reports this watcher has
  already filed (``discord_bug_reports``).  The third — the repository's full
  issue ledger plus a grep of the source — comes from a mirror node through
  the ordinary HTTPS gateway, and is used only when one answers.
* An inconclusive review still files.  Unlike the paid microjob reviewer,
  which fails closed because money is at stake, the cost of a possible
  duplicate here is far lower than the cost of dropping somebody's bug, and
  the channel reply says which corpus the answer was based on.

So a mirror node is a *quality* input to triage, never a dependency.
"""

import base64
import hashlib
import re
from urllib.parse import quote, urlparse


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# One Cron phase is one bounded Durable Object invocation sharing the Workers
# Free 50-subrequest ceiling (CRON_PHASE_D1_QUERY_LIMIT = 45 charged calls).
# Carrying one report to completion costs roughly twenty-five of those — two AI
# calls, the gateway reads, the attachment downloads, the inbox write with its
# notifications, and the reply — so exactly one report is finished per run.
# Scanning is cheap by comparison, and the cursor only advances over messages
# that were actually examined, so a burst drains at one report per minute
# instead of blowing the budget and failing the whole phase.
MAX_SCAN_PER_RUN = 8
MAX_FILED_PER_RUN = 1
# The duplicate reviewer's own search terms, capped tighter than the microjob
# reviewer's: each term costs one issue-search fetch plus one code-search fetch
# against the phase's subrequest ceiling.
MAX_REVIEW_TERMS = 2
# Discord returns at most 100 per page; the bot gate's path allowlist caps the
# limit at two digits.
MESSAGE_PAGE = 25

MAX_MESSAGE_CONTENT = 2_000
MAX_TITLE = 240
MAX_BODY = 8_000
MAX_REASON = 300
MAX_REPLY = 1_800
MAX_HANDLE = 100
# A Discord bug report that resolves to a real ForkMesh account receives an
# estimate immediately. It remains an estimate: payment is still an explicit
# owner-signed action, but putting it on the issue and the public ledger at
# filing time gives the reporter one stable record to follow.
MINIMUM_REPORT_REWARD_LAMPORTS = 1_000_000
MAXIMUM_REPORT_REWARD_LAMPORTS = 5_000_000
# The amount follows what owners already paid for similar reports: the closest
# published issue-report rewards by shared terms, weighted by closeness. With
# nothing similar the typical published amount applies, and with no history at
# all the most common one. Published amounts move in whole 0.001 SOL steps.
REPORT_REWARD_STEP_LAMPORTS = 1_000_000
DEFAULT_REPORT_REWARD_LAMPORTS = 2_000_000
REWARD_HISTORY_LIMIT = 200
REWARD_NEIGHBORS = 5
REWARD_MIN_SIMILARITY = 0.12
REWARD_SUMMARY_MAX = 360
REWARD_STOP_WORDS = frozenset((
    "the", "and", "for", "are", "but", "not", "you", "all", "any", "can",
    "had", "has", "have", "was", "were", "this", "that", "with", "from",
    "when", "then", "than", "into", "onto", "they", "them", "their", "there",
    "what", "which", "while", "will", "would", "should", "could", "does",
    "did", "doe", "its", "it's", "out", "our", "your", "just", "also", "only",
    "some", "such", "very", "more", "most", "other", "after", "before",
    "again", "still", "even", "being", "been", "because", "about", "these",
    "those", "here", "where", "how", "why", "who", "one", "two", "get",
    "gets", "got", "use", "using", "used", "user", "users", "able", "unable",
    "instead", "without", "within", "shows", "show", "shown", "page", "issue",
    "bug", "error", "work", "working", "works", "forkmesh", "reported",
    "discord", "report", "expected", "actual", "steps", "reproduce",
))
# The comparison corpus that needs no live host.
MAX_LOCAL_PENDING = 20
MAX_LOCAL_FILED = 20
LOCAL_FILED_WINDOW_MS = 30 * 24 * 60 * 60 * 1000

# Attachments are re-hosted into the issue, so they are bounded exactly like a
# desktop submission (MAX_ISSUE_ATTACHMENTS / MAX_ISSUE_ATTACHMENT_BYTES) plus
# a total, because every copied byte becomes base64 inside one encrypted D1
# inbox row and D1 rejects a row over 2,000,000 bytes. Anything larger, or of
# an unsupported kind, is linked instead of copied: a link that needs Discord
# beats losing the evidence.
MAX_ATTACHMENTS = 4
MAX_ATTACHMENT_TOTAL_BYTES = 1_000_000
ATTACHMENT_FETCH_TIMEOUT_SECONDS = 8
DISCORD_CDN_HOSTS = frozenset({
    "cdn.discordapp.com",
    "media.discordapp.net",
})
# Images and short clips both belong on a bug report; ISSUE_ATTACHMENT_NAME_RE
# in entry.py is the platform-wide allowlist these must stay inside.
ATTACHMENT_EXTENSIONS = frozenset({
    "png", "jpg", "jpeg", "gif", "webp", "bmp", "svg",
    "mp4", "webm", "mov",
})

SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]{1,80}$")
SNOWFLAKE_RE = re.compile(r"^[0-9]{17,20}$")

TRIAGE_SYSTEM_PROMPT = (
    "You read messages posted in a software project's Discord bug channel and "
    "decide which ones are actual bug reports. A bug report describes "
    "something that is broken, misbehaving, or missing: an error, a crash, a "
    "wrong result, a feature that does not work. It is NOT a bug report if it "
    "is a greeting, a question about how to use the software, a comment on "
    "someone else's report, a status update, praise, or a link with no "
    "complaint attached. When a message is a report, also write the issue it "
    "should become. Judge only the message you are given. Answer with JSON "
    "only."
)

TRIAGE_SCHEMA = {
    "name": "discord_bug_triage",
    "schema": {
        "type": "object",
        "properties": {
            "isBugReport": {"type": "boolean"},
            "title": {"type": "string"},
            "body": {"type": "string"},
        },
        "required": ["isBugReport"],
    },
}


def _text(value, maximum, fallback=""):
    text = clean_string(value, maximum).strip()
    return text or fallback


def _one_line(value, maximum, fallback=""):
    return _text(" ".join(str(value or "").split()), maximum, fallback)


def _int(value):
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _snowflake(value):
    value = str(value or "").strip()
    return value if SNOWFLAKE_RE.fullmatch(value) else ""


def watch_config(env):
    """The channel this deployment watches and the repository it files into.

    Both come from Worker configuration, never from a request: this is the
    operator's own bot in the operator's own guild. Returns None unless both
    are set and well formed, so an unconfigured deployment runs no watcher at
    all rather than guessing a channel to post into.
    """
    channel_id = _snowflake(getattr(env, "DISCORD_BUGS_CHANNEL_ID", "") or "")
    repo = _one_line(getattr(env, "DISCORD_BUGS_REPO", "") or "", 161).lower()
    if not channel_id or repo.count("/") != 1:
        return None
    owner, name = repo.split("/", 1)
    if not SEGMENT_RE.fullmatch(owner) or not SEGMENT_RE.fullmatch(name):
        return None
    return {
        "channelId": channel_id,
        "guildId": _snowflake(getattr(env, "DISCORD_BUGS_GUILD_ID", "") or ""),
        "owner": owner,
        "repo": name,
    }


def triage_prompt(content, attachment_names):
    """The exact user prompt the "is this a report?" classifier sees."""
    lines = ["DISCORD MESSAGE",
             _text(content, MAX_MESSAGE_CONTENT, "(no text)")]
    if attachment_names:
        lines.append("")
        lines.append("ATTACHED FILES: " + ", ".join(
            _one_line(name, 120)
            for name in list(attachment_names)[:MAX_ATTACHMENTS]))
    lines.append("")
    lines.append(
        'Answer with JSON: {"isBugReport":true|false,"title":"<short issue '
        'title>","body":"<what is broken, in the reporter\'s own terms>"}. '
        "Leave title and body empty when isBugReport is false."
    )
    return "\n".join(lines)


# Workers AI sometimes answers with typographic quotes around the JSON keys, so
# the string it returns as the title runs on into the rest of the object:
# 'PRs not visible on website”, “body”:'. Cut the title where that leak starts
# and drop a key prefix it carries. Quotes a reporter used inside a title, such
# as '“Create & mirror” does nothing', stay as they are.
TITLE_JSON_LEAK_RE = re.compile(
    r'\s*[”"]\s*,\s*(?:[“"][A-Za-z]*(?:[”"]?\s*:.*)?)?$', re.DOTALL)
TITLE_KEY_PREFIX_RE = re.compile(
    r'^\s*[“"]?\s*title\s*[”"]?\s*:\s*', re.IGNORECASE)


def clean_triage_title(title):
    """A drafted issue title without leaked JSON punctuation."""
    text = str(title or "").strip()
    text = TITLE_KEY_PREFIX_RE.sub("", text)
    text = TITLE_JSON_LEAK_RE.sub("", text).strip()
    # A quote left unpaired at either end belonged to the leak.
    if text.startswith(("“", '"')) and text.count(text[0]) == 1 and \
            "”" not in text:
        text = text[1:].lstrip()
    if text.endswith(("”", '"')) and "“" not in text and text.count('"') < 2:
        text = text[:-1].rstrip()
    # Cutting at the leak can take the closing half of a quote the reporter
    # opened; close it rather than leave it dangling.
    if text.count("“") > text.count("”"):
        text += "”"
    return text


def parse_triage(raw, fallback_content=""):
    """Normalize the classifier answer; anything unusable is "not a report".

    Failing closed matters twice over here: an unparseable answer must never
    open an issue, and it must never make the bot reply to ordinary
    conversation.
    """
    data = raw
    if isinstance(raw, str):
        data = _forkbot_json_object_from_text(raw)
    if not isinstance(data, dict):
        return {"isBugReport": False, "title": "", "body": ""}
    flag = data.get("isBugReport")
    if isinstance(flag, str):
        flag = flag.strip().lower() in ("true", "yes", "1")
    if not flag:
        return {"isBugReport": False, "title": "", "body": ""}
    body = _text(data.get("body"), MAX_BODY) or _text(
        fallback_content, MAX_BODY)
    title = _one_line(clean_triage_title(data.get("title")), MAX_TITLE)
    if not title and body:
        title = _one_line(_forkbot_issue_title(body), MAX_TITLE)
    if not title or not body:
        return {"isBugReport": False, "title": "", "body": ""}
    return {"isBugReport": True, "title": title, "body": body}


def reporter_credit(handle, account):
    """The credit line that goes in the issue body.

    ``account`` is the ForkMesh username the reporter's Discord id is linked
    to, or "" when there is no link. Nothing is inferred from the handle: an
    unlinked report credits the Discord name and says the link is missing, so
    the report is never quietly attributed to the wrong person.
    """
    handle = _one_line(handle, MAX_HANDLE, "a Discord user")
    if account:
        return "Reported by @%s on Discord (ForkMesh user %s)." % (
            handle, account)
    return (
        "Reported by @%s on Discord. No ForkMesh account is linked to that "
        "Discord user, so this is credited to the Discord name only." % handle)


def reward_terms(text):
    """The distinctive words two reports are compared on."""
    terms = set()
    for word in re.findall(r"[a-z0-9]+", str(text or "").lower()):
        if len(word) < 3 or word.isdigit() or word in REWARD_STOP_WORDS:
            continue
        if len(word) > 4 and word.endswith("s") and not word.endswith("ss"):
            word = word[:-1]
        terms.add(word)
    return terms


def _reward_step(lamports):
    steps = (max(0, int(lamports)) + REPORT_REWARD_STEP_LAMPORTS // 2) \
        // REPORT_REWARD_STEP_LAMPORTS
    return max(MINIMUM_REPORT_REWARD_LAMPORTS, min(
        MAXIMUM_REPORT_REWARD_LAMPORTS, steps * REPORT_REWARD_STEP_LAMPORTS))


def similar_reward_estimate(title, body, history):
    """Estimate a report's reward from the rewards similar reports received.

    ``history`` holds published issue-report rewards as dicts with ``number``,
    ``title``, ``summary`` and ``amountLamports``. Each is scored by the share
    of distinctive terms it has in common with this report (a cosine over the
    two term sets, so a long report cannot win on length alone). The closest
    few above a floor vote with their amounts, weighted by score, and the
    result is rounded to the whole-step amounts owners publish.
    """
    report = " ".join(str(body or "").split())[:REWARD_SUMMARY_MAX]
    wanted = reward_terms(str(title or "") + " " + report)
    amounts = []
    scored = []
    for past in history or []:
        if not isinstance(past, dict):
            continue
        amount = _int(past.get("amountLamports"))
        if not (MINIMUM_REPORT_REWARD_LAMPORTS <= amount
                <= MAXIMUM_REPORT_REWARD_LAMPORTS):
            continue
        amounts.append(amount)
        terms = reward_terms(
            str(past.get("title") or "") + " " + str(past.get("summary") or ""))
        shared = len(wanted & terms)
        if not shared:
            continue
        score = shared / ((len(wanted) * len(terms)) ** 0.5)
        if score >= REWARD_MIN_SIMILARITY:
            scored.append((score, _int(past.get("number")), amount))
    scored.sort(key=lambda entry: (-entry[0], -entry[1]))
    neighbors = scored[:REWARD_NEIGHBORS]
    if neighbors:
        weight = sum(score for score, _, _ in neighbors)
        return {
            "amountLamports": _reward_step(
                sum(score * amount for score, _, amount in neighbors)
                / weight),
            "basis": "similar",
            "similar": [number for _, number, _ in neighbors if number > 0],
        }
    if amounts:
        ordered = sorted(amounts)
        middle = len(ordered) // 2
        median = (ordered[middle] if len(ordered) % 2
                  else (ordered[middle - 1] + ordered[middle]) // 2)
        return {"amountLamports": _reward_step(median), "basis": "typical",
                "similar": []}
    return {"amountLamports": DEFAULT_REPORT_REWARD_LAMPORTS,
            "basis": "default", "similar": []}


def automatic_reward_estimate(message_id, account, title="", body="",
                              history=None):
    """Return the bounded, stable estimate for a linked Discord report.

    The Discord snowflake is already unique per report, so the reward id is
    derived from it and a retry can never mint a second allocation. The amount
    comes from :func:`similar_reward_estimate`. An unlinked Discord handle is
    deliberately not eligible: it is not a ForkMesh payout identity and must
    never receive an estimate by guesswork.
    """
    account = _one_line(account, MAX_HANDLE).lower()
    message_id = _snowflake(message_id)
    if not account or not message_id:
        return None
    digest = hashlib.sha256(
        ("discord-report-reward:" + message_id).encode("utf-8")).hexdigest()
    estimate = similar_reward_estimate(title, body, history)
    return {
        "id": "reward-discord-" + digest[:24],
        "recipient": account,
        "amountLamports": estimate["amountLamports"],
        "condition": "accepted_report",
        "status": "pending_wallet",
        "basis": estimate["basis"],
        "similar": estimate["similar"],
    }


async def _reward_history(env):
    """Published issue-report rewards, newest first, for the estimator."""
    try:
        rows = await d1_all(
            env,
            "SELECT subject_number,subject_title,subject_summary,"
            "amount_lamports FROM contributor_reward_estimates "
            "WHERE source_kind='issue' AND status<>'cancelled' "
            "ORDER BY created_at DESC LIMIT ?",
            REWARD_HISTORY_LIMIT)
    except Exception:
        return []
    return [{
        "number": _int(row.get("subject_number")),
        "title": _one_line(row.get("subject_title"), MAX_TITLE),
        "summary": _text(row.get("subject_summary"), REWARD_SUMMARY_MAX),
        "amountLamports": _int(row.get("amount_lamports")),
    } for row in rows or [] if isinstance(row, dict)]


async def _record_reward_estimate(env, owner, repo, number, title, body,
                                  reward):
    """Put a filed report's estimate on the ledger the owner pays from.

    The row is the same shape ``app/tools/sync_contributor_rewards.py``
    publishes from the issue record, keyed by the same reward id, so a later
    sync refreshes it instead of duplicating it. It never replaces an existing
    row: a retry, or a reward already published for this issue, wins.
    """
    if not isinstance(reward, dict) or number <= 0:
        return False
    now = int(Date.now())
    try:
        recipient_bi = await blind_index(env, reward["recipient"])
        await d1_run(
            env,
            "INSERT INTO contributor_reward_estimates (reward_id,"
            "recipient_bi,recipient_name,repo_owner,repo_name,source_kind,"
            "subject_number,subject_title,subject_summary,amount_lamports,"
            "condition,status,created_at,updated_at) "
            "VALUES (?,?,?,?,?,'issue',?,?,?,?,?,?,?,?) "
            "ON CONFLICT DO NOTHING",
            reward["id"], recipient_bi, reward["recipient"], owner, repo,
            int(number), _one_line(title, 500),
            " ".join(str(body or "").split())[:REWARD_SUMMARY_MAX],
            int(reward["amountLamports"]), reward["condition"],
            reward["status"], now, now)
        return True
    except Exception as error:
        await log_error(
            env, 500, "CRON", "discord-bugs",
            "Discord report reward could not be recorded for %s/%s#%d: %s"
            % (owner, repo, int(number), _safe_error_text(error)[:160]))
        return False


def message_url(guild_id, channel_id, message_id):
    """Permalink to the originating message, or "" without a guild."""
    if not guild_id or not channel_id or not message_id:
        return ""
    return "https://discord.com/channels/%s/%s/%s" % (
        guild_id, channel_id, message_id)


def report_body(body, handle, account, permalink, attachment_notes):
    """The issue body: the report itself, then how it got here."""
    footer = [reporter_credit(handle, account)]
    if permalink:
        footer.append("Original message: " + permalink)
    for note in attachment_notes or []:
        footer.append(note)
    footer.append("Filed by ForkBot from the Discord bug channel.")
    text = "%s\n\n---\n%s" % (
        _text(body, MAX_BODY),
        "\n".join("_%s_" % line for line in footer))
    return clean_string(text, MAX_ISSUE_BYTES)


def outcome_reply(handle, verdict, number, title, repo_url, corpus):
    """What the channel is told once a report has been triaged.

    Only ever reached for a message already classified as a bug report, so
    every branch says something the reporter can act on. Mention parsing is
    disabled on the send, so the @handle renders as plain text and pings
    nobody.
    """
    kind = str((verdict or {}).get("verdict") or "unclear")
    reason = _one_line((verdict or {}).get("reason"), MAX_REASON)
    duplicate_of = int((verdict or {}).get("duplicateOf") or 0)
    who = "@" + _one_line(handle, MAX_HANDLE, "there")
    if kind == "duplicate" and duplicate_of > 0:
        lines = ["%s thanks for the report — this looks like a duplicate of "
                 "#%d, so I have not opened a new issue." % (who, duplicate_of)]
        if reason:
            lines.append(reason)
        if repo_url:
            lines.append("%s/issues/%d" % (repo_url, duplicate_of))
        return "\n".join(lines)
    if kind == "already_addressed":
        lines = ["%s thanks for the report — the code already in the "
                 "repository looks like it covers this, so I have not opened "
                 "a new issue." % who]
        if reason:
            lines.append(reason)
        lines.append("Say so here if it is still broken for you and I will "
                     "take another look.")
        return "\n".join(lines)
    if number <= 0:
        return ("%s thanks for the report — I could not file it just now. "
                "Nothing is lost; please post it again shortly." % who)
    lines = ["%s thanks for the report — filed as issue #%d: %s"
             % (who, number, _one_line(title, MAX_TITLE))]
    if repo_url:
        lines.append("%s/issues/%d" % (repo_url, number))
    if corpus == "local":
        lines.append("No repository host was online, so I could only compare "
                     "this against reports still queued here — it may yet "
                     "turn out to be a duplicate.")
    elif reason:
        lines.append(reason)
    return "\n".join(lines)


def new_messages(payload, after_id, limit=MAX_SCAN_PER_RUN):
    """Human messages newer than the cursor, oldest first.

    Bot messages are dropped before anything else so the watcher can never
    triage its own replies, and snowflakes are compared as integers because
    they are time-ordered but not lexicographically ordered across digit
    lengths.
    """
    try:
        floor = int(after_id) if after_id else 0
    except (TypeError, ValueError):
        floor = 0
    items = []
    for item in (payload if isinstance(payload, list) else []):
        if not isinstance(item, dict):
            continue
        message_id = _snowflake(item.get("id"))
        if not message_id or int(message_id) <= floor:
            continue
        author = item.get("author")
        if isinstance(author, dict) and author.get("bot"):
            continue
        items.append(item)
    items.sort(key=lambda item: int(item["id"]))
    return items[:max(0, int(limit))]


def newest_message_id(payload):
    """The highest snowflake in a page, or "" for an empty channel."""
    newest = ""
    for item in (payload if isinstance(payload, list) else []):
        candidate = _snowflake((item or {}).get("id"))
        if candidate and (not newest or int(candidate) > int(newest)):
            newest = candidate
    return newest


def attachment_names(payload):
    """Filenames of a message's attachments, for the triage prompt."""
    return [
        _one_line((item or {}).get("filename"), 160)
        for item in (payload if isinstance(payload, list) else [])
        if isinstance(item, dict)
    ]


def attachment_plan(payload):
    """Split a message's attachments into "copy the bytes" and "link it".

    Discord hosts the file and expires nothing gracefully, so the issue needs
    its own copy. Files that are too large for one inbox row, of a kind the
    issue store does not carry, or served from anywhere other than Discord's
    own CDN stay links instead of being dropped.
    """
    copy, link = [], []
    budget = MAX_ATTACHMENT_TOTAL_BYTES
    for item in (payload if isinstance(payload, list) else []):
        if not isinstance(item, dict):
            continue
        url = str(item.get("url") or "")
        filename = _one_line(item.get("filename"), 160)
        if not url.startswith("https://"):
            continue
        try:
            parsed = urlparse(url)
            host = str(parsed.hostname or "").lower()
        except Exception:
            continue
        # Embedded credentials are refused for the same reason
        # microjobs.normalize_public_url refuses them: the hostname check
        # passes (userinfo is a separate URL component), but the fetch would
        # then carry a credential this Worker never issued to a host it never
        # authenticated. A Discord CDN link has no userinfo.
        if (host not in DISCORD_CDN_HOSTS
                or parsed.username or parsed.password
                or parsed.port not in (None, 443)):
            continue
        try:
            size = max(0, int(item.get("size") or 0))
        except (TypeError, ValueError):
            size = 0
        extension = (filename.rsplit(".", 1)[-1].lower()
                     if "." in filename else "")
        entry = {"url": url, "filename": filename, "extension": extension,
                 "size": size}
        if (len(copy) < MAX_ATTACHMENTS
                and extension in ATTACHMENT_EXTENSIONS
                and 0 < size <= min(budget, MAX_ISSUE_ATTACHMENT_BYTES)):
            budget -= size
            copy.append(entry)
        else:
            link.append(entry)
    return copy, link


def attachment_name(raw, extension):
    """The content-addressed name the desktop's IssueStore also derives."""
    extension = str(extension or "").lower()
    if extension not in ATTACHMENT_EXTENSIONS:
        extension = "bin"
    return "%s.%s" % (hashlib.sha256(raw).hexdigest()[:8], extension)


def attachment_note(name, filename):
    return "Attached from Discord: %s (%s)." % (name, filename)


def link_note(filename, url):
    return ("Attachment left on Discord (too large or unsupported here): "
            "%s — %s" % (filename, url))


async def _fetch_attachment(env, entry):
    """One bounded GET of a Discord CDN file as raw bytes, or None.

    Redirects are refused: the URL was accepted because its host is Discord's
    CDN, and a followed redirect would move the fetch to a host that was never
    checked.
    """
    try:
        response = await js_fetch_with_timeout(
            entry["url"], {"method": "GET", "redirect": "manual"},
            ATTACHMENT_FETCH_TIMEOUT_SECONDS)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        try:
            announced = int(response.headers.get("content-length") or 0)
        except (AttributeError, TypeError, ValueError, OverflowError):
            announced = 0
        if announced > MAX_ISSUE_ATTACHMENT_BYTES:
            return None
        raw = bytes(Uint8Array.new(await response.arrayBuffer()).to_py())
    except Exception:
        return None
    if not raw or len(raw) > MAX_ISSUE_ATTACHMENT_BYTES:
        return None
    return raw


async def collect_attachments(env, payload):
    """(inbox attachment entries, body footer notes) for one message."""
    copy, link = attachment_plan(payload)
    attachments, notes = [], []
    for entry in copy:
        raw = await _fetch_attachment(env, entry)
        if raw is None:
            link.append(entry)
            continue
        name = attachment_name(raw, entry["extension"])
        attachments.append(
            {"name": name, "data": base64.b64encode(raw).decode()})
        notes.append(attachment_note(name, entry["filename"]))
    for entry in link:
        notes.append(link_note(entry["filename"], entry["url"]))
    return attachments, notes


async def _repo_key(env, owner, repo):
    """The blind index the repository's inbox rows are really stored under.

    The configured repository is named by its public URL, which for an
    organization-fronted repository (``forkmesh/forkmesh``) is an alias: the
    repositories row and the issue inbox live under the *backing node*.
    Deriving a storage key from a URL owner is the standing way to silently
    lose data for an aliased repo, and reading the backlog under the alias
    would hand the reviewer an empty corpus exactly when it matters most --
    with every host offline and the backlog all it has.

    This resolves through ``_ap_org_alias_owner`` on purpose: it is the same
    resolver ``_forkbot_enqueue_issue`` uses to pick the key it inserts under,
    so the read is consistent with the write by construction, including when
    resolution fails and both fall back to the alias. The request-scoped
    ``_inbox_repo_key`` is not usable here (a Cron phase has no request), and
    its retryable raise would be the wrong failure mode anyway: a narrower
    comparison corpus degrades triage, while aborting the phase drops the
    report.
    """
    host_owner = await _ap_org_alias_owner(env, owner, repo)
    return await blind_index(env, (host_owner or owner) + "/" + repo)


async def _local_issue_corpus(env, owner, repo):
    """Everything this Worker knows about existing reports, with no host.

    Two D1 reads: the repository's undrained ``issue_inbox`` backlog, and the
    reports this watcher has already filed. Both are precisely the issues a
    live host could not show yet, so they are worth comparing against even
    when the mirror gateway does answer.
    """
    records = []
    try:
        repo_bi = await _repo_key(env, owner, repo)
    except Exception:
        return records
    try:
        rows = await d1_all(
            env,
            "SELECT data FROM issue_inbox WHERE repo_bi=? "
            "ORDER BY id DESC LIMIT ?",
            repo_bi, MAX_LOCAL_PENDING)
    except Exception:
        rows = []
    for row in rows or []:
        try:
            item = await decrypt_row(env, row.get("data"))
        except Exception:
            item = None
        if not isinstance(item, dict):
            continue
        event = item.get("event")
        event = event if isinstance(event, dict) else {}
        records.append({
            "number": _int(item.get("number")),
            "status": "queued",
            "title": _one_line(
                item.get("titleIfNew") or event.get("title"), MAX_TITLE),
            "body": _text(event.get("body"), 400),
        })
    try:
        rows = await d1_all(
            env,
            "SELECT data FROM discord_bug_reports WHERE repo_bi=? "
            "AND created_at>=? ORDER BY created_at DESC LIMIT ?",
            repo_bi, int(Date.now()) - LOCAL_FILED_WINDOW_MS, MAX_LOCAL_FILED)
    except Exception:
        rows = []
    seen = {record["number"] for record in records if record["number"] > 0}
    for row in rows or []:
        try:
            item = await decrypt_row(env, row.get("data"))
        except Exception:
            item = None
        if not isinstance(item, dict):
            continue
        number = _int(item.get("issueNumber"))
        if number <= 0 or number in seen:
            continue
        seen.add(number)
        records.append({
            "number": number,
            "status": "reported",
            "title": _one_line(item.get("title"), MAX_TITLE),
            "body": "",
        })
    return records


async def _search_code(env, owner, repo, term):
    data = await _forkbot_repo_gateway_json(
        env, owner, repo,
        "search?path=" + quote(str(term or "")[:120], safe=""))
    hits = (data or {}).get("code")
    return hits if isinstance(hits, list) else []


async def review_report(env, owner, repo, title, body):
    """Duplicate review over whatever comparison corpus is reachable.

    Returns ``(verdict, corpus)``. ``corpus`` is "repository" when a live host
    served the issue ledger and the source grep, and "local" when only this
    Worker's own queued and already-filed reports were available — the channel
    reply repeats that distinction rather than implying a full check happened.
    """
    terms = microjobs.search_terms(title, body)[:MAX_REVIEW_TERMS]
    numbers = await _forkbot_recent_issue_numbers(env, owner, repo)
    corpus = "repository" if numbers is not None else "local"
    issues = list(await _local_issue_corpus(env, owner, repo))
    code_hits = []
    if numbers is not None:
        matching = []
        for term in terms:
            for match in await _forkbot_search_issues(
                    env, owner, repo, term) or []:
                try:
                    number = int((match or {}).get("number") or 0)
                except (AttributeError, TypeError, ValueError):
                    number = 0
                if number > 0 and number not in matching:
                    matching.append(number)
        ordered = matching + [n for n in numbers if n not in matching]
        issues.extend(
            await _forkbot_load_issue_records(env, owner, repo, ordered) or [])
        # The source is the other half of "already reported": a fix already in
        # the tree makes the report stale rather than new. This is the one step
        # that genuinely needs a mirror node, so it is skipped when none
        # answers instead of blocking the review.
        for term in terms:
            for hit in await _search_code(env, owner, repo, term) or []:
                if len(code_hits) >= microjobs.MAX_AI_CODE_HITS:
                    break
                code_hits.append(hit)
            if len(code_hits) >= microjobs.MAX_AI_CODE_HITS:
                break
    if not issues and not code_hits:
        return ({"verdict": "new", "duplicateOf": 0,
                 "reason": "There is nothing to compare this against yet."},
                corpus)
    raw = await _run_ai(
        env, microjobs.AI_SYSTEM_PROMPT,
        microjobs.issue_review_prompt(title, body, issues, code_hits),
        microjobs.AI_VERDICT_SCHEMA)
    if raw is None:
        return {"verdict": "unclear", "duplicateOf": 0,
                "reason": "The Workers AI reviewer was unavailable."}, corpus
    return microjobs.parse_ai_verdict(raw), corpus


async def _run_ai(env, system_prompt, user_prompt, schema):
    """One Workers AI run, charged against the Cron phase's subrequest budget.

    A binding call does not pass through ``_cron_subrequest_charge`` on its
    own, so without this the counter under-reports by one per classification
    and the phase could be cut off by the platform partway through filing a
    report. Charging first makes the phase fail closed at its own limit, where
    the cursor is still where it was.
    """
    _cron_subrequest_charge()
    return await _forkbot_run_ai(
        env, system_prompt, user_prompt, schema=schema, max_tokens=400)


async def _reply(env, channel_id, text):
    text = clean_string(text, MAX_REPLY).strip()
    if not text:
        return False
    result = await _discord_api_request(
        env, "POST", "/channels/" + channel_id + "/messages",
        {"content": text, "allowed_mentions": {"parse": []}})
    return int((result or {}).get("status") or 0) in (200, 201)


async def _message_key(env, message_id):
    return await blind_index(env, "discord-bug-message:" + message_id)


async def _already_handled(env, message_id):
    """True when an earlier run already answered this message.

    The cursor normally prevents a second look. This covers the gap where a
    phase files the issue and then dies before its cursor write, which would
    otherwise re-file and re-reply on the next tick.
    """
    try:
        row = await d1_first(
            env, "SELECT 1 AS hit FROM discord_bug_reports WHERE message_bi=?",
            await _message_key(env, message_id))
    except Exception:
        return False
    return bool(row)


async def _record_report(env, repo_bi, message_id, record):
    try:
        await d1_run(
            env,
            "INSERT INTO discord_bug_reports "
            "(message_bi,repo_bi,data,created_at) VALUES (?,?,?,?) "
            "ON CONFLICT(message_bi) DO NOTHING",
            await _message_key(env, message_id), repo_bi,
            await encrypt_row(env, record), int(Date.now()))
    except Exception:
        pass


async def _load_cursor(env, channel_id):
    try:
        row = await d1_first(
            env, "SELECT last_message_id FROM discord_bug_watch "
            "WHERE channel_bi=?",
            await blind_index(env, "discord-bug-channel:" + channel_id))
    except Exception:
        return ""
    return _snowflake((row or {}).get("last_message_id"))


async def _save_cursor(env, channel_id, message_id):
    await d1_run(
        env,
        "INSERT INTO discord_bug_watch (channel_bi,last_message_id,updated_at) "
        "VALUES (?,?,?) ON CONFLICT(channel_bi) DO UPDATE SET "
        "last_message_id=excluded.last_message_id,"
        "updated_at=excluded.updated_at",
        await blind_index(env, "discord-bug-channel:" + channel_id),
        message_id, int(Date.now()))


async def _handle_report(env, watch, message, triage):
    """Review, file, credit and answer one classified bug report."""
    owner, repo = watch["owner"], watch["repo"]
    message_id = _snowflake(message.get("id"))
    author = message.get("author")
    author = author if isinstance(author, dict) else {}
    handle = _one_line(
        author.get("global_name") or author.get("username"), MAX_HANDLE)
    account = await _account_name_for_discord(env, author.get("id"))
    reward = None
    verdict, corpus = await review_report(
        env, owner, repo, triage["title"], triage["body"])
    kind = str(verdict.get("verdict") or "unclear")
    duplicate_of = int(verdict.get("duplicateOf") or 0)
    number = 0
    if kind == "duplicate" and duplicate_of > 0:
        pass
    elif kind == "already_addressed":
        pass
    else:
        # "new" and "unclear" both file. An inconclusive reviewer is a reason
        # to say so in the channel, not a reason to drop somebody's bug.
        attachments, notes = await collect_attachments(
            env, message.get("attachments"))
        body = report_body(
            triage["body"], handle, account,
            message_url(watch["guildId"], watch["channelId"], message_id),
            notes)
        if account:
            reward = automatic_reward_estimate(
                message_id, account, triage["title"], triage["body"],
                await _reward_history(env))
        ok, item = await _forkbot_enqueue_issue(
            env, owner, repo, triage["title"], body, account,
            source="discord", labels=["bug", "discord"],
            attachments=attachments, reward=reward)
        if ok:
            number = int((item or {}).get("number") or 0)
            if reward and number > 0:
                await _record_reward_estimate(
                    env, owner, repo, number, triage["title"], body, reward)
        else:
            await log_error(
                env, 500, "CRON", "discord-bugs",
                "Discord bug report could not be queued for %s/%s: %s"
                % (owner, repo, clean_string(item, 80)))
    try:
        repo_bi = await _repo_key(env, owner, repo)
    except Exception:
        repo_bi = ""
    await _record_report(env, repo_bi, message_id, {
        "messageId": message_id,
        "channelId": watch["channelId"],
        "repo": owner + "/" + repo,
        "reporter": account,
        "verdict": kind,
        "issueNumber": number,
        "duplicateOf": duplicate_of,
        "rewardId": (reward or {}).get("id", ""),
        "rewardLamports": (reward or {}).get("amountLamports", 0),
        "title": triage["title"],
        "corpus": corpus,
    })
    repo_url = _public_base_url(env).rstrip("/") + repo_web_href(owner, repo)
    await _reply(env, watch["channelId"], outcome_reply(
        handle, verdict, number, triage["title"], repo_url, corpus))
    return number > 0 or kind in ("duplicate", "already_addressed")


async def watch_discord_bug_reports(env):
    """Cron phase: turn new bug-channel messages into reviewed issues."""
    watch = watch_config(env)
    if not watch:
        return {"ok": True, "scanned": 0, "reason": "watch_not_configured"}
    if not _discord_bot_token(env):
        return {"ok": True, "scanned": 0, "reason": "discord_secret_required"}
    await ensure_schema(env)
    cursor = await _load_cursor(env, watch["channelId"])
    path = "/channels/%s/messages?limit=%d" % (
        watch["channelId"], MESSAGE_PAGE)
    if cursor:
        path += "&after=" + cursor
    result = await _discord_api_request(env, "GET", path)
    status = int((result or {}).get("status") or 0)
    if status != 200:
        return {"ok": False, "scanned": 0, "status": status}
    payload = (result or {}).get("data")
    if not cursor:
        # First sight of the channel. Adopt the newest message as the cursor
        # rather than triaging — and replying to — the entire backlog.
        newest = newest_message_id(payload)
        if newest:
            await _save_cursor(env, watch["channelId"], newest)
        return {"ok": True, "scanned": 0, "adopted": bool(newest)}
    scanned = 0
    handled = 0
    examined = cursor
    for message in new_messages(payload, cursor):
        if handled >= MAX_FILED_PER_RUN:
            break
        message_id = _snowflake(message.get("id"))
        content = _text(message.get("content"), MAX_MESSAGE_CONTENT)
        names = attachment_names(message.get("attachments"))
        if not content and not names:
            examined = message_id
            continue
        scanned += 1
        try:
            raw = await _run_ai(
                env, TRIAGE_SYSTEM_PROMPT, triage_prompt(content, names),
                TRIAGE_SCHEMA)
        except RuntimeError:
            # Out of phase subrequest budget. Stop cleanly with the cursor
            # still behind this message rather than starting a report this run
            # cannot finish; the next tick picks up exactly here.
            break
        if raw is None:
            # The classifier is down. Leaving the cursor where it is means
            # these messages are re-read once it is back, so no report is lost
            # to an AI outage — the cost is re-classifying a few messages.
            break
        triage = parse_triage(raw, content)
        if not triage["isBugReport"]:
            examined = message_id
            continue
        if await _already_handled(env, message_id):
            examined = message_id
            continue
        try:
            await _handle_report(env, watch, message, triage)
        except Exception as error:
            await log_error(
                env, 500, "CRON", "discord-bugs",
                "Discord bug report handling failed: "
                + _safe_error_text(error)[:300])
            break
        handled += 1
        examined = message_id
    if examined and examined != cursor:
        await _save_cursor(env, watch["channelId"], examined)
    return {"ok": True, "scanned": scanned, "handled": handled}
