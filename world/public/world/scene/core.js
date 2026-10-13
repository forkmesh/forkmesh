import { createWorldBackoff } from "../world-backoff.js";
import { normalizeOfficeFloorAccess } from "../world-office-tower.js";
import { createWorldSky } from "../world-sky.js";
import {
  auditWorldScene,
  createWorldAuditState,
  createWorldProfiler,
} from "../world-profiler.js";
import { INSTANCED_SOURCE_LAYER } from "../world-resource-cache.js";
import { FORKBOT_HOME } from "./agent-bots.js";
import { createAvatarPartInstances } from "./avatar-instancing.js";
import { createFigureBatches } from "./figure-batches.js";
import {
  createAvatar,
  normalizeSelfWorkBoard,
} from "./avatars.js";
import { MASTODON_KIOSK_REFRESH_MS } from "./boards.js";
import {
  canvasTextureScale,
  deferredCanvasPaints,
  setCanvasTextureScale,
  setDeferredCanvasPaints,
} from "./canvas.js";
import {
  DAYLIGHT_ENVIRONMENT,
  WORLD_FOG_FAR,
  WORLD_FOG_NEAR,
} from "./environment.js";
import {
  CAMERA_DISTANCE,
  CAMERA_FAR_PLANE,
  CAMERA_OFFSET,
  CAMERA_ZOOM_MAX,
  CAMERA_ZOOM_MIN,
  FIRST_PERSON_EYE_HEIGHT,
  FIRST_PERSON_ZOOM_MAX,
  FIRST_PERSON_ZOOM_MIN,
  JETPACK_MAX_ALTITUDE,
  LIGHT_LEVEL_DEFAULT,
  PLAYER_MAX_SPEED,
  PLAYER_SPEED,
  QUADCOPTER_MAX_ALTITUDE,
} from "./gameplay-constants.js";
import { createGeometryDeduper } from "./geometry-dedupe.js";
import { makePlayerLabel } from "./labels.js";
import {
  MOVEMENT_KEYS,
  WORLD_SPACE_FLOORS,
  officeCampusSpace,
  packageSpace,
  worldRadiusForSpace,
} from "./movement.js";
import { clamp } from "./object-utils.js";
import { createSystemCapacityPlatform } from "./office-infrastructure.js";
import { normalizeOfficeMarketingTasks } from "./office-marketing.js";
import {
  RENDER_FRAME_INTERVAL_MS,
  WEATHER_PARTICLE_COUNT,
  planWorldRenderer,
  releaseWorldRendererContext,
  worldQualityProfile,
} from "./quality.js";
import {
  CANVAS_RESIZE_COMPILE_WAIT_MS,
  FIRST_VIEW_HOLD_MS,
  FIRST_VIEW_WARM_PASSES,
  FIRST_VIEW_WARM_SLICE_MS,
  SCENE_BUILD_FRAME_MS,
  SCENE_BUILD_SLICE_MS,
  yieldSceneBuild,
  yieldSceneFrame,
} from "./scene-build.js";
import {
  createSettledGroupCuller,
  installSettledMatrixUpdate,
} from "./settled-matrices.js";
import { installShaderCompileGuard } from "./shader-warmup.js";
import { createRenderStallMonitor } from "./render-stalls.js";
import {
  createStripedUploads,
  createUploadBudget,
  installProgramPacer,
} from "./gpu-pacer.js";
import * as renderSetupRuntime from "./runtime/render-setup.js";
import * as worldElementsRuntime from "./runtime/world-elements.js";
import * as bikesDistrict from "./districts/bikes.js";
import * as beachDistrict from "./districts/beach.js";
import * as quadcoptersDistrict from "./districts/quadcopters.js";
import * as coasterDistrict from "./districts/coaster.js";
import * as billboardCirclesDistrict from "./districts/billboard-circles.js";
import * as arrivalDistrict from "./districts/arrival.js";
import * as socialPackagesDistrict from "./districts/social-packages.js";
import * as membersCircleDistrict from "./districts/members-circle.js";
import * as leaderboardsDistrict from "./districts/leaderboards.js";
import * as compactDistrictsModule from "./districts/compact-districts.js";
import * as agentBotsDistrict from "./districts/agent-bots.js";
import * as officeFloorsDistrict from "./districts/office-floors.js";
import * as officeRooftopDistrict from "./districts/office-rooftop.js";
import * as officeLobbyDistrict from "./districts/office-lobby.js";
import * as officeBoardsDistrict from "./districts/office-boards.js";
import * as officeDisplaysDistrict from "./districts/office-displays.js";
import * as qualityEnvironmentRuntime from "./runtime/quality-environment.js";
import * as officeNavigationRuntime from "./runtime/office-navigation.js";
import * as travelRuntime from "./runtime/travel.js";
import * as officePresenceRuntime from "./runtime/office-presence.js";
import * as officeExitsRuntime from "./runtime/office-exits.js";
import * as cameraControlsRuntime from "./runtime/camera-controls.js";
import * as officeWalkingRuntime from "./runtime/office-walking.js";
import * as ridesRuntime from "./runtime/rides.js";
import * as droneRaceRuntime from "./runtime/drone-race.js";
import * as coasterRuntime from "./runtime/coaster.js";
import * as walkPlayerRuntime from "./runtime/walk-player.js";
import * as botsRuntime from "./runtime/bots.js";
import * as cameraRuntime from "./runtime/camera.js";
import * as avatarSelectionRuntime from "./runtime/avatar-selection.js";
import * as remotePlayersRuntime from "./runtime/remote-players.js";
import * as networkRuntime from "./runtime/network.js";
import * as repositoryCatalogRuntime from "./runtime/repository-catalog.js";
import * as repositoryRecordsRuntime from "./runtime/repository-records.js";
import * as effectsRuntime from "./runtime/effects.js";
import * as pointerRuntime from "./runtime/pointer.js";
import * as animateRuntime from "./runtime/animate.js";
import * as diagnosticsRuntime from "./runtime/diagnostics.js";
import * as recreationDistrict from "./districts/recreation.js";
import * as leaderboardDistrictModule from "./districts/leaderboard-district.js";
import * as roadsDistrict from "./districts/roads.js";
import * as townTerrainDistrict from "./districts/town-terrain.js";
import * as townBoardsDistrict from "./districts/town-boards.js";
import * as resources from "./resources.js";

