from pathlib import Path
from world_scene_source import world_scene_source


SCENE = world_scene_source()


def _function(name):
    start = SCENE.index(f"  function {name}(")
    end = SCENE.index("\n  }\n", start)
    return SCENE[start:end]


def test_zip_line_spans_the_office_roof_and_the_beach():
    for contract in (
        'zipLine.name = "forkmesh-roof-beach-zip-line"',
        'const zipLineRoofFloorY = officeFloorY("rooftop") + 0.38',
        "BEACH_CENTER_X + zipLineBeachDirection.x * ZIP_LINE_BEACH_OFFSET",
        '"forkmesh-roof-beach-zip-line-roof-mast"',
        '"forkmesh-roof-beach-zip-line-beach-mast",\n    beachScene,',
        'officeFloorGroups.get("rooftop").add(zipLineStation)',
        'child.userData.interactive = "office-roof-zipline"',
        'if (mode === "beach") return [beachScene, zipLine]',
    ):
        assert contract in SCENE
    # The landing must sit inside the beach horizon, where the scene switch
    # happens, or the rider would land in the hidden beach.
    assert "const ZIP_LINE_BEACH_OFFSET = 44;" in SCENE
    assert "const ZIP_LINE_BEACH_ENTRY_RADIUS = 70;" in SCENE


def test_zip_line_ride_starts_on_the_roof_and_enters_the_beach_scene():
    start = _function("startZipLineRide")
    assert 'officeCurrentFloorId !== "rooftop"' in start
    assert "parachute: false" in start

    ride = _function("updateZipLineRide")
    assert "player.position.distanceToSquared(ride.lastPosition) > 1" in ride
    assert "setBeachScene(true, { relocate: false })" in ride
    assert "if (arrived) endZipLineRide();" in ride

    beach = _function("setBeachScene")
    assert "if (relocate) {" in beach

    assert "if (zipLineRide && updateZipLineRide(delta, time)) {" in SCENE
    assert "toggleNearestBikeRide() ||\n        startNearbyZipLineRide()" in SCENE
    assert 'interactive === "office-roof-zipline"' in SCENE
    assert "rideZipLine: startZipLineRide," in SCENE
