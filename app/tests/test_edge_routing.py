"""Direct and Tunnel-fallback HTTPS mirror routing policy."""

import hashlib
import json
import sys
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import edge_routing as routing  # noqa: E402


def _record(node="mirror-a", **extra):
    return {
        "node": node,
        "baseUrl": "https://mirror-a.example.net/forkmesh",
        "publicKey": "ed25519-public",
        "signature": "owner-signature",
        "issuedAt": 1,
        "checkedAt": 100_000,
        "latencyMs": 32,
        "healthy": True,
        "integrity": "ok",
        "region": "US",
        **extra,
    }


def test_endpoint_urls_are_https_public_and_credential_free():
    assert routing.normalize_base_url(
        "https://Mirror.Example.NET/forkmesh/",
    ) == "https://mirror.example.net/forkmesh"
    for value in (
        "http://mirror.example.net",
        "https://user:pass@mirror.example.net",
        "https://localhost",
        "https://127.0.0.1",
        "https://10.0.0.1",
        "https://worker.internal",
        "https://mirror.example.net:8443",
        "https://mirror.example.net/?token=secret",
    ):
        assert routing.normalize_base_url(value) == ""


def test_registration_and_health_challenge_bind_identity_endpoint_and_time():
    message = routing.registration_message(
        "mirror-a", "https://mirror.example.net", "pub", 123)
    assert message == (
        "forkmesh-https-endpoint-v1\nmirror-a\n"
        "https://mirror.example.net\npub\n123"
    )
    direct = routing.registration_message(
        "mirror-a", "https://tunnel.example.net", "pub", 123,
        "https://git.example.net")
    assert direct == (
        "forkmesh-https-endpoint-v2\nmirror-a\n"
        "https://tunnel.example.net\nhttps://git.example.net\npub\n123"
    )
    assert routing.health_challenge("mirror-a", "0123456789abcdef", 456) == (
        "forkmesh-https-health-v1\nmirror-a\n0123456789abcdef\n456"
    )
    operations = ["blob", "git-info-refs", "git-upload-pack", "raw", "tree"]
    digest = routing.operations_sha256(operations)
    refs = "1" * 64
    assert routing.repository_health_challenge(
        "mirror-a", "0123456789abcdef", 456, "forkmesh", "forkmesh",
        True, "ok", refs, digest) == (
            "forkmesh-https-health-repository-v1\nmirror-a\n"
            "0123456789abcdef\n456\nforkmesh\nforkmesh\n1\nok\n"
            + refs + "\n" + digest
        )


def test_tunnel_manifest_shape_and_signed_payload_are_bound_to_origin():
    origin = "https://mirror.example.net"
    public_key = "A" * 43
    manifest = {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-endpoint",
        "generatedAt": "2026-07-23T10:00:00Z",
        "node": {"name": "mirror-a", "publicKey": public_key},
        "endpoint": {
            "origin": origin,
            "healthUrl": origin + "/health",
            "manifestUrl": origin + "/forkmesh-mirror.json",
            "repositoryUrlTemplate": (
                origin
                + "/v1/repositories/{owner}/{repository}/{operation}"),
            "transport": "direct-https",
            "mainProxyMode": "masked",
        },
        "dns": {
            "recordName": "mirror.example.net",
            "recordType": "CNAME",
            "proxied": True,
            "target": "tunnel-id.cfargotunnel.com",
        },
        "edge": {
            "provider": "cloudflare",
            "kind": "cloudflare-tunnel",
            "tunnelId": "tunnel-id",
            "originExposure": "loopback-only",
        },
        "storage": {
            "repositoryBytesInD1": False,
            "repositoryByteOwner": "independent-mirror-host",
            "d1Purpose": ["service-discovery"],
        },
    }
    payload = json.dumps(
        manifest, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False).encode()
    manifest["signature"] = {
        "algorithm": "Ed25519",
        "encoding": "base64url-no-padding",
        "canonicalization": "forkmesh-json-sort-v1",
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "value": "B" * 86,
    }
    checked = routing.validate_tunnel_manifest(
        manifest, origin, "mirror-a", public_key)
    assert checked["payload"] == payload
    manifest["endpoint"]["origin"] = "https://attacker.example"
    assert routing.validate_tunnel_manifest(
        manifest, origin, "mirror-a", public_key) is None


