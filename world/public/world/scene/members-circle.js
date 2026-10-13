import { hashNumber } from "../world-avatar-face.js";
import { canvasArtTexture, canvasTexture } from "./canvas.js";
import { embossPlankText } from "./labels.js";
import { MEMBER_ENGAGEMENT_BY_ID } from "./member-engagement.js";
import * as resources from "./resources.js";

// Branded onto the plank itself (the seat's top face texture) instead of a
// floating sign, so an empty bench still says whose seat it is: the account
// it belongs to, and whether they are sitting on it or out walking the world.
// An unclaimed seat reads as the open guest bench. Canvas aspect (480x180)
// matches the seat top's width:depth ratio (1.6:0.6) so the wood grain and
// lettering aren't stretched.
export function drawCampfireSeatPlate(
  context,
  name,
  state = "seated",
  x = 0,
  y = 0,
  width = 480,
  height = 180,
  engagementId = "",
) {
  const label = String(name || "").trim().slice(0, 18);
  const away = state === "away";
  const seated = state === "seated";
  const engagement = MEMBER_ENGAGEMENT_BY_ID.get(String(engagementId || ""));
  const glow = label
    ? engagement?.accent ||
      (away ? "#ffd479" : seated ? "#9ef7c6" : "#d6d9d8")
    : "#77d9ff";
  context.save();
  context.translate(x, y);
  context.scale(width / 480, height / 180);
  context.fillStyle = engagement?.bench || "#8a5a33";
  context.fillRect(0, 0, 480, 180);
  context.strokeStyle = "rgba(63,38,17,0.35)";
  context.lineWidth = 2;
  for (let grain = 20; grain < 180; grain += 24) {
    context.beginPath();
    context.moveTo(0, grain);
    context.bezierCurveTo(120, grain - 5, 360, grain + 5, 480, grain);
    context.stroke();
  }
  context.textAlign = "center";
  context.textBaseline = "middle";
  embossPlankText(
    context,
    label || "OPEN SEAT",
    240,
    74,
    '700 52px "ForkMesh Favorit", system-ui, sans-serif',
    glow,
  );
  embossPlankText(
    context,
    label
      ? [
          engagement?.label?.toUpperCase(),
          away ? "OUT AND ABOUT" : seated ? "AT THE FIRE" : "ROSTER BENCH",
        ]
          .filter(Boolean)
          .join(" · ")
      : "GUESTS WELCOME",
    240,
    124,
    '400 24px "ForkMesh Mono", ui-monospace, monospace',
    glow,
  );
  context.restore();
}

export function campfireDirtTexture(THREE) {
  const texture = canvasArtTexture(THREE, 1024, 1024, (context, canvas) => {
    const gradient = context.createRadialGradient(512, 470, 40, 512, 512, 720);
    gradient.addColorStop(0, "#77502f");
    gradient.addColorStop(0.58, "#684326");
    gradient.addColorStop(1, "#4b301d");
    context.fillStyle = gradient;
    context.fillRect(0, 0, canvas.width, canvas.height);
    let seed = 0x46_4d_44_49;
    const random = () => {
      seed = (Math.imul(seed, 1_664_525) + 1_013_904_223) >>> 0;
      return seed / 0x1_0000_0000;
    };
    for (let index = 0; index < 3600; index += 1) {
      const x = random() * canvas.width;
      const y = random() * canvas.height;
      const radius = 0.7 + random() * 3.4;
      context.fillStyle =
        random() > 0.5
          ? `rgba(218,164,101,${0.035 + random() * 0.12})`
          : `rgba(37,22,13,${0.035 + random() * 0.11})`;
      context.beginPath();
      context.arc(x, y, radius, 0, Math.PI * 2);
      context.fill();
    }
    context.strokeStyle = "rgba(40,24,13,0.18)";
    context.lineWidth = 3;
    for (let ring = 0; ring < 18; ring += 1) {
      const x = 70 + random() * 884;
      const y = 70 + random() * 884;
      context.beginPath();
      context.ellipse(
        x,
        y,
        12 + random() * 42,
        3 + random() * 10,
        random() * Math.PI,
        0,
        Math.PI * 2,
      );
      context.stroke();
    }
  });
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.repeat.set(2.4, 2.4);
  texture.anisotropy = 8;
  return texture;
}

