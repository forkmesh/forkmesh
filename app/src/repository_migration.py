"""Provider adapters and canonical records for resumable repository migration.

The orchestration and D1 policy live in :mod:`repository_imports`; this module
contains no Worker dependencies.  Keeping provider variance here makes the
same fixture usable against GitHub, GitLab, Codeberg, and Gitea and keeps
checkpoint, identity, and convergence logic provider-neutral.

Provider credentials are deliberately absent from every value returned by
this module.  A caller may pass one to its HTTP transport, but it must never be
placed in a job, checkpoint, object, report, or telemetry record.
"""

import hashlib
import json
import math
import re
from urllib.parse import quote


SCHEMA = "forkmesh-repository-migration-v1"
STATE_SCHEMA = "forkmesh-repository-migration-state-v1"
OBJECT_SCHEMA = "forkmesh-provider-object-v1"

OBJECT_TYPES = (
    "git_refs",
    "issues",
    "pull_requests",
    "reviews",
    "releases",
    "documentation",
    "labels",
    "users",
    "collaboration_history",
)

# The order is part of the checkpoint contract.  In particular, pull requests
# must precede reviews because providers expose reviews below one pull request.
PHASES = OBJECT_TYPES

MAX_OBJECT_BYTES = 1024 * 1024
MAX_VALUE_DEPTH = 20
MAX_COLLECTION_ITEMS = 20_000
PAGE_SIZE = 100

_SECRET_KEYS = frozenset({
    "access_token", "authorization", "client_secret", "password",
    "private_token", "refresh_token", "secret", "token",
})
_NEXT_LINK_RE = re.compile(r'<([^>]+)>\s*;\s*rel="next"')


class MigrationContractError(ValueError):
    """A provider response cannot be represented without silent loss."""


def canonical_json(value):
    """Return the exact UTF-8 bytes hashed by object and state receipts."""
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def sha256_id(domain, *parts):
    body = "\n".join(str(part or "") for part in parts).encode("utf-8")
    return hashlib.sha256(str(domain).encode("utf-8") + b"\n" + body).hexdigest()


def migration_id(repository_id):
    return "mig_" + sha256_id("forkmesh-migration-job-v1", repository_id)[:24]


def object_key(object_type, source_id):
    return sha256_id("forkmesh-migration-object-v1", object_type, source_id)


def _clean_string(value, limit=4096):
    if value is None:
        return ""
    return str(value).replace("\x00", "")[:limit]


def _safe_value(value, redacted, path="", depth=0):
    """Copy JSON provider data while making every omission explicit.

    Provider objects are encrypted by the service.  We preserve their complete
    JSON shape for later adapters/two-way sync, except credential-shaped fields
    which are replaced by a marker and listed in ``redacted``.  Unsupported
    values fail the page instead of disappearing from an otherwise-successful
    report.
    """
    if depth > MAX_VALUE_DEPTH:
        raise MigrationContractError("provider_object_too_deep")
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise MigrationContractError("provider_object_non_finite_number")
        return value
    if isinstance(value, list):
        if len(value) > MAX_COLLECTION_ITEMS:
            raise MigrationContractError("provider_object_collection_too_large")
        return [
            _safe_value(item, redacted, "%s[%d]" % (path, index), depth + 1)
            for index, item in enumerate(value)
        ]
    if isinstance(value, dict):
        if len(value) > MAX_COLLECTION_ITEMS:
            raise MigrationContractError("provider_object_collection_too_large")
        output = {}
        for raw_key, item in value.items():
            key = str(raw_key)
            item_path = key if not path else path + "." + key
            if key.casefold() in _SECRET_KEYS:
                output[key] = "[redacted credential field]"
                redacted.append(item_path)
                continue
            output[key] = _safe_value(item, redacted, item_path, depth + 1)
        return output
    raise MigrationContractError("provider_object_unsupported_value")


def _first(value, names):
    if not isinstance(value, dict):
        return None
    for name in names:
        candidate = value.get(name)
        if candidate not in (None, ""):
            return candidate
    return None


def _actor(value):
    if not isinstance(value, dict):
        return {}
    return {
        "sourceId": _clean_string(_first(value, ("id", "node_id")), 160),
        "login": _clean_string(
            _first(value, ("login", "username", "name")), 240),
        "url": _clean_string(
            _first(value, ("html_url", "web_url", "url")), 1000),
    }


