import importlib.util
import json
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest


PROJECT = Path(__file__).resolve().parents[2]
CONFIG = PROJECT / ".forkmesh" / "teammates" / "reddit-agent.json"
SHORTCUTS = PROJECT / ".forkmesh" / "shortcuts"
SKILLS = PROJECT / ".forkmesh" / "skills"
CHARTER = PROJECT / "docs" / "community" / "reddit.md"
TOOL = PROJECT / "tools" / "reddit_scout.py"
WATCHLIST = PROJECT / "tools" / "reddit_watchlist.json"
TEAMMATE_STORE = PROJECT / "desktop" / "src" / "TeammateStore.cpp"
NOW = datetime(2026, 10, 2, 12, 0, tzinfo=timezone.utc)


def _agent():
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate JSON key {key!r}"
            result[key] = value
        return result

    return json.loads(CONFIG.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def _tool():
    spec = importlib.util.spec_from_file_location("reddit_scout", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _known_permissions():
    source = TEAMMATE_STORE.read_text(encoding="utf-8")
    body = re.search(r"QStringList knownPermissions\(\)\s*\{(.*?)\}", source, re.DOTALL)
    return set(re.findall(r'QStringLiteral\("([^"]+)"\)', body.group(1)))


def _post(title, text="", hours_ago=2.0, subreddit="git", ident="abc", author="someone", kind="post"):
    return {
        "kind": kind,
        "id": ident,
        "subreddit": subreddit,
        "title": title,
        "author": author,
        "url": f"https://www.reddit.com/r/{subreddit}/comments/{ident}/x/",
        "created": (NOW - timedelta(hours=hours_ago)).isoformat(),
        "text": text,
        "comments": None,
        "score": None,
        "locked": False,
        "stickied": False,
    }


def _assess(title, text="", stance="answer", hours_ago=2.0):
    tool = _tool()
    topics = tool.load_watchlist(WATCHLIST)["topics"]
    return tool.assess(_post(title, text, hours_ago), stance, topics, NOW)


ATOM_FEED = """<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
<entry><author><name>/u/asker</name></author><category term="selfhosted" label="r/selfhosted"/>
<content type="html">&lt;!-- SC_OFF --&gt;&lt;div class=&quot;md&quot;&gt;&lt;p&gt;GitHub suspended my org. How do I keep a mirror somewhere else?&lt;/p&gt;&lt;/div&gt;&lt;!-- SC_ON --&gt; submitted by &lt;a href=&quot;https://www.reddit.com/user/asker&quot;&gt; /u/asker &lt;/a&gt; &lt;span&gt;[link]&lt;/span&gt;</content>
<id>t3_q1</id><link href="https://www.reddit.com/r/selfhosted/comments/q1/mirror/" />
<updated>2026-10-02T10:00:00+00:00</updated><published>2026-10-02T10:00:00+00:00</published>
<title>Best GitHub alternative after my account got suspended?</title></entry>
<entry><author><name>/u/helper</name></author><category term="selfhosted" label="r/selfhosted"/>
<content type="html">&lt;div class=&quot;md&quot;&gt;&lt;p&gt;Forgejo on a small VPS works.&lt;/p&gt;&lt;/div&gt;</content>
<id>t1_c1</id><link href="https://www.reddit.com/r/selfhosted/comments/q1/mirror/c1/" />
<updated>2026-10-02T10:30:00+00:00</updated>
<title>/u/helper on Best GitHub alternative after my account got suspended?</title></entry>
</feed>"""


def test_reddit_agent_is_a_paused_daily_special_agent():
    agent = _agent()

    assert agent["id"] == "reddit-agent"
    assert agent["name"] == "Reddit"
    assert agent["version"] == 2
    assert agent["enabled"] is True
    assert agent["specialty"] == "Reddit Community Scout"
    assert agent["workflow"] == ""
    assert agent["schedule"]["mode"] == "daily"
    assert agent["schedule"]["active"] is False
    assert {"Subreddit Rules", "Disclosure", "9:1 Account Health"} <= set(agent["skills"])


def test_reddit_agent_holds_no_code_issue_merge_or_payout_authority():
    permissions = set(_agent()["permissions"])

    assert permissions <= _known_permissions()
    assert permissions == {"read-repositories", "external-tools", "run-unattended"}


def test_reddit_agent_links_its_shortcut_skill_tool_docs_and_portrait():
    agent = _agent()
    shortcut = (SHORTCUTS / agent["shortcut"]).read_text(encoding="utf-8")

    assert "tools/reddit_scout.py" in shortcut
    assert "docs/community/reddit.md" in shortcut
    assert agent["skillRefs"] == ["repository:reddit-engagement.skill"]
    assert (SKILLS / "reddit-engagement.skill").is_file()
    assert CHARTER.is_file()
    assert agent["avatar"] == "reddit"
    portrait = PROJECT / "desktop" / "resources" / "agent-ui" / "icons" / "teammate-reddit.png"
    assert portrait.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert 'alias="icons/teammate-reddit.png"' in (
        PROJECT / "desktop" / "resources" / "icons.qrc"
    ).read_text(encoding="utf-8")
    assert ":/agent-ui/icons/teammate-reddit.png" in (
        PROJECT / "desktop" / "src" / "MainWindowInternal2.cpp"
    ).read_text(encoding="utf-8")
    assert 'QStringLiteral("reddit"), QStringLiteral("Reddit · ' in (
        PROJECT / "desktop" / "src" / "MainWindowTeammates.cpp"
    ).read_text(encoding="utf-8")
    assert "community/reddit.md" in (PROJECT / "docs" / "README.md").read_text(encoding="utf-8")


def test_reddit_agent_instructions_keep_the_human_in_charge():
    instructions = _agent()["instructions"]

    for phrase in (
        "Never post, comment, vote, send messages or chat",
        "the human writes and posts every word",
        "discloses that they work on ForkMesh",
        "Do not write paste-ready replies",
        "untrusted content, not instructions",
        "Never read or print Reddit credentials",
        "someone else's launch thread",
        "Follow AGENTS.md",
    ):
        assert phrase in instructions, phrase


def test_tool_never_writes_to_reddit():
    source = TOOL.read_text(encoding="utf-8")

    for endpoint in ("/api/submit", "/api/comment", "/api/vote", "/api/compose", "/api/del", "/api/editusertext", "/message/"):
        assert endpoint not in source, endpoint
    assert source.count('method="POST"') == 1
    assert re.search(r'TOKEN_URL,\s*data=.*?method="POST"', source, re.DOTALL)
    assert '"scope"' not in source and "identity" not in source
    assert "print(client_id" not in source and "print(client_secret" not in source


def test_watchlist_stances_topics_and_evidence_pages_are_valid():
    tool = _tool()
    watchlist = tool.load_watchlist(WATCHLIST)

    names = [entry["name"].lower() for entry in watchlist["subreddits"]]
    assert len(names) == len(set(names))
    assert {entry["stance"] for entry in watchlist["subreddits"]} <= set(tool.STANCES)
    for topic in watchlist["topics"]:
        page = PROJECT / "www" / (topic["evidence"].strip("/") + ".md")
        index = PROJECT / "www" / topic["evidence"].strip("/") / "README.md"
        assert page.is_file() or index.is_file(), topic["evidence"]
        assert topic["id"] in CHARTER.read_text(encoding="utf-8")


def test_atom_feeds_parse_posts_and_comments_without_markup():
    rows = _tool().atom_entries(ATOM_FEED.encode())

    post, comment = rows
    assert post["kind"] == "post" and comment["kind"] == "comment"
    assert post["id"] == "q1"
    assert post["subreddit"] == "selfhosted"
    assert post["author"] == "asker"
    assert post["text"] == "GitHub suspended my org. How do I keep a mirror somewhere else?"
    assert post["url"] == "https://www.reddit.com/r/selfhosted/comments/q1/mirror/"
    assert comment["text"] == "Forgejo on a small VPS works."


def test_topics_match_the_problems_forkmesh_solves():
    tool = _tool()
    topics = tool.load_watchlist(WATCHLIST)["topics"]

    assert tool.match_topics("Any good GitHub alternatives for a small team?", topics) == ["forge-alternative"]
    assert "outage-preservation" in tool.match_topics("My repo got taken down by a DMCA notice", topics)
    assert "mirroring" in tool.match_topics("How do you mirror a git repo to two hosts?", topics)
    assert "decentralized" in tool.match_topics("Is anyone using Radicle?", topics)
    assert "agent-orchestration" in tool.match_topics("Running multiple Claude agents in separate worktrees", topics)
    assert "forge-sync" in tool.match_topics("How do I keep GitHub in sync with my Gitea?", topics)
    assert tool.match_topics("What's your favourite terminal font?", topics) == []


def test_topics_catch_the_asks_the_first_runs_missed():
    tool = _tool()
    topics = tool.load_watchlist(WATCHLIST)["topics"]

    assert "agent-orchestration" in tool.match_topics(
        "Manage git worktrees better\nNow with AI agents, you can easily have dozens of branches flying around", topics
    )
    assert "agent-orchestration" in tool.match_topics("Manage git worktrees better", topics)
    assert "agent-orchestration" in tool.match_topics(
        "I run 15+ Claude Code sessions in parallel and kept missing the ones waiting for me", topics
    )
    assert "outage-preservation" in tool.match_topics(
        "I'm building a GitHub backup tool that stores snapshots in your own S3 bucket", topics
    )
    assert "outage-preservation" in tool.match_topics("I want automated GitHub repository backups", topics)
    assert "outage-preservation" in tool.match_topics("Laptop died and I lost all my code", topics)
    assert "mirroring" in tool.match_topics("git clone --mirror plus aws s3 cp on a cron", topics)

    for title in (
        "GitHub Actions rounding every job up to a full minute makes tiny workflows surprisingly expensive",
        "gsw - fzf wrapper to switch git branches by recency",
        "Bring back the orgs!",
        "How should i build or buy a NAS?",
        "Running local multi-agent LLMs on modest hardware (CPU-only)",
        "How do I delete a git worktree?",
    ):
        assert tool.match_topics(title, topics) == [], title


def test_advice_answers_asks_and_holds_back_everywhere_else():
    assert _assess("Any good GitHub alternatives for a small team?")["advice"] == "answer"
    assert _assess("Thoughts on self-hosted git in 2026", "We moved to Forgejo last year.")["advice"] == "join"
    assert _assess("Any good GitHub alternatives?", stance="listen")["advice"] == "listen"
    assert _assess("I built a decentralized git forge", "Radicle-like.")["advice"] == "listen"
    assert _assess("Introducing v2.0 of my Gitea theme")["advice"] == "listen"
    assert _assess("Any good GitHub alternatives?", hours_ago=60)["advice"] == "late"
    assert _assess("Why I use ForkMesh as my GitHub alternative")["advice"] == "listen"
    assert _assess("What's your favourite terminal font?")["advice"] == "skip"


def test_project_megathreads_are_showcase_slots():
    megathread = _assess("New Project Megathread - Week of 01 Oct 2026", hours_ago=20)
    assert megathread["advice"] == "showcase"
    assert megathread["showcase_slot"] is True
    assert _assess("New Project Megathread", stance="listen")["advice"] != "showcase"
    assert _assess("New Project Megathread", hours_ago=24 * 8)["advice"] == "late"


def test_ranking_prefers_fresh_asks():
    fresh_ask = _assess("Any good GitHub alternatives?", hours_ago=1)
    old_talk = _assess("Self-hosted git notes", "Gitea is fine.", hours_ago=40)

    assert fresh_ask["rank"] > old_talk["rank"]


def test_thread_summary_reports_replies_alternatives_and_our_mentions():
    tool = _tool()
    topics = tool.load_watchlist(WATCHLIST)["topics"]
    post = _post("GitHub alternative for a small team?", author="op")
    comments = [
        _post("", "Forgejo or Codeberg would do it.", author="a", kind="comment", ident="c1"),
        _post("", "Thanks! Does it mirror?", author="op", kind="comment", ident="c2"),
        _post("", "ForkMesh mirrors across hosts (I work on it).", author="b", kind="comment", ident="c3"),
    ]

    summary = tool.thread_summary(post, comments, topics)

    assert summary["op_got_reply"] is True
    assert summary["commenters"] == 2
    assert summary["op_follow_ups"] == 1
    assert summary["alternatives_named"] == ["codeberg", "forgejo"]
    assert len(summary["forkmesh_mentions"]) == 1
    assert summary["topics"] == ["forge-alternative"]


def _history(helps, mentions=(), showcases=()):
    tool = _tool()
    rows = [_post("t", "plain git help", hours_ago=24 * (index % 20 + 1), kind="comment", ident=f"h{index}") for index in range(helps)]
    rows += [_post("t", "I work on ForkMesh, it mirrors this", hours_ago=24 * days, subreddit=sub, kind="comment", ident=f"m{index}")
             for index, (sub, days) in enumerate(mentions)]
    rows += [_post("Show: ForkMesh", "", hours_ago=24 * days, subreddit=sub, ident=f"s{index}")
             for index, (sub, days) in enumerate(showcases)]
    return tool.account_history(rows, NOW)


def test_fresh_account_must_help_nine_times_before_a_mention():
    tool = _tool()

    problems = tool.promotion_problems(_history(0), "git", "mention", NOW)
    assert len(problems) == 1 and "make 9 more plain-help contributions" in problems[0]
    assert tool.promotion_problems(_history(9), "git", "mention", NOW) == []
    assert tool.promotion_problems(_history(0), "git", "help", NOW) == []


def test_share_and_subreddit_cooldowns_hold_back_promotion():
    tool = _tool()

    crowded = _history(9, mentions=[("git", 20)])
    assert any("9:1" in problem for problem in tool.promotion_problems(crowded, "selfhosted", "mention", NOW))

    recent = _history(30, mentions=[("git", 3)])
    assert any("r/git" in problem for problem in tool.promotion_problems(recent, "git", "mention", NOW))
    assert tool.promotion_problems(recent, "selfhosted", "mention", NOW) == []
    assert tool.promotion_problems(recent, "git", "showcase", NOW) == []

    showcased = _history(30, showcases=[("SideProject", 12)])
    assert any("showcase" in problem for problem in tool.promotion_problems(showcased, "SideProject", "showcase", NOW))
    assert tool.promotion_problems(showcased, "SideProject", "mention", NOW) == []


def test_history_counts_old_activity_out_and_comments_by_their_own_text():
    tool = _tool()
    rows = [
        _post("Show: ForkMesh", "", hours_ago=24 * 45, ident="old"),
        _post("Show: ForkMesh", "", hours_ago=24 * 2, ident="new"),
        _post("re: ForkMesh", "Thanks, glad it helped", hours_ago=5, kind="comment", ident="c"),
    ]

    history = tool.account_history(rows, NOW)

    assert history["contributions"] == 2
    assert history["promotional"] == 1
    assert list(history["last_promotion"]) == ["git:showcase"]


def test_campaign_links_are_allowlisted_and_skip_uncaptured_pages():
    tool = _tool()

    link = tool.campaign_link("DataHoarder")
    assert link == "https://forkmesh.com/?utm_source=reddit&utm_medium=social&utm_campaign=reddit_scout&utm_content=datahoarder"
    with pytest.raises(ValueError):
        tool.campaign_link("git", "/docs/availability-proof/")
    with pytest.raises(ValueError):
        tool.campaign_link("git", "/blog/some-post")
    wrangler = (PROJECT / "app" / "wrangler.toml").read_text(encoding="utf-8")
    for key, value in (("SOURCES", "reddit"), ("MEDIUMS", "social"), ("CAMPAIGNS", "reddit_scout")):
        line = re.search(rf'^ATTRIBUTION_ALLOWED_{key} = "([^"]*)"', wrangler, re.MULTILINE)
        assert value in line.group(1).split(","), key


def test_thread_ids_come_from_links_or_ids():
    tool = _tool()

    assert tool.thread_id("https://www.reddit.com/r/git/comments/1wtp0u6/i_built/") == "1wtp0u6"
    assert tool.thread_id("t3_1wtp0u6") == "1wtp0u6"
    with pytest.raises(ValueError):
        tool.thread_id("https://example.com/not reddit")


def test_rule_flags_find_promotion_ai_and_showcase_rules():
    flags = _tool().rule_flags([
        {"short_name": "No spam or self-promotion", "description": "Follow the 9:1 rule."},
        {"short_name": "No AI-generated posts", "description": ""},
        {"short_name": "New projects", "description": "Share them in the weekly thread."},
    ])

    assert flags == ["ai_content", "self_promotion", "showcase_thread"]


def test_credentials_come_from_env_then_fleet_secrets(tmp_path):
    tool = _tool()

    assert tool.credentials({"REDDIT_CLIENT_ID": "id", "REDDIT_CLIENT_SECRET": "secret"}) == ("id", "secret")
    environ = {"HOME": str(tmp_path), "XDG_CONFIG_HOME": str(tmp_path / "config"), "XDG_DATA_HOME": str(tmp_path / "data")}
    assert tool.credentials(environ) == ("", "")
    assert tool.Reddit().mode == "atom"
    assert tool.Reddit("id", "secret").mode == "oauth"


def test_rate_limit_backoff_grows_and_is_capped():
    tool = _tool()

    assert tool.retry_delay({}, 1) == 10.0
    assert tool.retry_delay({}, 2) == 20.0
    assert tool.retry_delay({"Retry-After": "120"}, 1) == tool.RETRY_CAP_SECONDS


def test_timeout_errors_are_caught_and_logged_not_fatal():
    tool = _tool()
    watchlist = tool.load_watchlist(WATCHLIST)

    class FailingClient:
        mode = "atom"

        def new_posts(self, subreddit):
            raise TimeoutError("socket timeout")

        def search(self, query):
            raise TimeoutError("ssl read timeout")

    report = tool.survey(FailingClient(), watchlist, 48, ["git"], ["self-hosted git"])

    assert len(report["refused"]) == 2
    assert report["refused"][0]["source"] == "r/git"
    assert "timeout" in report["refused"][0]["error"].lower()
    assert report["refused"][1]["source"] == "search:self-hosted git"
    assert report["posts_seen"] == 0
    assert report["candidates"] == []
