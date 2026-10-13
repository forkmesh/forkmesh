import {
  animateAvatarActivity,
  applyLegPitch,
  applySeatedLegPose,
  seatedAvatarY,
} from "../avatars.js";
import { coasterSample, coasterTrack } from "../coaster.js";
import {
  COASTER_MOUNT_DISTANCE,
  COASTER_RIDING_ACTIVITY,
} from "../gameplay-constants.js";

const SEAT_TOP = 0.93;
const SEAT_FORWARD = 0.45;
const PLATFORM_OFFSET = 4.6;
const RIDE_SUBSTEPS = 3;
const ARMS_UP = -2.75;
const ARMS_HOLD = -0.32;
const MARK_MESSAGE_MS = 2200;
const MAX_HEADING_STEP = 0.5;

function wrapAngle(angle) {
  return Math.atan2(Math.sin(angle), Math.cos(angle));
}

export function functions({
  THREE,
  labelLayer,
  reducedMotion,
  ctx,
  fns,
}) {
  const sample = {};
  const basis = new THREE.Matrix4();
  const right = new THREE.Vector3();
  const up = new THREE.Vector3();
  const back = new THREE.Vector3();
  const seat = new THREE.Vector3();
  let hud = null;
  let shownSpeed = "";
  let shownMessage = "";

  function buildHud() {
    if (hud || !labelLayer) return hud;
    const root = document.createElement("div");
    root.className = "world-coaster-hud";
    root.hidden = true;
    root.innerHTML = `
      <output class="world-coaster-hud__speed" aria-label="Speed">0</output>
      <p class="world-coaster-hud__message" role="status" aria-live="polite" hidden></p>`;
    labelLayer.append(root);
    hud = {
      root,
      speed: root.querySelector(".world-coaster-hud__speed"),
      message: root.querySelector(".world-coaster-hud__message"),
    };
    return hud;
  }

  function setHudVisible(visible) {
    const target = buildHud();
    if (!target) return;
    target.root.hidden = !visible;
    if (!visible) {
      shownSpeed = "";
      shownMessage = "";
      target.message.hidden = true;
    }
  }

  function paintHud(speed, message) {
    const target = buildHud();
    if (!target) return;
    const speedText = `${Math.round(speed * 3.6)} km/h`;
    if (speedText !== shownSpeed) {
      shownSpeed = speedText;
      target.speed.textContent = speedText;
    }
    if (message !== shownMessage) {
      shownMessage = message;
      target.message.textContent = message;
      target.message.hidden = !message;
    }
  }

  function placeTrain(distance) {
    if (ctx.coaster) fns.placeCoasterTrain(ctx.coaster.train, distance);
  }

  function seatRider(distance) {
    coasterSample(distance, sample);
    back.set(-sample.tx, -sample.ty, -sample.tz);
    up.set(sample.ux, sample.uy, sample.uz);
    right.crossVectors(up, back).normalize();
    basis.makeBasis(right, up, back);
    const lift = seatedAvatarY(SEAT_TOP, ctx.player.scale.x);
    seat
      .set(sample.x, sample.y, sample.z)
      .addScaledVector(up, lift)
      .addScaledVector(back, -SEAT_FORWARD);
    ctx.player.position.copy(seat);
    ctx.player.rotation.order = "YXZ";
    ctx.player.quaternion.setFromRotationMatrix(basis);
    applySeatedLegPose(ctx.player);
    return sample;
  }

  function emitMovement(moving) {
    ctx.lastMovementEmit = performance.now();
    ctx.lastPosition.copy(ctx.player.position);
    fns.queueMovementEvent({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving,
      activity: COASTER_RIDING_ACTIVITY,
    });
  }

  function rideCoaster() {
    if (ctx.coasterRide) {
      dismountCoaster();
      return true;
    }
    if (ctx.officeSceneMode !== "town" || !ctx.coaster?.train.parent) return false;
    fns.dismountSwing({ relocate: false });
    fns.dismountBike({ relocate: false });
    fns.dismountCar({ relocate: false });
    fns.dismountQuadcopter({ relocate: false });
    fns.standUpFromBench();
    fns.cancelDash();
    fns.removeRoofParachute();
    fns.completeStartHereStep("explore");
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    ctx.coasterRide = {
      distance: ctx.coaster.stationDistance,
      travelled: 0,
      speed: 0,
      heading: null,
      mark: "",
      markUntil: 0,
      laps: 0,
    };
    ctx.coaster.hint.visible = false;
    seatRider(ctx.coasterRide.distance);
    ctx.cameraSnapPending = true;
    setHudVisible(true);
    emitMovement(false);
    return true;
  }

  function dismountCoaster({ relocate = true } = {}) {
    const ride = ctx.coasterRide;
    if (!ride) return;
    ctx.coasterRide = null;
    const finished = ride.travelled >= coasterTrack().length;
    placeTrain(ctx.coaster.stationDistance);
    ctx.coaster.hint.visible = true;
    setHudVisible(false);
    ctx.player.quaternion.identity();
    ctx.player.rotation.order = "YXZ";
    ctx.player.rotation.set(0, Math.atan2(-sample.tx, -sample.tz), 0);
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applyLegPitch(ctx.player, 0, 0);
    if (relocate && finished) {
      coasterSample(ctx.coaster.stationDistance, sample);
      right.set(-sample.tz, 0, sample.tx).normalize();
      ctx.player.position.set(
        sample.x + right.x * PLATFORM_OFFSET,
        ctx.currentFloorY,
        sample.z + right.z * PLATFORM_OFFSET,
      );
      ctx.jumpVelocity = 0;
    } else if (ctx.player.position.y > ctx.currentFloorY + 2) {
      fns.prepareRoofParachute();
      ctx.jumpVelocity = -0.4;
    } else {
      ctx.player.position.y = ctx.currentFloorY;
      ctx.jumpVelocity = 0;
    }
    ctx.lastPosition.copy(ctx.player.position);
    ctx.cameraSnapPending = true;
  }

  function toggleNearestCoasterRide() {
    if (ctx.coasterRide) {
      dismountCoaster();
      return true;
    }
    if (ctx.officeSceneMode !== "town" || !ctx.coaster?.train.parent) return false;
    coasterSample(ctx.coaster.stationDistance, sample);
    const distance = Math.hypot(
      sample.x - ctx.player.position.x,
      sample.z - ctx.player.position.z,
    );
    if (distance > COASTER_MOUNT_DISTANCE) return false;
    return rideCoaster();
  }

  function followHeading(ride, current) {
    const flat = Math.hypot(current.tx, current.tz);
    if (current.uy < 0.3 || flat < 0.3) return;
    const heading = Math.atan2(-current.tx, -current.tz);
    if (ride.heading !== null) {
      const turn = wrapAngle(heading - ride.heading);
      if (Math.abs(turn) < MAX_HEADING_STEP) {
        ctx.cameraYaw = wrapAngle(ctx.cameraYaw + turn);
      }
    }
    ride.heading = heading;
  }

  function updateCoasterRide(input, delta, time) {
    const ride = ctx.coasterRide;
    if (!ride) return false;
    const track = coasterTrack();
    ctx.cameraFocus = null;
    ctx.focusedRepositoryKey = "";
    ctx.jumpQueued = false;
    ctx.jumpVelocity = 0;
    const step = Math.min(0.1, Math.max(0, delta)) / RIDE_SUBSTEPS;
    for (let substep = 0; substep < RIDE_SUBSTEPS; substep += 1) {
      coasterSample(ride.distance, sample);
      ride.speed = sample.speed;
      ride.distance += sample.speed * step;
      ride.travelled += sample.speed * step;
    }
    if (ride.travelled >= track.length) {
      ride.laps += 1;
      dismountCoaster();
      return true;
    }
    placeTrain(ride.distance);
    const current = seatRider(ride.distance);
    followHeading(ride, current);
    const section = track.sections[current.section];
    if (section.mark && section.mark !== ride.mark) {
      ride.mark = section.mark;
      ride.markUntil = performance.now() + MARK_MESSAGE_MS;
    }
    const thrill = !section.rails || (current.ty < -0.35 && current.speed > 30);
    const arms = thrill ? ARMS_UP : ARMS_HOLD;
    ctx.player.userData.leftArm.rotation.x = arms;
    ctx.player.userData.rightArm.rotation.x = arms;
    ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
    animateAvatarActivity(ctx.player, time, delta, reducedMotion);
    paintHud(
      current.speed,
      performance.now() < ride.markUntil ? ride.mark.toUpperCase() : "",
    );
    if (performance.now() - ctx.lastMovementEmit > 250) emitMovement(true);
    ctx.wasWalking = false;
    return true;
  }

  function coasterState() {
    const ride = ctx.coasterRide;
    const track = coasterTrack();
    return {
      riding: Boolean(ride),
      distance: ride ? Number(ride.distance.toFixed(2)) : 0,
      travelled: ride ? Number(ride.travelled.toFixed(2)) : 0,
      speed: ride ? Number(ride.speed.toFixed(2)) : 0,
      length: Number(track.length.toFixed(2)),
      element: ride ? ride.mark : "",
      marks: track.marks.map(({ name, d }) => ({ name, d: Number(d.toFixed(1)) })),
    };
  }

  return {
    coasterState,
    dismountCoaster,
    rideCoaster,
    toggleNearestCoasterRide,
    updateCoasterRide,
  };
}
