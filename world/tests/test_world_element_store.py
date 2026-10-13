#!/usr/bin/env python3
"""Contracts for the purchasable World element store.

The issue asks for elements that can be bought in a store so each visitor can
customise their world: elements live in their own folder (later a separate
repository) as modular plugins with everything they need, visitors set them
with parameters and can link them to network requests, and a purchase is paid
in SOL with 50% to the treasury and 50% to online functioning mirror nodes.
"""

import re
import sys
from pathlib import Path
from world_scene_source import world_scene_source

ROOT = Path(__file__).resolve().parents[2] / "app"
sys.path.insert(0, str(ROOT / "src"))

import world_element_store as store  # noqa: E402

WORLD = ROOT.parent / "world" / "public" / "world"
ELEMENTS = WORLD / "elements"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")


def test_elements_live_in_their_own_folder_as_self_contained_plugins():
    assert (ELEMENTS / "README.md").is_file()
    assert (ELEMENTS / "index.js").is_file()
    for item in store.CATALOG:
        module = ELEMENTS / Path(item["module"]).name
        source = module.read_text(encoding="utf-8")
        assert "export const manifest" in source
        assert "export function build(" in source
        # A plugin folder that becomes its own repository cannot reach back
        # into the world bundle.
        assert not re.search(r'from\s+"\.\./', source), module.name


def test_catalog_and_browser_manifests_stay_in_lockstep():
    index = (ELEMENTS / "index.js").read_text(encoding="utf-8")
    for item in store.CATALOG:
        assert f'"{item["id"]}": () => import(' in index
        source = (ELEMENTS / Path(item["module"]).name).read_text(
            encoding="utf-8")
        assert f'id: "{item["id"]}"' in source
        for spec in item["params"]:
            assert f'key: "{spec["key"]}"' in source, spec["key"]
        for path in item["network"]:
            assert f'"{path}"' in source


def test_prices_are_server_side_only():
    # The browser copy must never be able to set what a purchase costs.
    for name in ("index.js", *[Path(i["module"]).name for i in store.CATALOG]):
        source = (ELEMENTS / name).read_text(encoding="utf-8")
        assert "priceLamports" not in source, name
    assert all(item["priceLamports"] > 0 for item in store.CATALOG)


def test_parameters_are_clamped_to_the_catalog_definition():
    beacon = store.element("aurora-beacon")
    cleaned = store.clean_params(beacon, {
        "height": 9999,
        "palette": "not-a-palette",
        "ribbons": -4,
        "drift": "yes",
    })
    assert cleaned["height"] == 14
    assert cleaned["palette"] == "aurora"
    assert cleaned["ribbons"] == 1
    assert cleaned["drift"] is True
    # Missing and non-numeric values fall back to the declared defaults.
    assert store.clean_params(beacon, {})["height"] == 8
    assert store.clean_params(beacon, {"height": "tall"})["height"] == 8
    assert store.clean_params(beacon, None)["palette"] == "aurora"


def test_text_parameters_are_bounded():
    obelisk = store.element("status-obelisk")
    cleaned = store.clean_params(obelisk, {"caption": "x" * 500})
    assert len(cleaned["caption"]) == 40


def test_placement_is_clamped_to_the_plaza():
    placement = store.clean_placement({"x": 9e9, "z": -9e9, "heading": 99})
    assert placement["x"] == store.MAX_PLACEMENT_RADIUS
    assert placement["z"] == -store.MAX_PLACEMENT_RADIUS
    assert abs(placement["heading"]) <= 3.15
    assert store.clean_placement({"x": float("nan")})["x"] == 0


def test_purchase_splits_half_to_treasury_and_half_to_mirror_nodes():
    assert store.TREASURY_SPLIT_PERCENT == 50
    assert store.MIRROR_SPLIT_PERCENT == 50
    split = store.split_lamports(50_000_000)
    assert split["treasuryLamports"] == 25_000_000
    assert split["mirrorLamports"] == 25_000_000
    # An odd amount keeps its remainder with the treasury, so the two halves
    # can never exceed what the pool actually received.
    odd = store.split_lamports(7)
    assert odd["treasuryLamports"] + odd["mirrorLamports"] == 7
    assert odd["treasuryLamports"] == 4


def test_owned_map_drops_unknown_elements_and_normalises_entries():
    owned = store.clean_owned({
        "aurora-beacon": {"params": {"height": 99}, "enabled": False},
        "not-an-element": {"params": {}},
    })
    assert set(owned) == {"aurora-beacon"}
    assert owned["aurora-beacon"]["params"]["height"] == 14
    assert owned["aurora-beacon"]["enabled"] is False
    assert store.clean_owned(None) == {}


