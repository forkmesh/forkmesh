# The projects folder on iOS and Android

The phone keeps its stores inside the app: Keychain or Keystore, preferences,
Documents, and Application Support or `filesDir`. The phone deletes all of it
with the app, and a fresh install even erases a Keychain the old app left
behind (`FreshInstallReset`).

So the phone also keeps its projects in a projects folder, laid out like the
desktop's `~/projects/<project>/data`
([projects-layout.md](../tools/projects-layout.md)):

```text
<projects folder>/
  forkmesh-data.json                         marker: type forkmesh.data-folder, created/updated, platform
  forkmesh/                                  the first (legacy) project, like the desktop's home project
    project.json                             slug, projectId, namespace, name, public key
    data/core/config/ForkMesh/settings.json  every preference except device-only keys
    data/core/identity/recovery-kit.json     the identity key, encrypted with a passphrase
    data/core/state/sync-index.json          folder path -> local path and size:mtime of each copied file
    data/core/logs/diagnostics/…             diagnostics
    data/core/documents/…  data/core/support/…   files that belong to no plugin
    data/chat/history/…  data/chat/local-mesh/…  data/chat/people-pictures/…
    data/agents/next-task/…  data/secure-drive/…  data/rss-reader/…
  <project-slug>/                            every other project (ProjectScope.slug), same shape
```

`DataFolder.pluginEntries` (Swift) and `DataFolder.PLUGIN_ENTRIES` (Kotlin)
map a top-level name in Documents or Application Support / `filesDir` to its
plugin folder. Anything else goes under `data/core/<documents|support>/`.

## Picking the folder

The app picks the folder itself, and the user can change it (Settings → Data
folder on iOS, Settings → Data on Android).

- **iOS.** The default is the app's iCloud Drive container
  (`url(forUbiquityContainerIdentifier: nil)/Documents/ForkMesh projects`). It
  survives an uninstall, and a reinstall finds it again with no picking.
  - The container needs the iCloud Documents capability on the app id. Until
    that is turned on, the lookup returns nil and the default is
    `Documents/ForkMesh projects`, visible in Files under On My iPhone ›
    ForkMesh but deleted with the app.
  - Any folder in Files can be chosen instead: On My iPhone, iCloud Drive or a
    provider. A folder inside the app container is refused.
- **Android.** The default is `getExternalFilesDir()/ForkMesh projects`,
  deleted with the app. The user chooses a Storage Access Framework tree
  (`OpenDocumentTree`, persisted read/write permission), for example
  Documents, for a copy that survives.
- *Use the automatic folder* goes back to the default. Switching never moves
  or deletes the old folder: the next copy fills the new one.

## Alert until it is safe

At every launch the app shows *Keep your ForkMesh data safe* in three cases:

- the projects folder is inside the app (it would be deleted with it);
- the folder can't be opened;
- the folder doesn't hold this project's identity key for its current public
  key.

*Not now* hides it until the next launch. On iOS the alert sits on the root
view; on Android it is drawn outside `AccountFeature.Gate`. Either way it shows
before sign-in, so a fresh install can restore first.

## Copying and restoring

- **Identity key.** Never written in clear. The folder holds the same
  encrypted container as a recovery kit (`RecoveryKit.export`, a passphrase of
  12 or more characters). `dataFolder/kitKey/<slug>` records which public key
  was saved, so a rotated key asks again.
- **Copying.** On launch, and whenever the app goes to the background (iOS
  `.background` inside a background task; Android `onPause`), the app copies
  the active project's settings, `project.json` and changed files.
  - A file is copied when its size:mtime differs from the index.
  - `LocalModels`, `CodeInsight`, `.Trash` and `tmp` are skipped, and so is the
    legacy project's `projects/` folder, which holds the other projects.
  - A copy goes to a partial file that is then renamed over the old one.
    Nothing in the folder is ever deleted.
- **Settings.** iOS writes `forkmesh.mobile-settings`. Android writes
  `forkmesh.android-settings`, raw values tagged with their type. Keys under
  `dataFolder/` and `install/` stay on the device. They live in `forkmesh/`
  because most preference keys are app-wide (`ProjectKeyPolicy`).
- **Restore.** A folder that holds projects whose saved identity key differs
  from this install's offers *Restore* (one passphrase for all of them) or
  *Keep this install's data instead*.
  - Restore writes each seed into `<namespace>identity/ed25519Seed`, applies
    the settings, and copies back, through the index, only files missing
    locally. Index paths with `..` are refused.
  - It then asks for a restart (iOS *Close ForkMesh*, Android *Restart
    ForkMesh*).

## Code

| Piece | iOS | Android |
| --- | --- | --- |
| Layout, plugin map, settings codec, copy, kit | `ForkMeshCore/Sources/FMAccount/DataFolder.swift` | `core/.../settings/DataFolder.kt` |
| Kit opening | `RecoveryKit.openSeed` | `RecoveryKit.openSeed` |
| Folder access | automatic folder, or a security-scoped bookmark (`dataFolder/location`) | automatic folder, or `SafDataFolderIo` over the tree URI (`dataFolder/location`) |
| State and actions | `ForkMesh/Features/Account/Data/DataFolderModel.swift` | `app/.../settings/DataFolderFeature.kt` |
| Alert and page | `DataFolderViews.swift` (`.dataFolderAlert()` on `AppHomeView`) | `DataFolderScreen.kt` (`DataFolderAlert()` in `Features.Root`) |

Tests: `swift test --filter DataFolderTests` (it works on Linux) and
`./gradlew :core:coreTest -Pfilter=DataFolder`.
