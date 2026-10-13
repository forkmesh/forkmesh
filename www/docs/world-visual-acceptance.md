# ForkMesh World visual acceptance

The three supplied sample images are reference material for composition and
interaction density only. They are not ForkMesh production assets, and their
branding, character designs, text, logos, textures, and distinctive decorative
details are not copied.

## Patterns retained

- Sample 1: a layered city that remains legible as a whole, persistent
  edge-navigation, a readable code destination, visible collaboration
  activity, and chat that is reachable without leaving the spatial context.
- Sample 2: one strong infrastructure focal point, surrounding node/routing
  relationships, and compact status panels that explain the operational
  meaning of the visual.
- Sample 3: translucent operational panels, a clearly identified community
  destination, and nearby work/review destinations with different visual
  identities.

ForkMesh applies those general patterns through its original Town Square,
quick destination dock, organization and repository districts, routing
station, chat destination, and separate Support Center.
Every spatial metaphor keeps a technical explanation panel.

## Reproducible acceptance viewports

Playwright owns deterministic Chromium baselines for:

- Desktop: 1440 × 900, fine pointer.
- Portrait phone: 390 × 844, coarse pointer.
- Landscape phone: 844 × 390, coarse pointer.

The tests fix the UTC clock, API fixtures, day theme, reduced-motion mode, and
paused scene before comparison. They also exercise desktop keyboard movement, portrait
visual-viewport resizing, and landscape touch movement.

Run:

```sh
cd world/browser_tests
npx playwright test
```

Only an intentional visual review should refresh baselines:

```sh
npx playwright test --update-snapshots
```
