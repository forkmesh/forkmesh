import importlib.util
from pathlib import Path

WORLD_PATH = Path(__file__).resolve().parents[1] / "src" / "world.py"
spec = importlib.util.spec_from_file_location("forkmesh_world_race_protocol", WORLD_PATH)
world = importlib.util.module_from_spec(spec)
spec.loader.exec_module(world)


def _move(payload, current=None):
    current = current or world.default_presence("peer", 1000)
    kind, state = world.sanitize_message({"type": "move", **payload}, current, 2000)
    assert kind == "move"
    return state


def test_a_flying_move_frame_relays_a_bounded_race_state():
    state = _move({
        "x": 10,
        "z": 5,
        "moving": True,
        "race": {"ring": 3.4, "lap": 21_400.7, "best": 52_310, "name": "never"},
    })
    assert state["race"] == {"ring": 3, "lap": 21_401, "best": 52_310}
    delta = world.movement_delta(state)
    assert delta["race"] == {"ring": 3, "lap": 21_401, "best": 52_310}
    assert "never" not in repr(delta)


def test_race_values_are_clamped_and_junk_is_dropped():
    state = _move({
        "race": {"ring": 9999, "lap": -5, "best": float("inf")},
    })
    assert state["race"] == {
        "ring": world.WORLD_DRONE_RACE_MAX_RINGS,
        "lap": 0,
        "best": 0,
    }
    assert _move({"race": {"ring": True, "lap": "x", "best": None}})["race"] == {
        "ring": 0, "lap": 0, "best": 0,
    }
    assert _move({"race": "fast"})["race"] is None


def test_a_move_without_race_clears_it_and_keeps_the_plain_frame_shape():
    flying = _move({"race": {"ring": 2, "lap": 5000, "best": 0}})
    walking = _move({"x": 1, "moving": True}, flying)
    assert walking["race"] is None
    delta = world.movement_delta(walking)
    assert set(delta) == {
        "type", "id", "x", "y", "z", "yaw", "lookYaw", "lookPitch",
        "moving", "updatedAt",
    }
    assert "race" not in world.WORLD_PUBLIC_FIELDS
