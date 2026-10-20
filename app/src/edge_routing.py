"""Pure policy for HTTPS routing to independently operated mirrors.

The public Worker remains the stable repository URL.  It selects an attested,
recently healthy endpoint and may send Git clients to that mirror's advertised
public URL.  Older Tunnel-only mirrors are still streamed in place; repository
bytes are never placed in D1.

Cryptographic signature verification and network I/O stay in ``entry.py``.
This module validates records and constructs the canonical messages those
runtime operations verify.
"""

import hashlib
import json
import ipaddress
import re
from urllib.parse import quote, urlencode, urlparse, urlunparse


# Endpoint registration is a coarse availability lease.  Hosts renew it every
# four minutes and the bounded renewal unit may spend up to five minutes
# validating a large encrypted mirror or waiting on the control plane.  Keep
# that healthy path inside this ten-minute lease.  Repository traffic is still
# fail-closed: entry.py independently requires a fresh node-signed repository
# proof and caches it for at most 60 seconds before every routed operation.
ENDPOINT_STALE_MS = 10 * 60 * 1000
MAX_FAILOVER_ATTEMPTS = 4
NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
OPAQUE_REPLICA_RE = re.compile(r"^[0-9a-f]{64}$")
PUBLIC_OPERATIONS = frozenset({
    "git-info-refs", "git-upload-pack", "git-receive-pack", "tree", "blobs",
    "blob", "raw", "history",
    "commit", "compare", "branches", "search", "stats", "sizes", "release-blob",
    "merge-pull", "merge-preview", "update-pull-branch", "actions-status",
    "actions-control", "merge-agent-branch", "delete-agent-branch",
    "branch-trash",
})
# The owner-authorized pull mutations. A mirror only serves these with an
# operator-configured merge executor, and a read-only node advertises neither,
# so routing one of them must look past the ordinary read rotation instead of
# hoping the one capable mirror landed inside it.
MERGE_EXECUTOR_OPERATIONS = frozenset({
    "merge-pull", "update-pull-branch", "merge-agent-branch",
    "delete-agent-branch",
})


def repository_proof_cache_key(endpoint, context, prefix, origin=""):
    """Bind an edge proof to the node key, repository, and admitted refs."""
    payload = json.dumps({
        "node": str(endpoint.get("node") or "").lower(),
        "publicKey": str(endpoint.get("publicKey") or ""),
        **({"origin": normalize_base_url(origin)} if origin else {}),
        "owner": str(context.get("routeOwner") or context["owner"]).lower(),
        "repo": str(context.get("repo") or "").lower(),
        "pins": sorted(str(pin or "").lower()
                       for pin in context.get("pins", set())),
    }, sort_keys=True, separators=(",", ":"))
    return prefix + hashlib.sha256(payload.encode("utf-8")).hexdigest()


async def repository_proof_cache_get(cache, key, now, pins, ttl_ms):
    """Return only a still-admitted proof previously verified by this Worker."""
    try:
        response = await cache.match(key)
        if response is None:
            return None
        data = json.loads(str(await response.text()))
        checked_at = int(data.get("checkedAt") or 0)
        digest = str(data.get("refsSha256") or "").lower()
        operations = data.get("operations")
        if (
            checked_at <= 0
            or checked_at > now + 10_000
            or now - checked_at > ttl_ms
            or digest not in pins
            or not isinstance(operations, list)
            or not 1 <= len(operations) <= 32
            or any(not isinstance(item, str) or
                   not re.fullmatch(r"[a-z][a-z0-9-]{0,39}", item)
                   for item in operations)
        ):
            return None
        return {
            "checkedAt": checked_at,
            "refsSha256": digest,
            "operations": frozenset(operations),
        }
    except Exception:
        return None


