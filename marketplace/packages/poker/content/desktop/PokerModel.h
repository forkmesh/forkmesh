#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

namespace forkmesh::poker {

struct PokerCard {
    QChar rank;
    QChar suit;
    bool hidden = false;
    bool valid = false;

    static PokerCard parse(const QString &code);
    QString code() const;
    QString rankLabel() const;
    QString suitSymbol() const;
    bool red() const;
    QString label() const;
};

QList<PokerCard> parseCards(const QJsonArray &cards);
QString cardsLabel(const QList<PokerCard> &cards);

struct PokerAction {
    QString kind;
    qint64 amount = 0;
};

struct PokerSeat {
    int index = -1;
    bool empty = true;
    QString name;
    QString label;
    bool bot = false;
    qint64 stack = 0;
    qint64 bet = 0;
    qint64 totalBet = 0;
    QString status;
    QList<PokerCard> cards;
    PokerAction lastAction;
    bool isTurn = false;
    bool away = false;
    QString handName;
    bool winner = false;
    qint64 won = 0;

    bool inHand() const;
    bool busted() const;
    QString displayName() const;
};

struct PokerLegal {
    bool present = false;
    bool fold = false;
    qint64 call = 0;
    bool canRaise = false;
    qint64 raiseMin = 0;
    qint64 raiseMax = 0;
    qint64 allIn = 0;

    bool canCheck() const { return present && call == 0; }
};

struct PokerPot {
    qint64 amount = 0;
    QList<int> eligible;
};

struct PokerEvent {
    qint64 seq = 0;
    QString kind;
    int seat = -1;
    qint64 amount = 0;
};

struct PokerLogLine {
    qint64 at = 0;
    int seat = -1;
    QString name;
    QString kind;
    qint64 amount = 0;
    QString text;
};

struct PokerChatLine {
    qint64 at = 0;
    QString name;
    QString text;
};

struct PokerWinner {
    int seat = -1;
    QString name;
    qint64 amount = 0;
    QString handName;
    QList<PokerCard> cards;
    QList<PokerCard> best;
};

struct PokerShown {
    int seat = -1;
    QString name;
    QList<PokerCard> cards;
    QString handName;
};

struct PokerRevealedCard {
    int index = -1;
    QString card;
    QString salt;
};

struct PokerLastHand {
    int number = 0;
    QList<PokerCard> board;
    QList<PokerWinner> winners;
    QList<PokerShown> shown;
    QString commitment;
    QStringList hashes;
    QList<PokerRevealedCard> revealed;

    bool valid() const { return number > 0; }
};

struct PokerHistoryLine {
    int number = 0;
    QStringList winners;
    qint64 amount = 0;
    QString handName;
    qint64 at = 0;
};

struct PokerTable {
    bool valid = false;
    QString id;
    QString name;
    QString code;
    bool isPrivate = false;
    QString owner;
    bool ownerIsMe = false;
    QString stakes;
    qint64 small = 0;
    qint64 big = 0;
    qint64 minBuyIn = 0;
    qint64 maxBuyIn = 0;
    int seatCount = 0;
    int turnSeconds = 30;
    qint64 version = 0;
    qint64 now = 0;
    int handNumber = 0;
    QString phase;
    QList<PokerCard> board;
    QList<PokerPot> pots;
    qint64 potTotal = 0;
    int dealer = -1;
    int smallBlind = -1;
    int bigBlind = -1;
    int toAct = -1;
    qint64 actStartedAt = 0;
    qint64 actDeadline = 0;
    qint64 currentBet = 0;
    qint64 minRaiseTo = 0;
    qint64 wakeAt = 0;
    int mySeat = -1;
    qint64 wallet = 0;
    QList<PokerSeat> seats;
    PokerLegal legal;
    QString myHandName;
    QList<PokerCard> myHandCards;
    QString dealerLine;
    QString commitment;
    QList<PokerEvent> events;
    QList<PokerLogLine> log;
    QList<PokerChatLine> chat;
    PokerLastHand lastHand;
    QList<PokerHistoryLine> history;

    static PokerTable parse(const QJsonObject &object);
    const PokerSeat *seat(int index) const;
    const PokerSeat *mine() const;
    bool seated() const { return mySeat >= 0; }
    bool handRunning() const;
    qint64 streetBets() const;
    QList<int> botSeats() const;
    QList<int> emptySeats() const;
    bool myTurn() const { return seated() && toAct == mySeat && legal.present; }
};

struct PokerWallet {
    qint64 chips = 0;
    qint64 onTables = 0;
    bool canClaim = false;
    qint64 claimAmount = 0;
    qint64 nextClaimAt = 0;
    int handsPlayed = 0;
    int handsWon = 0;
    qint64 biggestPot = 0;
    QString bestHand;