def test_cloudflare_proxy_dns_requires_independent_official_ranges():
    cloudflare_ranges = (
        "173.245.48.0/20 104.16.0.0/13",
        "2606:4700::/32",
    )
    assert routing.cloudflare_proxied_dns_answers(
        [
            {
                "Status": 0,
                "Answer": [
                    {"name": "mirror.example.", "type": 1,
                     "data": "104.16.10.20"},
                ],
            },
            {
                "Status": 0,
                "Answer": [
                    {"name": "mirror.example.", "type": 28,
                     "data": "2606:4700::1234"},
                ],
            },
        ],
        cloudflare_ranges,
    )
    assert not routing.cloudflare_proxied_dns_answers(
        [
            {"Status": 0, "Answer": [
                {"type": 1, "data": "203.0.113.10"}]},
            {"Status": 0, "Answer": []},
        ],
        cloudflare_ranges,
    )
    assert not routing.cloudflare_proxied_dns_answers(
        [{"Status": 2, "Answer": []}], cloudflare_ranges)
    assert not routing.cloudflare_proxied_dns_answers(
        [{"Status": 0, "Answer": [{"type": 5, "data": "target.example."}]}],
        cloudflare_ranges,
    )
    assert not routing.cloudflare_proxied_dns_answers(
        [{"Status": 0, "Answer": [{"type": 1, "data": "104.16.1.1"}]}],
        ("not-a-cidr",),
    )


def test_direct_dns_requires_only_global_addresses():
    assert routing.public_dns_answers([
        {"Status": 0, "Answer": [{"type": 1, "data": "8.8.8.8"}]},
        {"Status": 0, "Answer": []},
    ])
    assert not routing.public_dns_answers([
        {"Status": 0, "Answer": [{"type": 1, "data": "10.0.0.8"}]},
        {"Status": 0, "Answer": []},
    ])
    assert not routing.public_dns_answers([
        {"Status": 0, "Answer": [{"type": 5, "data": "target.example."}]},
    ])


def test_direct_manifest_and_smart_http_urls_are_origin_bound():
    origin = "https://git.example.net"
    public_key = "A" * 43
    manifest = {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-endpoint",
        "generatedAt": "2026-08-17T10:00:00Z",
        "node": {"name": "mirror-a", "publicKey": public_key},
        "endpoint": {
            "origin": origin,
            "healthUrl": origin + "/health",
            "manifestUrl": origin + "/forkmesh-mirror.json",
            "repositoryUrlTemplate": origin + "/{owner}/{repository}.git",
            "transport": "direct-https",
            "mainProxyMode": "direct",
        },
        "dns": {
            "recordName": "git.example.net",
            "recordType": "A/AAAA",
            "proxied": False,
        },
        "edge": {
            "provider": "independent",
            "kind": "public-https-origin",
            "originExposure": "public-https",
        },
        "storage": {
            "repositoryBytesInD1": False,
            "repositoryByteOwner": "independent-mirror-host",
            "d1Purpose": ["service-discovery"],
        },
    }
    payload = json.dumps(
        manifest, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False).encode()
    manifest["signature"] = {
        "algorithm": "Ed25519",
        "encoding": "base64url-no-padding",
        "canonicalization": "forkmesh-json-sort-v1",
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "value": "B" * 86,
    }
    assert routing.validate_direct_manifest(
        manifest, origin, "mirror-a", public_key)["payload"] == payload
    assert routing.direct_repository_url(
        origin, "alice", "project") == (
            "https://git.example.net/alice/project.git")
    assert routing.direct_repository_page_url(
        origin, "alice", "project") == (
            "https://git.example.net/alice/project")
    assert routing.direct_git_target_url(
        origin, "alice", "project", "git-info-refs",
        {"service": "git-upload-pack"}) == (
            "https://git.example.net/alice/project.git/info/refs"
            "?service=git-upload-pack")


def test_selection_requires_fresh_health_integrity_and_no_abuse_flag():
    now = 100_500
    direct = _record(mirrorUrl="https://git.example.net")
    assert routing.endpoint_eligible(direct, now)
    assert routing.select_endpoints([direct], now)[0]["mirrorUrl"] == (
        "https://git.example.net")
    assert not routing.endpoint_eligible(_record(healthy=False), now)
    assert not routing.endpoint_eligible(_record(integrity="rejected"), now)
    assert not routing.endpoint_eligible(_record(abuseBlocked=True), now)
    assert not routing.endpoint_eligible(
        _record(checkedAt=now - routing.ENDPOINT_STALE_MS - 1), now)


