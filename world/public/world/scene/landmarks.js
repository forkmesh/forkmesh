import { addSectionPlaque } from "./arrival.js";
import { makeLabelSprite } from "./labels.js";
import { makeMaterial, makeSharedMaterial } from "./materials.js";
import { setShadows } from "./object-utils.js";
import * as resources from "./resources.js";

function createOrganizationQuarter(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const buildingMaterial = makeMaterial(THREE, "#42385c", { roughness: 0.55 });
  const edgeMaterial = makeMaterial(THREE, "#d5b6ff", {
    emissive: "#60458b",
    emissiveIntensity: 0.72,
    metalness: 0.18,
  });
  const windowMaterial = makeMaterial(THREE, "#e7dcff", {
    emissive: "#a17bdd",
    emissiveIntensity: 0.82,
    roughness: 0.32,
  });

  const heights = [6.8, 4.7, 8.4];
  const xs = [-2.9, 0, 2.7];
  heights.forEach((height, index) => {
    const tower = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", index === 1 ? 2.5 : 2.15, height, 2.8),
      buildingMaterial,
    );
    tower.position.set(xs[index], height / 2, index === 1 ? 0.5 : 0);
    group.add(tower);
    for (let floor = 0; floor < Math.floor(height / 1.15); floor += 1) {
      const window = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", index === 1 ? 1.45 : 1.2, 0.28, 0.04),
        windowMaterial,
      );
      window.position.set(xs[index], 0.9 + floor * 1.12, -1.42);
      group.add(window);
    }
    const roof = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", index === 1 ? 2.8 : 2.45, 0.16, 3.1),
      edgeMaterial,
    );
    roof.position.set(xs[index], height + 0.08, index === 1 ? 0.5 : 0);
    group.add(roof);
  });

  const lobby = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 6.7, 1.55, 2.9), edgeMaterial);
  lobby.position.set(0, 0.8, -1.15);
  group.add(lobby);
  const doorway = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 1.1, 1.15, 0.06),
    makeMaterial(THREE, "#9ef7c6", {
      transparent: true,
      opacity: 0.7,
      emissive: "#38d58d",
      emissiveIntensity: 0.75,
    }),
  );
  doorway.position.set(0, 0.64, -2.64);
  group.add(doorway);
  addSectionPlaque(THREE, group, position, "ORGANIZATIONS", "permissioned team spaces", "#d5b6ff", 4.4);

  group.position.set(...position);
  group.userData.landmark = "organizations";
  group.traverse((child) => {
    if (child.isMesh) {
      child.userData.landmark = "organizations";
      interactive.push(child);
    }
  });
  setShadows(group);
  animated.push((time) => {
    doorway.material.opacity = 0.58 + Math.sin(time * 0.002) * 0.17;
  });
  return group;
}

function createFediverseCenter(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const base = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 4.8, 5.2, 0.42, 7),
    makeSharedMaterial(THREE, "#3c2531"),
  );
  base.position.y = 0.21;
  group.add(base);
  const colors = ["#8c8dff", "#8fcf85", "#ffffff", "#ff806f", "#77d9ff"];
  const labels = ["M", "L", "X", "R", "↗"];
  const orbs = [];
  for (let index = 0; index < 5; index += 1) {
    const angle = (index / 5) * Math.PI * 2 + Math.PI / 4;
    const orb = new THREE.Mesh(
      resources.primitive(THREE, "IcosahedronGeometry", 0.92, 1),
      makeMaterial(THREE, colors[index], {
        emissive: colors[index],
        emissiveIntensity: 0.32,
        roughness: 0.38,
      }),
    );
    orb.position.set(Math.cos(angle) * 2.8, 2.6, Math.sin(angle) * 2.8);
    orb.userData = { angle, radius: 2.8, index };
    orbs.push(orb);
    group.add(orb);

    const label = makeLabelSprite(
      THREE,
      labels[index],
      ["Mastodon", "Lemmy", "Twitter / X", "Reddit", "public web"][index],
      colors[index],
    );
    label.scale.set(2.1, 0.7, 1);
    label.position.copy(orb.position).add(new THREE.Vector3(0, 1.35, 0));
    group.add(label);
  }
  const hub = new THREE.Mesh(
    resources.primitive(THREE, "OctahedronGeometry", 1.28, 1),
    makeSharedMaterial(THREE, "#ff9eb7", {
      emissive: "#b8466d",
      emissiveIntensity: 0.68,
      metalness: 0.18,
    }),
  );
  hub.position.y = 2.2;
  group.add(hub);
  addSectionPlaque(THREE, group, position, "FEDIVERSE", "consent-aware social center", "#ff9eb7", 6.4);
  group.userData.socialOrbs = orbs;

  group.position.set(...position);
  group.userData.landmark = "fediverse";
  group.traverse((child) => {
    if (child.isMesh) {
      child.userData.landmark = "fediverse";
      interactive.push(child);
    }
  });
  setShadows(group);
  animated.push((time) => {
    hub.rotation.y = time * 0.00035;
    hub.rotation.x = time * 0.00018;
    orbs.forEach((orb, index) => {
      const angle = orb.userData.angle + time * 0.00009 * (index % 2 ? -1 : 1);
      orb.position.x = Math.cos(angle) * orb.userData.radius;
      orb.position.z = Math.sin(angle) * orb.userData.radius;
      orb.position.y = 2.6 + Math.sin(time * 0.0013 + index) * 0.24;
      orb.rotation.y += 0.004;
    });
  });
  return group;
}

