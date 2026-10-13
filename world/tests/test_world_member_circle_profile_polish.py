"""Focused contracts for the member-circle and avatar profile polish."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2]
SCENE = world_scene_source()
APP = (ROOT / "world/public/world/world.js").read_text()
HTML = (ROOT / "world/public/world/index.html").read_text()
ENTRY = (
    (ROOT / "app/src/entry.py").read_text()
    + (ROOT / "app/src/app_runtime.py").read_text()
)


def test_members_circle_is_an_open_fixed_cost_clearing():
    assert "function campfireDirtTexture(THREE)" in SCENE
    assert 'campfireGround.name = "campfire-member-circle-dirt"' in SCENE
    assert "const MEMBER_CLEARING_RADIUS = 12;" in SCENE
    assert "campfireGround.scale.setScalar(MEMBER_CLEARING_RADIUS + 0.8)" in SCENE
    assert "membersYurt" not in SCENE
    assert "members-yurt" not in SCENE
    assert "function rebuildCampfireMemberLogs" not in SCENE
    assert "memberLogIndex" not in SCENE
    # The hearth remains bounded and independent of roster growth.
    assert "const CAMPFIRE_BASE_FIRE_LEVEL = 3;" in SCENE
    assert "proceduralFire.position.y = FLAME_BASE_Y;" in SCENE


def test_campfire_uses_layered_procedural_flames_and_atmosphere():
    effect = SCENE.split("function createProceduralCampfireEffect(THREE)", 1)[1]
    effect = effect.split("\n// Who joined last", 1)[0]
    assert "new THREE.ConeGeometry" not in effect
    assert "const flameLayers = [" in effect
    assert "fragmentShader: flameFragmentShader" in effect
    assert "blending: THREE.AdditiveBlending" in effect
    assert "if (alpha < 0.008) discard;" in effect
    assert '"campfire-rising-embers"' in effect
    assert '"campfire-ground-glow"' in effect
    assert '"campfire-smoke-puffs"' in effect
    assert '"campfire-flame-layers"' in effect
    assert "tongue.renderOrder" not in effect
    assert '"campfire-charred-burning-logs"' in SCENE
    assert "fireLight.position.set(" in SCENE


def test_member_count_is_on_a_shared_mesh_sign_at_the_open_approach():
    assert (
        "function membersCircleInfoTexture(THREE, total, newest = \"\", counts = {})"
        in SCENE
    )
    assert 'memberCircleInfo.name = "members-circle-info-sign"' in SCENE
    assert 'memberCircleInfoFace.name = "members-circle-member-info"' in SCENE
    assert "cloneSharedPlane(" in SCENE
    assert '"member-circle-sign-post"' in SCENE
    assert "const MEMBER_INFO_SIGN_WIDTH = 6.2;" in SCENE
    assert (
        "(WORLD_PATH_WIDTH + MEMBER_INFO_SIGN_WIDTH) / 2 + 1.1" in SCENE
    )
    assert "memberCircleInfo.position.set(\n    MEMBER_INFO_SIGN_X," in SCENE
    # PlaneGeometry faces +Z by default, while visitors approach from -Z.
    assert "memberCircleInfoFace.rotation.y = Math.PI;" in SCENE
    assert 'context.fillText("MEMBERS CENTER", 512, 72);' in SCENE


def test_latest_member_name_is_painted_on_the_door_plaque():
    texture = SCENE.split("function membersCircleInfoTexture", 1)[1].split(
        "\n}", 1
    )[0]
    assert 'latest ? `LATEST · ${latest.toUpperCase()}`' in texture
    assert "NEWEST MEMBER" not in texture
    assert "campfire-newest-member-name-sparkles" not in SCENE
    assert "campfire-member-count-high" not in SCENE


def test_campfire_is_three_times_large_not_only_three_times_tall():
    assert (
        "proceduralFire.scale.set(\n"
        "    CAMPFIRE_BASE_FIRE_LEVEL,\n"
        "    CAMPFIRE_BASE_FIRE_LEVEL,"
    ) in SCENE
    assert (
        "CAMPFIRE_BASE_FIRE_LEVEL * (1 + (fireGrowth - 1) * 0.32)"
        in SCENE
    )
    assert "fireLevel * (1 + slowFlicker * 0.055" in SCENE
    assert "new THREE.CylinderGeometry(1.45, 1.65, 0.22, 16)" in SCENE
    assert "Math.cos(stoneAngle) * 1.85" in SCENE


def test_fresh_arrivals_spawn_seated_without_overriding_saved_views():
    bootstrap = APP.split("async bootstrap() {", 1)[1].split(
        "\n  setupControls()", 1
    )[0]
    assert bootstrap.index("this.syncMemberLounge();") < bootstrap.index(
        "this.seatFreshArrivalAtCampfire();"
    )
    assert bootstrap.index("this.seatFreshArrivalAtCampfire();") < bootstrap.index(
        "if (this.restoredPosition) {"
    )
    arrival = APP.split("seatFreshArrivalAtCampfire() {", 1)[1].split(
        "\n  returnToCampfireBench()", 1
    )[0]
    assert "this.restoredPosition ||" in arrival
    assert "this.sharedView ||" in arrival
    assert "this.spawnSelected ||" in arrival
    assert 'this.currentSpace !== "town-square"' in arrival
    assert 'this.world?.returnToCampfireBench?.(this.identity?.name || "")' in arrival
    assert "this.freshArrivalCampfireSeated = true;" in arrival
    assert "FRESH_ARRIVAL_CAMPFIRE_PREVIEW" in APP
    assert "restoreCampfireSeatIfNearby" in APP
    assert "function restoreCampfireSeatIfNearby(spawn, name)" in SCENE
    welcome = APP.split(
        'if (message.type === "welcome" && Array.isArray(message.peers)) {', 1
    )[1].split(
        '} else if (["presence", "join"].includes(message.type)', 1
    )[0]
    assert "this.initialPresenceWelcomePending &&" in welcome
    assert "this.syncMemberLounge();" in welcome
    assert 'this.world?.returnToCampfireBench?.(this.identity?.name || "");' in welcome


def test_email_pin_switches_between_verified_check_and_unverified_x():
    texture = SCENE.split(
        "function emailVerificationPinTexture", 1
    )[1].split("\nfunction ", 1)[0]
    assert 'verified ? "#9ef7c6" : "#ff6b72"' in texture
    assert "context.lineTo(65, 65)" in texture
    assert "context.lineTo(31, 65)" in texture
    sync = SCENE.split(
        "function syncAvatarVerifiedPin", 1
    )[1].split("\nfunction ", 1)[0]
    assert "pin.visible = registered" in sync
    assert "avatarPinMaterial(THREE, verified)" in sync
    pin_material = SCENE.split(
        "function avatarPinMaterial", 1
    )[1].split("\nfunction ", 1)[0]
    assert "emailVerificationPinTexture(THREE, verified)" in pin_material


def test_public_chat_line_repaints_matching_avatar_chest_only():
    assert 'context.fillText("RECENT PUBLIC CHAT", 34, 356)' in SCENE
    assert "identity.recentPublicMessage" in SCENE
    assert "function setAvatarRecentPublicMessage(sender, text)" in SCENE
    assert "setAvatarRecentPublicMessage," in SCENE
    assert "this.world?.setAvatarRecentPublicMessage?.(" in APP


def test_world_users_do_not_have_a_separate_top_hair_cap():
    avatar = SCENE.split(
        "function createAvatar(THREE, identity, options = {})", 1
    )[1].split("\nconst WORK_BADGE_ROWS", 1)[0]
    assert "const hair = new THREE.Mesh(" not in avatar
    assert "group.add(hair)" not in avatar


def test_avatar_profile_signals_moved_to_head_and_back():
    assert "const AVATAR_FACE_DEPTH = 0.08;" in SCENE
    assert "shirtMeshes: [torso, leftArm, rightArm, head]" in SCENE
    assert 'activityRing.name = "avatar-activity-ring"' in SCENE
    assert '"avatar-mood-icon-word"' not in SCENE
    assert "MOOD · ${frontMood}" in SCENE
    assert 'backName.name = "avatar-back-username"' in SCENE
    assert "const firstCounts = `PRS ${pulls} · ISSUES ${issues}`;" in SCENE
    assert "const discussionCounts = `DISCUSSIONS ${discussions}`;" in SCENE
    assert 'context.fillText("OPEN FILTERED ACTIVITY ↗", 320, 181);' in SCENE
    assert "`TASKS ${taskTotal} · RUNNING ${taskActive}`" in SCENE
    assert '"SELECT USER FOR FULL PROFILE"' in SCENE
    assert "typeTotals:" in APP
    assert "#contributions" in APP
    assert '"https://forkmesh.com"' in APP
    assert "function worldPublicProfileUrl(name, suffix = \"\")" in APP
    assert "worldPublicProfileUrl(name, \"#contributions\")" in APP


def test_badge_has_large_qr_email_and_social_icons_without_activity_border():
    assert "context.strokeRect(376, 12, 124, 124);" in SCENE
    assert "drawQrModules(context, walletAddress, 384, 20, 108);" in SCENE
    assert '"LAST EMAIL · NOT SHARED"' in SCENE
    assert "loadWorldEmailActivity" in APP
    assert "[270, 294].forEach" in SCENE
    assert "[326, 350, 374].forEach" in SCENE
    assert "const activityBorder =" not in SCENE
    assert "verifiedPin.position.set(-0.66, 2.92, -0.22);" in SCENE


def test_mirror_lights_are_colored_and_warning_states_blink():
    assert "function mirrorNodeVisualState(node)" in SCENE
    assert 'return { color: "#22e06a", blink: false };' in SCENE
    assert 'return { color: "#ffd23f", blink: true };' in SCENE
    assert 'return { color: "#ff4d57", blink: true };' in SCENE
    assert "child.userData?.mirrorStatusLight" in SCENE
    assert "mirrorByName" in APP


def test_non_walking_directory_members_use_full_detail_avatars_in_the_open_circle():
    interior = SCENE.split(
        "// Every member the public directory lists owns a named bench", 1
    )[1].split("// Legacy bench-circle implementation", 1)[0]
    assert "const MEMBER_CIRCLE_STANDING_ROOM = 2.4;" in SCENE
    assert "const slot = layout.memberSlots[index]?.position || seat;" in interior
    assert "member?.away === true" in interior
    assert 'status: "gathering in the open Members Circle"' in interior
    assert "figure = createAvatar(" in interior
    assert "figure.position.set(" in interior
    assert "applyLegPitch(figure, 0, 0)" in interior
    assert "figure.userData.ambientInteraction = null" in SCENE
    assert "const memberWorldDestinations" not in SCENE
    assert "const memberNpcStations" not in SCENE
    assert "ambientRoutePosition" not in SCENE


def test_repeated_avatar_meshes_share_geometry_and_keep_full_detail():
    shared = SCENE.split("function cloneSharedMesh", 1)[1].split(
        "function setShadows", 1
    )[0]
    avatar = SCENE.split(
        "function createAvatar(THREE, identity, options = {})", 1
    )[1].split("\nconst WORK_BADGE_ROWS", 1)[0]
    assert "const sharedMeshPrototypeCaches = new WeakMap();" in SCENE
    assert "prototype.clone(false)" in shared
    assert "geometry.userData.forkmeshSharedResource = true" in shared
    assert "geometry.userData?.forkmeshSharedResource === true" in SCENE
    assert "return false;" in shared
    assert 'highDetail.name = "avatar-full-detail"' in avatar
    assert "group.add(highDetail);" in avatar
    assert "new THREE.LOD()" not in avatar
    assert "cloneAvatarFarModel" not in SCENE
    assert "cloneSharedMesh(" in avatar
    assert "cloneSharedPlane(" in avatar


def test_camera_pan_and_tilt_turn_a_peer_head_and_leave_your_own_square():
    assert "function applyAvatarLookDirection(avatar, yaw, pitch, turnBody = true)" in SCENE
    assert 'headRig.name = "avatar-head-look-rig"' in SCENE
    assert "const lookPitch = clamp(-(Number(pitch) || 0), -0.62, 0.62);" in SCENE
    assert "avatar.userData.headRig.rotation.x = lookPitch;" in SCENE
    assert "avatar.userData.torso.rotation.x = lookPitch * 0.12;" in SCENE
    # Peers wear the angles; the visitor driving the camera does not, so the
    # portrait and chest card on their own avatar stay centred and level.
    remote = SCENE.split("function updateRemotePlayers(delta, time)", 1)[1].split(
        "\n  function ", 1
    )[0]
    assert "applyAvatarLookDirection(" in remote
    assert "avatar.userData.lookYaw," in remote
    reset = SCENE.split("function resetAvatarLookDirection(avatar) {", 1)[1].split(
        "\n}", 1
    )[0]
    assert "avatar.userData.headRig?.rotation.set(0, 0, 0);" in reset
    assert "avatar.userData.torso?.rotation.set(0, 0, 0);" in reset
    rotate = SCENE.split("function rotateCamera(deltaX, deltaY)", 1)[1].split(
        "\n  function ", 1
    )[0]
    assert "resetAvatarLookDirection(player);" in rotate
    assert "applyAvatarLookDirection(" not in rotate
    # The angles still leave the client, so other visitors keep seeing them.
    assert "onCameraLook({" in rotate
    assert "yaw: cameraYaw," in rotate


def test_user_selection_has_a_scoped_clock_and_prefers_avatar_hit():
    selection = SCENE.split(
        "function setActiveAvatarSelection", 1
    )[1].split("\n  function ", 1)[0]
    assert "const now = performance.now()" in selection
    assert "if (now >= nextAvatarHighlightAt)" in selection
    assert "const avatarHit = hits.find(" in SCENE
    assert "const hit = avatarHit || firstHit" in SCENE


def test_lobby_has_wall_doorways_with_access_and_walk_through_checks():
    assert '"forkmesh-office-floor-warp-hub"' in SCENE
    assert "OFFICE_FLOORS.forEach((floor, index) =>" in SCENE
    assert "`forkmesh-office-floor-doorway-${floor.id}`" in SCENE
    assert "OFFICE_FLOOR_WARP_DOOR_START_X +" in SCENE
    assert 'portal.userData.interactive = "office-floor-warp"' in SCENE
    assert 'closedDoor.userData.interactive = "office-floor-warp"' in SCENE
    assert '"CLOSED · ACCESS REQUIRED"' in SCENE
    assert "function refreshOfficeFloorWarpDoors()" in SCENE
    assert "doorway.closedDoor.visible = !allowed" in SCENE
    assert "function warpToOfficeFloor(floorId)" in SCENE
    assert "canAccessOfficeFloor(officeFloorAccess, floor.id)" in SCENE
    assert "function tryOfficeFloorWarpDoorway(avatar, previousPosition)" in SCENE
    assert "current.z > OFFICE_FLOOR_WARP_TRIGGER_Z" in SCENE
    assert "tryOfficeFloorWarpDoorway(avatar, previousPosition)" in SCENE
    assert 'interactive === "office-floor-warp"' in SCENE
    assert "hit.object.userData.officeFloorTargetId" in SCENE


def test_world_asset_loading_and_unavailable_actions_are_console_quiet():
    # Distant scene textures no longer receive HTML preload priority. Their
    # same-origin loaders request them as scene construction needs them.
    for asset in (
        "concrete-brick-path-v1.webp",
        "beach-horizon-v1.webp",
    ):
        assert asset not in HTML
        assert f'"/world/assets/{asset}"' in SCENE
    assert "city-park-grass-v1.webp" not in HTML
    face = SCENE.split(
        "function makeConsentedProfileFace", 1
    )[1].split("\nfunction ", 1)[0]
    assert "avatar.origin === window.location.origin" in face
    assert "missing" in face
    handler = ENTRY.split(
        "async def repository_actions_status_handler", 1
    )[1].split("\n\nasync def ", 1)[0]
    assert '"state": "unavailable"' in handler
    assert '"retryable": True' in handler
    assert "status=503" in handler
    assert "EXPECTED_DEGRADED_HEADERS" in handler


def test_back_name_and_work_board_are_worn_on_the_torso_back():
    avatar = SCENE.split(
        "function createAvatar(THREE, identity, options = {})", 1
    )[1].split("\nfunction applyAvatarLookDirection", 1)[0]
    assert "torso.add(backName);" in avatar
    assert "highDetail.add(backName);" not in avatar
    assert "backName.scale.setScalar(0.628);" in avatar
    assert "backName.position.set(0, 0.47, 0.33);" in avatar
    assert "(avatar.userData.torso || avatarDetailParent(avatar)).add(badge);" in SCENE
    assert "badge.scale.setScalar(0.63);" in SCENE
    assert "badge.position.set(0, -0.2, 0.33);" in SCENE
    half_width, half_height, back_face = 0.55, 0.75, 0.31
    belt_top = 1.43 + 0.09 - 2.15
    name_width, name_height = 1.72 * 0.628, 0.806 * 0.628
    assert name_width / 2 <= half_width
    assert 0.47 + name_height / 2 <= half_height
    board_width, board_height = 1.06 * 0.63, 1.32 * 0.63
    assert board_width / 2 <= half_width
    assert -0.2 + board_height / 2 <= 0.47 - name_height / 2 + 1e-3
    assert -0.2 - board_height / 2 >= belt_top
    assert 0 < 0.33 - back_face <= 0.03