def test_selection_prefers_region_latency_and_has_bounded_failover():
    records = [
        _record("mirror-a", region="EU", latencyMs=20),
        _record(
            "mirror-b", baseUrl="https://mirror-b.example.net",
            region="US", latencyMs=40),
        _record(
            "mirror-c", baseUrl="https://mirror-c.example.net",
            region="US", latencyMs=10),
    ]
    selected = routing.select_endpoints(
        records, 100_500, preferred_region="US")
    assert [record["node"] for record in selected] == [
        "mirror-c", "mirror-b", "mirror-a",
    ]
    assert len(selected) <= routing.MAX_FAILOVER_ATTEMPTS


def test_selection_cursor_round_robins_every_eligible_endpoint():
    records = [
        _record(
            "mirror-%s" % suffix,
            baseUrl="https://mirror-%s.example.net" % suffix,
            latencyMs=index * 10,
        )
        for index, suffix in enumerate(("a", "b", "c", "d"), start=1)
    ]
    selected_first = [
        routing.select_endpoints(records, 100_500, cursor=cursor)[0]["node"]
        for cursor in range(len(records))
    ]
    assert selected_first == [
        "mirror-a", "mirror-b", "mirror-c", "mirror-d",
    ]
    assert routing.select_endpoints(
        records, 100_500, cursor=len(records)
    )[0]["node"] == "mirror-a"


def test_bounded_window_holds_a_pinned_or_capability_named_endpoint():
    # The window is a read rotation. A clone pin, an in-flight merge job's
    # recorded node, and the mirrors that advertise the merge executor are
    # named endpoints: rotating them out of a group larger than the window
    # silently broke the exact pin that named them, which is what an owner
    # saw as "no online mirror can merge this pull request".
    now = 100_500
    records = [
        _record(
            "mirror-%s" % suffix,
            baseUrl="https://mirror-%s.example.net" % suffix,
            latencyMs=index * 10,
        )
        for index, suffix in enumerate(("a", "b", "c", "d", "e", "f"), start=1)
    ]
    assert len(records) > routing.MAX_FAILOVER_ATTEMPTS
    assert "mirror-f" not in [
        record["node"] for record in routing.select_endpoints(records, now)
    ]
    for cursor in range(len(records)):
        selected = [
            record["node"] for record in routing.select_endpoints(
                records, now, cursor=cursor, prefer=("Mirror-F",))
        ]
        assert selected[0] == "mirror-f"
        assert len(selected) <= routing.MAX_FAILOVER_ATTEMPTS
        assert len(set(selected)) == len(selected)
    # Naming an endpoint never admits one: eligibility still decides.
    stale = _record(
        "mirror-stale",
        baseUrl="https://mirror-stale.example.net",
        checkedAt=now - routing.ENDPOINT_STALE_MS - 1,
    )
    assert "mirror-stale" not in [
        record["node"] for record in routing.select_endpoints(
            records + [stale], now, prefer=("mirror-stale",))
    ]
    # Naming several endpoints keeps a ring among them, so a group where
    # every mirror runs the executor still spreads the merges.
    assert [
        routing.select_endpoints(
            records, now, cursor=cursor, prefer=("mirror-e", "mirror-f"),
        )[0]["node"]
        for cursor in range(2)
    ] == ["mirror-e", "mirror-f"]
    # Unnamed selection keeps rotating exactly as before.
    assert [
        record["node"] for record in routing.select_endpoints(
            records, now, cursor=1)
    ] == ["mirror-b", "mirror-c", "mirror-d", "mirror-e"]


