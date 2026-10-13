#!/usr/bin/env python3
"""Privacy-bounded, interoperable World emoji-status contracts."""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
DATA_MODULE = WORLD / "world-data.js"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (WORLD / "world.css").read_text(encoding="utf-8")


def run_data_module(source):
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            source,
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_picker_is_broad_and_free_form_validation_accepts_unicode_sequences():
    script = f"""
      import {{
        WORLD_EMOJI_CATEGORIES,
        normalizeWorldEmoji,
        normalizeWorldStatus,
        normalizeWorldStatusNote
      }} from {json.dumps(DATA_MODULE.as_uri())};
      const picker = WORLD_EMOJI_CATEGORIES.flatMap(
        (category) => category.emoji
      );
      process.stdout.write(JSON.stringify({{
        categories: WORLD_EMOJI_CATEGORIES.map((category) => category.id),
        count: picker.length,
        invalidPicker: picker.filter((emoji) => !normalizeWorldEmoji(emoji)),
        valid: [
          "🧑🏽‍💻", "👨‍👩‍👧‍👦", "🏳️‍🌈", "🇺🇸", "1️⃣", "❤️"
        ].map(normalizeWorldEmoji),
        invalid: [
          "hello", "😀😀", "<script>", "https://example.test", "😀 private"
        ].map(normalizeWorldEmoji),
        notes: [
          "coding", "on-call", "débogage", "コード", "l’équipe"
        ].map(normalizeWorldStatusNote),
        badNotes: [
          "two words", "https://example.test", "<script>", "/private"
        ].map(normalizeWorldStatusNote),
        noteWithoutEmoji: normalizeWorldStatus("", "secret")
      }}));
    """
    value = run_data_module(script)
    assert value["categories"] == [
        "faces",
        "gestures",
        "nature",
        "food",
        "activity",
        "travel",
        "objects",
        "symbols",
        "flags",
    ]
    assert value["count"] >= 250
    assert value["invalidPicker"] == []
    assert all(value["valid"])
    assert value["invalid"] == [""] * 5
    assert all(value["notes"])
    assert value["badNotes"] == [""] * 4
    assert value["noteWithoutEmoji"] == {"emoji": "", "note": ""}


def test_settings_persist_only_normalized_status_and_publish_on_profile_frames():
    for contract in (
        'statusEmoji: ""',
        'statusNote: ""',
        "normalizeWorldStatus(",
        "data-world-status-emoji",
        "data-world-status-note",
        "data-world-status-emoji-choice",
        "data-world-emoji-category-panel",
        "data-world-status-clear",
        "commitWorldStatus(emojiValue, noteValue)",
        "next.emoji !== this.settings.statusEmoji",
        "next.note !== this.settings.statusNote",
        "this.saveSettings()",
        "PRESENCE_PROFILE_DEBOUNCE_MS",
        "statusEmoji: publicStatus.emoji",
        "statusNote: publicStatus.note",
    ):
        assert contract in APP
    assert "Any single Unicode emoji sequence is accepted" in APP
    assert "Only those two bounded values are public" in APP

    movement = APP[
        APP.index('    } else if (message.type === "move") {'):
        APP.index('    } else if (message.type === "ping") {')
    ]
    assert "statusEmoji" not in movement
    assert "statusNote" not in movement
    status_commit = APP[
        APP.index("  commitWorldStatus(emojiValue, noteValue) {"):
        APP.index("\n  updateWorldStatusUI()", APP.index(
            "  commitWorldStatus(emojiValue, noteValue) {"
        ))
    ]
    assert "commitPublicSettings()" in status_commit
    assert "sendPresenceNow" not in status_commit
    assert 'target="_blank"' not in status_commit
    assert "location.assign" not in status_commit


def test_scene_keeps_status_in_accessible_labels_without_duplicate_overhead_banner():
    for contract in (
        "function syncAvatarStatus",
        "avatar.userData.emojiStatusSprite = null;",
        "function updatePlayerLabel",
        '"world-player-label-status"',
        '"aria-label"',
        "statusEmoji: remote.statusEmoji",
        "statusNote: remote.statusNote",
    ):
        assert contract in SCENE
    assert ".world-status-editor" in CSS
    assert ".world-emoji-grid" in CSS
    assert ".world-player-label-status" in CSS
    assert "@media (max-width: 480px)" in CSS
    assert "grid-template-columns: repeat(5, minmax(34px, 1fr))" in CSS
