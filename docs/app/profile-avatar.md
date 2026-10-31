# Profile avatars

Part of [the App docs](README.md). The profile page (`/@name`, which `/<name>`
redirects to), the dashboard's own profile and the header's account avatar
resolve a person's picture the same way issue and pull request avatars do
(`hydrateIssueActorAvatars` in `02-helpers.js`).

## Why

Issue pages draw a person's avatar from their uploaded account picture
(`/api/accounts/<name>`, `issueActorAccount`), else from the open repository's
contributor roster (`.forkmesh/contributors/contributors.json` plus
`.forkmesh/contributors/<slug>.png`, `repoPeopleRosterMatch`). Many people
never upload a picture but are in a roster. A profile that read only the
payload's `avatarPng` (or the session snapshot taken at login) showed them an
initial, and missed an avatar uploaded from another device.

## Resolution (`applyAvatar`, `03-issues-profile-io.js`)

1. `avatarPng` from the payload being shown (the public account, or the
   session for the owner's own pages) is drawn at once.
2. Otherwise the initial is drawn, the image is hidden with no `src`, and
   `hydrateProfileAvatar` looks the account up (`issueActorAccount`, the same
   cached lookup, by the lower-cased name, since display names keep their
   capitalisation), then reads the contributor rosters of up to
   `PROFILE_AVATAR_ROSTER_REPOS` (4) public repositories from the catalog:
   the person's own first, then the parent of each of their forks (a fork
   usually carries an older copy of its parent's roster), then the others in
   catalog order. The first picture found is drawn; one that fails to load
   falls back to the initial.
3. The catalog can arrive after the profile, so `renderProfileRepositories`
   calls `hydrateProfileAvatars`, which retries only when the candidate
   repositories have changed.

A bot or system account with no picture gets its built-in glyph
(`showProfileAvatarBot`, [bot-avatars.md](bot-avatars.md)). Organization
profiles keep their logo. Name links on the same pages are in
[profile-links.md](profile-links.md).

Tests: `app/tests/test_profile_avatar_fallback.py`.
