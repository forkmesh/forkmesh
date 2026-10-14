# The login page

`app/public/login.html` is the sign-in form at `/login`. It loads the shared
auth stylesheet `www/public/auth-page-theme.css` and the map backdrop from
`www/public/auth-mesh.js` ([docs/www/auth-mesh.md](../www/auth-mesh.md)).

| Path | Responsibility |
| --- | --- |
| `app/public/login.html` | Markup, inline styles, per-theme palette |
| `app/public/login.js` | Validation, password toggle, sign-in, localhost demo login |
| `app/tests/test_auth_pages_frontend.py` | Static contracts for the login and signup pages |

## Styling rules

- The card is a bordered frame (`.auth-card`) around an inset panel
  (`.auth-card-body`); fields and the primary button share one height.
- The palette is declared as the shared `--auth-*` tokens: dark values on
  `body[data-fm-auth]`, light values on `html.light body[data-fm-auth]`.
  `auth-page-theme.css` loads after the inline styles and its `html.light`
  rules are unlayered, so a light colour set directly on an element loses;
  set the token instead. Page-only tokens use the same prefix.
- Both themes are neutral (no glow, grey focus ring).
- Autofilled fields keep their colours via an inset box-shadow and
  `-webkit-text-fill-color`; the focused autofill rule repeats the focus ring.

The "Local demo login" box renders only on `localhost` and `127.0.0.1`
(`demoLoginAllowed()` in `login.js`).
