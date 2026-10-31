import asyncio
import hashlib
import json
from pathlib import Path
import random
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import poker  # noqa: E402


ACCOUNTS = {"alice", "bob", "carol", "dave", "erin"}
START = 1_789_561_860_000


class Runtime:
    def __init__(self):
        poker._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = START
        self.ids = 0
        self.audits = []
        self.rings = []
        self.pings = []

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return True
    def now(self): return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in ACCOUNTS else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)
    async def notify_account(self, owner, topic, started_by=""): self.rings.append((owner, topic))
    async def account_exists(self, name): return name in ACCOUNTS
    async def notify(self, recipient, kind, title, **kwargs): self.pings.append((recipient, kind, title, kwargs))


def call(runtime, method, path, actor="alice", body=None):
    return asyncio.run(poker.handle(runtime.use(method, actor, body), path))


def ok(response, status=200):
    assert response["status"] == status, response
    return response["data"]


def table(runtime, table_id, actor="alice"):
    return ok(call(runtime, "GET", "/api/poker/tables/" + table_id, actor))["table"]


def act(runtime, table_id, actor, action, amount=0):
    return call(runtime, "POST", "/api/poker/tables/%s/act" % table_id, actor, {"action": action, "amount": amount})


def tick(runtime, table_id, actor="alice"):
    return ok(call(runtime, "POST", "/api/poker/tables/%s/tick" % table_id, actor))["table"]


def wallet(runtime, actor):
    return ok(call(runtime, "GET", "/api/poker", actor))["wallet"]


def rig(monkey_deck):
    def shuffle(deck):
        rest = [card for card in deck if card not in monkey_deck]
        deck[:] = rest + list(reversed(monkey_deck))
    poker._RNG.shuffle = shuffle


def unrig():
    poker._RNG = poker.secrets.SystemRandom()


def deck_for(holes, board):
    order = []
    for round_index in range(2):
        for hole in holes:
            order.append(hole[round_index])
    burn = [card for card in poker.FULL_DECK if card not in set(order) | set(board)][:3]
    order += [burn[0]] + board[:3] + [burn[1], board[3], burn[2], board[4]]
    return order


def verify(last):
    reveal = last["reveal"]
    hashes = reveal["hashes"]
    if hashlib.sha256(",".join(hashes).encode()).hexdigest() != last["commitment"] or len(hashes) != 52:
        return False
    return all(hashlib.sha256(("%s:%d:%s" % (item["salt"], item["index"], item["card"])).encode()).hexdigest()
               == hashes[item["index"]] for item in reveal["cards"])


def stacks(view):
    return {seat["name"]: seat["stack"] for seat in view["seats"] if not seat["empty"]}


def two_player_table(runtime, buy_in=1000, stakes="starter"):
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                      {"name": "Duel", "stakes": stakes, "seats": 2, "buyIn": buy_in}))["table"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % created["id"], "bob", {"seat": 1, "buyIn": buy_in}))
    runtime.clock += poker.FIRST_DEAL_MS
    return created["id"], tick(runtime, created["id"])


def test_evaluator_orders_every_category():
    hands = [
        ["As", "Ks", "Qs", "Js", "Ts", "2c", "3d"],
        ["9h", "8h", "7h", "6h", "5h", "Ac", "Ad"],
        ["Jc", "Jd", "Jh", "Js", "2c", "3d", "4h"],
        ["Kc", "Kd", "Kh", "7s", "7c", "2d", "3h"],
        ["Ah", "Th", "8h", "4h", "2h", "Kc", "Qd"],
        ["Tc", "9d", "8h", "7s", "6c", "2d", "2h"],
        ["5c", "5d", "5h", "Ks", "9c", "2d", "3h"],
        ["Kc", "Kd", "7h", "7s", "2c", "3d", "9h"],
        ["Ac", "Ad", "7h", "5s", "2c", "3d", "9h"],
        ["Kc", "Jd", "7h", "5s", "2c", "3d", "9h"],
    ]
    names = [poker.hand_name(poker.evaluate(cards)) for cards in hands]
    assert names == [
        "Royal Flush", "Straight Flush, Nine high", "Four of a Kind, Jacks",
        "Full House, Kings over Sevens", "Flush, Ace high", "Straight, Ten high",
        "Three of a Kind, Fives", "Two Pair, Kings and Sevens", "Pair of Aces", "High Card, King",
    ]
    ranks = [poker.evaluate(cards) for cards in hands]
    assert ranks == sorted(ranks, reverse=True)


