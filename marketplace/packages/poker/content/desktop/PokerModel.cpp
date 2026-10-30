#include "PokerModel.h"

#include <QCryptographicHash>
#include <QJsonValue>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace forkmesh::poker {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 number(const QJsonObject &object, const char *key, qint64 fallback = 0)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isDouble())
        return qint64(std::llround(value.toDouble()));
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

int integer(const QJsonObject &object, const char *key, int fallback = 0)
{
    return int(number(object, key, fallback));
}

bool flag(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toBool();
}

QJsonArray array(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toArray();
}

QStringList strings(const QJsonArray &values)
{
    QStringList out;
    for (const QJsonValue &value : values)
        if (value.isString())
            out << value.toString();
    return out;
}

PokerSeat parseSeat(const QJsonObject &object, int fallbackIndex)
{
    PokerSeat seat;
    seat.index = integer(object, "index", fallbackIndex);
    seat.empty = object.value(QStringLiteral("empty")).toBool(text(object, "name").isEmpty());
    if (seat.empty)
        return seat;
    seat.name = text(object, "name");
    seat.label = text(object, "displayName");
    seat.bot = flag(object, "bot");
    seat.stack = number(object, "stack");
    seat.bet = number(object, "bet");
    seat.totalBet = number(object, "totalBet");
    seat.status = text(object, "status");
    seat.cards = parseCards(array(object, "cards"));
    const QJsonObject last = object.value(QStringLiteral("lastAction")).toObject();
    seat.lastAction.kind = text(last, "kind");
    seat.lastAction.amount = number(last, "amount");
    seat.isTurn = flag(object, "isTurn");
    seat.away = flag(object, "away");
    seat.handName = text(object, "handName");
    seat.winner = flag(object, "winner");
    seat.won = number(object, "won");
    return seat;
}

QString capitalized(const QString &value)
{
    if (value.isEmpty())
        return value;
    return value.left(1).toUpper() + value.mid(1);
}

}

PokerCard PokerCard::parse(const QString &code)
{
    PokerCard card;
    const QString trimmed = code.trimmed();
    if (trimmed == QLatin1String("??")) {
        card.hidden = true;
        card.valid = true;
        return card;
    }
    if (trimmed.size() != 2)
        return card;
    const QChar rank = trimmed.at(0).toUpper();
    const QChar suit = trimmed.at(1).toLower();
    if (!QStringLiteral("23456789TJQKA").contains(rank) || !QStringLiteral("cdhs").contains(suit))
        return card;
    card.rank = rank;
    card.suit = suit;
    card.valid = true;
    return card;
}

QString PokerCard::code() const
{
    if (!valid)
        return {};
    if (hidden)
        return QStringLiteral("??");
    return QString(rank) + QString(suit);
}

QString PokerCard::rankLabel() const
{
    if (!valid || hidden)
        return {};
    if (rank == QLatin1Char('T'))
        return QStringLiteral("10");
    return QString(rank);
}

QString PokerCard::suitSymbol() const
{
    switch (suit.unicode()) {
    case 'c': return QString(QChar(0x2663));
    case 'd': return QString(QChar(0x2666));
    case 'h': return QString(QChar(0x2665));
    case 's': return QString(QChar(0x2660));
    default: return {};
    }
}

bool PokerCard::red() const
{
    return suit == QLatin1Char('h') || suit == QLatin1Char('d');
}

QString PokerCard::label() const
{
    if (!valid)
        return {};
    if (hidden)
        return QStringLiteral("??");
    return rankLabel() + suitSymbol();
}

QList<PokerCard> parseCards(const QJsonArray &cards)
{
    QList<PokerCard> out;
    for (const QJsonValue &value : cards) {
        const PokerCard card = PokerCard::parse(value.toString());
        if (card.valid)
            out << card;
    }
    return out;
}

QString cardsLabel(const QList<PokerCard> &cards)
{
    QStringList parts;
    for (const PokerCard &card : cards)
        parts << card.label();
    return parts.join(QLatin1Char(' '));
}

bool PokerSeat::inHand() const
{
    return !empty && (status == QLatin1String("active") || status == QLatin1String("allin"));
}

bool PokerSeat::busted() const
{
    return !empty && stack <= 0 && !inHand() && status != QLatin1String("folded");
}

QString PokerSeat::displayName() const
{
    if (empty)
        return {};
    if (!label.isEmpty())
        return label;
    if (bot && !name.endsWith(QLatin1String("(bot)")))
        return name + QStringLiteral(" (bot)");
    return name;
}