def test_stored_parameters_can_never_introduce_a_wallet_key_name():
    # Owned elements are written to the account record, which the legacy
    # custody guard scans by key name at startup. Parameters are emitted from
    # the catalog definition, never copied from the request, so a crafted
    # payload cannot plant a flagged key and fail the Worker closed.
    beacon = store.element("aurora-beacon")
    cleaned = store.clean_params(beacon, {
        "walletSecret": "hunter2",
        "mnemonic": "abandon abandon",
        "height": 6,
    })
    assert set(cleaned) == {spec["key"] for spec in beacon["params"]}
    owned = store.clean_owned({
        "aurora-beacon": {
            "params": {"donationSecret": "x"},
            "placement": {"x": 1, "secret": "x"},
        },
    })
    entry = owned["aurora-beacon"]
    assert set(entry["params"]) == {spec["key"] for spec in beacon["params"]}
    assert set(entry["placement"]) == {"x", "z", "heading"}
    assert "secret" not in repr(owned).lower()


def test_split_policy_makes_no_investment_claim():
    policy = store.split_policy_public()
    assert policy["forkMeshHoldsUserKeys"] is False
    assert policy["userWalletSignatureRequired"] is True
    assert policy["ownershipInterestGranted"] is False
    assert policy["financialReturnPromised"] is False
    assert "does not purchase ownership" in policy["notice"]


def test_direct_path_stays_non_custodial():
    # The direct method must keep putting no key on the Worker: it only mints
    # a Solana Pay reference the buyer's own wallet tags.
    handler = ENTRY.split("async def world_element_store_handler")[1]
    handler = handler.split("\ndef _chain_intent_public")[0]
    assert "reference = _base58_encode(_random_bytes(32))" in handler
    # Confirmation reuses the audited finalized-transfer verifier.
    assert "_solana_contribution_details(" in handler
    # A keypair is minted only on the deposit branch.
    assert 'if method == "deposit":' in handler
    assert "deposit_address, seed = await _new_solana_keypair()" in handler


def test_deposit_path_is_gated_and_sweeps_fifty_fifty():
    gate = ENTRY.split("def _custodial_deposits_enabled(env):")[1]
    gate = gate.split("async def _online_mirror_payout_addresses")[0]
    # Off unless a treasury exists to sweep into, and killable outright.
    assert 'flag in ("0", "false", "off", "no")' in gate
    assert "return bool(_deposit_treasury_address(env))" in gate
    assert "DEPOSIT_TREASURY_SPLIT_NUMERATOR = 1" in ENTRY
    assert "DEPOSIT_TREASURY_SPLIT_DENOMINATOR = 2" in ENTRY
    # The sweep pays online eligible mirrors, not arbitrary presence rows.
    payees = ENTRY.split("async def _online_mirror_payout_addresses")[1]
    payees = payees.split("def _deposit_sweep_plan")[0]
    assert "_eligible_reward_snapshot(env, int(Date.now()))" in payees
    assert "MAX_SWEEP_PAYEES" in payees


def test_deposit_key_is_encrypted_and_destroyed_on_sweep():
    handler = ENTRY.split("async def world_element_store_handler")[1]
    assert 'deposit_secret = await encrypt_row(env, {"seed": seed})' in (
        ENTRY.split("async def world_element_store_handler")[1])
    sweep = ENTRY.split("async def _sweep_element_deposit")[1]
    sweep = sweep.split("async def _record_sweep_error")[0]
    # Idempotent, and the key is cleared the moment the sweep lands.
    assert 'if row.get("sweep_signature"):' in sweep
    assert "deposit_secret=''" in sweep
    due_phases = ENTRY[
        ENTRY.index("def _cron_due_phases"):
        ENTRY.index("async def _run_cron_phase")
    ]
    phase_runner = ENTRY[
        ENTRY.index("async def _run_cron_phase"):
        ENTRY.index("def _cron_tick_phase_order")
    ]
    assert '"reward-sweeps"' in due_phases
    assert "elif minute % 5 == 1:" in due_phases
    assert 'if phase == "reward-sweeps":' in phase_runner
    assert "return await sweep_pending_element_deposits(env)" in phase_runner
    del handler


def test_deposit_purchase_does_not_also_mint_a_split_intent():
    # The sweep already pays mirrors on chain; an intent would double-pay.
    grant = ENTRY.split("async def _grant_purchased_element")[1]
    grant = grant.split("async def _element_deposit_status")[0]
    assert 'if str(row.get("method") or "direct") != "deposit":' in grant


