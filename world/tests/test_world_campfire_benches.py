#!/usr/bin/env python3
"""Scene contracts for the campfire benches and sitting on them."""

import math
from pathlib import Path
import re
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
DATA = (WORLD / "world-data.js").read_text(encoding="utf-8")
WORLD_PY = (ROOT.parent / "world" / "src" / "world.py").read_text(
    encoding="utf-8")


def test_open_member_rings_keep_people_clear_of_the_fire():
    first = re.search(r"const MEMBER_CIRCLE_FIRST_RADIUS = ([\d.]+);", SCENE)
    assert first and float(first.group(1)) >= 4.4
    assert "let nextRadius = MEMBER_CIRCLE_FIRST_RADIUS;" in SCENE
    assert "const MEMBER_CLEARING_RADIUS = 12;" in SCENE


def test_directory_figures_are_positioned_relative_to_the_live_campfire():
    lounge = SCENE.split("function updateMemberLounge", 1)[1].split(
        "function visitNeighborhoodHome", 1
    )[0]
    assert "const slot = layout.memberSlots[index]?.position || seat;" in lounge
    assert "campfire.position.x + slot.x" in lounge
    assert "campfire.position.z + slot.z" in lounge
    # Sitters are placed off the plank the same positioner built, so a bench
    # and the figure on it can never drift apart when the campfire moves.
    assert "campfire.position.x + seat.x" in lounge
    assert "campfire.position.z + seat.z" in lounge


def test_sitting_holds_a_seated_pose_facing_the_fire():
    sit = SCENE.split("function sitOnCampfireBench", 1)[1].split(
        "function walkPlayer", 1
    )[0]
    assert "campfire.position.x" in sit and "campfire.position.z" in sit
    assert "activity: CAMPFIRE_SEATED_ACTIVITY" in sit
    # adhoc #303: the pitch is positive so the legs swing forward over the
    # front edge of the bench instead of out behind the sitter.
    assert "applySeatedLegPose(player);" in sit
    assert "const SEATED_LEG_PITCH = 1.45;" in SCENE
    # adhoc #323: the knee folds back by the same amount, so the shin drops
    # straight down off the front edge instead of the whole leg pivoting.
    assert "const SEATED_KNEE_PITCH = -SEATED_LEG_PITCH;" in SCENE
    # Sitters land by their hips, not by their feet: the pose used to leave the
    # avatar standing on the plank with its shoes still flat on it.
    assert "seatedAvatarY(" in sit
    # The pose is held every frame instead of applied once as a teleport.
    assert "applyBenchSeatPose();" in SCENE
    assert "function standUpFromBench()" in SCENE


def test_double_click_on_a_bench_sits_instead_of_dashing():
    dbl = SCENE.split("function handleDoubleClick", 1)[1].split(
        "function handlePointerCancel", 1
    )[0]
    assert "campfireBench" in dbl
    assert dbl.index("sitOnCampfireBench") < dbl.index("dashTarget = point")


def test_open_member_circle_seats_full_detail_avatars_on_real_benches():
    # The benches are back, but not as the old roster-mesh stand-ins: the
    # figures on them are the same full-detail avatar the player walks in.
    assert "function rebuildCampfireCircle" not in SCENE
    assert "function rebuildMemberCircleBenches(layout) {" in SCENE
    assert 'seatInstances.name = "campfire-member-bench-seats";' in SCENE
    assert 'legInstances.name = "campfire-member-bench-legs";' in SCENE
    assert 'highDetail.name = "avatar-full-detail"' in SCENE
    assert "cloneAvatarFarModel" not in SCENE
    assert "const slot = layout.memberSlots[index]?.position || seat;" in SCENE