async def repository_proof_cache_put(
        cache, response_type, to_js, key, checked_at, digest, operations,
        ttl_ms):
    try:
        payload = json.dumps({
            "checkedAt": int(checked_at),
            "refsSha256": digest,
            "operations": sorted(set(operations)),
        }, separators=(",", ":"), sort_keys=True)
        response = response_type.new(payload, to_js({
            "status": 200,
            "headers": {
                "content-type": "application/json; charset=utf-8",
                "cache-control": "public, max-age=%d" % (ttl_ms // 1000),
            },
        }))
        await cache.put(key, response)
    except Exception:
        pass


def cloudflare_proxied_dns_answers(payloads, cidr_documents):
    """Return True only when DoH resolved exclusively to Cloudflare ranges.

    ``payloads`` are Cloudflare DNS-over-HTTPS JSON responses for A/AAAA.
    ``cidr_documents`` are the independently fetched official Cloudflare
    IPv4/IPv6 range documents. CNAME records are ignored; at least one address
    record is required and every returned address must be within a published
    Cloudflare network. Malformed input fails closed.
    """
    networks = []
    try:
        for document in cidr_documents:
            for token in str(document or "").split():
                networks.append(ipaddress.ip_network(token, strict=True))
    except ValueError:
        return False
    if not networks:
        return False
    addresses = []
    for payload in payloads:
        if not isinstance(payload, dict):
            return False
        try:
            status = int(payload.get("Status"))
        except (TypeError, ValueError):
            return False
        if status != 0:
            return False
        answers = payload.get("Answer", [])
        if not isinstance(answers, list):
            return False
        for answer in answers:
            if not isinstance(answer, dict):
                return False
            try:
                record_type = int(answer.get("type"))
            except (TypeError, ValueError):
                continue
            if record_type not in (1, 28):
                continue
            try:
                addresses.append(ipaddress.ip_address(
                    str(answer.get("data") or "").strip()))
            except ValueError:
                return False
    if not addresses:
        return False
    return all(
        any(
            address.version == network.version and address in network
            for network in networks
        )
        for address in addresses
    )


def public_dns_answers(payloads):
    """Return True only when every A/AAAA answer is globally routable."""
    addresses = []
    for payload in payloads:
        if not isinstance(payload, dict):
            return False
        try:
            status = int(payload.get("Status"))
        except (TypeError, ValueError):
            return False
        if status != 0:
            return False
        answers = payload.get("Answer", [])
        if not isinstance(answers, list):
            return False
        for answer in answers:
            if not isinstance(answer, dict):
                return False
            try:
                record_type = int(answer.get("type"))
            except (TypeError, ValueError):
                continue
            if record_type not in (1, 28):
                continue
            try:
                address = ipaddress.ip_address(
                    str(answer.get("data") or "").strip())
            except ValueError:
                return False
            if not address.is_global:
                return False
            addresses.append(address)
    return bool(addresses)


def _public_hostname(hostname):
    hostname = str(hostname or "").strip().lower().rstrip(".")
    if not hostname or hostname == "localhost" or hostname.endswith(".localhost"):
        return False
    if hostname.endswith((".local", ".internal", ".home", ".lan")):
        return False
    try:
        address = ipaddress.ip_address(hostname.strip("[]"))
    except ValueError:
        # A DNS name needs at least one dot. Cloudflare-managed mirror names
        # satisfy this and it avoids accidentally targeting an internal label.
        return "." in hostname
    return bool(address.is_global)


def normalize_base_url(value):
    """Return a safe public HTTPS endpoint base URL or an empty string."""
    try:
        parsed = urlparse(str(value or "").strip())
    except Exception:
        return ""
    if parsed.scheme != "https" or not parsed.hostname:
        return ""
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        return ""
    if parsed.port not in (None, 443) or not _public_hostname(parsed.hostname):
        return ""
    path = "/" + "/".join(
        quote(part, safe="-._~") for part in parsed.path.split("/") if part
    )
    path = "" if path == "/" else path.rstrip("/")
    hostname = parsed.hostname.lower()
    netloc = "[" + hostname + "]" if ":" in hostname else hostname
    return urlunparse(("https", netloc, path, "", "", ""))


def registration_message(
        node, base_url, public_key, issued_at, mirror_url=""):
    """Canonical record a node identity key signs before registration."""
    node = str(node or "").strip().lower()
    base_url = normalize_base_url(base_url)
    mirror_url = normalize_base_url(mirror_url) if mirror_url else ""
    public_key = str(public_key or "").strip()
    try:
        issued_at = int(issued_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(node)
        or not base_url
        or not public_key
        or issued_at <= 0
    ):
        return ""
    if mirror_url:
        return "\n".join((
            "forkmesh-https-endpoint-v2",
            node,
            base_url,
            mirror_url,
            public_key,
            str(issued_at),
        ))
    return "\n".join((
        "forkmesh-https-endpoint-v1", node, base_url, public_key,
        str(issued_at),
    ))


def health_challenge(node, nonce, issued_at):
    node = str(node or "").strip().lower()
    nonce = str(nonce or "").strip()
    try:
        issued_at = int(issued_at)
    except (TypeError, ValueError):
        return ""
    if not NODE_RE.fullmatch(node) or len(nonce) < 16 or issued_at <= 0:
        return ""
    return "\n".join((
        "forkmesh-https-health-v1", node, nonce[:128], str(issued_at),
    ))


def repository_health_challenge(
        node, nonce, issued_at, owner, repository, available, integrity,
        refs_sha256, operations_sha256):
    """Canonical signed availability proof for one explicitly named repo."""
    base = health_challenge(node, nonce, issued_at)
    owner = str(owner or "").strip().lower()
    repository = str(repository or "").strip()
    integrity = str(integrity or "").strip().lower()
    refs_sha256 = str(refs_sha256 or "").strip().lower()
    operations_sha256 = str(operations_sha256 or "").strip().lower()
    if (
        not base
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or integrity not in ("ok", "unavailable")
        or not re.fullmatch(r"[0-9a-f]{64}", refs_sha256)
        or not re.fullmatch(r"[0-9a-f]{64}", operations_sha256)
        or (bool(available) != (integrity == "ok"))
    ):
        return ""
    return "\n".join((
        "forkmesh-https-health-repository-v1",
        str(node).strip().lower(),
        str(nonce).strip(),
        str(int(issued_at)),
        owner,
        repository,
        "1" if available else "0",
        integrity,
        refs_sha256,
        operations_sha256,
    ))


def operations_sha256(operations):
    """Digest the gateway's sorted, unique public operation projection."""
    if not isinstance(operations, (list, tuple)):
        return ""
    normalized = []
    for operation in operations:
        operation = str(operation or "").strip()
        if operation not in PUBLIC_OPERATIONS or operation in normalized:
            return ""
        normalized.append(operation)
    if normalized != sorted(normalized):
        return ""
    return hashlib.sha256("\n".join(normalized).encode("utf-8")).hexdigest()


def validate_tunnel_manifest(value, base_url, node, public_key):
    """Validate the secret-free signed-manifest shape.

    Signature verification stays in the Worker runtime.  The returned bytes
    are exactly what the node signs with ``forkmesh-json-sort-v1``.
    """
    if not isinstance(value, dict):
        return None
    base_url = normalize_base_url(base_url)
    node = str(node or "").strip().lower()
    public_key = str(public_key or "").strip()
    signature = value.get("signature")
    endpoint = value.get("endpoint")
    manifest_node = value.get("node")
    dns = value.get("dns")
    edge = value.get("edge")
    storage = value.get("storage")
    parsed = urlparse(base_url)
    top_fields = {
        "schemaVersion", "type", "generatedAt", "node", "endpoint", "dns",
        "edge", "storage", "signature",
    }
    endpoint_fields = {
        "origin", "healthUrl", "manifestUrl", "repositoryUrlTemplate",
        "transport", "mainProxyMode",
    }
    dns_fields = {"recordName", "recordType", "proxied", "target"}
    edge_fields = {"provider", "kind", "tunnelId", "originExposure"}
    storage_fields = {
        "repositoryBytesInD1", "repositoryByteOwner", "d1Purpose",
    }
    d1_purposes = storage.get("d1Purpose") if isinstance(storage, dict) else None
    if (
        not base_url
        or not NODE_RE.fullmatch(node)
        or not re.fullmatch(r"[A-Za-z0-9_-]{43}", public_key)
        or set(value) != top_fields
        or value.get("schemaVersion") != 1
        or value.get("type") != "forkmesh.mirror-endpoint"
        or not re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T[0-9:.+-]+Z?",
            str(value.get("generatedAt") or ""))
        or not isinstance(signature, dict)
        or set(signature) != {
            "algorithm", "encoding", "canonicalization",
            "payloadSha256", "value",
        }
        or signature.get("algorithm") != "Ed25519"
        or signature.get("encoding") != "base64url-no-padding"
        or signature.get("canonicalization") != "forkmesh-json-sort-v1"
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(signature.get("payloadSha256") or ""))
        or not re.fullmatch(
            r"[A-Za-z0-9_-]{86}", str(signature.get("value") or ""))
        or not isinstance(manifest_node, dict)
        or manifest_node != {"name": node, "publicKey": public_key}
        or not isinstance(endpoint, dict)
        or set(endpoint) != endpoint_fields
        or endpoint.get("origin") != base_url
        or endpoint.get("healthUrl") != base_url + "/health"
        or endpoint.get("manifestUrl") != base_url + "/forkmesh-mirror.json"
        or endpoint.get("repositoryUrlTemplate") != (
            base_url
            + "/v1/repositories/{owner}/{repository}/{operation}")
        or endpoint.get("transport") != "direct-https"
        or endpoint.get("mainProxyMode") != "masked"
        or not isinstance(dns, dict)
        or set(dns) != dns_fields
        or dns.get("recordName") != parsed.hostname
        or dns.get("recordType") != "CNAME"
        or dns.get("proxied") is not True
        or not isinstance(edge, dict)
        or set(edge) != edge_fields
        or edge.get("provider") != "cloudflare"
        or edge.get("kind") != "cloudflare-tunnel"
        or edge.get("originExposure") != "loopback-only"
        or not isinstance(storage, dict)
        or set(storage) != storage_fields
        or storage.get("repositoryBytesInD1") is not False
        or storage.get("repositoryByteOwner") != "independent-mirror-host"
        or not isinstance(d1_purposes, list)
        or not d1_purposes
        or len(d1_purposes) != len(set(d1_purposes))
        or not set(d1_purposes).issubset({
            "service-discovery", "health-metadata", "routing-metadata",
            "signed-manifests",
        })
    ):
        return None
    tunnel_id = str(edge.get("tunnelId") or "")
    if (
        not re.fullmatch(r"[A-Za-z0-9-]+", tunnel_id)
        or dns.get("target") != tunnel_id + ".cfargotunnel.com"
    ):
        return None
    unsigned = {key: item for key, item in value.items() if key != "signature"}
    payload = json.dumps(
        unsigned, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    if not hashlib.sha256(payload).hexdigest() == signature["payloadSha256"]:
        return None
    return {
        "payload": payload,
        "signature": signature["value"],
        "payloadSha256": signature["payloadSha256"],
    }


def validate_direct_manifest(value, base_url, node, public_key):
    """Validate a signed manifest for an ordinary public HTTPS origin."""
    if not isinstance(value, dict):
        return None
    base_url = normalize_base_url(base_url)
    node = str(node or "").strip().lower()
    public_key = str(public_key or "").strip()
    signature = value.get("signature")
    endpoint = value.get("endpoint")
    manifest_node = value.get("node")
    dns = value.get("dns")
    edge = value.get("edge")
    storage = value.get("storage")
    parsed = urlparse(base_url)
    top_fields = {
        "schemaVersion", "type", "generatedAt", "node", "endpoint", "dns",
        "edge", "storage", "signature",
    }
    endpoint_fields = {
        "origin", "healthUrl", "manifestUrl", "repositoryUrlTemplate",
        "transport", "mainProxyMode",
    }
    d1_purposes = storage.get("d1Purpose") if isinstance(storage, dict) else None
    if (
        not base_url
        or not NODE_RE.fullmatch(node)
        or not re.fullmatch(r"[A-Za-z0-9_-]{43}", public_key)
        or set(value) != top_fields
        or value.get("schemaVersion") != 1
        or value.get("type") != "forkmesh.mirror-endpoint"
        or not re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T[0-9:.+-]+Z?",
            str(value.get("generatedAt") or ""))
        or not isinstance(signature, dict)
        or set(signature) != {
            "algorithm", "encoding", "canonicalization",
            "payloadSha256", "value",
        }
        or signature.get("algorithm") != "Ed25519"
        or signature.get("encoding") != "base64url-no-padding"
        or signature.get("canonicalization") != "forkmesh-json-sort-v1"
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(signature.get("payloadSha256") or ""))
        or not re.fullmatch(
            r"[A-Za-z0-9_-]{86}", str(signature.get("value") or ""))
        or manifest_node != {"name": node, "publicKey": public_key}
        or not isinstance(endpoint, dict)
        or set(endpoint) != endpoint_fields
        or endpoint.get("origin") != base_url
        or endpoint.get("healthUrl") != base_url + "/health"
        or endpoint.get("manifestUrl") != base_url + "/forkmesh-mirror.json"
        or endpoint.get("repositoryUrlTemplate") != (
            base_url + "/{owner}/{repository}.git")
        or endpoint.get("transport") != "direct-https"
        or endpoint.get("mainProxyMode") != "direct"
        or not isinstance(dns, dict)
        or set(dns) != {"recordName", "recordType", "proxied"}
        or dns.get("recordName") != parsed.hostname
        or dns.get("recordType") not in {"A", "AAAA", "A/AAAA"}
        or dns.get("proxied") is not False
        or edge != {
            "provider": "independent",
            "kind": "public-https-origin",
            "originExposure": "public-https",
        }
        or not isinstance(storage, dict)
        or set(storage) != {
            "repositoryBytesInD1", "repositoryByteOwner", "d1Purpose"}
        or storage.get("repositoryBytesInD1") is not False
        or storage.get("repositoryByteOwner") != "independent-mirror-host"
        or not isinstance(d1_purposes, list)
        or not d1_purposes
        or len(d1_purposes) != len(set(d1_purposes))
        or not set(d1_purposes).issubset({
            "service-discovery", "health-metadata", "routing-metadata",
            "signed-manifests",
        })
    ):
        return None
    unsigned = {key: item for key, item in value.items() if key != "signature"}
    payload = json.dumps(
        unsigned, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    if hashlib.sha256(payload).hexdigest() != signature["payloadSha256"]:
        return None
    return {
        "payload": payload,
        "signature": signature["value"],
        "payloadSha256": signature["payloadSha256"],
    }


def direct_repository_url(base_url, owner, repository):
    """Build one advertised Smart-HTTP repository URL."""
    page_url = direct_repository_page_url(base_url, owner, repository)
    return page_url + ".git" if page_url else ""


def direct_repository_page_url(base_url, owner, repository):
    """Build the mirror dashboard URL for one public repository."""
    base_url = normalize_base_url(base_url)
    owner = str(owner or "").strip().lower()
    repository = str(repository or "").strip()
    if (
        not base_url
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
    ):
        return ""
    return base_url + "/" + quote(owner, safe="-._~") + "/" + quote(
        repository, safe="-._~")


def direct_git_target_url(base_url, owner, repository, operation, query=None):
    """Build a standard read-only Git Smart-HTTP operation URL."""
    repository_url = direct_repository_url(base_url, owner, repository)
    suffix = {
        "git-info-refs": "/info/refs",
        "git-upload-pack": "/git-upload-pack",
    }.get(str(operation or ""))
    if not repository_url or not suffix:
        return ""
    encoded_query = urlencode(query or {}, doseq=True)
    return repository_url + suffix + (
        ("?" + encoded_query) if encoded_query else "")


# Body binding of a capability whose request body the Worker streams rather
# than buffers (a Git pack above HTTPS_MIRROR_BODY_MAX_BYTES). The Worker
# cannot know the digest of bytes it never holds, so the capability carries
# this fixed digest instead; a mirror gateway honours it only for a
# receive-pack POST, where the capability's single-use request id, freshness
# window and exact method/target binding remain in force and the pack itself
# is still bounded by the gateway's own receive.maxInputSize. The literal is
# the sha256 of a string, so no request body can collide with it.
STREAMED_REQUEST_BODY_SHA256 = hashlib.sha256(
    b"forkmesh-streamed-request-body-v1").hexdigest()


def request_message(node, method, path, body_sha256, request_id, issued_at):
    """Canonical per-request capability signed by the routing Worker."""
    node = str(node or "").strip().lower()
    method = str(method or "").strip().upper()
    path = str(path or "").strip()
    body_sha256 = str(body_sha256 or "").strip().lower()
    request_id = str(request_id or "").strip()
    try:
        issued_at = int(issued_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(node)
        or method not in ("GET", "HEAD", "POST")
        or not path.startswith("/")
        or "\n" in path
        or not re.fullmatch(r"[0-9a-f]{64}", body_sha256)
        or not re.fullmatch(r"[A-Za-z0-9_-]{12,80}", request_id)
        or issued_at <= 0
    ):
        return ""
    return "\n".join((
        "forkmesh-masked-proxy-v1", node, method, path, body_sha256,
        request_id, str(issued_at),
    ))


RUNNER_GRANT_MAX_TTL_MS = 3 * 60 * 60 * 1000
RUNNER_GRANT_ACCESS = ("read", "push", "actions-push")
_RUNNER_BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
_RUNNER_REF_TAIL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,150}$")
_GRANT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")


