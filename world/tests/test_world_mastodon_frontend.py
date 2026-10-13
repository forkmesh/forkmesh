#!/usr/bin/env python3
"""Contracts for the Mastodon kiosk and mini-app beside the ForkMesh Office."""

import json
from pathlib import Path
import re
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
MASTODON_PATH = ROOT.parent / "world" / "public" / "world" / "world-mastodon.js"
SCENE_PATH = ROOT.parent / "world" / "public" / "world" / "world-scene.js"
WORLD_PATH = ROOT.parent / "world" / "public" / "world" / "world.js"
CSS_PATH = ROOT.parent / "world" / "public" / "world" / "world.css"


def _source(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def _node(script):
    return json.loads(
        subprocess.run(
            ["node", "--input-type=module", "-e", script],
            check=True,
            text=True,
            capture_output=True,
        ).stdout
    )


def test_mastodon_module_exists():
    assert MASTODON_PATH.exists()


def test_scene_places_a_clickable_kiosk_beside_the_office():
    scene = world_scene_source()
    assert "createMastodonKiosk" in scene
    assert '"mastodon-board"' in scene
    assert "onMastodonBoardSelect = () => {}" in scene
    assert 'userData?.interactive === "mastodon-board"' in scene
    # The kiosk stands beside the Office approach at [45, 0, -27], not in the
    # arrival grid or in front of the Office door / exterior keypad.
    assert "group.position.set(33.5, 0, -18.5);" in scene


def test_world_opens_the_mini_app_and_fetches_without_credentials():
    world = _source(WORLD_PATH)
    assert "onMastodonBoardSelect: () => this.openMastodonBoard()" in world
    assert 'from "./world-mastodon.js"' in world
    assert 'credentials: "omit"' in world
    assert "MASTODON_LOOKUP_URL" in world
    assert "exclude_replies=true" in world
    assert 'class="world-mastodon-app"' in world
    assert "data-world-mastodon-toots" in world
    assert "data-world-mastodon-retry" in world
    # Untrusted remote text is always escaped before injection.
    assert re.search(r"escapeHTML\(\s*status\.text,?\s*\)", world)


def test_kiosk_refreshes_every_ten_minutes_behind_an_mmss_timer():
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    assert "const MASTODON_REFRESH_MS = 10 * 60 * 1000;" in world
    assert "Date.now() - this.mastodonFetchedAt < MASTODON_REFRESH_MS" in world
    assert "startMastodonRefresh()" in world
    assert "this.startMastodonRefresh();" in world
    assert "window.clearInterval(this.mastodonRefreshTimer);" in world
    assert "updateMastodonCountdown" in world
    # The countdown runs from the attempt so a failed fetch cannot hot-loop.
    assert "this.mastodonRequestedAt = Date.now();" in world
    assert "MASTODON_KIOSK_REFRESH_MS = 10 * 60 * 1000" in scene
    assert "function mastodonCountdownTexture(" in scene
    assert '"forkmesh-mastodon-kiosk-countdown"' in scene
    assert "function updateMastodonCountdown(" in scene
    assert "updateMastodonCountdown," in scene
    # A plain MM:SS readout: no ring, no arc, no progress sweep.
    assert "function mastodonCountdownClock(" in scene
    countdown = scene.split("function mastodonCountdownClock(", 1)[1].split(
        "function createMastodonKiosk(", 1
    )[0]
    assert "context.arc(" not in countdown
    assert "progress" not in countdown
    assert 'String(seconds % 60).padStart(' in countdown


def test_countdown_clock_formats_mm_ss():
    scene = world_scene_source()
    body = scene.split("function mastodonCountdownClock(", 1)[1].split("\n}\n", 1)[0]
    clock = _node(
        "function mastodonCountdownClock(" + body + "\n}\n"
        "process.stdout.write(JSON.stringify(["
        "mastodonCountdownClock(0),"
        "mastodonCountdownClock(59_000),"
        "mastodonCountdownClock(10 * 60 * 1000),"
        "]));"
    )
    assert clock == ["00:00", "00:59", "10:00"]


def test_kiosk_shows_time_since_last_post_with_cadence_colors():
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    # The stand carries a second plate beside NEXT SYNC: how long since the
    # newest toot, tinted by whether the once-a-day cadence is being kept.
    assert "mastodonLastPostAgo()" in world
    assert "lastPostAgoMs: this.mastodonLastPostAgo()," in world
    assert "const MASTODON_POST_FRESH_MS = 24 * 60 * 60 * 1000;" in scene
    assert "const MASTODON_POST_STALE_MS = 72 * 60 * 60 * 1000;" in scene
    assert "function mastodonLastPostTexture(" in scene
    assert '"forkmesh-mastodon-kiosk-lastpost"' in scene
    # The plate label is parameterized so the social banners can reuse the
    # painter; the kiosk keeps the LAST POST default.
    assert "context.fillText(label, 128, 44);" in scene
    assert 'label = "LAST POST",' in scene


def test_last_post_clock_and_colors_follow_the_cadence():
    scene = world_scene_source()
    clock = scene.split("function mastodonLastPostClock(", 1)[1].split(
        "\n}\n", 1
    )[0]
    color = scene.split("function mastodonLastPostColor(", 1)[1].split(
        "\n}\n", 1
    )[0]
    hour = 60 * 60 * 1000
    values = _node(
        "const MASTODON_POST_FRESH_MS = 24 * 60 * 60 * 1000;\n"
        "const MASTODON_POST_STALE_MS = 72 * 60 * 60 * 1000;\n"
        "function mastodonLastPostClock(" + clock + "\n}\n"
        "function mastodonLastPostColor(" + color + "\n}\n"
        "process.stdout.write(JSON.stringify(["
        "mastodonLastPostClock(0),"
        f"mastodonLastPostClock({5 * hour}),"
        f"mastodonLastPostClock({80 * hour}),"
        f"mastodonLastPostColor({5 * hour}),"
        f"mastodonLastPostColor({30 * hour}),"
        f"mastodonLastPostColor({80 * hour}),"
        "]));"
    )
    assert values == [
        "0M AGO",
        "5H AGO",
        "3D AGO",
        "#9ef7c6",
        "#ffb454",
        "#ff7a7a",
    ]


def test_kiosk_and_mini_app_carry_replies_with_author_icons():
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    # Replies are collected from the thread context of the newest toots.
    assert "async fetchMastodonReplies(" in world
    assert "/context`" in world
    assert "reply.authorAcct === account.acct" in world
    assert "const MASTODON_REPLY_LIMIT = 12;" in world
    assert "replies: this.mastodonReplies.map(" in world
    assert "avatar: reply.authorAvatar," in world
    # Mini-app section plus the kiosk strip, each with the replier's icon.
    assert "mastodonRepliesHTML()" in world
    assert "data-world-mastodon-replies" in world
    assert re.search(r"escapeHTML\(\s*reply\.text,?\s*\)", world)
    assert re.search(r"escapeHTML\(\s*reply\.authorAvatar,?\s*\)", world)
    assert "const MASTODON_KIOSK_VISIBLE_REPLIES = 5;" in scene
    assert 'context.fillText("REPLIES", 56, 3560);' in scene
    assert "const icon = image(reply.avatar);" in scene
    assert "Array.isArray(snapshot.replies) ? snapshot.replies : []" in scene


def test_kiosk_board_is_larger_and_carries_post_images():
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    assert "images: status.images.map((media) => media.url)" in world
    assert "const MASTODON_KIOSK_WIDTH = 1536;" in scene
    # Twice as tall as the old 2048 board, so the attachment strip and a third
    # toot card fit without crowding the replies section.
    assert "const MASTODON_KIOSK_HEIGHT = 4096;" in scene
    assert "const MASTODON_KIOSK_VISIBLE_TOOTS = 3;" in scene
    # Board plane and canvas keep the same 0.375 aspect ratio.
    assert "new THREE.PlaneGeometry(7.2, 19.2)" in scene
    assert "new THREE.BoxGeometry(7.9, 20.0, 0.36)" in scene
    assert "Array.isArray(toot.images) ? toot.images : []" in scene
    # A third open-in-a-new-tab button rides along with the third card.
    assert 'setMastodonOpenButton("toot-2", visibleToots[2]?.url);' in scene


def test_kiosk_cards_carry_stars_boosts_and_reply_counts():
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    # The snapshot carries the engagement counts the mini-app footer shows.
    assert "replies: formatMastodonCount(status.repliesCount)," in world
    assert "boosts: formatMastodonCount(status.reblogsCount)," in world
    assert "stars: formatMastodonCount(status.favouritesCount)," in world
    for line in ("${toot.stars ?? 0} STARS", "${toot.boosts ?? 0} BOOSTS",
                 "${toot.replies ?? 0} REPLIES"):
        assert line in scene
    # Pinned / boost / content-warning markers get their own card line.
    assert "marker," in world
    assert "const marker = String(toot.marker || \"\").trim();" in scene


def test_kiosk_board_carries_the_profile_bio_and_link_fields():
    world = _source(WORLD_PATH)
    scene = world_scene_source()
    assert "note: account.note," in world
    assert "fields: account.fields.map((field) => ({" in world
    assert "wrapCanvasText(context, snapshot.note, 56, 930, 1424, 46, 4);" in scene
    assert "Array.isArray(snapshot.fields) ? snapshot.fields : []" in scene
    assert 'field.verified ? "#9ef7c6" : "#e8e9ff"' in scene


def test_css_keeps_toots_scrollable_under_the_profile():
    css = _source(CSS_PATH)
    assert ".world-mastodon-app" in css
    assert ".world-mastodon-toot-list" in css
    assert "overflow-y: auto;" in css


def test_lookup_endpoint_targets_the_public_forkmesh_account():
    module = _source(MASTODON_PATH)
    assert (
        "https://mastodon.social/api/v1/accounts/lookup?acct=forkmesh" in module
    )
    assert "https://mastodon.social/@forkmesh" in module


def test_account_normalization_bounds_and_flattens_remote_html():
    account = _node(
        f"""
      import {{ normalizeMastodonAccount }} from {json.dumps(MASTODON_PATH.as_uri())};
      process.stdout.write(JSON.stringify(normalizeMastodonAccount({{
        id: "114",
        acct: "forkmesh",
        display_name: "Forkmesh",
        url: "https://mastodon.social/@forkmesh",
        avatar_static: "http://insecure.example/a.png",
        header_static: "https://files.mastodon.social/header.png",
        note: "<p>Community-owned <b>Git forge</b> &amp; mesh</p>",
        followers_count: 157,
        following_count: 1400,
        statuses_count: 79,
        created_at: "2025-07-03T00:00:00.000Z",
        fields: [{{
          name: "Website",
          value: '<a href="https://forkmesh.com">forkmesh.com</a>',
          verified_at: "2025-07-04T00:00:00.000Z",
        }}],
      }})));
    """
    )
    assert account["note"] == "Community-owned Git forge & mesh"
    assert account["avatar"] == ""  # http:// is rejected
    assert account["header"] == "https://files.mastodon.social/header.png"
    assert account["followersCount"] == 157
    assert account["fields"] == [
        {
            "name": "Website",
            "value": "forkmesh.com",
            "url": "https://forkmesh.com/",
            "verified": True,
        }
    ]


def test_status_normalization_unwraps_boosts_and_strips_markup():
    status = _node(
        f"""
      import {{ normalizeMastodonStatus }} from {json.dumps(MASTODON_PATH.as_uri())};
      process.stdout.write(JSON.stringify(normalizeMastodonStatus({{
        id: "9",
        created_at: "2026-07-04T00:00:00.000Z",
        reblog: {{
          content: "<p>Hello <script>alert(1)</script>Fediverse</p>",
          url: "javascript:alert(1)",
          created_at: "2026-07-01T00:00:00.000Z",
          replies_count: 3,
          account: {{ acct: "friend@example.social", display_name: "Friend" }},
          media_attachments: [
            {{ type: "image", preview_url: "https://files.example/p.png", description: "alt" }},
            {{ type: "video", url: "https://files.example/v.mp4" }},
          ],
        }},
      }})));
    """
    )
    assert status["boostedFrom"] == "friend@example.social"
    assert "<" not in status["text"] and "Fediverse" in status["text"]
    assert status["url"] == ""  # non-https status URLs are rejected
    assert status["images"] == [
        {"url": "https://files.example/p.png", "alt": "alt"}
    ]
    assert status["repliesCount"] == 3


def test_deploy_freshness_check_covers_the_mastodon_module():
    deploy_targets = _source(ROOT / "tools" / "deploy_targets.py")
    assert 'if target == "world":' in deploy_targets
    assert '"world/public",' in deploy_targets