def test_hundreds_of_benches_cost_a_handful_of_draw_calls():
    # A lifetime roster is hundreds of seats; one mesh each would put thousands
    # of submissions in the frame. The shared wood is two instanced draws and
    # every name plate shares a single atlas-backed mesh.
    build = SCENE.split("function rebuildMemberCircleBenches", 1)[1].split(
        "function setCampfireSeatLabel", 1
    )[0]
    assert "new THREE.InstancedMesh(\n      seatGeometry,\n" in build
    assert "legInstances = new THREE.InstancedMesh(" in build
    assert "seatInstances.instanceMatrix.needsUpdate = true;" in build
    assert "legInstances.instanceMatrix.needsUpdate = true;" in build
    assert 'labelMesh.name = "campfire-member-bench-label-atlas";' in build
    # Per-seat picking survives as invisible proxies, which the renderer skips.
    assert "seat.visible = false;" in build
    assert "seat.userData.campfireBench = true;" in build
    assert "seat.userData.raycastProxy = true;" in build
    # A rebuild hands the old ring's geometry and its hit targets back, through
    # the same helper every other regenerated layer is torn down with.
    assert (
        "removeGeneratedLayer(campfire, campfire.userData.seatRing, interactive);"
        in build
    )


def test_a_bench_is_branded_with_the_name_of_the_member_it_belongs_to():
    plate = SCENE.split("function drawCampfireSeatPlate(", 1)[1].split(
        "\n}\n", 1
    )[0]
    # The name is burned into the plank itself, so an empty bench still says
    # whose seat it is.
    assert "embossPlankText(" in plate
    assert 'label || "OPEN SEAT"' in plate
    assert "engagement?.label?.toUpperCase()" in plate
    # Sitting, out walking the world, or open to whoever arrives next.
    assert '"OUT AND ABOUT"' in plate
    assert '"AT THE FIRE"' in plate
    assert '"GUESTS WELCOME"' in plate
    # Repainting a plank that already reads this way costs no canvas work.
    setter = SCENE.split("function setCampfireSeatLabel", 1)[1].split(
        "\n  }", 1
    )[0]
    assert "const key = `${name} ${state} ${category}`;" in setter
    assert "if (bench.labelKey === key) return;" in setter


def test_a_member_out_in_the_world_keeps_their_named_bench_empty():
    lounge = SCENE.split("function updateMemberLounge", 1)[1]
    # An away member is no longer filtered out of the roster: they keep their
    # own seat index, so nobody else's name lands on their plank.
    assert "member?.away === true &&" in lounge
    assert 'away ? "away" : "seated",' in lounge
    assert "member.engagement.id," in lounge
    assert "if (away) return;" in lounge
    # Except when the away member is this browser, sitting on that very bench.
    assert "name.toLowerCase() !== localBenchName.toLowerCase()" in lounge


def test_arriving_guests_get_their_own_bench_not_a_strangers_plank():
    lounge = SCENE.split("function updateMemberLounge", 1)[1]
    # One spare bench per guest present without a users-table row, and never
    # fewer than one, so a visitor always finds a seat of their own.
    assert "const guestBenches = Math.max(" in lounge
    assert "Math.min(64, Math.round(Number(guests) || 0))" in lounge
    assert "const layout = buildMemberCircleLayout(rosterMembers, guestBenches);" in lounge
    assert "rebuildMemberCircleBenches(layout)" in lounge
    # Benches past the roster read as open seats.
    assert 'setCampfireSeatLabel(index, "", "open");' in lounge
    # Arriving by the world map lands a member on their own bench and a guest
    # on the first open one.
    seat_for = SCENE.split("function memberCircleSeatFor(name)", 1)[1].split(
        "\n  }", 1
    )[0]
    assert "campfire.userData.seatByName?.get(wanted)" in seat_for
    assert "campfire.userData.openSeatIndex" in seat_for
    # The circle is never bare, and one runaway roster cannot ask for an
    # unbounded name atlas.
    assert "const CAMPFIRE_MIN_BENCHES = 6;" in SCENE
    assert "const CAMPFIRE_MAX_BENCHES = 500;" in SCENE