def runner_agent_ref_valid(ref):
    """True for exactly one branch under refs/heads/agent/."""
    ref = str(ref or "")
    prefix = "refs/heads/agent/"
    if not ref.startswith(prefix):
        return False
    tail = ref[len(prefix):]
    return bool(
        _RUNNER_REF_TAIL_RE.fullmatch(tail)
        and ".." not in tail
        and "//" not in tail
        and "@{" not in tail
        and "/." not in tail
        and not tail.endswith(("/", ".lock", "."))
    )


def runner_actions_ref_valid(ref):
    ref = str(ref or "")
    prefix = "refs/heads/"
    if not ref.startswith(prefix):
        return False
    branch = ref[len(prefix):]
    return bool(
        _RUNNER_BRANCH_RE.fullmatch(branch)
        and ".." not in branch
        and "//" not in branch
        and "@{" not in branch
        and not branch.endswith(("/", ".lock", "."))
    )


def runner_grant_message(
        node, owner, repository, access, ref, runner, grant_id, issued_at,
        expires_at):
    """Canonical router grant letting one runner job reach one repository.

    ``read`` grants clone/fetch only (ref is empty). ``push`` grants a
    receive-pack that may update exactly ``ref`` — always one branch under
    refs/heads/agent/. Unlike ``request_message`` the grant is reusable until
    ``expires_at`` (bounded by RUNNER_GRANT_MAX_TTL_MS) because one git clone
    or push is several HTTP requests; the gateway enforces the ref scope.
    """
    node = str(node or "").strip().lower()
    owner = str(owner or "").strip().lower()
    repository = str(repository or "").strip()
    access = str(access or "").strip().lower()
    ref = str(ref or "")
    runner = str(runner or "").strip().lower()
    grant_id = str(grant_id or "").strip()
    try:
        issued_at = int(issued_at)
        expires_at = int(expires_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(node)
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or access not in RUNNER_GRANT_ACCESS
        or (access == "read" and ref != "")
        or (access == "push" and not runner_agent_ref_valid(ref))
        or (access == "actions-push" and not runner_actions_ref_valid(ref))
        or not NODE_RE.fullmatch(runner)
        or not _GRANT_ID_RE.fullmatch(grant_id)
        or issued_at <= 0
        or not issued_at < expires_at <= issued_at + RUNNER_GRANT_MAX_TTL_MS
    ):
        return ""
    return "\n".join((
        "forkmesh-runner-grant-v1", node, owner, repository, access, ref,
        runner, grant_id, str(issued_at), str(expires_at),
    ))