    static PokerWallet parse(const QJsonObject &object);
};

struct PokerStake {
    QString id;
    QString label;
    qint64 small = 0;
    qint64 big = 0;
    qint64 minBuyIn = 0;
    qint64 maxBuyIn = 0;
};

struct PokerLobbyTable {
    QString id;
    QString name;
    QString code;
    bool isPrivate = false;
    QString owner;
    QString stakes;
    qint64 small = 0;
    qint64 big = 0;
    qint64 minBuyIn = 0;
    qint64 maxBuyIn = 0;
    int seats = 0;
    int players = 0;
    int humans = 0;
    int bots = 0;
    int handNumber = 0;
    int turnSeconds = 30;
    bool mine = false;
    bool invited = false;
    qint64 updatedAt = 0;
};

struct PokerLobby {
    bool valid = false;
    QString actor;
    PokerWallet wallet;
    QList<PokerStake> stakes;
    QList<PokerLobbyTable> tables;
    qint64 now = 0;

    static PokerLobby parse(const QJsonObject &object);
    const PokerStake *stake(const QString &id) const;
};

QList<PokerStake> defaultStakes();
QString amountText(qint64 amount);
QString shortAmountText(qint64 amount);
QString stakesLabel(qint64 small, qint64 big);
QString countdownText(qint64 untilMs);
QString claimLabel(const PokerWallet &wallet, qint64 now);
QString errorText(const QString &code, const QString &fallback);
QString phaseLabel(const QString &phase);
QString actionTag(const PokerAction &action);
QString statusLabel(const PokerSeat &seat);
QString sha256Hex(const QString &text);
QString cardHash(const QString &salt, int index, const QString &card);
QString hashesCommitment(const QStringList &hashes);
bool revealMatches(const PokerLastHand &hand);
bool isValidTableCode(const QString &code);
QString normalizedTableCode(const QString &code);

QString callButtonLabel(const PokerLegal &legal);
QString raiseButtonLabel(const PokerLegal &legal, qint64 currentBet, qint64 amount);
QString raiseVerb(const PokerLegal &legal, qint64 currentBet);
QString raiseAction(qint64 currentBet);
qint64 clampRaise(const PokerLegal &legal, qint64 amount);
qint64 potFractionRaise(const PokerTable &table, double fraction);

QList<qint64> chipDenominations();
QString chipColor(qint64 denomination);
QList<QPair<qint64, int>> chipBreakdown(qint64 amount, int maxChips = 14);

int rotatedSlot(int seatIndex, int mySeat, int seatCount);
double seatFraction(int slot, int seatCount);

struct PokerClock {
    bool running = false;
    double remaining = 0;
    double fraction = 0;
    QString color;
};

PokerClock turnClock(qint64 now, qint64 started, qint64 deadline);
qint64 wakeDelay(qint64 wakeAt, qint64 serverNow, qint64 margin = 150);

QList<PokerEvent> eventsAfter(const QList<PokerEvent> &events, qint64 seq);
qint64 highestSeq(const QList<PokerEvent> &events);
QString soundForEvent(const PokerEvent &event, int mySeat);
bool streetEndEvent(const QString &kind);

struct PokerSeatGeometry {
    int seat = -1;
    int slot = 0;
    QPointF anchor;
    QPointF avatar;
    QPointF cards;
    QPointF bet;
    QPointF tag;
    QPointF button;
    bool bottom = false;
};

struct PokerLayout {
    QRectF room;
    QRectF rail;
    QRectF felt;
    QRectF inset;
    QPointF centre;
    QPointF dealer;
    QPointF dealerLine;
    QPointF pot;
    QRectF board;
    QSizeF card;
    QSizeF myCard;
    double avatarRadius = 0;
    double chipRadius = 0;
    double scale = 1;
    QList<PokerSeatGeometry> seats;

    static PokerLayout compute(const QSizeF &size, int seatCount, int mySeat);
    const PokerSeatGeometry *seat(int index) const;
    QPointF boardCard(int index) const;
};

QPointF stadiumPoint(const QRectF &rect, double fraction, double grow = 0);
QPointF towards(const QPointF &from, const QPointF &to, double distance);

}
