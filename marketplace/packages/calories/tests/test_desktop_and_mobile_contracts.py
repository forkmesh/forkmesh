"""Desktop and mobile contracts for Calorie Counter: proofs, registration, safe uninstall."""

import ast
import json
import re
from pathlib import Path


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
ENTRY = (REPO / "app" / "src" / "entry.py").read_text(encoding="utf-8")
WORKER = (PACKAGE / "content" / "worker" / "calories.py").read_text(encoding="utf-8")
INTERNAL = (REPO / "desktop" / "src" / "MainWindowInternal.h").read_text(encoding="utf-8")
HOST = (REPO / "desktop" / "src" / "MainWindowMarket.cpp").read_text(encoding="utf-8")
FEATURE = (PACKAGE / "content" / "desktop" / "CaloriesFeature.cpp").read_text(encoding="utf-8")
FEATURE_HEADER = (PACKAGE / "content" / "desktop" / "CaloriesFeature.h").read_text(encoding="utf-8")
BUILD = json.loads((PACKAGE / "content" / "desktop" / "build.json").read_text(encoding="utf-8"))
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
ICONS = (REPO / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
CATALOG = json.loads((REPO / "desktop" / "resources" / "network" / "endpoint-catalog.json")
                     .read_text(encoding="utf-8"))
ANDROID = (REPO / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh"
           / "nativeapp" / "calories" / "Calories.kt").read_text(encoding="utf-8")


def _constant(name, source):
    tree = ast.parse(source)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == name
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(f"missing {name}")


def _header_constant(name):
    match = re.search(name + r"\s*=\s*\n?\s*QStringLiteral\(\"([^\"]+)\"\)", INTERNAL)
    assert match, f"missing {name}"
    return match.group(1)


def test_desktop_proof_constants_match_the_worker_byte_for_byte():
    proof = _constant("REQUEST_PROOF", WORKER)
    assert f'QStringLiteral("{proof}")' in FEATURE
    assert _header_constant("kPluginRequestProof") == _constant("PLUGIN_REQUEST_PROOF", ENTRY)
    assert proof != _constant("PLUGIN_REQUEST_PROOF", ENTRY)


def test_package_registers_its_native_surface_from_inside_the_package():
    assert "NativePackageRegistration caloriesRegistration" in FEATURE
    assert 'QStringLiteral("builtin:calories")' in FEATURE
    assert 'QStringLiteral("/api/calories")' in FEATURE
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:calories"
    assert BUILD == {"schemaVersion": 1,
                     "sources": ["CaloriesFeature.h", "CaloriesFeature.cpp"],
                     "mocHeaders": []}
    # The host stays package-neutral: no calories identifier in core sources.
    assert "calories" not in HOST.lower()
    # The rail icon must be an Octicon the desktop ships, or the button is blank.
    icon = re.search(r'QStringLiteral\("/api/calories"\)', FEATURE)
    assert icon
    registration = FEATURE[FEATURE.index("caloriesRegistration"):]
    name = re.search(r'QStringLiteral\("Calories"\),\s*QStringLiteral\("([a-z-]+)"\)', registration)
    assert name and f"icons/{name.group(1)}.svg" in ICONS


def test_desktop_uninstall_downloads_the_export_before_deleting_anything():
    body = FEATURE.split("void CaloriesFeature::uninstallPlugin()", 1)[1]
    body = body.split("\nvoid CaloriesFeature::", 1)[0]
    confirm = body.index("QMessageBox::warning")
    export_call = body.index('apiPath(QStringLiteral("/export"))')
    uninstall_call = body.index('apiPath(QStringLiteral("/uninstall"))')
    assert confirm < export_call < uninstall_call
    guard = body.index("!saveExportBundle(payload)")
    assert export_call < guard < uninstall_call
    assert body.count("return;") >= 3


def test_desktop_and_mobile_install_per_account_without_organizations():
    for source in (FEATURE, FEATURE_HEADER, ANDROID):
        assert "organization" not in source.lower()
        assert "/api/plugins/" not in source
    assert 'apiPath(QStringLiteral("/install"))' in FEATURE
    assert '"/api/calories$suffix"' in ANDROID
    assert 'call("POST", "/install"' in ANDROID


def test_desktop_plugin_surface_keeps_no_local_trace():
    for source in (FEATURE, FEATURE_HEADER):
        assert "QSettings" not in source
    assert "setEchoMode(QLineEdit::Password)" in FEATURE
    # Photos are bounded and sent once; nothing is written to disk.
    assert 'QStringLiteral("data:image/jpeg;base64,")' in FEATURE
    assert "QSaveFile" in FEATURE and FEATURE.count("QSaveFile file(") == 1


def test_desktop_network_catalog_lists_every_calories_api_family():
    rows = {(row["pattern"], row.get("method")) for row in CATALOG["endpoints"]}
    assert ("app.forkmesh.com/api/calories", "GET") in rows
    for method in ("GET", "POST", "PATCH", "PUT", "DELETE"):
        assert ("app.forkmesh.com/api/calories/*", method) in rows
    # Food databases are reached by the Worker, never by the desktop directly.
    assert not any("openfoodfacts" in pattern or "nal.usda.gov" in pattern
                   for pattern, _method in rows)


def test_host_hands_packages_a_coarse_country_and_nothing_finer():
    # The unit default rides the same approximate code a world flag uses.
    runtime = ENTRY.split("class _WorldCommunityRuntime", 1)[1]
    runtime = runtime.split("\nasync def calendar_handler", 1)[0]
    assert "    def country(self):" in runtime
    assert "return world_request_country(self.request)" in runtime
    # A cron run has no request, so it has no country rather than a guess.
    country = runtime.split("    def country(self):", 1)[1].split("\n    def ", 1)[0]
    assert "if self.request is None:" in country and 'return ""' in country


def test_every_surface_speaks_the_units_preference():
    # The Worker resolves; the clients render "units" and only ever store a
    # preference, so an auto profile follows the member across devices.
    assert 'UNIT_CHOICES = ("auto", "metric", "imperial")' in WORKER
    assert "IMPERIAL_WEIGHT_COUNTRIES" in WORKER
    for key in ("unitsPreference", "autoUnits"):
        assert key in WORKER and key in FEATURE and key in ANDROID
        assert key in (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
    # No client guesses the unit from its own locale: one account reading the
    # same profile from two devices must not see two different units.
    assert "MeasurementSystem" not in FEATURE
    assert "Locale" not in ANDROID
    assert 'QStringLiteral("unitsPreference"), m_units->currentData().toString()' in FEATURE
    assert '"unitsPreference" to unitsPreference,' in ANDROID


def test_desktop_footer_prompt_logs_to_the_diary_on_the_calories_section():
    extension = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(encoding="utf-8")
    issues = (REPO / "desktop" / "src" / "MainWindowIssues.cpp").read_text(encoding="utf-8")
    assert "virtual NativePackagePromptTarget hostPromptTarget() const" in extension
    assert "virtual bool submitHostPrompt(const QString &, const QStringList &)" in extension
    assert "std::function<void()> promptTargetChanged;" in extension
    assert "[this] { updateQuickAddEnterTarget(); }," in HOST
    assert "bool MainWindow::submitQuickAddToPackage()" in HOST
    enter = issues.split("if (obj == m_issueQuickAdd && event->type() == QEvent::KeyPress)", 1)[1]
    assert enter.index("submitQuickAddToPackage()") < enter.index("quickAddShouldFollowUpAgent()")
    assert "if (!m_issueQuickAdd || submitQuickAddToPackage())" in issues
    assert "hostPromptTarget() const override" in FEATURE_HEADER
    assert "submitHostPrompt(const QString &text, const QStringList &attachments) override" in FEATURE_HEADER
    body = FEATURE.split("bool CaloriesFeature::submitHostPrompt(", 1)[1].split("\nvoid CaloriesFeature::", 1)[0]
    assert "if (!m_installed)\n        return false;" in body
    for path in ('"/analyze"', '"/weights/"'):
        assert path in body
    assert 'QStringLiteral("quick")' in body and "promptMatches(value)" in body
    assert 'apiPath(QStringLiteral("/receipt"))' in FEATURE
    assert 'apiPath(QStringLiteral("/quick-foods"))' in FEATURE
    sources = {source for row in CATALOG["endpoints"] for source in row.get("source", [])}
    for function in ("submitHostPrompt", "readReceipt", "saveMyFoods"):
        assert any(source.endswith("CaloriesFeature::" + function) for source in sources)