def _source_id(stream, item, safe_payload):
    fields = tuple(stream.get("idFields") or ("id", "number", "iid"))
    value = _first(item, fields)
    prefix = str(stream.get("sourcePrefix") or "")
    parent = str(stream.get("parent") or "")
    if value not in (None, ""):
        source_id = str(value)
        kind = fields[next(
            index for index, name in enumerate(fields)
            if item.get(name) not in (None, ""))]
    else:
        # Some provider contributor/ref responses have no immutable database
        # id.  A canonical content identity is stable and is labelled as such,
        # so the report never pretends the provider supplied an id.
        source_id = "sha256:" + hashlib.sha256(
            canonical_json(safe_payload)).hexdigest()
        kind = "content_hash"
    if parent:
        source_id = parent + ":" + source_id
    if prefix:
        source_id = prefix + ":" + source_id
    return source_id, kind


def normalize_object(adapter, stream, item, repository_id):
    """Build one loss-detecting, provider-neutral preservation envelope."""
    if not isinstance(item, dict):
        raise MigrationContractError("provider_object_not_an_object")
    redacted = []
    safe_payload = _safe_value(item, redacted)
    encoded = canonical_json(safe_payload)
    if len(encoded) > MAX_OBJECT_BYTES:
        raise MigrationContractError("provider_object_too_large")
    source_id, source_id_kind = _source_id(stream, item, safe_payload)
    object_type = str(stream.get("objectType") or "")
    if object_type not in OBJECT_TYPES:
        raise MigrationContractError("unsupported_object_type")
    author = _first(item, ("user", "author", "creator", "released_by"))
    labels = item.get("labels")
    if isinstance(labels, list):
        label_ids = [
            _clean_string(
                _first(label, ("id", "name")) if isinstance(label, dict)
                else label,
                240,
            )
            for label in labels
        ]
    else:
        label_ids = []
    record = {
        "schema": OBJECT_SCHEMA,
        "objectType": object_type,
        "source": {
            "provider": adapter["provider"],
            "repositoryId": _clean_string(repository_id, 240),
            "id": source_id,
            "idKind": source_id_kind,
            "stream": str(stream.get("id") or ""),
            "url": _clean_string(
                _first(item, ("html_url", "web_url", "url")), 1000),
            "createdAt": _clean_string(
                _first(item, ("created_at", "createdAt", "authored_date")),
                80,
            ),
            "updatedAt": _clean_string(
                _first(item, (
                    "updated_at", "updatedAt", "committed_date",
                    "published_at", "released_at",
                )),
                80,
            ),
        },
        "common": {
            "title": _clean_string(
                _first(item, ("title", "name", "ref", "path")), 1000),
            "body": _clean_string(
                _first(item, ("body", "description", "content", "note")),
                MAX_OBJECT_BYTES,
            ),
            "state": _clean_string(
                _first(item, ("state", "status", "type")), 160),
            "author": _actor(author),
            "labels": [label for label in label_ids if label],
            "parentSourceId": str(stream.get("parent") or ""),
        },
        # The original provider JSON is the fidelity/audit layer.  Common fields
        # above are an indexable projection, not a lossy replacement.
        "providerPayload": safe_payload,
        "redactedFields": sorted(set(redacted)),
    }
    payload_hash = "sha256:" + hashlib.sha256(canonical_json(record)).hexdigest()
    return {
        "objectType": object_type,
        "sourceId": source_id,
        "sourceKey": object_key(object_type, source_id),
        "payloadHash": payload_hash,
        "record": record,
        "redactedFields": record["redactedFields"],
    }


def _stream(identifier, path, object_type, **options):
    return {
        "id": identifier,
        "path": path,
        "objectType": object_type,
        "shape": options.pop("shape", "list"),
        "pageSize": int(options.pop("pageSize", PAGE_SIZE)),
        "pageParameter": options.pop("pageParameter", "page"),
        "optional": bool(options.pop("optional", False)),
        **options,
    }