def test_two_tier_cap_keeps_the_named_endpoint_from_the_failover_tier():
    # Current-generation and historical rings are concatenated, so the final
    # cap is the second place a named endpoint could fall out of the window.
    now = 100_500
    current = [
        _record(
            "mirror-%s" % suffix,
            baseUrl="https://mirror-%s.example.net" % suffix,
            latencyMs=index * 10,
        )
        for index, suffix in enumerate(("a", "b", "c", "d"), start=1)
    ]
    historical = [_record(
        "mirror-old", baseUrl="https://mirror-old.example.net", latencyMs=90)]
    combined = (
        routing.select_endpoints(current, now)
        + routing.select_endpoints(historical, now))
    assert [
        record["node"] for record in routing.cap_failover(combined)
    ] == ["mirror-a", "mirror-b", "mirror-c", "mirror-d"]
    capped = routing.cap_failover(combined, prefer=("mirror-old",))
    assert capped[0]["node"] == "mirror-old"
    assert len(capped) == routing.MAX_FAILOVER_ATTEMPTS


def test_merge_executor_operations_are_the_owner_authorized_mutations():
    assert routing.MERGE_EXECUTOR_OPERATIONS == {
        "merge-pull", "update-pull-branch", "merge-agent-branch",
        "delete-agent-branch"}
    assert routing.MERGE_EXECUTOR_OPERATIONS <= routing.PUBLIC_OPERATIONS


def _refs_lag(**overrides):
    details = {
        "reason": "refs_mismatch",
        "signed": True,
        "refsSha256": "b" * 64,
        "operations": ["blob", "merge-pull", "update-pull-branch",
                       "merge-agent-branch", "delete-agent-branch",
                       "git-receive-pack"],
    }
    details.update(overrides)
    return details


def test_signed_refs_lag_names_only_a_signed_pin_lag():
    assert routing.signed_refs_lag("merge-pull", _refs_lag()) == "b" * 64
    assert routing.signed_refs_lag(
        "merge-pull", _refs_lag(refsSha256="B" * 64)) == "b" * 64
    for broken in (
        _refs_lag(signed=False),
        _refs_lag(reason="repository_signature_invalid"),
        _refs_lag(reason="mirror_unhealthy"),
        _refs_lag(reason="git_operation_unavailable"),
        _refs_lag(refsSha256="nope"),
        _refs_lag(operations=["blob"]),
        _refs_lag(refsSha256=hashlib.sha256(b"").hexdigest()),
    ):
        assert routing.signed_refs_lag("merge-pull", broken) == ""
    # Two gateways mid-reload both sign the empty digest; that is no quorum.
    empty = _refs_lag(refsSha256=hashlib.sha256(b"").hexdigest())
    seen = {}
    for node in ("mirror36", "mirror37"):
        assert not routing.mismatch_is_servable("git-upload-pack", node, empty, seen)
    assert routing.signed_refs_lag("merge-pull", None) == ""


def test_owner_pull_mutations_ride_the_two_node_refs_lag_quorum():
    # forkmesh/forkmesh#268: every mirror advertised merge-pull and was
    # healthy, but the pin set had not yet learned the digest they signed
    # after the last push, so the owner's web merge found no mirror at all.
    for operation in sorted(routing.MERGE_EXECUTOR_OPERATIONS):
        seen = {}
        assert not routing.mismatch_is_servable(
            operation, "mirror35", _refs_lag(), seen)
        assert not routing.mismatch_is_servable(
            operation, "mirror35", _refs_lag(), seen)
        assert not routing.mismatch_is_servable(
            operation, "mirror37", _refs_lag(refsSha256="c" * 64), seen)
        assert routing.mismatch_is_servable(
            operation, "mirror36", _refs_lag(), seen)
    # A push still never rides a lagging proof.
    seen = {}
    for node in ("mirror35", "mirror36"):
        assert not routing.mismatch_is_servable(
            "git-receive-pack", node, _refs_lag(), seen)


def test_merge_unavailable_reason_blames_the_build_only_when_it_is_the_build():
    reason = routing.merge_unavailable_reason
    assert reason(0, set()) == "no_online_mirror"
    assert reason(3, {"git_operation_unavailable"}) == "no_merge_executor"
    assert reason(3, {"refs_mismatch"}) == "refs_catching_up"
    assert reason(
        3, {"refs_mismatch", "git_operation_unavailable"}) == "refs_catching_up"
    assert reason(2, {"mirror_unhealthy"}) == "mirror_unverified"
    assert reason(
        2, {"git_operation_unavailable", None}) == "mirror_unverified"