export async function createWorldScene({
  THREE,
  container,
  labelLayer,
  identity,
  initialSpawn = null,
  initialDisabledElements = [],
  initialRemovedWorldPackages = [],
  initialInstalledWorldPackages = [],
  initialDeletedObjects = [],
  reducedMotion = false,
  forceCompactRenderer = false,
  bootTrace = null,
  sceneSliceMs = SCENE_BUILD_SLICE_MS,
  // A tier chosen by the embedder — a saved support preference, or the
  // ?quality= parameter QA drives the device matrix with — overrides
  // detection completely. Anything unrecognised falls back to detection.
  qualityTier = "",
  adaptiveQuality = true,
  checkShaderErrors = false,
  preparedRenderer = null,
  onLandmarkSelect = () => {},
  onOfficeProximity = () => {},
  onOfficeEnter = () => {},
  onOfficeTaskBoardSelect = () => {},
  onOfficeTaskWallAction = () => {},
  onOfficeMeetingBoardSelect = () => {},
  onOfficeRooftopLaptopSelect = () => {},
  onWorldBulletinSelect = () => {},
  onWorldGeneralChatSelect = () => {},
  onWorldDiscordBoardSelect = () => {},
  onFloorTap = () => {},
  onMastodonBoardSelect = () => {},
  onMastodonOpenLink = () => {},
  onReferralBoardSelect = () => {},
  onSiteReferrerOpen = () => {},
  onLaunchMirrorSelect = () => {},
  onStartWorldSignSelect = () => {},
  onDrinkWater = () => {},
  onFederatedWorldTravel = () => {},
  onInstanceHexagonSelect = () => {},
  onSystemCapacityTableSelect = () => {},
  onQaBoardNearby = () => {},
  onQaBoardAway = () => {},
  onLeaderboardCircleNearby = () => {},
  onLeaderboardCircleAway = () => {},
  onSocialCircleNearby = () => {},
  onSocialCircleAway = () => {},
  onQaVerdict = () => {},
  onQaAction = () => {},
  onRendererStateChange = () => {},
  onOfficeChairSelect = () => {},
  onOfficeMovement = () => {},
  onOfficeElevatorSound = () => {},
  onWorldPackageAccessRequest = () => {},
  onLocationChange = () => {},
  onRepositoryDistrictEnter = () => {},
  onRegionChange = () => {},
  onMovement = () => {},
  onModeration = () => {},
  onAdminGuestCopy = () => {},
  onOrgTeamAssign = () => {},
  onFediverseProfile = () => {},
  onFediverseFollow = () => {},
  onAvatarSelect = () => {},
  onUnverifiedAvatarDelete = () => {},
  onAvatarWalletAction = () => {},
  onForkbotChat = () => {},
  onAgentBotChat = () => {},
  onOpenForkmeshSong = () => {},
  onCreateRepository = () => {},
  onStartNodeDownload = () => {},
  onRewardBoardHover = () => {},
  onSwingRide = () => {},
  onCameraMode = () => {},
  onCameraLook = () => {},
  onJetpackChange = () => {},
  onGymState = () => {},
  onStartHereSelect = () => {},
}) {
  const fns = {};
  const ctx = new (class WorldSceneContext {
    get activeAvatarSelection() { return activeAvatarSelection; } set activeAvatarSelection(value) { activeAvatarSelection = value; }
    get activeWalkSurfaceContains() { return activeWalkSurfaceContains; }
    get adaptiveQualityEnabled() { return adaptiveQualityEnabled; } set adaptiveQualityEnabled(value) { adaptiveQualityEnabled = value; }
    get adminGuestCopyActions() { return adminGuestCopyActions; }
    get administrativelyRemovedAccounts() { return administrativelyRemovedAccounts; }
    get agentBotAccessAllowed() { return agentBotAccessAllowed; } set agentBotAccessAllowed(value) { agentBotAccessAllowed = value; }
    get agentBots() { return agentBots; }
    get agentBotStates() { return agentBotStates; }
    get animated() { return animated; }
    get aquariumControlLocalPosition() { return aquariumControlLocalPosition; }
    get aquariumPanel() { return aquariumPanel; } set aquariumPanel(value) { aquariumPanel = value; }
    get arrivalBox() { return arrivalBox; }
    get arrivalPlaque() { return arrivalPlaque; }
    get avatarPartInstances() { return avatarPartInstances; }
    get figureBatches() { return figureBatches; }
    get avatarSelectionBounds() { return avatarSelectionBounds; }
    get avatarSelectionHighlight() { return avatarSelectionHighlight; }
    get avatarSelections() { return avatarSelections; }
    get badgeDetailWorldPosition() { return badgeDetailWorldPosition; }
    get beachScene() { return beachScene; } set beachScene(value) { beachScene = value; }
    get beachSceneActive() { return beachSceneActive; } set beachSceneActive(value) { beachSceneActive = value; }
    get benchSeat() { return benchSeat; } set benchSeat(value) { benchSeat = value; }
    get bikeLoopLength() { return bikeLoopLength; } set bikeLoopLength(value) { bikeLoopLength = value; }
    get bikeRide() { return bikeRide; } set bikeRide(value) { bikeRide = value; }
    get bikeSeatPosition() { return bikeSeatPosition; }
    get bikeStates() { return bikeStates; } set bikeStates(value) { bikeStates = value; }
    get billboardCircleOrigin() { return billboardCircleOrigin; } set billboardCircleOrigin(value) { billboardCircleOrigin = value; }
    get billboardIslandBounds() { return billboardIslandBounds; } set billboardIslandBounds(value) { billboardIslandBounds = value; }
    get billboardSpinners() { return billboardSpinners; } set billboardSpinners(value) { billboardSpinners = value; }
    get bootQualityTier() { return bootQualityTier; }
    get botAgents() { return botAgents; }
    get bulletinFace() { return bulletinFace; }
    get bulletinFrame() { return bulletinFrame; }
    get bulletinScrollDown() { return bulletinScrollDown; }
    get bulletinScrollUp() { return bulletinScrollUp; }
    get camera() { return camera; }
    get cameraDesired() { return cameraDesired; }
    get cameraDirection() { return cameraDirection; }
    get cameraEye() { return cameraEye; }
    get cameraFocus() { return cameraFocus; } set cameraFocus(value) { cameraFocus = value; }
    get cameraLimitLocalTarget() { return cameraLimitLocalTarget; }
    get cameraLocalDesired() { return cameraLocalDesired; }
    get cameraLocalPosition() { return cameraLocalPosition; }
    get cameraLocalTarget() { return cameraLocalTarget; }
    get cameraLookTarget() { return cameraLookTarget; }
    get cameraMode() { return cameraMode; } set cameraMode(value) { cameraMode = value; }
    get cameraOffsetDirection() { return cameraOffsetDirection; }
    get cameraPitch() { return cameraPitch; } set cameraPitch(value) { cameraPitch = value; }
    get cameraSnapPending() { return cameraSnapPending; } set cameraSnapPending(value) { cameraSnapPending = value; }
    get cameraTarget() { return cameraTarget; }
    get cameraYaw() { return cameraYaw; } set cameraYaw(value) { cameraYaw = value; }
    get cameraZoom() { return cameraZoom; } set cameraZoom(value) { cameraZoom = value; }
    get campfire() { return campfire; }
    get CAMPFIRE_BENCH_LEG_HEIGHT() { return CAMPFIRE_BENCH_LEG_HEIGHT; }
    get CAMPFIRE_SEAT_HALF_THICKNESS() { return CAMPFIRE_SEAT_HALF_THICKNESS; }
    get CAMPFIRE_SEAT_TOP_Y() { return CAMPFIRE_SEAT_TOP_Y; }
    get CAMPFIRE_SEAT_Y() { return CAMPFIRE_SEAT_Y; }
    get campfireGround() { return campfireGround; }
    get canvasRectStale() { return canvasRectStale; } set canvasRectStale(value) { canvasRectStale = value; }
    get carRide() { return carRide; } set carRide(value) { carRide = value; }
    get carStates() { return carStates; } set carStates(value) { carStates = value; }
    get coaster() { return coaster; } set coaster(value) { coaster = value; }
    get coasterRide() { return coasterRide; } set coasterRide(value) { coasterRide = value; }
    get chestControls() { return chestControls; }
    get chrome() { return chrome; }
    get chromeCube() { return chromeCube; }
    get compactDistricts() { return compactDistricts; } set compactDistricts(value) { compactDistricts = value; }
    get compactRenderer() { return compactRenderer; }
    get compactTextureSlots() { return compactTextureSlots; } set compactTextureSlots(value) { compactTextureSlots = value; }
    get continuousCityLand() { return continuousCityLand; }
    get currentFloorY() { return currentFloorY; } set currentFloorY(value) { currentFloorY = value; }
    get currentLocation() { return currentLocation; } set currentLocation(value) { currentLocation = value; }
    get currentSpace() { return currentSpace; } set currentSpace(value) { currentSpace = value; }
    get darkChrome() { return darkChrome; }
    get dashTarget() { return dashTarget; } set dashTarget(value) { dashTarget = value; }
    get daylightMode() { return daylightMode; } set daylightMode(value) { daylightMode = value; }
    get deferredDistrictInstalls() { return deferredDistrictInstalls; } set deferredDistrictInstalls(value) { deferredDistrictInstalls = value; }
    get deletedNodeIdentifiers() { return deletedNodeIdentifiers; }
    get deletedWorldObjects() { return deletedWorldObjects; }
    get desktopAgentBots() { return desktopAgentBots; }
    get desktopAgentTextureLoader() { return desktopAgentTextureLoader; }
    get diagnosticsDrawingBuffer() { return diagnosticsDrawingBuffer; }
    get diagnosticsFrameCount() { return diagnosticsFrameCount; } set diagnosticsFrameCount(value) { diagnosticsFrameCount = value; }
    get diagnosticsFrameTimes() { return diagnosticsFrameTimes; }
    get diagnosticsFrameWorkTotalMs() { return diagnosticsFrameWorkTotalMs; } set diagnosticsFrameWorkTotalMs(value) { diagnosticsFrameWorkTotalMs = value; }
    get diagnosticsLastMovementInputMs() { return diagnosticsLastMovementInputMs; } set diagnosticsLastMovementInputMs(value) { diagnosticsLastMovementInputMs = value; }
    get diagnosticsLongestFrameMs() { return diagnosticsLongestFrameMs; } set diagnosticsLongestFrameMs(value) { diagnosticsLongestFrameMs = value; }
    get diagnosticsLongFrames() { return diagnosticsLongFrames; } set diagnosticsLongFrames(value) { diagnosticsLongFrames = value; }
    get diagnosticsPointerMoves() { return diagnosticsPointerMoves; } set diagnosticsPointerMoves(value) { diagnosticsPointerMoves = value; }
    get diagnosticsPointerWorstGapMs() { return diagnosticsPointerWorstGapMs; } set diagnosticsPointerWorstGapMs(value) { diagnosticsPointerWorstGapMs = value; }
    get diagnosticsRendererCalls() { return diagnosticsRendererCalls; } set diagnosticsRendererCalls(value) { diagnosticsRendererCalls = value; }
    get diagnosticsRendererLines() { return diagnosticsRendererLines; } set diagnosticsRendererLines(value) { diagnosticsRendererLines = value; }
    get diagnosticsRendererPoints() { return diagnosticsRendererPoints; } set diagnosticsRendererPoints(value) { diagnosticsRendererPoints = value; }
    get diagnosticsRendererTriangles() { return diagnosticsRendererTriangles; } set diagnosticsRendererTriangles(value) { diagnosticsRendererTriangles = value; }
    get diagnosticsSampleAt() { return diagnosticsSampleAt; } set diagnosticsSampleAt(value) { diagnosticsSampleAt = value; }
    get diagnosticsWorstFrameSinceLoadMs() { return diagnosticsWorstFrameSinceLoadMs; } set diagnosticsWorstFrameSinceLoadMs(value) { diagnosticsWorstFrameSinceLoadMs = value; }
    get diagnosticsWorstMovementInputMs() { return diagnosticsWorstMovementInputMs; } set diagnosticsWorstMovementInputMs(value) { diagnosticsWorstMovementInputMs = value; }
    get disabledWorldElements() { return disabledWorldElements; }
    get displayFrameIntervalMs() { return displayFrameIntervalMs; } set displayFrameIntervalMs(value) { displayFrameIntervalMs = value; }
    get disposed() { return disposed; } set disposed(value) { disposed = value; }
    get districtPackages() { return districtPackages; } set districtPackages(value) { districtPackages = value; }
    get drinkingUntil() { return officeBoardsDistrictParts.drinkingUntil; } set drinkingUntil(value) { officeBoardsDistrictParts.drinkingUntil = value; }
    get elevatorCutMaxX() { return elevatorCutMaxX; } set elevatorCutMaxX(value) { elevatorCutMaxX = value; }
    get elevatorCutMinX() { return elevatorCutMinX; } set elevatorCutMinX(value) { elevatorCutMinX = value; }
    get elevatorCutMaxZ() { return elevatorCutMaxZ; } set elevatorCutMaxZ(value) { elevatorCutMaxZ = value; }
    get emoteSprites() { return emoteSprites; }
    get enclosureRootBounds() { return enclosureRootBounds; } set enclosureRootBounds(value) { enclosureRootBounds = value; }
    get enclosureRootCenter() { return enclosureRootCenter; } set enclosureRootCenter(value) { enclosureRootCenter = value; }
    get enclosureViewCenter() { return enclosureViewCenter; } set enclosureViewCenter(value) { enclosureViewCenter = value; }
    get engineeringDebugPanel() { return engineeringDebugPanel; }
    get engineeringDebugSampleAt() { return engineeringDebugSampleAt; } set engineeringDebugSampleAt(value) { engineeringDebugSampleAt = value; }
    get enterBeachScene() { return enterBeachScene; }
    get externalTouchInteractionActive() { return externalTouchInteractionActive; } set externalTouchInteractionActive(value) { externalTouchInteractionActive = value; }
    get farSceneDetail() { return farSceneDetail; } set farSceneDetail(value) { farSceneDetail = value; }
    get fireLevel() { return membersCircleDistrictParts.fireLevel; } set fireLevel(value) { membersCircleDistrictParts.fireLevel = value; }
    get firstPersonPitch() { return firstPersonPitch; } set firstPersonPitch(value) { firstPersonPitch = value; }
    get firstPersonZoom() { return firstPersonZoom; } set firstPersonZoom(value) { firstPersonZoom = value; }
    get flightPreviousPosition() { return flightPreviousPosition; }
    get focusedRepositoryKey() { return focusedRepositoryKey; } set focusedRepositoryKey(value) { focusedRepositoryKey = value; }
    get forkbot() { return forkbot; }
    get forkbotExcitement() { return forkbotExcitement; } set forkbotExcitement(value) { forkbotExcitement = value; }
    get forkbotScreenTexture() { return forkbotScreenTexture; }
    get forkbotWanderTarget() { return forkbotWanderTarget; }
    get frameAnimated() { return frameAnimated; }
    get geometryDeduper() { return geometryDeduper; }
    get groundPlane() { return groundPlane; }
    get groundProbe() { return groundProbe; }
    get groundProbeDown() { return groundProbeDown; }
    get groundProbeOrigin() { return groundProbeOrigin; }
    get groundSurfaces() { return groundSurfaces; } set groundSurfaces(value) { groundSurfaces = value; }
    get gym() { return gym; }
    get gymPublicState() { return gymPublicState; }
    get gymState() { return gymState; }
    get handleContextLost() { return handleContextLost; }
    get handleContextRestored() { return handleContextRestored; }
    get hemisphere() { return hemisphere; }
    get installedWorldPackages() { return installedWorldPackages; } set installedWorldPackages(value) { installedWorldPackages = value; }
    get instancedMirrorSets() { return instancedMirrorSets; }
    get interactive() { return interactive; }
    get invalidateStaticMirrors() { return invalidateStaticMirrors; }
    get jetpackEquipped() { return jetpackEquipped; } set jetpackEquipped(value) { jetpackEquipped = value; }
    get jumpQueued() { return jumpQueued; } set jumpQueued(value) { jumpQueued = value; }
    get jumpVelocity() { return jumpVelocity; } set jumpVelocity(value) { jumpVelocity = value; }
    get keyboardMovementSpeed() { return keyboardMovementSpeed; } set keyboardMovementSpeed(value) { keyboardMovementSpeed = value; }
    get keys() { return keys; }
    get landmarkObjects() { return landmarkObjects; }
    get lastFrame() { return lastFrame; } set lastFrame(value) { lastFrame = value; }
    get lastMovementEmit() { return lastMovementEmit; } set lastMovementEmit(value) { lastMovementEmit = value; }
    get lastOfficeAccessPayload() { return lastOfficeAccessPayload; } set lastOfficeAccessPayload(value) { lastOfficeAccessPayload = value; }
    get lastPosition() { return lastPosition; }
    get lastVisualAnimationAt() { return lastVisualAnimationAt; } set lastVisualAnimationAt(value) { lastVisualAnimationAt = value; }
    get latestSocialBannerPayload() { return latestSocialBannerPayload; } set latestSocialBannerPayload(value) { latestSocialBannerPayload = value; }
    get leaderboardCardState() { return leaderboardCardState; }
    get leaderboardDistrict() { return leaderboardDistrict; }
    get leaderboardInterior() { return leaderboardInterior; }
    get leaveBeachScene() { return leaveBeachScene; }
    get lightLevel() { return lightLevel; } set lightLevel(value) { lightLevel = value; }
    get localBenchName() { return localBenchName; } set localBenchName(value) { localBenchName = value; }
    get localDaylightMinute() { return localDaylightMinute; } set localDaylightMinute(value) { localDaylightMinute = value; }
    get localPointLightPosition() { return localPointLightPosition; }
    get localPointLights() { return localPointLights; }
    get logoEnvironmentMap() { return logoEnvironmentMap; }
    get logoPanelChrome() { return logoPanelChrome; }
    get logoReflectionEnvironment() { return officeDisplaysDistrictParts.logoReflectionEnvironment; } set logoReflectionEnvironment(value) { officeDisplaysDistrictParts.logoReflectionEnvironment = value; }
    get logoReflectionSettleMs() { return logoReflectionSettleMs; }
    get loungeMembers() { return loungeMembers; }
    get mainlandDistanceLod() { return mainlandDistanceLod; }
    get mastodonKiosk() { return mastodonKiosk; }
    get mastodonKioskCountdown() { return mastodonKioskCountdown; } set mastodonKioskCountdown(value) { mastodonKioskCountdown = value; }
    get mastodonKioskWheelTargets() { return mastodonKioskWheelTargets; }
    get MEMBER_CLEARING_RADIUS() { return MEMBER_CLEARING_RADIUS; }
    get memberCircleInfo() { return memberCircleInfo; }
    get memberCircleInfoFace() { return memberCircleInfoFace; } set memberCircleInfoFace(value) { memberCircleInfoFace = value; }
    get memberFacts() { return memberFacts; }
    get memoryConstrainedRenderer() { return memoryConstrainedRenderer; }
    get moderationActions() { return moderationActions; }
    get moon() { return moon; }
    get movementDashDirection() { return movementDashDirection; }
    get movementEventTimer() { return movementEventTimer; } set movementEventTimer(value) { movementEventTimer = value; }
    get movementForward() { return movementForward; }
    get movementInputStartedAt() { return movementInputStartedAt; } set movementInputStartedAt(value) { movementInputStartedAt = value; }
    get movementInputState() { return movementInputState; }
    get movementInputWarningTimer() { return movementInputWarningTimer; } set movementInputWarningTimer(value) { movementInputWarningTimer = value; }
    get movementPreviousPosition() { return movementPreviousPosition; }
    get movementRight() { return movementRight; }
    get movementVector() { return movementVector; }
    get moveSpeedScale() { return moveSpeedScale; } set moveSpeedScale(value) { moveSpeedScale = value; }
    get nextAvatarHighlightAt() { return nextAvatarHighlightAt; } set nextAvatarHighlightAt(value) { nextAvatarHighlightAt = value; }
    get nextRenderFrameAt() { return nextRenderFrameAt; } set nextRenderFrameAt(value) { nextRenderFrameAt = value; }
    get nextVisualAnimationAt() { return nextVisualAnimationAt; } set nextVisualAnimationAt(value) { nextVisualAnimationAt = value; }
    get nodeInfrastructure() { return nodeInfrastructure; }
    get nodeCabinetMirror() { return nodeCabinetMirror; } set nodeCabinetMirror(value) { nodeCabinetMirror = value; }
    get nodeInterior() { return nodeInterior; }
    get OFFICE_CHAIR_SEAT_TOP_Y() { return OFFICE_CHAIR_SEAT_TOP_Y; }
    get officeAquarium() { return officeAquarium; } set officeAquarium(value) { officeAquarium = value; }
    get officeAttendanceBoard() { return officeAttendanceBoard; }
    get officeAttendanceObservedAt() { return officeAttendanceObservedAt; } set officeAttendanceObservedAt(value) { officeAttendanceObservedAt = value; }
    get officeBridge() { return officeBridge; }
    get officeBubbles() { return officeBubbles; }
    get officeCameraLocalPosition() { return officeCameraLocalPosition; }
    get officeChairs() { return officeChairs; }
    get officeChairSeat() { return officeChairSeat; } set officeChairSeat(value) { officeChairSeat = value; }
    get officeClockInBoard() { return officeClockInBoard; }
    get officeCurrentFloorId() { return officeCurrentFloorId; } set officeCurrentFloorId(value) { officeCurrentFloorId = value; }
    get officeDoorClosedX() { return officeDoorClosedX; }
    get officeDoorLocalPosition() { return officeDoorLocalPosition; }
    get officeDoorOpenX() { return officeDoorOpenX; }
    get officeDoorStatus() { return officeDoorStatus; }
    get officeDoorwayEntryPending() { return officeDoorwayEntryPending; } set officeDoorwayEntryPending(value) { officeDoorwayEntryPending = value; }
    get officeElevatorButtons() { return officeElevatorButtons; }
    get officeElevatorCameraLocked() { return officeElevatorCameraLocked; } set officeElevatorCameraLocked(value) { officeElevatorCameraLocked = value; }
    get officeElevatorCameraReleased() { return officeElevatorCameraReleased; } set officeElevatorCameraReleased(value) { officeElevatorCameraReleased = value; }
    get officeElevatorCar() { return officeElevatorCar; }
    get officeElevatorDoors() { return officeElevatorDoors; }
    get officeElevatorRide() { return officeElevatorRide; } set officeElevatorRide(value) { officeElevatorRide = value; }
    get officeExitHandler() { return officeExitHandler; } set officeExitHandler(value) { officeExitHandler = value; }
    get officeExitPending() { return officeExitPending; } set officeExitPending(value) { officeExitPending = value; }
    get officeFarProxyDirty() { return officeFarProxyDirty; } set officeFarProxyDirty(value) { officeFarProxyDirty = value; }
    get officeFarSharedTextures() { return officeFarSharedTextures; } set officeFarSharedTextures(value) { officeFarSharedTextures = value; }
    get officeFloorAccess() { return officeFloorAccess; } set officeFloorAccess(value) { officeFloorAccess = value; }
    get officeFloorGroups() { return officeFloorGroups; } set officeFloorGroups(value) { officeFloorGroups = value; }
    get officeFloorHandler() { return officeFloorHandler; } set officeFloorHandler(value) { officeFloorHandler = value; }
    get officeFloorWarpDoors() { return officeFloorWarpDoors; }
    get officeInterior() { return officeInterior; }
    get officeLandscaping() { return officeLandscaping; } set officeLandscaping(value) { officeLandscaping = value; }
    get officeLobbyPlayer() { return officeLobbyPlayer; }
    get officeLobbyPlayerMoving() { return officeLobbyPlayerMoving; } set officeLobbyPlayerMoving(value) { officeLobbyPlayerMoving = value; }
    get officeLocalParticipantId() { return officeLocalParticipantId; } set officeLocalParticipantId(value) { officeLocalParticipantId = value; }
    get officeLodWorldPosition() { return officeLodWorldPosition; }
    get officeMarketingAttendanceBoard() { return officeMarketingAttendanceBoard; }
    get officeMarketingInitiativesBoard() { return officeMarketingInitiativesBoard; }
    get officeMarketingRosterGroup() { return officeMarketingRosterGroup; }
    get officeMarketingTaskBoard() { return officeMarketingTaskBoard; }
    get officeMarketingTaskSnapshot() { return officeMarketingTaskSnapshot; } set officeMarketingTaskSnapshot(value) { officeMarketingTaskSnapshot = value; }
    get officeMovementEventTimer() { return officeMovementEventTimer; } set officeMovementEventTimer(value) { officeMovementEventTimer = value; }
    get officeParticipantLabels() { return officeParticipantLabels; }
    get officeParticipants() { return officeParticipants; }
    get officeReceptionLocalPosition() { return officeReceptionLocalPosition; }
    get officeReceptionNoah() { return officeReceptionNoah; }
    get officeRoofJumping() { return officeRoofJumping; } set officeRoofJumping(value) { officeRoofJumping = value; }
    get officeSceneMode() { return officeSceneMode; } set officeSceneMode(value) { officeSceneMode = value; }
    get officeSlidingDoorOpen() { return officeSlidingDoorOpen; } set officeSlidingDoorOpen(value) { officeSlidingDoorOpen = value; }
    get officeSlidingDoorPanels() { return officeSlidingDoorPanels; }
    get officeTableLogoMaterial() { return officeTableLogoMaterial; }
    get officeWallPositionScratch() { return officeWallPositionScratch; }
    get officeWallPreviousScratch() { return officeWallPreviousScratch; }
    get officeWarpCurrentScratch() { return officeWarpCurrentScratch; }
    get officeWarpPreviousScratch() { return officeWarpPreviousScratch; }
    get officeWasMoving() { return officeWasMoving; } set officeWasMoving(value) { officeWasMoving = value; }
    get officeZoneState() { return officeZoneState; } set officeZoneState(value) { officeZoneState = value; }
    get orgTeamActions() { return orgTeamActions; }
    get packageAccessRequested() { return packageAccessRequested; }
    get packageSpaces() { return packageSpaces; } set packageSpaces(value) { packageSpaces = value; }
    get pendingMovementEvent() { return pendingMovementEvent; } set pendingMovementEvent(value) { pendingMovementEvent = value; }
    get pendingOfficeMovementEvent() { return pendingOfficeMovementEvent; } set pendingOfficeMovementEvent(value) { pendingOfficeMovementEvent = value; }
    get pendingPackageSpawn() { return pendingPackageSpawn; } set pendingPackageSpawn(value) { pendingPackageSpawn = value; }
    get pendingTouchResize() { return pendingTouchResize; } set pendingTouchResize(value) { pendingTouchResize = value; }
    get pendingWorldPackageInstalls() { return pendingWorldPackageInstalls; } set pendingWorldPackageInstalls(value) { pendingWorldPackageInstalls = value; }
    get pinchActive() { return pinchActive; } set pinchActive(value) { pinchActive = value; }
    get pinchStartZoom() { return pinchStartZoom; } set pinchStartZoom(value) { pinchStartZoom = value; }
    get player() { return player; }
    get playerLabel() { return playerLabel; }
    get pointer() { return pointer; }
    get pointerCurrent() { return pointerCurrent; }
    get pointerLast() { return pointerLast; }
    get pointerStart() { return pointerStart; }
    get primaryPointerId() { return primaryPointerId; } set primaryPointerId(value) { primaryPointerId = value; }
    get proximityBoards() { return proximityBoards; }
    get proximityBoardWorldPosition() { return proximityBoardWorldPosition; }
    get pushSurges() { return pushSurges; }
    get qaBoardFace() { return qaBoardFace; }
    get qaBoardFrame() { return qaBoardFrame; }
    get qaBoardSnapshot() { return qaBoardSnapshot; } set qaBoardSnapshot(value) { qaBoardSnapshot = value; }
    get qaSwipeCues() { return qaSwipeCues; }
    get quadcopterFlight() { return quadcopterFlight; }
    get quadcopterPrototype() { return quadcopterPrototype; } set quadcopterPrototype(value) { quadcopterPrototype = value; }
    get quadcopterRide() { return quadcopterRide; } set quadcopterRide(value) { quadcopterRide = value; }
    get quadcopterRotorMirror() { return quadcopterRotorMirror; } set quadcopterRotorMirror(value) { quadcopterRotorMirror = value; }
    get quadcopterStates() { return quadcopterStates; } set quadcopterStates(value) { quadcopterStates = value; }
    get qualityBaseTier() { return qualityBaseTier; } set qualityBaseTier(value) { qualityBaseTier = value; }
    get qualityChangedAt() { return qualityChangedAt; } set qualityChangedAt(value) { qualityChangedAt = value; }
    get qualityProfile() { return qualityProfile; } set qualityProfile(value) { qualityProfile = value; }
    get qualitySampleFrames() { return qualitySampleFrames; } set qualitySampleFrames(value) { qualitySampleFrames = value; }
    get qualitySampleStartedAt() { return qualitySampleStartedAt; } set qualitySampleStartedAt(value) { qualitySampleStartedAt = value; }
    get raycaster() { return raycaster; }
    get reflectionCamera() { return reflectionCamera; }
    get reflectionTarget() { return reflectionTarget; }
    get relayoutBillboardCircle() { return relayoutBillboardCircle; } set relayoutBillboardCircle(value) { relayoutBillboardCircle = value; }
    get remoteDetailFrustum() { return remoteDetailFrustum; }
    get remoteDetailMatrix() { return remoteDetailMatrix; }
    get remoteDetailSphere() { return remoteDetailSphere; }
    get remoteLabels() { return remoteLabels; }
    get remotePlayers() { return remotePlayers; }
    get renderer() { return renderer; }
    get rendererSizeScratch() { return rendererSizeScratch; }
    get renderFrameIntervalMs() { return renderFrameIntervalMs; } set renderFrameIntervalMs(value) { renderFrameIntervalMs = value; }
    get renderStallWarningTimer() { return renderStallWarningTimer; } set renderStallWarningTimer(value) { renderStallWarningTimer = value; }
    get repositoryAgentTasksByRepository() { return repositoryAgentTasksByRepository; }
    get repositoryCodeLandings() { return repositoryCodeLandings; }
    get repositoryPortalBornAt() { return repositoryPortalBornAt; }
    get repositoryPortals() { return repositoryPortals; }
    get repositoryRecordBoardNearby() { return repositoryRecordBoardNearby; } set repositoryRecordBoardNearby(value) { repositoryRecordBoardNearby = value; }
    get repositoryRecordLayerPosition() { return repositoryRecordLayerPosition; }
    get repositoryRevealPending() { return repositoryRevealPending; } set repositoryRevealPending(value) { repositoryRevealPending = value; }
    get repositorySparkleUntil() { return repositorySparkleUntil; }
    get resize() { return resize; }
    get resizeFrame() { return resizeFrame; } set resizeFrame(value) { resizeFrame = value; }
    get resizeObserver() { return resizeObserver; }
    get rewardFlights() { return rewardFlights; }
    get roofParachute() { return roofParachute; } set roofParachute(value) { roofParachute = value; }
    get running() { return running; } set running(value) { running = value; }
    get scene() { return scene; }
    get sceneConstructed() { return sceneConstructed; }
    get screenLabelOrigin() { return screenLabelOrigin; }
    get screenLabelPosition() { return screenLabelPosition; }
    get seatedMovementInput() { return seatedMovementInput; }
    get selectedLandmark() { return selectedLandmark; } set selectedLandmark(value) { selectedLandmark = value; }
    get selfWorkBadgeVisible() { return selfWorkBadgeVisible; } set selfWorkBadgeVisible(value) { selfWorkBadgeVisible = value; }
    get selfWorkBoard() { return selfWorkBoard; } set selfWorkBoard(value) { selfWorkBoard = value; }
    get serveFlights() { return serveFlights; }
    get settledCuller() { return settledCuller; }
    get shaderCompileGuard() { return shaderCompileGuard; }
    get constructionCanvasPaints() { return constructionCanvasPaints; } set constructionCanvasPaints(value) { constructionCanvasPaints = value; }
    get firstViewHoldUntil() { return firstViewHoldUntil; } set firstViewHoldUntil(value) { firstViewHoldUntil = value; }
    get shadowsElementEnabled() { return shadowsElementEnabled; }
    get shadowsFarCamera() { return shadowsFarCamera; } set shadowsFarCamera(value) { shadowsFarCamera = value; }
    get skyEnvironment() { return skyEnvironment; } set skyEnvironment(value) { skyEnvironment = value; }
    get skyEnvironmentPmrem() { return skyEnvironmentPmrem; } set skyEnvironmentPmrem(value) { skyEnvironmentPmrem = value; }
    get preparedSkyEnvironment() { return preparedSkyEnvironment; } set preparedSkyEnvironment(value) { preparedSkyEnvironment = value; }
    get skyViewDirection() { return skyViewDirection; }
    get socialBanners() { return socialBanners; } set socialBanners(value) { socialBanners = value; }
    get socialBannerTimerRecords() { return socialBannerTimerRecords; } set socialBannerTimerRecords(value) { socialBannerTimerRecords = value; }
    get startHereBoard() { return startHereBoard; }
    get startHereBoardFace() { return startHereBoardFace; }
    get startHereCompleted() { return startHereCompleted; }
    get startHereStorageKey() { return startHereStorageKey; }
    get staticMirrorSets() { return staticMirrorSets; }
    get storedObjectDeletions() { return storedObjectDeletions; }
    get storeElementBackoff() { return storeElementBackoff; }
    get sun() { return sun; }
    get superJumping() { return superJumping; } set superJumping(value) { superJumping = value; }
    get SWING_ARM_HOLD_PITCH() { return SWING_ARM_HOLD_PITCH; }
    get SWING_SEAT_HALF_THICKNESS() { return SWING_SEAT_HALF_THICKNESS; }
    get swingRide() { return swingRide; } set swingRide(value) { swingRide = value; }
    get swingSeatScratch() { return swingSeatScratch; }
    get swingSet() { return swingSet; }
    get desertPlots() { return desertPlots; }
    get swingSpeedLevel() { return swingSpeedLevel; } set swingSpeedLevel(value) { swingSpeedLevel = value; }
    get swingStates() { return swingStates; }
    get systemCapacityPlatform() { return systemCapacityPlatform; }
    get touchKeys() { return touchKeys; }
    get touchMovement() { return touchMovement; }
    get touchPointers() { return touchPointers; }
    get triangleViewCameraMask() { return triangleViewCameraMask; } set triangleViewCameraMask(value) { triangleViewCameraMask = value; }
    get triangleViewEnabled() { return triangleViewEnabled; } set triangleViewEnabled(value) { triangleViewEnabled = value; }
    get triangleViewLabelsHidden() { return triangleViewLabelsHidden; } set triangleViewLabelsHidden(value) { triangleViewLabelsHidden = value; }
    get triangleViewMaterial() { return triangleViewMaterial; }
    get viewportRect() { return viewportRect; }
    get wasWalking() { return wasWalking; } set wasWalking(value) { wasWalking = value; }
    get weather() { return weather; } set weather(value) { weather = value; }
    get weatherParticleBudget() { return weatherParticleBudget; } set weatherParticleBudget(value) { weatherParticleBudget = value; }
    get world() { return world; }
    get worldBuilt() { return worldBuilt; }
    get worldBulletin() { return worldBulletin; }
    get worldElements() { return worldElements; }
    get worldGeneralChatFace() { return worldGeneralChatFace; }
    get worldPackageModules() { return worldPackageModules; } set worldPackageModules(value) { worldPackageModules = value; }
    get worldProfiler() { return worldProfiler; }
    get renderStalls() { return renderStalls; }
    get programPacer() { return programPacer; }
    get uploadBudget() { return uploadBudget; }
    get stripedUploads() { return stripedUploads; }
    get worldQaBoard() { return worldQaBoard; }
    get worldSky() { return worldSky; }
    get zipLine() { return zipLine; }
    get zipLineAccent() { return zipLineAccent; } set zipLineAccent(value) { zipLineAccent = value; }
    get zipLineEnd() { return zipLineEnd; } set zipLineEnd(value) { zipLineEnd = value; }
    get zipLineHeading() { return zipLineHeading; } set zipLineHeading(value) { zipLineHeading = value; }
    get zipLineLength() { return zipLineLength; }
    get zipLineRide() { return zipLineRide; } set zipLineRide(value) { zipLineRide = value; }
    get zipLineStart() { return zipLineStart; } set zipLineStart(value) { zipLineStart = value; }
    get zipLineSteel() { return zipLineSteel; } set zipLineSteel(value) { zipLineSteel = value; }
    get zipLineStopProgress() { return zipLineStopProgress; }
    get zipLineStrap() { return zipLineStrap; }
    get zipLineTrolley() { return zipLineTrolley; }
  })();
  Object.assign(
    fns,
    renderSetupRuntime.functions({
      THREE,
      labelLayer,
      ctx,
    }),
  );
  Object.assign(
    fns,
    worldElementsRuntime.functions({
      THREE,
      bootTrace,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    bikesDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    beachDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    quadcoptersDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    coasterDistrict.functions({
      THREE,
    }),
  );
  Object.assign(
    fns,
    billboardCirclesDistrict.functions({
      THREE,
      ctx,
    }),
  );
  Object.assign(
    fns,
    arrivalDistrict.functions({
      THREE,
      ctx,
    }),
  );
  Object.assign(
    fns,
    socialPackagesDistrict.functions({
      THREE,
      onOfficeElevatorSound,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    membersCircleDistrict.functions({
      THREE,
      ctx,
    }),
  );
  Object.assign(
    fns,
    leaderboardsDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    compactDistrictsModule.functions({
      THREE,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    agentBotsDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officeFloorsDistrict.functions({
      THREE,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officeRooftopDistrict.functions({
      THREE,
      ctx,
    }),
  );
  Object.assign(
    fns,
    officeLobbyDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officeDisplaysDistrict.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    qualityEnvironmentRuntime.functions({
      THREE,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officeNavigationRuntime.functions({
      THREE,
      onOfficeElevatorSound,
      onOfficeEnter,
      onRepositoryDistrictEnter,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    travelRuntime.functions({
      THREE,
      onLocationChange,
      onMovement,
      onOfficeProximity,
      onRegionChange,
      onWorldPackageAccessRequest,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officePresenceRuntime.functions({
      THREE,
      labelLayer,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officeExitsRuntime.functions({
      THREE,
      onMovement,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    cameraControlsRuntime.functions({
      THREE,
      onCameraMode,
      onMovement,
      onOfficeMovement,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    officeWalkingRuntime.functions({
      THREE,
      onMovement,
      onOfficeElevatorSound,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    ridesRuntime.functions({
      THREE,
      onGymState,
      onJetpackChange,
      onMovement,
      onSwingRide,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    droneRaceRuntime.functions({
      THREE,
      labelLayer,
      reducedMotion,
      ctx,
    }),
  );
  Object.assign(
    fns,
    coasterRuntime.functions({
      THREE,
      labelLayer,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    walkPlayerRuntime.functions({
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    botsRuntime.functions({
      THREE,
      identity,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    cameraRuntime.functions({
      onLocationChange,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    avatarSelectionRuntime.functions({
      THREE,
      identity,
      onAvatarWalletAction,
      onFediverseFollow,
      onFediverseProfile,
      reducedMotion,
      ctx,
    }),
  );
  Object.assign(
    fns,
    remotePlayersRuntime.functions({
      THREE,
      identity,
      labelLayer,
      onLocationChange,
      onMovement,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    networkRuntime.functions({
      THREE,
      identity,
      onFediverseProfile,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    repositoryCatalogRuntime.functions({
      THREE,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    repositoryRecordsRuntime.functions({
      THREE,
      ctx,
    }),
  );
  Object.assign(
    fns,
    effectsRuntime.functions({
      THREE,
      identity,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    pointerRuntime.functions({
      THREE,
      identity,
      onAdminGuestCopy,
      onAgentBotChat,
      onAvatarSelect,
      onCameraLook,
      onCreateRepository,
      onDrinkWater,
      onFederatedWorldTravel,
      onFediverseProfile,
      onInstanceHexagonSelect,
      onFloorTap,
      onForkbotChat,
      onLandmarkSelect,
      onLaunchMirrorSelect,
      onMastodonBoardSelect,
      onMastodonOpenLink,
      onModeration,
      onOfficeChairSelect,
      onOfficeEnter,
      onOfficeMeetingBoardSelect,
      onOfficeRooftopLaptopSelect,
      onOfficeTaskBoardSelect,
      onOfficeTaskWallAction,
      onOpenForkmeshSong,
      onOrgTeamAssign,
      onQaAction,
      onQaVerdict,
      onReferralBoardSelect,
      onRewardBoardHover,
      onSiteReferrerOpen,
      onStartHereSelect,
      onStartNodeDownload,
      onStartWorldSignSelect,
      onSystemCapacityTableSelect,
      onUnverifiedAvatarDelete,
      onWorldBulletinSelect,
      onWorldDiscordBoardSelect,
      onWorldGeneralChatSelect,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    animateRuntime.functions({
      THREE,
      reducedMotion,
      ctx,
      fns,
    }),
  );
  Object.assign(
    fns,
    diagnosticsRuntime.functions({
      THREE,
      labelLayer,
      ctx,
      fns,
    }),
  );
  const {
    addBeachCar,
    addLeaderboardCard,
    addOfficeCeilingSurface,
    addOfficeDeferredFloorDisplay,
    addOfficeFloorAtmosphere,
    addOfficeFloorSurface,
    addOfficeFunFloorProps,
    addWorldBench,
    addWorldBike,
    addWorldQuadcopter,
    addZipLineMast,
    animate,
    animateUnverifiedAvatarDeletion,
    applyRendererResolution,
    applyShadowQuality,
    applyWeatherParticleBudget,
    armMirrorPushEffect,
    noteMirrorTraffic,
    nodeMachineStats,
    baseMoveSpeed,
    beginGymExercise,
    beginOfficeExit,
    beginRepositoryDistrictReveal,
    billboardCircle,
    buildSkyEnvironment,
    clearFocus,
    completeAgentTask,
    createBeachPortal,
    createDroneRaceArrows,
    createDroneRaceCourse,
    createDroneRaceGate,
    createDroneRaceGhost,
    createWorldQuadcopterPrototype,
    createCoasterStation,
    createCoasterTrack,
    createCoasterTrain,
    placeCoasterTrain,
    deleteNetworkNode,
    deleteWorldObject,
    dismountQuadcopter,
    dismountSwing,
    dispose,
    districtWalkSurfaceContains,
    enterOffice,
    enterOfficeLobby,
    enterOfficeMeeting,
    exciteAgentBot,
    exciteForkbot,
    feedOfficeAquarium,
    focusCampfireCircle,
    focusLandmark,
    focusNetworkNode,
    focusRepositoryPortal,
    getDiagnostics,
    getEnvironmentState,
    getOfficeAquariumState,
    getSavedViewState,
    greetForkbot,
    handleDoubleClick,
    handleKeyDown,
    handleKeyUp,
    handlePointerCancel,
    handlePointerDown,
    handlePointerMove,
    handlePointerUp,
    handleWheel,
    handleWindowBlur,
    installDistrictPackage,
    installStoreElement,
    installWorldPackage,
    instanceRepeatedStaticMeshes,
    invalidateCanvasRect,
    leaveOfficeInterior,
    listDeletedWorldObjects,
    listSceneObjects,
    listWorldElementParts,
    listWorldElements,
    officeAttendanceTexture,
    officeCeilingFinishMaterial,
    officeClockInTexture,
    officeFloorFinishMaterial,
    outdoorFloorYAt,
    performGymRep,
    pickWorldObject,
    placeBillboardOnIsland,
    playEmote,
    playHandshake,
    playRewardEvent,
    prepareFirstView,
    warmFirstView,
    qualityState,
    rebuildOfficeFarProxy,
    rebuildOfficeFarProxySliced,
    registerAvatarChestControls,
    registerCompactDistrictRoot,
    registerWorldElement,
    registerWorldElementSystem,
    removeStoreElement,
    removeWorldPackage,
    rendererContextInfo,
    resetOfficeLogoEnvironment,
    restoreAllWorldObjects,
    restoreCampfireSeatIfNearby,
    restoreSavedViewState,
    restoreWorldObject,
    campfireSeatsReady,
    whenCampfireSeatsReady,
    returnToCampfireBench,
    rideQuadcopter,
    rideSwing,
    scheduleGeometryDedupe,
    scheduleGpuWarmup,
    setAgentBotAccess,
    setAvatarFaceImage,
    setAvatarFediverseProfile,
    setAvatarRecentPublicMessage,
    setBeachScene,
    setBillboardRefreshing,
    setCameraMode,
    setCameraView,
    setCameraZoom,
    setControl,
    setDaylightMode,
    setGymAuto,
    setGymWeight,
    setInputActive,
    setJetpackEquipped,
    setJetpackVertical,
    setLightLevel,
    setLocalOrgTeam,
    setMemberCircleInfo,
    setMovementTuning,
    setOfficeAccess,
    setOfficeAttendance,
    setOfficeBuildStamp,
    setOfficeDoorStatus,
    setOfficeDoorwayEntryPending,
    setOfficeExitHandler,
    setOfficeFloorHandler,
    setOfficeParticipants,
    setOfficeSeatState,
    setPaused,
    setPreviewCadence,
    setQualityTier,
    setRemotePlayerMovement,
    setRemotePlayers,
    setRepositoryImportState,
    setRepositoryIssueAgentPicker,
    setRepositoryIssuePageExpanded,
    setRepositorySizeLoading,
    setSelfWorkBadgeVisibility,
    setSelfWorkBoard,
    setSpawn,
    setSwingSpeed,
    setTheme,
    setTouchInteractionActive,
    setTouchMovement,
    setTriangleView,
    setWorldElementEnabled,
    showAgentTaskBubble,
    showChatBubble,
    showMemberChatBubble,
    showOfficeBubble,
    sitOnOfficeChair,
    startZipLineRide,
    stopGymExercise,
    summonDesktopAgentBots,
    swingRideAmplitude,
    syncEnclosureSceneVisibility,
    toggleJetpack,
    travelToOfficeFloor,
    travelToRegion,
    updateArrivalStats,
    updateBots,
    updateDesktopAgentBots,
    updateFederatedInstances,
    updateInstanceHexes,
    updateFediverseDirectory,
    updateIdentity,
    updateLeaderboards,
    updateMastodonCountdown,
    updateMastodonKiosk,
    updateMediaSpaces,
    updateMemberLounge,
    updateMirrorAgentTasks,
    updateNetworkNodes,
    updateOfficeMarketingTasks,
    updateOrganizations,
    updateQaBoard,
    updateReferralLeaderboard,
    updateRepositoryActivity,
    updateRepositoryCatalog,
    updateRepositoryGraph,
    updateRepositoryRecordDesk,
    updateRepositorySizeMap,
    updateRewardPool,
    updateSiteReferrerLeaderboard,
    updateSocialBannerTimers,
    updateSocialBanners,
    updateSystemCapacity,
    updateWorldBulletin,
    updateWorldGeneralChat,
    visitNeighborhoodHome,
    whenOfficeAquariumReady,
    worldElementOwner,
    zipLineCablePoint,
  } = fns;

  let sceneSliceStartedAt = performance.now();
  let sceneFrameStartedAt = sceneSliceStartedAt;
  async function sceneSlice(id = "", label = "") {
    if (id) bootTrace?.sceneElement?.(id, label, []);
    if (!(sceneSliceMs > 0)) return;
    const reached = performance.now();
    if (reached - sceneSliceStartedAt < sceneSliceMs) return;
    if (reached - sceneFrameStartedAt >= SCENE_BUILD_FRAME_MS) {
      await yieldSceneFrame();
      sceneFrameStartedAt = performance.now();
    } else {
      await yieldSceneBuild();
    }
    sceneSliceStartedAt = performance.now();
    bootTrace?.sceneYield?.(sceneSliceStartedAt - reached);
  }
  // Phones frequently expose a high-density screen to a comparatively small
  // GPU.  Use the same scene, but avoid allocating multisample and shadow-map
  // buffers that can make WebGL context creation fail outright on those
  // devices.  Coarse pointer is capability-based, so a small desktop window
  // keeps its full renderer.  The embedder also forces this mode when the
  // previous session in this tab crashed, so a GPU that just died is not
  // asked for the same multisample and shadow allocations again.
  const rendererPlan = planWorldRenderer({
    container,
    forceCompactRenderer,
    qualityTier,
  });
  const { bootQualityTier, compactRenderer, initialBounds, memoryConstrainedRenderer } =
    rendererPlan;
  let qualityProfile = rendererPlan.qualityProfile;
  // Set before anything paints: every plate below reads this while building
  // its backing store, and the tab is killed by their sum, not by any one.
  setCanvasTextureScale(
    compactRenderer
      ? 0.25
      : memoryConstrainedRenderer
        ? 0.5
        : 1,
  );
  // Pointer type and surface area between them still cannot tell a flagship
  // phone from a 3 GB Android, so the resolved tier gets the last word here.
  // It may lower this further; it never raises it.
  setCanvasTextureScale(
    Math.min(canvasTextureScale, qualityProfile.canvasTextureScale),
  );
  setDeferredCanvasPaints([]);
  let constructionCanvasPaints = [];
  const scene = new THREE.Scene();
  installSettledMatrixUpdate(THREE, scene);
  const settledCuller = createSettledGroupCuller(THREE, scene);
  scene.background = new THREE.Color(DAYLIGHT_ENVIRONMENT.background);
  const worldSky = createWorldSky({
    THREE,
    parent: scene,
    compact: compactRenderer,
    starCount: qualityProfile.starCount,
    maxSatellites: qualityProfile.maxSatellites,
  });

  const camera = new THREE.PerspectiveCamera(
    44,
    1,
    0.1,
    CAMERA_FAR_PLANE,
  );
  camera.position.set(...CAMERA_OFFSET);

  const preparedContext =
    preparedRenderer?.key === rendererPlan.key && !preparedRenderer.context.isContextLost?.()
      ? preparedRenderer
      : null;
  if (preparedRenderer && !preparedContext) releaseWorldRendererContext(preparedRenderer);
  let preparedSkyEnvironment = preparedContext?.sky || null;
  const renderer = preparedContext?.renderer || new THREE.WebGLRenderer({
    canvas: preparedContext?.canvas,
    context: preparedContext?.context,
    antialias: !memoryConstrainedRenderer,
    alpha: false,
    // No World material or pass uses the stencil buffer. Avoid allocating and
    // clearing an attachment that otherwise adds bandwidth to every frame.
    stencil: false,
    // A large desktop surface may have both integrated and discrete GPUs.
    // Keep requesting the discrete GPU there; compact/mobile devices use the
    // browser default because they generally have only one adapter.
    powerPreference: compactRenderer ? "default" : "high-performance",
  });
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1.08;
  renderer.shadowMap.enabled = qualityProfile.shadows;
  renderer.shadowMap.type = THREE.PCFShadowMap;
  // Checking a program reads its link log and both shader logs: three more
  // synchronous GPU round trips on every program's first draw, which queued
  // behind the shaders still compiling and cost the first frame most of a
  // second. A shader that fails still draws nothing; the embedder turns the
  // reports back on for material work.
  renderer.debug.checkShaderErrors = checkShaderErrors === true;
  // three.js asks for the anisotropy limit on the first filtered texture
  // upload, a synchronous read that otherwise waits behind the warm-up.
  renderer.capabilities.getMaxAnisotropy();
  // Only the main view defers; see installShaderCompileGuard.
  const deferrableCameras = new Set([camera]);
  const shaderCompileGuard = installShaderCompileGuard(
    renderer,
    (drawCamera, drawScene) =>
      (drawScene === scene && deferrableCameras.has(drawCamera)) || drawScene === null,
  );
  const worldProfiler = createWorldProfiler({
    renderer,
    camera,
    ownerOf: worldElementOwner,
  });
  const worldAuditState = createWorldAuditState();
  const renderStalls = createRenderStallMonitor({
    renderer,
    camera,
    ownerOf: worldElementOwner,
    isWarming: () => shaderCompileGuard.warming,
  });
  const profilerMark = worldProfiler.mark;
  worldProfiler.mark = function markRenderStallPhase(phase) {
    renderStalls.mark(phase);
    return profilerMark.call(this, phase);
  };
  const programPacer = installProgramPacer(renderer.getContext());
  const uploadBudget = createUploadBudget();
  const stripedUploads = createStripedUploads({ renderer, THREE, budget: uploadBudget });
  const unprofiledRenderBufferDirect = renderer.renderBufferDirect;
  renderer.renderBufferDirect = function profiledRenderBufferDirect(
    drawCamera,
    drawScene,
    geometry,
    material,
    object,
    group,
  ) {
    worldProfiler.noteDraw(drawCamera, drawScene, geometry, material, object, group);
    if (drawScene === scene && drawCamera === camera && programPacer.stats.paced) {
      stripedUploads.deferDrawUploads(material);
    }
    renderStalls.beginDraw(drawCamera, drawScene, material, object);
    try {
      return unprofiledRenderBufferDirect.call(
        this,
        drawCamera,
        drawScene,
        geometry,
        material,
        object,
        group,
      );
    } finally {
      renderStalls.endDraw();
    }
  };
  const triangleViewMaterial = resources.uniqueMaterial(THREE, {
    color: "#72e6ff",
    wireframe: true,
    transparent: true,
    opacity: 0.88,
    depthTest: true,
    depthWrite: true,
  }, "MeshBasicMaterial");
  let triangleViewEnabled = false;
  let triangleViewCameraMask = camera.layers.mask;
  let triangleViewLabelsHidden = labelLayer.hidden;
  // The sun and almost all shadow casters are static. Rebuilding the complete
  // atlas on every display frame nearly doubles town-square draw calls, so
  // refresh it at a bounded cadence while retaining live shadows.
  renderer.shadowMap.autoUpdate = false;
  renderer.shadowMap.needsUpdate = qualityProfile.shadows;
  renderer.domElement.className = "world-canvas";
  renderer.domElement.setAttribute("aria-hidden", "true");
  // The canvas is purely visual; leaving a tabindex (even -1) lets a pointer
  // click move focus into an aria-hidden subtree.  A later dialog close then
  // triggers the browser's hidden-focused-element accessibility warning.
  renderer.domElement.removeAttribute("tabindex");
  renderer.domElement.dataset.cameraControl = "drag";
  renderer.domElement.dataset.cameraMode = "third-person";
  renderer.domElement.dataset.dragging = "false";
  container.appendChild(renderer.domElement);
  // Read the context once now. getParameter is a synchronous round trip to
  // the GPU process; first asked for by the diagnostics sampler, it waited
  // behind the shader warm-up's queue for 200ms.
  rendererContextInfo();
  await sceneSlice("webgl-context", "WebGL context");
  const handleContextLost = (event) => {
    // preventDefault opts into WebGL's restoration path. Without it, a
    // transient mobile GPU reset can leave partially redrawn "scratchy"
    // geometry in place until the whole page is reloaded.
    event.preventDefault();
    // Three.js marks its GL state as lost, but its animation callback can
    // still run until the shell hears about this event. Stop it synchronously
    // so a dead context cannot spend the recovery window burning CPU at 0 FPS.
    running = false;
    renderer.setAnimationLoop(null);
    onRendererStateChange("lost", {
      // Chromium sometimes exposes a driver reset message here. It is empty
      // on most browsers, so the shell treats it as optional diagnostics.
      statusMessage: String(event.statusMessage || "").slice(0, 160),
    });
  };
  const handleContextRestored = () => {
    renderer.resetState();
    renderer.info.reset();
    // The environment lived in a render target the lost context took with it.
    buildSkyEnvironment();
    resetOfficeLogoEnvironment();
    renderer.shadowMap.needsUpdate = renderer.shadowMap.enabled;
    scene.traverse((object) => {
      if (!object.isMesh) return;
      const materials = Array.isArray(object.material)
        ? object.material
        : [object.material];
      materials.filter(Boolean).forEach((material) => {
        material.needsUpdate = true;
        if (material.map) material.map.needsUpdate = true;
      });
    });
    resize();
    running = true;
    lastFrame = performance.now();
    nextRenderFrameAt = 0;
    lastVisualAnimationAt = lastFrame;
    nextVisualAnimationAt = 0;
    renderer.setAnimationLoop(animate);
    onRendererStateChange("restored");
  };
  renderer.domElement.addEventListener(
    "webglcontextlost",
    handleContextLost,
  );
  renderer.domElement.addEventListener(
    "webglcontextrestored",
    handleContextRestored,
  );

  const hemisphere = new THREE.HemisphereLight("#d5fff1", "#19362d", 2.1);
  scene.add(hemisphere);
  // Distance haze in the sky's own colour: far districts and the rim of the
  // island settle into the horizon instead of ending on a hard edge. The sky
  // bodies opt out (fog: false) so the sun, moon and stars stay crisp.
  scene.fog = new THREE.Fog(
    scene.background.clone(),
    WORLD_FOG_NEAR,
    WORLD_FOG_FAR,
  );
  // A soft sky-gradient environment gives the PBR materials something to
  // reflect — glass, metal trims and screens pick up the sky instead of going
  // flat grey — while the hemisphere light gives up the matching share of
  // ambient. Phones keep the cheaper hemisphere-only shading.
  let skyEnvironment = null;
  // The generator outlives the bake. Its filter programs stay compiled, and
  // three.js's own conversion of the Office logo's live cube map (same 256
  // size) reuses them instead of compiling and drawing them in one blocking
  // frame the first time the warm-up or a capture touches the logo.
  let skyEnvironmentPmrem = null;
  buildSkyEnvironment();
  await sceneSlice("sky-environment", "Sky environment (PMREM)");
  const sun = new THREE.DirectionalLight("#fff1c4", 3.4);
  sun.position.set(-24, 35, 18);
  sun.castShadow = qualityProfile.shadows;
  // Three independent authorities decide whether the sun casts shadows: the
  // device tier, the operator's element toggle, and the far-camera LOD step.
  // Each one is recorded so that any of them changing re-derives the same
  // answer instead of overwriting what the other two decided.
  let shadowsElementEnabled = true;
  let shadowsFarCamera = false;
  // The World contains hundreds of casters. A 1024px atlas rebuilt on a timer
  // created periodic 150–600ms renderer stalls on integrated GPUs; a soft
  // 512px map with settled-input refreshes is enough for this wide light.
  sun.shadow.mapSize.set(512, 512);
  sun.shadow.camera.left = -90;
  sun.shadow.camera.right = 90;
  sun.shadow.camera.top = 90;
  sun.shadow.camera.bottom = -90;
  sun.shadow.camera.near = 1;
  sun.shadow.camera.far = 220;
  sun.shadow.bias = -0.0003;
  scene.add(sun);
  scene.add(sun.target);
  const moon = new THREE.DirectionalLight("#c8eaff", 0.82);
  moon.name = "forkmesh-world-moonlight";
  moon.position.set(24, 48, -18);
  // The sun owns the one shadow map. Moonlight is a cheap cool fill, so a
  // bright night does not double the scene's shadow-rendering cost.
  moon.castShadow = false;
  scene.add(moon);
  scene.add(moon.target);

  const world = new THREE.Group();
  scene.add(world);
  // Every walkable surface tags itself userData.ground as it is built, so the
  // floor under a visitor is one short downward ray against that small set —
  // a couple of dozen discs, rings and slabs — rather than a second copy of
  // the terrain layout that would drift out of step with the terrain itself.
  const groundProbe = new THREE.Raycaster();
  groundProbe.layers.enable(INSTANCED_SOURCE_LAYER);
  const groundProbeOrigin = new THREE.Vector3();
  const groundProbeDown = new THREE.Vector3(0, -1, 0);
  let groundSurfaces = null;
  // Districts are still being added while the World is under construction, so
  // the list is only worth keeping once the last of them is in the scene.
  let worldBuilt = false;
  // Legs, shoes and shoulder antennas for every avatar in four draw calls.
  // onBeforeRender is the one hook that runs after the renderer has refreshed
  // every world matrix and before it builds the render list, so the batches are
  // filled from settled poses and are current for the shadow pass too.
  const avatarCrowdBatches = new THREE.Group();
  avatarCrowdBatches.name = "forkmesh-avatar-crowd-batches";
  scene.add(avatarCrowdBatches);
  const avatarPartInstances = createAvatarPartInstances(THREE, avatarCrowdBatches);
  const figureBatches = createFigureBatches(THREE, {
    parent: avatarCrowdBatches,
    renderer,
    camera,
    forceFullDetail: (avatar) =>
      avatar === activeAvatarSelection ||
      avatar.userData.badgeDetail === "reading",
    gpuBusy: () =>
      programPacer.stats.paced
        ? programPacer.busy()
        : shaderCompileGuard.pendingPrograms() > 0,
  });
  const instancedMirrorSets = new Set();
  const staticMirrorSets = new Map();
  const invalidateStaticMirrors = () => {
    staticMirrorSets.forEach((mirror) => mirror.invalidate());
  };
  scene.onBeforeRender = (_renderer, _scene, renderCamera) => {
    figureBatches.restore();
    avatarPartInstances.sync();
    for (const mirror of instancedMirrorSets) mirror.sync();
    figureBatches.sync(renderCamera);
  };
  scene.onAfterRender = () => {
    figureBatches.restore();
  };
  const interactive = [];
  const animated = worldProfiler.watchCallbacks([]);
  bootTrace?.onSceneElement?.((_id, label) => worldProfiler.claimCallbacks(label));
  // Camera-coupled or player-driven motion retains display cadence. The
  // larger ambient list below is intentionally sampled less often.
  const frameAnimated = [];
  const landmarkObjects = new Map();

  // -------------------------------------------------------------------------
  // Administrator element registry. Every named piece of the World registers
  // its scene roots (or a per-frame system hook) here so the admin-only
  // Elements settings pane can pull any single one completely out of the
  // game — scene graph, raycast targets, and per-frame work — and put it
  // back live, isolating what each piece costs. The registry is local-render
  // only: it never touches presence, layout, or any shared world state.
  const worldElements = new Map();
  let officeFarProxyDirty = true;
  let officeFarSharedTextures = new Set();
  const localPointLights = new Set();
  const disabledWorldElements = new Set(
    (Array.isArray(initialDisabledElements) ? initialDisabledElements : [])
      .map((id) => String(id || "").slice(0, 64))
      .filter(Boolean),
  );
  const storeElementBackoff = createWorldBackoff();

  // -------------------------------------------------------------------------
  // Right-click deletion. An administrator (or anyone running the debug panel)
  // can aim at any single thing in the world and pull it straight out of the
  // game instead of hunting for its row in the Elements tree. A pick resolves
  // whatever sits under the cursor to the element that owns it and to an index
  // path inside that element — the exact addressing the Elements panel already
  // uses — so a deletion survives a reload and can always be undone. This is
  // local render state: nothing is sent anywhere and no other visitor's world
  // changes.
  const deletedWorldObjects = new Map();
  const storedObjectDeletions = new Set(
    (Array.isArray(initialDeletedObjects) ? initialDeletedObjects : [])
      .map((key) => String(key || "").slice(0, 160))
      .filter(Boolean),
  );

  // Whole-scene systems and the fixtures that exist before the static town
  // builds below. Everything else registers inline where it is constructed.
  registerWorldElement(
    "lighting",
    "Sun, moon & ambient light",
    "Systems",
    [hemisphere, sun, moon],
  );
  await sceneSlice();
  registerWorldElement("sky", "Sky, stars & satellites", "Systems", worldSky.group);
  await sceneSlice();
  registerWorldElementSystem("shadows", "Shadow maps", "Systems", (on) => {
    shadowsElementEnabled = on === true;
    applyShadowQuality();
  });
  await sceneSlice();
  registerWorldElementSystem("screen-labels", "Name labels", "Systems", (on) => {
    labelLayer.style.visibility = on ? "" : "hidden";
  });
  await sceneSlice();
  registerWorldElementSystem("animations", "Ambient animations", "Systems");
  await sceneSlice();
  registerWorldElementSystem("effects", "One-shot effects", "Systems");
  await sceneSlice();
  registerWorldElementSystem("chat-bubbles", "Chat bubbles & emotes", "Systems");
  await sceneSlice();

  const {
    worldBulletin,
    bulletinFrame,
    bulletinFace,
    bulletinScrollUp,
    bulletinScrollDown,
    worldGeneralChatBoard,
    worldGeneralChatFace,
    worldDiscordBoard,
  } = await townBoardsDistrict.build({
    THREE,
    interactive,
    registerWorldElement,
    sceneSlice,
    world,
  });
  await sceneSlice();

  const {
    continuousCityLand,
    desertPlots,
    mainlandDistanceLod,
  } = await townTerrainDistrict.build({
    THREE,
    interactive,
    registerWorldElement,
    sceneSlice,
    world,
  });
  await sceneSlice();

  roadsDistrict.build({
    THREE,
    registerWorldElement,
    world,
  });
  await sceneSlice();

  let bikeStates, bikeLoopLength;
  bikesDistrict.build({
    addWorldBike,
    ctx,
  });

  let beachScene, carStates;
  const {
    zipLine,
  } = await beachDistrict.build({
    THREE,
    addBeachCar,
    addWorldBench,
    createBeachPortal,
    ctx,
    registerWorldElement,
    sceneSlice,
    world,
  });

  let quadcopterStates, quadcopterPrototype, quadcopterRotorMirror;
  await quadcoptersDistrict.build({
    THREE,
    addWorldQuadcopter,
    createDroneRaceArrows,
    createDroneRaceCourse,
    createDroneRaceGate,
    createDroneRaceGhost,
    createWorldQuadcopterPrototype,
    ctx,
    instancedMirrorSets,
    registerWorldElement,
    sceneSlice,
    world,
  });
  await sceneSlice();
  let coaster = null;
  await coasterDistrict.build({
    THREE,
    createCoasterStation,
    createCoasterTrack,
    createCoasterTrain,
    ctx,
    placeCoasterTrain,
    registerWorldElement,
    sceneSlice,
    world,
  });
  await sceneSlice();

  const {
    leaderboardDistrict,
    leaderboardInterior,
  } = await leaderboardDistrictModule.build({
    THREE,
    registerWorldElement,
    sceneSlice,
    world,
  });
  await sceneSlice();

  let billboardIslandBounds, billboardCircleOrigin, billboardSpinners,
    relayoutBillboardCircle;
  billboardCirclesDistrict.build({
    THREE,
    animated,
    ctx,
    placeBillboardOnIsland,
    worldBulletin,
    worldDiscordBoard,
    worldGeneralChatBoard,
  });

  const {
    startHereStorageKey,
    startHereCompleted,
    startHereBoard,
    startHereBoardFace,
    officeBridge,
    arrivalBox,
    arrivalPlaque,
  } = await arrivalDistrict.build({
    THREE,
    animated,
    interactive,
    landmarkObjects,
    registerWorldElement,
    sceneSlice,
    world,
  });
  // District packages (islands, buildings, rides) hook into the walk surface,
  // spaces, camera, and frame loop. Kept as an array because the frame loop
  // reads it every tick.
  let districtPackages = [];
  let sceneConstructed = false;
  let pendingPackageSpawn = null;
  let lastOfficeAccessPayload = null;
  let latestSocialBannerPayload = null;
  let mastodonKioskCountdown = {
    remainingMs: MASTODON_KIOSK_REFRESH_MS,
    totalMs: MASTODON_KIOSK_REFRESH_MS,
    loading: false,
  };

  let officeLandscaping, socialBanners, socialBannerTimerRecords,
    worldPackageModules, installedWorldPackages, pendingWorldPackageInstalls,
    packageSpaces, deferredDistrictInstalls;
  const {
    officeHighFidelity,
    mastodonKiosk,
    mastodonKioskWheelTargets,
  } = await socialPackagesDistrict.build({
    THREE,
    ctx,
    initialInstalledWorldPackages,
    initialRemovedWorldPackages,
    installWorldPackage,
    interactive,
    landmarkObjects,
    placeBillboardOnIsland,
    registerWorldElement,
    sceneSlice,
    world,
  });

  let memberCircleInfoFace;
  const membersCircleDistrictParts = await membersCircleDistrict.build({
    THREE,
    animated,
    ctx,
    interactive,
    landmarkObjects,
    registerWorldElement,
    sceneSlice,
    setMemberCircleInfo,
    world,
  });
  const {
    campfire,
    campfireGround,
    MEMBER_CLEARING_RADIUS,
    CAMPFIRE_SEAT_HALF_THICKNESS,
    CAMPFIRE_SEAT_TOP_Y,
    CAMPFIRE_SEAT_Y,
    CAMPFIRE_BENCH_LEG_HEIGHT,
    memberCircleInfo,
  } = membersCircleDistrictParts;

  const {
    SWING_SEAT_HALF_THICKNESS,
    SWING_ARM_HOLD_PITCH,
    swingSet,
    swingStates,
    gym,
    gymState,
    gymPublicState,
  } = await recreationDistrict.build({
    THREE,
    ctx,
    frameAnimated,
    instancedMirrorSets,
    interactive,
    onGymState,
    reducedMotion,
    registerWorldElement,
    sceneSlice,
    swingRideAmplitude,
    world,
  });
  await sceneSlice();

  const {
    leaderboardCardState,
  } = leaderboardsDistrict.build({
    THREE,
    addLeaderboardCard,
  });
  const nodeDistrict = new THREE.Group();
  nodeDistrict.name = "forkmesh-node-district";
  let nodeCabinetMirror = null;
  const nodeInterior = new THREE.Group();
  nodeInterior.name = "forkmesh-node-interior";
  nodeInterior.userData.nodeInterior = true;
  const fountainLandmark = landmarkObjects.get("fountain");
  if (fountainLandmark) nodeInterior.add(fountainLandmark);
  nodeDistrict.add(nodeInterior);
  nodeDistrict.userData.nodeInterior = nodeInterior;
  world.add(nodeDistrict);
  registerWorldElement(
    "node-district", "Mirror node yard", "Districts", nodeDistrict,
  );
  await sceneSlice();
  const systemCapacityPlatform = createSystemCapacityPlatform(THREE);
  await sceneSlice("system-capacity", "System capacity platform");
  const player = createAvatar(THREE, identity, {
    instances: avatarPartInstances,
  });
  const requestedSpawnSpace = String(initialSpawn?.space || "");
  // A position saved inside a district package parks until that package has
  // registered its spaces (it installs after construction, or never); the
  // avatar starts in Town rather than over an empty ocean.
  if (packageSpace(requestedSpawnSpace)) {
    pendingPackageSpawn = { ...initialSpawn };
  }
  const initialSpawnSpace = pendingPackageSpawn ? "town-square" : requestedSpawnSpace;
  const initialSpawnRadius = worldRadiusForSpace(initialSpawnSpace);
  const initialSpawnX =
    !pendingPackageSpawn && Number.isFinite(Number(initialSpawn?.x))
      ? clamp(Number(initialSpawn.x), -initialSpawnRadius, initialSpawnRadius)
      : -8.1;
  const initialSpawnZ =
    !pendingPackageSpawn && Number.isFinite(Number(initialSpawn?.z))
      ? clamp(Number(initialSpawn.z), -initialSpawnRadius, initialSpawnRadius)
      : 30;
  const initialSpawnFloor = officeCampusSpace(initialSpawnSpace)
    ? WORLD_SPACE_FLOORS[initialSpawnSpace]
    : outdoorFloorYAt(initialSpawnX, initialSpawnZ);
  let currentSpace = Object.hasOwn(WORLD_SPACE_FLOORS, initialSpawnSpace)
    ? initialSpawnSpace
    : "town-square";
  let currentFloorY = initialSpawnFloor;
  player.position.set(initialSpawnX, initialSpawnFloor, initialSpawnZ);
  player.rotation.y = Number.isFinite(Number(initialSpawn?.heading))
    ? clamp(Number(initialSpawn.heading), -Math.PI, Math.PI)
    : Math.PI;
  world.add(player);
  registerWorldElement("player-avatar", "Your avatar", "Avatars & bots", player);
  await sceneSlice();
  registerWorldElement(
    "avatar-crowd-batches",
    "Avatar crowd batches",
    "Avatars & bots",
    avatarCrowdBatches,
  );
  await sceneSlice();
  let officeSceneMode = "town";
  let beachSceneActive = false;
  // "" → "raising" → "sealed" → "dissolving" → "". Only "sealed" hides the rest
  // of the World: the shell has to be opaque and closed before the triangles
  // behind it are allowed to disappear, and the outside comes back the instant
  // the visitor steps through the door rather than after the dissolve plays.

  let compactDistricts, compactTextureSlots, enclosureViewCenter,
    enclosureRootBounds, enclosureRootCenter;
  const {
    activeWalkSurfaceContains,
  } = compactDistrictsModule.build({
    THREE,
    beachScene,
    campfire,
    ctx,
    districtWalkSurfaceContains,
    landmarkObjects,
    leaderboardDistrict,
    officeHighFidelity,
    registerCompactDistrictRoot,
    startHereBoard,
    syncEnclosureSceneVisibility,
    world,
  });
  // The camera used to start at CAMERA_OFFSET relative to the world origin
  // even though the avatar starts elsewhere. It then spent the first visible
  // second easing across the map, which made the first movement input feel
  // delayed. Start in the final chase pose before the first frame is painted.
  camera.position.set(
    player.position.x + CAMERA_OFFSET[0],
    player.position.y + FIRST_PERSON_EYE_HEIGHT + CAMERA_OFFSET[1],
    player.position.z + CAMERA_OFFSET[2],
  );
  const playerLabel = makePlayerLabel(player, labelLayer);

  const {
    forkbot,
    forkbotScreenTexture,
  } = agentBotsDistrict.build({
    THREE,
    animated,
    ctx,
    interactive,
    registerWorldElement,
    world,
  });
  await sceneSlice();

  // Organization coding agents use their own fixed identities. They wander
  // like ForkBot, but their task execution stays on an authorized headless
  // mirror; these avatars are only a visible chat/status surface.
  const agentBots = new Map();
  const agentBotStates = new Map();
  const desktopAgentBots = new Map();
  const desktopAgentTextureLoader = new THREE.TextureLoader();
  let agentBotAccessAllowed = false;

  const remotePlayers = new Map();
  const remoteLabels = new Map();
  // A successful administrative deletion must win over the next stale
  // presence/directory frame while the edge cache catches up.
  const administrativelyRemovedAccounts = new Set();
  const moderationActions = new WeakMap();
  const adminGuestCopyActions = new WeakMap();
  const orgTeamActions = new WeakMap();
  const avatarSelections = new WeakMap();
  const avatarSelectionBounds = new THREE.Box3();
  const badgeDetailWorldPosition = new THREE.Vector3();
  const avatarSelectionHighlight = new THREE.Box3Helper(
    avatarSelectionBounds,
    "#77d9ff",
  );
  avatarSelectionHighlight.name = "forkmesh-avatar-selection-highlight";
  avatarSelectionHighlight.visible = false;
  avatarSelectionHighlight.renderOrder = 41;
  avatarSelectionHighlight.material.transparent = true;
  avatarSelectionHighlight.material.opacity = 0.42;
  avatarSelectionHighlight.material.depthTest = false;
  avatarSelectionHighlight.material.depthWrite = false;
  scene.add(avatarSelectionHighlight);
  let activeAvatarSelection = null;
  // Badge plane and tab buttons -> the avatar they belong to, so one click
  // handler can switch chest tabs and hit the follow pill.
  const chestControls = new WeakMap();
  registerAvatarChestControls(player, identity.id);
  const officeParticipants = new Map();
  const officeParticipantLabels = new Map();
  const officeBubbles = new Map();
  const officeChairs = new Map();
  let officeMarketingTaskSnapshot = normalizeOfficeMarketingTasks({
    authorized: false,
    state: "locked",
  });
  let engineeringDebugSampleAt = performance.now();

  let officeFloorGroups, elevatorCutMinX, elevatorCutMaxX, elevatorCutMaxZ,
    officeAquarium, aquariumPanel;
  const {
    officeInterior,
    officeLodWorldPosition,
    officeFloorAccent,
    aquariumControlLocalPosition,
    engineeringDebugPanel,
  } = await officeFloorsDistrict.build({
    THREE,
    addOfficeCeilingSurface,
    addOfficeDeferredFloorDisplay,
    addOfficeFloorAtmosphere,
    addOfficeFloorSurface,
    addOfficeFunFloorProps,
    ctx,
    interactive,
    labelLayer,
    landmarkObjects,
    officeCeilingFinishMaterial,
    officeFloorFinishMaterial,
    registerCompactDistrictRoot,
    registerWorldElement,
    sceneSlice,
    systemCapacityPlatform,
    world,
  });
  await sceneSlice("office-fun-floor", "Office fun-floor props");

  let zipLineStart, zipLineEnd, zipLineHeading, zipLineSteel, zipLineAccent;
  const {
    zipLineLength,
    zipLineStopProgress,
    zipLineTrolley,
    zipLineStrap,
  } = officeRooftopDistrict.build({
    THREE,
    addZipLineMast,
    beachScene,
    ctx,
    interactive,
    officeFloorGroups,
    officeInterior,
    registerWorldElement,
    world,
    zipLine,
    zipLineCablePoint,
  });
  await sceneSlice();
  let officeSlidingDoorOpen = 0;

  const {
    officeSlidingDoorPanels,
    officeDoorStatus,
    officeDoorClosedX,
    officeDoorOpenX,
    officeTableLogoMaterial,
    OFFICE_CHAIR_SEAT_TOP_Y,
    officeMarketingTaskBoard,
    officeMarketingInitiativesBoard,
    officeMarketingRosterGroup,
    officeMarketingAttendanceBoard,
    officeLobbyPlayer,
    officeReceptionNoah,
    officeFloorWarpDoors,
  } = officeLobbyDistrict.build({
    THREE,
    addOfficeDeferredFloorDisplay,
    avatarPartInstances,
    identity,
    interactive,
    landmarkObjects,
    officeChairs,
    officeInterior,
    registerAvatarChestControls,
  });

  // An open-sided garden tent makes instance creation feel like a welcoming
  // fair booth. Credentials remain in the device-local UI.
  await sceneSlice("office-marketing", "Office doors & marketing floor");
  const officeBoardsDistrictParts = await officeBoardsDistrict.build({
    THREE,
    animated,
    ctx,
    interactive,
    officeFloorAccent,
    officeInterior,
    placeBillboardOnIsland,
    registerCompactDistrictRoot,
    registerWorldElement,
    sceneSlice,
    world,
  });
  const {
    worldQaBoard,
    qaBoardFrame,
    qaBoardFace,
    qaSwipeCues,
  } = officeBoardsDistrictParts;

  let qaBoardSnapshot = { view: "cards", list: [] };
  let officeLobbyPlayerMoving = false;

  const officeDisplaysDistrictParts = await officeDisplaysDistrict.build({
    THREE,
    animated,
    billboardCircle,
    compactRenderer,
    ctx,
    interactive,
    officeAttendanceTexture,
    officeClockInTexture,
    officeFloorAccent,
    officeInterior,
    onLeaderboardCircleAway,
    onLeaderboardCircleNearby,
    onQaBoardAway,
    onQaBoardNearby,
    onSocialCircleAway,
    onSocialCircleNearby,
    sceneSlice,
    worldQaBoard,
  });
  const {
    proximityBoards,
    officeAttendanceBoard,
    officeClockInBoard,
    reflectionTarget,
    reflectionCamera,
    logoEnvironmentMap,
    chrome,
    darkChrome,
    logoPanelChrome,
    chromeCube,
    logoReflectionSettleMs,
    officeElevatorButtons,
    officeElevatorCar,
    officeElevatorDoors,
  } = officeDisplaysDistrictParts;
  reflectionCamera.children.forEach((face) => deferrableCameras.add(face));
  await sceneSlice("office-elevator", "Office glass elevator");
  const nodeInfrastructure = new Map();
  const deletedNodeIdentifiers = new Set();
  const repositoryAgentTasksByRepository = new Map();
  const botAgents = new Map();
  const loungeMembers = new Map();
  // Public account facts the member directory publishes but a live presence
  // frame never carries — joined date and total active time — keyed by
  // lowercased display name and refreshed by updateMemberLounge. Every avatar
  // drawn for a signed-in account wears them, so nobody's chest badge shows
  // less than their campfire bench figure does.
  const memberFacts = new Map();
  const repositoryPortals = new Map();
  const repositoryPortalBornAt = new Map();
  // Sparkle deadline per portal key. A portal that is still waiting for its
  // size tree shimmers until this timestamp; the map landing rebuilds the
  // layer without a sparkle, and a map that never arrives simply fades out.
  const repositorySparkleUntil = new Map();
  // Set by the shell the moment it starts the deferred catalog read, so the
  // first portals to arrive rise out of the ground instead of appearing.
  let repositoryRevealPending = false;
  const emoteSprites = [];
  const rewardFlights = [];
  const pushSurges = [];
  const repositoryCodeLandings = [];
  const serveFlights = [];
  const forkbotWanderTarget = new THREE.Vector3(...FORKBOT_HOME);
  let forkbotExcitement = null;
  const keys = new Set();
  const touchKeys = new Set();
  const touchMovement = new THREE.Vector2();
  const touchPointers = new Map();
  let externalTouchInteractionActive = false;
  let pendingTouchResize = false;
  const raycaster = new THREE.Raycaster();
  raycaster.layers.enable(INSTANCED_SOURCE_LAYER);
  const groundPlane = new THREE.Plane(new THREE.Vector3(0, 1, 0), 0);
  const pointer = new THREE.Vector2();
  const pointerStart = new THREE.Vector2();
  const pointerLast = new THREE.Vector2();
  const pointerCurrent = new THREE.Vector2();
  // Hot-path scratch values. Reusing these avoids producing several short-
  // lived vectors on every movement and label frame, which otherwise turns
  // into intermittent garbage-collection pauses while walking.
  const movementVector = new THREE.Vector3();
  const movementForward = new THREE.Vector3();
  const movementRight = new THREE.Vector3();
  const movementPreviousPosition = new THREE.Vector3();
  const movementDashDirection = new THREE.Vector3();
  const bikeSeatPosition = new THREE.Vector3();
  const quadcopterFlight = new THREE.Vector3();
  const flightPreviousPosition = new THREE.Vector3();
  const screenLabelPosition = new THREE.Vector3();
  const screenLabelOrigin = new THREE.Vector3();
  const seatedMovementVector = new THREE.Vector3();
  const movementInputState = {
    keyboardActive: false,
    bikeDirection: 0,
    bikeSteer: 0,
    ridingBike: false,
    ridingCar: false,
    sprinting: false,
    touchStrength: 0,
    inputStrength: 0,
    movement: movementVector,
  };
  const seatedMovementInput = {
    keyboardActive: false,
    bikeDirection: 0,
    bikeSteer: 0,
    ridingBike: false,
    ridingCar: false,
    sprinting: false,
    touchStrength: 0,
    inputStrength: 0,
    movement: seatedMovementVector,
  };
  const cameraEye = new THREE.Vector3();
  const cameraTarget = new THREE.Vector3();
  const cameraDirection = new THREE.Vector3();
  const cameraLookTarget = new THREE.Vector3();
  const cameraOffsetDirection = new THREE.Vector3();
  // Sampled after the camera update each frame so the sky layer can drop its
  // stars, planets, and satellites whenever the view is not tilted upward.
  const skyViewDirection = new THREE.Vector3();
  const cameraDesired = new THREE.Vector3();
  const cameraLocalTarget = new THREE.Vector3();
  const cameraLocalDesired = new THREE.Vector3();
  const cameraLocalPosition = new THREE.Vector3();
  const cameraLimitLocalTarget = new THREE.Vector3();
  const officeCameraLocalPosition = new THREE.Vector3();
  const diagnosticsDrawingBuffer = new THREE.Vector2();
  const officeDoorLocalPosition = new THREE.Vector3();
  const officeReceptionLocalPosition = new THREE.Vector3();
  const proximityBoardWorldPosition = new THREE.Vector3();
  const localPointLightPosition = new THREE.Vector3();
  scene.traverse((object) => {
    if (object.isPointLight) localPointLights.add(object);
  });
  const viewportRect = { width: 1, height: 1 };
  let canvasRectStale = true;
  let currentLocation = "Town Square";
  // Package ids whose access hydration was already requested this visit.
  const packageAccessRequested = new Set();
  let running = true;
  let firstViewHoldUntil = 0;
  let disposed = false;
  let lastFrame = performance.now();
  let nextRenderFrameAt = 0;
  let displayFrameIntervalMs = RENDER_FRAME_INTERVAL_MS;
  let renderFrameIntervalMs = RENDER_FRAME_INTERVAL_MS;
  let lastVisualAnimationAt = lastFrame;
  let nextVisualAnimationAt = 0;
  let diagnosticsSampleAt = lastFrame;
  let diagnosticsFrameCount = 0;
  let diagnosticsRendererCalls = 0;
  let diagnosticsRendererTriangles = 0;
  let diagnosticsRendererLines = 0;
  let diagnosticsRendererPoints = 0;
  let diagnosticsLongestFrameMs = 0;
  let diagnosticsLongFrames = 0;
  // Raw per-frame durations for the current one-second sample so the debug
  // panel can report a p95 alongside the average. Bounded: the buffer clears
  // on every diagnostics read and truncates itself when nothing is polling.
  const diagnosticsFrameTimes = [];
  let diagnosticsFrameWorkTotalMs = 0;
  let diagnosticsWorstFrameSinceLoadMs = 0;
  let renderStallWarningTimer = 0;
  let diagnosticsPointerMoves = 0;
  let diagnosticsPointerWorstGapMs = 0;
  let diagnosticsLastMovementInputMs = 0;
  let diagnosticsWorstMovementInputMs = 0;
  let movementInputStartedAt = 0;
  let movementInputWarningTimer = 0;
  let lastMovementEmit = 0;
  let movementEventTimer = 0;
  let pendingMovementEvent = null;
  let officeMovementEventTimer = 0;
  let pendingOfficeMovementEvent = null;
  let lastPosition = player.position.clone();
  let wasWalking = false;
  let cameraFocus = null;
  let cameraSnapPending = false;
  let officeZoneState = "distant";
  let focusedRepositoryKey = "";
  let repositoryRecordBoardNearby = false;
  const repositoryRecordLayerPosition = new THREE.Vector3();
  let cameraZoom = 1;
  let firstPersonZoom = 1;
  let cameraYaw = Math.atan2(CAMERA_OFFSET[0], CAMERA_OFFSET[2]);
  let cameraPitch = Math.asin(CAMERA_OFFSET[1] / CAMERA_DISTANCE);
  let firstPersonPitch = 0;
  let cameraMode = "third-person";
  let jumpVelocity = 0;
  let superJumping = false;
  let jumpQueued = false;
  let officeRoofJumping = false;
  let roofParachute = null;
  let zipLineRide = null;
  // Per-device movement tuning scales the shared speed. Keyboard movement uses a
  // blend state so holding direction ramps to the selected top speed.
  let moveSpeedScale = 1;
  let keyboardMovementSpeed = PLAYER_SPEED;
  let dashTarget = null;
  // Set while the player is sitting on a campfire bench: the seat pose is held
  // every frame until the visitor walks, dashes or jumps away from it.
  let benchSeat = null;
  // The account name currently sitting on its own named bench in the Members
  // Circle. Presence marks that account "away" from the directory's point of
  // view, so without this the plank under the visitor would read OUT AND
  // ABOUT while they are plainly sitting on it.
  let localBenchName = "";
  // Set while the player is riding a swing: the avatar is glued to the moving
  // seat every frame until a second click — or any movement input — hops off.
  let swingRide = null;
  // A bike stays under the local avatar and uses the normal movement,
  // collision, camera, and presence loop; clicking the same bike dismounts.
  let bikeRide = null;
  // The beach car uses the same immediate input path as walking and cycling;
  // only the top-speed multiplier and seated vehicle pose differ.
  let carRide = null;
  // The quadcopter owns all three movement axes while mounted. Its position
  // remains local scene state; the ordinary bounded presence frame carries
  // only the rider position and activity, just like the bike and car.
  let quadcopterRide = null;
  let coasterRide = null;
  // The jetpack is wearable rather than a mount. It leaves the avatar visible,
  // follows the normal camera, and unlocks camera-relative horizontal flight
  // plus explicit ascent/descent controls.
  let jetpackEquipped = false;
  // 0..1 from the shell's swing-speed slider; maps onto the pendulum amplitude.
  let swingSpeedLevel = 0.55;
  let primaryPointerId = null;
  let pinchStartZoom = cameraZoom;
  let pinchActive = false;
  let lightLevel = LIGHT_LEVEL_DEFAULT;
  let daylightMode = "auto";
  let localDaylightMinute = -1;
  let farSceneDetail = null;
  let nextAvatarHighlightAt = 0;
  let officeCurrentFloorId = "lobby";
  let officeChairSeat = null;
  let officeFloorAccess = normalizeOfficeFloorAccess();
  let officeFloorHandler = null;
  let officeElevatorRide = null;
  let officeElevatorCameraLocked = false;
  let officeElevatorCameraReleased = false;
  let officeAttendanceObservedAt = performance.now();
  let selfWorkBoard = normalizeSelfWorkBoard({});
  let selfWorkBadgeVisible = true;
  // main dropped its own selectedLandmark when the floating landmark labels went
  // away (adhoc #243); the Office still tracks it to frame the camera on entry.
  let selectedLandmark = "";
  let officeLocalParticipantId = "";
  let officeWasMoving = false;
  let officeExitPending = false;
  let officeDoorwayEntryPending = false;
  let officeExitHandler = null;
  await sceneSlice("scene-state", "Scene state & light scan");
  let weather = null;
  let weatherParticleBudget = WEATHER_PARTICLE_COUNT;
  registerWorldElement("weather", "Weather (rain & snow)", "Systems", []);
  await sceneSlice();
  // The buffer is allocated at the largest population any tier draws; the boot
  // tier's own share of it is applied before the first animated frame.
  applyWeatherParticleBudget(qualityProfile.weatherParticles);
  let aerialLandmarkMarkers = null;
  let aerialLandmarkMarkersUnavailable = false;

  // ── Adaptive quality ───────────────────────────────────────────────────
  // Detection at boot is a guess from cheap signals; the frame rate is the
  // measurement. This ladder reconciles the two, stepping the live knobs down
  // when a sample window presents well under the cadence the loop asked for
  // and back up once the device has held a comfortable margin for a while.
  let adaptiveQualityEnabled = adaptiveQuality !== false;
  // The tier the ladder measures against. Detection sets it; a manual pin
  // rebases it, so QA holding the world at mobile-low is never dragged back
  // up to the tier this machine would have detected.
  let qualityBaseTier = bootQualityTier;
  let qualityChangedAt = performance.now();
  let qualitySampleStartedAt = 0;
  let qualitySampleFrames = 0;

  const rendererSizeScratch = new THREE.Vector2();

  const officeWarpCurrentScratch = new THREE.Vector3();
  const officeWarpPreviousScratch = new THREE.Vector3();
  const officeWallPositionScratch = new THREE.Vector3();
  const officeWallPreviousScratch = new THREE.Vector3();

  const enterBeachScene = () => setBeachScene(true);
  const leaveBeachScene = () => setBeachScene(false);
  const swingSeatScratch = new THREE.Vector3();
  const remoteDetailFrustum = new THREE.Frustum();
  const remoteDetailMatrix = new THREE.Matrix4();
  const remoteDetailSphere = new THREE.Sphere(new THREE.Vector3(), 2.4);

  renderer.domElement.addEventListener("pointerdown", handlePointerDown);
  renderer.domElement.addEventListener("dblclick", handleDoubleClick);
  renderer.domElement.addEventListener("pointermove", handlePointerMove, {
    passive: false,
  });
  renderer.domElement.addEventListener("wheel", handleWheel, {
    passive: false,
  });
  window.addEventListener("pointerup", handlePointerUp);
  window.addEventListener("pointercancel", handlePointerCancel);
  window.addEventListener("keydown", handleKeyDown);
  window.addEventListener("keyup", handleKeyUp);
  window.addEventListener("blur", handleWindowBlur);

  let resizeFrame = 0;
  let observedCanvasSize = null;
  let canvasSized = false;
  const resize = (entries) => {
    const box = Array.isArray(entries)
      ? entries[entries.length - 1]?.contentRect
      : null;
    if (box) observedCanvasSize = { width: box.width, height: box.height };
    canvasRectStale = true;
    if (resizeFrame || disposed || !canvasSized) return;
    // Mobile browser chrome and visualViewport can resize the canvas wrapper
    // while a finger pans, tilts, or pinches. Reallocating the WebGL drawing
    // buffer mid-gesture clears it and looks exactly like a full page refresh.
    // Keep the current frame and apply one settled resize after the final
    // touch pointer is released.
    if (touchPointers.size > 0 || externalTouchInteractionActive) {
      pendingTouchResize = true;
      return;
    }
    resizeFrame = window.requestAnimationFrame(applyCanvasSize);
  };
  let resizeDeferredSince = 0;
  const applyCanvasSize = () => {
    resizeFrame = 0;
    if (disposed) return;
    const compiling = programPacer.stats.paced
      ? programPacer.busy()
      : shaderCompileGuard.pendingPrograms() > 0;
    if (renderer.info.render.frame > 0 && compiling) {
      const now = performance.now();
      resizeDeferredSince ||= now;
      if (now - resizeDeferredSince < CANVAS_RESIZE_COMPILE_WAIT_MS) {
        resizeFrame = window.requestAnimationFrame(applyCanvasSize);
        return;
      }
    }
    resizeDeferredSince = 0;
    canvasRectStale = true;
    if (!observedCanvasSize) {
      const measured = container.getBoundingClientRect();
      observedCanvasSize = { width: measured.width, height: measured.height };
    }
    const rect = observedCanvasSize;
    const width = Math.max(1, Math.floor(rect.width));
    const height = Math.max(1, Math.floor(rect.height));
    if (width === viewportRect.width && height === viewportRect.height) {
      return;
    }
    viewportRect.width = width;
    viewportRect.height = height;
    programPacer.hold();
    // Bound the real buffer area as well as DPR. A 1.35 ratio is modest on a
    // laptop but still asks a 4K window to shade more than 15 million samples
    // once colour and depth are counted. Resolution scaling keeps that cost
    // stable across displays while DOM labels remain native-resolution.
    applyRendererResolution();
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    // Do not clear and repaint the drawing buffer during a live touch pan.
    // The animation loop already owns that frame; one settled resize is
    // enough after the browser chrome and shell height stop moving.
    if (
      running &&
      !disposed &&
      !(firstViewHoldUntil > performance.now()) &&
      renderer.info.render.frame > 0 &&
      primaryPointerId === null &&
      touchPointers.size === 0 &&
      shaderCompileGuard.pendingPrograms() === 0
    ) {
      renderer.render(scene, camera);
    }
  };
  // ResizeObserver is unavailable in older mobile WebViews.  A window resize
  // listener still gives those browsers a correctly sized, working World.
  const resizeObserver =
    typeof ResizeObserver === "function" ? new ResizeObserver(resize) : null;
  resizeObserver?.observe(container);
  window.addEventListener("resize", resize, { passive: true });
  window.addEventListener("scroll", invalidateCanvasRect, {
    capture: true,
    passive: true,
  });
  window.addEventListener("orientationchange", invalidateCanvasRect, {
    passive: true,
  });
  window.visualViewport?.addEventListener("scroll", invalidateCanvasRect, {
    passive: true,
  });
  if (initialBounds.width > 0 && initialBounds.height > 0) {
    observedCanvasSize = {
      width: initialBounds.width,
      height: initialBounds.height,
    };
  }
  const geometryDeduper = createGeometryDeduper(THREE);

  await sceneSlice("scene-systems", "Scene systems & input");
  setTheme("world");
  // Every district is in the scene now, so the walkable surfaces the ground
  // probe reads are complete and worth holding on to.
  worldBuilt = true;
  groundSurfaces = null;
  player.position.y = officeCampusSpace(initialSpawnSpace)
    ? player.position.y
    : outdoorFloorYAt(player.position.x, player.position.z);
  // District packages wait for the avatar and the built world: install the
  // ones queued during construction, then let every district borrow from the
  // finished Office interior.
  sceneConstructed = true;
  deferredDistrictInstalls
    .splice(0)
    .forEach(({ packageId, entry, module }) =>
      installDistrictPackage(packageId, entry, module),
    );
  for (let index = 0; index < districtPackages.length; index += 1) {
    districtPackages[index].instance.onWorldReady?.({ officeInterior });
  }
  currentFloorY = player.position.y;
  lastPosition.copy(player.position);
  camera.lookAt(player.position);
  await sceneSlice("district-packages", "District packages & spawn");
  instanceRepeatedStaticMeshes();
  await sceneSlice("instanced-statics", "Instanced repeated meshes");
  await rebuildOfficeFarProxySliced(sceneSlice);
  await sceneSlice("office-far-proxy", "Office far proxy");
  canvasSized = true;
  constructionCanvasPaints = deferredCanvasPaints || [];
  setDeferredCanvasPaints(null);
  firstViewHoldUntil = performance.now() + FIRST_VIEW_HOLD_MS;
  applyCanvasSize();
  await sceneSlice("first-view-warm", "First view shader warm-up");
  warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS });
  for (
    let pass = 1;
    shaderCompileGuard.warmComplete === false && pass < FIRST_VIEW_WARM_PASSES;
    pass += 1
  ) {
    await sceneSlice(
      `first-view-warm-${pass + 1}`,
      `First view shader warm-up · pass ${pass + 1}`,
    );
    warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS, resume: true, shadows: false });
  }
  renderer.setAnimationLoop(animate);
  scheduleGeometryDedupe();
  scheduleGpuWarmup();

  return {
    camera,
    scene,
    player,
    renderer,
    focusLandmark,
    enterBeachScene,
    leaveBeachScene,
    isBeachScene: () => beachSceneActive,
    rideZipLine: startZipLineRide,
    isRidingZipLine: () => Boolean(zipLineRide),
    enterOffice,
    enterOfficeLobby,
    enterOfficeMeeting,
    beginOfficeExit,
    setOfficeExitHandler,
    setOfficeDoorwayEntryPending,
    setOfficeDoorStatus,
    setOfficeFloorHandler,
    setOfficeAccess,
    setOfficeAttendance,
    setSelfWorkBoard,
    setSelfWorkBadgeVisibility,
    travelToOfficeFloor,
    sitOnOfficeChair,
    feedOfficeAquarium,
    getOfficeAquariumState,
    whenOfficeAquariumReady,
    getWorldPackageState: (id) =>
      installedWorldPackages.get(String(id || ""))?.instance?.state?.() ??
      null,
    getOfficeTransportState: () => ({ riding: false, space: currentSpace }),
    setOfficeParticipants,
    updateOfficeMarketingTasks,
    isOfficeInterior: () => officeSceneMode !== "town",
    updateWorldBulletin,
    updateWorldGeneralChat,
    updateSatelliteSky: (snapshot, sgp4Engine) =>
      worldSky.update(snapshot, sgp4Engine),
    updateMastodonKiosk,
    updateMastodonCountdown,
    updateSocialBanners,
    updateSocialBannerTimers,
    installWorldPackage,
    removeWorldPackage,
    installedWorldPackageIds: () => [...installedWorldPackages.keys()],
    worldPackageCatalog: () =>
      [...worldPackageModules.values()].map((entry) => ({
        id: String(entry.manifest?.id || ""),
        label: String(entry.manifest?.label || entry.manifest?.id || "Package"),
        category: String(entry.manifest?.category || "Packages"),
        summary: String(entry.manifest?.summary ||
          `${entry.manifest?.label || "This package"} adds a removable World object.`),
        installedByDefault: entry.installedByDefault !== false,
        installed:
          installedWorldPackages.has(String(entry.manifest?.id || "")) ||
          deferredDistrictInstalls.some(
            (item) => item.packageId === String(entry.manifest?.id || ""),
          ),
        loading: pendingWorldPackageInstalls.has(String(entry.manifest?.id || "")),
      })),
    setOfficeBuildStamp,
    setOfficeSeatState,
    showOfficeBubble,
    leaveOfficeInterior,
    focusRepositoryPortal,
    clearFocus,
    setCameraMode,
    setCameraView,
    getSavedViewState,
    restoreSavedViewState,
    setTheme,
    setLightLevel,
    setDaylightMode,
    setMovementTuning,
    setControl,
    setTouchMovement,
    setTouchInteractionActive,
    setSpawn,
    travelToRegion,
    visitNeighborhoodHome,
    campfireSeatsReady,
    whenCampfireSeatsReady,
    returnToCampfireBench,
    restoreCampfireSeatIfNearby,
    focusCampfireCircle,
    rideSwing,
    dismountSwing,
    rideQuadcopter,
    dismountQuadcopter,
    rideCoaster: () => fns.rideCoaster(),
    dismountCoaster: () => fns.dismountCoaster(),
    getCoasterState: () => fns.coasterState(),
    toggleJetpack,
    setJetpackEquipped,
    setJetpackVertical,
    getJetpackState: () => ({
      equipped: jetpackEquipped,
      altitude: Math.max(0, player.position.y - currentFloorY),
      maxAltitude: JETPACK_MAX_ALTITUDE,
    }),
    beginGymExercise,
    stopGymExercise,
    setGymWeight,
    performGymRep,
    setGymAuto,
    getGymState: () => gymPublicState(gymState.active === "bench"),
    getQuadcopterState: () => ({
      riding: Boolean(quadcopterRide),
      altitude: quadcopterRide
        ? Number(
            quadcopterStates[quadcopterRide.index]?.quadcopter.position.y
              .toFixed(2),
          )
        : 0,
      maxAltitude: QUADCOPTER_MAX_ALTITUDE,
      speed: quadcopterRide ? Number(quadcopterRide.speed.toFixed(2)) : 0,
      race: fns.droneRaceState(),
    }),
    getDroneCourseLayout: () => fns.droneCourseLayout(),
    setSwingSpeed,
    getSwingState: () => ({
      riding: Boolean(swingRide),
      seat: swingRide ? swingRide.index : -1,
      speedLevel: swingSpeedLevel,
      seats: swingStates.map((swing) => ({
        remoteId: swing.remoteId,
        amplitude: Number(swing.amplitude.toFixed(3)),
      })),
    }),
    setRemotePlayers,
    setRemotePlayerMovement,
    animateUnverifiedAvatarDeletion,
    setLocalOrgTeam,
    setAvatarFediverseProfile,
    setAvatarRecentPublicMessage,
    setAvatarFaceImage,
    updateArrivalStats,
    updateMemberLounge,
    updateLeaderboards,
    updateReferralLeaderboard,
    updateSiteReferrerLeaderboard,
    setBillboardRefreshing,
    deleteNetworkNode,
    updateNetworkNodes,
    armMirrorPushEffect,
    noteMirrorTraffic,
    nodeMachineStats,
    focusNetworkNode,
    updateFederatedInstances,
    updateInstanceHexes,
    updateBots,
    updateMirrorAgentTasks,
    updateSystemCapacity,
    updateQaBoard,
    updateOrganizations,
    updateFediverseDirectory,
    updateMediaSpaces,
    updateIdentity,
    setInputActive,
    updateRepositoryCatalog,
    beginRepositoryDistrictReveal,
    updateRepositoryGraph,
    updateRepositoryActivity,
    setRepositoryImportState,
    updateRepositorySizeMap,
    updateRepositoryRecordDesk,
    setRepositoryIssuePageExpanded,
    setRepositoryIssueAgentPicker,
    setRepositorySizeLoading,
    playEmote,
    playHandshake,
    showChatBubble,
    showMemberChatBubble,
    greetForkbot,
    exciteForkbot,
    setAgentBotAccess,
    exciteAgentBot,
    completeAgentTask,
    showAgentTaskBubble,
    updateDesktopAgentBots,
    summonDesktopAgentBots,
    desktopAgentBotCount: () => desktopAgentBots.size,
    updateRewardPool,
    playRewardEvent,
    setPaused,
    setPreviewCadence,
    setQualityTier,
    getQualityState: qualityState,
    setTriangleView,
    isTriangleView: () => triangleViewEnabled,
    dispose,
    setCameraZoom,
    getCameraState: () => ({
      mode: cameraMode,
      firstPerson: cameraMode === "first-person",
      zoom: cameraMode === "first-person" ? firstPersonZoom : cameraZoom,
      minZoom:
        cameraMode === "first-person"
          ? FIRST_PERSON_ZOOM_MIN
          : CAMERA_ZOOM_MIN,
      maxZoom:
        cameraMode === "first-person"
          ? FIRST_PERSON_ZOOM_MAX
          : CAMERA_ZOOM_MAX,
      yaw: cameraYaw,
      pitch:
        cameraMode === "first-person" ? firstPersonPitch : cameraPitch,
      orbitPitch: cameraPitch,
      firstPersonPitch,
      dragging: primaryPointerId !== null,
      pointerLocked: false,
    }),
    getMovementState: () => ({
      speed: keyboardMovementSpeed,
      baseSpeed: baseMoveSpeed(),
      maxSpeed: PLAYER_MAX_SPEED * moveSpeedScale,
      speedScale: moveSpeedScale,
      immediateDigitalInput: false,
      keyboardActive: [...keys].some((code) => MOVEMENT_KEYS.has(code)),
      touchActive: touchMovement.lengthSq() > 0,
      touchStrength: touchMovement.length(),
      touchX: touchMovement.x,
      touchY: touchMovement.y,
    }),
    getDiagnostics,
    setFigureDetailThresholds: (thresholds) =>
      figureBatches.setDetailThresholds(thresholds),
    getInstancedMirrorStats: () => ({
      avatars: avatarPartInstances.stats(),
      figures: figureBatches.stats(),
      sets: [...instancedMirrorSets].map((mirror) => ({
        parent: mirror.parent.name || mirror.parent.type,
        ...mirror.stats(),
      })),
    }),
    setProfiling: (enabled) => worldProfiler.setEnabled(enabled === true),
    getProfile: () => worldProfiler.sample(),
    auditScene: (profile = null) => {
      const drawingBuffer = renderer.getDrawingBufferSize(
        diagnosticsDrawingBuffer,
      );
      return auditWorldScene({
        scene,
        renderer,
        ownerOf: worldElementOwner,
        state: worldAuditState,
        profile,
        quality: qualityState(),
        output: {
          drawingBufferWidth: drawingBuffer.x,
          drawingBufferHeight: drawingBuffer.y,
          pixelRatio: renderer.getPixelRatio(),
        },
      });
    },
    // Read once at startup; asking the context again is a GPU round trip.
    rendererGpu: () => rendererContextInfo().gpu,
    shaderCompileStats: () => shaderCompileGuard,
    prepareFirstView,
    listWorldElements,
    setWorldElementEnabled,
    listSceneObjects,
    listWorldElementParts,
    pickWorldObject,
    deleteWorldObject,
    restoreWorldObject,
    restoreAllWorldObjects,
    listDeletedWorldObjects,
    installStoreElement,
    removeStoreElement,
    getEnvironmentState,
    getPosition: () => ({
      x: player.position.x,
      y: player.position.y,
      z: player.position.z,
      heading: player.rotation.y,
      space: currentSpace,
    }),
  };
}