// Each lifecycle band gets its own tactile read as well as a colour. The
// patterns are deliberately procedural and tiny: ten repeatable 256px maps
// cost less than shipping a ground atlas and remain crisp as the circle grows.
export function memberEngagementBandTexture(THREE, engagement) {
  const texture = canvasArtTexture(THREE, 256, 256, (context, canvas) => {
    context.fillStyle = engagement.ground;
    context.fillRect(0, 0, canvas.width, canvas.height);
    let seed = hashNumber(`member-band:${engagement.id}`) || 1;
    const random = () => {
      seed = (Math.imul(seed, 1_664_525) + 1_013_904_223) >>> 0;
      return seed / 0x1_0000_0000;
    };
    context.strokeStyle = engagement.accent;
    context.fillStyle = engagement.accent;
    context.globalAlpha = 0.24;
    context.lineCap = "round";

    if (engagement.texture === "trail") {
      // A single, continuous track reads as progress toward the fire rather
      // than the scattered footprints used by the Casual lifecycle band.
      context.lineWidth = 4;
      context.beginPath();
      context.moveTo(-12, 228);
      context.bezierCurveTo(54, 178, 82, 88, 142, 112);
      context.bezierCurveTo(196, 134, 208, 46, 274, 18);
      context.stroke();
      context.globalAlpha = 0.5;
      for (let index = 0; index < 12; index += 1) {
        const progress = index / 11;
        const x = 12 + progress * 244;
        const y =
          218 - progress * 182 + Math.sin(progress * Math.PI * 4) * 24;
        context.save();
        context.translate(x, y);
        context.rotate(-0.42 + Math.cos(progress * Math.PI * 4) * 0.16);
        context.beginPath();
        context.ellipse(
          index % 2 ? 5 : -5,
          0,
          3.5,
          8,
          0,
          0,
          Math.PI * 2,
        );
        context.fill();
        context.restore();
      }
    } else if (engagement.texture === "embers") {
      context.lineWidth = 3;
      for (let index = 0; index < 24; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        context.beginPath();
        context.moveTo(x, y);
        context.lineTo(x + (random() - 0.5) * 32, y + random() * 18);
        context.lineTo(x + (random() - 0.5) * 42, y + 20 + random() * 20);
        context.stroke();
      }
      context.globalAlpha = 0.55;
      for (let index = 0; index < 42; index += 1) {
        context.beginPath();
        context.arc(
          random() * 256,
          random() * 256,
          1 + random() * 2.4,
          0,
          Math.PI * 2,
        );
        context.fill();
      }
    } else if (engagement.texture === "sprouts") {
      context.lineWidth = 2.5;
      for (let index = 0; index < 34; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        context.beginPath();
        context.moveTo(x, y + 8);
        context.quadraticCurveTo(x - 2, y, x - 8, y - 5);
        context.moveTo(x, y + 8);
        context.quadraticCurveTo(x + 2, y, x + 8, y - 5);
        context.stroke();
      }
    } else if (engagement.texture === "kindling") {
      context.lineWidth = 4;
      for (let offset = -256; offset < 512; offset += 34) {
        context.beginPath();
        context.moveTo(offset, 256);
        context.lineTo(offset + 256, 0);
        context.stroke();
      }
    } else if (engagement.texture === "footprints") {
      for (let index = 0; index < 28; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        context.beginPath();
        context.ellipse(x - 4, y - 5, 3, 7, 0, 0, Math.PI * 2);
        context.ellipse(x + 5, y + 6, 3, 7, 0, 0, Math.PI * 2);
        context.fill();
      }
    } else if (engagement.texture === "leaves") {
      for (let index = 0; index < 38; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        context.beginPath();
        context.ellipse(
          x,
          y,
          3 + random() * 5,
          1.5 + random() * 2,
          random() * Math.PI,
          0,
          Math.PI * 2,
        );
        context.fill();
      }
    } else if (engagement.texture === "pebbles") {
      context.lineWidth = 2;
      for (let index = 0; index < 52; index += 1) {
        context.beginPath();
        context.ellipse(
          random() * 256,
          random() * 256,
          2 + random() * 5,
          1.5 + random() * 3,
          random() * Math.PI,
          0,
          Math.PI * 2,
        );
        context.stroke();
      }
    } else if (engagement.texture === "unlit") {
      context.lineWidth = 2;
      context.setLineDash([3, 9]);
      for (let line = 16; line < 256; line += 32) {
        context.beginPath();
        context.moveTo(line, 0);
        context.lineTo(line, 256);
        context.stroke();
      }
    } else if (engagement.texture === "crosses") {
      context.lineWidth = 3;
      for (let index = 0; index < 30; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        const size = 4 + random() * 5;
        context.beginPath();
        context.moveTo(x - size, y - size);
        context.lineTo(x + size, y + size);
        context.moveTo(x + size, y - size);
        context.lineTo(x - size, y + size);
        context.stroke();
      }
    } else if (engagement.texture === "portraits") {
      context.lineWidth = 3;
      for (let index = 0; index < 18; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        context.beginPath();
        context.arc(x, y, 9, 0, Math.PI * 2);
        context.stroke();
        context.beginPath();
        context.arc(x, y + 2, 4, 0.2, Math.PI - 0.2);
        context.stroke();
      }
    } else {
      context.lineWidth = 2.5;
      for (let index = 0; index < 22; index += 1) {
        const x = random() * 256;
        const y = random() * 256;
        context.beginPath();
        context.moveTo(x - 18, y + 10);
        context.lineTo(x, y);
        context.lineTo(x + 8, y - 18);
        context.moveTo(x, y);
        context.lineTo(x + 18, y + 12);
        context.stroke();
      }
    }
    context.globalAlpha = 1;
  });
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.repeat.set(4, 4);
  texture.anisotropy = 8;
  return texture;
}

