import ast
import re
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))
for extra in (ROOT / "tools", APP / "host"):
    if str(extra) not in sys.path:
        sys.path.insert(0, str(extra))

import edge_routing  # noqa: E402
import mirror_gateway  # noqa: E402
import urls  # noqa: E402
from forkmesh_host import repo_fastpath  # noqa: E402

RETIRED_ACTIONS = {
    "host": "retired repository socket transport; answers the cacheable 410",
}


def _module_constant(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == name
                for target in node.targets):
            value = node.value
            if isinstance(value, ast.Call):
                value = value.args[0]
            return frozenset(ast.literal_eval(value))
    raise AssertionError("%s not found in %s" % (name, path))


def _regex_actions():
    match = re.search(r"\(([a-z|-]+)\)\$$", urls.REPO_HOST_RE.pattern)
    assert match, urls.REPO_HOST_RE.pattern
    return frozenset(match.group(1).split("|"))


SERVED = _module_constant(SRC / "app_runtime.py", "REPO_HOST_READ_ACTIONS")


def test_every_repo_route_action_is_served_or_explicitly_retired():
    actions = _regex_actions()
    assert not SERVED & set(RETIRED_ACTIONS)
    assert actions == SERVED | set(RETIRED_ACTIONS), (
        "unrouted: %s, unmatched: %s" % (
            sorted(actions - SERVED - set(RETIRED_ACTIONS)),
            sorted((SERVED | set(RETIRED_ACTIONS)) - actions)))


def test_merge_preview_is_a_served_read_not_the_retired_socket():
    assert "merge-preview" in SERVED
    assert urls.REPO_HOST_RE.match(
        "/api/repo/forkmesh/forkmesh/merge-preview")


def test_route_dispatch_uses_the_shared_action_set():
    source = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    block = source.split("host_match = REPO_HOST_RE.match", 1)[1].split(
        "return _repository_socket_retired()", 1)[0]
    assert "action in REPO_HOST_READ_ACTIONS" in block
    assert "_https_mirror_proxy" in block


CARRIED = {
    mirror_gateway.PULSE_OPERATION: mirror_gateway.PULSE_CARRIER_OPERATION,
    mirror_gateway.ISSUE_INDEX_OPERATION:
        mirror_gateway.ISSUE_INDEX_CARRIER_OPERATION,
    mirror_gateway.ISSUE_SEARCH_OPERATION:
        mirror_gateway.ISSUE_INDEX_CARRIER_OPERATION,
}


def test_every_served_action_is_routable_to_a_node():
    advertised = (SERVED - set(CARRIED)) | set(CARRIED.values())
    assert SERVED <= edge_routing.PUBLIC_OPERATIONS
    assert advertised <= mirror_gateway.DEFAULT_PUBLIC_OPERATIONS
    assert SERVED <= mirror_gateway.DIRECT_PUBLIC_READ_OPERATIONS
    go = (ROOT / "server" / "merge.go").read_text(encoding="utf-8")
    block = go.split("defaultGatewayOperations = []string{", 1)[1].split(
        "}", 1)[0]
    assert advertised <= frozenset(re.findall(r'"([a-z-]+)"', block))


def test_carried_reads_are_never_advertised_on_their_own():
    for carried in CARRIED:
        assert carried not in mirror_gateway.PUBLIC_OPERATIONS
        assert carried not in mirror_gateway.DEFAULT_PUBLIC_OPERATIONS


def test_private_hosted_and_host_fast_path_cover_the_same_reads():
    hosted = _module_constant(
        SRC / "hosted_git.py", "HOSTED_API_READ_OPERATIONS")
    assert hosted == SERVED
    assert repo_fastpath.READ_OPERATIONS == SERVED


def test_fast_path_builds_the_phone_merge_preview_query():
    sha = "a" * 40
    pairs = repo_fastpath.gateway_query(
        "merge-preview", "base=%s&head=feature%%2Fx&ref=" % sha)
    assert pairs == [("base", sha), ("head", "feature/x")]
    assert repo_fastpath.cache_control("merge-preview", pairs) == (
        repo_fastpath.REVALIDATE)
    pinned = [("base", sha), ("head", "b" * 40)]
    assert repo_fastpath.cache_control("merge-preview", pinned) == (
        repo_fastpath.IMMUTABLE)
