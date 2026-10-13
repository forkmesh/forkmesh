"""The Discord bug-channel watcher: triage gate, credit, duplicates, offline.

The module is loaded into a fresh namespace and handed stub runtime primitives
through its ``_bind_runtime`` hook, exactly as entry.py's ``_LazyModule`` does
in production, so these tests exercise the real source with no Worker runtime.
"""

import asyncio
import base64
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = SRC / "entry.py"
MODULE = SRC / "discord_bugs.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)


def run_async_test(function):
    def wrapped(*args, **kwargs):
        return asyncio.run(function(*args, **kwargs))
    return wrapped


class _Env:
    def __init__(self, **values):
        for name, value in values.items():
            setattr(self, name, value)


def _clean_string(value, maximum=0):
    text = "".join(
        character if character.isprintable() or character in "\n\t" else " "
        for character in str(value if value is not None else "")
    )
    return text[:maximum] if maximum else text


class _Runtime:
    """Stub Worker primitives plus a recorder for everything written out."""

    def __init__(self, **overrides):
        self.now = 1_783_463_371_000
        self.rows = {"issue_inbox": [], "discord_bug_reports": [],
                     "discord_bug_watch": {}, "reward_history": [],
                     "rewards": []}
        self.filed = []
        self.replies = []
        self.errors = []
        self.ai_calls = []
        self.gateway = {}
        self.accounts = {}
        self.discord = []
        self.charged = 0
        self.queried = []
        self.alias = {}
        self.fetched = {}
        self.overrides = overrides

    def namespace(self):
        source = MODULE.read_text(encoding="utf-8")
        namespace = {"__name__": "discord_bugs"}
        exec(compile(source, str(MODULE), "exec"), namespace)
        namespace["_bind_runtime"](self.primitives())
        return namespace

    def primitives(self):
        runtime = self
        MAX_ISSUE_BYTES = 64 * 1024

        class _Date:
            @staticmethod
            def now():
                return runtime.now

        class _Uint8Array:
            @staticmethod
            def new(buffer):
                class _View:
                    def to_py(self):
                        return buffer
                return _View()

        async def blind_index(env, value):
            return "bi:" + hashlib.sha256(
                str(value).encode()).hexdigest()[:16]

        async def encrypt_row(env, item):
            return json.dumps(item)

        async def decrypt_row(env, data):
            try:
                return json.loads(data)
            except Exception:
                return None

        async def d1_all(env, sql, *args):
            runtime.queried.append((sql, args))
            if "FROM issue_inbox" in sql:
                if args and args[0] != runtime.expected_repo_key():
                    return []
                return [{"data": json.dumps(item)}
                        for item in runtime.rows["issue_inbox"]]
            if "FROM discord_bug_reports" in sql:
                return [{"data": json.dumps(item)}
                        for item in runtime.rows["discord_bug_reports"]]
            if "FROM contributor_reward_estimates" in sql:
                return list(runtime.rows["reward_history"])
            return []

        async def d1_first(env, sql, *args):
            if "FROM discord_bug_reports WHERE message_bi=?" in sql:
                return ({"hit": 1} if args[0] in runtime.handled_keys()
                        else None)
            if "FROM discord_bug_watch" in sql:
                cursor = runtime.rows["discord_bug_watch"].get(args[0])
                return {"last_message_id": cursor} if cursor else None
            return None

        async def d1_run(env, sql, *args):
            if "INTO discord_bug_watch" in sql:
                runtime.rows["discord_bug_watch"][args[0]] = args[1]
            elif "INTO discord_bug_reports" in sql:
                runtime.rows["discord_bug_reports"].append(
                    json.loads(args[2]))
            elif "INTO contributor_reward_estimates" in sql:
                if runtime.overrides.get("reward_insert_fails"):
                    raise RuntimeError("D1 unavailable")
                runtime.rows["rewards"].append(args)
            return None

        async def ensure_schema(env):
            return None

        async def log_error(env, status, kind, where, message):
            runtime.errors.append(message)

        async def _ap_org_alias_owner(env, owner, repo):
            return runtime.alias.get(owner, owner)

        async def _account_name_for_discord(env, discord_id):
            return runtime.accounts.get(str(discord_id or ""), "")

        async def _forkbot_run_ai(env, system, user, schema=None,
                                  max_tokens=512):
            runtime.ai_calls.append(
                {"system": system, "user": user, "schema": schema})
            answer = runtime.overrides.get("ai")
            return answer(system, user) if callable(answer) else answer

        async def _forkbot_enqueue_issue(env, owner, repo, title, body,
                                         requester, source="forkbot",
                                         labels=None, attachments=None,
                                         **kwargs):
            if runtime.overrides.get("enqueue_fails"):
                return False, "inbox_full"
            number = 900 + len(runtime.filed)
            runtime.filed.append({
                "owner": owner, "repo": repo, "title": title, "body": body,
                "requester": requester, "source": source,
                "labels": list(labels or []),
                "attachments": list(attachments or []),
                "reward": kwargs.get("reward"),
                "number": number,
            })
            return True, {"number": number}

        async def _forkbot_recent_issue_numbers(env, owner, repo):
            return runtime.gateway.get("numbers")

        async def _forkbot_search_issues(env, owner, repo, term):
            return runtime.gateway.get("search", [])

        async def _forkbot_load_issue_records(env, owner, repo, numbers):
            return runtime.gateway.get("records", [])

        async def _forkbot_repo_gateway_json(env, owner, repo, action):
            return runtime.gateway.get("code_json")

        def _forkbot_json_object_from_text(text):
            start, end = str(text).find("{"), str(text).rfind("}")
            if 0 <= start < end:
                try:
                    return json.loads(str(text)[start:end + 1])
                except Exception:
                    return None
            return None

        def _forkbot_issue_title(description):
            return " ".join(str(description).split())[:60]

        async def _discord_api_request(env, method, path, body=None):
            runtime.discord.append(
                {"method": method, "path": path, "body": body})
            if method == "POST":
                runtime.replies.append(body.get("content", ""))
                return {"status": 200, "data": {}}
            return {"status": runtime.overrides.get("discord_status", 200),
                    "data": runtime.overrides.get("messages", [])}

        def _cron_subrequest_charge(count=1):
            runtime.charged += max(0, int(count or 0))
            if runtime.charged > runtime.overrides.get("budget", 40):
                raise RuntimeError("cron phase subrequest budget exhausted")

        def _discord_bot_token(env):
            return runtime.overrides.get("token", "bot-token")

        async def js_fetch_with_timeout(url, options, seconds):
            payload = runtime.fetched.get(url)
            if payload is None:
                raise RuntimeError("unreachable")

            class _Headers:
                @staticmethod
                def get(name):
                    return str(len(payload)) if name == "content-length" else ""

            class _Response:
                status = 200
                headers = _Headers()

                async def arrayBuffer(self):
                    return payload

            return _Response()

        def _public_base_url(env, request=None):
            return "https://forkmesh.com"

        def repo_web_href(owner, repo):
            return "/%s/%s" % (owner, repo)

        def _safe_error_text(error):
            return str(error)

        class _Microjobs:
            AI_SYSTEM_PROMPT = "review"
            AI_VERDICT_SCHEMA = {"name": "microjob_issue_review"}
            MAX_AI_CODE_HITS = 12

            @staticmethod
            def search_terms(title, body):
                return ["crash"]

            @staticmethod
            def issue_review_prompt(title, body, issues, code_hits):
                return "REVIEW %s :: %d issues :: %d hits" % (
                    title, len(issues), len(code_hits))

            @staticmethod
            def parse_ai_verdict(raw):
                data = raw if isinstance(raw, dict) else {}
                return {
                    "verdict": str(data.get("verdict") or "unclear"),
                    "duplicateOf": int(data.get("duplicateOf") or 0),
                    "reason": str(data.get("reason") or ""),
                }

        return {
            "Date": _Date,
            "Uint8Array": _Uint8Array,
            "MAX_ISSUE_BYTES": MAX_ISSUE_BYTES,
            "MAX_ISSUE_ATTACHMENT_BYTES": 2 * 1024 * 1024,
            "blind_index": blind_index,
            "clean_string": _clean_string,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "encrypt_row": encrypt_row,
            "ensure_schema": ensure_schema,
            "log_error": log_error,
            "microjobs": _Microjobs,
            "js_fetch_with_timeout": js_fetch_with_timeout,
            "repo_web_href": repo_web_href,
            "_account_name_for_discord": _account_name_for_discord,
            "_ap_org_alias_owner": _ap_org_alias_owner,
            "_discord_api_request": _discord_api_request,
            "_cron_subrequest_charge": _cron_subrequest_charge,
            "_discord_bot_token": _discord_bot_token,
            "_forkbot_enqueue_issue": _forkbot_enqueue_issue,
            "_forkbot_issue_title": _forkbot_issue_title,
            "_forkbot_json_object_from_text": _forkbot_json_object_from_text,
            "_forkbot_load_issue_records": _forkbot_load_issue_records,
            "_forkbot_recent_issue_numbers": _forkbot_recent_issue_numbers,
            "_forkbot_repo_gateway_json": _forkbot_repo_gateway_json,
            "_forkbot_search_issues": _forkbot_search_issues,
            "_forkbot_run_ai": _forkbot_run_ai,
            "_public_base_url": _public_base_url,
            "_safe_error_text": _safe_error_text,
        }

    def expected_repo_key(self):
        """The blind index the module must key inbox reads on."""
        owner = self.alias.get("forkmesh", "forkmesh")
        return "bi:" + hashlib.sha256(
            (owner + "/forkmesh").encode()).hexdigest()[:16]

    def handled_keys(self):
        return {
            "bi:" + hashlib.sha256(
                ("discord-bug-message:" + str(
                    record.get("messageId"))).encode()).hexdigest()[:16]
            for record in self.rows["discord_bug_reports"]
        }