const MEMBER_BAND_LETTERING_PPU = 56;
const MEMBER_BAND_LETTERING_FONT = '800 {size}px "ForkMesh Favorit", system-ui, sans-serif';

export function memberBandLetteringText(spec, count) {
  const total = Math.max(0, Math.round(Number(count) || 0));
  return `${String(spec?.label || "").toUpperCase()}  ${total}`;
}

export function memberBandLetteringHeight(band) {
  return Math.max(0.3, Math.min(0.78, (band.outerRadius - band.innerRadius) * 0.5));
}

function memberBandLetteringLayout(entries) {
  const measure = document.createElement("canvas").getContext("2d");
  let halfWidth = 1;
  let top = 1;
  const placed = entries.map((entry) => {
    const radius = (entry.band.innerRadius + entry.band.outerRadius) / 2;
    const height = memberBandLetteringHeight(entry.band);
    const size = height * MEMBER_BAND_LETTERING_PPU;
    measure.font = MEMBER_BAND_LETTERING_FONT.replace("{size}", size.toFixed(1));
    const chars = [...entry.text].map((char) => ({
      char,
      width: measure.measureText(char).width + size * 0.06,
    }));
    const length = chars.reduce((sum, char) => sum + char.width, 0);
    const halfAngle = Math.min(Math.PI * 0.95, length / 2 / (radius * MEMBER_BAND_LETTERING_PPU));
    halfWidth = Math.max(
      halfWidth,
      (halfAngle >= Math.PI / 2 ? radius : radius * Math.sin(halfAngle)) + height,
    );
    top = Math.max(top, radius + height);
    return { ...entry, radius, size, chars, length, halfAngle };
  });
  return {
    placed,
    xMin: -halfWidth,
    width: halfWidth * 2,
    yMin: -top,
    height: top * 2,
  };
}

export function memberBandLetteringTexture(THREE, entries) {
  const layout = memberBandLetteringLayout(entries);
  const ppu = MEMBER_BAND_LETTERING_PPU;
  const width = Math.ceil(layout.width * ppu);
  const height = Math.ceil(layout.height * ppu);
  const scale = Math.min(1, 2048 / width, 2048 / height);
  const texture = canvasArtTexture(THREE, width, height, (context) => {
    context.fillStyle = "#000000";
    context.fillRect(0, 0, width, height);
    const cx = -layout.xMin * ppu;
    const cy = height / 2;
    context.textAlign = "center";
    context.textBaseline = "middle";
    const letter = (entry) => {
      const radius = entry.radius * ppu;
      context.font = MEMBER_BAND_LETTERING_FONT.replace("{size}", entry.size.toFixed(1));
      context.lineJoin = "round";
      let angle = -Math.PI / 2 + entry.length / 2 / radius;
      entry.chars.forEach(({ char, width: advance }) => {
        const middle = angle - advance / 2 / radius;
        context.save();
        context.translate(cx + radius * Math.cos(middle), cy + radius * Math.sin(middle));
        context.rotate(middle - Math.PI / 2);
        context.lineWidth = entry.size * 0.16;
        context.strokeStyle = "#5a5a5a";
        context.strokeText(char, 0, 0);
        context.fillStyle = "#ffffff";
        context.fillText(char, 0, 0);
        context.restore();
        angle -= advance / radius;
      });
    };
    [0, Math.PI].forEach((turn) => {
      context.save();
      context.translate(cx, cy);
      context.rotate(turn);
      context.translate(-cx, -cy);
      layout.placed.forEach(letter);
      context.restore();
    });
  }, 1, scale);
  texture.colorSpace = THREE.NoColorSpace;
  texture.wrapS = THREE.ClampToEdgeWrapping;
  texture.wrapT = THREE.ClampToEdgeWrapping;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.generateMipmaps = true;
  texture.anisotropy = 8;
  texture.userData.letteringRect = {
    xMin: layout.xMin,
    width: width / ppu,
    yMin: -height / 2 / ppu,
    height: height / ppu,
  };
  return texture;
}

export function memberBandLetteringFor(base, outerRadius) {
  const rect = base.userData.letteringRect;
  const texture = base.clone();
  texture.userData = { letteringRect: rect };
  texture.repeat.set((2 * outerRadius) / rect.width, (2 * outerRadius) / rect.height);
  texture.offset.set(
    (-outerRadius - rect.xMin) / rect.width,
    (-outerRadius - rect.yMin) / rect.height,
  );
  return texture;
}