def _github_adapter(source, root, context):
    owner = quote(source["owner"], safe="")
    name = quote(source["name"], safe="")
    base = "/repos/%s/%s" % (owner, name)
    pulls = context.get("pull_requests") or []
    review_streams = [
        _stream(
            "pull_reviews:%s" % number,
            base + "/pulls/%s/reviews?per_page=100" % quote(str(number), safe=""),
            "reviews", parent="pull_request:%s" % number,
            sourcePrefix="review", idFields=("id",),
        )
        for number in pulls
    ]
    return {
        "provider": "github",
        "repositoryId": str(root.get("id") or ""),
        "streams": {
            "git_refs": [
                _stream("branches", base + "/branches?per_page=100", "git_refs",
                        sourcePrefix="branch", idFields=("name",)),
                _stream("tags", base + "/tags?per_page=100", "git_refs",
                        sourcePrefix="tag", idFields=("name",)),
            ],
            "issues": [_stream(
                "issues", base + "/issues?state=all&per_page=100", "issues",
                sourcePrefix="issue", idFields=("id", "number"),
                excludePulls=True,
            )],
            "pull_requests": [_stream(
                "pull_requests", base + "/pulls?state=all&per_page=100",
                "pull_requests", sourcePrefix="pull_request",
                idFields=("id", "number"),
            )],
            "reviews": review_streams + [_stream(
                "pull_review_comments", base + "/pulls/comments?per_page=100",
                "reviews", sourcePrefix="review_comment", idFields=("id",),
            )],
            "releases": [_stream(
                "releases", base + "/releases?per_page=100", "releases",
                sourcePrefix="release", idFields=("id", "tag_name"),
            )],
            "documentation": [_stream(
                "readme", base + "/readme", "documentation", shape="single",
                optional=True, sourcePrefix="document", idFields=("path", "sha"),
            )],
            "labels": [_stream(
                "labels", base + "/labels?per_page=100", "labels",
                sourcePrefix="label", idFields=("id", "name"),
            )],
            "users": [_stream(
                "contributors", base + "/contributors?per_page=100&anon=1",
                "users", sourcePrefix="user", idFields=("id", "login", "name"),
            )],
            "collaboration_history": [
                _stream(
                    "issue_comments", base + "/issues/comments?per_page=100",
                    "collaboration_history", sourcePrefix="issue_comment",
                    idFields=("id",),
                ),
                _stream(
                    "issue_events", base + "/issues/events?per_page=100",
                    "collaboration_history", sourcePrefix="issue_event",
                    idFields=("id",), optional=True,
                ),
                _stream(
                    "commit_comments", base + "/comments?per_page=100",
                    "collaboration_history", sourcePrefix="commit_comment",
                    idFields=("id",), optional=True,
                ),
            ],
        },
        "unsupported": [
            "GitHub wiki pages are a separate Git repository and require a "
            "separate mirror operation.",
            "Workflow logs and artifact bytes are not collaboration records; "
            "their source URLs remain in preserved provider payloads.",
        ],
    }


def _gitlab_adapter(source, root, context):
    project = quote(str(root.get("id") or ""), safe="")
    base = "/projects/" + project
    branch = quote(str(root.get("default_branch") or "main"), safe="")
    pulls = context.get("pull_requests") or []
    issues = context.get("issues") or []
    review_streams = []
    history_streams = [_stream(
        "events", base + "/events?per_page=100", "collaboration_history",
        sourcePrefix="event", idFields=("id",), optional=True,
    )]
    for number in pulls:
        encoded = quote(str(number), safe="")
        review_streams.append(_stream(
            "merge_request_discussions:%s" % number,
            base + "/merge_requests/%s/discussions?per_page=100" % encoded,
            "reviews", parent="merge_request:%s" % number,
            sourcePrefix="discussion", idFields=("id",),
        ))
        history_streams.append(_stream(
            "merge_request_notes:%s" % number,
            base + "/merge_requests/%s/notes?per_page=100" % encoded,
            "collaboration_history", parent="merge_request:%s" % number,
            sourcePrefix="merge_request_note", idFields=("id",),
            optional=True,
        ))
    for number in issues:
        encoded = quote(str(number), safe="")
        history_streams.append(_stream(
            "issue_notes:%s" % number,
            base + "/issues/%s/notes?per_page=100" % encoded,
            "collaboration_history", parent="issue:%s" % number,
            sourcePrefix="issue_note", idFields=("id",),
        ))
    return {
        "provider": "gitlab",
        "repositoryId": str(root.get("id") or ""),
        "streams": {
            "git_refs": [
                _stream("branches", base + "/repository/branches?per_page=100",
                        "git_refs", sourcePrefix="branch", idFields=("name",)),
                _stream("tags", base + "/repository/tags?per_page=100",
                        "git_refs", sourcePrefix="tag", idFields=("name",)),
            ],
            "issues": [_stream(
                "issues", base + "/issues?scope=all&per_page=100", "issues",
                sourcePrefix="issue", idFields=("id", "iid"),
            )],
            "pull_requests": [_stream(
                "merge_requests",
                base + "/merge_requests?scope=all&per_page=100",
                "pull_requests", sourcePrefix="merge_request",
                idFields=("id", "iid"),
            )],
            "reviews": review_streams,
            "releases": [_stream(
                "releases", base + "/releases?per_page=100", "releases",
                sourcePrefix="release", idFields=("tag_name", "name"),
            )],
            "documentation": [_stream(
                "readme",
                base + "/repository/files/README.md?ref=" + branch,
                "documentation", shape="single", optional=True,
                sourcePrefix="document", idFields=("file_path", "blob_id"),
            )],
            "labels": [_stream(
                "labels", base + "/labels?per_page=100", "labels",
                sourcePrefix="label", idFields=("id", "name"),
            )],
            "users": [_stream(
                "contributors", base + "/repository/contributors?per_page=100",
                "users", sourcePrefix="user",
                idFields=("id", "username", "name"),
            )],
            "collaboration_history": history_streams,
        },
        "unsupported": [
            "GitLab wiki pages are a separate Git repository and require a "
            "separate mirror operation.",
        ],
    }