function createSecurityWorkshop(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const platform = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.8, 0.4, 5.4),
    makeSharedMaterial(THREE, "#173c37"),
  );
  platform.position.y = 0.2;
  group.add(platform);
  const building = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 6.4, 3.7, 3.8),
    makeSharedMaterial(THREE, "#1d544d"),
  );
  building.position.y = 2.15;
  group.add(building);

  const clipboard = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 2.6, 3.6, 0.28),
    makeSharedMaterial(THREE, "#d9ece2"),
  );
  clipboard.position.set(0, 3.3, -2.08);
  clipboard.rotation.x = -0.04;
  group.add(clipboard);
  const clip = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.95, 0.34, 0.18),
    makeSharedMaterial(THREE, "#88f0df", {
      metalness: 0.25,
      emissive: "#42aa9a",
      emissiveIntensity: 0.52,
    }),
  );
  clip.position.set(0, 5.05, -2.27);
  group.add(clip);
  for (let index = 0; index < 5; index += 1) {
    const line = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", index === 4 ? 1.2 : 1.75, 0.09, 0.035),
      makeMaterial(THREE, index === 0 ? "#ff8e78" : "#315b52"),
    );
    line.position.set(index === 4 ? -0.28 : 0, 4.32 - index * 0.54, -2.245);
    group.add(line);
  }
  const scan = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 2.4, 0.08, 0.06),
    makeSharedMaterial(THREE, "#88f0df", {
      emissive: "#88f0df",
      emissiveIntensity: 1.6,
    }),
  );
  scan.position.set(0, 4.62, -2.29);
  group.add(scan);
  addSectionPlaque(THREE, group, position, "SECURITY", "scoped scans + human review", "#88f0df", 5.2);

  group.position.set(...position);
  group.userData.landmark = "security";
  group.traverse((child) => {
    if (child.isMesh) {
      child.userData.landmark = "security";
      interactive.push(child);
    }
  });
  setShadows(group);
  animated.push((time) => {
    scan.position.y = 2.15 + ((time * 0.0011) % 2.9);
  });
  return group;
}

function finishLandmark(group, id, position, interactive) {
  group.position.set(...position);
  group.userData.landmark = id;
  group.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.landmark = id;
    interactive.push(child);
  });
  setShadows(group);
  return group;
}

function createCommunityStage(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const platform = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 4.4, 4.8, 0.5, 10),
    makeSharedMaterial(THREE, "#5b3827"),
  );
  platform.position.y = 0.25;
  group.add(platform);
  const stage = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 6.4, 0.55, 3.8),
    makeSharedMaterial(THREE, "#ffb77d", { roughness: 0.48 }),
  );
  stage.position.set(0, 0.62, 0.6);
  group.add(stage);
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 5.8, 3.1, 0.28),
    makeMaterial(THREE, "#13231e"),
  );
  board.position.set(0, 3.0, 1.75);
  group.add(board);
  for (let index = 0; index < 4; index += 1) {
    const row = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 4.6 - index * 0.35, 0.12, 0.04),
      makeMaterial(THREE, index === 0 ? "#ffb77d" : "#9ef7c6", {
        emissive: index === 0 ? "#ff8b52" : "#2b9c66",
        emissiveIntensity: 0.85,
      }),
    );
    row.position.set(-0.25 + index * 0.08, 3.85 - index * 0.57, 1.58);
    group.add(row);
  }
  addSectionPlaque(THREE, group, position, "EVENTS", "UTC community stage", "#ffb77d", 6);
  finishLandmark(group, "events", position, interactive);
  animated.push((time) => {
    board.material.emissive?.set?.("#2c1711");
    board.material.emissiveIntensity = 0.12 + Math.sin(time * 0.0012) * 0.05;
  });
  return group;
}