PokerTable PokerTable::parse(const QJsonObject &input)
{
    const QJsonObject object = input.contains(QStringLiteral("table")) && input.value(QStringLiteral("table")).isObject()
        ? input.value(QStringLiteral("table")).toObject()
        : input;
    PokerTable table;
    table.id = text(object, "id");
    table.valid = !table.id.isEmpty();
    table.name = text(object, "name");
    table.code = text(object, "code");
    table.isPrivate = flag(object, "private");
    table.owner = text(object, "owner");
    table.ownerIsMe = flag(object, "ownerIsMe");
    table.stakes = text(object, "stakes");
    table.small = number(object, "small");
    table.big = number(object, "big");
    table.minBuyIn = number(object, "minBuyIn");
    table.maxBuyIn = number(object, "maxBuyIn");
    table.seatCount = std::clamp(integer(object, "seatCount"), 0, 10);
    table.turnSeconds = integer(object, "turnSeconds", 30);
    table.version = number(object, "version");
    table.now = number(object, "now");
    table.handNumber = integer(object, "handNumber");
    table.phase = text(object, "phase");
    if (table.phase.isEmpty())
        table.phase = QStringLiteral("waiting");
    table.board = parseCards(array(object, "board"));
    for (const QJsonValue &value : array(object, "pots")) {
        const QJsonObject pot = value.toObject();
        PokerPot entry;
        entry.amount = number(pot, "amount");
        for (const QJsonValue &seat : pot.value(QStringLiteral("eligible")).toArray())
            entry.eligible << seat.toInt();
        table.pots << entry;
    }
    table.potTotal = number(object, "potTotal");
    table.dealer = integer(object, "dealer", -1);
    table.smallBlind = integer(object, "smallBlind", -1);
    table.bigBlind = integer(object, "bigBlind", -1);
    table.toAct = integer(object, "toAct", -1);
    table.actStartedAt = number(object, "actStartedAt");
    table.actDeadline = number(object, "actDeadline");
    table.currentBet = number(object, "currentBet");
    table.minRaiseTo = number(object, "minRaiseTo");
    table.wakeAt = number(object, "wakeAt");
    table.mySeat = object.value(QStringLiteral("mySeat")).isDouble() ? integer(object, "mySeat", -1) : -1;
    table.wallet = number(object, "wallet");
    const QJsonArray seats = array(object, "seats");
    for (int index = 0; index < seats.size(); ++index)
        table.seats << parseSeat(seats.at(index).toObject(), index);
    if (table.seatCount <= 0)
        table.seatCount = int(table.seats.size());
    for (int index = int(table.seats.size()); index < table.seatCount; ++index) {
        PokerSeat empty;
        empty.index = index;
        table.seats << empty;
    }
    const QJsonValue legalValue = object.value(QStringLiteral("legal"));
    if (legalValue.isObject()) {
        const QJsonObject legal = legalValue.toObject();
        table.legal.present = true;
        table.legal.fold = legal.value(QStringLiteral("fold")).toBool(true);
        table.legal.call = number(legal, "call");
        table.legal.allIn = number(legal, "allIn");
        const QJsonValue raise = legal.value(QStringLiteral("raise"));
        if (raise.isObject()) {
            table.legal.canRaise = true;
            table.legal.raiseMin = number(raise.toObject(), "min");
            table.legal.raiseMax = number(raise.toObject(), "max");
            if (table.legal.raiseMax < table.legal.raiseMin)
                table.legal.raiseMax = table.legal.raiseMin;
        }
    }
    const QJsonObject myHand = object.value(QStringLiteral("myHand")).toObject();
    table.myHandName = text(myHand, "name");
    table.myHandCards = parseCards(array(myHand, "cards"));
    table.dealerLine = text(object, "dealerLine");
    table.commitment = text(object, "commitment");
    for (const QJsonValue &value : array(object, "events")) {
        const QJsonObject event = value.toObject();
        PokerEvent entry;
        entry.seq = number(event, "seq");
        entry.kind = text(event, "kind");
        entry.seat = integer(event, "seat", -1);
        entry.amount = number(event, "amount");
        table.events << entry;
    }
    std::sort(table.events.begin(), table.events.end(),
              [](const PokerEvent &left, const PokerEvent &right) { return left.seq < right.seq; });
    for (const QJsonValue &value : array(object, "log")) {
        const QJsonObject line = value.toObject();
        table.log << PokerLogLine{number(line, "at"), integer(line, "seat", -1), text(line, "name"),
                                  text(line, "kind"), number(line, "amount"), text(line, "text")};
    }
    for (const QJsonValue &value : array(object, "chat")) {
        const QJsonObject line = value.toObject();
        table.chat << PokerChatLine{number(line, "at"), text(line, "name"), text(line, "text")};
    }
    const QJsonObject last = object.value(QStringLiteral("lastHand")).toObject();
    table.lastHand.number = integer(last, "number");
    table.lastHand.board = parseCards(array(last, "board"));
    for (const QJsonValue &value : array(last, "winners")) {
        const QJsonObject winner = value.toObject();
        table.lastHand.winners << PokerWinner{integer(winner, "seat", -1), text(winner, "name"),
                                              number(winner, "amount"), text(winner, "handName"),
                                              parseCards(array(winner, "cards")),
                                              parseCards(array(winner, "best"))};
    }
    for (const QJsonValue &value : array(last, "shown")) {
        const QJsonObject shown = value.toObject();
        table.lastHand.shown << PokerShown{integer(shown, "seat", -1), text(shown, "name"),
                                           parseCards(array(shown, "cards")), text(shown, "handName")};
    }
    table.lastHand.commitment = text(last, "commitment");
    const QJsonObject reveal = last.value(QStringLiteral("reveal")).toObject();
    table.lastHand.hashes = strings(reveal.value(QStringLiteral("hashes")).toArray());
    for (const QJsonValue &value : reveal.value(QStringLiteral("cards")).toArray()) {
        const QJsonObject card = value.toObject();
        table.lastHand.revealed << PokerRevealedCard{integer(card, "index", -1), text(card, "card"), text(card, "salt")};
    }
    for (const QJsonValue &value : array(object, "history")) {
        const QJsonObject line = value.toObject();
        table.history << PokerHistoryLine{integer(line, "number"), strings(array(line, "winners")),
                                          number(line, "amount"), text(line, "handName"), number(line, "at")};
    }
    return table;
}