def test_evaluator_edge_cases():
    assert poker.evaluate(["Ac", "2d", "3h", "4s", "5c", "Kd", "Qh"]) == (4, 5)
    assert poker.evaluate(["6c", "2d", "3h", "4s", "5c", "Ad", "Qh"]) == (4, 6)
    assert poker.evaluate(["Kc", "Kd", "Kh", "7s", "7c", "7d", "3h"]) == (6, 13, 7)
    assert poker.evaluate(["Ac", "Ad", "Kh", "Ks", "Qc", "Qd", "3h"]) == (2, 14, 13, 12)
    assert poker.evaluate(["Ac", "Kc", "Qc", "Jc", "9c", "8c", "2c"])[:3] == (5, 14, 13)
    rank, best = poker.best_five(["Ah", "Th", "8h", "4h", "2h", "Kc", "Qd"])
    assert rank[0] == 5 and best == ["Ah", "Th", "8h", "4h", "2h"]
    assert poker.starting_hand_name(["As", "Ad"]) == "Pocket Aces"
    assert poker.starting_hand_name(["Ks", "As"]) == "Ace King suited"
    assert poker.starting_hand_name(["7d", "Tc"]) == "Ten Seven offsuit"


def test_side_pots_from_contributions():
    seats = [
        {"total": 100, "status": "allin"},
        {"total": 300, "status": "allin"},
        {"total": 500, "status": "active"},
        {"total": 200, "status": "folded"},
        None,
    ]
    pots = poker.build_pots(seats)
    assert pots == [
        {"amount": 400, "eligible": [0, 1, 2]},
        {"amount": 500, "eligible": [1, 2]},
        {"amount": 200, "eligible": [2]},
    ]
    assert sum(pot["amount"] for pot in pots) == 1100


def test_wallet_starts_and_claims_every_four_hours():
    runtime = Runtime()
    first = wallet(runtime, "alice")
    assert first["chips"] == poker.STARTING_CHIPS and first["canClaim"] is True
    claimed = ok(call(runtime, "POST", "/api/poker/claim"))["wallet"]
    assert claimed["chips"] == poker.STARTING_CHIPS + poker.CLAIM_AMOUNT and claimed["canClaim"] is False
    assert call(runtime, "POST", "/api/poker/claim")["status"] == 409
    runtime.clock += poker.CLAIM_INTERVAL_MS
    assert ok(call(runtime, "POST", "/api/poker/claim"))["wallet"]["chips"] == poker.STARTING_CHIPS + 2 * poker.CLAIM_AMOUNT
    assert call(runtime, "GET", "/api/poker", "nobody")["status"] == 401


def test_create_table_with_bots_deals_a_private_hand():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                      {"name": "Practice", "stakes": "low", "seats": 6, "bots": 2, "buyIn": 3000}))["table"]
    assert created["seatCount"] == 6 and created["mySeat"] == 0 and created["phase"] == "waiting"
    assert wallet(runtime, "alice")["chips"] == poker.STARTING_CHIPS - 3000
    assert wallet(runtime, "alice")["onTables"] == 3000
    assert [seat["bot"] for seat in created["seats"] if not seat["empty"]] == [False, True, True]
    assert created["wakeAt"] == START + poker.FIRST_DEAL_MS
    runtime.clock += poker.FIRST_DEAL_MS
    view = tick(runtime, created["id"])
    assert view["phase"] == "preflop" and view["handNumber"] == 1
    assert len(view["seats"][0]["cards"]) == 2 and "??" not in view["seats"][0]["cards"]
    others = [seat for seat in view["seats"] if not seat["empty"] and seat["index"] != 0]
    assert all(seat["cards"] == ["??", "??"] for seat in others)
    assert view["potTotal"] == 75 and len(view["commitment"]) == 64
    assert any(event["kind"] == "deal" for event in view["events"])
    assert view["myHand"]["name"]
    text = json.dumps(view)
    assert "bi:alice" not in text and '"deck"' not in text and '"salt"' not in text
    spectator = table(runtime, created["id"], "bob")
    assert spectator["mySeat"] == -1 and spectator["legal"] is None
    assert all("??" in seat["cards"] or not seat["cards"] for seat in spectator["seats"] if not seat["empty"])