def test_a_roster_past_the_bench_ceiling_stands_on_rings_not_in_the_fire():
    # The bench ceiling caps the woodwork, not the lifecycle layout. A member
    # past the last plank still receives a real member slot rather than being
    # planted in the middle of the flames with every other overflow account.
    build = SCENE.split("function rebuildMemberCircleBenches", 1)[1].split(
        "function setCampfireSeatLabel", 1
    )[0]
    assert "const slots = Array.isArray(layout?.slots) ? layout.slots : [];" in build
    assert "const count = Math.min(CAMPFIRE_MAX_BENCHES, wanted);" in build
    assert "const layoutSlot = slots[index];" in build
    assert "const slot = layoutSlot?.position || new THREE.Vector3();" in build
    # A rebuild that keeps the same bench count but changes lifecycle bands
    # still has to re-cut the rings and recolour their benches.
    assert "campfire.userData.seatLayoutKey === layout?.key" in build
    assert "campfire.userData.seatLayoutTotal = wanted;" in build
    lounge = SCENE.split("function updateMemberLounge", 1)[1]
    assert "const slot = layout.memberSlots[index]?.position || seat;" in lounge
    # The dirt reaches the outermost occupant, standing or seated.
    assert "...layout.memberSlots.map((slot) => slot.position.length())," in lounge


def test_circle_holds_the_whole_directory_plus_room_for_guests():
    lounge = SCENE.split("function updateMemberLounge", 1)[1].split(
        "function visitNeighborhoodHome", 1
    )[0]
    # Every member the public directory lists owns a bench in the circle: there
    # is no ring-load cap that would leave the rest of the roster unseated.
    assert "MEMBER_CIRCLE_VISIBLE_LIMIT" not in SCENE
    assert ".slice(0, MEMBER_CIRCLE" not in lounge
    # A roster larger than the pavement widens the dirt instead of seating
    # people in the grass, and the dirt is measured off the benches actually
    # built rather than a guess at where the last one landed.
    assert "MEMBER_CLEARING_RADIUS + 0.8," in lounge
    assert "occupiedRadius + 0.9," in lounge
    assert "seatOffsets.map((offset) => Math.hypot(offset.x, offset.z))" in lounge
    assert "rebuildCampfireCircle" not in SCENE


def test_every_ring_inside_the_clearing_is_sized_by_standing_room():
    # Dynamic rings stand one figure's depth apart and calculate shoulder room
    # from their circumference. Most state boundaries add a narrow visual seam;
    # the two innermost rows get wider breaks before the other bands.
    assert "const MEMBER_CIRCLE_FIRST_RADIUS = 4.4;" in SCENE
    assert "const MEMBER_CIRCLE_RING_DEPTH = 1.5;" in SCENE
    assert "const MEMBER_CIRCLE_STATE_GAP = 0.35;" in SCENE
    assert "const MEMBER_CIRCLE_INNER_STATE_GAP = 0.85;" in SCENE
    capacity = SCENE.split("function memberCircleRingCapacity", 1)[1].split(
        "function memberCircleRingPosition", 1
    )[0]
    assert "(Math.PI * 2 * radius) / MEMBER_CIRCLE_STANDING_ROOM" in capacity
    layout = SCENE.split("function buildMemberCircleLayout", 1)[1].split(
        "campfireGround.scale.setScalar", 1
    )[0]
    assert "nextRadius += MEMBER_CIRCLE_RING_DEPTH;" in layout
    assert 'state.id === "world-step" || state.id === "power"' in layout
    assert "? MEMBER_CIRCLE_INNER_STATE_GAP" in layout
    assert ": MEMBER_CIRCLE_STATE_GAP" in layout


def test_a_roster_larger_than_the_pavement_grows_outward_rings():
    positioner = SCENE.split("function buildMemberCircleLayout", 1)[1].split(
        "campfireGround.scale.setScalar", 1
    )[0]
    # Nobody is dropped for want of a ring: the positioner keeps adding rings
    # until every member has a slot.
    assert "while (offset < categoryMembers.length) {" in positioner
    assert "while (openRemaining > 0) {" in positioner
    assert "nextRadius += MEMBER_CIRCLE_RING_DEPTH;" in positioner
    assert "const capacity = memberCircleRingCapacity(nextRadius);" in positioner
    # The first overflow ring falls in behind the last paved one at the same
    # depth the paved rings keep between themselves.
    assert "const MEMBER_CIRCLE_RING_DEPTH = 1.5;" in SCENE


