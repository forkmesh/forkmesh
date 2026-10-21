#!/usr/bin/env python3
"""Fail-closed local HTTPS-origin gateway for independent ForkMesh mirrors.

The process intentionally listens on loopback HTTP behind an operator-owned
direct HTTPS reverse proxy. Public Git Smart-HTTP reads
are independently reachable; control and non-Git repository requests retain
the routing Worker's short-lived capability. Health challenges are signed by
a separate local signer process, so this process never loads a private key.

Repository names, paths, request queries, Git stderr, source bytes, client
addresses, and user agents are never written to the gateway log.
"""

from __future__ import annotations

import argparse
import base64
import bisect
import fnmatch
from collections import Counter, OrderedDict
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import ipaddress
import itertools
import json
import mimetypes
import os
from pathlib import Path
import posixpath
import re
import selectors
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Iterable, Mapping
from urllib import error as urllib_error
from urllib.parse import parse_qs, unquote, urlencode, urlsplit
from urllib import request as urllib_request

try:
    import fcntl
except ImportError:
    fcntl = None


# The dashboard replica contract is shared with the Worker. Installed nodes
# stage these two files beside this script; source checkouts keep them under
# app/src. Explicit paths keep the systemd ``python -I`` validation mode
# deterministic without depending on a user-controlled PYTHONPATH.
for _shared_source in (
    Path(__file__).resolve().parent,
    Path(__file__).resolve().parents[1] / "app" / "src",
):
    if (_shared_source / "mirror_replica.py").is_file():
        sys.path.insert(0, str(_shared_source))
        break
try:
    import mirror_replica
except ImportError:
    # Older standalone packages contain only mirror_gateway.py. They keep all
    # prior Git/dashboard behavior; replicaPath fails configuration validation
    # until the two shared contract files are installed beside this script.
    mirror_replica = None
try:
    import portable_sessions
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PublicKey,
    )
except ImportError:
    # Portable login is an additive dashboard feature. Older packages keep
    # serving public Git/dashboard reads but fail closed as logged out.
    portable_sessions = None
    Ed25519PublicKey = None
# Operator policy (node_operator_policy.py) ships beside this script. It is
# opt-in: a package without it serves exactly as before, and a configuration
# that names operatorPolicy fails validation until the module is installed.
if (Path(__file__).resolve().parent / "node_operator_policy.py").is_file():
    if str(Path(__file__).resolve().parent) not in sys.path:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import node_operator_policy
except ImportError:
    node_operator_policy = None


SCHEMA_VERSION = 1
CAPABILITY_WINDOW_MS = 60_000
MAX_REQUEST_BODY = 8 * 1024 * 1024
VISIBLE_REF_PREFIXES = ("refs/heads/", "refs/tags/", "refs/pr/", "refs/remotes/origin/")
# A receive-pack POST carrying a verified router capability may exceed
# MAX_REQUEST_BODY up to limits.receiveMaxInputBytes (git's own
# receive.maxInputSize). This default matches the SSH gateway's; a single
# pack of that size holds a repository far beyond 100k lines of source.
DEFAULT_RECEIVE_MAX_INPUT_BYTES = 256 * 1024 * 1024
# Large pushes are read fully into memory before git sees them, so only a
# few may be in flight at once; the rest are answered 503 without a read.
LARGE_REQUEST_BODY_SLOTS = 2
# Chunked receive-pack bodies arrive in whatever pieces the Worker's fetch
# emits; the chunk-count guard scales with the body allowance so it still
# rejects a degenerate stream of tiny chunks without capping legitimate size.
MIN_AVERAGE_CHUNK_BYTES = 256
# receive-pack indexes and fsck-checks every object; its deadline grows with
# the pack so a large legitimate push is not cut off at the fixed floor.
RECEIVE_PACK_BYTES_PER_SECOND = 1024 * 1024
MAX_REQUEST_CHUNKS = 4096
MAX_JSON_BLOB = 2 * 1024 * 1024
MAX_JSON_OUTPUT = 8 * 1024 * 1024
MAX_COMPARE_BYTES = 4 * 1024 * 1024
MAX_COMPARE_COMMITS = 500
MAX_CHERRY_UPSTREAM_COMMITS = 2000
# A readiness preview reports which paths a merge would leave conflicted. The
# list is for a reviewer to read, not to reconstruct a tree from, so it is
# bounded well below the patch limits above.
MAX_PREVIEW_CONFLICTS = 200
MAX_TREE_ENTRIES = 500
MAX_SEARCH_RESULTS = 60
MAX_SEARCH_OUTPUT_BYTES = 2 * 1024 * 1024
MAX_ANALYSIS_FILES = 320
MAX_ANALYSIS_REPOSITORY_FILES = 20_000
MAX_ANALYSIS_FILE_BYTES = 256 * 1024
MAX_ANALYSIS_TOTAL_BYTES = 6 * 1024 * 1024
MAX_COVERAGE_ARTIFACT_BYTES = 2 * 1024 * 1024
MAX_DEPENDENCY_EDGES = 4_000
PROCESS_TIMEOUT_SECONDS = 30
# The first byte is a bounded request/host-health wait. Once a pack has begun,
# allow a substantially longer host-inactivity window: large clones commonly
# take minutes. These are inactivity windows, not total-transfer deadlines.
STREAM_INITIAL_RESPONSE_TIMEOUT_SECONDS = 60
STREAM_INACTIVITY_TIMEOUT_SECONDS = 30 * 60
MERGE_EXECUTOR_TIMEOUT_SECONDS = 10 * 60
PREWARM_AFTER_MERGE_QUIET_SECONDS = 5.0
BACKGROUND_MUTATIONS = frozenset({"delete-agent-branch"})
MAX_MERGE_REQUEST_BODY = 8 * 1024
MAX_MERGE_JOB_CACHE = 1_000
# A published mutation result carries the executor's node-signed attestation
# of the refs this mirror serves right after the change. The relay checks the
# attestation's freshness before admitting that state, so a cached result
# older than this is re-signed (one bounded "status" call) before it is
# answered again.
MERGE_ATTESTATION_REFRESH_SECONDS = 60
MERGE_ATTESTATION_SIGNATURE_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
MAX_ACTIONS_SUMMARY_BYTES = 256 * 1024
MAX_ACTIONS_SUMMARY_RUNS = 20
MAX_ACTIONS_LOG_TAIL_BYTES = 16 * 1024
MAX_ACTIONS_SUMMARY_LEASE_MS = 15 * 60 * 1000
# Summary v2 (www/docs/operations/mirror-actions-control.md). Every bound is
# shared with the executor that writes the document and the Worker that
# revalidates the gateway's answer.
MAX_ACTIONS_RUN_STEPS = 64
MAX_ACTIONS_STEP_NAME_CHARS = 120
MAX_ACTIONS_SUMMARY_REPOSITORIES = 64
MAX_ACTIONS_REPOSITORY_WORKFLOWS = 32
MAX_ACTIONS_WORKFLOW_NAME_CHARS = 160
MAX_ACTIONS_WORKFLOW_EVENTS = 8
MAX_ACTIONS_WORKFLOW_EVENT_CHARS = 40
MAX_ACTIONS_WORKFLOW_RUNNER_LABELS = 8
MAX_ACTIONS_WORKFLOW_RUNNER_LABEL_CHARS = 64
MAX_ACTIONS_SUMMARY_CONTROLS = 32
ACTIONS_STEP_STATUSES = frozenset({
    "pending", "running", "success", "failed", "skipped", "cancelled",
})
ACTIONS_CONTROL_RESULT_STATUSES = frozenset({"applied", "rejected"})
ACTIONS_CONTROL_REJECTION_REASONS = frozenset({
    "actions_disabled",
    "invalid_request",
    "not_awaiting_approval",
    "not_cancellable",
    "not_manual",
    "operator_disabled",
    "ref_not_found",
    "repository_not_found",
    "run_not_found",
    "workflow_disabled",
    "workflow_invalid",
    "workflow_not_found",
    "wrong_node",
})
# Control requests are couriered, never decided, here: the gateway stores the
# router capability beside the exact body and the root executor verifies that
# signature itself before acting. The gateway account can therefore drop or
# delay a request, but it cannot author, alter, or replay one.
ACTIONS_CONTROL_DIRECTORY = "actions-control"
ACTIONS_CONTROL_WIRE_TYPE = "forkmesh.actions-control-v1"
ACTIONS_CONTROL_ENVELOPE_TYPE = "forkmesh.mirror-actions-control"
MAX_ACTIONS_CONTROL_BODY = 4 * 1024
MAX_ACTIONS_CONTROL_PENDING = 32
MAX_ACTIONS_CONTROL_ENVELOPE_BYTES = 16 * 1024
ACTIONS_CONTROL_EXPIRY_MS = 10 * 60 * 1000
ACTIONS_CONTROL_CLOCK_SKEW_MS = 60 * 1000
# Extra body keys per action; the common keys are always required as well.
ACTIONS_CONTROL_ACTIONS = {
    "dispatch": frozenset({"workflow", "ref"}),
    "rerun": frozenset({"runId"}),
    "cancel": frozenset({"runId"}),
    "approve": frozenset({"runId"}),
    "reject": frozenset({"runId"}),
    "set-workflow-enabled": frozenset({"workflow", "enabled"}),
    "set-repository-enabled": frozenset({"enabled"}),
}
ACTIONS_CONTROL_COMMON_FIELDS = frozenset({
    "schemaVersion", "type", "requestId", "action", "actor", "issuedAt",
})
ACTIONS_CONTROL_REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")
ACTIONS_CONTROL_ACTOR_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
ACTIONS_WORKFLOW_PATH_RE = re.compile(
    r"^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$")
ACTIONS_CONTROL_REF_RE = re.compile(r"^refs/heads/[A-Za-z0-9._/-]{1,200}$")
# One directory is shared by every application generation a reload builds, so
# the queue bound and the idempotency check are serialized process-wide.
_ACTIONS_CONTROL_LOCK = threading.Lock()
SERVICE_COUNTERS_FILE = "service-counters.json"
SERVICE_COUNTERS_TYPE = "forkmesh.mirror-service-counters"
GATEWAY_GENERATION_DIRECTORY = "pull-jobs"
GATEWAY_GENERATION_FILE = "gateway-generation"
MAX_SERVICE_COUNTERS_BYTES = 1024 * 1024
MAX_SERVICE_COUNTER = 999_999_999_999
MAX_SAFE_JSON_INTEGER = (1 << 53) - 1
WEBSITE_COUNTER_OPERATIONS = frozenset({
    "tree",
    "blobs",
    "blob",
    "raw",
    "history",
    "commit",
    "compare",
    "branches",
    "search",
    "stats",
    "sizes",
    "release-blob",
    "merge-preview",
    "pulse",
})
SERVICE_COUNTER_ROW_FIELDS = ("clonesServed", "websiteServed", "updatedAt")
# Optional per-row extension: when the last clone / website request was served
# and which class of client it was. Only the generalized class below is stored,
# never the raw User-Agent, so no request fingerprint reaches a public record.
SERVICE_COUNTER_STAMP_FIELDS = (
    "cloneServedAt",
    "cloneServedAgent",
    "websiteServedAt",
    "websiteServedAgent",
)
SERVICE_COUNTER_AGENT_CLASSES = frozenset({
    "forkmesh-node",
    "git-client",
    "bot-tool",
    "browser",
    "client",
})
MAX_ACTIONS_SUMMARY_CLOCK_SKEW_MS = 60 * 1000
SYSTEM_ACTIONS_SUMMARY_PATH = Path(
    "/var/lib/forkmesh-mirror/gateway/actions-summary.json"
)
HOSTED_PUBLIC_GIT_ROOT = Path("/srv/forkmesh-git/imports")
COLLABORATION_TREE_ROOTS = frozenset({
    ".forkmesh/pulls", ".forkmesh/issues", ".forkmesh/discussions", ".forkmesh/releases",
})
PUBLIC_OPERATIONS = frozenset(
    {
        "git-info-refs",
        "git-upload-pack",
        "git-receive-pack",
        "tree",
        "blobs",
        "blob",
        "raw",
        "history",
        "commit",
        "compare",
        "branches",
        "search",
        "stats",
        "sizes",
        "release-blob",
        "merge-pull",
        "merge-preview",
        "update-pull-branch",
        "merge-agent-branch",
        "delete-agent-branch",
        "actions-status",
        "actions-control",
    }
)
# Write operations are never enabled implicitly: git-receive-pack requires an
# explicit per-repository operations entry, or the writable-owner allowlist.
DEFAULT_PUBLIC_OPERATIONS = PUBLIC_OPERATIONS - {
    "git-receive-pack",
    "merge-pull",
    "update-pull-branch",
    "merge-agent-branch",
    "delete-agent-branch",
    "actions-status",
    "actions-control",
}
# Operations a repository created under writableRepositoryRoot serves. Kept to
# the Git transport: website reads still route through repositories the
# refresh cycle has explicitly rendered into the signed configuration.
WRITABLE_REPOSITORY_OPERATIONS = frozenset(
    {"git-info-refs", "git-upload-pack", "git-receive-pack"}
)
RECEIVE_PACK_TIMEOUT_SECONDS = 120
# writableOwners entry that accepts any owner the control plane authorized.
# A node hosting repositories created from the website (issue #676) serves
# accounts it cannot enumerate — the owner of a repository created in a
# browser has no node here at all — so an operator opts into that role with
# this single entry instead of a name list it could never keep current. Every
# request that reaches a write already carried a verified router capability,
# so the control plane remains the thing deciding who may push; the wildcard
# only says this node is willing to hold the result.
WRITABLE_OWNERS_WILDCARD = "*"
HOSTED_PUSHES_FILE = "hosted-pushes.json"
HOSTED_PUSHES_TYPE = "forkmesh.hosted-pushes"
MAX_HOSTED_PUSH_REPOSITORIES = 200
# Must match tools/headless_mirror_refresh.MAX_HOSTED_PUSHES_BYTES: both sides
# of the sidecar refuse to read a document larger than the write path can
# produce, so a planted file cannot be turned into unbounded work.
MAX_HOSTED_PUSHES_BYTES = 256 * 1024
# Coalescing marker consumed by the packaged systemd path unit
# (server/packaging/systemd/forkmesh-mirror-refresh.path); creating it wakes
# the same refresh/registration cycle an SSH push notifies.
REFRESH_PENDING_PATH = Path("/run/forkmesh-mirror-refresh/pending")
# Public repositories may be read directly by native clients without asking the
# routing Worker to mint a per-request capability.  Keep this list deliberately
# narrower than PUBLIC_OPERATIONS: Git POSTs, private replicas, Actions output,
# and every mutation still require the existing router authorization boundary.
DIRECT_PUBLIC_READ_OPERATIONS = frozenset({
    "tree",
    "blobs",
    "blob",
    "raw",
    "history",
    "commit",
    "compare",
    "branches",
    "search",
    "stats",
    "sizes",
    "release-blob",
    "merge-preview",
    "pulse",
})
# Mutating pull operations share one operator-owned executor command and one
# job runner. Everything that differs between them is data: the wire type the
# browser signs, the executor protocol type, and the terminal status plus the
# object ids that status publishes.
PULL_MUTATIONS = {
    "merge-pull": {
        "wireType": "forkmesh.pull-merge-v1",
        "executorType": "forkmesh.pull-merge-executor-v1",
        "terminal": "merged",
        "before": ("baseBefore", "head", "pullsBefore"),
        "after": ("baseAfter", "pullsAfter"),
        "failures": frozenset({
            "checks_required", "merge_conflict", "pull_not_found",
            "pull_not_open", "review_required", "rules_required",
            "stale_base", "stale_head", "stale_pull_metadata",
            "unsupported_pull",
        }),
    },
    "update-pull-branch": {
        "wireType": "forkmesh.pull-update-v1",
        "executorType": "forkmesh.pull-update-executor-v1",
        "terminal": "updated",
        "before": ("baseBefore", "head", "pullsBefore"),
        "after": ("headAfter",),
        "failures": frozenset({
            "already_current", "merge_conflict", "pull_not_found",
            "pull_not_open", "stale_base", "stale_head",
            "stale_pull_metadata", "unsupported_pull",
        }),
    },
    "merge-agent-branch": {
        "wireType": "forkmesh.branch-merge-v1",
        "executorType": "forkmesh.branch-merge-executor-v1",
        "terminal": "merged",
        "branch": True,
        "before": ("baseBefore", "head"),
        "after": ("baseAfter",),
        "failures": frozenset({
            "already_merged", "branch_not_found", "merge_conflict",
            "stale_base", "stale_head", "unsupported_branch",
        }),
    },
    "delete-agent-branch": {
        "wireType": "forkmesh.branch-delete-v1",
        "executorType": "forkmesh.branch-delete-executor-v1",
        "terminal": "deleted",
        "branch": True,
        "before": ("head",),
        "after": (),
        "failures": frozenset({
            "branch_not_found", "not_merged", "stale_base", "stale_head",
            "unsupported_branch",
        }),
    },
}
PULL_RULES_EVALUATE_TYPE = "forkmesh.pull-rules-evaluate-v1"
MAX_ORG_PULL_RULES_BYTES = 6 * 1024
MAX_PULL_RULES_RESPONSE_BYTES = 64 * 1024
PULL_RULES_NUMBER_RE = re.compile(r"^[1-9][0-9]{0,8}$")
MERGE_BASE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
INTERNAL_GIT_REF_NAMESPACE = "refs/forkmesh/"
MAINTENANCE_LOCK_NAME = "forkmesh-maintenance.lock"
MAINTENANCE_MARKER_NAME = "forkmesh-maintenance-due"
MAINTENANCE_LOCK_WAIT_SECONDS = 120.0
BRANCH_TRASH_OPERATION = "branch-trash"
BRANCH_TRASH_CARRIER_OPERATION = "delete-agent-branch"
BRANCH_TRASH_PREFIXES = (
    "refs/forkmesh/trash/branch/",
    "refs/forkmesh/trash/agent-branch/",
)
BRANCH_TRASH_TOKEN_RE = re.compile(r"^[0-9a-f]{16}$")
BRANCH_TRASH_OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
FULL_COMMIT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
MAX_BRANCH_TRASH_ENTRIES = 5000
LOG_OPERATIONS = PUBLIC_OPERATIONS | {
    "private-replica", "dashboard", BRANCH_TRASH_OPERATION, "pulse"}
NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
# Body binding of a capability whose pack the Worker streamed without holding
# (edge_routing.STREAMED_REQUEST_BODY_SHA256). Honoured only for a
# receive-pack POST: the capability's single-use request id, freshness window
# and exact method/target binding still apply, and receive.maxInputSize still
# bounds the pack. Being the sha256 of a string, no body can collide with it.
STREAMED_REQUEST_BODY_SHA256 = hashlib.sha256(
    b"forkmesh-streamed-request-body-v1").hexdigest()
# Per-operation query allowlist shared by the capability route, the direct
# public route, and the dashboard's /api/repo alias.
OPERATION_QUERY_FIELDS = {
    "git-info-refs": frozenset({"service"}),
    "git-upload-pack": frozenset(),
    "git-receive-pack": frozenset(
        {"service", "tagMoves", "protect", "pushPrefix"}),
    "tree": frozenset({"path", "ref"}),
    "blobs": frozenset({"path", "ref"}),
    "blob": frozenset({"path", "ref"}),
    "raw": frozenset({"path", "ref"}),
    "history": frozenset({"ref", "all", "since", "until", "path"}),
    "commit": frozenset({"path"}),
    "compare": frozenset({"base", "head", "cherry", "limit"}),
    "merge-preview": frozenset({"base", "head", "rules", "pull", "orgRules"}),
    "update-pull-branch": frozenset(),
    "branches": frozenset(),
    "search": frozenset({"path", "ref"}),
    "stats": frozenset({"ref"}),
    "pulse": frozenset({"period", "person"}),
    "sizes": frozenset({"ref"}),
    "release-blob": frozenset({"sha256", "path"}),
    "merge-pull": frozenset(),
    "merge-agent-branch": frozenset(),
    "delete-agent-branch": frozenset(),
    "actions-status": frozenset(),
    "actions-control": frozenset(),
    "branch-trash": frozenset(),
}
# ---------------------------------------------------------------------------
# Independent mirror dashboard.
#
# The mirror can serve the SAME website the relay Worker serves, sourced from
# the built assets already present as git blobs inside the designated public
# repository it mirrors (normally forkmesh/forkmesh). Assets therefore update
# with ordinary repository sync, stay covered by the integrity pin, and never
# require a separate deployment artifact. api-client.js resolves the API
# origin to location.origin on non-canonical hosts, so the unmodified bundle
# talks to this gateway's /api subset with no injected configuration.
# ---------------------------------------------------------------------------
DYNAMIC_REPOSITORIES_TYPE = "forkmesh.dynamic-repositories"
MAX_DYNAMIC_REPOSITORY_ENTRIES = 1000
MAX_DASHBOARD_ASSET_BYTES = 8 * 1024 * 1024
DASHBOARD_ASSET_CACHE_ENTRIES = 24
DASHBOARD_ASSET_CACHE_BYTES = 12 * 1024 * 1024
DASHBOARD_CATALOG_CACHE_ENTRIES = 8
DASHBOARD_CATALOG_CACHE_BYTES = 8 * 1024 * 1024
# Long-lived process memory is the primary serving tier for immutable Git
# generations.  Entries remain valid until a repository/config/replica event
# explicitly advances the generation; these limits make an adversarial mix of
# valid public queries incapable of consuming unbounded RAM.
MEMORY_RESPONSE_CACHE_ENTRIES = 512
MEMORY_RESPONSE_CACHE_BYTES = 24 * 1024 * 1024
MEMORY_RESPONSE_CACHE_OBJECT_BYTES = 8 * 1024 * 1024
MEMORY_SESSION_CACHE_ENTRIES = 512
MEMORY_SESSION_CACHE_BYTES = 2 * 1024 * 1024
MEMORY_CACHE_ENTRY_OVERHEAD = 1024
RELEASE_DIGEST_MEMO_ENTRIES = 512
RELEASE_DIGEST_RECHECK_SECONDS = 3600
_REPOSITORY_CACHE_GENERATIONS = itertools.count(1)
DASHBOARD_CSP = (
    "default-src 'self'; base-uri 'self'; object-src 'none'; "
    "frame-ancestors 'none'; form-action 'self'; "
    "script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; "
    "style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data: blob: https:; font-src 'self' data:; "
    "connect-src 'self' https: wss:; media-src 'self' blob: https:; "
    "frame-src 'self' https:; worker-src 'self' blob:; "
    "manifest-src 'self'; upgrade-insecure-requests"
)
DASHBOARD_HTML_HEADERS = {
    "Content-Security-Policy": DASHBOARD_CSP,
    "Referrer-Policy": "strict-origin-when-cross-origin",
    "X-Frame-Options": "DENY",
    "Cache-Control": "public, max-age=300",
}
DASHBOARD_ASSET_HEADERS = {
    "Content-Security-Policy": DASHBOARD_CSP,
    "Referrer-Policy": "strict-origin-when-cross-origin",
    "Cache-Control": "public, max-age=300",
}
# Browser path -> exact blob path inside the assets repository.
DASHBOARD_ASSET_ROUTES = {
    "/dashboard.js": "app/public/dashboard.js",
    "/membership-prompt.js": "app/public/membership-prompt.js",
    "/dashboard-composer.js": "app/public/dashboard-composer.js",
    "/dashboard-composer.css": "app/public/dashboard-composer.css",
    "/api-client.js": "www/public/api-client.js",
    "/dashboard/tailwind.css": "app/public/dashboard/tailwind.css",
    "/404.html": "www/public/404.html",
    "/favicon.ico": "www/public/favicon/favicon.ico",
}
# Browser prefix -> candidate blob prefixes, searched in order.
DASHBOARD_ASSET_PREFIXES = (
    ("/favicon/", ("www/public/favicon/",)),
    ("/assets/", ("app/public/assets/", "www/public/assets/")),
)
DASHBOARD_PAGE_ROOT = "app/public/dashboard"
DASHBOARD_404_ASSET = "www/public/404.html"
DASHBOARD_REPO_ASSET = "app/public/dashboard/repo.html"
DASHBOARD_ASSET_PATH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,200}$")
DASHBOARD_BUILT_PREFIXES = ("app/public/", "www/public/")
DASHBOARD_VERSION_PLACEHOLDER_RE = re.compile(rb'\?v=\$\{version\}(["\'])')
DASHBOARD_PAGE_RE = re.compile(r"^[a-z][a-z0-9-]{0,40}$")
DASHBOARD_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".avif": "image/avif",
    ".webp": "image/webp",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".ttf": "font/ttf",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".webmanifest": "application/manifest+json",
    ".json": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".mp4": "video/mp4",
}
# First path segments the repo-page router must never claim. Mirrors the
# relay's static_routes.RESERVED_ROUTE_PREFIXES plus this gateway's own
# control routes.
DASHBOARD_RESERVED_SEGMENTS = frozenset({
    "api", "v1", "health", "healthz", "readyz", "dashboard", "assets",
    "favicon", "favicon.ico", "dashboard.js", "api-client.js", "analytics.js",
    "analytics-sdk.js",
    "404.html", "login", "signup", "logout", "notes", "world", "blog",
    "docs", "chat", "microjobs", "referral", "status", "install.sh",
    "uninstall.sh", "forkmesh-mirror.json", ".well-known", "ap", "rss.xml",
    "feed.xml", "robots.txt", "sitemap.xml", "terms", "privacy", "security",
    "about", "pricing",
})
ANALYTICS_STUB_BODY = b"/* analytics disabled on this mirror */\n"
GIT_OBJECT_ID_RE = re.compile(r"^[0-9a-f]{40}$")
RETIRED_ROOTS_KEPT = 2
RETIRED_ROOT_GRACE_SECONDS = 900
RUNTIME_DIRECTORY_RE = re.compile(
    r"^forkmesh-mirror-runtime-[A-Za-z0-9_]+$"
)
OPAQUE_REPLICA_RE = re.compile(r"^[0-9a-f]{64}$")
AGE_NATIVE_HEADER = b"age-encryption.org/v1\n"
AGE_ARMORED_HEADER = b"-----BEGIN AGE ENCRYPTED FILE-----\n"
COMMIT_RE = re.compile(r"^[0-9a-fA-F]{4,64}$")
BASE64URL_RE = re.compile(r"^[A-Za-z0-9_-]+$")
FORBIDDEN_CONFIG_KEY_RE = re.compile(
    r"(?:private.?key|secret|token|seed|mnemonic|keypair|password|credential)",
    re.IGNORECASE,
)
SENSITIVE_ENV_RE = re.compile(
    r"(?:SECRET|TOKEN|PRIVATE|SEED|MNEMONIC|KEYPAIR|PASSWORD|CREDENTIAL)",
    re.IGNORECASE,
)

ANALYZED_SOURCE_SUFFIXES = frozenset(
    {
        ".c", ".cc", ".cpp", ".cs", ".css", ".dart", ".go", ".h", ".hpp",
        ".java", ".js", ".jsx", ".kt", ".mjs", ".php", ".py", ".rb", ".rs",
        ".scss", ".swift", ".ts", ".tsx",
    }
)
COVERAGE_ARTIFACT_NAMES = frozenset(
    {
        "coverage.json",
        "coverage-final.json",
        "coverage-summary.json",
        "cobertura.xml",
        "jacoco.xml",
        "lcov.info",
    }
)


def _managed_branch(name: str) -> bool:
    return bool(
        MERGE_BASE_RE.fullmatch(name)
        and name not in ("HEAD", "agent/")
        and not name.startswith("forkmesh/")
    )


GIT_BRANCH_FORBIDDEN = frozenset(" ~^:?*[\\")
GIT_BRANCH_MAX_BYTES = 255


def _git_branch_name(name: str) -> bool:
    return bool(
        isinstance(name, str)
        and name
        and len(name.encode("utf-8")) <= GIT_BRANCH_MAX_BYTES
        and name not in ("HEAD", "@")
        and not name.startswith(("-", "/"))
        and not name.endswith(("/", "."))
        and "//" not in name
        and ".." not in name
        and "@{" not in name
        and not any(
            ord(ch) < 0x20 or ord(ch) == 0x7F or ch in GIT_BRANCH_FORBIDDEN
            for ch in name)
        and not any(
            part.startswith(".") or part.endswith(".lock")
            for part in name.split("/"))
    )


def _deletable_branch(name: str) -> bool:
    return (
        _git_branch_name(name)
        and name != "agent/"
        and not name.startswith("forkmesh/")
    )


def _lower_thread_priority(increment: int = 10) -> None:
    try:
        tid = threading.get_native_id()
        current = os.getpriority(os.PRIO_PROCESS, tid)
        os.setpriority(os.PRIO_PROCESS, tid, min(19, current + increment))
    except (AttributeError, OSError):
        pass


class GatewayError(RuntimeError):
    """Safe-to-display configuration or request failure."""


class PushRefused(GatewayError):
    pass


class GitError(RuntimeError):
    """Internal Git failure; its message never includes Git stderr or paths."""


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )


def _base64url_decode(value: str, expected_bytes: int | None = None) -> bytes:
    if not isinstance(value, str) or not BASE64URL_RE.fullmatch(value):
        raise GatewayError("value must be unpadded base64url")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except ValueError as exc:
        raise GatewayError("value is not valid base64url") from exc
    if expected_bytes is not None and len(decoded) != expected_bytes:
        raise GatewayError(f"value must contain exactly {expected_bytes} bytes")
    return decoded


def _base64url_encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def _assert_no_secret_fields(value: Any, context: str = "configuration") -> None:
    """Reject secret-shaped fields at every depth.

    Public-key and opaque key-reference names are deliberately allowed.  A key
    reference identifies an OS keychain/HSM entry; it is not key material.
    """

    allowed = {
        "publicKey",
        "routerPublicKey",
        "keyReference",
        "expectedRefsSha256",
        "ciphertextSha256",
        "sha256",
    }
    if isinstance(value, dict):
        for key, item in value.items():
            name = str(key)
            if name not in allowed and FORBIDDEN_CONFIG_KEY_RE.search(name):
                raise GatewayError(f"{context} contains a prohibited secret field")
            _assert_no_secret_fields(item, context)
    elif isinstance(value, list):
        for item in value:
            _assert_no_secret_fields(item, context)


def _safe_external_environment() -> dict[str, str]:
    """Return a local process environment with ambient credentials removed."""

    return {
        key: value
        for key, value in os.environ.items()
        if not SENSITIVE_ENV_RE.search(key)
        and key
        not in {
            "CLOUDFLARE_API_TOKEN",
            "CF_API_TOKEN",
            "CLOUDFLARE_TOKEN",
            "CF_TOKEN",
        }
    }


def _safe_merge_environment() -> dict[str, str]:
    """Minimal environment for the fixed refresh/merge controller."""
    environment = {
        "PATH": os.environ.get(
            "PATH",
            "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        ),
        "LANG": "C",
        "LC_ALL": "C",
    }
    temporary_raw = os.environ.get("TMPDIR", "")
    if temporary_raw:
        temporary = Path(temporary_raw)
        try:
            info = temporary.lstat()
        except OSError:
            info = None
        if (
            temporary.is_absolute()
            and temporary == Path(os.path.normpath(str(temporary)))
            and info is not None
            and stat.S_ISDIR(info.st_mode)
            and not stat.S_ISLNK(info.st_mode)
            and info.st_uid == os.geteuid()
            and not stat.S_IMODE(info.st_mode)
            & (stat.S_IRWXG | stat.S_IRWXO)
        ):
            environment["TMPDIR"] = str(temporary)
    return environment


def _validate_command(value: Any, label: str) -> tuple[str, ...]:
    if (
        not isinstance(value, list)
        or not value
        or any(not isinstance(part, str) or not part for part in value)
    ):
        raise GatewayError(f"{label} must be a non-empty JSON string array")
    for part in value:
        if re.search(
            r"(?:--?(?:token|secret|private[-_]?key|password)|"
            r"(?:token|secret|private[-_]?key|password)=)",
            part,
            re.IGNORECASE,
        ):
            raise GatewayError(f"{label} must not contain credential arguments")
    return tuple(value)


def _normalize_origin(value: Any) -> str:
    parsed = urlsplit(str(value or "").strip())
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
        or parsed.port not in (None, 443)
    ):
        raise GatewayError("publicOrigin must be a credential-free HTTPS origin")
    hostname = parsed.hostname.lower()
    netloc = "[" + hostname + "]" if ":" in hostname else hostname
    return "https://" + netloc


def _public_git_route(path: str) -> tuple[str, str, str] | None:
    """Parse /owner/repository.git Smart-HTTP read routes."""
    pieces = str(path or "").split("/")
    if len(pieces) == 5 and pieces[3:] == ["info", "refs"]:
        operation = "git-info-refs"
    elif len(pieces) == 4 and pieces[3] == "git-upload-pack":
        operation = "git-upload-pack"
    else:
        return None
    try:
        owner = unquote(pieces[1], errors="strict").lower()
        encoded_repository = unquote(pieces[2], errors="strict")
    except (UnicodeError, ValueError):
        return None
    if not encoded_repository.endswith(".git"):
        return None
    repository = encoded_repository[:-4]
    if not NODE_RE.fullmatch(owner) or not REPO_RE.fullmatch(repository):
        return None
    return owner, repository, operation


def _receive_pack_target(method: str, path: str) -> bool:
    """True for the one capability route whose body may exceed the base cap."""
    if str(method or "").upper() != "POST":
        return False
    pieces = str(path or "").split("/")
    if len(pieces) != 6 or pieces[1:3] != ["v1", "repositories"]:
        return False
    try:
        return unquote(pieces[5], errors="strict") == "git-receive-pack"
    except (UnicodeError, ValueError):
        return False


GITHUB_NOREPLY_RE = re.compile(
    r"^(?:([0-9]{1,12})\+)?([A-Za-z0-9](?:[A-Za-z0-9-]{0,38})(?:\[bot\])?)"
    r"@users\.noreply\.github\.com$", re.IGNORECASE)


def _github_noreply_avatar(email: str) -> str:
    match = GITHUB_NOREPLY_RE.match(str(email or "").strip())
    if not match:
        return ""
    if match.group(1):
        return "https://avatars.githubusercontent.com/u/%s?s=56&v=4" % match.group(1)
    login = match.group(2)
    if login.lower().endswith("[bot]"):
        return ""
    return "https://github.com/%s.png?size=56" % login


def _public_https_url(value: Any) -> str:
    """Return one bounded, credential-free public image URL."""
    raw = str(value or "").strip()
    if not raw or len(raw) > 500:
        return ""
    parsed = urlsplit(raw)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.fragment
    ):
        return ""
    return raw


def _is_loopback(value: str) -> bool:
    if value.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(value).is_loopback
    except ValueError:
        return False


def _safe_repo_path(value: Any, *, allow_empty: bool = True) -> str:
    text = str(value or "")
    if not text:
        if allow_empty:
            return ""
        raise GatewayError("repository path is required")
    if (
        len(text.encode("utf-8")) > 1024
        or text.startswith("/")
        or "\x00" in text
        or "\r" in text
        or "\n" in text
        or "\\" in text
        or ":" in text
    ):
        raise GatewayError("invalid repository path")
    parts = text.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise GatewayError("invalid repository path")
    return text


def _safe_ref(value: Any) -> str:
    text = str(value or "").strip() or "HEAD"
    if (
        len(text) > 200
        or text.startswith("-")
        or ".." in text
        or "@{" in text
        or text.endswith(".")
        or not re.fullmatch(r"[A-Za-z0-9._/-]+", text)
    ):
        raise GatewayError("invalid repository ref")
    return text


def _decode_git_body(
    data: bytes, content_encoding: str, max_bytes: int = MAX_REQUEST_BODY
) -> bytes:
    encodings = [
        item.strip().lower()
        for item in str(content_encoding or "").split(",")
        if item.strip()
    ]
    for encoding in reversed(encodings):
        if encoding == "identity":
            continue
        if encoding != "gzip":
            raise GatewayError("unsupported Git content encoding")
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(data)) as stream:
                data = stream.read(max_bytes + 1)
        except (EOFError, OSError) as exc:
            raise GatewayError("invalid compressed Git request") from exc
        if len(data) > max_bytes:
            raise GatewayError("expanded Git request is too large")
    return data


def request_message(
    node: str,
    method: str,
    target: str,
    body_sha256: str,
    request_id: str,
    issued_at: int,
) -> str:
    """Canonical capability message shared with ``edge_routing.py``."""

    node = str(node or "").strip().lower()
    method = str(method or "").strip().upper()
    target = str(target or "")
    body_sha256 = str(body_sha256 or "").strip().lower()
    request_id = str(request_id or "").strip()
    try:
        issued_at = int(issued_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(node)
        or method not in ("GET", "HEAD", "POST")
        or not target.startswith("/")
        or "\r" in target
        or "\n" in target
        or not SHA256_RE.fullmatch(body_sha256)
        or not REQUEST_ID_RE.fullmatch(request_id)
        or issued_at <= 0
    ):
        return ""
    return "\n".join(
        (
            "forkmesh-masked-proxy-v1",
            node,
            method,
            target,
            body_sha256,
            request_id,
            str(issued_at),
        )
    )


# ---------------------------------------------------------------------------
# Runner grants (ForkMesh Next Phase 5).
#
# An untrusted runner machine executes agent jobs for repositories this node
# serves. It never holds a node identity that can write here: the control
# plane hands it one router-signed grant per job, carried by native git in the
# X-ForkMesh-Runner-Grant header. A read grant clones one repository; a push
# grant moves exactly one refs/heads/agent/<session> ref of it, fast-forward
# only, and nothing else (no other branch, tag, or delete).
# ---------------------------------------------------------------------------
RUNNER_GRANT_HEADER = "x-forkmesh-runner-grant"
RUNNER_GRANT_MAX_TTL_MS = 3 * 60 * 60 * 1000
RUNNER_GRANT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")
RUNNER_AGENT_REF_REST_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,150}$")
RUNNER_BRANCH_REF_REST_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
RUNNER_GRANT_MAX_TOKEN = 4096
RUNNER_PUSH_ACCESS = ("push", "actions-push")
RUNNER_READ_ACCESS = ("read", "actions-push")
# A push grant names one ref, so one command is the most a valid push holds.
RUNNER_PUSH_MAX_COMMANDS = 1
RUNNER_PUSH_MAX_PREAMBLE_LINES = 64
GIT_OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")


def runner_agent_ref(value: Any) -> str:
    """The exact refs/heads/agent/<rest> a push grant may name, else ""."""
    ref = str(value or "")
    prefix = "refs/heads/agent/"
    if not ref.startswith(prefix):
        return ""
    rest = ref[len(prefix):]
    if (
        not RUNNER_AGENT_REF_REST_RE.fullmatch(rest)
        or ".." in rest
        or "//" in rest
        or "@{" in rest
        or rest.endswith(("/", ".lock", "."))
    ):
        return ""
    return ref


def runner_branch_ref(value: Any) -> str:
    ref = str(value or "")
    prefix = "refs/heads/"
    if not ref.startswith(prefix):
        return ""
    rest = ref[len(prefix):]
    if (
        not RUNNER_BRANCH_REF_REST_RE.fullmatch(rest)
        or ".." in rest
        or "//" in rest
        or "@{" in rest
        or rest.endswith(("/", ".lock", "."))
    ):
        return ""
    return ref


def runner_grant_message(
    node: str,
    owner: str,
    repository: str,
    access: str,
    ref: str,
    runner: str,
    grant_id: str,
    issued_at: int,
    expires_at: int,
) -> str:
    """Canonical runner grant; byte-compatible with ``edge_routing.py``."""
    node = str(node or "")
    owner = str(owner or "")
    repository = str(repository or "")
    access = str(access or "")
    ref = str(ref or "")
    runner = str(runner or "")
    grant_id = str(grant_id or "")
    try:
        issued_at = int(issued_at)
        expires_at = int(expires_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(node)
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or not NODE_RE.fullmatch(runner)
        or not RUNNER_GRANT_ID_RE.fullmatch(grant_id)
        or issued_at <= 0
        or not issued_at < expires_at <= issued_at + RUNNER_GRANT_MAX_TTL_MS
    ):
        return ""
    if access == "read":
        if ref:
            return ""
    elif access == "push":
        if runner_agent_ref(ref) != ref or not ref:
            return ""
    elif access == "actions-push":
        if runner_branch_ref(ref) != ref or not ref:
            return ""
    else:
        return ""
    return "\n".join(
        (
            "forkmesh-runner-grant-v1",
            node,
            owner,
            repository,
            access,
            ref,
            runner,
            grant_id,
            str(issued_at),
            str(expires_at),
        )
    )


@dataclass(frozen=True)
class RunnerGrant:
    node: str
    owner: str
    repository: str
    access: str
    ref: str
    runner: str
    grant_id: str
    issued_at: int
    expires_at: int
    message: str
    signature: str


def parse_runner_grant(token: Any) -> RunnerGrant:
    """Decode a grant token; the signature is verified by the caller."""
    token = str(token or "").strip()
    if not token or len(token) > RUNNER_GRANT_MAX_TOKEN or token.count(".") != 1:
        raise GatewayError("runner grant is invalid")
    encoded, signature = token.split(".", 1)
    _base64url_decode(signature, 64)
    try:
        message = _base64url_decode(encoded).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise GatewayError("runner grant is invalid") from exc
    fields = message.split("\n")
    if len(fields) != 10 or fields[0] != "forkmesh-runner-grant-v1":
        raise GatewayError("runner grant is invalid")
    if not all(item.isdigit() for item in fields[8:10]):
        raise GatewayError("runner grant is invalid")
    canonical = runner_grant_message(*fields[1:8], int(fields[8]), int(fields[9]))
    if not canonical or canonical != message:
        raise GatewayError("runner grant is invalid")
    return RunnerGrant(
        node=fields[1], owner=fields[2], repository=fields[3],
        access=fields[4], ref=fields[5], runner=fields[6],
        grant_id=fields[7], issued_at=int(fields[8]),
        expires_at=int(fields[9]), message=message, signature=signature,
    )


def _runner_git_route(path: str) -> tuple[str, str, str] | None:
    """Parse /v1/runner/<owner>/<repo>.git/{info/refs,git-*-pack}."""
    pieces = str(path or "").split("/")
    if len(pieces) < 6 or pieces[1:3] != ["v1", "runner"]:
        return None
    if len(pieces) == 7 and pieces[5:] == ["info", "refs"]:
        operation = "info-refs"
    elif len(pieces) == 6 and pieces[5] in ("git-upload-pack", "git-receive-pack"):
        operation = pieces[5]
    else:
        return None
    try:
        owner = unquote(pieces[3], errors="strict").lower()
        encoded_repository = unquote(pieces[4], errors="strict")
    except (UnicodeError, ValueError):
        return None
    if not encoded_repository.endswith(".git"):
        return None
    repository = encoded_repository[:-4]
    if not NODE_RE.fullmatch(owner) or not REPO_RE.fullmatch(repository):
        return None
    return owner, repository, operation


def receive_pack_commands(
    body: bytes, max_lines: int = RUNNER_PUSH_MAX_PREAMBLE_LINES
) -> list[tuple[str, str, str]]:
    """Ref update commands at the head of a receive-pack request.

    Reads pkt-lines up to the first flush: optional ``shallow <oid>`` lines,
    then ``<old> <new> <ref>`` commands (the first carrying capabilities
    after a NUL). Push certificates are refused outright; anything malformed
    raises ``GatewayError`` so the request never reaches git.
    """
    commands: list[tuple[str, str, str]] = []
    position = 0
    lines = 0
    while True:
        header = body[position:position + 4]
        if len(header) != 4:
            raise GatewayError("receive-pack request is truncated")
        try:
            length = int(header.decode("ascii"), 16)
        except (UnicodeDecodeError, ValueError) as exc:
            raise GatewayError("receive-pack request is malformed") from exc
        if not re.fullmatch(rb"[0-9a-f]{4}", header):
            raise GatewayError("receive-pack request is malformed")
        if length == 0:
            break
        if length < 5 or position + length > len(body):
            raise GatewayError("receive-pack request is malformed")
        lines += 1
        if lines > max_lines:
            raise GatewayError("receive-pack request is malformed")
        payload = body[position + 4:position + length]
        position += length
        if payload.endswith(b"\n"):
            payload = payload[:-1]
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise GatewayError("receive-pack request is malformed") from exc
        if not commands and text.startswith("shallow "):
            if not GIT_OID_RE.fullmatch(text[len("shallow "):]):
                raise GatewayError("receive-pack request is malformed")
            continue
        if text.startswith("push-cert"):
            raise GatewayError("push certificates are not accepted")
        command, _, capabilities = text.partition("\0")
        if commands and capabilities:
            raise GatewayError("receive-pack request is malformed")
        if any(
            item == "push-cert" or item.startswith("push-cert=")
            for item in capabilities.split(" ")
        ):
            raise GatewayError("push certificates are not accepted")
        parts = command.split(" ")
        if (
            len(parts) != 3
            or not GIT_OID_RE.fullmatch(parts[0])
            or not GIT_OID_RE.fullmatch(parts[1])
            or len(parts[0]) != len(parts[1])
            or not parts[2]
        ):
            raise GatewayError("receive-pack request is malformed")
        commands.append((parts[0], parts[1], parts[2]))
    return commands


PUSH_MAX_PREAMBLE_LINES = 4096
TAG_MOVES_QUERY_FIELD = "tagMoves"
MAX_TAG_MOVES = 8
PROTECT_QUERY_FIELD = "protect"
MAX_PROTECTED_REFS = 8
PUSH_PREFIX_QUERY_FIELD = "pushPrefix"
PUSH_PREFIX_ROOT = "refs/heads/pr/"
PUSH_PREFIX_ACCOUNT_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,62}$")
MAX_CONTRIBUTOR_BRANCHES = 32
REF_UPDATES_HEADER = "X-ForkMesh-Ref-Updates"
REF_UPDATES_MAX = 40
REF_UPDATE_COMMIT_COUNT_LIMIT = 10000


def parse_tag_moves(value: Any) -> dict[str, str]:
    text = str(value or "")
    moves: dict[str, str] = {}
    if not text:
        return moves
    items = text.split(" ")
    if len(items) > MAX_TAG_MOVES:
        raise GatewayError("tag move grant is invalid")
    for item in items:
        ref, separator, oid = item.rpartition(":")
        if (
            not separator
            or not ref.startswith("refs/tags/")
            or len(ref) <= len("refs/tags/")
            or ref in moves
            or not GIT_OID_RE.fullmatch(oid)
        ):
            raise GatewayError("tag move grant is invalid")
        try:
            if _safe_ref(ref) != ref:
                raise GatewayError("tag move grant is invalid")
        except GatewayError as exc:
            raise GatewayError("tag move grant is invalid") from exc
        moves[ref] = oid
    return moves


def parse_protected_refs(value: Any) -> list[str]:
    text = str(value or "")
    if not text:
        return []
    items = text.split(" ")
    if len(items) > MAX_PROTECTED_REFS:
        raise GatewayError("branch protection is invalid")
    refs = []
    for item in items:
        name = item[:-1] if item.endswith("/") else item
        if not name.startswith("refs/heads/") or len(name) <= len("refs/heads/"):
            raise GatewayError("branch protection is invalid")
        try:
            if _safe_ref(name) != name:
                raise GatewayError("branch protection is invalid")
        except GatewayError as exc:
            raise GatewayError("branch protection is invalid") from exc
        refs.append(item)
    return refs


def enforce_protected_refs(
    commands: list[tuple[str, str, str]], protected: list[str]
) -> None:
    for _old, _new, ref in commands:
        for item in protected:
            if ref == item or (item.endswith("/") and ref.startswith(item)):
                raise GatewayError(
                    "%s is protected: it changes only by merging an approved "
                    "pull request" % ref)


def parse_push_prefix(value: Any) -> str:
    text = str(value or "")
    if not text:
        return ""
    account = text[len(PUSH_PREFIX_ROOT):-1]
    if (
        not text.startswith(PUSH_PREFIX_ROOT)
        or not text.endswith("/")
        or not PUSH_PREFIX_ACCOUNT_RE.fullmatch(account)
        or ".." in account
        or account.endswith((".", ".lock"))
    ):
        raise GatewayError("contributor push grant is invalid")
    return text


def receive_pack_capabilities(body: bytes) -> set[str]:
    position = 0
    while len(body) >= position + 4:
        try:
            length = int(body[position:position + 4].decode("ascii"), 16)
        except (UnicodeDecodeError, ValueError):
            return set()
        if length < 5:
            return set()
        payload = body[position + 4:position + length]
        position += length
        if payload.startswith(b"shallow "):
            continue
        _command, _, capabilities = payload.rstrip(b"\n").partition(b"\0")
        return set(capabilities.decode("utf-8", "replace").split())
    return set()


def _pkt(data: bytes) -> bytes:
    return b"%04x" % (len(data) + 4) + data


def receive_pack_rejection(
    body: bytes, commands: list[tuple[str, str, str]], reason: str
) -> bytes:
    reason = re.sub(r"[^\x20-\x7e]", " ", str(reason or "refused"))[:200]
    report = _pkt(b"unpack ok\n") + b"".join(
        _pkt(("ng %s %s\n" % (ref, reason)).encode("utf-8", "replace"))
        for _old, _new, ref in commands
    ) + b"0000"
    capabilities = receive_pack_capabilities(body)
    if not capabilities & {"report-status", "report-status-v2"}:
        return b"0000"
    if not capabilities & {"side-band-64k", "side-band"}:
        return report
    chunk = 65515 if "side-band-64k" in capabilities else 995
    framed = b"".join(
        _pkt(b"\x01" + report[index:index + chunk])
        for index in range(0, len(report), chunk)
    )
    return framed + b"0000"


def enforce_push_prefix(
    commands: list[tuple[str, str, str]],
    prefix: str,
    existing: Mapping[str, str] | None = None,
) -> None:
    if not prefix:
        return
    for _old, _new, ref in commands:
        if not ref.startswith(prefix) or len(ref) <= len(prefix):
            raise PushRefused(
                "you may push only branches named %s<branch>; open a pull "
                "request from one" % prefix[len("refs/heads/"):])
    if existing is None:
        return
    branches = {ref for ref in existing if ref.startswith(prefix)}
    for _old, new, ref in commands:
        if set(new) == {"0"}:
            branches.discard(ref)
        else:
            branches.add(ref)
    if len(branches) > MAX_CONTRIBUTOR_BRANCHES:
        raise PushRefused(
            "a contributor may keep at most %d branches here; ask a "
            "maintainer to delete merged ones" % MAX_CONTRIBUTOR_BRANCHES)


def enforce_push_ref_policy(
    commands: list[tuple[str, str, str]], tag_moves: Mapping[str, str]
) -> None:
    for old, new, ref in commands:
        if not ref.startswith(("refs/heads/", "refs/tags/")):
            raise GatewayError("only branches and tags may be pushed")
        if not ref.startswith("refs/tags/") or set(old) == {"0"}:
            continue
        if set(new) == {"0"}:
            raise GatewayError("tags may not be deleted")
        if tag_moves.get(ref) != new:
            raise GatewayError(
                "moving an existing tag needs an owner's tag move")


def stable_refs(git_dir: Path) -> dict[str, str]:
    refs: dict[str, str] = {}
    for line in refs_canonical(git_dir).splitlines():
        oid, _, ref = line.partition(" ")
        if oid and ref:
            refs[ref] = oid
    return refs


def ref_transitions(
    before: Mapping[str, str], after: Mapping[str, str]
) -> list[tuple[str, str, str]]:
    changes = []
    for ref in sorted(set(before) | set(after)):
        old = before.get(ref, "")
        new = after.get(ref, "")
        if old != new:
            zero = "0" * len(new or old)
            changes.append((ref, old or zero, new or zero))
    return changes


def enforce_runner_push_scope(
    commands: list[tuple[str, str, str]],
    allowed_ref: str,
    access: str = "push",
    current: str | None = None,
) -> None:
    """Refuse a push unless it moves only the grant's one ref, forward."""
    if access == "actions-push":
        valid_ref = runner_branch_ref(allowed_ref)
    elif access == "push":
        valid_ref = runner_agent_ref(allowed_ref)
    else:
        raise GatewayError("runner push grant names no ref")
    if not allowed_ref or valid_ref != allowed_ref:
        raise GatewayError("runner push grant names no ref")
    if not commands or len(commands) > RUNNER_PUSH_MAX_COMMANDS:
        raise GatewayError("runner push must update exactly one ref")
    for old, new, ref in commands:
        if ref != allowed_ref:
            raise GatewayError("runner push names a ref outside its grant")
        if set(new) == {"0"}:
            raise GatewayError("runner push may not delete its ref")
        if access == "actions-push":
            if set(old) == {"0"} or not current:
                raise GatewayError("actions push may not create a branch")
            if old != current:
                raise GatewayError("actions push is not based on the tip")


# The desktop that deployed a host follows that host's whole journal through
# GET /v1/node/logs. Caddy serves it over HTTP/3, so the feed rides QUIC. Only
# Ed25519 keys listed in this file (one base64url key per line, written by the
# desktop's host installer) may open it; a host without the file has no feed.
NODE_LOGS_PATH = "/v1/node/logs"
NODE_LOG_VIEWER_KEYS_FILE = "/etc/forkmesh/log-viewer-keys"
NODE_LOG_VIEWER_KEYS_ENV = "FORKMESH_LOG_VIEWER_KEYS"
# Well inside the 30 s QUIC idle timeout Caddy (quic-go) and OpenSSL both
# default to, so an idle journal never lets the HTTP/3 connection lapse.
NODE_LOG_KEEPALIVE_SECONDS = 10
NODE_LOG_BACKLOG_LINES = 60
NODE_LOG_MAX_STREAMS = 4
PUBLIC_KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")


def node_logs_message(
    node: str, viewer_key: str, request_id: str, issued_at: int
) -> str:
    """The exact bytes a log viewer signs to open one node-log stream."""
    node = str(node or "").strip().lower()
    if (
        not NODE_RE.fullmatch(node)
        or not PUBLIC_KEY_RE.fullmatch(str(viewer_key or ""))
        or not REQUEST_ID_RE.fullmatch(str(request_id or ""))
        or int(issued_at) <= 0
    ):
        return ""
    return "\n".join(
        ("forkmesh-node-logs-v1", node, viewer_key, request_id, str(issued_at))
    )


def load_log_viewer_keys(path: str | Path | None = None) -> frozenset[str]:
    """Keys allowed to follow this host's logs; empty when none are listed."""
    source = Path(
        path or os.environ.get(NODE_LOG_VIEWER_KEYS_ENV) or NODE_LOG_VIEWER_KEYS_FILE
    )
    try:
        text = source.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return frozenset()
    return frozenset(
        line.strip()
        for line in text.splitlines()
        if PUBLIC_KEY_RE.fullmatch(line.strip())
    )


def _verify_ed25519(public_key: str, message: str, signature: str) -> bool:
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PublicKey as _Ed25519PublicKey,
        )

        _Ed25519PublicKey.from_public_bytes(
            _base64url_decode(public_key, 32)
        ).verify(_base64url_decode(signature, 64), message.encode("utf-8"))
        return True
    except Exception:
        return False


def health_challenge(node: str, nonce: str, issued_at: int) -> str:
    node = str(node or "").strip().lower()
    nonce = str(nonce or "").strip()
    try:
        issued_at = int(issued_at)
    except (TypeError, ValueError):
        return ""
    if (
        not NODE_RE.fullmatch(node)
        or not re.fullmatch(r"[A-Za-z0-9_-]{16,128}", nonce)
        or issued_at <= 0
    ):
        return ""
    return "\n".join(
        ("forkmesh-https-health-v1", node, nonce, str(issued_at))
    )


def repository_health_challenge(
    node: str,
    nonce: str,
    issued_at: int,
    owner: str,
    repository: str,
    *,
    available: bool,
    integrity: str,
    refs_digest: str,
    operations_digest: str,
) -> str:
    """Canonical signed proof for one explicitly requested public repository."""

    base = health_challenge(node, nonce, issued_at)
    owner = str(owner or "").strip().lower()
    repository = str(repository or "").strip()
    integrity = str(integrity or "").strip().lower()
    refs_digest = str(refs_digest or "").strip().lower()
    operations_digest = str(operations_digest or "").strip().lower()
    if (
        not base
        or not NODE_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or integrity not in ("ok", "unavailable")
        or not SHA256_RE.fullmatch(refs_digest)
        or not SHA256_RE.fullmatch(operations_digest)
        or (available and integrity != "ok")
        or (not available and integrity != "unavailable")
    ):
        return ""
    return "\n".join(
        (
            "forkmesh-https-health-repository-v1",
            node,
            nonce,
            str(int(issued_at)),
            owner,
            repository,
            "1" if available else "0",
            integrity,
            refs_digest,
            operations_digest,
        )
    )


class ExternalJSONCommand:
    """Direct, shell-free JSON adapter with redacted errors and bounded output."""

    def __init__(
        self,
        command: Iterable[str],
        *,
        timeout: int = 15,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self.command = tuple(command)
        if not self.command:
            raise GatewayError("external command is required")
        self.timeout = timeout
        self._runner = runner

    def call(self, request: dict[str, Any]) -> dict[str, Any]:
        try:
            completed = self._runner(
                list(self.command),
                input=_canonical_json(request) + "\n",
                text=True,
                capture_output=True,
                env=_safe_external_environment(),
                timeout=self.timeout,
                check=True,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise GatewayError("external identity helper failed") from exc
        if len(completed.stdout.encode("utf-8", "replace")) > 16 * 1024:
            raise GatewayError("external identity helper returned too much data")
        try:
            response = json.loads(completed.stdout)
        except (json.JSONDecodeError, TypeError) as exc:
            raise GatewayError("external identity helper returned invalid JSON") from exc
        if not isinstance(response, dict):
            raise GatewayError("external identity helper returned a non-object")
        _assert_no_secret_fields(response, "external identity helper response")
        return response


class ExternalCapabilityVerifier:
    def __init__(
        self,
        command: Iterable[str],
        public_key: str,
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self.public_key = public_key
        _base64url_decode(public_key, 32)
        self.helper = ExternalJSONCommand(command, runner=runner)

    def verify(self, message: str, signature: str) -> bool:
        try:
            _base64url_decode(signature, 64)
            payload = message.encode("utf-8")
            response = self.helper.call(
                {
                    "schemaVersion": 1,
                    "type": "forkmesh.request-capability-verification",
                    "algorithm": "Ed25519",
                    "encoding": "base64url-no-padding",
                    "publicKey": self.public_key,
                    "messageBase64": _base64url_encode(payload),
                    "messageSha256": hashlib.sha256(payload).hexdigest(),
                    "signature": signature,
                }
            )
            return (
                response.get("valid") is True
                and response.get("publicKey", self.public_key) == self.public_key
            )
        except GatewayError:
            return False


class ExternalHealthSigner:
    def __init__(
        self,
        command: Iterable[str],
        public_key: str,
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self.public_key = public_key
        _base64url_decode(public_key, 32)
        self.helper = ExternalJSONCommand(command, runner=runner)

    def sign(self, message: str) -> str:
        payload = message.encode("utf-8")
        response = self.helper.call(
            {
                "schemaVersion": 1,
                "type": "forkmesh.health-challenge-signing",
                "algorithm": "Ed25519",
                "encoding": "base64url-no-padding",
                "publicKey": self.public_key,
                "messageBase64": _base64url_encode(payload),
                "messageSha256": hashlib.sha256(payload).hexdigest(),
            }
        )
        if response.get("publicKey", self.public_key) != self.public_key:
            raise GatewayError("health signer used a different public key")
        signature = str(response.get("signature") or "")
        _base64url_decode(signature, 64)
        return signature


def _merge_attestation_fields(value: Mapping[str, Any]) -> dict[str, str]:
    """Project the executor's optional node-signed refs attestation.

    ``state`` is the sha256 refs digest this node serves after the mutation,
    ``stateTs`` the signing time in epoch milliseconds and ``stateSig`` the
    node identity's Ed25519 signature over the same ``forkmesh-repostate-v1``
    canonical the catalog and collaboration drains sign. The gateway never
    signs anything itself here; a malformed or absent attestation is simply
    omitted, and the relay then falls back to its ordinary pin rules.
    """
    state = str(value.get("state") or "").strip().lower()
    ts = str(value.get("stateTs") or "").strip()
    signature = str(value.get("stateSig") or "").strip()
    if (
        not SHA256_RE.fullmatch(state)
        or not re.fullmatch(r"[1-9][0-9]{0,18}", ts)
        or not MERGE_ATTESTATION_SIGNATURE_RE.fullmatch(signature)
    ):
        return {}
    return {"state": state, "stateTs": ts, "stateSig": signature}


def _merge_attestation_stale(value: Mapping[str, Any], now_ms: int) -> bool:
    """True when a published result's attestation should be re-signed."""
    fields = _merge_attestation_fields(value)
    if not fields:
        return False
    try:
        age_ms = int(now_ms) - int(fields["stateTs"])
    except (TypeError, ValueError):
        return True
    return age_ms > MERGE_ATTESTATION_REFRESH_SECONDS * 1000 or age_ms < 0


class ExternalMergeExecutor:
    """Fixed, operator-owned merge command; repository input stays typed JSON."""

    def __init__(
        self,
        command: Iterable[str],
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self.command = tuple(command)
        if not self.command:
            raise GatewayError("merge executor is not configured")
        self._runner = runner

    def call(
        self,
        request: dict[str, Any],
        *,
        timeout: int = 20,
        max_bytes: int = 16 * 1024,
    ) -> dict[str, Any]:
        try:
            completed = self._runner(
                list(self.command),
                input=_canonical_json(request) + "\n",
                text=True,
                capture_output=True,
                env=_safe_merge_environment(),
                timeout=timeout,
                check=True,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise GatewayError("merge executor failed") from exc
        if len(completed.stdout.encode("utf-8", "replace")) > max_bytes:
            raise GatewayError("merge executor returned too much data")
        try:
            response = json.loads(completed.stdout)
        except (json.JSONDecodeError, TypeError) as exc:
            raise GatewayError("merge executor returned invalid JSON") from exc
        if not isinstance(response, dict):
            raise GatewayError("merge executor returned a non-object")
        _assert_no_secret_fields(response, "merge executor response")
        return response


@dataclass(frozen=True)
class EncryptedArchive:
    scheme: str
    ciphertext_path: Path
    ciphertext_sha256: str
    key_reference: str
    materialize_command: tuple[str, ...]


@dataclass(frozen=True)
class RepositoryConfig:
    owner: str
    name: str
    visibility: str
    enabled: bool
    git_dir: Path | None
    release_store: Path | None
    expected_refs_sha256: str
    operations: frozenset[str]
    encrypted_archive: EncryptedArchive | None = None
    # Node-created fork registry entries carry no owner-signed integrity pin;
    # they trust their startup ref generation instead (see check_integrity).
    dynamic: bool = False


@dataclass(frozen=True)
class GatewayConfig:
    config_path: Path
    generation_sha256: str
    node: str
    public_key: str
    router_public_key: str
    public_origin: str
    listen_host: str
    listen_port: int
    manifest_path: Path
    verifier_command: tuple[str, ...]
    health_signer_command: tuple[str, ...]
    merge_executor_command: tuple[str, ...]
    repositories: tuple[RepositoryConfig, ...]
    actions_summary_path: Path | None = None
    actions_control_path: Path | None = None
    max_release_bytes: int = 2 * 1024 * 1024 * 1024
    private_replica_store: Path | None = None
    max_private_replica_bytes: int = 512 * 1024 * 1024
    dashboard_enabled: bool = False
    dashboard_assets_repository: tuple[str, str] | None = None
    dashboard_canonical_origin: str = ""
    dashboard_ssh_origin: str = ""
    dashboard_replica_path: Path | None = None
    dashboard_assets_root: Path | None = None
    collaboration_control_url: str = ""
    dynamic_repositories_path: Path | None = None
    dynamic_repositories_skipped: int = 0
    writable_repository_root: Path | None = None
    writable_owners: frozenset[str] = frozenset()
    receive_max_input_bytes: int = DEFAULT_RECEIVE_MAX_INPUT_BYTES
    operator_policy_path: Path | None = None
    operator_policy_state: Path | None = None


def _atomic_write_json(path: Path, value: Any) -> None:
    """Write owner-only JSON via a same-directory rename (crash-safe)."""
    raw = (
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    temporary = path.with_name(
        "." + path.name + "." + _base64url_encode(os.urandom(9)))
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temporary, flags, 0o600)
    try:
        view = memoryview(raw)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError("short write")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    try:
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _read_hosted_pushes(path: Path) -> list[dict[str, Any]]:
    """Read the hosted-pushes sidecar fail-closed, or return no entries.

    The sidecar is machine state this process writes itself, so it is read
    through a no-follow descriptor, bounded to the size the write path can
    produce, and required to carry the exact envelope. Anything else - a
    symlink, an oversized or truncated document, a foreign type, an unknown
    field - yields no repositories at all rather than a partial view.
    """
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError:
        return []
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_size > MAX_HOSTED_PUSHES_BYTES
            or info.st_uid != os.geteuid()
        ):
            return []
        raw = os.read(descriptor, MAX_HOSTED_PUSHES_BYTES + 1)
    except OSError:
        return []
    finally:
        os.close(descriptor)
    if len(raw) > MAX_HOSTED_PUSHES_BYTES:
        return []
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return []
    items = value.get("repositories") if isinstance(value, dict) else None
    if (
        not isinstance(value, dict)
        or set(value) - {"schemaVersion", "type", "repositories"}
        or value.get("schemaVersion") != SCHEMA_VERSION
        or value.get("type") != HOSTED_PUSHES_TYPE
        or not isinstance(items, list)
        or len(items) > MAX_HOSTED_PUSH_REPOSITORIES
        or any(not isinstance(item, dict) for item in items)
    ):
        return []
    return list(items)


def _load_json_file(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GatewayError(f"{label} is not readable valid JSON") from exc
    if not isinstance(value, dict):
        raise GatewayError(f"{label} must be a JSON object")
    return value


def validate_manifest(
    manifest: dict[str, Any],
    *,
    node: str,
    public_key: str,
    public_origin: str,
) -> None:
    _assert_no_secret_fields(manifest, "mirror manifest")
    signature = manifest.get("signature")
    endpoint = manifest.get("endpoint")
    manifest_node = manifest.get("node")
    if (
        manifest.get("schemaVersion") != 1
        or manifest.get("type") != "forkmesh.mirror-endpoint"
        or not isinstance(signature, dict)
        or signature.get("algorithm") != "Ed25519"
        or signature.get("encoding") != "base64url-no-padding"
        or not isinstance(endpoint, dict)
        or endpoint.get("origin") != public_origin
        or endpoint.get("healthUrl") != public_origin + "/health"
        or endpoint.get("manifestUrl")
        != public_origin + "/forkmesh-mirror.json"
        or endpoint.get("transport") != "direct-https"
        or endpoint.get("mainProxyMode") not in {"masked", "direct"}
        or not isinstance(manifest_node, dict)
        or manifest_node.get("name") != node
        or manifest_node.get("publicKey") != public_key
    ):
        raise GatewayError("mirror manifest does not match gateway identity/origin")
    _base64url_decode(str(signature.get("value") or ""), 64)
    unsigned = {key: value for key, value in manifest.items() if key != "signature"}
    payload_hash = hashlib.sha256(
        _canonical_json(unsigned).encode("utf-8")
    ).hexdigest()
    if signature.get("payloadSha256") != payload_hash:
        raise GatewayError("mirror manifest payload digest is invalid")


def _actions_parent_metadata_allowed(
    info: os.stat_result,
    *,
    effective_uid: int,
    effective_gid: int,
) -> bool:
    """Require the gateway's exact private service-owned handoff directory."""

    return (
        stat.S_ISDIR(info.st_mode)
        and info.st_uid == effective_uid
        and info.st_gid == effective_gid
        and stat.S_IMODE(info.st_mode) == 0o700
    )


def _actions_summary_metadata_allowed(
    path: Path,
    info: os.stat_result,
    *,
    effective_uid: int,
    effective_gid: int,
) -> bool:
    """Accept only same-account 0600 or the fixed root-to-gateway handoff."""

    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_nlink != 1
        or not 0 < info.st_size <= MAX_ACTIONS_SUMMARY_BYTES
    ):
        return False
    mode = stat.S_IMODE(info.st_mode)
    if info.st_uid == effective_uid:
        return mode == 0o600
    return (
        effective_uid != 0
        and path == SYSTEM_ACTIONS_SUMMARY_PATH
        and info.st_uid == 0
        and info.st_gid == effective_gid
        and mode == 0o640
    )


def load_config(path: Path) -> GatewayConfig:
    try:
        config_raw = path.read_bytes()
        source = json.loads(config_raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GatewayError(
            "gateway configuration is not readable valid JSON") from exc
    if not isinstance(source, dict):
        raise GatewayError("gateway configuration must be a JSON object")
    _assert_no_secret_fields(source)
    if set(source) - {
        "schemaVersion",
        "node",
        "routerPublicKey",
        "publicOrigin",
        "listen",
        "manifestPath",
        "requestVerifierCommand",
        "healthSignerCommand",
        "collaborationControlUrl",
        "mergeExecutorCommand",
        "actionsSummaryPath",
        "actionsControlPath",
        "repositories",
        "privateReplicaStore",
        "writableRepositoryRoot",
        "writableOwners",
        "operatorPolicy",
        "limits",
        "dashboard",
        "dynamicRepositoriesPath",
    }:
        raise GatewayError("gateway configuration contains an unknown field")
    if source.get("schemaVersion") != SCHEMA_VERSION:
        raise GatewayError("unsupported gateway configuration schemaVersion")
    base = path.resolve().parent
    node_value = source.get("node")
    if not isinstance(node_value, dict):
        raise GatewayError("node must be an object")
    if set(node_value) != {"name", "publicKey"}:
        raise GatewayError("node contains an unknown or missing field")
    node = str(node_value.get("name") or "").strip().lower()
    if not NODE_RE.fullmatch(node):
        raise GatewayError("node.name is invalid")
    public_key = str(node_value.get("publicKey") or "")
    router_public_key = str(source.get("routerPublicKey") or "")
    _base64url_decode(public_key, 32)
    _base64url_decode(router_public_key, 32)
    public_origin = _normalize_origin(source.get("publicOrigin"))

    listen = source.get("listen")
    if not isinstance(listen, dict):
        raise GatewayError("listen must be an object")
    if set(listen) != {"host", "port"}:
        raise GatewayError("listen contains an unknown or missing field")
    listen_host = str(listen.get("host") or "127.0.0.1")
    if not _is_loopback(listen_host):
        raise GatewayError("gateway must listen on loopback behind a TLS proxy")
    try:
        listen_port = int(listen.get("port"))
    except (TypeError, ValueError) as exc:
        raise GatewayError("listen.port is invalid") from exc
    if not 1 <= listen_port <= 65535:
        raise GatewayError("listen.port is outside 1..65535")

    collaboration_control_url = str(
        source.get("collaborationControlUrl") or ""
    ).strip()
    if collaboration_control_url:
        try:
            control = urlsplit(collaboration_control_url)
            control_port = control.port
        except ValueError as exc:
            raise GatewayError(
                "collaborationControlUrl is invalid"
            ) from exc
        if (
            control.scheme != "http"
            or not control.hostname
            or not _is_loopback(control.hostname)
            or control.username
            or control.password
            or control_port is None
            or not 1 <= control_port <= 65535
            or control.path != "/v1/control/collaboration"
            or control.query
            or control.fragment
        ):
            raise GatewayError(
                "collaborationControlUrl must be the loopback collaboration endpoint"
            )

    manifest_raw = source.get("manifestPath")
    if not isinstance(manifest_raw, str) or not manifest_raw:
        raise GatewayError("manifestPath is required")
    manifest_path = (base / manifest_raw).resolve()
    validate_manifest(
        _load_json_file(manifest_path, "mirror manifest"),
        node=node,
        public_key=public_key,
        public_origin=public_origin,
    )

    verifier = _validate_command(
        source.get("requestVerifierCommand"), "requestVerifierCommand"
    )
    signer = _validate_command(
        source.get("healthSignerCommand"), "healthSignerCommand"
    )
    merge_executor_raw = source.get("mergeExecutorCommand")
    merge_executor = (
        _validate_command(merge_executor_raw, "mergeExecutorCommand")
        if merge_executor_raw is not None
        else ()
    )
    actions_summary_path = None
    actions_summary_raw = source.get("actionsSummaryPath")
    if actions_summary_raw is not None:
        if not isinstance(actions_summary_raw, str) or not actions_summary_raw:
            raise GatewayError("actionsSummaryPath must be a path string")
        actions_summary_path = Path(actions_summary_raw)
        if (
            not actions_summary_path.is_absolute()
            or actions_summary_path
            != Path(os.path.normpath(actions_summary_raw))
            or actions_summary_path.name != "actions-summary.json"
        ):
            raise GatewayError(
                "actionsSummaryPath must be an absolute normalized summary path"
            )
        parent = actions_summary_path.parent
        try:
            parent_info = parent.lstat()
            parent_is_real = parent.resolve(strict=True) == parent
        except (OSError, RuntimeError) as exc:
            raise GatewayError(
                "actionsSummaryPath must have a protected real parent"
            ) from exc
        if not parent_is_real or not _actions_parent_metadata_allowed(
            parent_info,
            effective_uid=os.geteuid(),
            effective_gid=os.getegid(),
        ):
            raise GatewayError(
                "actionsSummaryPath must have a protected real parent")
        try:
            target_info = actions_summary_path.lstat()
        except FileNotFoundError:
            target_info = None
        except OSError as exc:
            raise GatewayError(
                "actionsSummaryPath cannot be inspected safely") from exc
        if target_info is not None:
            if stat.S_ISLNK(target_info.st_mode):
                raise GatewayError(
                    "actionsSummaryPath must not be a symbolic link")
    actions_control_path = None
    actions_control_raw = source.get("actionsControlPath")
    if actions_control_raw is not None:
        if not isinstance(actions_control_raw, str) or not actions_control_raw:
            raise GatewayError("actionsControlPath must be a path string")
        actions_control_path = Path(actions_control_raw)
        if (
            not actions_control_path.is_absolute()
            # Compared as text: Path() would quietly collapse "/./" and "//".
            or actions_control_raw != os.path.normpath(actions_control_raw)
            or actions_control_path.name != ACTIONS_CONTROL_DIRECTORY
        ):
            raise GatewayError(
                "actionsControlPath must be an absolute normalized "
                "actions-control directory"
            )
        try:
            control_info = actions_control_path.lstat()
            control_is_real = (
                actions_control_path.resolve(strict=True)
                == actions_control_path
            )
        except (OSError, RuntimeError) as exc:
            raise GatewayError(
                "actionsControlPath must be a protected real directory"
            ) from exc
        if not control_is_real or not _actions_parent_metadata_allowed(
            control_info,
            effective_uid=os.geteuid(),
            effective_gid=os.getegid(),
        ):
            raise GatewayError(
                "actionsControlPath must be a protected real directory")
    limits = source.get("limits", {})
    if not isinstance(limits, dict) or set(limits) - {
        "maxReleaseBytes", "maxPrivateReplicaBytes", "receiveMaxInputBytes"
    }:
        raise GatewayError("limits contains an unknown field")
    try:
        max_release = int(
            limits.get(
                "maxReleaseBytes", 2 * 1024 * 1024 * 1024
            )
        )
    except (AttributeError, TypeError, ValueError) as exc:
        raise GatewayError("limits.maxReleaseBytes is invalid") from exc
    if not 1 <= max_release <= 16 * 1024 * 1024 * 1024:
        raise GatewayError("limits.maxReleaseBytes exceeds the safe range")
    try:
        max_private_replica = int(
            limits.get("maxPrivateReplicaBytes", 512 * 1024 * 1024)
        )
    except (AttributeError, TypeError, ValueError) as exc:
        raise GatewayError(
            "limits.maxPrivateReplicaBytes is invalid") from exc
    if not 1 <= max_private_replica <= 2 * 1024 * 1024 * 1024:
        raise GatewayError(
            "limits.maxPrivateReplicaBytes exceeds the safe range")
    try:
        receive_max_input = int(
            limits.get("receiveMaxInputBytes", DEFAULT_RECEIVE_MAX_INPUT_BYTES))
    except (AttributeError, TypeError, ValueError) as exc:
        raise GatewayError("limits.receiveMaxInputBytes is invalid") from exc
    if not 1024 * 1024 <= receive_max_input <= 4 * 1024 * 1024 * 1024:
        raise GatewayError(
            "limits.receiveMaxInputBytes exceeds the safe range")

    writable_repository_root = None
    writable_root_raw = source.get("writableRepositoryRoot")
    if writable_root_raw is not None:
        if not isinstance(writable_root_raw, str) or not writable_root_raw:
            raise GatewayError("writableRepositoryRoot must be a path string")
        writable_repository_root = Path(writable_root_raw)
        try:
            root_info = writable_repository_root.lstat()
            root_resolved = writable_repository_root.resolve(strict=True)
        except OSError as exc:
            raise GatewayError(
                "writableRepositoryRoot must be an existing directory"
            ) from exc
        if (
            not writable_repository_root.is_absolute()
            or writable_repository_root
            != Path(os.path.normpath(writable_root_raw))
            or root_resolved != writable_repository_root
            or not stat.S_ISDIR(root_info.st_mode)
            or stat.S_ISLNK(root_info.st_mode)
            or root_info.st_uid != os.geteuid()
            or stat.S_IMODE(root_info.st_mode)
            & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            raise GatewayError(
                "writableRepositoryRoot must be an owner-controlled real "
                "absolute directory"
            )
    writable_owners_raw = source.get("writableOwners", [])
    if not isinstance(writable_owners_raw, list):
        raise GatewayError("writableOwners must be an array of owner names")
    writable_owners: list[str] = []
    for raw_owner in writable_owners_raw:
        if not isinstance(raw_owner, str):
            raise GatewayError(
                "writableOwners must be an array of owner names")
        owner_name = raw_owner.strip().lower()
        if owner_name == WRITABLE_OWNERS_WILDCARD:
            # Exclusive on purpose: a mixed ["*", "alice"] list reads as
            # narrower than it is, so refuse it rather than silently widen.
            if len(writable_owners_raw) != 1:
                raise GatewayError(
                    "writableOwners wildcard must be the only entry")
            writable_owners.append(owner_name)
            continue
        if not NODE_RE.fullmatch(owner_name) or owner_name in writable_owners:
            raise GatewayError(
                "writableOwners contains an invalid or duplicate owner")
        writable_owners.append(owner_name)
    if writable_owners and writable_repository_root is None:
        raise GatewayError("writableOwners requires writableRepositoryRoot")

    operator_policy_path: Path | None = None
    operator_policy_state: Path | None = None
    operator_policy_raw = source.get("operatorPolicy")
    if operator_policy_raw is not None:
        if node_operator_policy is None:
            raise GatewayError(
                "operatorPolicy requires node_operator_policy.py beside the "
                "gateway")
        if (
            not isinstance(operator_policy_raw, dict)
            or set(operator_policy_raw) != {"policyPath", "stateDirectory"}
            or not all(
                isinstance(value, str) and os.path.isabs(value)
                for value in operator_policy_raw.values()
            )
        ):
            raise GatewayError(
                "operatorPolicy must name absolute policyPath and "
                "stateDirectory")
        operator_policy_path = Path(operator_policy_raw["policyPath"])
        operator_policy_state = Path(operator_policy_raw["stateDirectory"])
        try:
            # Validate now so --check refuses a policy the hook would reject.
            node_operator_policy.load_policy(operator_policy_path)
            node_operator_policy._require_state_directory(
                operator_policy_state)
        except (node_operator_policy.PolicyError, OSError) as exc:
            raise GatewayError("operatorPolicy is invalid: %s" % exc) from exc

    private_replica_store = None
    private_store_raw = source.get("privateReplicaStore")
    if private_store_raw is not None:
        if not isinstance(private_store_raw, str) or not private_store_raw:
            raise GatewayError("privateReplicaStore must be a path string")
        private_replica_store = (base / private_store_raw).resolve()
        if (
            not private_replica_store.is_dir()
            or private_replica_store.is_symlink()
        ):
            raise GatewayError(
                "privateReplicaStore must be an existing real directory")
        mode = stat.S_IMODE(private_replica_store.stat().st_mode)
        if mode & (stat.S_IRWXG | stat.S_IRWXO):
            raise GatewayError(
                "privateReplicaStore must be accessible only to its owner")

    repositories_raw = source.get("repositories")
    if not isinstance(repositories_raw, list):
        raise GatewayError("repositories must be an array")
    repositories: list[RepositoryConfig] = []
    identities: set[tuple[str, str]] = set()
    for item in repositories_raw:
        if not isinstance(item, dict):
            raise GatewayError("repository entry must be an object")
        owner = str(item.get("owner") or "").strip().lower()
        name = str(item.get("name") or "").strip()
        if not NODE_RE.fullmatch(owner) or not REPO_RE.fullmatch(name):
            raise GatewayError("repository owner/name is invalid")
        identity = (owner, name.lower())
        if identity in identities:
            raise GatewayError("repository entries must be unique")
        identities.add(identity)
        visibility = str(item.get("visibility") or "private").lower()
        # "restricted" is a plaintext repository the node holds for the
        # control plane (a ForkMesh-hosted private repository, docs/
        # HOSTED_REPOS.md): configured exactly like a public one, but served
        # only on router-capability routes and runner grants, never on the
        # unauthenticated /v1/public, /<owner>/<repo>.git or dashboard routes.
        if visibility not in ("public", "private", "restricted"):
            raise GatewayError(
                "repository visibility must be public, private or restricted")
        enabled = item.get("enabled") is True
        if visibility == "private":
            if enabled:
                raise GatewayError("private repositories cannot be enabled publicly")
            if set(item) - {"owner", "name", "visibility", "enabled"}:
                raise GatewayError(
                    "private repository entry may contain identity fields only"
                )
            # Do not inspect or materialize private repository locations.
            repositories.append(
                RepositoryConfig(
                    owner,
                    name,
                    visibility,
                    False,
                    None,
                    None,
                    "",
                    frozenset(),
                )
            )
            continue
        if not enabled:
            if set(item) - {"owner", "name", "visibility", "enabled"}:
                raise GatewayError(
                    "disabled public repository may contain identity fields only"
                )
            repositories.append(
                RepositoryConfig(
                    owner,
                    name,
                    visibility,
                    False,
                    None,
                    None,
                    "",
                    frozenset(),
                )
            )
            continue

        integrity = item.get("integrity")
        if not isinstance(integrity, dict) or set(integrity) != {
            "expectedRefsSha256"
        }:
            raise GatewayError("repository integrity metadata is invalid")
        expected_refs = (
            str(integrity.get("expectedRefsSha256") or "").lower()
            if isinstance(integrity, dict)
            else ""
        )
        if not SHA256_RE.fullmatch(expected_refs):
            raise GatewayError(
                "enabled public repository requires integrity.expectedRefsSha256"
            )
        # A missing capability list keeps the historical read-only surface.
        # Write operations are never enabled implicitly.
        operations_raw = item.get(
            "operations", sorted(DEFAULT_PUBLIC_OPERATIONS)
        )
        if (
            not isinstance(operations_raw, list)
            or not operations_raw
            or any(op not in PUBLIC_OPERATIONS for op in operations_raw)
        ):
            raise GatewayError("repository operations contain an unsupported value")
        operations = frozenset(operations_raw)
        if "merge-pull" in operations and not merge_executor:
            raise GatewayError(
                "merge-pull requires an operator-owned mergeExecutorCommand"
            )
        if "update-pull-branch" in operations and not merge_executor:
            raise GatewayError(
                "update-pull-branch requires an operator-owned "
                "mergeExecutorCommand"
            )
        for branch_operation in ("merge-agent-branch", "delete-agent-branch"):
            if branch_operation in operations and not merge_executor:
                raise GatewayError(
                    branch_operation
                    + " requires an operator-owned mergeExecutorCommand"
                )
        if "actions-status" in operations and actions_summary_path is None:
            raise GatewayError(
                "actions-status requires a protected actionsSummaryPath"
            )
        if "actions-control" in operations:
            # A control request is only useful where the same executor
            # publishes the summary that later reports its outcome.
            if "actions-status" not in operations:
                raise GatewayError(
                    "actions-control requires actions-status")
            if actions_control_path is None:
                raise GatewayError(
                    "actions-control requires a protected actionsControlPath"
                )

        git_dir: Path | None = None
        encrypted_archive: EncryptedArchive | None = None
        allowed_repository_fields = {
            "owner",
            "name",
            "forkedFrom",
            "visibility",
            "enabled",
            "integrity",
            "operations",
            "releaseStore",
            "gitDir",
        }
        if set(item) - allowed_repository_fields:
            raise GatewayError("repository entry contains an unknown field")
        forked_from = str(item.get("forkedFrom") or "").strip().lower()
        if forked_from and not re.fullmatch(
            r"[A-Za-z0-9._:-]{1,80}/[A-Za-z0-9._:-]{1,80}",
            forked_from,
        ):
            raise GatewayError("repository forkedFrom is invalid")
        git_dir_raw = item.get("gitDir")
        if not isinstance(git_dir_raw, str) or not git_dir_raw:
            raise GatewayError(
                "enabled public repository requires plaintext gitDir storage"
            )
        supplied_git_dir = Path(git_dir_raw)
        if not supplied_git_dir.is_absolute():
            raise GatewayError("public gitDir must be an absolute path")
        try:
            supplied_info = supplied_git_dir.lstat()
            git_dir = supplied_git_dir.resolve(strict=True)
            git_dir_info = git_dir.stat()
        except OSError as exc:
            raise GatewayError("public gitDir is unavailable") from exc
        git_mode = stat.S_IMODE(git_dir_info.st_mode)
        if (
            not stat.S_ISDIR(supplied_info.st_mode)
            or stat.S_ISLNK(supplied_info.st_mode)
            or not stat.S_ISDIR(git_dir_info.st_mode)
            or git_dir_info.st_uid != os.geteuid()
            or git_mode & stat.S_IWOTH
            or (git_mode & stat.S_IWGRP and
                git_dir_info.st_gid not in os.getgroups())
            or not (git_dir / "HEAD").is_file()
            or not (git_dir / "objects").is_dir()
            or not (git_dir / "refs").is_dir()
        ):
            raise GatewayError(
                "public gitDir must be an owner-controlled bare repository"
            )

        release_store = None
        if item.get("releaseStore"):
            if not isinstance(item["releaseStore"], str):
                raise GatewayError("releaseStore must be a path string")
            release_store = (base / item["releaseStore"]).resolve()
        else:
            # Every publisher writes the release CAS beside the served mirror
            # as <gitDir>/forkmesh-releases (tools/forkmesh-release-publish.sh,
            # and the identical default in server/gateway.go's loader). A
            # config that names no store explicitly — the desktop host writes
            # one, as does a headless refresh with no releaseStore set — must
            # still serve from there: otherwise the node holding a freshly
            # published artifact answers 404 for every release-blob request,
            # and since the Worker reports 404 only when *all* mirrors do, the
            # artifact reads as missing fleet-wide. gitDir is already verified
            # owner-controlled and not group/other-writable above, so only the
            # symlink case needs rejecting here.
            default_store = git_dir / "forkmesh-releases"
            if default_store.is_dir() and not default_store.is_symlink():
                release_store = default_store.resolve()
        repositories.append(
            RepositoryConfig(
                owner=owner,
                name=name,
                visibility=visibility,
                enabled=True,
                git_dir=git_dir,
                release_store=release_store,
                expected_refs_sha256=expected_refs,
                operations=operations,
                encrypted_archive=encrypted_archive,
            )
        )

    dynamic_repositories_path, dynamic_skipped = _merge_dynamic_repositories(
        source.get("dynamicRepositoriesPath"), repositories
    )
    (
        dashboard_enabled,
        dashboard_assets_repository,
        dashboard_canonical_origin,
        dashboard_ssh_origin,
        dashboard_replica_path,
        dashboard_assets_root,
    ) = _dashboard_settings(source.get("dashboard"), repositories)
    return GatewayConfig(
        config_path=path.resolve(),
        generation_sha256=hashlib.sha256(config_raw).hexdigest(),
        node=node,
        public_key=public_key,
        router_public_key=router_public_key,
        public_origin=public_origin,
        listen_host=listen_host,
        listen_port=listen_port,
        manifest_path=manifest_path,
        verifier_command=verifier,
        health_signer_command=signer,
        merge_executor_command=merge_executor,
        actions_summary_path=actions_summary_path,
        actions_control_path=actions_control_path,
        repositories=tuple(repositories),
        max_release_bytes=max_release,
        private_replica_store=private_replica_store,
        max_private_replica_bytes=max_private_replica,
        dashboard_enabled=dashboard_enabled,
        dashboard_assets_repository=dashboard_assets_repository,
        dashboard_canonical_origin=dashboard_canonical_origin,
        dashboard_ssh_origin=dashboard_ssh_origin,
        dashboard_replica_path=dashboard_replica_path,
        dashboard_assets_root=dashboard_assets_root,
        collaboration_control_url=collaboration_control_url,
        dynamic_repositories_path=dynamic_repositories_path,
        dynamic_repositories_skipped=dynamic_skipped,
        writable_repository_root=writable_repository_root,
        writable_owners=frozenset(writable_owners),
        receive_max_input_bytes=receive_max_input,
        operator_policy_path=operator_policy_path,
        operator_policy_state=operator_policy_state,
    )


def _merge_dynamic_repositories(
    raw_path: Any,
    repositories: list[RepositoryConfig],
) -> tuple[Path | None, int]:
    """Append validated fork-registry entries as pin-less public repositories.

    A structurally invalid registry or entry never takes the whole gateway
    down: the mirror keeps serving its operator-configured repositories and
    the skipped count is surfaced through --check. A missing file simply
    means no fork has been created yet.
    """

    if raw_path is None:
        return None, 0
    if not isinstance(raw_path, str) or not raw_path:
        raise GatewayError("dynamicRepositoriesPath must be a path string")
    registry = Path(raw_path)
    if not registry.is_absolute() or registry != Path(
        os.path.normpath(raw_path)
    ):
        raise GatewayError(
            "dynamicRepositoriesPath must be an absolute normalized path"
        )
    try:
        registry.lstat()
    except OSError:
        return registry, 0
    skipped = 0
    try:
        value = _load_json_file(registry, "dynamic repository registry")
        entries = value.get("repositories")
        if (
            value.get("type") != DYNAMIC_REPOSITORIES_TYPE
            or not isinstance(entries, list)
            or len(entries) > MAX_DYNAMIC_REPOSITORY_ENTRIES
        ):
            raise GatewayError("dynamic repository registry is invalid")
    except GatewayError:
        return registry, 1
    known = {
        (repository.owner, repository.name.lower())
        for repository in repositories
    }
    for entry in entries:
        try:
            if not isinstance(entry, dict):
                raise GatewayError("registry entry is invalid")
            owner = str(entry.get("owner") or "").strip().lower()
            name = str(entry.get("name") or "").strip()
            git_dir_raw = str(entry.get("path") or "")
            if (
                not NODE_RE.fullmatch(owner)
                or not REPO_RE.fullmatch(name)
                or (owner, name.lower()) in known
                or not git_dir_raw
            ):
                raise GatewayError("registry entry is invalid")
            supplied = Path(git_dir_raw)
            if not supplied.is_absolute() or supplied != Path(
                os.path.normpath(git_dir_raw)
            ):
                raise GatewayError("registry entry is invalid")
            supplied_info = supplied.lstat()
            git_dir = supplied.resolve(strict=True)
            git_dir_info = git_dir.stat()
            git_mode = stat.S_IMODE(git_dir_info.st_mode)
            if (
                not stat.S_ISDIR(supplied_info.st_mode)
                or stat.S_ISLNK(supplied_info.st_mode)
                or not stat.S_ISDIR(git_dir_info.st_mode)
                or git_dir_info.st_uid != os.geteuid()
                or git_mode & stat.S_IWOTH
                or (git_mode & stat.S_IWGRP and
                    git_dir_info.st_gid not in os.getgroups())
                or not (git_dir / "HEAD").is_file()
                or not (git_dir / "objects").is_dir()
                or not (git_dir / "refs").is_dir()
            ):
                raise GatewayError("registry entry is invalid")
        except (GatewayError, OSError):
            skipped += 1
            continue
        known.add((owner, name.lower()))
        repositories.append(
            RepositoryConfig(
                owner=owner,
                name=name,
                visibility="public",
                enabled=True,
                git_dir=git_dir,
                release_store=None,
                expected_refs_sha256="",
                operations=frozenset(DEFAULT_PUBLIC_OPERATIONS),
                dynamic=True,
            )
        )
    return registry, skipped


def _dashboard_settings(
    value: Any,
    repositories: list[RepositoryConfig],
) -> tuple[bool, tuple[str, str] | None, str, str, Path | None, Path | None]:
    if value is None:
        return False, None, "", "", None, None
    allowed = {"enabled", "assetsRepository"}
    optional = {"canonicalOrigin", "sshOrigin", "replicaPath", "assetsRoot"}
    if not isinstance(value, dict) or not (
        allowed <= set(value) <= allowed | optional
    ):
        raise GatewayError("dashboard configuration is invalid")
    if value.get("enabled") is not True:
        raise GatewayError("dashboard.enabled must be true when configured")
    assets_raw = str(value.get("assetsRepository") or "")
    pieces = assets_raw.split("/")
    if len(pieces) != 2:
        raise GatewayError("dashboard.assetsRepository must be owner/name")
    assets_owner = pieces[0].strip().lower()
    assets_name = pieces[1].strip()
    if not NODE_RE.fullmatch(assets_owner) or not REPO_RE.fullmatch(
        assets_name
    ):
        raise GatewayError("dashboard.assetsRepository must be owner/name")
    key = (assets_owner, assets_name.lower())
    if not any(
        (repository.owner, repository.name.lower()) == key
        and repository.enabled
        and repository.visibility == "public"
        and not repository.dynamic
        for repository in repositories
    ):
        raise GatewayError(
            "dashboard.assetsRepository must name an enabled public repository"
        )
    canonical_origin = ""
    if "canonicalOrigin" in value:
        canonical_origin = _normalize_origin(value.get("canonicalOrigin"))
    ssh_origin = ""
    if "sshOrigin" in value:
        ssh_origin = str(value.get("sshOrigin") or "").strip()
        try:
            parsed = urlsplit(ssh_origin)
            port_ok = parsed.port is None or 1 <= parsed.port <= 65535
        except ValueError:
            port_ok = False
        if (
            not ssh_origin
            or parsed.scheme != "ssh"
            or not parsed.hostname
            or parsed.username != "git"
            or parsed.password
            or parsed.path not in ("", "/")
            or parsed.query
            or parsed.fragment
            or not port_ok
        ):
            raise GatewayError("dashboard.sshOrigin must be an ssh://git@ URL")
        ssh_origin = ssh_origin.rstrip("/")
    replica_path = None
    if "replicaPath" in value:
        if mirror_replica is None:
            raise GatewayError(
                "dashboard replica contract is not installed"
            )
        replica_path = Path(str(value.get("replicaPath") or ""))
        try:
            replica_path = mirror_replica.validate_destination(replica_path)
        except mirror_replica.ReplicaError as error:
            raise GatewayError("dashboard.replicaPath is unsafe") from error
        if not canonical_origin:
            raise GatewayError(
                "dashboard.canonicalOrigin is required with replicaPath"
            )
    assets_root = None
    if "assetsRoot" in value:
        raw_root = value.get("assetsRoot")
        if (
            not isinstance(raw_root, str)
            or not raw_root.startswith("/")
            or "\x00" in raw_root
            or posixpath.normpath(raw_root) != raw_root.rstrip("/")
            or raw_root.rstrip("/") == ""
        ):
            raise GatewayError("dashboard.assetsRoot must be an absolute path")
        assets_root = Path(raw_root.rstrip("/"))
    return True, key, canonical_origin, ssh_origin, replica_path, assets_root


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


_RELEASE_DIGEST_MEMO: "OrderedDict[tuple, float]" = OrderedDict()
_RELEASE_DIGEST_LOCK = threading.Lock()


def _release_digest_matches(path: Path, digest: str) -> bool:
    info = path.stat()
    key = (str(path), digest, info.st_size, info.st_mtime_ns, info.st_ino)
    now = time.monotonic()
    with _RELEASE_DIGEST_LOCK:
        checked = _RELEASE_DIGEST_MEMO.get(key)
        if checked is not None and now - checked < RELEASE_DIGEST_RECHECK_SECONDS:
            _RELEASE_DIGEST_MEMO.move_to_end(key)
            return True
    if not hmac.compare_digest(_sha256_file(path), digest):
        with _RELEASE_DIGEST_LOCK:
            _RELEASE_DIGEST_MEMO.pop(key, None)
        return False
    with _RELEASE_DIGEST_LOCK:
        _RELEASE_DIGEST_MEMO[key] = now
        _RELEASE_DIGEST_MEMO.move_to_end(key)
        while len(_RELEASE_DIGEST_MEMO) > RELEASE_DIGEST_MEMO_ENTRIES:
            _RELEASE_DIGEST_MEMO.popitem(last=False)
    return True


class ArchiveMaterializer:
    """Materialize an encrypted archive through an operator-owned crypto tool."""

    def __init__(
        self,
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self._runner = runner

    def materialize(self, archive: EncryptedArchive, destination: Path) -> Path:
        if archive.scheme != "age-encrypted-tar-v1":
            raise GatewayError(
                "repository archive must use age-encrypted-tar-v1")
        if not archive.ciphertext_path.is_file():
            raise GatewayError("encrypted repository archive is unavailable")
        if not _sha256_file(archive.ciphertext_path) == (
            archive.ciphertext_sha256
        ):
            raise GatewayError("encrypted repository archive digest mismatch")
        try:
            with archive.ciphertext_path.open("rb") as stream:
                header = stream.read(max(
                    len(AGE_NATIVE_HEADER), len(AGE_ARMORED_HEADER)))
        except OSError as exc:
            raise GatewayError(
                "encrypted repository archive is unavailable") from exc
        if not (
            header.startswith(AGE_NATIVE_HEADER)
            or header.startswith(AGE_ARMORED_HEADER)
        ):
            raise GatewayError(
                "encrypted repository archive is not an age file")
        request = {
            "schemaVersion": 1,
            "type": "forkmesh.repository-archive-materialize",
            "scheme": archive.scheme,
            "ciphertextPath": str(archive.ciphertext_path),
            "ciphertextSha256": archive.ciphertext_sha256,
            "keyReference": archive.key_reference,
            "destination": str(destination),
        }
        helper = ExternalJSONCommand(
            archive.materialize_command,
            # Decrypting and validating a flagship repository on a 1 GB
            # mirror legitimately takes longer than the 15-second budget used
            # by signing/verification helpers. It remains bounded, and the
            # gateway stays unavailable until the archive is authenticated.
            timeout=5 * 60,
            runner=self._runner,
        )
        response = helper.call(request)
        if response.get("ok") is not True:
            raise GatewayError("repository archive materialization was rejected")
        relative = _safe_repo_path(
            response.get("repositoryPath"), allow_empty=False
        )
        repository_path = (destination / relative).resolve()
        if destination.resolve() not in repository_path.parents:
            raise GatewayError("materialized repository escaped its destination")
        return repository_path


def _git_environment(git_protocol: str = "") -> dict[str, str]:
    environment = {
        "PATH": os.environ.get("PATH", ""),
        "LANG": "C",
        "LC_ALL": "C",
        # The served bare repository is data, not policy. Ignore its local
        # config so an uploaded or operator-planted uploadpack.* value cannot
        # undo the command-line hidden-ref and hook restrictions below.
        "GIT_CONFIG": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_OPTIONAL_LOCKS": "0",
    }
    if git_protocol in ("version=1", "version=2"):
        environment["GIT_PROTOCOL"] = git_protocol
    return environment


def _git_prefix(git_dir: Path) -> list[str]:
    # Command-line values override executable local configuration that could
    # otherwise start helper programs while serving a repository. Git ignores
    # repository-scoped uploadpack.packObjectsHook because it is protected
    # configuration; setting that key to an empty command makes upload-pack
    # try to execute an empty program instead of disabling the hook.
    return [
        "git",
        "-c",
        "core.alternateRefsCommand=/usr/bin/true",
        "-c",
        "core.fsmonitor=",
        "-c",
        "credential.helper=",
        "-c",
        "protocol.ext.allow=never",
        # Merge transaction refs are owner-only recovery/idempotency state.
        # Hide the whole internal namespace from both advertisements and
        # unadvertised object wants, including future internal ref families.
        "-c",
        "uploadpack.hideRefs=" + INTERNAL_GIT_REF_NAMESPACE,
        # Repository-local configuration is untrusted. Explicitly refuse all
        # unadvertised object-id wants so a planted allowAnySHA1InWant setting
        # cannot fetch an object reachable only from a hidden internal ref.
        "-c",
        "uploadpack.allowAnySHA1InWant=false",
        "-c",
        "uploadpack.allowReachableSHA1InWant=false",
        "-c",
        "uploadpack.allowTipSHA1InWant=false",
        f"--git-dir={git_dir}",
    ]


COMMIT_GRAPH_TIMEOUT_SECONDS = 300
# Tree listings with at least this many entries take their last-commit
# column from one directory walk (GitRepository._directory_summaries).
TREE_BATCH_SUMMARY_MIN_ENTRIES = 65
# Operations whose answer depends only on the commit their ``ref`` resolves
# to (and the other query fields), so their cache key can use the commit.
COMMIT_KEYED_OPERATIONS = frozenset({
    "tree", "blob", "blobs", "history", "search", "stats", "sizes",
})


def _lower_priority() -> None:
    try:
        os.nice(10)
    except OSError:
        pass


def _run_git(
    git_dir: Path,
    arguments: Iterable[str],
    *,
    input_bytes: bytes | None = None,
    max_output: int = MAX_JSON_OUTPUT,
    timeout: int = PROCESS_TIMEOUT_SECONDS,
    allow_exit_one: bool = False,
    git_protocol: str = "",
    alternate: Path | None = None,
) -> bytes:
    environment = _git_environment(git_protocol)
    if alternate is not None:
        environment["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = str(
            alternate / "objects")
    try:
        completed = subprocess.run(
            _git_prefix(git_dir) + list(arguments),
            input=input_bytes,
            capture_output=True,
            env=environment,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise GitError("git operation unavailable") from exc
    if completed.returncode != 0 and not (
        allow_exit_one and completed.returncode == 1
    ):
        raise GitError("git operation failed")
    if len(completed.stdout) > max_output:
        raise GitError("git operation exceeded its output limit")
    return completed.stdout


def _upload_pack_wants(body: bytes) -> frozenset[str]:
    """Return object ids requested by one pkt-line upload-pack request.

    Git's own allow*SHA1InWant settings do not reliably reject an exact object
    id on every supported protocol/version combination. Parse the bounded wire
    request before launching upload-pack so hidden-ref objects never reach that
    compatibility-dependent path.
    """
    wants: set[str] = set()
    offset = 0
    while offset < len(body):
        if offset + 4 > len(body):
            raise GatewayError("git upload-pack request is malformed")
        header = body[offset:offset + 4]
        try:
            length = int(header, 16)
        except ValueError as exc:
            raise GatewayError("git upload-pack request is malformed") from exc
        if length in (0, 1, 2):
            offset += 4
            continue
        if length < 4 or offset + length > len(body):
            raise GatewayError("git upload-pack request is malformed")
        payload = body[offset + 4:offset + length]
        offset += length
        if payload.startswith(b"want-ref "):
            # ref-in-want is not advertised and accepting a client-selected
            # ref here would recreate the hidden-ref disclosure boundary.
            raise GatewayError("git upload-pack requested an unavailable ref")
        if not payload.startswith(b"want "):
            continue
        oid = payload[5:].split(None, 1)[0].decode("ascii", "strict").lower()
        if not GIT_OBJECT_ID_RE.fullmatch(oid):
            raise GatewayError("git upload-pack requested an invalid object")
        wants.add(oid)
    return frozenset(wants)


def _run_git_status(
    git_dir: Path,
    arguments: Iterable[str],
    *,
    max_output: int = MAX_JSON_OUTPUT,
    allowed_codes: tuple[int, ...] = (0,),
) -> tuple[int, bytes]:
    """Like ``_run_git`` but reports the exit code the caller branches on.

    ``merge-tree`` distinguishes a clean merge (0) from a conflicted one (1)
    only through its status, so a preview cannot infer that from stdout alone.
    """
    try:
        completed = subprocess.run(
            _git_prefix(git_dir) + list(arguments),
            capture_output=True,
            env=_git_environment(),
            timeout=PROCESS_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise GitError("git operation unavailable") from exc
    if completed.returncode not in allowed_codes:
        raise GitError("git operation failed")
    if len(completed.stdout) > max_output:
        raise GitError("git operation exceeded its output limit")
    return completed.returncode, completed.stdout


def refs_canonical(git_dir: Path) -> str:
    output = _run_git(
        git_dir,
        [
            "for-each-ref",
            "--sort=refname",
            "--format=%(objectname) %(refname)",
            "refs/heads/",
            "refs/tags/",
        ],
        max_output=16 * 1024 * 1024,
    )
    return "\n".join(
        line
        for line in output.decode("utf-8", "replace").splitlines()
        if line
    )


def prefix_refs(git_dir: Path, prefix: str) -> dict[str, str]:
    output = _run_git(
        git_dir,
        ["for-each-ref", "--format=%(objectname) %(refname)", prefix],
        max_output=1024 * 1024,
    )
    refs: dict[str, str] = {}
    for line in output.decode("utf-8", "replace").splitlines():
        oid, _, ref = line.partition(" ")
        if oid and ref:
            refs[ref] = oid
    return refs


def broken_served_refs(git_dir: Path) -> list[str]:
    broken: list[str] = []
    loose: list[tuple[str, str]] = []
    for namespace in ("refs/heads", "refs/tags"):
        root = git_dir / namespace
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.is_symlink() or not path.is_file() or path.name.endswith(".lock"):
                continue
            ref = path.relative_to(git_dir).as_posix()
            try:
                value = path.read_text("ascii", "replace").strip()
            except OSError:
                continue
            if value.startswith("ref: "):
                continue
            if not GIT_OBJECT_ID_RE.fullmatch(value) or not value.strip("0"):
                broken.append(ref)
            else:
                loose.append((ref, value))
    if loose:
        output = _run_git(
            git_dir,
            ["cat-file", "--batch-check=%(objectname)"],
            input_bytes=("\n".join(oid for _, oid in loose) + "\n").encode("ascii"),
            max_output=4 * 1024 * 1024,
        )
        lines = output.decode("ascii", "replace").splitlines()
        if len(lines) == len(loose):
            broken.extend(
                ref for (ref, _), line in zip(loose, lines)
                if line.strip().endswith(" missing")
            )
    return sorted(broken)


def refs_sha256(git_dir: Path) -> str:
    return hashlib.sha256(refs_canonical(git_dir).encode("utf-8")).hexdigest()


def routing_refs_canonical(git_dir: Path) -> str:
    """Stable serving generation: default branch plus release tags.

    Auxiliary agent and pull-request branches legitimately move at different
    moments on independently syncing mirrors.  Using every head as the routing
    lease made an otherwise current fleet disappear whenever any one of those
    branches was in flight.  The default branch and tags are the durable public
    generation; branch-specific reads remain protected by the endpoint's fresh
    signed request/proof and fail over normally.
    """
    head = _run_git(
        git_dir,
        ["symbolic-ref", "--quiet", "HEAD"],
        max_output=1024,
        allow_exit_one=True,
    ).decode("utf-8", "replace").strip()
    if not head.startswith("refs/heads/"):
        head = "refs/heads/main"
    output = _run_git(
        git_dir,
        [
            "for-each-ref",
            "--sort=refname",
            "--format=%(objectname) %(refname)",
            head,
            "refs/tags/",
        ],
        max_output=4 * 1024 * 1024,
    )
    return "\n".join(
        line
        for line in output.decode("utf-8", "replace").splitlines()
        if line
    )


def routing_refs_sha256(git_dir: Path) -> str:
    return hashlib.sha256(
        routing_refs_canonical(git_dir).encode("utf-8")
    ).hexdigest()


def private_replica_file(
    store: Path | None, opaque_id: str, max_bytes: int
) -> tuple[Path, int, str]:
    """Validate one Qt PrivateMirrorStore ciphertext without decrypting it."""
    if store is None or not OPAQUE_REPLICA_RE.fullmatch(str(opaque_id or "")):
        raise GatewayError("private replica was not found")
    root = store.resolve()
    path = root / (opaque_id + ".fm-private")
    try:
        if path.is_symlink() or not path.is_file() or path.parent.resolve() != root:
            raise GatewayError("private replica was not found")
        size = path.stat().st_size
        if size <= 0 or size > max_bytes:
            raise GatewayError("private replica was not found")
        raw = path.read_bytes()
        value = json.loads(raw)
    except (OSError, json.JSONDecodeError) as exc:
        raise GatewayError("private replica was not found") from exc
    if not isinstance(value, dict) or set(value) != {
        "schemaVersion", "kind", "opaqueId", "keyEpoch", "createdAt",
        "updatedAt", "ciphertextSha256", "envelope",
    }:
        raise GatewayError("private replica was not found")
    envelope = value.get("envelope")
    if (
        value.get("schemaVersion") != 1
        or value.get("kind") != "forkmesh.private-replica"
        or value.get("opaqueId") != opaque_id
        or isinstance(value.get("keyEpoch"), bool)
        or not isinstance(value.get("keyEpoch"), int)
        or value.get("keyEpoch") <= 0
        or isinstance(value.get("createdAt"), bool)
        or not isinstance(value.get("createdAt"), int)
        or isinstance(value.get("updatedAt"), bool)
        or not isinstance(value.get("updatedAt"), int)
        or value.get("createdAt") <= 0
        or value.get("updatedAt") < value.get("createdAt")
        or not SHA256_RE.fullmatch(
            str(value.get("ciphertextSha256") or ""))
        or not isinstance(envelope, dict)
        or set(envelope) != {
            "kind", "v", "alg", "nonce", "tag", "body", "recipients",
        }
        or envelope.get("kind") != "forkmesh.mirror"
        or envelope.get("v") != 1
        or envelope.get("alg") != "x25519+mlkem768/aes256gcm"
    ):
        raise GatewayError("private replica was not found")
    recipients = envelope.get("recipients")
    if not isinstance(recipients, list) or not 1 <= len(recipients) <= 256:
        raise GatewayError("private replica was not found")
    b64_re = re.compile(r"^[A-Za-z0-9+/]*={0,2}$")
    if any(
        not isinstance(envelope.get(field), str)
        or not envelope.get(field)
        or not b64_re.fullmatch(envelope[field])
        for field in ("nonce", "tag", "body")
    ):
        raise GatewayError("private replica was not found")
    seen = set()
    for recipient in recipients:
        if not isinstance(recipient, dict) or set(recipient) != {
            "kid", "x25519", "mlkem768", "nonce", "tag", "key",
        }:
            raise GatewayError("private replica was not found")
        kid = str(recipient.get("kid") or "")
        if (
            not SHA256_RE.fullmatch(kid)
            or kid in seen
            or any(
                not isinstance(recipient.get(field), str)
                or not recipient.get(field)
                or not b64_re.fullmatch(recipient[field])
                for field in ("x25519", "mlkem768", "nonce", "tag", "key")
            )
        ):
            raise GatewayError("private replica was not found")
        seen.add(kid)
    actual_digest = hashlib.sha256(
        _canonical_json(envelope).encode("utf-8")
    ).hexdigest()
    if not hmac.compare_digest(
        actual_digest, str(value["ciphertextSha256"])):
        raise GatewayError("private replica was not found")
    return path, size, hashlib.sha256(raw).hexdigest()


def _ensure_bare_repository(git_dir: Path) -> None:
    if not git_dir.is_dir():
        raise GatewayError("configured public mirror repository is unavailable")
    try:
        value = _run_git(
            git_dir, ["rev-parse", "--is-bare-repository"], max_output=128
        )
    except GitError as exc:
        raise GatewayError("configured public mirror is not a Git repository") from exc
    if value.strip() != b"true":
        raise GatewayError("configured mirror must be a bare Git repository")


def _safe_analysis_path(value: Any) -> str:
    """Normalize an artifact/import path without allowing repository escape."""

    raw = str(value or "").strip().replace("\\", "/")
    if not raw or "\x00" in raw or "\r" in raw or "\n" in raw:
        return ""
    raw = raw.split("#", 1)[0].split("?", 1)[0]
    if raw.startswith("file://"):
        raw = raw[7:]
    normalized = posixpath.normpath(raw).lstrip("/")
    if normalized in ("", ".") or normalized == ".." or normalized.startswith("../"):
        return ""
    return normalized[:500]


def _extract_dependency_specs(path: str, text: str) -> list[str]:
    """Return bounded source-level import/include targets.

    Only syntax that names another source path/module is considered. Package
    registry coordinates remain manifest concerns and are deliberately not
    turned into fake file edges.
    """

    suffix = Path(path).suffix.lower()
    specs: list[str] = []

    def add(value: Any) -> None:
        value = str(value or "").strip()
        if value and value not in specs and len(specs) < 80:
            specs.append(value)

    if suffix in {".js", ".jsx", ".mjs", ".ts", ".tsx"}:
        patterns = (
            r"\b(?:from|import)\s*\(?\s*[\"']([^\"']+)[\"']",
            r"\brequire\s*\(\s*[\"']([^\"']+)[\"']\s*\)",
            r"\bimport\s*\(\s*[\"']([^\"']+)[\"']\s*\)",
        )
        for pattern in patterns:
            for match in re.finditer(pattern, text):
                add(match.group(1))
    elif suffix == ".py":
        for match in re.finditer(
            r"(?m)^\s*from\s+([.A-Za-z_][A-Za-z0-9_.]*)\s+import\s+"
            r"([A-Za-z_][A-Za-z0-9_, ]*)",
            text,
        ):
            module = match.group(1)
            if module.startswith("."):
                dots = len(module) - len(module.lstrip("."))
                prefix = "../" * max(0, dots - 1)
                local_module = module[dots:].replace(".", "/")
                if local_module:
                    add(prefix + local_module)
                else:
                    for imported in match.group(2).split(","):
                        add(prefix + "./" + imported.strip().split()[0])
            else:
                add(module.replace(".", "/"))
        for match in re.finditer(
            r"(?m)^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)", text
        ):
            add(match.group(1).replace(".", "/"))
    elif suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
        for match in re.finditer(
            r"(?m)^\s*#\s*include\s*\"([^\"\r\n]+)\"", text
        ):
            add(match.group(1))
    elif suffix == ".rs":
        for match in re.finditer(
            r"(?m)^\s*(?:pub\s+)?mod\s+([A-Za-z_][A-Za-z0-9_]*)\s*;",
            text,
        ):
            add("./" + match.group(1))
        for match in re.finditer(
            r"(?m)^\s*use\s+crate::([A-Za-z_][A-Za-z0-9_:]*)", text
        ):
            add(match.group(1).replace("::", "/"))
    elif suffix in {".java", ".kt"}:
        for match in re.finditer(
            r"(?m)^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)\s*;?", text
        ):
            add(match.group(1).replace(".", "/"))
    elif suffix == ".dart":
        for match in re.finditer(
            r"\b(?:import|export|part)\s+[\"']([^\"']+)[\"']", text
        ):
            add(match.group(1))
    elif suffix == ".rb":
        for match in re.finditer(
            r"\brequire_relative\s*[ \(]?\s*[\"']([^\"']+)[\"']", text
        ):
            add(match.group(1))
    elif suffix in {".css", ".scss"}:
        for match in re.finditer(
            r"@(?:import|use|forward)\s+(?:url\()?\s*[\"']([^\"']+)[\"']",
            text,
        ):
            add(match.group(1))
    elif suffix == ".go":
        for block in re.finditer(
            r"(?ms)^\s*import\s*(?:\((.*?)\)|[\"']([^\"']+)[\"'])", text
        ):
            if block.group(2):
                add(block.group(2))
            else:
                for match in re.finditer(r"[\"']([^\"']+)[\"']", block.group(1)):
                    add(match.group(1))
    elif suffix in {".php", ".cs", ".swift"}:
        for match in re.finditer(
            r"(?m)^\s*(?:require_once|require|include|using|import)\s*"
            r"(?:\(\s*)?[\"']?([^\"';\r\n)]+)",
            text,
        ):
            add(match.group(1).strip())
    return specs


def _dependency_target(
    source: str,
    spec: str,
    known_paths: set[str],
    suffix_index: Mapping[str, list[str]],
) -> str:
    """Resolve one import to a committed repository file, or return empty."""

    raw = str(spec or "").strip().replace("\\", "/")
    if (
        not raw
        or raw.startswith(
            ("//", "http:", "https:", "data:", "node:", "dart:")
        )
        or "\x00" in raw
    ):
        return ""
    source_dir = posixpath.dirname(source)
    relative = raw.startswith(".")
    source_suffix = Path(source).suffix.lower()
    # Bare JavaScript/TypeScript specifiers use package or project-alias
    # resolution. Without a committed resolver configuration, treating a
    # same-named source file as the target would manufacture an edge.
    if (
        source_suffix in {".js", ".jsx", ".mjs", ".ts", ".tsx"}
        and not relative
    ):
        return ""
    if relative:
        base = _safe_analysis_path(posixpath.join(source_dir, raw))
    else:
        base = _safe_analysis_path(raw)
    if not base:
        return ""

    extensions = [
        source_suffix,
        ".ts", ".tsx", ".js", ".jsx", ".mjs", ".py", ".rs", ".go",
        ".java", ".kt", ".dart", ".rb", ".php", ".c", ".cc", ".cpp",
        ".h", ".hpp", ".cs", ".swift", ".css", ".scss",
    ]
    candidates = [base]
    if Path(base).suffix.lower() not in ANALYZED_SOURCE_SUFFIXES:
        candidates.extend(base + extension for extension in extensions)
        candidates.extend(
            posixpath.join(base, name)
            for name in (
                "__init__.py", "index.ts", "index.tsx", "index.js",
                "index.jsx", "mod.rs", "lib.rs",
            )
        )
    for candidate in candidates:
        if candidate in known_paths and candidate != source:
            return candidate

    # Absolute module/package imports often include a configured project or
    # Java package prefix. Resolve only a unique committed suffix; ambiguity
    # stays unresolved instead of manufacturing an edge.
    if not relative:
        for candidate in candidates:
            matches = suffix_index.get(candidate, [])
            if len(matches) == 1 and matches[0] != source:
                return matches[0]
    return ""


def _match_coverage_path(raw: Any, known_paths: set[str]) -> str:
    candidate = _safe_analysis_path(raw)
    if candidate in known_paths:
        return candidate
    if not candidate:
        return ""
    matches = [
        path
        for path in known_paths
        if candidate.endswith("/" + path) or path.endswith("/" + candidate)
    ]
    return matches[0] if len(matches) == 1 else ""


def _percentage(covered: Any, total: Any) -> float | None:
    try:
        covered_number = max(0.0, float(covered))
        total_number = max(0.0, float(total))
    except (TypeError, ValueError):
        return None
    if total_number <= 0:
        return None
    return round(min(100.0, covered_number * 100.0 / total_number), 1)


def _parse_json_coverage(
    payload: bytes, known_paths: set[str]
) -> dict[str, float]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
        return {}
    if not isinstance(value, dict):
        return {}
    files = value.get("files")
    records = files if isinstance(files, dict) else value
    result: dict[str, float] = {}
    for raw_path, record in list(records.items())[:10_000]:
        if not isinstance(record, dict):
            continue
        path = _match_coverage_path(raw_path, known_paths)
        if not path:
            continue
        percent: float | None = None
        summary = record.get("summary")
        if isinstance(summary, dict):
            percent_value = summary.get(
                "percent_covered",
                summary.get("percentCovered", summary.get("percent")),
            )
            try:
                percent = round(float(percent_value), 1)
            except (TypeError, ValueError):
                percent = _percentage(
                    summary.get("covered_lines", summary.get("covered")),
                    summary.get("num_statements", summary.get("total")),
                )
        statement_counts = record.get("s")
        if percent is None and isinstance(statement_counts, dict):
            counts = [
                count for count in statement_counts.values()
                if isinstance(count, (int, float)) and not isinstance(count, bool)
            ]
            percent = _percentage(
                sum(1 for count in counts if count > 0), len(counts)
            )
        lines = record.get("lines")
        if percent is None and isinstance(lines, dict):
            percent = _percentage(lines.get("covered"), lines.get("total"))
            try:
                if percent is None and "pct" in lines:
                    percent = round(float(lines["pct"]), 1)
            except (TypeError, ValueError):
                pass
        if percent is not None and 0 <= percent <= 100:
            result[path] = percent
    return result


def _parse_lcov_coverage(
    payload: bytes, known_paths: set[str]
) -> dict[str, float]:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        return {}
    result: dict[str, float] = {}
    current = ""
    found = hit = 0
    for line in text.splitlines()[:200_000]:
        if line.startswith("SF:"):
            current = _match_coverage_path(line[3:], known_paths)
            found = hit = 0
        elif line.startswith("DA:") and current:
            parts = line[3:].split(",", 2)
            if len(parts) >= 2:
                found += 1
                try:
                    hit += int(parts[1]) > 0
                except ValueError:
                    pass
        elif line.startswith("LF:") and current:
            try:
                found = max(found, int(line[3:]))
            except ValueError:
                pass
        elif line.startswith("LH:") and current:
            try:
                hit = max(hit, int(line[3:]))
            except ValueError:
                pass
        elif line == "end_of_record" and current:
            percent = _percentage(hit, found)
            if percent is not None:
                result[current] = percent
            current = ""
    if current:
        percent = _percentage(hit, found)
        if percent is not None:
            result[current] = percent
    return result


def _parse_xml_coverage(
    payload: bytes, known_paths: set[str]
) -> dict[str, float]:
    """Parse common Cobertura/JaCoCo file counters without an XML entity parser."""

    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        return {}
    result: dict[str, float] = {}
    # Cobertura class records usually carry a direct line-rate.
    for match in itertools.islice(re.finditer(
        r"<class\b[^>]*\bfilename=[\"']([^\"']+)[\"'][^>]*"
        r"\bline-rate=[\"']([0-9.]+)[\"']",
        text,
        re.IGNORECASE,
    ), 10_000):
        path = _match_coverage_path(match.group(1), known_paths)
        try:
            percent = round(float(match.group(2)) * 100.0, 1)
        except ValueError:
            continue
        if path and 0 <= percent <= 100:
            result[path] = percent
    # JaCoCo sourcefile records expose missed/covered LINE counters.
    for package in itertools.islice(re.finditer(
        r"<package\b[^>]*\bname=[\"']([^\"']*)[\"'][^>]*>(.*?)</package>",
        text,
        re.IGNORECASE | re.DOTALL,
    ), 2_000):
        prefix = package.group(1).strip("/")
        for source in itertools.islice(re.finditer(
            r"<sourcefile\b[^>]*\bname=[\"']([^\"']+)[\"'][^>]*>"
            r"(.*?)</sourcefile>",
            package.group(2),
            re.IGNORECASE | re.DOTALL,
        ), 10_000):
            path = _match_coverage_path(
                posixpath.join(prefix, source.group(1)), known_paths
            )
            counter = re.search(
                r"<counter\b[^>]*\btype=[\"']LINE[\"'][^>]*"
                r"\bmissed=[\"']([0-9]+)[\"'][^>]*"
                r"\bcovered=[\"']([0-9]+)[\"']",
                source.group(2),
                re.IGNORECASE,
            )
            if not path or not counter:
                continue
            missed = int(counter.group(1))
            covered = int(counter.group(2))
            percent = _percentage(covered, missed + covered)
            if percent is not None:
                result[path] = percent
    return result


def _dependency_depths(
    adjacency: Mapping[str, list[str]], maximum: int = 12
) -> dict[str, int]:
    """Compute bounded transitive outbound depth with cycle-safe DFS."""

    memo: dict[str, int] = {}
    visiting: set[str] = set()

    def depth(path: str) -> int:
        if path in memo:
            return memo[path]
        if path in visiting:
            return 0
        visiting.add(path)
        value = 0
        for target in adjacency.get(path, []):
            value = max(value, min(maximum, 1 + depth(target)))
            if value >= maximum:
                break
        visiting.discard(path)
        memo[path] = value
        return value

    for path in adjacency:
        depth(path)
    return memo


PULSE_OPERATION = "pulse"
PULSE_CARRIER_OPERATION = "stats"
PULSE_DEFAULT_PERIOD = "1w"
PULSE_PERIODS = {
    "24h": {"seconds": 86400, "bucket": 300, "step": 3600},
    "3d": {"seconds": 3 * 86400, "bucket": 900, "step": 6 * 3600},
    "1w": {"seconds": 7 * 86400, "bucket": 1800, "step": 86400},
    "1m": {"seconds": 30 * 86400, "bucket": 3600, "step": 86400},
    "all": {"seconds": 0, "bucket": 3600, "step": 0},
}
PULSE_ALL_TIME_COMPARE_SECONDS = 7 * 86400
PULSE_SCORE_WEIGHTS = {
    "merged": 10,
    "commits": 1,
    "issuesClosed": 3,
    "reviews": 4,
    "comments": 1,
}
PULSE_COMMITS_PER_DAY = 5
PULSE_COMMENTS_PER_THREAD = 3
PULSE_LIST_LIMIT = 200
PULSE_LEADERBOARD_LIMIT = 50
PULSE_COMMITTERS_LIMIT = 20
PULSE_NEWCOMERS_LIMIT = 20
PULSE_HISTORY_COMMITS = 250_000
PULSE_WINDOW_COMMITS = 20_000
PULSE_NUMSTAT_COMMITS = 1_000
PULSE_NUMSTAT_BATCH = 100
PULSE_NUMSTAT_SECONDS = 3.0
PULSE_DIFF_FILE_LIMIT = "256k"
PULSE_NUMSTAT_MEMO = 4_000
PULSE_NUMSTAT_PATHS = 2_000
PULSE_MAX_PULLS = 5_000
PULSE_MAX_ISSUES = 10_000
PULSE_RECORD_BYTES = 256 * 1024
PULSE_READ_BATCH_BYTES = 16 * 1024 * 1024
PULSE_PERSON_RE = re.compile(r"^[^\x00-\x1f\x7f]{1,100}$")
PULSE_BUG_LABEL_RE = re.compile(r"bug|defect|security|vulnerab", re.IGNORECASE)
PULSE_EVENT_FILE_RE = re.compile(r"^[0-9]{4,}-([a-z][a-z-]{0,39})\.md$")
PULSE_ISSUE_PATH_RE = re.compile(
    r"^\.forkmesh/issues/(?:(open|closed)/)?([1-9][0-9]{0,8})/"
    r"issue-([1-9][0-9]{0,8})\.json$")
PULSE_PULL_COMMENT_TYPES = frozenset({
    "comment", "line-comment", "thread-comment", "thread-reply"})
PULSE_MERGED_STATUSES = frozenset({"merged", "closed-merged"})
PULSE_FINISHED_STATUSES = frozenset({"closed", "merged", "closed-merged"})


PULSE_PARTIAL_HEADER = "X-ForkMesh-Partial"


def _pulse_response(value: Mapping[str, Any]) -> "GatewayResponse":
    response = json_response(value)
    if (value.get("summary") or {}).get("partial"):
        response.headers[PULSE_PARTIAL_HEADER] = "1"
        response.headers["Cache-Control"] = "no-store"
    return response


def pulse_period(query: Mapping[str, str]) -> str:
    period = str(query.get("period") or PULSE_DEFAULT_PERIOD)
    if period not in PULSE_PERIODS:
        raise GatewayError("invalid pulse period")
    return period


def pulse_person(query: Mapping[str, str]) -> str:
    raw = str(query.get("person") or "")
    person = " ".join(raw.split())
    if "person" in query and not (
            PULSE_PERSON_RE.fullmatch(raw) and person):
        raise GatewayError("invalid pulse person")
    return person


def pulse_window_end(period: str, now_seconds: int) -> int:
    bucket = PULSE_PERIODS[period]["bucket"]
    return -(-max(0, int(now_seconds)) // bucket) * bucket


def _pulse_ms(value: Any) -> int:
    try:
        number = int(float(str(value if value is not None else "").strip()
                           or 0))
    except (TypeError, ValueError, OverflowError):
        return 0
    if number <= 0:
        return 0
    return number * 1000 if number < 10_000_000_000 else number


def _pulse_key(name: Any) -> str:
    return " ".join(str(name or "").split()).casefold()


PULSE_EXCLUDE_ENV = "ACTIVITY_EXCLUDE_AUTHORS"
PULSE_AUTOMATED_NAMES = re.compile(
    r"^(?:"
    r".*\[bot\]|.*(?:^|[\s_-])bot|github[\s_-]?actions?|[a-z0-9][a-z0-9_-]*-actions"
    r"|dependabot|renovate|forkmesh(?: [a-z]+)?|.* reconciler"
    r"|claude(?: (?:code|opus|sonnet|haiku|instant|agent)\b.*| \d.*)?"
    r"|(?:openai )?codex(?: cli| agent)?|(?:chat)?gpt(?:[\s-].*)?|o\d(?:-mini)?"
    r"|(?:google )?gemini(?: (?:code assist|cli|agent))?|google jules"
    r"|(?:github )?copilot(?: agent)?|copilot-swe-agent|llama \d.*"
    r"|devin ai|cursor agent|openhands|sweep ai"
    r"|fmnext-[a-z0-9-]+|mirror\d+|[a-z0-9][a-z0-9-]*-mirror"
    r")$",
    re.IGNORECASE)
PULSE_AMBIGUOUS_NAMES = re.compile(
    r"^(?:jules|devin|cursor|aider|sweep|llama)$", re.IGNORECASE)
PULSE_AUTOMATED_EMAILS = (
    "*[[]bot[]]@*", "*@forkmesh.local", "*@forkmesh.invalid", "*-reconciler@*",
    "noreply@anthropic.com", "llama@cloudflare.com", "noreply@openai.com",
    "codex@openai.com", "*@devin.ai", "cursoragent@cursor.com",
    "jules@google.com", "*copilot*@users.noreply.github.com",
)


def pulse_exclusion_rules(
    node: str = "", environ: Mapping[str, str] | None = None
) -> tuple[str, ...]:
    raw = (os.environ if environ is None else environ).get(
        PULSE_EXCLUDE_ENV, "")
    rules = [item.strip().casefold() for item in str(raw).split(",")]
    if node:
        rules.append(str(node).strip().casefold())
    return tuple(sorted({rule for rule in rules if rule}))[:200]


def _pulse_is_automated(
    name: Any, email: Any = "", rules: Iterable[str] = ()
) -> bool:
    name = " ".join(str(name or "").split())
    email = str(email or "").strip().casefold()
    if not name and not email:
        return True
    folded = name.casefold()
    if name and PULSE_AUTOMATED_NAMES.fullmatch(name):
        return True
    if name and not email and PULSE_AMBIGUOUS_NAMES.fullmatch(name):
        return True
    if email and any(fnmatch.fnmatchcase(email, pattern)
                     for pattern in PULSE_AUTOMATED_EMAILS):
        return True
    return any(
        fnmatch.fnmatchcase(folded, rule)
        or (email and fnmatch.fnmatchcase(email, rule))
        for rule in rules)


def _pulse_document(raw: bytes) -> tuple[dict[str, str], str]:
    text = raw.decode("utf-8", "replace").lstrip()
    fields = _pull_record_fields(text)
    if not fields:
        return {}, ""
    end = text.find("\n---", 3)
    return fields, (text[end + 4:] if end >= 0 else "").strip()


def _pulse_pull_record(
    number: int, files: Mapping[str, bytes]
) -> dict[str, Any] | None:
    fields, _body = _pulse_document(files.get("pull.md", b""))
    if not fields:
        return None
    status = str(fields.get("status") or "open").strip().lower()
    if status == "deleted":
        return None
    opened = _pulse_ms(fields.get("ts"))
    closed = 0
    events: list[tuple[int, str, str]] = []
    for name in sorted(files):
        match = PULSE_EVENT_FILE_RE.fullmatch(name)
        if not match:
            continue
        event, body = _pulse_document(files[name])
        kind = str(event.get("type") or match.group(1)).strip().lower()
        at = _pulse_ms(event.get("ts"))
        if not at:
            continue
        if kind == "status" and str(
                event.get("state") or "").strip().lower() == "closed":
            closed = max(closed, at)
        elif kind == "review" or (
                kind in PULSE_PULL_COMMENT_TYPES and not body.startswith("/")):
            events.append((
                at, "review" if kind == "review" else "comment",
                str(event.get("authorName") or "").strip()[:100]))
    newest = max([opened] + [item[0] for item in events])
    merged = 0
    if status in PULSE_MERGED_STATUSES:
        merged = _pulse_ms(fields.get("mergedAt")) or closed or newest
        closed = closed or merged
    elif status in PULSE_FINISHED_STATUSES:
        closed = closed or newest
    else:
        closed = 0
    return {
        "number": number,
        "title": str(fields.get("title") or "")[:300],
        "author": str(fields.get("authorName") or "").strip()[:100],
        "status": status,
        "opened": opened,
        "merged": merged,
        "closed": closed,
        "events": tuple(events),
    }


def _pulse_issue_record(number: int, raw: bytes) -> dict[str, Any] | None:
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
        return None
    if not isinstance(data, dict):
        return None
    status = str(data.get("status") or "open").strip().lower()
    if status == "deleted":
        return None
    labels = data.get("labels") if isinstance(data.get("labels"), list) else []
    closed, closer = 0, ""
    events: list[tuple[int, str, str]] = []
    for event in data.get("events") if isinstance(
            data.get("events"), list) else []:
        if not isinstance(event, dict):
            continue
        kind = str(event.get("type") or "")
        at = _pulse_ms(event.get("ts"))
        who = str(event.get("authorName") or "").strip()[:100]
        if not at:
            continue
        if kind == "status" and str(
                event.get("status") or "").strip().lower() == "closed":
            if at >= closed:
                closed, closer = at, who
        elif kind == "comment" and not str(
                event.get("body") or "").lstrip().startswith("/"):
            events.append((at, "comment", who))
    if status != "closed":
        closed, closer = 0, ""
    elif not closed:
        closed = _pulse_ms(data.get("updatedAt"))
    return {
        "number": number,
        "title": str(data.get("title") or "")[:300],
        "author": str(data.get("authorName") or "").strip()[:100],
        "status": "closed" if status == "closed" else "open",
        "opened": _pulse_ms(data.get("createdAt")),
        "closed": closed,
        "closer": closer,
        "bug": any(PULSE_BUG_LABEL_RE.search(str(label)) for label in labels),
        "events": tuple(events),
    }


def _pulse_tree_entries(raw: bytes, oid_bytes: int) -> dict[str, str]:
    entries: dict[str, str] = {}
    cursor = 0
    while cursor < len(raw):
        space = raw.find(b" ", cursor)
        nul = raw.find(b"\0", space + 1)
        if space < 0 or nul < 0 or nul + 1 + oid_bytes > len(raw):
            break
        mode = raw[cursor:space]
        name = raw[space + 1:nul].decode("utf-8", "replace")
        oid = raw[nul + 1:nul + 1 + oid_bytes].hex()
        cursor = nul + 1 + oid_bytes
        if mode in (b"100644", b"100755"):
            entries[name] = oid
    return entries


def _pulse_streaks(days: list[int], today: int) -> tuple[int, int]:
    longest = run = 0
    previous = None
    for day in days:
        if day > today:
            break
        run = run + 1 if previous is not None and day == previous + 1 else 1
        longest = max(longest, run)
        previous = day
    current = run if previous is not None and previous >= today - 1 else 0
    return longest, current


def _pulse_iso(seconds: int) -> str:
    return datetime.fromtimestamp(
        max(0, int(seconds)), timezone.utc).isoformat().replace("+00:00", "Z")


def _pulse_ranking(
    scores: Mapping[str, tuple[int, dict[str, int]]],
    names: Mapping[str, str],
) -> list[tuple[int, str, int, dict[str, int]]]:
    ordered = sorted(
        scores.items(),
        key=lambda item: (-item[1][0], names.get(item[0], item[0]).casefold()),
    )
    ranked: list[tuple[int, str, int, dict[str, int]]] = []
    previous_score = None
    rank = 0
    for position, (key, (score, parts)) in enumerate(ordered, 1):
        if score != previous_score:
            rank = position
            previous_score = score
        ranked.append((rank, key, score, parts))
    return ranked


def _pulse_empty_parts() -> dict[str, int]:
    return {name: 0 for name in PULSE_SCORE_WEIGHTS}


def _pulse_scores(
    ledger: Mapping[str, Any], start: int, end: int
) -> dict[str, tuple[int, dict[str, int]]]:
    parts: dict[str, dict[str, int]] = {}
    events = ledger["events"]
    threads: Counter[tuple[str, str]] = Counter()
    fields = {"merged": "merged", "review": "reviews", "closed": "issuesClosed"}
    for _at, key, kind, thread, _number, _bug in events[
            bisect.bisect_left(events, (start,)):
            bisect.bisect_left(events, (end,))]:
        field = fields.get(kind, "")
        if kind == "comment":
            threads[(key, thread)] += 1
            if threads[(key, thread)] <= PULSE_COMMENTS_PER_THREAD:
                field = "comments"
        if field:
            parts.setdefault(key, _pulse_empty_parts())[field] += 1
    for key, times in ledger["commits"].items():
        low = bisect.bisect_left(times, start)
        high = bisect.bisect_left(times, end)
        if low < high:
            days = Counter(at // 86400 for at in times[low:high])
            parts.setdefault(key, _pulse_empty_parts())["commits"] += sum(
                min(PULSE_COMMITS_PER_DAY, count) for count in days.values())
    return {
        key: (sum(PULSE_SCORE_WEIGHTS[name] * value
                  for name, value in row.items()), row)
        for key, row in parts.items()
        if any(row.values())
    }


def _pulse_totals(
    ledger: Mapping[str, Any], key: str, end: int, today: int
) -> dict[str, Any]:
    events = ledger["byPerson"].get(key, [])
    upto = events[:bisect.bisect_left(events, (end,))]
    kinds = Counter(event[2] for event in upto)
    times = ledger["commits"].get(key, [])
    commits = bisect.bisect_left(times, end)
    days = sorted(
        {at // 86400 for at in times[:commits]}
        | {event[0] // 86400 for event in upto})
    longest, current = _pulse_streaks(days, today)
    stamps = ([times[0]] if commits else []) + ([upto[0][0]] if upto else [])
    merged = [event[0] for event in upto if event[2] == "merged"]
    return {
        "commits": commits,
        "mergedPulls": kinds["merged"],
        "openedPulls": kinds["pull"],
        "reviews": kinds["review"],
        "comments": kinds["comment"],
        "issuesOpened": kinds["issue"],
        "issuesClosed": kinds["closed"],
        "bugsClosed": sum(1 for event in upto
                          if event[2] == "closed" and event[5]),
        "longestStreak": longest,
        "currentStreak": current,
        "activeDays": len(days),
        "firstActivityAt": min(stamps) * 1000 if stamps else None,
        "firstMergedAt": merged[0] * 1000 if merged else None,
    }


def _pulse_person_entry(ledger: Mapping[str, Any], key: str) -> dict[str, Any]:
    entry: dict[str, Any] = {"name": ledger["names"].get(key, key)}
    avatar = ledger["avatars"].get(key)
    if avatar:
        entry["avatarUrl"] = avatar
    return entry


class GitRepository:
    def __init__(self, config: RepositoryConfig, git_dir: Path) -> None:
        self.config = config
        self.git_dir = git_dir
        self._integrity_lock = threading.Lock()
        self._integrity_checked_at = 0.0
        self._integrity_ok = False
        self._trusted_routing_refs_sha256 = ""
        self._analysis_lock = threading.Lock()
        self._analysis_commit = ""
        self._analysis_value: dict[str, Any] = {}
        self._counts_lock = threading.Lock()
        self._counts_key: tuple[str, str, str] = ("", "", "")
        self._counts_value: dict[str, int] = {}
        self._deleted_pulls_key = ""
        self._deleted_pulls_value: dict[str, int] = {}
        self._upload_objects_lock = threading.Lock()
        self._pulse_lock = threading.Lock()
        self._pulse_numstat_lock = threading.Lock()
        self._pulse_commits: dict[str, Any] = {}
        self._pulse_issues: tuple[str, list[dict[str, Any]]] = ("\0", [])
        self._pulse_issue_records: dict[str, dict[str, Any] | None] = {}
        self._pulse_pulls: tuple[tuple[str, str], list[dict[str, Any]]] = (
            ("\0", "\0"), [])
        self._pulse_pull_records: dict[str, dict[str, Any] | None] = {}
        self._pulse_ledger: dict[str, Any] = {}
        self._pulse_numstat_memo: OrderedDict[
            str, tuple[int, int, tuple[int, ...], bool] | None] = OrderedDict()
        self._upload_objects_key = ""
        self._upload_objects: frozenset[str] = frozenset()
        # Ref -> commit within one cache generation (see cache_ref).
        self._ref_memo_lock = threading.Lock()
        self._ref_memo: dict[tuple[int, str], str] = {}
        # Process-unique and advanced after an in-process receive-pack. A
        # daemon generation reload creates fresh repository objects, so a
        # response can never cross a validated refs/config boundary.
        self.cache_generation = next(_REPOSITORY_CACHE_GENERATIONS)
        # The application points this at its operator-policy hooks directory;
        # repository-planted hooks never run either way.
        self.receive_hooks_path = os.devnull
        _ensure_bare_repository(git_dir)
        self.check_integrity(force=True)

    def invalidate_memory_generation(self) -> None:
        self.cache_generation = next(_REPOSITORY_CACHE_GENERATIONS)
        with self._integrity_lock:
            self._integrity_checked_at = 0.0
        with self._analysis_lock:
            self._analysis_commit = ""
            self._analysis_value = {}
        with self._counts_lock:
            self._counts_key = ("", "", "")
            self._counts_value = {}
        with self._upload_objects_lock:
            self._upload_objects_key = ""
            self._upload_objects = frozenset()
        with self._ref_memo_lock:
            self._ref_memo.clear()

    def cache_ref(self, requested: Any = "") -> str:
        """The commit ``requested`` names in the current cache generation.

        A generation is one validated ref state (a moved ref fails the
        integrity check until the daemon's reload starts a new generation),
        so within it ``ref=main``, no ref (HEAD) and the tip's commit id all
        name the same commit and may share one cached response. Memoized so
        a cache hit costs no ``git rev-parse``.
        """
        key = (self.cache_generation, str(requested or ""))
        with self._ref_memo_lock:
            commit = self._ref_memo.get(key)
        if commit:
            return commit
        commit = self.resolve_commit(requested)
        with self._ref_memo_lock:
            if len(self._ref_memo) >= 256:
                self._ref_memo.clear()
            self._ref_memo[key] = commit
        return commit

    def write_commit_graph(self) -> bool:
        """Keep a commit-graph with changed-path Bloom filters current.

        Every tree listing runs one path-limited ``git log`` per entry for its
        last-commit column; without Bloom filters each walks full commit
        history, which on a 1 vCPU node made a cold root tree of a 50k-commit
        repository take several seconds after every push. The graph is
        derived data (Git ignores a stale or missing one), written as an
        incremental split layer so a push costs only its new commits. Any
        failure (read-only storage, a concurrent writer's lock) is ignored.
        """
        try:
            completed = subprocess.run(
                _git_prefix(self.git_dir) + [
                    "commit-graph", "write", "--reachable",
                    "--changed-paths", "--split", "--no-progress",
                ],
                capture_output=True,
                env=_git_environment(),
                timeout=COMMIT_GRAPH_TIMEOUT_SECONDS,
                check=False,
                preexec_fn=_lower_priority,
            )
        except (OSError, subprocess.SubprocessError):
            return False
        return completed.returncode == 0

    def check_integrity(self, *, force: bool = False) -> bool:
        with self._integrity_lock:
            now = time.monotonic()
            if not force and now - self._integrity_checked_at < 15:
                return self._integrity_ok
            if "git-receive-pack" in self.config.operations:
                # A writable repository's refs legitimately move on every
                # authorized push, so the immutable bootstrap pin cannot
                # apply. Integrity here means the bare repository remains
                # readable and its refs still enumerate; the routing Worker
                # independently re-verifies the served generation through
                # the signed repository proof.
                try:
                    refs_sha256(self.git_dir)
                    self._integrity_ok = not broken_served_refs(self.git_dir)
                except (GitError, OSError):
                    self._integrity_ok = False
                self._integrity_checked_at = now
                return self._integrity_ok
            try:
                actual = refs_sha256(self.git_dir)
                if self.config.dynamic and not self.config.expected_refs_sha256:
                    # A dynamic fork has no owner-signed pin; trust the state
                    # present when this process started (the gateway restarts
                    # after every authorized write) and detect divergence
                    # within the process lifetime like the pinned path does.
                    exact = not self._trusted_routing_refs_sha256
                else:
                    exact = hmac.compare_digest(
                        actual, self.config.expected_refs_sha256)
                routing_actual = routing_refs_sha256(self.git_dir)
                if exact and not self._trusted_routing_refs_sha256:
                    # The signed, owner-written configuration authenticates the
                    # complete initial ref set. Capture its stable serving
                    # generation only after that strict bootstrap succeeds.
                    self._trusted_routing_refs_sha256 = routing_actual
                self._integrity_ok = bool(
                    self._trusted_routing_refs_sha256
                    and hmac.compare_digest(
                        routing_actual,
                        self._trusted_routing_refs_sha256,
                    )
                    and not broken_served_refs(self.git_dir)
                )
            except (GitError, OSError):
                self._integrity_ok = False
            self._integrity_checked_at = now
            return self._integrity_ok

    def resolve_commit(self, requested: Any = "") -> str:
        ref = _safe_ref(requested)
        candidates = [ref]
        if ref.startswith("refs/"):
            if not ref.startswith(VISIBLE_REF_PREFIXES):
                raise GatewayError("invalid repository ref")
        elif ref != "HEAD":
            candidates = [
                "refs/heads/" + ref,
                "refs/remotes/origin/" + ref,
                "refs/tags/" + ref,
            ]
            if re.fullmatch(r"[0-9a-f]{4,64}", ref):
                candidates.append(ref)
        for candidate in candidates:
            code, output = _run_git_status(
                self.git_dir,
                [
                    "rev-parse",
                    "--verify",
                    "--quiet",
                    "--end-of-options",
                    candidate + "^{commit}",
                ],
                max_output=128,
                allowed_codes=(0, 1),
            )
            if code == 1:
                continue
            commit = output.decode("ascii", "ignore").strip().lower()
            if re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", commit):
                return commit
        # ``rev-parse --verify --quiet`` reserves exit 1 for a name that does
        # not resolve.  Keep that ordinary repository answer separate from an
        # invocation/storage failure, which _run_git_status raises as GitError
        # and the gateway correctly exposes as a retryable 503.
        raise GatewayError("repository ref was not found")

    def _commit_summary(self, commit: str, path: str = "") -> dict[str, Any]:
        args = [
            "log",
            "-n",
            "20",
            "--format=%H%x1f%an%x1f%cI%x1f%s",
            commit,
        ]
        if path:
            args += ["--", path]
        output = _run_git(self.git_dir, args, max_output=128 * 1024)
        rows = output.decode("utf-8", "replace").splitlines()
        if not rows:
            return {}
        fields = rows[0].split("\x1f", 3)
        if len(fields) != 4:
            return {}
        return {
            "hash": fields[0],
            "commit": fields[0],
            "commitHash": fields[0],
            "author": fields[1],
            "date": fields[2],
            "subject": fields[3],
            "message": fields[3],
            "commitMessage": fields[3],
            "changeCount": len(rows),
            "changeCountCapped": len(rows) >= 20,
        }

    def _directory_summaries(
        self, commit: str, prefix: str, names: set[str]
    ) -> dict[str, dict[str, Any]]:
        """Last-commit summaries for every child of ``prefix`` in one walk.

        ``_commit_summary`` per entry costs a process and a history walk
        each; a 400-entry directory took over a minute on a 1 vCPU node.
        Here one ``git log --name-only`` over the directory attributes each
        changed path to its top-level child: the first commit seen is the
        entry's last change, and counting stops at 20 like the per-entry
        form. Merges are attributed by their dense combined diff (the paths
        a merge itself resolved). History simplification is per directory
        rather than per entry, so on side-branch-heavy history the change
        count can be higher than the per-entry walk's; the last commit
        agrees for nearly every entry.
        """
        args = [
            "log", "--no-renames", "-z", "--diff-merges=dense-combined",
            "--format=%x01%H%x1f%an%x1f%cI%x1f%s", "--name-only", commit,
        ]
        if prefix:
            args += ["--", ":(top,literal)" + prefix]
        summaries: dict[str, dict[str, Any]] = {}
        try:
            process = subprocess.Popen(
                _git_prefix(self.git_dir) + args,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=_git_environment(),
            )
        except OSError as exc:
            raise GitError("git operation unavailable") from exc
        deadline = time.monotonic() + PROCESS_TIMEOUT_SECONDS
        current: dict[str, Any] = {}
        touched: set[str] = set()
        capped: set[str] = set()
        buffer = b""
        assert process.stdout is not None
        try:
            while True:
                if time.monotonic() > deadline:
                    raise GitError("git operation timed out")
                chunk = process.stdout.read1(65536)
                if not chunk:
                    break
                buffer += chunk
                parts = buffer.split(b"\0")
                buffer = parts.pop()
                for part in parts:
                    part = part.lstrip(b"\n")
                    if part.startswith(b"\x01"):
                        fields = part[1:].decode("utf-8", "replace").split(
                            "\x1f", 3)
                        current = (
                            {
                                "hash": fields[0],
                                "commit": fields[0],
                                "commitHash": fields[0],
                                "author": fields[1],
                                "date": fields[2],
                                "subject": fields[3],
                                "message": fields[3],
                                "commitMessage": fields[3],
                            }
                            if len(fields) == 4 else {}
                        )
                        touched = set()
                        continue
                    if not part or not current:
                        continue
                    name = part.decode("utf-8", "replace")
                    if prefix:
                        if not name.startswith(prefix):
                            continue
                        name = name[len(prefix):]
                    child = name.split("/", 1)[0]
                    if not child or child in touched:
                        continue
                    touched.add(child)
                    summary = summaries.get(child)
                    if summary is None:
                        summary = dict(current)
                        summary["changeCount"] = 0
                        summaries[child] = summary
                    if summary["changeCount"] < 20:
                        summary["changeCount"] += 1
                        if summary["changeCount"] >= 20:
                            capped.add(child)
                    summary["changeCountCapped"] = summary["changeCount"] >= 20
                if names and names <= capped:
                    break
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
        if process.returncode not in (0, -9) and not summaries:
            raise GitError("git operation failed")
        return summaries

    def _read_analysis_blobs(
        self,
        commit: str,
        blobs: list[tuple[int, str]],
    ) -> tuple[dict[str, bytes], bool]:
        """Read selected committed blobs through one bounded cat-file process."""

        selected: list[tuple[int, str]] = []
        total = 0
        coverage_candidates = sorted(
            (
                (size, path)
                for size, path in blobs
                if path.rsplit("/", 1)[-1].lower() in COVERAGE_ARTIFACT_NAMES
                and size <= MAX_COVERAGE_ARTIFACT_BYTES
            ),
            key=lambda item: (
                0 if item[1].lower().endswith("lcov.info") else 1,
                item[1],
            ),
        )
        source_candidates = sorted(
            (
                (size, path)
                for size, path in blobs
                if Path(path).suffix.lower() in ANALYZED_SOURCE_SUFFIXES
                and size <= MAX_ANALYSIS_FILE_BYTES
            ),
            key=lambda item: item[1],
        )
        for size, path in coverage_candidates + source_candidates:
            if (
                len(selected) >= MAX_ANALYSIS_FILES
                or total + size > MAX_ANALYSIS_TOTAL_BYTES
                or any(character in path for character in ("\x00", "\r", "\n"))
            ):
                continue
            selected.append((size, path))
            total += size
        partial = len(source_candidates) > sum(
            Path(path).suffix.lower() in ANALYZED_SOURCE_SUFFIXES
            for _size, path in selected
        )
        if not selected:
            return {}, partial
        request = b"".join(
            f"{commit}:{path}\n".encode("utf-8") for _size, path in selected
        )
        output = _run_git(
            self.git_dir,
            ["cat-file", "--batch"],
            input_bytes=request,
            max_output=MAX_ANALYSIS_TOTAL_BYTES + len(selected) * 160,
        )
        cursor = 0
        result: dict[str, bytes] = {}
        for _expected_size, path in selected:
            line_end = output.find(b"\n", cursor)
            if line_end < 0:
                break
            header = output[cursor:line_end].split()
            cursor = line_end + 1
            if len(header) != 3 or header[1] != b"blob":
                continue
            try:
                size = int(header[2])
            except ValueError:
                break
            end = cursor + size
            if size < 0 or end > len(output):
                break
            result[path] = output[cursor:end]
            cursor = end + 1
        return result, partial

    def _build_analysis(self, commit: str) -> dict[str, Any]:
        all_blobs = self._all_blobs(commit)
        repository_partial = len(all_blobs) > MAX_ANALYSIS_REPOSITORY_FILES
        blobs = all_blobs[:MAX_ANALYSIS_REPOSITORY_FILES]
        known_paths = {path for _size, path in blobs}
        raw_blobs, partial = self._read_analysis_blobs(commit, blobs)
        partial = partial or repository_partial
        source_text: dict[str, str] = {}
        for path, payload in raw_blobs.items():
            if Path(path).suffix.lower() not in ANALYZED_SOURCE_SUFFIXES:
                continue
            try:
                source_text[path] = payload.decode("utf-8")
            except UnicodeDecodeError:
                continue

        suffix_index: dict[str, list[str]] = {}
        for path in known_paths:
            parts = path.split("/")
            for start in range(len(parts)):
                suffix = "/".join(parts[start:])
                suffix_index.setdefault(suffix, []).append(path)
        adjacency: dict[str, list[str]] = {path: [] for path in source_text}
        edge_count = 0
        for source, text in source_text.items():
            for spec in _extract_dependency_specs(source, text):
                target = _dependency_target(
                    source, spec, known_paths, suffix_index
                )
                if (
                    target
                    and target not in adjacency[source]
                    and edge_count < MAX_DEPENDENCY_EDGES
                ):
                    adjacency[source].append(target)
                    edge_count += 1
        depths = _dependency_depths(adjacency)

        coverage: dict[str, float] = {}
        artifacts: list[str] = []
        for path, payload in raw_blobs.items():
            name = path.rsplit("/", 1)[-1].lower()
            if name not in COVERAGE_ARTIFACT_NAMES:
                continue
            if name.endswith(".json"):
                parsed = _parse_json_coverage(payload, known_paths)
            elif name == "lcov.info":
                parsed = _parse_lcov_coverage(payload, known_paths)
            else:
                parsed = _parse_xml_coverage(payload, known_paths)
            if parsed:
                artifacts.append(path)
                for source_path, percent in parsed.items():
                    coverage.setdefault(source_path, percent)

        return {
            "commit": commit,
            "adjacency": adjacency,
            "depths": depths,
            "coverage": coverage,
            "summary": {
                "commit": commit,
                "dependency": {
                    "status": "partial" if partial else "complete",
                    "filesParsed": len(source_text),
                    "edgeCount": edge_count,
                    "maxFiles": MAX_ANALYSIS_FILES,
                    "maxRepositoryFiles": MAX_ANALYSIS_REPOSITORY_FILES,
                    "maxBytes": MAX_ANALYSIS_TOTAL_BYTES,
                },
                "coverage": {
                    "status": "available" if artifacts else "unavailable",
                    "artifacts": artifacts[:12],
                    "files": len(coverage),
                },
            },
        }

    def _analysis(self, commit: str) -> dict[str, Any]:
        with self._analysis_lock:
            if self._analysis_commit == commit and self._analysis_value:
                return self._analysis_value
            value = self._build_analysis(commit)
            self._analysis_commit = commit
            self._analysis_value = value
            return value

    @staticmethod
    def _analysis_child(path: str, candidate: str) -> str:
        prefix = path + "/" if path else ""
        if prefix and not candidate.startswith(prefix):
            return ""
        remainder = candidate[len(prefix):] if prefix else candidate
        first = remainder.split("/", 1)[0]
        return prefix + first if first else ""

    def _tree_analysis_fields(
        self,
        analysis: Mapping[str, Any],
        directory: str,
        full_path: str,
        object_type: str,
    ) -> dict[str, Any]:
        adjacency = analysis.get("adjacency", {})
        depths = analysis.get("depths", {})
        coverage = analysis.get("coverage", {})
        if object_type == "blob":
            return {
                "path": full_path,
                "dependencies": list(adjacency.get(full_path, []))[:80],
                "dependencyDepth": int(depths.get(full_path, 0)),
                "coverage": coverage.get(full_path),
                "analysisCommit": analysis.get("commit", ""),
            }

        prefix = full_path + "/"
        descendants = [
            source for source in adjacency if source.startswith(prefix)
        ]
        dependencies: set[str] = set()
        for source in descendants:
            for target in adjacency.get(source, []):
                child = self._analysis_child(directory, target)
                if child and child != full_path:
                    dependencies.add(child)
        covered = [
            float(percent)
            for source, percent in coverage.items()
            if source.startswith(prefix)
        ]
        return {
            "path": full_path,
            "dependencies": sorted(dependencies)[:80],
            "dependencyDepth": max(
                (int(depths.get(source, 0)) for source in descendants),
                default=0,
            ),
            "coverage": (
                round(sum(covered) / len(covered), 1) if covered else None
            ),
            "analysisCommit": analysis.get("commit", ""),
        }

    def _numbered_directories(self, commit: str, path: str) -> set[str]:
        """Return numeric child trees below one collaboration directory."""
        prefix = path.rstrip("/") + "/"
        output = _run_git(
            self.git_dir,
            [
                "ls-tree", "-d", "-z", "--name-only", commit, "--",
                ":(top,literal)" + prefix,
            ],
            max_output=16 * 1024 * 1024,
        )
        directories: set[str] = set()
        for raw_path in output.split(b"\0"):
            candidate = raw_path.decode("utf-8", "replace")
            if not candidate.startswith(prefix):
                continue
            name = candidate[len(prefix):]
            if re.fullmatch(r"[1-9][0-9]{0,8}", name):
                directories.add(name)
        return directories

    def _matching_paths(
        self,
        commit: str,
        pattern: str,
        pathspec: str,
    ) -> set[str]:
        """Return paths whose committed text matches a bounded git-grep."""
        code, output = _run_git_status(
            self.git_dir,
            ["grep", "-l", "-z", "-E", pattern, commit, "--", pathspec],
            max_output=16 * 1024 * 1024,
            allowed_codes=(0, 1),
        )
        if code == 1:
            return set()
        tree_prefix = commit.encode("ascii") + b":"
        paths: set[str] = set()
        for raw_path in output.split(b"\0"):
            if raw_path.startswith(tree_prefix):
                raw_path = raw_path[len(tree_prefix):]
            if raw_path:
                paths.add(raw_path.decode("utf-8", "replace"))
        return paths

    def _independent_pull_refs(self) -> tuple[str, dict[str, str]]:
        """Map each ``refs/heads/forkmesh/pull/<n>`` number to its commit.

        Per-pull metadata branches are authoritative; the aggregate
        ``forkmesh/pulls`` ref is only a compatibility index that can lag them.
        Also returns the raw listing so callers can key caches on it.
        """
        try:
            raw = _run_git(
                self.git_dir,
                ["for-each-ref", "--sort=refname",
                 "--format=%(refname) %(objectname)",
                 "refs/heads/forkmesh/pull/"],
                max_output=4 * 1024 * 1024,
            ).decode("ascii", "replace")
        except GitError:
            raw = ""
        refs: dict[str, str] = {}
        for line in raw.splitlines():
            fields = line.split(" ", 1)
            match = re.fullmatch(
                r"refs/heads/forkmesh/pull/([1-9][0-9]{0,8})",
                fields[0],
            )
            if len(fields) != 2 or not match:
                continue
            oid = fields[1].strip().lower()
            if re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", oid):
                refs[match.group(1)] = oid
        return raw, refs

    def _deleted_pull_times(self, pulls_commit: str) -> dict[str, int]:
        if not pulls_commit:
            return {}
        if pulls_commit == self._deleted_pulls_key:
            return self._deleted_pulls_value
        try:
            output = _run_git(
                self.git_dir,
                ["log", "--format=%x00%ct", "--diff-filter=D", "--name-only",
                 pulls_commit, "--",
                 ":(top,glob).forkmesh/pulls/*/pull.md"],
                max_output=16 * 1024 * 1024,
            ).decode("utf-8", "replace")
        except GitError:
            return {}
        deleted: dict[str, int] = {}
        for chunk in output.split("\0"):
            lines = [line.strip() for line in chunk.splitlines() if line.strip()]
            if not lines or not lines[0].isdigit():
                continue
            when = int(lines[0])
            for path in lines[1:]:
                match = re.fullmatch(
                    r"\.forkmesh/pulls/([1-9][0-9]{0,8})/pull\.md", path)
                if match:
                    deleted.setdefault(match.group(1), when)
        self._deleted_pulls_value = deleted
        self._deleted_pulls_key = pulls_commit
        return deleted

    def _live_independent_pull_refs(
        self, pulls_commit: str, refs: dict[str, str]
    ) -> dict[str, str]:
        deleted = self._deleted_pull_times(pulls_commit)
        candidates = {number: oid for number, oid in refs.items()
                      if number in deleted}
        if not candidates:
            return dict(refs)
        indexed = self._numbered_directories(pulls_commit, ".forkmesh/pulls")
        candidates = {number: oid for number, oid in candidates.items()
                      if number not in indexed}
        if not candidates:
            return dict(refs)
        try:
            output = _run_git(
                self.git_dir,
                ["log", "--no-walk=unsorted", "--format=%H %ct",
                 *sorted(set(candidates.values()))],
                max_output=1024 * 1024,
            ).decode("ascii", "replace")
        except GitError:
            return dict(refs)
        times: dict[str, int] = {}
        for line in output.splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[1].isdigit():
                times[fields[0].lower()] = int(fields[1])
        return {
            number: oid for number, oid in refs.items()
            if number not in candidates
            or times.get(oid, 0) > deleted[number]
        }

    def _root_collaboration_counts(self, commit: str) -> dict[str, int]:
        """Count live issue and pull state without loading either web tab."""
        try:
            pulls_commit = self.resolve_commit("refs/heads/forkmesh/pulls")
        except GatewayError:
            pulls_commit = ""
        pull_refs_raw, independent_pull_refs = self._independent_pull_refs()
        cache_key = (commit, pulls_commit, pull_refs_raw)
        with self._counts_lock:
            if cache_key == self._counts_key and self._counts_value:
                return dict(self._counts_value)

            open_issue_dirs = self._numbered_directories(
                commit, ".forkmesh/issues/open")
            closed_issue_dirs = self._numbered_directories(
                commit, ".forkmesh/issues/closed")
            legacy_issue_dirs = self._numbered_directories(
                commit, ".forkmesh/issues")
            # Status-split folders supersede any stale pre-migration copy.
            legacy_issue_dirs -= open_issue_dirs | closed_issue_dirs
            closed_legacy_paths = self._matching_paths(
                commit,
                r'"status"[[:space:]]*:[[:space:]]*"closed"',
                ":(top,glob).forkmesh/issues/[0-9]*/issue-*.json",
            )
            closed_legacy = {
                match.group(1)
                for path in closed_legacy_paths
                if (match := re.fullmatch(
                    r"\.forkmesh/issues/([1-9][0-9]{0,8})/"
                    r"issue-[1-9][0-9]{0,8}\.json",
                    path,
                ))
                and match.group(1) in legacy_issue_dirs
            }
            open_issues = len(open_issue_dirs) + (
                len(legacy_issue_dirs) - len(closed_legacy))
            closed_issues = len(closed_issue_dirs) + len(closed_legacy)

            pull_dirs: set[str] = set()
            open_pull_dirs: set[str] = set()
            merged_pull_dirs: set[str] = set()
            if pulls_commit:
                pull_dirs = self._numbered_directories(
                    pulls_commit, ".forkmesh/pulls")
                open_pull_paths = self._matching_paths(
                    pulls_commit,
                    r"^status:[[:space:]]*open[[:space:]]*$",
                    ":(top,glob).forkmesh/pulls/*/pull.md",
                )
                open_pull_dirs = {
                    match.group(1)
                    for path in open_pull_paths
                    if (match := re.fullmatch(
                        r"\.forkmesh/pulls/([1-9][0-9]{0,8})/pull\.md",
                        path,
                    ))
                    and match.group(1) in pull_dirs
                }
                merged_pull_paths = self._matching_paths(
                    pulls_commit,
                    r"^status:[[:space:]]*merged[[:space:]]*$",
                    ":(top,glob).forkmesh/pulls/*/pull.md",
                )
                merged_pull_dirs = {
                    match.group(1)
                    for path in merged_pull_paths
                    if (match := re.fullmatch(
                        r"\.forkmesh/pulls/([1-9][0-9]{0,8})/pull\.md",
                        path,
                    ))
                    and match.group(1) in pull_dirs
                }
            aggregate_pull_dirs = set(pull_dirs)
            independent_pull_refs = self._live_independent_pull_refs(
                pulls_commit, independent_pull_refs)
            pull_dirs.update(independent_pull_refs)
            # Normally the compatibility index has the same records, so this
            # performs no extra reads. If that index is missing one PR, read
            # only the isolated record needed to classify it.
            for number in pull_dirs - aggregate_pull_dirs:
                ref = independent_pull_refs[number]
                try:
                    record = _run_git(
                        self.git_dir,
                        ["show", f"{ref}:.forkmesh/pulls/{number}/pull.md"],
                        max_output=256 * 1024,
                    ).decode("utf-8", "replace")
                except GitError:
                    continue
                if re.search(r"(?m)^status:[ \t]*open[ \t]*$", record):
                    open_pull_dirs.add(number)
                elif re.search(r"(?m)^status:[ \t]*merged[ \t]*$", record):
                    merged_pull_dirs.add(number)
            # A malformed legacy record can contain more than one status line.
            # Keep the buckets mutually exclusive and preserve the longstanding
            # rule that an explicit open state wins.
            merged_pull_dirs.difference_update(open_pull_dirs)

            discussions = self._numbered_directories(
                commit, ".forkmesh/discussions")
            counts = {
                "issues": open_issues + closed_issues,
                "openIssues": open_issues,
                "closedIssues": closed_issues,
                "pulls": len(pull_dirs),
                "openPulls": len(open_pull_dirs),
                "mergedPulls": len(merged_pull_dirs),
                "closedPulls": len(pull_dirs) - len(open_pull_dirs),
                "closedUnmergedPulls": (
                    len(pull_dirs) - len(open_pull_dirs) - len(merged_pull_dirs)
                ),
                "discussions": len(discussions),
            }
            self._counts_key = cache_key
            self._counts_value = dict(counts)
            return counts

    def tree(self, query: Mapping[str, str]) -> dict[str, Any]:
        path = _safe_repo_path(query.get("path", ""))
        commit = self.resolve_commit(query.get("ref", ""))
        # Pull and issue directory reads are control metadata, not code-graph
        # exploration. Building the whole-repository dependency/coverage
        # analysis here made a 44-PR listing scan thousands of files before it
        # could return, then _commit_summary spawned one `git log` per PR. On a
        # one-vCPU mirror (especially while fsck is running) that turned a
        # small exact-ref read into a 20-35 second request. Keep the regular
        # rich analysis everywhere else, while these two explicit public
        # metadata namespaces receive commit-pinned neutral analysis fields.
        collaboration_tree = any(
            path == root or path.startswith(root + "/")
            for root in COLLABORATION_TREE_ROOTS
        )
        analysis = None if collaboration_tree else self._analysis(commit)
        # ``git ls-tree <commit>:<path>`` exits 128 when an otherwise valid
        # path is absent.  The dispatch boundary cannot distinguish that
        # expected repository answer from a corrupt/unavailable object store,
        # so it used to turn ordinary missing paths into 503
        # ``mirror_unavailable`` responses.  Query from the commit root with a
        # literal directory pathspec instead: Git then exits zero with no
        # output for an absent path, while real command failures still raise
        # GitError and retain the 503 contract.
        prefix = path + "/" if path else ""
        arguments = ["ls-tree", "-l", "-z", commit]
        if prefix:
            arguments.extend(["--", ":(top,literal)" + prefix])
        output = _run_git(
            self.git_dir,
            arguments,
            max_output=16 * 1024 * 1024,
        )
        if prefix and not output:
            raise GatewayError("repository path was not found")
        entries = []
        records = output.split(b"\0")
        if len(records) > MAX_TREE_ENTRIES + 1:
            records = records[:MAX_TREE_ENTRIES]
            truncated = True
        else:
            truncated = False
        # A large directory gets its last-commit column from one history
        # walk instead of one `git log` per entry (see _directory_summaries).
        batch_summaries = None
        if not collaboration_tree and sum(
            1 for record in records if b"\t" in record
        ) >= TREE_BATCH_SUMMARY_MIN_ENTRIES:
            batch_summaries = self._directory_summaries(
                commit, prefix, {
                    record.split(b"\t", 1)[1].decode("utf-8", "replace")[
                        len(prefix):]
                    for record in records if b"\t" in record
                })
        for record in records:
            if not record or b"\t" not in record:
                continue
            metadata, raw_name = record.split(b"\t", 1)
            fields = metadata.split()
            if len(fields) < 4:
                continue
            name = raw_name.decode("utf-8", "replace")
            if prefix:
                if not name.startswith(prefix):
                    raise GitError("git tree returned an unexpected path")
                name = name[len(prefix):]
            full_path = name if not path else path + "/" + name
            try:
                size = int(fields[3]) if fields[3] != b"-" else 0
            except ValueError:
                size = 0
            entry = {
                "name": name,
                "type": fields[1].decode("ascii", "replace"),
                "size": max(0, size),
                "mode": fields[0].decode("ascii", "replace"),
                "oid": fields[2].decode("ascii", "replace"),
            }
            if collaboration_tree:
                entry.update({
                    "path": full_path,
                    "dependencies": [],
                    "dependencyDepth": 0,
                    "coverage": None,
                    "analysisCommit": commit,
                })
            else:
                entry.update(
                    self._tree_analysis_fields(
                        analysis, path, full_path, entry["type"]
                    )
                )
                if batch_summaries is not None:
                    entry.update(batch_summaries.get(name, {}))
                else:
                    entry.update(self._commit_summary(commit, full_path))
            entries.append(entry)
        analysis_summary = (
            {
                "commit": commit,
                "dependency": {
                    "status": "not-requested",
                    "filesParsed": 0,
                    "edgeCount": 0,
                    "maxFiles": MAX_ANALYSIS_FILES,
                    "maxRepositoryFiles": MAX_ANALYSIS_REPOSITORY_FILES,
                    "maxBytes": MAX_ANALYSIS_TOTAL_BYTES,
                },
                "coverage": {
                    "status": "not-requested",
                    "artifacts": [],
                    "files": 0,
                },
            }
            if collaboration_tree
            else analysis["summary"]
        )
        result = {
            "ok": True,
            "commit": commit,
            "entries": entries,
            "latestCommit": self._commit_summary(commit),
            "analysis": analysis_summary,
            "truncated": truncated,
        }
        # The catalog is an asynchronously published snapshot. Attach exact
        # collaboration tallies to the root tree so repo-tab badges can update
        # as soon as the page opens, without eagerly fetching every issue or PR
        # record. Pulls are counted from their independent metadata branch.
        if not path:
            result["counts"] = self._root_collaboration_counts(commit)
        # The root openPulls count unions the aggregate index with every
        # per-pull branch. Advertise those branches with the pull listing so a
        # web list can read the records the index is missing, instead of
        # showing fewer rows than its own Open badge (issue #683).
        if path == ".forkmesh/pulls":
            try:
                pulls_commit = self.resolve_commit("refs/heads/forkmesh/pulls")
            except GatewayError:
                pulls_commit = ""
            result["pullRefs"] = self._live_independent_pull_refs(
                pulls_commit, self._independent_pull_refs()[1])
        return result

    def _tree_entries(
        self, commit: str, paths: Iterable[str]
    ) -> dict[str, tuple[str, str]]:
        pathspecs = [":(top,literal)" + path for path in paths]
        if not pathspecs:
            return {}
        output = _run_git(
            self.git_dir,
            ["ls-tree", "-z", commit, "--", *pathspecs],
        )
        entries: dict[str, tuple[str, str]] = {}
        for record in output.split(b"\0"):
            meta, separator, name = record.partition(b"\t")
            fields = meta.split()
            if not separator or len(fields) != 3:
                continue
            entries[name.decode("utf-8", "surrogateescape")] = (
                fields[1].decode("ascii", "replace"),
                fields[2].decode("ascii", "replace"),
            )
        return entries

    def blob(self, query: Mapping[str, str]) -> dict[str, Any]:
        path = _safe_repo_path(query.get("path"), allow_empty=False)
        commit = self.resolve_commit(query.get("ref", ""))
        return self._blob_payload(self._tree_entries(commit, [path]).get(path))

    def _blob_payload(self, entry: tuple[str, str] | None) -> dict[str, Any]:
        if entry is None:
            raise GatewayError("repository path was not found")
        object_type, object_id = entry
        if object_type != "blob" or not GIT_OID_RE.match(object_id):
            raise GatewayError("repository path is not a blob")
        output = _run_git(
            self.git_dir,
            ["cat-file", "blob", object_id],
            max_output=MAX_JSON_BLOB + 1,
        )
        truncated = len(output) > MAX_JSON_BLOB
        output = output[:MAX_JSON_BLOB]
        try:
            content = output.decode("utf-8")
            encoding = "utf8"
        except UnicodeDecodeError:
            content = base64.b64encode(output).decode("ascii")
            encoding = "base64"
        return {
            "ok": True,
            "size": len(output),
            "truncated": truncated,
            "encoding": encoding,
            "content": content,
        }

    def blobs(self, paths: Iterable[str], ref: str = "") -> dict[str, Any]:
        """Bounded compatibility batch for the repository web UI."""
        # Resolve once so every member of the batch and the returned proof name
        # the same immutable repository state even when the caller supplied a
        # moving branch name.
        commit = self.resolve_commit(ref)
        output: dict[str, Any] = {}
        # Reserve the bounded envelope before accounting for member payloads.
        used = len(commit) + 128
        truncated = False
        requested = list(paths)[:60]
        safe_paths: dict[str, str] = {}
        for path in requested:
            try:
                safe_paths[path] = _safe_repo_path(path, allow_empty=False)
            except GatewayError:
                pass
        try:
            entries = self._tree_entries(commit, dict.fromkeys(
                safe_paths.values()))
        except GitError:
            entries = {}
        for path in requested:
            try:
                result = self._blob_payload(
                    entries.get(safe_paths.get(path, "")))
            except (GatewayError, GitError):
                result = None
            encoded_size = len(
                json.dumps(
                    result, sort_keys=True, separators=(",", ":"),
                    ensure_ascii=False,
                ).encode("utf-8")
            )
            if used + encoded_size > MAX_JSON_OUTPUT:
                output[path] = None
                truncated = True
                continue
            output[path] = result
            used += encoded_size
        return {
            "ok": True,
            "commit": commit,
            "blobs": output,
            "truncated": truncated,
        }

    def raw_spec(self, query: Mapping[str, str]) -> "StreamSpec":
        path = _safe_repo_path(query.get("path"), allow_empty=False)
        commit = self.resolve_commit(query.get("ref", ""))
        object_name = f"{commit}:{path}"
        # A catalog may legitimately advertise an immutable commit from before
        # an optional image or metadata file was added.  ``cat-file -t`` uses
        # the same non-zero exit for that ordinary absence and for a damaged
        # object store, which made the dispatch boundary report a missing raw
        # file as 503 ``mirror_unavailable``.  Ask ``ls-tree`` for the exact
        # literal path first: an absent path is a successful empty result,
        # while repository corruption still raises GitError and retains the
        # availability-failure contract.
        entry = _run_git(
            self.git_dir,
            ["ls-tree", "-z", commit, "--", ":(top,literal)" + path],
            max_output=1024,
        )
        if not entry:
            raise GatewayError("repository path was not found")
        object_type = _run_git(
            self.git_dir,
            ["cat-file", "-t", object_name],
            max_output=64,
        ).strip()
        if object_type != b"blob":
            raise GatewayError("repository path is not a blob")
        size_raw = _run_git(
            self.git_dir,
            ["cat-file", "-s", object_name],
            max_output=64,
        )
        try:
            size = int(size_raw)
        except ValueError as exc:
            raise GitError("repository object has an invalid size") from exc
        guessed_type = mimetypes.guess_type(path)[0] or ""
        active_types = {
            "image/svg+xml",
            "text/html",
            "application/xhtml+xml",
            "application/xml",
            "text/xml",
            "application/javascript",
            "text/javascript",
        }
        if guessed_type in active_types:
            content_type = "application/octet-stream"
            disposition = "attachment"
        elif guessed_type.startswith("text/"):
            content_type = "text/plain; charset=utf-8"
            disposition = "inline"
        else:
            content_type = guessed_type or "application/octet-stream"
            disposition = "inline"
        return StreamSpec(
            kind="process",
            content_type=content_type,
            content_length=size,
            command=tuple(
                _git_prefix(self.git_dir) + ["cat-file", "blob", object_name]
            ),
            filename=Path(path).name,
            disposition=disposition,
        )

    def history(self, query: Mapping[str, str]) -> dict[str, Any]:
        all_refs = str(query.get("all") or "")
        if all_refs not in ("", "1"):
            raise GatewayError("invalid history scope")
        if all_refs and query.get("ref"):
            raise GatewayError("history scope is ambiguous")
        since_raw = str(query.get("since") or "")
        until_raw = str(query.get("until") or "")
        if all_refs and (not since_raw or not until_raw):
            raise GatewayError("all-ref history requires a time range")
        if bool(since_raw) != bool(until_raw):
            raise GatewayError("history time range is incomplete")
        since = until = 0
        if since_raw:
            if not since_raw.isascii() or not since_raw.isdigit() or \
                    not until_raw.isascii() or not until_raw.isdigit():
                raise GatewayError("invalid history time range")
            since, until = int(since_raw), int(until_raw)
            if since < 0 or until <= since or until - since > 366 * 86400:
                raise GatewayError("invalid history time range")
        path = _safe_repo_path(query.get("path", ""))
        if path and all_refs:
            raise GatewayError("history scope is ambiguous")
        commit = self.resolve_commit("" if all_refs else query.get("ref", ""))
        arguments = ["log"]
        if all_refs:
            # Traverse every published branch/tag while excluding operational
            # refs (merge staging, jobs) that public reads must never expose.
            # Git still emits a commit reachable from several refs only once.
            arguments += ["--branches", "--tags", "--remotes", "HEAD"]
        else:
            arguments += ["-n", "60", commit]
        if since_raw:
            arguments += [f"--since=@{since}", f"--until=@{until}"]
        arguments.append("--format=%H%x1f%an%x1f%cI%x1f%P%x1f%s")
        if path:
            arguments += ["--", path]
        output = _run_git(
            self.git_dir,
            arguments,
            max_output=4 * 1024 * 1024 if all_refs else 512 * 1024,
        )
        commits = []
        for row in output.decode("utf-8", "replace").splitlines():
            fields = row.split("\x1f", 4)
            if len(fields) == 5:
                commits.append(
                    {
                        "hash": fields[0],
                        "author": fields[1],
                        "date": fields[2],
                        "parents": fields[3],
                        "subject": fields[4],
                    }
                )
        now = datetime.now(timezone.utc)
        today_start = datetime(
            now.year, now.month, now.day, tzinfo=timezone.utc
        )
        week_start_seconds = (
            int(today_start.timestamp()) - today_start.weekday() * 24 * 60 * 60
        )
        month_start = datetime(
            now.year, now.month, 1, tzinfo=timezone.utc
        )

        windows = {
            "today": int(today_start.timestamp()),
            "week": week_start_seconds,
            "month": int(month_start.timestamp()),
        }

        # The three windows nest inside the earliest one, so one walk answers
        # all of them. Three separate `rev-list --count` walks used to run per
        # history read, and under `all=1` each of those traverses every branch
        # and tag - the dominant cost of a Calendar page that reads activity
        # from every repository at once.
        earliest = min(windows.values())
        count_arguments = ["rev-list", f"--since=@{earliest}", "--format=%ct"]
        if all_refs:
            count_arguments += ["--branches", "--tags", "--remotes", "HEAD"]
        else:
            count_arguments.append(commit)
        # One line per commit rather than one number, so the ceiling is the
        # generous shared one: ~140k commits since the start of this month.
        stamps: list[int] = []
        for row in _run_git(
            self.git_dir, count_arguments, max_output=MAX_JSON_OUTPUT,
        ).decode("ascii", "replace").splitlines():
            row = row.strip()
            # `rev-list --format` prefixes each commit with a "commit <oid>"
            # header line; only the timestamp rows are counted.
            if not row or not row.isascii() or not row.isdigit():
                continue
            stamps.append(int(row))

        def count_since(start_seconds: int) -> int:
            return sum(1 for stamp in stamps if stamp >= start_seconds)
        return {
            "ok": True,
            "commit": commit,
            "commits": commits,
            "activity": {
                "generatedAt": now.isoformat(),
                "timezone": "UTC",
                "weekStartsOn": "monday",
                "windows": {
                    name: {
                        "start": datetime.fromtimestamp(
                            start_seconds, timezone.utc
                        ).isoformat(),
                        "commits": count_since(start_seconds),
                    }
                    for name, start_seconds in windows.items()
                },
            },
        }

    def commit(self, query: Mapping[str, str]) -> dict[str, Any]:
        requested = str(query.get("path") or "").lower()
        if not COMMIT_RE.fullmatch(requested):
            raise GatewayError("invalid commit hash")
        commit = self.resolve_commit(requested)
        metadata = _run_git(
            self.git_dir,
            [
                "show",
                "-s",
                "--format=%H%x1f%an%x1f%cI%x1f%P%x1f%s%x1f%b",
                commit,
            ],
            max_output=512 * 1024,
        ).decode("utf-8", "replace")
        fields = metadata.split("\x1f", 5)
        if len(fields) < 5:
            raise GitError("commit metadata is invalid")
        parents = fields[3].strip().split()
        base = (
            parents[0]
            if parents
            else "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
        )
        numstat = _run_git(
            self.git_dir,
            ["diff", "--numstat", base, commit],
            max_output=2 * 1024 * 1024,
        )
        files = []
        for row in numstat.decode("utf-8", "replace").splitlines()[:1000]:
            parts = row.split("\t", 2)
            if len(parts) == 3:
                files.append(
                    {"path": parts[2], "adds": parts[0], "dels": parts[1]}
                )
        diff = _run_git(
            self.git_dir,
            ["diff", "-M", "--no-color", base, commit],
            max_output=2 * 1024 * 1024,
        )
        return {
            "ok": True,
            "hash": fields[0],
            "author": fields[1],
            "date": fields[2],
            "parents": parents,
            "subject": fields[4],
            "body": fields[5] if len(fields) > 5 else "",
            "files": files,
            "diff": diff.decode("utf-8", "replace"),
            "diffTruncated": len(diff) >= 2 * 1024 * 1024,
        }

    def compare(self, query: Mapping[str, str]) -> dict[str, Any]:
        """Return one immutable, portable pull-request change set.

        The browser cannot safely hand the owner only mutable branch names:
        by the time the inbox drains, either ref may have moved or may not
        exist on the owner's node. Resolve both names once, then carry the
        exact binary patch and (for linear histories) authored commit series
        that the submitter signs. Merge commits fall back to the net patch
        because ``format-patch`` intentionally omits merge commits and could
        otherwise describe a different tree.
        """
        if not query.get("base") or not query.get("head"):
            raise GatewayError("repository comparison requires base and head")
        base = self.resolve_commit(query["base"])
        head = self.resolve_commit(query["head"])
        merge_base_raw = _run_git(
            self.git_dir,
            ["merge-base", base, head],
            max_output=128,
            allow_exit_one=True,
        )
        merge_base = merge_base_raw.decode("ascii", "ignore").strip().lower()
        if not re.fullmatch(
            r"(?:[0-9a-f]{40}|[0-9a-f]{64})", merge_base
        ):
            raise GatewayError("repository refs do not share a merge base")

        count_raw = _run_git(
            self.git_dir,
            [
                "rev-list",
                "--count",
                f"--max-count={MAX_COMPARE_COMMITS + 1}",
                base + ".." + head,
            ],
            max_output=32,
        )
        try:
            commit_count = int(count_raw)
        except ValueError as exc:
            raise GitError("repository commit count is invalid") from exc
        if commit_count > MAX_COMPARE_COMMITS:
            raise GatewayError("repository comparison is too large")

        patch = _run_git(
            self.git_dir,
            [
                "diff",
                "--no-ext-diff",
                "--no-textconv",
                "--binary",
                merge_base,
                head,
            ],
            max_output=MAX_COMPARE_BYTES,
        )
        merges = _run_git(
            self.git_dir,
            ["rev-list", "--min-parents=2", "--max-count=1", base + ".." + head],
            max_output=128,
        ).strip()
        commits = b""
        if not merges:
            commits = _run_git(
                self.git_dir,
                [
                    "format-patch",
                    "--binary",
                    "--no-signature",
                    "--stdout",
                    base + ".." + head,
                ],
                max_output=MAX_COMPARE_BYTES,
            )
        if len(patch) + len(commits) > MAX_COMPARE_BYTES:
            raise GatewayError("repository comparison is too large")
        return {
            "ok": True,
            "baseOid": base,
            "headOid": head,
            "mergeBaseOid": merge_base,
            "commitCount": commit_count,
            "patch": patch.decode("utf-8", "replace"),
            "commits": commits.decode("utf-8", "replace"),
        }

    def cherry(
        self, query: Mapping[str, str], upstream_dir: Path
    ) -> dict[str, Any]:
        values = {
            key: str(query.get(key) or "").lower()
            for key in ("base", "head", "limit")
        }
        if not all(
            FULL_COMMIT_ID_RE.fullmatch(values[key])
            for key in ("base", "head")
        ) or (values["limit"] and not FULL_COMMIT_ID_RE.fullmatch(
                values["limit"])):
            raise GatewayError("upstream comparison needs commit ids")

        def git(arguments: list[str], **options: Any) -> bytes:
            return _run_git(
                self.git_dir, arguments, alternate=upstream_dir, **options)

        for key, value in values.items():
            if value and not git(
                ["rev-parse", "--verify", "--quiet", "--end-of-options",
                 value + "^{commit}"],
                max_output=128, allow_exit_one=True,
            ).strip():
                raise GatewayError("upstream comparison commit is missing")
        since = values["limit"] or values["base"]
        count_raw = git(
            ["rev-list", "--count", f"--max-count={MAX_COMPARE_COMMITS + 1}",
             "^" + since, "^" + values["base"], values["head"]],
            max_output=32,
        )
        try:
            own = int(count_raw)
        except ValueError as exc:
            raise GitError("repository commit count is invalid") from exc
        if own > MAX_COMPARE_COMMITS:
            raise GatewayError("repository comparison is too large")
        upstream_raw = git(
            ["rev-list", "--count",
             f"--max-count={MAX_CHERRY_UPSTREAM_COMMITS + 1}",
             "^" + values["head"], values["base"]],
            max_output=32,
        )
        try:
            upstream = int(upstream_raw)
        except ValueError as exc:
            raise GitError("repository commit count is invalid") from exc
        if upstream > MAX_CHERRY_UPSTREAM_COMMITS:
            raise GatewayError("upstream comparison is too large")
        arguments = ["cherry", values["base"], values["head"]]
        if values["limit"]:
            arguments.append(values["limit"])
        lines = git(
            arguments, max_output=(MAX_COMPARE_COMMITS + 1) * 72,
            timeout=PROCESS_TIMEOUT_SECONDS,
        ).decode("ascii", "replace").splitlines()
        commits = []
        for line in lines:
            mark, _, oid = line.strip().partition(" ")
            if mark in ("+", "-") and FULL_COMMIT_ID_RE.fullmatch(oid):
                commits.append({"commit": oid, "upstream": mark == "-"})
        pending = sum(1 for item in commits if not item["upstream"])
        return {
            "ok": True,
            "baseOid": values["base"],
            "headOid": values["head"],
            "limitOid": values["limit"],
            "commits": commits,
            "pending": pending,
            "merged": len(commits) - pending,
        }

    def merge_preview(self, query: Mapping[str, str]) -> dict[str, Any]:
        """Report whether ``head`` still merges cleanly into ``base``.

        A reviewer should learn that a pull request is stale or conflicted
        before spending a click on the merge control, and the only honest
        source for that is a real merge trial on the mirror that would run it.
        ``merge-tree --write-tree`` performs the whole three-way merge in the
        object database: it checks nothing out, runs no hook, and writes no
        ref, so this stays an ordinary read-only route.
        """
        if not query.get("base") or not query.get("head"):
            raise GatewayError("repository comparison requires base and head")
        base = self.resolve_commit(query["base"])
        head = self.resolve_commit(query["head"])
        merge_base_raw = _run_git(
            self.git_dir,
            ["merge-base", base, head],
            max_output=128,
            allow_exit_one=True,
        )
        merge_base = merge_base_raw.decode("ascii", "ignore").strip().lower()
        if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", merge_base):
            raise GatewayError("repository refs do not share a merge base")

        def revision_count(range_text: str) -> int:
            raw = _run_git(
                self.git_dir,
                [
                    "rev-list",
                    "--count",
                    f"--max-count={MAX_COMPARE_COMMITS + 1}",
                    range_text,
                ],
                max_output=32,
            )
            try:
                return int(raw)
            except ValueError as exc:
                raise GitError(
                    "repository commit count is invalid") from exc

        # "ahead" is what the pull request contributes; "behind" is what the
        # base branch gained since the merge base, i.e. what an update pulls in.
        ahead = revision_count(base + ".." + head)
        behind = revision_count(head + ".." + base)
        conflicts: list[str] = []
        clean = True
        if behind:
            # Same parent order as the update transaction: the head branch is
            # "ours", so a conflicting path is reported as the contributor sees
            # it when they merge the base branch locally.
            code, output = _run_git_status(
                self.git_dir,
                [
                    "merge-tree", "--write-tree", "--name-only",
                    "--no-messages", head, base,
                ],
                max_output=256 * 1024,
                allowed_codes=(0, 1),
            )
            clean = code == 0
            if not clean:
                # First line is the (unreferenced) tree object; the remaining
                # lines name the conflicted paths.
                for path in output.decode(
                        "utf-8", "replace").splitlines()[1:]:
                    entry = path.strip()
                    if entry and entry not in conflicts:
                        conflicts.append(entry)
                    if len(conflicts) >= MAX_PREVIEW_CONFLICTS:
                        break
        return {
            "ok": True,
            "baseOid": base,
            "headOid": head,
            "mergeBaseOid": merge_base,
            "ahead": ahead,
            "behind": behind,
            "clean": clean,
            "conflicts": conflicts,
        }

    def default_branch_ref(self) -> str:
        """The ref HEAD points at, or the conventional default when unborn."""
        head = _run_git(
            self.git_dir,
            ["symbolic-ref", "--quiet", "HEAD"],
            max_output=1024,
            allow_exit_one=True,
        ).decode("utf-8", "replace").strip()
        return head if head.startswith("refs/heads/") else "refs/heads/main"

    def branches(self, _query: Mapping[str, str]) -> dict[str, Any]:
        default_ref = self.default_branch_ref()
        row_format = (
            "%(refname)%00%(objectname)%00%(committerdate:iso8601)"
            "%00%(subject)%00%(authorname)"
        )

        def listing(fields_format: str) -> bytes:
            return _run_git(
                self.git_dir,
                [
                    "for-each-ref",
                    "--sort=refname",
                    "--format=" + fields_format,
                    "refs/heads/",
                    "refs/remotes/",
                ],
                max_output=2 * 1024 * 1024,
            )

        # Divergence from the default branch is what makes a branch list
        # actionable ("2 behind, 1 ahead" - the same status the desktop app
        # shows), and Git computes every ref's counts in one walk. The atom is
        # newer than the oldest Git a mirror operator may be running, and it
        # needs a resolvable default ref, so a rejected format is retried
        # without it rather than failing the whole listing.
        try:
            output = listing(
                row_format + "%00%(ahead-behind:" + default_ref + ")")
            counted = True
        except GitError:
            output = listing(row_format)
            counted = False
        branches = []
        seen: set[str] = set()
        for row in output.decode("utf-8", "replace").splitlines():
            fields = row.split("\x00", 5 if counted else 4)
            if len(fields) != (6 if counted else 5):
                continue
            name = fields[0]
            for prefix in ("refs/heads/", "refs/remotes/origin/"):
                if name.startswith(prefix):
                    name = name[len(prefix) :]
                    break
            if name.endswith("/HEAD") or name in seen:
                continue
            if (name == "forkmesh/pulls" or
                    name.startswith("forkmesh/pull/")):
                continue
            seen.add(name)
            entry = {
                "name": name,
                "commit": fields[1],
                "updatedAt": fields[2],
                "message": fields[3],
                "author": fields[4],
            }
            if counted:
                # "<ahead> <behind>" relative to the default branch: commits
                # this branch adds, then commits it is missing. Absent for a
                # ref Git could not relate to the default branch at all.
                parts = fields[5].split()
                if len(parts) == 2 and all(
                        part.isdigit() and len(part) <= 9 for part in parts):
                    entry["ahead"] = int(parts[0])
                    entry["behind"] = int(parts[1])
            branches.append(entry)
        return {
            "ok": True,
            "defaultBranch": default_ref[len("refs/heads/"):],
            "branches": branches,
        }

    def branch_trash(self) -> dict[str, Any]:
        output = _run_git(
            self.git_dir,
            [
                "for-each-ref",
                "--sort=refname",
                "--count=%d" % (MAX_BRANCH_TRASH_ENTRIES + 1),
                "--format=%(refname)%00%(objectname)",
                *BRANCH_TRASH_PREFIXES,
            ],
            max_output=4 * 1024 * 1024,
        )
        rows = output.decode("utf-8", "replace").splitlines()
        entries = []
        seen: set[tuple[str, str]] = set()
        for row in rows[:MAX_BRANCH_TRASH_ENTRIES]:
            fields = row.split("\x00")
            if len(fields) != 2 or not BRANCH_TRASH_OID_RE.fullmatch(fields[1]):
                continue
            prefix = next(
                (item for item in BRANCH_TRASH_PREFIXES
                 if fields[0].startswith(item)), "")
            branch, _, token = fields[0][len(prefix):].rpartition("/")
            if (
                not prefix
                or not BRANCH_TRASH_TOKEN_RE.fullmatch(token)
                or not MERGE_BASE_RE.fullmatch(branch)
                or ".." in branch
                or "//" in branch
                or branch.endswith((".", "/", ".lock"))
                or (branch, fields[1]) in seen
            ):
                continue
            seen.add((branch, fields[1]))
            entries.append({"branch": branch, "oid": fields[1]})
        return {
            "ok": True,
            "entries": entries,
            "truncated": len(rows) > MAX_BRANCH_TRASH_ENTRIES,
        }

    def search(self, query: Mapping[str, str]) -> dict[str, Any]:
        needle = str(query.get("path") or "").strip()
        if (
            len(needle) < 2
            or len(needle) > 120
            or "\x00" in needle
            or "\r" in needle
            or "\n" in needle
        ):
            raise GatewayError("invalid search query")
        commit = self.resolve_commit(query.get("ref", ""))
        try:
            process = subprocess.Popen(
                _git_prefix(self.git_dir)
                + ["grep", "-n", "-I", "-i", "-F", "-e", needle, commit, "--"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=_git_environment(),
            )
        except OSError as exc:
            raise GitError("git operation unavailable") from exc
        deadline = time.monotonic() + PROCESS_TIMEOUT_SECONDS
        code = []
        prefix = commit + ":"
        buffer = b""
        consumed = 0
        stopped = False
        assert process.stdout is not None
        try:
            while len(code) < MAX_SEARCH_RESULTS:
                if (
                    time.monotonic() > deadline
                    or consumed > MAX_SEARCH_OUTPUT_BYTES
                ):
                    stopped = True
                    break
                chunk = process.stdout.read1(65536)
                if not chunk:
                    break
                consumed += len(chunk)
                buffer += chunk
                rows = buffer.split(b"\n")
                buffer = rows.pop()
                for raw in rows:
                    if len(code) >= MAX_SEARCH_RESULTS:
                        break
                    row = raw.decode("utf-8", "replace")
                    if row.startswith(prefix):
                        row = row[len(prefix) :]
                    match = re.match(r"^(.+?):([0-9]+):(.*)$", row)
                    if not match:
                        continue
                    code.append(
                        {
                            "path": match.group(1),
                            "line": int(match.group(2)),
                            "snippet": match.group(3).strip()[:200],
                        }
                    )
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
        if process.returncode not in (0, 1, -9) and not code:
            raise GitError("git operation failed")
        if stopped and not code:
            raise GitError("git operation timed out")
        return {
            "ok": True,
            "issues": [],
            "pulls": [],
            "code": code,
            "truncated": stopped or len(code) >= MAX_SEARCH_RESULTS,
        }

    def _all_blobs(self, commit: str) -> list[tuple[int, str]]:
        output = _run_git(
            self.git_dir,
            ["ls-tree", "-r", "-l", "-z", commit],
            max_output=64 * 1024 * 1024,
        )
        blobs = []
        for record in output.split(b"\0"):
            if not record or b"\t" not in record:
                continue
            metadata, path = record.split(b"\t", 1)
            fields = metadata.split()
            if len(fields) < 4 or fields[1] != b"blob":
                continue
            try:
                size = int(fields[3])
            except ValueError:
                continue
            blobs.append((max(0, size), path.decode("utf-8", "replace")))
        return blobs

    def stats(self, query: Mapping[str, str]) -> dict[str, Any]:
        commit = self.resolve_commit(query.get("ref", ""))
        blobs = self._all_blobs(commit)
        extensions: dict[str, dict[str, int]] = {}
        for size, path in blobs:
            name = path.rsplit("/", 1)[-1]
            if "." not in name or name.startswith("."):
                continue
            extension = name.rsplit(".", 1)[-1].lower()
            if not extension or len(extension) > 12:
                continue
            bucket = extensions.setdefault(extension, {"bytes": 0, "files": 0})
            bucket["bytes"] += size
            bucket["files"] += 1
        shortlog = _run_git(
            self.git_dir,
            ["shortlog", "-nse", commit],
            max_output=2 * 1024 * 1024,
        )
        contributor_avatars = self._contributor_avatars(commit)
        contributors = []
        for row in shortlog.decode("utf-8", "replace").splitlines():
            match = re.match(r"^\s*([0-9]+)\s+(.+?)(?:\s+<([^>]*)>)?\s*$", row)
            if match:
                name = match.group(2)
                contributor = {
                    "name": name,
                    "commits": int(match.group(1)),
                    "_identity": (
                        (match.group(3) or name).strip().casefold()
                    ),
                }
                avatar = contributor_avatars.get(name.strip().casefold(), "") \
                    or _github_noreply_avatar(match.group(3) or "")
                if avatar:
                    contributor["avatarUrl"] = avatar
                contributors.append(contributor)

        # Keep the response compact while still giving the Insights table a
        # useful, shared-scale view of recent work. Counts come from shortlog
        # across the complete selected history; only the tiny activity graph is
        # bounded to 52 weeks and 50k commits (the same safety ceiling as the
        # native Insights view).
        now = datetime.now(timezone.utc)
        this_week = datetime(
            now.year, now.month, now.day, tzinfo=timezone.utc
        ) - timedelta(days=now.weekday())
        activity_start = this_week - timedelta(weeks=51)
        activity_by_identity: dict[str, list[int]] = {}
        activity_log = _run_git(
            self.git_dir,
            [
                "log", "-n", "50000",
                f"--since=@{int(activity_start.timestamp())}",
                "--format=%aN%x1f%aE%x1f%ct", commit,
            ],
            max_output=MAX_JSON_OUTPUT,
        )
        for row in activity_log.decode("utf-8", "replace").splitlines():
            fields = row.split("\x1f", 2)
            if len(fields) != 3:
                continue
            identity = (fields[1] or fields[0]).strip().casefold()
            try:
                timestamp = int(fields[2])
            except ValueError:
                continue
            bucket = int(
                (timestamp - int(activity_start.timestamp())) // (7 * 86400)
            )
            if 0 <= bucket < 52:
                series = activity_by_identity.setdefault(identity, [0] * 52)
                series[bucket] += 1

        contributor_count = len(contributors)
        commit_count = sum(item["commits"] for item in contributors)
        # A pathological history can contain thousands of one-off identities.
        # Preserve the exact totals and explicitly mark the bounded rows rather
        # than risking an unbounded public JSON response.
        returned_contributors = contributors[:500]
        for contributor in returned_contributors:
            contributor["activityWeeks"] = activity_by_identity.get(
                contributor.pop("_identity"), [0] * 52
            )
        return {
            "ok": True,
            "commit": commit,
            "fileCount": len(blobs),
            "totalBytes": sum(size for size, _path in blobs),
            "extensions": extensions,
            "contributors": returned_contributors,
            "contributorCount": contributor_count,
            "contributorsTruncated": contributor_count > len(returned_contributors),
            "commitCount": commit_count,
            "activityStart": activity_start.isoformat(),
            "counts": self._root_collaboration_counts(commit),
        }

    def _contributor_avatars(self, commit: str) -> dict[str, str]:
        avatars: dict[str, str] = {}
        try:
            info_raw = _run_git(
                self.git_dir,
                ["show", commit + ":.forkmesh/info.json"],
                max_output=256 * 1024,
            )
            info = json.loads(info_raw.decode("utf-8"))
            for item in (
                info.get("contributors", [])
                if isinstance(info, dict)
                else []
            )[:500]:
                if not isinstance(item, dict):
                    continue
                name = str(
                    item.get("name") or item.get("login") or ""
                ).strip().casefold()
                avatar = _public_https_url(
                    item.get("avatarUrl") or item.get("avatar"))
                if name and avatar:
                    avatars[name] = avatar
        except (GitError, UnicodeDecodeError, json.JSONDecodeError):
            pass
        return avatars

    def _pulse_sizes(self, specs: list[str]) -> dict[str, tuple[str, str, int]]:
        if not specs:
            return {}
        output = _run_git(
            self.git_dir,
            ["cat-file", "--batch-check"],
            input_bytes="".join(spec + "\n" for spec in specs).encode("utf-8"),
            max_output=len(specs) * 256 + 1024,
        )
        found: dict[str, tuple[str, str, int]] = {}
        for spec, line in zip(
                specs, output.decode("utf-8", "replace").splitlines()):
            fields = line.split()
            if len(fields) == 3 and fields[2].isdigit():
                found[spec] = (fields[0].lower(), fields[1], int(fields[2]))
        return found

    def _pulse_read(self, items: list[tuple[str, int]]) -> dict[str, bytes]:
        found: dict[str, bytes] = {}
        batches: list[list[tuple[str, int]]] = [[]]
        total = 0
        for oid, size in items:
            if batches[-1] and total + size > PULSE_READ_BATCH_BYTES:
                batches.append([])
                total = 0
            batches[-1].append((oid, size))
            total += size
        for batch in batches:
            if not batch:
                continue
            output = _run_git(
                self.git_dir,
                ["cat-file", "--batch"],
                input_bytes="".join(
                    oid + "\n" for oid, _size in batch).encode("ascii"),
                max_output=sum(size for _oid, size in batch)
                + len(batch) * 160 + 1024,
            )
            cursor = 0
            for oid, _size in batch:
                line_end = output.find(b"\n", cursor)
                if line_end < 0:
                    break
                header = output[cursor:line_end].split()
                cursor = line_end + 1
                if len(header) != 3:
                    continue
                try:
                    size = int(header[2])
                except ValueError:
                    break
                if size < 0 or cursor + size > len(output):
                    break
                found[oid] = output[cursor:cursor + size]
                cursor += size + 1
        return found

    def _pulse_commit_index(
        self, main: str, rules: tuple[str, ...]
    ) -> dict[str, Any]:
        state = self._pulse_commits
        if state.get("commit") == main and state.get("rules") == rules:
            return state
        people: dict[str, tuple[str, str]] = {}
        times: dict[str, list[int]] = {}
        revision = main
        if state.get("commit") and state.get("rules") == rules:
            code, _output = _run_git_status(
                self.git_dir,
                ["merge-base", "--is-ancestor", state["commit"], main],
                max_output=256,
                allowed_codes=(0, 1),
            )
            if code == 0:
                revision = state["commit"] + ".." + main
                people = dict(state["people"])
                times = dict(state["times"])
        output = _run_git(
            self.git_dir,
            ["log", "--no-merges", "-n", str(PULSE_HISTORY_COMMITS),
             "--format=%aN%x1f%aE%x1f%ct", revision],
            max_output=64 * 1024 * 1024,
        )
        added: dict[str, list[int]] = {}
        for row in output.decode("utf-8", "replace").splitlines():
            fields = row.split("\x1f")
            if len(fields) != 3 or not fields[2].isdigit():
                continue
            name = " ".join(fields[0].split())[:100]
            email = fields[1].strip()[:200]
            if _pulse_is_automated(name, email, rules):
                continue
            key = _pulse_key(name or email)
            if not key:
                continue
            added.setdefault(key, []).append(int(fields[2]))
            people.setdefault(key, (name or email, email))
        for key, values in added.items():
            times[key] = sorted(times.get(key, []) + values)
        state = {
            "commit": main, "rules": rules, "people": people, "times": times}
        self._pulse_commits = state
        return state

    def _pulse_issue_index(self, main: str) -> tuple[str, list[dict[str, Any]]]:
        code, output = _run_git_status(
            self.git_dir,
            ["rev-parse", "--verify", "--quiet", main + ":.forkmesh/issues"],
            max_output=256,
            allowed_codes=(0, 1),
        )
        tree = output.decode("ascii", "replace").strip().lower() \
            if code == 0 else ""
        if tree == self._pulse_issues[0]:
            return self._pulse_issues
        records: list[dict[str, Any]] = []
        fresh: dict[str, dict[str, Any] | None] = {}
        if tree:
            listing = _run_git(
                self.git_dir,
                ["ls-tree", "-r", "-l", "-z", tree],
                max_output=64 * 1024 * 1024,
            )
            chosen: dict[int, tuple[int, str, int]] = {}
            for entry in listing.split(b"\0"):
                meta, _tab, path = entry.partition(b"\t")
                fields = meta.split()
                if len(fields) != 4 or fields[1] != b"blob" \
                        or not fields[3].isdigit():
                    continue
                match = PULSE_ISSUE_PATH_RE.fullmatch(
                    ".forkmesh/issues/" + path.decode("utf-8", "replace"))
                if not match or match.group(2) != match.group(3):
                    continue
                number = int(match.group(2))
                priority = 1 if match.group(1) else 0
                current = chosen.get(number)
                if current is None or priority > current[0]:
                    chosen[number] = (
                        priority, fields[2].decode("ascii").lower(),
                        int(fields[3]))
            numbers = sorted(chosen, reverse=True)[:PULSE_MAX_ISSUES]
            cache = self._pulse_issue_records
            blobs = self._pulse_read([
                (chosen[number][1], chosen[number][2]) for number in numbers
                if chosen[number][1] not in cache
                and chosen[number][2] <= MAX_JSON_BLOB
            ])
            for number in numbers:
                oid = chosen[number][1]
                if oid in cache:
                    record = cache[oid]
                elif oid in blobs:
                    record = _pulse_issue_record(number, blobs[oid])
                else:
                    continue
                fresh[oid] = record
                if record:
                    records.append(record)
        self._pulse_issue_records = fresh
        self._pulse_issues = (tree, records)
        return self._pulse_issues

    def _pulse_pull_index(
        self,
    ) -> tuple[tuple[str, str], list[dict[str, Any]]]:
        try:
            pulls_commit = self.resolve_commit("refs/heads/forkmesh/pulls")
        except GatewayError:
            pulls_commit = ""
        raw, refs = self._independent_pull_refs()
        key = (pulls_commit, raw)
        if key == self._pulse_pulls[0]:
            return self._pulse_pulls
        folders: dict[str, str] = {}
        if pulls_commit:
            listing = _run_git(
                self.git_dir,
                ["ls-tree", "-d", "-z", pulls_commit, "--",
                 ":(top,literal).forkmesh/pulls/"],
                max_output=16 * 1024 * 1024,
            )
            for entry in listing.split(b"\0"):
                meta, _tab, path = entry.partition(b"\t")
                fields = meta.split()
                match = re.fullmatch(
                    rb"\.forkmesh/pulls/([1-9][0-9]{0,8})", path)
                if len(fields) == 3 and fields[1] == b"tree" and match:
                    folders[match.group(1).decode("ascii")] = \
                        fields[2].decode("ascii").lower()
        live = self._live_independent_pull_refs(pulls_commit, refs)
        specs = {
            number: "%s:.forkmesh/pulls/%s" % (oid, number)
            for number, oid in live.items()
        }
        sizes = self._pulse_sizes(list(specs.values()))
        for number, spec in specs.items():
            found = sizes.get(spec)
            if found and found[1] == "tree":
                folders[number] = found[0]
        numbers = sorted(folders, key=int, reverse=True)[:PULSE_MAX_PULLS]
        cache = self._pulse_pull_records
        missing = sorted({
            folders[number] for number in numbers
            if folders[number] not in cache
        })
        folder_files: dict[str, dict[str, str]] = {}
        blobs: dict[str, bytes] = {}
        if missing:
            tree_sizes = self._pulse_sizes(missing)
            trees = self._pulse_read([
                (oid, tree_sizes[oid][2]) for oid in missing
                if oid in tree_sizes and tree_sizes[oid][1] == "tree"
            ])
            for oid in missing:
                folder_files[oid] = {
                    name: blob
                    for name, blob in _pulse_tree_entries(
                        trees.get(oid, b""), len(oid) // 2).items()
                    if name == "pull.md" or PULSE_EVENT_FILE_RE.fullmatch(name)
                }
            wanted = sorted({
                blob for files in folder_files.values()
                for blob in files.values()
            })
            blob_sizes = self._pulse_sizes(wanted)
            blobs = self._pulse_read([
                (blob, blob_sizes[blob][2]) for blob in wanted
                if blob in blob_sizes
                and blob_sizes[blob][2] <= PULSE_RECORD_BYTES
            ])
        records: list[dict[str, Any]] = []
        fresh: dict[str, dict[str, Any] | None] = {}
        for number in numbers:
            oid = folders[number]
            if oid in cache:
                record = cache[oid]
            else:
                record = _pulse_pull_record(int(number), {
                    name: blobs[blob]
                    for name, blob in folder_files.get(oid, {}).items()
                    if blob in blobs
                })
            fresh[oid] = record
            if record:
                records.append(record)
        self._pulse_pull_records = fresh
        self._pulse_pulls = (key, records)
        return self._pulse_pulls

    def _pulse_index(self, rules: tuple[str, ...] = ()) -> dict[str, Any]:
        main = self.resolve_commit("HEAD")
        commits = self._pulse_commit_index(main, rules)
        issues_key, issues = self._pulse_issue_index(main)
        pulls_key, pulls = self._pulse_pull_index()
        key = (main, issues_key, pulls_key, rules)
        if self._pulse_ledger.get("key") == key:
            return self._pulse_ledger
        names: dict[str, str] = {}

        def person(name: str) -> str:
            identity = _pulse_key(name)
            if not identity or _pulse_is_automated(name, "", rules):
                return ""
            names.setdefault(identity, " ".join(str(name).split()))
            return identity

        events: list[tuple[int, str, str, str, int, bool]] = []
        for pull in pulls:
            number = pull["number"]
            thread = "p%d" % number
            author = person(pull["author"])
            if author and pull["opened"]:
                events.append((
                    pull["opened"] // 1000, author, "pull", thread, number,
                    False))
            if author and pull["merged"]:
                events.append((
                    pull["merged"] // 1000, author, "merged", thread, number,
                    False))
            reviewers: set[str] = set()
            for at, kind, who in pull["events"]:
                identity = person(who)
                if not identity:
                    continue
                if kind == "review":
                    if identity == author or identity in reviewers:
                        continue
                    reviewers.add(identity)
                events.append((at // 1000, identity, kind, thread, number,
                               False))
        for issue in issues:
            number = issue["number"]
            thread = "i%d" % number
            author = person(issue["author"])
            if author and issue["opened"]:
                events.append((
                    issue["opened"] // 1000, author, "issue", thread, number,
                    False))
            closer = person(issue["closer"])
            if closer and issue["closed"]:
                events.append((
                    issue["closed"] // 1000, closer, "closed", thread, number,
                    bool(issue["bug"])))
            for at, _kind, who in issue["events"]:
                identity = person(who)
                if identity:
                    events.append((at // 1000, identity, "comment", thread,
                                   number, False))
        events.sort()
        for identity, (name, _email) in commits["people"].items():
            names.setdefault(identity, name)
        by_person: dict[str, list[tuple[int, str, str, str, int, bool]]] = {}
        first: dict[str, tuple[int, str, int]] = {}
        for identity, values in commits["times"].items():
            if values:
                first[identity] = (values[0], "commit", 0)
        for event in events:
            by_person.setdefault(event[1], []).append(event)
            known = first.get(event[1])
            if known is None or event[0] < known[0]:
                first[event[1]] = (event[0], event[2], event[4])
        avatars = self._contributor_avatars(main)
        for identity, (_name, email) in commits["people"].items():
            if identity not in avatars:
                avatar = _github_noreply_avatar(email)
                if avatar:
                    avatars[identity] = avatar
        ledger = {
            "key": key,
            "rules": rules,
            "main": main,
            "names": names,
            "avatars": avatars,
            "commits": commits["times"],
            "events": events,
            "byPerson": by_person,
            "first": first,
            "pulls": pulls,
            "issues": issues,
        }
        self._pulse_ledger = ledger
        return ledger

    def _pulse_numstat(
        self, commits: list[str]
    ) -> dict[str, tuple[int, int, tuple[int, ...], bool] | None]:
        with self._pulse_numstat_lock:
            memo = self._pulse_numstat_memo
            missing = [commit for commit in commits if commit not in memo]
            deadline = time.monotonic() + PULSE_NUMSTAT_SECONDS
            for offset in range(0, len(missing), PULSE_NUMSTAT_BATCH):
                if time.monotonic() > deadline:
                    break
                chunk = missing[offset:offset + PULSE_NUMSTAT_BATCH]
                try:
                    output = _run_git(
                        self.git_dir,
                        ["-c", "core.bigFileThreshold=" + PULSE_DIFF_FILE_LIMIT,
                         "log", "--no-walk=unsorted", "--stdin", "--numstat",
                         "--no-renames", "--format=%x1e%H"],
                        input_bytes=("\n".join(chunk) + "\n").encode("ascii"),
                        max_output=32 * 1024 * 1024,
                    )
                except GitError:
                    output = b""
                for block in output.split(b"\x1e")[1:]:
                    lines = block.decode("utf-8", "replace").split("\n")
                    commit = lines[0].strip().lower()
                    additions = deletions = changed = 0
                    paths: list[int] = []
                    for line in lines[1:]:
                        parts = line.split("\t", 2)
                        if len(parts) != 3:
                            continue
                        additions += int(parts[0]) if parts[0].isdigit() else 0
                        deletions += int(parts[1]) if parts[1].isdigit() else 0
                        changed += 1
                        if len(paths) < PULSE_NUMSTAT_PATHS:
                            paths.append(hash(parts[2]))
                    memo[commit] = (
                        additions, deletions, tuple(paths),
                        changed > PULSE_NUMSTAT_PATHS)
                for commit in chunk:
                    if commit not in memo:
                        memo[commit] = None
            found = {}
            for commit in commits:
                if commit in memo:
                    memo.move_to_end(commit)
                    found[commit] = memo[commit]
            while len(memo) > PULSE_NUMSTAT_MEMO:
                memo.popitem(last=False)
            return found

    def _pulse_commit_window(
        self, ledger: Mapping[str, Any], start: int, end: int, step: int
    ) -> dict[str, Any]:
        window = ["--since=@%d" % start, "--until=@%d" % max(start, end - 1)]
        branch_rows = _run_git(
            self.git_dir,
            ["log", "--no-merges", "-n", str(PULSE_WINDOW_COMMITS), *window,
             "--format=%H%x1f%aN%x1f%aE", "--exclude=forkmesh/*",
             "--branches"],
            max_output=MAX_JSON_OUTPUT,
        ).decode("utf-8", "replace").splitlines()
        authors: set[str] = set()
        every: set[str] = set()
        rules = ledger["rules"]
        for row in branch_rows:
            fields = row.split("\x1f")
            if len(fields) == 3 and not _pulse_is_automated(
                    fields[1], fields[2], rules):
                every.add(fields[0])
                authors.add(_pulse_key(fields[1] or fields[2]))
        main_rows = []
        for row in _run_git(
            self.git_dir,
            ["log", "--no-merges", "-n", str(PULSE_WINDOW_COMMITS), *window,
             "--format=%H%x1f%aN%x1f%aE%x1f%ct", ledger["main"]],
            max_output=MAX_JSON_OUTPUT,
        ).decode("utf-8", "replace").splitlines():
            fields = row.split("\x1f")
            if len(fields) == 4 and fields[3].isdigit() and not (
                    _pulse_is_automated(fields[1], fields[2], rules)):
                main_rows.append((
                    fields[0].lower(), " ".join(fields[1].split())[:100],
                    fields[2].strip(), int(fields[3])))
                authors.add(_pulse_key(fields[1] or fields[2]))
                every.add(fields[0])
        stats = self._pulse_numstat(
            [row[0] for row in main_rows[:PULSE_NUMSTAT_COMMITS]])
        truncated = (
            len(main_rows) > PULSE_NUMSTAT_COMMITS
            or len(main_rows) >= PULSE_WINDOW_COMMITS
            or len(branch_rows) >= PULSE_WINDOW_COMMITS
        )
        measured = 0
        buckets = [
            {"commits": 0, "additions": 0, "deletions": 0}
            for _ in range(max(1, -(-(end - start) // step)))
        ]
        additions = deletions = 0
        files: set[int] = set()
        committers: Counter[str] = Counter()
        committer_names: dict[str, tuple[str, str]] = {}
        for commit, name, email, at in main_rows:
            key = _pulse_key(name or email)
            committers[key] += 1
            committer_names.setdefault(key, (name or email, email))
            bucket = buckets[min(len(buckets) - 1, max(0, (at - start) // step))]
            bucket["commits"] += 1
            if commit not in stats:
                continue
            stat = stats[commit]
            if stat is None:
                truncated = True
                continue
            measured += 1
            additions += stat[0]
            deletions += stat[1]
            bucket["additions"] += stat[0]
            bucket["deletions"] += stat[1]
            files.update(stat[2])
            truncated = truncated or stat[3]
        top = sorted(
            committers.items(),
            key=lambda item: (-item[1], committer_names[item[0]][0].casefold()),
        )[:PULSE_COMMITTERS_LIMIT]
        committer_rows = []
        for key, count in top:
            name, email = committer_names[key]
            row: dict[str, Any] = {"name": name, "commits": count}
            avatar = ledger["avatars"].get(key) or _github_noreply_avatar(email)
            if avatar:
                row["avatarUrl"] = avatar
            committer_rows.append(row)
        return {
            "summary": {
                "authors": len(authors - {""}),
                "main": len(main_rows),
                "all": len(every),
                "filesChanged": len(files),
                "additions": additions,
                "deletions": deletions,
                "measuredCommits": measured,
                "partial": any(
                    row[0] not in stats
                    for row in main_rows[:PULSE_NUMSTAT_COMMITS]),
                "truncated": truncated,
            },
            "committers": committer_rows,
            "committerCount": len(committers),
            "series": {
                "start": _pulse_iso(start),
                "stepSeconds": step,
                "buckets": buckets,
            },
        }

    def pulse(
        self, query: Mapping[str, str], now_seconds: int,
        rules: tuple[str, ...] = (),
    ) -> dict[str, Any]:
        period = pulse_period(query)
        person = pulse_person(query)
        spec = PULSE_PERIODS[period]
        now_seconds = max(0, int(now_seconds))
        end = pulse_window_end(period, now_seconds)
        start = max(0, end - spec["seconds"]) if spec["seconds"] else 0
        with self._pulse_lock:
            ledger = self._pulse_index(tuple(rules))
        today = now_seconds // 86400
        response: dict[str, Any] = {
            "ok": True,
            "commit": ledger["main"],
            "branch": self.default_branch_ref()[len("refs/heads/"):],
            "period": period,
            "since": _pulse_iso(start) if spec["seconds"] else None,
            "until": _pulse_iso(min(end, now_seconds)),
            "generatedAt": _pulse_iso(now_seconds),
            "refreshSeconds": spec["bucket"],
            "scoring": {
                "weights": dict(PULSE_SCORE_WEIGHTS),
                "commitsPerDay": PULSE_COMMITS_PER_DAY,
                "commentsPerThread": PULSE_COMMENTS_PER_THREAD,
                "excludesAutomated": True,
            },
        }
        if spec["seconds"]:
            previous_bounds = (max(0, start - spec["seconds"]), start)
        else:
            previous_bounds = (0, max(0, end - PULSE_ALL_TIME_COMPARE_SECONDS))
        scores = _pulse_scores(ledger, start, end)
        ranked = _pulse_ranking(scores, ledger["names"])
        if person:
            key = _pulse_key(person)
            mine = next((row for row in ranked if row[1] == key), None)
            before_end = start if spec["seconds"] else previous_bounds[1]
            response["person"] = {
                **_pulse_person_entry(ledger, key),
                "found": key in ledger["names"],
                "rank": mine[0] if mine else None,
                "score": mine[2] if mine else 0,
                "breakdown": mine[3] if mine else _pulse_empty_parts(),
                "totals": _pulse_totals(ledger, key, end, today),
                "before": _pulse_totals(
                    ledger, key, before_end, max(0, before_end - 1) // 86400),
            }
            return response
        previous = {
            row[1]: row[0] for row in _pulse_ranking(
                _pulse_scores(ledger, *previous_bounds), ledger["names"])
        }
        first = ledger["first"]
        leaderboard = []
        for rank, key, score, parts in ranked[:PULSE_LEADERBOARD_LIMIT]:
            totals = _pulse_totals(ledger, key, end, today)
            leaderboard.append({
                **_pulse_person_entry(ledger, key),
                "rank": rank,
                "previousRank": previous.get(key),
                "score": score,
                "breakdown": parts,
                "streak": totals["currentStreak"],
                "achievements": {
                    name: totals[name] for name in (
                        "mergedPulls", "reviews", "bugsClosed",
                        "longestStreak")
                },
                "newcomer": bool(spec["seconds"]) and key in first
                and start <= first[key][0] < end,
            })
        response["leaderboard"] = leaderboard
        response["leaderboardTotal"] = len(ranked)
        if not spec["seconds"]:
            return response
        response.update(self._pulse_commit_window(
            ledger, start, end, spec["step"]))
        newcomers = sorted(
            (
                (at, key, kind, number)
                for key, (at, kind, number) in first.items()
                if start <= at < end
            ),
            reverse=True,
        )
        response["newcomers"] = [
            {
                **_pulse_person_entry(ledger, key),
                "at": at * 1000,
                "kind": kind,
                "number": number or None,
            }
            for at, key, kind, number in newcomers[:PULSE_NEWCOMERS_LIMIT]
        ]
        response["newcomerCount"] = len(newcomers)
        lower, upper = start * 1000, end * 1000

        def listing(records: list[dict[str, Any]], field: str) -> dict[str, Any]:
            chosen = sorted(
                (record for record in records
                 if lower <= record[field] < upper),
                key=lambda record: (-record[field], -record["number"]),
            )
            return {
                "count": len(chosen),
                "authors": len({
                    _pulse_key(record["author"]) for record in chosen
                    if not _pulse_is_automated(
                        record["author"], "", ledger["rules"])
                }),
                "items": [
                    {
                        "number": record["number"],
                        "title": record["title"],
                        "author": record["author"],
                        "status": record["status"],
                        "at": record[field],
                    }
                    for record in chosen[:PULSE_LIST_LIMIT]
                ],
                "truncated": len(chosen) > PULSE_LIST_LIMIT,
            }

        response["pulls"] = {
            "merged": listing(ledger["pulls"], "merged"),
            "opened": listing(ledger["pulls"], "opened"),
        }
        response["issues"] = {
            "closed": listing(ledger["issues"], "closed"),
            "opened": listing(ledger["issues"], "opened"),
        }
        return response

    def sizes(self, query: Mapping[str, str]) -> dict[str, Any]:
        commit = self.resolve_commit(query.get("ref", ""))
        blobs = self._all_blobs(commit)
        max_depth = 8
        max_children = 40
        root: dict[str, Any] = {
            "name": "",
            "size": 0,
            "type": "directory",
            "_children": {},
        }
        for size, path in blobs:
            root["size"] += size
            current = root
            parts = path.split("/")
            directories = parts[:-1]
            for part in directories[:max_depth]:
                children = current["_children"]
                current = children.setdefault(
                    "directory:" + part,
                    {
                        "name": part,
                        "size": 0,
                        "type": "directory",
                        "_children": {},
                    },
                )
                current["size"] += size
            display_name = parts[-1]
            if len(directories) > max_depth:
                display_name = "…/" + display_name
            current["_children"]["file:" + path] = {
                "name": display_name,
                "path": path,
                "size": size,
                "type": "file",
            }

        def serialize(node: dict[str, Any]) -> dict[str, Any]:
            result = {
                "name": node["name"],
                "size": node["size"],
                "type": node["type"],
            }
            if node["type"] == "file":
                result["path"] = node["path"]
                return result
            children = sorted(
                node["_children"].values(),
                key=lambda item: (-item["size"], item["name"]),
            )
            shown = children[:max_children]
            output = [serialize(item) for item in shown]
            if len(children) > max_children:
                output.append(
                    {
                        "name": "…",
                        "size": sum(
                            item["size"] for item in children[max_children:]
                        ),
                        "type": "summary",
                    }
                )
            if output:
                result["children"] = output
            return result

        serialized = serialize(root)
        serialized["ok"] = True
        serialized["commit"] = commit
        serialized["fileCount"] = len(blobs)
        return serialized

    def release_spec(
        self, query: Mapping[str, str], max_release_bytes: int
    ) -> "StreamSpec":
        digest = str(query.get("sha256") or query.get("path") or "").lower()
        if not SHA256_RE.fullmatch(digest) or self.config.release_store is None:
            raise GatewayError("release blob is unavailable")
        root = self.config.release_store.resolve()
        path = (root / "sha256" / digest[:2] / digest / "data").resolve()
        if root not in path.parents or not path.is_file():
            raise GatewayError("release blob is unavailable")
        size = path.stat().st_size
        if size > max_release_bytes:
            raise GatewayError("release blob exceeds configured size limit")
        if not _release_digest_matches(path, digest):
            raise GatewayError("release blob integrity check failed")
        return StreamSpec(
            kind="file",
            content_type="application/octet-stream",
            content_length=size,
            path=path,
            filename=digest,
            disposition="attachment",
        )

    def git_advertisement(self, git_protocol: str = "") -> bytes:
        advertisement = _run_git(
            self.git_dir,
            [
                "-c",
                "uploadpack.allowTipSHA1InWant=false",
                "-c",
                "uploadpack.allowReachableSHA1InWant=false",
                "-c",
                "uploadpack.allowAnySHA1InWant=false",
                "-c",
                "uploadpack.allowFilter=true",
                "upload-pack",
                "--stateless-rpc",
                "--advertise-refs",
                str(self.git_dir),
            ],
            max_output=16 * 1024 * 1024,
            git_protocol=git_protocol,
        )
        if git_protocol == "version=2":
            return advertisement
        service = b"# service=git-upload-pack\n"
        return ("%04x" % (len(service) + 4)).encode("ascii") + service + b"0000" + advertisement

    def upload_pack_spec(
        self, body: bytes, git_protocol: str = ""
    ) -> "StreamSpec":
        wants = _upload_pack_wants(body)
        if wants:
            advertised = _run_git(
                self.git_dir,
                [
                    "for-each-ref",
                    "--format=%(objectname)%00%(*objectname)%00%(refname)",
                    "refs/",
                ],
                max_output=16 * 1024 * 1024,
            )
            public_oids: set[str] = set()
            public_refs: list[str] = []
            for row in advertised.decode("ascii", "strict").splitlines():
                fields = row.split("\x00", 2)
                if len(fields) != 3 or fields[2].startswith(
                    INTERNAL_GIT_REF_NAMESPACE
                ):
                    continue
                public_refs.append(fields[2])
                for oid in fields[:2]:
                    oid = oid.strip().lower()
                    if GIT_OBJECT_ID_RE.fullmatch(oid):
                        public_oids.add(oid)
            if not wants.issubset(public_oids):
                generation = hashlib.sha256(advertised).hexdigest()
                with self._upload_objects_lock:
                    if generation != self._upload_objects_key:
                        reachable = _run_git(
                            self.git_dir,
                            [
                                "rev-list",
                                "--objects",
                                "--no-object-names",
                                "--stdin",
                            ],
                            input_bytes=("\n".join(public_refs) + "\n").encode(
                                "ascii"
                            ),
                            max_output=16 * 1024 * 1024,
                        )
                        self._upload_objects = frozenset(
                            oid
                            for oid in reachable.decode(
                                "ascii", "strict"
                            ).splitlines()
                            if GIT_OBJECT_ID_RE.fullmatch(oid)
                        )
                        self._upload_objects_key = generation
                    public_oids.update(self._upload_objects)
            if not wants.issubset(public_oids):
                raise GatewayError(
                    "git upload-pack requested an unadvertised object"
                )
        return StreamSpec(
            kind="process",
            content_type="application/x-git-upload-pack-result",
            content_length=None,
            command=tuple(
                _git_prefix(self.git_dir)
                + [
                    "-c",
                    "uploadpack.allowTipSHA1InWant=false",
                    "-c",
                    "uploadpack.allowReachableSHA1InWant=false",
                    "-c",
                    "uploadpack.allowAnySHA1InWant=false",
                    "-c",
                    "uploadpack.allowFilter=true",
                    "upload-pack",
                    "--stateless-rpc",
                    str(self.git_dir),
                ]
            ),
            input_bytes=body,
            git_protocol=git_protocol,
        )

    @staticmethod
    def _receive_pack_configuration(
        max_input_bytes: int, hooks_path: str = os.devnull
    ) -> list[str]:
        """Fixed fail-closed receive-pack policy.

        Deletes and non-fast-forward updates are always refused, and hooks in
        the repository never run: the only hooks directory is either the null
        device or the gateway-owned operator-policy directory holding a single
        pre-receive. Objects are integrity-checked and kept packed so a small
        compressed input cannot expand into many large loose files.
        """
        return [
            "-c",
            "receive.denyDeletes=true",
            "-c",
            "receive.denyNonFastForwards=true",
            "-c",
            "core.hooksPath=" + hooks_path,
            "-c",
            "receive.fsckObjects=true",
            "-c",
            "receive.hideRefs=" + INTERNAL_GIT_REF_NAMESPACE,
            "-c",
            "receive.maxInputSize=" + str(int(max_input_bytes)),
            "-c",
            "receive.unpackLimit=0",
        ]

    def receive_pack_advertisement(self) -> bytes:
        advertisement = _run_git(
            self.git_dir,
            self._receive_pack_configuration(MAX_REQUEST_BODY)
            + [
                "receive-pack",
                "--stateless-rpc",
                "--advertise-refs",
                str(self.git_dir),
            ],
            max_output=16 * 1024 * 1024,
        )
        service = b"# service=git-receive-pack\n"
        return (
            ("%04x" % (len(service) + 4)).encode("ascii")
            + service
            + b"0000"
            + advertisement
        )

    def receive_pack_result(
        self, body: bytes, max_input_bytes: int
    ) -> tuple[bytes, bool]:
        """Run one bounded stateless receive-pack; report whether refs moved.

        Unlike upload-pack, the report-status response is tiny, so the push
        runs synchronously under a fixed deadline. That also lets the caller
        observe the accepted ref transition before answering (the sidecar
        write below depends on it).
        """
        output, changes = self.receive_pack_transitions(body, max_input_bytes)
        return output, bool(changes)

    def receive_pack_transitions(
        self, body: bytes, max_input_bytes: int
    ) -> tuple[bytes, list[tuple[str, str, str]]]:
        if len(body) > max_input_bytes:
            raise GatewayError("push request is too large")
        lock = maintenance_push_lock(self.git_dir)
        try:
            before = stable_refs(self.git_dir)
            output = _run_git(
                self.git_dir,
                self._receive_pack_configuration(
                    max_input_bytes, self.receive_hooks_path)
                + ["receive-pack", "--stateless-rpc", str(self.git_dir)],
                input_bytes=body,
                max_output=MAX_JSON_OUTPUT,
                timeout=receive_pack_timeout_seconds(len(body)),
            )
            changes = ref_transitions(before, stable_refs(self.git_dir))
        finally:
            if lock is not None:
                os.close(lock)
        if changes:
            mark_maintenance_due(self.git_dir)
        return output, changes

    def new_commit_count(self, old: str, new: str) -> int:
        if set(new) == {"0"} or not GIT_OID_RE.fullmatch(new):
            return 0
        revisions = [new] if set(old) == {"0"} else [new, "^" + old]
        try:
            output = _run_git(
                self.git_dir,
                ["rev-list", "--count",
                 "--max-count=%d" % REF_UPDATE_COMMIT_COUNT_LIMIT,
                 *revisions, "--"],
                max_output=64,
            )
            return int(output.decode("ascii").strip() or 0)
        except (GitError, OSError, ValueError):
            return 0


def maintenance_push_lock(git_dir: Path | None) -> int | None:
    if fcntl is None or git_dir is None:
        return None
    flags = os.O_RDWR | getattr(os, "O_CLOEXEC", 0) | getattr(
        os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(Path(git_dir) / MAINTENANCE_LOCK_NAME, flags)
    except OSError:
        return None
    deadline = time.monotonic() + MAINTENANCE_LOCK_WAIT_SECONDS
    while True:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
            return descriptor
        except BlockingIOError:
            if time.monotonic() >= deadline:
                return descriptor
            time.sleep(0.2)
        except OSError:
            return descriptor


def mark_maintenance_due(git_dir: Path | None) -> None:
    if git_dir is None or not (Path(git_dir) / MAINTENANCE_LOCK_NAME).is_file():
        return
    flags = os.O_WRONLY | os.O_CREAT | getattr(os, "O_CLOEXEC", 0) | getattr(
        os, "O_NOFOLLOW", 0)
    try:
        os.close(os.open(Path(git_dir) / MAINTENANCE_MARKER_NAME, flags, 0o600))
    except OSError:
        pass


def receive_pack_timeout_seconds(body_bytes: int) -> int:
    """Fixed floor plus a per-byte allowance for indexing and fsck."""
    return RECEIVE_PACK_TIMEOUT_SECONDS + max(0, int(body_bytes)) // (
        RECEIVE_PACK_BYTES_PER_SECOND
    )


@dataclass
class StreamSpec:
    kind: str
    content_type: str
    content_length: int | None
    path: Path | None = None
    command: tuple[str, ...] = ()
    input_bytes: bytes = b""
    git_protocol: str = ""
    filename: str = ""
    disposition: str = "inline"


@dataclass
class GatewayResponse:
    status: int
    content_type: str
    body: bytes = b""
    headers: dict[str, str] = field(default_factory=dict)
    stream: StreamSpec | None = None


@dataclass
class _MemoryCacheEntry:
    value: Any
    size: int
    inserted_at: float


class BoundedMemoryCache:
    """Thread-safe LRU with operational counters and explicit invalidation.

    There is intentionally no time-to-live. Repository response keys include
    the validated application/repository generation, while session entries are
    rechecked against their signed expiry. Time-based expiry would cause hot
    mirrors to fall back to disk even when no authoritative state changed.
    """

    def __init__(self, name: str, *, max_entries: int, max_bytes: int,
                 entry_overhead: int = 0) -> None:
        self.name = name
        self.max_entries = max_entries
        self.max_bytes = max_bytes
        self.entry_overhead = max(0, int(entry_overhead))
        self._lock = threading.Lock()
        self._entries: OrderedDict[Any, _MemoryCacheEntry] = OrderedDict()
        self._bytes = 0
        self._hits = 0
        self._misses = 0
        self._evictions = 0
        self._invalidations = 0
        self._warmups = 0
        self._backend_reads = 0

    def get(self, key: Any) -> tuple[bool, Any]:
        with self._lock:
            entry = self._entries.get(key)
            if entry is None:
                self._misses += 1
                return False, None
            self._entries.move_to_end(key)
            self._hits += 1
            return True, entry.value

    def backend_read(self) -> None:
        with self._lock:
            self._backend_reads += 1

    def put(self, key: Any, value: Any, *, size: int) -> bool:
        size = max(0, int(size)) + self.entry_overhead
        if size > self.max_bytes:
            return False
        with self._lock:
            prior = self._entries.pop(key, None)
            if prior is not None:
                self._bytes -= prior.size
            while self._entries and (
                len(self._entries) >= self.max_entries
                or self._bytes + size > self.max_bytes
            ):
                _old_key, old = self._entries.popitem(last=False)
                self._bytes -= old.size
                self._evictions += 1
            if (
                len(self._entries) >= self.max_entries
                or self._bytes + size > self.max_bytes
            ):
                return False
            self._entries[key] = _MemoryCacheEntry(
                value=value, size=size, inserted_at=time.monotonic())
            self._bytes += size
            self._warmups += 1
            return True

    def invalidate_all(self) -> int:
        with self._lock:
            removed = len(self._entries)
            self._entries.clear()
            self._bytes = 0
            self._invalidations += removed
            return removed

    def invalidate(self, key: Any) -> bool:
        with self._lock:
            entry = self._entries.pop(key, None)
            if entry is None:
                return False
            self._bytes -= entry.size
            self._invalidations += 1
            return True

    def diagnostics(self) -> dict[str, Any]:
        with self._lock:
            now = time.monotonic()
            oldest = next(iter(self._entries.values()), None)
            return {
                "entries": len(self._entries),
                "bytes": self._bytes,
                "maxEntries": self.max_entries,
                "maxBytes": self.max_bytes,
                "hits": self._hits,
                "misses": self._misses,
                "evictions": self._evictions,
                "invalidations": self._invalidations,
                "warmups": self._warmups,
                "backendReads": self._backend_reads,
                "diskReadsAvoided": self._hits,
                "oldestAgeSeconds": (
                    max(0, int(now - oldest.inserted_at))
                    if oldest is not None else 0
                ),
            }


def _copy_cached_response(response: GatewayResponse) -> GatewayResponse:
    return GatewayResponse(
        response.status,
        response.content_type,
        response.body,
        dict(response.headers),
    )


def _repository_serves(repository: Any, operation: str) -> bool:
    operations = repository.config.operations
    if operation == BRANCH_TRASH_OPERATION:
        return BRANCH_TRASH_CARRIER_OPERATION in operations
    if operation == PULSE_OPERATION:
        return PULSE_CARRIER_OPERATION in operations
    return operation in operations


def json_response(value: Any, status: int = 200) -> GatewayResponse:
    body = (
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
        + b"\n"
    )
    return GatewayResponse(status, "application/json; charset=utf-8", body)


GIT_ERROR_HELP_URL = "https://forkmesh.com/docs/git-errors/"
GIT_ERROR_TEXT = {
    "not_found": "This ForkMesh mirror has no public repository at this "
                 "address. Check the owner and repository name.",
    "mirror_unavailable": "This ForkMesh mirror could not read the "
                          "repository just now. Try again in a minute.",
}


def dispatch_error_response(code: str, status: int, target: str) -> GatewayResponse:
    try:
        route = _public_git_route(urlsplit(target).path)
    except ValueError:
        route = None
    if route is None or route[2] != "git-info-refs":
        return json_response({"ok": False, "error": code}, status)
    text = "%s\nHelp: %s\n" % (GIT_ERROR_TEXT[code], GIT_ERROR_HELP_URL)
    return GatewayResponse(status, "text/plain; charset=utf-8", text.encode("utf-8"))


def _actions_single_line(value: Any, minimum: int, maximum: int) -> bool:
    return (
        isinstance(value, str)
        and minimum <= len(value) <= maximum
        and not any(character in value for character in ("\x00", "\r", "\n"))
    )


def _actions_bounded_integer(value: Any, minimum: int, maximum: int) -> bool:
    # bool is an int subclass; a JSON true must never pass for 1.
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and minimum <= value <= maximum
    )


def _actions_summary_identity(item: Mapping[str, Any]) -> tuple[str, str]:
    """Owner/repository of one v2 entry, normalized the way runs are."""

    owner_value = item.get("owner")
    repository_value = item.get("repository")
    if not isinstance(owner_value, str) or not isinstance(
            repository_value, str):
        raise GatewayError("Actions summary is unavailable")
    entry_owner = owner_value.strip().lower()
    entry_repository = repository_value.strip()
    if not NODE_RE.fullmatch(entry_owner) or not REPO_RE.fullmatch(
            entry_repository):
        raise GatewayError("Actions summary is unavailable")
    return entry_owner, entry_repository


def _actions_summary_run_extension(item: Mapping[str, Any]) -> dict[str, Any]:
    """Validate the v2-only keys of one run."""

    workflow_path = item.get("workflowPath")
    pull_number = item.get("pullNumber")
    steps = item.get("steps")
    if (
        not isinstance(workflow_path, str)
        or (
            workflow_path
            and not ACTIONS_WORKFLOW_PATH_RE.fullmatch(workflow_path)
        )
        or not _actions_bounded_integer(pull_number, 0, MAX_SAFE_JSON_INTEGER)
        or not isinstance(steps, list)
        or len(steps) > MAX_ACTIONS_RUN_STEPS
    ):
        raise GatewayError("Actions summary is unavailable")
    output_steps = []
    for step in steps:
        if (
            not isinstance(step, dict)
            or set(step) != {"name", "status"}
            or not _actions_single_line(
                step.get("name"), 0, MAX_ACTIONS_STEP_NAME_CHARS)
            or not isinstance(step.get("status"), str)
            or step.get("status") not in ACTIONS_STEP_STATUSES
        ):
            raise GatewayError("Actions summary is unavailable")
        output_steps.append(
            {"name": step["name"], "status": step["status"]})
    return {
        "workflowPath": workflow_path,
        "pullNumber": pull_number,
        "steps": output_steps,
    }


def _actions_summary_catalog(
    repositories: Any, *, owner: str, repository: str
) -> dict[str, Any]:
    """Validate every v2 repository entry; return the requested one's."""

    if (
        not isinstance(repositories, list)
        or len(repositories) > MAX_ACTIONS_SUMMARY_REPOSITORIES
    ):
        raise GatewayError("Actions summary is unavailable")
    # Absent entry: the executor does not run Actions for this repository.
    selected: dict[str, Any] = {"actionsEnabled": False, "workflows": []}
    seen_repositories: set[tuple[str, str]] = set()
    for item in repositories:
        if not isinstance(item, dict) or set(item) != {
            "owner", "repository", "actionsEnabled", "workflows",
        }:
            raise GatewayError("Actions summary is unavailable")
        entry_owner, entry_repository = _actions_summary_identity(item)
        identity = (entry_owner, entry_repository.lower())
        workflows = item.get("workflows")
        if (
            identity in seen_repositories
            or not isinstance(item.get("actionsEnabled"), bool)
            or not isinstance(workflows, list)
            or len(workflows) > MAX_ACTIONS_REPOSITORY_WORKFLOWS
        ):
            raise GatewayError("Actions summary is unavailable")
        seen_repositories.add(identity)
        output_workflows = []
        seen_paths: set[str] = set()
        for workflow in workflows:
            if not isinstance(workflow, dict) or set(workflow) != {
                "path", "name", "events", "manual", "enabled", "runsOn",
                "valid",
            }:
                raise GatewayError("Actions summary is unavailable")
            workflow_path = workflow.get("path")
            events = workflow.get("events")
            runs_on = workflow.get("runsOn")
            if (
                not isinstance(workflow_path, str)
                or not ACTIONS_WORKFLOW_PATH_RE.fullmatch(workflow_path)
                or workflow_path in seen_paths
                or not _actions_single_line(
                    workflow.get("name"), 0, MAX_ACTIONS_WORKFLOW_NAME_CHARS)
                or not isinstance(events, list)
                or len(events) > MAX_ACTIONS_WORKFLOW_EVENTS
                or any(
                    not _actions_single_line(
                        event, 1, MAX_ACTIONS_WORKFLOW_EVENT_CHARS)
                    for event in events
                )
                or not isinstance(runs_on, list)
                or len(runs_on) > MAX_ACTIONS_WORKFLOW_RUNNER_LABELS
                or any(
                    not _actions_single_line(
                        label, 1, MAX_ACTIONS_WORKFLOW_RUNNER_LABEL_CHARS)
                    for label in runs_on
                )
                or any(
                    not isinstance(workflow.get(flag), bool)
                    for flag in ("manual", "enabled", "valid")
                )
            ):
                raise GatewayError("Actions summary is unavailable")
            seen_paths.add(workflow_path)
            output_workflows.append({
                "path": workflow_path,
                "name": workflow["name"],
                "events": list(events),
                "manual": workflow["manual"],
                "enabled": workflow["enabled"],
                "runsOn": list(runs_on),
                "valid": workflow["valid"],
            })
        if identity == (owner.lower(), repository.lower()):
            selected = {
                "actionsEnabled": item["actionsEnabled"],
                "workflows": output_workflows,
            }
    return selected


def _actions_summary_controls(
    controls: Any, *, owner: str, repository: str, now_ms: int
) -> list[dict[str, Any]]:
    """Validate every v2 control outcome; return the requested repository's.

    A request id may legitimately repeat: the executor records a replayed
    envelope as ``rejected`` beside the original ``applied`` outcome.
    """

    if (
        not isinstance(controls, list)
        or len(controls) > MAX_ACTIONS_SUMMARY_CONTROLS
    ):
        raise GatewayError("Actions summary is unavailable")
    output = []
    for item in controls:
        if not isinstance(item, dict) or set(item) != {
            "owner", "repository", "requestId", "action", "status",
            "reason", "runId", "finishedAt",
        }:
            raise GatewayError("Actions summary is unavailable")
        entry_owner, entry_repository = _actions_summary_identity(item)
        request_id = item.get("requestId")
        action = item.get("action")
        status_value = item.get("status")
        reason = item.get("reason")
        if (
            not isinstance(request_id, str)
            or not ACTIONS_CONTROL_REQUEST_ID_RE.fullmatch(request_id)
            or not isinstance(action, str)
            or action not in ACTIONS_CONTROL_ACTIONS
            or not isinstance(status_value, str)
            or status_value not in ACTIONS_CONTROL_RESULT_STATUSES
            or not isinstance(reason, str)
            or (status_value == "applied" and reason != "")
            or (
                status_value == "rejected"
                and reason not in ACTIONS_CONTROL_REJECTION_REASONS
            )
            or not _actions_bounded_integer(
                item.get("runId"), 0, MAX_SAFE_JSON_INTEGER)
            or not _actions_bounded_integer(
                item.get("finishedAt"),
                0,
                now_ms + MAX_ACTIONS_SUMMARY_CLOCK_SKEW_MS,
            )
        ):
            raise GatewayError("Actions summary is unavailable")
        if (
            entry_owner != owner.lower()
            or entry_repository.lower() != repository.lower()
        ):
            continue
        output.append({
            "requestId": request_id,
            "action": action,
            "status": status_value,
            "reason": reason,
            "runId": item["runId"],
            "finishedAt": item["finishedAt"],
        })
    return output


def _load_actions_summary(
    path: Path,
    *,
    node: str,
    owner: str,
    repository: str,
    now_ms: int,
) -> dict[str, Any]:
    """Read one short-lived, protected, already-redacted Actions summary."""

    parent_descriptor = -1
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    try:
        if (
            not path.is_absolute()
            or path != Path(os.path.normpath(str(path)))
            or path.name != "actions-summary.json"
            or path.parent.resolve(strict=True) != path.parent
        ):
            raise GatewayError("Actions summary is unavailable")
        parent_flags = (
            os.O_RDONLY
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        parent_descriptor = os.open(path.parent, parent_flags)
        parent_info = os.fstat(parent_descriptor)
        if not _actions_parent_metadata_allowed(
            parent_info,
            effective_uid=os.geteuid(),
            effective_gid=os.getegid(),
        ):
            raise GatewayError("Actions summary is unavailable")
        expected = os.stat(
            path.name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        descriptor = os.open(
            path.name, flags, dir_fd=parent_descriptor)
        info = os.fstat(descriptor)
        if (
            expected.st_dev != info.st_dev
            or expected.st_ino != info.st_ino
            or not _actions_summary_metadata_allowed(
                path,
                info,
                effective_uid=os.geteuid(),
                effective_gid=os.getegid(),
            )
        ):
            raise GatewayError("Actions summary is unavailable")
        raw = b""
        while len(raw) <= MAX_ACTIONS_SUMMARY_BYTES:
            chunk = os.read(
                descriptor,
                min(64 * 1024, MAX_ACTIONS_SUMMARY_BYTES + 1 - len(raw)),
            )
            if not chunk:
                break
            raw += chunk
        if (
            len(raw) != info.st_size
            or len(raw) > MAX_ACTIONS_SUMMARY_BYTES
        ):
            raise GatewayError("Actions summary is unavailable")
    except (OSError, GatewayError) as exc:
        if isinstance(exc, GatewayError):
            raise
        raise GatewayError("Actions summary is unavailable") from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if parent_descriptor >= 0:
            os.close(parent_descriptor)

    def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        output: dict[str, Any] = {}
        for key, value in pairs:
            if key in output:
                raise ValueError("duplicate")
            output[key] = value
        return output

    try:
        value = json.loads(raw, object_pairs_hook=reject_duplicates)
    except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as exc:
        raise GatewayError("Actions summary is unavailable") from exc
    required = {
        "schemaVersion",
        "type",
        "node",
        "updatedAt",
        "expiresAt",
        "runs",
    }
    # Version 1 is exactly the historical document. Version 2 adds the
    # workflow catalog, control outcomes and per-run steps; nothing else may
    # ride along in either version.
    version = value.get("schemaVersion") if isinstance(value, dict) else None
    if (
        not isinstance(version, int)
        or isinstance(version, bool)
        or version not in (1, 2)
    ):
        raise GatewayError("Actions summary is unavailable")
    extended = version == 2
    if extended:
        required |= {"repositories", "controls"}
    if set(value) != required:
        raise GatewayError("Actions summary is unavailable")
    if (
        value.get("type") != "forkmesh.mirror-actions-summary"
        or value.get("node") != node
        or isinstance(value.get("updatedAt"), bool)
        or isinstance(value.get("expiresAt"), bool)
    ):
        raise GatewayError("Actions summary is unavailable")
    try:
        updated_at = int(value["updatedAt"])
        expires_at = int(value["expiresAt"])
    except (TypeError, ValueError, OverflowError) as exc:
        raise GatewayError("Actions summary is unavailable") from exc
    if (
        updated_at < now_ms - MAX_ACTIONS_SUMMARY_LEASE_MS
        or updated_at > now_ms + MAX_ACTIONS_SUMMARY_CLOCK_SKEW_MS
        or expires_at <= now_ms
        or expires_at <= updated_at
        or expires_at - updated_at > MAX_ACTIONS_SUMMARY_LEASE_MS
    ):
        raise GatewayError("Actions summary is unavailable")
    runs = value.get("runs")
    if (
        not isinstance(runs, list)
        or len(runs) > MAX_ACTIONS_SUMMARY_RUNS
    ):
        raise GatewayError("Actions summary is unavailable")
    allowed_statuses = {
        "awaiting-approval",
        "queued",
        "running",
        "success",
        "failed",
        "rejected",
        "cancelled",
        "skipped",
    }
    expected_run_fields = {
        "id",
        "owner",
        "repository",
        "workflow",
        "commit",
        "ref",
        "status",
        "createdAt",
        "startedAt",
        "finishedAt",
        "logTail",
    }
    if extended:
        expected_run_fields |= {"workflowPath", "pullNumber", "steps"}
    output_runs = []
    for item in runs:
        if not isinstance(item, dict) or set(item) != expected_run_fields:
            raise GatewayError("Actions summary is unavailable")
        if any(
            isinstance(item.get(field), bool)
            for field in ("id", "createdAt", "startedAt", "finishedAt")
        ):
            raise GatewayError("Actions summary is unavailable")
        try:
            run_id = int(item.get("id"))
            created_at = int(item.get("createdAt"))
            started_at = int(item.get("startedAt"))
            finished_at = int(item.get("finishedAt"))
        except (TypeError, ValueError, OverflowError) as exc:
            raise GatewayError("Actions summary is unavailable") from exc
        run_owner = str(item.get("owner") or "").strip().lower()
        run_repository = str(item.get("repository") or "").strip()
        workflow = str(item.get("workflow") or "").strip()
        commit = str(item.get("commit") or "").strip().lower()
        ref = str(item.get("ref") or "").strip()
        status_value = str(item.get("status") or "")
        log_tail = str(item.get("logTail") or "")
        if (
            not 1 <= run_id <= 2_147_483_647
            or not NODE_RE.fullmatch(run_owner)
            or not REPO_RE.fullmatch(run_repository)
            or not 1 <= len(workflow) <= 160
            or any(character in workflow for character in ("\x00", "\r", "\n"))
            or not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", commit)
            or not 1 <= len(ref) <= 160
            or any(character in ref for character in ("\x00", "\r", "\n"))
            or status_value not in allowed_statuses
            or min(created_at, started_at, finished_at) < 0
            or max(created_at, started_at, finished_at) >
                now_ms + MAX_ACTIONS_SUMMARY_CLOCK_SKEW_MS
            or len(log_tail.encode("utf-8")) > MAX_ACTIONS_LOG_TAIL_BYTES
            or "\x00" in log_tail
        ):
            raise GatewayError("Actions summary is unavailable")
        run_extension = (
            _actions_summary_run_extension(item) if extended else {})
        if (
            run_owner != owner.lower()
            or run_repository.lower() != repository.lower()
        ):
            continue
        output_runs.append({
            "id": run_id,
            "workflow": workflow,
            "commit": commit,
            "ref": ref,
            "status": status_value,
            "createdAt": created_at,
            "startedAt": started_at,
            "finishedAt": finished_at,
            "logTail": log_tail,
            **run_extension,
        })
    output = {
        "ok": True,
        "node": node,
        "updatedAt": updated_at,
        "expiresAt": expires_at,
        "runs": output_runs,
    }
    if extended:
        output["schemaVersion"] = 2
        output.update(_actions_summary_catalog(
            value.get("repositories"), owner=owner, repository=repository))
        output["controls"] = _actions_summary_controls(
            value.get("controls"),
            owner=owner,
            repository=repository,
            now_ms=now_ms,
        )
    return output


class ReplayCache:
    def __init__(self, maximum: int = 10_000) -> None:
        self.maximum = maximum
        self._seen: dict[str, int] = {}
        self._lock = threading.Lock()

    def claim(self, request_id: str, expires_at: int, now_ms: int) -> bool:
        with self._lock:
            self._seen = {
                key: expiry for key, expiry in self._seen.items() if expiry >= now_ms
            }
            if request_id in self._seen:
                return False
            if len(self._seen) >= self.maximum:
                oldest = min(self._seen, key=self._seen.get)
                del self._seen[oldest]
            self._seen[request_id] = expires_at
            return True


def service_counter_agent_class(user_agent: str) -> str:
    """Reduce a request User-Agent to one bounded, non-identifying class.

    The raw header is never stored or published: it is a fingerprint. The
    coarse class below is enough to say whether the last served clone went to
    a git client, a browser, another mesh node, or a crawler, and it matches
    the desktop's own generalized request log vocabulary.
    """
    value = str(user_agent or "").lower()
    if "forkmesh" in value:
        return "forkmesh-node"
    if "git/" in value or "libgit2" in value or "jgit" in value:
        return "git-client"
    if any(
        token in value
        for token in ("bot", "crawl", "curl", "wget", "python-requests")
    ):
        return "bot-tool"
    if any(
        token in value
        for token in ("mozilla", "chrome", "safari", "firefox")
    ):
        return "browser"
    return "client"


class GatewayServiceCounters:
    """Persist content-free clone and website totals beside gateway config."""

    def __init__(
        self,
        path: Path,
        *,
        clock_ms: Callable[[], int],
    ) -> None:
        self.path = path
        self.clock_ms = clock_ms
        self._lock = threading.Lock()
        self._repositories: dict[str, dict[str, int]] = {}
        self._dirty = False
        self._flush_timer: threading.Timer | None = None
        self._load()

    def _load(self) -> None:
        try:
            info = self.path.lstat()
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_nlink != 1
                or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o600
                or not 0 < info.st_size <= MAX_SERVICE_COUNTERS_BYTES
            ):
                return
            value = json.loads(self.path.read_text(encoding="utf-8"))
        except (FileNotFoundError, OSError, json.JSONDecodeError):
            return
        if (
            not isinstance(value, dict)
            or set(value) != {"schemaVersion", "type", "repositories"}
            or value.get("schemaVersion") != SCHEMA_VERSION
            or value.get("type") != SERVICE_COUNTERS_TYPE
            or not isinstance(value.get("repositories"), dict)
        ):
            return
        loaded: dict[str, dict[str, int | str]] = {}
        for key, row in list(value["repositories"].items())[:4096]:
            # The last-served stamps are an optional extension: a row written
            # by an older gateway carries the three core fields only.
            if (
                not isinstance(key, str)
                or len(key) > 220
                or key.count("/") != 1
                or not isinstance(row, dict)
                or not set(SERVICE_COUNTER_ROW_FIELDS) <= set(row)
                or not set(row)
                <= set(SERVICE_COUNTER_ROW_FIELDS + SERVICE_COUNTER_STAMP_FIELDS)
            ):
                continue
            owner, repository = key.split("/", 1)
            if (
                not NODE_RE.fullmatch(owner)
                or not REPO_RE.fullmatch(repository)
            ):
                continue
            fields: dict[str, int | str] = {}
            valid = True
            for field in SERVICE_COUNTER_ROW_FIELDS:
                raw = row.get(field)
                if isinstance(raw, bool):
                    valid = False
                    break
                try:
                    fields[field] = int(raw)
                except (TypeError, ValueError, OverflowError):
                    valid = False
                    break
            if (
                not valid
                or not 0 <= fields["clonesServed"] <= MAX_SERVICE_COUNTER
                or not 0 <= fields["websiteServed"] <= MAX_SERVICE_COUNTER
                or not 0 <= fields["updatedAt"] <= MAX_SAFE_JSON_INTEGER
            ):
                continue
            for field in SERVICE_COUNTER_STAMP_FIELDS:
                if field not in row:
                    continue
                raw = row[field]
                if field.endswith("At"):
                    if isinstance(raw, bool):
                        continue
                    try:
                        stamp = int(raw)
                    except (TypeError, ValueError, OverflowError):
                        continue
                    if 0 < stamp <= MAX_SAFE_JSON_INTEGER:
                        fields[field] = stamp
                elif raw in SERVICE_COUNTER_AGENT_CLASSES:
                    fields[field] = raw
            loaded[key] = fields
        self._repositories = loaded

    def _save(self) -> None:
        payload = _canonical_json({
            "schemaVersion": SCHEMA_VERSION,
            "type": SERVICE_COUNTERS_TYPE,
            "repositories": self._repositories,
        }) + "\n"
        encoded = payload.encode("utf-8")
        if len(encoded) > MAX_SERVICE_COUNTERS_BYTES:
            raise GatewayError("service counters are full")
        parent = self.path.parent
        parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        descriptor, staged_name = tempfile.mkstemp(
            prefix=".service-counters-",
            suffix=".json",
            dir=parent,
        )
        staged = Path(staged_name)
        try:
            os.fchmod(descriptor, 0o600)
            with os.fdopen(descriptor, "wb") as stream:
                descriptor = -1
                stream.write(encoded)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(staged, self.path)
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            try:
                staged.unlink()
            except FileNotFoundError:
                pass

    def record(
        self,
        owner: str,
        repository: str,
        operation: str,
        user_agent: str = "",
    ) -> bool:
        if operation == "git-upload-pack":
            field = "clonesServed"
            stamp_field, agent_field = "cloneServedAt", "cloneServedAgent"
        elif operation in WEBSITE_COUNTER_OPERATIONS:
            field = "websiteServed"
            stamp_field, agent_field = "websiteServedAt", "websiteServedAgent"
        else:
            return False
        key = owner.lower() + "/" + repository.lower()
        if (
            not NODE_RE.fullmatch(owner.lower())
            or not REPO_RE.fullmatch(repository)
        ):
            return False
        with self._lock:
            previous = dict(self._repositories.get(key, {
                "clonesServed": 0,
                "websiteServed": 0,
                "updatedAt": 0,
            }))
            previous[field] = min(
                MAX_SERVICE_COUNTER,
                previous[field] + 1,
            )
            served_at = min(
                MAX_SAFE_JSON_INTEGER,
                max(0, int(self.clock_ms())),
            )
            previous["updatedAt"] = served_at
            # "When did this node last serve a clone / a website read, and to
            # what kind of client" -- the pair the Mirror node cards show
            # beside each total.
            if served_at > 0:
                previous[stamp_field] = served_at
                previous[agent_field] = service_counter_agent_class(user_agent)
            self._repositories[key] = previous
            self._dirty = True
            if self._flush_timer is None:
                self._flush_timer = threading.Timer(1.0, self.flush)
                self._flush_timer.daemon = True
                self._flush_timer.start()
        return True

    def ensure_repository(self, owner: str, repository: str) -> bool:
        clean_owner = owner.lower()
        clean_repository = repository.lower()
        if (
            not NODE_RE.fullmatch(clean_owner)
            or not REPO_RE.fullmatch(clean_repository)
        ):
            return False
        key = clean_owner + "/" + clean_repository
        with self._lock:
            if key in self._repositories:
                return True
            self._repositories[key] = {
                "clonesServed": 0,
                "websiteServed": 0,
                "updatedAt": min(
                    MAX_SAFE_JSON_INTEGER,
                    max(0, int(self.clock_ms())),
                ),
            }
            self._dirty = True
            if self._flush_timer is None:
                self._flush_timer = threading.Timer(1.0, self.flush)
                self._flush_timer.daemon = True
                self._flush_timer.start()
        return True

    def flush(self) -> bool:
        with self._lock:
            timer = self._flush_timer
            self._flush_timer = None
            if timer is not None and timer is not threading.current_thread():
                timer.cancel()
            if not self._dirty:
                return True
            try:
                self._save()
            except (OSError, GatewayError):
                return False
            self._dirty = False
            return True

    def close(self) -> None:
        self.flush()


def _portable_dashboard_session(
    headers: Mapping[str, str], router_public_key: str, now_ms: int
) -> dict[str, Any] | None:
    """Verify the controller grant carried by the shared mirror cookie."""
    if portable_sessions is None or Ed25519PublicKey is None:
        return None
    normalized = {str(key).lower(): str(value) for key, value in headers.items()}
    token = portable_sessions.cookie_value(normalized.get("cookie", ""))
    if not token:
        return None
    return _portable_dashboard_token(token, router_public_key, now_ms)


def _portable_dashboard_token(
    token: str, router_public_key: str, now_ms: int
) -> dict[str, Any] | None:
    """Verify one bounded controller grant from a cookie or handoff body."""
    if portable_sessions is None or Ed25519PublicKey is None:
        return None
    try:
        verifier = Ed25519PublicKey.from_public_bytes(
            _base64url_decode(router_public_key, 32)
        )

        def verify(message: bytes, signature: bytes) -> bool:
            verifier.verify(signature, message)
            return True

        return portable_sessions.parse_token(
            token, now_ms=int(now_ms), verify=verify
        )
    except Exception:
        return None


EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS = 1798761600000


def _event_signing_repo_key(owner: str, name: str) -> str:
    owner = str(owner or "").strip().lower()
    name = str(name or "").strip().lower()
    if not owner or not name or any(
        mark in part for part in (owner, name) for mark in ("/", "\n", "\0")
    ):
        return ""
    return owner + "/" + name


def _event_signing_messages(
    kind: str, body: str, owner: str, name: str,
    now_ms: int | None = None,
) -> list[bytes]:
    messages = []
    key = _event_signing_repo_key(owner, name)
    if key:
        messages.append(
            ("forkmesh-" + kind + "-v2\n" + key + "\n" + body).encode("utf-8"))
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    if now_ms < EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS:
        messages.append(("forkmesh-" + kind + "-v1\n" + body).encode("utf-8"))
    return messages


def _verified_dashboard_event(
    author: str, signature: Any, kind: str, body: str, owner: str, name: str,
) -> bool:
    return any(
        _verified_dashboard_signature(author, signature, message)
        for message in _event_signing_messages(kind, body, owner, name)
    )


def _verified_dashboard_signature(
    author: Any, signature: Any, message: bytes
) -> bool:
    if Ed25519PublicKey is None:
        return False
    try:
        public_key = _base64url_decode(str(author or ""), 32)
        signature_raw = _base64url_decode(str(signature or ""), 64)
        if (
            _base64url_encode(public_key) != str(author or "")
            or _base64url_encode(signature_raw) != str(signature or "")
        ):
            return False
        Ed25519PublicKey.from_public_bytes(public_key).verify(
            signature_raw, message
        )
        return True
    except Exception:
        return False


def _dashboard_event_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _open_loopback_control(request: Any, timeout: int) -> Any:
    """Open a validated loopback URL without consulting ambient proxies."""
    opener = urllib_request.build_opener(urllib_request.ProxyHandler({}))
    return opener.open(request, timeout=timeout)


def _dashboard_issue_content(event: Mapping[str, Any]) -> str | None:
    event_type = str(event.get("type") or "")
    attachments = event.get("attachments") or []
    if not isinstance(attachments, list) or any(
        not isinstance(item, str) for item in attachments
    ):
        return None
    if event_type == "open":
        return "\0".join([
            str(event.get("title") or ""),
            str(event.get("body") or ""),
            ",".join(attachments),
        ])
    if event_type in ("comment", "edit"):
        return "\0".join([
            str(event.get("body") or ""), ",".join(attachments)
        ])
    if event_type in ("title", "status", "milestone"):
        return str(event.get(event_type) or "")
    if event_type in ("labels", "assignees"):
        values = event.get(event_type)
        if not isinstance(values, list) or any(
            not isinstance(item, str) for item in values
        ):
            return None
        return ",".join(values)
    if event_type == "dates":
        try:
            return str(int(event.get("startDate") or 0)) + "\0" + str(
                int(event.get("endDate") or 0)
            )
        except (TypeError, ValueError):
            return None
    if event_type in ("priority", "progress"):
        try:
            return str(int(event.get(event_type) or 0))
        except (TypeError, ValueError):
            return None
    if event_type == "claim":
        return str(event.get("assignee") or "") + "\0" + str(
            event.get("claimState") or ""
        )
    if event_type in ("delete",):
        return str(event.get("target") or "")
    if event_type == "vote":
        return ""
    return None


def _verify_dashboard_issue(
    number: int, event: Mapping[str, Any], owner: str = "", name: str = "",
) -> bool:
    event_type = str(event.get("type") or "")
    content = _dashboard_issue_content(event)
    if (
        content is None
        or event_type == "open" and number != 0
        or event_type != "open" and number <= 0
        or event_type == "status"
        and str(event.get("status") or "") not in ("open", "closed")
        or event_type == "claim"
        and str(event.get("claimState") or "") not in ("assigned", "released")
    ):
        return False
    try:
        timestamp = int(event.get("ts") or 0)
    except (TypeError, ValueError):
        return False
    author = str(event.get("author") or "")
    body = (
        event_type + "\n" + str(number) + "\n" + author + "\n"
        + str(timestamp) + "\n" + _dashboard_event_hash(content)
    )
    return timestamp > 0 and _verified_dashboard_event(
        author, event.get("sig"), "issue-event", body, owner, name
    )


def _dashboard_pull_event_content(event: Mapping[str, Any]) -> str | None:
    event_type = str(event.get("type") or "")
    body = str(event.get("body") or "")

    def integer(key: str) -> str:
        return str(int(event.get(key) or 0))

    try:
        if event_type == "comment":
            return body
        if event_type == "review":
            return str(event.get("state") or "") + "\0" + body
        if event_type == "line-comment":
            return "\0".join([
                str(event.get("path") or ""), str(event.get("side") or ""),
                integer("line"), body,
            ])
        if event_type == "thread-comment":
            return "\0".join([
                str(event.get("threadId") or ""),
                str(event.get("path") or ""), str(event.get("side") or ""),
                integer("lineStart"), integer("lineEnd"), body,
                str(event.get("suggestionPatch") or ""),
            ])
        if event_type == "thread-reply":
            return "\0".join([
                str(event.get("threadId") or ""),
                str(event.get("parentId") or ""), body,
            ])
        if event_type == "thread-state":
            return "\0".join([
                str(event.get("threadId") or ""),
                str(event.get("state") or ""), body,
            ])
        if event_type == "suggestion-state":
            return "\0".join([
                str(event.get("threadId") or ""),
                str(event.get("state") or ""),
                str(event.get("appliedCommit") or ""), body,
            ])
        if event_type == "status":
            if event.get("state") != "closed":
                return None
            return "closed\0" + body
        if event_type == "delete":
            return body
    except (TypeError, ValueError):
        return None
    return None


PULL_LIFECYCLE_EVENT_TYPES = frozenset({"status", "delete"})
PULL_LIFECYCLE_FINAL_STATUSES = frozenset({"merged", "closed-merged", "deleted"})


def _pull_record_fields(text: str) -> dict[str, str]:
    raw = str(text or "").lstrip()
    if not raw.startswith("---"):
        return {}
    header = raw[3:]
    end = header.find("\n---")
    if end < 0:
        return {}
    fields: dict[str, str] = {}
    for line in header[:end].splitlines():
        key, separator, value = line.partition(":")
        key = key.strip()
        if separator and key and key not in fields:
            fields[key] = value.strip()
    return fields


def _pull_lifecycle_record(repository: Any, number: int) -> dict[str, str]:
    path = ".forkmesh/pulls/%d/pull.md" % number
    for ref in (
        "refs/heads/forkmesh/pull/%d" % number,
        "refs/pr/%d/metadata" % number,
        "refs/heads/forkmesh/pulls",
    ):
        try:
            payload = repository.blob({"path": path, "ref": ref})
        except (GatewayError, GitError):
            continue
        if payload.get("encoding") != "utf8":
            continue
        fields = _pull_record_fields(payload.get("content") or "")
        if fields.get("number") == str(number):
            return fields
    return {}


def _pull_lifecycle_refusal(
    repository: Any, number: int, event: Mapping[str, Any], owner: str,
    actor: str, trusted_browser_origin: bool,
) -> tuple[str, int] | None:
    record = _pull_lifecycle_record(repository, number)
    if not record:
        return "pull_unavailable", 503
    status = str(record.get("status") or "open").strip().lower()
    if status in PULL_LIFECYCLE_FINAL_STATUSES:
        return "pull_final", 409
    pull_key = str(record.get("author") or "").strip()
    if pull_key and str(event.get("author") or "").strip() == pull_key:
        return None
    actor = str(actor or "").strip().lower()
    if not actor or not trusted_browser_origin:
        return "not_authorized", 403
    if actor == str(owner or "").strip().lower():
        return None
    if actor == str(record.get("authorName") or "").strip().lower():
        return None
    return "not_authorized", 403


def _verify_dashboard_pull_event(
    number: int, event: Mapping[str, Any], owner: str = "", name: str = "",
) -> bool:
    content = _dashboard_pull_event_content(event)
    if number <= 0 or content is None:
        return False
    try:
        timestamp = int(event.get("ts") or 0)
    except (TypeError, ValueError):
        return False
    event_type = str(event.get("type") or "")
    author = str(event.get("author") or "")
    body = (
        event_type + "\n" + str(number)
        + "\n" + author + "\n" + str(timestamp) + "\n"
        + _dashboard_event_hash(content)
    )
    return timestamp > 0 and _verified_dashboard_event(
        author, event.get("sig"), "pull-comment", body, owner, name
    )


def _verify_dashboard_pull(
    pull: Mapping[str, Any], owner: str = "", name: str = "",
) -> bool:
    fields = ("title", "base", "head", "patch", "commits")
    if any(not isinstance(pull.get(key, ""), str) for key in fields):
        return False
    try:
        timestamp = int(pull.get("ts") or 0)
    except (TypeError, ValueError):
        return False
    author = str(pull.get("author") or "")
    contents = ["\0".join(str(pull.get(key) or "") for key in fields)]
    if not pull.get("commits"):
        contents.append(
            "\0".join(str(pull.get(key) or "") for key in fields[:-1])
        )
    for content in contents:
        body = (
            author + "\n" + str(timestamp)
            + "\n" + _dashboard_event_hash(content)
        )
        if timestamp > 0 and _verified_dashboard_event(
            author, pull.get("sig"), "pull-event", body, owner, name
        ):
            return True
    return False


def _verify_dashboard_discussion(
    number: int, event: Mapping[str, Any], owner: str = "", name: str = "",
) -> bool:
    event_type = str(event.get("type") or "")
    if event_type == "open":
        content = "\0".join([
            str(event.get("title") or ""), str(event.get("body") or ""),
            str(event.get("category") or ""),
        ])
    elif event_type == "comment" and number > 0:
        content = str(event.get("body") or "")
    elif event_type == "answer" and number > 0:
        content = str(event.get("target") or "")
    else:
        return False
    try:
        timestamp = int(event.get("ts") or 0)
    except (TypeError, ValueError):
        return False
    author = str(event.get("author") or "")
    body = (
        event_type + "\n" + str(number)
        + "\n" + author + "\n" + str(timestamp) + "\n"
        + _dashboard_event_hash(content)
    )
    return timestamp > 0 and _verified_dashboard_event(
        author, event.get("sig"), "discussion-event", body, owner, name
    )


class DashboardService:
    """Same-codebase website served straight from mirrored git blobs.

    Static assets come from the designated assets repository (checked-in build
    outputs under app/public/ and www/public/), so the mirror serves the
    byte-identical dashboard the relay Worker serves and picks up updates with
    ordinary repository sync. The /api subset below is the read-only surface
    the logged-out dashboard needs; everything else fails the same way it does
    for a guest on the canonical origin.
    """

    def __init__(self, application: "GatewayApplication") -> None:
        self.application = application
        self.config = application.config
        self._asset_cache = BoundedMemoryCache(
            "dashboardAssets",
            max_entries=DASHBOARD_ASSET_CACHE_ENTRIES,
            max_bytes=DASHBOARD_ASSET_CACHE_BYTES,
            entry_overhead=MEMORY_CACHE_ENTRY_OVERHEAD,
        )
        self._catalog_cache = BoundedMemoryCache(
            "repositoryCatalog",
            max_entries=DASHBOARD_CATALOG_CACHE_ENTRIES,
            max_bytes=DASHBOARD_CATALOG_CACHE_BYTES,
        )
        self._replica = (
            mirror_replica.ReplicaStore(self.config.dashboard_replica_path)
            if self.config.dashboard_replica_path is not None
            else None
        )

    def _portable_session(
        self, headers: Mapping[str, str]
    ) -> dict[str, Any] | None:
        if portable_sessions is None:
            return None
        normalized = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        token = portable_sessions.cookie_value(normalized.get("cookie", ""))
        if not token:
            return None
        cache_key = hashlib.sha256(token.encode("utf-8")).digest()
        found, cached = self.application.session_cache.get(cache_key)
        now_ms = self.application.clock_ms()
        if found and isinstance(cached, dict):
            # Signed expiry remains authoritative even though verification is
            # cached. This also bounds the useful life of stolen process RAM.
            if int(cached.get("exp", 0)) > int(now_ms) - int(
                portable_sessions.SESSION_CLOCK_SKEW_MS
            ):
                return dict(cached)
            self.application.session_cache.invalidate(cache_key)
        self.application.session_cache.backend_read()
        session = _portable_dashboard_session(
            headers,
            self.config.router_public_key,
            now_ms,
        )
        if session is not None:
            encoded_size = len(_canonical_json(session).encode("utf-8"))
            self.application.session_cache.put(
                cache_key, dict(session), size=encoded_size)
        return session

    def invalidate_replica(self) -> None:
        if self._replica is not None:
            self._replica.invalidate(preload=True)

    def invalidate_repository_generation(self) -> None:
        self._asset_cache.invalidate_all()
        self._catalog_cache.invalidate_all()

    # -- routing ------------------------------------------------------------

    def maybe_dispatch(
        self,
        method: str,
        parsed: Any,
        headers: Mapping[str, str],
        body: bytes,
    ) -> GatewayResponse | None:
        path = parsed.path
        if (
            _public_git_route(path) is not None
            or path.startswith("/v1/")
            or path in ("/health", "/healthz", "/readyz")
            or path in (
                "/forkmesh-mirror.json", "/.well-known/forkmesh-mirror.json"
            )
        ):
            return None
        if path.startswith("/api/") or path == "/api":
            if method == "OPTIONS":
                return self._api_preflight(parsed, headers)
            response = self._api(method, parsed, headers, body)
            return self._api_cors(response, method, parsed, headers)
        if method not in ("GET", "HEAD"):
            return None
        if path == "/":
            return self._redirect("/dashboard")
        if path == "/login" and self._portable_session(headers) is not None:
            return self._redirect("/dashboard")
        if path in ("/login", "/signup") and self.config.dashboard_canonical_origin:
            return self._redirect(
                self.config.dashboard_canonical_origin + path
            )
        if path in ("/analytics.js", "/analytics-sdk.js"):
            return GatewayResponse(
                200,
                "text/javascript; charset=utf-8",
                ANALYTICS_STUB_BODY,
                dict(DASHBOARD_ASSET_HEADERS),
            )
        if parsed.query:
            # Static documents and assets ignore query strings (the checked-in
            # ?v=${version} stamps arrive URL-decoded as ordinary queries).
            pass
        asset = DASHBOARD_ASSET_ROUTES.get(path)
        if asset is not None:
            return self._serve_blob(asset, path, headers)
        for browser_prefix, repo_prefixes in DASHBOARD_ASSET_PREFIXES:
            if path.startswith(browser_prefix):
                suffix = path[len(browser_prefix):]
                if not DASHBOARD_ASSET_PATH_RE.fullmatch(suffix) or (
                    "//" in suffix or "/../" in "/" + suffix + "/"
                ):
                    return self._not_found_page()
                for repo_prefix in repo_prefixes:
                    response = self._serve_blob(
                        repo_prefix + suffix, path, headers, missing_ok=True
                    )
                    if response is not None:
                        return response
                return self._not_found_page()
        if self.config.dashboard_assets_root is not None:
            relative = path.lstrip("/")
            extension = posixpath.splitext(relative)[1].lower()
            if (
                DASHBOARD_ASSET_PATH_RE.fullmatch(relative)
                and "//" not in relative
                and "/../" not in "/" + relative + "/"
                and extension in DASHBOARD_CONTENT_TYPES
                and extension != ".html"
            ):
                response = self._serve_blob(
                    relative, path, headers, missing_ok=True
                )
                if response is not None:
                    return response
        if path == "/dashboard" or path == "/dashboard/":
            if path.endswith("/"):
                return self._redirect("/dashboard")
            return self._serve_page(DASHBOARD_PAGE_ROOT + "/index.html")
        if path.startswith("/dashboard/"):
            name = path[len("/dashboard/"):].rstrip("/")
            if DASHBOARD_PAGE_RE.fullmatch(name):
                return self._serve_page(
                    DASHBOARD_PAGE_ROOT + "/" + name + "/index.html",
                    missing_ok=True,
                ) or self._not_found_page()
            return self._not_found_page()
        segments = [piece for piece in path.split("/") if piece]
        repository_key: tuple[str, str] | None = None
        if len(segments) >= 2:
            try:
                repository_key = (
                    unquote(segments[0], errors="strict").lower(),
                    unquote(segments[1], errors="strict").lower(),
                )
            except UnicodeError:
                repository_key = None
        if (
            len(segments) >= 2
            and segments[0] not in DASHBOARD_RESERVED_SEGMENTS
            and not segments[0].startswith(("@", "_", "."))
            and NODE_RE.fullmatch(segments[0])
            and REPO_RE.fullmatch(unquote(segments[1]))
            # A mirror must not render the generic repository shell for an
            # identity it does not actually serve.  Besides making stale
            # node-owner URLs look valid, the old catch-all turned typos into
            # misleading 200s.  Keep clean/deep links aligned with the signed
            # public repository configuration, just like Smart HTTP and the
            # read-only API routes below.
            and repository_key in self.application.repositories
        ):
            return self._serve_page(DASHBOARD_REPO_ASSET)
        return self._not_found_page()

    # -- static assets ------------------------------------------------------

    def _assets_repository(self) -> "GitRepository | None":
        key = self.config.dashboard_assets_repository
        if key is None:
            return None
        return self.application.repositories.get(key)

    def _built_asset(self, repo_path: str) -> tuple[str, bytes] | None:
        relative = repo_path
        for prefix in DASHBOARD_BUILT_PREFIXES:
            if relative.startswith(prefix):
                relative = relative[len(prefix):]
                break
        if not DASHBOARD_ASSET_PATH_RE.fullmatch(relative) or (
            "//" in relative or "/../" in "/" + relative + "/"
        ):
            return None
        try:
            root = os.path.realpath(str(self.config.dashboard_assets_root))
            target = os.path.realpath(os.path.join(root, relative))
            if not target.startswith(root + os.sep):
                return None
            info = os.stat(target)
        except OSError:
            return None
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_DASHBOARD_ASSET_BYTES:
            return None
        cache_key = ("root", target, info.st_mtime_ns, info.st_size)
        found, cached = self._asset_cache.get(cache_key)
        if found and isinstance(cached, tuple):
            return cached
        self._asset_cache.backend_read()
        try:
            with open(target, "rb") as stream:
                content = stream.read(MAX_DASHBOARD_ASSET_BYTES + 1)
        except OSError:
            return None
        if len(content) > MAX_DASHBOARD_ASSET_BYTES:
            return None
        entry = (hashlib.sha256(content).hexdigest(), content)
        self._asset_cache.put(cache_key, entry, size=len(content))
        return entry

    def _asset_blob(self, repo_path: str) -> tuple[str, bytes] | None:
        if self.config.dashboard_assets_root is not None:
            return self._built_asset(repo_path)
        repository = self._assets_repository()
        if repository is None:
            return None
        cache_key = (repository.cache_generation, repo_path)
        found, cached = self._asset_cache.get(cache_key)
        if found and isinstance(cached, tuple):
            return cached
        if not repository.check_integrity():
            return None
        self._asset_cache.backend_read()
        try:
            commit = repository.resolve_commit("")
            oid = _run_git(
                repository.git_dir,
                [
                    "rev-parse",
                    "--verify",
                    "--quiet",
                    "--end-of-options",
                    commit + ":" + repo_path,
                ],
                max_output=128,
                allow_exit_one=True,
            ).decode("ascii", "ignore").strip()
        except (GatewayError, GitError):
            return None
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", oid):
            return None
        try:
            content = _run_git(
                repository.git_dir,
                ["cat-file", "blob", oid],
                max_output=MAX_DASHBOARD_ASSET_BYTES,
            )
        except (GatewayError, GitError):
            return None
        if repo_path.endswith(".html"):
            stamp = b"?v=" + commit[:12].encode("ascii") + rb"\1"
            content = DASHBOARD_VERSION_PLACEHOLDER_RE.sub(stamp, content)
            oid = hashlib.sha256(content).hexdigest()
        entry = (oid, content)
        self._asset_cache.put(cache_key, entry, size=len(content))
        return entry

    def _serve_blob(
        self,
        repo_path: str,
        browser_path: str,
        headers: Mapping[str, str],
        *,
        missing_ok: bool = False,
    ) -> GatewayResponse | None:
        extension = posixpath.splitext(browser_path)[1].lower()
        content_type = DASHBOARD_CONTENT_TYPES.get(extension)
        if content_type is None:
            return None if missing_ok else self._not_found_page()
        blob = self._asset_blob(repo_path)
        if blob is None:
            return None if missing_ok else self._not_found_page()
        oid, content = blob
        etag = '"' + oid + '"'
        response_headers = dict(
            DASHBOARD_HTML_HEADERS
            if extension == ".html"
            else DASHBOARD_ASSET_HEADERS
        )
        response_headers["ETag"] = etag
        normalized = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        if normalized.get("if-none-match", "") == etag:
            return GatewayResponse(304, content_type, b"", response_headers)
        return GatewayResponse(200, content_type, content, response_headers)

    def _serve_page(
        self, repo_path: str, *, missing_ok: bool = False
    ) -> GatewayResponse | None:
        blob = self._asset_blob(repo_path)
        if blob is None:
            return None if missing_ok else self._not_found_page()
        _oid, content = blob
        return GatewayResponse(
            200,
            "text/html; charset=utf-8",
            content,
            dict(DASHBOARD_HTML_HEADERS),
        )

    def _not_found_page(self) -> GatewayResponse:
        blob = self._asset_blob(DASHBOARD_404_ASSET)
        if blob is None:
            return json_response({"ok": False, "error": "not_found"}, 404)
        _oid, content = blob
        return GatewayResponse(
            404,
            "text/html; charset=utf-8",
            content,
            dict(DASHBOARD_HTML_HEADERS),
        )

    def _redirect(self, target: str) -> GatewayResponse:
        return GatewayResponse(
            308,
            "text/plain; charset=utf-8",
            b"",
            {"Location": target, "Cache-Control": "no-store"},
        )

    def _replica_response(self, path: str) -> GatewayResponse | None:
        if self._replica is None:
            return None
        document = self._replica.document(path)
        if document is None:
            return None
        response = json_response(document["payload"])
        response.headers.update({
            "Cache-Control": "public, max-age=30",
            "X-ForkMesh-Replica": "public-projection",
            "X-ForkMesh-Replica-Fetched-At": str(document["fetchedAt"]),
            "X-ForkMesh-Replica-Generated-At": str(document["generatedAt"]),
        })
        return response

    def _api_cors(
        self,
        response: GatewayResponse,
        method: str,
        parsed: Any,
        headers: Mapping[str, str],
    ) -> GatewayResponse:
        """Expose only the independent dashboard's deliberate API surface.

        Public Git reads are credential-free and may be fetched from any site.
        Collaboration POSTs carry the shared HttpOnly portable cookie, so they
        are restricted to this deployment's canonical dashboard origin and
        explicitly opt into credentials. No capability/private route enters
        DashboardService, keeping CORS away from the router trust boundary.
        """
        match = re.fullmatch(
            r"/api/repo/([^/]+)/([^/]+)/([^/]+)", parsed.path
        )
        if match is None:
            return response
        operation = match.group(3)
        normalized = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        origin = normalized.get("origin", "")
        if method in ("GET", "HEAD") and operation in (
            DIRECT_PUBLIC_READ_OPERATIONS | {"about", "mirrors"}
        ):
            response.headers["Access-Control-Allow-Origin"] = "*"
            response.headers["Access-Control-Expose-Headers"] = (
                "ETag, X-ForkMesh-Served-By, X-ForkMesh-Memory-Cache"
            )
        elif (
            method == "POST"
            and operation in {"issues", "pulls", "discussions"}
            and origin
            and origin == self.config.dashboard_canonical_origin
        ):
            response.headers["Access-Control-Allow-Origin"] = origin
            response.headers["Access-Control-Allow-Credentials"] = "true"
            response.headers["Access-Control-Expose-Headers"] = (
                "X-ForkMesh-Served-By"
            )
            response.headers["Vary"] = "Origin"
        return response

    def _api_preflight(
        self, parsed: Any, headers: Mapping[str, str]
    ) -> GatewayResponse:
        normalized = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        origin = normalized.get("origin", "")
        requested_method = normalized.get(
            "access-control-request-method", ""
        ).upper()
        requested_headers = {
            item.strip().lower()
            for item in normalized.get(
                "access-control-request-headers", ""
            ).split(",")
            if item.strip()
        }
        match = re.fullmatch(
            r"/api/repo/([^/]+)/([^/]+)/(issues|pulls|discussions)",
            parsed.path,
        )
        if (
            match is None
            or origin != self.config.dashboard_canonical_origin
            or requested_method != "POST"
            or not requested_headers.issubset({"accept", "content-type"})
        ):
            return json_response({"ok": False, "error": "not_found"}, 404)
        return GatewayResponse(
            204,
            "text/plain; charset=utf-8",
            b"",
            {
                "Access-Control-Allow-Origin": origin,
                "Access-Control-Allow-Credentials": "true",
                "Access-Control-Allow-Methods": "POST, OPTIONS",
                "Access-Control-Allow-Headers": "Accept, Content-Type",
                "Access-Control-Max-Age": "600",
                "Vary": "Origin",
            },
        )

    # -- read-only /api subset ----------------------------------------------

    def _api(
        self,
        method: str,
        parsed: Any,
        headers: Mapping[str, str],
        body: bytes,
    ) -> GatewayResponse:
        path = parsed.path
        if path == "/api/version" and method in ("GET", "HEAD"):
            return json_response(
                {
                    "ok": True,
                    "version": "mirror/" + self.config.node,
                    "dashboardReplica": self._replica is not None,
                }
            )
        if path == "/api/accounts/session-handoff" and method == "POST":
            return self._accept_portable_session_handoff(headers, body)
        if path == "/api/accounts/sessions" and method in ("GET", "HEAD"):
            session = self._portable_session(headers)
            if session is None:
                return json_response({"error": "unauthorized"}, 401)
            return json_response(
                {
                    "ok": True,
                    "sessions": [
                        {
                            "id": session["sid"],
                            "deviceLabel": "Portable mirror session",
                            "ipAddress": "",
                            "createdAt": session["iat"],
                            "lastSeenAt": session["iat"],
                            "expiresAt": session["exp"],
                            "current": True,
                        }
                    ],
                    "account": {
                        "nodeName": session["name"],
                        "status": "active",
                        "emailVerified": True,
                        "kind": "user",
                        "lastSeenAt": session["iat"],
                    },
                    "portable": True,
                    "controllerIndependent": True,
                }
            )
        if path == "/api/repositories" and method in ("GET", "HEAD"):
            return json_response(
                {
                    "ok": True,
                    "repositories": self._catalog(),
                    "sshGatewayConfigured": bool(
                        self.config.dashboard_ssh_origin
                    ),
                }
            )
        match = re.fullmatch(r"/api/repo/([^/]+)/([^/]+)/([^/]+)", path)
        if match is not None:
            try:
                owner = unquote(match.group(1), errors="strict").lower()
                name = unquote(match.group(2), errors="strict")
                operation = unquote(match.group(3), errors="strict")
            except UnicodeError:
                return json_response(
                    {"ok": False, "error": "not_found"}, 404
                )
            if operation == "logo" and method in ("GET", "HEAD"):
                return self._repo_logo(owner, name)
            if operation == "about" and method in ("GET", "HEAD"):
                return self._repo_about(owner, name, path)
            if operation in DIRECT_PUBLIC_READ_OPERATIONS and method in (
                "GET",
                "HEAD",
            ):
                return self._repo_read(
                    method, owner, name, operation, parsed.query, headers
                )
            if operation in {"issues", "pulls", "discussions"} and method == "POST":
                return self._repo_collaboration(
                    owner, name, operation, headers, body
                )
            if operation == "mirrors" and method in ("GET", "HEAD"):
                return self._repo_mirrors(owner, name)
        reachability = re.fullmatch(
            r"/api/repo/([^/]+)/([^/]+)/mirrors/([^/]+)/reachability", path
        )
        if reachability is not None and method in ("GET", "HEAD", "POST"):
            return self._repo_reachability(
                reachability.group(1).lower(),
                reachability.group(2),
                reachability.group(3).lower(),
            )
        if method in ("GET", "HEAD") and self._replica is not None:
            response = self._replica_response(path)
            if response is not None:
                return response
        return json_response({"ok": False, "error": "not_found"}, 404)

    def _accept_portable_session_handoff(
        self, headers: Mapping[str, str], body: bytes
    ) -> GatewayResponse:
        """Exchange an App form POST for this mirror's HttpOnly cookie."""
        normalized = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        if (
            normalized.get("origin", "")
            != self.config.dashboard_canonical_origin
            or normalized.get("content-type", "").split(";", 1)[0]
            .strip().lower() != "application/x-www-form-urlencoded"
            or not body
            or len(body) > 16 * 1024
        ):
            return json_response({"error": "unauthorized"}, 401)
        try:
            values = parse_qs(
                body.decode("utf-8"),
                keep_blank_values=True,
                strict_parsing=True,
                max_num_fields=2,
            )
        except (UnicodeError, ValueError):
            return json_response({"error": "invalid_handoff"}, 400)
        if set(values) != {"grant", "next"} or any(
            len(items) != 1 for items in values.values()
        ):
            return json_response({"error": "invalid_handoff"}, 400)
        grant = values["grant"][0]
        next_path = values["next"][0]
        try:
            target = urlsplit(next_path)
        except ValueError:
            target = None
        if (
            target is None
            or target.scheme
            or target.netloc
            or target.fragment
            or not target.path.startswith("/")
            or target.path.startswith("//")
            or len(next_path) > 2048
            or any(ch == "\\" or ord(ch) < 32 or ord(ch) == 127
                   for ch in next_path)
        ):
            return json_response({"error": "invalid_handoff"}, 400)
        session = _portable_dashboard_token(
            grant, self.config.router_public_key, self.application.clock_ms()
        )
        if session is None:
            return json_response({"error": "invalid_session"}, 401)
        max_age = max(
            0,
            int((int(session["exp"]) - self.application.clock_ms()) / 1000),
        )
        try:
            public_host = str(
                urlsplit(self.config.public_origin).hostname or ""
            ).lower()
        except ValueError:
            public_host = ""
        domain = (
            "; Domain=.forkmesh.com"
            if public_host == "forkmesh.com"
            or public_host.endswith(".forkmesh.com")
            else ""
        )
        return GatewayResponse(
            303,
            "text/plain; charset=utf-8",
            b"",
            {
                "Location": next_path,
                "Set-Cookie": (
                    "forkmesh_portable=" + grant + "; Path=/" + domain
                    + "; Max-Age=%d; HttpOnly; Secure; SameSite=Strict"
                    % max_age
                ),
                "Cache-Control": "no-store, max-age=0, must-revalidate",
                "Referrer-Policy": "no-referrer",
                "X-Content-Type-Options": "nosniff",
            },
        )

    def _repo_blob_oid(
        self,
        repository: "GitRepository", commit: str, path: str
    ) -> str:
        cache_key = (
            "repository-media-oid",
            repository.config.owner,
            repository.config.name.lower(),
            repository.cache_generation,
            commit,
            path,
        )
        found, cached = self._asset_cache.get(cache_key)
        if found and isinstance(cached, str):
            return cached
        self._asset_cache.backend_read()
        try:
            output = _run_git(
                repository.git_dir,
                ["ls-tree", "-z", commit, "--", ":(top,literal)" + path],
                max_output=1024,
            )
            oid = ""
            if output:
                fields = output.split(b"\t", 1)[0].split()
                if len(fields) == 3 and fields[1] == b"blob":
                    candidate = fields[2].decode("ascii", "ignore").lower()
                    if re.fullmatch(r"[0-9a-f]{40,64}", candidate):
                        oid = candidate
            # The commit is immutable and the repository generation is part of
            # the key, so ordinary absence is safe to cache too. This keeps an
            # optional logo/banner check from turning hot page loads into Git
            # disk reads.
            self._asset_cache.put(cache_key, oid, size=len(oid))
            return oid
        except (GatewayError, GitError, UnicodeError):
            return ""

    def _repo_head_commit(self, repository: "GitRepository") -> str:
        cache_key = (
            "repository-head-commit",
            repository.config.owner,
            repository.config.name.lower(),
            repository.cache_generation,
        )
        found, cached = self._asset_cache.get(cache_key)
        if found and isinstance(cached, str):
            return cached
        self._asset_cache.backend_read()
        commit = repository.resolve_commit("")
        self._asset_cache.put(cache_key, commit, size=len(commit))
        return commit

    def _repo_logo(self, owner: str, name: str) -> GatewayResponse:
        repository = self.application.repositories.get((owner, name.lower()))
        if repository is None or not repository.check_integrity():
            return json_response({"ok": False, "error": "not_found"}, 404)
        try:
            commit = self._repo_head_commit(repository)
        except (GatewayError, GitError):
            return json_response({"ok": False, "error": "not_found"}, 404)
        logo_url = ""
        for logo_path in ("logo.png", ".forkmesh/logo.png"):
            if self._repo_blob_oid(repository, commit, logo_path):
                logo_url = (
                    "/api/repo/" + owner + "/" + name + "/raw?"
                    + urlencode({"path": logo_path, "ref": commit})
                )
                break
        response = json_response({
            "ok": True,
            "repositoryId": owner + "/" + name,
            "logo": {
                "dataUrl": logo_url,
                "source": "repository" if logo_url else "none",
                "fallbackDataUrl": "",
            },
        })
        response.headers["Cache-Control"] = "public, max-age=300"
        return response

    def _repo_about(
        self, owner: str, name: str, path: str
    ) -> GatewayResponse:
        response = self._replica_response(path)
        if response is None:
            return json_response({"ok": False, "error": "not_found"}, 404)
        repository = self.application.repositories.get((owner, name.lower()))
        try:
            payload = json.loads(response.body.decode("utf-8"))
        except (AttributeError, UnicodeError, json.JSONDecodeError):
            return response
        if not isinstance(payload, dict):
            return response
        banner_url = ""
        logo_url = ""
        if repository is not None and repository.check_integrity():
            try:
                commit = self._repo_head_commit(repository)
                if self._repo_blob_oid(
                    repository, commit, ".forkmesh/banner.png"
                ):
                    banner_url = (
                        "/api/repo/" + owner + "/" + name + "/raw?"
                        + urlencode({
                            "path": ".forkmesh/banner.png",
                            "ref": commit,
                        })
                    )
                for logo_path in ("logo.png", ".forkmesh/logo.png"):
                    if self._repo_blob_oid(repository, commit, logo_path):
                        logo_url = (
                            "/api/repo/" + owner + "/" + name + "/raw?"
                            + urlencode({"path": logo_path, "ref": commit})
                        )
                        break
            except (GatewayError, GitError):
                banner_url = ""
                logo_url = ""
        # Counts and profile text may safely come from the last controller
        # projection, but media URLs must always describe this mirror's local
        # validated generation. A replica can otherwise retain an old commit
        # pin and cause a guaranteed 404 after the repository advances.
        payload["bannerUrl"] = banner_url
        payload["logoUrl"] = logo_url
        sanitized = json_response(payload)
        sanitized.headers.update(response.headers)
        return sanitized

    def _repo_read(
        self,
        method: str,
        owner: str,
        name: str,
        operation: str,
        query: str,
        headers: Mapping[str, str],
    ) -> GatewayResponse:
        # Same rewrite the routing Worker applies before proxying a repo read:
        # keep only the operation's own query fields, dropping the catalog
        # view-token and cache-version parameters the front-end appends.
        allowed = OPERATION_QUERY_FIELDS.get(operation, frozenset())
        pairs: list[tuple[str, str]] = []
        if query:
            try:
                parsed_query = parse_qs(
                    query,
                    keep_blank_values=True,
                    max_num_fields=80,
                )
            except ValueError:
                return json_response(
                    {"ok": False, "error": "not_found"}, 404
                )
            for field_name in sorted(allowed):
                for value in parsed_query.get(field_name, []):
                    pairs.append((field_name, value))
        target = (
            "/v1/public/repositories/" + owner + "/" + name + "/" + operation
        )
        if pairs:
            target += "?" + urlencode(pairs)
        response = self.application.dispatch(method, target, headers, b"")
        response.headers.setdefault(
            "X-ForkMesh-Served-By", self.config.node
        )
        return response

    def _repo_collaboration(
        self,
        owner: str,
        name: str,
        operation: str,
        headers: Mapping[str, str],
        body: bytes,
    ) -> GatewayResponse:
        normalized_headers = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        if not normalized_headers.get("content-type", "").lower().startswith(
            "application/json"
        ):
            return json_response(
                {"ok": False, "error": "unsupported_media_type"}, 415
            )
        repository = self.application.repositories.get((owner, name.lower()))
        if (
            repository is None
            or not repository.check_integrity()
            or not self.config.collaboration_control_url
        ):
            return json_response(
                {"ok": False, "error": "mirror_unavailable"}, 503
            )
        if not body or len(body) > MAX_REQUEST_BODY:
            return json_response(
                {"ok": False, "error": "invalid_json"}, 400
            )
        try:
            data = json.loads(body.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError):
            return json_response(
                {"ok": False, "error": "invalid_json"}, 400
            )
        if not isinstance(data, dict):
            return json_response(
                {"ok": False, "error": "invalid_json"}, 400
            )
        try:
            number = int(data.get("number") or 0)
        except (TypeError, ValueError):
            number = 0
        event = data.get("event")
        pull = data.get("pull")
        session = self._portable_session(headers)
        actor = str((session or {}).get("name") or "")
        request_origin = normalized_headers.get("origin", "")
        trusted_browser_origin = request_origin in {
            self.config.public_origin,
            self.config.dashboard_canonical_origin,
        }
        item: dict[str, Any]
        if operation == "issues":
            if not isinstance(event, dict) or not _verify_dashboard_issue(
                number, event, owner, name
            ):
                return json_response(
                    {"ok": False, "error": "bad_signature"}, 401
                )
            if len(str(event.get("body") or "").encode("utf-8")) > 256 * 1024:
                return json_response(
                    {"ok": False, "error": "issue_too_large"}, 413
                )
            event_type = str(event.get("type") or "")
            manager_types = {
                "edit", "title", "status", "labels", "milestone", "dates",
                "priority", "progress", "bounty", "assignees", "agent",
                "delete",
            }
            if event_type in manager_types and (
                actor != owner or not trusted_browser_origin
            ):
                return json_response(
                    {"ok": False, "error": "not_authorized"}, 403
                )
            if event_type == "claim" and (
                not trusted_browser_origin
                or actor.lower() != str(event.get("assignee") or "")
                .strip().lower()
            ):
                return json_response(
                    {"ok": False, "error": "not_authorized"}, 403
                )
            meta = data.get("meta") if isinstance(data.get("meta"), dict) else {}
            # Starting an agent is an immediate machine side effect, not a Git
            # collaboration mutation. Keep it on the controller path until the
            # agent scheduler itself is replicated.
            if meta.get("wantsAgent"):
                return json_response(
                    {"ok": False, "error": "controller_required"}, 409
                )
            event = dict(event)
            event["authorName"] = actor
            item = {
                "number": number,
                "signedNumber": number,
                "titleIfNew": str(data.get("titleIfNew") or "")[:240],
                "event": event,
                "meta": meta,
                "attachmentData": data.get("attachmentData")
                if isinstance(data.get("attachmentData"), list)
                else [],
            }
        elif operation == "pulls":
            if isinstance(event, dict):
                if not _verify_dashboard_pull_event(
                    number, event, owner, name
                ):
                    return json_response(
                        {"ok": False, "error": "bad_signature"}, 401
                    )
                if len(str(event.get("body") or "").encode("utf-8")) > 256 * 1024:
                    return json_response(
                        {"ok": False, "error": "event_too_large"}, 413
                    )
                if str(event.get("type") or "") in PULL_LIFECYCLE_EVENT_TYPES:
                    refusal = _pull_lifecycle_refusal(
                        repository, number, event, owner, actor,
                        trusted_browser_origin,
                    )
                    if refusal is not None:
                        return json_response(
                            {"ok": False, "error": refusal[0]}, refusal[1]
                        )
                event = dict(event)
                event["authorName"] = actor
                item = {"number": number, "event": event}
            elif isinstance(pull, dict) and _verify_dashboard_pull(
                pull, owner, name
            ):
                if sum(
                    len(str(pull.get(key) or "").encode("utf-8"))
                    for key in ("patch", "commits")
                ) > MAX_COMPARE_BYTES:
                    return json_response(
                        {"ok": False, "error": "pull_too_large"}, 413
                    )
                pull = dict(pull)
                pull["authorName"] = actor
                pull["description"] = str(
                    pull.get("description", pull.get("body", "")) or ""
                )[: 256 * 1024]
                pull.pop("body", None)
                item = {"number": number, "pull": pull}
            else:
                return json_response(
                    {"ok": False, "error": "bad_signature"}, 401
                )
        else:
            if not isinstance(event, dict):
                return json_response(
                    {"ok": False, "error": "event_required"}, 400
                )
            if str(event.get("type") or "") == "open":
                number = 0
            if not _verify_dashboard_discussion(number, event, owner, name):
                return json_response(
                    {"ok": False, "error": "bad_signature"}, 401
                )
            if len(str(event.get("body") or "").encode("utf-8")) > 256 * 1024:
                return json_response(
                    {"ok": False, "error": "discussion_too_large"}, 413
                )
            event = dict(event)
            event["authorName"] = actor
            item = {
                "number": number,
                "titleIfNew": str(data.get("titleIfNew") or "")[:240],
                "event": event,
            }
            if (
                str(event.get("type") or "") == "answer"
                and actor
                and actor == owner
                and trusted_browser_origin
            ):
                item["meta"] = {"ownerApproved": True}
        item["submittedAt"] = self.application.clock_ms()
        payload = json.dumps(
            {
                "owner": owner,
                "repo": name,
                "kind": operation,
                "item": item,
            },
            separators=(",", ":"),
        ).encode("utf-8")
        request = urllib_request.Request(
            self.config.collaboration_control_url,
            data=payload,
            method="POST",
            headers={
                "Accept": "application/json",
                "Content-Type": "application/json",
            },
        )
        try:
            with _open_loopback_control(request, timeout=20) as result:
                raw = result.read(64 * 1024 + 1)
                status = int(result.status)
        except urllib_error.HTTPError as error:
            raw = error.read(64 * 1024 + 1)
            status = int(error.code)
        except (OSError, urllib_error.URLError):
            return json_response(
                {"ok": False, "error": "mirror_unavailable"}, 503
            )
        if len(raw) > 64 * 1024:
            return json_response(
                {"ok": False, "error": "mirror_unavailable"}, 503
            )
        try:
            result_data = json.loads(raw.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError):
            result_data = {"ok": False, "error": "mirror_unavailable"}
        response = json_response(
            result_data if isinstance(result_data, dict) else {
                "ok": False, "error": "mirror_unavailable"
            },
            status,
        )
        response.headers["X-ForkMesh-Served-By"] = self.config.node
        return response

    def _catalog(self) -> list[dict[str, Any]]:
        repositories = [
            (key, self.application.repositories[key])
            for key in sorted(self.application.repositories)
        ]
        cache_key = tuple(
            (key, repository.cache_generation)
            for key, repository in repositories
        )
        found, cached = self._catalog_cache.get(cache_key)
        if found and isinstance(cached, list):
            return [dict(item) for item in cached]
        self._catalog_cache.backend_read()
        items: list[tuple[tuple[str, str], Any, str]] = []
        for key, repository in repositories:
            try:
                commit = repository.resolve_commit("")
            except (GatewayError, GitError):
                continue
            items.append((key, repository, commit))
        rows = [
            self._catalog_row(repository, commit)
            for _key, repository, commit in items
        ]
        self._catalog_cache.put(
            cache_key,
            [dict(item) for item in rows],
            size=len(_canonical_json(rows).encode("utf-8")),
        )
        return rows

    def _catalog_row(
        self, repository: "GitRepository", commit: str
    ) -> dict[str, Any]:
        owner = repository.config.owner
        name = repository.config.name
        branch = ""
        commit_at = 0
        branch_count = 0
        try:
            head = _run_git(
                repository.git_dir,
                ["symbolic-ref", "--quiet", "HEAD"],
                max_output=512,
                allow_exit_one=True,
            ).decode("utf-8", "ignore").strip()
            if head.startswith("refs/heads/"):
                branch = head[len("refs/heads/"):]
            stamp = _run_git(
                repository.git_dir,
                ["log", "-1", "--format=%ct", commit],
                max_output=64,
            ).decode("ascii", "ignore").strip()
            commit_at = int(stamp) * 1000 if stamp.isdigit() else 0
            heads = _run_git(
                repository.git_dir,
                ["for-each-ref", "--format=x", "refs/heads"],
                max_output=MAX_JSON_OUTPUT,
            )
            branch_count = heads.count(b"x")
        except (GatewayError, GitError):
            pass
        try:
            counts = repository._root_collaboration_counts(commit)
        except (GatewayError, GitError):
            counts = {}
        row: dict[str, Any] = {
            "owner": owner,
            "name": name,
            "visibility": "public",
            "isPrivate": False,
            "cloneUrl": (
                self.config.public_origin + "/" + owner + "/" + name + ".git"
            ),
            "mirrorUrl": (
                self.config.public_origin + "/" + owner + "/" + name + ".git"
            ),
            "dashboardUrl": (
                self.config.public_origin + "/" + owner + "/" + name
            ),
            "commit": commit,
            "branch": branch,
            "branchCount": branch_count,
            "updatedAt": commit_at,
            "lastSync": commit_at,
            "commitAt": commit_at,
            "issueCount": int(counts.get("issues", 0)),
            "pullCount": int(counts.get("pulls", 0)),
            "discussionCount": int(counts.get("discussions", 0)),
            "source": "mirror-node",
            "liveHost": self.config.node,
            "nodeId": self.config.node,
            "machineName": self.config.node,
            "runtimeMode": "headless",
        }
        if self.config.dashboard_ssh_origin:
            row["sshUrl"] = (
                self.config.dashboard_ssh_origin
                + "/" + owner + "/" + name + ".git"
            )
        return row

    def _repo_mirrors(self, owner: str, name: str) -> GatewayResponse:
        repository = self.application.repositories.get((owner, name.lower()))
        if repository is None:
            return json_response({"ok": False, "error": "not_found"}, 404)
        # The local repository remains the content authority, but this public
        # fleet summary is controller-maintained metadata. Retain and serve the
        # last validated projection so the Mirrors badge/list does not collapse
        # to "1" merely because the visitor opened an independent host. If no
        # projection has ever arrived, the truthful local-only fallback below
        # still keeps the repo usable during first boot.
        replica = self._replica_response(
            "/api/repo/" + owner + "/" + name + "/mirrors"
        )
        if replica is not None:
            return replica
        healthy = repository.check_integrity()
        now = self.application.clock_ms()
        item: dict[str, Any] = {
            "node": self.config.node,
            "owner": self.config.node,
            "repo": repository.config.name,
            "status": "online" if healthy else "verifying",
            "cloneAvailable": healthy,
            "mirrorUrl": (
                self.config.public_origin
                + "/" + owner + "/" + repository.config.name + ".git"
            ),
            "dashboardUrl": (
                self.config.public_origin
                + "/" + owner + "/" + repository.config.name
            ),
            "integrity": "ok" if healthy else "pending",
            "activity": "serving" if healthy else "verifying",
            "endpoint": self.config.public_origin,
            "endpointHealthy": healthy,
            "endpointIntegrity": "ok" if healthy else "pending",
            "endpointFresh": healthy,
            "checkedAt": now,
            "lastSeen": now,
            "runtimeMode": "headless",
            "operations": sorted(
                repository.config.operations & DIRECT_PUBLIC_READ_OPERATIONS
            ),
        }
        return json_response(
            {
                "ok": True,
                "owner": owner,
                "repo": repository.config.name,
                "generatedAt": now,
                "summary": {
                    "mirrors": 1,
                    "online": 1 if healthy else 0,
                    "cloneAvailable": 1 if healthy else 0,
                },
                "mirrors": [item],
            }
        )

    def _repo_reachability(
        self, owner: str, name: str, node: str
    ) -> GatewayResponse:
        repository = self.application.repositories.get((owner, name.lower()))
        if repository is None or node != self.config.node:
            return json_response({"ok": False, "error": "not_found"}, 404)
        healthy = repository.check_integrity()
        return json_response(
            {
                "ok": True,
                "reachable": True,
                "integrity": "ok" if healthy else "pending",
                "mirrorUrl": (
                    self.config.public_origin
                    + "/" + owner + "/" + repository.config.name + ".git"
                ),
                "checkedAt": self.application.clock_ms(),
                "latencyMs": 0,
                "statusCode": 200,
            }
        )


class GatewayApplication:
    def __init__(
        self,
        config: GatewayConfig,
        *,
        verifier: Any | None = None,
        health_signer: Any | None = None,
        materializer: ArchiveMaterializer | None = None,
        merge_executor: Any | None = None,
        clock_ms: Callable[[], int] = lambda: int(time.time() * 1000),
        log: Callable[[dict[str, Any]], None] | None = None,
    ) -> None:
        self.config = config
        self.clock_ms = clock_ms
        self.log = log or self._default_log
        # Operator policy: a gateway-owned pre-receive hook refuses pushes the
        # node's policy disallows (Git discards the staged objects), and
        # quarantine markers hide repositories from this node only.
        self.operator_policy = None
        self.receive_hooks_path = os.devnull
        if config.operator_policy_path is not None:
            assert config.operator_policy_state is not None
            try:
                self.receive_hooks_path = str(
                    node_operator_policy.install_hook(
                        config.operator_policy_state / "hooks",
                        config.operator_policy_path,
                        config.operator_policy_state,
                        config.writable_repository_root,
                    )
                )
            except (node_operator_policy.PolicyError, OSError) as exc:
                raise GatewayError(
                    "operator policy hook could not be installed") from exc
            self.operator_policy = node_operator_policy.PolicyHandle(
                config.operator_policy_path)
        self.verifier = verifier or ExternalCapabilityVerifier(
            config.verifier_command, config.router_public_key
        )
        self.health_signer = health_signer or ExternalHealthSigner(
            config.health_signer_command, config.public_key
        )
        self.merge_executor = merge_executor or (
            ExternalMergeExecutor(config.merge_executor_command)
            if config.merge_executor_command
            else None
        )
        self._materializer = materializer or ArchiveMaterializer()
        self.replays = ReplayCache()
        self.response_cache = BoundedMemoryCache(
            "repositoryResponses",
            max_entries=MEMORY_RESPONSE_CACHE_ENTRIES,
            max_bytes=MEMORY_RESPONSE_CACHE_BYTES,
            entry_overhead=MEMORY_CACHE_ENTRY_OVERHEAD,
        )
        # Cache keys being computed right now (see _single_flight).
        self._inflight_lock = threading.Lock()
        self._inflight: dict[tuple[Any, ...], threading.Event] = {}
        self.session_cache = BoundedMemoryCache(
            "portableSessions",
            max_entries=MEMORY_SESSION_CACHE_ENTRIES,
            max_bytes=MEMORY_SESSION_CACHE_BYTES,
        )
        self.service_counters = GatewayServiceCounters(
            config.config_path.parent / SERVICE_COUNTERS_FILE,
            clock_ms=clock_ms,
        )
        self._merge_lock = threading.Lock()
        # A merge completion and a daemon-requested SIGHUP can arrive at the
        # same time.  Both rebuild a complete, integrity-checked application
        # before swapping it in, so serialize only that uncommon control path;
        # ordinary repository reads continue on the current generation.
        self._reload_lock = threading.Lock()
        self._merge_threads: dict[str, threading.Thread] = {}
        self._merge_results: dict[str, dict[str, Any]] = {}
        self._merge_payload_digests: dict[str, str] = {}
        self._prewarm_lock = threading.Lock()
        self._prewarm_due: float | None = None
        self._prewarm_thread: threading.Thread | None = None
        self._retired_roots: list[
            tuple[float, tempfile.TemporaryDirectory[str]]] = []
        self._temporary_root = tempfile.TemporaryDirectory(
            prefix="forkmesh-mirror-runtime-"
        )
        os.chmod(self._temporary_root.name, 0o700)
        materializer = self._materializer
        self.repositories: dict[tuple[str, str], GitRepository] = {}
        # Restricted repositories (see the config parser): kept apart from
        # ``repositories`` so no public route, the mirror dashboard or a
        # public count can ever reach them by accident.
        self.restricted_repositories: dict[
            tuple[str, str], GitRepository] = {}
        self.quarantined_count = 0
        materialized_archives: dict[EncryptedArchive, Path | None] = {}
        for index, repository in enumerate(config.repositories):
            if (
                repository.visibility not in ("public", "restricted")
                or not repository.enabled
            ):
                continue
            try:
                git_dir = repository.git_dir
                archive = repository.encrypted_archive
                if archive is not None:
                    if archive not in materialized_archives:
                        destination = (
                            Path(self._temporary_root.name)
                            / f"repository-{index}"
                        )
                        destination.mkdir(mode=0o700)
                        try:
                            materialized_archives[archive] = (
                                materializer.materialize(
                                    archive, destination
                                )
                            )
                        except (GatewayError, GitError, OSError):
                            # One failed encrypted identity remains unavailable
                            # for every alias during this application lifetime.
                            materialized_archives[archive] = None
                            raise
                    git_dir = materialized_archives[archive]
                if git_dir is None:
                    raise GatewayError("public repository storage is missing")
                runtime = GitRepository(repository, git_dir)
                runtime.receive_hooks_path = self.receive_hooks_path
                if not runtime.check_integrity(force=True):
                    raise GatewayError("public repository integrity pin mismatch")
                target = (
                    self.restricted_repositories
                    if repository.visibility == "restricted"
                    else self.repositories
                )
                target[(repository.owner, repository.name.lower())] = runtime
                self.service_counters.ensure_repository(
                    repository.owner,
                    repository.name,
                )
            except (GatewayError, GitError, OSError):
                # Do not log the repository identity or local storage path.
                self.quarantined_count += 1

        # Repositories created by authorized first pushes under the writable
        # root. The hosted-pushes sidecar re-registers them after a restart so
        # clone/advertisement reads keep working while the refresh cycle
        # renders them into the signed configuration.
        self._writable_lock = threading.Lock()
        self.writable_repositories: dict[tuple[str, str], GitRepository] = {}
        self._load_hosted_push_repositories()

        self._manifest = config.manifest_path.read_bytes()
        self.dashboard = (
            DashboardService(self) if config.dashboard_enabled else None
        )

    def _hosted_pushes_path(self) -> Path:
        return self.config.config_path.parent / HOSTED_PUSHES_FILE

    def _load_hosted_push_repositories(self) -> None:
        root = self.config.writable_repository_root
        if root is None:
            return
        for item in _read_hosted_pushes(self._hosted_pushes_path()):
            owner = str(item.get("owner") or "").strip().lower()
            name = str(item.get("name") or "").strip()
            if not NODE_RE.fullmatch(owner) or not REPO_RE.fullmatch(name):
                continue
            key = (owner, name.lower())
            if key in self.repositories or key in self.writable_repositories:
                continue
            repository = self._open_writable_repository(owner, name)
            if repository is not None:
                self.writable_repositories[key] = repository
                self.service_counters.ensure_repository(owner, name)

    def _operator_quarantined(self, owner: str, name: str) -> bool:
        state = self.config.operator_policy_state
        return state is not None and node_operator_policy.is_quarantined(
            state, owner, name)

    def _operator_policy_refuses_new(self, owner: str, name: str) -> bool:
        """Whether this node's policy refuses creating one repository.

        An unreadable policy refuses: the node accepts nothing it cannot
        check.
        """
        if self.operator_policy is None:
            return False
        try:
            return bool(node_operator_policy.new_repository_admission(
                self.operator_policy.get(),
                self.config.writable_repository_root,
                owner,
                name,
            ))
        except (node_operator_policy.PolicyError, OSError, ValueError):
            return True

    def _operator_private_replicas_allowed(self) -> bool:
        if self.operator_policy is None:
            return True
        try:
            return bool(
                self.operator_policy.get()["seeding"]["privateReplicas"])
        except (node_operator_policy.PolicyError, OSError, ValueError):
            return False

    def _writable_owner_allowed(self, owner: str) -> bool:
        """Whether this node may create and serve pushes for one owner.

        The wildcard entry answers for every owner (see
        WRITABLE_OWNERS_WILDCARD); any other configuration is the explicit
        name list, and an empty list allows nothing.
        """
        allowlist = self.config.writable_owners
        return (
            WRITABLE_OWNERS_WILDCARD in allowlist or owner in allowlist
        )

    def _writable_git_dir(self, owner: str, name: str) -> Path | None:
        root = self.config.writable_repository_root
        if root is None:
            return None
        try:
            resolved_root = root.resolve(strict=True)
        except OSError:
            return None
        return resolved_root / owner / (name + ".git")

    def _open_writable_repository(
        self, owner: str, name: str
    ) -> GitRepository | None:
        """Open an existing writable-root repository; never creates one.

        The owner allowlist gates serving, not only creation. Removing an
        owner from ``writableOwners`` is the operator's revocation control:
        the refresh cycle immediately stops rendering that owner's
        push-created repositories, and this single choke point - shared by
        the restart-time sidecar load, plain lookups, and the reopen after a
        creation - keeps the running gateway from continuing to serve, or
        accept pushes into, a repository the configuration no longer allows.
        """
        if not self._writable_owner_allowed(owner):
            return None
        git_dir = self._writable_git_dir(owner, name)
        if git_dir is None:
            return None
        try:
            info = git_dir.lstat()
            resolved = git_dir.resolve(strict=True)
        except OSError:
            return None
        root = self.config.writable_repository_root
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or resolved != git_dir
            or root is None
            or root.resolve() not in resolved.parents
            or info.st_uid != os.geteuid()
        ):
            return None
        try:
            repository = GitRepository(
                RepositoryConfig(
                    owner=owner,
                    name=name,
                    visibility="public",
                    enabled=True,
                    git_dir=resolved,
                    release_store=None,
                    expected_refs_sha256="",
                    operations=WRITABLE_REPOSITORY_OPERATIONS,
                ),
                resolved,
            )
        except (GatewayError, GitError, OSError):
            return None
        repository.receive_hooks_path = self.receive_hooks_path
        return repository

    def _writable_repository(
        self, owner: str, name: str, *, create: bool
    ) -> GitRepository | None:
        """Resolve (and, for an authorized push, create) a writable repo.

        Creation is limited to explicitly allowlisted owners under the
        configured writable root; every request reaching this point already
        carried a verified router capability, so a receive-pack target here
        is an authorized first push.
        """
        if not NODE_RE.fullmatch(owner) or not REPO_RE.fullmatch(name):
            return None
        if self.config.writable_repository_root is None:
            return None
        key = (owner, name.lower())
        with self._writable_lock:
            existing = self.writable_repositories.get(key)
            if existing is not None:
                return existing
            repository = self._open_writable_repository(owner, name)
            if repository is None:
                if not create or not self._writable_owner_allowed(owner):
                    return None
                if self._operator_policy_refuses_new(owner, name):
                    return None
                git_dir = self._writable_git_dir(owner, name)
                if git_dir is None or git_dir.is_symlink():
                    return None
                try:
                    owner_directory = git_dir.parent
                    if owner_directory.is_symlink():
                        return None
                    owner_directory.mkdir(mode=0o700, exist_ok=True)
                    completed = subprocess.run(
                        [
                            "git",
                            "init",
                            "--bare",
                            "--initial-branch=main",
                            str(git_dir),
                        ],
                        stdin=subprocess.DEVNULL,
                        capture_output=True,
                        env=_git_environment(),
                        timeout=PROCESS_TIMEOUT_SECONDS,
                        check=False,
                    )
                    if completed.returncode != 0:
                        return None
                    os.chmod(git_dir, 0o700)
                except (OSError, subprocess.SubprocessError):
                    return None
                repository = self._open_writable_repository(owner, name)
                if repository is None:
                    return None
            self.writable_repositories[key] = repository
            self.service_counters.ensure_repository(owner, name)
            return repository

    def _record_hosted_push(self, repository: GitRepository) -> None:
        """Append/update the atomic hosted-pushes sidecar for one repo."""
        path = self._hosted_pushes_path()
        now = self.clock_ms()
        entry = {
            "owner": repository.config.owner,
            "name": repository.config.name,
            "gitDir": str(repository.git_dir),
            "firstPushedAt": now,
            "lastPushedAt": now,
        }
        with self._writable_lock:
            # An unreadable or hostile sidecar is replaced wholesale rather
            # than merged into: the accepted push is real, so the file must
            # end up naming it, and a partial or attacker-shaped carry-over
            # would be the only way a bad document could survive.
            items = _read_hosted_pushes(path)
            for item in items:
                if (
                    str(item.get("owner") or "") == entry["owner"]
                    and str(item.get("name") or "").lower()
                    == entry["name"].lower()
                ):
                    item["gitDir"] = entry["gitDir"]
                    item["lastPushedAt"] = now
                    break
            else:
                items.append(entry)
            _atomic_write_json(
                path,
                {
                    "schemaVersion": SCHEMA_VERSION,
                    "type": HOSTED_PUSHES_TYPE,
                    "repositories": items[:MAX_HOSTED_PUSH_REPOSITORIES],
                },
            )

    @staticmethod
    def _touch_refresh_notifier() -> None:
        """Best-effort wake of the packaged refresh path unit after a push."""
        try:
            parent = REFRESH_PENDING_PATH.parent
            if not parent.is_dir():
                return
            temporary = parent / (
                ".notify-" + _base64url_encode(os.urandom(9)))
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            flags |= getattr(os, "O_CLOEXEC", 0)
            flags |= getattr(os, "O_NOFOLLOW", 0)
            descriptor = os.open(temporary, flags, 0o600)
            try:
                # Byte-identical to server/packaging/ssh/ssh-refresh-notify's
                # marker, so ssh_post_receive_refresh._safe_marker accepts a
                # push-created wake exactly as it accepts an SSH one.
                os.write(descriptor, b"forkmesh-refresh-v1\n")
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            try:
                os.replace(temporary, REFRESH_PENDING_PATH)
            finally:
                try:
                    temporary.unlink()
                except FileNotFoundError:
                    pass
        except OSError:
            pass

    def close(self) -> None:
        self.service_counters.close()
        self._temporary_root.cleanup()
        for _, root in self._retired_roots:
            root.cleanup()
        self._retired_roots.clear()

    def _reap_retired_roots(self) -> None:
        now = time.monotonic()
        keep = self._retired_roots[-RETIRED_ROOTS_KEPT:]
        for retired_at, root in self._retired_roots[:-RETIRED_ROOTS_KEPT]:
            if now - retired_at < RETIRED_ROOT_GRACE_SECONDS:
                keep.insert(0, (retired_at, root))
                continue
            root.cleanup()
        keep.sort(key=lambda item: item[0])
        self._retired_roots = keep

    @staticmethod
    def _merge_request(
        owner: str,
        repository: str,
        body: bytes,
        operation: str = "merge-pull",
    ) -> dict[str, Any]:
        profile = PULL_MUTATIONS[operation]
        if not body or len(body) > MAX_MERGE_REQUEST_BODY:
            raise GatewayError("merge request is invalid")
        if profile.get("branch"):
            return GatewayApplication._branch_merge_request(
                owner, repository, body, profile)

        def reject_duplicates(
            pairs: list[tuple[str, Any]],
        ) -> dict[str, Any]:
            output: dict[str, Any] = {}
            for key, value in pairs:
                if key in output:
                    raise GatewayError("merge request is invalid")
                output[key] = value
            return output

        try:
            value = json.loads(
                body.decode("utf-8"),
                object_pairs_hook=reject_duplicates,
            )
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise GatewayError("merge request is invalid")
        expected = {
            "schemaVersion",
            "type",
            "pullNumber",
            "requestId",
            "expectedBaseOid",
            "expectedHeadOid",
            "expectedPullsOid",
        }
        if not isinstance(value, dict) or not expected <= set(value) or (
            # A repository owner may waive the executor's independent-review
            # gate; the router authorizes and audits that before signing the
            # request, so the only check owed here is the shape of the field.
            set(value) - expected - {"overrideReview", "deleteHead"}
        ):
            raise GatewayError("merge request is invalid")
        override_review = value.get("overrideReview", False)
        delete_head = value.get("deleteHead", False)
        if not isinstance(override_review, bool) or (
            override_review and operation != "merge-pull"
        ) or not isinstance(delete_head, bool) or (
            delete_head and operation != "merge-pull"
        ):
            raise GatewayError("merge request is invalid")
        try:
            pull_number = int(value.get("pullNumber"))
        except (TypeError, ValueError, OverflowError) as exc:
            raise GatewayError("merge request is invalid") from exc
        request_id = str(value.get("requestId") or "").strip()
        oids = {
            field: str(value.get(field) or "").strip().lower()
            for field in (
                "expectedBaseOid", "expectedHeadOid", "expectedPullsOid")
        }
        if (
            value.get("schemaVersion") != 1
            or value.get("type") != profile["wireType"]
            or not 1 <= pull_number <= 999_999_999
            or not REQUEST_ID_RE.fullmatch(request_id)
            or any(
                not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", oid)
                for oid in oids.values()
            )
            or len({len(oid) for oid in oids.values()}) != 1
        ):
            raise GatewayError("merge request is invalid")
        return {
            "schemaVersion": 1,
            "type": profile["executorType"],
            "owner": owner,
            "repository": repository,
            "pullNumber": pull_number,
            "requestId": request_id,
            **oids,
            # Absent unless waived, so an executor that predates the waiver
            # keeps seeing exactly the request shape it already accepts.
            **({"overrideReview": True} if override_review else {}),
            **({"deleteHead": True} if delete_head else {}),
        }

    @staticmethod
    def _branch_merge_request(
        owner: str,
        repository: str,
        body: bytes,
        profile: Mapping[str, Any],
    ) -> dict[str, Any]:
        def reject_duplicates(
            pairs: list[tuple[str, Any]],
        ) -> dict[str, Any]:
            output: dict[str, Any] = {}
            for key, value in pairs:
                if key in output:
                    raise GatewayError("merge request is invalid")
                output[key] = value
            return output

        try:
            value = json.loads(
                body.decode("utf-8"),
                object_pairs_hook=reject_duplicates,
            )
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise GatewayError("merge request is invalid")
        merging = profile["executorType"] == "forkmesh.branch-merge-executor-v1"
        branch_ok = _managed_branch if merging else _deletable_branch
        required = {
            "schemaVersion", "type", "requestId", "branch", "base",
            "expectedHeadOid",
        } | ({"expectedBaseOid"} if merging else set())
        optional = {"deleteBranch"} if merging else {"discard"}
        if not isinstance(value, dict) or not required <= set(value) or (
            set(value) - required - optional
        ):
            raise GatewayError("merge request is invalid")
        delete_branch = value.get("deleteBranch", False)
        discard = value.get("discard", False)
        request_id = str(value.get("requestId") or "").strip()
        branch = str(value.get("branch") or "").strip()
        base = str(value.get("base") or "").strip()
        oids = {
            field: str(value.get(field) or "").strip().lower()
            for field in ("expectedBaseOid", "expectedHeadOid")
            if field in required
        }
        if (
            value.get("schemaVersion") != 1
            or value.get("type") != profile["wireType"]
            or not isinstance(delete_branch, bool)
            or not isinstance(discard, bool)
            or not REQUEST_ID_RE.fullmatch(request_id)
            or not branch_ok(branch)
            or not _managed_branch(base)
            or branch == base
            or any(
                bad in name
                for name in (branch, base)
                for bad in ("..", "//", "@{", "\\")
            )
            or branch.endswith((".", "/", ".lock"))
            or base.endswith((".", "/", ".lock"))
            or any(
                not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", oid)
                for oid in oids.values()
            )
            or len({len(oid) for oid in oids.values()}) != 1
        ):
            raise GatewayError("merge request is invalid")
        return {
            "schemaVersion": 1,
            "type": profile["executorType"],
            "owner": owner,
            "repository": repository,
            "requestId": request_id,
            "branch": branch,
            "base": base,
            **oids,
            **({"deleteBranch": True} if delete_branch else {}),
            **({"discard": True} if discard else {}),
        }

    def reload_from_disk(self, *, prewarm: bool = True) -> str:
        """Validate and atomically adopt the current on-disk generation.

        ``prewarm=False`` returns as soon as the swap is complete. The live
        server uses it so the daemon's acknowledgement and every request
        waiting behind the swap are not held for the full metadata warmup
        (stats/sizes walk every blob and shortlog walks all history, which
        takes many seconds on a small node serving a large repository); the
        server warms the new generation in the background afterwards.
        """
        with self._reload_lock:
            new_config = load_config(self.config.config_path)
            if (
                new_config.node != self.config.node
                or new_config.public_key != self.config.public_key
                or new_config.router_public_key != self.config.router_public_key
                or new_config.public_origin != self.config.public_origin
                or new_config.listen_host != self.config.listen_host
                or new_config.listen_port != self.config.listen_port
                or new_config.verifier_command != self.config.verifier_command
                or new_config.health_signer_command
                != self.config.health_signer_command
                or new_config.merge_executor_command
                != self.config.merge_executor_command
                or new_config.actions_summary_path
                != self.config.actions_summary_path
                or new_config.actions_control_path
                != self.config.actions_control_path
            ):
                raise GatewayError(
                    "gateway reload changed a process-bound setting")
            replacement = GatewayApplication(
                new_config,
                verifier=self.verifier,
                health_signer=self.health_signer,
                materializer=self._materializer,
                merge_executor=self.merge_executor,
                clock_ms=self.clock_ms,
                log=self.log,
            )
            if replacement.quarantined_count or not replacement.repositories:
                replacement.close()
                raise GatewayError("repository generation is unavailable")
            # Old response bodies are bound to old repository objects. Free
            # them as part of the atomic generation swap so the next request
            # warms the new durable refs exactly once.
            self.response_cache.invalidate_all()
            old_root = self._temporary_root
            self.config = replacement.config
            self.repositories = replacement.repositories
            self.restricted_repositories = (
                replacement.restricted_repositories)
            self.quarantined_count = replacement.quarantined_count
            self._manifest = replacement._manifest
            if self.dashboard is not None:
                self.dashboard.invalidate_repository_generation()
            self._temporary_root = replacement._temporary_root
            self._retired_roots.append((time.monotonic(), old_root))
            self._reap_retired_roots()
            # The replacement loaded the same durable counter file only so its
            # newly validated repositories could be initialized. Keep this
            # live application's in-memory tally and stop the replacement's
            # timer.
            replacement.service_counters.close()
            if prewarm:
                self.prewarm_memory_caches()
            return new_config.generation_sha256

    @staticmethod
    def _merge_response(
        value: Mapping[str, Any], operation: str = "merge-pull"
    ) -> GatewayResponse:
        profile = PULL_MUTATIONS[operation]
        request_id = str(value.get("requestId") or "")
        status = str(value.get("status") or "")
        if not REQUEST_ID_RE.fullmatch(request_id):
            raise GatewayError("merge executor result is invalid")
        if status == "failed":
            error = str(value.get("error") or "")
            if error not in profile["failures"]:
                raise GatewayError("merge executor result is invalid")
            code = 404 if error == "pull_not_found" else 409
            return json_response({
                "ok": False,
                "status": "failed",
                "requestId": request_id,
                "error": error,
            }, code)
        if (
            status != profile["terminal"]
            or value.get("published") is not True
        ):
            return json_response({
                "ok": True,
                "status": "processing",
                "requestId": request_id,
            }, 202)
        output = {
            "ok": True,
            "status": profile["terminal"],
            "requestId": request_id,
            "published": True,
        }
        for result_field in profile["before"] + profile["after"]:
            oid = str(value.get(result_field) or "").lower()
            if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", oid):
                raise GatewayError("merge executor result is invalid")
            output[result_field] = oid
        if profile.get("branch"):
            branch = str(value.get("branch") or "")
            base = str(value.get("base") or "")
            branch_ok = (
                _deletable_branch if operation == "delete-agent-branch"
                else _managed_branch)
            if (
                not branch_ok(branch)
                or not _managed_branch(base)
                or not isinstance(value.get("branchDeleted", False), bool)
            ):
                raise GatewayError("merge executor result is invalid")
            output["branch"] = branch
            output["base"] = base
            output["branchDeleted"] = value.get("branchDeleted") is True
        output.update(_merge_attestation_fields(value))
        return json_response(output, 200)

    def _run_merge_job(
        self,
        request_id: str,
        payload: dict[str, Any],
        operation: str = "merge-pull",
    ) -> None:
        terminal = PULL_MUTATIONS[operation]["terminal"]
        result: dict[str, Any] = {
            "ok": True, "status": "processing", "requestId": request_id}
        if operation in BACKGROUND_MUTATIONS:
            _lower_thread_priority()
        try:
            status_payload = dict(payload, action="status")
            status = self.merge_executor.call(status_payload, timeout=30)
            if status.get("status") == "failed":
                result = status
            else:
                if not (
                    status.get("status") == terminal
                    and status.get("generationReady") is True
                ):
                    status = self.merge_executor.call(
                        dict(payload, action="execute"),
                        timeout=MERGE_EXECUTOR_TIMEOUT_SECONDS,
                    )
                if (
                    status.get("status") == terminal
                    and status.get("generationReady") is True
                ):
                    self.reload_from_disk(prewarm=False)
                    self._schedule_prewarm()
                    registered = self.merge_executor.call(
                        dict(payload, action="register"),
                        timeout=MERGE_EXECUTOR_TIMEOUT_SECONDS,
                    )
                    if registered.get("status") == "registered":
                        result = dict(status)
                        result["published"] = True
        except GatewayError:
            pass
        finally:
            with self._merge_lock:
                self._merge_results[request_id] = result
                self._merge_threads.pop(request_id, None)

    def _prewarm_after_merge(self) -> None:
        try:
            self.prewarm_memory_caches()
        except Exception:  # noqa: BLE001
            pass

    def _schedule_prewarm(self) -> None:
        with self.__dict__.setdefault("_prewarm_lock", threading.Lock()):
            self._prewarm_due = time.monotonic() + PREWARM_AFTER_MERGE_QUIET_SECONDS
            running = getattr(self, "_prewarm_thread", None)
            if running is not None and running.is_alive():
                return
            self._prewarm_thread = threading.Thread(
                target=self._prewarm_when_quiet,
                name="forkmesh-merge-prewarm",
                daemon=True,
            )
            self._prewarm_thread.start()

    def _prewarm_when_quiet(self) -> None:
        _lower_thread_priority()
        while True:
            with self._prewarm_lock:
                due = self._prewarm_due
                if due is None:
                    self._prewarm_thread = None
                    return
                wait = due - time.monotonic()
                if wait <= 0:
                    self._prewarm_due = None
            if wait > 0:
                time.sleep(min(wait, PREWARM_AFTER_MERGE_QUIET_SECONDS))
                continue
            self._prewarm_after_merge()

    def _refresh_merge_attestation(
        self,
        request_id: str,
        payload: dict[str, Any],
        result: dict[str, Any],
    ) -> dict[str, Any]:
        """Re-sign a published result's refs attestation through the executor.

        The executor re-reads its durable record and signs the refs this node
        serves *now*, so a relay that first observes the terminal result long
        after the job finished still receives an attestation it can admit.
        Best-effort: on any failure the cached result is answered unchanged.
        """
        try:
            status = self.merge_executor.call(
                dict(payload, action="status"), timeout=30)
        except GatewayError:
            return result
        if (
            status.get("status") != result.get("status")
            or status.get("requestId") != request_id
            or not _merge_attestation_fields(status)
        ):
            return result
        refreshed = dict(result)
        refreshed.update(_merge_attestation_fields(status))
        with self._merge_lock:
            if self._merge_results.get(request_id) is result:
                self._merge_results[request_id] = refreshed
        return refreshed

    def _dispatch_merge_pull(
        self,
        owner: str,
        repository: str,
        body: bytes,
        operation: str = "merge-pull",
    ) -> GatewayResponse:
        if self.merge_executor is None:
            raise GatewayError("repository route was not found")
        payload = self._merge_request(owner, repository, body, operation)
        request_id = payload["requestId"]
        payload_digest = hashlib.sha256(
            _canonical_json(payload).encode("utf-8")
        ).hexdigest()
        with self._merge_lock:
            if (
                request_id not in self._merge_payload_digests
                and len(self._merge_payload_digests) >= MAX_MERGE_JOB_CACHE
            ):
                for old_id in tuple(self._merge_payload_digests):
                    if old_id not in self._merge_threads:
                        self._merge_payload_digests.pop(old_id, None)
                        self._merge_results.pop(old_id, None)
                        break
                if len(self._merge_payload_digests) >= MAX_MERGE_JOB_CACHE:
                    raise GatewayError("merge job capacity is exhausted")
            previous_digest = self._merge_payload_digests.get(request_id)
            if (
                previous_digest
                and not hmac.compare_digest(previous_digest, payload_digest)
            ):
                raise GatewayError("merge request id was already used")
            self._merge_payload_digests[request_id] = payload_digest
            result = self._merge_results.get(request_id)
            running = request_id in self._merge_threads
            terminal = bool(result) and (
                result.get("status") == "failed"
                or result.get("published") is True
            )
            if not terminal and not running:
                thread = threading.Thread(
                    target=self._run_merge_job,
                    args=(request_id, dict(payload), operation),
                    name="forkmesh-pull-merge",
                    daemon=True,
                )
                self._merge_threads[request_id] = thread
                thread.start()
        if terminal:
            # Outside the lock: re-signing goes through the executor process
            # and must not serialize every other merge request behind it.
            if (
                result.get("published") is True
                and _merge_attestation_fields(result)
                and _merge_attestation_stale(result, self.clock_ms())
            ):
                result = self._refresh_merge_attestation(
                    request_id, dict(payload), result)
            return self._merge_response(result, operation)
        return self._merge_response({
            "ok": True,
            "status": "processing",
            "requestId": request_id,
        }, operation)

    def _actions_status(
        self, owner: str, repository: str
    ) -> GatewayResponse:
        path = self.config.actions_summary_path
        if path is None:
            raise GatewayError("repository route was not found")
        response = json_response(_load_actions_summary(
            path,
            node=self.config.node,
            owner=owner,
            repository=repository,
            now_ms=self.clock_ms(),
        ))
        response.headers["Cache-Control"] = "no-store"
        return response

    @staticmethod
    def _actions_control_request(body: bytes, now_ms: int) -> dict[str, Any]:
        """Validate one control body exactly; the executor repeats this."""

        if not body or len(body) > MAX_ACTIONS_CONTROL_BODY:
            raise GatewayError("Actions control request is invalid")

        def reject_duplicates(
            pairs: list[tuple[str, Any]],
        ) -> dict[str, Any]:
            output: dict[str, Any] = {}
            for key, value in pairs:
                if key in output:
                    raise GatewayError("Actions control request is invalid")
                output[key] = value
            return output

        try:
            value = json.loads(
                body.decode("utf-8"),
                object_pairs_hook=reject_duplicates,
            )
        except (UnicodeDecodeError, ValueError, RecursionError) as exc:
            raise GatewayError("Actions control request is invalid") from exc
        action = value.get("action") if isinstance(value, dict) else None
        if (
            not isinstance(action, str)
            or action not in ACTIONS_CONTROL_ACTIONS
            or set(value)
            != ACTIONS_CONTROL_COMMON_FIELDS | ACTIONS_CONTROL_ACTIONS[action]
        ):
            raise GatewayError("Actions control request is invalid")
        request_id = value.get("requestId")
        actor = value.get("actor")
        if (
            not _actions_bounded_integer(value.get("schemaVersion"), 1, 1)
            or value.get("type") != ACTIONS_CONTROL_WIRE_TYPE
            or not isinstance(request_id, str)
            or not ACTIONS_CONTROL_REQUEST_ID_RE.fullmatch(request_id)
            or not isinstance(actor, str)
            or not ACTIONS_CONTROL_ACTOR_RE.fullmatch(actor)
            or not _actions_bounded_integer(
                value.get("issuedAt"),
                max(0, now_ms - ACTIONS_CONTROL_CLOCK_SKEW_MS),
                now_ms + ACTIONS_CONTROL_CLOCK_SKEW_MS,
            )
        ):
            raise GatewayError("Actions control request is invalid")
        if "workflow" in value:
            workflow = value.get("workflow")
            if not isinstance(
                workflow, str
            ) or not ACTIONS_WORKFLOW_PATH_RE.fullmatch(workflow):
                raise GatewayError("Actions control request is invalid")
        if "ref" in value:
            ref = value.get("ref")
            if (
                not isinstance(ref, str)
                or not ACTIONS_CONTROL_REF_RE.fullmatch(ref)
                or ".." in ref
                or "//" in ref
                or ref.endswith(("/", ".lock"))
            ):
                raise GatewayError("Actions control request is invalid")
        if "runId" in value and not _actions_bounded_integer(
            value.get("runId"), 1, MAX_SAFE_JSON_INTEGER
        ):
            raise GatewayError("Actions control request is invalid")
        if "enabled" in value and not isinstance(value.get("enabled"), bool):
            raise GatewayError("Actions control request is invalid")
        return value

    def _actions_control_capability(
        self,
        owner: str,
        repository: str,
        target: str,
        headers: Mapping[str, str],
    ) -> dict[str, Any]:
        """The already-verified capability, as the executor will recheck it.

        ``_authorize`` accepted these header values for this exact target and
        body before the route was reached. The executor rebuilds the canonical
        message from the envelope, so the target must be the one canonical
        spelling of the route: no query, no escapes, lower-case owner.
        """
        path = (
            "/v1/repositories/" + owner + "/" + repository
            + "/actions-control"
        )
        normalized_headers = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        request_id = normalized_headers.get(
            "x-forkmesh-request-id", "").strip()
        body_hash = normalized_headers.get(
            "x-forkmesh-body-sha256", "").strip().lower()
        signature = normalized_headers.get("x-forkmesh-signature", "")
        try:
            issued_at = int(normalized_headers.get("x-forkmesh-issued-at", ""))
        except ValueError as exc:
            raise GatewayError("Actions control request is invalid") from exc
        if (
            target != path
            or not REQUEST_ID_RE.fullmatch(request_id)
            or not SHA256_RE.fullmatch(body_hash)
            or not MERGE_ATTESTATION_SIGNATURE_RE.fullmatch(signature)
            or not 0 < issued_at <= MAX_SAFE_JSON_INTEGER
        ):
            raise GatewayError("Actions control request is invalid")
        return {
            "method": "POST",
            "path": path,
            "requestId": request_id,
            "issuedAt": issued_at,
            "bodySha256": body_hash,
            "signature": signature,
        }

    @staticmethod
    def _actions_control_existing(
        directory: int, file_name: str
    ) -> dict[str, Any] | None:
        """Read one pending envelope without following a link, if present."""

        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(file_name, flags, dir_fd=directory)
        except FileNotFoundError:
            return None
        except OSError as exc:
            raise GatewayError("control request id was already used") from exc
        try:
            info = os.fstat(descriptor)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_nlink != 1
                or not 0 < info.st_size <= MAX_ACTIONS_CONTROL_ENVELOPE_BYTES
            ):
                raise GatewayError("control request id was already used")
            raw = os.read(descriptor, MAX_ACTIONS_CONTROL_ENVELOPE_BYTES + 1)
        except OSError as exc:
            raise GatewayError("control request id was already used") from exc
        finally:
            os.close(descriptor)
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError, RecursionError) as exc:
            raise GatewayError("control request id was already used") from exc
        if not isinstance(value, dict):
            raise GatewayError("control request id was already used")
        return value

    def _actions_control(
        self,
        owner: str,
        repository: str,
        body: bytes,
        capability: Mapping[str, Any],
    ) -> GatewayResponse:
        """Courier one router-signed control request to the executor."""

        control_path = self.config.actions_control_path
        summary_path = self.config.actions_summary_path
        if control_path is None or summary_path is None:
            raise GatewayError("repository route was not found")
        now_ms = self.clock_ms()
        request = self._actions_control_request(body, now_ms)
        request_id = str(request["requestId"])
        # Nobody consumes the queue while the executor is offline, and a
        # version 1 executor predates control requests altogether.
        try:
            summary = _load_actions_summary(
                summary_path,
                node=self.config.node,
                owner=owner,
                repository=repository,
                now_ms=now_ms,
            )
        except GatewayError as exc:
            raise GatewayError("Actions executor is unavailable") from exc
        if summary.get("schemaVersion") != 2:
            raise GatewayError("Actions executor is unavailable")
        body_text = body.decode("utf-8")
        envelope = json.dumps(
            {
                "schemaVersion": 1,
                "type": ACTIONS_CONTROL_ENVELOPE_TYPE,
                "node": self.config.node,
                "owner": owner,
                "repository": repository,
                "receivedAt": now_ms,
                "capability": dict(capability),
                # Verbatim: the executor hashes these exact bytes against the
                # signed bodySha256 before it parses them.
                "body": body_text,
            },
            separators=(",", ":"),
        ).encode("utf-8") + b"\n"
        if len(envelope) > MAX_ACTIONS_CONTROL_ENVELOPE_BYTES:
            raise GatewayError("Actions control request is invalid")
        file_name = request_id + ".json"
        temporary_name = (
            "." + request_id + "." + os.urandom(8).hex() + ".tmp")
        directory = -1
        descriptor = -1
        created = False
        with _ACTIONS_CONTROL_LOCK:
            try:
                if (
                    not control_path.is_absolute()
                    or control_path
                    != Path(os.path.normpath(str(control_path)))
                    or control_path.name != ACTIONS_CONTROL_DIRECTORY
                    or control_path.resolve(strict=True) != control_path
                ):
                    raise GatewayError("Actions control queue is unavailable")
                directory = os.open(
                    control_path,
                    os.O_RDONLY
                    | getattr(os, "O_CLOEXEC", 0)
                    | getattr(os, "O_DIRECTORY", 0)
                    | getattr(os, "O_NOFOLLOW", 0),
                )
                if not _actions_parent_metadata_allowed(
                    os.fstat(directory),
                    effective_uid=os.geteuid(),
                    effective_gid=os.getegid(),
                ):
                    raise GatewayError("Actions control queue is unavailable")
                # Expired first: an executor that stopped consuming must not
                # leave the queue permanently full, and an abandoned temporary
                # file must not outlive the request that made it.
                pending = 0
                for entry in os.listdir(directory):
                    is_envelope = entry.endswith(".json")
                    if not is_envelope and not (
                        entry.startswith(".") and entry.endswith(".tmp")
                    ):
                        continue
                    try:
                        info = os.stat(
                            entry, dir_fd=directory, follow_symlinks=False)
                    except FileNotFoundError:
                        continue
                    age_ms = now_ms - info.st_mtime_ns // 1_000_000
                    if (
                        not stat.S_ISDIR(info.st_mode)
                        and abs(age_ms) > ACTIONS_CONTROL_EXPIRY_MS
                    ):
                        try:
                            os.unlink(entry, dir_fd=directory)
                        except FileNotFoundError:
                            pass
                        continue
                    if is_envelope:
                        pending += 1
                existing = self._actions_control_existing(
                    directory, file_name)
                if existing is not None:
                    if (
                        existing.get("body") != body_text
                        or existing.get("owner") != owner
                        or existing.get("repository") != repository
                    ):
                        raise GatewayError(
                            "control request id was already used")
                elif pending >= MAX_ACTIONS_CONTROL_PENDING:
                    raise GatewayError("control queue is full")
                else:
                    descriptor = os.open(
                        temporary_name,
                        os.O_WRONLY
                        | os.O_CREAT
                        | os.O_EXCL
                        | getattr(os, "O_CLOEXEC", 0)
                        | getattr(os, "O_NOFOLLOW", 0),
                        0o640,
                        dir_fd=directory,
                    )
                    created = True
                    # The service umask must not narrow the handoff mode.
                    os.fchmod(descriptor, 0o640)
                    view = memoryview(envelope)
                    while view:
                        view = view[os.write(descriptor, view):]
                    # Expiry follows the gateway clock, not the filesystem's.
                    os.utime(
                        descriptor,
                        ns=(now_ms * 1_000_000, now_ms * 1_000_000),
                    )
                    os.fsync(descriptor)
                    os.close(descriptor)
                    descriptor = -1
                    os.rename(
                        temporary_name,
                        file_name,
                        src_dir_fd=directory,
                        dst_dir_fd=directory,
                    )
                    created = False
                    os.fsync(directory)
            except OSError as exc:
                raise GatewayError(
                    "Actions control queue is unavailable") from exc
            finally:
                if descriptor >= 0:
                    os.close(descriptor)
                if created:
                    try:
                        os.unlink(temporary_name, dir_fd=directory)
                    except OSError:
                        pass
                if directory >= 0:
                    os.close(directory)
        response = json_response({
            "ok": True,
            "status": "accepted",
            "requestId": request_id,
            "node": self.config.node,
        }, 202)
        response.headers["Cache-Control"] = "no-store"
        return response

    @staticmethod
    def _default_log(event: dict[str, Any]) -> None:
        print(_canonical_json(event), flush=True)

    def _record(
        self,
        *,
        request_id: str,
        operation: str,
        status: int,
        started: float,
        byte_count: int,
    ) -> None:
        safe_request_id = (
            request_id if REQUEST_ID_RE.fullmatch(request_id) else ""
        )
        self.log(
            {
                "time": datetime.now(timezone.utc)
                .isoformat()
                .replace("+00:00", "Z"),
                "requestId": safe_request_id,
                "operation": operation
                if operation in LOG_OPERATIONS
                else "control",
                "status": int(status),
                "durationMs": max(0, int((time.monotonic() - started) * 1000)),
                "bytes": max(0, int(byte_count)),
            }
        )

    def record_service_counter(
        self,
        target: str,
        operation: str,
        status: int,
        *,
        delivered: bool,
        user_agent: str = "",
    ) -> bool:
        if not delivered or not 200 <= int(status) < 300:
            return False
        try:
            path = urlsplit(target).path
            direct = _public_git_route(path)
            if direct is not None:
                owner, repository, route_operation = direct
            else:
                pieces = path.split("/")
                if (
                    len(pieces) != 6
                    or pieces[1:3] != ["v1", "repositories"]
                ):
                    return False
                owner = unquote(pieces[3], errors="strict").lower()
                repository = unquote(pieces[4], errors="strict")
                route_operation = unquote(pieces[5], errors="strict")
        except (UnicodeError, ValueError):
            return False
        if operation != route_operation:
            return False
        return self.service_counters.record(
            owner,
            repository,
            operation,
            user_agent,
        )

    def _capability_from_headers(
        self,
        method: str,
        target: str,
        headers: Mapping[str, str],
    ) -> tuple[str, str, int, int]:
        """Verify everything a capability states about itself, body aside.

        Returns ``(request_id, body_hash, issued_at, now)``. The replay claim
        and the body-hash comparison stay with ``_authorize``; this half is
        also what decides, before any body is read, whether a receive-pack
        request may carry a pack above ``MAX_REQUEST_BODY``.
        """
        normalized_headers = {str(k).lower(): str(v) for k, v in headers.items()}
        node = normalized_headers.get("x-forkmesh-node", "")
        request_id = normalized_headers.get("x-forkmesh-request-id", "")
        body_hash = normalized_headers.get("x-forkmesh-body-sha256", "").lower()
        signature = normalized_headers.get("x-forkmesh-signature", "")
        try:
            issued_at = int(normalized_headers.get("x-forkmesh-issued-at", "0"))
        except ValueError:
            raise GatewayError("request capability is invalid")
        now = self.clock_ms()
        if (
            node != self.config.node
            or abs(now - issued_at) > CAPABILITY_WINDOW_MS
        ):
            raise GatewayError("request capability is invalid")
        message = request_message(
            node, method, target, body_hash, request_id, issued_at
        )
        if not message or not self.verifier.verify(message, signature):
            raise GatewayError("request capability is invalid")
        return request_id, body_hash, issued_at, now

    def _run_receive_pack(
        self,
        repository: "GitRepository",
        owner: str,
        name: str,
        decoded_body: bytes,
        actions: bool = False,
    ) -> GatewayResponse:
        """Run one authorized receive-pack and publish its side effects."""
        if len(decoded_body) > self.config.receive_max_input_bytes:
            return json_response(
                {"ok": False, "error": "request_too_large"}, 413)
        output, changes = repository.receive_pack_transitions(
            decoded_body, self.config.receive_max_input_bytes
        )
        refs_changed = bool(changes)
        headers = {"Cache-Control": "no-store"}
        if changes:
            updates = []
            for ref, old, new in changes[:REF_UPDATES_MAX]:
                update: dict[str, Any] = {"ref": ref, "old": old, "new": new}
                if ref.startswith("refs/heads/"):
                    update["commits"] = repository.new_commit_count(old, new)
                updates.append(update)
            report: dict[str, Any] = {
                "total": len(changes), "updates": updates}
            if actions:
                report["actions"] = True
            headers[REF_UPDATES_HEADER] = json.dumps(
                report,
                ensure_ascii=True, separators=(",", ":"), sort_keys=True)
            entry: dict[str, Any] = {
                "time": datetime.now(timezone.utc)
                .isoformat()
                .replace("+00:00", "Z"),
                "operation": "git-receive-pack",
                "status": 200,
                "refsUpdated": len(changes),
            }
            if actions:
                entry["actions"] = True
            self.log(entry)
        if refs_changed:
            repository.invalidate_memory_generation()
            self.response_cache.invalidate_all()
            if self.dashboard is not None:
                self.dashboard.invalidate_repository_generation()
            key = (owner, name.lower())
            if key in self.writable_repositories:
                self._record_hosted_push(repository)
            # Wake the refresh/registration cycle either way: a
            # configured repository needs its integrity pin and
            # catalog state re-rendered after an accepted push.
            self._touch_refresh_notifier()
        return GatewayResponse(
            200,
            "application/x-git-receive-pack-result",
            output,
            headers,
        )

    def _runner_grant(
        self,
        headers: Mapping[str, str],
        owner: str,
        repository: str,
        access: str | tuple[str, ...],
    ) -> RunnerGrant:
        """Verify a runner grant for one repository and access level."""
        normalized = {str(k).lower(): str(v) for k, v in headers.items()}
        grant = parse_runner_grant(normalized.get(RUNNER_GRANT_HEADER, ""))
        now = self.clock_ms()
        accepted = (access,) if isinstance(access, str) else tuple(access)
        if (
            grant.node != self.config.node
            or grant.owner != owner.lower()
            or grant.repository.lower() != repository.lower()
            or grant.access not in accepted
            or now < grant.issued_at - CAPABILITY_WINDOW_MS
            or now > grant.expires_at
        ):
            raise GatewayError("runner grant is invalid")
        if not self.verifier.verify(grant.message, grant.signature):
            raise GatewayError("runner grant is invalid")
        return grant

    def _runner_repository(self, owner: str, name: str) -> "GitRepository":
        """A configured, enabled repository; never one created on demand."""
        if self._operator_quarantined(owner, name):
            raise GatewayError("repository route was not found")
        repository = self.repositories.get((owner, name.lower()))
        if repository is None:
            # A grant is a router capability, so it reaches restricted
            # repositories too (core replicas fetch hosted private ones).
            repository = self.restricted_repositories.get(
                (owner, name.lower()))
        if (
            repository is None
            or not repository.config.enabled
            or repository.git_dir is None
            or not repository.check_integrity()
        ):
            raise GatewayError("repository route was not found")
        return repository

    def _dispatch_runner_git(
        self,
        method: str,
        raw_query: str,
        headers: Mapping[str, str],
        body: bytes,
        route: tuple[str, str, str],
    ) -> GatewayResponse:
        """Native git smart-HTTP for one runner grant (see RUNNER_GRANT_*).

        A read or actions-push grant serves info/refs + upload-pack. A push
        grant serves the receive-pack advertisement and a receive-pack whose
        every command updates exactly the grant's ref, forward only; the
        command section is checked before git runs, so a refused push changes
        nothing.
        """
        owner, name, route_operation = route
        normalized = {str(k).lower(): str(v) for k, v in headers.items()}
        git_protocol = normalized.get("git-protocol", "").strip()
        if git_protocol not in ("", "version=1", "version=2"):
            raise GatewayError("Git protocol is not available")
        content_type = (
            normalized.get("content-type", "").split(";", 1)[0].strip())
        if route_operation == "info-refs":
            query = self._parse_query(raw_query, frozenset({"service"}))
            service = query.get("service", "")
            if method not in ("GET", "HEAD") or service not in (
                "git-upload-pack", "git-receive-pack"
            ):
                raise GatewayError("Git service is not available")
            reading = service == "git-upload-pack"
            self._runner_grant(
                headers, owner, name,
                RUNNER_READ_ACCESS if reading else RUNNER_PUSH_ACCESS)
            repository = self._runner_repository(owner, name)
            if reading:
                return GatewayResponse(
                    200,
                    "application/x-git-upload-pack-advertisement",
                    repository.git_advertisement(git_protocol),
                    {"Cache-Control": "no-store"},
                )
            return GatewayResponse(
                200,
                "application/x-git-receive-pack-advertisement",
                repository.receive_pack_advertisement(),
                {"Cache-Control": "no-store"},
            )
        if method != "POST" or raw_query:
            raise GatewayError("Git service is not available")
        if route_operation == "git-upload-pack":
            if content_type != "application/x-git-upload-pack-request":
                raise GatewayError("Git service is not available")
            self._runner_grant(headers, owner, name, RUNNER_READ_ACCESS)
            repository = self._runner_repository(owner, name)
            decoded_body = _decode_git_body(
                body, normalized.get("content-encoding", ""))
            return GatewayResponse(
                200,
                "application/x-git-upload-pack-result",
                stream=repository.upload_pack_spec(decoded_body, git_protocol),
            )
        if content_type != "application/x-git-receive-pack-request":
            raise GatewayError("Git service is not available")
        grant = self._runner_grant(headers, owner, name, RUNNER_PUSH_ACCESS)
        repository = self._runner_repository(owner, name)
        decoded_body = _decode_git_body(
            body,
            normalized.get("content-encoding", ""),
            max(MAX_REQUEST_BODY, self.config.receive_max_input_bytes),
        )
        actions = grant.access == "actions-push"
        current = None
        if actions:
            current = stable_refs(repository.git_dir).get(grant.ref, "")
        enforce_runner_push_scope(
            receive_pack_commands(decoded_body), grant.ref, grant.access,
            current)
        return self._run_receive_pack(
            repository, owner, repository.config.name, decoded_body, actions)

    def request_body_limit(
        self, method: str, target: str, headers: Mapping[str, str]
    ) -> int:
        """Bytes one request may carry, decided from its headers alone.

        Only a receive-pack POST whose router capability already verifies may
        exceed the base cap, up to the configured receive.maxInputSize; an
        unsigned or mis-signed request never makes the gateway hold more than
        ``MAX_REQUEST_BODY`` before it is refused.
        """
        runner_route = _runner_git_route(urlsplit(target).path)
        if (
            runner_route is not None
            and runner_route[2] == "git-receive-pack"
            and str(method or "").upper() == "POST"
        ):
            try:
                self._runner_grant(
                    headers, runner_route[0], runner_route[1],
                    RUNNER_PUSH_ACCESS)
            except GatewayError:
                return MAX_REQUEST_BODY
            return max(MAX_REQUEST_BODY, self.config.receive_max_input_bytes)
        if not _receive_pack_target(method, urlsplit(target).path):
            return MAX_REQUEST_BODY
        try:
            self._capability_from_headers(method, target, headers)
        except GatewayError:
            return MAX_REQUEST_BODY
        return max(MAX_REQUEST_BODY, self.config.receive_max_input_bytes)

    def _authorize(
        self,
        method: str,
        target: str,
        headers: Mapping[str, str],
        body: bytes,
    ) -> str:
        request_id, body_hash, issued_at, now = self._capability_from_headers(
            method, target, headers
        )
        # A pack the Worker streamed rather than held is bound to the fixed
        # streamed-body digest instead of its own; every other capability
        # must name the exact bytes it accompanies.
        streamed = (
            _receive_pack_target(method, urlsplit(target).path)
            and hmac.compare_digest(body_hash, STREAMED_REQUEST_BODY_SHA256)
        )
        if not streamed and not hmac.compare_digest(
            body_hash, hashlib.sha256(body).hexdigest()
        ):
            raise GatewayError("request capability is invalid")
        if not self.replays.claim(
            request_id, issued_at + CAPABILITY_WINDOW_MS, now
        ):
            raise GatewayError("request capability was already used")
        return request_id

    @staticmethod
    def _parse_query(
        raw_query: str, allowed: frozenset[str]
    ) -> dict[str, str]:
        try:
            parsed = parse_qs(
                raw_query,
                keep_blank_values=True,
                strict_parsing=bool(raw_query),
                max_num_fields=8,
            )
        except ValueError as exc:
            raise GatewayError("request query is invalid") from exc
        if any(key not in allowed or len(values) != 1 for key, values in parsed.items()):
            raise GatewayError("request query is invalid")
        return {key: values[0] for key, values in parsed.items()}

    def _compare(
        self, repository: GitRepository, query: Mapping[str, str]
    ) -> dict[str, Any]:
        if "cherry" not in query and "limit" not in query:
            return repository.compare(query)
        owner, _, name = str(query.get("cherry") or "").partition("/")
        owner = owner.lower()
        if (
            not NODE_RE.fullmatch(owner)
            or not REPO_RE.fullmatch(name)
            or self._operator_quarantined(owner, name)
        ):
            raise GatewayError("upstream repository was not found")
        upstream = self.repositories.get((owner, name.lower()))
        if (
            upstream is None
            or not _repository_serves(upstream, "compare")
            or not upstream.check_integrity()
        ):
            raise GatewayError("upstream repository was not found")
        return repository.cherry(query, upstream.git_dir)

    def _merge_preview(
        self, repository: GitRepository, query: Mapping[str, str]
    ) -> dict[str, Any]:
        preview = repository.merge_preview(query)
        if query.get("rules") != "1":
            return preview
        pull = str(query.get("pull") or "")
        if not PULL_RULES_NUMBER_RE.fullmatch(pull):
            raise GatewayError("pull rules need a pull request number")
        org_rules = _decode_org_pull_rules(str(query.get("orgRules") or ""))
        preview["rules"] = {"available": False, "rules": []}
        if self.merge_executor is None:
            return preview
        request: dict[str, Any] = {
            "schemaVersion": 1,
            "type": PULL_RULES_EVALUATE_TYPE,
            "owner": repository.config.owner,
            "repository": repository.config.name,
            "base": preview["baseOid"],
            "head": preview["headOid"],
            "pullNumber": int(pull),
        }
        if org_rules:
            request["orgRules"] = org_rules
        try:
            response = self.merge_executor.call(
                request, timeout=60, max_bytes=MAX_PULL_RULES_RESPONSE_BYTES)
        except GatewayError:
            return preview
        rules = response.get("rules")
        if response.get("ok") is True and isinstance(rules, dict):
            preview["rules"] = rules
        return preview

    @staticmethod
    def _repository_response_cache_key(
        repository: GitRepository,
        operation: str,
        query: Mapping[str, Any],
        variant: str = "",
    ) -> tuple[Any, ...]:
        return (
            repository.cache_generation,
            operation,
            _canonical_json(dict(query)),
            variant,
        )

    @staticmethod
    def _commit_keyed_query(
        repository: GitRepository,
        operation: str,
        query: Mapping[str, Any],
    ) -> Mapping[str, Any]:
        """Cache-key form of a read query: the ref as the commit it names.

        The web UI asks for ``tree?path=&ref=main`` while the warmup (and a
        ref-less early fetch) ask for ``tree`` with no query at all; keyed
        literally, the warmed root tree was never the one visitors read, and
        after every push the first visitor paid the whole cold listing.
        """
        if operation not in COMMIT_KEYED_OPERATIONS or (
            operation == "history" and query.get("all")
        ):
            return query
        normalized = dict(query)
        if operation == "tree" and not normalized.get("path"):
            normalized.pop("path", None)
        try:
            normalized["ref"] = repository.cache_ref(
                normalized.get("ref", ""))
        except (GatewayError, GitError):
            return query
        return normalized

    def _single_flight(
        self,
        key: tuple[Any, ...],
        compute: Any,
    ) -> GatewayResponse:
        """Compute one cacheable read once for all concurrent misses.

        A page view asks for the root tree from two places, and the warmup
        thread builds the same listing; on one vCPU three concurrent cold
        builds each took three times as long. Followers wait for the leader
        and then read its cached answer (or compute it themselves when the
        leader failed or its answer was not cacheable).
        """
        while True:
            with self._inflight_lock:
                event = self._inflight.get(key)
                if event is None:
                    event = threading.Event()
                    self._inflight[key] = event
                    break
            event.wait(PROCESS_TIMEOUT_SECONDS)
            cached = self._cached_repository_response(key)
            if cached is not None:
                return cached
        try:
            self.response_cache.backend_read()
            return self._remember_repository_response(key, compute())
        finally:
            with self._inflight_lock:
                self._inflight.pop(key, None)
            event.set()

    def _cached_repository_response(
        self, key: tuple[Any, ...]
    ) -> GatewayResponse | None:
        found, value = self.response_cache.get(key)
        if not found or not isinstance(value, GatewayResponse):
            return None
        response = _copy_cached_response(value)
        response.headers["X-ForkMesh-Memory-Cache"] = "hit"
        return response

    def _remember_repository_response(
        self, key: tuple[Any, ...], response: GatewayResponse
    ) -> GatewayResponse:
        if (
            response.stream is None
            and response.status == 200
            and response.headers.get(PULSE_PARTIAL_HEADER) != "1"
            and len(response.body) <= MEMORY_RESPONSE_CACHE_OBJECT_BYTES
        ):
            stored = _copy_cached_response(response)
            stored.headers.pop("X-ForkMesh-Memory-Cache", None)
            self.response_cache.put(key, stored, size=len(stored.body))
        response.headers["X-ForkMesh-Memory-Cache"] = "miss"
        return response

    def cache_diagnostics(self) -> dict[str, Any]:
        dashboard_assets = (
            self.dashboard._asset_cache.diagnostics()
            if self.dashboard is not None else {}
        )
        repository_catalog = (
            self.dashboard._catalog_cache.diagnostics()
            if self.dashboard is not None else {}
        )
        replica = (
            self.dashboard._replica.diagnostics()
            if self.dashboard is not None and self.dashboard._replica is not None
            else {
                "loaded": False,
                "documents": 0,
                "bytes": 0,
                "hits": 0,
                "misses": 0,
                "reloads": 0,
                "invalidations": 0,
                "diskReads": 0,
                "diskReadsAvoided": 0,
                "ageSeconds": 0,
            }
        )
        return {
            "repositoryResponses": self.response_cache.diagnostics(),
            "dashboardAssets": dashboard_assets,
            "repositoryCatalog": repository_catalog,
            "portableSessions": self.session_cache.diagnostics(),
            "dashboardReplica": replica,
        }

    def invalidate_projection_cache(self) -> None:
        if self.dashboard is not None:
            self.dashboard.invalidate_replica()

    def prewarm_memory_caches(self) -> None:
        """Prime bounded caches once for the generation about to serve.

        Projection documents are loaded by ``ReplicaStore`` itself. This pass
        covers the other predictable hot paths: the dashboard shell/assets,
        repository catalog, root repository views, counts, and Smart HTTP ref
        advertisements. Parameterized searches, blobs, diffs, and private or
        streaming bodies remain demand-loaded by design.
        """
        if self.dashboard is not None:
            try:
                self.dashboard._catalog()
            except (GatewayError, GitError, OSError):
                pass
            asset_paths = set(DASHBOARD_ASSET_ROUTES.values()) | {
                DASHBOARD_PAGE_ROOT + "/index.html",
                DASHBOARD_REPO_ASSET,
                DASHBOARD_404_ASSET,
            }
            for path in sorted(asset_paths):
                try:
                    self.dashboard._asset_blob(path)
                except (GatewayError, GitError, OSError):
                    continue

        warm_reads = {
            "tree": GitRepository.tree,
            "history": GitRepository.history,
            "branches": GitRepository.branches,
            "stats": GitRepository.stats,
            "sizes": GitRepository.sizes,
        }
        repositories = list(self.repositories.values()) + list(
            self.writable_repositories.values()
        )
        for repository in repositories:
            if not repository.check_integrity():
                continue
            if os.environ.get("FORKMESH_GATEWAY_COMMIT_GRAPH", "1") != "0":
                repository.write_commit_graph()
            for operation, function in warm_reads.items():
                if operation not in repository.config.operations:
                    continue
                key = self._repository_response_cache_key(
                    repository, operation,
                    self._commit_keyed_query(repository, operation, {}))
                found, _cached = self.response_cache.get(key)
                if found:
                    continue
                try:
                    self._single_flight(
                        key,
                        lambda function=function, repository=repository:
                        json_response(function(repository, {})))
                except (GatewayError, GitError, OSError):
                    continue
            if "git-info-refs" not in repository.config.operations:
                continue
            for protocol in ("", "version=2"):
                query = {"service": "git-upload-pack"}
                key = self._repository_response_cache_key(
                    repository, "git-info-refs", query, protocol)
                found, _cached = self.response_cache.get(key)
                if found:
                    continue
                try:
                    self.response_cache.backend_read()
                    self._remember_repository_response(
                        key,
                        GatewayResponse(
                            200,
                            "application/x-git-upload-pack-advertisement",
                            repository.git_advertisement(protocol),
                            {"Cache-Control": "no-store"},
                        ),
                    )
                except (GatewayError, GitError, OSError):
                    continue

    def _health(
        self, query_text: str, headers: Mapping[str, str]
    ) -> GatewayResponse:
        query = self._parse_query(
            query_text, frozenset({"nonce", "issuedAt", "owner", "repo"})
        )
        response: dict[str, Any] = {
            "ok": self.quarantined_count == 0,
            "type": "forkmesh.mirror-health",
            "schemaVersion": 1,
            "node": self.config.node,
            "publicKey": self.config.public_key,
            "transport": "direct-https",
            "integrity": "ok" if self.quarantined_count == 0 else "degraded",
            # The gateway exposes canonical and node-owner aliases for the
            # same bare repository. Report physical repositories, not route
            # aliases, so fleet UIs do not claim one mirror stores two repos.
            "publicRepositoryCount": len({
                repository.git_dir for repository in self.repositories.values()
            }),
            "quarantinedRepositoryCount": self.quarantined_count,
            "checkedAt": self.clock_ms(),
            "cache": self.cache_diagnostics(),
        }
        if query:
            if set(query) not in (
                {"nonce", "issuedAt"},
                {"nonce", "issuedAt", "owner", "repo"},
            ):
                raise GatewayError("health challenge is incomplete")
            try:
                issued_at = int(query["issuedAt"])
            except ValueError as exc:
                raise GatewayError("health challenge is invalid") from exc
            if abs(self.clock_ms() - issued_at) > CAPABILITY_WINDOW_MS:
                raise GatewayError("health challenge is stale")
            message_type = "forkmesh-https-health-v1"
            if "owner" in query:
                owner = query["owner"].strip().lower()
                repository_name = query["repo"].strip()
                if (
                    not NODE_RE.fullmatch(owner)
                    or not REPO_RE.fullmatch(repository_name)
                ):
                    raise GatewayError("health repository request is invalid")
                repository = self.repositories.get(
                    (owner, repository_name.lower())
                )
                if repository is None:
                    # The proof names only availability and a refs digest; the
                    # routing Worker needs it to admit a node for a restricted
                    # repository it is about to reach with a capability.
                    repository = self.restricted_repositories.get(
                        (owner, repository_name.lower()))
                if repository is None:
                    repository = self._writable_repository(
                        owner, repository_name, create=False)
                available = bool(
                    repository is not None
                    and not self._operator_quarantined(owner, repository_name)
                    and repository.check_integrity(force=True)
                )
                empty_digest = hashlib.sha256(b"").hexdigest()
                refs_digest = (
                    routing_refs_sha256(repository.git_dir)
                    if available and repository is not None
                    else empty_digest
                )
                operations = (
                    sorted(repository.config.operations)
                    if available and repository is not None
                    else []
                )
                operations_digest = hashlib.sha256(
                    "\n".join(operations).encode("utf-8")
                ).hexdigest()
                integrity = "ok" if available else "unavailable"
                response["repositoryProof"] = {
                    "owner": owner,
                    "repository": repository_name,
                    "available": available,
                    "integrity": integrity,
                    "refsSha256": refs_digest,
                    "operations": operations,
                    "operationsSha256": operations_digest,
                }
                message = repository_health_challenge(
                    self.config.node,
                    query["nonce"],
                    issued_at,
                    owner,
                    repository_name,
                    available=available,
                    integrity=integrity,
                    refs_digest=refs_digest,
                    operations_digest=operations_digest,
                )
                message_type = "forkmesh-https-health-repository-v1"
            else:
                message = health_challenge(
                    self.config.node, query["nonce"], issued_at
                )
            if not message:
                raise GatewayError("health challenge is invalid")
            response["challenge"] = {
                "messageType": message_type,
                "nonce": query["nonce"],
                "issuedAt": issued_at,
                "algorithm": "Ed25519",
                "encoding": "base64url-no-padding",
                "messageSha256": hashlib.sha256(
                    message.encode("utf-8")
                ).hexdigest(),
                "signature": self.health_signer.sign(message),
            }
        result = json_response(response, 200 if response["ok"] else 503)
        normalized_headers = {
            str(key).lower(): str(value) for key, value in headers.items()
        }
        cors_origin = normalized_headers.get("origin", "")
        if not query and cors_origin:
            # This unsigned document is public and contains no credentials.
            # Mirrors diagnose peers directly, so any page origin may read it;
            # signed challenges and capability routes retain no CORS access.
            result.headers["Access-Control-Allow-Origin"] = "*"
        return result

    def _node_logs(
        self, method: str, raw_query: str, headers: Mapping[str, str]
    ) -> GatewayResponse:
        """Open this host's journal as an event stream for a listed viewer.

        Every refusal is the same 404, so the route says nothing about which
        keys are listed or whether the host offers a feed at all.
        """
        missing = json_response({"ok": False, "error": "not_found"}, 404)
        if method != "GET" or raw_query:
            return missing
        normalized = {str(k).lower(): str(v) for k, v in headers.items()}
        viewer = normalized.get("x-forkmesh-log-viewer", "")
        request_id = normalized.get("x-forkmesh-request-id", "")
        signature = normalized.get("x-forkmesh-signature", "")
        try:
            issued_at = int(normalized.get("x-forkmesh-issued-at", "0"))
        except ValueError:
            return missing
        now = self.clock_ms()
        if abs(now - issued_at) > CAPABILITY_WINDOW_MS:
            return missing
        if viewer not in load_log_viewer_keys():
            return missing
        message = node_logs_message(self.config.node, viewer, request_id, issued_at)
        if not message or not _verify_ed25519(viewer, message, signature):
            return missing
        if not self.replays.claim(request_id, issued_at + CAPABILITY_WINDOW_MS, now):
            return missing
        return GatewayResponse(
            200,
            "text/event-stream; charset=utf-8",
            stream=StreamSpec(
                kind="node-logs",
                content_type="text/event-stream; charset=utf-8",
                content_length=None,
                command=(
                    "journalctl",
                    "--no-pager",
                    "-q",
                    "-n",
                    str(NODE_LOG_BACKLOG_LINES),
                    "-f",
                    "-o",
                    "short-iso",
                ),
            ),
        )

    def dispatch(
        self,
        method: str,
        target: str,
        headers: Mapping[str, str],
        body: bytes,
    ) -> GatewayResponse:
        started = time.monotonic()
        operation = "control"
        request_id = ""
        try:
            if len(body) > MAX_REQUEST_BODY and len(body) > (
                self.request_body_limit(method, target, headers)
            ):
                return json_response({"ok": False, "error": "request_too_large"}, 413)
            parsed = urlsplit(target)
            if parsed.fragment or not parsed.path.startswith("/"):
                raise GatewayError("request target is invalid")
            if method in ("GET", "HEAD") and body:
                raise GatewayError("GET/HEAD requests must not contain a body")
            if parsed.path == "/health" and method in ("GET", "HEAD"):
                return self._health(parsed.query, headers)
            if parsed.path == NODE_LOGS_PATH:
                return self._node_logs(method, parsed.query, headers)
            runner_route = _runner_git_route(parsed.path)
            if runner_route is not None:
                # Runner grants are their own authorization; they never fall
                # through to the per-request router capability below.
                return self._dispatch_runner_git(
                    method, parsed.query, headers, body, runner_route)
            if (
                parsed.path
                in ("/forkmesh-mirror.json", "/.well-known/forkmesh-mirror.json")
                and method in ("GET", "HEAD")
                and not parsed.query
            ):
                return GatewayResponse(
                    200, "application/json; charset=utf-8", self._manifest
                )
            if self.dashboard is not None:
                dashboard_response = self.dashboard.maybe_dispatch(
                    method, parsed, headers, body
                )
                if dashboard_response is not None:
                    return dashboard_response

            pieces = parsed.path.split("/")
            direct_public_git = _public_git_route(parsed.path)
            direct_public_read = (
                len(pieces) == 7
                and pieces[1:4] == ["v1", "public", "repositories"]
            )
            # v0.7.22 briefly shipped the direct fleet probe against the
            # capability-protected spelling below.  Branch names and their
            # public commit metadata are already exposed by the canonical
            # /v1/public route, so keep this one exact GET/HEAD spelling as a
            # compatibility read while those clients upgrade.  Do not widen
            # the exception to another operation, query, method, or private
            # repository: every mutation and non-public read still requires a
            # router capability.
            legacy_public_branches_read = (
                len(pieces) == 6
                and pieces[1:3] == ["v1", "repositories"]
                and method in ("GET", "HEAD")
                and not parsed.query
                and unquote(pieces[5], errors="strict") == "branches"
            )
            public_read = bool(
                direct_public_git
                or direct_public_read
                or legacy_public_branches_read
            )
            if not public_read:
                request_id = self._authorize(method, target, headers, body)
            if (
                len(pieces) == 4
                and pieces[1] == "v1"
                and pieces[2] == "private-replicas"
            ):
                operation = "private-replica"
                opaque_id = unquote(pieces[3], errors="strict").lower()
                if method not in ("GET", "HEAD") or parsed.query:
                    raise GatewayError("private replica was not found")
                if not self._operator_private_replicas_allowed():
                    raise GatewayError("private replica was not found")
                path, size, replica_digest = private_replica_file(
                    self.config.private_replica_store,
                    opaque_id,
                    self.config.max_private_replica_bytes,
                )
                return GatewayResponse(
                    200,
                    "application/vnd.forkmesh.private-replica+json",
                    stream=StreamSpec(
                        kind="file",
                        content_type=(
                            "application/vnd.forkmesh.private-replica+json"),
                        content_length=size,
                        path=path,
                        filename=opaque_id + ".fm-private",
                        disposition="attachment",
                    ),
                    headers={"ETag": '"sha256-' + replica_digest + '"'},
                )
            masked_repository_read = (
                len(pieces) == 6
                and pieces[1:3] == ["v1", "repositories"]
            )
            if (
                direct_public_git is None
                and not direct_public_read
                and not masked_repository_read
            ):
                raise GatewayError("repository route was not found")
            try:
                if direct_public_git is not None:
                    owner, name, operation = direct_public_git
                else:
                    offset = 1 if direct_public_read else 0
                    owner = unquote(
                        pieces[3 + offset], errors="strict").lower()
                    name = unquote(pieces[4 + offset], errors="strict")
                    operation = unquote(
                        pieces[5 + offset], errors="strict")
            except UnicodeError as exc:
                raise GatewayError("repository route was not found") from exc
            if direct_public_read and (
                method not in ("GET", "HEAD")
                or operation not in DIRECT_PUBLIC_READ_OPERATIONS
            ):
                raise GatewayError("repository route was not found")
            # Unknown, disabled, private, and quarantined repositories all take
            # the same lookup path and produce the same response. A repository
            # missing from the signed configuration may still resolve under
            # the writable root; it is created only for a capability-verified
            # receive-pack target whose owner is explicitly allowlisted.
            if self._operator_quarantined(owner, name):
                raise GatewayError("repository route was not found")
            repository = self.repositories.get((owner, name.lower()))
            if repository is None and not public_read:
                # Only a verified router capability reaches a restricted
                # repository; every public spelling stays blind to it.
                repository = self.restricted_repositories.get(
                    (owner, name.lower()))
            if repository is None and not direct_public_read:
                repository = self._writable_repository(
                    owner,
                    name,
                    create=(operation == "git-receive-pack"),
                )
            if repository is None or not _repository_serves(
                    repository, operation):
                raise GatewayError("repository route was not found")

            query_fields = OPERATION_QUERY_FIELDS
            if operation == "blobs":
                try:
                    repeated = parse_qs(
                        parsed.query,
                        keep_blank_values=True,
                        strict_parsing=bool(parsed.query),
                        max_num_fields=61,
                    )
                except ValueError as exc:
                    raise GatewayError("request query is invalid") from exc
                if (
                    set(repeated) - {"path", "ref"}
                    or not 1 <= len(repeated.get("path", [])) <= 60
                    or len(repeated.get("ref", [])) > 1
                ):
                    raise GatewayError("request query is invalid")
                paths = [
                    _safe_repo_path(path, allow_empty=False)
                    for path in repeated["path"]
                ]
                query = {
                    "paths": paths,
                    "ref": (repeated.get("ref") or [""])[0],
                }
            else:
                query = self._parse_query(
                    parsed.query, query_fields[operation])
            cache_key: tuple[Any, ...] | None = None
            if operation == "git-info-refs" and method in ("GET", "HEAD"):
                normalized_headers = {
                    str(key).lower(): str(value)
                    for key, value in headers.items()
                }
                git_protocol = normalized_headers.get(
                    "git-protocol", "").strip()
                if git_protocol not in ("", "version=1", "version=2"):
                    raise GatewayError("Git protocol is not available")
                cache_key = self._repository_response_cache_key(
                    repository, operation, query, git_protocol)
            elif method in ("GET", "HEAD") and operation == PULSE_OPERATION:
                cache_key = self._repository_response_cache_key(
                    repository, operation, query, "until:%d" % pulse_window_end(
                        pulse_period(query), int(self.clock_ms()) // 1000))
            elif method in ("GET", "HEAD") and operation in (
                DIRECT_PUBLIC_READ_OPERATIONS - {"raw", "release-blob"}
            ):
                cache_key = self._repository_response_cache_key(
                    repository, operation,
                    self._commit_keyed_query(repository, operation, query))
            if cache_key is not None:
                cached = self._cached_repository_response(cache_key)
                if cached is not None:
                    # This body was produced only after the repository passed
                    # its generation integrity check. Serve those trusted RAM
                    # bytes without touching disk until an explicit generation
                    # event invalidates them.
                    return cached
            if not repository.check_integrity():
                raise GatewayError("repository route was not found")
            if operation in PULL_MUTATIONS:
                if method != "POST" or query:
                    raise GatewayError("merge request is invalid")
                return self._dispatch_merge_pull(
                    owner, name, body, operation)
            if operation == "actions-status":
                if method != "GET" or query or body:
                    raise GatewayError("Actions status request is invalid")
                return self._actions_status(owner, name)
            if operation == BRANCH_TRASH_OPERATION:
                if method != "GET" or query or body:
                    raise GatewayError("branch trash request is invalid")
                listing = repository.branch_trash()
                listing["node"] = self.config.node
                response = json_response(listing)
                response.headers["Cache-Control"] = "no-store"
                return response
            if operation == "actions-control":
                if method != "POST" or query or not body:
                    raise GatewayError("Actions control request is invalid")
                return self._actions_control(
                    owner,
                    name,
                    body,
                    self._actions_control_capability(
                        owner, name, target, headers),
                )
            if operation == "git-info-refs":
                if method not in ("GET", "HEAD") or query != {
                    "service": "git-upload-pack"
                }:
                    raise GatewayError("Git service is not available")
                normalized_headers = {
                    str(key).lower(): str(value)
                    for key, value in headers.items()
                }
                git_protocol = normalized_headers.get("git-protocol", "").strip()
                if git_protocol not in ("", "version=1", "version=2"):
                    raise GatewayError("Git protocol is not available")
                assert cache_key is not None
                self.response_cache.backend_read()
                payload = repository.git_advertisement(git_protocol)
                return self._remember_repository_response(cache_key, GatewayResponse(
                    200,
                    "application/x-git-upload-pack-advertisement",
                    payload,
                    {"Cache-Control": "no-store"},
                ))
            if operation == "git-upload-pack":
                normalized_headers = {
                    str(key).lower(): str(value)
                    for key, value in headers.items()
                }
                if (
                    method != "POST"
                    or normalized_headers.get("content-type", "")
                    .split(";", 1)[0]
                    .strip()
                    != "application/x-git-upload-pack-request"
                ):
                    raise GatewayError("Git service is not available")
                git_protocol = normalized_headers.get("git-protocol", "").strip()
                if git_protocol not in ("", "version=1", "version=2"):
                    raise GatewayError("Git protocol is not available")
                decoded_body = _decode_git_body(
                    body, normalized_headers.get("content-encoding", "")
                )
                return GatewayResponse(
                    200,
                    "application/x-git-upload-pack-result",
                    stream=repository.upload_pack_spec(
                        decoded_body, git_protocol),
                )
            if operation == "git-receive-pack":
                # The verified capability's method/target binding is the
                # complete authorization: the routing Worker only signs a
                # receive-pack capability for a push it already authorized.
                if method in ("GET", "HEAD"):
                    if query != {"service": "git-receive-pack"}:
                        raise GatewayError("Git service is not available")
                    payload = repository.receive_pack_advertisement()
                    return GatewayResponse(
                        200,
                        "application/x-git-receive-pack-advertisement",
                        payload,
                        {"Cache-Control": "no-store"},
                    )
                normalized_headers = {
                    str(key).lower(): str(value)
                    for key, value in headers.items()
                }
                if (
                    method != "POST"
                    or set(query) - {
                        TAG_MOVES_QUERY_FIELD, PROTECT_QUERY_FIELD,
                        PUSH_PREFIX_QUERY_FIELD}
                    or normalized_headers.get("content-type", "")
                    .split(";", 1)[0]
                    .strip()
                    != "application/x-git-receive-pack-request"
                ):
                    raise GatewayError("Git service is not available")
                tag_moves = parse_tag_moves(
                    query.get(TAG_MOVES_QUERY_FIELD, ""))
                protected = parse_protected_refs(
                    query.get(PROTECT_QUERY_FIELD, ""))
                decoded_body = _decode_git_body(
                    body,
                    normalized_headers.get("content-encoding", ""),
                    max(MAX_REQUEST_BODY, self.config.receive_max_input_bytes),
                )
                commands = receive_pack_commands(
                    decoded_body, PUSH_MAX_PREAMBLE_LINES)
                push_prefix = parse_push_prefix(
                    query.get(PUSH_PREFIX_QUERY_FIELD, ""))
                if push_prefix and tag_moves:
                    raise GatewayError("contributor push grant is invalid")
                enforce_push_ref_policy(commands, tag_moves)
                enforce_protected_refs(commands, protected)
                try:
                    enforce_push_prefix(
                        commands, push_prefix,
                        prefix_refs(repository.git_dir, push_prefix)
                        if push_prefix else None)
                except PushRefused as refused:
                    return GatewayResponse(
                        200,
                        "application/x-git-receive-pack-result",
                        receive_pack_rejection(
                            decoded_body, commands, str(refused)),
                        {"Cache-Control": "no-store"},
                    )
                return self._run_receive_pack(
                    repository, owner, name, decoded_body)
            if method not in ("GET", "HEAD"):
                raise GatewayError("repository operation is read-only")
            if operation == "blobs":
                assert cache_key is not None
                return self._single_flight(
                    cache_key,
                    lambda: json_response(repository.blobs(
                        query["paths"], query.get("ref", ""))),
                )
            if operation == "raw":
                stream = repository.raw_spec(query)
                return GatewayResponse(
                    200,
                    stream.content_type,
                    stream=stream,
                )
            if operation == "release-blob":
                stream = repository.release_spec(
                    query, self.config.max_release_bytes
                )
                return GatewayResponse(
                    200,
                    "application/octet-stream",
                    headers={
                        "Cache-Control": (
                            "public, max-age=31536000, immutable"
                        ),
                        "ETag": f'"sha256-{stream.filename}"',
                    },
                    stream=stream,
                )
            if operation == PULSE_OPERATION:
                assert cache_key is not None
                now_seconds = int(self.clock_ms()) // 1000
                return self._single_flight(
                    cache_key,
                    lambda: _pulse_response(repository.pulse(
                        query, now_seconds,
                        pulse_exclusion_rules(self.config.node))),
                )
            function = {
                "tree": repository.tree,
                "blob": repository.blob,
                "history": repository.history,
                "commit": repository.commit,
                "compare": lambda value: self._compare(repository, value),
                "merge-preview": lambda value: self._merge_preview(
                    repository, value),
                "branches": repository.branches,
                "search": repository.search,
                "stats": repository.stats,
                "sizes": repository.sizes,
            }[operation]
            assert cache_key is not None
            return self._single_flight(
                cache_key, lambda: json_response(function(query)))
        except GatewayError:
            response = dispatch_error_response("not_found", 404, target)
        except GitError:
            response = dispatch_error_response("mirror_unavailable", 503, target)
        except Exception:
            response = dispatch_error_response("mirror_unavailable", 503, target)
        finally:
            # Successful responses are recorded by the HTTP adapter after
            # streaming. Error/control responses are intentionally identity-free.
            pass
        self._record(
            request_id=request_id,
            operation=operation,
            status=response.status,
            started=started,
            byte_count=len(response.body),
        )
        return response


def _decode_org_pull_rules(value: str) -> str:
    if not value:
        return ""
    if len(value) > MAX_ORG_PULL_RULES_BYTES * 2 or not re.fullmatch(
            r"[A-Za-z0-9_-]+", value):
        raise GatewayError("organization rules are invalid")
    try:
        decoded = base64.urlsafe_b64decode(
            value + "=" * (-len(value) % 4)).decode("utf-8")
    except (ValueError, UnicodeDecodeError) as exc:
        raise GatewayError("organization rules are invalid") from exc
    if len(decoded.encode("utf-8")) > MAX_ORG_PULL_RULES_BYTES:
        raise GatewayError("organization rules are too large")
    return decoded


def _safe_download_filename(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._ -]+", "_", value).strip(" .")
    return (cleaned or "file")[:180]


class MirrorGatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ForkMeshMirrorGateway/1"
    sys_version = ""
    # Headers and body leave in separate writes; on a kept-alive connection
    # (the app host's fast path reuses one) Nagle's algorithm held the body
    # until the peer's delayed ACK, adding ~40 ms to every read.
    disable_nagle_algorithm = True

    @property
    def application(self) -> GatewayApplication:
        return self.server.application  # type: ignore[attr-defined]

    def log_message(self, _format: str, *_arguments: Any) -> None:
        # BaseHTTPRequestHandler logs client IP and raw path by default.
        return

    def do_GET(self) -> None:  # noqa: N802
        self._handle()

    def do_HEAD(self) -> None:  # noqa: N802
        self._handle()

    def do_POST(self) -> None:  # noqa: N802
        self._handle()

    def do_OPTIONS(self) -> None:  # noqa: N802
        # Credentialed dashboard collaboration uses a browser CORS preflight.
        # Route it through the same bounded dispatcher; DashboardService owns
        # the exact origin/method/header allowlist and every other OPTIONS path
        # remains a 404.
        self._handle()

    def do_PUT(self) -> None:  # noqa: N802
        self._method_not_allowed()

    def do_DELETE(self) -> None:  # noqa: N802
        self._method_not_allowed()

    def do_PATCH(self) -> None:  # noqa: N802
        self._method_not_allowed()

    def _method_not_allowed(self) -> None:
        response = json_response({"ok": False, "error": "read_only"}, 405)
        self._send_headers(response, content_length=len(response.body))
        if self.command != "HEAD":
            self.wfile.write(response.body)

    def _read_chunked_body(self, limit: int = MAX_REQUEST_BODY) -> bytes:
        """Decode one strictly framed, bounded HTTP/1.1 chunked body.

        Cloudflare Tunnel can legitimately translate a bodyless HTTP/2 GET
        into an empty chunked HTTP/1.1 request.  Rejecting the transfer coding
        itself made those reads intermittent 400s.  Decode the one supported
        coding instead, while rejecting extensions, trailers, ambiguous
        Content-Length framing, overlong size lines, and oversized bodies.
        The application still rejects a non-empty GET/HEAD body.

        A streamed push (the Worker forwards a large pack chunked) is spooled
        to disk past the base cap so the gateway holds one copy of it, not a
        growing buffer plus its final ``bytes``.
        """
        with tempfile.SpooledTemporaryFile(max_size=MAX_REQUEST_BODY) as spool:
            self._read_chunks_into(spool, limit)
            spool.seek(0)
            return spool.read()

    def _read_chunks_into(self, sink: Any, limit: int) -> int:
        body_length = 0
        chunk_count = 0
        max_chunks = MAX_REQUEST_CHUNKS
        if limit > MAX_REQUEST_BODY:
            max_chunks = max(max_chunks, limit // MIN_AVERAGE_CHUNK_BYTES)
        while True:
            size_line = self.rfile.readline(66)
            if (
                not size_line.endswith(b"\r\n")
                or len(size_line) > 65
            ):
                raise GatewayError("invalid chunked request body")
            size_text = size_line[:-2]
            if (
                not size_text
                or b";" in size_text
                or not re.fullmatch(rb"[0-9A-Fa-f]{1,16}", size_text)
            ):
                raise GatewayError("invalid chunked request body")
            size = int(size_text, 16)
            if size == 0:
                # Trailers are unnecessary for the gateway's bounded signed
                # requests and create another framing surface, so accept only
                # the canonical empty trailer section.
                if self.rfile.readline(3) != b"\r\n":
                    raise GatewayError("invalid chunked request body")
                return body_length
            chunk_count += 1
            if chunk_count > max_chunks:
                raise GatewayError("invalid chunked request body")
            if size > limit - body_length:
                raise GatewayError("invalid request length")
            chunk = self.rfile.read(size + 2)
            if len(chunk) != size + 2 or chunk[-2:] != b"\r\n":
                raise GatewayError("invalid chunked request body")
            sink.write(chunk[:-2])
            body_length += size

    def _read_body(self, limit: int = MAX_REQUEST_BODY) -> bytes:
        transfer_encoding = self.headers.get("Transfer-Encoding")
        raw_length = self.headers.get("Content-Length")
        if transfer_encoding is not None:
            if (
                raw_length is not None
                or transfer_encoding.strip().lower() != "chunked"
            ):
                raise GatewayError("ambiguous request framing")
            return self._read_chunked_body(limit)
        raw_length = raw_length or "0"
        try:
            length = int(raw_length)
        except ValueError as exc:
            raise GatewayError("invalid request length") from exc
        if length < 0 or length > limit:
            raise GatewayError("invalid request length")
        return self.rfile.read(length) if length else b""

    # Concurrent pushes above the base cap each hold their pack in memory
    # while git indexes it; the rest wait on the client's retry, not on RAM.
    _large_body_slots = threading.BoundedSemaphore(LARGE_REQUEST_BODY_SLOTS)

    def _handle(self) -> None:
        started = time.monotonic()
        large_slot = False
        try:
            # A repository sync can change the integrity pin between requests.
            # If SIGHUP arrived while the server was busy, make the first new
            # request wait for the validated in-place swap instead of letting
            # it observe the old pin before serve_forever's next idle poll.
            self.server.reload_if_requested()  # type: ignore[attr-defined]
            limit = self.application.request_body_limit(
                self.command, self.path, dict(self.headers.items())
            )
            if limit > MAX_REQUEST_BODY:
                large_slot = self._large_body_slots.acquire(blocking=False)
                if not large_slot:
                    # Unread pack bytes stay on the socket; drop it rather
                    # than let them be parsed as the next request.
                    self.close_connection = True
                    response = json_response(
                        {"ok": False, "error": "push_busy"}, 503)
                    response.headers["Retry-After"] = "30"
                    self._send_headers(
                        response, content_length=len(response.body))
                    self.wfile.write(response.body)
                    return
            body = self._read_body(limit)
            response = self.application.dispatch(
                self.command,
                self.path,
                dict(self.headers.items()),
                body,
            )
            del body
        except GatewayError:
            # A framing rejection may leave unread bytes on the socket. Never
            # let a subsequent request reinterpret them on this connection.
            self.close_connection = True
            response = json_response({"ok": False, "error": "bad_request"}, 400)
        finally:
            if large_slot:
                self._large_body_slots.release()
        byte_count = 0
        delivered = False
        try:
            if response.stream is not None:
                content_length = response.stream.content_length
            else:
                content_length = len(response.body)
            self._send_headers(response, content_length=content_length)
            if self.command == "HEAD":
                delivered = True
                return
            if response.stream is not None:
                byte_count = self._stream(response.stream)
            else:
                self.wfile.write(response.body)
                byte_count = len(response.body)
            delivered = True
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        finally:
            operation = self.path.rsplit("/", 1)[-1].split("?", 1)[0]
            direct_git = _public_git_route(urlsplit(self.path).path)
            if direct_git is not None:
                operation = direct_git[2]
            if urlsplit(self.path).path.startswith("/v1/private-replicas/"):
                operation = "private-replica"
            self.application._record(
                request_id=self.headers.get("X-ForkMesh-Request-Id", ""),
                operation=operation,
                status=response.status,
                started=started,
                byte_count=byte_count,
            )
            self.application.record_service_counter(
                self.path,
                operation,
                response.status,
                delivered=delivered,
                user_agent=self.headers.get("User-Agent", ""),
            )

    def _send_headers(
        self, response: GatewayResponse, *, content_length: int | None
    ) -> None:
        self.send_response(response.status)
        self.send_header("Content-Type", response.content_type)
        if content_length is not None:
            self.send_header("Content-Length", str(content_length))
        else:
            self.send_header("Connection", "close")
            self.close_connection = True
        # Dashboard responses override the locked-down defaults; every other
        # response keeps the historical no-store / default-src 'none' posture.
        overridden = {str(key).lower() for key in response.headers}
        if "cache-control" not in overridden:
            self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        if "content-security-policy" not in overridden:
            self.send_header("Content-Security-Policy", "default-src 'none'")
        if "referrer-policy" not in overridden:
            self.send_header("Referrer-Policy", "no-referrer")
        for key, value in response.headers.items():
            if key.lower() in {
                "cache-control",
                "etag",
                "last-modified",
                "accept-ranges",
                "content-security-policy",
                "referrer-policy",
                "x-frame-options",
                "location",
                "x-forkmesh-served-by",
                "x-forkmesh-memory-cache",
                "x-forkmesh-ref-updates",
                "retry-after",
                # Only gateway-created responses can reach this adapter.  The
                # portable-session handoff needs its verified grant to cross
                # the final HTTP boundary as an HttpOnly cookie; omitting this
                # header leaves the browser logged out even though dispatch
                # accepted the handoff and returned its clean redirect.
                "set-cookie",
                "access-control-allow-origin",
                "access-control-allow-credentials",
                "access-control-allow-methods",
                "access-control-allow-headers",
                "access-control-expose-headers",
                "access-control-max-age",
                "vary",
            }:
                self.send_header(key, value)
        if response.stream and response.stream.filename:
            filename = _safe_download_filename(response.stream.filename)
            self.send_header(
                "Content-Disposition",
                response.stream.disposition
                + '; filename="'
                + filename.replace('"', "_")
                + '"',
            )
        self.end_headers()

    _node_log_slots = threading.BoundedSemaphore(NODE_LOG_MAX_STREAMS)

    def _stream_node_logs(self, spec: StreamSpec) -> int:
        """Relay journalctl -f as server-sent events until the viewer leaves.

        Each event carries one line in the desktop feed's tagged format
        ("J <journal line>", "M host <note>"), and an idle journal still sends
        "K" every NODE_LOG_KEEPALIVE_SECONDS so the viewer can tell a quiet
        host from a dead connection. The stream only ends when a write to the
        viewer fails or journalctl exits.
        """
        def send(line: str) -> int:
            data = ("data: " + line.replace("\r", " ").replace("\n", " ") + "\n\n").encode(
                "utf-8", "replace"
            )
            self.wfile.write(data)
            self.wfile.flush()
            return len(data)

        if not self._node_log_slots.acquire(blocking=False):
            return send("M host too many log viewers are connected to this host")
        total = 0
        process: subprocess.Popen[bytes] | None = None
        try:
            try:
                process = subprocess.Popen(
                    list(spec.command),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            except OSError:
                return send("M host journalctl is not available on this host")
            assert process.stdout is not None
            os.set_blocking(process.stdout.fileno(), False)
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            pending = b""
            last_sent = time.monotonic()
            total += send("K")
            try:
                while True:
                    ready = selector.select(timeout=1)
                    if ready:
                        try:
                            chunk = os.read(process.stdout.fileno(), 64 * 1024)
                        except BlockingIOError:
                            chunk = None
                        if chunk == b"":
                            if pending:
                                total += send("J " + pending.decode("utf-8", "replace"))
                            total += send("M host journalctl exited")
                            return total
                        if chunk:
                            pending += chunk
                            *lines, pending = pending.split(b"\n")
                            for raw in lines:
                                total += send("J " + raw.decode("utf-8", "replace"))
                            if len(pending) > 64 * 1024:
                                total += send("J " + pending.decode("utf-8", "replace"))
                                pending = b""
                            last_sent = time.monotonic()
                    if time.monotonic() - last_sent >= NODE_LOG_KEEPALIVE_SECONDS:
                        total += send("K")
                        last_sent = time.monotonic()
            finally:
                selector.close()
        finally:
            self._node_log_slots.release()
            if process is not None and process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=2)
                except (OSError, subprocess.TimeoutExpired):
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except OSError:
                        pass

    def _stream(self, spec: StreamSpec) -> int:
        if spec.kind == "file" and spec.path is not None:
            total = 0
            with spec.path.open("rb") as source:
                for chunk in iter(lambda: source.read(256 * 1024), b""):
                    self.wfile.write(chunk)
                    total += len(chunk)
            return total
        if spec.kind == "node-logs" and spec.command:
            return self._stream_node_logs(spec)
        if spec.kind != "process" or not spec.command:
            raise GatewayError("invalid stream specification")
        process = subprocess.Popen(
            list(spec.command),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=_git_environment(spec.git_protocol),
            start_new_session=True,
        )
        assert process.stdin is not None
        assert process.stdout is not None
        os.set_blocking(process.stdin.fileno(), False)
        os.set_blocking(process.stdout.fileno(), False)
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ, "stdout")
        pending = memoryview(spec.input_bytes)
        if pending:
            selector.register(process.stdin, selectors.EVENT_WRITE, "stdin")
        else:
            process.stdin.close()
        total = 0
        received_output = False
        host_wait_started = time.monotonic()
        try:
            while selector.get_map():
                inactivity_timeout = (
                    STREAM_INACTIVITY_TIMEOUT_SECONDS
                    if received_output
                    else STREAM_INITIAL_RESPONSE_TIMEOUT_SECONDS
                )
                remaining = inactivity_timeout - (
                    time.monotonic() - host_wait_started
                )
                if remaining <= 0:
                    phase = "mid-stream" if received_output else "initial response"
                    raise TimeoutError("Git %s timed out" % phase)
                for key, _mask in selector.select(timeout=min(1, remaining)):
                    if key.data == "stdin":
                        try:
                            written = os.write(process.stdin.fileno(), pending[:65536])
                            pending = pending[written:]
                        except BlockingIOError:
                            continue
                        if not pending:
                            selector.unregister(process.stdin)
                            process.stdin.close()
                    else:
                        try:
                            chunk = os.read(process.stdout.fileno(), 256 * 1024)
                        except BlockingIOError:
                            continue
                        if not chunk:
                            selector.unregister(process.stdout)
                            continue
                        received_output = True
                        host_wait_started = time.monotonic()
                        self.wfile.write(chunk)
                        total += len(chunk)
                        # wfile.write can block for an arbitrarily slow HTTP
                        # receiver. During that time the Git process eventually
                        # blocks on its stdout pipe too, so neither silence is a
                        # host stall. Start the next host-observation window only
                        # after downstream capacity returns.
                        host_wait_started = time.monotonic()
            if process.wait(timeout=5) != 0:
                raise GitError("streaming Git operation failed")
            return total
        finally:
            selector.close()
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=2)
                except (OSError, subprocess.TimeoutExpired):
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except OSError:
                        pass


def gateway_generation_marker_path(config_path: Path) -> Path:
    return (
        config_path.parent
        / GATEWAY_GENERATION_DIRECTORY
        / GATEWAY_GENERATION_FILE
    )


def record_gateway_generation(
    config_path: Path, generation_sha256: str
) -> None:
    """Acknowledge the exact validated configuration with an atomic marker."""
    if not re.fullmatch(r"[0-9a-f]{64}", generation_sha256):
        raise GatewayError("gateway generation digest is invalid")
    marker = gateway_generation_marker_path(config_path)
    marker.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".generation-", dir=marker.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = -1
            stream.write(generation_sha256.encode("ascii") + b"\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, marker)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


class MirrorGatewayServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self, address: tuple[str, int], application: GatewayApplication
    ) -> None:
        super().__init__(address, MirrorGatewayHandler)
        self.application = application
        self._reload_requested = threading.Event()
        self._cache_invalidation_requested = threading.Event()
        self._reload_in_progress = threading.Event()
        self._reload_control_lock = threading.Lock()
        self._prewarm_state_lock = threading.Lock()
        self._prewarm_thread: threading.Thread | None = None
        self._prewarm_again = False

    def start_prewarm(self) -> None:
        """Warm the adopted generation off the accept loop.

        serve_forever accepts connections on the thread that runs
        service_actions, so warming there would stop new connections for the
        whole warmup. At most one warmup thread runs; a generation adopted
        while it is busy queues exactly one more pass.
        """
        with self._prewarm_state_lock:
            if self._prewarm_thread is not None:
                self._prewarm_again = True
                return
            self._prewarm_again = False
            thread = threading.Thread(
                target=self._run_prewarm,
                name="gateway-prewarm",
                daemon=True,
            )
            self._prewarm_thread = thread
        thread.start()

    def _run_prewarm(self) -> None:
        while True:
            try:
                self.application.prewarm_memory_caches()
            except Exception:  # noqa: BLE001 - warmup is best-effort
                pass
            with self._prewarm_state_lock:
                if not self._prewarm_again:
                    self._prewarm_thread = None
                    return
                self._prewarm_again = False

    def wait_for_prewarm(self, timeout: float | None = None) -> None:
        with self._prewarm_state_lock:
            thread = self._prewarm_thread
        if thread is not None:
            thread.join(timeout)

    def request_reload(self) -> None:
        # Signal handlers may only perform trivial work.  serve_forever calls
        # service_actions on its own thread, where the validated replacement
        # can be built without interrupting an active request handler.
        self._reload_requested.set()

    def request_cache_invalidation(self) -> None:
        self._cache_invalidation_requested.set()

    def reload_if_requested(self) -> None:
        if (
            not self._reload_requested.is_set()
            and not self._cache_invalidation_requested.is_set()
            and not self._reload_in_progress.is_set()
        ):
            return
        # Request handlers are concurrent. Once one sees the event, all later
        # callers wait behind this lock until the replacement is ready.
        adopted = False
        with self._reload_control_lock:
            reload_requested = self._reload_requested.is_set()
            projection_requested = self._cache_invalidation_requested.is_set()
            if not reload_requested and not projection_requested:
                return
            # Set progress before clearing the request so another handler can
            # never observe both events clear while the swap is underway.
            self._reload_in_progress.set()
            try:
                if reload_requested:
                    self._reload_requested.clear()
                    started = time.monotonic()
                    status = 200
                    try:
                        # Acknowledge as soon as the validated generation is
                        # swapped in. Warming its caches used to run first,
                        # inside this lock, and routinely outlasted the
                        # daemon's acknowledgement deadline on 1 vCPU nodes,
                        # so every refs change hard-restarted the gateway on
                        # the whole fleet at once (2026-09-12).
                        generation = self.application.reload_from_disk(
                            prewarm=False)
                        record_gateway_generation(
                            self.application.config.config_path, generation)
                        adopted = True
                    except (GatewayError, OSError):
                        # Keep serving the last validated generation. The daemon
                        # sees the stale acknowledgement and retains its restart
                        # fallback.
                        status = 503
                    self.application.log(
                        {
                            "time": datetime.now(timezone.utc)
                            .isoformat()
                            .replace("+00:00", "Z"),
                            "requestId": "",
                            "operation": "reload",
                            "status": status,
                            "durationMs": int(
                                (time.monotonic() - started) * 1000),
                            "bytes": 0,
                        }
                    )
                if projection_requested:
                    self._cache_invalidation_requested.clear()
                    self.application.invalidate_projection_cache()
            finally:
                self._reload_in_progress.clear()
        if adopted:
            self.start_prewarm()

    def service_actions(self) -> None:
        self.reload_if_requested()

    def server_close(self) -> None:
        try:
            super().server_close()
        finally:
            self.application.close()

    def handle_error(
        self, _request: Any, _client_address: Any
    ) -> None:
        # socketserver's default includes the raw client address and traceback.
        self.application.log(
            {
                "time": datetime.now(timezone.utc)
                .isoformat()
                .replace("+00:00", "Z"),
                "requestId": "",
                "operation": "control",
                "status": 503,
                "durationMs": 0,
                "bytes": 0,
            }
        )


def cleanup_stale_runtime(root: Path) -> int:
    """Remove only gateway-owned materializations between service starts."""
    if not root.is_absolute() or root != Path(os.path.normpath(str(root))):
        raise GatewayError("runtime cleanup root is invalid")
    try:
        root_info = root.lstat()
    except OSError as exc:
        raise GatewayError("runtime cleanup root is unavailable") from exc
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or stat.S_ISLNK(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
        or stat.S_IMODE(root_info.st_mode) & (stat.S_IRWXG | stat.S_IRWXO)
    ):
        raise GatewayError("runtime cleanup root is unsafe")
    removed = 0
    try:
        candidates = tuple(root.iterdir())
    except OSError as exc:
        raise GatewayError("runtime cleanup root cannot be listed") from exc
    for path in candidates:
        if not RUNTIME_DIRECTORY_RE.fullmatch(path.name):
            continue
        try:
            info = path.lstat()
            resolved = path.resolve()
        except OSError as exc:
            raise GatewayError("runtime materialization is unsafe") from exc
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.geteuid()
            or resolved.parent != root.resolve()
        ):
            raise GatewayError("runtime materialization is unsafe")
        try:
            shutil.rmtree(path)
        except OSError as exc:
            raise GatewayError("runtime materialization cleanup failed") from exc
        removed += 1
    return removed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Serve explicitly configured public Git mirrors on loopback for a "
            "direct HTTPS reverse proxy. Private repositories fail closed."
        )
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--config", type=Path, help="gateway JSON configuration"
    )
    source.add_argument(
        "--refs-sha256",
        type=Path,
        metavar="BARE_REPOSITORY",
        help="print the canonical heads/tags SHA-256 integrity pin",
    )
    source.add_argument(
        "--cleanup-runtime",
        type=Path,
        metavar="DIRECTORY",
        help="remove stale gateway materializations before service startup",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate configuration, manifest, repositories, and integrity pins",
    )
    parser.add_argument(
        "--sync-dashboard-replica",
        action="store_true",
        help=(
            "refresh allowlisted public dashboard projections from the "
            "configured canonical origin and exit"
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    application: GatewayApplication | None = None
    try:
        if args.cleanup_runtime is not None:
            removed = cleanup_stale_runtime(args.cleanup_runtime)
            print(_canonical_json({"ok": True, "removed": removed}))
            return 0
        if args.refs_sha256 is not None:
            repository_path = args.refs_sha256.resolve()
            _ensure_bare_repository(repository_path)
            print(refs_sha256(repository_path))
            return 0
        assert args.config is not None
        config = load_config(args.config)
        if args.sync_dashboard_replica:
            if (
                not config.dashboard_enabled
                or config.dashboard_replica_path is None
                or not config.dashboard_canonical_origin
            ):
                raise GatewayError(
                    "dashboard replica synchronization is not configured"
                )
            summary = mirror_replica.refresh_bundle(
                config.dashboard_replica_path,
                config.dashboard_canonical_origin,
            )
            print(_canonical_json(summary))
            return 0
        # The supervisor may SIGHUP this process as soon as it is spawned, and
        # building plus warming a large repository takes seconds. SIGHUP's
        # default action terminates the process, so hold an early request
        # until the live handler is installed below instead of dying.
        original_hup_handler = signal.getsignal(signal.SIGHUP)
        early_reload = threading.Event()
        if not args.check:
            signal.signal(
                signal.SIGHUP, lambda *_arguments: early_reload.set())
        application = GatewayApplication(config)
        if args.check:
            summary = {
                "ok": application.quarantined_count == 0,
                "node": config.node,
                "transport": "loopback-http-behind-direct-https-proxy",
                "publicRepositoryCount": len(application.repositories),
                "quarantinedRepositoryCount": application.quarantined_count,
                "privateRepositoryDetailsPublished": False,
                "privateKeysLoaded": False,
                "dashboardEnabled": config.dashboard_enabled,
                "dashboardReplicaConfigured": (
                    config.dashboard_replica_path is not None
                ),
                "dynamicRepositoriesSkipped": (
                    config.dynamic_repositories_skipped
                ),
            }
            print(_canonical_json(summary))
            return 0 if summary["ok"] else 1
        application.prewarm_memory_caches()
        server = MirrorGatewayServer(
            (config.listen_host, config.listen_port), application
        )
        application = None
        # The daemon compares this acknowledgement with the exact bytes it
        # wrote.  Subsequent repository-ref changes use SIGHUP to replace the
        # validated application in place, leaving the listener continuously
        # available to visitors.
        previous_hup_handler = original_hup_handler
        previous_usr1_handler = signal.getsignal(signal.SIGUSR1)
        signal.signal(signal.SIGHUP, lambda *_arguments: server.request_reload())
        if early_reload.is_set():
            server.request_reload()
        signal.signal(
            signal.SIGUSR1,
            lambda *_arguments: server.request_cache_invalidation(),
        )
        try:
            record_gateway_generation(
                config.config_path, config.generation_sha256)
            print(
                _canonical_json(
                    {
                        "ok": True,
                        "event": "gateway_started",
                        "listen": f"{config.listen_host}:{config.listen_port}",
                        "publicRepositoryCount": len(
                            server.application.repositories),
                    }
                ),
                flush=True,
            )
            try:
                server.serve_forever(poll_interval=0.5)
            except KeyboardInterrupt:
                # systemd uses SIGINT so TemporaryDirectory cleanup runs
                # instead of leaking one materialized repository per restart.
                pass
        finally:
            signal.signal(signal.SIGHUP, previous_hup_handler)
            signal.signal(signal.SIGUSR1, previous_usr1_handler)
            server.server_close()
        return 0
    except (GatewayError, OSError) as exc:
        print(f"mirror gateway failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if application is not None:
            application.close()


if __name__ == "__main__":
    raise SystemExit(main())