def test_heads_up_hand_to_showdown_pays_the_best_hand():
    runtime = Runtime()
    rig(deck_for([["7d", "2c"], ["As", "Ks"]], ["Ah", "Kd", "9c", "4s", "3h"]))
    try:
        table_id, view = two_player_table(runtime)
    finally:
        unrig()
    assert view["dealer"] == 0 and view["smallBlind"] == 0 and view["bigBlind"] == 1
    assert view["toAct"] == 0 and view["legal"]["call"] == 5 and view["legal"]["raise"]["min"] == 20
    assert act(runtime, table_id, "bob", "call")["status"] == 409
    ok(act(runtime, table_id, "alice", "raise", 40))
    view = ok(act(runtime, table_id, "bob", "call"))["table"]
    assert view["phase"] == "flop" and view["board"] == ["Ah", "Kd", "9c"] and view["toAct"] == 1
    assert view["potTotal"] == 80 and view["pots"] == [{"amount": 80, "eligible": [0, 1]}]
    ok(act(runtime, table_id, "bob", "check"))
    ok(act(runtime, table_id, "alice", "bet", 60))
    view = ok(act(runtime, table_id, "bob", "call"))["table"]
    assert view["phase"] == "turn"
    ok(act(runtime, table_id, "bob", "check"))
    ok(act(runtime, table_id, "alice", "check"))
    ok(act(runtime, table_id, "bob", "check"))
    view = ok(act(runtime, table_id, "alice", "check"))["table"]
    assert view["phase"] == "showdown" and view["board"] == ["Ah", "Kd", "9c", "4s", "3h"]
    assert stacks(view) == {"alice": 1100, "bob": 900}
    assert view["seats"][1]["cards"] == ["7d", "2c"]
    assert view["seats"][0]["handName"] == "Two Pair, Aces and Kings" and view["seats"][0]["winner"]
    last = view["lastHand"]
    assert last["winners"][0]["name"] == "alice" and last["winners"][0]["amount"] == 200
    assert verify(last)
    assert {item["card"] for item in last["reveal"]["cards"]} == {"As", "Ks", "7d", "2c", "Ah", "Kd", "9c", "4s", "3h"}
    assert ("bob", poker.TOPIC) in runtime.rings and ("alice", poker.TOPIC) in runtime.rings
    runtime.clock += poker.NEXT_HAND_MS
    view = tick(runtime, table_id)
    assert view["handNumber"] == 2 and view["dealer"] == 1 and view["toAct"] == 1
    assert wallet(runtime, "alice")["handsWon"] == 1 and wallet(runtime, "bob")["handsPlayed"] == 1
    assert wallet(runtime, "alice")["bestHand"] == "Two Pair, Aces and Kings"


def test_fold_returns_uncalled_bet_and_hides_cards():
    runtime = Runtime()
    table_id, view = two_player_table(runtime)
    ok(act(runtime, table_id, "alice", "call"))
    ok(act(runtime, table_id, "bob", "bet", 100))
    view = ok(act(runtime, table_id, "alice", "fold"))["table"]
    assert view["phase"] == "showdown"
    assert stacks(view) == {"alice": 990, "bob": 1010}
    assert view["lastHand"]["winners"][0]["cards"] == []
    assert view["lastHand"]["reveal"]["cards"] == [] and verify(view["lastHand"])
    assert table(runtime, table_id, "alice")["seats"][1]["cards"] == ["??", "??"]