def _env(**overrides):
    values = {
        "DISCORD_BUGS_CHANNEL_ID": "1531718105124831304",
        "DISCORD_BUGS_GUILD_ID": "1531610390574665809",
        "DISCORD_BUGS_REPO": "forkmesh/forkmesh",
    }
    values.update(overrides)
    return _Env(**values)


def _message(message_id, content, author_id="400000000000000001",
             username="luistellom", attachments=None, bot=False):
    return {
        "id": message_id,
        "content": content,
        "author": {"id": author_id, "username": username, "bot": bot},
        "attachments": attachments or [],
    }


def _triage_answer(is_report, title="", body=""):
    return json.dumps(
        {"isBugReport": is_report, "title": title, "body": body})


def _seeded(runtime, cursor="500000000000000000"):
    channel = "bi:" + hashlib.sha256(
        ("discord-bug-channel:1531718105124831304").encode()).hexdigest()[:16]
    runtime.rows["discord_bug_watch"][channel] = cursor
    return runtime


# --- The "is this even a report?" gate ---------------------------------------

def test_chatter_is_never_answered_and_never_filed():
    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "morning everyone")],
        ai=lambda system, user: (
            _triage_answer(False) if "DISCORD MESSAGE" in user else {})))
    namespace = runtime.namespace()
    result = run_async_test(namespace["watch_discord_bug_reports"])(_env())
    assert result["handled"] == 0
    assert runtime.filed == []
    # The decisive property: an ordinary message gets no bot reply at all.
    assert runtime.replies == []


