#!/usr/bin/env python3
"""Runtime contracts for the unified five-storey World office campus."""

import ast
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2] / "app"
TOWER_MODULE = ROOT.parent / "world" / "public" / "world" / "world-office-tower.js"
MOVEMENT_MODULE = ROOT.parent / "world" / "public" / "world" / "scene" / "movement.js"
ENTRY_MODULE = ROOT / "src" / "entry.py"


def run_tower_script(body):
    """Run a small assertion probe against the browser-independent ES module."""

    script = f"""
      import * as tower from {json.dumps(TOWER_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def entry_constant(name):
    tree = ast.parse(ENTRY_MODULE.read_text(encoding="utf-8"))
    for node in tree.body:
        if (
            isinstance(node, ast.Assign)
            and any(getattr(target, "id", "") == name for target in node.targets)
        ):
            return ast.literal_eval(node.value)
    raise AssertionError(f"{name} not found")


def test_tower_is_a_four_floor_hexagon_a_quarter_of_the_doubled_width():
    assert TOWER_MODULE.exists()
    result = run_tower_script(
        """
        process.stdout.write(JSON.stringify({
          count: tower.OFFICE_FLOOR_COUNT,
          width: tower.OFFICE_WIDTH,
          depth: tower.OFFICE_DEPTH,
          radius: tower.OFFICE_HEX_RADIUS,
          apothem: tower.OFFICE_HEX_APOTHEM,
          sides: tower.OFFICE_HEX_SIDES.map((side) => side.id),
          sideLengths: tower.OFFICE_HEX_SIDES.map((side) => side.length),
          corners: tower.OFFICE_HEX_CORNERS,
          frontNormal: [
            tower.officeHexSide("front").normalX,
            tower.officeHexSide("front").normalZ,
          ],
          center: tower.OFFICE_ISLAND_CENTER,
          entranceZ: tower.OFFICE_ISLAND_CENTER[2] + tower.OFFICE_FRONT_Z,
          floorHeight: tower.OFFICE_FLOOR_HEIGHT,
          towerHeight: tower.OFFICE_TOWER_HEIGHT,
          floors: tower.OFFICE_FLOORS.map((floor) => ({
            id: floor.id,
            level: floor.level,
            team: floor.team,
            publicForMembers: floor.publicForMembers === true,
            y: tower.officeFloorY(floor.id),
          })),
        }));
        """
    )

    assert result["count"] == 4
    assert len(result["floors"]) == 4
    assert [floor["level"] for floor in result["floors"]] == list(range(4))
    assert len({floor["id"] for floor in result["floors"]}) == 4
    assert [floor["id"] for floor in result["floors"]] == [
        "lobby",
        "marketing",
        "engineering",
        "rooftop",
    ]
    assert 0.24 <= result["width"] / 340 <= 0.26
    assert result["apothem"] == 37
    assert abs(result["radius"] - 74 / 3 ** 0.5) < 1e-9
    assert abs(result["width"] - 2 * result["radius"]) < 1e-9
    assert result["depth"] == 74
    assert result["sides"] == [
        "front-right", "front", "front-left", "back-left", "back", "back-right",
    ]
    assert all(abs(length - result["radius"]) < 1e-9 for length in result["sideLengths"])
    assert len(result["corners"]) == 6
    assert all(
        abs((corner["x"] ** 2 + corner["z"] ** 2) ** 0.5 - result["radius"]) < 1e-6
        for corner in result["corners"]
    )
    assert result["frontNormal"] == [0, 1]
    assert result["center"] == [0, 0, -207]
    assert result["entranceZ"] == -170
    assert result["floorHeight"] == 16
    assert result["towerHeight"] == 4 * 16
    assert [floor["y"] for floor in result["floors"]] == [
        level * 16 for level in range(4)
    ]

    public_floors = {
        floor["id"] for floor in result["floors"] if floor["publicForMembers"]
    }
    # Marketing is a department storey, not a common floor: only the lobby and
    # the rooftop patio are shared by every signed-in member.
    assert public_floors == {"lobby", "rooftop"}
    restricted = [
        floor for floor in result["floors"] if not floor["publicForMembers"]
    ]
    assert len(restricted) == 2
    assert all(floor["team"] for floor in restricted)
    assert {floor["id"] for floor in restricted} == {
        "marketing",
        "engineering",
    }


def test_authenticated_members_get_two_common_floors_but_team_floors_stay_restricted():
    result = run_tower_script(
        """
        const guest = tower.normalizeOfficeFloorAccess({
          authenticated: false,
          allowedFloorIds: ["lobby", "engineering"],
        });
        const member = tower.normalizeOfficeFloorAccess({
          authenticated: true,
          account: "Alice",
        });
        const teamHintOnly = tower.normalizeOfficeFloorAccess({
          authenticated: true,
          account: "Alice",
          teams: ["engineering", "marketing"],
        });
        const serverAuthorized = tower.normalizeOfficeFloorAccess({
          authenticated: true,
          account: "Alice",
          teams: ["engineering"],
          allowedFloorIds: ["engineering"],
        });
        const marketingMember = tower.normalizeOfficeFloorAccess({
          authenticated: true,
          account: "Mallory",
          teams: ["marketing"],
          allowedFloorIds: ["marketing"],
        });
        process.stdout.write(JSON.stringify({
          guestCanLobby: tower.canAccessOfficeFloor(guest, "lobby"),
          guestCanEngineering:
            tower.canAccessOfficeFloor(guest, "engineering"),
          defaults: tower.OFFICE_FLOORS
            .filter((floor) => tower.canAccessOfficeFloor(member, floor.id))
            .map((floor) => floor.id),
          teamHintCanEngineering:
            tower.canAccessOfficeFloor(teamHintOnly, "engineering"),
          teamHintCanMarketing:
            tower.canAccessOfficeFloor(teamHintOnly, "marketing"),
          authorizedCanEngineering:
            tower.canAccessOfficeFloor(serverAuthorized, "engineering"),
          authorizedCannotMarketing:
            tower.canAccessOfficeFloor(serverAuthorized, "marketing"),
          marketingMemberCanMarketing:
            tower.canAccessOfficeFloor(marketingMember, "marketing"),
          authorizedStillCannotInfrastructure:
            tower.canAccessOfficeFloor(serverAuthorized, "infrastructure"),
          removedFloor:
            tower.canAccessOfficeFloor(serverAuthorized, "executive"),
          unknownFloor: tower.canAccessOfficeFloor(serverAuthorized, "unknown"),
          account: serverAuthorized.account,
        }));
        """
    )

    assert result["guestCanLobby"] is False
    assert result["guestCanEngineering"] is False
    # Signing in is not a Marketing badge: only the lobby and the rooftop are
    # granted without a server-issued team floor.
    assert set(result["defaults"]) == {"lobby", "rooftop"}
    # Team names are display/context data. Only the server-provided floor
    # allowlist may unlock a restricted elevator button.
    assert result["teamHintCanEngineering"] is False
    assert result["teamHintCanMarketing"] is False
    assert result["authorizedCanEngineering"] is True
    assert result["authorizedCannotMarketing"] is False
    assert result["marketingMemberCanMarketing"] is True
    assert result["authorizedStillCannotInfrastructure"] is False
    # Retired department storeys are gone from the tower entirely: their team
    # names still exist elsewhere, but no elevator button can select them.
    assert result["removedFloor"] is False
    assert result["unknownFloor"] is False
    assert result["account"] == "alice"


def test_team_floor_explanations_match_every_server_alias_exactly():
    server_aliases = entry_constant("OFFICE_FLOOR_TEAM_ALIASES")
    result = run_tower_script(
        """
        const aliases = tower.OFFICE_FLOOR_TEAM_ALIASES;
        const resolved = Object.fromEntries(
          Object.entries(aliases).map(([floorId, teamAliases]) => [
            floorId,
            Object.fromEntries(
              teamAliases.map((team) => [
                team,
                tower.officeFloorsForTeam(team).map((floor) => floor.id),
              ]),
            ),
          ]),
        );
        process.stdout.write(JSON.stringify({
          aliases,
          resolved,
          normalizedInfrastructure: tower.officeFloorsForTeam(
            " Site Reliability ",
          ).map((floor) => floor.id),
          retiredTeam: tower.officeFloorsForTeam("trust-and-safety"),
          unknown: tower.officeFloorsForTeam("not-a-real-team"),
        }));
        """
    )

    assert {
        floor_id: set(aliases)
        for floor_id, aliases in result["aliases"].items()
    } == {
        floor_id: set(aliases)
        for floor_id, aliases in server_aliases.items()
        if floor_id != "infrastructure"
    }
    for floor_id, aliases in result["resolved"].items():
        assert aliases
        assert all(resolved == [floor_id] for resolved in aliases.values())
    assert result["normalizedInfrastructure"] == []
    assert result["retiredTeam"] == []
    assert result["unknown"] == []


def test_office_campus_surface_contains_the_island_and_bridge_but_not_water():
    result = run_tower_script(
        """
        const avatarRadius = tower.OFFICE_AVATAR_RADIUS;
        const routeZ =
          (tower.OFFICE_BRIDGE_START_Z + tower.OFFICE_BRIDGE_END_Z) / 2;
        const routeX = tower.OFFICE_ISLAND_CENTER[0];
        process.stdout.write(JSON.stringify({
          islandCenter: tower.officeCampusSurfaceContains(
            tower.OFFICE_ISLAND_CENTER[0],
            tower.OFFICE_ISLAND_CENTER[2],
            avatarRadius,
          ),
          islandSafeEdge: tower.officeCampusSurfaceContains(
            tower.OFFICE_ISLAND_CENTER[0] +
              tower.OFFICE_ISLAND_RADIUS - avatarRadius,
            tower.OFFICE_ISLAND_CENTER[2],
            avatarRadius,
          ),
          beyondIslandEdge: tower.officeCampusSurfaceContains(
            tower.OFFICE_ISLAND_CENTER[0] +
              tower.OFFICE_ISLAND_RADIUS - avatarRadius + 0.01,
            tower.OFFICE_ISLAND_CENTER[2],
            avatarRadius,
          ),
          bridgeCenter: tower.officeCampusSurfaceContains(
            routeX, routeZ, avatarRadius),
          bridgeSafeEdge: tower.officeCampusSurfaceContains(
            routeX + tower.OFFICE_BRIDGE_WIDTH / 2 - avatarRadius,
            routeZ,
            avatarRadius,
          ),
          beyondBridgeRail: tower.officeCampusSurfaceContains(
            routeX + tower.OFFICE_BRIDGE_WIDTH / 2 - avatarRadius + 0.01,
            routeZ,
            avatarRadius,
          ),
          mainShoreOverlap: tower.officeCampusSurfaceContains(
            routeX, tower.OFFICE_BRIDGE_START_Z, 0),
          waterBesideBridge: tower.officeCampusSurfaceContains(
            routeX + 25, routeZ, 0),
          beforeBridge: tower.officeCampusSurfaceContains(
            routeX, tower.OFFICE_BRIDGE_START_Z + 1, 0),
          invalid: tower.officeCampusSurfaceContains(Number.NaN, routeZ, 0),
          beyondSouthShore: tower.officeCampusSurfaceContains(
            routeX,
            tower.OFFICE_ISLAND_CENTER[2] + tower.OFFICE_ISLAND_RADIUS + 0.01,
            0,
          ),
          islandHoldsTheTower: tower.OFFICE_HEX_CORNERS.every((corner) =>
            tower.officeCampusSurfaceContains(
              tower.OFFICE_ISLAND_CENTER[0] + corner.x,
              tower.OFFICE_ISLAND_CENTER[2] + corner.z,
              avatarRadius,
            )),
        }));
        """
    )

    assert result == {
        "islandCenter": True,
        "islandSafeEdge": True,
        "beyondIslandEdge": False,
        "bridgeCenter": True,
        "bridgeSafeEdge": True,
        "beyondBridgeRail": False,
        "mainShoreOverlap": True,
        "waterBesideBridge": False,
        "beforeBridge": False,
        "invalid": False,
        "beyondSouthShore": False,
        "islandHoldsTheTower": True,
    }


def test_office_interior_bounds_reserve_avatar_clearance_from_every_wall():
    result = run_tower_script(
        """
        const radius = tower.OFFICE_AVATAR_RADIUS;
        const reach = tower.OFFICE_HEX_APOTHEM - radius - 0.65;
        const cornerReach = (reach * 2) / Math.sqrt(3);
        process.stdout.write(JSON.stringify({
          center: tower.officeInteriorContains(0, 0, radius),
          sideEdges: tower.OFFICE_HEX_SIDES.map((side) =>
            tower.officeInteriorContains(
              side.normalX * reach, side.normalZ * reach, radius)),
          pastSideEdges: tower.OFFICE_HEX_SIDES.map((side) =>
            tower.officeInteriorContains(
              side.normalX * (reach + 0.01), side.normalZ * (reach + 0.01), radius)),
          corners: tower.OFFICE_HEX_CORNERS.map((corner) => {
            const scale = cornerReach / tower.OFFICE_HEX_RADIUS;
            return tower.officeInteriorContains(
              corner.x * scale * 0.999, corner.z * scale * 0.999, radius);
          }),
          pastCorners: tower.OFFICE_HEX_CORNERS.map((corner) => {
            const scale = (cornerReach + 0.02) / tower.OFFICE_HEX_RADIUS;
            return tower.officeInteriorContains(
              corner.x * scale, corner.z * scale, radius);
          }),
          formerBoxCornerOutside: tower.officeInteriorContains(
            tower.OFFICE_WIDTH / 2 - 2, tower.OFFICE_DEPTH / 2 - 2, radius),
          invalid: tower.officeInteriorContains(Number.NaN, 0, radius),
        }));
        """
    )

    assert result["center"] is True
    assert result["sideEdges"] == [True] * 6
    assert result["pastSideEdges"] == [False] * 6
    assert result["corners"] == [True] * 6
    assert result["pastCorners"] == [False] * 6
    assert result["formerBoxCornerOutside"] is False
    assert result["invalid"] is False


def test_back_elevator_cabin_and_approach_are_walkable_but_glass_sides_are_solid():
    result = run_tower_script(
        """
        const radius = tower.OFFICE_AVATAR_RADIUS;
        const centerX = tower.OFFICE_ELEVATOR_CENTER_X;
        const centerZ = tower.OFFICE_ELEVATOR_CENTER_Z;
        process.stdout.write(JSON.stringify({
          centerX,
          centerZ,
          frontZ: tower.OFFICE_FRONT_Z,
          rotation: tower.OFFICE_ELEVATOR_ROTATION_Y,
          cabinCenter: tower.officeElevatorCabinContains(
            centerX,
            centerZ,
            radius,
          ),
          cabinCenterWalkable: tower.officeInteriorPointIsWalkable(
            "lobby",
            centerX,
            centerZ,
            radius,
          ),
          interiorApproachWalkable: tower.officeInteriorPointIsWalkable(
            "lobby",
            centerX,
            centerZ + 5,
            radius,
          ),
          rearDoorOpen: !tower.officePointHitsObstacle(
            "lobby",
            centerX,
            centerZ + 4,
            radius,
          ),
          leftGlassBlocked: tower.officePointHitsObstacle(
            "lobby",
            centerX - 5,
            centerZ,
            radius,
          ),
          rightGlassBlocked: tower.officePointHitsObstacle(
            "lobby",
            centerX + 5,
            centerZ,
            radius,
          ),
          outwardGlassBlocked: tower.officePointHitsObstacle(
            "lobby",
            centerX,
            centerZ - 4,
            radius,
          ),
          pastCabin: tower.officeElevatorCabinContains(
            centerX,
            centerZ - 5,
            radius,
          ),
        }));
        """
    )

    assert result["centerX"] == 12
    assert result["centerZ"] == -result["frontZ"]
    assert abs(result["rotation"] - 3.141592653589793) < 1e-12
    assert result["cabinCenter"] is True
    assert result["cabinCenterWalkable"] is True
    assert result["interiorApproachWalkable"] is True
    assert result["rearDoorOpen"] is True
    assert result["leftGlassBlocked"] is True
    assert result["rightGlassBlocked"] is True
    assert result["outwardGlassBlocked"] is True
    assert result["pastCabin"] is False


def test_engineering_workstation_gaps_are_walkable():
    result = run_tower_script(
        """
        const radius = tower.OFFICE_AVATAR_RADIUS;
        const xs = tower.OFFICE_ENGINEERING_STATION_XS;
        const z = tower.OFFICE_ENGINEERING_STATION_Z;
        process.stdout.write(JSON.stringify({
          desk: tower.officePointHitsObstacle(
            "engineering", xs[0], z, radius),
          firstGap: tower.officeInteriorPointIsWalkable(
            "engineering", (xs[0] + xs[1]) / 2, z, radius),
          centerGap: tower.officeInteriorPointIsWalkable(
            "engineering", (xs[2] + xs[3]) / 2, z, radius),
          outerStationsInside: [xs[0], xs[xs.length - 1]].every((x) =>
            tower.officeHexContains(
              x + Math.sign(x) * tower.OFFICE_ENGINEERING_STATION_HALF_WIDTH,
              z,
              2,
            )),
          stations: xs,
        }));
        """
    )
    assert result == {
        "desk": True,
        "firstGap": True,
        "centerGap": True,
        "outerStationsInside": True,
        "stations": [-28, -14, 0, 14, 28],
    }


def test_every_floor_has_collidable_obstacles_and_reachable_open_space():
    result = run_tower_script(
        """
        const obstacleSamples = {
          lobby: [tower.OFFICE_LOBBY_FOUNTAIN.x, tower.OFFICE_LOBBY_FOUNTAIN.z],
          marketing: [0, 0],
          engineering: [0, tower.OFFICE_ENGINEERING_STATION_Z],
          rooftop: [tower.OFFICE_ROOFTOP_TABLES[1].x, tower.OFFICE_ROOFTOP_TABLES[1].z],
        };
        const floors = tower.OFFICE_FLOORS.map((floor) => {
          const blocked = obstacleSamples[floor.id];
          return {
            id: floor.id,
            obstacleHit: tower.officePointHitsObstacle(
              floor.id,
              blocked[0],
              blocked[1],
            ),
            obstacleNotWalkable: !tower.officeInteriorPointIsWalkable(
              floor.id,
              blocked[0],
              blocked[1],
            ),
            openSpaceWalkable: tower.officeInteriorPointIsWalkable(
              floor.id,
              0,
              28,
            ),
          };
        });
        process.stdout.write(JSON.stringify({
          floors,
          expandedMarketingObstacle: tower.officePointHitsObstacle(
            "marketing",
            tower.OFFICE_MEETING_TABLE.radius +
              tower.OFFICE_AVATAR_RADIUS -
              0.01,
            0,
          ),
          pastMarketingObstacle: tower.officePointHitsObstacle(
            "marketing",
            tower.OFFICE_MEETING_TABLE.radius +
              tower.OFFICE_AVATAR_RADIUS +
              0.01,
            0,
          ),
          // The round top is 11.8 across. The former collider was a 7.8x4
          // rectangle inscribed in it, so these four points on the wood
          // reported open floor.
          marketingTabletopEdgesBlocked: [
            0,
            Math.PI / 2,
            Math.PI,
            Math.PI * 1.5,
          ].every((angle) =>
            tower.officePointHitsObstacle(
              "marketing",
              Math.cos(angle) * 5.4,
              Math.sin(angle) * 5.4,
            )
          ),
          formerMarketingFenceOpen: tower.officeInteriorPointIsWalkable(
            "marketing",
            8,
            0,
          ),
          receptionDeskBlocked: tower.officePointHitsObstacle(
            "lobby",
            tower.OFFICE_RECEPTION_DESK.x,
            tower.OFFICE_RECEPTION_DESK.z,
          ),
          formerReceptionFenceOpen: tower.officeInteriorPointIsWalkable(
            "lobby",
            23,
            19.5,
          ),
          rooftopTableBlocked: tower.OFFICE_ROOFTOP_TABLES.every((table) =>
            tower.officePointHitsObstacle("rooftop", table.x, table.z)
          ),
          rooftopChairCentersWalkable: tower.OFFICE_ROOFTOP_TABLES.every((table) =>
            [0, Math.PI / 2, Math.PI, Math.PI * 1.5].every((angle) =>
              tower.officeInteriorPointIsWalkable(
                "rooftop",
                table.x + Math.cos(angle) * 5,
                table.z + Math.sin(angle) * 5,
              )
            )
          ),
          formerStrayTelescopeSpaceWalkable:
            tower.officeInteriorPointIsWalkable("rooftop", 22, -8),
        }));
        """
    )

    assert len(result["floors"]) == 4
    assert all(floor["obstacleHit"] for floor in result["floors"])
    assert all(floor["obstacleNotWalkable"] for floor in result["floors"])
    assert all(floor["openSpaceWalkable"] for floor in result["floors"])
    assert result["expandedMarketingObstacle"] is True
    assert result["pastMarketingObstacle"] is False
    assert result["marketingTabletopEdgesBlocked"] is True
    assert result["formerMarketingFenceOpen"] is True
    assert result["receptionDeskBlocked"] is True
    assert result["formerReceptionFenceOpen"] is True
    assert result["rooftopTableBlocked"] is True
    assert result["rooftopChairCentersWalkable"] is True
    assert result["formerStrayTelescopeSpaceWalkable"] is True


def test_round_tables_collide_on_the_circle_their_mesh_actually_occupies():
    """Every table in the tower is a cylinder, so every collider is a circle.

    The Marketing meeting top is 11.8 across and the lounge top is 6.4, and
    both used to let a walker stand in the middle of the wood: the meeting
    table was fenced by a 7.8x4 rectangle inscribed inside it, and the lounge
    table had no collider at all.
    """

    result = run_tower_script(
        """
        const interiorFloors = ["lobby", "marketing", "engineering"];
        const L = tower.OFFICE_LOUNGE;
        const ring = (radius, count) =>
          Array.from({ length: count }, (_, index) => {
            const angle = (index / count) * Math.PI * 2;
            return [Math.cos(angle) * radius, Math.sin(angle) * radius];
          });
        process.stdout.write(JSON.stringify({
          meetingTable: tower.OFFICE_MEETING_TABLE,
          meetingTopBlocked: ring(tower.OFFICE_MEETING_TABLE.radius - 0.2, 24)
            .every(([x, z]) => tower.officePointHitsObstacle("marketing", x, z)),
          pastMeetingTopOpen: ring(
            tower.OFFICE_MEETING_TABLE.radius +
              tower.OFFICE_AVATAR_RADIUS + 0.2,
            24,
          ).every(([x, z]) =>
            tower.officeInteriorPointIsWalkable("marketing", x, z)),
          meetingChairsWalkable: [
            [-7.4, 0], [-5.25, -5.25], [0, -7.4], [5.25, -5.25],
            [7.4, 0], [5.25, 5.25], [0, 7.4], [-5.25, 5.25],
          ].every(([x, z]) =>
            tower.officeInteriorPointIsWalkable("marketing", x, z)),
          loungeTopBlocked: interiorFloors.map((floorId) =>
            ring(3.0, 16).every(([x, z]) =>
              tower.officePointHitsObstacle(floorId, L.x + x, L.z + z))),
          pastLoungeTopOpen: interiorFloors.map((floorId) =>
            ring(3.2 + tower.OFFICE_AVATAR_RADIUS + 0.2, 16).every(([x, z]) =>
              tower.officeInteriorPointIsWalkable(floorId, L.x + x, L.z + z))),
          loungeSeatsWalkable: ["lobby", "marketing", "engineering"].map(
            (floorId) =>
              [-L.seatOffsetX, L.seatOffsetX].every((x) =>
                tower.officeInteriorPointIsWalkable(floorId, L.x + x, L.z))),
          rooftopHasNoLounge:
            tower.officeInteriorPointIsWalkable("rooftop", L.x, L.z - 2.5),
        }));
        """
    )

    assert result["meetingTable"]["type"] == "circle"
    assert result["meetingTable"]["radius"] == 5.9
    assert result["meetingTopBlocked"] is True
    assert result["pastMeetingTopOpen"] is True
    assert result["meetingChairsWalkable"] is True
    assert result["loungeTopBlocked"] == [True, True, True]
    assert result["pastLoungeTopOpen"] == [True, True, True]
    assert result["loungeSeatsWalkable"] == [True, True, True]
    assert result["rooftopHasNoLounge"] is True


def test_a_jumper_clears_a_tabletop_but_never_a_wall_or_the_lift_shell():
    """Jumping over a patio table is the point; passing through one is not.

    The rooftop tops are 2.85 above the slab and everyone standing on a floor
    is 0.38 above it, so only a jump can report feet above a table. Nothing
    without a declared top — the tower shell, the elevator glass — is ever
    passable, however high the jumper is.
    """

    result = run_tower_script(
        """
        const radius = tower.OFFICE_AVATAR_RADIUS;
        const standing = 0.38;
        const P = tower.OFFICE_ROOFTOP_TABLES[1];
        const L = tower.OFFICE_LOUNGE;
        const R = tower.OFFICE_RECEPTION_DESK;
        process.stdout.write(JSON.stringify({
          standingHitsPatioTable:
            tower.officePointHitsObstacle("rooftop", P.x, P.z, radius, standing),
          shinHeightHitsPatioTable:
            tower.officePointHitsObstacle("rooftop", P.x, P.z, radius, 1.4),
          justUnderTheTopHits:
            tower.officePointHitsObstacle("rooftop", P.x, P.z, radius, 2.84),
          clearingTheTopPasses:
            tower.officePointHitsObstacle("rooftop", P.x, P.z, radius, 2.86),
          standingHitsMeetingTable:
            tower.officePointHitsObstacle("marketing", 0, 0, radius, standing),
          clearingMeetingTablePasses:
            tower.officePointHitsObstacle("marketing", 0, 0, radius, 2.4),
          standingHitsLoungeTable:
            tower.officePointHitsObstacle("lobby", L.x, L.z, radius, standing),
          clearingLoungeTablePasses:
            tower.officePointHitsObstacle("lobby", L.x, L.z, radius, 1.3),
          liftShellStaysSolidMidJump: tower.officePointHitsObstacle(
            "rooftop",
            tower.OFFICE_ELEVATOR_CENTER_X,
            tower.OFFICE_ELEVATOR_CENTER_Z - 4,
            radius,
            40,
          ),
          receptionDeskStaysSolidMidJump:
            tower.officePointHitsObstacle("lobby", R.x, R.z, radius, 40),
          defaultHeightIsTheFloor:
            tower.officePointHitsObstacle("rooftop", P.x, P.z) &&
            tower.officePointHitsObstacle("marketing", 0, 0),
        }));
        """
    )

    assert result["standingHitsPatioTable"] is True
    assert result["shinHeightHitsPatioTable"] is True
    assert result["justUnderTheTopHits"] is True
    assert result["clearingTheTopPasses"] is False
    assert result["standingHitsMeetingTable"] is True
    assert result["clearingMeetingTablePasses"] is False
    assert result["standingHitsLoungeTable"] is True
    assert result["clearingLoungeTablePasses"] is False
    assert result["liftShellStaysSolidMidJump"] is True
    assert result["receptionDeskStaysSolidMidJump"] is True
    assert result["defaultHeightIsTheFloor"] is True


def test_a_jump_that_lands_in_a_table_is_put_back_on_open_floor():
    """A landing inside a table has no earlier position to slide back to.

    Both the attempted and the previous point are inside the same footprint, so
    the ordinary axis slide pinned the visitor in the wood permanently. Leaving
    by the nearest edge is the only answer that terminates.
    """

    result = run_tower_script(
        """
        const radius = tower.OFFICE_AVATAR_RADIUS;
        const [west, middle, east] = tower.OFFICE_ROOFTOP_TABLES;
        const L = tower.OFFICE_LOUNGE;
        const escapes = [
          ["rooftop", middle.x + 0.4, middle.z + 0.2],
          ["rooftop", west.x, west.z],
          ["rooftop", east.x + 1.5, east.z + 1.4],
          ["marketing", 0, 0],
          ["marketing", 4.8, -1.2],
          ["lobby", L.x, L.z],
          ["engineering", L.x + 1.1, L.z + 1.4],
        ].map(([floorId, x, z]) => {
          const clear = tower.officePointOutsideObstacles(floorId, x, z);
          return {
            floorId,
            stillHit: tower.officePointHitsObstacle(
              floorId, clear.x, clear.z, radius),
            walkable: tower.officeInteriorPointIsWalkable(
              floorId, clear.x, clear.z, radius),
            travelled: Math.hypot(clear.x - x, clear.z - z),
          };
        });
        process.stdout.write(JSON.stringify({
          escapes,
          openFloorIsLeftAlone: (() => {
            const clear = tower.officePointOutsideObstacles("rooftop", 60, -20);
            return clear.x === 60 && clear.z === -20;
          })(),
          liftShellCornerEscapes: (() => {
            const clear = tower.officePointOutsideObstacles(
              "rooftop",
              tower.OFFICE_ELEVATOR_CENTER_X + 5,
              tower.OFFICE_ELEVATOR_CENTER_Z + 4,
            );
            return !tower.officePointHitsObstacle(
              "rooftop", clear.x, clear.z, radius);
          })(),
          aClearedTableIsNotEscaped: (() => {
            const clear = tower.officePointOutsideObstacles(
              "rooftop", middle.x, middle.z, radius, 3.4);
            return clear.x === middle.x && clear.z === middle.z;
          })(),
        }));
        """
    )

    assert [escape["stillHit"] for escape in result["escapes"]] == [False] * 7
    assert [escape["walkable"] for escape in result["escapes"]] == [True] * 7
    # The furthest any of these is from open floor is the middle of the widest
    # table, so nothing should be flung across the room to get out.
    assert all(escape["travelled"] <= 6.4 for escape in result["escapes"])
    assert result["openFloorIsLeftAlone"] is True
    assert result["liftShellCornerEscapes"] is True
    assert result["aClearedTableIsNotEscaped"] is True


def test_standing_meeting_poses_land_around_the_table_and_never_in_it():
    result = run_tower_script(
        """
        const ring = tower.OFFICE_MEETING_STANDING_RADIUS;
        const poses = [
          [0, 0],
          [0, 4.1],
          [0, -3],
          [2.2, 2.2],
          [-5, 1],
        ].map(([x, z]) => tower.officeMeetingStandingPoint(x, z));
        process.stdout.write(JSON.stringify({
          ring,
          poses,
          allOutsideTheTop: poses.every((pose) =>
            Math.hypot(pose.x, pose.z) >=
            tower.OFFICE_MEETING_TABLE.radius),
          allWalkable: poses.every((pose) =>
            tower.officeInteriorPointIsWalkable("marketing", pose.x, pose.z)),
          defaultJoinPose: tower.officeMeetingStandingPoint(0, 4.1),
          openFloorKept: tower.officeMeetingStandingPoint(-24, 18),
          farSideKept: tower.officeMeetingStandingPoint(0, -33),
          wildFrameClamped: tower.officeMeetingStandingPoint(1e9, -1e9),
          missingFrameIsNotTheTableCentre:
            tower.officeMeetingStandingPoint(undefined, undefined),
        }));
        """
    )

    assert result["ring"] == 6.71
    assert result["allOutsideTheTop"] is True
    assert result["allWalkable"] is True
    # The room's own default join pose used to be (0, 4.1) — inside the top.
    assert result["defaultJoinPose"] == {"x": 0, "z": 6.71}
    assert result["openFloorKept"] == {"x": -24, "z": 18}
    assert result["farSideKept"] == {"x": 0, "z": -33}
    assert abs(
        max(
            abs(result["wildFrameClamped"]["z"]),
            3 ** 0.5 / 2 * abs(result["wildFrameClamped"]["x"])
            + 0.5 * abs(result["wildFrameClamped"]["z"]),
        )
        - (37 - 0.46 - 0.65)
    ) < 1e-3
    assert result["missingFrameIsNotTheTableCentre"] == {"x": 0, "z": 6.71}


def test_campus_walkers_cannot_step_through_the_protruding_cabin_glass():
    """The cabin straddles the facade, so half its glass shell stands outdoors.

    A campus walker who reaches the cabin has an inert floor panel: office
    entry never ran out there, so no floor handler and no server-derived
    authorization colours were ever installed. The outdoor half must be sealed
    by the same three panes the interior obstacle set already honours.
    """

    result = run_tower_script(
        """
        const radius = tower.OFFICE_AVATAR_RADIUS;
        const centerX = tower.OFFICE_ELEVATOR_CENTER_X;
        const centerZ = tower.OFFICE_ELEVATOR_CENTER_Z;
        const facadeLine = -(tower.OFFICE_DEPTH / 2 + radius);
        const step = 0.25;

        // Flood the campus in front of the tower, treating the elevator shell
        // as the only wall, and see whether the cabin can be reached.
        const key = (x, z) => `${x.toFixed(2)},${z.toFixed(2)}`;
        const start = [centerX, facadeLine - 24];
        const seen = new Set([key(start[0], start[1])]);
        const queue = [start];
        let reachedCabin = false;
        while (queue.length) {
          const [x, z] = queue.shift();
          if (tower.officeElevatorCabinContains(x, z, radius)) {
            reachedCabin = true;
            break;
          }
          for (const [dx, dz] of [[step, 0], [-step, 0], [0, step], [0, -step]]) {
            const nx = x + dx;
            const nz = z + dz;
            // The campus half-plane the footprint clamp leaves reachable.
            if (nz > facadeLine || nz < facadeLine - 26) continue;
            if (Math.abs(nx - centerX) > 26) continue;
            const cell = key(nx, nz);
            if (seen.has(cell)) continue;
            seen.add(cell);
            if (tower.officeElevatorShellContains(nx, nz, radius)) continue;
            queue.push([nx, nz]);
          }
        }

        process.stdout.write(JSON.stringify({
          panes: tower.OFFICE_ELEVATOR_SHELL.length,
          reachedCabin,
          // The clamp line really does run through the cabin: this is why an
          // unsealed shell put a campus walker inside a dead car.
          facadeLineIsInsideTheCabin: tower.officeElevatorCabinContains(
            centerX,
            facadeLine,
            radius,
          ),
          outwardGlassBlocks: tower.officeElevatorShellContains(
            centerX,
            centerZ - 4,
            radius,
          ),
          leftGlassBlocks: tower.officeElevatorShellContains(
            centerX - 5,
            centerZ - 2,
            radius,
          ),
          rightGlassBlocks: tower.officeElevatorShellContains(
            centerX + 5,
            centerZ - 2,
            radius,
          ),
          // The rear face is the one real entrance and stays open.
          rearFaceOpen: !tower.officeElevatorShellContains(
            centerX,
            centerZ + 4,
            radius,
          ),
          // The staffed doorway and the open campus are untouched.
          mainDoorwayOpen: !tower.officeElevatorShellContains(
            0,
            centerZ,
            radius,
          ),
          openCampusOpen: !tower.officeElevatorShellContains(
            centerX,
            centerZ - 12,
            radius,
          ),
          // The shell is exactly the interior obstacle set it mirrors.
          matchesInteriorObstacles: [
            [centerX, centerZ - 4],
            [centerX - 5, centerZ - 2],
            [centerX + 5, centerZ - 2],
            [centerX, centerZ + 4],
            [centerX, centerZ],
          ].every(([x, z]) =>
            tower.officeElevatorShellContains(x, z, radius) ===
            tower.officePointHitsObstacle("lobby", x, z, radius)
          ),
        }));
        """
    )

    assert result["panes"] == 3
    assert result["facadeLineIsInsideTheCabin"] is True
    assert result["reachedCabin"] is False
    assert result["outwardGlassBlocks"] is True
    assert result["leftGlassBlocks"] is True
    assert result["rightGlassBlocks"] is True
    assert result["rearFaceOpen"] is True
    assert result["mainDoorwayOpen"] is True
    assert result["openCampusOpen"] is True
    assert result["matchesInteriorObstacles"] is True


def test_the_tower_shell_turns_back_flight_that_crosses_a_restricted_floor():
    result = run_tower_script(
        """
        const craft = 3.2;
        const avatar = tower.OFFICE_AVATAR_RADIUS;
        const facadeZ = tower.OFFICE_DEPTH / 2;
        const flankX = tower.OFFICE_WIDTH / 2;
        const roofY = tower.OFFICE_TOWER_HEIGHT;
        // Engineering and Infrastructure are the storeys a quadcopter used to
        // reach by flying up the glass and turning in through the wall.
        const engineeringY = tower.officeFloorY("engineering") + 6;
        const marketingY = tower.officeFloorY("marketing") + 6;
        const report = (previous, current, radius) => {
          const exit = tower.officeTowerShellExit(previous, current, radius);
          if (!exit) return null;
          return {
            ...exit,
            insideAFloor: tower.officeInteriorContains(exit.x, exit.z, avatar),
          };
        };
        process.stdout.write(JSON.stringify({
          engineeringY,
          marketingY,
          roofY,
          facadeZ,
          flankX,
          climbingTheFacade: report(
            { x: 0, y: 4, z: facadeZ + 12 },
            { x: 0, y: engineeringY, z: facadeZ + 12 },
            craft,
          ),
          throughTheFrontGlass: report(
            { x: 0, y: engineeringY, z: facadeZ + 12 },
            { x: 0, y: engineeringY, z: 0 },
            craft,
          ),
          flankNormal: [
            tower.officeHexSide("front-left").normalX,
            tower.officeHexSide("front-left").normalZ,
          ],
          throughTheFlankGlass: report(
            {
              x: tower.officeHexSide("front-left").normalX * (facadeZ + 12),
              y: marketingY,
              z: tower.officeHexSide("front-left").normalZ * (facadeZ + 12),
            },
            {
              x: tower.officeHexSide("front-left").normalX * 20,
              y: marketingY,
              z: tower.officeHexSide("front-left").normalZ * 20,
            },
            craft,
          ),
          strandedExtent: (() => {
            const exit = tower.officeTowerShellExit(
              { x: 0, y: engineeringY, z: 0 },
              { x: 0, y: engineeringY, z: 0 },
              craft,
            );
            return tower.officeHexExtent(exit.x, exit.z);
          })(),
          throughTheBackWall: report(
            { x: 0, y: engineeringY, z: -flankX },
            { x: 0, y: engineeringY, z: -10 },
            craft,
          ),
          divingThroughTheRoof: report(
            { x: 0, y: roofY + 40, z: 0 },
            { x: 0, y: marketingY, z: 0 },
            craft,
          ),
          cruisingOverTheRoof: report(
            { x: 0, y: roofY + 40, z: facadeZ + 12 },
            { x: 0, y: roofY + 40, z: 0 },
            craft,
          ),
          jetpackThroughTheGlass: report(
            { x: 0, y: engineeringY, z: facadeZ + 4 },
            { x: 0, y: engineeringY, z: 20 },
            avatar,
          ),
          strandedInside: report(
            { x: 0, y: engineeringY, z: 0 },
            { x: 0, y: engineeringY, z: 0 },
            craft,
          ),
          heldAtTheGlass: report(
            { x: 0, y: engineeringY, z: facadeZ + craft },
            { x: 0, y: engineeringY, z: facadeZ },
            craft,
          ),
          acrossTheCampus: report(
            { x: 0, y: 4, z: 200 },
            { x: 0, y: 4, z: 180 },
            craft,
          ),
          clearOfTheRoof: tower.officeTowerShellContains(0, roofY + 4, 0, craft),
          insideTheShell: tower.officeTowerShellContains(
            0,
            engineeringY,
            0,
            craft,
          ),
          invalid: tower.officeTowerShellContains(Number.NaN, 4, 0, craft),
        }));
        """
    )

    craft = 3.2
    # Nothing about flying past the facade is blocked: the climb itself is fine.
    assert result["climbingTheFacade"] is None
    # Turning in at a restricted storey is where the shell answers, and it
    # answers on the face the craft crossed rather than shoving it out the far
    # side of the building.
    assert result["throughTheFrontGlass"] == {
        "face": "front",
        "x": 0,
        "y": result["engineeringY"],
        "z": result["facadeZ"] + craft,
        "insideAFloor": False,
    }
    flank = result["throughTheFlankGlass"]
    assert flank["face"] == "front-left"
    assert flank["y"] == result["marketingY"]
    assert abs(flank["x"] - result["flankNormal"][0] * (result["facadeZ"] + craft)) < 1e-9
    assert abs(flank["z"] - result["flankNormal"][1] * (result["facadeZ"] + craft)) < 1e-9
    assert flank["insideAFloor"] is False
    assert result["throughTheBackWall"]["face"] == "back"
    assert result["throughTheBackWall"]["z"] == -(result["facadeZ"] + craft)
    assert result["throughTheBackWall"]["insideAFloor"] is False
    # A dive from above stops on the roof line instead of dropping through the
    # ceiling onto Infrastructure.
    assert result["divingThroughTheRoof"] == {
        "face": "roof",
        "x": 0,
        "y": result["roofY"] + craft,
        "z": 0,
        "insideAFloor": True,
    }
    # Flying over the tower stays free; only entering it is refused.
    assert result["cruisingOverTheRoof"] is None
    assert result["clearOfTheRoof"] is False
    assert result["insideTheShell"] is True
    # The wearable jetpack meets the same wall at its own, smaller clearance.
    assert result["jetpackThroughTheGlass"]["face"] == "front"
    assert result["jetpackThroughTheGlass"]["z"] == result["facadeZ"] + 0.46
    assert result["jetpackThroughTheGlass"]["insideAFloor"] is False
    # No approach history at all still resolves out to the nearest facade
    # rather than lifting the craft through every storey above it.
    assert result["strandedInside"] is not None
    assert result["strandedInside"]["insideAFloor"] is False
    assert abs(result["strandedExtent"] - (result["facadeZ"] + craft)) < 1e-9
    # Leaning on the glass holds the same face frame after frame.
    assert result["heldAtTheGlass"]["face"] == "front"
    assert result["heldAtTheGlass"]["z"] == result["facadeZ"] + craft
    # The rest of the campus is not a no-fly zone.
    assert result["acrossTheCampus"] is None
    assert result["invalid"] is False


def test_every_floor_is_reachable_from_the_elevator_inside_the_hexagon():
    result = run_tower_script(
        f"""
        const movement = await import({json.dumps(MOVEMENT_MODULE.as_uri())});
        const step = 0.5;
        const reach = tower.OFFICE_HEX_RADIUS;
        const minX = -Math.ceil(reach);
        const minZ = -tower.OFFICE_HEX_APOTHEM - 6;
        const nx = Math.round((2 * Math.ceil(reach)) / step) + 1;
        const nz = Math.round((2 * tower.OFFICE_HEX_APOTHEM + 6) / step) + 1;
        const floors = {{}};
        for (const floor of tower.OFFICE_FLOORS) {{
          const walk = new Uint8Array(nx * nz);
          let walkable = 0;
          let outsideShell = 0;
          for (let i = 0; i < nx; i += 1) {{
            for (let j = 0; j < nz; j += 1) {{
              const x = minX + i * step;
              const z = minZ + j * step;
              if (!movement.officeScenePointIsWalkable(
                floor.id, x, z, tower.OFFICE_AVATAR_RADIUS)) continue;
              walk[i * nz + j] = 1;
              walkable += 1;
              if (
                !tower.officeHexContains(x, z) &&
                !tower.officeElevatorCabinContains(x, z)
              ) outsideShell += 1;
            }}
          }}
          const cell = (x, z) =>
            Math.round((x - minX) / step) * nz + Math.round((z - minZ) / step);
          const seen = new Uint8Array(nx * nz);
          const start = cell(
            tower.OFFICE_ELEVATOR_CENTER_X, tower.OFFICE_ELEVATOR_CENTER_Z);
          const queue = [start];
          seen[start] = 1;
          let reached = 0;
          const bounds = {{ minX: 0, maxX: 0, minZ: 0, maxZ: 0 }};
          while (queue.length) {{
            const index = queue.pop();
            if (!walk[index]) continue;
            reached += 1;
            const i = Math.floor(index / nz);
            const j = index % nz;
            bounds.minX = Math.min(bounds.minX, minX + i * step);
            bounds.maxX = Math.max(bounds.maxX, minX + i * step);
            bounds.minZ = Math.min(bounds.minZ, minZ + j * step);
            bounds.maxZ = Math.max(bounds.maxZ, minZ + j * step);
            for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {{
              const ni = i + di;
              const nj = j + dj;
              if (ni < 0 || nj < 0 || ni >= nx || nj >= nz) continue;
              const next = ni * nz + nj;
              if (seen[next] || !walk[next]) continue;
              seen[next] = 1;
              queue.push(next);
            }}
          }}
          floors[floor.id] = {{
            walkable,
            reached,
            outsideShell,
            bounds,
            doorReached: Boolean(seen[cell(0, tower.OFFICE_FRONT_Z - 1.5)]),
            everyCornerReached: tower.OFFICE_HEX_CORNERS.every((corner) => {{
              const scale = (tower.OFFICE_HEX_RADIUS - 3) / tower.OFFICE_HEX_RADIUS;
              return Boolean(seen[cell(corner.x * scale, corner.z * scale)]);
            }}),
          }};
        }}
        process.stdout.write(JSON.stringify({{
          floors,
          radius: tower.OFFICE_HEX_RADIUS,
          apothem: tower.OFFICE_HEX_APOTHEM,
        }}));
        """
    )

    assert set(result["floors"]) == {"lobby", "marketing", "engineering", "rooftop"}
    limit = result["apothem"] - 0.46 - 0.65
    for floor_id, floor in result["floors"].items():
        assert floor["walkable"] > 14_000, floor_id
        assert floor["reached"] == floor["walkable"], floor_id
        assert floor["outsideShell"] == 0, floor_id
        assert floor["doorReached"] is True, floor_id
        assert floor["everyCornerReached"] is True, floor_id
        assert floor["bounds"]["maxZ"] >= limit - 0.5, floor_id
        assert floor["bounds"]["minZ"] <= -result["apothem"] - 2, floor_id
        assert floor["bounds"]["maxX"] >= limit * 2 / 3 ** 0.5 - 0.6, floor_id
        assert floor["bounds"]["minX"] <= -(limit * 2 / 3 ** 0.5 - 0.6), floor_id
