import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAGE = (ROOT / "app" / "public" / "start.html").read_text(encoding="utf-8")
SCRIPT = (ROOT / "app" / "public" / "start.js").read_text(encoding="utf-8")
HOME = (ROOT / "www" / "templates" / "pages" / "index.html").read_text(encoding="utf-8")
HOME_PUBLIC = (ROOT / "www" / "public" / "index.html").read_text(encoding="utf-8")
WORLD = (ROOT / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
CSS = (ROOT / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")
RUNTIME = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")


def test_the_form_asks_for_everything_in_six_sections():
    for name in ("projectName", "host", "sshUser", "port", "sshPassword",
                 "sshPrivateKey", "sshPassphrase", "hostKeyFingerprint", "domain",
                 "adminUsername", "adminEmail", "adminPassword", "ownerType",
                 "ownerName", "mailProvider", "mailApiKey", "mailFrom",
                 "mailgunDomain", "mailgunRegion", "ownWorld", "listOnHomepage"):
        assert 'name="%s"' % name in PAGE, name
    for provider in ("mailtrap", "sendgrid", "mailgun", "none"):
        assert '<option value="%s">' % provider in PAGE
    assert PAGE.count('class="section-num"') == 6
    assert "Powered by ForkMesh" in PAGE
    assert 'type="file" data-start-key-file' in PAGE
    assert "vultr" not in PAGE.lower()
    assert not re.search(r'(name|id|data-[a-z-]*)="[^"]*cloudflare', PAGE, re.I)
    assert "api token" not in PAGE.lower()


def test_the_script_launches_streams_and_redirects():
    assert 'fetch("/api/start/launch"' in SCRIPT
    assert "/api/start/launches/${encodeURIComponent(id)}" in SCRIPT
    assert "POLL_MS = 1000" in SCRIPT
    assert "location.assign(url)" in SCRIPT
    assert 'params.get("launch")' in SCRIPT
    assert 'params.get("plot")' in SCRIPT
    assert 'addEventListener("pagehide", clearSecrets)' in SCRIPT
    for name in ("sshPassword", "sshPrivateKey", "sshPassphrase", "adminPassword", "mailApiKey"):
        assert '"%s"' % name in SCRIPT.split("SECRET_FIELDS = [", 1)[1].split("]", 1)[0]
    assert "localStorage" not in SCRIPT and "sessionStorage" not in SCRIPT


def test_the_old_cloudflare_and_vultr_launcher_is_gone():
    assert not (ROOT / "app" / "src" / "start_setup.py").exists()
    assert "_start_setup" not in RUNTIME
    assert 'url.path.startswith("/api/start/")' in RUNTIME
    assert "instance_launch.route(" in RUNTIME


def test_the_homepage_links_to_the_start_page():
    for html in (HOME, HOME_PUBLIC):
        assert 'href="/start"' in html
        assert "Start your own instance" in html


def test_world_plots_open_the_start_page():
    assert "onStartWorldSignSelect: (plot) => this.openStartInstance(plot)," in WORLD
    opener = WORLD.split("  openStartInstance(plot = \"\") {", 1)[1].split("\n  }\n", 1)[0]
    assert '"/start"' in opener
    assert 'searchParams.set("plot"' in opener
    for retired in ("data-world-instance-launcher", "data-world-start-world",
                    "continueInstanceLaunch", "downloadInstanceSetupCard",
                    "validateInstanceLauncher", "chooseStartWorldConnection",
                    "startWorldSetupScript", "continueStartWorldServer",
                    "cloudflareToken", "openStartWorldDesktop"):
        assert retired not in WORLD, retired
    assert ".world-start-world" not in CSS
    assert ".world-instance-launcher" not in CSS


def test_a_settings_file_fills_the_whole_form_and_starts_in_one_click():
    import json
    example = json.loads((ROOT / "app" / "public" / "start-config.example.json").read_text(encoding="utf-8"))
    fields = SCRIPT.split("CONFIG_FIELDS = [", 1)[1].split("]", 1)[0]
    form_fields = [key for key in example if not key.startswith("_")]
    for name in form_fields:
        assert name == "owners" or 'name="%s"' % name in PAGE, name
        assert '"%s"' % name in fields, name
    assert example["_docs"] == "https://forkmesh.com/docs/start-config/"
    assert 'href="https://forkmesh.com/docs/start-config/"' in PAGE
    assert 'href="/start-config.example.json" download=' in PAGE
    assert "data-start-config-copy" in PAGE and "navigator.clipboard.writeText" in SCRIPT
    assert 'accept=".zip,application/zip,.json,application/json" data-start-config-file' in PAGE
    assert 'href="/start-config.example.zip" download="forkmesh-instance.zip"' in PAGE
    assert 'configLaunch.addEventListener("click", () => form.requestSubmit())' in SCRIPT
    guide = (ROOT / "www" / "docs" / "start-config.md").read_text(encoding="utf-8")
    for name in form_fields:
        assert "`%s`" % name in guide, name
    assert "https://app.forkmesh.com/start-config.example.json" in guide
    assert "https://app.forkmesh.com/start-config.example.zip" in guide
    assert "(start-config.md)" in (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")


def test_the_form_takes_an_optional_logo():
    assert 'type="hidden" name="logo"' in PAGE
    assert 'accept="image/png,image/jpeg,image/webp,image/svg+xml" data-start-logo-file' in PAGE
    assert "data-start-preview-logo" in PAGE and "data-start-logo-clear" in PAGE
    assert "reader.readAsDataURL(file)" in SCRIPT
    assert "MAX_LOGO_BYTES = 200 * 1024" in SCRIPT
    assert 'logo: value("logo"),' in SCRIPT
    assert '"logo"' in SCRIPT.split("CONFIG_FIELDS = [", 1)[1].split("]", 1)[0]


def test_anyone_can_build_a_theme_with_a_live_preview_at_setup():
    import sys
    sys.path.insert(0, str(ROOT / "app" / "src"))
    import instance_theme
    assert '<input type="hidden" name="theme" />' in PAGE
    for hook in ("data-start-theme-accents", "data-start-theme-neutrals", 'data-start-theme-mode="light"',
                 'data-start-theme-mode="dark"', "data-start-theme-shuffle", "data-start-theme-preview",
                 "data-start-theme-tokens", "data-start-theme-reset"):
        assert hook in PAGE, hook
    groups = SCRIPT.split("const THEME_GROUPS = [", 1)[1].split("\n  ];", 1)[0]
    page_tokens = re.findall(r'"([a-z-]+)"', groups.split("[", 1)[1])
    assert sorted(set(page_tokens)) == sorted(instance_theme.TOKENS)
    presets = SCRIPT.split("const THEME_PRESETS = {", 1)[1].split("};", 1)[0]
    for name in instance_theme.PRESETS:
        assert "%s:" % name in presets, name
    preview = PAGE.split("data-start-theme-preview", 1)[1].split("data-start-theme-more", 1)[0]
    used = set(re.findall(r"var\(--t-([a-z-]+)\)", PAGE))
    assert used <= set(instance_theme.TOKENS)
    assert {"background", "primary", "link", "border", "tab-underline", "open"} <= used
    assert "data-start-preview-name" in preview and "data-start-preview-logo" in preview
    assert 'theme: value("theme").startsWith("{") ? JSON.parse(value("theme")) : value("theme"),' in SCRIPT
    assert 'if (key === "theme") {' in SCRIPT
    assert '"theme"' in SCRIPT.split("CONFIG_FIELDS = [", 1)[1].split("]", 1)[0]
    assert "blt" not in PAGE.lower()


def test_the_example_zip_holds_the_example_settings_and_a_logo():
    import sys
    import zipfile
    sys.path.insert(0, str(ROOT / "app" / "src"))
    import instance_launch
    example = (ROOT / "app" / "public" / "start-config.example.json").read_bytes()
    with zipfile.ZipFile(ROOT / "app" / "public" / "start-config.example.zip") as bundle:
        assert sorted(bundle.namelist()) == ["forkmesh-instance.json", "logo.svg"]
        assert bundle.read("forkmesh-instance.json") == example
        logo = bundle.read("logo.svg")
        assert all(info.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) for info in bundle.infolist())
    assert instance_launch.logo_kind(logo) == "svg+xml"
    assert len(logo) <= instance_launch.MAX_LOGO_BYTES
    assert json.loads(example)["logo"] == "logo.svg"


def test_a_settings_zip_is_read_in_the_browser():
    assert 'new DecompressionStream("deflate-raw")' in SCRIPT
    assert "0x06054b50" in SCRIPT and "0x02014b50" in SCRIPT and "0x04034b50" in SCRIPT
    assert "MAX_ZIP_BYTES = 1024 * 1024" in SCRIPT
    assert '!entry.path.startsWith("__MACOSX/")' in SCRIPT
    assert "if (total > limit)" in SCRIPT
    assert "settings.logo = await dataUrl(" in SCRIPT


def test_an_organization_can_have_other_owners():
    assert "data-start-owner-add" in PAGE and "data-start-owner-template" in PAGE
    row = PAGE.split("<template data-start-owner-template>", 1)[1].split("</template>", 1)[0]
    for name in ("username", "email", "password"):
        assert 'data-owner="%s"' % name in row
        assert "name=" not in row.split('data-owner="%s"' % name, 1)[0].rsplit("<input", 1)[1]
    assert 'type="password" data-owner="password"' in row
    assert "MAX_OWNERS = 20" in SCRIPT
    assert 'for (const row of ownerRows()) ownerInput(row, "password").value = "";' in SCRIPT
    assert 'for (const owner of body.owners) owner.password = "";' in SCRIPT
    assert '$("[data-start-owner-others]").hidden = ownerType !== "org";' in SCRIPT


def test_world_and_homepage_listing_are_on_by_default():
    assert 'type="checkbox" name="ownWorld" checked' in PAGE
    assert 'type="checkbox" name="listOnHomepage" checked' in PAGE
    assert 'ownWorld: field("ownWorld").checked' in SCRIPT
    assert 'listOnHomepage: field("listOnHomepage").checked' in SCRIPT
    assert 'input.checked = !/^(false|no|off|0)$/i.test(value.trim())' in SCRIPT
    example = json.loads((ROOT / "app" / "public" / "start-config.example.json").read_text(encoding="utf-8"))
    assert example["ownWorld"] is True and example["listOnHomepage"] is True
