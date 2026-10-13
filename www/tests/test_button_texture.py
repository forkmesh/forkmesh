import re
from pathlib import Path

WWW = Path(__file__).resolve().parents[1] / "public"

DARK_PRIMARY_FILL = "linear-gradient(180deg, #ffffff 0%, #e8e8ea 100%)"
LIGHT_PRIMARY_FILL = "linear-gradient(180deg, #2c2c2c 0%, #222222 100%)"
LIGHT_PRIMARY_SHADOW = (
    "inset 0 1px 0 rgba(255, 255, 255, 0.17), 0 1px 2px rgba(0, 0, 0, 0.16)"
)
LIGHT_SECONDARY_FILL = "linear-gradient(180deg, #ffffff 0%, #fcfcfc 100%)"
LIGHT_SECONDARY_BORDER = "#e1e2e3"

TOKENS = {
    "hackathon.css": "--btn-",
    "styles.css": "--button-",
    "mesh-page-theme.css": "--mesh-btn-",
}


def squash(text):
    return re.sub(r"\s+", "", text)


def test_every_surface_uses_the_same_textured_recipe():
    for name, prefix in TOKENS.items():
        source = squash((WWW / name).read_text(encoding="utf-8"))
        primary = "fill" if prefix == "--button-" else "primary-fill"
        shadow = "shadow" if prefix == "--button-" else "primary-shadow"
        assert squash(f"{prefix}{primary}: {DARK_PRIMARY_FILL};") in source, name
        assert squash(f"{prefix}{primary}: {LIGHT_PRIMARY_FILL};") in source, name
        assert squash(f"{prefix}{shadow}: {LIGHT_PRIMARY_SHADOW};") in source, name
        assert squash(
            f"{prefix}secondary-fill: {LIGHT_SECONDARY_FILL};") in source, name
        assert squash(
            f"{prefix}secondary-border: {LIGHT_SECONDARY_BORDER};") in source, name


def test_inline_pages_carry_the_same_values():
    blog = squash((WWW / "blog.html").read_text(encoding="utf-8"))
    assert squash(f"--blog1-btn-fill:{LIGHT_PRIMARY_FILL};") in blog
    assert squash(f"--blog1-btn-fill:{DARK_PRIMARY_FILL};") in blog
    assert "background:var(--blog1-btn-fill);" in blog

    report = squash((WWW / "security-report.html").read_text(encoding="utf-8"))
    assert squash(f"background: {LIGHT_PRIMARY_FILL};") in report
    assert squash(f"background: {DARK_PRIMARY_FILL};") in report


def test_primary_and_secondary_buttons_wear_the_texture():
    styles = squash((WWW / "styles.css").read_text(encoding="utf-8"))
    primary = styles.split(".primary-button{", 1)[1].split("}", 1)[0]
    assert "background:var(--button-fill);" in primary
    assert "box-shadow:var(--button-shadow);" in primary
    secondary = styles.split(
        ".secondary-button{border-color:var(--button-secondary-border);", 1,
    )[1].split("}", 1)[0]
    assert "background:var(--button-secondary-fill);" in secondary
    assert "box-shadow:var(--button-secondary-shadow);" in secondary