def test_an_unusable_classifier_answer_is_not_a_report():
    namespace = _Runtime().namespace()
    for answer in ("not json at all", '{"isBugReport":true}', "", None,
                   '{"isBugReport":false,"title":"x","body":"y"}'):
        assert namespace["parse_triage"](answer, "")["isBugReport"] is False


def test_a_report_missing_a_title_still_gets_one_from_its_body():
    namespace = _Runtime().namespace()
    triage = namespace["parse_triage"](
        '{"isBugReport":true,"body":"Clone fails with a 500"}', "")
    assert triage["isBugReport"] is True
    assert triage["title"] == "Clone fails with a 500"


def test_the_watcher_never_triages_its_own_replies():
    namespace = _Runtime().namespace()
    fresh = namespace["new_messages"]([
        _message("500000000000000002", "filed as issue #12", bot=True),
        _message("500000000000000003", "the clone button crashes"),
    ], "500000000000000000")
    assert [item["id"] for item in fresh] == ["500000000000000003"]


def test_messages_are_processed_oldest_first_by_snowflake_order():
    namespace = _Runtime().namespace()
    fresh = namespace["new_messages"]([
        _message("500000000000000009", "c"),
        _message("500000000000000007", "a"),
        _message("500000000000000008", "b"),
        _message("499999999999999999", "already seen"),
    ], "500000000000000006")
    assert [item["id"] for item in fresh] == [
        "500000000000000007", "500000000000000008", "500000000000000009"]


# --- Filing, credit and the reply --------------------------------------------

def _report_run(runtime, accounts=None, **env_overrides):
    runtime.accounts = accounts or {}
    namespace = runtime.namespace()
    return run_async_test(namespace["watch_discord_bug_reports"])(
        _env(**env_overrides))


def test_a_real_report_is_filed_credited_and_answered():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(
                True, "Clone button crashes", "Pressing clone crashes the app.")
        return {"verdict": "new", "reason": "Nothing like it exists."}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001",
                           "the clone button crashes every time")],
        ai=ai))
    runtime.gateway = {"numbers": [1, 2], "records": [
        {"number": 1, "status": "open", "title": "Login fails", "body": ""}]}
    result = _report_run(runtime, accounts={"400000000000000001": "spidertest"})

    assert result["handled"] == 1
    filed = runtime.filed[0]
    # Credit rides `requester`, which becomes the inbox item's meta.reporter.
    assert filed["requester"] == "spidertest"
    assert filed["source"] == "discord"
    assert filed["labels"] == ["bug", "discord"]
    assert "ForkMesh user spidertest" in filed["body"]
    reward = filed["reward"]
    assert reward["recipient"] == "spidertest"
    assert reward["condition"] == "accepted_report"
    assert reward["status"] == "pending_wallet"
    assert 1_000_000 <= reward["amountLamports"] <= 5_000_000
    assert ("https://discord.com/channels/1531610390574665809/"
            "1531718105124831304/500000000000000001") in filed["body"]
    assert "filed as issue #900" in runtime.replies[0]
    assert "https://forkmesh.com/forkmesh/forkmesh/issues/900" in (
        runtime.replies[0])


def test_an_unlinked_reporter_is_credited_by_discord_name_and_never_guessed():
    namespace = _Runtime().namespace()
    credited = namespace["reporter_credit"]("luistellom", "")
    assert "@luistellom" in credited
    assert "No ForkMesh account is linked" in credited
    # Nothing that looks like a username is invented from the handle.
    assert "ForkMesh user" not in credited


def test_an_unlinked_reporter_is_never_resolved_to_a_forkmesh_account():
    """The Discord handle must not leak into the ForkMesh credit field.

    ``requester`` becomes the inbox item's ``meta.reporter``, which the owner's
    node turns into the issue's author name. A handle that merely looks like a
    username there would attribute the report to whoever holds that ForkMesh
    account -- or invent one that does not exist.
    """
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes",
                           username="luistellom")],
        ai=ai))
    _report_run(runtime, accounts={})  # nobody linked

    assert runtime.filed[0]["requester"] == ""
    body = runtime.filed[0]["body"]
    assert "@luistellom" in body
    assert "No ForkMesh account is linked" in body
    assert "ForkMesh user" not in body
    assert runtime.filed[0]["reward"] is None


