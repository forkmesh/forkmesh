"""Public free-protocol / paid-services entitlement contract."""

from html import unescape
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "www" / "public"
DOCS = ROOT / "www" / "docs"
CONTRACT_PATH = PUBLIC / "business-model.json"
RESEARCH_PATH = DOCS / "research" / "pricing-interviews.json"


def _json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def _html_text(path):
    source = path.read_text(encoding="utf-8")
    without_blocks = re.sub(
        r"<(?:style|script)\b.*?</(?:style|script)>", " ", source,
        flags=re.IGNORECASE | re.DOTALL,
    )
    return " ".join(unescape(re.sub(r"<[^>]+>", " ", without_blocks)).split())


def _normalized(path):
    return " ".join(path.read_text(encoding="utf-8").split())


def test_machine_contract_keeps_protocol_and_portability_rights_free():
    contract = _json(CONTRACT_PATH)
    commitments = contract["permanentCommitments"]

    assert contract["schema"] == "forkmesh-business-model-v1"
    assert contract["schemaVersion"] == 1
    assert contract["publishedAt"] == "2026-08-20"
    assert commitments["protocolLicense"] == "MIT"
    assert commitments["independentImplementationsAllowed"] is True
    assert commitments["noDataSale"] is True
    for field in (
        "protocolUseRequiresPayment",
        "selfHostingRequiresPayment",
        "publicReadAndCloneRequirePaidPlan",
        "portableGitDataRequiresPaidPlan",
        "portableSignedCollaborationRequiresPaidPlan",
        "governanceRequiresPaidPlan",
    ):
        assert commitments[field] is False

    portability = contract["portability"]
    assert portability["canonicalRepositoryFormat"] == "Git objects and refs"
    assert portability["canonicalCollaborationFormat"] == (
        "Signed .forkmesh repository records")
    assert portability["exportFeeMicrousd"] == 0
    assert any("after cancellation" in item for item in portability["commitments"])
    assert any("clean self-hosted" in item for item in portability["commitments"])


def test_initial_plans_publish_price_availability_limits_and_support():
    plans = {plan["id"]: plan for plan in _json(CONTRACT_PATH)["plans"]}
    assert set(plans) == {
        "protocol-self-hosted",
        "hosted-free",
        "supporter-membership",
        "pro-membership",
        "managed-services",
    }
    assert plans["protocol-self-hosted"]["price"] == {
        "amountMicrousd": 0, "interval": "forever"}
    assert plans["hosted-free"]["price"]["amountMicrousd"] == 0
    assert plans["supporter-membership"]["price"] == {
        "amountMicrousd": 5_000_000, "interval": "month"}
    assert plans["pro-membership"]["price"] == {
        "amountMicrousd": 10_000_000, "interval": "month"}
    managed = plans["managed-services"]
    assert managed["availability"] == "research-preview-not-for-sale"
    assert managed["price"]["amountMicrousd"] is None
    assert "No unlimited plan" in " ".join(managed["limits"])
    for plan in plans.values():
        assert plan["limits"]
        assert plan["support"].strip()


def test_public_page_has_the_full_entitlement_matrix_and_all_initial_plans():
    page = PUBLIC / "docs" / "business-model" / "index.html"
    source = page.read_text(encoding="utf-8")
    text = _html_text(page)

    for heading in (
        "Capability",
        "Protocol right",
        "Self-hosted capability",
        "Hosted Free limit",
        "Paid service",
    ):
        assert f"<th>{heading}</th>" in source
    for plan in (
        "Protocol + self-hosted",
        "Hosted Free",
        "Supporter membership",
        "Pro membership",
        "Managed services",
    ):
        assert plan in text
    for boundary in (
        "$0 forever",
        "$5 / member / month",
        "$10 / member / month",
        "Research preview · not for sale",
        "Price and allowances not yet validated",
        "Clone/export without upgrading",
        "$0 export fee",
        "no response-time or uptime promise",
    ):
        assert boundary in text
    assert source.count("<tbody>") == 1
    assert source.count("<tr><td>") == len(_json(CONTRACT_PATH)["entitlements"])


def test_homepage_pricing_exposes_the_boundary_without_managed_checkout():
    source = (PUBLIC / "index.html").read_text(encoding="utf-8")
    pricing = source[source.index('<section id="pricing"'):
                     source.index('<section id="support-forkmesh"')]
    text = " ".join(unescape(re.sub(r"<[^>]+>", " ", pricing)).split())

    for statement in (
        "Free protocol. Paid service.",
        "Use, fork, and self-host ForkMesh forever.",
        "Protocol + Hosted Free",
        "MIT + best-effort hosting",
        "Research / not for sale",
        "Join a pricing interview",
        "No membership changes protocol rights",
        "Managed checkout remains disabled",
    ):
        assert statement in text
    assert 'data-monthly="5"' in pricing
    assert 'data-monthly="10"' in pricing
    # No checkout of any kind is hosted on the homepage any more.
    assert "data-checkout" not in source
    assert "Contact sales" not in text


def test_every_public_pricing_preview_removes_unvalidated_enterprise_sales():
    for relative in (
        "index.html",
    ):
        text = _html_text(PUBLIC / relative)
        assert "Enterprise" not in text
        assert "Contact sales" not in text
        assert "Custom rollout" not in text
        assert "Managed services" in text


