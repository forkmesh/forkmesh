from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WWW = ROOT / "www" / "public"


def read(path):
    return path.read_text(encoding="utf-8")


def assert_kit_hero(html, name):
    assert '<body class="kit-page">' in html, name
    assert 'class="kit-badge"' in html, name
    assert 'class="kit-title"' in html, name
    assert 'class="eyebrow"' not in html, name


def test_desktop_and_press_open_on_the_page_kit():
    for name in ("desktop.html", "press.html"):
        assert_kit_hero(read(WWW / name), name)


def test_desktop_install_command_has_one_prompt_and_its_own_copy_button():
    html = read(WWW / "desktop.html")
    assert 'id="install-cmd"' not in html
    assert 'id="install-copy"' not in html
    assert html.count('class="install-cmd-prompt"') == 1
    assert "data-install-copy" in html
    assert "await navigator.clipboard.writeText(text);" in html
    for os in ("macos", "windows", "linux"):
        assert f'data-os-card="{os}"' in html
    assert html.count('class="kit-tile"') == 3
    assert ".os-card:hover" not in html
    assert "item.title = `Not published in ${manifest.tag}: ${name}`;" in html
    assert 'here.textContent = "This device";' in html