def test_open_circle_keeps_a_town_side_walk_in_gap():
    positioner = SCENE.split("function memberCircleRingPosition", 1)[1].split(
        "function buildMemberCircleLayout", 1
    )[0]
    assert "const entranceGap = Math.min(Math.PI / 2, 3.6 / radius);" in positioner
    assert "const usableArc = Math.PI * 2 - entranceGap;" in positioner
    assert "-Math.PI / 2 + entranceGap / 2" in positioner


def test_live_idle_members_wait_at_the_open_circle_approach_not_roster_benches():
    remote = SCENE.split("function setRemotePlayers", 1)[1].split(
        "function updateMemberLounge", 1
    )[0]
    assert "Live idle peers keep their presence avatar" in remote
    assert "MEMBER_CLEARING_RADIUS" in remote
    assert "outside the open" in remote
    assert "campfire.userData.seatOffsets" not in remote
    assert "campfire.userData.seatByName" not in remote


def test_faces_wear_status_emoji_over_a_unique_generated_default():
    # A chosen status emoji still takes over the face; otherwise each identity
    # gets a stable generated portrait instead of every account sharing 🙂.
    assert "function avatarFaceTexture" in SCENE
    assert "function proceduralAvatarFaceTexture" in SCENE
    assert "function syncAvatarFace" in SCENE
    assert "const drawn = statusEmoji" in SCENE
    assert "? avatarFaceTexture(THREE, statusEmoji)" in SCENE
    assert "proceduralAvatarFaceTexture(THREE, identityKey)" in SCENE
    # Status changes re-render the face card.
    assert "syncAvatarFace(THREE, avatar);" in SCENE
    assert "faceMesh.rotation.y = Math.PI;" in SCENE


def test_emoji_fills_the_face_with_no_gap_of_bare_head():
    # adhoc #316: the decal used to stop short of the glyph, leaving corners
    # and a rim of head-coloured sphere showing as a seam around the face.
    # The canvas is flooded underneath the glyph with its own rim colour, so
    # the disc carries that colour all the way out to its rim.
    assert 'context.globalCompositeOperation = "destination-over";' in SCENE
    assert "context.fillRect(0, 0, 128, 128);" in SCENE
    assert 'context.globalCompositeOperation = "source-over";' in SCENE


def test_the_face_is_a_flat_disc_on_a_flat_cut_head():
    # The face used to be a curved shell wrapped round the head, which warped
    # an uploaded avatar photo. The head's front is now sliced off flat and the
    # face is a plain circle lying on that cut.
    assert "function flattenSphereFront" in SCENE
    assert "if (position.getZ(i) < -depth) position.setZ(i, -depth);" in SCENE
    assert "geometry.computeVertexNormals();" in SCENE
    assert (
        "export function avatarHeadGeometry(THREE, widthSegments = 32, heightSegments = 24) {\n"
        "  return flattenSphereFront(\n"
        "    new THREE.SphereGeometry(AVATAR_HEAD_RADIUS, widthSegments, heightSegments),\n"
        "    AVATAR_FACE_DEPTH,\n"
        "  );" in SCENE
    )
    assert '"avatar-head",\n    () => avatarHeadGeometry(THREE),' in SCENE
    assert "new THREE.CircleGeometry(AVATAR_FACE_RADIUS, 48)" in SCENE
    # No wrapped shell left behind.
    assert "AVATAR_FACE_PHI_START" not in SCENE
    assert "AVATAR_FACE_THETA_START" not in SCENE

    def number(name):
        found = re.search(rf"const {name} = ([\d.]+);", SCENE)
        assert found, f"{name} missing"
        return float(found.group(1))

    radius = number("AVATAR_HEAD_RADIUS")
    depth = number("AVATAR_FACE_DEPTH")
    # The shallow cut makes the photo disc almost the full sphere diameter,
    # leaving only a narrow patterned helmet rim.
    assert 0 < depth < 0.25 * radius
    assert (
        "Math.sqrt(\n"
        "  AVATAR_HEAD_RADIUS * AVATAR_HEAD_RADIUS - AVATAR_FACE_DEPTH * AVATAR_FACE_DEPTH,\n"
        ")" in SCENE
    )
    # Turned to look out of the cut, and held just off it so the coplanar disc
    # and cap cannot z-fight.
    assert (
        "faceMesh.position.z = -(AVATAR_FACE_DEPTH + AVATAR_FACE_LIFT);" in SCENE
    )
    assert 0 < number("AVATAR_FACE_LIFT") < 0.02
    # An unscaled head keeps the cut a true circle, so the photo stays square.
    assert "head.scale.y" not in SCENE
    assert "faceMesh.scale.y" not in SCENE


