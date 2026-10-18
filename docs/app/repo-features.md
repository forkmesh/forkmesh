# Repository features: issues, pull requests, discussions

A repository administrator (the owner, or an organization owner or admin of a
linked repository, [repo-settings-access.md](repo-settings-access.md)) can
switch Issues, Pull requests and Discussions off in
the repository's Settings tab (the Features group above the ActivityPub
switches).

- **Storage:** `repo_feature_settings` (migration 0270), one row per
  repository, `repo_bi = blind_index("repo-feature-settings:<owner>/<repo>")`
  with both halves lowercased and an organization alias resolved to its node
  (`_repo_features_bi`). `data` is plaintext JSON of booleans, the same trust
  level as `ap_repo_settings`. No row means everything is on.
- **API:** `GET /api/repo/<owner>/<repo>/about` returns `features`;
  `POST .../about` accepts `features` (absent means unchanged) with the same
  owner authorization as the other About fields and answers with the stored
  switches.
- **Enforcement:** while a feature is off, `POST` to `issues_handler`,
  `pulls_handler` or `discussions_handler` answers
  `403 {"error": "feature_disabled", "feature": ...}`, so nothing new is
  opened or commented. Existing records stay readable and come back when the
  feature is switched on. The web page hides the tab
  (`applyRepoFeatures` in `06-repo-content.js`).
- **Repository delete** purges the row with the other settings tables
  (`_delete_repo_named_state`).

## Self-hosted instances

`instance-bootstrap` writes all three switches off for the
`<owner>/forkmesh` mirror the instance creates (`ensure_mirror_features`,
stored under the node name `forkmesh/forkmesh`). It uses `INSERT OR IGNORE`,
so running the bootstrap again never undoes an owner who turned a feature
back on. Instances set up earlier keep their current switches.

Tests: `app/tests/test_repo_features.py`, `app/tests/test_ap_repo_settings.py`,
`app/host/tests_unit/test_instance_bootstrap.py`.
