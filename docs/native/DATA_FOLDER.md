# The data folder on iOS and Android

The phone keeps its stores inside the app (Keychain / Keystore, preferences,
Documents and Application Support / `filesDir`), and the phone deletes all of it
with the app. A fresh install even erases a Keychain the old app left behind
(`FreshInstallReset`). So the phone also keeps a copy of its data in a folder
the user picks, outside the app. That folder has the same layout as the
desktop's central store ([projects-layout.md](../tools/projects-layout.md)):

```text
<picked folder>/forkmesh_data/          (the picked folder itself if it is named forkmesh_data)
  forkmesh-data.json                    marker: type forkmesh.data-folder, created/updated, platform
  forkmesh/                             the first (legacy) project, like the desktop's home project
    project.json                        slug, projectId, namespace, name, public key
    data/identity/recovery-kit.json     the identity key, encrypted with a passphrase
    data/documents/…  data/support/…    copies of the project's Documents and Application Support / filesDir
    config/ForkMesh/settings.json       every preference except device-only keys
    state/sync-index.json               size:mtime of each file at its last copy
    cache/ logs/ ssh/                   created for parity with the desktop; empty on a phone
  <project-slug>/                       every other project (ProjectScope.slug), same shape
```

## Behaviour

- **Alert until it is set up.** At every launch the app shows *Keep your
  ForkMesh data safe* until a folder is chosen, the folder can be opened, and
  the active project's identity key has been saved there for its current public
  key. *Not now* hides it until the next launch. The same page opens from
  Settings: iOS has a *Data folder* row in *Data and security*; Android has a
  card on Settings → Data. On iOS the alert sits on the root view, so it also
  shows before sign-in. On Android it is drawn outside `AccountFeature.Gate`
  and opens its own full-screen page, so a fresh install can restore before
  signing in.
- **Identity key.** The seed is never written in clear: the folder holds the
  same encrypted container as a recovery kit (`RecoveryKit.export`, at least
  12 characters of passphrase). `dataFolder/kitKey/<slug>` records which public
  key was saved, so a rotated key asks again.
- **Copying.** On launch, and whenever the app goes to the background (iOS
  scene phase `.background`, inside a background task; Android `onPause`), the
  active project's settings, `project.json` and changed files are copied. A file
  is copied when its `size:mtime` differs from the index. `LocalModels`,
  `CodeInsight`, `.Trash` and `tmp` are skipped. So is the legacy project's
  `projects/` folder, which holds the other projects. Nothing in the folder is
  ever deleted.
- **Settings.** iOS writes `forkmesh.mobile-settings` (typed JSON values from
  the `fm.`-prefixed UserDefaults). Android writes `forkmesh.android-settings`
  (raw SharedPreferences values tagged with their type, so double-as-long-bits
  and string sets survive). Keys under `dataFolder/` and `install/` stay on the
  device. The settings live in the `forkmesh/` folder because most preference
  keys are app-wide (`ProjectKeyPolicy`).
- **Restore.** Picking a folder that already holds projects with a saved
  identity key that differs from this install's offers *Restore* (one
  passphrase for all projects) or *Keep this install's data instead*.
  - Restore writes each seed into `<namespace>identity/ed25519Seed`, applies
    the settings, and copies back only the files missing locally.
  - Then it asks for a restart: iOS *Close ForkMesh*, Android *Restart
    ForkMesh*. The services read identity and preferences at launch.
- **Where.** iOS refuses a folder inside the app container. Any folder in
  Files works, including On My iPhone and iCloud Drive (a plain file copy, not
  a Git checkout, so iCloud is fine here; the repositories folder still refuses
  iCloud). Android uses a Storage Access Framework tree
  (`OpenDocumentTree`, persisted read/write permission), for example Documents
  on the phone.

## Code

| Piece | iOS | Android |
| --- | --- | --- |
| Layout, settings codec, copy, kit | `ForkMeshCore/Sources/FMAccount/DataFolder.swift` | `core/.../settings/DataFolder.kt` |
| Kit opening | `RecoveryKit.openSeed` | `RecoveryKit.openSeed` |
| Folder access | security-scoped bookmark (`dataFolder/location`) | `SafDataFolderIo` over the tree URI (`dataFolder/location`) |
| State and actions | `ForkMesh/Features/Account/Data/DataFolderModel.swift` | `app/.../settings/DataFolderFeature.kt` |
| Alert and page | `DataFolderViews.swift` (`.dataFolderAlert()` on `AppHomeView`) | `DataFolderScreen.kt` (`DataFolderAlert()` in `Features.Root`) |

Tests: `swift test --filter DataFolderTests` (Linux works) and
`./gradlew :core:coreTest -Pfilter=DataFolder`.