const PokerSeat *PokerTable::seat(int index) const
{
    for (const PokerSeat &entry : seats)
        if (entry.index == index)
            return &entry;
    return nullptr;
}

const PokerSeat *PokerTable::mine() const
{
    return mySeat >= 0 ? seat(mySeat) : nullptr;
}

bool PokerTable::handRunning() const
{
    return phase == QLatin1String("preflop") || phase == QLatin1String("flop") ||
           phase == QLatin1String("turn") || phase == QLatin1String("river");
}

qint64 PokerTable::streetBets() const
{
    qint64 total = 0;
    for (const PokerSeat &entry : seats)
        total += entry.empty ? 0 : entry.bet;
    return total;
}

QList<int> PokerTable::botSeats() const
{
    QList<int> out;
    for (const PokerSeat &entry : seats)
        if (!entry.empty && entry.bot)
            out << entry.index;
    return out;
}

QList<int> PokerTable::emptySeats() const
{
    QList<int> out;
    for (const PokerSeat &entry : seats)
        if (entry.empty)
            out << entry.index;
    return out;
}

PokerWallet PokerWallet::parse(const QJsonObject &object)
{
    PokerWallet wallet;
    wallet.chips = number(object, "chips");
    wallet.onTables = number(object, "onTables");
    wallet.canClaim = flag(object, "canClaim");
    wallet.claimAmount = number(object, "claimAmount", 2000);
    wallet.nextClaimAt = number(object, "nextClaimAt");
    wallet.handsPlayed = integer(object, "handsPlayed");
    wallet.handsWon = integer(object, "handsWon");
    wallet.biggestPot = number(object, "biggestPot");
    wallet.bestHand = text(object, "bestHand");
    return wallet;
}

PokerLobby PokerLobby::parse(const QJsonObject &object)
{
    PokerLobby lobby;
    lobby.valid = object.contains(QStringLiteral("wallet"));
    lobby.actor = text(object, "actor");
    lobby.wallet = PokerWallet::parse(object.value(QStringLiteral("wallet")).toObject());
    for (const QJsonValue &value : array(object, "stakes")) {
        const QJsonObject stake = value.toObject();
        PokerStake entry{text(stake, "id"), text(stake, "label"), number(stake, "small"), number(stake, "big"),
                         number(stake, "minBuyIn"), number(stake, "maxBuyIn")};
        if (!entry.id.isEmpty())
            lobby.stakes << entry;
    }
    if (lobby.stakes.isEmpty())
        lobby.stakes = defaultStakes();
    for (const QJsonValue &value : array(object, "tables")) {
        const QJsonObject row = value.toObject();
        PokerLobbyTable entry;
        entry.id = text(row, "id");
        if (entry.id.isEmpty())
            continue;
        entry.name = text(row, "name");
        entry.code = text(row, "code");
        entry.isPrivate = flag(row, "private");
        entry.owner = text(row, "owner");
        entry.stakes = text(row, "stakes");
        entry.small = number(row, "small");
        entry.big = number(row, "big");
        entry.minBuyIn = number(row, "minBuyIn");
        entry.maxBuyIn = number(row, "maxBuyIn");
        entry.seats = integer(row, "seats");
        entry.players = integer(row, "players");
        entry.humans = integer(row, "humans");
        entry.bots = integer(row, "bots");
        entry.handNumber = integer(row, "handNumber");
        entry.turnSeconds = integer(row, "turnSeconds", 30);
        entry.mine = flag(row, "mine");
        entry.invited = flag(row, "invited");
        entry.updatedAt = number(row, "updatedAt");
        lobby.tables << entry;
    }
    lobby.now = number(object, "now");
    return lobby;
}