export function createCampfireBurningLogs(THREE) {
  const group = new THREE.Group();
  group.name = "campfire-charred-burning-logs";
  const matrix = new THREE.Matrix4();
  const rotation = new THREE.Euler();
  const quaternion = new THREE.Quaternion();
  const position = new THREE.Vector3();
  const scale = new THREE.Vector3(1, 1, 1);
  const logs = new THREE.InstancedMesh(
    resources.primitive(THREE, "CylinderGeometry", 0.105, 0.12, 1.18, 10),
    resources.uniqueMaterial(THREE, {
      color: "#ffffff",
      roughness: 1,
      metalness: 0.08,
    }, "MeshStandardMaterial"),
    3,
  );
  logs.name = "campfire-charred-logs";
  const coalShape = resources.primitive(THREE, "IcosahedronGeometry", 1, 1).clone();
  const coalGlow = new Float32Array(9 * 4);
  coalShape.setAttribute("aCoal", new THREE.InstancedBufferAttribute(coalGlow, 4));
  const coalUniforms = {
    uCoalClock: { value: 0 },
    uCoalSpark: { value: 0 },
  };
  const coalMaterial = resources.uniqueMaterial(THREE, {
    color: "#ffffff",
    roughness: 0.78,
    metalness: 0.08,
    emissive: "#ffffff",
    emissiveIntensity: 1,
  }, "MeshStandardMaterial");
  coalMaterial.onBeforeCompile = (shader) => {
    shader.uniforms.uCoalClock = coalUniforms.uCoalClock;
    shader.uniforms.uCoalSpark = coalUniforms.uCoalSpark;
    shader.vertexShader = shader.vertexShader
      .replace("#include <common>", "#include <common>\nattribute vec4 aCoal;\nvarying vec4 vCoal;")
      .replace("#include <begin_vertex>", "#include <begin_vertex>\nvCoal = aCoal;");
    shader.fragmentShader = shader.fragmentShader
      .replace(
        "#include <common>",
        "#include <common>\nuniform float uCoalClock;\nuniform float uCoalSpark;\nvarying vec4 vCoal;",
      )
      .replace(
        "#include <emissivemap_fragment>",
        "totalEmissiveRadiance = vCoal.rgb * (2.15 + sin(uCoalClock + vCoal.a) * 0.55 + uCoalSpark * 0.18);",
      );
  };
  coalMaterial.customProgramCacheKey = () => "campfire-glowing-coals";
  const coals = new THREE.InstancedMesh(coalShape, coalMaterial, 9);
  coals.name = "campfire-glowing-coals";
  const tint = new THREE.Color();
  for (let index = 0; index < 3; index += 1) {
    const logY = 0.31 + (index % 2) * 0.07;
    const logAngle = (index / 3) * Math.PI + 0.08;
    rotation.set((index - 1) * 0.08, logAngle, Math.PI / 2);
    quaternion.setFromEuler(rotation);
    position.set(0, logY, 0);
    scale.set(1, 1, 1);
    logs.setMatrixAt(index, matrix.compose(position, quaternion, scale));
    logs.setColorAt(index, tint.set(index === 1 ? "#21140e" : "#2b1810"));
    for (let coalIndex = 0; coalIndex < 3; coalIndex += 1) {
      const slot = index * 3 + coalIndex;
      const along = (coalIndex - 1) * 0.31;
      const radius = 0.075 + coalIndex * 0.012;
      position.set(
        Math.cos(logAngle) * along,
        logY + 0.09 + (coalIndex % 2) * 0.025,
        -Math.sin(logAngle) * along,
      );
      quaternion.identity();
      scale.set(1.35 * radius, 0.52 * radius, 0.75 * radius);
      coals.setMatrixAt(slot, matrix.compose(position, quaternion, scale));
      coals.setColorAt(slot, tint.set(coalIndex === 1 ? "#ff6a17" : "#b62d0b"));
      tint.set(coalIndex === 1 ? "#ff4d0a" : "#8e1605");
      coalGlow[slot * 4] = tint.r;
      coalGlow[slot * 4 + 1] = tint.g;
      coalGlow[slot * 4 + 2] = tint.b;
      coalGlow[slot * 4 + 3] = index * 2.4 + coalIndex;
    }
  }
  logs.computeBoundingSphere();
  coals.computeBoundingSphere();
  group.add(logs, coals);
  group.userData.coalUniforms = coalUniforms;
  return group;
}

