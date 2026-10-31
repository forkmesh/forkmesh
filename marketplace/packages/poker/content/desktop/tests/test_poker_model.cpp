#include "PokerModel.h"
#include "PokerSynth.h"
#include "PokerTableView.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>

#include <cmath>
#include <cstdio>

using namespace forkmesh::poker;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
        ++failures;
    }
}

QJsonObject object(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

const char *kFlopTable = R"({
  "ok": true,
  "table": {
    "id": "0123456789abcdef0123456789abcdef", "name": "Friday night", "code": "QXRTMB", "private": false,
    "owner": "alice", "ownerIsMe": true, "stakes": "starter", "small": 5, "big": 10,
    "minBuyIn": 200, "maxBuyIn": 1000, "seatCount": 6, "turnSeconds": 30,
    "version": 211, "now": 1790000000000, "handNumber": 17, "phase": "flop",
    "board": ["Ah", "Kd", "7c"],
    "pots": [{"amount": 300, "eligible": [0, 2, 4]}],
    "potTotal": 380, "dealer": 2, "smallBlind": 4, "bigBlind": 0,
    "toAct": 0, "actStartedAt": 1789999990000, "actDeadline": 1790000020000,
    "currentBet": 40, "minRaiseTo": 80, "wakeAt": 1790000020000, "mySeat": 0, "wallet": 8750,
    "seats": [
      {"index": 0, "empty": false, "name": "alice", "bot": false, "stack": 960, "bet": 0, "totalBet": 40,
       "status": "active", "cards": ["As", "Qs"], "lastAction": {"kind": "call", "amount": 40}, "isTurn": true,
       "away": false, "handName": "", "winner": false, "won": 0},
      {"index": 1, "empty": true},
      {"index": 2, "empty": false, "name": "river-rita", "bot": true, "stack": 1000, "bet": 40, "totalBet": 80,
       "status": "active", "cards": ["??", "??"], "lastAction": {"kind": "bet", "amount": 40}, "isTurn": false,
       "away": false, "handName": "", "winner": false, "won": 0},
      {"index": 3, "empty": false, "name": "bob", "bot": false, "stack": 2450, "bet": 0, "totalBet": 10,
       "status": "folded", "cards": [], "lastAction": {"kind": "fold", "amount": 0}, "isTurn": false,
       "away": false, "handName": "", "winner": false, "won": 0},
      {"index": 4, "empty": false, "name": "carol", "bot": false, "stack": 640, "bet": 40, "totalBet": 80,
       "status": "active", "cards": ["??", "??"], "lastAction": {"kind": "call", "amount": 40}, "isTurn": false,
       "away": false, "handName": "", "winner": false, "won": 0},
      {"index": 5, "empty": false, "name": "lucky-lou", "bot": true, "stack": 0, "bet": 0, "totalBet": 0,
       "status": "out", "cards": [], "lastAction": {}, "isTurn": false, "away": true, "handName": "",
       "winner": false, "won": 0}
    ],
    "legal": {"fold": true, "check": false, "call": 40, "raise": {"min": 80, "max": 960}, "allIn": 960},
    "myHand": {"name": "Pair of Aces", "cards": ["As", "Ah", "Kd", "Qs", "7c"]},
    "dealerLine": "Flop: A♥ K♦ 7♣",
    "commitment": "aa",
    "events": [{"seq": 801, "kind": "flop", "seat": -1, "amount": 0}, {"seq": 799, "kind": "call", "seat": 4, "amount": 10}],
    "log": [{"at": 1789999990000, "seat": 2, "name": "river-rita", "kind": "bet", "amount": 40, "text": "River Rita bets 40"}],
    "chat": [{"at": 1789999980000, "name": "bob", "text": "nice hand"}],
    "lastHand": {"number": 16, "board": ["2c", "5d", "9h", "Js", "Qh"],
      "winners": [{"seat": 2, "name": "river-rita", "amount": 600, "handName": "Pair of Queens", "cards": ["Qd", "3c"],
                   "best": ["Qd", "Qh", "Js", "9h", "5d"]}],
      "shown": [{"seat": 0, "name": "alice", "cards": ["Tc", "Td"], "handName": "Pair of Tens"}],
      "commitment": "bb", "reveal": {"hashes": ["aa", "bb"],
        "cards": [{"index": 4, "card": "Qd", "salt": "00112233445566ff"}, {"index": 9, "card": "Qh", "salt": "ffeeddccbbaa9988"}]}},
    "history": [{"number": 16, "winners": ["river-rita"], "amount": 600, "handName": "Pair of Queens", "at": 1789999900000}]
  }
})";