def _gitea_api_adapter(source, root, context):
    owner = quote(source["owner"], safe="")
    name = quote(source["name"], safe="")
    base = "/repos/%s/%s" % (owner, name)
    branch = quote(str(root.get("default_branch") or "main"), safe="")
    pulls = context.get("pull_requests") or []
    review_streams = [
        _stream(
            "pull_reviews:%s" % number,
            base + "/pulls/%s/reviews?limit=100" % quote(str(number), safe=""),
            "reviews", parent="pull_request:%s" % number,
            sourcePrefix="review", idFields=("id",), optional=True,
            pageParameter="page",
        )
        for number in pulls
    ]
    return {
        "provider": source["provider"],
        "repositoryId": str(root.get("id") or ""),
        "streams": {
            "git_refs": [
                _stream("branches", base + "/branches?limit=100", "git_refs",
                        sourcePrefix="branch", idFields=("name",)),
                _stream("tags", base + "/tags?limit=100", "git_refs",
                        sourcePrefix="tag", idFields=("name",)),
            ],
            "issues": [_stream(
                "issues", base + "/issues?state=all&type=issues&limit=100",
                "issues", sourcePrefix="issue", idFields=("id", "number"),
            )],
            "pull_requests": [_stream(
                "pull_requests", base + "/pulls?state=all&limit=100",
                "pull_requests", sourcePrefix="pull_request",
                idFields=("id", "number"),
            )],
            "reviews": review_streams,
            "releases": [_stream(
                "releases", base + "/releases?limit=100", "releases",
                sourcePrefix="release", idFields=("id", "tag_name"),
            )],
            "documentation": [_stream(
                "readme", base + "/contents/README.md?ref=" + branch,
                "documentation", shape="single", optional=True,
                sourcePrefix="document", idFields=("path", "sha"),
            )],
            "labels": [_stream(
                "labels", base + "/labels?limit=100", "labels",
                sourcePrefix="label", idFields=("id", "name"),
            )],
            "users": [_stream(
                "contributors", base + "/contributors?limit=100", "users",
                sourcePrefix="user", idFields=("id", "login", "username"),
            )],
            "collaboration_history": [_stream(
                "issue_comments", base + "/issues/comments?limit=100",
                "collaboration_history", sourcePrefix="issue_comment",
                idFields=("id",),
            )],
        },
        "unsupported": [
            "%s wiki pages are a separate Git repository and require a "
            "separate mirror operation." % _GITEA_API_LABELS[source["provider"]],
        ],
    }


_GITEA_API_LABELS = {"codeberg": "Codeberg", "gitea": "Gitea"}

_ADAPTERS = {
    "github": _github_adapter,
    "gitlab": _gitlab_adapter,
    "codeberg": _gitea_api_adapter,
    "gitea": _gitea_api_adapter,
}


def provider_adapter(source, root, context=None):
    """Return one provider capability manifest for the current job state."""
    provider = str((source or {}).get("provider") or "")
    factory = _ADAPTERS.get(provider)
    if not factory:
        raise MigrationContractError("unsupported_provider")
    if not isinstance(root, dict) or not str(root.get("id") or ""):
        raise MigrationContractError("invalid_provider_repository")
    adapter = factory(source, root, context or {})
    missing = [phase for phase in PHASES if phase not in adapter["streams"]]
    if missing:
        raise MigrationContractError(
            "adapter_missing_capabilities:" + ",".join(missing))
    return adapter