def test_triage_titles_lose_leaked_json_punctuation():
    """Titles filed as 'PRs not visible on website”, “body”:' (#661)."""
    clean = _Runtime().namespace()["clean_triage_title"]
    leaked = {
        "PRs not visible on website”, “body”:": "PRs not visible on website",
        "Unable to create multiple PRs”, “body”:":
            "Unable to create multiple PRs",
        "Missing theme change option on website”,":
            "Missing theme change option on website",
        'Clone button crashes", "body": "It crashes."':
            "Clone button crashes",
        "Offline toggle label does not update”, “bo":
            "Offline toggle label does not update",
        "“title”: “Clone crashes”,": "Clone crashes",
        "Sign out “other devices”, “body”:": "Sign out “other devices”",
    }
    for raw, expected in leaked.items():
        assert clean(raw) == expected, raw
    for kept in (
            "“Create & mirror” does nothing",
            "Repositories list shows an enabled “Show more” button",
            "Clicking “Save”, nothing happens",
            'The "Offline" toggle never changes',
            "Plain title"):
        assert clean(kept) == kept


def test_parse_triage_cleans_a_leaked_title():
    parsed = _Runtime().namespace()["parse_triage"]({
        "isBugReport": True,
        "title": "Volunteer to mirror button is unresponsive in web app”,",
        "body": "Nothing happens when the button is pressed."})
    assert parsed["title"] == "Volunteer to mirror button is unresponsive in web app"


def test_automatic_reward_estimate_is_stable_and_uses_only_linked_accounts():
    namespace = _Runtime().namespace()
    estimate = namespace["automatic_reward_estimate"](
        "500000000000000001", "spidertest")
    assert estimate == namespace["automatic_reward_estimate"](
        "500000000000000001", "spidertest")
    assert estimate["recipient"] == "spidertest"
    assert estimate["id"].startswith("reward-discord-")
    assert 1_000_000 <= estimate["amountLamports"] <= 5_000_000
    assert namespace["automatic_reward_estimate"](
        "500000000000000001", "") is None


REWARD_HISTORY = [
    {"number": 592, "title": "Unverified accounts can bypass email "
     "verification with Continue", "summary": "Signing up without verifying "
     "the email still reaches the dashboard session.",
     "amountLamports": 5_000_000},
    {"number": 595, "title": "Sign out other devices leaves other sessions "
     "usable", "summary": "After sign out other devices the old session "
     "token keeps working.", "amountLamports": 4_000_000},
    {"number": 597, "title": "Inline Public profile button in Account "
     "settings is misaligned", "summary": "The button sits below the "
     "baseline of its label.", "amountLamports": 1_000_000},
    {"number": 596, "title": "Login page has no show/hide password toggle",
     "summary": "There is no toggle to reveal the password field.",
     "amountLamports": 1_000_000},
    {"number": 649, "title": "Desktop Ledger dead end", "summary": "The "
     "ledger page cannot be opened.", "amountLamports": 2_000_000},
]


def test_reward_estimate_follows_what_similar_reports_were_paid():
    estimate = _Runtime().namespace()["similar_reward_estimate"]
    severe = estimate(
        "Signing out other devices does not end their sessions",
        "The other session token keeps working after sign out.",
        REWARD_HISTORY)
    cosmetic = estimate(
        "Profile settings button is misaligned",
        "The public profile button sits below its label in account settings.",
        REWARD_HISTORY)
    assert severe["basis"] == "similar" and severe["similar"][0] == 595
    assert severe["amountLamports"] >= 4_000_000
    assert cosmetic["basis"] == "similar" and cosmetic["similar"][0] == 597
    assert cosmetic["amountLamports"] <= 2_000_000
    # Always a whole published step inside the issue-report range.
    for result in (severe, cosmetic):
        assert result["amountLamports"] % 1_000_000 == 0
        assert 1_000_000 <= result["amountLamports"] <= 5_000_000


def test_reward_estimate_falls_back_to_the_typical_then_default_amount():
    estimate = _Runtime().namespace()["similar_reward_estimate"]
    unrelated = estimate("Quadcopter wobbles", "Rotor animation stutters.",
                         REWARD_HISTORY)
    assert unrelated == {"amountLamports": 2_000_000, "basis": "typical",
                         "similar": []}
    assert estimate("Anything", "at all", []) == {
        "amountLamports": 2_000_000, "basis": "default", "similar": []}
    # Amounts outside the issue-report range never vote.
    assert estimate("Quadcopter wobbles", "", [
        {"number": 1, "title": "Quadcopter wobbles", "summary": "",
         "amountLamports": 20_000_000}])["basis"] == "default"


def test_a_filed_report_puts_its_estimate_on_the_ledger():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(
                True, "Sign out other devices leaves sessions usable",
                "After sign out other devices the old session keeps working.")
        return {"verdict": "new", "reason": "Nothing like it exists."}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "old sessions keep working")],
        ai=ai))
    runtime.rows["reward_history"] = [{
        "subject_number": item["number"], "subject_title": item["title"],
        "subject_summary": item["summary"],
        "amount_lamports": item["amountLamports"]} for item in REWARD_HISTORY]
    _report_run(runtime, accounts={"400000000000000001": "spidertest"})

    reward = runtime.filed[0]["reward"]
    assert reward["amountLamports"] >= 4_000_000
    assert reward["similar"][0] == 595
    assert len(runtime.rows["rewards"]) == 1
    (reward_id, recipient_bi, recipient, owner, repo, number, title, summary,
     amount, condition, status, created, updated) = runtime.rows["rewards"][0]
    assert reward_id == reward["id"] and recipient == "spidertest"
    assert recipient_bi.startswith("bi:")
    assert (owner, repo, number) == ("forkmesh", "forkmesh",
                                     runtime.filed[0]["number"])
    assert amount == reward["amountLamports"]
    assert (condition, status) == ("accepted_report", "pending_wallet")
    assert "ForkMesh user spidertest" in summary
    assert runtime.rows["discord_bug_reports"][0]["rewardId"] == reward["id"]


