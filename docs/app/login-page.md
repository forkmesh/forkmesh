# The login page

`app/public/login.html` is the sign-in form at `/login`. Its card was rebuilt
on 2026-09-27 to follow the Cal.com login the design was taken from. The page
loads the shared auth stylesheet `www/public/auth-page-theme.css` and the map
backdrop from `www/public/auth-mesh.js`; the backdrop is described in
`docs/www/auth-mesh.md`.

## Code map

| Path | Responsibility |
| --- | --- |
| `app/public/login.html` | Markup and inline styles: the card, and its palette for each theme |
| `app/public/login.js` | Validation, the password toggle, sign-in, and the localhost demo login |
| `app/tests/test_auth_pages_frontend.py` | Static contracts for the login and signup pages |

## Architecture

### The card is a frame around an inset panel

`.auth-card` is the outer frame: a 1px border, a 16px radius and the footer
colour as its background. The form sits in `.auth-card-body`, a panel with a
15px radius (the frame's radius less its border). The panel's bottom edge is
drawn with a `0 1px 0` box-shadow rather than a border, so it follows the
panel's rounded bottom corners and the footer colour shows in the corner
notches, as in the reference. The footer links sit on the frame colour with
no divider of their own.

The email and password fields and the "Log in" button share one height,
2.5rem, on a 0.625rem radius. The earlier card had a 46px button under 44px
fields; the user asked for a button no taller than the fields. (In the
reference itself the button is about 4px taller than its fields.)

### One palette per theme, expressed as the shared tokens

Every auth page loads `auth-page-theme.css` after its inline `<style>`, and
that sheet's `html.light` rules are unlayered, so in light mode they win over
a page's own rules whatever their order. The login page therefore declares its
palette as the shared `--auth-*` tokens, dark values on `body[data-fm-auth]`
and light values on `html.light body[data-fm-auth]`, and the shared light
rules resolve to the login palette. Two tokens feed two different colours and
are re-scoped: `.field` maps `--auth-text-secondary` to the label colour (the
subtitle keeps the muted value), and `.field-label-row` maps `--auth-link` to
the muted "Forgot?" colour (the footer links keep the strong one). Page-only
tokens (`--auth-panel-bg`, `--auth-panel-edge`, `--auth-label`,
`--auth-muted`, `--auth-icon`, `--auth-action-line`, `--auth-action-shadow`)
use the same prefix.

Both themes are neutral: no glow at the top (`--auth-glow: none`), a grey
focus ring, and a primary action that is off-white in dark mode and near
black in light mode, never green. The user asked for the green glow and the
green focus ring to go.

### Autofill

Chrome paints autofilled fields light blue with an `!important` background,
which is most of why the light card looked wrong in a signed-in browser. The
fields keep their own colours with an inset box-shadow of the field colour
and `-webkit-text-fill-color`; the focused autofill rule repeats the focus
ring, because it replaces the whole box-shadow.

## Traps

- The "Local demo login" box renders only on `localhost` and `127.0.0.1`
  (`demoLoginAllowed()` in `login.js`), so local screenshots show a card that
  production never does.
- A light-mode colour set directly on an element in the inline style loses to
  the shared sheet's `html.light` rule for the same property. Set the token
  instead.

## Recent changes

- 2026-09-27: framed card after the Cal.com reference, 40px fields and
  button, neutral palette in both themes, autofill colours fixed.
