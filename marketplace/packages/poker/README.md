# Poker

Poker is an optional ForkMesh package that any account installs for itself:
no-limit Texas Hold'em at a felt table with other ForkMesh users, played for
virtual chips. It runs on the web dashboard (`/dashboard/poker`), as a native
desktop rail page (**Poker**), and as native iPhone and Android screens.

Chips are play money: every account starts with 10,000, can collect 2,000
free chips every four hours, and can never buy, sell or cash them out.

## What it does

- **A real table.** A dealer at the top who calls each street, a dealer
  button that moves every hand, small and big blinds, two hole cards each,
  flop, turn, river and showdown, side pots, uncalled bets returned, split
  pots with the odd chip left of the button, and a run-out of the board when
  everybody is all in.
- **Play with people.** Public tables show in the lobby; private tables are
  reached by link, by six-letter code or by invitation (a ping to the person
  you invite). Table chat sits beside the felt.
- **Bots to fill seats.** The table's owner can seat bots with their own
  playing styles so a game starts with one person.
- **Turn clocks.** 15 to 60 seconds a turn, a ring around the player's
  avatar, and a check or fold when it runs out; two in a row sit you out.
- **Sounds.** Shuffle, deal, chips, check knocks, folds, your-turn chime,
  the last five seconds of your clock, all-ins and wins, synthesized on every
  platform. The web page can also read the dealer's lines aloud.
- **Provably fair.** Each hand publishes a hash of the shuffled deck before
  the deal and reveals the deck afterwards.

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/poker.py`, `/api/poker` | Wallets, tables, the dealer and the bots. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/poker`, laid out for phones too. |
| Desktop | `content/desktop/Poker*.{h,cpp}` | Rail page **Poker**, signed with `forkmesh-poker-request-v1`. |
| iOS | `native/ios/ForkMesh/Features/Packages/Poker*.swift`, `ForkMeshCore/Sources/FMPackages/Poker*.swift` | |
| Android | `native/android/app/.../feature/poker/`, `native/android/core/.../poker/` | |

The API, the rules, the look and the sound recipes are in
[docs/marketplace/poker.md](../../../docs/marketplace/poker.md).