def stream_path(stream, page):
    """Append an opaque numeric page without modifying provider-owned cursors."""
    path = str(stream.get("path") or "")
    if not path.startswith("/"):
        raise MigrationContractError("invalid_adapter_path")
    page = max(1, int(page or 1))
    parameter = str(stream.get("pageParameter") or "page")
    separator = "&" if "?" in path else "?"
    return "%s%s%s=%d" % (path, separator, quote(parameter, safe=""), page)


def page_items(stream, data):
    shape = stream.get("shape") or "list"
    if shape == "single":
        if not isinstance(data, dict):
            raise MigrationContractError("provider_page_invalid_shape")
        return [data]
    if not isinstance(data, list):
        raise MigrationContractError("provider_page_invalid_shape")
    return data


def next_page(stream, headers, item_count, current_page):
    """Interpret GitHub, GitLab, and Forgejo pagination consistently."""
    if stream.get("shape") == "single":
        return 0
    lowered = {
        str(key).lower(): str(value or "")
        for key, value in (headers or {}).items()
    }
    next_value = lowered.get("x-next-page", "").strip()
    if next_value.isdigit() and int(next_value) > int(current_page):
        return int(next_value)
    link = lowered.get("link", "")
    if link and _NEXT_LINK_RE.search(link):
        return int(current_page) + 1
    if int(item_count) >= int(stream.get("pageSize") or PAGE_SIZE):
        return int(current_page) + 1
    return 0


def pull_context(records):
    """Return stable provider pull numbers from preserved pull envelopes."""
    output = set()
    for record in records or []:
        payload = record.get("providerPayload") if isinstance(record, dict) else None
        value = _first(payload, ("number", "iid"))
        if value not in (None, ""):
            output.add(str(value))
    return sorted(output, key=lambda value: (not value.isdigit(), int(value) if value.isdigit() else value))


def relationship_context(pull_records, issue_records):
    return {
        "pull_requests": pull_context(pull_records),
        "issues": pull_context(issue_records),
    }


def _release_assets(payload):
    """Return the provider-declared assets without changing their payload.

    GitHub/Forgejo return a list while GitLab nests links under ``assets``.
    This deliberately only *reads* those values: ``providerPayload`` remains
    the byte-for-byte JSON fidelity layer, and this projection is used solely
    to say precisely what preservation evidence is still absent.
    """
    assets = payload.get("assets") if isinstance(payload, dict) else None
    if isinstance(assets, list):
        return [asset for asset in assets if isinstance(asset, dict)]
    if isinstance(assets, dict):
        links = assets.get("links")
        if isinstance(links, list):
            return [asset for asset in links if isinstance(asset, dict)]
    return []


def _release_asset_name(asset, index):
    return _clean_string(
        _first(asset, ("name", "label", "filepath", "file_name")), 255
    ) or ("asset-%d" % (index + 1))


def _release_asset_digest(asset):
    # Providers use several spellings.  A digest in a provider response is
    # source metadata, not proof that the bytes landed in ForkMesh storage.
    value = _first(asset, (
        "sha256", "sha256sum", "digest", "checksum", "checksum_sha256",
    ))
    value = _clean_string(value, 200).lower()
    if value.startswith("sha256:"):
        value = value[7:]
    return value if re.fullmatch(r"[0-9a-f]{64}", value) else ""


