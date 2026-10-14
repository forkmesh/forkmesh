# People

A plugin that seats every ForkMesh member around a campfire. The most active
sit on the inner rings, closest to the flames; quieter members sit further
out. Picking someone opens a profile card with Follow and Message.

Package: [`marketplace/packages/people`](../../marketplace/packages/people/README.md)
(1.0.0, 2026-10-13). It runs on the desktop, the iPhone, Android and the web.

## Why

On 2026-10-13 the user asked for "a page for the mobile app for users where you
can see their avatars and their name kind of like how the world fire is with
the different circles at different levels of engagement of the user", where
clicking a user shows "their profile information and have the option to follow
them or to message them", "make it look very nice and clearly show what the
delineating sections are", and "make this a plug-in that works on both desktop
mobile and all platforms".

"The world fire" is the World campfire, whose benches ring members by
engagement (`memberEngagementProfile` in
`world/public/world/scene/member-engagement.js`). People reuses its data and
its points, but not its states. The World's rings are an operator view:
"Churned", "Never activated" and "Email unverified 7d+" describe product
problems, and the innermost "World step" ring is about the World itself. A
members page should not label people that way, so People has five plain rings
of its own.

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

Each page has the same three parts:

1. The campfire: a dark square with five coloured rings around an animated
   fire. Avatars sit on their ring, spread around the bottom of the circle, so
   a ring with only a few people still reads as a group by the fire. Each ring
   has a labelled pill at its top with its name and count. When a ring has
   more people than fit, its last seat is a "+N" chip that picks that ring.
   Avatars are 6.6% of the campfire's width (22 to 36 points) so neighbouring
   rings never touch.
2. A legend of ring chips. Picking a chip or a ring label shows only that ring
   and dims the others in the campfire; picking it again shows all of them.
3. One card per ring, with the ring's colour as a bar down its left edge, its
   name, what it means and a count, then everyone in it. Empty rings still
   show their card ("No one in this ring right now"), so the five sections are
   always visible.

A search box filters by name (a leading `@` is ignored). On the desktop the top
search box does the same (`setHostSearchQuery`) and finds people from any page
(`searchHits`).

The profile card has a header in the ring's colour, the avatar, the name, a
"<Ring> ring" badge and when they were last active; then Follow / Following
and Message, followers, following and the join month, status, bio and
location, and "View full profile" (iOS and Android `.profile`, the desktop's
profile dialog, the web's `/@name`). Your own card says "This is you." instead
of the buttons. On the phones, Follow and Message ask a guest to sign in first
(`AccountFeature.requireAccount`).

### Desktop host hooks

The desktop page's API boundary is `/api/accounts` (request proof
`forkmesh-people-request-v1`); it only builds the three paths above
(`PeopleModel.cpp`, checked by `tests/test_people.py`). The host signs with the
account session when there is one, which the follow route requires. Opening a
conversation and a profile needed the window, so `NativePackageHost` gained
`accountName`, `openDirectMessage` and `openUserProfile`
([README](README.md)).

### Phones

Route `.people` / `Route.People`, kind `people` (`WS18`), URL
`/dashboard/people`, in the menu's App group after Organizations
(iOS symbol `person.3`, Android icon `groups`). The package gate hides the menu
entry until the package is installed; the route itself always opens. iOS added
`bedtime_outlined`, `local_fire_department_rounded` and `person_add` to
`MaterialIconCodepoints.swift`.

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
