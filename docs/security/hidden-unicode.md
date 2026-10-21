# Hidden Unicode in user input

Every text field on the web, the desktop and both phone apps removes
invisible Unicode characters as the user types or pastes, and the app server
removes them again from every parsed request body. One policy,
`forkmesh-hidden-unicode-v1`, is shared by all of them and pinned by
`conformance/hidden-unicode/v1/vectors.json`.

## Why

Invisible characters let text say one thing to a person and another to a
program:

- **Tag characters (U+E0000 to U+E007F)** spell out ASCII that no font draws.
  This "ASCII smuggling" hides instructions for language models in otherwise
  normal text. In 2026 it was used at scale in phishing mail, and it breaks
  agent pipelines that read untrusted text ([The New Stack, September 2026](https://thenewstack.io/?p=22835903)).
- **Variation selectors (U+FE00 to U+FE0F, U+E0100 to U+E01EF)**: there are 256
  of them, one per byte value, so a chain of them after a single emoji can carry
  any payload ([Paul Butler, 2025](https://paulbutler.org/2025/smuggling-arbitrary-data-through-an-emoji/)).
  The same trick reportedly hid an npm loader in 2025.
- **Bidi overrides, embeddings and isolates (U+202A to U+202E, U+2066 to U+2069)**
  reorder what is displayed without changing the logical text. This is the
  "Trojan Source" attack on code review ([Krebs, 2021](https://krebsonsecurity.com/2021/11/trojan-source-bug-threatens-the-security-of-all-code/)).
- **Zero-width and other format characters** (U+200B, U+2060 to U+2064,
  U+FEFF, the soft hyphen, the Hangul fillers) hide binary payloads, split
  words to slip past filters, and make two names that look the same compare
  unequal.

Agents on this platform read issues, chat, pull requests and notes. Nothing a
person cannot see should reach an agent, the database or another user.

## The policy

`app/src/hidden_unicode.py` is the reference. Its rules:

1. **Always removed:**
   - C0 and C1 controls, except tab, line feed and carriage return
   - soft hyphen U+00AD and the combining grapheme joiner U+034F
   - the Hangul fillers U+115F, U+1160, U+3164 and U+FFA0
   - the Khmer inherent vowels U+17B4 and U+17B5
   - the Mongolian free variation selectors and vowel separator (U+180B to U+180F)
   - zero-width space U+200B
   - the bidi embeddings, overrides and isolates
   - U+2060 to U+206F
   - the byte-order mark U+FEFF
   - interlinear annotation (U+FFF9 to U+FFFB)
   - the shorthand and musical format controls
   - every noncharacter and lone surrogate
   - variation selectors U+FE00 to U+FE0D and U+E0100 to U+E01EF
   - tag characters, except the one valid use below
2. **Line and paragraph separators** U+2028 and U+2029 become a newline.
3. **Kept only where real text needs them.** Each rule looks at the character
   before (the last character kept) and the character after.
   - **Emoji presentation selectors** U+FE0E and U+FE0F: kept once after a
     visible character. They are dropped at the start of the text, after
     whitespace, after a joiner, or when repeated.
   - **Zero-width joiner** U+200D: kept between two emoji (this keeps family,
     profession and flag sequences working). Also kept between two letters of
     a script that joins: Arabic, Syriac, the Indic scripts, Tibetan, Myanmar,
     Khmer and Mongolian.
   - **Zero-width non-joiner** U+200C: kept only between letters of those
     joining scripts, as Persian and the Indic scripts need it.
   - **Direction marks** U+200E, U+200F and U+061C: kept only next to a
     right-to-left letter.
   - **Tag characters:** kept only as the subdivision flag sequence. That is
     U+1F3F4, then 2 to 7 tag letters or digits, then the cancel tag (the flags
     of England, Scotland and Wales).

Each rule only removes characters: nothing is added or reordered, and visible
text (accents, CJK, private-use glyphs, visible spaces) is untouched. Stripping
is idempotent.

**Out of scope:** visible abuse such as stacked combining marks
("Zalgo") or look-alike letters (homoglyphs). The policy removes what cannot
be seen.

## Where it runs

| Surface | Code | When |
| --- | --- | --- |
| App server | `app/src/hidden_unicode.py`; `request_bodies.bounded_json_request`, every `json_body` copy, the admin form parsers, `app/host/forkmesh_host/js.py` `Request.json()` / `Request.formData()` | after parsing, before the handler sees the body |
| Web | `www/public/safe-input.js`, loaded by every page next to `analytics.js` | capture-phase `input`, `change` and `compositionend` on inputs, textareas and contenteditable |
| Desktop | `desktop/src/HiddenUnicode.{h,cpp}`, `HiddenUnicodeGuard.{h,cpp}`, installed in `main.cpp` | `QLineEdit` through a validator; `QPlainTextEdit` and `QTextEdit` on each user change |
| iPhone | `native/ios/ForkMeshCore/Sources/FMBase/HiddenUnicode.swift`, `native/ios/ForkMesh/Platform/HiddenUnicodeInputGuard.swift`, installed by `AppDelegate` | every `UITextField` and `UITextView` text change, SwiftUI fields included |
| Android | `native/android/core/.../security/HiddenUnicode.kt`, `app/.../ui/kit/FmSafeInput.kt` | `FmTextField`, `AccountFilledField`, every raw `BasicTextField` / `OutlinedTextField`, and an `InputFilter` on the two `EditText`s |

### Exactness exceptions

Some text has to stay byte for byte:

- **Raw bodies.** The text and byte readers are not touched:
  `bounded_request_text`, `request.text()`, `request.bytes()`. Signature and
  digest checks (ActivityPub inbox, webhooks, federation relay, runner
  leases, hosted Git) therefore still see the exact body. Only parsed values
  are cleaned.
- **Secrets.** On the server, values under keys that name a secret stay
  exact. These are keys containing `password`, `passphrase`, `passcode`,
  `secret`, `token`, `signature`, `ciphertext`, `privatekey` or `sealed`,
  plus the exact keys `sig`, `nonce`, `salt`, `otp`, `totp`, `pin`, `mac`
  and `hmac`. Every client leaves password fields alone too
  (`type=password`, `QLineEdit::Password`, `isSecureTextEntry`,
  `PasswordVisualTransformation`). A password is hashed and never shown, and
  rewriting it would lock out anyone whose saved password already contains
  such a character.
- **IME composition.** Text is cleaned only once composition ends, so CJK and
  other input methods are not disturbed.
- **Read-only desktop views.** These show text exactly as received. Editable
  desktop editors clean only what the user inserts, and only while the editor
  is the window's focus widget. A file loaded into an editor is therefore not
  rewritten until someone edits it.
- **Undo.** After an undo or redo the desktop leaves the text alone. Undo
  first takes back the cleanup, and a second undo takes back the paste; this
  is the same as autocorrect.
- **Not covered.** The terminal widget passes keystrokes to a shell
  unchanged. End-to-end sealed chat is ciphertext on the server, so only the
  clients clean it.

## Changing the policy

1. Edit `app/src/hidden_unicode.py`.
2. Mirror the change in `safe-input.js`, `HiddenUnicode.cpp`,
   `HiddenUnicode.swift` and `HiddenUnicode.kt`.
3. Add a case to `conformance/hidden-unicode/v1/generate_vectors.py` and run
   it.
4. Copy `vectors.json` to
   `native/android/core/src/test/resources/hidden-unicode/v1/`.
5. Run the checks:
   - `python -m pytest -q app/tests/test_hidden_unicode.py`
   - `forkmesh-hidden-unicode-tests`
   - `swift test --filter HiddenUnicodeTests` in `native/ios/ForkMeshCore`
   - `./gradlew :core:coreTest -Pfilter=HiddenUnicode`

A change that keeps or drops a different set of characters is a new policy
version, so it goes in `v2`.

Write invisible characters in source as numeric code points (`0x200B`,
`chr(0x200B)`). Editing tools can turn a typed `\u200b` escape into the real
invisible character. `test_new_sources_hold_no_literal_hidden_characters`
fails if one slips into the policy files.

## Signed code in pull requests

A pull request's `patch` and `commits` are code and are signed byte for byte,
so cleaning them broke every pull whose code holds a character on the list (a
UTF-8 byte-order mark at the top of a Windows file, a zero-width space inside
a string, a whitespace table written with literal characters) with
`bad_signature`, and would otherwise have changed the code silently.
`pulls_handler` reads the body once more without cleaning
(`bounded_json_request_with_raw`), verifies the signature over the exact
`patch` and `commits`, keeps them exact, and stores `hiddenUnicode: true` on
the pull when either holds a character this policy removes, so a reviewer and
an agent can be warned instead of the bytes being changed. Prose stays clean:
when the signature only verifies over a title, base or head that cleaning
changed, the server answers 400 `hidden_unicode` with the field names instead
of `bad_signature`.