def test_an_unlinked_report_puts_nothing_on_the_ledger():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")], ai=ai))
    runtime.rows["reward_history"] = [{
        "subject_number": 1, "subject_title": "Clone crashes",
        "subject_summary": "", "amount_lamports": 3_000_000}]
    _report_run(runtime, accounts={})
    assert len(runtime.filed) == 1
    assert runtime.rows["rewards"] == []
    # The history is not even read when nobody can be paid.
    assert not any("contributor_reward_estimates" in sql
                   for sql, _ in runtime.queried)


def test_a_ledger_write_failure_still_files_and_answers():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")], ai=ai,
        reward_insert_fails=True))
    result = _report_run(runtime, accounts={"400000000000000001": "spidertest"})
    assert result["handled"] == 1
    assert len(runtime.filed) == 1 and runtime.replies
    assert any("reward could not be recorded" in error
               for error in runtime.errors)


def test_a_duplicate_is_reported_back_without_opening_an_issue():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "duplicate", "duplicateOf": 42,
                "reason": "Same crash as #42."}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")], ai=ai))
    runtime.gateway = {"numbers": [42], "records": [
        {"number": 42, "status": "open", "title": "Clone crashes", "body": ""}]}
    _report_run(runtime)

    assert runtime.filed == []
    assert "duplicate of #42" in runtime.replies[0]
    assert "issues/42" in runtime.replies[0]


def test_an_inconclusive_review_still_files_rather_than_losing_the_report():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "unclear", "reason": "Not enough context."}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")], ai=ai))
    runtime.gateway = {"numbers": [1], "records": [
        {"number": 1, "status": "open", "title": "Other", "body": ""}]}
    _report_run(runtime)
    assert len(runtime.filed) == 1


def test_a_refused_inbox_is_reported_honestly_instead_of_a_fake_issue_number():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")],
        ai=ai, enqueue_fails=True))
    _report_run(runtime)
    assert "could not file it just now" in runtime.replies[0]
    assert runtime.errors


# --- Working while every repository host is offline ---------------------------

def test_review_falls_back_to_the_worker_s_own_queue_with_no_live_host():
    runtime = _Runtime(ai=lambda system, user: {
        "verdict": "duplicate", "duplicateOf": 77, "reason": "Already queued."})
    runtime.gateway = {"numbers": None}  # no host answered
    runtime.rows["issue_inbox"] = [{
        "number": 77, "titleIfNew": "Clone button crashes",
        "event": {"title": "Clone button crashes", "body": "crash on clone"},
    }]
    namespace = runtime.namespace()
    verdict, corpus = run_async_test(namespace["review_report"])(
        _env(), "forkmesh", "forkmesh", "Clone crashes", "it crashes")

    assert corpus == "local"
    assert verdict["duplicateOf"] == 77
    # The queued issue really did reach the reviewer's prompt.
    assert "1 issues" in runtime.ai_calls[-1]["user"]


def test_the_offline_corpus_is_read_under_the_backing_node_not_the_org_alias():
    """forkmesh/forkmesh is an organization alias, not a node.

    Its issue_inbox rows are stored under the BACKING node's blind index --
    _forkbot_enqueue_issue resolves the alias before inserting. Keying the
    backlog read on the alias returns nothing, which would hand the reviewer an
    empty corpus exactly when no host is online and the backlog is all it has.
    """
    runtime = _Runtime(ai=lambda system, user: {
        "verdict": "duplicate", "duplicateOf": 77, "reason": "Already queued."})
    runtime.alias = {"forkmesh": "newnewnode"}  # org alias -> backing node
    runtime.gateway = {"numbers": None}
    runtime.rows["issue_inbox"] = [{
        "number": 77, "titleIfNew": "Clone button crashes",
        "event": {"title": "Clone button crashes", "body": "crash on clone"},
    }]
    namespace = runtime.namespace()
    verdict, corpus = run_async_test(namespace["review_report"])(
        _env(), "forkmesh", "forkmesh", "Clone crashes", "it crashes")

    assert corpus == "local"
    # The queued issue was found, which is only possible via the resolved key.
    assert verdict["duplicateOf"] == 77
    assert "1 issues" in runtime.ai_calls[-1]["user"]
    inbox_reads = [args for sql, args in runtime.queried
                   if "FROM issue_inbox" in sql]
    assert inbox_reads
    assert all(args[0] == runtime.expected_repo_key() for args in inbox_reads)


def test_a_corrupt_backlog_row_skips_that_record_not_the_whole_corpus():
    """Decrypted rows are data, not a contract."""
    runtime = _Runtime(ai=lambda system, user: {"verdict": "new"})
    runtime.gateway = {"numbers": None}
    runtime.rows["issue_inbox"] = [
        {"number": "not-a-number", "titleIfNew": "Broken row",
         "event": {"body": "x"}},
        {"number": 12, "titleIfNew": "Good row", "event": {"body": "y"}},
    ]
    namespace = runtime.namespace()
    run_async_test(namespace["review_report"])(
        _env(), "forkmesh", "forkmesh", "Clone crashes", "it crashes")
    # Both rows still reached the reviewer; neither raised.
    assert "2 issues" in runtime.ai_calls[-1]["user"]