def test_head_colour_is_sampled_from_the_edge_of_the_emoji():
    # adhoc #316: the head takes the glyph's own rim colour so the sphere and
    # the decal wrapped over it are indistinguishable. The whole-glyph
    # sampler stays as the fallback for emoji with no single rim colour.
    assert "function edgeEmojiColor" in SCENE
    assert (
        "edgeEmojiColor(context, canvas) || dominantEmojiColor(context, canvas)"
        in SCENE
    )
    assert "skin.color.set(drawn.color);" in SCENE


def test_open_members_circle_is_the_existing_world_map_destination():
    assert 'id: "campfire"' in DATA
    assert 'shortLabel: "Members Center"' in DATA
    block = DATA[DATA.index('id: "campfire"'):]
    block = block[: block.index("\n  },")]
    assert "position: [0, 0, 130]" in block
    assert 'action: "campfire"' in block
    assert 'eyebrow: "OPEN COMMUNITY / 04"' in block
    assert "There is no enclosing shell, roof, or entry collision" in block
    assert "campfire.position.set(...landmarkById(\"campfire\").position);" in SCENE
    assert '  "campfire",\n' in APP.split("LOCAL_LIVE_LANDMARKS", 1)[1]


def test_members_circle_has_no_shell_door_visibility_or_collision_gate():
    clearing = SCENE.split("const MEMBER_CLEARING_RADIUS = 12;", 1)[1].split(
        "const SWING_SEAT_COUNT", 1
    )[0]
    assert 'memberCircleInfo.name = "members-circle-info-sign"' in clearing
    assert "campfire.add(memberCircleInfo);" in clearing
    assert "membersYurt" not in SCENE
    assert "members-yurt" not in SCENE
    assert "updateMembersYurtOccupancy" not in SCENE
    assert "constrainMembersYurt" not in SCENE
    assert "syncEnclosureDoors" not in SCENE
    assert "setSlidingEnclosureDoorOpen" not in SCENE


def test_choosing_the_members_spot_moves_you_to_the_open_circle_edge():
    scene = SCENE.split("function returnToCampfireBench", 1)[1].split(
        "\n  function ", 1
    )[0]
    assert "MEMBER_CLEARING_RADIUS" in scene
    assert 'activity: "visiting the Members Circle"' in scene
    assert "player.position.copy(entrance)" in scene
    assert "campfire.userData.seatByName" not in scene
    assert 'officeSceneMode !== "town"' in scene
    assert "returnToCampfireBench,\n" in SCENE

    # The map button travels instead of opening a reading panel.
    click = APP.split('const id = landmarkButton.dataset.worldLandmark;', 1)[1]
    click = click[: click.index("this.openLandmark(id")]
    assert 'if (id === "campfire")' in click
    assert (
        "this.world?.returnToCampfireBench?.(this.identity?.name || \"\")"
        in APP
    )


def test_sitting_survives_the_campfire_landmark_proximity_label():
    # The bench is inside the campfire landmark's own label radius, so the
    # proximity relabel would otherwise overwrite the seated activity other
    # visitors render the pose from.
    # The constant lives with the data both the shell and the scene already
    # import: the shell reads it before the renderer module has even loaded.
    assert "export const CAMPFIRE_SEATED_ACTIVITY" in DATA
    assert "export const SWING_RIDING_ACTIVITY" in DATA
    data_import = APP.split('from "./world-data.js";', 1)[0]
    assert "CAMPFIRE_SEATED_ACTIVITY," in data_import
    assert "SWING_RIDING_ACTIVITY," in data_import
    assert "this.world = await scene.createWorldScene({" in APP
    location = APP.split("  updateLocation(label, id) {", 1)[1].split(
        "\n  updateRegion(", 1
    )[0]
    assert "this.lastMovement.activity !== CAMPFIRE_SEATED_ACTIVITY" in location