def test_equal_signed_refs_expand_current_round_robin_set():
    refs = "a" * 64
    records = [
        routing.normalize_endpoint_record(_record("mirror-a", refsSha256=refs)),
        routing.normalize_endpoint_record(_record(
            "mirror-b",
            baseUrl="https://mirror-b.example.net",
            refsSha256=refs,
        )),
        routing.normalize_endpoint_record(_record(
            "mirror-c",
            baseUrl="https://mirror-c.example.net",
            refsSha256="b" * 64,
        )),
    ]
    assert routing.equivalent_current_endpoint_nodes(
        records, {"mirror-a"}, set()
    ) == {"mirror-a", "mirror-b"}
    assert routing.equivalent_current_endpoint_nodes(
        records, set(), {refs}
    ) == {"mirror-a", "mirror-b"}


def test_internal_target_is_bounded_and_never_a_client_redirect_contract():
    target = routing.masked_target_url(
        "https://mirror.example.net/edge", "alice", "repo", "tree",
        {"path": "src/core", "token": "must-drop"},
    )
    assert target == (
        "https://mirror.example.net/edge/v1/repositories/alice/repo/tree"
        "?path=src%2Fcore"
    )
    assert "token" not in target
    assert routing.masked_target_url(
        "https://mirror.example.net", "alice", "repo", "admin", {}) == ""
    repeated = routing.masked_target_url(
        "https://mirror.example.net", "alice", "repo", "blobs",
        {"path": ["a.md", "b.md"], "token": "must-drop"})
    assert repeated.endswith("blobs?path=a.md&path=b.md")
    comparison = routing.masked_target_url(
        "https://mirror.example.net", "alice", "repo", "compare",
        {"base": "main", "head": "feature/web", "token": "must-drop"})
    assert comparison.endswith("compare?base=main&head=feature%2Fweb")
    assert "token" not in comparison


def test_proxy_request_and_response_do_not_expose_endpoint_or_credentials():
    message = routing.request_message(
        "mirror-a", "GET", "/v1/repositories/alice/repo/tree",
        routing.empty_body_sha256(), "request_123456", 99)
    assert message.startswith("forkmesh-masked-proxy-v1\nmirror-a\nGET\n")
    headers = routing.response_headers({
        "Content-Type": "application/json",
        "ETag": "abc",
        "Location": "https://mirror.example.net/private",
        "Set-Cookie": "secret=1",
        "Server": "mirror-a",
    })
    assert headers == {
        "content-type": "application/json",
        "etag": "abc",
        "x-content-type-options": "nosniff",
        "content-security-policy": "default-src 'none'",
    }


def test_proxy_response_never_relays_active_content_from_a_mirror():
    for media in (
        "text/html", "text/html; charset=utf-8", "TEXT/HTML",
        "application/xhtml+xml", "image/svg+xml", "text/xml",
        "application/xml", "application/javascript", "text/javascript",
        "multipart/x-mixed-replace; boundary=x", "",
    ):
        headers = routing.response_headers({
            "Content-Type": media,
            "Content-Disposition": "inline",
        })
        assert headers["content-type"] == "application/octet-stream", media
        assert headers["content-disposition"] == "attachment", media
        assert headers["content-security-policy"] == "default-src 'none'"
    image = routing.response_headers({"Content-Type": "image/png"})
    assert image["content-type"] == "image/png"
    assert "content-disposition" not in image


def test_private_route_signature_and_target_keep_repo_name_off_mirror_request():
    opaque_id = "a" * 64
    replica_digest = "b" * 64
    message = routing.private_route_message(
        "alice", "secret-project", "mirror-a", opaque_id, replica_digest,
        4, True, 123)
    assert message == (
        "forkmesh-private-route-v1\nalice\nsecret-project\nmirror-a\n"
        + opaque_id + "\n" + replica_digest + "\n4\n1\n123"
    )
    target = routing.masked_private_replica_url(
        "https://mirror.example.net/edge", opaque_id)
    assert target == (
        "https://mirror.example.net/edge/v1/private-replicas/" + opaque_id)
    assert "alice" not in target
    assert "secret-project" not in target
    assert routing.masked_private_replica_url(
        "https://mirror.example.net", "../secret") == ""
    assert routing.private_route_message(
        "alice", "secret-project", "mirror-a", opaque_id, replica_digest,
        4, "true", 123) == ""