AGENT_MODEL_GRANT_PROVIDERS = ("claude-code", "codex")
_AGENT_MODEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$")
_AGENT_LEASE_RE = re.compile(r"^[A-Za-z0-9_-]{8,80}$")
_AGENT_RUNNER_KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")


def agent_model_grant_message(
        runner, runner_key, owner, repository, ref, provider, model, lease_id,
        issued_at, expires_at):
    runner = str(runner or "").strip().lower()
    runner_key = str(runner_key or "").strip()
    owner = str(owner or "").strip().lower()
    repository = str(repository or "").strip()
    ref = str(ref or "")
    provider = str(provider or "")
    model = str(model or "")
    lease_id = str(lease_id or "")
    try:
        issued_at = int(issued_at)
        expires_at = int(expires_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(runner)
        or not _AGENT_RUNNER_KEY_RE.fullmatch(runner_key)
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or not runner_agent_ref_valid(ref)
        or provider not in AGENT_MODEL_GRANT_PROVIDERS
        or not _AGENT_MODEL_RE.fullmatch(model)
        or not _AGENT_LEASE_RE.fullmatch(lease_id)
        or issued_at <= 0
        or expires_at <= issued_at
    ):
        return ""
    return "\n".join((
        "forkmesh-agent-model-grant-v1", runner, runner_key, owner,
        repository, ref, provider, model, lease_id, str(issued_at),
        str(expires_at),
    ))


