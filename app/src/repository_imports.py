"""External repository imports, stubs, invitations, and logo helpers.

The relay keeps external-provider metadata separate from ``repositories``.
That separation is intentional: a GitHub/GitLab/Codeberg/Gitea listing is never cloneable
through ForkMesh until an independently verified mirror is linked to it.

This module is stdlib-only.  The Worker-specific adapter in ``entry.py``
injects D1, encryption, session, HTTP, and email functions into
``RepositoryImportService``.  Keeping provider normalization and policy here
makes the privacy and authorization rules directly unit-testable.

A private provider import is owner-only job metadata, not a materialized
ForkMesh repository.  It deliberately does not inherit ``repo_shares`` access:
the normal repository ACL applies only after an owner publishes/materializes a
native repository record.
"""

import base64
import binascii
import hashlib
import hmac
import html
import re
from urllib.parse import parse_qs, quote, unquote, urlparse


PROVIDERS = ("github", "gitlab", "codeberg", "gitea")
GITEA_API_HOSTS = {"codeberg": "codeberg.org", "gitea": "gitea.com"}
GITEA_API_PROVIDERS = tuple(GITEA_API_HOSTS)
PROVIDER_LABELS = {
    "github": "GitHub",
    "gitlab": "GitLab",
    "codeberg": "Codeberg",
    "gitea": "Gitea",
}
IMPORT_MODES = ("stub", "import")
REPOSITORY_STATUSES = (
    "external_repository",
    "stub_only",
    "mirror_requested",
    "partially_mirrored",
    "actively_mirrored",
    "mirror_unavailable",
    "archived",
)
STATUS_LABELS = {
    "external_repository": "External repository",
    "stub_only": "Stub only",
    "mirror_requested": "Mirror requested",
    "partially_mirrored": "Partially mirrored",
    "actively_mirrored": "Actively mirrored",
    "mirror_unavailable": "Mirror unavailable",
    "archived": "Archived",
}
MIRRORED_STATUSES = frozenset({"partially_mirrored", "actively_mirrored"})
INVITATION_BASES = (
    "public_for_invitations",
    "prior_consent",
    "owner_supplied",
)
OWNER_PROVENANCE_SOURCE_TYPES = (
    "repository_owner_contact",
    "organization_directory",
    "offline_address_book",
)
INVITATION_DAILY_LIMIT = 20
INVITATION_REPO_DAILY_LIMIT = 10
INVITATION_RECIPIENT_COOLDOWN_MS = 30 * 24 * 60 * 60 * 1000
PROVIDER_ADMIN_PROOF_TTL_MS = 24 * 60 * 60 * 1000
MAX_PUBLIC_IMPORTS = 200
MAX_PROVIDER_TOKEN = 2048
MAX_PROVIDER_BODY = 1024 * 1024
MAX_LOGO_BYTES = 256 * 1024
MAX_SUGGESTIONS_PER_REPOSITORY = 50
MAX_PENDING_LOGO_SUGGESTIONS_PER_PROPOSER = 5
MAX_INVITATIONS_PER_REPOSITORY = 500
MAX_NAMESPACE_REPOSITORIES = 200
# Bulk discovery is one request per page, so the page budget is what bounds a
# namespace walk's cost on the relay's free plan. Four full pages already reach
# MAX_NAMESPACE_REPOSITORIES; anything beyond that is reported as incomplete
# rather than silently truncated.
NAMESPACE_PAGE_SIZE = 50
NAMESPACE_MAX_PAGES = 4
MAX_MIGRATION_REPORT_EVENTS = 500

_SEGMENT_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}$")
_IMPORT_ID_RE = re.compile(r"^ext_[0-9a-f]{24}$")
_MIGRATION_ID_RE = re.compile(r"^mig_[0-9a-f]{24}$")
_MIGRATION_SOURCE_KEY_RE = re.compile(r"^[0-9a-f]{64}$")
_RECORD_ID_RE = re.compile(r"^[a-z]+_[0-9a-f]{24}$")
_EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$"
)
_PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
_JPEG_MAGIC = b"\xff\xd8\xff"
_WEBP_RIFF = b"RIFF"

_RESERVATION_ERRORS = {
    "logo_pending_proposer_limit": (
        "too_many_logo_suggestions_by_proposer", 429),
    "logo_community_repository_limit": (
        "too_many_logo_suggestions", 429),
    "invitation_recipient_opted_out": ("recipient_opted_out", 409),
    "invitation_recipient_cooldown": ("recipient_invited_recently", 409),
    "invitation_daily_limit": ("daily_invitation_limit", 429),
    "invitation_repository_daily_limit": (
        "repository_invitation_limit", 429),
    "invitation_repository_history_limit": (
        "too_many_repository_invitations", 429),
    "invitation_provenance_invalid": (
        "invitation_provenance_invalid", 409),
}


class ProviderSourceError(ValueError):
    """The supplied URL is not a supported canonical provider repository."""


class ProviderRequestError(RuntimeError):
    """A provider API request could not produce an importable snapshot."""

    def __init__(self, code, status=400, retry_after=""):
        super().__init__(code)
        self.code = code
        self.status = int(status)
        self.retry_after = str(retry_after or "")


def _repository_migration_module():
    """Load the adapter core lazily in the Worker and direct module tests."""
    import repository_migration
    return repository_migration


def _repository_migration_report_module():
    """Load the signed migration report builder lazily."""
    import repository_migration_report
    return repository_migration_report


def clean_text(value, limit=240):
    if not isinstance(value, str):
        return ""
    return " ".join(value.strip().split())[:limit]


def reservation_error(exc):
    """Map only allowlisted SQLite trigger markers to public API errors."""
    message = str(exc or "").lower()
    for marker, result in _RESERVATION_ERRORS.items():
        if marker in message:
            return result
    return None


def clean_multiline(value, limit=1000):
    if not isinstance(value, str):
        return ""
    return value.replace("\x00", "").strip()[:limit]


def clean_provider_token(value):
    """Return a bounded request-only token, rejecting control characters."""
    if not isinstance(value, str):
        return ""
    token = value.strip()
    if not token:
        return ""
    if len(token) > MAX_PROVIDER_TOKEN:
        raise ProviderSourceError("provider_token_too_long")
    if any(ord(char) < 32 or ord(char) == 127 for char in token):
        raise ProviderSourceError("invalid_provider_token")
    return token


def _safe_segment(value):
    value = unquote(str(value or "")).strip()
    return value if _SEGMENT_RE.fullmatch(value) else ""


def parse_provider_source(value):
    """Parse a public web/clone URL for GitHub, GitLab, Codeberg, or Gitea.

    Only the primary provider hosts are accepted.  API URLs, credentials,
    query strings, fragments, arbitrary ports, and ambiguous extra paths are
    rejected, preventing this metadata feature from becoming an SSRF proxy.
    """
    raw = str(value or "").strip()
    if not raw or len(raw) > 2048:
        raise ProviderSourceError("source_url_required")
    if "://" not in raw:
        raw = "https://" + raw
    parsed = urlparse(raw)
    if parsed.scheme != "https" or parsed.username or parsed.password:
        raise ProviderSourceError("https_provider_url_required")
    host = (parsed.hostname or "").lower().rstrip(".")
    try:
        port = parsed.port
    except ValueError as exc:
        raise ProviderSourceError("invalid_provider_url") from exc
    if port not in (None, 443) or parsed.query or parsed.fragment:
        raise ProviderSourceError("invalid_provider_url")
    path = parsed.path.strip("/")
    if path.endswith(".git"):
        path = path[:-4]
    parts = [_safe_segment(part) for part in path.split("/") if part]
    if not parts or any(not part for part in parts):
        raise ProviderSourceError("invalid_repository_path")
    if host in ("github.com", "www.github.com"):
        if len(parts) != 2:
            raise ProviderSourceError("github_owner_and_repository_required")
        owner, name = parts
        canonical = "https://github.com/%s/%s" % (
            quote(owner, safe=""), quote(name, safe=""))
        return {
            "provider": "github",
            "host": "github.com",
            "owner": owner,
            "name": name,
            "fullName": owner + "/" + name,
            "canonicalUrl": canonical,
            "metadataPath": "/repos/%s/%s" % (
                quote(owner, safe=""), quote(name, safe="")),
        }
    if host in ("gitlab.com", "www.gitlab.com"):
        if len(parts) < 2 or len(parts) > 20:
            raise ProviderSourceError("gitlab_namespace_and_repository_required")
        owner = "/".join(parts[:-1])
        name = parts[-1]
        full_name = "/".join(parts)
        canonical = "https://gitlab.com/" + "/".join(
            quote(part, safe="") for part in parts)
        return {
            "provider": "gitlab",
            "host": "gitlab.com",
            "owner": owner,
            "name": name,
            "fullName": full_name,
            "canonicalUrl": canonical,
            "metadataPath": "/projects/%s?license=true" % quote(
                full_name, safe=""),
        }
    gitea_provider = _gitea_api_provider(host)
    if gitea_provider:
        provider_host = GITEA_API_HOSTS[gitea_provider]
        if len(parts) != 2:
            raise ProviderSourceError(
                gitea_provider + "_owner_and_repository_required")
        owner, name = parts
        canonical = "https://%s/%s/%s" % (
            provider_host, quote(owner, safe=""), quote(name, safe=""))
        return {
            "provider": gitea_provider,
            "host": provider_host,
            "owner": owner,
            "name": name,
            "fullName": owner + "/" + name,
            "canonicalUrl": canonical,
            "metadataPath": "/repos/%s/%s" % (
                quote(owner, safe=""), quote(name, safe="")),
        }
    raise ProviderSourceError("unsupported_provider")


def _gitea_api_provider(host):
    for provider, provider_host in GITEA_API_HOSTS.items():
        if host in (provider_host, "www." + provider_host):
            return provider
    return ""


def parse_provider_namespace(value):
    """Parse an organization/group URL into a bulk-import namespace.

    A GitLab organization is a group, and a group may nest subgroups, so its
    namespace is a slash-joined path and discovery starts at whichever level
    was given.  A Codeberg or Gitea organization (or user) is a single-segment
    profile path.  The host, scheme, credential, port, query, and segment rules match
    ``parse_provider_source`` so bulk import cannot become an SSRF proxy
    either.
    """
    raw = str(value or "").strip()
    if not raw or len(raw) > 2048:
        raise ProviderSourceError("source_url_required")
    if "://" not in raw:
        raw = "https://" + raw
    parsed = urlparse(raw)
    if parsed.scheme != "https" or parsed.username or parsed.password:
        raise ProviderSourceError("https_provider_url_required")
    host = (parsed.hostname or "").lower().rstrip(".")
    try:
        port = parsed.port
    except ValueError as exc:
        raise ProviderSourceError("invalid_provider_url") from exc
    if port not in (None, 443) or parsed.query or parsed.fragment:
        raise ProviderSourceError("invalid_provider_url")
    parts = [_safe_segment(part) for part in parsed.path.strip("/").split("/")
             if part]
    gitea_provider = _gitea_api_provider(host)
    if gitea_provider:
        if len(parts) != 1 or not parts[0]:
            raise ProviderSourceError(gitea_provider + "_namespace_required")
        return {"provider": gitea_provider, "namespace": parts[0]}
    if host in ("gitlab.com", "www.gitlab.com"):
        if not parts or len(parts) > 20 or any(not part for part in parts):
            raise ProviderSourceError("gitlab_group_required")
        return {"provider": "gitlab", "namespace": "/".join(parts)}
    raise ProviderSourceError("unsupported_namespace_provider")


def namespace_discovery_paths(provider, namespace, page):
    """List endpoints that enumerate one namespace's repositories, in order.

    GitLab needs two candidates because a namespace segment names a group for
    an organization but a user for a personal account, and the two live on
    different collections; the first that answers is reused for later pages.
    ``include_subgroups`` is what makes a group import cover the *entire*
    organization rather than only the projects sitting at its top level.
    """
    encoded = quote(namespace, safe="")
    if provider in GITEA_API_PROVIDERS:
        return ["/users/%s/repos?limit=%d&page=%d"
                % (encoded, NAMESPACE_PAGE_SIZE, page)]
    if provider == "gitlab":
        paths = [
            "/groups/%s/projects?include_subgroups=true&archived=false"
            "&order_by=path&sort=asc&per_page=%d&page=%d"
            % (encoded, NAMESPACE_PAGE_SIZE, page),
        ]
        if "/" not in namespace:
            # Only a top-level segment can name a user; a subgroup path never
            # resolves on the user collection, so do not spend a request on it.
            paths.append(
                "/users/%s/projects?archived=false&order_by=path&sort=asc"
                "&per_page=%d&page=%d" % (encoded, NAMESPACE_PAGE_SIZE, page))
        return paths
    raise ProviderSourceError("unsupported_namespace_provider")


def namespace_repository_url(provider, row, token_present=False):
    """Return the canonical web URL for one row of a namespace listing.

    Non-public entries are skipped unless a token was supplied: without a
    credential the provider only lists public repositories anyway, and with one
    the caller has already proven it may see the rest (the import itself
    re-verifies that access before recording a private repository).
    """
    if not isinstance(row, dict):
        return ""
    if provider == "gitlab":
        public = str(row.get("visibility") or "").lower() == "public"
        candidate = row.get("web_url") or row.get("http_url_to_repo")
    else:
        public = not row.get("private")
        candidate = row.get("html_url") or row.get("clone_url")
    if not public and not token_present:
        return ""
    return candidate if isinstance(candidate, str) else ""


def namespace_contains(namespace, owner):
    """True when a discovered repository really sits under the namespace.

    Provider listings can include entries from elsewhere (forks, shared
    projects), and a GitLab subgroup project reports the full subgroup path as
    its owner, so a prefix match is required rather than equality.
    """
    namespace = str(namespace or "").casefold()
    owner = str(owner or "").casefold()
    return bool(namespace) and (
        owner == namespace or owner.startswith(namespace + "/"))


def provider_api_origin(provider):
    if provider == "github":
        return "https://api.github.com"
    if provider == "gitlab":
        return "https://gitlab.com/api/v4"
    if provider in GITEA_API_PROVIDERS:
        return "https://%s/api/v1" % GITEA_API_HOSTS[provider]
    raise ProviderSourceError("unsupported_provider")


def provider_extra_paths(source, root):
    """Metadata-only API paths; no blob, archive, raw-file, or source endpoint."""
    if source["provider"] in ("github",) + GITEA_API_PROVIDERS:
        base = "/repos/%s/%s" % (
            quote(source["owner"], safe=""), quote(source["name"], safe=""))
        default_branch = clean_text(root.get("default_branch"), 160)
        paths = {
            "topics": base + "/topics?per_page=100",
            "languages": base + "/languages",
            # Git's tree API returns path/type metadata, never blob contents.
            # This gives the local logo generator and repository classifier an
            # owner-authorized, non-sensitive structural summary.
            "structure": (
                base + "/git/trees/" + quote(default_branch, safe="")
                + "?recursive=1"
            ) if default_branch else "",
            "branches": base + "/branches?per_page=100",
            "contributors": base + "/contributors?per_page=100&anon=1",
            "commits": base + "/commits?per_page=30",
            "issues": base + "/issues?state=all&per_page=50",
            "pullRequests": base + "/pulls?state=all&per_page=50",
            "releases": base + "/releases?per_page=30",
        }
        if source["provider"] in GITEA_API_PROVIDERS:
            paths.update({
                "topics": base + "/topics",
                "structure": (
                    base + "/git/trees/" + quote(default_branch, safe="")
                    + "?recursive=true"
                ) if default_branch else "",
                "branches": base + "/branches?limit=100",
                "contributors": base + "/contributors?limit=100",
                "commits": base + "/commits?limit=30",
                "issues": base + "/issues?state=all&limit=50&type=issues",
                "pullRequests": base + "/pulls?state=all&limit=50",
                "releases": base + "/releases?limit=30",
            })
        return paths
    project_id = root.get("id")
    if not isinstance(project_id, int) and not str(project_id or "").isdigit():
        raise ProviderRequestError("invalid_provider_response", status=502)
    base = "/projects/" + quote(str(project_id), safe="")
    return {
        "languages": base + "/languages",
        "structure": base + "/repository/tree?recursive=true&per_page=100",
        "branches": base + "/repository/branches?per_page=100",
        "contributors": base + "/repository/contributors?per_page=100",
        "commits": base + "/repository/commits?per_page=30",
        "issues": base + "/issues?scope=all&per_page=50",
        "pullRequests": base + "/merge_requests?scope=all&per_page=50",
        "releases": base + "/releases?per_page=30",
    }


def _repository_shape(extras):
    """Return bounded path labels, framework hints, and a broad category.

    Provider tree responses are metadata-only.  ForkMesh deliberately does not
    fetch manifests or source code merely to create a logo.
    """
    raw = extras.get("structure")
    if isinstance(raw, dict):
        raw = raw.get("tree")
    paths = []
    for item in _bounded_list(raw, 500):
        if not isinstance(item, dict):
            continue
        path = clean_text(item.get("path"), 240).strip("/")
        item_type = clean_text(item.get("type"), 20).lower()
        if not path or item_type in ("commit", "submodule"):
            continue
        paths.append(path)
    top_level = sorted({
        path.split("/", 1)[0] for path in paths if path.split("/", 1)[0]
    })[:24]
    lowered = {path.lower() for path in paths}
    frameworks = set()
    framework_markers = {
        "Next.js": ("next.config.js", "next.config.mjs", "next.config.ts"),
        "Angular": ("angular.json",),
        "Vite": ("vite.config.js", "vite.config.ts", "vite.config.mjs"),
        "Django": ("manage.py",),
        "Flutter": ("pubspec.yaml", "lib/main.dart"),
        "Qt": ("cmakelists.txt", ".pro"),
        "Rust": ("cargo.toml",),
        "Go": ("go.mod",),
    }
    basenames = {path.rsplit("/", 1)[-1] for path in lowered}
    for framework, markers in framework_markers.items():
        if any(
                marker in lowered
                or marker in basenames
                or (marker.startswith(".") and any(
                    name.endswith(marker) for name in basenames))
                for marker in markers):
            frameworks.add(framework)
    topics = extras.get("topics")
    if isinstance(topics, dict):
        topics = topics.get("names")
    for topic in _bounded_list(topics, 40):
        label = clean_text(topic, 80)
        known = {
            "react": "React",
            "vue": "Vue",
            "svelte": "Svelte",
            "fastapi": "FastAPI",
            "flask": "Flask",
            "rails": "Rails",
            "laravel": "Laravel",
        }.get(label.lower())
        if known:
            frameworks.add(known)
    category = "library"
    if any(path.startswith((".github/workflows/", ".gitlab-ci")) for path in lowered):
        category = "developer tooling"
    if any(path.startswith(("ios/", "android/")) for path in lowered) or \
            "pubspec.yaml" in basenames:
        category = "mobile application"
    if any(name in basenames for name in (
            "next.config.js", "next.config.mjs", "angular.json",
            "vite.config.js", "vite.config.ts", "manage.py")):
        category = "web application"
    if any(path.startswith(("infra/", "terraform/", "deploy/"))
           for path in lowered):
        category = "infrastructure"
    return {
        "fileStructure": top_level,
        "frameworks": sorted(frameworks)[:12],
        "projectCategory": category,
    }