def release_coverage(records):
    """Produce a loss-detecting release-retention report.

    A provider release object is useful source metadata, but it is not a
    downloadable release until its tag, bytes, integrity material, provenance,
    and documentation have each been retained.  Keep the report deterministic
    because it is included in the signed migration state receipt.

    The current provider-object transfer retains JSON envelopes only.  Thus
    every provider-declared binary is reported as missing bytes rather than
    being silently promoted to an available ForkMesh artifact.  Consumers can
    use the stable source id/name entries as a concrete recovery checklist.
    """
    tags = set()
    releases = []
    documents = []
    for record in records or []:
        if not isinstance(record, dict):
            continue
        kind = str(record.get("objectType") or "")
        payload = record.get("providerPayload")
        payload = payload if isinstance(payload, dict) else {}
        source = record.get("source")
        source = source if isinstance(source, dict) else {}
        source_id = _clean_string(source.get("id"), 400)
        if kind == "git_refs" and source_id.startswith("tag:"):
            name = _clean_string(payload.get("name"), 160)
            if name:
                tags.add(name)
        elif kind == "releases":
            tag = _clean_string(_first(payload, ("tag_name", "tag")), 160)
            releases.append((source_id, tag, payload))
        elif kind == "documentation":
            path = _clean_string(_first(payload, ("path", "file_path")), 1000)
            if path:
                documents.append(path)

    missing_tags = []
    missing_artifacts = []
    missing_checksums = []
    missing_provenance = []
    missing_manifests = []
    for source_id, tag, payload in releases:
        release_label = tag or source_id or "unknown-release"
        if not tag or tag not in tags:
            missing_tags.append(release_label)
        # A provider release JSON is metadata, not a ForkMesh signed manifest.
        missing_manifests.append(release_label)
        assets = _release_assets(payload)
        for index, asset in enumerate(assets):
            name = _release_asset_name(asset, index)
            entry = {"release": release_label, "name": name}
            missing_artifacts.append(entry)
            if not _release_asset_digest(asset):
                missing_checksums.append(entry)
            # An external URL or provider-side digest does not prove who built
            # an artifact.  A retained attestation/SLSA statement is required.
            missing_provenance.append(entry)

    # Documentation adapters currently retain the requested README only.  Do
    # not fabricate a complete documentation-site inventory from that one
    # object; expose the known retained paths and mark site discovery absent.
    return {
        "schema": "forkmesh-release-coverage-v1",
        "retainedDocumentation": sorted(set(documents)),
        "documentationSiteInventory": "unavailable",
        "missingTags": sorted(set(missing_tags)),
        "missingArtifacts": sorted(
            missing_artifacts, key=lambda item: (item["release"], item["name"])),
        "missingChecksums": sorted(
            missing_checksums, key=lambda item: (item["release"], item["name"])),
        "missingProvenance": sorted(
            missing_provenance, key=lambda item: (item["release"], item["name"])),
        "missingManifests": sorted(set(missing_manifests)),
        "missingDocumentationAssets": [],
    }


def state_document(repository_id, provider_repository_id, object_rows,
                   unsupported, skipped=None, skipped_overflow=0,
                   records=None):
    """Canonical state excludes run timestamps/counters so reruns converge."""
    objects = sorted(
        (
            {
                "objectType": str(row.get("object_type") or row.get("objectType") or ""),
                "sourceKey": str(row.get("source_key") or row.get("sourceKey") or ""),
                "payloadHash": str(row.get("payload_hash") or row.get("payloadHash") or ""),
            }
            for row in object_rows or []
        ),
        key=lambda row: (row["objectType"], row["sourceKey"]),
    )
    counts = {kind: 0 for kind in OBJECT_TYPES}
    for row in objects:
        if row["objectType"] in counts:
            counts[row["objectType"]] += 1
    return {
        "schema": STATE_SCHEMA,
        "repositoryId": str(repository_id or ""),
        "providerRepositoryId": str(provider_repository_id or ""),
        "objects": objects,
        "counts": counts,
        "unsupported": sorted(set(str(value) for value in unsupported or [])),
        "skipped": sorted(
            list(skipped or []), key=lambda value: canonical_json(value)),
        "skippedOverflow": max(0, int(skipped_overflow or 0)),
        # This is intentionally part of the signed state rather than mutable
        # job telemetry. A completed migration must never be mistaken for a
        # complete, independently recoverable release archive.
        "releaseCoverage": release_coverage(records),
    }


def state_hash(document):
    return "sha256:" + hashlib.sha256(canonical_json(document)).hexdigest()


def percentile(values, percentile_value):
    """Nearest-rank percentile used by the public setup p50/p95 metric."""
    samples = sorted(max(0, int(value)) for value in values)
    if not samples:
        return None
    rank = max(1, math.ceil((float(percentile_value) / 100.0) * len(samples)))
    return samples[min(len(samples), rank) - 1]


__all__ = (
    "MAX_OBJECT_BYTES", "MigrationContractError", "OBJECT_TYPES", "PHASES",
    "SCHEMA", "STATE_SCHEMA", "canonical_json", "migration_id",
    "next_page", "normalize_object", "object_key", "page_items",
    "percentile", "provider_adapter", "pull_context", "relationship_context",
    "release_coverage",
    "sha256_id",
    "state_document", "state_hash", "stream_path",
)
