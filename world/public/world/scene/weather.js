import {
  WEATHER_FIELD_HEIGHT,
  WEATHER_FIELD_POINTS,
  WEATHER_FIELD_SPAN,
} from "./office-building.js";

export function createWeather(THREE, scene) {
  const count = WEATHER_FIELD_POINTS;
  const positions = new Float32Array(count * 3);
  for (let index = 0; index < count; index += 1) {
    positions[index * 3] = (Math.random() - 0.5) * WEATHER_FIELD_SPAN;
    positions[index * 3 + 1] = Math.random() * WEATHER_FIELD_HEIGHT;
    positions[index * 3 + 2] = (Math.random() - 0.5) * WEATHER_FIELD_SPAN;
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  const rain = new THREE.Points(
    geometry,
    new THREE.PointsMaterial({
      color: "#9edfff",
      size: 0.07,
      transparent: true,
      opacity: 0.55,
    }),
  );
  rain.visible = false;
  rain.userData.kind = "rain";
  // The points are re-placed in world space rather than moved as a group, so
  // the bounding sphere Three.js caches from their first frame would cull the
  // whole field away as soon as the visitor walked out of the middle.
  rain.frustumCulled = false;
  scene.add(rain);

  const snow = new THREE.Points(
    geometry.clone(),
    new THREE.PointsMaterial({
      color: "#ffffff",
      size: 0.18,
      transparent: true,
      opacity: 0.75,
    }),
  );
  snow.visible = false;
  snow.userData.kind = "snow";
  snow.frustumCulled = false;
  scene.add(snow);
  return { rain, snow, anchor: new THREE.Vector3(0, 0, 0) };
}

// A quality tier changes weather through BufferGeometry.drawRange. Keep the
// CPU work in the traveling field and the animation loop on that same prefix,
// rather than continuing to update particles that will not be drawn.
export function weatherDrawCount(points) {
  const allocated = points.geometry.attributes.position.array.length / 3;
  const range = points.geometry.drawRange?.count;
  return Number.isFinite(range)
    ? Math.max(0, Math.min(allocated, range))
    : allocated;
}

// Fold one axis of a drop back into the span-wide column centred on `anchor`.
// Wrapping (rather than translating the field) keeps every drop in world
// space, so walking through rain looks like walking through rain instead of
// dragging a cloud of points along with the avatar.
function wrapWeatherAxis(value, anchor, span) {
  const offset = value - anchor;
  if (offset >= -span / 2 && offset <= span / 2) return value;
  return value - span * Math.round(offset / span);
}

export function followWeather(points, anchor) {
  if (!points.visible || !points.parent) return;
  const positions = points.geometry.attributes.position.array;
  const floor = anchor.y;
  const drawn = weatherDrawCount(points);
  let moved = false;
  for (let index = 0; index < drawn * 3; index += 3) {
    const x = wrapWeatherAxis(positions[index], anchor.x, WEATHER_FIELD_SPAN);
    const z = wrapWeatherAxis(
      positions[index + 2],
      anchor.z,
      WEATHER_FIELD_SPAN,
    );
    // The column follows the visitor upward too, so weather still surrounds
    // someone riding a jetpack or standing on high ground.
    const y = wrapWeatherAxis(
      positions[index + 1],
      floor + WEATHER_FIELD_HEIGHT / 2,
      WEATHER_FIELD_HEIGHT,
    );
    if (
      x === positions[index] &&
      y === positions[index + 1] &&
      z === positions[index + 2]
    ) {
      continue;
    }
    positions[index] = x;
    positions[index + 1] = y;
    positions[index + 2] = z;
    moved = true;
  }
  if (moved) points.geometry.attributes.position.needsUpdate = true;
}

export function animateWeather(points, time, delta, kind, anchor) {
  if (!points.visible) return;
  const positions = points.geometry.attributes.position.array;
  const speed = kind === "rain" ? 15 : 1.4;
  // Integrate only the range that is actually drawn. The tail of the buffer
  // stays allocated so a quality step is a draw-range write instead of a
  // reallocation, and it costs nothing per frame while it is unused.
  const floor = anchor.y;
  const drawn = weatherDrawCount(points);
  for (let index = 0; index < drawn; index += 1) {
    positions[index * 3 + 1] -= delta * speed;
    if (kind === "snow") {
      positions[index * 3] += Math.sin(time * 0.001 + index) * delta * 0.35;
      positions[index * 3 + 2] += Math.cos(time * 0.0008 + index) * delta * 0.25;
    }
    if (positions[index * 3 + 1] < floor) {
      positions[index * 3 + 1] = floor + WEATHER_FIELD_HEIGHT;
    }
  }
  points.geometry.attributes.position.needsUpdate = true;
}
