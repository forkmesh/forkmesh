"""Purchasable World elements: catalog, parameters and the payment split.

Elements are plugins that live in ``public/world/elements`` (a staging area for
a future separate repository). Each one is self-contained geometry plus the
parameters a visitor may set and the endpoints it is allowed to read. This
module is the authority for what an element costs and which parameter values
are accepted; the browser copy may only ever narrow that.

Payment is non-custodial and follows the reward-contribution pattern exactly:
the visitor's own wallet signs one public transfer to the published community
pool address, tagged with a unique per-purchase reference. ForkMesh never
creates a keypair, never holds a key and never sweeps funds. The 50/50 split
described below is therefore expressed as an *unsigned intent* over the half
owed to mirror nodes, which the instance owner's local signer reviews; the
other half stays in the pool that received the transfer.
"""

# Half of every purchase is retained by the treasury (the published community
# pool address that received the transfer) and half is distributed to online,
# eligible mirror nodes.
TREASURY_SPLIT_PERCENT = 50
MIRROR_SPLIT_PERCENT = 100 - TREASURY_SPLIT_PERCENT

PURCHASE_EXPIRES_MS = 30 * 60 * 1000
MAX_OWNED_ELEMENTS = 64
MAX_TEXT_PARAM = 80
# Placements are cosmetic and clamped to the plaza so a purchase can never
# park geometry outside the walkable world.
MAX_PLACEMENT_RADIUS = 120.0

CATALOG = [
    {
        "id": "aurora-beacon",
        "label": "Aurora beacon",
        "category": "Store",
        "module": "./elements/aurora-beacon.js",
        "priceLamports": 20_000_000,
        "summary": (
            "A quiet column of light with drifting aurora ribbons above the "
            "plaza."
        ),
        "params": [
            {"key": "height", "type": "number", "default": 8,
             "min": 4, "max": 14, "step": 0.5, "label": "Height"},
            {"key": "palette", "type": "select", "default": "aurora",
             "options": ["aurora", "ember", "ice"], "label": "Palette"},
            {"key": "ribbons", "type": "number", "default": 3,
             "min": 1, "max": 6, "step": 1, "label": "Ribbons"},
            {"key": "drift", "type": "toggle", "default": True,
             "label": "Drifting motion"},
        ],
        "network": [],
    },
    {
        "id": "orbit-sculpture",
        "label": "Orbit sculpture",
        "category": "Store",
        "module": "./elements/orbit-sculpture.js",
        "priceLamports": 35_000_000,
        "summary": (
            "A slow kinetic sculpture of nested rings around a lit core."
        ),
        "params": [
            {"key": "rings", "type": "number", "default": 3,
             "min": 1, "max": 5, "step": 1, "label": "Rings"},
            {"key": "radius", "type": "number", "default": 2.2,
             "min": 1, "max": 5, "step": 0.1, "label": "Radius"},
            {"key": "tint", "type": "select", "default": "brass",
             "options": ["brass", "slate", "violet"], "label": "Tint"},
            {"key": "speed", "type": "number", "default": 1,
             "min": 0, "max": 3, "step": 0.1, "label": "Turn speed"},
        ],
        "network": [],
    },
    {
        "id": "status-obelisk",
        "label": "Status obelisk",
        "category": "Store",
        "module": "./elements/status-obelisk.js",
        "priceLamports": 50_000_000,
        "summary": (
            "A standing obelisk that shows the live deploy state of the "
            "instance."
        ),
        "params": [
            {"key": "caption", "type": "text", "default": "Deploy status",
             "maxLength": 40, "label": "Caption"},
            {"key": "refreshSeconds", "type": "number", "default": 60,
             "min": 15, "max": 600, "step": 5, "label": "Refresh (seconds)"},
            {"key": "accent", "type": "select", "default": "green",
             "options": ["green", "amber", "blue"], "label": "Accent"},
        ],
        # Every endpoint an element may read is declared here and disclosed in
        # the store before purchase. The browser loader refuses anything else.
        "network": ["/api/world/deploy-status"],
    },
    {
        "id": "status-board",
        "label": "System status board",
        "category": "Store",
        "module": "./elements/status-board.js",
        "priceLamports": 50_000_000,
        "summary": (
            "A standing board that mirrors forkmesh.com/status: every "
            "monitored system with its last hour, day and month of checks."
        ),
        "params": [
            {"key": "caption", "type": "text", "default": "System status",
             "maxLength": 40, "label": "Caption"},
            {"key": "refreshSeconds", "type": "number", "default": 180,
             "min": 60, "max": 900, "step": 30, "label": "Refresh (seconds)"},
            {"key": "frame", "type": "select", "default": "green",
             "options": ["green", "blue", "slate"], "label": "Frame"},
        ],
        "network": ["/api/status/world"],
    },
]

_BY_ID = {item["id"]: item for item in CATALOG}


def element(element_id):
    return _BY_ID.get(str(element_id or ""))


def catalog_public():
    """Store listing: price, parameters and disclosed network reads."""
    return [
        {
            "id": item["id"],
            "label": item["label"],
            "category": item["category"],
            "summary": item["summary"],
            "priceLamports": item["priceLamports"],
            "priceSol": float(f"{item['priceLamports'] / 1_000_000_000:.9f}"),
            "params": [dict(spec) for spec in item["params"]],
            "network": list(item["network"]),
        }
        for item in CATALOG
    ]


