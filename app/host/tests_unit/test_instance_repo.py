import os
import pwd
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forkmesh_host import instance_repo as repo  # noqa: E402


def args(tmp_path, **changes):
    values = {"owner": "Alice", "organization": "acme", "name": "Bolt",
              "description": "  An   import ", "visibility": "public",
              "source": str(tmp_path), "hosted_root": "/var/lib/x/hosted",
              "force": False, "no_hooks": False, "origin": "",
              "app_user": None, "git_user": None}
    values.update(changes)
    return SimpleNamespace(**values)


def test_spec_is_normalized_and_bad_values_are_refused(tmp_path):
    spec = repo.read_spec(args(tmp_path))
    assert spec["owner"] == "alice" and spec["name"] == "Bolt"
    assert spec["description"] == "An import" and spec["hooks"] is True
    assert repo.read_spec(args(tmp_path, organization=""))["organization"] == ""
    for bad in ({"owner": "bad name"}, {"organization": "-x"},
                {"name": ".hidden"}, {"name": "x.git"}, {"name": "a/b"},
                {"source": str(tmp_path / "missing")},
                {"hosted_root": "relative"}):
        with pytest.raises(repo.RepoError):
            repo.read_spec(args(tmp_path, **bad))


@pytest.mark.skipif(os.geteuid() == 0, reason="checks the non-root path")
def test_switching_users_needs_root(tmp_path):
    me = pwd.getpwuid(os.geteuid()).pw_name
    assert repo._users(args(tmp_path)) == ["", ""]
    assert repo._users(args(tmp_path, app_user=me, git_user="")) == ["", ""]
    with pytest.raises(repo.RepoError):
        repo._users(args(tmp_path, app_user="root"))


def test_forked_phase_returns_its_value_or_its_error():
    me = pwd.getpwuid(os.geteuid()).pw_name
    assert repo._forked(me, lambda: {"pid": os.getpid()})["pid"] != os.getpid()

    def fail():
        raise repo.RepoError("broken")
    with pytest.raises(repo.RepoError, match="broken"):
        repo._forked(me, fail)