def test_short_all_in_does_not_reopen_raising():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                      {"stakes": "starter", "seats": 3, "buyIn": 1000}))["table"]
    table_id = created["id"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 1, "buyIn": 1000}))
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "carol", {"seat": 2, "buyIn": 200}))
    runtime.clock += poker.FIRST_DEAL_MS
    view = tick(runtime, table_id)
    assert view["dealer"] == 0 and view["toAct"] == 0
    ok(act(runtime, table_id, "alice", "raise", 150))
    ok(act(runtime, table_id, "bob", "call"))
    ok(act(runtime, table_id, "carol", "allin"))
    view = table(runtime, table_id, "alice")
    assert view["currentBet"] == 200 and view["toAct"] == 0
    assert view["legal"]["raise"] is None and view["legal"]["call"] == 50
    assert act(runtime, table_id, "alice", "raise", 400)["status"] == 400


def test_side_pot_showdown_with_three_all_ins():
    runtime = Runtime()
    rig(deck_for([["Kc", "Kd"], ["Ac", "Ad"], ["2d", "7h"]], ["Ah", "Kh", "9s", "4c", "3d"]))
    try:
        created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                          {"stakes": "starter", "seats": 3, "buyIn": 1000}))["table"]
        table_id = created["id"]
        ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 1, "buyIn": 600}))
        ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "carol", {"seat": 2, "buyIn": 200}))
        runtime.clock += poker.FIRST_DEAL_MS
        tick(runtime, table_id)
    finally:
        unrig()
    ok(act(runtime, table_id, "alice", "allin"))
    ok(act(runtime, table_id, "bob", "allin"))
    view = ok(act(runtime, table_id, "carol", "allin"))["table"]
    assert view["phase"] == "preflop" and view["toAct"] == -1 and view["wakeAt"]
    assert all(seat["cards"] and "??" not in seat["cards"] for seat in view["seats"])
    for street in ("flop", "turn", "river", "showdown"):
        runtime.clock += poker.RUNOUT_MS
        view = tick(runtime, table_id)
        assert view["phase"] == street
    assert stacks(view) == {"alice": 400, "bob": 800, "carol": 600}
    winners = {item["name"]: item["amount"] for item in view["lastHand"]["winners"]}
    assert winners == {"carol": 600, "bob": 800}


def test_turn_clock_checks_then_folds_then_sits_out():
    runtime = Runtime()
    table_id, view = two_player_table(runtime)
    assert view["actDeadline"] == view["now"] + 30_000
    runtime.clock = view["actDeadline"] + 1
    view = tick(runtime, table_id, "bob")
    assert view["phase"] == "showdown" and stacks(view) == {"alice": 995, "bob": 1005}
    assert any(event["kind"] == "timeout" for event in view["events"])
    runtime.clock += poker.FOLD_WIN_NEXT_MS
    view = tick(runtime, table_id, "bob")
    assert view["toAct"] == 1
    ok(act(runtime, table_id, "bob", "call"))
    view = table(runtime, table_id)
    runtime.clock = view["actDeadline"] + 1
    view = tick(runtime, table_id)
    assert view["phase"] == "flop" and view["seats"][0]["away"] is True
    assert view["seats"][0]["lastAction"]["kind"] == "check"


def test_leaving_mid_hand_folds_and_cashes_out():
    runtime = Runtime()
    table_id, view = two_player_table(runtime)
    view = ok(call(runtime, "POST", "/api/poker/tables/%s/leave" % table_id, "bob"))["table"]
    assert view["phase"] == "showdown" and stacks(view)["alice"] == 1010
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS - 1000
    runtime.clock += poker.FOLD_WIN_NEXT_MS
    view = tick(runtime, table_id)
    assert view["seats"][1]["empty"] and view["phase"] == "waiting"
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS - 10
    assert call(runtime, "POST", "/api/poker/tables/%s/leave" % table_id, "bob")["status"] == 409


def test_seat_rules_and_buy_in_limits():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice", {"stakes": "starter", "seats": 4}))["table"]
    table_id = created["id"]
    assert created["mySeat"] == -1
    assert call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"buyIn": 50})["status"] == 400
    assert call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"buyIn": 5000})["status"] == 400
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 2, "buyIn": 500}))
    assert call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "carol", {"seat": 2, "buyIn": 500})["status"] == 409
    assert wallet(runtime, "carol")["chips"] == poker.STARTING_CHIPS
    assert call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 3, "buyIn": 500})["status"] == 409
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS - 500
    code = created["code"]
    assert ok(call(runtime, "GET", "/api/poker/codes/" + code.lower(), "carol"))["tableId"] == table_id
    assert call(runtime, "GET", "/api/poker/codes/ZZZZZZ", "carol")["status"] == 404
    assert call(runtime, "POST", "/api/poker/tables", "alice", {"stakes": "nope"})["status"] == 400