const char *kShowdownTable = R"({
    "id": "0123456789abcdef0123456789abcdef", "name": "High rollers", "code": "PLMQRS", "private": true,
    "owner": "bob", "ownerIsMe": false, "stakes": "mid", "small": 100, "big": 200,
    "minBuyIn": 4000, "maxBuyIn": 20000, "seatCount": 9, "turnSeconds": 30,
    "version": 400, "now": 1790000000000, "handNumber": 31, "phase": "showdown",
    "board": ["Qh", "Jh", "Th", "4c", "2s"], "pots": [], "potTotal": 0, "dealer": 5, "smallBlind": 6, "bigBlind": 7,
    "toAct": -1, "actStartedAt": 0, "actDeadline": 0, "currentBet": 0, "minRaiseTo": 0, "wakeAt": 1790000004000,
    "mySeat": 3, "wallet": 12000,
    "seats": [
      {"index": 0, "empty": false, "name": "dana", "stack": 18200, "status": "folded", "cards": []},
      {"index": 1, "empty": false, "name": "eve", "stack": 7000, "status": "active", "cards": ["Ah", "Kh"],
       "handName": "Royal Flush", "winner": true, "won": 9600},
      {"index": 2, "empty": true},
      {"index": 3, "empty": false, "name": "alice", "stack": 4100, "status": "active", "cards": ["Qs", "Qd"],
       "handName": "Three of a Kind, Queens"},
      {"index": 4, "empty": false, "name": "felt-fred", "bot": true, "stack": 15000, "status": "folded", "cards": []},
      {"index": 5, "empty": false, "name": "gus", "stack": 22000, "status": "active", "cards": ["9c", "9d"],
       "handName": "Pair of Nines"},
      {"index": 6, "empty": false, "name": "hana", "stack": 5200, "status": "folded", "cards": []},
      {"index": 7, "empty": true},
      {"index": 8, "empty": false, "name": "ivan", "stack": 0, "status": "out", "away": false, "cards": []}
    ],
    "dealerLine": "Eve wins 9,600 with a Royal Flush",
    "lastHand": {"number": 31, "board": ["Qh", "Jh", "Th", "4c", "2s"],
      "winners": [{"seat": 1, "name": "eve", "amount": 9600, "handName": "Royal Flush", "cards": ["Ah", "Kh"],
                   "best": ["Ah", "Kh", "Qh", "Jh", "Th"]}],
      "shown": [], "commitment": "dd", "reveal": {"hashes": [], "cards": []}},
    "events": [{"seq": 40, "kind": "win", "seat": 1, "amount": 9600}]
})";

const char *kHeadsUpTable = R"({
    "id": "fedcba9876543210fedcba9876543210", "name": "Heads up", "code": "ABCDEF", "seatCount": 2,
    "small": 25, "big": 50, "minBuyIn": 1000, "maxBuyIn": 5000, "handNumber": 3, "phase": "preflop",
    "board": [], "pots": [], "potTotal": 75, "dealer": 1, "smallBlind": 1, "bigBlind": 0, "toAct": 1,
    "now": 1790000000000, "actStartedAt": 1789999992000, "actDeadline": 1790000004000, "currentBet": 50,
    "mySeat": 0, "wallet": 3000,
    "seats": [
      {"index": 0, "empty": false, "name": "alice", "stack": 2950, "bet": 50, "status": "active", "cards": ["7d", "7s"],
       "lastAction": {"kind": "big blind", "amount": 50}},
      {"index": 1, "empty": false, "name": "river-rita", "bot": true, "stack": 4975, "bet": 25, "status": "active",
       "cards": ["??", "??"], "lastAction": {"kind": "small blind", "amount": 25}}
    ],
    "myHand": {"name": "Pocket Sevens", "cards": ["7d", "7s"]},
    "dealerLine": "River Rita (bot) to act",
    "events": [{"seq": 5, "kind": "blinds", "seat": -1, "amount": 0}]
})";

void save(const QImage &image, const QString &name)
{
    const QString directory = qEnvironmentVariable("FORKMESH_POKER_RENDER");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    image.save(QDir(directory).filePath(name));
}

