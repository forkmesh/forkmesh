from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WWW = ROOT / "www" / "public"


def read(path):
    return path.read_text(encoding="utf-8")


def test_page_titles_use_favorit_rather_than_the_old_serif():
    styles = read(WWW / "styles.css")
    assert '--display: "ForkMesh Favorit"' in styles
    assert 'src: url("/assets/fonts/ABCFavorit-Bold.woff2") format("woff2")' in styles
    assert '--serif: "ForkMesh Favorit"' in read(WWW / "feature-post.css")
    assert '--blog1-serif:"ForkMesh Favorit"' in read(WWW / "blog.html")
    for name in ("privacy.html", "terms.html", "404.html"):
        html = read(WWW / name)
        assert "Domine" not in html, name
        assert '"ForkMesh Favorit"' in html, name


def test_kit_title_is_left_aligned_in_the_display_face():
    styles = read(WWW / "styles.css")
    title = styles.split(".kit-title {", 1)[1].split("}", 1)[0]
    assert "font-family: var(--display);" in title
    assert "text-align: center" not in title
    for rule in (
        "body.kit-page {",
        ".kit-hero {",
        ".kit-badge {",
        ".kit-lede {",
        ".kit-btn {",
        ".kit-btn.is-primary {",
        ".kit-card {",
        ".kit-tile {",
    ):
        assert rule in styles, rule
