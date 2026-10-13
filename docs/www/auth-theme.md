# Auth page theme tokens

`www/public/auth-page-theme.css` is the light and dark theme shared by the
login, signup, forgot-password, reset-password, verify-pending and
mirror-payouts pages and the security report. It declares the `--auth-*`
tokens for both themes and the light-mode rules that apply them.

## Architecture

- Each page styles dark mode in its own inline `<style>` and loads this sheet
  after it. `test_page_families_load_complete_light_theme_styles` pins that
  order, and `FAMILY_THEME_CONTRACTS` in `app/tests/test_public_light_theme.py`
  pins the light token values.
- The dark mappings sit in `@layer auth-page-theme`, so a page's unlayered
  inline rules win in dark mode. The `html.light` rules are unlayered and come
  later, so in light mode they win over a page's inline rules. A page that
  wants its own light look overrides the tokens, which these rules read, on an
  element of its own; `login.html` does this on `body[data-fm-auth]`
  (`docs/app/login-page.md`).

## Recent changes

- 2026-09-27: light mode no longer has a green glow at the top of the page
  (`--auth-glow: none`) or a green focus ring (`--auth-focus: #57606a`, a
  neutral grey that also serves as the checkbox accent). The user asked for
  both to go on the login page, and every page that shares this sheet showed
  the same green.