def test_an_offline_filing_tells_the_channel_the_check_was_narrow():
    namespace = _Runtime().namespace()
    reply = namespace["outcome_reply"](
        "luistellom", {"verdict": "new", "reason": "looks new"}, 900,
        "Clone crashes", "https://forkmesh.com/forkmesh/forkmesh", "local")
    assert "No repository host was online" in reply


def test_source_search_is_skipped_rather_than_blocking_when_no_host_answers():
    runtime = _Runtime(ai=lambda system, user: {"verdict": "new"})
    runtime.gateway = {"numbers": None}
    runtime.rows["issue_inbox"] = [{
        "number": 5, "titleIfNew": "x", "event": {"title": "x", "body": "y"}}]
    namespace = runtime.namespace()
    run_async_test(namespace["review_report"])(
        _env(), "forkmesh", "forkmesh", "Clone crashes", "it crashes")
    assert "0 hits" in runtime.ai_calls[-1]["user"]


def test_nothing_to_compare_against_is_new_not_unclear():
    runtime = _Runtime()
    runtime.gateway = {"numbers": None}
    namespace = runtime.namespace()
    verdict, corpus = run_async_test(namespace["review_report"])(
        _env(), "forkmesh", "forkmesh", "Clone crashes", "it crashes")
    assert verdict["verdict"] == "new"
    # No corpus means no reason to spend an AI call.
    assert runtime.ai_calls == []


# --- Attachments --------------------------------------------------------------

def test_images_and_clips_are_copied_and_oversized_files_stay_links():
    namespace = _Runtime().namespace()
    copy, link = namespace["attachment_plan"]([
        {"url": "https://cdn.discordapp.com/a/shot.png", "filename": "shot.png",
         "size": 1000},
        {"url": "https://cdn.discordapp.com/a/clip.mp4", "filename": "clip.mp4",
         "size": 2000},
        {"url": "https://cdn.discordapp.com/a/huge.png", "filename": "huge.png",
         "size": 9 * 1024 * 1024},
        {"url": "https://cdn.discordapp.com/a/notes.docx",
         "filename": "notes.docx", "size": 500},
        {"url": "https://evil.example.com/a/shot.png", "filename": "shot.png",
         "size": 100},
    ])
    assert [entry["filename"] for entry in copy] == ["shot.png", "clip.mp4"]
    # Oversized and unsupported files are linked, never silently dropped.
    assert [entry["filename"] for entry in link] == ["huge.png", "notes.docx"]
    # A non-Discord host is dropped entirely rather than fetched.
    assert all("evil.example.com" not in entry["url"]
               for entry in copy + link)


def test_only_a_bare_discord_cdn_url_is_ever_fetched():
    """The one place this module pulls bytes off the public internet.

    Each of these passes a naive "does the string contain the CDN host" check
    or a naive hostname compare, and none may be fetched. The credentialed
    form is the subtle one: ``hostname`` really is the CDN, but following it
    would send a credential this Worker never issued.
    """
    namespace = _Runtime().namespace()
    hostile = [
        "https://cdn.discordapp.com.evil.com/a/s.png",
        "https://evil.com/cdn.discordapp.com/s.png",
        "https://user:pw@cdn.discordapp.com/a/s.png",
        "https://cdn.discordapp.com@evil.com/s.png",
        "https://cdn.discordapp.com:8443/a/s.png",
        "http://cdn.discordapp.com/a/s.png",
        "https://169.254.169.254/latest/meta-data",
        "https://localhost/a/s.png",
        "//cdn.discordapp.com/a/s.png",
        "file:///etc/passwd",
    ]
    for url in hostile:
        copy, _link = namespace["attachment_plan"](
            [{"url": url, "filename": "s.png", "size": 100}])
        assert copy == [], url

    for url in ("https://cdn.discordapp.com/a/s.png",
                "https://media.discordapp.net/a/s.png"):
        copy, _link = namespace["attachment_plan"](
            [{"url": url, "filename": "s.png", "size": 100}])
        assert len(copy) == 1, url


def test_no_more_than_the_attachment_cap_is_ever_copied():
    namespace = _Runtime().namespace()
    copy, link = namespace["attachment_plan"]([
        {"url": "https://cdn.discordapp.com/a/%d.png" % i,
         "filename": "%d.png" % i, "size": 100} for i in range(20)])
    assert len(copy) == namespace["MAX_ATTACHMENTS"]
    assert len(link) == 20 - namespace["MAX_ATTACHMENTS"]


def test_copied_attachment_bytes_are_bounded_in_total_for_one_inbox_row():
    namespace = _Runtime().namespace()
    copy, link = namespace["attachment_plan"]([
        {"url": "https://cdn.discordapp.com/a/one.png", "filename": "one.png",
         "size": 900_000},
        {"url": "https://cdn.discordapp.com/a/two.png", "filename": "two.png",
         "size": 900_000},
    ])
    assert [entry["filename"] for entry in copy] == ["one.png"]
    assert [entry["filename"] for entry in link] == ["two.png"]


