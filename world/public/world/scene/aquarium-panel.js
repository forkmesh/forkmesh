import { canvasArtTexture, repaintCanvasTexture, roundedRect } from "./canvas.js";
import * as resources from "./resources.js";

export const AQUARIUM_PANEL_WIDTH = 2.5;
export const AQUARIUM_PANEL_HEIGHT = 1.8;
export const AQUARIUM_PANEL_DEPTH = 0.12;
export const AQUARIUM_PANEL_SCHOOL_MODES = Object.freeze([
  "auto",
  "species",
  "colour",
  "scatter",
]);
export const AQUARIUM_PANEL_CONTROLS = Object.freeze([
  "feed",
  "tap",
  "follow",
  "school",
  "backdrop",
  "glass-response",
  "light",
]);

const PRESS_MS = 190;
const THROW_MS = 380;
const DIAL_MS = 260;
const FLASH_MS = 520;
const LEVER_ON = 0.38;
const LEVER_OFF = Math.PI - 0.38;
const TOGGLE_UP = Math.PI / 2 - 0.75;
const TOGGLE_DOWN = Math.PI / 2 + 0.75;
const DIAL_ANGLES = Object.freeze([1.1, 0.37, -0.37, -1.1]);
const LAYOUT = Object.freeze({
  screen: Object.freeze({ x: -0.36, y: 0.6, width: 1.62, height: 0.44 }),
  feed: Object.freeze({ x: -0.92, y: 0.16 }),
  tap: Object.freeze({ x: -0.46, y: 0.16 }),
  follow: Object.freeze({ x: 0, y: 0.16 }),
  buttonLabelY: -0.07,
  school: Object.freeze({ x: -0.8, y: -0.5 }),
  backdrop: Object.freeze({ x: -0.16, y: -0.5 }),
  glass: Object.freeze({ x: 0.32, y: -0.5 }),
  light: Object.freeze({ x: 0.9, y: -0.02 }),
});
const FACE_PIXELS_PER_UNIT = 320;
const SCREEN_WIDTH = 520;
const SCREEN_HEIGHT = 140;

function easeInOut(value) {
  const amount = value < 0 ? 0 : value > 1 ? 1 : value;
  return amount * amount * (3 - 2 * amount);
}

function easeBack(value) {
  const amount = value < 0 ? 0 : value > 1 ? 1 : value;
  const overshoot = 1.9;
  const shifted = amount - 1;
  return 1 + (overshoot + 1) * shifted * shifted * shifted + overshoot * shifted * shifted;
}

function faceX(x) {
  return (x + AQUARIUM_PANEL_WIDTH / 2) * FACE_PIXELS_PER_UNIT;
}

function faceY(y) {
  return (AQUARIUM_PANEL_HEIGHT / 2 - y) * FACE_PIXELS_PER_UNIT;
}

