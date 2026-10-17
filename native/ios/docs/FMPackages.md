# FMPackages

The organization packages (CRM, Marketing, Analytics, Competitors) and the Market. `import
FMPackages` (re-import `FMBase` for `JSONObject`). The SwiftUI views are in
`ForkMesh/Features/Packages`; tests are in `ForkMeshCore/Tests/FMPackagesTests`.

## Marketing Reddit desk

- `MarketingRedditSection` (`Features/Packages/MarketingSections.swift`) lists the team's Reddit
  accounts under "Your accounts" (`MarketingOverview.redditAccounts`, parsed from
  `redditDesk.accounts`; each row links u/name to its profile URL, or reads "No Reddit accounts
  yet."). They are edited in the `.redditWatch` form's Accounts field (one per line, up to 10; a
  name, `u/name` or a profile link), sent as `reddit.accounts` in the same settings PUT as
  subreddits and searches. More than 10 is refused before the request; the server's
  `invalid_reddit_account` and `too_many_reddit_accounts` map to messages in `PackagesError`.
- Each thread row has a caption from `MarketingRecord.redditActivity(now:)`
  (`MarketingWorkspace.swift`, kept in core so it is unit tested): comments, points and views of
  the post; "your comment: N views, N points" when a comment is watched (`monitorCommentUrl`);
  "views show only to u/…" (`commentAuthor`, else `author`) when the thread was read but Reddit
  hid every view count; and "read 2h ago" from `checkedAt`, followed by "as u/…" (`readAs`, the
  account the browser extension was signed in as), or "not read yet". Points and comments moved from the
  row's facts to this caption so they are not shown twice. Same rules as Android
  ([docs/native/ANDROID.md](../../../docs/native/ANDROID.md), Marketing Reddit desk).