def test_attachment_names_match_the_desktop_issue_store_convention():
    namespace = _Runtime().namespace()
    raw = b"screenshot-bytes"
    expected = hashlib.sha256(raw).hexdigest()[:8]
    assert namespace["attachment_name"](raw, "png") == expected + ".png"
    assert namespace["attachment_name"](raw, "exe") == expected + ".bin"
    # Every name this can produce is one the platform allowlist accepts.
    allowlist = re.compile(
        r"^[0-9a-f]{8}\.(png|jpg|jpeg|gif|webp|bmp|svg|mp4|webm|mov|bin)$")
    for extension in sorted(namespace["ATTACHMENT_EXTENSIONS"]) + ["exe"]:
        assert allowlist.fullmatch(namespace["attachment_name"](raw, extension))


def test_downloaded_media_rides_the_issue_and_is_named_in_the_body():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    raw = b"png-bytes-here"
    runtime = _seeded(_Runtime(
        messages=[_message(
            "500000000000000001", "crash, see the screenshot",
            attachments=[
                {"url": "https://cdn.discordapp.com/a/shot.png",
                 "filename": "shot.png", "size": len(raw)},
                {"url": "https://cdn.discordapp.com/a/huge.mov",
                 "filename": "huge.mov", "size": 9 * 1024 * 1024},
            ])],
        ai=ai))
    runtime.fetched["https://cdn.discordapp.com/a/shot.png"] = raw
    _report_run(runtime)

    attachments = runtime.filed[0]["attachments"]
    name = hashlib.sha256(raw).hexdigest()[:8] + ".png"
    assert attachments == [
        {"name": name, "data": base64.b64encode(raw).decode()}]
    body = runtime.filed[0]["body"]
    assert ("Attached from Discord: %s (shot.png)." % name) in body
    # The clip that was too large to copy is still reachable from the issue.
    assert "huge.mov — https://cdn.discordapp.com/a/huge.mov" in body


def test_an_unreachable_attachment_downgrades_to_a_link_and_still_files():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(
        messages=[_message(
            "500000000000000001", "crash",
            attachments=[{"url": "https://cdn.discordapp.com/a/gone.png",
                          "filename": "gone.png", "size": 10}])],
        ai=ai))
    _report_run(runtime)  # nothing seeded in runtime.fetched
    assert runtime.filed[0]["attachments"] == []
    assert "gone.png — https://cdn.discordapp.com/a/gone.png" in (
        runtime.filed[0]["body"])


# --- Cursor, idempotency and bounds -------------------------------------------

def test_first_sight_of_a_channel_adopts_the_cursor_without_replying():
    runtime = _Runtime(messages=[
        _message("500000000000000001", "an old report nobody answered"),
        _message("500000000000000009", "a newer one"),
    ])
    namespace = runtime.namespace()
    result = run_async_test(namespace["watch_discord_bug_reports"])(_env())
    assert result["adopted"] is True
    assert runtime.replies == []
    assert runtime.filed == []
    assert list(runtime.rows["discord_bug_watch"].values()) == [
        "500000000000000009"]


def test_a_message_already_answered_is_never_filed_or_answered_twice():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Clone crashes", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")], ai=ai))
    runtime.rows["discord_bug_reports"] = [
        {"messageId": "500000000000000001", "issueNumber": 900,
         "title": "Clone crashes"}]
    _report_run(runtime)
    assert runtime.filed == []
    assert runtime.replies == []


def test_only_one_report_is_carried_to_completion_per_run():
    def ai(system, user):
        if "DISCORD MESSAGE" in user:
            return _triage_answer(True, "Crash", "It crashes.")
        return {"verdict": "new", "reason": ""}

    runtime = _seeded(_Runtime(messages=[
        _message("500000000000000001", "crash one"),
        _message("500000000000000002", "crash two"),
    ], ai=ai))
    result = _report_run(runtime)
    assert result["handled"] == 1
    # The cursor stops at the message that was finished, so the second report
    # is picked up by the next tick rather than skipped.
    assert list(runtime.rows["discord_bug_watch"].values()) == [
        "500000000000000001"]


def test_a_classifier_outage_leaves_the_cursor_put_so_nothing_is_lost():
    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")], ai=None))
    result = _report_run(runtime)
    assert result["handled"] == 0
    assert runtime.replies == []
    # Cursor unchanged: these messages are re-read once Workers AI is back.
    assert list(runtime.rows["discord_bug_watch"].values()) == [
        "500000000000000000"]


def test_running_out_of_phase_budget_stops_before_starting_a_report():
    """The phase must stop where the cursor still is, not mid-report.

    A Cron phase shares one 40-subrequest ceiling. Being cut off after the
    issue is filed but before the reply would leave a reporter with silence,
    so the budget is charged for each classification and the scan ends cleanly
    the moment it runs out.
    """
    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes"),
                  _message("500000000000000002", "another crash")],
        ai=lambda system, user: _triage_answer(True, "Crash", "It crashes."),
        budget=0))
    result = _report_run(runtime)

    assert result["handled"] == 0
    assert runtime.filed == []
    assert runtime.replies == []
    # Cursor untouched, so the next tick re-reads both messages.
    assert list(runtime.rows["discord_bug_watch"].values()) == [
        "500000000000000000"]


def test_each_classification_is_charged_against_the_phase_budget():
    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "just saying hello")],
        ai=lambda system, user: _triage_answer(False)))
    _report_run(runtime)
    assert runtime.charged == 1


def test_an_unconfigured_deployment_makes_no_discord_call_at_all():
    runtime = _Runtime()
    namespace = runtime.namespace()
    result = run_async_test(namespace["watch_discord_bug_reports"])(
        _env(DISCORD_BUGS_CHANNEL_ID=""))
    assert result["reason"] == "watch_not_configured"
    assert runtime.discord == []