def test_rebuy_waits_for_the_hand_and_respects_the_maximum():
    runtime = Runtime()
    table_id, view = two_player_table(runtime, buy_in=500)
    assert call(runtime, "POST", "/api/poker/tables/%s/rebuy" % table_id, "bob", {"amount": 600})["status"] == 400
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS - 500
    view = ok(call(runtime, "POST", "/api/poker/tables/%s/rebuy" % table_id, "bob", {"amount": 400}))["table"]
    assert view["seats"][1]["rebuy"] == 400 and view["seats"][1]["stack"] == 490
    ok(act(runtime, table_id, "alice", "fold"))
    runtime.clock += poker.FOLD_WIN_NEXT_MS
    view = tick(runtime, table_id)
    assert view["seats"][1]["rebuy"] == 0 and view["seats"][1]["stack"] + view["seats"][1]["bet"] == 905


def test_owner_bots_chat_invite_and_close():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                      {"stakes": "starter", "seats": 4, "buyIn": 1000, "private": True}))["table"]
    table_id = created["id"]
    assert call(runtime, "POST", "/api/poker/tables/%s/bots" % table_id, "bob", {"add": True})["status"] == 403
    view = ok(call(runtime, "POST", "/api/poker/tables/%s/bots" % table_id, "alice", {"add": True}))["table"]
    bot = next(seat for seat in view["seats"] if not seat["empty"] and seat["bot"])
    assert bot["stack"] == 1000 and bot["name"] in poker.BOT_NAMES
    view = ok(call(runtime, "POST", "/api/poker/tables/%s/chat" % table_id, "alice", {"text": "  good   luck  "}))["table"]
    assert view["chat"][-1]["text"] == "good luck"
    for _index in range(4):
        ok(call(runtime, "POST", "/api/poker/tables/%s/chat" % table_id, "alice", {"text": "hi"}))
    assert call(runtime, "POST", "/api/poker/tables/%s/chat" % table_id, "alice", {"text": "hi"})["status"] == 409
    invited = ok(call(runtime, "POST", "/api/poker/tables/%s/invite" % table_id, "alice",
                      {"names": ["@bob", "ghost", "alice"]}))
    assert invited["invited"] == ["bob"] and invited["skipped"] == ["ghost"]
    assert runtime.pings[0][0] == "bob" and "table=" + table_id in runtime.pings[0][3]["href"]
    lobby = ok(call(runtime, "GET", "/api/poker", "bob"))
    assert [item["id"] for item in lobby["tables"]] == [table_id] and lobby["tables"][0]["invited"]
    assert ok(call(runtime, "GET", "/api/poker", "carol"))["tables"] == []
    assert call(runtime, "DELETE", "/api/poker/tables/" + table_id, "bob")["status"] == 403
    runtime.clock += poker.FIRST_DEAL_MS
    tick(runtime, table_id)
    assert call(runtime, "DELETE", "/api/poker/tables/" + table_id, "alice")["status"] == 409
    for _step in range(200):
        view = table(runtime, table_id)
        if view["phase"] not in poker.STREETS:
            break
        if view["legal"]:
            ok(act(runtime, table_id, "alice", "fold"))
        elif view["wakeAt"]:
            runtime.clock = max(runtime.clock, view["wakeAt"])
    stack = stacks(view)["alice"]
    ok(call(runtime, "DELETE", "/api/poker/tables/" + table_id, "alice"))
    assert wallet(runtime, "alice")["chips"] == poker.STARTING_CHIPS - 1000 + stack
    assert call(runtime, "GET", "/api/poker/tables/" + table_id, "alice")["status"] == 404


