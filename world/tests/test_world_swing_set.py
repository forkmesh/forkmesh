#!/usr/bin/env python3
"""Scene contracts for the town swing set and riding it."""

from pathlib import Path
import re
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")


def test_swing_set_has_three_clickable_seats():
    seats = re.search(r"const SWING_SEAT_COUNT = (\d+);", SCENE)
    assert seats, "swing seat count constant missing"
    # The issue asks for a swing set up to three people can ride at once.
    assert int(seats.group(1)) == 3
    build = SCENE.split("const swingSet = new THREE.Group();", 1)[1].split(
        "const activeLeaderboardSign", 1
    )[0]
    assert "seat.userData.swingSeat = seatIndex;" in build
    assert "rope.userData.swingSeat = seatIndex;" in build
    assert build.count("interactive.push") >= 2
    assert "world.add(swingSet);" in build


def test_clicking_a_seat_rides_and_clicking_again_hops_off():
    assert "function rideSwing(index)" in SCENE
    ride = SCENE.split("function rideSwing", 1)[1].split(
        "function applySwingRidePose", 1
    )[0]
    # A second click on the seat you are riding is the get-off gesture.
    assert "dismountSwing();" in ride
    assert "activity: SWING_RIDING_ACTIVITY" in ride
    # The pointer dispatch routes seat clicks to the ride, and a swingSeat of
    # index 0 must not be dropped by a truthiness check.
    assert "Number.isInteger(hit?.object?.userData?.swingSeat)" in SCENE
    assert "rideSwing(hit.object.userData.swingSeat);" in SCENE


def test_riders_sit_on_the_moving_plank_every_frame():
    pose = SCENE.split("function applySwingRidePose", 1)[1].split(
        "function dismountSwing", 1
    )[0]
    # The seat position is read live from the world matrix so the avatar and
    # both camera modes follow the pendulum arc.
    assert "swing.seat.getWorldPosition(new THREE.Vector3())" in pose
    assert "seatedAvatarY(" in pose
    assert "applySeatedLegPose(player);" in pose
    walk = SCENE.split("function walkPlayer", 1)[1].split(
        "function updateRemotePlayers", 1
    )[0]
    assert "applySwingRidePose();" in walk
    # Movement input remains the universal stand-up gesture alongside the
    # second click.
    assert walk.index("if (swingRide)") < walk.index("if (benchSeat)")


def test_double_click_on_a_swing_rides_instead_of_dashing():
    dbl = SCENE.split("function handleDoubleClick", 1)[1].split(
        "function handlePointerCancel", 1
    )[0]
    assert "swingSeat" in dbl
    assert dbl.index("rideSwing") < dbl.index("dashTarget = point")


def test_swing_animation_is_a_continuous_pendulum():
    build = SCENE.split("const swingPendulumOmega", 1)[1].split(
        "setShadows(swingSet);", 1
    )[0]
    # Amplitude eases toward the rider's target and the phase is continuous,
    # so speed changes never snap the seat sideways.
    assert "swing.amplitude += (target - swing.amplitude)" in build
    assert "swing.pivot.rotation.x = swing.amplitude * Math.sin(swing.phase);" in build


def test_riders_lean_front_to_back_only():
    # The swing set faces the fountain at an angle, so a rider pitched about
    # the default XYZ order's world X axis reads as rocking side to side. Yaw
    # has to be applied before the pitch for the lean to follow the ropes.
    pose = SCENE.split("function applySwingRidePose", 1)[1].split(
        "function dismountSwing", 1
    )[0]
    assert "player.rotation.order = \"YXZ\";" in pose
    assert pose.index("rotation.order") < pose.index("player.rotation.x =")
    remote = SCENE.split("function updateRemotePlayers", 1)[1].split(
        "function updateForkbot", 1
    )[0]
    assert "avatar.rotation.order = \"YXZ\";" in remote
    assert remote.index("rotation.order") < remote.index("avatar.rotation.x =")


def test_remote_visitors_render_riding_their_claimed_swings():
    remote = SCENE.split("function updateRemotePlayers", 1)[1].split(
        "function updateForkbot", 1
    )[0]
    # Occupancy is re-derived every frame from where visitors stand, which is
    # what caps the ride at three people and refuses clicks on taken seats.
    assert "swing.remoteId = \"\";" in remote
    assert "SWING_REST_CLAIM_DISTANCE_SQ" in remote
    assert "applySeatedLegPose(avatar);" in remote


def test_shell_exposes_speed_control_and_ride_feedback():
    assert "data-world-swing-panel" in APP
    assert "data-world-swing-speed" in APP
    assert "data-world-swing-dismount" in APP
    assert "setSwingSpeed(" in APP
    assert "onSwingRide: (state) => this.handleSwingRide(state)," in APP
    # Riding must survive the landmark-proximity relabel like the campfire
    # sit does, so other visitors keep rendering the rider on the swing.
    assert "this.lastMovement.activity !== SWING_RIDING_ACTIVITY" in APP
    assert ".world-swing-panel" in CSS