def external_repository_id(provider, external_id, full_name=""):
    identity = clean_text(external_id, 160)
    if not identity:
        identity = clean_text(full_name, 300).lower()
    stable = "%s\n%s\n%s" % (
        clean_text(provider, 20).lower(),
        identity,
        "" if external_id else clean_text(full_name, 300).lower(),
    )
    return "ext_" + hashlib.sha256(stable.encode()).hexdigest()[:24]


def opaque_record_id(prefix, *parts):
    payload = "\n".join(str(part or "") for part in parts)
    return prefix + "_" + hashlib.sha256(payload.encode()).hexdigest()[:24]


def valid_import_id(value):
    return bool(_IMPORT_ID_RE.fullmatch(str(value or "")))


def valid_record_id(value):
    return bool(_RECORD_ID_RE.fullmatch(str(value or "")))


def _bounded_list(value, limit):
    return list(value[:limit]) if isinstance(value, list) else []


def _api_user(value):
    value = value if isinstance(value, dict) else {}
    return {
        "login": clean_text(
            value.get("login") or value.get("username") or value.get("name"), 120),
        "profileUrl": clean_text(
            value.get("html_url") or value.get("web_url"), 500),
        "avatarUrl": clean_text(value.get("avatar_url"), 500),
        "type": clean_text(value.get("type") or value.get("state"), 40),
    }


def _iso_date(value):
    return clean_text(value, 40)


def _github_permissions(root, token_present):
    permissions = root.get("permissions")
    permissions = permissions if isinstance(permissions, dict) else {}
    private = bool(root.get("private"))
    can_admin = bool(
        token_present and (
            permissions.get("admin") or permissions.get("maintain")))
    return {
        "privateAccessVerified": bool(private and token_present),
        "administratorVerified": can_admin,
        "canInviteContributors": can_admin,
    }


def _gitlab_permissions(root, token_present):
    permissions = root.get("permissions")
    permissions = permissions if isinstance(permissions, dict) else {}
    levels = []
    for key in ("project_access", "group_access"):
        access = permissions.get(key)
        if isinstance(access, dict):
            try:
                levels.append(int(access.get("access_level") or 0))
            except (TypeError, ValueError):
                pass
    level = max(levels or [0])
    private = str(root.get("visibility") or "").lower() != "public"
    can_admin = bool(token_present and level >= 40)
    return {
        "privateAccessVerified": bool(private and token_present),
        "administratorVerified": can_admin,
        "canInviteContributors": can_admin,
    }


def _github_metadata(root, extras):
    languages = extras.get("languages")
    languages = languages if isinstance(languages, dict) else {}
    language_items = {}
    for key, value in list(languages.items())[:50]:
        name = clean_text(key, 80)
        try:
            amount = max(0, int(value))
        except (TypeError, ValueError):
            amount = 0
        if name:
            language_items[name] = amount
    branches = []
    for item in _bounded_list(extras.get("branches"), 100):
        if not isinstance(item, dict):
            continue
        commit = item.get("commit")
        commit = commit if isinstance(commit, dict) else {}
        branches.append({
            "name": clean_text(item.get("name"), 160),
            "protected": bool(item.get("protected")),
            "commitSha": clean_text(commit.get("sha"), 64),
        })
    contributors = []
    for item in _bounded_list(extras.get("contributors"), 100):
        if not isinstance(item, dict):
            continue
        user = _api_user(item)
        try:
            contributions = max(0, int(item.get("contributions") or 0))
        except (TypeError, ValueError):
            contributions = 0
        contributors.append({**user, "contributions": contributions})
    commits = []
    for item in _bounded_list(extras.get("commits"), 30):
        if not isinstance(item, dict):
            continue
        commit = item.get("commit")
        commit = commit if isinstance(commit, dict) else {}
        author = commit.get("author")
        author = author if isinstance(author, dict) else {}
        commits.append({
            "sha": clean_text(item.get("sha"), 64),
            "message": clean_multiline(commit.get("message"), 500),
            "author": _api_user(item.get("author")),
            "authorName": clean_text(author.get("name"), 120),
            "committedAt": _iso_date(author.get("date")),
            "url": clean_text(item.get("html_url"), 500),
        })
    issues = []
    for item in _bounded_list(extras.get("issues"), 50):
        if not isinstance(item, dict) or item.get("pull_request"):
            continue
        issues.append({
            "number": int(item.get("number") or 0),
            "title": clean_text(item.get("title"), 300),
            "state": clean_text(item.get("state"), 20),
            "author": _api_user(item.get("user")),
            "createdAt": _iso_date(item.get("created_at")),
            "updatedAt": _iso_date(item.get("updated_at")),
            "url": clean_text(item.get("html_url"), 500),
        })
    pulls = []
    for item in _bounded_list(extras.get("pullRequests"), 50):
        if not isinstance(item, dict):
            continue
        pulls.append({
            "number": int(item.get("number") or 0),
            "title": clean_text(item.get("title"), 300),
            "state": clean_text(item.get("state"), 20),
            "draft": bool(item.get("draft")),
            "author": _api_user(item.get("user")),
            "createdAt": _iso_date(item.get("created_at")),
            "updatedAt": _iso_date(item.get("updated_at")),
            "url": clean_text(item.get("html_url"), 500),
        })
    releases = []
    for item in _bounded_list(extras.get("releases"), 30):
        if not isinstance(item, dict):
            continue
        releases.append({
            "tag": clean_text(item.get("tag_name"), 160),
            "name": clean_text(item.get("name"), 240),
            "draft": bool(item.get("draft")),
            "prerelease": bool(item.get("prerelease")),
            "publishedAt": _iso_date(item.get("published_at")),
            "url": clean_text(item.get("html_url"), 500),
        })
    license_data = root.get("license")
    license_data = license_data if isinstance(license_data, dict) else {}
    organization = root.get("organization")
    organization = organization if isinstance(organization, dict) else {}
    root_owner = root.get("owner")
    root_owner = root_owner if isinstance(root_owner, dict) else {}
    if not organization and root_owner.get("type") == "Organization":
        organization = root_owner
    return {
        "description": clean_text(root.get("description"), 500),
        "license": {
            "spdxId": clean_text(license_data.get("spdx_id"), 80),
            "name": clean_text(license_data.get("name"), 160),
            "url": clean_text(license_data.get("html_url"), 500),
        },
        "topics": [
            clean_text(topic, 80) for topic in _bounded_list(
                (
                    extras.get("topics", {}).get("names")
                    if isinstance(extras.get("topics"), dict) else None
                ) or root.get("topics"),
                40,
            )
            if clean_text(topic, 80)
        ],
        "languages": language_items,
        "primaryLanguage": clean_text(root.get("language"), 80),
        "defaultBranch": clean_text(root.get("default_branch"), 160),
        "branches": branches,
        "contributors": contributors,
        "commits": commits,
        "issues": issues,
        "pullRequests": pulls,
        "releases": releases,
        "repositoryAvatarUrl": "",
        "organization": {
            "name": clean_text(
                organization.get("login") or organization.get("name"), 160),
            "url": clean_text(organization.get("html_url"), 500),
            "avatarUrl": clean_text(organization.get("avatar_url"), 500),
        },
        "counts": {
            "stars": int(root.get("stargazers_count") or 0),
            "forks": int(root.get("forks_count") or 0),
            "openIssues": int(root.get("open_issues_count") or 0),
            "watchers": int(root.get("subscribers_count") or 0),
        },
    }


def _gitlab_metadata(root, extras):
    languages = extras.get("languages")
    languages = languages if isinstance(languages, dict) else {}
    language_items = {}
    for key, value in list(languages.items())[:50]:
        name = clean_text(key, 80)
        try:
            # GitLab returns percentages.  Keep an integer basis-point value so
            # consumers do not lose deterministic precision in JSON/D1.
            amount = max(0, int(float(value) * 100))
        except (TypeError, ValueError):
            amount = 0
        if name:
            language_items[name] = amount
    branches = []
    for item in _bounded_list(extras.get("branches"), 100):
        if not isinstance(item, dict):
            continue
        commit = item.get("commit")
        commit = commit if isinstance(commit, dict) else {}
        branches.append({
            "name": clean_text(item.get("name"), 160),
            "protected": bool(item.get("protected")),
            "commitSha": clean_text(commit.get("id"), 64),
        })
    contributors = []
    for item in _bounded_list(extras.get("contributors"), 100):
        if not isinstance(item, dict):
            continue
        try:
            contributions = max(0, int(item.get("commits") or 0))
        except (TypeError, ValueError):
            contributions = 0
        contributors.append({
            # Some GitLab responses contain an email. Do not copy it into the
            # import record or use it as an invitation address.
            "login": clean_text(
                item.get("username") or item.get("name"), 120),
            "profileUrl": "",
            "avatarUrl": "",
            "type": "User",
            "contributions": contributions,
        })
    commits = []
    for item in _bounded_list(extras.get("commits"), 30):
        if not isinstance(item, dict):
            continue
        commits.append({
            "sha": clean_text(item.get("id"), 64),
            "message": clean_multiline(
                item.get("title") or item.get("message"), 500),
            "author": {
                "login": "",
                "profileUrl": "",
                "avatarUrl": "",
                "type": "User",
            },
            "authorName": clean_text(item.get("author_name"), 120),
            "committedAt": _iso_date(item.get("committed_date")),
            "url": clean_text(item.get("web_url"), 500),
        })
    issues = []
    for item in _bounded_list(extras.get("issues"), 50):
        if not isinstance(item, dict):
            continue
        issues.append({
            "number": int(item.get("iid") or 0),
            "title": clean_text(item.get("title"), 300),
            "state": clean_text(item.get("state"), 20),
            "author": _api_user(item.get("author")),
            "createdAt": _iso_date(item.get("created_at")),
            "updatedAt": _iso_date(item.get("updated_at")),
            "url": clean_text(item.get("web_url"), 500),
        })
    pulls = []
    for item in _bounded_list(extras.get("pullRequests"), 50):
        if not isinstance(item, dict):
            continue
        pulls.append({
            "number": int(item.get("iid") or 0),
            "title": clean_text(item.get("title"), 300),
            "state": clean_text(item.get("state"), 20),
            "draft": bool(item.get("draft") or item.get("work_in_progress")),
            "author": _api_user(item.get("author")),
            "createdAt": _iso_date(item.get("created_at")),
            "updatedAt": _iso_date(item.get("updated_at")),
            "url": clean_text(item.get("web_url"), 500),
        })
    releases = []
    for item in _bounded_list(extras.get("releases"), 30):
        if not isinstance(item, dict):
            continue
        released = item.get("released_at")
        releases.append({
            "tag": clean_text(item.get("tag_name"), 160),
            "name": clean_text(item.get("name"), 240),
            "draft": False,
            "prerelease": bool(item.get("upcoming_release")),
            "publishedAt": _iso_date(released),
            "url": clean_text(
                (item.get("_links") or {}).get("self")
                if isinstance(item.get("_links"), dict) else "", 500),
        })
    license_data = root.get("license")
    license_data = license_data if isinstance(license_data, dict) else {}
    namespace = root.get("namespace")
    namespace = namespace if isinstance(namespace, dict) else {}
    primary = ""
    if language_items:
        primary = max(language_items, key=lambda key: language_items[key])
    return {
        "description": clean_text(root.get("description"), 500),
        "license": {
            "spdxId": clean_text(
                license_data.get("key") or license_data.get("spdx_identifier"), 80),
            "name": clean_text(license_data.get("name"), 160),
            "url": clean_text(license_data.get("html_url"), 500),
        },
        "topics": [
            clean_text(topic, 80)
            for topic in _bounded_list(
                root.get("topics") or root.get("tag_list"), 40)
            if clean_text(topic, 80)
        ],
        "languages": language_items,
        "primaryLanguage": primary,
        "defaultBranch": clean_text(root.get("default_branch"), 160),
        "branches": branches,
        "contributors": contributors,
        "commits": commits,
        "issues": issues,
        "pullRequests": pulls,
        "releases": releases,
        "repositoryAvatarUrl": clean_text(root.get("avatar_url"), 500),
        "organization": {
            "name": clean_text(
                namespace.get("full_path") or namespace.get("name"), 160),
            "url": clean_text(namespace.get("web_url"), 500),
            "avatarUrl": clean_text(namespace.get("avatar_url"), 500),
        },
        "counts": {
            "stars": int(root.get("star_count") or 0),
            "forks": int(root.get("forks_count") or 0),
            "openIssues": int(root.get("open_issues_count") or 0),
            "watchers": 0,
        },
    }


def build_repository_record(
        source, root, extras, actor, mode, now_ms, token_present=False,
        provider_rate=None, incomplete=None):
    """Build a bounded provider-neutral record from metadata API responses."""
    if mode not in IMPORT_MODES:
        raise ProviderSourceError("invalid_import_mode")
    if not isinstance(root, dict):
        raise ProviderRequestError("invalid_provider_response", status=502)
    provider = source["provider"]
    if provider in ("github",) + GITEA_API_PROVIDERS:
        external_id = str(root.get("id") or "")
        full_name = clean_text(root.get("full_name"), 300) or source["fullName"]
        original_url = clean_text(root.get("html_url"), 500) or source["canonicalUrl"]
        visibility = "private" if bool(root.get("private")) else "public"
        permissions = _github_permissions(root, token_present)
        metadata = _github_metadata(root, extras)
        owner_data = _api_user(root.get("owner"))
        archived = bool(root.get("archived"))
        created_at = _iso_date(root.get("created_at"))
        provider_updated_at = _iso_date(root.get("updated_at"))
    elif provider == "gitlab":
        external_id = str(root.get("id") or "")
        full_name = clean_text(
            root.get("path_with_namespace"), 300) or source["fullName"]
        original_url = clean_text(root.get("web_url"), 500) or source["canonicalUrl"]
        visibility = (
            "public" if str(root.get("visibility") or "").lower() == "public"
            else "private")
        permissions = _gitlab_permissions(root, token_present)
        metadata = _gitlab_metadata(root, extras)
        namespace = root.get("namespace")
        namespace = namespace if isinstance(namespace, dict) else {}
        owner_data = {
            "login": clean_text(
                namespace.get("full_path") or namespace.get("name"), 120),
            "profileUrl": clean_text(namespace.get("web_url"), 500),
            "avatarUrl": clean_text(namespace.get("avatar_url"), 500),
            "type": clean_text(namespace.get("kind"), 40),
        }
        archived = bool(root.get("archived"))
        created_at = _iso_date(root.get("created_at"))
        provider_updated_at = _iso_date(root.get("last_activity_at"))
    else:
        raise ProviderSourceError("unsupported_provider")
    metadata.update(_repository_shape(extras))
    if not external_id:
        raise ProviderRequestError("invalid_provider_response", status=502)
    if visibility == "private" and not (
            token_present and permissions["privateAccessVerified"]):
        raise ProviderRequestError("private_authorization_required", status=403)
    full_parts = [part for part in full_name.split("/") if part]
    provider_owner = "/".join(full_parts[:-1]) or source["owner"]
    provider_name = full_parts[-1] if full_parts else source["name"]
    status = "archived" if archived else (
        "stub_only" if mode == "stub" else "external_repository")
    record_id = external_repository_id(provider, external_id, full_name)
    return {
        "id": record_id,
        "provider": provider,
        "providerHost": source["host"],
        "providerId": external_id,
        "providerOwner": clean_text(provider_owner, 240),
        "name": clean_text(provider_name, 100),
        "fullName": full_name,
        "originalUrl": original_url,
        "visibility": visibility,
        "isPrivate": visibility == "private",
        "status": status,
        "statusLabel": STATUS_LABELS[status],
        "mirrored": status in MIRRORED_STATUSES,
        "importMode": mode,
        "listedBy": clean_text(actor, 100).lower(),
        "createdAt": int(now_ms),
        "updatedAt": int(now_ms),
        "providerCreatedAt": created_at,
        "providerUpdatedAt": provider_updated_at,
        "authorization": {
            **permissions,
            "mode": (
                "private_token" if visibility == "private"
                else "provider_token" if token_present else "public_metadata"),
            "verifiedAt": int(now_ms) if token_present else 0,
            # Provider credentials are intentionally absent.  This field makes
            # the storage contract explicit to API consumers and audits.
            "credentialStored": False,
        },
        "owner": owner_data,
        "metadata": metadata,
        "providerRateLimit": provider_rate or {},
        "metadataIncomplete": list(incomplete or []),
        "attribution": {
            "provider": PROVIDER_LABELS[provider],
            "originalRepository": original_url,
            "api": provider_api_origin(provider),
            "termsUrl": (
                "https://docs.github.com/en/site-policy/github-terms/"
                "github-terms-of-service"
                if provider == "github"
                else "https://about.gitlab.com/terms/"
                if provider == "gitlab"
                else "https://about.gitea.com/terms-of-service/"
                if provider == "gitea"
                else "https://codeberg.org/Codeberg/org/src/branch/main/TermsOfUse.md"),
            "rateLimitPolicy": (
                "https://docs.github.com/rest/using-the-rest-api/"
                "rate-limits-for-the-rest-api"
                if provider == "github"
                else "https://docs.gitlab.com/administration/"
                "settings/user_and_ip_rate_limits/"
                if provider == "gitlab"
                else "https://docs.gitea.com/development/api-usage/"
                if provider == "gitea"
                else "https://docs.codeberg.org/getting-started/faq/"),
        },
        "ownershipNotice": (
            "External repository metadata. ForkMesh does not own or control "
            "this repository."),
        "mirrorNotice": (
            "This entry is not mirrored by ForkMesh."
            if status not in MIRRORED_STATUSES else
            "A separately verified ForkMesh mirror is linked to this entry."),
        "sourceCodeFetched": False,
        "privateSourceSentToExternalModel": False,
    }


def _rate_info(headers):
    headers = headers if isinstance(headers, dict) else {}
    return {
        "limit": clean_text(
            headers.get("x-ratelimit-limit") or headers.get("ratelimit-limit"), 30),
        "remaining": clean_text(
            headers.get("x-ratelimit-remaining")
            or headers.get("ratelimit-remaining"), 30),
        "reset": clean_text(
            headers.get("x-ratelimit-reset") or headers.get("ratelimit-reset"), 40),
        "retryAfter": clean_text(headers.get("retry-after"), 40),
    }


def _provider_error(result, has_token):
    status = int((result or {}).get("status") or 0)
    headers = (result or {}).get("headers")
    rate = _rate_info(headers)
    if status in (429,):
        return ProviderRequestError(
            "provider_rate_limited", 429, rate.get("retryAfter"))
    if status == 403 and (
            rate.get("remaining") == "0" or rate.get("retryAfter")):
        return ProviderRequestError(
            "provider_rate_limited", 429,
            rate.get("retryAfter"))
    if status in (401, 403):
        return ProviderRequestError("provider_authorization_failed", 403)
    if status == 404:
        return ProviderRequestError(
            "repository_not_found_or_authorization_required",
            404 if has_token else 400)
    return ProviderRequestError("provider_unavailable", 502)


