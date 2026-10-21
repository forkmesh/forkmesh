# iOS → Android sync ledger

New phone features and UI changes land in the iPhone app (`native/ios`) first.
This page is the list of every one of them that the Android app
(`native/android`) still has to match, so the two apps stay the same app: the
same features, and the same look (the iOS Liquid Glass theme, down to sizes,
spacing and motion).

How it works:

1. **Register on iOS.** Whoever ships a user-visible change on iOS (a feature,
   a UI change, or a fix users would notice) adds one row in the same pull
   request, in plain words a user would understand. Internal work (performance
   internals, build fixes, tests, docs) gets no row.
2. **Port on Android.** Android work picks rows that are `pending`, marks them
   `doing` while it works, and builds them to look and behave like iOS.
3. **Close the row.** When the Android change is on `main`, the row becomes
   `synced` with the Android commit. From then on that row is settled: Android
   and iOS match, and the Android code is the reference for that behaviour on
   Android. A row that cannot exist on Android (an Apple-only API such as
   ARKit, MLX or App Store review) becomes `n/a` with the reason.

`tools/native_sync.py` keeps the table honest:

```sh
python3 tools/native_sync.py status                 # counts and every open row
python3 tools/native_sync.py status --area Chat     # one area
python3 tools/native_sync.py untracked              # iOS commits with no row yet
python3 tools/native_sync.py add <ios-sha> "Swipe left on a message for Message Info" --area Chat --kind ui
python3 tools/native_sync.py mark S012 --doing
python3 tools/native_sync.py mark S012 S013 --synced <android-sha>
python3 tools/native_sync.py mark S014 --na "ARKit has no Android equivalent"
python3 tools/native_sync.py triage <ios-sha>       # every iOS commit up to here has a row or needs none
python3 tools/native_sync.py check [--strict]       # validate; --strict also fails on untriaged commits
```

`check` also fails a `synced` row whose Android commit does not resolve in
this repository: a row cites the commit that is on `main`, never a local or
rewritten one. Commit arguments are passed to git after `--end-of-options`.

The triage marker below names the last iOS commit that has been looked at;
`untracked` lists the iOS commits after it that no row cites. A commit
whose only change under `native/ios` is the shared icon font
(`ForkMesh/Resources/Fonts/material_icons_regular.otf`, regenerated whenever
either app starts using a new icon) or Markdown is not an iOS change and is
left out. Rows that
describe a gap older than the ledger (iOS features Android never had) say
`baseline` in the iOS commit column.

Columns:

| Column | Values |
| --- | --- |
| ID | `S001`, `S002`, … never reused |
| Date | the iOS commit date |
| Area | the app area (Chat, Shell, Home, Market, Router, …) |
| Kind | `feature`, `ui` (look and feel only) or `fix` |
| Change on iOS | one sentence, user-facing |
| iOS commit | the commit(s) that shipped it on iOS, or `baseline` |
| Android | `pending`, `doing`, `synced` or `n/a` |
| Android commit or reason | the Android commit for `synced`, the reason for `n/a` |