def test_usage_units_are_unique_visible_and_have_cost_sources():
    contract = _json(CONTRACT_PATH)
    units = contract["usageUnits"]
    unit_ids = [unit["id"] for unit in units]
    page_text = _html_text(PUBLIC / "docs" / "business-model" / "index.html")

    assert len(unit_ids) == len(set(unit_ids)) == 9
    assert {
        "active-member-month",
        "indexed-project-month",
        "managed-replica-gib-month",
        "verified-egress-gib",
        "hosted-runner-minute",
        "priority-support-hour",
        "worker-request",
        "durable-object-request",
        "durable-object-gb-second",
    } == set(unit_ids)
    for unit in units:
        assert unit["definition"].strip()
        assert unit["costSource"].strip()
        assert unit["label"].lower() in page_text.lower()


def test_pricing_research_is_honest_versioned_and_blocks_managed_sales():
    research = _json(RESEARCH_PATH)
    register = (DOCS / "research" / "pricing-interviews.md").read_text(
        encoding="utf-8")
    public_text = _html_text(
        PUBLIC / "docs" / "business-model" / "index.html")

    assert research["status"] == "not-validated"
    assert research["qualifyingInterviews"] == 0
    assert research["qualifyingInterviewIds"] == []
    assert research["organizationsRepresented"] == 0
    assert research["organizationSizeBandsRepresented"] == 0
    assert research["syntheticRecordsCount"] == 0
    assert research["buyerValidated"] is False
    assert research["budgetValidated"] is False
    assert research["topPaidJobsValidated"] is False
    assert research["latestRevision"] == 1
    assert research["validatedRevision"] is None
    assert "zero qualifying buyer interviews" in public_text
    assert "These are hypotheses, not findings" in register
    assert "Candidate budget | Must be elicited" in register
    assert "## Interview record template" in register
    assert "## Revision log" in register
    assert "| 1 | 2026-08-20 |" in register


def test_no_data_sale_and_support_boundaries_are_user_facing():
    statement = (
        "ForkMesh does not sell personal data, repository data, collaboration "
        "data, telemetry, or derived profiles")
    for path in (
        PUBLIC / "business-model.json",
        PUBLIC / "docs" / "business-model" / "index.html",
        PUBLIC / "privacy.html",
        DOCS / "business-model.md",
    ):
        assert statement in _normalized(path)

    privacy = _html_text(PUBLIC / "privacy.html")
    terms = _html_text(PUBLIC / "terms.html")
    assert "No sale of data" in privacy
    assert "Payment never buys access to another user’s data" in privacy
    assert "Free protocol and hosted services" in terms
    assert "Public read and clone" in terms
    assert "cannot be made paid-only" in terms
    assert "without a response-time, reserved-capacity, recovery, or uptime commitment" in terms


def test_acquisition_margin_failure_and_recovery_models_are_documented():
    model = _normalized(DOCS / "business-model.md")
    runbook = _normalized(DOCS / "operations" / "business-model-rollout.md")
    schema = _json(DOCS / "unit-economics-period.schema.json")

    for marker in (
        "## Acquisition and paid adoption model",
        "public projects reaching proof",
        "qualified service conversations",
        "trial-to-paid conversion",
        "gross margin",
        "Provider analytics unavailability is a failed sample, not a zero-cost day",
    ):
        assert marker in model
    for marker in (
        "## Supported surfaces",
        "## Failure paths",
        "## Rollback",
        "## Recovery and portability drill",
        "## Telemetry and privacy",
        "analytics_unconfigured",
        "analytics_unavailable",
        "clean self-hosted deployment",
    ):
        assert marker in runbook
    required = set(schema["required"])
    assert {"funnel", "recognizedRevenueMicrousd", "measurements", "evidence"} <= required


def test_p0_gate_links_automated_platform_failure_docs_and_telemetry_evidence():
    config = _json(DOCS / "engineering" / "quality-gates.json")
    gates = {gate["id"]: gate for gate in config["gates"]}
    gate = gates["business-model-contract"]
    command = gate["command"]

    assert gate["category"] == "operations"
    assert "business-model-contract" in config["profiles"]["core"]
    assert "business-model-contract" in config["profiles"]["release"]
    for test_path in (
        "app/tests/test_business_model_contract.py",
        "app/tests/test_unit_economics.py",
        "app/tests/test_admin_usage_report.py",
        "app/tests/test_first_party_analytics.py",
        "app/tests/test_installer_release.py",
        "app/tests/test_mirror_serving.py",
        "app/tests/test_https_mirror_routing_integration.py",
        "app/tests/test_mirror_service_recovery.py",
    ):
        assert (ROOT / test_path).is_file()
    for gate_test in (
        "app/tests/test_business_model_contract.py",
        "app/tests/test_unit_economics.py",
        "app/tests/test_admin_usage_report.py",
        "app/tests/test_first_party_analytics.py",
        "app/tests/test_installer_release.py",
        "app/tests/test_mirror_serving.py",
        "app/tests/test_https_mirror_routing_integration.py",
        "app/tests/test_mirror_service_recovery.py",
    ):
        assert gate_test in command

    controller = (PUBLIC / "analytics.js").read_text(encoding="utf-8")
    assert '"pricing-contract-opened"' in controller
    assert '"pricing-managed-interest"' in controller
    assert 'href="/docs/business-model"' in (
        PUBLIC / "docs" / "index.html").read_text(encoding="utf-8")
    assert "free-protocol / paid-services contract" in (
        ROOT / "README.md").read_text(encoding="utf-8")
