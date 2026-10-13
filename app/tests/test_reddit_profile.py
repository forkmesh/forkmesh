import importlib.util
import json
import re
import sys
from pathlib import Path

import pytest


PROJECT = Path(__file__).resolve().parents[2]
TOOL = PROJECT / "tools" / "reddit_profile.py"
PROFILE = PROJECT / "tools" / "reddit_profile.json"
CONFIG = PROJECT / ".forkmesh" / "teammates" / "reddit-agent.json"
CHARTER = PROJECT / "docs" / "community" / "reddit.md"


def _tool():
    sys.path.insert(0, str(TOOL.parent))
    spec = importlib.util.spec_from_file_location("reddit_profile", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeClient:
    def __init__(self, signed_in="forkmesh", current=None, after=None, can_write=True):
        self.can_write = can_write
        self.mode = "oauth"
        self._signed_in = signed_in
        self._current = current or {"title": "Old", "public_description": "old about"}
        self._after = after
        self.saved = []

    def signed_in_as(self):
        return self._signed_in

    def current(self, user):
        return dict(self._after if self.saved and self._after is not None else self._current)

    def settings(self, user):
        return {"subreddit_id": "t5_abc", "title": "Old", "public_description": "old about", "default_set": True,
                "header_hover_text": "", "language": "en", "content_options": "any", "subreddit_type": "user",
                "over_18": False, "description": None, "free_form_reports": True}

    def save(self, form):
        self.saved.append(form)


def _profile(tmp_path, **fields):
    path = tmp_path / "profile.json"
    path.write_text(json.dumps({"user": "forkmesh", "profile": fields}), encoding="utf-8")
    return path


def test_checked_in_profile_is_valid_and_names_the_forkmesh_account():
    profile = _tool().load_profile(PROFILE)

    assert profile["user"] == "forkmesh"
    assert set(profile["profile"]) == {"title", "public_description"}


def test_profile_file_is_limited_to_display_name_and_about(tmp_path):
    tool = _tool()

    with pytest.raises(ValueError, match="only title, public_description"):
        tool.load_profile(_profile(tmp_path, title="ForkMesh", icon_img="x.png"))
    with pytest.raises(ValueError, match="Reddit allows 30"):
        tool.load_profile(_profile(tmp_path, title="x" * 31))
    with pytest.raises(ValueError, match="Reddit allows 200"):
        tool.load_profile(_profile(tmp_path, public_description="x" * 201))
    with pytest.raises(ValueError, match="non-empty"):
        tool.load_profile(_profile(tmp_path, title="  "))
    assert tool.load_profile(_profile(tmp_path, title="ForkMesh"))["profile"] == {"title": "ForkMesh"}


def test_changes_list_only_fields_that_differ():
    diff = _tool().changes({"title": "ForkMesh", "public_description": "same"}, {"title": None, "public_description": "same"})

    assert diff == {"title": {"from": None, "to": "ForkMesh"}}


def test_site_admin_form_keeps_every_other_setting():
    tool = _tool()
    form = tool.site_admin_form(FakeClient().settings("forkmesh"), {"title": "ForkMesh"})

    assert form["sr"] == "t5_abc"
    assert form["title"] == "ForkMesh"
    assert form["public_description"] == "old about"
    assert form["allow_top"] == "true" and form["over_18"] == "false"
    assert form["lang"] == "en" and form["link_type"] == "any" and form["type"] == "user"
    assert form["header-title"] == ""
    assert form["api_type"] == "json"
    assert "description" not in form and "subreddit_id" not in form
    with pytest.raises(RuntimeError):
        tool.site_admin_form({"title": "x"}, {"title": "y"})


def test_apply_lists_changes_and_saves_only_with_yes(tmp_path, monkeypatch, capsys):
    tool = _tool()
    path = _profile(tmp_path, title="ForkMesh", public_description="new about")
    dry = FakeClient()
    monkeypatch.setattr(tool, "_client", lambda: dry)

    assert tool.main(["--profile", str(path), "apply"]) == 0
    assert dry.saved == []
    assert "dry run" in capsys.readouterr().out

    live = FakeClient(after={"title": "ForkMesh", "public_description": "new about"})
    monkeypatch.setattr(tool, "_client", lambda: live)
    assert tool.main(["--profile", str(path), "apply", "--yes"]) == 0
    assert len(live.saved) == 1
    assert live.saved[0]["title"] == "ForkMesh" and live.saved[0]["public_description"] == "new about"
    assert "saved" in capsys.readouterr().out


def test_apply_refuses_the_wrong_account_missing_credentials_and_unconfirmed_saves(tmp_path, monkeypatch, capsys):
    tool = _tool()
    path = _profile(tmp_path, title="ForkMesh")

    other = FakeClient(signed_in="someone_else")
    monkeypatch.setattr(tool, "_client", lambda: other)
    assert tool.main(["--profile", str(path), "apply", "--yes"]) == 2
    assert other.saved == []

    monkeypatch.setattr(tool, "_client", lambda: FakeClient(can_write=False))
    assert tool.main(["--profile", str(path), "apply", "--yes"]) == 2
    assert "REDDIT_REFRESH_TOKEN" in capsys.readouterr().err

    stuck = FakeClient(after={"title": "Old", "public_description": "old about"})
    monkeypatch.setattr(tool, "_client", lambda: stuck)
    assert tool.main(["--profile", str(path), "apply", "--yes"]) == 1


def test_feed_subtitle_is_the_about_text():
    raw = ('<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><title>overview for forkmesh</title>'
           "<subtitle>ForkMesh is an open world.</subtitle></feed>")

    assert _tool().feed_profile(raw) == {"title": None, "public_description": "ForkMesh is an open world."}


def test_profile_tool_only_writes_the_profile_settings():
    source = TOOL.read_text(encoding="utf-8")

    for endpoint in ("/api/submit", "/api/comment", "/api/vote", "/api/compose", "/api/del", "/api/editusertext",
                     "/message/", "upload_sr_img", "/api/v1/me/prefs", "/api/friend", "/api/subscribe"):
        assert endpoint not in source, endpoint
    assert source.count('method="POST"') == 2
    assert re.search(r'/api/site_admin",\s*data=', source)
    assert not re.search(r"print\([^)]*(_password|_refresh_token|_client_secret|found\[)", source)


def test_grant_prefers_a_refresh_token_and_falls_back_to_app_only():
    tool = _tool()

    assert tool.ProfileClient("id", "secret", "refresh")._grant()["grant_type"] == "refresh_token"
    assert tool.ProfileClient("id", "secret", "", "forkmesh", "pw")._grant()["grant_type"] == "password"
    assert tool.ProfileClient("id", "secret")._grant()["grant_type"] == "client_credentials"
    assert not tool.ProfileClient("id", "secret").can_write
    assert tool.ProfileClient("id", "secret", "refresh").can_write
    assert not tool.ProfileClient().can_write


def test_reddit_agent_may_update_only_the_profile():
    instructions = json.loads(CONFIG.read_text(encoding="utf-8"))["instructions"]
    charter = CHARTER.read_text(encoding="utf-8")

    assert "tools/reddit_profile.py" in instructions
    assert "tools/reddit_profile.json" in instructions
    assert "Never post, comment, vote, send messages or chat" in instructions
    assert "## The u/forkmesh profile" in charter