export function createProceduralCampfireEffect(THREE) {
  const effect = new THREE.Group();
  effect.name = "campfire-procedural-fire";

  const flameVertexShader = `
    uniform float uTime;
    attribute vec4 aFlame;
    attribute vec4 aShape;
    attribute vec2 aOffset;
    varying vec2 vUv;
    varying float vTime;
    varying float vSpeed;
    varying float vPhase;
    varying float vOpacity;
    void main() {
      vUv = uv;
      float time = uTime + aShape.w * 0.17;
      float speed = aFlame.x;
      float phase = aFlame.y;
      float curl = aFlame.z;
      vTime = time;
      vSpeed = speed;
      vPhase = phase;
      vOpacity = aFlame.w;
      float lift = pow(uv.y, 1.45);
      float broadSway = sin(time * speed + phase) * curl;
      float curling = sin(time * speed * 1.73 + uv.y * 8.0 + phase * 2.1)
        * curl * 0.42;
      float sway = (broadSway + curling) * lift;
      float rise = sin(time * speed * 1.31 + phase + uv.y * 5.0)
        * 0.035 * lift;
      float x = position.x + sway;
      vec3 transformed = vec3(
        x * aShape.x + aOffset.x,
        (position.y + rise) * aShape.z,
        x * aShape.y + aOffset.y
      );
      gl_Position = projectionMatrix * modelViewMatrix * vec4(transformed, 1.0);
    }
  `;
  const flameFragmentShader = `
    varying vec2 vUv;
    varying float vTime;
    varying float vSpeed;
    varying float vPhase;
    varying float vOpacity;

    float hash(vec2 p) {
      p = fract(p * vec2(123.34, 456.21));
      p += dot(p, p + 45.32);
      return fract(p.x * p.y);
    }
    float noise(vec2 p) {
      vec2 i = floor(p);
      vec2 f = fract(p);
      f = f * f * (3.0 - 2.0 * f);
      return mix(
        mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0)), f.x),
        f.y
      );
    }
    float fbm(vec2 p) {
      float value = 0.0;
      float amplitude = 0.55;
      for (int octave = 0; octave < 4; octave++) {
        value += amplitude * noise(p);
        p = p * 2.03 + 17.17;
        amplitude *= 0.5;
      }
      return value;
    }
    void main() {
      float t = vTime * vSpeed;
      float y = vUv.y;
      float x = vUv.x * 2.0 - 1.0;
      float turbulence = fbm(vec2(y * 3.35 - t * 0.72, vPhase));
      float fineNoise = fbm(vec2(x * 2.8 + vPhase, y * 5.4 - t * 1.18));
      float center = (turbulence - 0.5) * (0.25 + y * 0.44)
        + sin(y * 9.0 - t * 1.45 + vPhase) * 0.075 * y;
      float width = mix(0.72, 0.035, pow(y, 0.72));
      width *= 0.82 + turbulence * 0.42;
      width += (fineNoise - 0.5) * (0.10 + y * 0.19);

      // A moving notch near the tip makes tall tongues fork before they close.
      float fork = smoothstep(0.57, 0.9, y)
        * (1.0 - smoothstep(0.0, 0.18, abs(x - center)))
        * (0.35 + 0.65 * noise(vec2(floor(t * 2.0), vPhase)));
      float edgeDistance = abs(x - center) / max(width, 0.025);
      float body = 1.0 - smoothstep(0.67, 1.0, edgeDistance);
      body *= 1.0 - fork * smoothstep(0.68, 0.98, y);

      float raggedTop = 0.9
        + (fbm(vec2(x * 2.1 + vPhase, -t * 1.3)) - 0.5) * 0.24;
      float topFade = 1.0 - smoothstep(raggedTop - 0.12, raggedTop, y);
      float alpha = body * topFade * smoothstep(0.0, 0.1, y);
      alpha *= 0.74 + fineNoise * 0.32;
      alpha *= vOpacity;
      if (alpha < 0.008) discard;

      float heat = clamp(edgeDistance, 0.0, 1.0);
      vec3 whiteCore = mix(vec3(1.0, 0.72, 0.16), vec3(1.0, 0.98, 0.72),
        (1.0 - heat) * (1.0 - y * 0.38));
      vec3 orangeMiddle = vec3(1.0, 0.23, 0.012);
      vec3 redEdge = vec3(0.48, 0.018, 0.002);
      vec3 color = mix(whiteCore, orangeMiddle, smoothstep(0.16, 0.62, heat));
      color = mix(color, redEdge, smoothstep(0.62, 1.0, heat));
      color *= 0.88 + fineNoise * 0.3;
      gl_FragColor = vec4(color, alpha);
    }
  `;

  const flameGeometry = new THREE.PlaneGeometry(1, 1, 18, 28);
  flameGeometry.translate(0, 0.5, 0);
  const flameLayers = [
    {
      width: 1.35,
      height: 2.25,
      angle: 0.1,
      speed: 1.72,
      phase: 0.7,
      opacity: 0.72,
    },
    {
      width: 1.2,
      height: 2,
      angle: Math.PI / 2,
      speed: 2.13,
      phase: 2.8,
      opacity: 0.68,
    },
    {
      width: 1,
      height: 1.72,
      angle: Math.PI / 4,
      speed: 2.58,
      phase: 4.4,
      opacity: 0.78,
    },
    {
      width: 0.82,
      height: 1.48,
      angle: -Math.PI / 4,
      speed: 3.04,
      phase: 6.1,
      opacity: 0.84,
    },
    {
      width: 0.58,
      height: 1.17,
      angle: 1.16,
      speed: 3.47,
      phase: 8.3,
      opacity: 0.9,
    },
  ];
  const flameMaterial = new THREE.ShaderMaterial({
    uniforms: {
      uTime: { value: 0 },
    },
    vertexShader: flameVertexShader,
    fragmentShader: flameFragmentShader,
    transparent: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    forceSinglePass: true,
    blending: THREE.AdditiveBlending,
    toneMapped: false,
  });
  const flameMaterials = [flameMaterial];
  const layerCount = flameLayers.length;
  const flameAttribute = new Float32Array(layerCount * 4);
  const flameShape = new Float32Array(layerCount * 4);
  const flameOffset = new Float32Array(layerCount * 2);
  flameLayers.forEach((layer, index) => {
    flameAttribute[index * 4] = layer.speed;
    flameAttribute[index * 4 + 1] = layer.phase;
    flameAttribute[index * 4 + 2] = 0.13 + index * 0.018;
    flameAttribute[index * 4 + 3] = layer.opacity;
    flameShape[index * 4] = Math.cos(layer.angle) * layer.width;
    flameShape[index * 4 + 1] = -Math.sin(layer.angle) * layer.width;
    flameShape[index * 4 + 2] = layer.height;
    flameShape[index * 4 + 3] = index;
    flameOffset[index * 2] = Math.sin(layer.phase) * 0.09;
    flameOffset[index * 2 + 1] = Math.cos(layer.phase) * 0.09;
  });
  const flameMerged = new THREE.InstancedBufferGeometry();
  flameMerged.setIndex(flameGeometry.index);
  flameMerged.setAttribute("position", flameGeometry.attributes.position);
  flameMerged.setAttribute("uv", flameGeometry.attributes.uv);
  flameMerged.setAttribute("aFlame", new THREE.InstancedBufferAttribute(flameAttribute, 4));
  flameMerged.setAttribute("aShape", new THREE.InstancedBufferAttribute(flameShape, 4));
  flameMerged.setAttribute("aOffset", new THREE.InstancedBufferAttribute(flameOffset, 2));
  flameMerged.instanceCount = layerCount;
  flameMerged.boundingSphere = new THREE.Sphere(new THREE.Vector3(0, 1.15, 0), 1.7);
  const flames = new THREE.Mesh(flameMerged, flameMaterial);
  flames.name = "campfire-flame-layers";
  flames.renderOrder = 4;
  effect.add(flames);

  const emberCount = 34;
  const emberPositions = new Float32Array(emberCount * 3);
  const emberSeeds = new Float32Array(emberCount);
  const emberSizes = new Float32Array(emberCount);
  let emberRandomState = 0x46_49_52_45;
  const emberRandom = () => {
    emberRandomState =
      (Math.imul(emberRandomState, 1_664_525) + 1_013_904_223) >>> 0;
    return emberRandomState / 0x1_0000_0000;
  };
  for (let index = 0; index < emberCount; index += 1) {
    const offset = index * 3;
    emberPositions[offset] = (emberRandom() - 0.5) * 0.65;
    emberPositions[offset + 1] = emberRandom() * 0.3;
    emberPositions[offset + 2] = (emberRandom() - 0.5) * 0.65;
    emberSeeds[index] = emberRandom();
    emberSizes[index] = 1.5 + emberRandom() * 3.3;
  }
  const emberGeometry = new THREE.BufferGeometry();
  emberGeometry.setAttribute(
    "position",
    new THREE.BufferAttribute(emberPositions, 3),
  );
  emberGeometry.setAttribute(
    "aSeed",
    new THREE.BufferAttribute(emberSeeds, 1),
  );
  emberGeometry.setAttribute(
    "aSize",
    new THREE.BufferAttribute(emberSizes, 1),
  );
  const emberMaterial = new THREE.ShaderMaterial({
    uniforms: {
      uTime: { value: 0 },
      uHeight: { value: 1 },
    },
    vertexShader: `
      uniform float uTime;
      uniform float uHeight;
      attribute float aSeed;
      attribute float aSize;
      varying float vAlpha;
      void main() {
        float age = fract(uTime * (0.12 + aSeed * 0.09) + aSeed * 7.31);
        vec3 ember = position;
        float rise = age * (1.65 + aSeed * 1.75) * uHeight;
        ember.y += rise;
        ember.x += sin(age * 8.0 + aSeed * 31.0) * (0.08 + age * 0.36);
        ember.z += cos(age * 6.7 + aSeed * 27.0) * (0.07 + age * 0.28);
        vec4 viewPosition = modelViewMatrix * vec4(ember, 1.0);
        gl_Position = projectionMatrix * viewPosition;
        float life = smoothstep(0.0, 0.08, age) * (1.0 - smoothstep(0.58, 1.0, age));
        gl_PointSize = aSize * life * (95.0 / max(1.0, -viewPosition.z));
        vAlpha = life;
      }
    `,
    fragmentShader: `
      varying float vAlpha;
      void main() {
        float radius = length(gl_PointCoord - 0.5) * 2.0;
        float alpha = (1.0 - smoothstep(0.18, 1.0, radius)) * vAlpha;
        if (alpha < 0.01) discard;
        vec3 color = mix(vec3(1.0, 0.16, 0.01), vec3(1.0, 0.88, 0.38),
          1.0 - radius);
        gl_FragColor = vec4(color, alpha);
      }
    `,
    transparent: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
    toneMapped: false,
  });
  const embers = new THREE.Points(emberGeometry, emberMaterial);
  embers.name = "campfire-rising-embers";
  embers.position.y = 0.12;
  embers.renderOrder = 10;
  effect.add(embers);

  const smokeCount = 7;
  const smokeCorners = [[-0.5, 0.5], [0.5, 0.5], [-0.5, -0.5], [0.5, -0.5]];
  const smokeCorner = new Float32Array(smokeCount * 4 * 3);
  const smokeUv = new Float32Array(smokeCount * 4 * 2);
  const smokePuff = new Float32Array(smokeCount * 4 * 4);
  const smokeIndex = [];
  for (let index = 0; index < smokeCount; index += 1) {
    smokeCorners.forEach(([x, y], corner) => {
      const vertex = index * 4 + corner;
      smokeCorner.set([x, y, 0], vertex * 3);
      smokeUv.set([x + 0.5, y + 0.5], vertex * 2);
      smokePuff.set([
        index / smokeCount,
        index * 1.91,
        (index * Math.PI) / 3.5,
        index * 2.37,
      ], vertex * 4);
    });
    const base = index * 4;
    smokeIndex.push(base, base + 2, base + 1, base + 2, base + 3, base + 1);
  }
  const smokeGeometry = new THREE.BufferGeometry();
  smokeGeometry.setAttribute("position", new THREE.BufferAttribute(smokeCorner, 3));
  smokeGeometry.setAttribute("uv", new THREE.BufferAttribute(smokeUv, 2));
  smokeGeometry.setAttribute("aPuff", new THREE.BufferAttribute(smokePuff, 4));
  smokeGeometry.setIndex(smokeIndex);
  smokeGeometry.boundingSphere = new THREE.Sphere(new THREE.Vector3(0, 3.2, 0), 3.6);
  const smokeMaterial = new THREE.ShaderMaterial({
    uniforms: {
      uTime: { value: 0 },
      uGrowth: { value: 1 },
    },
    vertexShader: `
      uniform float uTime;
      uniform float uGrowth;
      attribute vec4 aPuff;
      varying vec2 vUv;
      varying float vPhase;
      varying float vOpacity;
      void main() {
        vUv = uv;
        float seed = aPuff.x;
        float phase = aPuff.y;
        float age = fract(uTime * (0.09 + seed * 0.025) + seed);
        float size = 0.42 + age * (1.15 + seed * 0.55);
        vec2 corner = position.xy * vec2(size, size * 1.18);
        float roll = sin(uTime * 0.21 + phase) * 0.3;
        vec2 rolled = vec2(
          corner.x * cos(roll) - corner.y * sin(roll),
          corner.x * sin(roll) + corner.y * cos(roll)
        );
        float turn = aPuff.z;
        vec3 local = vec3(rolled.x * cos(turn), rolled.y, -rolled.x * sin(turn));
        vec3 center = vec3(
          sin(uTime * 0.54 + phase) * age * 0.48,
          1.55 + age * (2.15 + uGrowth * 0.45),
          cos(uTime * 0.39 + phase * 1.4) * age * 0.32
        );
        float fadeIn = clamp(age / 0.18, 0.0, 1.0);
        float fadeOut = 1.0 - clamp((age - 0.48) / 0.52, 0.0, 1.0);
        vOpacity = 0.105 * fadeIn * fadeIn * fadeOut * fadeOut;
        vPhase = aPuff.w;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(center + local, 1.0);
      }
    `,
    fragmentShader: `
      varying vec2 vUv;
      varying float vPhase;
      varying float vOpacity;
      float hash(vec2 p) {
        return fract(sin(dot(p, vec2(127.1, 311.7)) + vPhase) * 43758.5453);
      }
      void main() {
        vec2 p = (vUv - 0.5) * 2.0;
        float angle = atan(p.y, p.x);
        float uneven = 0.78 + hash(vec2(floor(angle * 5.0), vPhase)) * 0.22;
        float alpha = (1.0 - smoothstep(0.2, uneven, length(p))) * vOpacity;
        if (alpha < 0.006) discard;
        gl_FragColor = vec4(vec3(0.24, 0.22, 0.2), alpha);
      }
    `,
    transparent: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    forceSinglePass: true,
    blending: THREE.NormalBlending,
    toneMapped: false,
  });
  const smoke = new THREE.Mesh(smokeGeometry, smokeMaterial);
  smoke.name = "campfire-smoke-puffs";
  effect.add(smoke);

  const groundGlow = new THREE.Mesh(
    resources.primitive(THREE, "CircleGeometry", 3.5, 48),
    new THREE.ShaderMaterial({
      uniforms: { uOpacity: { value: 0.34 } },
      vertexShader: `
        varying vec2 vUv;
        void main() {
          vUv = uv;
          gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }
      `,
      fragmentShader: `
        uniform float uOpacity;
        varying vec2 vUv;
        void main() {
          float radius = length(vUv - 0.5) * 2.0;
          float alpha = (1.0 - smoothstep(0.05, 1.0, radius));
          alpha *= alpha * uOpacity;
          if (alpha < 0.004) discard;
          gl_FragColor = vec4(1.0, 0.19, 0.015, alpha);
        }
      `,
      transparent: true,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
      toneMapped: false,
    }),
  );
  groundGlow.name = "campfire-ground-glow";
  groundGlow.rotation.x = -Math.PI / 2;
  groundGlow.position.y = -0.245;
  groundGlow.renderOrder = 2;
  effect.add(groundGlow);

  effect.userData.flameMaterials = flameMaterials;
  effect.userData.emberMaterial = emberMaterial;
  effect.userData.smokeMaterial = smokeMaterial;
  effect.userData.groundGlow = groundGlow;
  return effect;
}

