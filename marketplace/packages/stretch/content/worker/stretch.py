import math
import re


PREFIX = "/api/stretch"
PLUGIN_ID = "stretch"
REQUEST_PROOF = "forkmesh-stretch-request-v1"
MAX_BODY_BYTES = 32 * 1024
MAX_SESSIONS_PER_DAY = 50
MAX_RECENT = 20
CHART_DAYS = 14
KEY_VERSION = 1
MAX_SESSION_SECONDS = 4 * 3600
SOURCES = ("web", "desktop", "ios", "android")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
DAY_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$")

FIGURE = {
    "width": 200,
    "height": 264,
    "hip": [100, 158],
    "hipShift": 16,
    "kneeDip": 14,
    "torso": 80,
    "shoulderHalf": 30,
    "shrug": 7,
    "neck": 24,
    "head": 16,
    "upperArm": 36,
    "forearm": 32,
    "hipHalf": 12,
    "feet": [[78, 254], [122, 254]],
    "tapLift": 4,
    "tapHz": 3,
}
POSE_FIELDS = ("hipX", "knees", "lean", "twist", "fold", "shrug", "headTilt", "headTurn",
               "headNod", "lArm", "lElbow", "rArm", "rElbow")
REST = {"lArm": 8, "rArm": 8}

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS stretch_keys (
        account_bi TEXT PRIMARY KEY,
        key_version INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        wrapped TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS stretch_profiles (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        installed_at INTEGER NOT NULL CHECK(installed_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS stretch_sessions (
        session_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        day TEXT NOT NULL,
        logged_at INTEGER NOT NULL CHECK(logged_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_stretch_sessions_account "
    "ON stretch_sessions(account_bi,day,logged_at)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_stretch_sessions_account",
    "DROP TABLE IF EXISTS stretch_sessions",
    "DROP TABLE IF EXISTS stretch_profiles",
    "DROP TABLE IF EXISTS stretch_keys",
]

_SCHEMA_READY = False


def _shoulder(side):
    x = FIGURE["hip"][0] + (-1 if side == "l" else 1) * FIGURE["shoulderHalf"]
    return x, FIGURE["hip"][1] - FIGURE["torso"]


def _reach(side, x, y, out=False):
    sx, sy = _shoulder(side)
    upper, fore = FIGURE["upperArm"], FIGURE["forearm"]
    vx, vy = x - sx, y - sy
    distance = max(abs(upper - fore) + 1.0, min(upper + fore - 0.5, math.hypot(vx, vy)))
    sign = -1 if side == "l" else 1
    toward = math.atan2(sign * vx, vy)
    inner = math.acos(max(-1.0, min(1.0, (upper * upper + distance * distance - fore * fore)
                                    / (2 * upper * distance))))
    best = None
    for arm in (toward + inner, toward - inner):
        ex, ey = sx + sign * math.sin(arm) * upper, sy + math.cos(arm) * upper
        forearm = math.atan2(sign * (x - ex), y - ey)
        rank = abs(ex - FIGURE["hip"][0]) if out else ey
        if best is None or rank > best[0]:
            best = (rank, arm, forearm - arm)
    arm, elbow = math.degrees(best[1]), math.degrees(best[2])
    elbow = (elbow + 180) % 360 - 180
    return round(arm, 1), round(elbow, 1)


def _hands(left=None, right=None, out=False, **extra):
    pose = dict(extra)
    base = FIGURE["hip"][1] - 150
    if left:
        pose["lArm"], pose["lElbow"] = _reach("l", left[0], left[1] + base, out)
    if right:
        pose["rArm"], pose["rElbow"] = _reach("r", right[0], right[1] + base, out)
    return pose


def _motion(period, frames, tap=""):
    count = len(frames)
    full = []
    for index, frame in enumerate(frames):
        pose = {field: 0 for field in POSE_FIELDS}
        pose.update(REST)
        pose.update(frame)
        full.append(dict({"t": round(index / count, 4)}, **{key: pose[key] for key in POSE_FIELDS}))
    full.append(dict(full[0], t=1))
    return {"period": period, "tap": tap, "frames": full}


def _along(start, end, fraction):
    return (start[0] + (end[0] - start[0]) * fraction, start[1] + (end[1] - start[1]) * fraction)


HELD_ARM = (10, -95)


def _held_points(side):
    sx, sy = _shoulder(side)
    sign = -1 if side == "l" else 1
    arm, elbow = HELD_ARM
    upper, fore = FIGURE["upperArm"], FIGURE["forearm"]
    ex = sx + sign * math.sin(math.radians(arm)) * upper
    ey = sy + math.cos(math.radians(arm)) * upper
    wx = ex + sign * math.sin(math.radians(arm + elbow)) * fore
    wy = ey + math.cos(math.radians(arm + elbow)) * fore
    return [(sx, sy + 4), (ex, ey), (wx, wy)]


def _tap_arm(side):
    held, tapper = side, ("r" if side == "l" else "l")
    top, elbow, wrist = _held_points(held)
    base = FIGURE["hip"][1] - 150
    path = [_along(top, elbow, 0.1), _along(top, elbow, 0.6), elbow, _along(elbow, wrist, 0.5),
            _along(elbow, wrist, 0.9), _along(elbow, wrist, 0.5), elbow, _along(top, elbow, 0.6)]
    frames = []
    for point in path:
        pose = {held + "Arm": HELD_ARM[0], held + "Elbow": HELD_ARM[1]}
        pose.update(_hands(**{("left" if tapper == "l" else "right"): (point[0], point[1] - 4 - base)}))
        frames.append(pose)
    return _motion(6, frames, "right" if tapper == "r" else "left")


def _both(points, **extra):
    return [_hands(left=(100 - dx, y), right=(100 + dx, y), **extra) for dx, y in points]


MOTIONS = {
    "breathe": _motion(8, [{"lArm": 10, "rArm": 10}, {"lArm": 168, "rArm": 168, "lElbow": 8, "rElbow": 8,
                                                      "headNod": -0.3}]),
    "hip-circles": _motion(4, [
        _hands(left=(84, 120), right=(116, 120), out=True, hipX=shift, knees=dip)
        for shift, dip in ((-1, 0), (0, 0.5), (1, 0), (0, 0))
    ]),
    "arm-swings": _motion(2.6, [
        {"twist": -1, "headTurn": -0.6, "lArm": 75, "lElbow": 15, "rArm": -45, "rElbow": 55, "hipX": -0.15},
        {"twist": 1, "headTurn": 0.6, "lArm": -45, "lElbow": 55, "rArm": 75, "rElbow": 15, "hipX": 0.15},
    ]),
    "side-bend-left": _motion(4, [
        {"lean": -16, "rArm": 160, "rElbow": 20, "lArm": 12, "headTilt": -6},
        {"lean": -24, "rArm": 168, "rElbow": 14, "lArm": 16, "headTilt": -8},
    ]),
    "side-bend-right": _motion(4, [
        {"lean": 16, "lArm": 160, "lElbow": 20, "rArm": 12, "headTilt": 6},
        {"lean": 24, "lArm": 168, "lElbow": 14, "rArm": 16, "headTilt": 8},
    ]),
    "twist-left": _motion(4, [
        {"twist": -0.8, "headTurn": -0.7, "lArm": 55, "lElbow": 40, "rArm": -35, "rElbow": 70},
        {"twist": -1, "headTurn": -0.9, "lArm": 60, "lElbow": 40, "rArm": -40, "rElbow": 70},
    ]),
    "twist-right": _motion(4, [
        {"twist": 0.8, "headTurn": 0.7, "rArm": 55, "rElbow": 40, "lArm": -35, "lElbow": 70},
        {"twist": 1, "headTurn": 0.9, "rArm": 60, "rElbow": 40, "lArm": -40, "lElbow": 70},
    ]),
    "neck-rolls": _motion(8, [
        {"headTilt": -30}, {"headTilt": -15, "headNod": 0.7}, {"headNod": 1},
        {"headTilt": 15, "headNod": 0.7}, {"headTilt": 30}, {"headTilt": 15, "headNod": 0.7},
        {"headNod": 1}, {"headTilt": -15, "headNod": 0.7},
    ]),
    "neck-tilt-left": _motion(5, [{"headTilt": -28}, {"headTilt": -36, "lArm": 10, "rArm": 4}]),
    "neck-tilt-right": _motion(5, [{"headTilt": 28}, {"headTilt": 36, "rArm": 10, "lArm": 4}]),
    "neck-turns": _motion(6, [{"headTurn": -1}, {"headTurn": 0}, {"headTurn": 1}, {"headTurn": 0}]),
    "shoulder-rolls": _motion(3, [
        {"shrug": 0}, {"shrug": 1, "lArm": 10, "rArm": 10}, {"shrug": 0.6, "lArm": 14, "rArm": 14},
        {"shrug": -0.4, "lArm": 6, "rArm": 6},
    ]),
    "chest-opener": _motion(5, [
        {"lArm": 4, "lElbow": -12, "rArm": 4, "rElbow": -12, "headNod": -0.3, "shrug": -0.3},
        {"lArm": 10, "lElbow": -22, "rArm": 10, "rElbow": -22, "headNod": -0.6, "shrug": -0.6},
    ]),
    "forward-fold": _motion(6, [
        {"fold": 0.55, "headNod": 0.8, "knees": 0.3, "lArm": 6, "rArm": 6},
        {"fold": 0.8, "headNod": 1, "knees": 0.35, "lArm": 4, "rArm": 4},
    ]),
    "reach-up": _motion(4, [
        {"lArm": 176, "rArm": 164, "lElbow": 0, "rElbow": 6, "lean": 3},
        {"lArm": 164, "rArm": 176, "lElbow": 6, "rElbow": 0, "lean": -3},
    ]),
    "tap-collarbones": _motion(4, _both([(14, 78), (20, 77), (26, 76), (20, 77)]), "both"),
    "tap-arm-left": _tap_arm("l"),
    "tap-arm-right": _tap_arm("r"),
    "tap-armpits": _motion(4, [
        _hands(left=(126, 94), right=(74, 94)), _hands(left=(124, 98), right=(76, 98)),
    ], "both"),
    "tap-sides": _motion(5, _both([(26, 98), (24, 114), (22, 130), (24, 114)]), "both"),
    "tap-head": _motion(4, _both([(8, 32), (16, 33), (22, 37), (16, 33)]), "both"),
    "tap-eyes": _motion(5, _both([(6, 40), (12, 39), (17, 45), (12, 51), (6, 50)]), "both"),
    "tap-face-neck": _motion(5, _both([(12, 54), (14, 60), (13, 68), (15, 76)]), "both"),
    "tap-chest": _motion(3, [_hands(left=(96, 92), right=(104, 92)), _hands(left=(96, 98), right=(104, 98))],
                         "both"),
    "shake-out": _motion(0.6, [
        {"knees": 0.25, "lArm": 12, "rArm": 4, "shrug": 0.2}, {"knees": 0, "lArm": 4, "rArm": 12},
    ]),
}

DESK_STEPS = [
    ("stand-tall", "Stand tall", 20, "breathe",
     "Stand up and step away from your desk. Feet hip width apart. Breathe in and sweep your arms up, "
     "then breathe out and let them float down.", ""),
    ("hip-circles", "Hip circles", 30, "hip-circles",
     "Hands on your hips. Make big slow circles with your hips, as if you are drawing a circle on the "
     "floor with your belly button.", "Now circle the other way."),
    ("arm-swings", "Arm swings", 40, "arm-swings",
     "Let your arms hang loose and swing them from side to side. Let your torso rotate with them, "
     "and let your arms wrap around you. Keep your hips facing forward.",
     "Keep it easy and loose. Let your head turn with you."),
    ("side-bend-left", "Side stretch to the left", 20, "side-bend-left",
     "Reach your right arm up and over your head and lean to the left. Feel the stretch all along "
     "your right side.", "Breathe into your ribs."),
    ("side-bend-right", "Side stretch to the right", 20, "side-bend-right",
     "Now reach your left arm up and over and lean to the right. Long through the left side.",
     "Breathe into your ribs."),
    ("twist-left", "Twist to the left", 20, "twist-left",
     "Keep your hips square and rotate your chest to the left. Look over your left shoulder.",
     "Grow tall, then twist a little further."),
    ("twist-right", "Twist to the right", 20, "twist-right",
     "Come back through the middle and rotate to the right. Look over your right shoulder.",
     "Grow tall, then twist a little further."),
    ("neck-rolls", "Neck rolls", 30, "neck-rolls",
     "Drop your chin to your chest and slowly roll your head from one shoulder to the other, "
     "across the front. Keep it slow and gentle.", "Keep breathing. Roll back the other way."),
    ("neck-tilt-left", "Neck stretch to the left", 15, "neck-tilt-left",
     "Tilt your left ear toward your left shoulder. Let your right shoulder sink down.", ""),
    ("neck-tilt-right", "Neck stretch to the right", 15, "neck-tilt-right",
     "Tilt your right ear toward your right shoulder. Let your left shoulder sink down.", ""),
    ("neck-turns", "Look left and right", 20, "neck-turns",
     "Slowly turn your head to look over one shoulder, then the other.", ""),
    ("shoulder-rolls", "Shoulder rolls", 20, "shoulder-rolls",
     "Lift your shoulders up to your ears, roll them back, and drop them down. Big slow circles.",
     "Now roll them forward."),
    ("chest-opener", "Chest opener", 20, "chest-opener",
     "Clasp your hands behind your back, squeeze your shoulder blades together, and lift your chest. "
     "This undoes the desk hunch.", "Lift your gaze a little."),
    ("forward-fold", "Forward fold", 25, "forward-fold",
     "Soften your knees and fold forward from your hips. Let your head and arms hang heavy and "
     "your lower back release.", "Nod your head yes and no to loosen your neck."),
    ("reach-up", "Reach up", 15, "reach-up",
     "Slowly roll up, one vertebra at a time, and reach both arms up high. Reach one hand, then the "
     "other, as if you are climbing a ladder.", ""),
]

LYMPH_STEPS = [
    ("belly-breaths", "Deep belly breaths", 30, "breathe",
     "Stand or sit tall. Breathe in slowly through your nose, letting your belly expand, and breathe "
     "out through your mouth. Deep breathing helps move lymph.", ""),
    ("tap-collarbones", "Tap your collarbones", 30, "tap-collarbones",
     "With your fingertips, gently tap just under your collarbones, from the middle out toward your "
     "shoulders. Light, quick taps.", "Keep tapping, nice and light."),
    ("tap-arm-left", "Tap your left arm", 30, "tap-arm-left",
     "Bend your left arm in front of you. With your right hand, tap down the outside of your arm to "
     "your wrist, then tap back up the inside to your armpit.", "Down the outside, up the inside."),
    ("tap-arm-right", "Tap your right arm", 30, "tap-arm-right",
     "Switch sides. Bend your right arm in front of you and tap down the outside with your left hand, "
     "then back up the inside.", "Down the outside, up the inside."),
    ("tap-armpits", "Tap under your arms", 20, "tap-armpits",
     "Cross your arms and tap gently in each armpit, where there are many lymph nodes.", ""),
    ("tap-sides", "Tap down your sides", 20, "tap-sides",
     "Tap down the sides of your body from your armpits to your hips, and back up.", ""),
    ("tap-head", "Tap the top of your head", 20, "tap-head",
     "Use all ten fingertips to tap all over the top of your head, like gentle rain.", ""),
    ("tap-eyes", "Tap around your eyes", 25, "tap-eyes",
     "With your ring fingers, very lightly tap along your eyebrows, out to your temples, and under "
     "your eyes. Keep it feather light.", "Close your eyes if you like."),
    ("tap-face-neck", "Face and neck", 25, "tap-face-neck",
     "Tap your cheeks and along your jaw, then tap down the sides of your neck toward your "
     "collarbones.", "Always tap downward on the neck."),
    ("tap-chest", "Tap your chest", 20, "tap-chest",
     "Tap gently in the middle of your chest, on your breastbone, and keep breathing deeply.", ""),
    ("shake-out", "Shake it out", 20, "shake-out",
     "Bounce gently on your heels and shake out your arms and hands. Let everything go loose.", ""),
]

ROUTINES = (
    ("desk", "Desk stretch", "Hips, arm swings, side stretches, twists and neck: for your back and "
     "posture after a long day at the computer.", "flexibility", DESK_STEPS),
    ("lymph", "Lymph tapping", "Gentle tapping down your arms, around your eyes and head, and along "
     "your neck and chest, with deep breathing.", "mindAndBody", LYMPH_STEPS),
)
ROUTINE_IDS = ("desk", "lymph", "full")


def _step(item):
    step_id, title, seconds, motion, say, half = item
    return {"id": step_id, "title": title, "seconds": seconds, "say": say, "half": half,
            "motion": motion}


def _routines():
    result = []
    for routine_id, title, summary, workout, steps in ROUTINES:
        items = [_step(step) for step in steps]
        result.append({"id": routine_id, "title": title, "summary": summary, "workout": workout,
                       "seconds": sum(step["seconds"] for step in items), "steps": items})
    full = [step for routine in result for step in routine["steps"]]
    result.append({"id": "full", "title": "Both routines",
                   "summary": "The desk stretch, then the lymph tapping routine.",
                   "workout": "flexibility", "seconds": sum(step["seconds"] for step in full),
                   "steps": full})
    return result


def library():
    return {"version": 1, "figure": FIGURE, "poseFields": list(POSE_FIELDS), "motions": MOTIONS,
            "routines": _routines()}


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


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
        return int(round(number))


def _day(value, default=""):
    value = _clean(value, 10)
    return value if DAY_RE.fullmatch(value) else default


def _day_ordinal(day):
    year, month, date = (int(part) for part in day.split("-"))
    if month < 3:
        year -= 1
        month += 12
    era = year // 400
    year_of_era = year - era * 400
    day_of_year = (153 * (month - 3) + 2) // 5 + date - 1
    day_of_era = year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year
    return era * 146097 + day_of_era - 719468


def _ordinal_day(ordinal):
    value = int(ordinal) + 719468
    era = (value if value >= 0 else value - 146096) // 146097
    day_of_era = value - era * 146097
    year_of_era = (day_of_era - day_of_era // 1460 + day_of_era // 36524
                   - day_of_era // 146096) // 365
    year = year_of_era + era * 400
    day_of_year = day_of_era - (365 * year_of_era + year_of_era // 4 - year_of_era // 100)
    month_index = (5 * day_of_year + 2) // 153
    date = day_of_year - (153 * month_index + 2) // 5 + 1
    month = month_index + 3 if month_index < 10 else month_index - 9
    if month <= 2:
        year += 1
    return "%04d-%02d-%02d" % (year, month, date)


def _day_from_ms(ms, offset_minutes=0):
    return _ordinal_day((int(ms) - int(offset_minutes) * 60_000) // 86_400_000)


def _shift_day(day, delta):
    return _ordinal_day(_day_ordinal(day) + int(delta))


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


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


class _Vault:
    def __init__(self, runtime):
        self._runtime = runtime
        self._keys = {}

    def __getattr__(self, name):
        return getattr(self._runtime, name)

    async def key(self, account_bi, create=False):
        account_bi = str(account_bi or "")
        if not account_bi:
            return ""
        material = self._keys.get(account_bi)
        if material:
            return material
        material = await self._stored_key(account_bi)
        if not material and create:
            await self._runtime.d1_run(
                "INSERT INTO stretch_keys(account_bi,key_version,created_at,wrapped) "
                "VALUES(?,?,?,?) ON CONFLICT(account_bi) DO NOTHING",
                account_bi, KEY_VERSION, self._runtime.now(),
                await self._runtime.seal(
                    {"key": self._runtime.new_secret(), "version": KEY_VERSION}),
            )
            material = await self._stored_key(account_bi)
        if material:
            self._keys[account_bi] = material
        return material

    async def _stored_key(self, account_bi):
        row = await self._runtime.d1_first(
            "SELECT wrapped FROM stretch_keys WHERE account_bi=?", account_bi)
        if not row:
            return ""
        opened = await self._runtime.open(row.get("wrapped"))
        return str(opened.get("key") or "") if isinstance(opened, dict) else ""

    async def seal_for(self, account_bi, value):
        material = await self.key(account_bi, create=True)
        if not material:
            raise RuntimeError("stretch: no key for this account")
        return await self._runtime.seal_with(material, value)

    async def open_for(self, account_bi, stored):
        if not stored:
            return {}
        material = await self.key(account_bi)
        if not material:
            return {}
        opened = await self._runtime.open_with(material, stored)
        return opened if isinstance(opened, dict) else {}

    def forget(self, account_bi):
        self._keys.pop(str(account_bi or ""), None)


def _vault(runtime):
    return runtime if isinstance(runtime, _Vault) else _Vault(runtime)


def _profile_data(data, existing=None):
    result = {"voice": True, "routine": "desk", "tzOffsetMinutes": 0}
    result.update({key: value for key, value in (existing or {}).items() if key in result})
    if isinstance(data.get("voice"), bool):
        result["voice"] = data["voice"]
    if "routine" in data:
        routine = _clean(data.get("routine"), 16).lower()
        if routine in ROUTINE_IDS:
            result["routine"] = routine
    if "tzOffsetMinutes" in data:
        result["tzOffsetMinutes"] = max(-840, min(840, _integer(data.get("tzOffsetMinutes"), 0)))
    if result["routine"] not in ROUTINE_IDS:
        result["routine"] = "desk"
    result["voice"] = result["voice"] is not False
    return result


async def _profile_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_name,installed_at,updated_at,data FROM stretch_profiles "
        "WHERE account_bi=?", account_bi)


async def _profile(runtime, account_bi, row=None):
    row = row or await _profile_row(runtime, account_bi)
    if not row:
        return _profile_data({})
    return _profile_data({}, await runtime.open_for(account_bi, row.get("data")))


async def _install(runtime, account_bi, actor, data):
    profile = _profile_data(data if isinstance(data, dict) else {})
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO stretch_profiles(account_bi,account_name,installed_at,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(account_bi) DO UPDATE SET "
        "account_name=excluded.account_name",
        account_bi, actor, now, now, await runtime.seal_for(account_bi, profile),
    )
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {"ok": True, "installed": True, "plugin": PLUGIN_ID})


async def _save_profile(runtime, row, account_bi, actor, data):
    profile = _profile_data(data, await runtime.open_for(account_bi, row.get("data")))
    await runtime.d1_run(
        "UPDATE stretch_profiles SET account_name=?,updated_at=?,data=? WHERE account_bi=?",
        actor, runtime.now(), await runtime.seal_for(account_bi, profile), account_bi,
    )
    await runtime.audit(actor, "stretch.profile.save", "stretch_profile", actor)
    return profile


def _project_session(row, data):
    return {
        "id": str(row.get("session_id") or ""),
        "day": str(row.get("day") or ""),
        "loggedAt": int(row.get("logged_at") or 0),
        "routine": str(data.get("routine") or "desk"),
        "seconds": max(0, _integer(data.get("seconds"))),
        "steps": max(0, _integer(data.get("steps"))),
        "stepsTotal": max(0, _integer(data.get("stepsTotal"))),
        "completed": data.get("completed") is True,
        "at": _integer(data.get("at")),
        "source": str(data.get("source") or "web"),
        "health": data.get("health") is True,
    }


async def _sessions(runtime, account_bi, since="", limit=20000):
    rows = await runtime.d1_all(
        "SELECT session_id,day,logged_at,data FROM stretch_sessions "
        "WHERE account_bi=? AND day>=? ORDER BY day DESC,logged_at DESC LIMIT ?",
        account_bi, since, int(limit))
    result = []
    for row in rows or []:
        result.append(_project_session(row, await runtime.open_for(account_bi, row.get("data"))))
    return result


async def _active_days(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT DISTINCT day FROM stretch_sessions WHERE account_bi=? ORDER BY day", account_bi)
    return [str(row.get("day") or "") for row in rows or [] if row.get("day")]


def _streaks(days, today):
    ordinals = sorted({_day_ordinal(day) for day in days if day <= today})
    best, run, previous = 0, 0, None
    for ordinal in ordinals:
        run = run + 1 if previous is not None and ordinal == previous + 1 else 1
        best = max(best, run)
        previous = ordinal
    present = set(ordinals)
    cursor = _day_ordinal(today)
    if cursor not in present:
        cursor -= 1
    streak = 0
    while cursor in present:
        streak += 1
        cursor -= 1
    return streak, best


async def _overview(runtime, row, account_bi, actor, today):
    profile = await _profile(runtime, account_bi, row)
    first_chart_day = _shift_day(today, -(CHART_DAYS - 1))
    recent = await _sessions(runtime, account_bi, first_chart_day)
    totals = {}
    for item in recent:
        slot = totals.setdefault(item["day"], {"sessions": 0, "seconds": 0})
        slot["sessions"] += 1
        slot["seconds"] += item["seconds"]
    days = []
    for offset in range(CHART_DAYS - 1, -1, -1):
        day = _shift_day(today, -offset)
        slot = totals.get(day, {"sessions": 0, "seconds": 0})
        days.append({"day": day, "sessions": slot["sessions"], "seconds": slot["seconds"]})
    streak, best = _streaks(await _active_days(runtime, account_bi), today)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM stretch_sessions WHERE account_bi=?", account_bi)
    week_start = _shift_day(today, -6)
    today_slot = totals.get(today, {"sessions": 0, "seconds": 0})
    return _response(runtime, {
        "ok": True,
        "installed": True,
        "actor": actor,
        "day": today,
        "profile": profile,
        "library": library(),
        "today": {"day": today, "sessions": today_slot["sessions"], "seconds": today_slot["seconds"]},
        "summary": {
            "streak": streak,
            "bestStreak": best,
            "sessions": int((count or {}).get("n") or 0),
            "weekSeconds": sum(item["seconds"] for item in recent if item["day"] >= week_start),
            "weekSessions": sum(1 for item in recent if item["day"] >= week_start),
        },
        "days": days,
        "recent": [item for item in recent if item["day"] <= today][:MAX_RECENT],
    })


async def _create_session(runtime, account_bi, actor, data, today):
    routine = _clean(data.get("routine"), 16).lower()
    if routine not in ROUTINE_IDS:
        return _response(runtime, {"error": "invalid_routine"}, 400)
    seconds = _integer(data.get("seconds"), -1)
    if seconds < 1 or seconds > MAX_SESSION_SECONDS:
        return _response(runtime, {"error": "invalid_seconds"}, 400)
    day = _day(data.get("day")) or today
    if day > today or _day_ordinal(day) < _day_ordinal(today) - 366:
        return _response(runtime, {"error": "invalid_day"}, 400)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM stretch_sessions WHERE account_bi=? AND day=?", account_bi, day)
    if int((count or {}).get("n") or 0) >= MAX_SESSIONS_PER_DAY:
        return _response(runtime, {"error": "day_full"}, 400)
    total = max(0, min(200, _integer(data.get("stepsTotal"))))
    steps = max(0, min(total or 200, _integer(data.get("steps"))))
    source = _clean(data.get("source"), 16).lower()
    now = runtime.now()
    stored = {
        "routine": routine,
        "seconds": seconds,
        "steps": steps,
        "stepsTotal": total,
        "completed": data.get("completed") is True,
        "at": max(0, _integer(data.get("at"), now)) or now,
        "source": source if source in SOURCES else "web",
        "health": data.get("health") is True,
    }
    session_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO stretch_sessions(session_id,account_bi,day,logged_at,data) VALUES(?,?,?,?,?)",
        session_id, account_bi, day, now, await runtime.seal_for(account_bi, stored))
    await runtime.audit(actor, "stretch.session.create", "stretch_session", actor)
    row = {"session_id": session_id, "day": day, "logged_at": now}
    return _response(runtime, {"ok": True, "session": _project_session(row, stored)}, 201)


async def _delete_session(runtime, account_bi, actor, session_id):
    row = await runtime.d1_first(
        "SELECT session_id FROM stretch_sessions WHERE session_id=? AND account_bi=?",
        session_id, account_bi)
    if not row:
        return _response(runtime, {"error": "session_not_found"}, 404)
    await runtime.d1_run(
        "DELETE FROM stretch_sessions WHERE session_id=? AND account_bi=?", session_id, account_bi)
    await runtime.audit(actor, "stretch.session.delete", "stretch_session", actor)
    return _response(runtime, {"ok": True, "deleted": True})


async def account_installed(runtime, account_bi):
    return bool(await _profile_row(runtime, account_bi))


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM stretch_sessions WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def export_account_data(runtime, account_bi, actor):
    runtime = _vault(runtime)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-stretch-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "profile": await _profile(runtime, account_bi),
        "sessions": await _sessions(runtime, account_bi),
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    for table in ("stretch_sessions", "stretch_profiles", "stretch_keys"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE account_bi=?", account_bi)
    if isinstance(runtime, _Vault):
        runtime.forget(account_bi)


def _export_filename(actor):
    return "forkmesh-stretch-" + actor + ".json"


async def _uninstall(runtime, account_bi, actor):
    had_data = await account_has_data(runtime, account_bi)
    export = await export_account_data(runtime, account_bi, actor) if had_data else None
    await purge_account_data(runtime, account_bi, actor)
    await runtime.audit(actor, "plugin.uninstall", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {
        "ok": True,
        "uninstalled": True,
        "plugin": PLUGIN_ID,
        "hadData": had_data,
        "filename": _export_filename(actor),
        "export": export,
    })


def _allowed_methods(parts):
    head = parts[0] if parts else ""
    if not parts:
        return ("GET",)
    if head in ("install", "uninstall") and len(parts) == 1:
        return ("POST",)
    if head == "profile" and len(parts) == 1:
        return ("GET", "PUT")
    if head in ("export", "library") and len(parts) == 1:
        return ("GET",)
    if head == "sessions":
        if len(parts) == 1:
            return ("POST",)
        if len(parts) == 2:
            return ("DELETE",)
    return ()


async def handle(runtime, path):
    runtime = _vault(runtime)
    parts = _route(path)
    if parts is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    allowed = _allowed_methods(parts)
    if not allowed:
        return _response(runtime, {"error": "not_found"}, 404)
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    if parts == ("library",):
        return _response(runtime, dict(library(), ok=True))
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error},
                             413 if error == "payload_too_large" else 400)
        if not isinstance(data, dict):
            data = {}
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    await _ensure_tables(runtime)
    params = runtime.query_params() if hasattr(runtime, "query_params") else {}
    head = parts[0] if parts else ""
    if head == "install":
        return await _install(runtime, account_bi, actor, data)
    row = await _profile_row(runtime, account_bi)
    if not row:
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor,
                                       "library": library()})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    today = _day(params.get("day")) or _day(data.get("today"))
    if not today:
        profile = await _profile(runtime, account_bi, row)
        today = _day_from_ms(runtime.now(), profile["tzOffsetMinutes"])
    if not parts:
        return await _overview(runtime, row, account_bi, actor, today)
    if head == "uninstall":
        return await _uninstall(runtime, account_bi, actor)
    if head == "profile":
        if method == "GET":
            return _response(runtime, {"ok": True, "profile": await _profile(runtime, account_bi, row)})
        profile = await _save_profile(runtime, row, account_bi, actor, data)
        return _response(runtime, {"ok": True, "profile": profile})
    if head == "export":
        return _response(runtime, {
            "ok": True, "filename": _export_filename(actor),
            "export": await export_account_data(runtime, account_bi, actor),
        })
    if len(parts) == 1:
        return await _create_session(runtime, account_bi, actor, data, today)
    if not ID_RE.fullmatch(parts[1]):
        return _response(runtime, {"error": "session_not_found"}, 404)
    return await _delete_session(runtime, account_bi, actor, parts[1])