The ledger starts from merge `3c902443b1` (2026-10-16, "Android: iOS Liquid
Glass look"), the last time Android was brought level with the iOS chrome.
Design tokens both apps share are in [THEMES.md](THEMES.md) and
[PARITY.md](PARITY.md) section 1.1; Android-specific decisions are in
[ANDROID.md](ANDROID.md).

<!-- triaged-through: 0efc715361 -->

## Ledger

<!-- ledger:start -->
| ID | Date | Area | Kind | Change on iOS | iOS commit | Android | Android commit or reason |
|---|---|---|---|---|---|---|---|
| S001 | 2026-10-16 | Theme | ui | Text uses an SF Pro-like typeface and a Menlo-like mono font, with the same sizes, weights and tracking as iOS | baseline | synced | 5ae36b9440 |
| S002 | 2026-10-16 | Theme | ui | Chrome icons (tab bar, top bar, Home, menus) look like SF Symbols, outline when idle and filled when selected; tab glyphs 20, Home glyphs 22 | baseline | synced | acbeba07b6 |
| S003 | 2026-10-16 | Glass | ui | Liquid Glass: clear lens with edge refraction, specular rim, press response and merging pieces, instead of a frosted 55% fill | baseline | synced | 53dc4590c9 |
| S004 | 2026-10-16 | Glass | ui | Every bar is glass over the content behind it, on pushed pages too (back button, segment bars, action bars, composer, chat header) | baseline | synced | 53dc4590c9 |
| S005 | 2026-10-16 | Kit | ui | iOS controls: accent-tinted switches, a pressed wash instead of the Material ripple, inset grouped lists, centred alerts, action sheets with a detached Cancel, popover menus, segmented controls | baseline | synced | 855d20de2d |
| S006 | 2026-10-16 | Navigation | ui | Floating navigation bar with scroll-edge blur and large titles, edge-swipe back with the iOS push motion, glass sheets with medium and large detents | baseline | synced | 44d33eaad8 |
| S007 | 2026-10-16 | Theme | ui | A background picture behind the glass, with a dim slider (0 to 90%, default 40%) | baseline | synced | 90dfba94d4 |
| S008 | 2026-10-16 | Kit | ui | Kit details: glass bottom bar capsule, thin loading bar, copy and send-to-agent icons on notices, white labels on filled buttons in dark mode, 14 pt dialog corners | baseline | synced | b8471aef7d |
| S009 | 2026-10-16 | Theme | ui | Haptics for send, palette, success, warning and error, and spring motion for pushes and tab switches | baseline | synced | 47b5352015 |
| S010 | 2026-10-16 | Home | ui | Home grid: 22 pt icons in 66 pt tiles, 4/5/6 across, Outline/Filled/Accent/Tiles styles, count pills, sort, hide, drag and search | baseline | synced | 88d3444558 |
| S011 | 2026-10-16 | Chat | ui | Direct messages and private groups use iMessage-style bubbles: joined corners, blue sent and grey received, time, expiry ring and receipt inside the bubble | baseline | synced | ef9550665b |
| S012 | 2026-10-18 | Shell | feature | The bottom bar can be customised: Double height (two rows), shape, fill, size, selection mark, folders, drag and sort, with a settings sheet | c674a75011 | synced | 88d3444558 |
| S013 | 2026-10-18 | Shell | ui | The You hub drops down as one small floating card with a status button and small side-by-side account buttons | 9bc65f326b | synced | 71ff862530 |
| S014 | 2026-10-18 | Shell | ui | The avatar shows your picture with a coloured status stroke around the square and the status emoji at its corner | 2a35167c0d | synced | 71ff862530 |
| S015 | 2026-10-17 | Shell | feature | Touch and hold the new-agent button to speak a prompt; it listens until you let go and starts an agent with your last repo and model | 27ece2a6ac 166931a19d | synced | 6bc4dbb50d |
| S016 | 2026-10-18 | Shell | ui | While a shortcut runs, a progress ring fills around the Shortcuts icon in the bottom bar, with the activity halo outside it | 781fb5e04a 1cfb66266f | synced | f54df69cfa |
| S017 | 2026-10-18 | Shell | feature | Badges: open pull requests on the Pull requests icon on every page, new pull requests appear at once, unread counts on RSS Reader and assigned Tasks, new reported bugs on the Bug icon | a77de75505 bbae74adbf b5a3eaea71 | synced | 4bc152afac |
| S018 | 2026-10-18 | Home | feature | Plugin icons on Home show a dot when updated or never opened since install; long-press the background to clear all dots or show them again | 07cfdd597b 1cab6906cf 11580ffda1 | synced | 6473f4a50e |
| S019 | 2026-10-18 | Home | fix | The repository icons on Home follow the repository picked in the composer | 01db24796b | synced | 5858a39e33 |
| S020 | 2026-10-16 | Shell | ui | A thin accent loading bar shows on slow loads, and the Agents list's first load is not thrown away when the account socket rings | 34d6c89ca0 | synced | 5858a39e33 |
| S021 | 2026-10-20 | Shell | ui | A slow blinking dot on desktop-dependent plugins while the desktop is offline | 19bbfb04ed | synced | 6746b35f9f |
| S022 | 2026-10-19 | Chat | feature | Swipe left on a message for Message Info; delivered and read times are kept, and "Delivered 11:04 PM" / "Read 11:06 PM" shows under your latest sent message | d0e2129595 fef6617730 ce786c636e | synced | 3ccb1d91c2 |
| S023 | 2026-10-18 | Chat | feature | Live touch in direct messages: tap to buzz, drawing with a toolbar in the header, the partner's live 3D phone that follows their tilt, and real online presence | f105dde9e1 0171705c75 c49b247867 1525873546 63c2e5033a | synced | 2cb1a0cc47 |
| S024 | 2026-10-18 | Chat | fix | Chat history is kept per project and room, survives updates, saves office channels too, and never overwrites a file it cannot read | f83279993b | synced | 80a362e7b9 |
| S025 | 2026-10-16 | Chat | fix | A notification tap that opens a conversation closes the channel drawer, pages and sheets over it | cb79f4719b | synced | 8d47e84a23 |
| S026 | 2026-10-16 | Chat | feature | Conversation header shows the route label; own-device notices and three distinct safety-number scan errors | ee89323637 | synced | fc0e493716 |
| S027 | 2026-10-17 | People | fix | Message on a People profile opens that account's direct message | 3987a8c21b | synced | 813baf4190 |
| S028 | 2026-10-18 | Composer | feature | The prompt bar's model menu also offers Bug report, Task with an assignee, Create issue, Post and Note | 8fe216625b | synced | 0718b455f1 |
| S029 | 2026-10-17 | Market | ui | The Market puts back, search, sort and filter on one slim navigation-bar line | 40996d9d78 | synced | 6140ab4e3e |
| S030 | 2026-10-17 | Inbox | ui | Inbox filters and menu actions are one centred bar of small icon squares with counts | 480b201969 | synced | f0cda9a3cf |
| S031 | 2026-10-16 | RSS | ui | The article screen shows the timeline's unread count beside the back button | 727cb0ea4e | synced | d42412c83e |
| S032 | 2026-10-18 | Capture | feature | Long-press any blank area to select part of the screen or the whole screen, mark it up and copy it | f9b2722fcb | synced | a0d165cf31 |
| S033 | 2026-10-18 | Vault | feature | Import passwords from Apple Passwords, 1Password, Chrome, Firefox, Bitwarden and other CSV exports | bd6f2eff97 | synced | 77513981c2 |
| S034 | 2026-10-19 | CRM | feature | The CRM shows Zoho Mail status with missing steps and one-tap Reconnect; the reconnect form remembers the client ID and data centre and says the secret is saved | 2dcae104e0 d691cab86b | synced | 04312fc970 |
| S035 | 2026-10-18 | App Store Status | fix | When the server's snapshot predates tester screenshots, the phone asks the paired desktop for a fresh one | b076fa42d3 | synced | 4bf9706f24 |
| S036 | 2026-10-17 | Tools | fix | Reported bugs: a cancelled load is not an error, and an older relay says to update it | 7564804e42 | synced | d9706a85a4 |
| S037 | 2026-10-18 | Face Changer | fix | An empty recording says "Nothing was recorded. Record for a little longer, then stop." | a4dbcbb101 | synced | 564cde2652 |
| S038 | 2026-10-19 | Audio | fix | During another app's call, no interface sounds play and ForkMesh mixes instead of ducking | a5066ceef3 | synced | 8c76d9ecb5 |
| S039 | 2026-10-18 | Projects | fix | A new instance added without a name is saved under the instance's own name, not its host | 209c231722 | synced | e98d70e975 |
| S040 | 2026-10-18 | Planning | fix | Calendar saves never lose a new event or bring back a deleted one when an older list arrives late | f83279993b | synced | 3bdf63d1a5 |
| S041 | 2026-10-19 | Instance Feed | feature | Instance Feed routes and feed models on the phone (Android also needs the screen the route opens) | aed0eb24aa | synced | 855cb388e1 |
| S042 | 2026-10-16 | Agents | feature | The Eagle view of agents, where each row keeps its last line and only running agents scroll | 1249eb6e4c | synced | ee2f08fab1 |
| S043 | 2026-10-17 | Diagnostics | fix | Diagnostics reports list stalls by name and screen in their own section | 11fe00dbcf 5640007bfc | synced | 312740ec59 |
| S044 | 2026-10-18 | Code | ui | Diff toolbar buttons are separate 30 pt glass squares with 17 pt icons; hold one for its name | 4abec31fba 9f94acfccb d30d377967 | synced | c1743055b5 |
| S045 | 2026-10-18 | DJ Studio | feature | Video Mix DJ Studio as a native two-turntable deck: tempo, crossfade, scratch, queue, AUTOMIX, playlists and tags, BPM, jam sessions, background play, top-bar record chip | ba8f1e9f16 cb7b757230 0b45817615 a7acffc3a9 eddefe58dc 0965e59a7f 2b810cf988 4e7173979f 8a06b1c50d fa0323b5ce a3b81ce35a f2812ad9c6 0c631f3ff9 e67dddfe7f 9662594de2 376759c3ee 90ba74af87 bd018e6df5 ac3e470a4c | synced | 7bb033434c |
| S046 | 2026-10-17 | NEXUS | feature | NEXUS network inspection package: port scan, DNS, service discovery, HTTP/TLS inspection, history and reports | 58ae4e504f | synced | 94445ac329 |
| S047 | 2026-10-18 | Genie | feature | The Genie assistant and spider, including badge cheers, tilt, the top-bar mail chip and Zoho auto-send outreach | da641ed9a5 2319221d13 64807bab53 d45b486561 99eb104207 | synced | 598418db68 |
| S048 | 2026-10-16 | Tools | feature | Health and wellbeing tools: Steps, Sleep, Water, Stretch, Sugar Break and Food Guide | baseline | synced | 8c1142e039 |
| S049 | 2026-10-16 | Aquarium | feature | The Aquarium screen | baseline | synced | 42df06b6eb |
| S050 | 2026-10-18 | Eye Control | feature | Eye Control: a gaze cursor that snaps to buttons and taps on a blink | 095de8852d df26aeddeb | n/a | Needs ARKit TrueDepth face tracking; Android has no equivalent |
| S051 | 2026-10-18 | Spider Room | feature | Spider Room: the Genie spider in your room in AR | 52fa2a9ba8 | n/a | ARKit and RealityKit scene; revisit with ARCore only if wanted |
| S052 | 2026-10-16 | Shell | feature | The app is blocked until it is updated to the App Store's newest version | 059bb8bd94 | n/a | App Store lookup; Android is directly distributed and has its own update notice |
| S053 | 2026-10-16 | LocalAI | feature | On-device Gemma through MLX | baseline | n/a | MLX runs on Apple silicon only |
| S054 | 2026-10-16 | Shell | fix | The Back button stays on ping pages opened from a notification | acc294a715 | n/a | UIKit navigation bar bug; Android ping pages draw their own back button |
| S055 | 2026-10-17 | People | fix | People keeps its search box in the navigation bar and loads the directory again | a6fd54488b c9004c97c7 | n/a | iOS searchable placement and URL bugs; Android never had them |
| S056 | 2026-10-16 | Theme | ui | The app icon is the white cube on the dark gradient tile, filling the launcher shape | baseline | synced | 750c8fbee2 |
| S057 | 2026-10-16 | Shell | feature | The You hub has a Pocket mode row that locks the screen to Glasses Alert, pings and walking | baseline | synced | e738e2d21f |
| S058 | 2026-10-16 | Genie | feature | The You hub has a Suggestion mode switch for the Genie's suggestions | baseline | synced | 598418db68 |
| S059 | 2026-10-20 | CRM | feature | In the CRM, each email shows its opens, clicks, read time, scroll depth, slide reach and the reader's mail app | f949c51083 c1869c71bb dc6e17fc09 | pending |  |
| S060 | 2026-10-20 | Runners | feature | The Runners screen lists runner accounts and their sign-in state | 3222755e03 | synced | 0b14ef1da7 |
| S061 | 2026-10-20 | Usage | ui | Usage re-reads every account on Refresh and shows the clock time each limit resets | e0d7e0d7e0 | pending |  |
| S062 | 2026-10-19 | Account | feature | You can keep a copy of the app's data in a folder you pick, with projects kept as project/data folders | 0f47d42761 cf611dc5c4 c7c7c20118 ee9e61002f | synced | 0f47d42761 |
| S063 | 2026-10-19 | DJ Studio | feature | DJ Studio's AUTO DJ plays a related song people liked on the next deck, originals first, and a shake loads a new pick | c29dedba06 4258be9411 f03ffa723f e41a94a7e9 | pending |  |
| S064 | 2026-10-19 | Feed | feature | The Feed package has native screens and is called just Feed | 9ac8f02944 | synced | 9ac8f02944 |
| S065 | 2026-10-19 | Home | ui | Home keeps its size while loading, shows a spinner on each loading icon, and Refresh reloads everything | 931f5bb99f | pending |  |
| S066 | 2026-10-19 | Genie | feature | Auto-send can ask the paired desktop's Claude Code or Codex to write the email | 9445548507 | pending |  |
| S067 | 2026-10-19 | NEXUS | feature | NEXUS has a link in the You hub's Workspace group and detects the scan range from the phone's IP address | f9db4247cf ded23e8d5a | pending |  |
| S068 | 2026-10-20 | Projects | feature | New project loads the checked instance's logo and can quick-add from the ForkMesh directory | 73d80cbfb6 | pending |  |
| S069 | 2026-10-19 | Router | fix | Router network sharing creates a missing share config so a new share shows up | 3330f9588b be89626f7f | synced | be89626f7f |
| S070 | 2026-10-20 | Chat | feature | Usernames and @mentions in chat, issues, pull requests and discussions open that person's ForkMesh profile when tapped | 079280627a | synced | 079280627a |
| S071 | 2026-10-21 | Kit | fix | Every text field strips hidden Unicode (zero-width, bidi override, tag and smuggling characters) as you type or paste; passwords stay exact | 1c703806a7 | synced | 1c703806a7 |
| S072 | 2026-10-21 | Projects | fix | Switching projects no longer shows another project's chat channels, private rooms or direct messages | 2b6c130a28 | n/a | Android rebuilds its chat services per project (ProjectLifecycle.close, Features.install), so it never had this leak |
| S073 | 2026-10-21 | Projects | fix | Adding an instance that another project already uses opens that project instead of creating a second one | 4beeb217c2 | synced | 68c7fcc9e7 |
<!-- ledger:end -->