def test_campfire_clearing_keeps_its_landmark_tree_ring_out():
    # Landmark tree clusters stand at radius 6.4-8.5, which is where the bench
    # ring already is, so the campfire opts out of them.
    trees = SCENE.split("for (let treeIndex = 0", 1)[0]
    assert 'if (["campfire", "office"].includes(landmark.id)) return;' in trees


def test_directory_members_are_all_visible_as_open_circle_avatars():
    lounge = SCENE.split("function updateMemberLounge", 1)[1].split(
        "function visitNeighborhoodHome", 1
    )[0]
    assert "const rosterMembers =" in lounge
    assert ".slice(0, MEMBER_CIRCLE" not in lounge
    assert "const slot = layout.memberSlots[index]?.position || seat;" in lounge
    assert 'status: "gathering in the open Members Circle"' in lounge
    assert "figure = createAvatar(" in lounge
    assert "campfire.userData.memberFigureCount = seen.size;" in lounge
    # Everyone the circle shows is on a bench, so the seated tally is the whole
    # set of figures rather than the zero a benchless circle reported.
    assert "campfire.userData.seatedMemberFigures = seen.size;" in lounge
    assert "return;" in lounge


def test_legs_hinge_at_the_hip_and_knee_with_the_shoe_on_the_shin():
    # adhoc #323: a single leg box pivoting about its own centre pushed half
    # the leg out behind the sitter and left the shoes behind on the plank.
    avatar = SCENE.split("function createAvatar", 1)[1].split(
        "function updateAvatarBadge", 1
    )[0]
    assert "const buildLeg = (side) => {" in avatar
    assert "hip.position.set(side * 0.3, AVATAR_HIP_Y, 0);" in avatar
    assert "knee.position.y = -AVATAR_LEG_SEGMENT;" in avatar
    # The shoe is a child of the knee, so feet travel with the leg they belong
    # to instead of staying planted under the body.
    assert "knee.add(foot);" in avatar
    assert "leftKnee: leftLegRig.knee," in avatar
    assert "rightKnee: rightLegRig.knee," in avatar
    # Straight-legged poses lock the knees back out.
    assert "function applyLegPitch" in SCENE
    assert "if (legs.leftKnee) legs.leftKnee.rotation.x = 0;" in SCENE


def test_bench_height_is_set_by_the_sitters_not_the_other_way_round():
    # The plank top lands a seated avatar's hip-to-sole above the dirt the
    # circle is laid on, so shins reach the ground instead of dangling or
    # folding through it — and a member and a visitor side by side sit at one
    # height. The clearing's own surface is what that is measured from: the
    # Office floor slab is an indoor height and left the whole ring hovering.
    assert (
        "const CAMPFIRE_SEAT_TOP_Y = CAMPFIRE_GROUND_Y + SEATED_SEAT_TO_SOLE;"
        in SCENE
    )
    assert (
        "const CAMPFIRE_SEAT_Y = CAMPFIRE_SEAT_TOP_Y - CAMPFIRE_SEAT_HALF_THICKNESS;"
        in SCENE
    )
    lounge = SCENE.split("function updateMemberLounge", 1)[1].split(
        "function visitNeighborhoodHome", 1
    )[0]
    assert "seatedAvatarY(campfire.position.y + seat.y, figure.scale.x)" in lounge
    assert "applySeatedLegPose(figure);" in lounge
    # Past the bench ceiling a figure stands in its place rather than being
    # dropped from the circle, on the same dirt the benches are planted in.
    assert "campfire.position.y + CAMPFIRE_GROUND_Y" in lounge
    assert "applyLegPitch(figure, 0, 0);" in lounge