const PokerStake *PokerLobby::stake(const QString &id) const
{
    for (const PokerStake &entry : stakes)
        if (entry.id == id)
            return &entry;
    return nullptr;
}

QList<PokerStake> defaultStakes()
{
    return {
        {QStringLiteral("starter"), QStringLiteral("Starter"), 5, 10, 200, 1000},
        {QStringLiteral("low"), QStringLiteral("Low"), 25, 50, 1000, 5000},
        {QStringLiteral("mid"), QStringLiteral("Mid"), 100, 200, 4000, 20000},
        {QStringLiteral("high"), QStringLiteral("High"), 500, 1000, 20000, 100000},
    };
}

QString amountText(qint64 amount)
{
    const bool negative = amount < 0;
    QString digits = QString::number(negative ? -amount : amount);
    for (int position = int(digits.size()) - 3; position > 0; position -= 3)
        digits.insert(position, QLatin1Char(','));
    return negative ? QStringLiteral("-") + digits : digits;
}

QString shortAmountText(qint64 amount)
{
    const qint64 magnitude = amount < 0 ? -amount : amount;
    if (magnitude < 10000)
        return amountText(amount);
    const auto scaled = [amount](double unit, const char *suffix) {
        const double value = double(amount) / unit;
        const int decimals = std::fabs(value) < 100 ? 1 : 0;
        QString number = QString::number(value, 'f', decimals);
        if (number.endsWith(QLatin1String(".0")))
            number.chop(2);
        return number + QLatin1String(suffix);
    };
    if (magnitude < 1000000)
        return scaled(1000.0, "K");
    return scaled(1000000.0, "M");
}

QString stakesLabel(qint64 small, qint64 big)
{
    return amountText(small) + QStringLiteral(" / ") + amountText(big);
}

QString countdownText(qint64 untilMs)
{
    if (untilMs <= 0)
        return QStringLiteral("now");
    const qint64 minutes = (untilMs + 59999) / 60000;
    if (minutes < 60)
        return QStringLiteral("%1m").arg(minutes);
    const qint64 hours = minutes / 60;
    const qint64 rest = minutes % 60;
    return rest == 0 ? QStringLiteral("%1h").arg(hours) : QStringLiteral("%1h %2m").arg(hours).arg(rest);
}

QString claimLabel(const PokerWallet &wallet, qint64 now)
{
    if (wallet.canClaim || (wallet.nextClaimAt > 0 && now >= wallet.nextClaimAt))
        return QStringLiteral("Claim %1 free chips").arg(amountText(wallet.claimAmount));
    if (wallet.nextClaimAt <= 0)
        return QStringLiteral("Free chips every four hours");
    return QStringLiteral("Free chips in %1").arg(countdownText(wallet.nextClaimAt - now));
}

QString errorText(const QString &code, const QString &fallback)
{
    static const QList<QPair<const char *, const char *>> messages{
        {"claim_not_ready", "Your free chips are not ready yet."},
        {"invalid_session", "Your ForkMesh session ran out. Sign in again."},
        {"not_found", "That was not found."},
        {"table_not_found", "That table is closed or the code is wrong."},
        {"table_full", "Every seat at that table is taken."},
        {"seat_taken", "Someone just took that seat. Pick another."},
        {"already_seated", "You already have a seat at this table."},
        {"not_seated", "You are not seated at this table."},
        {"not_enough_chips", "You do not have enough chips in your wallet."},
        {"invalid_buy_in", "That buy-in is outside this table's range."},
        {"too_many_tables", "You already own five open tables. Close one first."},
        {"owner_only", "Only the table's owner can do that."},
        {"hand_in_progress", "Wait for the hand to finish."},
        {"invalid_stakes", "Pick one of the listed stakes."},
        {"invalid_text", "That message cannot be sent."},
        {"hand_changed", "The hand moved on before your action arrived."},
        {"not_your_turn", "It is not your turn."},
        {"invalid_amount", "That amount is not allowed right now."},
        {"invalid_action", "That move is not allowed right now."},
        {"hand_not_running", "No hand is being played right now."},
        {"plugin_not_installed", "Poker is not installed for your account. Install it from the Market."},
    };
    for (const auto &entry : messages)
        if (code == QLatin1String(entry.first))
            return QString::fromUtf8(entry.second);
    if (!fallback.isEmpty())
        return fallback;
    return code.isEmpty() ? QStringLiteral("Something went wrong.") : code;
}