async def fetch_provider_snapshot(provider_fetch, env, source, token):
    """Fetch a metadata-only provider snapshot while honoring rate signals."""
    token = clean_provider_token(token)
    result = await provider_fetch(
        env, source["provider"], source["metadataPath"], token)
    if not isinstance(result, dict) or not (
            200 <= int(result.get("status") or 0) < 300):
        raise _provider_error(result, bool(token))
    root = result.get("data")
    if not isinstance(root, dict):
        raise ProviderRequestError("invalid_provider_response", status=502)
    extras = {}
    incomplete = []
    rate = _rate_info(result.get("headers"))
    for key, path in provider_extra_paths(source, root).items():
        if not path:
            incomplete.append(key + ":unavailable")
            continue
        if rate.get("remaining") == "0":
            incomplete.append(key + ":rate_limited")
            continue
        extra = await provider_fetch(env, source["provider"], path, token)
        status = int((extra or {}).get("status") or 0)
        extra_rate = _rate_info((extra or {}).get("headers"))
        if any(extra_rate.values()):
            rate = extra_rate
        if 200 <= status < 300:
            data = extra.get("data")
            if isinstance(data, (dict, list)):
                extras[key] = data
            else:
                incomplete.append(key + ":invalid_response")
        elif status in (403, 429) and (
                extra_rate.get("remaining") == "0"
                or extra_rate.get("retryAfter") or status == 429):
            incomplete.append(key + ":rate_limited")
            # Do not make more requests after the provider tells us to stop.
            for remaining in provider_extra_paths(source, root):
                if remaining not in extras and not any(
                        item.startswith(remaining + ":") for item in incomplete):
                    incomplete.append(remaining + ":rate_limited")
            break
        else:
            incomplete.append(key + ":unavailable")
    return root, extras, rate, incomplete


def status_transition_allowed(current, target):
    transitions = {
        "external_repository": {
            "mirror_requested", "archived", "stub_only"},
        "stub_only": {
            "mirror_requested", "external_repository", "archived"},
        "mirror_requested": {
            "partially_mirrored", "actively_mirrored",
            "mirror_unavailable", "archived"},
        "partially_mirrored": {
            "actively_mirrored", "mirror_unavailable", "archived"},
        "actively_mirrored": {"mirror_unavailable", "archived"},
        "mirror_unavailable": {
            "mirror_requested", "partially_mirrored",
            "actively_mirrored", "archived"},
        "archived": {"external_repository", "stub_only"},
    }
    return target in transitions.get(current, set())


def normalize_email(value):
    email_value = str(value or "").strip().lower()
    if len(email_value) > 254 or not _EMAIL_RE.fullmatch(email_value):
        return ""
    return email_value


def provider_public_invitation_candidates(provider, extras):
    """Extract transient provider-public contributor contact evidence.

    The returned addresses are used only to derive keyed blind indexes and are
    never copied into the import record. GitHub evidence must use the explicit
    ``public_email`` response field; GitLab's public contributors response may
    use ``public_email`` or its documented ``email`` field.
    """
    extras = extras if isinstance(extras, dict) else {}
    candidates = []
    seen = set()
    for item in _bounded_list(extras.get("contributors"), 100):
        if not isinstance(item, dict):
            continue
        if provider == "github":
            email_value = normalize_email(item.get("public_email"))
            contributor = clean_text(item.get("login"), 120)
        elif provider == "gitlab":
            email_value = normalize_email(
                item.get("public_email") or item.get("email"))
            contributor = clean_text(
                item.get("username") or item.get("name"), 120)
        else:
            continue
        key = (contributor.casefold(), email_value)
        if not contributor or not email_value or key in seen:
            continue
        seen.add(key)
        candidates.append({
            "contributor": contributor,
            "email": email_value,
            "source": provider + ":public-contributors-snapshot",
        })
    return candidates


def contributor_blind_value(contributor):
    return "contributor:" + clean_text(contributor, 120).casefold()


def contributor_identifiers(record):
    metadata = record.get("metadata")
    metadata = metadata if isinstance(metadata, dict) else {}
    contributors = metadata.get("contributors")
    out = set()
    for item in contributors if isinstance(contributors, list) else []:
        if not isinstance(item, dict):
            continue
        name = clean_text(item.get("login"), 120).casefold()
        if name:
            out.add(name)
    return out


def invitation_preview(record, inviter, contributor, email_value, basis,
                       origin, invite_id="", token=""):
    repo_name = clean_text(record.get("fullName"), 300)
    inviter = clean_text(inviter, 100)
    contributor = clean_text(contributor, 120)
    subject = "%s invited you to join %s on ForkMesh" % (inviter, repo_name)
    link = origin.rstrip("/") + "/signup"
    if invite_id and token:
        link += "?invitation=" + quote(invite_id) + "&token=" + quote(token)
    opt_out = ""
    report = ""
    if invite_id and token:
        base = (
            origin.rstrip("/") + "/api/repository-imports/"
            + quote(record["id"], safe="") + "/invitations/"
            + quote(invite_id, safe=""))
        opt_out = base + "/opt-out?token=" + quote(token)
        report = base + "/report?token=" + quote(token)
    text = (
        "%s invited %s to join the external repository %s on ForkMesh.\n\n"
        "ForkMesh lists provider metadata but does not own or control the "
        "repository. The listing may not yet be mirrored.\n\n"
        "Review the invitation: %s\n\n"
        "Why this address may be used: %s."
        % (inviter, contributor, repo_name, link, basis.replace("_", " ")))
    if opt_out:
        text += "\n\nStop future contributor invitations: " + opt_out
        text += "\nReport this invitation: " + report
    escaped = html.escape
    html_body = (
        "<p><strong>%s</strong> invited <strong>%s</strong> to join "
        "<strong>%s</strong> on ForkMesh.</p>"
        "<p>ForkMesh lists provider metadata but does not own or control this "
        "repository. It may not yet be mirrored.</p>"
        "<p><a href=\"%s\">Review invitation</a></p>"
        "<p>Address-use basis: %s.</p>"
        % (
            escaped(inviter), escaped(contributor), escaped(repo_name),
            escaped(link, quote=True), escaped(basis.replace("_", " ")),
        ))
    if opt_out:
        html_body += (
            "<p><a href=\"%s\">Stop future invitations</a> · "
            "<a href=\"%s\">Report abuse</a></p>"
            % (escaped(opt_out, quote=True), escaped(report, quote=True)))
    return {
        "to": email_value,
        "subject": subject,
        "text": text,
        "html": html_body,
        "inviter": inviter,
        "repository": repo_name,
        "contributor": contributor,
        "basis": basis,
        "optOutUrl": opt_out,
        "reportUrl": report,
    }


def validate_uploaded_logo(value):
    if not isinstance(value, str) or not value.startswith("data:image/"):
        raise ValueError("logo_data_url_required")
    try:
        header, encoded = value.split(",", 1)
    except ValueError as exc:
        raise ValueError("invalid_logo_data") from exc
    if ";base64" not in header.lower():
        raise ValueError("base64_logo_required")
    media = header[5:].split(";", 1)[0].lower()
    if media not in ("image/png", "image/jpeg", "image/webp"):
        raise ValueError("unsupported_logo_type")
    try:
        raw = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError("invalid_logo_data") from exc
    if not raw or len(raw) > MAX_LOGO_BYTES:
        raise ValueError("logo_too_large")
    valid = (
        (media == "image/png" and raw.startswith(_PNG_MAGIC))
        or (media == "image/jpeg" and raw.startswith(_JPEG_MAGIC))
        or (
            media == "image/webp" and raw.startswith(_WEBP_RIFF)
            and len(raw) >= 12 and raw[8:12] == b"WEBP"
        )
    )
    if not valid:
        raise ValueError("logo_signature_mismatch")
    return {
        "contentType": media,
        "dataUrl": "data:" + media + ";base64," + base64.b64encode(raw).decode(),
        "sizeBytes": len(raw),
    }


def public_repository_record(record, official_logo=None):
    """Return a technical, non-ownership-claiming representation."""
    output = dict(record or {})
    status = (
        output.get("status")
        if output.get("status") in REPOSITORY_STATUSES
        else "external_repository")
    output["status"] = status
    output["statusLabel"] = STATUS_LABELS[status]
    output["mirrored"] = status in MIRRORED_STATUSES
    output["mirrorNotice"] = (
        "A separately verified ForkMesh mirror is linked to this entry."
        if output["mirrored"] else
        "This entry is external metadata and is not currently mirrored by "
        "ForkMesh.")
    # Only a real logo: the owner-approved artwork, or nothing so the UI
    # falls back to its repository icon. ForkMesh never invents a mark for
    # a project it does not own.
    output["logo"] = official_logo or None
    output.pop("internal", None)
    return output


