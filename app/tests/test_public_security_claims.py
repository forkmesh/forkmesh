#!/usr/bin/env python3
"""Regression checks for public chat, bounty, and mobile capability claims."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
APP_PUBLIC = ROOT / "public"
PUBLIC = PROJECT / "www" / "public"


def _public_corpus():
    paths = [PROJECT / "README.md"]
    for public in (APP_PUBLIC, PUBLIC):
        paths.extend(public.rglob("*.html"))
        paths.extend(public.rglob("*.js"))
    return "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in paths
    )


def test_protocol_states_actual_shared_key_derivation_and_retention():
    protocol = (
        PUBLIC / "docs" / "protocol" / "index.html"
    ).read_text(encoding="utf-8")
    normalized = " ".join(protocol.split())
    for claim in (
        'SHA-256(DATA_KEY + ":room-chat-passphrase-v1")',
        'SHA-256(DATA_KEY + ":room-chat-passphrase-v2:" + lower(owner + "/" + repo))',
        "authorized for that repository",
        "PBKDF2-HMAC-SHA256",
        "210,000 rounds",
        "7 days",
        "500 frames",
        "relay operator can derive the outer room keys",
    ):
        assert claim in normalized


def test_public_room_copy_distinguishes_public_and_protected_chat():
    corpus = _public_corpus().lower()
    forbidden = (
        "the relay is a <strong>blind</strong> relay",
        "the server never sees plaintext",
        "end-to-end encrypted before it ever touches the relay",
        "the relay only ever sees ciphertext",
        "persists no message bodies, ever",
        "room content is not meant to be readable by the relay",
    )
    for claim in forbidden:
        assert claim not in corpus
    assert "relay-readable authenticated shared-key" in corpus
    assert "public channels are controller-readable" in corpus
    assert "device-to-device encryption" in corpus
    assert "500 persisted frames" in corpus


def test_recipient_encryption_update_carries_its_exact_trust_boundary():
    update = (
        PUBLIC / "blog" / "beyond-the-room-key" / "index.html"
    ).read_text(encoding="utf-8")
    normalized = " ".join(update.split())

    for claim in (
        "Public means controller-readable and integration-compatible.",
        "every message gets a fresh content key wrapped to known participant devices",
        "X25519 and HKDF-SHA256",
        "Public-key discovery is not independently authenticated",
        "It cannot retroactively open retained messages",
        "does not claim post-compromise security",
    ):
        assert claim in normalized

    encrypted_rooms = (
        PUBLIC / "blog" / "encrypted-repo-rooms" / "index.html"
    ).read_text(encoding="utf-8")
    assert "/assets/blog/features/beyond-the-room-key.webp" in encrypted_rooms
    assert "/assets/blog/features/encrypted-repo-rooms.webp" not in encrypted_rooms

    # The superseded artwork may remain visible only inside the retrospective,
    # where its old controller-blind claim is explicitly corrected.
    historical_image = "/assets/blog/features/encrypted-repo-rooms.webp"
    references = [
        path
        for path in PUBLIC.rglob("*.html")
        if historical_image in path.read_text(encoding="utf-8")
    ]
    assert references == [
        PUBLIC / "blog" / "beyond-the-room-key" / "index.html"
    ]
    assert "baked-in copy overstated controller blindness" in update


def test_historical_private_chat_designs_point_to_the_current_protocol():
    records = (
        PROJECT / "www" / "docs" / "superpowers" / "plans" /
        "2026-07-24-private-admin-chat-channels.md",
        PROJECT / "www" / "docs" / "superpowers" / "specs" /
        "2026-07-24-private-admin-chat-channels-design.md",
    )

    for record in records:
        normalized = " ".join(record.read_text(encoding="utf-8").split()).lower()
        assert "august 28, 2026" in normalized
        assert "../../security/sealed-chat.md" in normalized
        assert "public channels remain" in normalized
        assert "controller-readable" in normalized
        assert "active-controller limitations" in normalized


def test_public_durability_and_private_metadata_claims_are_bounded():
    corpus = _public_corpus().lower()
    forbidden = (
        "code that can't be taken down",
        "never lose the code that matters",
        "the worker and unauthorized mirror operators receive no private repository name or plaintext",
        "without packfile or message-body custody",
        "private repository names and private-agent content use separate owner-only encryption boundaries",
        "secure by default",
        "keep working even when the original host is offline",
        "authorized private repositories appear only to their owners and are never broadcast",
        "only owner, name, description, channel, sync time, maintainer key, signature",
    )
    for claim in forbidden:
        assert claim not in corpus
    assert "code beyond a single host" in corpus
    assert "preserve source-code repositories across independent hosts" in corpus
    assert "stores that metadata encrypted" in corpus
    assert "chat has bounded message retention; public channels are controller-readable" in corpus
    assert "private repository names reach forkmesh servers on authenticated control-plane requests" in corpus
    assert "private repository content and new private-agent payloads use separate client-side owner-sealed boundaries" in corpus
    assert "signed by default" in corpus
    assert "eligible, healthy, integrity-matching mirror" in corpus
    assert "their owner and explicitly authorized collaborators" in corpus
    assert "signed discovery and health metadata" in corpus
    assert "opaque client-encrypted replicas" in corpus


def test_provider_import_token_copy_discloses_worker_transit():
    corpus = _public_corpus().lower()
    assert "the token is sent only to the provider" not in corpus
    assert (
        "the token passes through forkmesh’s worker only for this request "
        "and is forwarded to the provider; it is not persisted"
    ) in corpus


def test_marketing_copy_matches_guest_registration_and_bounded_availability():
    corpus = _public_corpus().lower()
    for stale_claim in (
        "sign up with nothing but a key and a password",
        "no email, no phone",
        "every node has a public profile",
        "shows every online node",
        "the mesh always knows which nodes carry copies",
        "host offline? any mirror node serves",
        "failover kicks in the moment a host actually drops",
        "every repository gets its own encrypted room",
        "every repository would still exist",
        "always cloneable",
        "stay fresh automatically",
        "zero-custody relay",
    ):
        assert stale_claim not in corpus
    guest_post = (
        PUBLIC / "blog" / "anonymous-accounts" / "index.html"
    ).read_text(encoding="utf-8").lower()
    assert "enter the forkmesh world immediately as a guest" in guest_post
    assert "email, password, and email verification" in guest_post
    readme = (PROJECT / "README.md").read_text(encoding="utf-8").lower()
    normalized_readme = " ".join(readme.split())
    assert "public mirror eligibility requires a fresh account-bound signature" in normalized_readme
    assert "healthy direct https endpoint" in normalized_readme
    assert "an integrity-matching repository proof" in normalized_readme


def test_retired_bounty_and_phone_mirror_advertising_stays_removed():
    readme = (PROJECT / "README.md").read_text(encoding="utf-8").lower()
    changelog = (PUBLIC / "changelog.html").read_text(encoding="utf-8").lower()
    assert "fund any issue with one click" not in readme
    assert "pays out to the contributor when their pr is merged" not in readme
    assert "on-chain bounties" not in readme
    assert "mirror on your phone" not in readme
    assert "mirror on your phone" not in changelog
    assert "phones are access clients" in " ".join(readme.split())
    assert "access client; git mirroring remains a desktop/headless-node job" in changelog
    assert "legacy worker-custodied bounty deposits and automatic payouts are frozen" in readme


def _security_policy():
    return (PROJECT / "SECURITY.md").read_text(encoding="utf-8")


def test_security_policy_has_no_stray_markup_and_resolvable_links():
    policy = _security_policy()
    for stray in ("</content>", "</invoke>", "<invoke", "<content>"):
        assert stray not in policy

    links = re.findall(r"\]\((?!https?:|mailto:|#)([^)#]+)", policy)
    assert links, "expected relative links in SECURITY.md"
    for target in links:
        assert (PROJECT / target).exists(), target


def test_security_report_form_components_match_the_api():
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    block = re.search(
        r"SECURITY_REPORT_COMPONENTS = frozenset\(\{(.*?)\}\)", entry, re.S
    )
    assert block
    accepted = set(re.findall(r'"([a-z]+)"', block.group(1)))

    form = (PUBLIC / "security-report.html").read_text(encoding="utf-8")
    offered = set(re.findall(r'<option value="([a-z]+)"', form))
    assert offered == accepted

    desktop = (
        PROJECT / "desktop" / "src" / "MainWindowSettings.cpp"
    ).read_text(encoding="utf-8")
    offered = set(re.findall(
        r'm_vulnComponentCombo->addItem\([^,]+,\s*QStringLiteral\("([a-z]+)"\)',
        desktop,
    ))
    assert offered == accepted


def test_security_policy_names_the_private_report_form():
    policy = " ".join(_security_policy().split())
    assert "security@forkmesh.com" in policy
    assert "https://forkmesh.com/security-report" in policy
    assert "**Desktop → Settings → Security**" in policy
    assert "choose **Other** and name it in the description" in policy
    assert "security-report" in (ROOT / "src" / "static_routes.py").read_text(
        encoding="utf-8")


def test_public_security_report_page_links_a_reachable_policy():
    form = (PUBLIC / "security-report.html").read_text(encoding="utf-8")
    assert 'href="/SECURITY.md"' not in form
    assert (
        "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/SECURITY.md"
        in form
    )
    assert not (PUBLIC / "SECURITY.md").exists()
    redirects = (PUBLIC / "_redirects").read_text(encoding="utf-8")
    assert "SECURITY.md" not in redirects
