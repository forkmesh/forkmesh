import hashlib
import json
import re
import secrets
from itertools import combinations


PREFIX = "/api/poker"
PLUGIN_ID = "poker"
REQUEST_PROOF = "forkmesh-poker-request-v1"
TOPIC = "package:" + PLUGIN_ID
EXPORT_FORMAT = "forkmesh-poker-export-v1"
MAX_BODY_BYTES = 16 * 1024
STARTING_CHIPS = 10_000
CLAIM_AMOUNT = 2_000
CLAIM_INTERVAL_MS = 4 * 3600 * 1000
MIN_SEATS = 2
MAX_SEATS = 9
DEFAULT_SEATS = 6
TURN_CHOICES = (15, 30, 45, 60)
DEFAULT_TURN = 30
MAX_OWNED_TABLES = 5
MAX_LISTED = 60
MAX_EVENTS = 40
MAX_LOG = 30
MAX_CHAT = 40
MAX_HISTORY = 10
MAX_INVITES = 10
CHAT_LIMIT = 200
CHAT_BURST = 5
CHAT_WINDOW_MS = 10_000
WATCH_MS = 10 * 60 * 1000
WATCH_SAVE_MS = 2 * 60 * 1000
IDLE_CLOSE_MS = 3 * 3600 * 1000
IDLE_SWEEP = 10
FIRST_DEAL_MS = 2_000
NEXT_HAND_MS = 5_500
FOLD_WIN_NEXT_MS = 3_200
RUNOUT_MS = 1_600
BOT_MIN_MS = 1_000
BOT_MAX_MS = 2_500
TIMEOUTS_TO_AWAY = 2
WRITE_ATTEMPTS = 5
MC_TRIALS = 140
CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ"
CODE_LENGTH = 6
ID_RE = re.compile(r"^[0-9a-f]{32}$")
CODE_RE = re.compile(r"^[A-Z]{6}$")
NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
STREETS = ("preflop", "flop", "turn", "river")
IN_HAND = ("active", "allin")
ACTIONS = ("fold", "check", "call", "bet", "raise", "allin")
TABLE_ACTIONS = ("sit", "leave", "act", "tick", "away", "rebuy", "bots", "chat", "invite")

STAKES = (
    {"id": "starter", "label": "Starter", "small": 5, "big": 10, "minBuyIn": 200, "maxBuyIn": 1_000},
    {"id": "low", "label": "Low", "small": 25, "big": 50, "minBuyIn": 1_000, "maxBuyIn": 5_000},
    {"id": "mid", "label": "Mid", "small": 100, "big": 200, "minBuyIn": 4_000, "maxBuyIn": 20_000},
    {"id": "high", "label": "High roller", "small": 500, "big": 1_000, "minBuyIn": 20_000, "maxBuyIn": 100_000},
)
STAKES_BY_ID = {item["id"]: item for item in STAKES}

BOTS = (
    ("river-rita", "River Rita", "loose"),
    ("lucky-lou", "Lucky Lou", "aggressive"),
    ("pocket-pete", "Pocket Pete", "tight"),
    ("chip-charlie", "Chip Charlie", "careful"),
    ("bluff-betty", "Bluff Betty", "aggressive"),
    ("flush-frank", "Flush Frank", "loose"),
    ("nuts-nora", "Nuts Nora", "tight"),
    ("turn-tina", "Turn Tina", "careful"),
    ("ace-adams", "Ace Adams", "tight"),
    ("stack-sam", "Stack Sam", "loose"),
)
BOT_NAMES = {name: label for name, label, _persona in BOTS}
PERSONAS = {
    "tight": {"loose": -0.05, "aggression": 0.85, "bluff": 0.04},
    "loose": {"loose": 0.07, "aggression": 1.0, "bluff": 0.08},
    "aggressive": {"loose": 0.02, "aggression": 1.45, "bluff": 0.12},
    "careful": {"loose": -0.02, "aggression": 0.7, "bluff": 0.03},
}