def test_open_circle_fills_growing_rings_with_non_walking_member_figures():
    interior = SCENE.split(
        "// Every member the public directory lists owns a named bench", 1
    )[1].split("function visitNeighborhoodHome", 1)[0]
    assert "function buildMemberCircleLayout(rankedMembers, guestBenches)" in SCENE
    assert "const MEMBER_CIRCLE_STANDING_ROOM = 2.4;" in SCENE
    assert "member?.away === true" in interior
    assert "campfire.userData.detailedMemberFigures" in SCENE
    assert "campfire-instanced-member-sitters" not in SCENE
    assert "updateCompactCampfireSitters" not in SCENE
    assert "figure = createAvatar(" in interior
    # Directory members stand at a person's height, not shrunk to make the
    # roster fit: the same scale the player's own avatar is built at.
    assert "const MEMBER_CIRCLE_FIGURE_SCALE = 1;" in SCENE
    assert "scale: MEMBER_CIRCLE_FIGURE_SCALE," in interior
    assert "instances: avatarPartInstances," in interior
    assert "campfire.userData.seatedMemberFigures = seen.size;" in interior
    assert "figure.rotation.y = Math.atan2(slot.x, slot.z);" in interior


def test_member_total_and_latest_name_update_the_open_circle_sign_only():
    assert (
        "function membersCircleInfoTexture(THREE, total, newest = \"\", counts = {})"
        in SCENE
    )
    assert 'memberCircleInfoFace.name = "members-circle-member-info"' in SCENE
    assert "memberCircleInfoFace.material.map = membersCircleInfoTexture(" in SCENE
    assert 'latest ? `LATEST · ${latest.toUpperCase()}`' in SCENE
    lounge = SCENE.split("function updateMemberLounge", 1)[1]
    assert "newestMemberName(members)," in lounge
    assert "engagementCounts," in lounge
    setter = SCENE.split("function setCampfireMemberCount", 1)[1].split(
        "\n  }", 1
    )[0]
    assert "setMemberCircleInfo(count, latest, counts);" in setter
    assert "rebuildCampfireMemberLogs(count)" not in setter
    assert "memberCountSprite.material" not in setter
    assert "if (memberCountShown === key) return;" in SCENE


def test_fire_tracks_newest_member_without_rendering_a_center_label():
    newest = SCENE.split("function newestMemberName(members) {", 1)[1].split(
        "\n}", 1
    )[0]
    # Ranked by the directory's joined timestamp, newest wins ties-free.
    assert "if (!name || created <= joinedAt) return;" in newest
    # A member seated from a presence frame has no joined date (0) yet, so it
    # must not be ranked as the newest — or as the oldest.
    assert "const created = Number(member?.createdAt) || 0;" in newest
    # The repaint key carries the name, so a new arrival repaints at the same
    # count when the directory backfills their joined date.
    assert "const key = `${count}|${latest}|${JSON.stringify(boundedCounts)}`;" in SCENE


def test_member_total_tracks_arrivals_without_waiting_for_the_poll():
    # adhoc #393: the count is repainted from every presence frame, so a new
    # account is counted the moment it joins instead of at the next 60s tick.
    note = APP.split("noteDirectoryMembers(names) {", 1)[1].split("\n  }", 1)[0]
    assert "void this.refreshMemberDirectory(true, missing);" in note
    refresh = APP.split(
        "async refreshMemberDirectory(force = false, probed = []) {", 1)[1]
    refresh = refresh.split("\n  }", 1)[0]
    # The forced fetch bypasses the idle throttle and the client-side copy,
    # but never fires inside the endpoint's own edge-cache TTL.
    assert "? USERS_DIRECTORY_TTL_MS" in refresh
    assert ": WORLD_MEMBER_DIRECTORY_POLL_MS;" in refresh
    assert "maxAge: force ? 0 : WORLD_MEMBER_DIRECTORY_POLL_MS," in refresh
    assert "const USERS_DIRECTORY_TTL_MS = 30 * 1000;" in APP
    # A tab coming back from hidden is up to a full tick behind.
    visibility = APP.split("handleVisibility = () => {", 1)[1].split("\n  };", 1)[0]
    assert "void this.refreshMemberDirectory();" in visibility