def test_a_missing_bot_secret_stops_before_any_network_call():
    runtime = _Runtime(token="")
    namespace = runtime.namespace()
    result = run_async_test(namespace["watch_discord_bug_reports"])(_env())
    assert result["reason"] == "discord_secret_required"
    assert runtime.discord == []


def test_the_watched_repository_must_be_a_well_formed_owner_slash_repo():
    namespace = _Runtime().namespace()
    for repo in ("", "forkmesh", "forkmesh/forkmesh/extra", "../etc/passwd",
                 "forkmesh/", "/forkmesh"):
        assert namespace["watch_config"](
            _env(DISCORD_BUGS_REPO=repo)) is None
    assert namespace["watch_config"](_env())["repo"] == "forkmesh"


# --- Contracts with the rest of the Worker ------------------------------------

def test_the_paged_read_and_the_reply_are_both_inside_the_bot_gate_allowlist():
    allowed = ENTRY_TEXT[
        ENTRY_TEXT.index("_DISCORD_CHANNEL_MESSAGES_GET_PATH_RE"):
        ENTRY_TEXT.index("def _discord_oauth_path_allowed")]
    get_pattern = re.compile(
        r"^/channels/[0-9]{17,20}/messages\?limit=[1-9][0-9]?"
        r"(?:&after=[0-9]{17,20})?$")
    assert "&after=" in allowed
    runtime = _seeded(_Runtime(messages=[]))
    namespace = runtime.namespace()
    run_async_test(namespace["watch_discord_bug_reports"])(_env())
    assert get_pattern.fullmatch(runtime.discord[0]["path"])


def test_the_reply_can_never_ping_anybody():
    runtime = _seeded(_Runtime(
        messages=[_message("500000000000000001", "clone crashes")],
        ai=lambda system, user: (
            _triage_answer(True, "Crash", "It crashes.")
            if "DISCORD MESSAGE" in user
            else {"verdict": "new", "reason": ""})))
    _report_run(runtime)
    posts = [call for call in runtime.discord if call["method"] == "POST"]
    assert posts
    for call in posts:
        assert call["body"]["allowed_mentions"] == {"parse": []}


def test_the_cron_phase_is_registered_and_dispatched():
    assert 'phases.append("discord-bugs")' in ENTRY_TEXT
    assert 'if phase == "discord-bugs":' in ENTRY_TEXT
    assert "watch_discord_bug_reports = discord_bugs.export(" in ENTRY_TEXT
    assert 'discord_bugs = _LazyModule("discord_bugs")' in ENTRY_TEXT


def test_the_watcher_tables_ship_in_the_schema_and_a_migration():
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    migration = (ROOT / "migrations" /
                 "0150_discord_bug_watch.sql").read_text(encoding="utf-8")
    for table in ("discord_bug_watch", "discord_bug_reports"):
        assert table in schema
        assert table in migration


def test_the_module_never_reads_or_logs_the_bot_credential():
    source = MODULE.read_text(encoding="utf-8")
    assert "DISCORD_BOT_TOKEN" not in source
    assert "Authorization" not in source
    # Every Discord call goes through the gated helper, never a raw fetch.
    assert "discord.com/api" not in source


def test_every_injected_runtime_global_really_exists_in_the_entrypoint():
    """The lazy-binding contract, which stubs in these tests would otherwise hide.

    ``_bind_runtime`` copies entry.py's globals in wholesale, so a helper this
    module names but entry.py does not define fails with a NameError on the
    first Cron tick that reaches it -- in production, never here. Compare the
    free variables of the real source against entry.py's real global scope.
    """
    import ast
    import builtins

    tree = ast.parse(MODULE.read_text(encoding="utf-8"), filename=str(MODULE))
    bound = set(dir(builtins)) | {"__name__", "__doc__", "__builtins__"}

    def bind_target(target):
        for node in ast.walk(target):
            if isinstance(node, ast.Name):
                bound.add(node.id)

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            bound.add(node.name)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                bound.add((alias.asname or alias.name).split(".")[0])
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                bind_target(target)
        elif isinstance(node, (ast.AugAssign, ast.AnnAssign)):
            bind_target(node.target)
        elif isinstance(node, (ast.For, ast.AsyncFor)):
            bind_target(node.target)
        elif isinstance(node, ast.comprehension):
            bind_target(node.target)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            bound.add(node.name)
        elif isinstance(node, ast.withitem) and node.optional_vars is not None:
            bind_target(node.optional_vars)
        elif isinstance(node, ast.arguments):
            for argument in (node.posonlyargs + node.args + node.kwonlyargs
                             + [node.vararg, node.kwarg]):
                if argument is not None:
                    bound.add(argument.arg)

    expected = {node.id for node in ast.walk(tree)
                if isinstance(node, ast.Name)
                and isinstance(node.ctx, ast.Load)} - bound

    entry_tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    provided = set()
    for node in entry_tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            provided.add(node.name)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                provided.add((alias.asname or alias.name).split(".")[0])
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    provided.add(target.id)

    assert expected, "the scanner found no runtime globals at all"
    assert expected - provided == set()
    # And the stub table in this file covers the same contract, so a helper
    # added to the module without a stub fails loudly here.
    assert expected - set(_Runtime().primitives()) == set()