// Who joined last, by the public directory's joined timestamp. An account
// seated straight from a presence frame carries no joined date yet (0), so it
// is skipped rather than ranked as the oldest member in the circle.
export function newestMemberName(members) {
  let newest = "";
  let joinedAt = 0;
  (Array.isArray(members) ? members : []).forEach((member) => {
    const name = String(member?.name || "").trim();
    const created = Number(member?.createdAt) || 0;
    if (!name || created <= joinedAt) return;
    joinedAt = created;
    newest = name.slice(0, 32);
  });
  return newest;
}

export function membersCircleInfoTexture(THREE, total, newest = "", counts = {}) {
  const count = Math.max(0, Math.min(999999, Math.round(Number(total) || 0)));
  const latest = String(newest || "").trim().slice(0, 18);
  const stateCount = (id) =>
    Math.max(0, Math.min(999999, Math.round(Number(counts?.[id]) || 0)));
  return canvasTexture(THREE, 1024, 512, (context) => {
    const gradient = context.createLinearGradient(0, 0, 0, 512);
    gradient.addColorStop(0, "#724620");
    gradient.addColorStop(1, "#3d2412");
    context.fillStyle = gradient;
    context.fillRect(0, 0, 1024, 512);
    context.strokeStyle = "#d8aa62";
    context.lineWidth = 20;
    context.strokeRect(16, 16, 992, 480);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.shadowColor = "rgba(0,0,0,0.72)";
    context.shadowBlur = 10;
    context.font = '900 62px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffe6a6";
    context.fillText("MEMBERS CENTER", 512, 72);
    context.font = '900 96px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffffff";
    context.fillText(
      `${count.toLocaleString()} ${count === 1 ? "MEMBER" : "MEMBERS"}`,
      512,
      174,
    );
    context.font = '800 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffd27a";
    context.fillText(
      latest ? `LATEST · ${latest.toUpperCase()}` : "LATEST · WELCOME IN",
      512,
      270,
    );
    context.shadowBlur = 5;
    context.font = '750 23px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffc071";
    context.fillText(
      [
        `👣 WORLD STEP ${stateCount("world-step")}`,
        `🔥 POWER ${stateCount("power")}`,
        `ENGAGED ${stateCount("engaged")}`,
        `NEW ${stateCount("new")}`,
      ].join(" · "),
      512,
      356,
    );
    context.fillStyle = "#bfe8f8";
    context.fillText(
      [
        `CASUAL ${stateCount("casual")}`,
        `COOLING ${stateCount("cooling")}`,
        `DORMANT ${stateCount("dormant")}`,
        `NEVER ${stateCount("never-activated")}`,
        `CHURNED ${stateCount("churned")} 🧊`,
      ].join(" · "),
      512,
      404,
    );
    context.font = '750 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffb7b7";
    context.fillText(
      `EMAIL UNVERIFIED 7D+ ${stateCount("unverified")}`,
      512,
      448,
    );
    context.font = '650 16px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#fff0cf";
    context.fillText("ONE WORLD STEP SITS CLOSEST", 512, 482);
  });
}
