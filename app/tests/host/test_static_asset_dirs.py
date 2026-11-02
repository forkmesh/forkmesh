"""Asset folders that two-segment repository routes must not swallow."""

from pathlib import Path

from forkmesh_host.asgi import WorkerFirstRules

APP = Path(__file__).resolve().parents[2]
RULES = WorkerFirstRules.from_toml(APP / "wrangler.toml")


def test_chat_and_call_modules_are_served_as_assets():
    # chat.js imports ./fmm/fmm-local.js statically: a 404 there stops the
    # whole chat page before it reads the session or the channel list.
    files = sorted((APP / "public" / "fmm").glob("*.js"))
    assert any(path.name == "fmm-local.js" for path in files)
    for path in files:
        assert not RULES.worker_first("/fmm/" + path.name), path.name


def test_instance_theme_files_are_served_as_assets():
    for path in sorted((APP / "public" / "instance-themes").iterdir()):
        assert not RULES.worker_first("/instance-themes/" + path.name), path.name


def test_repository_addresses_still_reach_the_worker():
    assert RULES.worker_first("/owasp-blt/BLT")