RANKS = "23456789TJQKA"
SUITS = "cdhs"
SUIT_SYMBOLS = {"c": "♣", "d": "♦", "h": "♥", "s": "♠"}
RANK_VALUE = {rank: index + 2 for index, rank in enumerate(RANKS)}
RANK_NAME = {
    2: "Two", 3: "Three", 4: "Four", 5: "Five", 6: "Six", 7: "Seven", 8: "Eight",
    9: "Nine", 10: "Ten", 11: "Jack", 12: "Queen", 13: "King", 14: "Ace",
}
RANK_PLURAL = {
    2: "Twos", 3: "Threes", 4: "Fours", 5: "Fives", 6: "Sixes", 7: "Sevens", 8: "Eights",
    9: "Nines", 10: "Tens", 11: "Jacks", 12: "Queens", 13: "Kings", 14: "Aces",
}
FULL_DECK = tuple(rank + suit for rank in RANKS for suit in SUITS)

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS poker_wallets (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        chips INTEGER NOT NULL CHECK(chips >= 0),
        claimed_at INTEGER NOT NULL DEFAULT 0 CHECK(claimed_at >= 0),
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        stats TEXT NOT NULL DEFAULT '{}')""",
    """CREATE TABLE IF NOT EXISTS poker_tables (
        table_id TEXT PRIMARY KEY,
        code TEXT NOT NULL,
        name TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        stakes TEXT NOT NULL,
        private INTEGER NOT NULL CHECK(private IN (0, 1)),
        seat_count INTEGER NOT NULL CHECK(seat_count >= 2 AND seat_count <= 9),
        turn_seconds INTEGER NOT NULL CHECK(turn_seconds > 0),
        players INTEGER NOT NULL DEFAULT 0 CHECK(players >= 0),
        humans INTEGER NOT NULL DEFAULT 0 CHECK(humans >= 0),
        bots INTEGER NOT NULL DEFAULT 0 CHECK(bots >= 0),
        hand_number INTEGER NOT NULL DEFAULT 0 CHECK(hand_number >= 0),
        version INTEGER NOT NULL DEFAULT 0 CHECK(version >= 0),
        write_token TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_poker_tables_code ON poker_tables(code)",
    "CREATE INDEX IF NOT EXISTS idx_poker_tables_owner ON poker_tables(owner_bi)",
    "CREATE INDEX IF NOT EXISTS idx_poker_tables_listed ON poker_tables(private,updated_at)",
    """CREATE TABLE IF NOT EXISTS poker_seats (
        table_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        account_name TEXT NOT NULL,
        seat INTEGER NOT NULL CHECK(seat >= 0),
        joined_at INTEGER NOT NULL CHECK(joined_at >= 0),
        PRIMARY KEY(table_id,account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_poker_seats_account ON poker_seats(account_bi)",
    """CREATE TABLE IF NOT EXISTS poker_invites (
        table_id TEXT NOT NULL,
        account_name TEXT NOT NULL,
        invited_by TEXT NOT NULL,
        invited_at INTEGER NOT NULL CHECK(invited_at >= 0),
        PRIMARY KEY(table_id,account_name))""",
    "CREATE INDEX IF NOT EXISTS idx_poker_invites_name ON poker_invites(account_name)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_poker_invites_name",
    "DROP TABLE IF EXISTS poker_invites",
    "DROP INDEX IF EXISTS idx_poker_seats_account",
    "DROP TABLE IF EXISTS poker_seats",
    "DROP INDEX IF EXISTS idx_poker_tables_listed",
    "DROP INDEX IF EXISTS idx_poker_tables_owner",
    "DROP INDEX IF EXISTS idx_poker_tables_code",
    "DROP TABLE IF EXISTS poker_tables",
    "DROP TABLE IF EXISTS poker_wallets",
]

_SCHEMA_READY = False
_RNG = secrets.SystemRandom()


def _card(text):
    return RANK_VALUE[text[0]], text[1]


def _straight_high(ranks):
    for high in range(14, 4, -1):
        if all(high - step in ranks for step in range(5)):
            return high
    if {14, 2, 3, 4, 5} <= ranks:
        return 5
    return 0


def evaluate(cards):
    parsed = [_card(card) for card in cards]
    counts = {}
    by_suit = {}
    for rank, suit in parsed:
        counts[rank] = counts.get(rank, 0) + 1
        by_suit.setdefault(suit, []).append(rank)
    flush = None
    for ranks in by_suit.values():
        if len(ranks) >= 5:
            flush = sorted(ranks, reverse=True)
    if flush:
        high = _straight_high(set(flush))
        if high:
            return (8, high)
    ordered = sorted(counts, reverse=True)
    quads = [rank for rank in ordered if counts[rank] == 4]
    trips = [rank for rank in ordered if counts[rank] == 3]
    pairs = [rank for rank in ordered if counts[rank] == 2]
    if quads:
        return (7, quads[0], max(rank for rank in ordered if rank != quads[0]))
    if trips and (len(trips) > 1 or pairs):
        return (6, trips[0], max(trips[1:] + pairs))
    if flush:
        return (5,) + tuple(flush[:5])
    high = _straight_high(set(ordered))
    if high:
        return (4, high)
    if trips:
        return (3, trips[0]) + tuple(rank for rank in ordered if rank != trips[0])[:2]
    if len(pairs) > 1:
        kicker = max(rank for rank in ordered if rank not in pairs[:2])
        return (2, pairs[0], pairs[1], kicker)
    if pairs:
        return (1, pairs[0]) + tuple(rank for rank in ordered if rank != pairs[0])[:3]
    return (0,) + tuple(ordered[:5])


def best_five(cards):
    target = evaluate(cards)
    if len(cards) <= 5:
        return target, list(cards)
    for combo in combinations(cards, 5):
        if evaluate(combo) == target:
            ranked = sorted(combo, key=lambda card: RANK_VALUE[card[0]], reverse=True)
            return target, ranked
    return target, list(cards[:5])


def hand_name(rank):
    category = rank[0]
    if category == 8:
        return "Royal Flush" if rank[1] == 14 else "Straight Flush, %s high" % RANK_NAME[rank[1]]
    if category == 7:
        return "Four of a Kind, %s" % RANK_PLURAL[rank[1]]
    if category == 6:
        return "Full House, %s over %s" % (RANK_PLURAL[rank[1]], RANK_PLURAL[rank[2]])
    if category == 5:
        return "Flush, %s high" % RANK_NAME[rank[1]]
    if category == 4:
        return "Straight, %s high" % RANK_NAME[rank[1]]
    if category == 3:
        return "Three of a Kind, %s" % RANK_PLURAL[rank[1]]
    if category == 2:
        return "Two Pair, %s and %s" % (RANK_PLURAL[rank[1]], RANK_PLURAL[rank[2]])
    if category == 1:
        return "Pair of %s" % RANK_PLURAL[rank[1]]
    return "High Card, %s" % RANK_NAME[rank[1]]


def starting_hand_name(cards):
    if len(cards) != 2:
        return ""
    first, second = sorted((_card(card) for card in cards), reverse=True)
    if first[0] == second[0]:
        return "Pocket %s" % RANK_PLURAL[first[0]]
    return "%s %s %s" % (RANK_NAME[first[0]], RANK_NAME[second[0]],
                         "suited" if first[1] == second[1] else "offsuit")


def card_hashes(deck, salts):
    return [hashlib.sha256(("%s:%d:%s" % (salt, index, card)).encode()).hexdigest()
            for index, (card, salt) in enumerate(zip(deck, salts))]


def commitment(hashes):
    return hashlib.sha256(",".join(hashes).encode()).hexdigest()


def _reveal(state, public):
    deck, salts = list(state.get("dealt") or []), list(state.get("salts") or [])
    wanted = set(public)
    return {
        "hashes": card_hashes(deck, salts),
        "cards": [{"index": index, "card": card, "salt": salts[index]}
                  for index, card in enumerate(deck) if card in wanted],
    }


def build_pots(seats):
    contributions = [(index, seat) for index, seat in enumerate(seats)
                     if seat and int(seat.get("total") or 0) > 0]
    contenders = [index for index, seat in contributions if seat.get("status") in IN_HAND]
    levels = sorted({int(seats[index]["total"]) for index in contenders})
    pots = []
    previous = 0
    for level in levels:
        amount = sum(max(0, min(int(seat["total"]), level) - previous) for _index, seat in contributions)
        eligible = [index for index in contenders if int(seats[index]["total"]) >= level]
        if amount > 0:
            if pots and pots[-1]["eligible"] == eligible:
                pots[-1]["amount"] += amount
            else:
                pots.append({"amount": amount, "eligible": eligible})
        previous = level
    leftover = sum(max(0, int(seat["total"]) - previous) for _index, seat in contributions)
    if leftover and pots:
        pots[-1]["amount"] += leftover
    return pots


def equity(hole, board, opponents, trials=MC_TRIALS, rng=None):
    rng = rng or _RNG
    if opponents <= 0:
        return 1.0
    known = set(hole) | set(board)
    remaining = [card for card in FULL_DECK if card not in known]
    need = 5 - len(board)
    score = 0.0
    for _trial in range(trials):
        drawn = rng.sample(remaining, need + 2 * opponents)
        full = list(board) + drawn[:need]
        mine = evaluate(list(hole) + full)
        best = True
        ties = 0
        for opponent in range(opponents):
            start = need + 2 * opponent
            theirs = evaluate(drawn[start:start + 2] + full)
            if theirs > mine:
                best = False
                break
            if theirs == mine:
                ties += 1
        if best:
            score += 1.0 / (ties + 1)
    return score / trials


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _name(value):
    text = _clean(value, 64).lower()
    return text if NAME_RE.fullmatch(text) else ""


def _integer(value, default=0):
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            number = float(value)
        except (TypeError, ValueError):
            return default
        if number != number or number in (float("inf"), float("-inf")):
            return default
        return int(number)


def _chips(amount):
    return "{:,}".format(int(amount))


def _pretty(cards):
    return " ".join(card[0].replace("T", "10") + SUIT_SYMBOLS[card[1]] for card in cards)


def _label(seat):
    if not seat:
        return ""
    return BOT_NAMES.get(seat["name"], seat["name"]) if seat.get("bot") else seat["name"]


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _error(runtime, code, status=400):
    return _response(runtime, {"ok": False, "error": code}, status)


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ()
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    if any(len(part) > 64 for part in parts):
        return None
    return parts


def _changes(result):
    try:
        return int(result.meta.changes)
    except Exception:
        pass
    try:
        converted = result.to_py() if hasattr(result, "to_py") else result
        if isinstance(converted, dict):
            return int((converted.get("meta") or {}).get("changes") or 0)
    except Exception:
        pass
    count = getattr(result, "rowcount", None)
    if isinstance(count, int) and count >= 0:
        return count
    return None


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _name((record or {}).get("name"))


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


async def _notify(runtime, names):
    ring = getattr(runtime, "notify_account", None)
    if ring is None:
        return
    for name in sorted(set(name for name in names if name)):
        try:
            await ring(name, TOPIC)
        except Exception:
            pass


def _empty_stats():
    return {"handsPlayed": 0, "handsWon": 0, "biggestPot": 0, "bestHand": "", "bestRank": []}


def _stats(row):
    try:
        stored = json.loads(str((row or {}).get("stats") or "{}"))
    except ValueError:
        stored = {}
    stats = _empty_stats()
    if isinstance(stored, dict):
        for key in stats:
            if key in stored:
                stats[key] = stored[key]
    return stats


async def _wallet_row(runtime, account_bi, actor, create=True):
    row = await runtime.d1_first(
        "SELECT account_bi,account_name,chips,claimed_at,created_at,updated_at,stats "
        "FROM poker_wallets WHERE account_bi=?", account_bi)
    if row or not create:
        return row
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO poker_wallets(account_bi,account_name,chips,claimed_at,created_at,updated_at,stats) "
        "VALUES(?,?,?,?,?,?,?) ON CONFLICT(account_bi) DO NOTHING",
        account_bi, actor, STARTING_CHIPS, 0, now, now, json.dumps(_empty_stats()))
    return await runtime.d1_first(
        "SELECT account_bi,account_name,chips,claimed_at,created_at,updated_at,stats "
        "FROM poker_wallets WHERE account_bi=?", account_bi)


async def _on_tables(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT t.table_id,t.data FROM poker_seats s JOIN poker_tables t ON t.table_id=s.table_id "
        "WHERE s.account_bi=?", account_bi) or []
    total = 0
    for row in rows:
        state = await _open_state(runtime, row)
        for seat in state.get("seats") or []:
            if seat and not seat.get("bot") and seat.get("bi") == account_bi:
                total += int(seat.get("stack") or 0) + int(seat.get("rebuy") or 0)
                if state.get("phase") in STREETS:
                    total += int(seat.get("total") or 0)
    return total


def _wallet_view(row, now, on_tables=0):
    claimed = int((row or {}).get("claimed_at") or 0)
    next_at = claimed + CLAIM_INTERVAL_MS if claimed else 0
    stats = _stats(row)
    return {
        "chips": int((row or {}).get("chips") or 0),
        "onTables": int(on_tables),
        "canClaim": not claimed or now >= next_at,
        "claimAmount": CLAIM_AMOUNT,
        "nextClaimAt": next_at,
        "handsPlayed": int(stats["handsPlayed"] or 0),
        "handsWon": int(stats["handsWon"] or 0),
        "biggestPot": int(stats["biggestPot"] or 0),
        "bestHand": str(stats["bestHand"] or ""),
    }


async def _debit(runtime, account_bi, amount):
    if amount <= 0:
        return True
    result = await runtime.d1_run(
        "UPDATE poker_wallets SET chips=chips-?,updated_at=? WHERE account_bi=? AND chips>=?",
        amount, runtime.now(), account_bi, amount)
    changed = _changes(result)
    if changed is not None:
        return changed > 0
    row = await runtime.d1_first("SELECT chips FROM poker_wallets WHERE account_bi=?", account_bi)
    return row is not None


async def _credit(runtime, account_bi, amount):
    if amount <= 0 or not account_bi:
        return
    await runtime.d1_run(
        "UPDATE poker_wallets SET chips=chips+?,updated_at=? WHERE account_bi=?",
        int(amount), runtime.now(), account_bi)


async def _record_stats(runtime, account_bi, update):
    row = await runtime.d1_first("SELECT stats FROM poker_wallets WHERE account_bi=?", account_bi)
    if not row:
        return
    stats = _stats(row)
    stats["handsPlayed"] = int(stats["handsPlayed"] or 0) + 1
    if update.get("won"):
        stats["handsWon"] = int(stats["handsWon"] or 0) + 1
        stats["biggestPot"] = max(int(stats["biggestPot"] or 0), int(update["won"]))
    rank = update.get("rank")
    if rank and list(rank) > list(stats.get("bestRank") or []):
        stats["bestRank"] = list(rank)
        stats["bestHand"] = hand_name(tuple(rank))
    await runtime.d1_run(
        "UPDATE poker_wallets SET stats=?,updated_at=? WHERE account_bi=?",
        json.dumps(stats, separators=(",", ":")), runtime.now(), account_bi)


async def _open_state(runtime, row):
    opened = await runtime.open(row.get("data")) if row and row.get("data") else None
    return opened if isinstance(opened, dict) else {}


async def _table_row(runtime, table_id):
    return await runtime.d1_first(
        "SELECT table_id,code,name,owner_bi,owner_name,stakes,private,seat_count,turn_seconds,"
        "players,humans,bots,hand_number,version,created_at,updated_at,data "
        "FROM poker_tables WHERE table_id=?", table_id)


def _stake(row):
    return STAKES_BY_ID.get(str(row.get("stakes") or ""), STAKES[0])


class _Ctx:
    def __init__(self, row, now):
        self.row = row
        self.now = now
        self.stake = _stake(row)
        self.turn_ms = int(row.get("turn_seconds") or DEFAULT_TURN) * 1000
        self.credits = []
        self.stats = []
        self.seat_rows = []
        self.unseat = []

    @property
    def small(self):
        return int(self.stake["small"])

    @property
    def big(self):
        return int(self.stake["big"])


def _new_state(seat_count):
    return {
        "seats": [None] * seat_count, "hand": 0, "phase": "waiting", "deck": [], "salt": "",
        "commitment": "", "board": [], "dealer": -1, "sb": -1, "bb": -1, "toAct": -1,
        "actStartedAt": 0, "deadline": 0, "currentBet": 0, "lastRaise": 0, "wakeAt": 0,
        "wakeKind": "", "seq": 0, "events": [], "log": [], "chat": [], "history": [],
        "lastHand": None, "dealerLine": "Take a seat. The game starts when two players sit down.",
        "watchers": {},
    }


def _event(state, kind, seat=-1, amount=0):
    state["seq"] = int(state.get("seq") or 0) + 1
    state["events"] = (state.get("events") or [])[-(MAX_EVENTS - 1):] + [
        {"seq": state["seq"], "kind": kind, "seat": int(seat), "amount": int(amount)}]


def _log(state, now, seat, kind, amount, text):
    name = _label(state["seats"][seat]) if 0 <= seat < len(state["seats"]) else "Dealer"
    raw = state["seats"][seat]["name"] if 0 <= seat < len(state["seats"]) and state["seats"][seat] else ""
    state["log"] = (state.get("log") or [])[-(MAX_LOG - 1):] + [
        {"at": now, "seat": int(seat), "name": raw or name, "kind": kind, "amount": int(amount), "text": text}]


def _dealer_says(state, now, text):
    state["dealerLine"] = text
    _log(state, now, -1, "dealer", 0, text)


def _next_index(state, start, test):
    count = len(state["seats"])
    for step in range(1, count + 1):
        index = (start + step) % count
        seat = state["seats"][index]
        if seat and test(seat):
            return index
    return -1


def _eligible(seat):
    return (int(seat.get("stack") or 0) + int(seat.get("rebuy") or 0) > 0
            and not seat.get("away") and not seat.get("leaving"))


def _can_bet(seat):
    return seat.get("status") == "active" and int(seat.get("stack") or 0) > 0


def _put(seat, amount):
    amount = max(0, min(int(amount), int(seat["stack"])))
    seat["stack"] -= amount
    seat["bet"] += amount
    seat["total"] += amount
    if seat["stack"] == 0 and seat["status"] == "active":
        seat["status"] = "allin"
    return amount


def _schedule(state, at, kind):
    state["wakeAt"] = int(at)
    state["wakeKind"] = kind


def _clear_turn(state):
    state["toAct"] = -1
    state["actStartedAt"] = 0
    state["deadline"] = 0


def _counts(state):
    seats = [seat for seat in state["seats"] if seat]
    bots = sum(1 for seat in seats if seat.get("bot"))
    return len(seats), len(seats) - bots, bots


def _remove_seat(state, index, ctx, credit=True):
    seat = state["seats"][index]
    if not seat:
        return
    if not seat.get("bot"):
        if credit and not seat.get("purged"):
            ctx.credits.append((seat["bi"], int(seat.get("stack") or 0) + int(seat.get("rebuy") or 0)))
        ctx.unseat.append(seat["bi"])
    state["seats"][index] = None


def _maybe_schedule_start(state, now):
    if state["phase"] != "waiting" or state.get("wakeAt"):
        return
    if sum(1 for seat in state["seats"] if seat and _eligible(seat)) >= 2:
        _schedule(state, now + FIRST_DEAL_MS, "next")
        _dealer_says(state, now, "Shuffle up and deal. First hand coming up.")


def _start_hand(state, ctx):
    now = ctx.now
    for index, seat in enumerate(state["seats"]):
        if not seat:
            continue
        if seat.get("leaving"):
            _remove_seat(state, index, ctx)
            _event(state, "leave", index)
            continue
        if seat.get("bot") and int(seat.get("stack") or 0) <= 0:
            seat["stack"] = int(ctx.stake["maxBuyIn"])
            _log(state, now, index, "rebuy", seat["stack"], "%s rebuys for %s" % (_label(seat), _chips(seat["stack"])))
        if int(seat.get("rebuy") or 0):
            seat["stack"] = int(seat.get("stack") or 0) + int(seat["rebuy"])
            seat["rebuy"] = 0
        seat.update({"cards": [], "bet": 0, "total": 0, "acted": False, "locked": False,
                     "lastAction": None, "handName": "", "best": [], "won": 0, "winner": False, "shown": False,
                     "status": "active" if _eligible(seat) else "out"})
    players = [index for index, seat in enumerate(state["seats"]) if seat and seat["status"] == "active"]
    state.update({"board": [], "currentBet": 0, "lastRaise": ctx.big, "wakeAt": 0, "wakeKind": ""})
    _clear_turn(state)
    if len(players) < 2:
        state.update({"phase": "waiting", "deck": [], "dealt": [], "salts": [], "salt": "", "commitment": "",
                      "sb": -1, "bb": -1})
        for seat in state["seats"]:
            if seat:
                seat["status"] = "out"
        _dealer_says(state, now, "Waiting for players. The next hand starts when two players are ready.")
        return
    state["hand"] = int(state.get("hand") or 0) + 1
    active = lambda seat: seat["status"] == "active"
    state["dealer"] = _next_index(state, int(state.get("dealer") if state.get("dealer") is not None else -1), active)
    if len(players) == 2:
        state["sb"] = state["dealer"]
    else:
        state["sb"] = _next_index(state, state["dealer"], active)
    state["bb"] = _next_index(state, state["sb"], active)
    deck = list(FULL_DECK)
    _RNG.shuffle(deck)
    salts = [secrets.token_hex(8) for _card in deck]
    state["deck"] = deck
    state["dealt"] = list(deck)
    state["salts"] = salts
    state["salt"] = ""
    state["commitment"] = commitment(card_hashes(deck, salts))
    state["phase"] = "preflop"
    _event(state, "shuffle")
    _dealer_says(state, now, "Hand #%d. Blinds %s / %s." % (state["hand"], _chips(ctx.small), _chips(ctx.big)))
    small = _put(state["seats"][state["sb"]], ctx.small)
    big = _put(state["seats"][state["bb"]], ctx.big)
    state["seats"][state["sb"]]["lastAction"] = {"kind": "small blind", "amount": small}
    state["seats"][state["bb"]]["lastAction"] = {"kind": "big blind", "amount": big}
    _log(state, now, state["sb"], "blind", small, "%s posts the small blind %s" % (_label(state["seats"][state["sb"]]), _chips(small)))
    _log(state, now, state["bb"], "blind", big, "%s posts the big blind %s" % (_label(state["seats"][state["bb"]]), _chips(big)))
    _event(state, "blinds", state["bb"], small + big)
    state["currentBet"] = max(ctx.big, small, big)
    order = []
    index = state["dealer"]
    for _round in range(2):
        for _player in range(len(players)):
            index = _next_index(state, index, lambda seat: seat["status"] in IN_HAND)
            order.append(index)
    for index in order:
        state["seats"][index]["cards"].append(state["deck"].pop())
    _event(state, "deal", -1, len(players))
    first = _next_index(state, state["bb"], lambda seat: seat["status"] in IN_HAND)
    _continue_betting(state, ctx, first)


def _continue_betting(state, ctx, start):
    seats = state["seats"]
    in_hand = [index for index, seat in enumerate(seats) if seat and seat["status"] in IN_HAND]
    if len(in_hand) == 1:
        _award_uncontested(state, ctx, in_hand[0])
        return
    current = int(state["currentBet"])
    needing = [index for index in in_hand if _can_bet(seats[index])
               and (not seats[index]["acted"] or seats[index]["bet"] < current)]
    bettors = [index for index in in_hand if _can_bet(seats[index])]
    if needing and not (len(bettors) == 1 and seats[bettors[0]]["bet"] >= current):
        count = len(seats)
        for step in range(count):
            index = (start + step) % count
            if index in needing:
                _set_turn(state, ctx, index)
                return
    _end_street(state, ctx)


def _set_turn(state, ctx, index):
    seat = state["seats"][index]
    now = ctx.now
    state["toAct"] = index
    state["actStartedAt"] = now
    if seat.get("bot"):
        delay = _RNG.randint(BOT_MIN_MS, BOT_MAX_MS)
        state["deadline"] = now + delay
        _schedule(state, now + delay, "bot")
    elif seat.get("away") or seat.get("leaving"):
        state["deadline"] = now
        _schedule(state, now, "auto")
    else:
        state["deadline"] = now + ctx.turn_ms
        _schedule(state, now + ctx.turn_ms, "timeout")


def _return_uncalled(state, ctx):
    seats = [seat for seat in state["seats"] if seat and seat.get("bet")]
    if not seats:
        return
    ordered = sorted(seats, key=lambda seat: seat["bet"], reverse=True)
    top = ordered[0]
    second = ordered[1]["bet"] if len(ordered) > 1 else 0
    extra = top["bet"] - second
    if extra > 0 and top["status"] in IN_HAND:
        top["bet"] -= extra
        top["total"] -= extra
        top["stack"] += extra
        if top["status"] == "allin" and top["stack"] > 0:
            top["status"] = "active"
        index = state["seats"].index(top)
        _log(state, ctx.now, index, "return", extra, "%s gets %s back (uncalled)" % (_label(top), _chips(extra)))


def _end_street(state, ctx):
    _return_uncalled(state, ctx)
    for seat in state["seats"]:
        if seat:
            seat["bet"] = 0
            seat["acted"] = False
            seat["locked"] = False
    state["currentBet"] = 0
    state["lastRaise"] = ctx.big
    _clear_turn(state)
    in_hand = [index for index, seat in enumerate(state["seats"]) if seat and seat["status"] in IN_HAND]
    if len(in_hand) == 1:
        _award_uncontested(state, ctx, in_hand[0])
        return
    if state["phase"] == "river":
        _showdown(state, ctx)
        return
    bettors = [index for index in in_hand if _can_bet(state["seats"][index])]
    if len(bettors) < 2:
        if not any(seat and seat.get("shown") for seat in state["seats"]):
            for index in in_hand:
                state["seats"][index]["shown"] = True
            _event(state, "showdown")
        _schedule(state, ctx.now + RUNOUT_MS, "runout")
        _dealer_says(state, ctx.now, "All in. Running out the board.")
        return
    _deal_street(state, ctx)
    first = _next_index(state, state["dealer"], lambda seat: seat["status"] in IN_HAND)
    _continue_betting(state, ctx, first)


def _deal_street(state, ctx):
    deck = state["deck"]
    deck.pop()
    if state["phase"] == "preflop":
        state["board"] += [deck.pop(), deck.pop(), deck.pop()]
        state["phase"] = "flop"
        _dealer_says(state, ctx.now, "Flop: " + _pretty(state["board"]))
    elif state["phase"] == "flop":
        state["board"].append(deck.pop())
        state["phase"] = "turn"
        _dealer_says(state, ctx.now, "Turn: " + _pretty(state["board"][-1:]))
    else:
        state["board"].append(deck.pop())
        state["phase"] = "river"
        _dealer_says(state, ctx.now, "River: " + _pretty(state["board"][-1:]))
    _event(state, state["phase"])


def _runout(state, ctx):
    if state["phase"] == "river":
        _showdown(state, ctx)
        return
    _deal_street(state, ctx)
    _schedule(state, ctx.now + RUNOUT_MS, "runout")


def _finish(state, ctx, winners, shown, ranks):
    seats = state["seats"]
    summary = []
    for index, amount in winners.items():
        seat = seats[index]
        seat["won"] = amount
        seat["winner"] = amount > 0
        entry = {"seat": index, "name": seat["name"], "amount": amount, "handName": seat.get("handName") or "",
                 "cards": list(seat["cards"]) if index in shown else [], "best": seat.get("best") or []}
        summary.append(entry)
    summary.sort(key=lambda item: -item["amount"])
    for index, seat in enumerate(seats):
        if not seat or seat.get("bot") or seat["status"] == "out":
            continue
        ctx.stats.append((seat["bi"], {"won": winners.get(index, 0), "rank": ranks.get(index)}))
    state["lastHand"] = {
        "number": state["hand"], "board": list(state["board"]), "winners": summary,
        "shown": [{"seat": index, "name": seats[index]["name"], "cards": list(seats[index]["cards"]),
                   "handName": seats[index].get("handName") or ""} for index in shown],
        "commitment": state["commitment"],
        "reveal": _reveal(state, [card for index in shown for card in seats[index]["cards"]] + list(state["board"])),
    }
    top = summary[0] if summary else None
    state["history"] = (state.get("history") or [])[-(MAX_HISTORY - 1):] + [{
        "number": state["hand"], "winners": [item["name"] for item in summary],
        "amount": sum(item["amount"] for item in summary),
        "handName": top["handName"] if top else "", "at": ctx.now}]
    state["phase"] = "showdown"
    _clear_turn(state)


def _award_uncontested(state, ctx, winner):
    seats = state["seats"]
    _return_uncalled(state, ctx)
    total = sum(int(seat.get("total") or 0) for seat in seats if seat)
    seats[winner]["stack"] += total
    for seat in seats:
        if seat:
            seat["bet"] = 0
    seats[winner]["handName"] = ""
    _event(state, "win", winner, total)
    _dealer_says(state, ctx.now, "%s wins %s" % (_label(seats[winner]), _chips(total)))
    _finish(state, ctx, {winner: total}, [], {})
    _schedule(state, ctx.now + FOLD_WIN_NEXT_MS, "next")


def _showdown(state, ctx):
    seats = state["seats"]
    contenders = [index for index, seat in enumerate(seats) if seat and seat["status"] in IN_HAND]
    ranks = {}
    for index in contenders:
        rank, best = best_five(seats[index]["cards"] + state["board"])
        ranks[index] = rank
        seats[index]["handName"] = hand_name(rank)
        seats[index]["best"] = best
        seats[index]["shown"] = True
    pots = build_pots(seats)
    winners = {}
    order = []
    index = state["dealer"]
    for _step in range(len(seats)):
        index = (index + 1) % len(seats)
        order.append(index)
    for pot in pots:
        eligible = [item for item in pot["eligible"] if item in ranks]
        if not eligible:
            continue
        top = max(ranks[item] for item in eligible)
        tied = [item for item in order if item in eligible and ranks[item] == top]
        share, odd = divmod(pot["amount"], len(tied))
        for position, item in enumerate(tied):
            amount = share + (odd if position == 0 else 0)
            seats[item]["stack"] += amount
            winners[item] = winners.get(item, 0) + amount
    for seat in seats:
        if seat:
            seat["bet"] = 0
    _event(state, "showdown")
    for item, amount in sorted(winners.items(), key=lambda pair: -pair[1]):
        _event(state, "win", item, amount)
    if winners:
        top = max(winners, key=lambda item: winners[item])
        names = " and ".join(_label(seats[item]) for item in winners)
        _dealer_says(state, ctx.now, "%s %s %s with %s" % (
            names, "split" if len(winners) > 1 else "wins",
            _chips(sum(winners.values())), seats[top]["handName"]))
    _finish(state, ctx, winners, contenders, ranks)
    _schedule(state, ctx.now + NEXT_HAND_MS, "next")


def legal_actions(state, index, big):
    if state.get("phase") not in STREETS or state.get("toAct") != index:
        return None
    seat = state["seats"][index]
    if not seat or seat["status"] != "active":
        return None
    current = int(state["currentBet"])
    to_call = max(0, current - seat["bet"])
    call = min(to_call, seat["stack"])
    most = seat["bet"] + seat["stack"]
    least = current + max(int(state.get("lastRaise") or big), big) if current else big
    others = [other for position, other in enumerate(state["seats"])
              if other and position != index and _can_bet(other)]
    can_raise = most > current and not seat.get("locked") and bool(others)
    raise_range = {"min": min(least, most), "max": most} if can_raise else None
    return {"fold": True, "check": to_call == 0, "call": call, "raise": raise_range, "allIn": seat["stack"]}


def apply_action(state, ctx, index, action, amount=0, automatic=False):
    if state.get("phase") not in STREETS:
        return "hand_not_running"
    if state.get("toAct") != index:
        return "not_your_turn"
    seat = state["seats"][index]
    legal = legal_actions(state, index, ctx.big)
    if not legal:
        return "not_your_turn"
    now = ctx.now
    current = int(state["currentBet"])
    to_call = max(0, current - seat["bet"])
    label = _label(seat)
    if action == "allin":
        target = seat["bet"] + seat["stack"]
        if target <= current or not legal["raise"]:
            action = "call"
        else:
            action, amount = ("raise" if current else "bet"), target
    if action == "call" and to_call == 0:
        action = "check"
    if action == "fold":
        if to_call == 0 and automatic:
            action = "check"
        else:
            seat["status"] = "folded"
            seat["lastAction"] = {"kind": "fold", "amount": 0}
            _event(state, "fold", index)
            _log(state, now, index, "fold", 0, "%s folds" % label)
    if action == "check":
        if to_call:
            return "invalid_action"
        seat["lastAction"] = {"kind": "check", "amount": 0}
        _event(state, "check", index)
        _log(state, now, index, "check", 0, "%s checks" % label)
    elif action == "call":
        paid = _put(seat, to_call)
        kind = "allin" if seat["status"] == "allin" else "call"
        seat["lastAction"] = {"kind": "all in" if kind == "allin" else "call", "amount": seat["bet"]}
        _event(state, kind, index, paid)
        _log(state, now, index, kind, paid, "%s calls %s%s" % (label, _chips(paid), " and is all in" if kind == "allin" else ""))
    elif action in ("bet", "raise"):
        if not legal["raise"]:
            return "invalid_action"
        target = _integer(amount, 0)
        most = legal["raise"]["max"]
        if target >= most:
            target = most
        if target < legal["raise"]["min"] or target <= current:
            return "invalid_amount"
        paid = _put(seat, target - seat["bet"])
        size = target - current
        if size >= int(state["lastRaise"]):
            state["lastRaise"] = size
            for position, other in enumerate(state["seats"]):
                if other and position != index and other["status"] == "active":
                    other["acted"] = False
                    other["locked"] = False
        else:
            for position, other in enumerate(state["seats"]):
                if other and position != index and other["status"] == "active" and (other["acted"] or other["locked"]):
                    other["acted"] = False
                    other["locked"] = target - other["bet"] < int(state["lastRaise"])
        state["currentBet"] = target
        verb = "bets" if current == 0 else "raises to"
        kind = "allin" if seat["status"] == "allin" else ("bet" if current == 0 else "raise")
        seat["lastAction"] = {"kind": "all in" if kind == "allin" else ("bet" if current == 0 else "raise"), "amount": target}
        _event(state, kind, index, paid)
        _log(state, now, index, kind, target, "%s %s %s%s" % (label, verb, _chips(target), " (all in)" if kind == "allin" else ""))
    elif action != "fold":
        return "invalid_action"
    seat["acted"] = True
    if not automatic:
        seat["timeouts"] = 0
    _clear_turn(state)
    state["wakeAt"] = 0
    state["wakeKind"] = ""
    _continue_betting(state, ctx, (index + 1) % len(state["seats"]))
    return None


def bot_decision(state, index, big, rng=None):
    rng = rng or _RNG
    seat = state["seats"][index]
    legal = legal_actions(state, index, big)
    if not legal:
        return "fold", 0
    persona = PERSONAS.get(seat.get("persona") or "", PERSONAS["tight"])
    opponents = sum(1 for position, other in enumerate(state["seats"])
                    if other and position != index and other["status"] in IN_HAND)
    trials = max(50, MC_TRIALS // (1 + opponents // 3))
    strength = equity(seat["cards"], state["board"], opponents, trials, rng)
    adjusted = strength + persona["loose"] + rng.uniform(-0.04, 0.04)
    fair = 1.0 / (opponents + 1)
    pot = sum(int(other.get("total") or 0) for other in state["seats"] if other)
    to_call = legal["call"]
    odds = to_call / float(pot + to_call) if to_call else 0.0
    raising = legal["raise"]

    def sized(fraction):
        target = int(state["currentBet"]) + max(int(fraction * max(pot, big)), int(state.get("lastRaise") or big))
        target = max(raising["min"], min(raising["max"], target))
        if raising["max"] - target < big * 2 or target > 0.7 * raising["max"] and strength > 0.75:
            target = raising["max"]
        return target

    monster = adjusted > max(0.8, fair * 2.2)
    strong = adjusted > fair * 1.6 + 0.1
    if to_call == 0:
        if raising and (monster or strong and rng.random() < 0.55 * persona["aggression"]):
            return ("raise" if state["currentBet"] else "bet"), sized(rng.uniform(0.45, 0.85) * persona["aggression"])
        if raising and rng.random() < persona["bluff"]:
            return ("raise" if state["currentBet"] else "bet"), sized(0.5)
        return "check", 0
    if raising and (monster and rng.random() < 0.75 or strong and rng.random() < 0.3 * persona["aggression"]):
        return "raise", sized(rng.uniform(0.6, 1.0) * persona["aggression"])
    if adjusted >= odds + 0.04 or to_call <= big and adjusted >= fair * 0.8:
        return "call", 0
    if raising and rng.random() < persona["bluff"] * 0.4 and state["phase"] != "river":
        return "raise", sized(0.7)
    return "fold", 0


def run_due(state, ctx, limit=12):
    changed = False
    for _step in range(limit):
        due = int(state.get("wakeAt") or 0)
        if not due or due > ctx.now:
            break
        kind = state.get("wakeKind") or ""
        state["wakeAt"] = 0
        state["wakeKind"] = ""
        changed = True
        index = int(state.get("toAct") if state.get("toAct") is not None else -1)
        if kind == "next":
            _start_hand(state, ctx)
        elif kind == "runout":
            _runout(state, ctx)
        elif kind in ("bot", "auto", "timeout") and 0 <= index < len(state["seats"]) and state["seats"][index]:
            seat = state["seats"][index]
            if kind == "bot":
                action, amount = bot_decision(state, index, ctx.big)
                if apply_action(state, ctx, index, action, amount, automatic=True):
                    apply_action(state, ctx, index, "fold", 0, automatic=True)
            else:
                if kind == "timeout":
                    seat["timeouts"] = int(seat.get("timeouts") or 0) + 1
                    _event(state, "timeout", index)
                    _log(state, ctx.now, index, "timeout", 0, "%s ran out of time" % _label(seat))
                    if seat["timeouts"] >= TIMEOUTS_TO_AWAY:
                        seat["away"] = True
                apply_action(state, ctx, index, "fold", 0, automatic=True)
    return changed


def _bot_name(state):
    taken = {seat["name"] for seat in state["seats"] if seat}
    free = [bot for bot in BOTS if bot[0] not in taken]
    return _RNG.choice(free) if free else None


def _seat_of(state, account_bi):
    for index, seat in enumerate(state["seats"]):
        if seat and not seat.get("bot") and seat.get("bi") == account_bi:
            return index
    return -1


def _project_seat(state, index, viewer_seat):
    seat = state["seats"][index]
    if not seat:
        return {"index": index, "empty": True}
    phase = state.get("phase")
    cards = list(seat.get("cards") or [])
    status = seat.get("status") or "out"
    if seat.get("leaving") and status not in IN_HAND:
        status = "leaving"
    visible = index == viewer_seat or bool(seat.get("shown")) and status in IN_HAND
    if not cards or phase == "waiting":
        shown_cards = []
    elif index == viewer_seat:
        shown_cards = cards
    elif status in IN_HAND:
        shown_cards = cards if visible else ["??"] * len(cards)
    else:
        shown_cards = []
    return {
        "index": index, "empty": False, "name": seat["name"], "bot": bool(seat.get("bot")),
        "displayName": _label(seat) + (" (bot)" if seat.get("bot") else ""),
        "stack": int(seat.get("stack") or 0), "bet": int(seat.get("bet") or 0),
        "totalBet": int(seat.get("total") or 0), "status": status, "cards": shown_cards,
        "lastAction": seat.get("lastAction"), "isTurn": state.get("toAct") == index,
        "away": bool(seat.get("away")), "rebuy": int(seat.get("rebuy") or 0),
        "handName": (seat.get("handName") or "") if visible and phase == "showdown" else "",
        "winner": bool(seat.get("winner")) if phase == "showdown" else False,
        "won": int(seat.get("won") or 0) if phase == "showdown" else 0,
    }


def project(row, state, account_bi, now, wallet=None):
    stake = _stake(row)
    viewer = _seat_of(state, account_bi)
    seats = state.get("seats") or []
    pots, total = [], 0
    if state.get("phase") in STREETS:
        committed = [dict(seat, total=int(seat.get("total") or 0) - int(seat.get("bet") or 0)) if seat else None
                     for seat in seats]
        pots = build_pots(committed)
        total = sum(int(seat.get("total") or 0) for seat in seats if seat)
    view = {
        "id": row["table_id"], "name": row["name"], "code": row["code"], "private": bool(row["private"]),
        "owner": row["owner_name"], "ownerIsMe": row["owner_bi"] == account_bi,
        "stakes": stake["id"], "small": stake["small"], "big": stake["big"],
        "minBuyIn": stake["minBuyIn"], "maxBuyIn": stake["maxBuyIn"],
        "seatCount": len(seats), "turnSeconds": int(row["turn_seconds"]),
        "version": int(row.get("version") or 0), "now": now, "handNumber": int(state.get("hand") or 0),
        "phase": state.get("phase") or "waiting", "board": list(state.get("board") or []),
        "pots": pots, "potTotal": total,
        "dealer": int(state.get("dealer", -1)), "smallBlind": int(state.get("sb", -1)),
        "bigBlind": int(state.get("bb", -1)), "toAct": int(state.get("toAct", -1)),
        "actStartedAt": int(state.get("actStartedAt") or 0), "actDeadline": int(state.get("deadline") or 0),
        "currentBet": int(state.get("currentBet") or 0),
        "minRaiseTo": int(state.get("currentBet") or 0) + int(state.get("lastRaise") or stake["big"])
        if state.get("currentBet") else int(stake["big"]),
        "wakeAt": int(state.get("wakeAt") or 0), "mySeat": viewer,
        "seats": [_project_seat(state, index, viewer) for index in range(len(seats))],
        "legal": legal_actions(state, viewer, int(stake["big"])) if viewer >= 0 else None,
        "dealerLine": state.get("dealerLine") or "", "commitment": state.get("commitment") or "",
        "events": list(state.get("events") or []), "log": list(state.get("log") or []),
        "chat": list(state.get("chat") or []), "lastHand": state.get("lastHand"),
        "history": list(state.get("history") or []),
    }
    if wallet is not None:
        view["wallet"] = int(wallet)
    if viewer >= 0:
        mine = seats[viewer]
        if mine.get("cards") and mine.get("status") in IN_HAND and state.get("phase") in STREETS + ("showdown",):
            board = list(state.get("board") or [])
            if board:
                rank, best = best_five(list(mine["cards"]) + board)
                view["myHand"] = {"name": hand_name(rank), "cards": best}
            else:
                view["myHand"] = {"name": starting_hand_name(mine["cards"]), "cards": list(mine["cards"])}
    return view


def _summary(row, account_bi, seated, invited):
    stake = _stake(row)
    return {
        "id": row["table_id"], "name": row["name"], "code": row["code"], "private": bool(row["private"]),
        "owner": row["owner_name"], "stakes": stake["id"], "small": stake["small"], "big": stake["big"],
        "minBuyIn": stake["minBuyIn"], "maxBuyIn": stake["maxBuyIn"], "seats": int(row["seat_count"]),
        "players": int(row["players"]), "humans": int(row["humans"]), "bots": int(row["bots"]),
        "handNumber": int(row["hand_number"]), "turnSeconds": int(row["turn_seconds"]),
        "mine": row["table_id"] in seated, "invited": row["table_id"] in invited,
        "updatedAt": int(row["updated_at"]),
    }


async def _write(runtime, row, state, now):
    players, humans, bots = _counts(state)
    token = secrets.token_hex(8)
    version = int(row.get("version") or 0) + 1
    result = await runtime.d1_run(
        "UPDATE poker_tables SET data=?,version=?,write_token=?,players=?,humans=?,bots=?,hand_number=?,"
        "updated_at=? WHERE table_id=? AND version=?",
        await runtime.seal(state), version, token, players, humans, bots, int(state.get("hand") or 0),
        now, row["table_id"], int(row.get("version") or 0))
    changed = _changes(result)
    if changed is None:
        check = await runtime.d1_first("SELECT write_token FROM poker_tables WHERE table_id=?", row["table_id"])
        changed = 1 if check and check.get("write_token") == token else 0
    if changed:
        row["version"] = version
        row["updated_at"] = now
        row["players"], row["humans"], row["bots"] = players, humans, bots
        row["hand_number"] = int(state.get("hand") or 0)
    return bool(changed)


def _audience(state, now):
    names = [seat["name"] for seat in state["seats"] if seat and not seat.get("bot")]
    names += [name for name, seen in (state.get("watchers") or {}).items() if now - int(seen) < WATCH_MS]
    return names


async def _apply_effects(runtime, ctx, table_id):
    for account_bi, amount in ctx.credits:
        await _credit(runtime, account_bi, amount)
    for account_bi, update in ctx.stats:
        await _record_stats(runtime, account_bi, update)
    for account_bi in ctx.unseat:
        await runtime.d1_run("DELETE FROM poker_seats WHERE table_id=? AND account_bi=?", table_id, account_bi)
    for account_bi, name, seat, at in ctx.seat_rows:
        await runtime.d1_run(
            "INSERT INTO poker_seats(table_id,account_bi,account_name,seat,joined_at) VALUES(?,?,?,?,?) "
            "ON CONFLICT(table_id,account_bi) DO UPDATE SET seat=excluded.seat,account_name=excluded.account_name",
            table_id, account_bi, name, seat, at)


async def _mutate(runtime, table_id, change, viewer=""):
    for _attempt in range(WRITE_ATTEMPTS):
        row = await _table_row(runtime, table_id)
        if not row:
            return None, None, "table_not_found"
        state = await _open_state(runtime, row)
        if not state.get("seats"):
            return None, None, "table_not_found"
        now = runtime.now()
        before = json.dumps(state, sort_keys=True)
        ctx = _Ctx(row, now)
        problem = change(state, ctx) if change else None
        if problem:
            return row, state, problem
        run_due(state, ctx)
        if viewer:
            watchers = state.setdefault("watchers", {})
            if now - int(watchers.get(viewer) or 0) > WATCH_SAVE_MS:
                watchers[viewer] = now
            for name in [name for name, seen in watchers.items() if now - int(seen) > WATCH_MS]:
                watchers.pop(name, None)
        if json.dumps(state, sort_keys=True) == before:
            return row, state, None
        if await _write(runtime, row, state, now):
            await _apply_effects(runtime, ctx, table_id)
            await _notify(runtime, _audience(state, now))
            return row, state, None
    return None, None, "busy"


async def _table_response(runtime, row, state, account_bi, extra=None):
    wallet = await _wallet_row(runtime, account_bi, "", create=False)
    payload = {"ok": True, "table": project(row, state, account_bi, runtime.now(),
                                            int((wallet or {}).get("chips") or 0))}
    if extra:
        payload.update(extra)
    return _response(runtime, payload)


STATUS = {
    "table_not_found": 404, "not_found": 404, "busy": 503, "invalid_amount": 400,
    "invalid_action": 400, "invalid_stakes": 400, "invalid_text": 400, "invalid_seat": 400,
    "invalid_buy_in": 400,
}


def _status(code):
    return STATUS.get(code, 409)


async def _lobby(runtime, account_bi, actor):
    wallet = await _wallet_row(runtime, account_bi, actor)
    now = runtime.now()
    seated = {str(row["table_id"]) for row in await runtime.d1_all(
        "SELECT table_id FROM poker_seats WHERE account_bi=?", account_bi) or []}
    invited = {str(row["table_id"]) for row in await runtime.d1_all(
        "SELECT table_id FROM poker_invites WHERE account_name=?", actor) or []}
    columns = ("SELECT table_id,code,name,owner_bi,owner_name,stakes,private,seat_count,turn_seconds,"
               "players,humans,bots,hand_number,version,created_at,updated_at FROM poker_tables ")
    rows = {}
    for row in await runtime.d1_all(
            columns + "WHERE private=0 ORDER BY updated_at DESC LIMIT ?", MAX_LISTED) or []:
        rows[row["table_id"]] = row
    for row in await runtime.d1_all(columns + "WHERE owner_bi=?", account_bi) or []:
        rows[row["table_id"]] = row
    for table_id in sorted(seated | invited):
        if table_id not in rows:
            row = await runtime.d1_first(columns + "WHERE table_id=?", table_id)
            if row:
                rows[table_id] = row
    tables = [_summary(row, account_bi, seated, invited) for row in rows.values()]
    tables.sort(key=lambda item: (not item["mine"], not item["invited"], -item["humans"], -item["updatedAt"]))
    return _response(runtime, {
        "ok": True, "actor": actor,
        "wallet": _wallet_view(wallet, now, await _on_tables(runtime, account_bi)),
        "stakes": [dict(item) for item in STAKES], "tables": tables, "now": now,
    })


async def _claim(runtime, account_bi, actor):
    wallet = await _wallet_row(runtime, account_bi, actor)
    now = runtime.now()
    claimed = int(wallet.get("claimed_at") or 0)
    if claimed and now < claimed + CLAIM_INTERVAL_MS:
        return _response(runtime, {"ok": False, "error": "claim_not_ready",
                                   "nextClaimAt": claimed + CLAIM_INTERVAL_MS}, 409)
    result = await runtime.d1_run(
        "UPDATE poker_wallets SET chips=chips+?,claimed_at=?,updated_at=? WHERE account_bi=? AND claimed_at=?",
        CLAIM_AMOUNT, now, now, account_bi, claimed)
    if _changes(result) == 0:
        return _error(runtime, "claim_not_ready", 409)
    await runtime.audit(actor, "poker.claim", "poker_wallet", actor)
    wallet = await _wallet_row(runtime, account_bi, actor)
    return _response(runtime, {"ok": True, "wallet": _wallet_view(wallet, now, await _on_tables(runtime, account_bi))})


async def _new_code(runtime):
    for _attempt in range(20):
        code = "".join(_RNG.choice(CODE_ALPHABET) for _index in range(CODE_LENGTH))
        if not await runtime.d1_first("SELECT 1 AS taken FROM poker_tables WHERE code=?", code):
            return code
    return ""


def _seat_player(state, index, account_bi, actor, buy_in, now):
    state["seats"][index] = {
        "name": actor, "bi": account_bi, "bot": False, "stack": int(buy_in), "rebuy": 0,
        "status": "out", "cards": [], "bet": 0, "total": 0, "acted": False, "locked": False,
        "lastAction": None, "away": False, "leaving": False, "timeouts": 0, "joinedAt": now,
    }


def _seat_bot(state, index, stake, now):
    choice = _bot_name(state)
    if not choice:
        return False
    name, _label_text, persona = choice
    state["seats"][index] = {
        "name": name, "bi": "", "bot": True, "persona": persona, "stack": int(stake["maxBuyIn"]),
        "rebuy": 0, "status": "out", "cards": [], "bet": 0, "total": 0, "acted": False,
        "locked": False, "lastAction": None, "away": False, "leaving": False, "timeouts": 0,
        "joinedAt": now,
    }
    return True


async def _insert_table(runtime, table_id, code, name, account_bi, actor, stake, data, seat_count, turn,
                        players, humans, bots, now, state):
    await runtime.d1_run(
        "INSERT INTO poker_tables(table_id,code,name,owner_bi,owner_name,stakes,private,seat_count,"
        "turn_seconds,players,humans,bots,hand_number,version,write_token,created_at,updated_at,data) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,1,'',?,?,?)",
        table_id, code, name, account_bi, actor, stake["id"], 1 if data.get("private") else 0,
        seat_count, turn, players, humans, bots, now, now, await runtime.seal(state))


async def _create(runtime, account_bi, actor, data):
    owned = await runtime.d1_first("SELECT COUNT(*) AS total FROM poker_tables WHERE owner_bi=?", account_bi)
    if int((owned or {}).get("total") or 0) >= MAX_OWNED_TABLES:
        return _error(runtime, "too_many_tables", 409)
    stake = STAKES_BY_ID.get(str(data.get("stakes") or "starter"))
    if not stake:
        return _error(runtime, "invalid_stakes")
    seat_count = max(MIN_SEATS, min(MAX_SEATS, _integer(data.get("seats"), DEFAULT_SEATS)))
    turn = _integer(data.get("turnSeconds"), DEFAULT_TURN)
    turn = turn if turn in TURN_CHOICES else DEFAULT_TURN
    bots = max(0, min(seat_count - 1, _integer(data.get("bots"), 0)))
    buy_in = _integer(data.get("buyIn"), 0)
    if buy_in and not stake["minBuyIn"] <= buy_in <= stake["maxBuyIn"]:
        return _error(runtime, "invalid_buy_in")
    wallet = await _wallet_row(runtime, account_bi, actor)
    if buy_in and int(wallet.get("chips") or 0) < buy_in:
        return _error(runtime, "not_enough_chips", 409)
    name = _clean(data.get("name"), 40) or "%s's table" % actor
    code = await _new_code(runtime)
    if not code:
        return _error(runtime, "busy", 503)
    if buy_in and not await _debit(runtime, account_bi, buy_in):
        return _error(runtime, "not_enough_chips", 409)
    now = runtime.now()
    state = _new_state(seat_count)
    if buy_in:
        _seat_player(state, 0, account_bi, actor, buy_in, now)
    for offset in range(bots):
        index = (offset + 1) * seat_count // (bots + 1) if buy_in else offset
        while state["seats"][index % seat_count] is not None:
            index += 1
        _seat_bot(state, index % seat_count, stake, now)
    _maybe_schedule_start(state, now)
    table_id = runtime.new_id()
    players, humans, bot_count = _counts(state)
    try:
        await _insert_table(runtime, table_id, code, name, account_bi, actor, stake, data, seat_count, turn,
                            players, humans, bot_count, now, state)
    except Exception:
        await _credit(runtime, account_bi, buy_in)
        raise
    if buy_in:
        await runtime.d1_run(
            "INSERT INTO poker_seats(table_id,account_bi,account_name,seat,joined_at) VALUES(?,?,?,?,?)",
            table_id, account_bi, actor, 0, now)
    await runtime.audit(actor, "poker.table.create", "poker_table", table_id)
    row = await _table_row(runtime, table_id)
    return await _table_response(runtime, row, await _open_state(runtime, row), account_bi)


async def _sit(runtime, table_id, account_bi, actor, data):
    row = await _table_row(runtime, table_id)
    if not row:
        return _error(runtime, "table_not_found", 404)
    stake = _stake(row)
    buy_in = _integer(data.get("buyIn"), 0)
    if not stake["minBuyIn"] <= buy_in <= stake["maxBuyIn"]:
        return _error(runtime, "invalid_buy_in")
    await _wallet_row(runtime, account_bi, actor)
    if not await _debit(runtime, account_bi, buy_in):
        return _error(runtime, "not_enough_chips", 409)
    wanted = _integer(data.get("seat"), -1)

    def change(state, ctx):
        if _seat_of(state, account_bi) >= 0:
            return "already_seated"
        index = wanted
        if index < 0:
            index = next((position for position, seat in enumerate(state["seats"]) if seat is None), -1)
            if index < 0:
                return "table_full"
        if index >= len(state["seats"]):
            return "invalid_seat"
        if state["seats"][index] is not None:
            return "seat_taken"
        _seat_player(state, index, account_bi, actor, buy_in, ctx.now)
        ctx.seat_rows.append((account_bi, actor, index, ctx.now))
        _event(state, "join", index, buy_in)
        _log(state, ctx.now, index, "join", buy_in, "%s sits down with %s" % (actor, _chips(buy_in)))
        _maybe_schedule_start(state, ctx.now)
        return None

    row, state, problem = await _mutate(runtime, table_id, change, actor)
    if problem:
        await _credit(runtime, account_bi, buy_in)
        return _error(runtime, problem, _status(problem))
    await runtime.d1_run("DELETE FROM poker_invites WHERE table_id=? AND account_name=?", table_id, actor)
    return await _table_response(runtime, row, state, account_bi)


def _leave_change(account_bi, credit=True):
    def change(state, ctx):
        index = _seat_of(state, account_bi)
        if index < 0:
            return "not_seated"
        seat = state["seats"][index]
        if not credit:
            seat["purged"] = True
        if state["phase"] in STREETS and seat["status"] in IN_HAND + ("folded",):
            if seat.get("leaving"):
                return None
            seat["leaving"] = True
            if seat["status"] == "active":
                if state.get("toAct") == index:
                    apply_action(state, ctx, index, "fold", 0)
                else:
                    seat["status"] = "folded"
                    seat["lastAction"] = {"kind": "fold", "amount": 0}
                    _event(state, "fold", index)
                    in_hand = [position for position, other in enumerate(state["seats"])
                               if other and other["status"] in IN_HAND]
                    if len(in_hand) == 1:
                        _clear_turn(state)
                        _award_uncontested(state, ctx, in_hand[0])
            _log(state, ctx.now, index, "leave", 0, "%s will leave after this hand" % seat["name"])
            return None
        _remove_seat(state, index, ctx, credit)
        _event(state, "leave", index)
        _log(state, ctx.now, index, "leave", 0, "%s leaves the table" % seat["name"])
        return None
    return change


async def _leave(runtime, table_id, account_bi, actor):
    row, state, problem = await _mutate(runtime, table_id, _leave_change(account_bi), actor)
    if problem:
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _act(runtime, table_id, account_bi, actor, data):
    action = str(data.get("action") or "")
    if action not in ACTIONS:
        return _error(runtime, "invalid_action")
    hand = data.get("hand")

    def change(state, ctx):
        run_due(state, ctx)
        index = _seat_of(state, account_bi)
        if index < 0:
            return "not_seated"
        if hand is not None and _integer(hand, -1) != int(state.get("hand") or 0):
            return "hand_changed"
        return apply_action(state, ctx, index, action, data.get("amount"))

    row, state, problem = await _mutate(runtime, table_id, change, actor)
    if problem:
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _away(runtime, table_id, account_bi, actor, data):
    away = bool(data.get("away", True))

    def change(state, ctx):
        index = _seat_of(state, account_bi)
        if index < 0:
            return "not_seated"
        seat = state["seats"][index]
        seat["away"] = away
        seat["timeouts"] = 0
        _log(state, ctx.now, index, "away" if away else "back", 0,
             "%s %s" % (seat["name"], "is sitting out" if away else "is back"))
        if away and state.get("toAct") == index:
            apply_action(state, ctx, index, "fold", 0, automatic=True)
        if not away:
            _maybe_schedule_start(state, ctx.now)
        return None

    row, state, problem = await _mutate(runtime, table_id, change, actor)
    if problem:
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _rebuy(runtime, table_id, account_bi, actor, data):
    amount = _integer(data.get("amount"), 0)
    row = await _table_row(runtime, table_id)
    if not row:
        return _error(runtime, "table_not_found", 404)
    stake = _stake(row)
    if amount <= 0:
        return _error(runtime, "invalid_buy_in")
    if not await _debit(runtime, account_bi, amount):
        return _error(runtime, "not_enough_chips", 409)

    def change(state, ctx):
        index = _seat_of(state, account_bi)
        if index < 0:
            return "not_seated"
        seat = state["seats"][index]
        if int(seat.get("stack") or 0) + int(seat.get("rebuy") or 0) + amount > stake["maxBuyIn"]:
            return "invalid_buy_in"
        if state["phase"] in STREETS and seat["status"] in IN_HAND:
            seat["rebuy"] = int(seat.get("rebuy") or 0) + amount
        else:
            seat["stack"] = int(seat.get("stack") or 0) + amount
        _log(state, ctx.now, index, "rebuy", amount, "%s adds %s" % (seat["name"], _chips(amount)))
        _maybe_schedule_start(state, ctx.now)
        return None

    row, state, problem = await _mutate(runtime, table_id, change, actor)
    if problem:
        await _credit(runtime, account_bi, amount)
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _bots(runtime, table_id, account_bi, actor, data):
    row = await _table_row(runtime, table_id)
    if not row:
        return _error(runtime, "table_not_found", 404)
    if row["owner_bi"] != account_bi:
        return _error(runtime, "owner_only", 403)
    stake = _stake(row)
    remove = data.get("remove")

    def change(state, ctx):
        if remove is not None:
            index = _integer(remove, -1)
            if not 0 <= index < len(state["seats"]) or not (state["seats"][index] or {}).get("bot"):
                return "invalid_seat"
            seat = state["seats"][index]
            if state["phase"] in STREETS and seat["status"] in IN_HAND + ("folded",):
                seat["leaving"] = True
                if seat["status"] == "active" and state.get("toAct") != index:
                    seat["status"] = "folded"
                    _event(state, "fold", index)
                    in_hand = [position for position, other in enumerate(state["seats"])
                               if other and other["status"] in IN_HAND]
                    if len(in_hand) == 1:
                        _clear_turn(state)
                        _award_uncontested(state, ctx, in_hand[0])
                elif state.get("toAct") == index:
                    apply_action(state, ctx, index, "fold", 0)
            else:
                _remove_seat(state, index, ctx)
                _event(state, "leave", index)
            return None
        index = next((position for position, seat in enumerate(state["seats"]) if seat is None), -1)
        if index < 0:
            return "table_full"
        if not _seat_bot(state, index, stake, ctx.now):
            return "table_full"
        seat = state["seats"][index]
        _event(state, "join", index, seat["stack"])
        _log(state, ctx.now, index, "join", seat["stack"], "%s (bot) sits down" % _label(seat))
        _maybe_schedule_start(state, ctx.now)
        return None

    row, state, problem = await _mutate(runtime, table_id, change, actor)
    if problem:
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _chat(runtime, table_id, account_bi, actor, data):
    text = _clean(data.get("text"), CHAT_LIMIT)
    if not text:
        return _error(runtime, "invalid_text")

    def change(state, ctx):
        recent = [line for line in state.get("chat") or []
                  if line.get("name") == actor and ctx.now - int(line.get("at") or 0) < CHAT_WINDOW_MS]
        if len(recent) >= CHAT_BURST:
            return "slow_down"
        state["chat"] = (state.get("chat") or [])[-(MAX_CHAT - 1):] + [{"at": ctx.now, "name": actor, "text": text}]
        _event(state, "chat", _seat_of(state, account_bi))
        return None

    row, state, problem = await _mutate(runtime, table_id, change, actor)
    if problem:
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _invite(runtime, table_id, account_bi, actor, data):
    row = await _table_row(runtime, table_id)
    if not row:
        return _error(runtime, "table_not_found", 404)
    seated = await runtime.d1_first(
        "SELECT 1 AS seated FROM poker_seats WHERE table_id=? AND account_bi=?", table_id, account_bi)
    if not seated and row["owner_bi"] != account_bi:
        return _error(runtime, "not_seated", 409)
    names = data.get("names") if isinstance(data.get("names"), list) else []
    invited, skipped = [], []
    check = getattr(runtime, "account_exists", None)
    ping = getattr(runtime, "notify", None)
    for raw in names[:MAX_INVITES]:
        name = _name(str(raw or "").lstrip("@"))
        if not name or name == actor or name in invited:
            if _clean(raw, 64) and name != actor:
                skipped.append(_clean(raw, 64))
            continue
        if check is None or not await check(name):
            skipped.append(name)
            continue
        await runtime.d1_run(
            "INSERT INTO poker_invites(table_id,account_name,invited_by,invited_at) VALUES(?,?,?,?) "
            "ON CONFLICT(table_id,account_name) DO UPDATE SET invited_by=excluded.invited_by,"
            "invited_at=excluded.invited_at", table_id, name, actor, runtime.now())
        invited.append(name)
        if ping is not None:
            await ping(
                name, "plugin_activity", "%s invited you to play poker" % actor,
                body="Join %s at %s. Blinds %s / %s." % (actor, row["name"], _chips(_stake(row)["small"]),
                                                         _chips(_stake(row)["big"])),
                href="/dashboard/poker?table=" + table_id,
                dedupe="poker:invite:" + table_id + ":" + name,
                meta={"plugin": PLUGIN_ID, "tableId": table_id})
    if invited:
        await _notify(runtime, invited)
    state = await _open_state(runtime, row)
    return await _table_response(runtime, row, state, account_bi, {"invited": invited, "skipped": skipped})


async def close_table(runtime, table_id, credit=True, during_hand=True):
    for _attempt in range(WRITE_ATTEMPTS):
        row = await _table_row(runtime, table_id)
        if not row:
            return False
        state = await _open_state(runtime, row)
        if not during_hand and state.get("phase") in STREETS:
            return None
        result = await runtime.d1_run(
            "DELETE FROM poker_tables WHERE table_id=? AND version=?", table_id, int(row.get("version") or 0))
        changed = _changes(result)
        if changed is None:
            changed = 0 if await runtime.d1_first("SELECT 1 AS present FROM poker_tables WHERE table_id=?", table_id) else 1
        if not changed:
            continue
        audience = _audience(state, runtime.now()) if state.get("seats") else []
        if credit:
            for seat in state.get("seats") or []:
                if seat and not seat.get("bot") and not seat.get("purged"):
                    await _credit(runtime, seat.get("bi"), int(seat.get("stack") or 0) + int(seat.get("rebuy") or 0)
                                  + (int(seat.get("total") or 0) if state.get("phase") in STREETS else 0))
        await runtime.d1_run("DELETE FROM poker_seats WHERE table_id=?", table_id)
        await runtime.d1_run("DELETE FROM poker_invites WHERE table_id=?", table_id)
        await _notify(runtime, audience)
        return True
    return False


async def _view(runtime, table_id, account_bi, actor):
    row, state, problem = await _mutate(runtime, table_id, None, actor)
    if problem:
        return _error(runtime, problem, _status(problem))
    return await _table_response(runtime, row, state, account_bi)


async def _tick(runtime, table_id, account_bi, actor):
    return await _view(runtime, table_id, account_bi, actor)


def _allowed_methods(parts):
    if not parts:
        return ("GET",)
    head = parts[0]
    if head == "claim" and len(parts) == 1:
        return ("POST",)
    if head == "export" and len(parts) == 1:
        return ("GET",)
    if head == "codes" and len(parts) == 2:
        return ("GET",)
    if head == "tables":
        if len(parts) == 1:
            return ("GET", "POST")
        if len(parts) == 2:
            return ("GET", "DELETE")
        if len(parts) == 3 and parts[2] in TABLE_ACTIONS:
            return ("POST",)
    return ()


async def export_account_data(runtime, account_bi, actor):
    await _ensure_tables(runtime)
    wallet = await _wallet_row(runtime, account_bi, actor, create=False)
    now = runtime.now()
    return {
        "plugin": PLUGIN_ID, "format": EXPORT_FORMAT, "account": actor, "exportedAt": now,
        "wallet": _wallet_view(wallet, now) if wallet else None,
    }


async def handle(runtime, path):
    parts = _route(path)
    if parts is None:
        return _error(runtime, "not_found", 404)
    method = runtime.method()
    allowed = _allowed_methods(parts)
    if not allowed:
        return _error(runtime, "not_found", 404)
    if method not in allowed:
        return _response(runtime, {"ok": False, "error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _error(runtime, "origin_not_allowed", 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _error(runtime, error, 413 if error == "payload_too_large" else 400)
        data = data if isinstance(data, dict) else {}
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _error(runtime, "invalid_session", 401)
    await _ensure_tables(runtime)
    head = parts[0] if parts else ""
    if not parts or head == "tables" and len(parts) == 1 and method == "GET":
        return await _lobby(runtime, account_bi, actor)
    if head == "claim":
        return await _claim(runtime, account_bi, actor)
    if head == "export":
        return _response(runtime, {"ok": True, "filename": "forkmesh-poker-" + actor + ".json",
                                   "export": await export_account_data(runtime, account_bi, actor)})
    if head == "codes":
        code = parts[1].upper()
        row = await runtime.d1_first("SELECT table_id FROM poker_tables WHERE code=?", code) \
            if CODE_RE.fullmatch(code) else None
        if not row:
            return _error(runtime, "table_not_found", 404)
        return _response(runtime, {"ok": True, "tableId": row["table_id"]})
    if len(parts) == 1:
        return await _create(runtime, account_bi, actor, data)
    table_id = parts[1]
    if not ID_RE.fullmatch(table_id):
        return _error(runtime, "table_not_found", 404)
    if len(parts) == 2 and method == "GET":
        return await _view(runtime, table_id, account_bi, actor)
    if len(parts) == 2:
        row = await _table_row(runtime, table_id)
        if not row:
            return _error(runtime, "table_not_found", 404)
        if row["owner_bi"] != account_bi:
            return _error(runtime, "owner_only", 403)
        closed = await close_table(runtime, table_id, during_hand=False)
        if closed is None:
            return _error(runtime, "hand_in_progress", 409)
        if not closed:
            return _error(runtime, "busy", 503)
        await runtime.audit(actor, "poker.table.close", "poker_table", table_id)
        return _response(runtime, {"ok": True, "closed": True})
    action = parts[2]
    if action == "sit":
        return await _sit(runtime, table_id, account_bi, actor, data)
    if action == "leave":
        return await _leave(runtime, table_id, account_bi, actor)
    if action == "act":
        return await _act(runtime, table_id, account_bi, actor, data)
    if action == "away":
        return await _away(runtime, table_id, account_bi, actor, data)
    if action == "rebuy":
        return await _rebuy(runtime, table_id, account_bi, actor, data)
    if action == "bots":
        return await _bots(runtime, table_id, account_bi, actor, data)
    if action == "chat":
        return await _chat(runtime, table_id, account_bi, actor, data)
    if action == "invite":
        return await _invite(runtime, table_id, account_bi, actor, data)
    return await _tick(runtime, table_id, account_bi, actor)


async def account_has_data(runtime, account_bi):
    await _ensure_tables(runtime)
    row = await runtime.d1_first("SELECT 1 AS found FROM poker_wallets WHERE account_bi=?", account_bi)
    return bool(row)


async def purge_account_data(runtime, account_bi, account_name=""):
    await _ensure_tables(runtime)
    for row in await runtime.d1_all("SELECT table_id FROM poker_tables WHERE owner_bi=?", account_bi) or []:
        await close_table(runtime, str(row.get("table_id") or ""))
    for row in await runtime.d1_all("SELECT table_id FROM poker_seats WHERE account_bi=?", account_bi) or []:
        await _mutate(runtime, str(row.get("table_id") or ""), _leave_change(account_bi, credit=False))
    await runtime.d1_run("DELETE FROM poker_seats WHERE account_bi=?", account_bi)
    name = _name(account_name)
    if name:
        await runtime.d1_run("DELETE FROM poker_invites WHERE account_name=?", name)
    await runtime.d1_run("DELETE FROM poker_wallets WHERE account_bi=?", account_bi)


async def deliver_reminders(runtime):
    await _ensure_tables(runtime)
    cutoff = runtime.now() - IDLE_CLOSE_MS
    rows = await runtime.d1_all(
        "SELECT table_id FROM poker_tables WHERE updated_at<? ORDER BY updated_at LIMIT ?",
        cutoff, IDLE_SWEEP) or []
    closed = 0
    for row in rows:
        if await close_table(runtime, str(row.get("table_id") or "")):
            closed += 1
    return {"ok": True, "delivered": 0, "closed": closed}