def private_route_message(
        owner, repository, node, opaque_id, replica_sha256, key_epoch,
        active, issued_at):
    """Canonical owner authorization for one opaque private replica route."""
    owner = str(owner or "").strip().lower()
    repository = str(repository or "").strip()
    node = str(node or "").strip().lower()
    opaque_id = str(opaque_id or "").strip().lower()
    replica_sha256 = str(replica_sha256 or "").strip().lower()
    try:
        key_epoch = int(key_epoch)
        issued_at = int(issued_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or not NODE_RE.fullmatch(node)
        or not OPAQUE_REPLICA_RE.fullmatch(opaque_id)
        or not re.fullmatch(r"[0-9a-f]{64}", replica_sha256)
        or key_epoch <= 0
        or issued_at <= 0
        or not isinstance(active, bool)
    ):
        return ""
    return "\n".join((
        "forkmesh-private-route-v1",
        owner,
        repository,
        node,
        opaque_id,
        replica_sha256,
        str(key_epoch),
        "1" if active else "0",
        str(issued_at),
    ))


def normalize_endpoint_record(value):
    """Normalize the public, secret-free endpoint health projection."""
    if not isinstance(value, dict):
        return None
    node = str(value.get("node") or "").strip().lower()
    base_url = normalize_base_url(value.get("baseUrl"))
    mirror_url = normalize_base_url(value.get("mirrorUrl"))
    public_key = str(value.get("publicKey") or "").strip()
    signature = str(value.get("signature") or "").strip()
    if (
        not NODE_RE.fullmatch(node)
        or not base_url
        or not public_key
        or not signature
    ):
        return None
    try:
        issued_at = int(value.get("issuedAt") or 0)
        checked_at = int(value.get("checkedAt") or 0)
        latency_ms = max(0, min(60_000, int(value.get("latencyMs") or 0)))
    except (TypeError, ValueError):
        return None
    refs_sha256 = str(value.get("refsSha256") or "").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", refs_sha256):
        refs_sha256 = ""
    return {
        "node": node,
        "baseUrl": base_url,
        "mirrorUrl": mirror_url,
        "publicKey": public_key,
        "signature": signature,
        "issuedAt": issued_at,
        "checkedAt": checked_at,
        "latencyMs": latency_ms,
        "healthy": bool(value.get("healthy")),
        "integrity": str(value.get("integrity") or "unknown").lower(),
        "region": str(value.get("region") or "").strip().upper()[:8],
        "abuseBlocked": bool(value.get("abuseBlocked")),
        "refsSha256": refs_sha256,
    }


