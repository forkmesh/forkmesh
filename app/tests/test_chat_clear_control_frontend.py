from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")


def test_removed_clear_button_is_queried_safely_before_init_uses_it():
    declaration = 'const clearBtn = document.querySelector("#chat-clear");'
    assert declaration in CHAT
    assert CHAT.index(declaration) < CHAT.index("async function initChat()")
    assert 'if (clearBtn) clearBtn.addEventListener("click", clearChat);' in CHAT
