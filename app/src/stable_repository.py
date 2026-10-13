"""Provider-independent repository identity and verified dependency routes.

Implements ``forkmesh-dependency-workflows-v1`` (www/docs/dependency-workflows.md,
conformance/dependencies/v1/contract.json). Pure and js-free: the public
repository index, the dependency conformance probe, and the tests share it.

A repository's logical identity is FM-ID-001 over its verified root commit, so
it never mentions a host, owner, organization alias, or repository name. Those
are routing hints: a build may reach the same identity through any provider.
A route is used only when it proves it serves a state the source signed, and a
dependency pin additionally requires the exact commit object ID. Every skipped
route is reported with a closed reason code instead of being silently replaced.
"""

import hashlib
import re
import unicodedata

IDENTITY_PREFIX = "urn:forkmesh:repository:sha256:"
IDENTITY_DOMAIN = b"forkmesh-repository-identity-v1\n"
VERIFIED_ROOT_PREFIX = "git-root-commit:"
RESOLUTION_SCHEMA = "forkmesh-dependency-resolution-v1"

_OBJECT_ID = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
_STATE_HASH = re.compile(r"[0-9a-f]{64}")
_IDENTITY = re.compile(re.escape(IDENTITY_PREFIX) + r"[0-9a-f]{64}")
_REF = re.compile(r"refs/(?:heads|tags)/[A-Za-z0-9._/+-]{1,200}")

# Closed per-route reasons, in the order they are checked.
ROUTE_REASONS = (
    "unreachable",
    "identity_mismatch",
    "state_unverified",
    "commit_missing",
    "ref_moved",
)
PIN_ERRORS = ("identity_invalid", "mutable_pin", "ref_invalid")
STATUSES = ("resolved", "failed_over", "unavailable", "rejected")


def object_id(value):
    """A full lowercase Git object ID, or "" for anything abbreviated or symbolic."""
    text = str(value or "").strip().lower()
    return text if _OBJECT_ID.fullmatch(text) else ""


def verified_root(root_commit):
    """The FM-ID-001 verified-root string for a root commit, or ""."""
    oid = object_id(root_commit)
    return VERIFIED_ROOT_PREFIX + oid if oid else ""


def repository_identity(root_commit):
    """``urn:forkmesh:repository:sha256:<hex>`` for a root commit, or "".

    Fails closed: an absent or malformed root yields no identity rather than
    one derived from a name, which would change when the repository moves.
    """
    root = verified_root(root_commit)
    if not root:
        return ""
    digest = hashlib.sha256(
        IDENTITY_DOMAIN + unicodedata.normalize("NFC", root).encode("utf-8")
    ).hexdigest()
    return IDENTITY_PREFIX + digest


def record_identity(record):
    """Logical identity of a signed catalog record (its ``rootCommit``)."""
    return repository_identity((record or {}).get("rootCommit"))


def valid_identity(value):
    return bool(_IDENTITY.fullmatch(str(value or "")))


def pin_errors(pin):
    """Closed error codes that make a dependency pin unusable, in check order."""
    pin = pin if isinstance(pin, dict) else {}
    errors = []
    if not valid_identity(pin.get("identity")):
        errors.append("identity_invalid")
    if not object_id(pin.get("commit")):
        errors.append("mutable_pin")
    ref = pin.get("ref")
    if ref not in (None, "") and not _REF.fullmatch(str(ref)):
        errors.append("ref_invalid")
    return errors


def _accepted(states):
    return {
        str(value).lower() for value in (states or ())
        if _STATE_HASH.fullmatch(str(value).lower())
    }


def _route_reason(identity, route, accepted, pin=None):
    if route.get("reachable") is not True:
        return "unreachable"
    if route.get("identity") != identity:
        return "identity_mismatch"
    state = str(route.get("stateHash") or "").lower()
    if state not in accepted:
        return "state_unverified"
    if pin is None:
        return ""
    if route.get("hasCommit") is not True:
        return "commit_missing"
    ref = pin.get("ref")
    refs = route.get("refs") if isinstance(route.get("refs"), dict) else {}
    if ref and object_id(refs.get(ref)) != object_id(pin["commit"]):
        return "ref_moved"
    return ""


def _select(report, identity, routes, accepted, pin):
    for index, route in enumerate(routes or ()):
        route = route if isinstance(route, dict) else {}
        label = str(route.get("route") or "route-%d" % (index + 1))
        reason = _route_reason(identity, route, accepted, pin)
        report["attempts"].append({
            "route": label,
            "outcome": "skipped" if reason else "selected",
            "reason": reason or None,
        })
        if not reason:
            report["selected"] = label
            report["fallback"] = index > 0
            report["status"] = "failed_over" if index > 0 else "resolved"
            return report
    report["status"] = "unavailable"
    report["errors"] = ["no_verified_route"]
    return report


def _report(identity, commit, errors):
    return {
        "schema": RESOLUTION_SCHEMA,
        "identity": identity,
        "commit": commit,
        "status": "rejected" if errors else "unavailable",
        "selected": None,
        "fallback": False,
        "errors": list(errors),
        "attempts": [],
    }


def select_route(identity, routes, accepted_states):
    """Choose the first route, in declared order, serving a signed state.

    ``routes`` is the caller's ordered list of observations, one per provider:
    ``{"route", "reachable", "identity", "stateHash"}``. ``accepted_states``
    holds the repository state digests (FM-ID-005) the source signed. Order is
    the caller's declaration, never load or latency, so the same observations
    always select the same route. This is the check a relay can make before it
    streams any bytes; clients still enforce their exact commit pins.
    """
    identity = str(identity or "")
    errors = [] if valid_identity(identity) else ["identity_invalid"]
    report = _report(identity, "", errors)
    if errors:
        return report
    return _select(report, identity, routes, _accepted(accepted_states), None)


def resolve(pin, routes, accepted_states):
    """Resolve one dependency pin: a signed state that holds the exact commit.

    Beyond ``select_route``, each route observation carries ``hasCommit`` and
    ``refs`` (``{refname: object ID}``). Status is ``resolved`` (the first
    route served), ``failed_over`` (a later route served and the skipped ones
    are listed), ``unavailable`` (no route proved the content), or ``rejected``
    (the pin itself is unusable, for example a branch name instead of a commit).
    A route that fails any check is never selected.
    """
    pin = pin if isinstance(pin, dict) else {}
    errors = pin_errors(pin)
    report = _report(
        str(pin.get("identity") or ""), object_id(pin.get("commit")), errors)
    if errors:
        return report
    return _select(
        report, pin["identity"], routes, _accepted(accepted_states), pin)
