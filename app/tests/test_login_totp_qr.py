from pathlib import Path

PUBLIC = Path(__file__).resolve().parents[1] / "public"


def test_totp_setup_shows_a_qr_code_of_the_otpauth_uri():
    html = (PUBLIC / "login.html").read_text("utf-8")
    js = (PUBLIC / "login.js").read_text("utf-8")
    assert 'id="totp-setup-qr"' in html
    assert "showTotpQr(body.otpauthUri)" in js
    assert 'script.src = "/qr.js"' in js
    assert "ForkMeshQR.toCanvas(uri" in js
