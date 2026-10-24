# Tour

The `tour` package (1.2.0) is the first-run welcome and the guided tour on the
desktop and the phone. On 2026-10-24 the operator asked for three depths,
Essentials, Advanced and The whole shebang, and for the phone tour to look like
the screens it talks about.

## Desktop

`OnboardingTour` (`marketplace/packages/tour/content/desktop`) is an overlay on
the main window that spotlights the real control of each step.

- **Tracks.** `OnboardingStep::track` is `OnboardingTrack::Essentials`,
  `Advanced` or `Everything`; `OnboardingTour::stepsForTrack` keeps the steps
  up to the chosen track in their order. `MainWindow::featureTourSteps`
  (`desktop/src/MainWindowOnboarding.cpp`) builds all 25 and tags them by id:
  Essentials is the rail, agents, the prompt, repositories, the project
  picker, Pings, search and the finish (8); Advanced adds planning, review and
  merge, automation, the Market, chat, files, settings and capture (16); every
  other id, including any new step, is The whole shebang.
- **Choosing.** The first-run welcome card and the rail's Tour button
  (`MainWindow::startFeatureTour`, `OnboardingTour::showChooser`, view
  `Choose`) show three track buttons with their stop counts
  (`setTrackSizes`). Return takes Essentials and 1 to 3 pick a track. Closing
  the chooser records nothing; closing the welcome still completes the first
  run, and finishing any track sets `onboarding/tourCompleteV1`.
- Tests: `forkmesh-tour-tests` (the package's own) and the onboarding part of
  `forkmesh-window-tests` (`--onboarding-only`).

## Phone

The iPhone tour is described in
[../native/NAVIGATION.md](../native/NAVIGATION.md#2022-the-tour): the same
three tracks, each stop a live mock built from the shell's own components that
you can try, with a button that opens the real place. The catalog is
`TourCatalog` in `FMPackages`, tested by `TourCatalogTests`. Android follows the
iOS row in [../native/SYNC.md](../native/SYNC.md).
