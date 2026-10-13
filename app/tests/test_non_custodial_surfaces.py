"""Static contracts for ForkMesh's non-custodial user-facing boundary."""

from pathlib import Path


WORKER_ROOT = Path(__file__).resolve().parents[1]
PUBLIC = WORKER_ROOT / "public"
REPOSITORY = WORKER_ROOT.parent
PUBLIC_ROOTS = (
    REPOSITORY / "app" / "public",
    REPOSITORY / "www" / "public",
    REPOSITORY / "world" / "public",
)


def _lower(relative_path):
    if relative_path.startswith("world/"):
        root = REPOSITORY / "world" / "public"
    elif relative_path in {"index.html", "features.html", "terms.html", "privacy.html"} \
            or relative_path.startswith("docs/"):
        root = REPOSITORY / "www" / "public"
    else:
        root = PUBLIC
    text = (root / relative_path).read_text(encoding="utf-8").lower()
    return " ".join(text.split())


def test_interactive_crypto_surfaces_state_custody_and_fund_boundaries():
    required = {
        "index.html": (
            "non-custodial",
            "user-owned funds",
            "community-funded",
            "pending / finalized",
            "never stores community members’ wallet private keys",
        ),
        "features.html": (
            "non-custodial",
            "does not hold or control user funds",
            "for community members it stores only an optional public payout address",
            "community-pool signer stays encrypted",
            "user-owned funds",
            "community-funded public pool",
            "pending allocations",
            "completed on-chain transfers",
        ),
        "mirror-payouts.html": (
            "non-custodial payout wallet",
            "user-owned funds",
            "community-pool funds",
            "pending rewards",
            "completed on-chain transfers",
            "never the pool private key",
        ),
        "network.html": (
            "non-custodial",
            "public payout addresses only",
            "community-funded pool",
            "pending allocations",
            "completed on-chain transfers",
        ),
        "world/world.js": (
            "forkmesh is non-custodial",
            "does not hold or control your funds",
            "user-owned funds",
            "community-pool funds",
            "pending rewards",
            "completed transfers",
        ),
        "dashboard/partials/views/settings.html": (
            "non-custodial",
            "wallet keys and recovery phrases remain on your device",
            "user-owned funds",
            "community-pool funds",
            "pending rewards",
            "completed on-chain transfers",
        ),
        "dashboard/partials/modals.html": (
            "non-custodial",
            "keys and recovery phrases stay on your device",
            "user funds",
            "community pool",
            "pending rewards",
            "completed transfers",
        ),
    }
    for relative_path, phrases in required.items():
        text = _lower(relative_path)
        for phrase in phrases:
            assert phrase in text, (relative_path, phrase)


def test_policy_and_client_docs_explain_the_same_non_custodial_model():
    required = {
        "terms.html": (
            "does not hold, custody, or control user funds",
            "wallet keys and transaction approval stay on the user’s device",
            "community-funded public pool",
            "temporary pending allocations",
            "completed on-chain transfers",
        ),
        "privacy.html": (
            "does not store community members’ payout- or contribution-wallet private keys",
            "payout- and contribution-wallet private keys, seed phrases, mnemonics, and transaction approval remain on your device",
            "community-pool signer encrypted",
            "public community-pool address",
            "pending reward allocations are ledger reservations",
        ),
        "docs/qt-client/index.html": (
            "forkmesh does not hold or control user funds",
            "never stores a community member’s payout-wallet private key or recovery phrase",
            "existing community-pool key in an encrypted signer on that operator’s own device",
            "community-funded reward pool",
            "pending allocations",
            "completed on-chain transfers",
        ),
    }
    for relative_path, phrases in required.items():
        text = _lower(relative_path)
        for phrase in phrases:
            assert phrase in text, (relative_path, phrase)


def test_public_assets_do_not_restore_payment_promises_or_custodial_calls_to_action():
    forbidden = (
        "get paid to mirror",
        "getting paid",
        "you get paid",
        "forkmesh learned to pay the people",
        'let anyone "make it rain"',
        "live earnings calculator",
        "sol / join",
        "preserve code for just $1",
        "used for mirror rewards and bounty payouts",
        "issue-bounty escrow (solana deposit / confirm / split)",
        "one-click solana bounty",
        "per-node deposit key",
        "forkmesh never stores wallet private keys",
        "never stores a wallet private key or recovery phrase",
        "forkmesh never asks for a wallet private key",
        "stores no user wallet private keys",
        "never stores users’ wallet private keys",
        "forkmesh never asks for a private key or recovery phrase",
    )
    for root in PUBLIC_ROOTS:
        for path in sorted(root.rglob("*.html")) + sorted(root.rglob("*.js")):
            text = path.read_text(encoding="utf-8").lower()
            for phrase in forbidden:
                assert phrase not in text, (path.relative_to(root), phrase)


def test_protocol_docs_keep_repository_payloads_off_game_sockets():
    protocol = _lower("docs/protocol/index.html")
    for phrase in (
        "they never carry repository clones, blobs, releases, or other large payloads",
        "proxied over ordinary https",
        "401 push_unauthorized",
        "there is no websocket repository-data fallback",
        "private repositories are excluded from named public routing",
    ):
        assert phrase in protocol
    for stale in (
        "forwards the live clone/browse tunnel",
        "forwarded over that tunnel",
        "release-asset download over the host tunnel",
        "clones through the two-request tunnel",
    ):
        assert stale not in protocol


def test_public_copy_does_not_describe_repository_websocket_tunnels_as_active():
    stale = (
        "host tunnel",
        "live tunnel",
        "repo serving are already websocket based",
        "requests to the host tunnel instead of storing packfiles",
        "clones through the two-request tunnel",
    )
    for root in PUBLIC_ROOTS:
        for path in sorted(root.rglob("*.html")) + sorted(root.rglob("*.js")):
            text = " ".join(path.read_text(encoding="utf-8").lower().split())
            for phrase in stale:
                assert phrase not in text, (path.relative_to(root), phrase)
