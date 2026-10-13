from __future__ import annotations

import asyncio
import json
import os
import re
import sys

import pytest

APP_ORIGIN = "https://app.forkmesh.com"
RETIRED = "repository_socket_transport_retired"
PHONE_PREVIEW = {
    "ok": True,
    "baseOid": "a" * 40,
    "headOid": "b" * 40,
    "mergeBaseOid": "c" * 40,
    "ahead": 2,
    "behind": 1,
    "clean": False,
    "conflicts": ["src/main.py"],
}


@pytest.fixture(scope="module")
def runtime(staged_src, tmp_path_factory):
    del staged_src
    from forkmesh_host.env import HostConfig
    from forkmesh_host.runtime import HostRuntime
    from forkmesh_host.serve import _migrate

    tmp = tmp_path_factory.mktemp("repo-read-routes")
    host = HostRuntime(HostConfig(data_dir=tmp, environ={
        "FORKMESH_VAR_DATA_KEY": "unit-" + os.urandom(8).hex(),
        "FORKMESH_HOST_OFFLINE": "1",
    }))
    _migrate(host)
    return host


def _get(runtime, path):
    from forkmesh_host import js as _js

    async def main():
        response = await runtime.fetch(_js.Request.new(APP_ORIGIN + path))
        return response.status, str(await response.text())
    return asyncio.run(main())


def _actions(runtime):
    pattern = runtime.entry.REPO_HOST_RE.pattern
    return re.search(r"\(([a-z|-]+)\)\$$", pattern).group(1).split("|")


def test_no_repo_read_action_answers_the_retired_socket(runtime):
    actions = _actions(runtime)
    assert "merge-preview" in actions
    for action in actions:
        status, body = _get(
            runtime, "/api/repo/forkmesh/forkmesh/%s?base=main&head=dev"
            "&ref=&path=" % action)
        if action == "host":
            assert status == 410 and RETIRED in body
        else:
            assert status != 410 and RETIRED not in body, (
                action, status, body[:200])


def test_merge_preview_is_served_by_the_https_mirror_read_path(
        runtime, monkeypatch):
    _get(runtime, "/api/repo/forkmesh/forkmesh/host")
    module = sys.modules["app_runtime"]
    calls = []

    async def not_private(_env, _owner, _repo):
        return False

    async def mirror(env, request, owner, repo, operation, **_kwargs):
        calls.append((owner, repo, operation, str(request.url)))
        return module.json_response(PHONE_PREVIEW, cache_control="no-store")

    monkeypatch.setattr(module, "_repo_is_private", not_private)
    monkeypatch.setattr(module, "_https_mirror_proxy", mirror)
    status, body = _get(
        runtime, "/api/repo/forkmesh/forkmesh/merge-preview?base=" + "a" * 40
        + "&head=feature%2Fx&ref=")
    assert status == 200
    assert calls and calls[0][:3] == ("forkmesh", "forkmesh", "merge-preview")
    preview = json.loads(body)
    assert isinstance(preview["baseOid"], str) and preview["baseOid"]
    assert isinstance(preview["headOid"], str) and preview["headOid"]
    assert preview["clean"] is False
    assert preview["conflicts"] == ["src/main.py"]


def test_merge_preview_query_drops_the_phones_empty_ref(runtime):
    from urllib.parse import urlparse
    query = runtime.entry._https_mirror_request_query(
        urlparse(APP_ORIGIN + "/x?base=" + "a" * 40
                 + "&head=feature%2Fx&ref="), "merge-preview")
    assert query == {"base": "a" * 40, "head": "feature/x"}