def test_bots_play_hands_with_one_person():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                      {"stakes": "starter", "seats": 4, "bots": 3, "buyIn": 1000}))["table"]
    table_id = created["id"]
    hands = set()
    kinds = set()
    for _step in range(400):
        view = table(runtime, table_id)
        hands.add(view["handNumber"])
        kinds |= {entry["kind"] for entry in view["log"] if entry["seat"] > 0}
        if view["legal"]:
            legal = view["legal"]
            ok(act(runtime, table_id, "alice", "check" if legal["check"] else "call"))
            continue
        if view["wakeAt"]:
            runtime.clock = max(runtime.clock, view["wakeAt"])
            tick(runtime, table_id)
        else:
            runtime.clock += 1000
        for seat in view["seats"]:
            assert seat["empty"] or seat["stack"] >= 0
    assert len(hands) > 8
    assert {"fold", "call", "check"} <= kinds and kinds & {"bet", "raise", "allin"}


def test_random_play_conserves_every_chip():
    rng = random.Random(7)
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice",
                      {"stakes": "starter", "seats": 5, "buyIn": 600}))["table"]
    table_id = created["id"]
    players = ["alice", "bob", "carol", "dave"]
    for seat, name in enumerate(players[1:], start=1):
        ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, name,
                {"seat": seat, "buyIn": rng.choice([200, 400, 1000])}))

    def total():
        view = table(runtime, table_id)
        chips = sum(wallet(runtime, name)["chips"] for name in players)
        return chips + view["potTotal"] + sum(seat["stack"] + seat.get("rebuy", 0)
                                              for seat in view["seats"] if not seat["empty"])

    expected = 4 * poker.STARTING_CHIPS
    for _step in range(600):
        view = table(runtime, table_id)
        if view["toAct"] >= 0 and view["legal"] is None:
            name = view["seats"][view["toAct"]]["name"]
            mine = table(runtime, table_id, name)
            legal = mine["legal"]
            choice = rng.random()
            if legal["raise"] and choice < 0.25:
                amount = rng.randint(legal["raise"]["min"], legal["raise"]["max"])
                ok(act(runtime, table_id, name, "raise", amount))
            elif choice < 0.35:
                ok(act(runtime, table_id, name, "fold"))
            elif choice < 0.4:
                ok(act(runtime, table_id, name, "allin"))
            else:
                ok(act(runtime, table_id, name, "check" if legal["check"] else "call"))
        elif view["legal"]:
            ok(act(runtime, table_id, "alice", "check" if view["legal"]["check"] else "call"))
        elif view["wakeAt"]:
            runtime.clock = max(runtime.clock, view["wakeAt"])
            tick(runtime, table_id)
        else:
            for seat in view["seats"]:
                if not seat["empty"] and seat["stack"] == 0 and seat["status"] != "allin":
                    ok(call(runtime, "POST", "/api/poker/tables/%s/rebuy" % table_id, seat["name"], {"amount": 300}))
            runtime.clock += 1000
            tick(runtime, table_id)
        if _step % 25 == 0:
            assert total() == expected
    assert total() == expected
    assert table(runtime, table_id)["handNumber"] > 10


def test_purge_and_idle_sweep_return_chips():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice", {"stakes": "starter", "seats": 3, "buyIn": 800}))["table"]
    table_id = created["id"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 1, "buyIn": 700}))
    other = ok(call(runtime, "POST", "/api/poker/tables", "carol", {"stakes": "starter", "seats": 2, "buyIn": 500}))["table"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % other["id"], "bob", {"buyIn": 300}))
    asyncio.run(poker.purge_account_data(runtime, "bi:alice", "alice"))
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS - 300
    assert runtime.db.execute("SELECT COUNT(*) FROM poker_wallets WHERE account_bi='bi:alice'").fetchone()[0] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM poker_tables WHERE owner_bi='bi:alice'").fetchone()[0] == 0
    runtime.clock += poker.IDLE_CLOSE_MS + 1
    assert asyncio.run(poker.deliver_reminders(runtime))["closed"] == 1
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS
    assert wallet(runtime, "carol")["chips"] == poker.STARTING_CHIPS
    exported = ok(call(runtime, "GET", "/api/poker/export", "bob"))
    assert exported["export"]["format"] == poker.EXPORT_FORMAT and exported["export"]["wallet"]["chips"] == poker.STARTING_CHIPS
    assert asyncio.run(poker.account_has_data(runtime, "bi:bob"))


