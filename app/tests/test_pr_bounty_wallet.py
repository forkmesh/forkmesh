#!/usr/bin/env python3
"""Per-PR reward compatibility without Worker-held wallets."""

from pathlib import Path


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
ENTRY_TEXT = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + "\n" + (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
)
QT_SRC = Path(__file__).resolve().parents[2] / "desktop" / "src"
QT_TEXT = "\n".join(
    path.read_text(encoding="utf-8")
    for path in sorted(QT_SRC.glob("MainWindow*.cpp"))
)


def test_bounty_key_still_separates_issues_from_pull_requests():
    assert "async def _bounty_bi(env, owner, repo, number, kind=" in ENTRY_TEXT
    start = ENTRY_TEXT.index("async def _bounty_bi")
    body = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "async def _load_bounty", start)]
    assert '(kind + "-") if kind else ""' in body


def test_inbuilt_wallet_and_worker_escrow_creation_are_frozen():
    start = ENTRY_TEXT.index("async def bounties_handler")
    handler = ENTRY_TEXT[
        start:ENTRY_TEXT.index("\n\n# Cap on collaborators", start)
    ]
    assert 'if action in ("wallet", "create", "payout"):' in handler
    assert '"legacy_custody_disabled"' in handler
    assert '"external-self-custodial"' in handler
    assert "reviewed Solana program or multisig" in handler


def test_desktop_merge_hook_is_a_fail_closed_compatibility_noop():
    # Existing call sites may still invoke the merge hook, but its active body
    # only clears stale preferences and returns without making a custody request.
    start = QT_TEXT.index("void MainWindow::autoBountyForMergedPull")
    body = QT_TEXT[start:QT_TEXT.index(
        "void MainWindow::pollBountyPayout", start)]
    active = body[:body.index("return;") + len("return;")]
    assert "Q_UNUSED(pr);" in active
    assert "setValue(kAutoPrBountyEnabledSetting, false)" in active
    assert "m_networkAccess->post" not in active
    assert "forkmesh-bounty-create-v1" not in active
    assert "privateKeyStoredByWorker" in ENTRY_TEXT
    assert '"external-local-signer"' in ENTRY_TEXT


def test_reward_settings_never_launch_account_or_donation_setup():
    chat = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    start = chat.index("void MainWindow::enablePaidMirroring")
    body = chat[start:chat.index("\nvoid MainWindow::showSection", start)]
    assert "ensureNodeAccount" not in body
    assert "postAccount" not in body
    assert "donation-address" not in body
    assert "donation-status" not in body
    assert "Reward eligibility configured" in body
    assert "Selection and payment are not guaranteed" in body


def test_qt_wallet_check_has_no_deposit_or_account_join_gate():
    setup = (QT_SRC / "MainWindowSetup.cpp").read_text(encoding="utf-8")
    start = setup.index("void MainWindow::verifyWallet")
    body = setup[start:setup.index("\nvoid MainWindow::persistProfile", start)]
    assert "isValidSolanaPublicAddress" in body
    assert "hasOwnerSigningCapability" in body
    assert "sendNodeHeartbeat" in body
    assert "ensureNodeAccount" not in body
    assert "donation-address" not in body
    assert "donation-status" not in body
    assert "request a deposit" in body
    assert "Selection and payment are not guaranteed" in body


def test_active_qt_reward_copy_has_no_wallet_deposit_requirement():
    chat = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    assert "Verify wallet (deposit >= 0.001 SOL)" not in chat
    assert "Revenue-sharing eligibility" not in chat
    assert "Check reward settings" in chat
    assert "never requests a deposit or guarantees a reward" in chat
    assert "collecting rewards" not in chat
    assert "Earnings:" not in chat
    assert "Public wallet balance:" in chat
    assert "may be eligible" in chat
    assert "externalWalletBalanceTooltip" in chat
    assert (
        "Non-custodial: this is a public external-wallet balance."
        in chat
    )
    assert "Public wallet balance increased" in chat
    assert "external self-custodial " in chat
    assert "wallet. New public balance:" in chat


def test_qt_legacy_bounty_qr_fails_closed_before_any_historical_code():
    issues = (QT_SRC / "MainWindowIssues.cpp").read_text(encoding="utf-8")
    start = issues.index("void MainWindow::showBountyQrDialog")
    body = issues[start:issues.index(
        "\nvoid MainWindow::showBountyWalletDialog", start)]
    active = body[:body.index("return;") + len("return;")]
    assert "Legacy bounty funding disabled" in active
    assert "migration-only" in active
    assert "external self-custodial wallet" in active
    assert "m_networkAccess" not in active
    assert "QGuiApplication::clipboard" not in active
    assert "Fund the bounty" not in body
    assert "address to fund the bounty" not in body