QString phaseLabel(const QString &phase)
{
    if (phase == QLatin1String("preflop"))
        return QStringLiteral("Pre-flop");
    if (phase == QLatin1String("flop"))
        return QStringLiteral("Flop");
    if (phase == QLatin1String("turn"))
        return QStringLiteral("Turn");
    if (phase == QLatin1String("river"))
        return QStringLiteral("River");
    if (phase == QLatin1String("showdown"))
        return QStringLiteral("Showdown");
    return QStringLiteral("Waiting for players");
}

QString actionTag(const PokerAction &action)
{
    const QString kind = action.kind;
    if (kind.isEmpty())
        return {};
    if (kind == QLatin1String("check"))
        return QStringLiteral("Check");
    if (kind == QLatin1String("fold"))
        return QStringLiteral("Fold");
    if (kind == QLatin1String("call"))
        return QStringLiteral("Call %1").arg(amountText(action.amount));
    if (kind == QLatin1String("bet"))
        return QStringLiteral("Bet %1").arg(amountText(action.amount));
    if (kind == QLatin1String("raise"))
        return QStringLiteral("Raise %1").arg(amountText(action.amount));
    if (kind == QLatin1String("allin") || kind == QLatin1String("all in"))
        return QStringLiteral("All-in %1").arg(amountText(action.amount));
    if (kind == QLatin1String("sb") || kind == QLatin1String("small blind"))
        return QStringLiteral("SB %1").arg(amountText(action.amount));
    if (kind == QLatin1String("bb") || kind == QLatin1String("big blind"))
        return QStringLiteral("BB %1").arg(amountText(action.amount));
    if (kind == QLatin1String("blind") || kind == QLatin1String("blinds"))
        return QStringLiteral("Blind %1").arg(amountText(action.amount));
    if (kind == QLatin1String("timeout"))
        return QStringLiteral("Timed out");
    return action.amount > 0 ? QStringLiteral("%1 %2").arg(capitalized(kind), amountText(action.amount))
                             : capitalized(kind);
}

QString statusLabel(const PokerSeat &seat)
{
    if (seat.empty)
        return QStringLiteral("Empty seat");
    if (seat.status == QLatin1String("leaving"))
        return QStringLiteral("Leaving");
    if (seat.away)
        return QStringLiteral("Sitting out");
    if (seat.status == QLatin1String("folded"))
        return QStringLiteral("Folded");
    if (seat.status == QLatin1String("allin"))
        return QStringLiteral("All-in");
    if (seat.status == QLatin1String("out"))
        return seat.stack <= 0 ? QStringLiteral("Busted") : QStringLiteral("Next hand");
    return {};
}