def test_version_conflict_is_retried_not_lost():
    runtime = Runtime()
    table_id, _view = two_player_table(runtime)
    row = asyncio.run(poker._table_row(runtime, table_id))
    state = asyncio.run(poker._open_state(runtime, row))
    stale = dict(row)
    assert asyncio.run(poker._write(runtime, row, state, runtime.clock))
    assert not asyncio.run(poker._write(runtime, stale, state, runtime.clock))
    ok(act(runtime, table_id, "alice", "call"))


def test_routes_and_methods():
    runtime = Runtime()
    assert call(runtime, "GET", "/api/poker/nope")["status"] == 404
    assert call(runtime, "GET", "/api/poker/claim")["status"] == 405
    assert call(runtime, "POST", "/api/poker/tables/" + "0" * 32 + "/act", "alice", {"action": "dance"})["status"] == 400
    assert call(runtime, "GET", "/api/poker/tables/" + "0" * 32)["status"] == 404
    assert call(runtime, "GET", "/api/poker/tables/xyz")["status"] == 404
    assert ok(call(runtime, "GET", "/api/poker/tables"))["stakes"][0]["id"] == "starter"


def test_manifest_and_catalog_entries():
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert manifest["id"] == "poker" and manifest["worker"]["api"] == poker.PREFIX
    assert manifest["dataLifecycle"]["scope"] == "account" and manifest["install"]["sync"] == "account"
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text())
    assert {"id": "poker", "version": manifest["version"], "manifest": "packages/poker/manifest.json"} in catalog["packages"]
    assert "poker" in json.loads((REPO / "marketplace" / "plugins.json").read_text())["plugins"]
    assert len(list((PACKAGE / "content" / "worker").glob("*.py"))) == 1


def test_a_folded_player_leaving_keeps_their_chips_in_the_pot():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice", {"stakes": "starter", "seats": 3, "buyIn": 1000}))["table"]
    table_id = created["id"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 1, "buyIn": 1000}))
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "carol", {"seat": 2, "buyIn": 1000}))
    runtime.clock += poker.FIRST_DEAL_MS
    tick(runtime, table_id)
    ok(act(runtime, table_id, "alice", "raise", 100))
    ok(act(runtime, table_id, "bob", "fold"))
    view = ok(call(runtime, "POST", "/api/poker/tables/%s/leave" % table_id, "bob"))["table"]
    assert view["potTotal"] == 115 and not view["seats"][1]["empty"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/leave" % table_id, "bob"))
    view = ok(act(runtime, table_id, "carol", "fold"))["table"]
    assert stacks(view)["alice"] == 1015
    runtime.clock += poker.FOLD_WIN_NEXT_MS
    tick(runtime, table_id)
    chips = sum(wallet(runtime, name)["chips"] + wallet(runtime, name)["onTables"] for name in ("alice", "bob", "carol"))
    assert chips == 3 * poker.STARTING_CHIPS
    assert wallet(runtime, "bob")["chips"] == poker.STARTING_CHIPS - 5


def test_short_all_ins_adding_up_to_a_full_raise_reopen_raising():
    runtime = Runtime()
    created = ok(call(runtime, "POST", "/api/poker/tables", "alice", {"stakes": "starter", "seats": 4, "buyIn": 1000}))["table"]
    table_id = created["id"]
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "bob", {"seat": 1, "buyIn": 450}))
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "carol", {"seat": 2, "buyIn": 600}))
    ok(call(runtime, "POST", "/api/poker/tables/%s/sit" % table_id, "dave", {"seat": 3, "buyIn": 1000}))
    runtime.clock += poker.FIRST_DEAL_MS
    view = tick(runtime, table_id)
    assert view["toAct"] == 3
    ok(act(runtime, table_id, "dave", "raise", 300))
    ok(act(runtime, table_id, "alice", "call"))
    ok(act(runtime, table_id, "bob", "allin"))
    ok(act(runtime, table_id, "carol", "allin"))
    view = table(runtime, table_id, "dave")
    assert view["toAct"] == 3 and view["legal"]["raise"] is not None
