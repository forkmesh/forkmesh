import struct
from pathlib import Path


WWW_PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"


def _read(name: str) -> str:
    return (WWW_PUBLIC / name).read_text(encoding="utf-8")


def _css_rule(css: str, selector: str) -> str:
    start = css.index(selector + " {")
    return css[start:css.index("}", start)]


def test_auth_mesh_draws_the_generated_land_mask():
    script = _read("auth-mesh.js")
    header = (WWW_PUBLIC / "assets" / "land-mask.png").read_bytes()[:24]

    assert 'const MASK_SRC = "/assets/land-mask.png";' in script
    assert header[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", header[16:24]) == (1440, 720)
    for avatar in ("/assets/kaif.webp", "/assets/fediverse-avatar.png", "/assets/jett.webp", "/assets/logo.svg"):
        assert f'avatar: "{avatar}"' in script


def test_auth_mesh_no_longer_ships_the_hand_drawn_map():
    assert "mesh-map.svg" not in _read("auth-mesh.css")
    assert "mesh-map.svg" not in _read("auth-mesh.js")
    assert not (WWW_PUBLIC / "assets" / "mesh-map.svg").exists()


def test_auth_mesh_leaves_the_site_header_solid():
    css = _read("auth-mesh.css")

    assert ".forkmesh-simple-header" not in css
    assert "inset: 3.5rem 0 0;" in _css_rule(css, ".auth-mesh")