# sha256 of no bytes at all. Mirrors sign it for a moment while their gateway
# reloads after a ref move (seen on 2026-09-18 across the whole fleet), and two
# reloading nodes agreeing on it is not evidence of any repository state.
EMPTY_REFS_SHA256 = (
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")


def signed_refs_lag(operation, details):
    """The node-signed refs digest of a proof that failed only on the pin set.

    ``details`` is what ``_https_mirror_repository_proof`` reports. Its
    ``refs_mismatch`` reason is only reached once the proof is signed, healthy,
    available and intact, so the node differs from the relay's accepted pins
    in nothing but its refs digest. Returns that digest when the proof also
    advertises ``operation``, and "" for every other failure.
    """
    if not isinstance(details, dict):
        return ""
    if (
        details.get("reason") != "refs_mismatch"
        or details.get("signed") is not True
    ):
        return ""
    digest = str(details.get("refsSha256") or "").lower()
    if not re.fullmatch(r"[0-9a-f]{64}", digest) or digest == EMPTY_REFS_SHA256:
        return ""
    if operation not in set(details.get("operations") or ()):
        return ""
    return digest


def mismatch_is_servable(operation, node, details, agreeing):
    """Whether a signed proof that differs only in its refs digest may serve.

    Pins lag the fleet. After main moves, every mirror serves the new refs
    state within seconds, but the pin set learns it only once two endpoint
    proofs agree in the health cron. Until then every visitor read failed with
    refs_mismatch and answered mirror_unavailable: issue #692 (the Windows
    installer download) and the flagship_repository row's visitor 5xx minutes.

    A release blob is addressed by its sha256 and the gateway hashes it before
    answering, so the refs state says nothing about those bytes. A browse read
    takes the same trust the pin quorum does: two distinct admitted endpoints
    presenting matching node-signed proofs within this request.

    A clone takes that same quorum. Git transport used to keep the strict pin,
    so `git clone` answered 503 mirror_unavailable for the minute or two after
    every ref move while the website kept serving (adhoc #243). The two
    agreeing nodes hold one identical refs set, so either can answer the
    upload-pack for the other's advertisement; the proxy still returns a clone
    to its advertising node whenever that node is one of them. A push never
    rides a lagging proof.

    An owner's pull mutation takes the quorum too. Its job names the exact
    base, head and ledger commits, which the executor re-reads before it
    writes anything, so the quorum only decides which healthy node runs it.
    Held to the strict pin, a web merge answered merge_node_unavailable for
    the minute or two after every push reached the fleet.
    """
    if not isinstance(agreeing, dict):
        return False
    digest = signed_refs_lag(operation, details)
    if not digest:
        return False
    if operation == "git-receive-pack":
        return False
    if operation == "release-blob":
        return True
    node_name = str(node or "").strip().lower()
    if not node_name:
        return False
    nodes = agreeing.setdefault(digest, set())
    nodes.add(node_name)
    return len(nodes) >= 2


def merge_unavailable_reason(candidate_count, reasons):
    """Why no mirror could take an owner's pull mutation, for the browser.

    ``no_merge_executor`` is the one answer waiting cannot fix: every mirror
    that answered verified, but none advertises the executor (an older node
    build, or ``disableMergeExecutor``). The others clear on their own, so the
    dashboard retries them instead of blaming the mirrors' build.
    """
    reasons = {str(reason or "") for reason in (reasons or ())}
    if not candidate_count:
        return "no_online_mirror"
    if reasons == {"git_operation_unavailable"}:
        return "no_merge_executor"
    if "refs_mismatch" in reasons:
        return "refs_catching_up"
    return "mirror_unverified"


def endpoint_eligible(record, now, allowed_nodes=None):
    record = normalize_endpoint_record(record)
    if not record:
        return False
    try:
        now = int(now)
    except (TypeError, ValueError):
        return False
    if allowed_nodes is not None and record["node"] not in set(allowed_nodes):
        return False
    return bool(
        record["healthy"]
        and not record["abuseBlocked"]
        and record["integrity"] == "ok"
        and record["checkedAt"] > 0
        and 0 <= now - record["checkedAt"] <= ENDPOINT_STALE_MS
    )


def _rotate(records, cursor):
    """Start a bounded ring at the caller's cursor without stranding it."""
    if not records:
        return records
    cursor = int(cursor or 0) % len(records)
    return records[cursor:] + records[:cursor]


def select_endpoints(records, now, preferred_region="", cursor=0, prefer=()):
    """Healthy endpoints ordered for latency/region and deterministic failover.

    ``prefer`` names endpoints this request is already committed to: the node a
    clone advertisement pinned, the node an in-flight merge job recorded, or
    the mirrors whose last signed advertisement carried the operator-configured
    operation being routed. They are held at the front of the window rather
    than rotated through it. The bounded failover cap is applied last, so a
    group larger than ``MAX_FAILOVER_ATTEMPTS`` used to drop a named endpoint
    outright and break the very pin that named it.
    """
    eligible = [
        normalize_endpoint_record(record)
        for record in (records or [])
        if endpoint_eligible(record, now)
    ]
    preferred_region = str(preferred_region or "").strip().upper()
    eligible.sort(key=lambda record: (
        0 if preferred_region and record["region"] == preferred_region else 1,
        record["latencyMs"] or 60_000,
        record["node"],
    ))
    if not eligible:
        return []
    prefer = {str(node or "").strip().lower() for node in (prefer or ())}
    cursor = int(cursor or 0)
    # Each group keeps its own ring, so naming several endpoints — every
    # mirror that advertises the merge executor, say — still spreads the work
    # instead of always landing the same one first.
    held = _rotate(
        [record for record in eligible if record["node"] in prefer], cursor)
    rotating = _rotate(
        [record for record in eligible if record["node"] not in prefer],
        cursor)
    return (held + rotating)[:MAX_FAILOVER_ATTEMPTS]


def cap_failover(selected, prefer=()):
    """Bound a merged candidate list without dropping a committed endpoint.

    Two-tier selection concatenates the current generation's ring with the
    historical failover ring, so this final cap is the second place a pinned
    or capability-named endpoint could fall out of the window.
    """
    prefer = {str(node or "").strip().lower() for node in (prefer or ())}
    if not prefer:
        return list(selected or [])[:MAX_FAILOVER_ATTEMPTS]
    held = [
        record for record in (selected or [])
        if str(record.get("node") or "").strip().lower() in prefer
    ]
    rest = [
        record for record in (selected or [])
        if str(record.get("node") or "").strip().lower() not in prefer
    ]
    return (held + rest)[:MAX_FAILOVER_ATTEMPTS]


def equivalent_current_endpoint_nodes(records, current_nodes, current_pins):
    """Expand current preference to endpoints with identical signed refs."""
    current_nodes = {
        str(node or "").strip().lower() for node in (current_nodes or set())
    }
    current_pins = {
        str(pin or "").strip().lower() for pin in (current_pins or set())
    }
    current_refs = {
        record.get("refsSha256", "")
        for record in (records or [])
        if record.get("refsSha256")
        and (
            record.get("node") in current_nodes
            or record.get("refsSha256") in current_pins
        )
    }
    if not current_refs:
        return current_nodes
    return current_nodes.union({
        record.get("node")
        for record in (records or [])
        if record.get("node") and record.get("refsSha256") in current_refs
    })


def masked_target_url(base_url, owner, repo, operation, query=None):
    """Build an internal endpoint URL; callers must never return it to clients."""
    base_url = normalize_base_url(base_url)
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    operation = str(operation or "").strip()
    if (
        not base_url
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repo)
        or operation not in PUBLIC_OPERATIONS
    ):
        return ""
    path = "/v1/repositories/%s/%s/%s" % (
        quote(owner, safe="-._~"),
        quote(repo, safe="-._~"),
        quote(operation, safe="-"),
    )
    safe_query = {}
    for key in ("path", "ref", "base", "head", "cherry", "limit", "service",
                "sha256", "tagMoves", "protect", "pushPrefix"):
        if key in (query or {}):
            raw = (query or {}).get(key)
            values = raw if isinstance(raw, (list, tuple)) else [raw]
            clean = []
            for value in values[:60]:
                value = str(value or "")[:500]
                if "\x00" not in value and "\r" not in value and "\n" not in value:
                    clean.append(value)
            if clean:
                safe_query[key] = clean if len(clean) > 1 else clean[0]
    return base_url + path + (
        ("?" + urlencode(safe_query, doseq=True)) if safe_query else "")