QString sha256Hex(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString cardHash(const QString &salt, int index, const QString &card)
{
    return sha256Hex(salt + QLatin1Char(':') + QString::number(index) + QLatin1Char(':') + card);
}

QString hashesCommitment(const QStringList &hashes)
{
    return sha256Hex(hashes.join(QLatin1Char(',')));
}

bool revealMatches(const PokerLastHand &hand)
{
    if (!hand.valid() || hand.hashes.size() != 52 || hand.commitment.isEmpty())
        return false;
    if (hashesCommitment(hand.hashes).compare(hand.commitment, Qt::CaseInsensitive) != 0)
        return false;
    for (const PokerRevealedCard &card : hand.revealed) {
        if (card.index < 0 || card.index >= hand.hashes.size() || card.salt.isEmpty() || card.card.isEmpty())
            return false;
        if (cardHash(card.salt, card.index, card.card).compare(hand.hashes.at(card.index), Qt::CaseInsensitive) != 0)
            return false;
    }
    return true;
}

bool isValidTableCode(const QString &code)
{
    const QString normalized = normalizedTableCode(code);
    if (normalized.size() != 6)
        return false;
    for (const QChar character : normalized)
        if (character < QLatin1Char('A') || character > QLatin1Char('Z'))
            return false;
    return true;
}

QString normalizedTableCode(const QString &code)
{
    QString out;
    for (const QChar character : code)
        if (!character.isSpace() && character != QLatin1Char('-'))
            out += character.toUpper();
    return out;
}

QString callButtonLabel(const PokerLegal &legal)
{
    if (!legal.present || legal.call <= 0)
        return QStringLiteral("Check");
    if (legal.allIn > 0 && legal.call >= legal.allIn)
        return QStringLiteral("Call all-in %1").arg(amountText(legal.allIn));
    return QStringLiteral("Call %1").arg(amountText(legal.call));
}

QString raiseVerb(const PokerLegal &, qint64 currentBet)
{
    return currentBet <= 0 ? QStringLiteral("Bet") : QStringLiteral("Raise to");
}

QString raiseButtonLabel(const PokerLegal &legal, qint64 currentBet, qint64 amount)
{
    if (legal.canRaise && amount >= legal.raiseMax)
        return QStringLiteral("All-in %1").arg(amountText(legal.raiseMax));
    return QStringLiteral("%1 %2").arg(raiseVerb(legal, currentBet), amountText(amount));
}

QString raiseAction(qint64 currentBet)
{
    return currentBet <= 0 ? QStringLiteral("bet") : QStringLiteral("raise");
}

qint64 clampRaise(const PokerLegal &legal, qint64 amount)
{
    if (!legal.canRaise)
        return 0;
    return std::clamp(amount, legal.raiseMin, legal.raiseMax);
}

qint64 potFractionRaise(const PokerTable &table, double fraction)
{
    if (!table.legal.canRaise)
        return 0;
    const PokerSeat *seat = table.mine();
    const qint64 myBet = seat ? seat->bet : 0;
    const qint64 call = table.legal.call;
    const qint64 target = myBet + call + qint64(std::llround(fraction * double(table.potTotal + call)));
    return clampRaise(table.legal, target);
}

QList<qint64> chipDenominations()
{
    return {5000, 1000, 500, 100, 25, 5, 1};
}

QString chipColor(qint64 denomination)
{
    switch (denomination) {
    case 5000: return QStringLiteral("#ef6c00");
    case 1000: return QStringLiteral("#f9a825");
    case 500: return QStringLiteral("#6a1b9a");
    case 100: return QStringLiteral("#212121");
    case 25: return QStringLiteral("#2e7d32");
    case 5: return QStringLiteral("#c62828");
    default: return QStringLiteral("#f5f5f5");
    }
}

QList<QPair<qint64, int>> chipBreakdown(qint64 amount, int maxChips)
{
    QList<QPair<qint64, int>> out;
    if (amount <= 0)
        return out;
    qint64 rest = amount;
    for (const qint64 denomination : chipDenominations()) {
        const qint64 count = rest / denomination;
        if (count <= 0)
            continue;
        out << qMakePair(denomination, int(std::min<qint64>(count, 999)));
        rest -= count * denomination;
    }
    int total = 0;
    for (const auto &entry : out)
        total += entry.second;
    const int cap = std::max(1, maxChips);
    while (total > cap && out.size() > 1) {
        total -= out.last().second;
        out.removeLast();
    }
    if (total > cap)
        out.first().second = cap;
    return out;
}

int rotatedSlot(int seatIndex, int mySeat, int seatCount)
{
    if (seatCount <= 0)
        return 0;
    const int base = mySeat >= 0 && mySeat < seatCount ? mySeat : 0;
    return ((seatIndex - base) % seatCount + seatCount) % seatCount;
}

double seatFraction(int slot, int seatCount)
{
    if (seatCount <= 1 || slot <= 0)
        return 0.0;
    return double(slot % seatCount) / double(seatCount);
}

PokerClock turnClock(qint64 now, qint64 started, qint64 deadline)
{
    PokerClock clock;
    if (deadline <= 0 || deadline <= started)
        return clock;
    clock.running = true;
    clock.remaining = std::max<double>(0.0, double(deadline - now) / 1000.0);
    clock.fraction = std::clamp(double(deadline - now) / double(deadline - started), 0.0, 1.0);
    if (clock.remaining < 5)
        clock.color = QStringLiteral("#e53935");
    else if (clock.remaining < 10)
        clock.color = QStringLiteral("#ffb300");
    else
        clock.color = QStringLiteral("#43a047");
    return clock;
}

qint64 wakeDelay(qint64 wakeAt, qint64 serverNow, qint64 margin)
{
    if (wakeAt <= 0)
        return -1;
    return std::max<qint64>(0, wakeAt - serverNow) + margin;
}

QList<PokerEvent> eventsAfter(const QList<PokerEvent> &events, qint64 seq)
{
    QList<PokerEvent> out;
    for (const PokerEvent &event : events)
        if (event.seq > seq)
            out << event;
    std::sort(out.begin(), out.end(),
              [](const PokerEvent &left, const PokerEvent &right) { return left.seq < right.seq; });
    return out;
}

qint64 highestSeq(const QList<PokerEvent> &events)
{
    qint64 highest = 0;
    for (const PokerEvent &event : events)
        highest = std::max(highest, event.seq);
    return highest;
}

QString soundForEvent(const PokerEvent &event, int mySeat)
{
    const QString &kind = event.kind;
    if (kind == QLatin1String("shuffle"))
        return QStringLiteral("shuffle");
    if (kind == QLatin1String("deal") || kind == QLatin1String("flop") || kind == QLatin1String("turn") ||
        kind == QLatin1String("river"))
        return QStringLiteral("deal");
    if (kind == QLatin1String("blinds") || kind == QLatin1String("call") || kind == QLatin1String("bet") ||
        kind == QLatin1String("raise"))
        return QStringLiteral("chip");
    if (kind == QLatin1String("check"))
        return QStringLiteral("check");
    if (kind == QLatin1String("fold") || kind == QLatin1String("timeout"))
        return QStringLiteral("fold");
    if (kind == QLatin1String("allin"))
        return QStringLiteral("allin");
    if (kind == QLatin1String("win"))
        return mySeat >= 0 && event.seat == mySeat ? QStringLiteral("win") : QStringLiteral("chip");
    return {};
}

bool streetEndEvent(const QString &kind)
{
    return kind == QLatin1String("flop") || kind == QLatin1String("turn") || kind == QLatin1String("river") ||
           kind == QLatin1String("showdown");
}

QPointF stadiumPoint(const QRectF &input, double fraction, double grow)
{
    const QRectF rect = input.adjusted(-grow, -grow, grow, grow);
    const double radius = rect.height() / 2.0;
    const double straight = std::max(0.0, rect.width() / 2.0 - radius);
    const QPointF centre = rect.center();
    const double perimeter = 4 * straight + 2 * M_PI * radius;
    double along = std::fmod(fraction, 1.0);
    if (along < 0)
        along += 1.0;
    double distance = along * perimeter;
    if (distance <= straight)
        return {centre.x() - distance, rect.bottom()};
    distance -= straight;
    const double arc = M_PI * radius;
    if (distance <= arc) {
        const double angle = M_PI / 2 + distance / radius;
        return {centre.x() - straight + radius * std::cos(angle), centre.y() + radius * std::sin(angle)};
    }
    distance -= arc;
    if (distance <= 2 * straight)
        return {centre.x() - straight + distance, rect.top()};
    distance -= 2 * straight;
    if (distance <= arc) {
        const double angle = -M_PI / 2 + distance / radius;
        return {centre.x() + straight + radius * std::cos(angle), centre.y() + radius * std::sin(angle)};
    }
    distance -= arc;
    return {centre.x() + straight - distance, rect.bottom()};
}

QPointF towards(const QPointF &from, const QPointF &to, double distance)
{
    const QPointF delta = to - from;
    const double length = std::hypot(delta.x(), delta.y());
    if (length < 1e-6)
        return from;
    return from + delta * (distance / length);
}

PokerLayout PokerLayout::compute(const QSizeF &size, int seatCount, int mySeat)
{
    PokerLayout layout;
    const double width = std::max(1.0, size.width());
    const double height = std::max(1.0, size.height());
    layout.room = QRectF(0, 0, width, height);
    double scale = std::clamp(std::min(width / 1060.0, height / 640.0), 0.42, 2.2);
    double marginX = 66 * scale;
    double marginTop = 74 * scale;
    double marginBottom = 78 * scale;
    double railWidth = width - 2 * marginX;
    double railHeight = height - marginTop - marginBottom;
    if (railWidth > railHeight * 2.05)
        railWidth = railHeight * 2.05;
    if (railHeight > railWidth / 1.6)
        railHeight = railWidth / 1.6;
    scale = std::clamp(railHeight / 500.0, 0.42, 2.2);
    const double top = marginTop + std::max(0.0, (height - marginTop - marginBottom - railHeight) / 2.0);
    layout.rail = QRectF((width - railWidth) / 2.0, top, railWidth, railHeight);
    layout.scale = scale;
    const double walnut = 17 * scale;
    const double leather = 15 * scale;
    layout.felt = layout.rail.adjusted(walnut + leather, walnut + leather, -walnut - leather, -walnut - leather);
    layout.inset = layout.felt.adjusted(20 * scale, 20 * scale, -20 * scale, -20 * scale);
    layout.centre = layout.felt.center();
    layout.card = QSizeF(52 * scale, 74 * scale);
    layout.myCard = QSizeF(60 * scale, 85 * scale);
    layout.avatarRadius = 27 * scale;
    layout.chipRadius = 11 * scale;
    const double gap = 8 * scale;
    const double boardWidth = 5 * layout.card.width() + 4 * gap;
    layout.board = QRectF(layout.centre.x() - boardWidth / 2.0, layout.centre.y() - layout.card.height() / 2.0 + 6 * scale,
                          boardWidth, layout.card.height());
    layout.pot = QPointF(layout.centre.x(), layout.board.top() - 30 * scale);
    layout.dealer = layout.pot;
    const QRectF seatRing = layout.rail.adjusted(walnut * 0.9, walnut * 0.9, -walnut * 0.9, -walnut * 0.9);
    const int count = std::max(0, seatCount);
    for (int index = 0; index < count; ++index) {
        PokerSeatGeometry geometry;
        geometry.seat = index;
        geometry.slot = rotatedSlot(index, mySeat, count);
        geometry.bottom = geometry.slot == 0;
        const double fraction = seatFraction(geometry.slot, count);
        geometry.anchor = stadiumPoint(layout.rail, fraction, 0);
        geometry.avatar = stadiumPoint(seatRing, fraction, 0);
        const double radius = geometry.bottom ? layout.avatarRadius * 1.12 : layout.avatarRadius;
        const QPointF inward = towards(geometry.avatar, layout.centre, 1.0) - geometry.avatar;
        const QPointF side(-inward.y(), inward.x());
        if (geometry.bottom) {
            geometry.cards = geometry.avatar + QPointF(0, -(radius + layout.myCard.height() * 0.42 + 2 * scale));
            geometry.bet = QPointF(geometry.avatar.x(), geometry.cards.y() - layout.myCard.height() * 0.5 - 24 * scale);
            geometry.button = geometry.cards + QPointF(layout.myCard.width() * 1.35 + 14 * scale, 18 * scale);
        } else {
            const QSizeF card = layout.card * 0.72;
            geometry.cards = geometry.avatar + inward * (radius + card.height() * 0.5 + 4 * scale);
            const double toCentre = std::hypot(layout.centre.x() - geometry.avatar.x(),
                                               layout.centre.y() - geometry.avatar.y());
            const double reach = std::max(toCentre * 0.40, radius + card.height() + 22 * scale);
            geometry.bet = geometry.avatar + inward * reach;
            geometry.button = geometry.cards + inward * (card.height() * 0.5 + 16 * scale) + side * (card.width() * 1.1);
        }
        geometry.tag = geometry.avatar + QPointF(radius * 0.78, -radius * 1.02);
        layout.seats << geometry;
    }
    return layout;
}

PokerLayout PokerLayout::compute(const QRectF &area, int seatCount, int mySeat)
{
    PokerLayout layout = compute(area.size(), seatCount, mySeat);
    const QPointF offset = area.topLeft();
    for (QRectF *rect : {&layout.room, &layout.rail, &layout.felt, &layout.inset, &layout.board})
        rect->translate(offset);
    for (QPointF *point : {&layout.centre, &layout.dealer, &layout.pot})
        *point += offset;
    for (PokerSeatGeometry &geometry : layout.seats)
        for (QPointF *point : {&geometry.anchor, &geometry.avatar, &geometry.cards, &geometry.bet, &geometry.tag,
                               &geometry.button})
            *point += offset;
    return layout;
}

QRectF PokerLayout::seatBounds(const PokerSeatGeometry &geometry, double scale, double avatarRadius)
{
    const double radius = geometry.bottom ? avatarRadius * 1.12 : avatarRadius;
    const double reach = radius + 60 * scale;
    return {geometry.avatar.x() - 64 * scale, geometry.avatar.y() - reach, 128 * scale, 2 * reach};
}

const PokerSeatGeometry *PokerLayout::seat(int index) const
{
    for (const PokerSeatGeometry &geometry : seats)
        if (geometry.seat == index)
            return &geometry;
    return nullptr;
}

QPointF PokerLayout::boardCard(int index) const
{
    const double gap = 8 * scale;
    return {board.left() + card.width() / 2.0 + index * (card.width() + gap), board.center().y()};
}

}