def test_purchase_table_records_the_custodial_deposit_explicitly():
    assert "CREATE TABLE IF NOT EXISTS world_element_purchases" in SCHEMA
    table = SCHEMA.split("world_element_purchases (")[1].split('"""')[0]
    for column in (
        "reference_address", "tx_signature", "treasury_lamports",
        "mirror_lamports", "method", "deposit_address", "deposit_secret",
        "sweep_signature",
    ):
        assert column in table
    assert (ROOT / "migrations" / "0119_world_element_store.sql").is_file()


def test_mirror_half_is_only_ever_an_unsigned_intent():
    intent = ENTRY.split("async def _create_element_store_split_intent")[1]
    intent = intent.split("async def _grant_purchased_element")[0]
    assert '"world_element_store_mirror_split"' in intent
    assert "'pending_signature'" in intent
    assert '"method": "external-local-qt"' in intent
    # Only nodes the eligibility snapshot accepts are paid.
    assert "_eligible_reward_snapshot(env, now)" in intent
    assert "if not eligible:" in intent


def test_store_routes_are_dispatched():
    assert '"/api/world/store"' in ENTRY
    assert "world_element_store_handler(" in ENTRY
    for action in ('action == "prepare"', 'action != "confirm"',
                   'action == "configure"'):
        assert action in ENTRY, action


def test_configure_requires_ownership():
    handler = ENTRY.split("async def world_element_store_handler")[1]
    assert '"element_not_owned"' in handler
    assert '"element_already_owned"' in handler


def test_scene_builds_purchased_elements_through_the_element_registry():
    for contract in (
        "async function installStoreElement(spec)",
        "function removeStoreElement(id)",
        "function tickStoreElements(nowMs)",
        "const module = await loadStoreElementModule(id);",
        "registerWorldElement(",
        "    installStoreElement,",
        "    removeStoreElement,",
    ):
        assert contract in SCENE, contract
    # Purchased geometry ticks each frame like the rest of the world.
    assert "tickStoreElements(time);" in SCENE


def test_elements_may_only_read_endpoints_their_manifest_declared():
    fetcher = SCENE.split("function storeElementFetcher(manifest)")[1]
    fetcher = fetcher.split("async function installStoreElement")[0]
    assert "const allowed = new Set(manifest.network || []);" in fetcher
    assert "if (!allowed.has(target))" in fetcher
    assert 'credentials: "same-origin"' in fetcher


def test_world_shell_installs_owned_elements_and_offers_the_store():
    for contract in (
        "async refreshStoreLibrary()",
        'async purchaseStoreElement(elementId, method = "direct")',
        "async confirmStoreElementPurchase(transactionSignature)",
        "async pollElementDeposit()",
        "startElementDepositPolling()",
        "async configureStoreElement(elementId",
        "renderWorldStorePane(",
        'data-world-settings-tab="store"',
        'data-world-settings-pane="store"',
        "void Promise.allSettled([",
        "this.refreshStoreLibrary(),",
        "this.refreshStoreCatalog(),",
    ):
        assert contract in APP, contract
    # Store APIs and owned element modules are requested only when the visitor
    # opens the Store tab; they are absent from the scene's startup path.
    assert '"store",\n      "elements",' in APP


def test_store_pane_discloses_each_method_custody_honestly():
    pane = APP.split('data-world-settings-pane="store"')[1]
    pane = pane.split('data-world-settings-pane="elements"')[0]
    assert "half is shared" in pane
    assert "purchase ownership" in pane
    # The two methods have different custody and must not share one blurb.
    assert "never\n                receives your key" in pane
    assert "holds that address's key" in pane
    assert "could move them" in pane


def test_custody_notice_never_claims_non_custody_for_a_deposit():
    deposit = store.custody_notice("deposit")
    direct = store.custody_notice("direct")
    assert deposit["forkMeshHoldsDepositKey"] is True
    assert "never" not in deposit["notice"].lower().split("does not")[0]
    assert "ForkMesh can move these funds" in deposit["notice"]
    assert direct["forkMeshHoldsDepositKey"] is False
    assert "never receives your wallet key" in direct["notice"]
    for notice in (deposit, direct):
        assert notice["ownershipInterestGranted"] is False
        assert notice["financialReturnPromised"] is False
    both = store.payment_methods_public(direct=True, deposit=True)
    assert [entry["method"] for entry in both] == ["direct", "deposit"]
    assert store.payment_methods_public(direct=True, deposit=False) == [direct]
