import * as resources from "./resources.js";

export const LOCAL_ENVIRONMENT_OVERLAYS = Object.freeze({
  rain: {
    tint: "#5f7785",
    tintStrength: 0.2,
    lightMultiplier: 0.76,
    exposureMultiplier: 0.88,
  },
  snow: {
    tint: "#dcece9",
    tintStrength: 0.22,
    lightMultiplier: 1.04,
    exposureMultiplier: 1.02,
  },
  winter: {
    tint: "#b9ccca",
    tintStrength: 0.27,
    lightMultiplier: 0.94,
    exposureMultiplier: 0.96,
  },
  cyberpunk: {
    tint: "#35104c",
    tintStrength: 0.34,
    lightMultiplier: 0.9,
    exposureMultiplier: 0.9,
  },
  "low-light": {
    tint: "#07110f",
    tintStrength: 0.18,
    lightMultiplier: 0.64,
    exposureMultiplier: 0.7,
  },
});
// Linear haze band. Town (radius 365) stays crisp; the beach and remote
// districts beyond it soften into the horizon rather than vanish.
export const WORLD_FOG_NEAR = 280;
export const WORLD_FOG_FAR = 1600;
export const SKY_ENVIRONMENT_COLORS = Object.freeze({
  top: "#8fbfe0",
  horizon: "#e4f1f2",
  ground: "#56644a",
});
export const SKY_ENVIRONMENT_INTENSITY = 0.35;
export const SKY_ENVIRONMENT_HEMISPHERE_SHARE = 0.85;
export const DAYLIGHT_ENVIRONMENT = Object.freeze({
  // A deep teal night sky keeps the ground readable while letting the
  // deterministic stars, planets, and public-orbit satellites remain visible.
  background: "#06131d",
  hemiSky: "#d8fff1",
  hemiGround: "#25493a",
  sun: "#fff0bd",
  sunPower: 3.7,
  hemiPower: 2.1,
  exposure: 1.12,
});

function skyEnvironmentScene(THREE) {
  const envScene = new THREE.Scene();
  const dome = new THREE.SphereGeometry(10, 32, 16);
  const positions = dome.attributes.position;
  const colors = new Float32Array(positions.count * 3);
  const top = new THREE.Color(SKY_ENVIRONMENT_COLORS.top);
  const horizon = new THREE.Color(SKY_ENVIRONMENT_COLORS.horizon);
  const ground = new THREE.Color(SKY_ENVIRONMENT_COLORS.ground);
  const shade = new THREE.Color();
  for (let index = 0; index < positions.count; index += 1) {
    const y = positions.getY(index) / 10;
    if (y >= 0) shade.copy(horizon).lerp(top, y ** 0.6);
    else shade.copy(horizon).lerp(ground, Math.min(1, -y * 3));
    shade.toArray(colors, index * 3);
  }
  dome.setAttribute("color", new THREE.BufferAttribute(colors, 3));
  const domeMaterial = resources.uniqueMaterial(THREE, {
    vertexColors: true,
    side: THREE.BackSide,
  }, "MeshBasicMaterial");
  envScene.add(new THREE.Mesh(dome, domeMaterial));
  return { envScene, dome, domeMaterial };
}

export function prepareSkyEnvironment(THREE, renderer) {
  const prepared = { ...skyEnvironmentScene(THREE), pmrem: new THREE.PMREMGenerator(renderer) };
  const { pmrem } = prepared;
  if (typeof pmrem._setSize !== "function" || typeof pmrem._allocateTargets !== "function") {
    return prepared;
  }
  const output = renderer.getRenderTarget();
  let target = null;
  try {
    pmrem._setSize(256);
    target = pmrem._allocateTargets();
    renderer.setRenderTarget(target);
    renderer.compile(prepared.envScene, new THREE.PerspectiveCamera(90, 1, 0.04, 100));
    pmrem._backgroundBox ??= new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry"),
      resources.uniqueMaterial(THREE, {
        name: "PMREM.Background",
        side: THREE.BackSide,
        depthWrite: false,
        depthTest: false,
      }, "MeshBasicMaterial"),
    );
    renderer.compile(pmrem._backgroundBox, new THREE.PerspectiveCamera(90, 1, 0.04, 100));
    const lod = pmrem._lodMeshes?.[0];
    const flat = new THREE.OrthographicCamera();
    [pmrem._blurMaterial, pmrem._ggxMaterial].forEach((material) => {
      if (lod?.geometry && material) renderer.compile(new THREE.Mesh(lod.geometry, material), flat);
    });
  } catch (_) {
    return prepared;
  } finally {
    renderer.setRenderTarget(output);
    target?.dispose?.();
  }
  return prepared;
}

export function skyEnvironmentProgramsReady(renderer) {
  const programs = renderer?.info?.programs || [];
  for (let index = 0; index < programs.length; index += 1) {
    const program = programs[index];
    if (typeof program?.isReady === "function" && !program.isReady()) return false;
  }
  return true;
}

export function bakeSkyEnvironment(THREE, renderer, prepared = null) {
  const { envScene, dome, domeMaterial, pmrem } = prepared || {
    ...skyEnvironmentScene(THREE),
    pmrem: new THREE.PMREMGenerator(renderer),
  };
  try {
    const environment = pmrem.fromScene(envScene, 0.04);
    const outputTarget = renderer.getRenderTarget();
    renderer.setRenderTarget(environment);
    try {
      pmrem.compileCubemapShader();
    } finally {
      renderer.setRenderTarget(outputTarget);
    }
    return { environment, pmrem };
  } catch (_) {
    pmrem.dispose();
    return null;
  } finally {
    dome.dispose();
    domeMaterial.dispose();
  }
}