QImage render(PokerTableView &view, const QSize &size)
{
    view.resize(size);
    QImage image(size * view.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(view.devicePixelRatioF());
    image.fill(Qt::black);
    view.render(&image);
    return image;
}

bool teal(const QColor &color)
{
    return color.green() > color.red() + 30 && color.blue() > color.red() + 30 && color.green() > 50;
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const PokerCard ace = PokerCard::parse(QStringLiteral("Ah"));
    check(ace.valid && !ace.hidden && ace.rank == QLatin1Char('A') && ace.red() && ace.suitSymbol() == QString(QChar(0x2665)),
          "a card code parses into rank, suit and colour");
    const PokerCard ten = PokerCard::parse(QStringLiteral("Tc"));
    check(ten.rankLabel() == QStringLiteral("10") && !ten.red() && ten.code() == QStringLiteral("Tc"),
          "a ten reads as 10 and clubs are black");
    check(PokerCard::parse(QStringLiteral("??")).hidden && PokerCard::parse(QStringLiteral("??")).valid,
          "?? is a face-down card");
    check(!PokerCard::parse(QStringLiteral("Zz")).valid && !PokerCard::parse(QStringLiteral("A")).valid,
          "nonsense card codes are rejected");

    check(amountText(1234567) == QStringLiteral("1,234,567") && amountText(980) == QStringLiteral("980"),
          "chip amounts are grouped by thousands", amountText(1234567));
    check(shortAmountText(12500) == QStringLiteral("12.5K") && shortAmountText(2000000) == QStringLiteral("2M") &&
              shortAmountText(9999) == QStringLiteral("9,999"),
          "large amounts shorten on the felt", shortAmountText(12500));

    const QList<QPair<qint64, int>> chips = chipBreakdown(1260);
    check(chips == QList<QPair<qint64, int>>{{1000, 1}, {100, 2}, {25, 2}, {5, 2}},
          "chips break down by denomination, largest first");
    int total = 0;
    for (const auto &column : chipBreakdown(98765, 10))
        total += column.second;
    check(total <= 10 && chipBreakdown(98765, 10).first().first == 5000, "a big stack is capped to the visible chip count");
    check(chipColor(5) == QStringLiteral("#c62828") && chipColor(25) == QStringLiteral("#2e7d32") &&
              chipColor(100) == QStringLiteral("#212121") && chipColor(500) == QStringLiteral("#6a1b9a") &&
              chipColor(1000) == QStringLiteral("#f9a825") && chipColor(5000) == QStringLiteral("#ef6c00"),
          "chip colours follow the shared look");
    check(chipBreakdown(0).isEmpty(), "no chips for nothing");

    check(rotatedSlot(2, 2, 6) == 0 && rotatedSlot(3, 2, 6) == 1 && rotatedSlot(1, 2, 6) == 5,
          "seats rotate so the user's seat is slot 0");
    check(rotatedSlot(0, -1, 6) == 0 && rotatedSlot(4, -1, 6) == 4, "a watcher sees seat 1 at the bottom");
    bool fractionsOk = true;
    for (int count = 2; count <= 9; ++count) {
        double last = -1;
        for (int slot = 0; slot < count; ++slot) {
            const double fraction = seatFraction(slot, count);
            if (fraction <= last || fraction >= 1.0 || std::fabs(fraction - double(slot) / count) > 1e-9)
                fractionsOk = false;
            last = fraction;
        }
    }
    check(fractionsOk && seatFraction(3, 6) == 0.5 && seatFraction(1, 2) == 0.5,
          "seats are spread evenly round the table, with a player opposite the user at the top");

    const PokerLayout layout = PokerLayout::compute(QSizeF(1100, 700), 6, 2);
    const PokerSeatGeometry *mine = layout.seat(2);
    const PokerSeatGeometry *left = layout.seat(3);
    const PokerSeatGeometry *right = layout.seat(1);
    check(mine && mine->bottom && std::fabs(mine->avatar.x() - layout.centre.x()) < 1 && mine->avatar.y() > layout.centre.y(),
          "the user's seat sits at the bottom centre");
    check(left && right && left->avatar.x() < layout.centre.x() && right->avatar.x() > layout.centre.x(),
          "the next seat is on the user's left and the previous on the right");
    bool inside = true;
    for (const PokerSeatGeometry &geometry : layout.seats)
        inside = inside && layout.room.adjusted(20, 20, -20, -20).contains(geometry.avatar);
    check(inside, "every avatar is inside the view");
    check(layout.felt.width() > layout.felt.height() && layout.rail.contains(layout.felt),
          "the felt is a wide oval inside the rail");
    const PokerLayout small = PokerLayout::compute(QSizeF(560, 360), 9, 0);
    check(small.scale < layout.scale && small.seats.size() == 9, "the table scales with the window");

    const PokerTable table = PokerTable::parse(object(kFlopTable));
    check(table.valid && table.seats.size() == 6 && table.seatCount == 6 && table.seat(1)->empty,
          "a table parses every seat, empty ones included");
    check(table.mine() && table.mine()->cards.size() == 2 && table.mine()->cards.at(0).code() == QStringLiteral("As"),
          "the user's hole cards are visible");
    check(table.seat(2)->bot && table.seat(2)->displayName() == QStringLiteral("river-rita (bot)") &&
              table.seat(2)->cards.at(0).hidden,
          "bots are named as bots and other cards are face down");
    check(table.legal.present && table.legal.call == 40 && table.legal.canRaise && table.legal.raiseMin == 80 &&
              table.legal.raiseMax == 960 && table.myTurn(),
          "legal actions are read from the table");
    check(table.myHandName == QStringLiteral("Pair of Aces") && table.dealerLine.startsWith(QStringLiteral("Flop")),
          "the hand name and dealer line are read");
    check(table.events.size() == 2 && table.events.first().seq == 799 && highestSeq(table.events) == 801,
          "events are sorted by seq");
    check(table.lastHand.valid() && table.lastHand.winners.first().best.size() == 5 &&
              table.lastHand.hashes.size() == 2 && table.lastHand.revealed.size() == 2 &&
              table.lastHand.revealed.at(1).index == 9 && table.lastHand.revealed.at(1).card == QStringLiteral("Qh") &&
              table.lastHand.revealed.at(1).salt == QStringLiteral("ffeeddccbbaa9988"),
          "the last hand carries the provably fair reveal");
    QStringList deck;
    for (const QChar suit : QStringLiteral("cdhs"))
        for (const QChar rank : QStringLiteral("23456789TJQKA"))
            deck << QString(rank) + QString(suit);
    PokerLastHand reveal;
    reveal.number = 3;
    for (int index = 0; index < deck.size(); ++index)
        reveal.hashes << cardHash(QStringLiteral("%1").arg(index, 16, 16, QLatin1Char('0')), index, deck.at(index));
    check(reveal.hashes.first() == QStringLiteral("f4527695393ca385aa7901607aa594d736d22b5ba8d3407da2d28814c60175c8") &&
              reveal.hashes.last() == QStringLiteral("2daf7db2b215122804196638a184d265488a256b6ea1ec509f18a9d353ff8cee"),
          "each deck position hashes as SHA-256 of salt, position and card joined by colons");
    reveal.commitment = QStringLiteral("E8BFE91E74474E3FCD74D40DE8C01FC36DBDF38CBADC207771D427AD4F2CD01C");
    check(hashesCommitment(reveal.hashes).compare(reveal.commitment, Qt::CaseInsensitive) == 0,
          "the commitment is SHA-256 of the 52 position hashes joined by commas");
    for (const int index : {0, 13, 51})
        reveal.revealed << PokerRevealedCard{index, deck.at(index), QStringLiteral("%1").arg(index, 16, 16, QLatin1Char('0'))};
    check(revealMatches(reveal), "shown cards whose salts match their sealed positions check out");
    PokerLastHand nothingShown = reveal;
    nothingShown.revealed.clear();
    check(revealMatches(nothingShown), "a hand won without a showdown still checks the sealed deck");
    PokerLastHand wrongCard = reveal;
    wrongCard.revealed[1].card = QStringLiteral("3d");
    PokerLastHand wrongSalt = reveal;
    wrongSalt.revealed[2].salt = QStringLiteral("ffffffffffffffff");
    PokerLastHand wrongPlace = reveal;
    wrongPlace.revealed[0].index = 1;
    PokerLastHand tampered = reveal;
    tampered.hashes.swapItemsAt(0, 1);
    PokerLastHand partial = reveal;
    partial.hashes.removeLast();
    check(!revealMatches(wrongCard) && !revealMatches(wrongSalt) && !revealMatches(wrongPlace) &&
              !revealMatches(tampered) && !revealMatches(partial) && !revealMatches(table.lastHand),
          "a changed card, salt, position or hash list does not");
    check(table.botSeats() == QList<int>{2, 5} && table.emptySeats() == QList<int>{1}, "bot and empty seats are listed");
    check(table.streetBets() == 80, "street bets add up");
    check(table.seat(5)->away && statusLabel(*table.seat(5)) == QStringLiteral("Sitting out") &&
              statusLabel(*table.seat(3)) == QStringLiteral("Folded"),
          "seat statuses read plainly");

    check(callButtonLabel(table.legal) == QStringLiteral("Call 40"), "the call button names the amount");
    PokerLegal checkOnly = table.legal;
    checkOnly.call = 0;
    check(callButtonLabel(checkOnly) == QStringLiteral("Check") && checkOnly.canCheck(), "no call means check");
    PokerLegal shortCall = table.legal;
    shortCall.call = 960;
    check(callButtonLabel(shortCall) == QStringLiteral("Call all-in 960"), "a call of the whole stack says all-in");
    check(raiseVerb(table.legal, 40) == QStringLiteral("Raise to") && raiseVerb(table.legal, 0) == QStringLiteral("Bet"),
          "bet before anyone bets, raise to after");
    check(raiseButtonLabel(table.legal, 40, 960) == QStringLiteral("All-in 960") &&
              raiseButtonLabel(table.legal, 40, 200) == QStringLiteral("Raise to 200"),
          "the raise button reads the amount");
    check(raiseAction(0) == QStringLiteral("bet") && raiseAction(40) == QStringLiteral("raise"), "bet and raise map to actions");
    check(potFractionRaise(table, 1.0) == 460 && potFractionRaise(table, 0.5) == 250 && potFractionRaise(table, 5) == 960,
          "pot-sized raises count the call first and stop at the stack");
    check(clampRaise(table.legal, 10) == 80 && clampRaise(table.legal, 5000) == 960, "raises clamp to the legal range");

    check(eventsAfter(table.events, 799).size() == 1 && eventsAfter(table.events, 801).isEmpty() &&
              eventsAfter(table.events, 0).first().seq == 799,
          "only events newer than the last seen seq replay, in order");
    check(soundForEvent({1, QStringLiteral("raise"), 2, 80}, 0) == QStringLiteral("chip") &&
              soundForEvent({1, QStringLiteral("win"), 0, 80}, 0) == QStringLiteral("win") &&
              soundForEvent({1, QStringLiteral("win"), 2, 80}, 0) == QStringLiteral("chip") &&
              soundForEvent({1, QStringLiteral("chat"), 2, 0}, 0).isEmpty(),
          "events map to their sounds");
    check(streetEndEvent(QStringLiteral("turn")) && !streetEndEvent(QStringLiteral("call")),
          "streets end on the next card");

    const PokerClock green = turnClock(1000, 0, 30000);
    const PokerClock amber = turnClock(22000, 0, 30000);
    const PokerClock red = turnClock(27000, 0, 30000);
    check(green.running && green.color == QStringLiteral("#43a047") && amber.color == QStringLiteral("#ffb300") &&
              red.color == QStringLiteral("#e53935") && std::fabs(red.fraction - 0.1) < 1e-6,
          "the turn clock turns amber under 10 seconds and red under 5");
    check(!turnClock(1000, 0, 0).running, "no deadline, no clock");
    check(wakeDelay(1000, 400) == 750 && wakeDelay(0, 400) == -1 && wakeDelay(100, 400) == 150,
          "the wake timer fires 150 ms after wakeAt on the server's clock");

    PokerWallet wallet;
    wallet.claimAmount = 2000;
    wallet.nextClaimAt = 1000 + (3 * 60 + 12) * 60000;
    check(claimLabel(wallet, 1000) == QStringLiteral("Free chips in 3h 12m"), "the claim button counts down",
          claimLabel(wallet, 1000));
    wallet.canClaim = true;
    check(claimLabel(wallet, 1000) == QStringLiteral("Claim 2,000 free chips"), "a ready claim says how much");
    check(countdownText(30000) == QStringLiteral("1m") && countdownText(3600000) == QStringLiteral("1h"),
          "short countdowns round up");

    check(normalizedTableCode(QStringLiteral(" qx rt-mb ")) == QStringLiteral("QXRTMB") &&
              isValidTableCode(QStringLiteral("qxrtmb")) && !isValidTableCode(QStringLiteral("QXR1MB")) &&
              !isValidTableCode(QStringLiteral("QXRT")),
          "six-letter table codes are normalised and checked");
    check(errorText(QStringLiteral("seat_taken"), {}).contains(QStringLiteral("seat")) &&
              errorText(QStringLiteral("weird"), QStringLiteral("x")) == QStringLiteral("x"),
          "error codes read as sentences");

    const PokerLobby lobby = PokerLobby::parse(object(R"({"ok": true, "actor": "alice",
      "wallet": {"chips": 8750, "onTables": 1000, "canClaim": false, "claimAmount": 2000, "nextClaimAt": 5,
                 "handsPlayed": 42, "handsWon": 11, "biggestPot": 4200, "bestHand": "Flush, Ace high"},
      "stakes": [{"id": "starter", "label": "Starter", "small": 5, "big": 10, "minBuyIn": 200, "maxBuyIn": 1000}],
      "tables": [{"id": "abc", "name": "Friday night", "code": "QXRTMB", "seats": 6, "players": 3, "bots": 1,
                  "small": 5, "big": 10, "mine": true}], "now": 1})"));
    check(lobby.valid && lobby.wallet.chips == 8750 && lobby.wallet.bestHand == QStringLiteral("Flush, Ace high") &&
              lobby.tables.size() == 1 && lobby.tables.first().mine && lobby.stake(QStringLiteral("starter")),
          "the lobby parses the wallet, stakes and tables");
    check(PokerLobby::parse(object(R"({"wallet": {}})")).stakes.size() == 4, "stakes fall back to the four standard ones");

    bool soundsOk = true;
    for (const QString &name : soundNames()) {
        const std::vector<float> samples = synthesize(name, 44100);
        const float peak = peakLevel(samples);
        if (samples.empty() || peak < 0.05f || peak > 0.93f) {
            soundsOk = false;
            std::printf("  sound %s peak %f size %zu\n", qPrintable(name), double(peak), samples.size());
        }
    }
    check(soundsOk, "every sound in the recipe table synthesizes and stays below clipping");
    const double shuffleLength = soundSeconds(synthesize(QStringLiteral("shuffle"), 44100), 44100);
    const double dealLength = soundSeconds(synthesize(QStringLiteral("deal"), 44100), 44100);
    const double winLength = soundSeconds(synthesize(QStringLiteral("win"), 44100), 44100);
    check(shuffleLength > 0.3 && shuffleLength < 0.45 && dealLength < 0.09 && winLength > 0.8,
          "sound lengths follow the recipes",
          QStringLiteral("%1 %2 %3").arg(shuffleLength).arg(dealLength).arg(winLength));
    check(synthesize(QStringLiteral("nothing"), 44100).empty(), "an unknown sound is silent");
    check(pcm16({0.5f, -0.5f}, 2).size() == 8, "PCM output repeats each sample per channel");

    PokerTableView view;
    view.setServerOffset(1790000000000LL - QDateTime::currentMSecsSinceEpoch() + 15000);
    QImage empty = render(view, QSize(1100, 700));
    check(teal(empty.pixelColor(empty.width() / 2, int(empty.height() * 0.62))), "an empty view still draws the felt");
    view.setTable(table, {});
    QImage flop = render(view, QSize(1100, 700));
    save(flop, QStringLiteral("poker-flop-1100.png"));
    check(teal(flop.pixelColor(int(flop.width() * 0.3), flop.height() / 2)), "the felt is teal");
    const QColor corner = flop.pixelColor(3, 3);
    check(corner.lightness() < 50, "the room around the table is dark", corner.name());
    check(!view.animating(), "a first load plays no animation");

    PokerTable next = table;
    next.phase = QStringLiteral("turn");
    next.board << PokerCard::parse(QStringLiteral("2d"));
    for (PokerSeat &seat : next.seats)
        seat.bet = 0;
    next.events << PokerEvent{802, QStringLiteral("call"), 0, 40} << PokerEvent{803, QStringLiteral("turn"), -1, 0};
    view.setTable(next, eventsAfter(next.events, highestSeq(table.events)));
    check(view.animating(), "new events start an animation");
    view.finishAnimations();

    const PokerTable showdown = PokerTable::parse(object(kShowdownTable));
    check(showdown.seatCount == 9 && showdown.seat(1)->winner && !showdown.legal.present, "a showdown parses its winner");
    view.setTable(showdown, {});
    save(render(view, QSize(1100, 700)), QStringLiteral("poker-showdown-1100.png"));
    save(render(view, QSize(700, 450)), QStringLiteral("poker-showdown-700.png"));

    const PokerTable headsUp = PokerTable::parse(object(kHeadsUpTable));
    view.setTable(headsUp, {});
    save(render(view, QSize(900, 560)), QStringLiteral("poker-headsup-900.png"));
    save(render(view, QSize(1600, 900)), QStringLiteral("poker-headsup-1600.png"));
    check(headsUp.seatCount == 2 && !headsUp.myTurn(), "heads-up waits for the other player");

    std::printf("%s: %d failure%s\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