def masked_private_replica_url(base_url, opaque_id):
    """Build an internal ciphertext URL without a repository identity."""
    base_url = normalize_base_url(base_url)
    opaque_id = str(opaque_id or "").strip().lower()
    if not base_url or not OPAQUE_REPLICA_RE.fullmatch(opaque_id):
        return ""
    return base_url + "/v1/private-replicas/" + opaque_id


def response_headers(headers):
    """Allowlist endpoint response headers without leaking its identity/cookies."""
    headers = {
        str(key).lower(): str(value)
        for key, value in (headers or {}).items()
    }
    allowed = (
        "content-type", "content-length", "content-range", "accept-ranges",
        "etag", "last-modified", "cache-control", "content-disposition",
    )
    output = {key: headers[key] for key in allowed if key in headers}
    if active_content_type(output.get("content-type", "")):
        output["content-type"] = "application/octet-stream"
        output["content-disposition"] = "attachment"
    output["x-content-type-options"] = "nosniff"
    output["content-security-policy"] = "default-src 'none'"
    return output


def active_content_type(value):
    media = str(value or "").split(";", 1)[0].strip().lower()
    return (not media or "html" in media or "xml" in media
            or "script" in media or media.startswith("multipart/"))


def empty_body_sha256():
    return hashlib.sha256(b"").hexdigest()
