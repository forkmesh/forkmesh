"""Contracts for deleting an unverified account from its World avatar pin."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _function(source, signature):
    start = source.index(signature)
    next_function = source.find("\nasync def ", start + len(signature))
    if next_function < 0:
        next_function = len(source)
    return source[start:next_function]


def test_red_email_pin_is_the_admin_only_delete_gesture():
    click = SCENE[SCENE.index("if (avatarSelection) {"):]
    assert "hit.object === avatarSelection.avatar?.userData?.verifiedPin" in click
    assert "identity.isAdmin === true" in click
    assert "selectedMember.self !== true" in click
    assert "selectedMember.emailVerified !== true" in click
    assert 'selectedMember.accountStatus.toLowerCase() !== "guest"' in click
    assert "onUnverifiedAvatarDelete(selectedMember)" in click


def test_client_deletes_first_then_animates_and_removes_stale_roster_rows():
    start = WORLD.index("async deleteUnverifiedWorldMember")
    method = WORLD[
        start:
        WORLD.index("openWorldMemberDetail", start)
    ]
    assert "this.identity?.isAdmin !== true" in method
    assert "!validWorldSession()" in method
    assert 'name === ownName' in method
    assert '{ method: "DELETE", timeout: 20_000 }' in method
    assert "/api/accounts/admin-unverified/" in method
    assert method.index("await this.postJSON(") < method.index(
        "animateUnverifiedAvatarDeletion"
    )
    assert "this.memberDirectory = this.memberDirectory.filter" in method
    assert "this.responseCache.clear()" in method
    assert "requestAnimationFrame(frame)" in SCENE
    assert 'forkmesh-admin-delete-burst' in SCENE


def test_worker_reauthorizes_and_hard_deletes_only_unverified_non_admin_users():
    handler = _function(ENTRY, "async def _admin_delete_unverified_account")
    assert '_account_session_record(env, request, data)' in handler
    assert '_has_role(env, actor, "platform_administrator")' in handler
    assert '_account_kind(target_rec) != "user"' in handler
    assert 'target_rec.get("email_verified") is True' in handler
    assert "if await _is_admin(env, target)" in handler
    assert "_delete_account_namespace(env, target_bi, target_rec)" in handler
    assert '"accountDeleted": True' in handler
    assert '"admin.unverified_account_delete"' in handler
    assert 'admin_unverified_prefix = "/api/accounts/admin-unverified/"' in ENTRY
    assert 'and method == "DELETE"' in ENTRY[
        ENTRY.index("admin_unverified_prefix"):
        ENTRY.index("admin_unverified_prefix") + 500
    ]


def test_complete_account_cleanup_purges_identity_rows_and_public_caches():
    cleanup = _function(ENTRY, "async def _delete_account_namespace")
    for table in (
        "account_devices",
        "account_ssh_keys",
        "account_sessions",
        "repo_stars",
        "role_grants",
        "world_inactive_presence",
        "world_user_activity",
        "badge_awards",
        "org_team_collaborators",
        "users",
        "nodes",
    ):
        assert table in cleanup
    assert "ACCOUNT_LOOKUP_CACHE_PREFIX" in cleanup
    assert "USERS_DIRECTORY_CACHE_KEY" in cleanup
    assert "CHAT_ACTIVITY_CACHE_KEY" in cleanup
