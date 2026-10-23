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
| S059 | 2026-10-20 | CRM | feature | In the CRM, each email shows its opens, clicks, read time, scroll depth, slide reach and the reader's mail app | f949c51083 c1869c71bb dc6e17fc09 | synced | f2e754f726 |
| S060 | 2026-10-20 | Runners | feature | The Runners screen lists runner accounts and their sign-in state | 3222755e03 | synced | 0b14ef1da7 |
| S061 | 2026-10-20 | Usage | ui | Usage re-reads every account on Refresh and shows the clock time each limit resets | e0d7e0d7e0 | synced | c0ee86dab1 |
| S062 | 2026-10-19 | Account | feature | You can keep a copy of the app's data in a folder you pick, with projects kept as project/data folders | 0f47d42761 cf611dc5c4 c7c7c20118 ee9e61002f | synced | 0f47d42761 |
| S063 | 2026-10-19 | DJ Studio | feature | DJ Studio's AUTO DJ plays a related song people liked on the next deck, originals first, and a shake loads a new pick | c29dedba06 4258be9411 f03ffa723f e41a94a7e9 | synced | d3896918c7 |
| S064 | 2026-10-19 | Feed | feature | The Feed package has native screens and is called just Feed | 9ac8f02944 | synced | 9ac8f02944 |
| S065 | 2026-10-19 | Home | ui | Home keeps its size while loading, shows a spinner on each loading icon, and Refresh reloads everything | 931f5bb99f | synced | 099513c76f |
| S066 | 2026-10-19 | Genie | feature | Auto-send can ask the paired desktop's Claude Code or Codex to write the email | 9445548507 | synced | c70a2988cf |
| S067 | 2026-10-19 | NEXUS | feature | NEXUS has a link in the You hub's Workspace group and detects the scan range from the phone's IP address | f9db4247cf ded23e8d5a | synced | 62f90e0718 |
| S068 | 2026-10-20 | Projects | feature | New project loads the checked instance's logo and can quick-add from the ForkMesh directory | 73d80cbfb6 | synced | 999a723041 |
| S069 | 2026-10-19 | Router | fix | Router network sharing creates a missing share config so a new share shows up | 3330f9588b be89626f7f | synced | be89626f7f |
| S070 | 2026-10-20 | Chat | feature | Usernames and @mentions in chat, issues, pull requests and discussions open that person's ForkMesh profile when tapped | 079280627a | synced | 079280627a |
| S071 | 2026-10-21 | Kit | fix | Every text field strips hidden Unicode (zero-width, bidi override, tag and smuggling characters) as you type or paste; passwords stay exact | 1c703806a7 | synced | 1c703806a7 |
| S072 | 2026-10-21 | Projects | fix | Switching projects no longer shows another project's chat channels, private rooms or direct messages | 2b6c130a28 | n/a | Android rebuilds its chat services per project (ProjectLifecycle.close, Features.install), so it never had this leak |
| S073 | 2026-10-21 | Projects | fix | Adding an instance that another project already uses opens that project instead of creating a second one | 4beeb217c2 | synced | 68c7fcc9e7 |
| S074 | 2026-10-21 | Agents | fix | The first action on an agent's page is no longer cut off at the left of the top bar. | d070b14226 | synced | df87f5816b |
| S075 | 2026-10-21 | Agents | feature | Scrolling up in an agent's transcript keeps your place while it works, and an arrow above the composer takes you back to the latest output. | 74c29b740e | synced | 74c29b740e |
| S076 | 2026-10-20 | Notifications | fix | Notification links to a pull request open it at its new /pull/<number> address. | 8d87f5e467 | synced | 8d87f5e467 |
| S077 | 2026-10-21 | Notifications | feature | Pings say when someone reacted to your comment, with a reaction icon. | 8b43090ad3 | synced | 8b43090ad3 |
| S078 | 2026-10-21 | Pings | ui | Ping banners lead with who acted and where, with what they wrote straight under it, instead of a generic event label | 26a6431321 | synced | 30aff95785 |
| S079 | 2026-10-21 | Agents | feature | Every agent page pins the last thing you wrote to the agent above its transcript; pull the card down to flip back through earlier prompts. | cd58bc31c2 | synced | 9a886bb05c |
| S080 | 2026-10-21 | Code | feature | Mirror import accepts gitea.com repositories alongside GitHub, GitLab and Codeberg. | c8852efb65 | synced | c8852efb65 |
| S081 | 2026-10-21 | Settings | ui | What's new shows the same notes as forkmesh.com/changelog, with each entry's title and summary. | 43557597fd | synced | 43557597fd |
| S082 | 2026-10-21 | Chat | fix | A call now rings the other phone even when it is not on that chat or the app is in the background | 320fe7221a | synced | 320fe7221a |
| S083 | 2026-10-21 | Chat | feature | Incoming calls show on the iPhone's own call screen, also on the lock screen and when the app was closed | 5533f4d47f | n/a | Android has no CallKit or PushKit; its call notification already uses CallStyle |
| S084 | 2026-10-21 | Chat | feature | Private chats can hold encrypted group audio and video calls, like Google Meet or Discord, from a Call button in the chat header | 042d1ed488 | pending |  |
| S085 | 2026-10-21 | Chat | feature | Phone calls run on the native call engine with the voice changer and call backgrounds, instead of the web view | d5ab476f9e | pending |  |
| S086 | 2026-10-21 | Chat | feature | The face changer works in native calls: a chosen stand-in or custom face is drawn over the caller's face live | 28b18d33b9 | pending |  |
| S087 | 2026-10-21 | Chat | feature | Face changer in calls tracks the face in 3D on Face ID iPhones, keeps real hair around the new face and has cleaner edges than the browser | 9b199aef1b | pending |  |
| S088 | 2026-10-21 | Chat | fix | Calls still ring an iPhone that has notifications turned off, and calling again right after a decline rings straight away. | 5f0db42ebf | pending |  |
| S089 | 2026-10-21 | Calls | fix | Calls you place to an iPhone show its camera and play its voice once it answers. | 478fbe6a9c | n/a | Android calls run in the WebView page, which already adds its tracks with addTrack |
| S090 | 2026-10-21 | Calls | ui | Call buttons move to two rows when one row does not fit the screen. | 478fbe6a9c | pending |  |
| S091 | 2026-10-21 | Calls | feature | Drag your own video in a call to any corner and it stays there for the next call. | 478fbe6a9c | pending |  |
| S092 | 2026-10-21 | Inbox | feature | Review and merge-conflict pings on your own pull requests have their own icon, label, settings switch and open the pull request | ff273c2c19 | synced | ff273c2c19 |
| S093 | 2026-10-22 | Shell | feature | With Status Page installed, tiny status dots sit in the thin strip above the Dynamic Island on every screen. | 360acccbd4 | synced | 099513c76f |
| S094 | 2026-10-21 | Chat | fix | The phone leaves private and hidden channels out of its relay hello and does not adopt channels it has hidden, so private channel names stay out of other projects and devices | fe664f0c09 | synced | 3d7fccfeed |
| S095 | 2026-10-21 | Chat | fix | Chat stays snappy: a quick inactive step (Control Center, an alert) does not reconnect direct message rooms or rerun the chat refresh, and chat screens redraw less | fe664f0c09 | pending |  |
| S096 | 2026-10-22 | Chat | fix | Frames addressed to one person (DM edits, reactions, typing, deletes, invites) are sealed to that person only, and private-channel frames for channels the phone is not in are dropped | 3d7fccfeed | synced | 3d7fccfeed |
| S097 | 2026-10-22 | Chat | fix | Inbox chat pings are kept per project, removing a project erases its chat history, and every diagnostics report is scrubbed | 3d7fccfeed | pending |  |
| S098 | 2026-10-22 | Chat | feature | Chat relay hardening: DMs and private channels only through the server, signed device keys, sealed person frames, verified account names, server-kept blocks and webhook rooms marked server-visible | 440e84220c | synced | 440e84220c |
| S099 | 2026-10-22 | Chat | fix | #mesh carries no account name, uses a random beacon id and says anyone nearby can read it; less identity in shared rooms | 440e84220c | synced | 440e84220c |
| S100 | 2026-10-22 | Chat | ui | Link previews in shared rooms load on tap and never reach private addresses | 440e84220c | n/a | Android has no link previews |
| S101 | 2026-10-22 | Settings | feature | Settings > Data saves the home screen layout, news reader subscriptions and app settings to your ForkMesh account, encrypted, and lists everything ForkMesh keeps about you with a full download and per-item delete. | 306a20abda | synced | be58a0a3cd |
| S102 | 2026-10-22 | Agents | fix | The pinned last prompt no longer leaves an empty box under it, and swiping it left and right moves through your earlier and later prompts. | 6f57eaf518 | synced | 9a886bb05c |
| S103 | 2026-10-22 | Calls | feature | Calls show a small live quality pill with signal bars, a voice waveform and the delay; tap it for live network and video charts. | 6740cdc214 | synced | 6740cdc214 |
| S104 | 2026-10-22 | DJ Studio | feature | DJ Studio decks have a Repeat button that plays the song on that deck over and over until it is turned off. | c191462a0a | synced | d3896918c7 |
| S105 | 2026-10-22 | Home | ui | On Home, the number on each icon sits beside the icon instead of covering the name of the icon above, and the update dot is smaller. | 35c4317e51 | synced | 099513c76f |
| S106 | 2026-10-22 | Agents | ui | The terminal's keys fit on screen in two rows, a Ctrl key sends any Ctrl combination, and scrolling up to read earlier output no longer jumps back to the bottom. | ae5cba204b | synced | 9877d060b1 |
| S107 | 2026-10-22 | Code | fix | Merge all with AI on a pull request list now reaches the desktop's copy of the repository instead of saying the desktop does not have it. | c17892ba63 | synced | c17892ba63 |
| S108 | 2026-10-22 | Steps | fix | A walk shows in the Dynamic Island and on the Lock Screen once you have been walking with the app open, and stays when you lock the phone mid-walk. | 88396d68c9 | synced | a02639ecbd |
| S109 | 2026-10-22 | DJ Studio | fix | DJ Studio's Repeat now really loops the song and jumps straight from where the sound ends back to where it starts, skipping silence at both ends. | ab3a63d995 | synced | d3896918c7 |
| S110 | 2026-10-22 | Settings | feature | Settings > Appearance can match the instance's theme, so the app wears the colours the instance's admin picked. | 180d647034 | synced | 2ca5862405 |
| S111 | 2026-10-22 | Chat | fix | Face filters follow your head with less lag and your lips move with your speech. | d62d4aabc9 | synced | d62d4aabc9 |
| S112 | 2026-10-22 | Agents | ui | Agents Eagle rows drop the left agent dot and show an image tile with a green and red churn border, a file count and status dot pill, and blinking background-task dots. | 40906210d3 | synced | 0416f062e3 |
| S113 | 2026-10-23 | Settings | fix | With Diagnostics on, the status dots show above the Dynamic Island and the desktop and node lights sit just under the clock. | 977c57037b | pending |  |
| S114 | 2026-10-23 | Chat | fix | A chat line shown as Guest switches to the sender's account name once the server confirms their device. | 8215287d99 | pending |  |
| S115 | 2026-10-23 | Settings | ui | With Diagnostics on, the desktop and node lights sit above the clock with more room and the diagnostic charts sit filled above the battery. | 0eff45f617 | pending |  |
| S116 | 2026-10-23 | Agents | ui | The green and red churn ring around an agent's tile in the agent list turns while the agent is working and stops still when it is done. | d8dffb957e | pending |  |
| S117 | 2026-10-23 | Market | feature | Each Feed post has a share button to view it on the website or copy its public link. | 98e35b20f3 | synced | 6a733017ec |
| S118 | 2026-10-23 | Agents | feature | When a merge is blocked by uncommitted edits on the desktop, a Set aside and merge button saves them on a wip/ branch, merges, and puts them back. | 07bb2e29c5 | pending |  |
| S119 | 2026-10-23 | Settings | ui | With Diagnostics on, the core node and desktop lights show tiny CPU, memory and disk bars, sit further from the corner, and the desktop charts left the top right. | a345e3890b | pending |  |
| S120 | 2026-10-23 | DJ Studio | feature | DJ Studio's queue opens its menu (with Remove from Queue) when a song is held, drags a song onto a deck as soon as the finger moves across the queue, and plays a song added while nothing is playing right away. | fcd3936166 | pending |  |
| S121 | 2026-10-23 | Chat | fix | When several new devices join a private chat at once, one banner asks to approve or block all of them instead of one banner per device. | a0c1f528fa | pending |  |
| S122 | 2026-10-23 | Inbox | ui | Each ping in the Inbox shows the logo of the instance it came from, under the ping's icon. | 9d95e08a10 | pending |  |
| S123 | 2026-10-23 | DJ Studio | ui | In a DJ Studio jam, each queued song's thumbnail shows who added it and the date and time, and its menu says both in full. | 6740ef7e4c | pending |  |
| S124 | 2026-10-23 | Chat | fix | Reinstalling ForkMesh and restoring your identity key from the projects folder keeps your chat key, so safety numbers stay the same and nobody sees you as a new device. | b8b8103211 | pending |  |
| S125 | 2026-10-23 | Chat | fix | Calls reconnect by themselves after a network drop or when the other person's audio goes silent, and a call that cannot connect ends after 30 seconds with a clear message. | 407a9d2c6f | pending |  |
| S126 | 2026-10-23 | Chat | feature | A back button on the call screen returns to the chat while the call continues in a small window you can drag to any edge and tap to reopen. | 407a9d2c6f | pending |  |
| S127 | 2026-10-23 | Chat | ui | Video calls show your own camera with the other person's picture and name until their video arrives, and their picture again when their camera goes off. | 407a9d2c6f | pending |  |
| S128 | 2026-10-23 | Chat | feature | The call quality pill has a diagnostics button that turns red on a problem and explains the cause, the fixes and a prompt for an AI assistant. | 407a9d2c6f | pending |  |
| S129 | 2026-10-23 | Chat | feature | The pre-call check can record a 10-second microphone test and play it back before you call. | 407a9d2c6f | pending |  |
| S130 | 2026-10-23 | Chat | fix | App sounds, speech and dictation no longer cut off your microphone during a call, and call audio comes back after an interruption. | 407a9d2c6f | pending |  |
| S131 | 2026-10-23 | Chat | fix | Answering a call works even when the permission prompt stays open, and says why when the call can no longer be answered. | 407a9d2c6f | pending |  |
| S132 | 2026-10-23 | Chat | fix | Video calls switch to the other person's picture as soon as it arrives and play through the loudspeaker. | a53238311c | pending |  |
| S133 | 2026-10-23 | Chat | fix | The pre-call microphone test records your voice and plays it back on the speaker, even with the phone on silent. | a53238311c | pending |  |
<!-- ledger:end -->