function createNeighborhood(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const houseColors = ["#8dbd77", "#6e9f82", "#9ac89c"];
  for (let index = 0; index < 3; index += 1) {
    const house = new THREE.Group();
    const body = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 2.5, 2.3, 2.4),
      makeMaterial(THREE, houseColors[index]),
    );
    body.position.y = 1.25;
    house.add(body);
    const roof = new THREE.Mesh(
      resources.primitive(THREE, "ConeGeometry", 2.1, 1.25, 4),
      makeSharedMaterial(THREE, "#315b45"),
    );
    roof.rotation.y = Math.PI / 4;
    roof.position.y = 3.05;
    house.add(roof);
    const door = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.62, 1.35, 0.08),
      makeMaterial(THREE, index === 0 ? "#b8e986" : "#263f35", {
        emissive: index === 0 ? "#5aa733" : "#000000",
        emissiveIntensity: index === 0 ? 0.52 : 0,
      }),
    );
    door.position.set(0, 0.83, -1.23);
    house.add(door);
    house.position.set((index - 1) * 3.3, 0, index === 1 ? 0.8 : 0);
    group.add(house);
  }

  addSectionPlaque(
    THREE,
    group,
    position,
    "NEIGHBORHOOD",
    "knock · visit · privacy",
    "#b8e986",
    6.4,
  );
  finishLandmark(group, "neighborhood", position, interactive);
  animated.push((time) => {
    group.rotation.y = Math.sin(time * 0.00008) * 0.025;
  });
  return group;
}

function createBroadcastGarden(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const garden = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 4.5, 4.9, 0.38, 16),
    makeSharedMaterial(THREE, "#24513f"),
  );
  garden.position.y = 0.19;
  group.add(garden);
  const speakerMaterial = makeMaterial(THREE, "#1b2d35", {
    roughness: 0.48,
  });
  for (const x of [-2.8, 2.8]) {
    const speaker = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1.15, 2.75, 1.05),
      speakerMaterial,
    );
    speaker.position.set(x, 1.55, 0.5);
    group.add(speaker);
    for (const y of [0.95, 1.9]) {
      const cone = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.33, 0.46, 0.12, 24),
        makeSharedMaterial(THREE, "#8fcfff", {
          emissive: "#387ba6",
          emissiveIntensity: 0.48,
        }),
      );
      cone.rotation.x = Math.PI / 2;
      cone.position.set(x, y, -0.05);
      group.add(cone);
    }
  }
  const screen = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 4.0, 2.35, 0.2),
    makeMaterial(THREE, "#8fcfff", {
      transparent: true,
      opacity: 0.62,
      emissive: "#2b6e9b",
      emissiveIntensity: 0.78,
    }),
  );
  screen.position.set(0, 2.3, 1.35);
  group.add(screen);
  const note = new THREE.Mesh(
    resources.primitive(THREE, "TorusGeometry", 0.72, 0.12, 10, 42, Math.PI * 1.55),
    makeSharedMaterial(THREE, "#9ef7c6", {
      emissive: "#39c783",
      emissiveIntensity: 1.0,
    }),
  );
  note.position.set(0, 2.35, 1.12);
  group.add(note);
  addSectionPlaque(
    THREE,
    group,
    position,
    "BROADCAST",
    "opt-in media garden",
    "#8fcfff",
    6,
  );
  finishLandmark(group, "broadcast", position, interactive);
  animated.push((time) => {
    note.rotation.z = Math.sin(time * 0.001) * 0.2;
    screen.material.opacity = 0.54 + Math.sin(time * 0.0015) * 0.1;
  });
  return group;
}