def test_only_users_table_accounts_are_counted_at_the_fire():
    # adhoc #427: a presence frame is not a membership. Guests, bots and
    # private profiles broadcast names too, so a name is only ever a reason to
    # re-read /api/accounts/users — the count and the benches come from that
    # snapshot alone, and a name it does not list is never seated locally.
    note = APP.split("noteDirectoryMembers(names) {", 1)[1].split("\n  }", 1)[0]
    assert "this.memberDirectory = [" not in note
    assert "this.unlistedDirectoryNames.has(key)" in note
    refresh = APP.split(
        "async refreshMemberDirectory(force = false, probed = []) {", 1)[1]
    refresh = refresh.split("\n  }", 1)[0]
    assert "this.memberDirectory = directory;" in refresh
    # A name the directory came back without must not force a fetch again.
    assert "if (!listed.has(key)) this.unlistedDirectoryNames.add(key);" in refresh
    # Everyone present without a users-table row takes a spare seat instead of
    # a named bench, so the ring still fits the crowd.
    lounge = APP.split("flushMemberLounge() {", 1)[1].split("\n  }", 1)[0]
    assert "if (!listed.has(key)) guests += 1;" in lounge
    assert "this.memberDirectory.length," in lounge


def test_seated_members_get_a_chat_bubble_over_their_bench():
    # adhoc #363: a member talking in the website chat is usually not a live
    # world peer — their avatar is the figure sitting on their own campfire
    # bench, so the bubble floats over that seated figure.
    assert "function showMemberChatBubble(name, text)" in SCENE
    bubble = SCENE.split("function showMemberChatBubble", 1)[1].split(
        "\n  }", 1
    )[0]
    assert 'loungeMembers.has(`member:${wanted}`)' in bubble
    # Truncated senders (16-character room names) still find their bench.
    assert "if (wanted.length < 16) return false;" in bubble
    # The bubble resolver reaches seated figures, not just live peers.
    assert (
        'remotePlayers.get(String(peerId || "")) ||\n'
        '            loungeMembers.get(String(peerId || ""))' in SCENE
    )
    assert "showMemberChatBubble," in SCENE
    # Untrusted remote public-chat senders do not get to select a World avatar
    # by copying its display name. Own lines remain attached to this browser's
    # signed identity and seated-member resolution stays available to trusted
    # scene callers.
    chat = APP.split("handleWorldChatMessage = (event) => {", 1)[1].split(
        "\n  };", 1
    )[0]
    assert "if (data.self === true)" in chat
    assert "this.world?.showChatBubble?.(this.identity?.id, text, true);" in chat
    assert "this.world?.showMemberChatBubble?.(sender, text);" not in chat


def test_custom_avatar_members_sit_in_the_innermost_labelled_ring():
    assert 'id: "custom-avatar",' in SCENE
    layout = SCENE.split("function buildMemberCircleLayout", 1)[1].split(
        "campfireGround.scale.setScalar", 1
    )[0]
    assert "[MEMBER_CIRCLE_AVATAR_BAND, ...MEMBER_ENGAGEMENT_STATES]" in layout
    assert "memberHasCustomAvatar(member)" in layout
    lounge = SCENE.split("function updateMemberLounge", 1)[1].split(
        "function visitNeighborhoodHome", 1
    )[0]
    assert lounge.index("...rankedRoster.filter(memberHasCustomAvatar),") < (
        lounge.index("!memberHasCustomAvatar(member)")
    )
    bands = SCENE.split("function rebuildMemberEngagementBands", 1)[1].split(
        "const MEMBER_INFO_SIGN_WIDTH", 1
    )[0]
    assert "campfire-member-engagement-label-" not in bands
    assert "memberBandLetteringTexture(" in bands
    assert "memberBandLetteringFor(lettering, band.outerRadius)" in bands
    assert "emissiveMap: letters," in bands
    assert "bumpMap: letters," in bands


def test_member_face_photo_is_worn_unless_explicitly_opted_out():
    assert "faceImage: peer.faceImage !== false," in APP
    assert '"faceImage": None,' in WORLD_PY
    assert "AVATAR_FACE_IMAGE_RETRIES" in SCENE