function drawPanelFace(context, canvas) {
  const width = AQUARIUM_PANEL_WIDTH * FACE_PIXELS_PER_UNIT;
  const height = AQUARIUM_PANEL_HEIGHT * FACE_PIXELS_PER_UNIT;
  const base = context.createLinearGradient(0, 0, 0, height);
  base.addColorStop(0, "#2a3338");
  base.addColorStop(1, "#1a2024");
  context.fillStyle = base;
  context.fillRect(0, 0, width, height);
  context.strokeStyle = "rgba(143,191,190,0.55)";
  context.lineWidth = 4;
  roundedRect(context, 8, 8, width - 16, height - 16, 22);
  context.stroke();
  context.fillStyle = "rgba(0,0,0,0.22)";
  roundedRect(
    context,
    faceX(LAYOUT.light.x - 0.22),
    faceY(0.84),
    0.44 * FACE_PIXELS_PER_UNIT,
    1.66 * FACE_PIXELS_PER_UNIT,
    14,
  );
  context.fill();
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillStyle = "#d9f6f2";
  context.font = "700 26px system-ui, sans-serif";
  for (const [label, spot] of [
    ["FEED", LAYOUT.feed],
    ["TAP", LAYOUT.tap],
    ["FOLLOW", LAYOUT.follow],
  ]) {
    context.fillText(label, faceX(spot.x), faceY(LAYOUT.buttonLabelY));
  }
  context.fillText("LIGHT", faceX(LAYOUT.light.x), faceY(0.74));
  context.font = "700 21px system-ui, sans-serif";
  context.fillStyle = "#9ff5d0";
  context.fillText("ON", faceX(LAYOUT.light.x), faceY(0.6));
  context.fillStyle = "#f5a3a3";
  context.fillText("OFF", faceX(LAYOUT.light.x), faceY(-0.66));
  context.fillStyle = "#d9f6f2";
  context.font = "700 22px system-ui, sans-serif";
  context.fillText("SCHOOL", faceX(LAYOUT.school.x), faceY(-0.8));
  context.fillText("BACKDROP", faceX(LAYOUT.backdrop.x), faceY(-0.8));
  context.fillText("GLASS", faceX(LAYOUT.glass.x), faceY(-0.8));
  context.font = "600 17px system-ui, sans-serif";
  context.fillStyle = "#b7cfd2";
  context.fillText("OPAQUE", faceX(LAYOUT.backdrop.x), faceY(LAYOUT.backdrop.y + 0.25));
  context.fillText("CLEAR", faceX(LAYOUT.backdrop.x), faceY(LAYOUT.backdrop.y - 0.2));
  context.fillText("CURIOUS", faceX(LAYOUT.glass.x), faceY(LAYOUT.glass.y + 0.25));
  context.fillText("SHY", faceX(LAYOUT.glass.x), faceY(LAYOUT.glass.y - 0.2));
  const dialLabels = ["AUTO", "SPECIES", "COLOUR", "SCATTER"];
  context.font = "600 15px system-ui, sans-serif";
  for (let index = 0; index < dialLabels.length; index += 1) {
    const angle = DIAL_ANGLES[index];
    const x = LAYOUT.school.x - Math.sin(angle) * 0.38;
    const y = LAYOUT.school.y + Math.cos(angle) * 0.36;
    context.save();
    context.translate(faceX(x), faceY(y));
    context.rotate(-angle * 0.6);
    context.fillText(dialLabels[index], 0, 0);
    context.restore();
  }
  return canvas;
}

function drawScreen(context, lines) {
  const gradient = context.createLinearGradient(0, 0, 0, SCREEN_HEIGHT);
  gradient.addColorStop(0, "#052427");
  gradient.addColorStop(1, "#021315");
  context.fillStyle = gradient;
  context.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  context.textAlign = "left";
  context.textBaseline = "middle";
  context.fillStyle = "#7cf5d8";
  context.font = "700 27px ui-monospace, monospace";
  context.fillText(lines[0], 16, 26);
  context.font = "600 22px ui-monospace, monospace";
  context.fillStyle = lines[4] ? "#ffd27a" : "#bff8ec";
  context.fillText(lines[1], 16, 64);
  context.fillStyle = "#bff8ec";
  context.fillText(lines[2], 16, 94);
  context.fillStyle = "#86c9c0";
  context.font = "600 18px ui-monospace, monospace";
  context.fillText(lines[3], 16, 122);
}

function schoolLine(schooling) {
  if (!schooling) return "SCHOOL · AUTO · STANDBY";
  if (schooling.control && schooling.control !== "auto") {
    if (schooling.control === "scatter") return "SCHOOL · SCATTER";
    return `SCHOOL · ${schooling.label} · ${schooling.groupCount} GROUPS`;
  }
  return schooling.active
    ? `SCHOOL MODE · ${schooling.label} ACTIVE`
    : "SCHOOL · AUTO · STANDBY";
}