def clean_params(item, raw):
    """Clamp one visitor's parameters to the catalog definition."""
    source = raw if isinstance(raw, dict) else {}
    params = {}
    for spec in (item or {}).get("params", []):
        key = spec["key"]
        value = source.get(key)
        if spec["type"] == "number":
            try:
                number = float(value)
            except (TypeError, ValueError):
                number = float(spec["default"])
            if number != number or number in (float("inf"), float("-inf")):
                number = float(spec["default"])
            number = min(float(spec["max"]), max(float(spec["min"]), number))
            params[key] = int(number) if float(number).is_integer() else number
        elif spec["type"] == "select":
            params[key] = (
                value if value in spec["options"] else spec["default"]
            )
        elif spec["type"] == "toggle":
            params[key] = bool(value) if isinstance(value, bool) else bool(
                spec["default"])
        else:
            text = "" if value is None else str(value)
            params[key] = text[:min(
                int(spec.get("maxLength") or MAX_TEXT_PARAM), MAX_TEXT_PARAM)]
    return params


def clean_placement(raw):
    """Clamp a requested x/z/heading placement to the plaza."""
    source = raw if isinstance(raw, dict) else {}
    placement = {}
    for key in ("x", "z", "heading"):
        try:
            value = float(source.get(key) or 0)
        except (TypeError, ValueError):
            value = 0.0
        if value != value or value in (float("inf"), float("-inf")):
            value = 0.0
        limit = 3.15 if key == "heading" else MAX_PLACEMENT_RADIUS
        placement[key] = float(f"{min(limit, max(-limit, value)):.3f}")
    return placement


def split_lamports(amount_lamports):
    """Split one purchase into the treasury half and the mirror-node half.

    Integer division keeps the remainder with the treasury, which is where the
    funds already sit, so the two halves can never exceed what was received.
    """
    total = max(0, int(amount_lamports or 0))
    mirror = total * MIRROR_SPLIT_PERCENT // 100
    return {"treasuryLamports": total - mirror, "mirrorLamports": mirror}


def split_policy_public():
    return {
        "treasuryPercent": TREASURY_SPLIT_PERCENT,
        "mirrorNodePercent": MIRROR_SPLIT_PERCENT,
        "treasuryDestination": "published-community-pool-address",
        "mirrorDestination": "online-eligible-mirror-nodes",
        "custody": "direct-self-custodial-wallet-to-public-pool",
        "forkMeshHoldsUserKeys": False,
        "userWalletSignatureRequired": True,
        "ownershipInterestGranted": False,
        "financialReturnPromised": False,
        "notice": (
            "Buying an element unlocks cosmetic world content for your own "
            "account. ForkMesh never receives your wallet key; your wallet "
            "signs one direct public transfer. The mirror-node half is paid "
            "out only after the instance owner's local signer reviews and "
            "signs it. This does not purchase ownership, guaranteed rewards, "
            "investment returns, governance weight or merge influence."
        ),
    }


def custody_notice(method):
    """State plainly, per method, who holds the money and for how long.

    These two paths have genuinely different custody, so they must never share
    one blurb. Saying "ForkMesh never receives your key" about a deposit
    address the Worker generated would be false.
    """
    if method == "deposit":
        return {
            "method": "deposit",
            "label": "Pay a temporary ForkMesh deposit address",
            "forkMeshHoldsDepositKey": True,
            "custody": "custodial-temporary-worker-held-deposit-address",
            "keyLifetime": (
                "The signing key exists from the moment the address is "
                "created until the sweep confirms, then it is deleted."
            ),
            "ownershipInterestGranted": False,
            "financialReturnPromised": False,
            "notice": (
                "This address is generated and held by ForkMesh. Until the "
                "sweep completes, ForkMesh can move these funds and an "
                "operator compromise could too. Half is then sent to the "
                "treasury and half is shared between online mirror nodes. "
                "Buying an element does not purchase ownership, guaranteed "
                "rewards, investment returns or any influence over the "
                "project."
            ),
        }
    return {
        "method": "direct",
        "label": "Pay the published pool address from your own wallet",
        "forkMeshHoldsDepositKey": False,
        "custody": "direct-self-custodial-wallet-to-public-pool",
        "keyLifetime": "ForkMesh never receives your wallet key.",
        "ownershipInterestGranted": False,
        "financialReturnPromised": False,
        "notice": (
            "Your own wallet signs one direct public transfer; ForkMesh never "
            "receives your wallet key. Half stays with the treasury and half "
            "becomes a mirror-node payout the instance owner's local signer "
            "reviews. Buying an element does not purchase ownership, "
            "guaranteed rewards, investment returns or any influence over the "
            "project."
        ),
    }


def payment_methods_public(direct=True, deposit=False):
    methods = []
    if direct:
        methods.append(custody_notice("direct"))
    if deposit:
        methods.append(custody_notice("deposit"))
    return methods


def clean_owned(raw, now=0):
    """Normalise the per-account owned-element map stored on the record."""
    source = raw if isinstance(raw, dict) else {}
    owned = {}
    for element_id, entry in source.items():
        item = element(element_id)
        if not item or not isinstance(entry, dict):
            continue
        owned[item["id"]] = {
            "params": clean_params(item, entry.get("params")),
            "placement": clean_placement(entry.get("placement")),
            "enabled": entry.get("enabled") is not False,
            "purchaseId": str(entry.get("purchaseId") or "")[:32],
            "transactionSignature": str(
                entry.get("transactionSignature") or "")[:120],
            "purchasedAt": max(0, int(entry.get("purchasedAt") or 0)),
            "updatedAt": max(0, int(entry.get("updatedAt") or now or 0)),
        }
        if len(owned) >= MAX_OWNED_ELEMENTS:
            break
    return owned