class RepositoryImportService:
    """D1-backed request service using Worker primitives supplied by entry.py."""

    def __init__(self, dependencies):
        self.d = dict(dependencies)

    def _json(self, data, status=200, **kwargs):
        return self.d["json_response"](data, status=status, **kwargs)

    async def _actor(self, env, request, data=None):
        _, record = await self.d["session_record"](env, request, data)
        if not isinstance(record, dict):
            return "", None
        name = clean_text(record.get("name"), 100).lower()
        return name, record

    async def _target_owner(self, env, actor, requested=""):
        target = clean_text(requested, 100).lower() or actor
        resolver = self.d.get("target_owner")
        if callable(resolver):
            resolved = await resolver(env, actor, target)
            return resolved if isinstance(resolved, dict) else None
        if target != actor:
            return None
        return {
            "name": actor,
            "kind": "user",
            "ownerBi": await self.d["blind_index"](env, actor),
        }

    async def _can_manage_owner(self, env, actor, owner_bi):
        if not actor or not owner_bi:
            return False
        if owner_bi == await self.d["blind_index"](env, actor):
            return True
        checker = self.d.get("can_manage_owner")
        return bool(
            callable(checker)
            and await checker(env, actor, owner_bi)
        )

    async def _load(self, env, import_id, actor="", hide_private=True):
        if not valid_import_id(import_id):
            return None, None
        row = await self.d["d1_first"](
            env,
            "SELECT id, owner_bi, is_private, status, data, created_at, "
            "updated_at FROM repository_imports WHERE id=?",
            import_id,
        )
        if not row:
            return None, None
        if int(row.get("is_private") or 0):
            if not actor:
                return None, None
            actor_bi = await self.d["blind_index"](env, actor)
            if actor_bi != str(row.get("owner_bi") or ""):
                # Private entries deliberately return the same not-found shape
                # for missing, guessed, and unauthorized ids.
                return None, None
        record = await self.d["decrypt_row"](env, row.get("data"))
        if not isinstance(record, dict):
            return row, None
        record["status"] = (
            row.get("status") if row.get("status") in REPOSITORY_STATUSES
            else record.get("status"))
        return row, record

    async def _official_logo(self, env, import_id):
        row = await self.d["d1_first"](
            env,
            "SELECT data FROM repository_logo_suggestions "
            "WHERE repo_id=? AND official=1 LIMIT 1",
            import_id,
        )
        if not row:
            return None
        suggestion = await self.d["decrypt_row"](env, row.get("data"))
        return self._official_logo_payload(suggestion)

    @staticmethod
    def _official_logo_payload(suggestion):
        if not isinstance(suggestion, dict):
            return None
        image = suggestion.get("image")
        if not isinstance(image, dict):
            return None
        return {
            **image,
            "kind": suggestion.get("kind") or "approved_upload",
            "generated": False,
            "aiGenerated": bool(suggestion.get("aiGenerated")),
            "approved": True,
            "suggestionId": suggestion.get("id"),
            "attribution": suggestion.get("attribution") or "",
        }

    async def _official_logos(self, env, import_ids):
        ids = [item for item in import_ids if valid_import_id(item)]
        if not ids:
            return {}
        placeholders = ",".join("?" for _ in ids)
        rows = await self.d["d1_all"](
            env,
            "SELECT repo_id, data FROM repository_logo_suggestions "
            "WHERE official=1 AND repo_id IN (" + placeholders + ")",
            *ids,
        )
        output = {}
        for row in rows or []:
            repo_id = str(row.get("repo_id") or "")
            if repo_id not in ids:
                continue
            suggestion = await self.d["decrypt_row"](env, row.get("data"))
            logo = self._official_logo_payload(suggestion)
            if logo:
                output[repo_id] = logo
        return output

    async def _render(self, env, record):
        logo = await self._official_logo(env, record.get("id", ""))
        return public_repository_record(record, logo)

    async def _store(self, env, record, owner_bi, created_at=None):
        now = int(self.d["now_ms"]())
        record["updatedAt"] = now
        record["statusLabel"] = STATUS_LABELS[record["status"]]
        record["mirrored"] = record["status"] in MIRRORED_STATUSES
        encoded = await self.d["encrypt_row"](env, record)
        await self.d["d1_run"](
            env,
            "INSERT INTO repository_imports "
            "(id, provider, external_id, owner_bi, is_private, status, data, "
            "created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET provider=excluded.provider, "
            "external_id=excluded.external_id, owner_bi=excluded.owner_bi, "
            "is_private=excluded.is_private, "
            "status=excluded.status, data=excluded.data, "
            "updated_at=excluded.updated_at",
            record["id"], record["provider"], record["providerId"], owner_bi,
            1 if record.get("isPrivate") else 0, record["status"], encoded,
            int(created_at if created_at is not None else now), now,
        )

    async def _audit(self, env, actor, action, import_id, outcome, details):
        audit = self.d.get("audit")
        if callable(audit):
            await audit(
                env, actor, action, "repository_import", import_id,
                outcome, details)

    async def _save_invitation_provenance(
            self, env, import_id, contributor, email_value, basis,
            created_by_bi, created_by, source_type, evidence_digest,
            evidence_key):
        contributor = clean_text(contributor, 120)
        email_value = normalize_email(email_value)
        if (
                not contributor or not email_value
                or basis not in INVITATION_BASES):
            raise ValueError("invalid_invitation_provenance")
        contributor_bi = await self.d["blind_index"](
            env, contributor_blind_value(contributor))
        email_bi = await self.d["blind_index"](
            env, "contributor-email:" + email_value)
        provenance_id = opaque_record_id(
            "prov", import_id, contributor_bi, email_bi, basis, evidence_key)
        now = int(self.d["now_ms"]())
        saved = {
            "id": provenance_id,
            "repoId": import_id,
            "contributor": contributor,
            "maskedEmail": _mask_email(email_value),
            "basis": basis,
            "sourceType": clean_text(source_type, 80),
            "evidenceDigest": clean_text(evidence_digest, 128),
            "createdBy": clean_text(created_by, 100),
            "createdAt": now,
            "rawEmailStored": False,
            "rawEvidenceStored": False,
        }
        encoded = await self.d["encrypt_row"](env, saved)
        await self.d["d1_run"](
            env,
            "INSERT INTO contributor_invitation_provenance "
            "(id, repo_id, contributor_bi, email_bi, basis, created_by_bi, "
            "data, created_at, revoked_at) VALUES (?,?,?,?,?,?,?,?,0) "
            "ON CONFLICT(id) DO UPDATE SET "
            "contributor_bi=excluded.contributor_bi, "
            "email_bi=excluded.email_bi, basis=excluded.basis, "
            "created_by_bi=excluded.created_by_bi, data=excluded.data, "
            "created_at=excluded.created_at, revoked_at=0",
            provenance_id, import_id, contributor_bi, email_bi, basis,
            created_by_bi, encoded, now,
        )
        return {
            **saved,
            "contributorBi": contributor_bi,
            "emailBi": email_bi,
        }

    async def _sync_provider_invitation_provenance(
            self, env, record, owner_bi, candidates):
        """Replace provider-derived eligibility with this public snapshot."""
        if record.get("isPrivate"):
            candidates = []
        now = int(self.d["now_ms"]())
        await self.d["d1_run"](
            env,
            "UPDATE contributor_invitation_provenance SET revoked_at=? "
            "WHERE repo_id=? AND basis='public_for_invitations' "
            "AND revoked_at=0",
            now, record["id"],
        )
        saved = []
        for candidate in candidates or []:
            contributor = clean_text(candidate.get("contributor"), 120)
            email_value = normalize_email(candidate.get("email"))
            if (
                    not contributor or not email_value
                    or contributor.casefold() not in contributor_identifiers(
                        record)):
                continue
            source_type = clean_text(candidate.get("source"), 80)
            evidence_token = await self.d["blind_index"](
                env,
                "invitation-evidence:provider:" + record["id"] + ":"
                + contributor.casefold() + ":" + email_value + ":"
                + source_type)
            evidence_key = hashlib.sha256(
                str(evidence_token).encode()).hexdigest()
            item = await self._save_invitation_provenance(
                env, record["id"], contributor, email_value,
                "public_for_invitations", owner_bi, "provider snapshot",
                source_type, evidence_key, evidence_key)
            saved.append(item)
        return saved

    async def _provenance_row(self, env, import_id, provenance_id):
        if (
                not valid_record_id(provenance_id)
                or not str(provenance_id).startswith("prov_")):
            return None, None
        row = await self.d["d1_first"](
            env,
            "SELECT id, contributor_bi, email_bi, basis, created_by_bi, "
            "data, created_at, revoked_at FROM "
            "contributor_invitation_provenance WHERE id=? AND repo_id=?",
            provenance_id, import_id,
        )
        if not row:
            return None, None
        saved = await self.d["decrypt_row"](env, row.get("data"))
        return row, saved if isinstance(saved, dict) else None

    async def _list(self, env, request):
        actor, _ = await self._actor(env, request)
        actor_bi = await self.d["blind_index"](env, actor) if actor else ""
        params = parse_qs(
            urlparse(str(getattr(request, "url", "") or "")).query)
        digest_only = params.get("view", [""])[0] == "digest"
        if actor:
            rows = await self.d["d1_all"](
                env,
                "SELECT id, owner_bi, is_private, status, data, updated_at FROM "
                "repository_imports WHERE is_private=0 OR owner_bi=? "
                "ORDER BY updated_at DESC LIMIT ?",
                actor_bi, MAX_PUBLIC_IMPORTS,
            )
        else:
            rows = await self.d["d1_all"](
                env,
                "SELECT id, owner_bi, is_private, status, data, updated_at FROM "
                "repository_imports WHERE is_private=0 "
                "ORDER BY updated_at DESC LIMIT ?",
                MAX_PUBLIC_IMPORTS,
            )
        # Pollers only need a change token. Avoid decrypting and serializing up
        # to 200 rich provider snapshots (hundreds of KiB) every few seconds
        # merely to discover that nothing changed. Private rows are still
        # selected only for their authenticated owner by the queries above.
        if digest_only:
            return self._json(
                {
                    "ok": True,
                    "repositories": [
                        {
                            "id": str(row.get("id") or ""),
                            "status": str(row.get("status") or ""),
                            "updatedAt": int(row.get("updated_at") or 0),
                        }
                        for row in rows or []
                        if str(row.get("id") or "")
                    ],
                },
                cache_control=(
                    "no-store, max-age=0, must-revalidate"
                    if actor else "public, max-age=30"),
            )
        records = []
        for row in rows or []:
            record = await self.d["decrypt_row"](env, row.get("data"))
            if not isinstance(record, dict):
                continue
            if int(row.get("is_private") or 0) and row.get("owner_bi") != actor_bi:
                continue
            record["status"] = row.get("status") or record.get("status")
            records.append(record)
        logos = await self._official_logos(
            env, [record.get("id", "") for record in records])
        items = []
        rows_by_id = {str(row.get("id") or ""): row for row in rows or []}
        for record in records:
            item = public_repository_record(
                record, logos.get(record.get("id", "")))
            row = rows_by_id.get(str(record.get("id") or "")) or {}
            item["canManage"] = await self._can_manage_owner(
                env, actor, str(row.get("owner_bi") or ""))
            items.append(item)
        return self._json(
            {
                "ok": True,
                "repositories": items,
                "statuses": [
                    {"value": value, "label": STATUS_LABELS[value]}
                    for value in REPOSITORY_STATUSES
                ],
                "privacy": (
                    "Private provider-import job metadata is returned only to "
                    "its authenticated owner and is excluded before decryption "
                    "for other users. Repository collaborator ACLs apply only "
                    "after materialization as a native ForkMesh repository."),
            },
            cache_control=(
                "no-store, max-age=0, must-revalidate"
                if actor else "public, max-age=60"),
        )

    async def _create(self, env, request):
        try:
            data = await request.json()
        except Exception:
            return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        target_owner = await self._target_owner(
            env, actor, data.get("targetOwner"))
        if not target_owner:
            return self._json({"error": "target_owner_forbidden"}, status=403)
        try:
            source = parse_provider_source(data.get("sourceUrl"))
            token = clean_provider_token(data.get("providerToken"))
            mode = clean_text(data.get("mode") or "import", 20).lower()
            if mode not in IMPORT_MODES:
                raise ProviderSourceError("invalid_import_mode")
            root, extras, rate, incomplete = await fetch_provider_snapshot(
                self.d["provider_fetch"], env, source, token)
            record = build_repository_record(
                source, root, extras, actor, mode, self.d["now_ms"](),
                token_present=bool(token), provider_rate=rate,
                incomplete=incomplete)
            record["targetOwner"] = clean_text(
                target_owner.get("name"), 100).lower()
            record["targetOwnerType"] = (
                "organization"
                if target_owner.get("kind") == "organization"
                else "user")
            provider_invitation_candidates = (
                provider_public_invitation_candidates(
                    record.get("provider"), extras)
                if not record.get("isPrivate") else [])
        except ProviderSourceError as exc:
            return self._json({"error": str(exc)}, status=400)
        except ProviderRequestError as exc:
            kwargs = {}
            if exc.retry_after:
                kwargs["extra_headers"] = {"Retry-After": exc.retry_after}
            return self._json(
                {"error": exc.code}, status=exc.status, **kwargs)
        owner_bi = str(target_owner.get("ownerBi") or "")
        if not owner_bi:
            return self._json({"error": "target_owner_forbidden"}, status=403)
        existing_row = await self.d["d1_first"](
            env,
            "SELECT id, owner_bi, is_private, status, data, created_at, "
            "updated_at FROM repository_imports WHERE id=?",
            record["id"],
        )
        existing = None
        adopted = False
        if existing_row:
            existing_owner = str(existing_row.get("owner_bi") or "")
            may_read_existing = (
                existing_owner == owner_bi
                or not int(existing_row.get("is_private") or 0)
                or bool(record.get("authorization", {}).get(
                    "administratorVerified"))
            )
            if may_read_existing:
                existing = await self.d["decrypt_row"](
                    env, existing_row.get("data"))
            if not isinstance(existing, dict):
                # Do not reveal that a guessed private id already exists.
                return self._json({"error": "not_found"}, status=404)
            if existing_owner != owner_bi:
                # Duplicates return the existing neutral listing rather than
                # transferring moderation rights to a later submitter. A
                # current provider administrator can explicitly adopt it,
                # which lets the actual owner replace a community-created stub.
                if not record.get("authorization", {}).get(
                        "administratorVerified"):
                    return self._json({
                        "ok": True,
                        "created": False,
                        "repository": await self._render(env, existing),
                        "notice": (
                            "This external repository was already listed; "
                            "ForkMesh does not transfer or imply ownership."),
                    })
                adopted = True
                record["originalListedBy"] = (
                    existing.get("originalListedBy")
                    or existing.get("listedBy") or "")
            # An owner may refresh provider metadata/permissions with a fresh
            # one-request token.  Preserve mirror state and creation time.
            record["status"] = existing.get("status", record["status"])
            record["createdAt"] = existing.get(
                "createdAt", record["createdAt"])
            record["mirror"] = existing.get("mirror")
        await self._store(
            env, record, owner_bi,
            created_at=(existing_row or {}).get("created_at"))
        await self._sync_provider_invitation_provenance(
            env, record, owner_bi, provider_invitation_candidates)
        # Drop the provider token as soon as the metadata request completes.
        token = ""
        return self._json({
            "ok": True,
            "created": not bool(existing_row),
            "adoptedByProviderAdministrator": adopted,
            "repository": await self._render(env, record),
            "credentialStored": False,
        }, status=201 if not existing_row else 200)

    @staticmethod
    def _migration_public(job):
        """Owner-facing job projection with no raw provider payload or token."""
        checkpoint = job.get("checkpoint")
        checkpoint = checkpoint if isinstance(checkpoint, dict) else {}
        report = job.get("report")
        report = report if isinstance(report, dict) else {}
        state = job.get("state")
        state = state if isinstance(state, dict) else None
        return {
            "id": job.get("id"),
            "repositoryId": job.get("repositoryId"),
            "provider": job.get("provider"),
            "sourceUrl": job.get("sourceUrl"),
            "isPrivate": bool(job.get("isPrivate")),
            "status": job.get("status"),
            "phase": checkpoint.get("phase"),
            "checkpoint": {
                "phase": checkpoint.get("phase"),
                "phaseIndex": int(checkpoint.get("phaseIndex") or 0),
                "streamIndex": int(checkpoint.get("streamIndex") or 0),
                "page": int(checkpoint.get("page") or 1),
            },
            "generation": int(job.get("generation") or 1),
            "setup": {
                "durationMs": int(job.get("setupMs") or 0),
                "acceptedAt": int(job.get("acceptedAt") or 0),
                "measurement": job.get("setupMeasurement") or "request",
            },
            "transfer": {
                "startedAt": int(job.get("transferStartedAt") or 0),
                "completedAt": int(job.get("completedAt") or 0),
                "durationMs": int(job.get("transferMs") or 0),
            },
            "counts": dict(job.get("counts") or {}),
            "processed": int(job.get("processed") or 0),
            "objectCount": int(job.get("objectCount") or 0),
            "retryCount": int(job.get("retryCount") or 0),
            "resumeAfter": int(job.get("resumeAfter") or 0),
            "lastError": clean_text(job.get("lastError"), 120),
            "report": {
                "unsupported": list(report.get("unsupported") or []),
                "skipped": list(report.get("skipped") or []),
                "skippedOverflow": int(report.get("skippedOverflow") or 0),
                "failures": list(report.get("failures") or []),
                "failureOverflow": int(report.get("failureOverflow") or 0),
                "redactedCredentialFields": int(
                    report.get("redactedCredentialFields") or 0),
                "releaseCoverage": dict(report.get("releaseCoverage") or {}),
                "latest": _repository_migration_report_module().public_summary(
                    job),
            },
            "retrying": bool((job.get("retry") or {}).get("active")),
            "state": state,
            "credentialStored": False,
            "resumable": job.get("status") not in ("rolled_back",),
        }

    @staticmethod
    def _migration_report_event(report, category, event):
        events = report.setdefault(category, [])
        if len(events) < MAX_MIGRATION_REPORT_EVENTS:
            events.append(event)
            return
        overflow = (
            "skippedOverflow" if category == "skipped"
            else "failureOverflow")
        report[overflow] = int(report.get(overflow) or 0) + 1

    async def _load_migration(self, env, job_id, actor):
        if not _MIGRATION_ID_RE.fullmatch(str(job_id or "")) or not actor:
            return None, None
        row = await self.d["d1_first"](
            env,
            "SELECT id,repo_id,owner_bi,provider,status,phase,generation,"
            "setup_ms,transfer_started_at,completed_at,retry_count,"
            "object_count,state_hash,data,created_at,updated_at FROM "
            "repository_migration_jobs WHERE id=?",
            job_id,
        )
        if not row or not await self._can_manage_owner(
                env, actor, str(row.get("owner_bi") or "")):
            return None, None
        job = await self.d["decrypt_row"](env, row.get("data"))
        if not isinstance(job, dict):
            return row, None
        job["status"] = str(row.get("status") or job.get("status") or "")
        job["generation"] = int(
            row.get("generation") or job.get("generation") or 1)
        job["retryCount"] = int(
            row.get("retry_count") or job.get("retryCount") or 0)
        return row, job

    async def _save_migration(self, env, job, owner_bi, created_at=None):
        now = int(self.d["now_ms"]())
        checkpoint = job.get("checkpoint") or {}
        job["updatedAt"] = now
        encoded = await self.d["encrypt_row"](env, job)
        await self.d["d1_run"](
            env,
            "INSERT INTO repository_migration_jobs "
            "(id,repo_id,owner_bi,provider,status,phase,generation,setup_ms,"
            "transfer_started_at,completed_at,retry_count,object_count,"
            "state_hash,data,created_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET owner_bi=excluded.owner_bi,"
            "provider=excluded.provider,status=excluded.status,"
            "phase=excluded.phase,generation=excluded.generation,"
            "setup_ms=excluded.setup_ms,"
            "transfer_started_at=excluded.transfer_started_at,"
            "completed_at=excluded.completed_at,"
            "retry_count=excluded.retry_count,"
            "object_count=excluded.object_count,"
            "state_hash=excluded.state_hash,data=excluded.data,"
            "updated_at=excluded.updated_at",
            job["id"], job["repositoryId"], owner_bi, job["provider"],
            job["status"], str(checkpoint.get("phase") or "complete"),
            int(job.get("generation") or 1), int(job.get("setupMs") or 0),
            int(job.get("transferStartedAt") or 0),
            int(job.get("completedAt") or 0),
            int(job.get("retryCount") or 0),
            int(job.get("objectCount") or 0),
            str((job.get("state") or {}).get("hash") or ""), encoded,
            int(created_at if created_at is not None else now), now,
        )

    async def _save_migration_run(self, env, job, outcome=None):
        now = int(self.d["now_ms"]())
        transfer_started = int(job.get("transferStartedAt") or 0)
        completed = int(job.get("completedAt") or 0)
        transfer_ms = (
            max(0, completed - transfer_started)
            if transfer_started and completed else 0)
        run = {
            "jobId": job["id"],
            "generation": int(job.get("generation") or 1),
            "provider": job["provider"],
            "outcome": outcome or job.get("status") or "running",
            "counts": dict(job.get("counts") or {}),
            "fidelity": dict((job.get("state") or {}).get("counts") or {}),
            "failureCount": len((job.get("report") or {}).get("failures") or []),
        }
        latest_report = (
            _repository_migration_report_module().latest(job) or {}).get("report")
        if isinstance(latest_report, dict):
            reconciliation = latest_report.get("reconciliation") or {}
            run["report"] = {
                "sequence": latest_report.get("sequence"),
                "outcome": latest_report.get("outcome"),
                "reconciled": bool(reconciliation.get("reconciled")),
                "unclassified": int(reconciliation.get("unclassified") or 0),
                "failed": int((reconciliation.get("totals") or {}).get("failed") or 0),
                "external": int((reconciliation.get("totals") or {}).get("external") or 0),
            }
        retries = job.get("retryHistory")
        retries = retries if isinstance(retries, list) else []
        run["failedWorkRetries"] = len(retries)
        run["failedWorkRecovered"] = sum(
            int(entry.get("recovered") or 0) for entry in retries
            if isinstance(entry, dict))
        encoded = await self.d["encrypt_row"](env, run)
        await self.d["d1_run"](
            env,
            "INSERT INTO repository_migration_runs "
            "(job_id,generation,provider,outcome,setup_ms,transfer_ms,"
            "retry_count,object_count,data,started_at,finished_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(job_id,generation) DO UPDATE SET "
            "outcome=excluded.outcome,setup_ms=excluded.setup_ms,"
            "transfer_ms=excluded.transfer_ms,"
            "retry_count=excluded.retry_count,"
            "object_count=excluded.object_count,data=excluded.data,"
            "finished_at=excluded.finished_at",
            job["id"], int(job.get("generation") or 1), job["provider"],
            run["outcome"], int(job.get("setupMs") or 0), transfer_ms,
            int(job.get("retryCount") or 0),
            int(job.get("objectCount") or 0), encoded,
            int(job.get("transferStartedAt") or job.get("acceptedAt") or now),
            completed if run["outcome"] in (
                "completed", "abandoned", "failed", "rolled_back") else 0,
        )

    async def _migration_start(self, env, request):
        """Connect a forge and durably accept transfer after one root lookup."""
        migration = _repository_migration_module()
        request_started = int(self.d["now_ms"]())
        try:
            data = await request.json()
        except Exception:
            return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        target_owner = await self._target_owner(
            env, actor, data.get("targetOwner"))
        if not target_owner:
            return self._json({"error": "target_owner_forbidden"}, status=403)
        try:
            source = parse_provider_source(data.get("sourceUrl"))
            token = clean_provider_token(data.get("providerToken"))
            result = await self.d["provider_fetch"](
                env, source["provider"], source["metadataPath"], token)
            if not isinstance(result, dict) or not (
                    200 <= int(result.get("status") or 0) < 300):
                raise _provider_error(result, bool(token))
            root = result.get("data")
            if not isinstance(root, dict):
                raise ProviderRequestError(
                    "invalid_provider_response", status=502)
            record = build_repository_record(
                source, root, {}, actor, "import", self.d["now_ms"](),
                token_present=bool(token),
                provider_rate=_rate_info(result.get("headers")),
                incomplete=[phase + ":transfer_pending"
                            for phase in migration.PHASES],
            )
            adapter = migration.provider_adapter(source, root, {})
        except ProviderSourceError as exc:
            return self._json({"error": str(exc)}, status=400)
        except migration.MigrationContractError as exc:
            return self._json({"error": str(exc)}, status=502)
        except ProviderRequestError as exc:
            kwargs = {}
            if exc.retry_after:
                kwargs["extra_headers"] = {"Retry-After": exc.retry_after}
            return self._json({"error": exc.code}, status=exc.status, **kwargs)

        owner_bi = str(target_owner.get("ownerBi") or "")
        if not owner_bi:
            return self._json({"error": "target_owner_forbidden"}, status=403)
        record["targetOwner"] = clean_text(
            target_owner.get("name"), 100).lower()
        record["targetOwnerType"] = (
            "organization" if target_owner.get("kind") == "organization"
            else "user")
        existing_repo_row = await self.d["d1_first"](
            env,
            "SELECT id,owner_bi,is_private,status,data,created_at,updated_at "
            "FROM repository_imports WHERE id=?",
            record["id"],
        )
        if existing_repo_row:
            existing_owner = str(existing_repo_row.get("owner_bi") or "")
            may_adopt = bool(record.get("authorization", {}).get(
                "administratorVerified"))
            if existing_owner != owner_bi and not may_adopt:
                # Do not disclose a private job or move ownership based on a
                # repository URL alone.
                if int(existing_repo_row.get("is_private") or 0):
                    return self._json({"error": "not_found"}, status=404)
                return self._json({"error": "repository_already_managed"}, status=409)
            existing_record = await self.d["decrypt_row"](
                env, existing_repo_row.get("data"))
            if isinstance(existing_record, dict):
                record["createdAt"] = existing_record.get(
                    "createdAt", record["createdAt"])
                record["status"] = existing_record.get(
                    "status", record["status"])
                record["mirror"] = existing_record.get("mirror")
                record["originalListedBy"] = existing_record.get(
                    "originalListedBy") or existing_record.get("listedBy")
        await self._store(
            env, record, owner_bi,
            created_at=(existing_repo_row or {}).get("created_at"))

        job_id = migration.migration_id(record["id"])
        prior_row = await self.d["d1_first"](
            env,
            "SELECT id,repo_id,owner_bi,provider,status,phase,generation,"
            "setup_ms,transfer_started_at,completed_at,retry_count,"
            "object_count,state_hash,data,created_at,updated_at FROM "
            "repository_migration_jobs WHERE id=?",
            job_id,
        )
        prior = None
        if prior_row:
            if not await self._can_manage_owner(
                    env, actor, str(prior_row.get("owner_bi") or "")):
                return self._json({"error": "not_found"}, status=404)
            prior = await self.d["decrypt_row"](env, prior_row.get("data"))
        now = int(self.d["now_ms"]())
        try:
            supplied_setup_ms = int(data.get("setupDurationMs") or 0)
        except (TypeError, ValueError, OverflowError):
            supplied_setup_ms = 0
        if 0 < supplied_setup_ms <= 60 * 60 * 1000:
            setup_ms = supplied_setup_ms
            setup_measurement = "client_monotonic"
        else:
            setup_ms = max(0, now - request_started)
            setup_measurement = "request"
        resumable = isinstance(prior, dict) and prior.get("status") in (
            "pending", "running", "paused", "rate_limited", "failed")
        generation = int((prior or {}).get("generation") or 0)
        if not resumable:
            generation += 1
        if resumable:
            job = prior
            job["source"] = source
            job["root"] = {
                "id": root.get("id"),
                "default_branch": root.get("default_branch"),
                "provider_updated_at": (
                    root.get("updated_at") or root.get("last_activity_at")),
            }
            job["status"] = (
                "pending" if prior.get("status") == "failed"
                else prior.get("status"))
            job["lastError"] = ""
            job["resumeAfter"] = 0
        else:
            job = {
                "schema": migration.SCHEMA,
                "id": job_id,
                "repositoryId": record["id"],
                "provider": source["provider"],
                "providerRepositoryId": adapter["repositoryId"],
                "sourceUrl": source["canonicalUrl"],
                "source": source,
                "root": {
                    "id": root.get("id"),
                    "default_branch": root.get("default_branch"),
                    "provider_updated_at": (
                        root.get("updated_at") or root.get("last_activity_at")),
                },
                "isPrivate": bool(record.get("isPrivate")),
                "status": "pending",
                "generation": max(1, generation),
                "checkpoint": {
                    "phase": migration.PHASES[0],
                    "phaseIndex": 0,
                    "streamIndex": 0,
                    "page": 1,
                },
                "counts": {kind: 0 for kind in migration.OBJECT_TYPES},
                "processed": 0,
                "retryCount": 0,
                "report": {
                    "unsupported": list(adapter.get("unsupported") or []),
                    "skipped": [],
                    "failures": [],
                    "redactedCredentialFields": 0,
                },
                "ledger": _repository_migration_report_module().new_ledger(),
                "failedWork": {},
                # Earlier generations' signed reports stay in the chain; a new
                # full scan supersedes them rather than erasing them.
                "reports": list((prior or {}).get("reports") or []),
                "retryHistory": list((prior or {}).get("retryHistory") or []),
                "state": None,
                "acceptedAt": now,
                "transferStartedAt": 0,
                "completedAt": 0,
                "transferMs": 0,
                "lastError": "",
                "resumeAfter": 0,
                "credentialStored": False,
            }
        job["setupMs"] = setup_ms
        job["setupMeasurement"] = setup_measurement
        await self._save_migration(
            env, job, owner_bi,
            created_at=(prior_row or {}).get("created_at"))
        await self._save_migration_run(env, job, "running")
        await self._audit(
            env, actor, "repository_migration.start", job_id, "success", {
                "provider": source["provider"],
                "resumed": resumable,
                "generation": job["generation"],
                "setupMs": setup_ms,
                "credentialStored": False,
            })
        token = ""
        return self._json({
            "ok": True,
            "accepted": True,
            "resumed": resumable,
            "repository": await self._render(env, record),
            "migration": self._migration_public(job),
        }, status=202)

    async def _migration_relationship_context(self, env, job):
        migration = _repository_migration_module()
        by_type = {}
        for object_type in ("pull_requests", "issues"):
            rows = await self.d["d1_all"](
                env,
                "SELECT data FROM repository_migration_objects WHERE job_id=? "
                "AND object_type=? AND generation=? ORDER BY source_key",
                job["id"], object_type, int(job.get("generation") or 1),
            )
            records = []
            for row in rows or []:
                record = await self.d["decrypt_row"](env, row.get("data"))
                if isinstance(record, dict):
                    records.append(record)
            by_type[object_type] = records
        return migration.relationship_context(
            by_type["pull_requests"], by_type["issues"])

    @staticmethod
    def _advance_migration_checkpoint(job, phases, stream_count):
        checkpoint = job["checkpoint"]
        # The stream is finished, so a crash can no longer make its pages be
        # fetched again: fold their ledger entries into per-type totals.
        _repository_migration_report_module().fold_pages(job)
        checkpoint["page"] = 1
        checkpoint["streamIndex"] = int(checkpoint.get("streamIndex") or 0) + 1
        if checkpoint["streamIndex"] >= int(stream_count):
            checkpoint["phaseIndex"] = int(checkpoint.get("phaseIndex") or 0) + 1
            checkpoint["streamIndex"] = 0
            if checkpoint["phaseIndex"] < len(phases):
                checkpoint["phase"] = phases[checkpoint["phaseIndex"]]
            else:
                checkpoint["phase"] = "complete"

    async def _migration_store_objects(self, env, job, normalized, now):
        report = job.setdefault("report", {})
        report.setdefault("redactedCredentialFields", 0)
        generation = int(job.get("generation") or 1)
        for item in normalized:
            encoded = await self.d["encrypt_row"](env, item["record"])
            await self.d["d1_run"](
                env,
                "INSERT INTO repository_migration_objects "
                "(job_id,object_type,source_key,payload_hash,generation,data,"
                "imported_at) VALUES (?,?,?,?,?,?,?) "
                "ON CONFLICT(job_id,object_type,source_key) DO UPDATE SET "
                "payload_hash=excluded.payload_hash,"
                "generation=excluded.generation,data=excluded.data,"
                "imported_at=excluded.imported_at",
                job["id"], item["objectType"], item["sourceKey"],
                item["payloadHash"], generation, encoded, now,
            )
            report["redactedCredentialFields"] += len(
                item.get("redactedFields") or [])
        job["processed"] = int(job.get("processed") or 0) + len(normalized)

    def _migration_item_failures(self, job, phase, stream, page, failures):
        report = job.setdefault("report", {})
        for failure in failures:
            self._migration_report_event(report, "failures", {
                "phase": phase, "stream": stream["id"], "page": page,
                "sourceId": failure.get("sourceId") or "",
                "reason": failure.get("reason") or "",
            })

    async def _migration_rate_limited(self, env, row, job, rate, now):
        job["status"] = "rate_limited"
        job["retryCount"] = int(job.get("retryCount") or 0) + 1
        retry_after = clean_text(rate.get("retryAfter"), 40)
        reset = clean_text(rate.get("reset"), 40)
        try:
            if retry_after.isdigit():
                job["resumeAfter"] = now + int(retry_after) * 1000
            elif reset.isdigit():
                job["resumeAfter"] = int(reset) * 1000
        except (TypeError, ValueError, OverflowError):
            job["resumeAfter"] = 0
        job["lastError"] = "provider_rate_limited"
        await self._save_migration(
            env, job, str(row.get("owner_bi") or ""),
            created_at=row.get("created_at"))
        await self._save_migration_run(env, job, "rate_limited")
        kwargs = {}
        if retry_after:
            kwargs["extra_headers"] = {"Retry-After": retry_after}
        return self._json({
            "error": "provider_rate_limited",
            "migration": self._migration_public(job),
        }, status=429, **kwargs)

    async def _migration_stored_objects(self, env, job):
        rows = await self.d["d1_all"](
            env,
            "SELECT object_type,source_key,payload_hash,generation,data FROM "
            "repository_migration_objects WHERE job_id=? ORDER BY "
            "object_type,source_key",
            job["id"],
        )
        objects = []
        for object_row in rows or []:
            record = await self.d["decrypt_row"](env, object_row.get("data"))
            objects.append({
                "objectType": str(object_row.get("object_type") or ""),
                "sourceKey": str(object_row.get("source_key") or ""),
                "payloadHash": str(object_row.get("payload_hash") or ""),
                "generation": int(object_row.get("generation") or 0),
                "record": record if isinstance(record, dict) else {},
            })
        return list(rows or []), objects

    @staticmethod
    def _migration_paths(job):
        base = "/api/repository-imports/migrations/" + str(job.get("id") or "")
        return base + "/objects", base + "/report"

    async def _issue_migration_report(self, env, actor, job, objects, outcome):
        """Sign a report that supersedes the job's previous one."""
        reports = _repository_migration_report_module()
        previous = reports.latest(job)
        supersedes = ({
            "sequence": int(previous["sequence"]),
            "reportHash": previous["reportHash"],
        } if previous else None)
        objects_path, report_path = self._migration_paths(job)
        document = reports.build_report(
            job, objects, outcome, int(self.d["now_ms"]()),
            int((previous or {}).get("sequence") or 0) + 1, supersedes,
            objects_path=objects_path, report_path=report_path)
        digest = reports.report_hash(document)
        signer = self.d.get("report_sign")
        if not callable(signer):
            raise RuntimeError("repository_migration_report_signer_unavailable")
        signature = await signer(env, reports.signing_message(digest))
        if not isinstance(signature, dict) or not signature.get("signature"):
            raise RuntimeError("repository_migration_report_signature_failed")
        signature = {
            key: clean_text(signature.get(key), 200)
            for key in ("algorithm", "keyId", "publicKey", "signature")
            if signature.get(key)
        }
        reports.append_history(job, document, digest, signature)
        reconciliation = document["reconciliation"]
        await self._audit(
            env, actor, "repository_migration.report", job["id"], "success", {
                "sequence": document["sequence"],
                "reportHash": digest,
                "outcome": document["outcome"],
                "unclassifiedRecords": reconciliation["unclassified"],
                "failedRecords": reconciliation["totals"]["failed"],
                "externalRecords": reconciliation["totals"]["external"],
                "supersedes": (supersedes or {}).get("reportHash") or "",
                "credentialStored": False,
            })
        return document, digest

    async def _finish_migration(self, env, actor, row, job, retry_entry=None):
        migration = _repository_migration_module()
        generation = int(job.get("generation") or 1)
        # A source may change while offset pages are being traversed. Require
        # an object to be absent from two consecutive complete generations
        # before pruning it: one moving-page scan can delay a deletion, but it
        # can never silently delete a still-live source record.
        await self.d["d1_run"](
            env,
            "DELETE FROM repository_migration_objects WHERE job_id=? "
            "AND generation<?",
            job["id"], max(0, generation - 1),
        )
        object_rows, objects = await self._migration_stored_objects(env, job)
        records = [item["record"] for item in objects if item["record"]]
        document = migration.state_document(
            job["repositoryId"], job["providerRepositoryId"], object_rows,
            (job.get("report") or {}).get("unsupported") or [],
            (job.get("report") or {}).get("skipped") or [],
            (job.get("report") or {}).get("skippedOverflow") or 0, records,
        )
        job.setdefault("report", {})["releaseCoverage"] = (
            document["releaseCoverage"])
        digest = migration.state_hash(document)
        signer = self.d.get("migration_sign")
        if not callable(signer):
            raise RuntimeError("repository_migration_signer_unavailable")
        signature = await signer(env, digest)
        if not isinstance(signature, dict) or not signature.get("signature"):
            raise RuntimeError("repository_migration_signature_failed")
        now = int(self.d["now_ms"]())
        job["status"] = "completed"
        job["completedAt"] = now
        if retry_entry is None:
            job["transferMs"] = max(
                0, now - int(job.get("transferStartedAt") or now))
        else:
            # Retrying failed work is recorded separately; it must not stretch
            # the original transfer measurement.
            retry_entry["completedAt"] = now
            history = job.get("retryHistory")
            history = history if isinstance(history, list) else []
            job["retryHistory"] = (history + [retry_entry])[-50:]
            job["retry"] = None
        job["objectCount"] = len(object_rows or [])
        job["counts"] = document["counts"]
        job["state"] = {
            "schema": migration.STATE_SCHEMA,
            "hash": digest,
            "signature": signature.get("signature"),
            "algorithm": signature.get("algorithm") or "hmac-sha256",
            "keyId": signature.get("keyId") or "forkmesh-relay",
        }
        job["lastError"] = ""
        job["resumeAfter"] = 0
        report, report_digest = await self._issue_migration_report(
            env, actor, job, objects, "completed")
        await self._save_migration(
            env, job, str(row.get("owner_bi") or ""),
            created_at=row.get("created_at"))
        await self._save_migration_run(env, job, "completed")
        await self._audit(
            env, actor, "repository_migration.complete", job["id"],
            "success", {
                "generation": generation,
                "objectCount": job["objectCount"],
                "stateHash": digest,
                "reportHash": report_digest,
                "reportOutcome": report["outcome"],
                "retriedFailedWork": retry_entry is not None,
                "credentialStored": False,
            })
        return self._json({
            "ok": True, "hasMore": False,
            "migration": self._migration_public(job),
        })

    async def _migration_resume(self, env, request, job_id):
        migration = _repository_migration_module()
        reports = _repository_migration_report_module()
        try:
            data = await request.json()
        except Exception:
            data = {}
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        row, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        if job.get("status") == "completed":
            # Converge the telemetry row if a prior invocation committed the
            # signed job and then stopped before its final metrics upsert.
            await self._save_migration_run(env, job, "completed")
            return self._json({
                "ok": True, "hasMore": False,
                "migration": self._migration_public(job),
            })
        if job.get("status") == "abandoned" and not data.get("recover"):
            return self._json({
                "error": "migration_abandoned",
                "recovery": "Retry with recover=true or roll back the job.",
            }, status=409)
        try:
            token = clean_provider_token(data.get("providerToken"))
        except ProviderSourceError as exc:
            return self._json({"error": str(exc)}, status=400)
        if job.get("isPrivate") and not token:
            return self._json({
                "error": "private_authorization_required",
                "credentialStored": False,
            }, status=403)
        prior_status = str(job.get("status") or "")
        prior_error = clean_text(job.get("lastError"), 120)
        now = int(self.d["now_ms"]())
        if not int(job.get("transferStartedAt") or 0):
            job["transferStartedAt"] = now
        job["status"] = "running"
        job["completedAt"] = 0
        job["lastError"] = ""
        job["resumeAfter"] = 0
        checkpoint = job.get("checkpoint")
        if not isinstance(checkpoint, dict):
            return self._json({"error": "invalid_migration_checkpoint"}, status=500)
        if (job.get("retry") or {}).get("active"):
            response = await self._migration_retry_step(
                env, actor, row, job, token, now)
            token = ""
            return response

        # Empty dynamic phases (for example reviews in a repository with no
        # pull requests) advance without spending provider requests.
        while int(checkpoint.get("phaseIndex") or 0) < len(migration.PHASES):
            phase_index = int(checkpoint.get("phaseIndex") or 0)
            phase = migration.PHASES[phase_index]
            context = (
                await self._migration_relationship_context(env, job)
                if phase in ("reviews", "collaboration_history") else {})
            try:
                adapter = migration.provider_adapter(
                    job.get("source") or {}, job.get("root") or {}, context)
            except migration.MigrationContractError as exc:
                return self._json({"error": str(exc)}, status=500)
            streams = adapter["streams"][phase]
            stream_index = int(checkpoint.get("streamIndex") or 0)
            if stream_index < len(streams):
                break
            self._advance_migration_checkpoint(
                job, migration.PHASES, len(streams))
        else:
            return await self._finish_migration(env, actor, row, job)

        stream = streams[stream_index]
        page = max(1, int(checkpoint.get("page") or 1))
        if data.get("deferFailure"):
            # Keep importing the rest of the repository: the unreadable page
            # and everything after it in this stream become retryable failed
            # work, reported as unmeasured rather than silently skipped.
            if prior_status not in ("paused", "failed") or not prior_error:
                job["status"] = prior_status
                job["lastError"] = prior_error
                return self._json({"error": "no_failed_page"}, status=409)
            reports.note_failed_work(
                job, "stream_from_page", phase, stream["id"], page,
                [prior_error], attempts=1, failed_at=now)
            self._advance_migration_checkpoint(
                job, migration.PHASES, len(streams))
            await self._save_migration(
                env, job, str(row.get("owner_bi") or ""),
                created_at=row.get("created_at"))
            await self._save_migration_run(env, job, "running")
            await self._audit(
                env, actor, "repository_migration.defer_failure", job["id"],
                "success", {"phase": phase, "stream": stream["id"],
                            "page": page, "reason": prior_error})
            token = ""
            return self._json({
                "ok": True, "hasMore": True, "deferred": True,
                "migration": self._migration_public(job),
            })
        try:
            path = migration.stream_path(stream, page)
        except migration.MigrationContractError as exc:
            return self._json({"error": str(exc)}, status=500)
        result = await self.d["provider_fetch"](
            env, job["provider"], path, token)
        status = int((result or {}).get("status") or 0)
        rate = _rate_info((result or {}).get("headers"))
        report = job.setdefault("report", {})
        report.setdefault("unsupported", list(adapter.get("unsupported") or []))
        report.setdefault("skipped", [])
        report.setdefault("failures", [])
        report.setdefault("redactedCredentialFields", 0)
        if status == 404 and stream.get("optional"):
            self._migration_report_event(report, "skipped", {
                "phase": phase, "stream": stream["id"],
                "reason": "provider_endpoint_unavailable",
            })
            self._advance_migration_checkpoint(
                job, migration.PHASES, len(streams))
            await self._save_migration(
                env, job, str(row.get("owner_bi") or ""),
                created_at=row.get("created_at"))
            token = ""
            return self._json({
                "ok": True, "hasMore": True,
                "migration": self._migration_public(job),
            })
        rate_limited = status == 429 or (
            status == 403 and (
                rate.get("remaining") == "0" or rate.get("retryAfter")))
        if rate_limited:
            token = ""
            return await self._migration_rate_limited(env, row, job, rate, now)
        if not 200 <= status < 300:
            code = (
                "provider_authorization_failed" if status in (401, 403)
                else "provider_transfer_failed")
            job["status"] = "paused"
            job["retryCount"] = int(job.get("retryCount") or 0) + 1
            job["lastError"] = code
            self._migration_report_event(report, "failures", {
                "phase": phase, "stream": stream["id"],
                "page": page, "reason": code,
            })
            await self._save_migration(
                env, job, str(row.get("owner_bi") or ""),
                created_at=row.get("created_at"))
            await self._save_migration_run(env, job, "paused")
            token = ""
            return self._json({
                "error": code, "migration": self._migration_public(job),
                "recovery": "Resume to retry this page, or resume with "
                            "deferFailure=true to continue and retry it later.",
            }, status=403 if status in (401, 403) else 502)

        try:
            items = migration.page_items(stream, (result or {}).get("data"))
        except migration.MigrationContractError as exc:
            code = str(exc)
            job["status"] = "failed"
            job["lastError"] = code
            self._migration_report_event(report, "failures", {
                "phase": phase, "stream": stream["id"],
                "page": page, "reason": code,
            })
            job["completedAt"] = now
            await self._save_migration(
                env, job, str(row.get("owner_bi") or ""),
                created_at=row.get("created_at"))
            await self._save_migration_run(env, job, "failed")
            return self._json({
                "error": code, "migration": self._migration_public(job),
            }, status=502)

        # Each record is classified on its own: a record that cannot be stored
        # without loss is counted as failed (with its source id) and the rest
        # of the page still lands.
        normalized, page_counts, item_failures = reports.classify_page(
            adapter, stream, items, job["providerRepositoryId"])
        await self._migration_store_objects(env, job, normalized, now)
        counts = job.setdefault(
            "counts", {kind: 0 for kind in migration.OBJECT_TYPES})
        counts[phase] = int(counts.get(phase) or 0) + len(normalized)
        reports.record_page(job, phase, stream["id"], page, page_counts)
        work = reports.failed_work(job)
        key = reports.page_key(stream["id"], page)
        if item_failures:
            self._migration_item_failures(job, phase, stream, page, item_failures)
            reports.note_failed_work(
                job, "page", phase, stream["id"], page,
                [failure["reason"] for failure in item_failures],
                counts=page_counts, failed_at=now)
        elif (work.get(key) or {}).get("kind") == "page":
            # A clean fetch of the same page supersedes an earlier attempt.
            work.pop(key, None)
        following = migration.next_page(
            stream, (result or {}).get("headers"), len(items), page)
        if following:
            checkpoint["page"] = following
        else:
            self._advance_migration_checkpoint(
                job, migration.PHASES, len(streams))
        await self._save_migration(
            env, job, str(row.get("owner_bi") or ""),
            created_at=row.get("created_at"))
        await self._save_migration_run(env, job, "running")
        token = ""
        return self._json({
            "ok": True, "hasMore": True,
            "migration": self._migration_public(job),
        })

    async def _migration_retry(self, env, request, job_id):
        """Queue only the failed work of a finished import."""
        reports = _repository_migration_report_module()
        try:
            data = await request.json()
        except Exception:
            data = {}
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        row, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        if job.get("status") != "completed":
            return self._json({
                "error": "migration_not_completed",
                "recovery": "Resume the import; retrying failed work applies "
                            "to a finished import.",
            }, status=409)
        queue = reports.retry_queue(job)
        if not queue:
            return self._json({"error": "no_failed_work"}, status=409)
        previous = reports.latest(job)
        history = job.get("retryHistory")
        now = int(self.d["now_ms"]())
        job["retry"] = {
            "active": True,
            "sequence": len(history if isinstance(history, list) else []) + 1,
            "requestedAt": now,
            "queue": queue,
            "queued": len(queue),
            "recovered": 0,
            "stillFailing": 0,
            "supersedes": str((previous or {}).get("reportHash") or ""),
        }
        job["status"] = "running"
        job["completedAt"] = 0
        job["lastError"] = ""
        job["resumeAfter"] = 0
        await self._save_migration(
            env, job, str(row.get("owner_bi") or ""),
            created_at=row.get("created_at"))
        await self._save_migration_run(env, job, "running")
        await self._audit(
            env, actor, "repository_migration.retry_failed", job_id, "success", {
                "queued": len(queue),
                "supersedes": job["retry"]["supersedes"],
                "credentialStored": False,
            })
        return self._json({
            "ok": True, "accepted": True, "queued": len(queue),
            "migration": self._migration_public(job),
        }, status=202)

    async def _migration_retry_step(self, env, actor, row, job, token, now):
        """Fetch one queued page of failed work; finish when the queue drains."""
        migration = _repository_migration_module()
        reports = _repository_migration_report_module()
        retry = job["retry"]
        queue = retry.setdefault("queue", [])
        work_index = reports.failed_work(job)
        while queue and queue[0] not in work_index:
            queue.pop(0)
        if not queue:
            entry = {
                "sequence": int(retry.get("sequence") or 1),
                "requestedAt": int(retry.get("requestedAt") or 0),
                "queued": int(retry.get("queued") or 0),
                "recovered": int(retry.get("recovered") or 0),
                "stillFailing": int(retry.get("stillFailing") or 0),
                "remainingFailedWork": len(work_index),
                "supersedes": str(retry.get("supersedes") or ""),
            }
            return await self._finish_migration(
                env, actor, row, job, retry_entry=entry)
        owner_bi = str(row.get("owner_bi") or "")
        key = queue[0]
        work = work_index[key]
        phase = str(work.get("phase") or "")
        report = job.setdefault("report", {})
        report.setdefault("failures", [])
        context = (
            await self._migration_relationship_context(env, job)
            if phase in ("reviews", "collaboration_history") else {})
        try:
            adapter = migration.provider_adapter(
                job.get("source") or {}, job.get("root") or {}, context)
        except migration.MigrationContractError as exc:
            return self._json({"error": str(exc)}, status=500)
        stream = next((
            candidate for candidate in adapter["streams"].get(phase) or []
            if candidate.get("id") == work.get("stream")), None)
        attempts = int(work.get("attempts") or 0) + 1

        async def keep_failed(code):
            work["attempts"] = attempts
            work["reasons"] = sorted(set(work.get("reasons") or []) | {code})
            retry["stillFailing"] = int(retry.get("stillFailing") or 0) + 1
            queue.pop(0)
            self._migration_report_event(report, "failures", {
                "phase": phase, "stream": work.get("stream"),
                "page": work.get("page"), "reason": code, "retry": True,
            })
            await self._save_migration(
                env, job, owner_bi, created_at=row.get("created_at"))
            await self._save_migration_run(env, job, "running")
            return self._json({
                "ok": True, "hasMore": True,
                "migration": self._migration_public(job),
            })

        if stream is None:
            return await keep_failed("adapter_stream_unavailable")
        page = max(1, int(work.get("page") or 1))
        try:
            path = migration.stream_path(stream, page)
        except migration.MigrationContractError as exc:
            return self._json({"error": str(exc)}, status=500)
        result = await self.d["provider_fetch"](
            env, job["provider"], path, token)
        status = int((result or {}).get("status") or 0)
        rate = _rate_info((result or {}).get("headers"))
        rate_limited = status == 429 or (
            status == 403 and (
                rate.get("remaining") == "0" or rate.get("retryAfter")))
        if rate_limited:
            return await self._migration_rate_limited(env, row, job, rate, now)
        if status == 404 and stream.get("optional"):
            items = []
        elif not 200 <= status < 300:
            return await keep_failed(
                "provider_authorization_failed" if status in (401, 403)
                else "provider_transfer_failed")
        else:
            try:
                items = migration.page_items(stream, (result or {}).get("data"))
            except migration.MigrationContractError as exc:
                return await keep_failed(str(exc))
        normalized, page_counts, item_failures = reports.classify_page(
            adapter, stream, items, job["providerRepositoryId"])
        await self._migration_store_objects(env, job, normalized, now)
        # A retried page replaces what its failed attempt contributed; a page
        # from a deferred stream was never counted, so it only adds.
        reports.adjust_totals(
            job, phase, add=page_counts,
            subtract=work.get("counts") if work.get("kind") == "page" else None)
        work_index.pop(key, None)
        queue.pop(0)
        if item_failures:
            self._migration_item_failures(job, phase, stream, page, item_failures)
            reports.note_failed_work(
                job, "page", phase, stream["id"], page,
                [failure["reason"] for failure in item_failures],
                counts=page_counts, attempts=attempts,
                failed_at=work.get("firstFailedAt") or now)
            retry["stillFailing"] = int(retry.get("stillFailing") or 0) + 1
        else:
            retry["recovered"] = int(retry.get("recovered") or 0) + 1
        if work.get("kind") == "stream_from_page" and items:
            following = migration.next_page(
                stream, (result or {}).get("headers"), len(items), page)
            if following and reports.note_failed_work(
                    job, "stream_from_page", phase, stream["id"], following,
                    work.get("reasons"), attempts=attempts,
                    failed_at=work.get("firstFailedAt") or now):
                queue.insert(0, reports.page_key(stream["id"], following))
        await self._save_migration(
            env, job, owner_bi, created_at=row.get("created_at"))
        await self._save_migration_run(env, job, "running")
        return self._json({
            "ok": True, "hasMore": True,
            "migration": self._migration_public(job),
        })

    async def _migration_report(self, env, request, job_id):
        """Export the signed report, its history, and remediation steps."""
        reports = _repository_migration_report_module()
        actor, _ = await self._actor(env, request)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        _, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        params = parse_qs(urlparse(str(getattr(request, "url", "") or "")).query)
        raw_sequence = clean_text(params.get("sequence", [""])[0], 12)
        sequence = None
        if raw_sequence:
            if not raw_sequence.isdigit():
                return self._json({"error": "invalid_sequence"}, status=400)
            sequence = int(raw_sequence)
        bundle = reports.export_bundle(job, sequence)
        draft = False
        if bundle is None and sequence is None:
            # An import that has not ended yet still gets an honest view: it
            # is built on request, and is unsigned because it will change.
            _, objects = await self._migration_stored_objects(env, job)
            objects_path, report_path = self._migration_paths(job)
            status = str(job.get("status") or "")
            document = reports.build_report(
                job, objects,
                status if status in ("completed", "abandoned") else "in_progress",
                int(self.d["now_ms"]()), 1, None,
                objects_path=objects_path, report_path=report_path)
            bundle = {
                "schema": reports.EXPORT_SCHEMA, "report": document,
                "reportHash": reports.report_hash(document),
                "signature": None, "latest": True, "history": [],
            }
            draft = True
        if bundle is None:
            return self._json({"error": "report_not_found"}, status=404)
        await self._audit(
            env, actor, "repository_migration.report_export", job_id,
            "success", {
                "sequence": (bundle.get("report") or {}).get("sequence"),
                "reportHash": bundle.get("reportHash"),
                "draft": draft,
            })
        return self._json(dict(
            bundle, ok=True, draft=draft, status=job.get("status"),
            credentialStored=False,
        ), cache_control="no-store, max-age=0, must-revalidate")

    async def _migration_get(self, env, request, job_id):
        actor, _ = await self._actor(env, request)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        _, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        return self._json({
            "ok": True, "migration": self._migration_public(job),
        }, cache_control="no-store, max-age=0, must-revalidate")

    async def _migration_list(self, env, request):
        actor, _ = await self._actor(env, request)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        resolver = self.d.get("managed_owner_bis")
        owner_bis = (
            await resolver(env, actor) if callable(resolver)
            else [await self.d["blind_index"](env, actor)])
        owner_bis = list(dict.fromkeys(
            str(value or "") for value in owner_bis if str(value or "")))[:200]
        if not owner_bis:
            return self._json({"ok": True, "migrations": []})
        placeholders = ",".join("?" for _ in owner_bis)
        rows = await self.d["d1_all"](
            env,
            "SELECT id,owner_bi,data FROM repository_migration_jobs "
            "WHERE owner_bi IN (" + placeholders + ") "
            "ORDER BY updated_at DESC LIMIT 200",
            *owner_bis,
        )
        jobs = []
        for row in rows or []:
            job = await self.d["decrypt_row"](env, row.get("data"))
            if isinstance(job, dict):
                jobs.append(self._migration_public(job))
        return self._json({"ok": True, "migrations": jobs},
                          cache_control="no-store, max-age=0, must-revalidate")

    async def _migration_objects(self, env, request, job_id):
        actor, _ = await self._actor(env, request)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        _, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        params = parse_qs(urlparse(str(getattr(request, "url", "") or "")).query)
        object_type = clean_text(params.get("type", [""])[0], 40)
        after = clean_text(params.get("after", [""])[0], 120)
        try:
            limit = min(200, max(1, int(params.get("limit", ["100"])[0])))
        except (TypeError, ValueError, OverflowError):
            return self._json({"error": "invalid_limit"}, status=400)
        migration = _repository_migration_module()
        if object_type and object_type not in migration.OBJECT_TYPES:
            return self._json({"error": "invalid_object_type"}, status=400)
        if object_type:
            after_key = after
            if after_key and not _MIGRATION_SOURCE_KEY_RE.fullmatch(after_key):
                return self._json({"error": "invalid_cursor"}, status=400)
            rows = await self.d["d1_all"](
                env,
                "SELECT object_type,source_key,payload_hash,data FROM "
                "repository_migration_objects WHERE job_id=? AND "
                "object_type=? AND source_key>? ORDER BY source_key LIMIT ?",
                job_id, object_type, after_key, limit + 1,
            )
        else:
            after_type = ""
            after_key = ""
            if after:
                after_type, separator, after_key = after.partition(":")
                if (not separator or after_type not in migration.OBJECT_TYPES
                        or not _MIGRATION_SOURCE_KEY_RE.fullmatch(after_key)):
                    return self._json({"error": "invalid_cursor"}, status=400)
            rows = await self.d["d1_all"](
                env,
                "SELECT object_type,source_key,payload_hash,data FROM "
                "repository_migration_objects WHERE job_id=? AND "
                "(object_type>? OR (object_type=? AND source_key>?)) "
                "ORDER BY object_type,source_key LIMIT ?",
                job_id, after_type, after_type, after_key, limit + 1,
            )
        has_more = len(rows or []) > limit
        rows = list(rows or [])[:limit]
        objects = []
        for row in rows:
            record = await self.d["decrypt_row"](env, row.get("data"))
            if isinstance(record, dict):
                objects.append(record)
        last = rows[-1] if has_more and rows else None
        next_cursor = ""
        if last:
            next_cursor = str(last.get("source_key") or "")
            if not object_type:
                next_cursor = (
                    str(last.get("object_type") or "") + ":" + next_cursor)
        return self._json({
            "ok": True, "jobId": job_id, "objects": objects,
            "count": len(objects), "hasMore": has_more,
            "nextCursor": next_cursor, "state": job.get("state"),
        }, cache_control="no-store, max-age=0, must-revalidate")

    async def _migration_abandon(self, env, request, job_id):
        try:
            data = await request.json()
        except Exception:
            data = {}
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        row, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        if job.get("status") == "completed":
            return self._json({"error": "migration_already_completed"}, status=409)
        now = int(self.d["now_ms"]())
        job["status"] = "abandoned"
        job["completedAt"] = now
        job["transferMs"] = max(
            0, now - int(job.get("transferStartedAt") or now))
        _, objects = await self._migration_stored_objects(env, job)
        await self._issue_migration_report(env, actor, job, objects, "abandoned")
        await self._save_migration(
            env, job, str(row.get("owner_bi") or ""),
            created_at=row.get("created_at"))
        await self._save_migration_run(env, job, "abandoned")
        await self._audit(
            env, actor, "repository_migration.abandon", job_id, "success",
            {"generation": job.get("generation")})
        return self._json({
            "ok": True, "migration": self._migration_public(job),
        })

    async def _migration_rollback(self, env, request, job_id):
        try:
            data = await request.json()
        except Exception:
            data = {}
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        _, job = await self._load_migration(env, job_id, actor)
        if not job:
            return self._json({"error": "not_found"}, status=404)
        final_report = (
            _repository_migration_report_module().latest(job) or {})
        await self.d["d1_run"](
            env, "DELETE FROM repository_migration_objects WHERE job_id=?",
            job_id)
        await self.d["d1_run"](
            env, "DELETE FROM repository_migration_runs WHERE job_id=?",
            job_id)
        await self.d["d1_run"](
            env, "DELETE FROM repository_migration_jobs WHERE id=?", job_id)
        await self._audit(
            env, actor, "repository_migration.rollback", job_id, "success", {
                "repositoryListingPreserved": True,
                "sourceChanged": False,
                "finalReportHash": final_report.get("reportHash") or "",
                "finalReportSequence": final_report.get("sequence") or 0,
            })
        return self._json({
            "ok": True, "id": job_id, "rolledBack": True,
            "repositoryListingPreserved": True,
            "sourceChanged": False,
        })

    async def _migration_metrics(self, env, request):
        migration = _repository_migration_module()
        actor, _ = await self._actor(env, request)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        rows = await self.d["d1_all"](
            env,
            "SELECT provider,outcome,setup_ms,transfer_ms,retry_count,"
            "object_count,data,started_at,finished_at FROM "
            "repository_migration_runs ORDER BY started_at DESC LIMIT 1000",
        )
        decoded = []
        for row in rows or []:
            run = await self.d["decrypt_row"](env, row.get("data"))
            decoded.append(dict(row, run=run if isinstance(run, dict) else {}))
        rows = decoded

        def aggregate(selected):
            setups = [int(row.get("setup_ms") or 0) for row in selected]
            transfers = [int(row.get("transfer_ms") or 0) for row in selected
                         if int(row.get("transfer_ms") or 0) > 0]
            completed = sum(row.get("outcome") == "completed" for row in selected)
            abandoned = sum(row.get("outcome") == "abandoned" for row in selected)
            return {
                "runs": len(selected),
                "setupP50Ms": migration.percentile(setups, 50),
                "setupP95Ms": migration.percentile(setups, 95),
                "setupMedianWithinThreeMinutes": bool(setups) and (
                    migration.percentile(setups, 50) <= 3 * 60 * 1000),
                "transferP50Ms": migration.percentile(transfers, 50),
                "completed": completed,
                "completionRate": (
                    completed / len(selected) if selected else None),
                "abandoned": abandoned,
                "abandonmentRate": (
                    abandoned / len(selected) if selected else None),
                "retries": sum(int(row.get("retry_count") or 0)
                               for row in selected),
                "objects": sum(int(row.get("object_count") or 0)
                               for row in selected),
                "reports": {
                    "issued": sum(1 for row in selected
                                  if row["run"].get("report")),
                    "reconciled": sum(
                        1 for row in selected
                        if (row["run"].get("report") or {}).get("reconciled")),
                    "unclassifiedRecords": sum(
                        int((row["run"].get("report") or {}).get("unclassified") or 0)
                        for row in selected),
                    "failedRecords": sum(
                        int((row["run"].get("report") or {}).get("failed") or 0)
                        for row in selected),
                    "externalRecords": sum(
                        int((row["run"].get("report") or {}).get("external") or 0)
                        for row in selected),
                },
                "retryRecovery": {
                    "retries": sum(int(row["run"].get("failedWorkRetries") or 0)
                                   for row in selected),
                    "recoveredWork": sum(
                        int(row["run"].get("failedWorkRecovered") or 0)
                        for row in selected),
                },
            }

        providers = {}
        for provider in PROVIDERS:
            providers[provider] = aggregate([
                row for row in rows or [] if row.get("provider") == provider])
        return self._json({
            "ok": True,
            "window": "latest_1000_runs",
            "all": aggregate(list(rows or [])),
            "providers": providers,
            "definitions": {
                "setup": "User interaction from opening import to durable acceptance; provider transfer is excluded.",
                "transfer": "Durable acceptance through signed completion.",
                "p50": "nearest-rank median",
                "p95": "nearest-rank 95th percentile, reported separately",
                "unclassifiedRecords": "Source records not classified as imported, skipped, failed, or external in the latest signed report; the release gate is zero.",
                "recoveredWork": "Failed pages that imported cleanly when a maintainer retried only failed work.",
            },
        }, cache_control="no-store, max-age=0, must-revalidate")

    async def _discover(self, env, request):
        try:
            data = await request.json()
        except Exception:
            return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        try:
            target = parse_provider_namespace(data.get("sourceUrl"))
            token = clean_provider_token(data.get("providerToken"))
        except ProviderSourceError as exc:
            return self._json({"error": str(exc)}, status=400)
        provider = target["provider"]
        namespace = target["namespace"]
        urls = []
        seen = set()
        incomplete = False
        # Resolved from the first page that answers and then reused: probing
        # every candidate collection on every page would double the request
        # cost of walking a large organization.
        endpoint = None
        for page in range(1, NAMESPACE_MAX_PAGES + 1):
            options = namespace_discovery_paths(provider, namespace, page)
            result = None
            status = 0
            for index in ([endpoint] if endpoint is not None
                          else range(len(options))):
                result = await self.d["provider_fetch"](
                    env, provider, options[index], token)
                status = int((result or {}).get("status") or 0)
                if status == 200:
                    endpoint = index
                    break
            if status != 200:
                if not urls:
                    return self._json(
                        {"error": "provider_request_failed"},
                        status=502 if status >= 500 or status <= 0 else status,
                    )
                incomplete = True
                break
            rows = (result or {}).get("data")
            if not isinstance(rows, list):
                return self._json(
                    {"error": "invalid_provider_response"}, status=502)
            for row in rows:
                candidate = namespace_repository_url(
                    provider, row, bool(token))
                if not candidate:
                    continue
                try:
                    source = parse_provider_source(candidate)
                except ProviderSourceError:
                    continue
                if (
                        source["provider"] != provider
                        or not namespace_contains(namespace, source["owner"])
                        or source["canonicalUrl"] in seen):
                    continue
                seen.add(source["canonicalUrl"])
                urls.append(source["canonicalUrl"])
                if len(urls) >= MAX_NAMESPACE_REPOSITORIES:
                    incomplete = True
                    break
            if (len(urls) >= MAX_NAMESPACE_REPOSITORIES
                    or len(rows) < NAMESPACE_PAGE_SIZE):
                break
        else:
            # The page budget ran out on a full page, so the organization has
            # more repositories than this walk reported.
            incomplete = True
        return self._json({
            "ok": True,
            "provider": provider,
            "namespace": namespace,
            "repositories": urls,
            "count": len(urls),
            "incomplete": incomplete,
        }, cache_control="no-store, max-age=0, must-revalidate")

    async def _delete(self, env, request, import_id):
        try:
            data = await request.json()
        except Exception:
            data = {}
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        row = await self.d["d1_first"](
            env,
            "SELECT id, owner_bi, is_private, status, data, created_at, "
            "updated_at FROM repository_imports WHERE id=?",
            import_id,
        )
        if not row:
            return self._json({"error": "not_found"}, status=404)
        if not await self._can_manage_owner(
                env, actor, str(row.get("owner_bi") or "")):
            return self._json({"error": "forbidden"}, status=403)
        # Remove repository-scoped presentation/workflow rows first. Abuse
        # reports remain as moderation evidence and contain no source token.
        for sql in (
                "DELETE FROM repository_mirror_volunteers WHERE repo_id=?",
                "DELETE FROM contributor_invitation_provenance WHERE repo_id=?",
                "DELETE FROM contributor_invitations WHERE repo_id=?",
                "DELETE FROM contributor_invitation_rate WHERE repo_id=?",
                "DELETE FROM repository_logo_suggestions WHERE repo_id=?"):
            await self.d["d1_run"](env, sql, import_id)
        await self.d["d1_run"](
            env, "DELETE FROM repository_imports WHERE id=?", import_id)
        await self._audit(
            env, actor, "repository_import.delete", import_id, "success",
            {"externalRepository": True})
        return self._json({
            "ok": True, "id": import_id, "deleted": True})

    async def _get(self, env, request, import_id):
        actor, _ = await self._actor(env, request)
        _, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        return self._json({
            "ok": True, "repository": await self._render(env, record)})

    async def _can_admin(self, env, actor, record):
        if not actor:
            return False
        if await self.d["is_moderator"](env, actor):
            return True
        auth = record.get("authorization")
        if not (
            record.get("listedBy") == actor
            and isinstance(auth, dict)
            and auth.get("administratorVerified")):
            return False
        try:
            verified_at = int(auth.get("verifiedAt") or 0)
        except (TypeError, ValueError):
            return False
        return (
            verified_at > 0
            and int(self.d["now_ms"]()) - verified_at
            <= PROVIDER_ADMIN_PROOF_TTL_MS
        )

    async def _patch(self, env, request, import_id):
        try:
            data = await request.json()
        except Exception:
            return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        row, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        if not await self._can_admin(env, actor, record):
            return self._json({"error": "forbidden"}, status=403)
        target = clean_text(data.get("status"), 40).lower()
        current = record.get("status")
        if target not in REPOSITORY_STATUSES:
            return self._json({"error": "invalid_status"}, status=400)
        if target != current and not status_transition_allowed(current, target):
            return self._json(
                {"error": "invalid_status_transition"}, status=409)
        if target in MIRRORED_STATUSES:
            proof = await self.d["mirror_status"](
                env, actor, record, target, data)
            if not isinstance(proof, dict) or not proof.get("verified"):
                return self._json(
                    {"error": "verified_mirror_required"}, status=409)
            record["mirror"] = {
                "owner": clean_text(proof.get("owner"), 100),
                "name": clean_text(proof.get("name"), 100),
                "live": bool(proof.get("live")),
                "verifiedAt": int(self.d["now_ms"]()),
            }
            if target == "actively_mirrored" and not proof.get("live"):
                return self._json(
                    {"error": "healthy_live_mirror_required"}, status=409)
        record["status"] = target
        owner_bi = str(row.get("owner_bi") or "")
        await self._store(
            env, record, owner_bi, created_at=row.get("created_at"))
        return self._json({
            "ok": True, "repository": await self._render(env, record)})

    async def _volunteers(self, env, request, import_id):
        method = str(getattr(request, "method", "GET")).upper()
        data = {}
        if method in ("POST", "DELETE"):
            try:
                data = await request.json()
            except Exception:
                data = {}
        actor, _ = await self._actor(env, request, data)
        _, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        actor_bi = await self.d["blind_index"](env, actor) if actor else ""
        if method == "GET":
            count = await self.d["d1_first"](
                env,
                "SELECT COUNT(*) AS n FROM repository_mirror_volunteers "
                "WHERE repo_id=? AND status='requested'",
                import_id,
            )
            own = None
            if actor:
                own = await self.d["d1_first"](
                    env,
                    "SELECT node_label, status, created_at FROM "
                    "repository_mirror_volunteers "
                    "WHERE repo_id=? AND operator_bi=?",
                    import_id, actor_bi,
                )
            return self._json({
                "ok": True,
                "repositoryId": import_id,
                "volunteerCount": int((count or {}).get("n") or 0),
                "myRequest": own,
                "privacy": "Public responses show counts, not operator devices.",
            })
        if method not in ("POST", "DELETE"):
            return self._json({"error": "method_not_allowed"}, status=405)
        if not actor:
            return self._json({"error": "invalid_session"}, status=401)
        node = clean_text(data.get("node"), 100).lower()
        if method == "POST":
            if not node or not await self.d["operator_eligible"](
                    env, actor, node):
                return self._json(
                    {"error": "eligible_owned_mirror_node_required"},
                    status=403)
            now = int(self.d["now_ms"]())
            await self.d["d1_run"](
                env,
                "INSERT INTO repository_mirror_volunteers "
                "(repo_id, operator_bi, node_label, status, created_at, "
                "updated_at) VALUES (?,?,?,?,?,?) "
                "ON CONFLICT(repo_id,operator_bi) DO UPDATE SET "
                "node_label=excluded.node_label, status='requested', "
                "updated_at=excluded.updated_at",
                import_id, actor_bi, node, "requested", now, now,
            )
            if record.get("status") in (
                    "stub_only", "external_repository", "mirror_unavailable"):
                record["status"] = "mirror_requested"
                row = await self.d["d1_first"](
                    env,
                    "SELECT owner_bi, created_at FROM repository_imports "
                    "WHERE id=?", import_id)
                await self._store(
                    env, record, row.get("owner_bi"),
                    created_at=row.get("created_at"))
            return self._json({
                "ok": True, "requested": True,
                "repository": await self._render(env, record),
            }, status=201)
        await self.d["d1_run"](
            env,
            "UPDATE repository_mirror_volunteers SET status='withdrawn', "
            "updated_at=? WHERE repo_id=? AND operator_bi=?",
            int(self.d["now_ms"]()), import_id, actor_bi,
        )
        return self._json({"ok": True, "requested": False})

    async def _invitation_provenance(self, env, request, import_id):
        """Manage owner-supplied, non-raw invitation eligibility evidence."""
        method = str(getattr(request, "method", "GET")).upper()
        data = {}
        if method in ("POST", "DELETE"):
            try:
                data = await request.json()
            except Exception:
                return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        _, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        if not actor or not await self._can_admin(env, actor, record):
            return self._json({"error": "forbidden"}, status=403)

        if method == "GET":
            rows = await self.d["d1_all"](
                env,
                "SELECT id, basis, data, created_at FROM "
                "contributor_invitation_provenance "
                "WHERE repo_id=? AND revoked_at=0 "
                "ORDER BY created_at DESC LIMIT ?",
                import_id, MAX_INVITATIONS_PER_REPOSITORY,
            )
            evidence = []
            for row in rows or []:
                saved = await self.d["decrypt_row"](env, row.get("data"))
                if not isinstance(saved, dict):
                    continue
                evidence.append({
                    "id": row.get("id"),
                    "basis": row.get("basis"),
                    "contributor": saved.get("contributor"),
                    "maskedEmail": saved.get("maskedEmail"),
                    "sourceType": saved.get("sourceType"),
                    "evidenceDigest": saved.get("evidenceDigest"),
                    "createdBy": saved.get("createdBy"),
                    "createdAt": int(row.get("created_at") or 0),
                    "rawEmailStored": False,
                    "rawEvidenceStored": False,
                })
            return self._json(
                {"ok": True, "provenance": evidence},
                cache_control="no-store")

        if method == "DELETE":
            provenance_id = clean_text(data.get("provenanceId"), 64)
            row, _ = await self._provenance_row(
                env, import_id, provenance_id)
            if (
                    not row or row.get("basis") != "owner_supplied"
                    or int(row.get("revoked_at") or 0)):
                return self._json({"error": "not_found"}, status=404)
            now = int(self.d["now_ms"]())
            await self.d["d1_run"](
                env,
                "UPDATE contributor_invitation_provenance "
                "SET revoked_at=? WHERE id=? AND repo_id=? "
                "AND basis='owner_supplied'",
                now, provenance_id, import_id,
            )
            await self._audit(
                env, actor, "contributor_invitation.provenance_revoke",
                import_id, "success", {
                    "provenanceId": provenance_id,
                    "basis": "owner_supplied",
                })
            return self._json({
                "ok": True, "provenanceId": provenance_id, "revoked": True})

        if method != "POST":
            return self._json({"error": "method_not_allowed"}, status=405)
        contributor = clean_text(data.get("contributor"), 120)
        email_value = normalize_email(data.get("email"))
        source_type = clean_text(data.get("sourceType"), 80).lower()
        source_reference = clean_multiline(
            data.get("sourceReference"), 500)
        if (
                not contributor
                or contributor.casefold() not in contributor_identifiers(
                    record)):
            return self._json(
                {"error": "known_contributor_required"}, status=400)
        if not email_value:
            return self._json({"error": "valid_email_required"}, status=400)
        if (
                source_type not in OWNER_PROVENANCE_SOURCE_TYPES
                or len(source_reference) < 8):
            return self._json({
                "error": "owner_provenance_required",
                "allowedSourceTypes": list(OWNER_PROVENANCE_SOURCE_TYPES),
            }, status=400)
        actor_bi = await self.d["blind_index"](env, actor)
        evidence_token = await self.d["blind_index"](
            env, "invitation-evidence:owner:" + source_type + ":"
            + source_reference)
        evidence_digest = hashlib.sha256(
            str(evidence_token).encode()).hexdigest()
        saved = await self._save_invitation_provenance(
            env, import_id, contributor, email_value, "owner_supplied",
            actor_bi, actor, source_type, evidence_digest, evidence_digest)
        await self._audit(
            env, actor, "contributor_invitation.provenance_create",
            import_id, "success", {
                "provenanceId": saved["id"],
                "basis": saved["basis"],
                "sourceType": saved["sourceType"],
                "rawEmailStored": False,
                "rawEvidenceStored": False,
            })
        return self._json({
            "ok": True,
            "provenance": {
                key: saved[key] for key in (
                    "id", "contributor", "maskedEmail", "basis",
                    "sourceType", "evidenceDigest", "createdAt",
                    "rawEmailStored", "rawEvidenceStored")
            },
        }, status=201)

    async def _invitation_consent(self, env, request, import_id):
        """Record or revoke consent from a verified recipient account."""
        method = str(getattr(request, "method", "POST")).upper()
        if method not in ("POST", "DELETE"):
            return self._json({"error": "method_not_allowed"}, status=405)
        try:
            data = await request.json()
        except Exception:
            return self._json({"error": "invalid_json"}, status=400)
        actor, account = await self._actor(env, request, data)
        if not actor or not isinstance(account, dict):
            return self._json({"error": "invalid_session"}, status=401)
        _, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        contributor = clean_text(data.get("contributor"), 120)
        if (
                not contributor
                or contributor.casefold() not in contributor_identifiers(
                    record)):
            return self._json(
                {"error": "known_contributor_required"}, status=400)
        email_value = normalize_email(account.get("email"))
        if not email_value or not account.get("email_verified"):
            return self._json({
                "error": "verified_account_email_required",
            }, status=403)
        actor_bi = await self.d["blind_index"](env, actor)
        contributor_bi = await self.d["blind_index"](
            env, contributor_blind_value(contributor))
        email_bi = await self.d["blind_index"](
            env, "contributor-email:" + email_value)
        evidence_key = hashlib.sha256(
            (
                import_id + "\n" + actor_bi + "\n" + contributor_bi
                + "\n" + email_bi
            ).encode()
        ).hexdigest()
        provenance_id = opaque_record_id(
            "prov", import_id, contributor_bi, email_bi, "prior_consent",
            evidence_key)
        if method == "DELETE":
            now = int(self.d["now_ms"]())
            await self.d["d1_run"](
                env,
                "UPDATE contributor_invitation_provenance SET revoked_at=? "
                "WHERE id=? AND repo_id=? AND created_by_bi=? "
                "AND basis='prior_consent'",
                now, provenance_id, import_id, actor_bi,
            )
            await self._audit(
                env, actor, "contributor_invitation.consent_revoke",
                import_id, "success", {
                    "provenanceId": provenance_id,
                    "basis": "prior_consent",
                })
            return self._json({
                "ok": True, "provenanceId": provenance_id,
                "consented": False})
        saved = await self._save_invitation_provenance(
            env, import_id, contributor, email_value, "prior_consent",
            actor_bi, actor, "verified-account-consent",
            evidence_key, evidence_key)
        await self._audit(
            env, actor, "contributor_invitation.consent",
            import_id, "success", {
                "provenanceId": saved["id"],
                "basis": "prior_consent",
                "verifiedAccountEmail": True,
                "rawEmailStored": False,
            })
        return self._json({
            "ok": True,
            "consented": True,
            "provenance": {
                "id": saved["id"],
                "contributor": saved["contributor"],
                "maskedEmail": saved["maskedEmail"],
                "basis": saved["basis"],
                "createdAt": saved["createdAt"],
                "rawEmailStored": False,
            },
        }, status=201)

    async def _invitations(self, env, request, import_id):
        method = str(getattr(request, "method", "GET")).upper()
        data = {}
        if method == "POST":
            try:
                data = await request.json()
            except Exception:
                return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        row, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        if not actor or not await self._can_admin(env, actor, record):
            return self._json({"error": "forbidden"}, status=403)
        if method == "GET":
            rows = await self.d["d1_all"](
                env,
                "SELECT id, status, created_at, sent_at, data FROM "
                "contributor_invitations WHERE repo_id=? "
                "ORDER BY created_at DESC LIMIT ?",
                import_id, MAX_INVITATIONS_PER_REPOSITORY,
            )
            invitations = []
            for item in rows or []:
                saved = await self.d["decrypt_row"](env, item.get("data"))
                if not isinstance(saved, dict):
                    continue
                invitations.append({
                    "id": item.get("id"),
                    "contributor": saved.get("contributor"),
                    "maskedEmail": saved.get("maskedEmail"),
                    "basis": saved.get("basis"),
                    "provenanceId": saved.get("provenanceId"),
                    "inviter": saved.get("inviter"),
                    "status": item.get("status"),
                    "createdAt": int(item.get("created_at") or 0),
                    "sentAt": int(item.get("sent_at") or 0),
                })
            return self._json(
                {"ok": True, "invitations": invitations},
                cache_control="no-store")
        if method != "POST":
            return self._json({"error": "method_not_allowed"}, status=405)
        email_value = normalize_email(data.get("email"))
        contributor = clean_text(data.get("contributor"), 120)
        provenance_id = clean_text(data.get("provenanceId"), 64)
        if not email_value:
            return self._json({"error": "valid_email_required"}, status=400)
        if not contributor or contributor.casefold() not in contributor_identifiers(
                record):
            return self._json(
                {"error": "known_contributor_required"}, status=400)
        # A request boolean is not evidence. The referenced durable row is
        # matched again by the INSERT trigger so revocation races fail closed.
        if "basisConfirmed" in data or not provenance_id:
            return self._json(
                {
                    "error": "invitation_provenance_required",
                },
                status=400,
            )
        email_bi = await self.d["blind_index"](
            env, "contributor-email:" + email_value)
        contributor_bi = await self.d["blind_index"](
            env, contributor_blind_value(contributor))
        provenance_row, provenance = await self._provenance_row(
            env, import_id, provenance_id)
        if (
                not provenance_row or not isinstance(provenance, dict)
                or int(provenance_row.get("revoked_at") or 0)
                or provenance_row.get("basis") not in INVITATION_BASES
                or not hmac.compare_digest(
                    str(provenance_row.get("email_bi") or ""),
                    str(email_bi))
                or not hmac.compare_digest(
                    str(provenance_row.get("contributor_bi") or ""),
                    str(contributor_bi))):
            return self._json(
                {"error": "invitation_provenance_invalid"}, status=409)
        basis = str(provenance_row.get("basis") or "")
        opted_out = await self.d["d1_first"](
            env,
            "SELECT opted_out_at FROM contributor_invitation_optouts "
            "WHERE email_bi=?",
            email_bi,
        )
        if opted_out:
            return self._json({"error": "recipient_opted_out"}, status=409)
        now = int(self.d["now_ms"]())
        day = now // (24 * 60 * 60 * 1000)
        actor_bi = await self.d["blind_index"](env, actor)
        daily = await self.d["d1_first"](
            env,
            "SELECT sent_count FROM contributor_invitation_rate "
            "WHERE inviter_bi=? AND day_bucket=? AND repo_id='*'",
            actor_bi, day,
        )
        repo_daily = await self.d["d1_first"](
            env,
            "SELECT sent_count FROM contributor_invitation_rate "
            "WHERE inviter_bi=? AND day_bucket=? AND repo_id=?",
            actor_bi, day, import_id,
        )
        if int((daily or {}).get("sent_count") or 0) >= INVITATION_DAILY_LIMIT:
            return self._json(
                {"error": "daily_invitation_limit"}, status=429)
        if int((repo_daily or {}).get("sent_count") or 0) >= \
                INVITATION_REPO_DAILY_LIMIT:
            return self._json(
                {"error": "repository_invitation_limit"}, status=429)
        recent = await self.d["d1_first"](
            env,
            "SELECT created_at FROM contributor_invitations "
            "WHERE email_bi=? AND status IN ('pending','sent') "
            "AND created_at>? ORDER BY created_at DESC LIMIT 1",
            email_bi, now - INVITATION_RECIPIENT_COOLDOWN_MS,
        )
        if recent:
            return self._json(
                {"error": "recipient_invited_recently"}, status=409)
        preview = invitation_preview(
            record, actor, contributor, email_value, basis,
            self.d["public_origin"](env, request))
        if data.get("dryRun") or not data.get("confirm"):
            preview["to"] = _mask_email(email_value)
            return self._json({
                "ok": True,
                "dryRun": True,
                "preview": preview,
                "provenance": {
                    "id": provenance_id,
                    "basis": basis,
                    "sourceType": provenance.get("sourceType"),
                    "rawEmailStored": False,
                    "rawEvidenceStored": False,
                },
                "limits": {
                    "perInviterPerDay": INVITATION_DAILY_LIMIT,
                    "perRepositoryPerDay": INVITATION_REPO_DAILY_LIMIT,
                    "recipientCooldownDays": (
                        INVITATION_RECIPIENT_COOLDOWN_MS
                        // (24 * 60 * 60 * 1000)),
                },
            })
        count = await self.d["d1_first"](
            env,
            "SELECT COUNT(*) AS n FROM contributor_invitations WHERE repo_id=?",
            import_id,
        )
        if int((count or {}).get("n") or 0) >= MAX_INVITATIONS_PER_REPOSITORY:
            return self._json(
                {"error": "too_many_repository_invitations"}, status=429)
        invite_id = opaque_record_id(
            "inv", import_id, actor_bi, email_bi, now)
        token = await self.d["invitation_token"](
            env, import_id, invite_id, email_value)
        preview = invitation_preview(
            record, actor, contributor, email_value, basis,
            self.d["public_origin"](env, request), invite_id, token)
        saved = {
            "id": invite_id,
            "repoId": import_id,
            "contributor": contributor,
            "maskedEmail": _mask_email(email_value),
            "basis": basis,
            "provenanceId": provenance_id,
            "provenanceEvidenceDigest": provenance.get("evidenceDigest"),
            # The raw address is needed only for the immediate delivery call.
            # Retain a one-way verifier for later opt-out/report requests.
            "actionTokenDigest": hashlib.sha256(token.encode()).hexdigest(),
            "inviter": actor,
            "createdAt": now,
            "provider": record.get("provider"),
            "repository": record.get("fullName"),
        }
        encoded = await self.d["encrypt_row"](env, saved)
        try:
            # The schema trigger checks every cap/cooldown and increments both
            # rate counters in this same statement. Preflight reads above are
            # advisory UX only; this insert is the concurrency boundary.
            await self.d["d1_run"](
                env,
                "INSERT INTO contributor_invitations "
                "(id, repo_id, inviter_bi, email_bi, contributor_bi, "
                "provenance_id, status, data, created_at, sent_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,0)",
                invite_id, import_id, actor_bi, email_bi, contributor_bi,
                provenance_id, "pending", encoded, now,
            )
        except Exception as exc:
            limited = reservation_error(exc)
            if limited:
                code, response_status = limited
                return self._json({"error": code}, status=response_status)
            return self._json(
                {"error": "invitation_reservation_failed"}, status=503)

        try:
            sent = bool(await self.d["send_email"](
                env, email_value, preview["subject"],
                preview["text"], preview["html"]))
        except Exception:
            sent = False
        status = "sent" if sent else "delivery_unconfigured"
        sent_at = now if sent else 0
        if sent:
            # Transitioning pending -> sent retains the trigger-backed rate
            # reservation and recipient cooldown.
            await self.d["d1_run"](
                env,
                "UPDATE contributor_invitations SET status=?, sent_at=? "
                "WHERE id=?",
                status, sent_at, invite_id,
            )
        else:
            # Keep no undelivered invitation/address record. The AFTER DELETE
            # trigger releases both counters atomically with this rollback.
            await self.d["d1_run"](
                env,
                "DELETE FROM contributor_invitations "
                "WHERE id=? AND status='pending'",
                invite_id,
            )
        await self._audit(
            env, actor, "contributor_invitation.send", import_id,
            "success" if sent else "failed", {
                "invitationId": invite_id,
                "provenanceId": provenance_id,
                "basis": basis,
                "delivery": status,
                "recipient": _mask_email(email_value),
            })
        return self._json({
            "ok": True,
            "invitation": {
                "id": invite_id,
                "contributor": contributor,
                "maskedEmail": _mask_email(email_value),
                "inviter": actor,
                "status": status,
                "sentAt": sent_at,
            },
            "deliveryConfigured": bool(sent),
        }, status=201 if sent else 202)

    async def _invitation_action(
            self, env, request, import_id, invite_id, action):
        if not valid_record_id(invite_id):
            return self._json({"error": "not_found"}, status=404)
        row = await self.d["d1_first"](
            env,
            "SELECT email_bi, status, data FROM contributor_invitations "
            "WHERE id=? AND repo_id=?",
            invite_id, import_id,
        )
        if not row:
            return self._json({"error": "not_found"}, status=404)
        saved = await self.d["decrypt_row"](env, row.get("data"))
        if not isinstance(saved, dict):
            return self._json({"error": "not_found"}, status=404)
        query = urlparse(str(getattr(request, "url", ""))).query
        params = {}
        for item in query.split("&") if query else []:
            key, _, value = item.partition("=")
            params[unquote(key)] = unquote(value)
        supplied = clean_text(params.get("token"), 128)
        expected_digest = clean_text(
            saved.get("actionTokenDigest"), 64).lower()
        valid_token = False
        if supplied and len(expected_digest) == 64:
            supplied_digest = hashlib.sha256(supplied.encode()).hexdigest()
            valid_token = hmac.compare_digest(
                supplied_digest, expected_digest)
        elif supplied and saved.get("email"):
            # Compatibility for invitations created before raw recipient
            # minimization was deployed.
            expected = await self.d["invitation_token"](
                env, import_id, invite_id, saved.get("email", ""))
            valid_token = hmac.compare_digest(supplied, expected)
        if not valid_token:
            return self._json({"error": "invalid_invitation_token"}, status=403)
        method = str(getattr(request, "method", "GET")).upper()
        if method == "GET":
            return self._json({
                "ok": True,
                "confirmationRequired": True,
                "action": action,
                "invitationId": invite_id,
                "method": "POST",
                "notice": (
                    "Submit POST to confirm. GET never changes invitation or "
                    "opt-out state, which protects against email link scanners."),
            })
        if method != "POST":
            return self._json({"error": "method_not_allowed"}, status=405)
        now = int(self.d["now_ms"]())
        if action == "opt-out":
            await self.d["d1_run"](
                env,
                "INSERT INTO contributor_invitation_optouts "
                "(email_bi, opted_out_at, source_invitation_id) VALUES (?,?,?) "
                "ON CONFLICT(email_bi) DO UPDATE SET "
                "opted_out_at=excluded.opted_out_at, "
                "source_invitation_id=excluded.source_invitation_id",
                row.get("email_bi"), now, invite_id,
            )
            await self.d["d1_run"](
                env,
                "UPDATE contributor_invitations SET status='opted_out' "
                "WHERE id=?",
                invite_id,
            )
            return self._json({
                "ok": True,
                "optedOut": True,
                "notice": "Future contributor invitations are blocked.",
            })
        report_id = opaque_record_id("report", invite_id, now)
        try:
            body = await request.json()
        except Exception:
            body = {}
        reason = clean_text(
            body.get("reason") if isinstance(body, dict) else "", 240)
        report = {
            "id": report_id,
            "invitationId": invite_id,
            "reason": reason or "unsolicited invitation",
            "createdAt": now,
        }
        encoded = await self.d["encrypt_row"](env, report)
        await self.d["d1_run"](
            env,
            "INSERT INTO contributor_invitation_abuse "
            "(id, invitation_id, data, created_at, status) "
            "VALUES (?,?,?,?,?)",
            report_id, invite_id, encoded, now, "open",
        )
        await self.d["d1_run"](
            env,
            "UPDATE contributor_invitations SET status='reported' WHERE id=?",
            invite_id,
        )
        return self._json({
            "ok": True, "reported": True, "reportId": report_id}, status=201)

    async def _logo(self, env, request, import_id):
        actor, _ = await self._actor(env, request)
        _, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        return await self.logo_for_record(env, import_id)

    async def logo_for_record(self, env, repository_id):
        """Return the approved logo for any repository namespace, if any."""
        logo = await self._official_logo(env, repository_id)
        return self._json({
            "ok": True,
            "repositoryId": repository_id,
            "logo": logo,
        }, cache_control="no-store")

    async def _logo_suggestions(self, env, request, import_id):
        method = str(getattr(request, "method", "GET")).upper()
        data = {}
        if method in ("POST", "PATCH"):
            try:
                data = await request.json()
            except Exception:
                return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        _, record = await self._load(env, import_id, actor)
        if not record:
            return self._json({"error": "not_found"}, status=404)
        return await self._logo_suggestions_for_record(
            env, method, data, actor, import_id, record,
            lambda name: self._can_admin(env, name, record),
        )

    async def native_logo_suggestions(
            self, env, request, repository_id, record, can_admin):
        """Run the shared suggestion/review workflow for a native catalog repo.

        ``repository_id`` is an opaque, namespace-separated stable identifier;
        private names therefore never enter the plaintext index.
        """
        method = str(getattr(request, "method", "GET")).upper()
        data = {}
        if method in ("POST", "PATCH"):
            try:
                data = await request.json()
            except Exception:
                return self._json({"error": "invalid_json"}, status=400)
        actor, _ = await self._actor(env, request, data)
        return await self._logo_suggestions_for_record(
            env, method, data, actor, repository_id, record, can_admin)

    async def _logo_suggestions_for_record(
            self, env, method, data, actor, repository_id, record,
            can_admin):
        if method == "GET":
            rows = await self.d["d1_all"](
                env,
                "SELECT id, status, official, created_at, data FROM "
                "repository_logo_suggestions WHERE repo_id=? "
                "ORDER BY official DESC, created_at DESC LIMIT ?",
                repository_id, MAX_SUGGESTIONS_PER_REPOSITORY,
            )
            suggestions = []
            for row in rows or []:
                saved = await self.d["decrypt_row"](env, row.get("data"))
                if not isinstance(saved, dict):
                    continue
                suggestions.append({
                    "id": row.get("id"),
                    "status": row.get("status"),
                    "official": bool(row.get("official")),
                    "proposer": saved.get("proposer"),
                    "kind": saved.get("kind"),
                    "image": saved.get("image"),
                    "attribution": saved.get("attribution"),
                    "aiGenerated": bool(saved.get("aiGenerated")),
                    "createdAt": int(row.get("created_at") or 0),
                })
            return self._json({"ok": True, "suggestions": suggestions})
        if method == "POST":
            if not actor:
                return self._json({"error": "invalid_session"}, status=401)
            if not data.get("rightsConfirmed"):
                return self._json(
                    {"error": "logo_rights_confirmation_required"}, status=400)
            try:
                image = validate_uploaded_logo(data.get("imageData"))
            except ValueError as exc:
                return self._json({"error": str(exc)}, status=400)
            direct = clean_text(data.get("action"), 20) == "replace"
            if direct and not await can_admin(actor):
                return self._json({"error": "forbidden"}, status=403)
            proposer_bi = await self.d["blind_index"](env, actor)

            replaced_suggestion_id = ""
            if direct:
                current = await self.d["d1_first"](
                    env,
                    "SELECT id FROM repository_logo_suggestions "
                    "WHERE repo_id=? AND official=1 LIMIT 1",
                    repository_id,
                )
                replaced_suggestion_id = str(
                    (current or {}).get("id") or "")
            else:
                count = await self.d["d1_first"](
                    env,
                    "SELECT COUNT(*) AS n FROM repository_logo_suggestions "
                    "WHERE repo_id=?",
                    repository_id,
                )
                repository_count = int((count or {}).get("n") or 0)
                pending = await self.d["d1_first"](
                    env,
                    "SELECT COUNT(*) AS n FROM repository_logo_suggestions "
                    "WHERE repo_id=? AND proposer_bi=? AND status='pending'",
                    repository_id, proposer_bi,
                )
                if int((pending or {}).get("n") or 0) >= \
                        MAX_PENDING_LOGO_SUGGESTIONS_PER_PROPOSER:
                    return self._json(
                        {"error": "too_many_logo_suggestions_by_proposer"},
                        status=429,
                    )
                if repository_count >= \
                        MAX_SUGGESTIONS_PER_REPOSITORY - 1:
                    return self._json(
                        {"error": "too_many_logo_suggestions"}, status=429)

            now = int(self.d["now_ms"]())
            suggestion_id = opaque_record_id(
                "logo", repository_id, actor, now, image["sizeBytes"])
            saved = {
                "id": suggestion_id,
                "repoId": repository_id,
                "proposer": actor,
                "kind": "owner_upload" if direct else "public_suggestion",
                "image": image,
                "attribution": clean_text(data.get("attribution"), 240),
                "rightsConfirmed": True,
                "aiGenerated": bool(data.get("aiGenerated")),
                "createdAt": now,
            }
            if replaced_suggestion_id:
                saved["replacesSuggestionId"] = replaced_suggestion_id
            encoded = await self.d["encrypt_row"](env, saved)
            status = "approved" if direct else "pending"
            official = 1 if direct else 0
            try:
                # Quota checks, owner-history cleanup, and official replacement
                # are schema triggers on this one atomic insert.
                await self.d["d1_run"](
                    env,
                    "INSERT INTO repository_logo_suggestions "
                    "(id, repo_id, proposer_bi, status, official, data, "
                    "created_at, reviewed_at, reviewed_by_bi) "
                    "VALUES (?,?,?,?,?,?,?,?,?)",
                    suggestion_id, repository_id, proposer_bi, status, official,
                    encoded, now, now if direct else 0,
                    proposer_bi if direct else "",
                )
            except Exception as exc:
                limited = reservation_error(exc)
                if limited:
                    code, response_status = limited
                    return self._json({"error": code}, status=response_status)
                return self._json(
                    {"error": "logo_suggestion_write_failed"}, status=503)
            return self._json({
                "ok": True,
                "suggestion": {
                    "id": suggestion_id,
                    "status": status,
                    "official": bool(official),
                    "image": image,
                    "proposer": actor,
                },
            }, status=201)
        if method == "PATCH":
            if not actor or not await can_admin(actor):
                return self._json({"error": "forbidden"}, status=403)
            suggestion_id = clean_text(data.get("suggestionId"), 40)
            action = clean_text(data.get("action"), 20).lower()
            if not valid_record_id(suggestion_id) or action not in (
                    "approve", "reject"):
                return self._json(
                    {"error": "invalid_logo_review"}, status=400)
            row = await self.d["d1_first"](
                env,
                "SELECT id FROM repository_logo_suggestions "
                "WHERE id=? AND repo_id=?",
                suggestion_id, repository_id,
            )
            if not row:
                return self._json({"error": "not_found"}, status=404)
            now = int(self.d["now_ms"]())
            reviewer_bi = await self.d["blind_index"](env, actor)
            if action == "approve":
                await self.d["d1_run"](
                    env,
                    "UPDATE repository_logo_suggestions SET "
                    "status='approved', official=1, reviewed_at=?, "
                    "reviewed_by_bi=? WHERE id=? AND repo_id=?",
                    now, reviewer_bi, suggestion_id, repository_id,
                )
            else:
                await self.d["d1_run"](
                    env,
                    "UPDATE repository_logo_suggestions SET "
                    "status='rejected', official=0, reviewed_at=?, "
                    "reviewed_by_bi=? WHERE id=? AND repo_id=?",
                    now, reviewer_bi, suggestion_id, repository_id,
                )
            return self._json({
                "ok": True,
                "suggestionId": suggestion_id,
                "status": "approved" if action == "approve" else "rejected",
                "official": action == "approve",
            })
        return self._json({"error": "method_not_allowed"}, status=405)

    async def handle(self, env, request, path):
        await self.d["ensure_schema"](env)
        method = str(getattr(request, "method", "GET")).upper()
        normalized = str(path or "").rstrip("/") or "/"
        prefix = "/api/repository-imports"
        if normalized == prefix:
            if method == "GET":
                return await self._list(env, request)
            if method == "POST":
                return await self._create(env, request)
            return self._json({"error": "method_not_allowed"}, status=405)
        if normalized == prefix + "/migrations":
            if method == "GET":
                return await self._migration_list(env, request)
            if method == "POST":
                return await self._migration_start(env, request)
            return self._json({"error": "method_not_allowed"}, status=405)
        if normalized == prefix + "/migration-metrics":
            if method == "GET":
                return await self._migration_metrics(env, request)
            return self._json({"error": "method_not_allowed"}, status=405)
        migration_prefix = prefix + "/migrations/"
        if normalized.startswith(migration_prefix):
            migration_parts = normalized[len(migration_prefix):].split("/")
            job_id = migration_parts[0]
            if not _MIGRATION_ID_RE.fullmatch(job_id):
                return self._json({"error": "not_found"}, status=404)
            if len(migration_parts) == 1:
                if method == "GET":
                    return await self._migration_get(env, request, job_id)
                return self._json({"error": "method_not_allowed"}, status=405)
            if len(migration_parts) == 2:
                action = migration_parts[1]
                if action == "resume" and method == "POST":
                    return await self._migration_resume(env, request, job_id)
                if action == "objects" and method == "GET":
                    return await self._migration_objects(env, request, job_id)
                if action == "abandon" and method == "POST":
                    return await self._migration_abandon(env, request, job_id)
                if action == "rollback" and method == "POST":
                    return await self._migration_rollback(env, request, job_id)
                if action == "report" and method == "GET":
                    return await self._migration_report(env, request, job_id)
                if action == "retry" and method == "POST":
                    return await self._migration_retry(env, request, job_id)
            return self._json({"error": "not_found"}, status=404)
        if normalized == prefix + "/discover":
            if method == "POST":
                return await self._discover(env, request)
            return self._json({"error": "method_not_allowed"}, status=405)
        if not normalized.startswith(prefix + "/"):
            return self._json({"error": "not_found"}, status=404)
        parts = normalized[len(prefix) + 1:].split("/")
        import_id = parts[0]
        if not valid_import_id(import_id):
            return self._json({"error": "not_found"}, status=404)
        if len(parts) == 1:
            if method == "GET":
                return await self._get(env, request, import_id)
            if method == "PATCH":
                return await self._patch(env, request, import_id)
            if method == "DELETE":
                return await self._delete(env, request, import_id)
            return self._json({"error": "method_not_allowed"}, status=405)
        if len(parts) == 2 and parts[1] == "mirror-volunteers":
            return await self._volunteers(env, request, import_id)
        if len(parts) == 2 and parts[1] == "invitations":
            return await self._invitations(env, request, import_id)
        if len(parts) == 2 and parts[1] == "invitation-provenance":
            return await self._invitation_provenance(
                env, request, import_id)
        if len(parts) == 2 and parts[1] == "invitation-consent":
            return await self._invitation_consent(
                env, request, import_id)
        if len(parts) == 4 and parts[1] == "invitations" and parts[3] in (
                "opt-out", "report"):
            return await self._invitation_action(
                env, request, import_id, parts[2], parts[3])
        if len(parts) == 2 and parts[1] == "logo":
            return await self._logo(env, request, import_id)
        if len(parts) == 2 and parts[1] == "logo-suggestions":
            return await self._logo_suggestions(env, request, import_id)
        return self._json({"error": "not_found"}, status=404)


def _mask_email(value):
    local, _, domain = str(value or "").partition("@")
    if not domain:
        return ""
    shown = local[:1] + ("*" * max(2, min(8, len(local) - 1)))
    return shown + "@" + domain