export function createAquariumControlPanel(
  THREE,
  aquarium,
  {
    animated = null,
    interactive = null,
    reducedMotion = false,
    onToggle = () => {},
    now = () => performance.now(),
  } = {},
) {
  const { length: tankLength, depth: tankDepth } = aquarium.dimensions;
  const group = new THREE.Group();
  group.name = "forkmesh-office-aquarium-reef-panel";
  const glassFront = tankDepth / 2 + 0.04;
  group.position.set(
    glassFront + 0.02 + AQUARIUM_PANEL_DEPTH / 2,
    0.5 + AQUARIUM_PANEL_HEIGHT / 2,
    -tankLength / 2 + 0.42 + AQUARIUM_PANEL_WIDTH / 2,
  );
  group.rotation.y = Math.PI / 2;
  group.userData.officeFloorId = "lobby";
  group.userData.mountedOn = "forkmesh-office-marine-aquarium";
  aquarium.group.add(group);

  const faceZ = AQUARIUM_PANEL_DEPTH / 2;
  const housingMaterial = resources.material(THREE, {
    color: "#1b2226",
    metalness: 0.52,
    roughness: 0.42,
  });
  const chromeMaterial = resources.material(THREE, {
    color: "#a9b8bc",
    metalness: 0.82,
    roughness: 0.26,
  });
  const slotMaterial = resources.material(THREE, {
    color: "#080c0e",
    roughness: 0.92,
  });
  const housing = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      AQUARIUM_PANEL_WIDTH,
      AQUARIUM_PANEL_HEIGHT,
      AQUARIUM_PANEL_DEPTH,
    ),
    housingMaterial,
  );
  housing.name = "forkmesh-office-aquarium-reef-panel-housing";
  group.add(housing);
  const faceTexture = canvasArtTexture(
    THREE,
    AQUARIUM_PANEL_WIDTH * FACE_PIXELS_PER_UNIT,
    AQUARIUM_PANEL_HEIGHT * FACE_PIXELS_PER_UNIT,
    drawPanelFace,
  );
  const face = new THREE.Mesh(
    resources.primitive(
      THREE,
      "PlaneGeometry",
      AQUARIUM_PANEL_WIDTH - 0.04,
      AQUARIUM_PANEL_HEIGHT - 0.04,
    ),
    resources.uniqueMaterial(THREE, {
      map: faceTexture,
      roughness: 0.62,
      metalness: 0.18,
    }, "MeshStandardMaterial"),
  );
  face.name = "forkmesh-office-aquarium-reef-panel-face";
  face.position.z = faceZ + 0.002;
  group.add(face);
  for (const side of [-1, 1]) {
    const foot = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.24, 0.3, 0.035),
      chromeMaterial,
    );
    foot.name = `forkmesh-office-aquarium-reef-panel-mount-${side < 0 ? "left" : "right"}`;
    foot.position.set(side * 0.94, -AQUARIUM_PANEL_HEIGHT / 2 - 0.1, faceZ - 0.005);
    group.add(foot);
    for (const row of [-1, 1]) {
      const bolt = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.03, 0.03, 0.03, 8),
        chromeMaterial,
      );
      bolt.rotation.x = Math.PI / 2;
      bolt.position.set(
        side * (AQUARIUM_PANEL_WIDTH / 2 - 0.08),
        row * (AQUARIUM_PANEL_HEIGHT / 2 - 0.08),
        faceZ + 0.012,
      );
      group.add(bolt);
    }
  }

  const screenTexture = canvasArtTexture(THREE, SCREEN_WIDTH, SCREEN_HEIGHT, (context) =>
    drawScreen(context, ["REEF CONTROL", "SCHOOL · AUTO · STANDBY", "READY TO FEED", "", false]),
  );
  const screenMaterial = resources.uniqueMaterial(THREE, {
    map: screenTexture,
    toneMapped: false,
  }, "MeshBasicMaterial");
  const screenBezel = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      LAYOUT.screen.width + 0.1,
      LAYOUT.screen.height + 0.08,
      0.03,
    ),
    slotMaterial,
  );
  screenBezel.position.set(LAYOUT.screen.x, LAYOUT.screen.y, faceZ + 0.01);
  group.add(screenBezel);
  const screen = new THREE.Mesh(
    resources.primitive(
      THREE,
      "PlaneGeometry",
      LAYOUT.screen.width,
      LAYOUT.screen.height,
    ),
    screenMaterial,
  );
  screen.name = "forkmesh-office-aquarium-reef-panel-screen";
  screen.position.set(LAYOUT.screen.x, LAYOUT.screen.y, faceZ + 0.027);
  group.add(screen);

  const controls = new Map();
  const targets = [];
  const animating = new Set();

  function capMaterial(color) {
    return resources.uniqueMaterial(THREE, {
      color,
      emissive: color,
      emissiveIntensity: 0.06,
      roughness: 0.36,
      metalness: 0.12,
    }, "MeshStandardMaterial");
  }

  function sharedCapMaterial(color, emissiveIntensity) {
    return resources.material(THREE, {
      color,
      emissive: color,
      emissiveIntensity,
      roughness: 0.36,
      metalness: 0.12,
    }, "MeshStandardMaterial");
  }

  function addHitPad(id, x, y, width, height) {
    const pad = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", width, height, 0.34),
      housingMaterial,
    );
    pad.name = `forkmesh-office-aquarium-reef-control-${id}`;
    pad.position.set(x, y, faceZ + 0.14);
    pad.visible = false;
    pad.userData.raycastProxy = true;
    pad.userData.interactive = "aquarium-control";
    pad.userData.aquariumControl = id;
    pad.userData.officeFloorId = "lobby";
    group.add(pad);
    targets.push(pad);
    return pad;
  }

  function addControl(id, kind, parts) {
    const control = {
      id,
      kind,
      lit: false,
      hovered: false,
      flashUntil: 0,
      litColor: parts.litColor,
      material: parts.material,
      pad: parts.pad,
      moving: parts.moving,
      axis: parts.axis,
      rest: parts.rest,
      from: parts.rest,
      to: parts.rest,
      startedAt: 0,
      durationMs: 0,
      bounce: false,
    };
    parts.pad.userData.aquariumControlKind = kind;
    controls.set(id, control);
    return control;
  }

  function addButton(id, x, y, color) {
    const bezel = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.155, 0.165, 0.04, 20),
      chromeMaterial,
    );
    bezel.rotation.x = Math.PI / 2;
    bezel.position.set(x, y, faceZ + 0.02);
    group.add(bezel);
    const material = capMaterial(color);
    const cap = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.12, 0.12, 0.07, 20),
      material,
    );
    cap.name = `forkmesh-office-aquarium-reef-button-${id}`;
    cap.rotation.x = Math.PI / 2;
    cap.position.set(x, y, faceZ + 0.065);
    group.add(cap);
    return addControl(id, "button", {
      litColor: color,
      material,
      pad: addHitPad(id, x, y, 0.44, 0.46),
      moving: cap.position,
      axis: "z",
      rest: cap.position.z,
    });
  }

  function addToggle(id, x, y, color) {
    const plate = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.16, 0.26, 0.03),
      chromeMaterial,
    );
    plate.position.set(x, y, faceZ + 0.015);
    group.add(plate);
    const pivot = new THREE.Group();
    pivot.name = `forkmesh-office-aquarium-reef-switch-${id}`;
    pivot.position.set(x, y, faceZ + 0.03);
    pivot.rotation.x = TOGGLE_UP;
    group.add(pivot);
    const bat = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.024, 0.032, 0.24, 10),
      chromeMaterial,
    );
    bat.position.y = 0.12;
    pivot.add(bat);
    const material = capMaterial(color);
    const tip = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.045, 12, 8),
      material,
    );
    tip.position.y = 0.24;
    pivot.add(tip);
    return addControl(id, "switch", {
      litColor: color,
      material,
      pad: addHitPad(id, x, y, 0.44, 0.56),
      moving: pivot.rotation,
      axis: "x",
      rest: TOGGLE_UP,
    });
  }

  function addDial(id, x, y) {
    const bezel = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.2, 0.21, 0.03, 24),
      chromeMaterial,
    );
    bezel.rotation.x = Math.PI / 2;
    bezel.position.set(x, y, faceZ + 0.015);
    group.add(bezel);
    const pivot = new THREE.Group();
    pivot.name = `forkmesh-office-aquarium-reef-dial-${id}`;
    pivot.position.set(x, y, faceZ + 0.03);
    pivot.rotation.z = DIAL_ANGLES[0];
    group.add(pivot);
    const material = capMaterial("#8fd4ff");
    const knob = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.14, 0.15, 0.1, 24),
      material,
    );
    knob.rotation.x = Math.PI / 2;
    knob.position.z = 0.05;
    pivot.add(knob);
    const pointer = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.035, 0.13, 0.02),
      slotMaterial,
    );
    pointer.position.set(0, 0.07, 0.105);
    pivot.add(pointer);
    const leds = [];
    for (let index = 0; index < DIAL_ANGLES.length; index += 1) {
      const led = new THREE.Mesh(
        resources.primitive(THREE, "SphereGeometry", 0.028, 10, 6),
        sharedCapMaterial("#7cf5d8", 0.06),
      );
      led.position.set(
        x - Math.sin(DIAL_ANGLES[index]) * 0.25,
        y + Math.cos(DIAL_ANGLES[index]) * 0.25,
        faceZ + 0.01,
      );
      group.add(led);
      leds.push(led);
    }
    const control = addControl(id, "dial", {
      litColor: "#8fd4ff",
      material,
      pad: addHitPad(id, x, y, 0.62, 0.6),
      moving: pivot.rotation,
      axis: "z",
      rest: DIAL_ANGLES[0],
    });
    control.leds = leds;
    return control;
  }

  function addLever(id, x, y) {
    const plate = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.3, 0.86, 0.03),
      chromeMaterial,
    );
    plate.position.set(x, y, faceZ + 0.015);
    group.add(plate);
    const slot = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.07, 0.6, 0.012),
      slotMaterial,
    );
    slot.position.set(x, y, faceZ + 0.033);
    group.add(slot);
    const pivot = new THREE.Group();
    pivot.name = `forkmesh-office-aquarium-reef-lever-${id}`;
    pivot.position.set(x, y, faceZ + 0.05);
    pivot.rotation.x = LEVER_ON;
    group.add(pivot);
    const hub = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.065, 0.065, 0.2, 14),
      chromeMaterial,
    );
    hub.rotation.z = Math.PI / 2;
    pivot.add(hub);
    const shaft = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.026, 0.032, 0.42, 10),
      chromeMaterial,
    );
    shaft.position.y = 0.21;
    pivot.add(shaft);
    const material = capMaterial("#ff6a5c");
    const knob = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.085, 16, 10),
      material,
    );
    knob.name = `forkmesh-office-aquarium-reef-lever-knob-${id}`;
    knob.position.y = 0.44;
    pivot.add(knob);
    const ledMaterial = capMaterial("#62f59a");
    const led = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.035, 10, 6),
      ledMaterial,
    );
    led.position.set(x + 0.11, y + 0.37, faceZ + 0.035);
    group.add(led);
    const control = addControl(id, "lever", {
      litColor: "#ff6a5c",
      material,
      pad: addHitPad(id, x, y, 0.6, 1.3),
      moving: pivot.rotation,
      axis: "x",
      rest: LEVER_ON,
    });
    control.ledMaterial = ledMaterial;
    return control;
  }

  const feedButton = addButton("feed", LAYOUT.feed.x, LAYOUT.feed.y, "#f2a93b");
  const tapButton = addButton("tap", LAYOUT.tap.x, LAYOUT.tap.y, "#5fd6ff");
  const followButton = addButton("follow", LAYOUT.follow.x, LAYOUT.follow.y, "#7ef0a0");
  const schoolDial = addDial("school", LAYOUT.school.x, LAYOUT.school.y);
  const backdropSwitch = addToggle("backdrop", LAYOUT.backdrop.x, LAYOUT.backdrop.y, "#c7b8ff");
  const glassSwitch = addToggle("glass-response", LAYOUT.glass.x, LAYOUT.glass.y, "#7cf5d8");
  const lightLever = addLever("light", LAYOUT.light.x, LAYOUT.light.y);

  const glass = aquarium.frontGlass;
  glass.userData.interactive = "aquarium-glass";
  glass.userData.officeFloorId = "lobby";
  targets.push(glass);
  if (Array.isArray(interactive)) interactive.push(...targets);

  const localPoint = new THREE.Vector3();
  let hoveredControl = null;
  let hoverFollowing = false;
  let schoolIndex = 0;
  let screenKey = "";
  let feedingSeconds = -1;
  let lastState = null;

  function applyLook(control, time) {
    const flashing = time < control.flashUntil;
    const lit = control.lit || flashing;
    const base = lit ? 1.15 : 0.06;
    control.material.emissiveIntensity = base + (control.hovered ? 0.45 : 0);
  }

  function animateTo(control, value, durationMs, bounce = false, time = now()) {
    const current = control.moving[control.axis];
    control.from = current;
    control.to = value;
    control.bounce = bounce;
    control.startedAt = time;
    control.durationMs = reducedMotion ? 0 : durationMs;
    if (control.durationMs <= 0) {
      control.moving[control.axis] = bounce ? control.rest : value;
      return;
    }
    animating.add(control);
  }

  function update(time) {
    if (!animating.size) return false;
    for (const control of animating) {
      const progress = (time - control.startedAt) / control.durationMs;
      if (control.bounce) {
        const wave = progress < 0.4 ? progress / 0.4 : 1 - (progress - 0.4) / 0.6;
        control.moving[control.axis] =
          control.rest + (control.to - control.rest) * easeInOut(wave);
      } else {
        const eased = control.kind === "lever" ? easeBack(progress) : easeInOut(progress);
        control.moving[control.axis] = control.from + (control.to - control.from) * eased;
      }
      if (progress >= 1) {
        control.moving[control.axis] = control.bounce ? control.rest : control.to;
        animating.delete(control);
      }
      if (time >= control.flashUntil && control.flashUntil > 0) {
        control.flashUntil = 0;
        applyLook(control, time);
      }
    }
    return true;
  }

  function pressButton(control, time) {
    animateTo(control, control.rest - 0.04, PRESS_MS, true, time);
  }

  function state(time = now()) {
    const controlState = aquarium.getControlState(time);
    const feeding = aquarium.getFeedingState(time);
    return {
      feeding: feeding.active,
      feedingRemainingMs: feeding.remainingMs,
      backdropOpaque: controlState.backdropOpaque,
      lightEnabled: controlState.lightEnabled,
      schoolMode: controlState.schoolMode,
      schooling: controlState.schooling,
      glassResponse: controlState.glassResponse,
      followEnabled: controlState.followEnabled,
      lever: lightLever.moving.x,
      dial: schoolDial.moving.z,
    };
  }

  function repaintScreen(current) {
    const seconds = current.feeding
      ? Math.ceil(current.feedingRemainingMs / 1000)
      : -1;
    const lines = [
      current.lightEnabled ? "REEF CONTROL · LIGHT ON" : "REEF CONTROL · LIGHT OFF",
      current.feeding ? `FEEDING · ${seconds}s LEFT` : "READY TO FEED",
      schoolLine(current.schooling),
      `GLASS ${current.glassResponse.toUpperCase()} · FOLLOW ${
        current.followEnabled ? "ON" : "OFF"
      } · ${current.backdropOpaque ? "OPAQUE" : "CLEAR"}`,
      current.feeding,
    ];
    const key = lines.join("|");
    if (key === screenKey) return false;
    screenKey = key;
    feedingSeconds = seconds;
    repaintCanvasTexture(screenMaterial, (context) => drawScreen(context, lines));
    return true;
  }

  function sync(time = now(), animate = true) {
    const current = state(time);
    const previous = lastState;
    lastState = current;
    feedButton.lit = current.feeding;
    followButton.lit = current.followEnabled;
    tapButton.lit = false;
    backdropSwitch.lit = current.backdropOpaque;
    glassSwitch.lit = current.glassResponse === "curious";
    lightLever.lit = current.lightEnabled;
    schoolDial.lit = current.schoolMode !== "auto";
    lightLever.ledMaterial.emissiveIntensity = current.lightEnabled ? 1.3 : 0.04;
    const nextSchool = Math.max(0, AQUARIUM_PANEL_SCHOOL_MODES.indexOf(current.schoolMode));
    schoolIndex = nextSchool;
    for (let index = 0; index < schoolDial.leds.length; index += 1) {
      schoolDial.leds[index].material = sharedCapMaterial(
        "#7cf5d8",
        index === nextSchool ? 1.4 : 0.03,
      );
    }
    const duration = animate ? 1 : 0;
    if (!previous || previous.lightEnabled !== current.lightEnabled) {
      animateTo(lightLever, current.lightEnabled ? LEVER_ON : LEVER_OFF, THROW_MS * duration, false, time);
    }
    if (!previous || previous.backdropOpaque !== current.backdropOpaque) {
      animateTo(backdropSwitch, current.backdropOpaque ? TOGGLE_UP : TOGGLE_DOWN, PRESS_MS * duration, false, time);
    }
    if (!previous || previous.glassResponse !== current.glassResponse) {
      animateTo(glassSwitch, current.glassResponse === "curious" ? TOGGLE_UP : TOGGLE_DOWN, PRESS_MS * duration, false, time);
    }
    if (!previous || previous.schoolMode !== current.schoolMode) {
      animateTo(schoolDial, DIAL_ANGLES[nextSchool], DIAL_MS * duration, false, time);
    }
    for (const control of controls.values()) applyLook(control, time);
    repaintScreen(current);
    return current;
  }

  function press(id, time = now()) {
    const control = controls.get(id);
    if (!control) return null;
    let result = null;
    if (id === "feed") {
      pressButton(control, time);
      result = aquarium.feed(time);
    } else if (id === "tap") {
      pressButton(control, time);
      control.flashUntil = time + FLASH_MS;
      result = aquarium.tapGlass(time);
    } else if (id === "follow") {
      pressButton(control, time);
      result = aquarium.setFollowEnabled(!aquarium.getControlState(time).followEnabled);
      onToggle("follow", result);
    } else if (id === "school") {
      const next = AQUARIUM_PANEL_SCHOOL_MODES[(schoolIndex + 1) % AQUARIUM_PANEL_SCHOOL_MODES.length];
      result = aquarium.setSchoolMode(next, time);
    } else if (id === "backdrop") {
      result = aquarium.setBackdropOpaque(!aquarium.getControlState(time).backdropOpaque);
      onToggle("backdrop", result);
    } else if (id === "glass-response") {
      const current = aquarium.getControlState(time).glassResponse;
      result = aquarium.setGlassResponse(current === "curious" ? "shy" : "curious");
      onToggle("glass-response", result);
    } else if (id === "light") {
      result = aquarium.setLightEnabled(!aquarium.getControlState(time).lightEnabled, time);
      onToggle("light", result);
    }
    sync(time);
    return result;
  }

  function touchGlass(worldPoint, time = now()) {
    if (!worldPoint) return 0;
    aquarium.group.updateWorldMatrix(true, false);
    aquarium.group.worldToLocal(localPoint.copy(worldPoint));
    if (aquarium.getControlState(time).followEnabled) aquarium.setFollowTarget(localPoint);
    return aquarium.tapGlassAt(localPoint, time);
  }

  function hover(object, worldPoint = null, time = now()) {
    const control = object?.userData?.aquariumControl
      ? controls.get(object.userData.aquariumControl) || null
      : null;
    if (control !== hoveredControl) {
      if (hoveredControl) {
        hoveredControl.hovered = false;
        applyLook(hoveredControl, time);
      }
      hoveredControl = control;
      if (control) {
        control.hovered = true;
        applyLook(control, time);
      }
    }
    const onGlass = object === glass;
    if (onGlass && worldPoint && aquarium.getControlState(time).followEnabled) {
      aquarium.group.updateWorldMatrix(true, false);
      aquarium.group.worldToLocal(localPoint.copy(worldPoint));
      aquarium.setFollowTarget(localPoint);
      hoverFollowing = true;
    } else if (hoverFollowing && !onGlass) {
      aquarium.setFollowTarget(null);
      hoverFollowing = false;
    }
    if (control) return "control";
    return onGlass ? "glass" : "";
  }

  if (Array.isArray(animated)) {
    animated.push((time) => {
      if (animating.size) update(time);
    });
  }
  sync(now(), false);

  return {
    group,
    glass,
    targets,
    controls,
    press,
    touchGlass,
    hover,
    sync,
    update,
    state,
    get animating() {
      return animating.size;
    },
    get screenKey() {
      return screenKey;
    },
    get feedingSeconds() {
      return feedingSeconds;
    },
  };
}
