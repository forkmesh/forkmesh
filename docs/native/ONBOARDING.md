# Login and first-run onboarding

The phone apps' first screens share one look and one set of pieces. iOS keeps
them in `ForkMesh/Kit/FmOnboardingKit.swift`, Android in
`ui/kit/FmOnboardingKit.kt`. Both draw the same colours, sizes and timings, so
a change to one belongs in the other.

## What shows, and when

1. **Welcome, Log in, Sign up** (`AuthFlowView`, `AuthFlow`). One background
   stays in place while the pages change.
2. **Onboarding** (`OnboardingFlowView`, `OnboardingFlow`). On a fresh install,
   after sign-in or guest: the permissions page, then the starter pack. The
   steps come from what is still pending, so a phone that only owes one sees
   only that one. Back returns a step and keeps the chosen pack. Nothing is
   stored until the flow finishes, so leaving midway shows it again. Skip
   finishes with the default pack.
3. **Tour** stays as it was, a sheet over Home.

All of these screens are dark whatever the phone's appearance
(`fmForcedDark`, `FmForcedDark`).

## Pieces

- `FmPillButton`: primary is a white capsule with a soft top-to-bottom shade,
  58pt high; `dark` is the secondary. These replace the flat auth buttons on
  these screens only; `FmFilledButton` and the rest of the app are unchanged.
- `FmPillField`: the label stays above the field, 58pt high, radius 20, a thin
  border that brightens on focus.
- `FmAuroraBackground`: the sky gradient and a dotted world map. The map is a
  table of land spans drawn in code, not an image, so it stays sharp at any
  size and adds nothing to the download. It fades out at the top and bottom so
  it never sits behind the header or a title.
- `FmGlassIcon`, `FmAgentOrb`, `FmOnboardingProgress`,
  `FmOnboardingBackButton`.

## Motion

Taken from a reference recording, frame by frame.

- **Press:** the button grows 3% with a soft glow.
- **Leaving a page:** content fades in 160ms; the header stays. The new page
  waits for that fade, so the two never overlap.
- **Progress:** the long pill slides to the next step over 450ms.
- **Arriving:** a fixed order, about 800ms in all. The hero rises and scales
  in, each title line wipes in from the left with a soft edge and a tilt that
  settles (`FmRevealLine`), the subtitle follows the same way, the button
  brightens from grey (`fmBrighten`), and the last text line comes last. Small
  items such as chips and badges appear one after another.
- **Counting:** "Asked up front" runs from the permission count to 0.
- **Idle:** the orb's colours turn and its eyes blink every 2.8s, the hero card
  tilts slowly, and the map dots breathe.

Reduce Motion turns all of this off: every piece appears in its final state.
