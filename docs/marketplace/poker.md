# Poker

No-limit Texas Hold'em for ForkMesh users, played with virtual chips. The
owner asked for a poker plugin that looks great as a native desktop page, on
phones and on the web, where ForkMesh users play each other with virtual
chips, with sounds, a dealer and everything a standard game has, made to look
realistic and fun.

Package: [`marketplace/packages/poker`](../../marketplace/packages/poker/README.md).
Every account installs it for itself (`install.sync: "account"`); tables are
shared between the accounts seated at them.

Chips are play money. They cannot be bought, sold, cashed out or moved
between accounts, and no screen offers to. Every account starts with 10,000
chips and can collect 2,000 free chips every four hours.

## Code map

| Path | What it does |
| --- | --- |
| `content/worker/poker.py` | The Worker API at `/api/poker`: wallets, the lobby, tables, seats, the dealer (shuffle, blinds, betting rounds, side pots, showdown), hand evaluation, bots, chat, invites, export and purge. The build allows one module per package, so the engine lives here too. |
| `content/web/view.html`, `content/web/page.js` | The dashboard page `/dashboard/poker`: lobby, the felt table, cards, chips, the dealer, action bar, chat, Web Audio sounds and the optional spoken dealer. Laid out for phones too. |
| `content/desktop/Poker*.{h,cpp}`, `content/desktop/tests/test_poker_model.cpp` | The native desktop page (`builtin:poker`), painted with QPainter, with its own synthesized sounds; see [Desktop page](#desktop-page). |
| `native/ios/ForkMeshCore/Sources/FMPackages/Poker{Models,Game,Model,Synth}.swift`, `native/ios/ForkMesh/Features/Packages/Poker*.swift` | The iPhone models, rules helpers, screen model, sound synthesis, lobby and full-screen table (SwiftUI). Tests: `FMPackagesTests/PokerTests.swift`, `FMApiTests/PokerRouteTests.swift`. |
| `native/android/core/.../poker/`, `native/android/app/.../feature/poker/` | The Android models, API, rules helpers, session, sound synthesis, lobby and Canvas table (Compose). Test: `core/src/test/.../poker/PokerTest.kt`. |
| `app/src/entry.py` (`notify_account_event`), `app/public/dashboard/js/02-helpers.js` | `package:` doorbells are never coalesced; the dashboard turns them into a `forkmesh:package-event` window event (and one with an empty id after a reconnect). |
| `tests/test_poker.py` | Engine, API, betting, side pots, timers, bots and lifecycle against sqlite. |

## Rules

Standard no-limit Hold'em:

- 2 to 9 seats. A hand starts when two or more seated players have chips and
  are not sitting out. The dealer button moves one seat left every hand.
- Blinds: the seat left of the button posts the small blind and the next the
  big blind. Heads-up, the button posts the small blind, acts first before the
  flop and last after it.
- Betting: fold, check, call, bet, raise, all-in. The minimum bet is the big
  blind; a raise must raise by at least the last full raise. An all-in that is
  less than a full raise does not reopen betting for players who already
  acted: they may only call or fold, unless several short all-ins together
  make them face at least a full raise, which reopens it.
- Streets: preflop (two hole cards each), flop (three), turn, river, showdown.
  When at most one player can still bet, the remaining board is dealt one
  street at a time with a short pause so the run-out can be watched.
- Side pots are built from each player's total contribution; folded chips
  stay in the pots they were put into. Odd chips go to the first winner left
  of the button. An uncalled bet is returned.
- Showdown reveals the cards of every player still in the hand. A hand won
  because everybody else folded shows no cards.
- Each turn has a clock (15, 30, 45 or 60 seconds, chosen when the table is
  made). When it runs out the player checks if they can and folds otherwise;
  two timeouts in a row sit them out.
- A player who busts can rebuy from their wallet between hands. Leaving
  during a hand folds the hand and cashes the stack out when it ends; the
  seat (and a removed bot's seat) stays until then, because its chips are
  still in the pot.
- The owner can close a table only between hands (`409 hand_in_progress`
  otherwise), so a hand can't be cancelled by the player losing it. The
  idle sweep closes a table even mid-hand and gives every player back what
  they put into that hand.

Cards are dealt from a deck shuffled with the operating system's secure
random source. When a hand starts, each of the 52 shuffled positions `i` is
sealed as `h[i] = SHA-256("<salt_i>:<i>:<card>")` with its own random salt,
and the table publishes `commitment = SHA-256(h[0] + "," + ... + "," + h[51])`.
When it ends, `lastHand.reveal` holds all 52 `hashes` and, in `cards`, the
index, card and salt of only the cards that became public (the board and the
hands shown at showdown). Anyone can check those cards sat where the
committed deck said, while folded, mucked and burned cards stay secret. (The
first version revealed the whole deck and salt; a review found that gave away
every folded hand.)

## Stakes

| id | Blinds | Buy-in |
| --- | --- | --- |
| `starter` | 5 / 10 | 200 – 1,000 |
| `low` | 25 / 50 | 1,000 – 5,000 |
| `mid` | 100 / 200 | 4,000 – 20,000 |
| `high` | 500 / 1,000 | 20,000 – 100,000 |

## Bots

The table's owner can fill empty seats with bots so a game can start with
one person. Bots buy in for the maximum from the house (never from a wallet),
rebuy when they bust, and think for one to two and a half seconds before they
act. They play from a Monte Carlo estimate of their equity against the
players left in the hand, the pot odds, their position and a personality
(tight, loose, aggressive or careful), with an occasional bluff. Their names
come from a fixed list and always end in "(bot)" on screen.

## Live updates

There is no polling. Every change to a table rings each seated player and
everyone who opened the table in the last ten minutes with the account
doorbell topic `package:poker` (`runtime.notify_account`). Package topics are
never coalesced by the relay, so two actions a second apart both arrive.
Clients re-read the table when the doorbell rings, when the page opens, when
the socket reconnects and when the app returns to the foreground.

Timed steps (a turn clock running out, a bot's move, the next street of a
run-out, the next hand) happen on the server when any request touches the
table after `wakeAt`. A client that is showing the table sets one one-shot
timer for `wakeAt` (plus 150 ms) and sends `POST .../tick` when it fires; any
client may send it and it does nothing early. That timer is part of the game
clock, not a display refresh. The per-minute reminder phase closes tables
nobody has touched for three hours and pays every stack back to its wallet.

## API

All routes are under `/api/poker`, take and return JSON, and answer
`{"ok": false, "error": "<code>"}` with a 4xx status on failure. The desktop
signs requests with the proof `forkmesh-poker-request-v1`. Writes need a
same-origin request on the web.

| Method and path | Body | Answer |
| --- | --- | --- |
| `GET /api/poker` | | the lobby |
| `POST /api/poker/claim` | | `{ok, wallet}`; `409 claim_not_ready` before the four hours are up |
| `POST /api/poker/tables` | `{name, stakes, seats, private, turnSeconds, bots, buyIn}` | `{ok, table}`; the creator is seated with `buyIn` (0 or absent: watch only) |
| `GET /api/poker/codes/<CODE>` | | `{ok, tableId}` for a six-letter table code |
| `GET /api/poker/tables/<id>` | | `{ok, table}` |
| `POST /api/poker/tables/<id>/sit` | `{seat, buyIn}` | `{ok, table}` |
| `POST /api/poker/tables/<id>/leave` | | `{ok, table}` |
| `POST /api/poker/tables/<id>/act` | `{action, amount, hand}` | `{ok, table}` |
| `POST /api/poker/tables/<id>/tick` | | `{ok, table}` |
| `POST /api/poker/tables/<id>/away` | `{away}` | `{ok, table}` |
| `POST /api/poker/tables/<id>/rebuy` | `{amount}` | `{ok, table}` |
| `POST /api/poker/tables/<id>/bots` | `{add}` or `{remove: seat}` | `{ok, table}` (owner) |
| `POST /api/poker/tables/<id>/chat` | `{text}` | `{ok, table}` |
| `POST /api/poker/tables/<id>/invite` | `{names: [...]}` | `{ok, invited, skipped, table}` (seated players and the owner) |
| `DELETE /api/poker/tables/<id>` | | `{ok, closed: true}` (owner, between hands; every stack goes back to its wallet) |
| `GET /api/poker/export` | | `{ok, filename, export}` |

`act` actions: `fold`, `check`, `call`, `bet`, `raise`, `allin`. For `bet`
and `raise`, `amount` is the total the player's bet becomes this street
("raise to"), between `legal.raise.min` and `legal.raise.max`. `hand` is the
`handNumber` the client was looking at; a stale one answers
`409 hand_changed`. Acting out of turn answers `409 not_your_turn`; an
illegal amount `400 invalid_amount`.

Other error codes: `invalid_session` (401), `not_found`, `table_not_found`
(404), `table_full`, `seat_taken`, `already_seated`, `not_seated`,
`not_enough_chips`, `invalid_buy_in`, `too_many_tables`, `owner_only`,
`hand_in_progress` (409), `invalid_stakes`, `invalid_text` (400).

### Lobby

```json
{
  "ok": true,
  "actor": "alice",
  "wallet": {
    "chips": 8750, "onTables": 1000, "canClaim": false, "claimAmount": 2000,
    "nextClaimAt": 1790000000000, "handsPlayed": 42, "handsWon": 11,
    "biggestPot": 4200, "bestHand": "Flush, Ace high"
  },
  "stakes": [
    {"id": "starter", "label": "Starter", "small": 5, "big": 10, "minBuyIn": 200, "maxBuyIn": 1000}
  ],
  "tables": [
    {
      "id": "<32 hex>", "name": "Friday night", "code": "QXRTMB", "private": false,
      "owner": "alice", "stakes": "starter", "small": 5, "big": 10,
      "minBuyIn": 200, "maxBuyIn": 1000, "seats": 6, "players": 3, "humans": 2,
      "bots": 1, "handNumber": 17, "turnSeconds": 30, "mine": true,
      "invited": false, "updatedAt": 1790000000000
    }
  ],
  "now": 1790000000000
}
```

`tables` holds the open public tables, the tables the user is seated at, and
private tables they own or were invited to; private tables are otherwise
reached by link or code.

### Table

```json
{
  "id": "<32 hex>", "name": "Friday night", "code": "QXRTMB", "private": false,
  "owner": "alice", "ownerIsMe": true, "stakes": "starter", "small": 5, "big": 10,
  "minBuyIn": 200, "maxBuyIn": 1000, "seatCount": 6, "turnSeconds": 30,
  "version": 211, "now": 1790000000000, "handNumber": 17,
  "phase": "flop",
  "board": ["Ah", "Kd", "7c"],
  "pots": [{"amount": 300, "eligible": [0, 2, 4]}],
  "potTotal": 380,
  "dealer": 2, "smallBlind": 4, "bigBlind": 0,
  "toAct": 0, "actStartedAt": 1789999990000, "actDeadline": 1790000020000,
  "currentBet": 40, "minRaiseTo": 80,
  "wakeAt": 1790000020000,
  "mySeat": 0,
  "wallet": 8750,
  "seats": [
    {"index": 0, "empty": false, "name": "alice", "displayName": "alice", "bot": false, "stack": 960,
     "bet": 0, "totalBet": 40, "status": "active", "cards": ["As", "Qs"],
     "lastAction": {"kind": "call", "amount": 40}, "isTurn": true,
     "away": false, "handName": "", "winner": false, "won": 0},
    {"index": 1, "empty": true},
    {"index": 2, "empty": false, "name": "river-rita", "displayName": "River Rita (bot)", "bot": true, "stack": 1000,
     "bet": 40, "totalBet": 80, "status": "active", "cards": ["??", "??"],
     "lastAction": {"kind": "bet", "amount": 40}, "isTurn": false,
     "away": false, "handName": "", "winner": false, "won": 0}
  ],
  "legal": {"fold": true, "check": false, "call": 40,
            "raise": {"min": 80, "max": 960}, "allIn": 960},
  "myHand": {"name": "Pair of Aces", "cards": ["As", "Ah", "Kd", "Qs", "7c"]},
  "dealerLine": "Flop: A♥ K♦ 7♣",
  "commitment": "<64 hex>",
  "events": [{"seq": 801, "kind": "flop", "seat": -1, "amount": 0}],
  "log": [{"at": 1789999990000, "seat": 2, "name": "river-rita", "kind": "bet", "amount": 40, "text": "River Rita bets 40"}],
  "chat": [{"at": 1789999980000, "name": "bob", "text": "nice hand"}],
  "lastHand": {
    "number": 16, "board": ["2c", "5d", "9h", "Js", "Qh"],
    "winners": [{"seat": 2, "name": "river-rita", "amount": 600, "handName": "Pair of Queens",
                 "cards": ["Qd", "3c"], "best": ["Qd", "Qh", "Js", "9h", "5d"]}],
    "shown": [{"seat": 0, "name": "alice", "cards": ["Tc", "Td"], "handName": "Pair of Tens"}],
    "commitment": "<64 hex>",
    "reveal": {"hashes": ["<64 hex>", "... 52 in all"],
               "cards": [{"index": 3, "card": "Qd", "salt": "<16 hex>"}]}
  },
  "history": [{"number": 16, "winners": ["river-rita"], "amount": 600, "handName": "Pair of Queens", "at": 1789999900000}]
}
```

- Cards are two characters: rank `23456789TJQKA` and suit `c d h s`. `"??"`
  is a face-down card. Your own cards are always visible; another player's
  are visible only at showdown.
- `phase` is `waiting` (no hand), `preflop`, `flop`, `turn`, `river` or
  `showdown` (cards turned up, winners paid, next hand at `wakeAt`).
- `displayName` is what to show for a seat: the account name, or a bot's
  name with "(bot)" after it.
- `status` per seat: `active`, `folded`, `allin`, `out` (seated but not in
  this hand: joined mid-hand, sitting out or busted), `leaving`.
  `away` means sitting out; a seat with `stack` 0 and no hand is busted.
- `legal` is present only when it is your turn. `call` is the chips you add
  to call (0 when you can check); `raise` is null when you cannot raise.
- `events` are the table's last 40 events with an increasing `seq`. A client
  keeps the highest `seq` it has seen and plays the sound and animation of
  every newer one in order; on first load it plays none. Kinds: `shuffle`,
  `deal` (hole cards), `blinds`, `check`, `call`, `bet`, `raise`, `allin`,
  `fold`, `flop`, `turn`, `river`, `showdown`, `win`, `join`, `leave`,
  `chat`, `timeout`. `seat` is -1 for the dealer.
- `myHand` is the best hand the user holds right now (absent when not in a
  hand); before the flop its name is like "Pocket Aces" or "Ace King suited".
- `wakeAt` is 0 when nothing is scheduled.

Hand names: `Royal Flush`, `Straight Flush, Nine high`, `Four of a Kind,
Jacks`, `Full House, Kings over Sevens`, `Flush, Ace high`, `Straight, Ten
high`, `Three of a Kind, Fives`, `Two Pair, Kings and Sevens`, `Pair of
Aces`, `High Card, King`.

## Look and sound

Every client draws the same table so it feels like one game on every device:

- A dark room around an oval table: a polished walnut rail, a padded black
  leather arm rest, and green felt (`#0d5c3a` centre to `#083d27` edge, with
  a faint woven texture) with a thin gold line inset from the edge and the
  ForkMesh mark printed faintly in the middle.
- The dealer sits at the top centre (a dealer tray and a small dealer figure
  or badge) and says each step in `dealerLine`; the dealer button is a white
  puck with "DEALER" that slides to the button seat.
- The user's own seat is always at the bottom centre; the others are spread
  around the oval in seat order. Each seat shows an avatar circle with the
  initial, the name, the stack, the turn clock as a ring around the avatar
  (green, amber under 10 seconds, red under 5), the last action as a small
  tag, and the cards in front of it.
- Cards are white with rounded corners, the rank and suit in two corners and
  a large suit pip in the middle; hearts and diamonds `#c62828`, clubs and
  spades `#1a1a1a`. Backs are deep blue (`#1e3a8a`) with a fine diamond
  lattice and a white border. Cards are dealt from the dealer with a slide
  and flip.
- Chips by value: 1 white, 5 red `#c62828`, 25 green `#2e7d32`, 100 black
  `#212121`, 500 purple `#6a1b9a`, 1,000 yellow `#f9a825`, 5,000 orange
  `#ef6c00`, each with white edge stripes. Bets sit in front of the seat as a
  small stack and slide into the pot when a street ends; the pot slides to
  the winner.
- Winners glow gold, the winning five cards lift, and the hand name shows on
  the felt.

Sounds are synthesized on every platform (no audio files) from these
recipes, and can be muted; the choice is kept per device:

| Sound | Recipe |
| --- | --- |
| shuffle | 10 riffle ticks: 12 ms white-noise bursts through a 3 kHz band-pass, 32 ms apart (±6 ms), getting louder then softer |
| deal | 45 ms noise burst, band-pass sweeping 5 kHz → 1.5 kHz, fast exponential decay |
| chip | 3 clacks 45 ms apart: sines at 3.2 kHz and 5.1 kHz, 25 ms decay, with a 5 ms noise tick |
| check | two knocks 120 ms apart: 150 Hz sine plus a little noise, 60 ms decay |
| fold | 180 ms noise swoosh through a 1.2 kHz low-pass, fading in and out |
| your turn | two soft sines, 880 Hz then 1320 Hz, 140 ms each |
| clock | 1 kHz tick, 20 ms, once a second in the last five seconds of your turn |
| all-in | 400 ms sine rising 110 → 220 Hz under a chip cascade |
| win | triangle arpeggio C5 E5 G5 C6, 110 ms each, then a chip cascade |

## Web page

`/dashboard/poker` opens on the lobby; `?table=<id>` (the link the invite
ping and "Copy link" use) opens that table. The table is drawn with HTML and
CSS, sized from one unit (`--u`) worked out from the space the page has: a
wide oval beside the chat column on a desktop browser, a tall oval with the
chat in a drawer below 900 px, with larger cards for the player and the
action buttons in two rows on a phone. Seats, cards, bets and the dealer
button are placed on the oval by the angle rule above, the user's seat at
the bottom.

- **Animations** only play for what changed since the last read: hole cards
  fly from the dealer when a `deal` event is new, board cards flip in on a
  new street, bets slide into the pot when a street ends and the pot slides
  to each winner (Web Animations on a copy of the chips), and a tag, bet or
  banner pops in only when it differs from the previous read, so a re-read
  never makes the table flicker.
- **Turn clock**: a ring around the player whose turn it is, drawn each frame
  from `actStartedAt`, `actDeadline` and the server clock (`now` minus the
  local time when the answer arrived).
- **Controls**: Fold, Check or Call, Bet or Raise with a slider, a number
  box and ½ pot, ¾ pot and pot presets, and All in; keys F, C, R and A.
  While waiting, "Check / fold" and "Call any" act for the player the moment
  their turn comes and clear at the next hand. The menu holds sit out and
  back, add chips, the dealer voice, the provably fair panel, add and remove
  a bot (owner), leave and close (owner).
- **Sound** is Web Audio from the recipe table, started on the first click
  (browsers keep audio off until then); mute is kept in `localStorage`
  (`forkmesh.poker.muted`). The dealer voice reads the dealer's line for each
  street and win with the browser's speech synthesis, off by default
  (`forkmesh.poker.voice`).
- **Last hand** shows the winners, the cards that were shown, recent hands,
  and the commitment and revealed card salts with a "Check it now" button that hashes
  them in the browser.

## Desktop page

`builtin:poker` registers `PokerPage` (rail icon `spade`,
`desktop/resources/icons/forkmesh/spade.svg`). Its pieces:

| File | What it does |
| --- | --- |
| `PokerModel.{h,cpp}` | Widget-free: JSON to lobby and table structs, card codes, chip breakdown by denomination, seat rotation (your seat is slot 0, the next seat to your left), seat positions around the stadium-shaped rail with the dealer's place at the top kept free, legal-action labels, pot-fraction raises, the turn clock colour, the wake delay, event diffing by `seq`, and the reveal check (the 52 `hashes` hash to `commitment` joined by commas, and each revealed card's `salt:index:card` hashes to `hashes[index]`). Tested by `tests/test_poker_model.cpp` (`forkmesh-poker-tests`). |
| `PokerTableView.{h,cpp}` | The QPainter table. The room, dealer, rail, leather, felt, gold line, mark, dealer tray and board slots are painted once into a pixmap at the widget's size and device pixel ratio; cards, chips and avatars are cached pixmaps too, so a frame is a few blits. Newer events become a timeline of animations (deal from the tray with a flip for your cards, chips to the bet line, bets into the pot when a street ends, board cards, showdown flips, the pot to the winner, the button sliding) driven by one 16 ms frame timer that stops when the timeline is empty. While a turn clock runs it repaints only the acting avatar at 15 frames a second. |
| `PokerSynth.{h,cpp}`, `PokerSound.{h,cpp}` | The recipe table above, synthesized into float buffers and mixed by one pull-mode `QAudioSink` that closes 2.5 s after the last sound. Without Qt Multimedia the build has no sink and the page says "No sound". The mute choice is the package setting `soundMuted`. |
| `PokerPage.{h,cpp}` | Lobby (wallet, claim countdown, record, tables, new table, join by code) and the table page (seat, sit out, rebuy, leave, bots, invite, close, chat, hand log, last hand with the checked reveal), the action bar with F, C, R and A shortcuts. |

The page never polls. `packageChanged()` (the account doorbell `package:poker`)
re-reads the open table, and the lobby when it is showing (otherwise the lobby
is marked stale and read when it shows). The only timer that sends a request is
the one-shot `wakeAt` timer: its delay is `wakeAt - now` taken from the same
answer, so the local clock's offset does not matter, plus 150 ms, and it sends
`POST .../tick`. The lobby's claim label is refreshed by a local one-shot
timer that sends nothing.

Layout modes: Control Tower puts the chat, hand log and last hand in a column
beside the table; Focus gives the table the page and opens them as a drawer
over its right side from "Chat & log", which counts unread chat lines. The
mode is read on show and on refresh.

## Data

- `poker_wallets`: one row per account: chips, the last claim, and stats
  (hands played and won, biggest pot, best hand).
- `poker_tables`: one row per table. The plain columns are what the lobby
  lists (name, code, stakes, privacy, owner, seat and player counts, hand
  number, open flag, version, update time); the whole game state (deck,
  hole cards, seats, pots, events, chat, history) is sealed with the instance
  key in `data`. Writes compare and swap on `version`, so two players acting
  at once never lose a move.
- `poker_seats`: who sits at which table, so the lobby and the account purge
  can find a user's tables without opening every table.
- `poker_invites`: private-table invitations by account name.

Each account owns five open tables at most. Chat keeps the last 40 lines,
the log 30, events 40 and history 10 hands.

## Lifecycle

Installing is the shared account install. Removing Poker (and deleting the
account) cashes every stack the account has on a table back out, removes it
from every table (folding a hand in progress), closes the tables it owns, and
deletes its wallet. The export (`forkmesh-poker-export-v1`) holds the wallet
and its stats.
