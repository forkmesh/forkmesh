# Profile avatars

The profile page (`/@name`, which `/<name>` redirects to), the dashboard's own
profile and the account avatar in the header resolve a person's picture the
same way issue and pull request avatars do (`hydrateIssueActorAvatars` in
`02-helpers.js`).

## Why

Issue pages and lists draw a person's avatar from their uploaded account
picture (`/api/accounts/<name>`, `issueActorAccount`), and when there is none,
from the open repository's contributor roster
(`.forkmesh/contributors/contributors.json` plus
`.forkmesh/contributors/<slug>.png`, `repoPeopleRosterMatch`). Many people on
an instance never upload a picture but are in a roster, so they had a picture
next to their issues. The profile only looked at the `avatarPng` of the
account payload (or of the signed-in session, a snapshot taken at login), so
the same person showed only an initial on their own profile, and an avatar
uploaded from another device never reached the signed-in owner's own profile.

## Resolution (`applyAvatar`, `03-issues-profile-io.js`)

1. `avatarPng` from the payload being shown (the public account, or the
   session for the owner's own pages) is drawn at once.
2. Otherwise the initial is drawn, the image is hidden with no `src`, and
   `hydrateProfileAvatar` looks the account up (`issueActorAccount`, the same
   cached lookup, by the lower-cased name, since display names keep their
   capitalisation) and then the contributor rosters of up to
   `PROFILE_AVATAR_ROSTER_REPOS` (3) public repositories from the catalog:
   the person's own first, then the others in catalog order. The first
   picture found is drawn; a picture that fails to load falls back to the
   initial.
3. The catalog can arrive after the profile, so `renderProfileRepositories`
   calls `hydrateProfileAvatars`, which retries only when the candidate
   repositories have changed.

A bot or system account with no picture gets its built-in glyph
(`showProfileAvatarBot`, [bot-avatars.md](bot-avatars.md)). Organization
profiles keep their logo. Tests:
`app/tests/test_profile_avatar_fallback.py`.

A fork usually carries an older copy of its parent's roster, so the parent of each of the person's own forks is read right after their own repositories (up to `PROFILE_AVATAR_ROSTER_REPOS`, 4, repositories in all).
