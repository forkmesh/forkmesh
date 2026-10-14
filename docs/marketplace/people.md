# People

A plugin that seats every ForkMesh member around a campfire. The most active
sit on the inner rings, closest to the flames; quieter members sit further
out. Picking someone opens a profile card with Follow and Message.

Package: [`marketplace/packages/people`](../../marketplace/packages/people/README.md).
It runs on the desktop, the iPhone, Android and the web.

The rings reuse the World campfire's engagement data and points
(`memberEngagementProfile` in
`world/public/world/scene/member-engagement.js`) but not its operator-facing
states; People has five plain rings of its own.

## The rings

Every member gets a score from 0 to 100, the World's score without the live
presence inputs the public directory does not carry (`visitCount`,
`activeNow`):

- recency, from `activityBucket`: 48 within an hour, 46 within five hours, 42
  within a day, then 36, 32, 25, 18, 11 and 5 for 3, 5, 10, 30, 60 and 120
  days;
- depth: World active time (up to 22), World steps (up to 13), Office visits
  (up to 6) and contributions (`floor(log2(n + 1) * 3.5)`, up to 14);
- readiness: verified email 4, a payout address 4, linked nodes 3 each up to 7.

The ring, checked in this order:

| Ring | Rule | Colour |
| --- | --- | --- |
| Blazing | active within a day and a score of 70 or more | `#ff6b2b` |
| Glowing | active within five days | `#ffa43a` |
| Kindling | joined less than 14 days ago | `#f4cf6b` |
| Warm | active within 30 days, or any contributions, World steps or Office visits | `#5fc28e` |
| Embers | everyone else | `#7ea6c4` |

Inside a ring people sort by score, then active time, then name.

Node accounts (`kind: "node"`) are left out; the directory already leaves out
private and inactive accounts.

## Data

All four apps call the same routes:

- `GET /api/accounts/users?view=world`: the public member directory the World
  loads (edge-cached for 30 s). Each row carries `avatarThumb`, a 128 px WebP
  data URL, which every client decodes in place, so the page makes one request
  however many avatars it shows.
- `GET /api/accounts/<name>?viewer=<me>`: the profile card (bio, location,
  joined, follower counts, `isFollowing`, status). `viewer` only fills
  `isFollowing`.
- `POST` / `DELETE /api/accounts/<name>/follow`: follow and unfollow. It needs
  an account session; a 403 means the email is not verified yet, and each app
  says so.
- Message opens the existing private conversation route: iOS `.dm(user:)`,
  Android `Route.Dm`, the desktop's `startDirectMessageWith`, the web's
  `/chat?dm=<name>`. People adds no server code.

## Where the code lives

| Platform | Logic | Screen |
| --- | --- | --- |
| Web | `content/web/page.js` (`window.ForkMeshPeopleRings`) | `content/web/view.html`, `page.js` |
| Desktop | `content/desktop/PeopleModel.{h,cpp}` | `content/desktop/PeopleFeature.{h,cpp}` (`builtin:people`) |
| iPhone | `native/ios/ForkMeshCore/Sources/FMAccount/Identity/People.swift` | `native/ios/ForkMesh/Features/People/PeopleScreen.swift` |
| Android | `native/android/core/.../settings/People.kt` | `native/android/app/.../feature/people/PeopleScreen.kt` |

Each page shows the campfire (five coloured rings around an animated fire,
avatars seated on their ring, a labelled pill with name and count per ring,
a "+N" chip when a ring overflows), a legend of ring chips that filters to
one ring, and one card per ring listing everyone in it (empty rings still
show their card). A search box filters by name; on the desktop the top search
box does the same (`setHostSearchQuery`, `searchHits`).

The profile card shows the ring, last activity, Follow / Following and
Message, follower counts, join month, status, bio and location, and "View
full profile". Your own card has no buttons. On the phones, Follow and
Message ask a guest to sign in first (`AccountFeature.requireAccount`).

The desktop page's API boundary is `/api/accounts` (request proof
`forkmesh-people-request-v1`), using the host's `accountName`,
`openDirectMessage` and `openUserProfile`. On the phones the route is
`.people` / `Route.People` (`/dashboard/people`), hidden from the menu until
the package is installed.

## One set of rules, four implementations

`marketplace/packages/people/tests/fixtures/rings.json` holds raw directory
rows with the expected parse, score and ring, the expected order inside each
ring, and ring radii and capacities at two sizes. Every implementation is
tested against it:

- web: `marketplace/packages/people/tests/test_people.py` runs `page.js` under
  Node;
- desktop: `content/desktop/tests/test_people_model.cpp`
  (`forkmesh-people-tests`);
- iPhone: `PeopleRingsTests` in `FMAccountTests`
  (`FM_ONLY=FMAccount swift test --filter People` on Linux);
- Android: `PeopleTest` in `:core:coreTest`.

A change to the rules changes `page.js` first; regenerate the fixture from it
(`node marketplace/packages/people/tests/make_fixture.js
marketplace/packages/people/content/web/page.js
marketplace/packages/people/tests/fixtures/rings.json`), then make the other
three pass. The phones and the desktop also test loading,
de-duplication, follow and unfollow, the self and signed-out cases, and the
unverified-email message.
