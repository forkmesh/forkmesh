(() => {
  "use strict";

  const mounts = document.querySelectorAll("[data-forkmesh-auth-mesh]");
  if (!mounts.length) return;

  const MASK_SRC = "/assets/land-mask.png";
  const LATITUDE_TOP = 78;
  const LATITUDE_BOTTOM = -57;
  const LONGITUDE_CENTER = 4;
  const PITCH = 6;
  const RADIUS = 1.35;
  const SHADES = 6;
  const FLOOR = 0.34;
  const MAX_PIXELS = 16e6;

  const NODES = {
    sanFrancisco: { lat: 37.77, lon: -122.42, avatar: "/assets/kaif.webp", ping: 0 },
    toronto: { lat: 43.65, lon: -79.38, wide: true },
    newYork: { lat: 40.71, lon: -74.01 },
    mexicoCity: { lat: 19.43, lon: -99.13, wide: true },
    santiago: { lat: -33.45, lon: -70.67, avatar: "/assets/fediverse-avatar.png", ping: 1 },
    saoPaulo: { lat: -23.55, lon: -46.63 },
    london: { lat: 51.51, lon: -0.13, ping: 2 },
    frankfurt: { lat: 50.11, lon: 8.68 },
    warsaw: { lat: 52.23, lon: 21.01, wide: true },
    lagos: { lat: 6.52, lon: 3.38 },
    nairobi: { lat: -1.29, lon: 36.82, wide: true },
    capeTown: { lat: -33.92, lon: 18.42, wide: true },
    dubai: { lat: 25.2, lon: 55.27, wide: true },
    bengaluru: { lat: 12.97, lon: 77.59, avatar: "/assets/jett.webp", ping: 3 },
    singapore: { lat: 1.35, lon: 103.82 },
    perth: { lat: -31.95, lon: 115.86, wide: true },
    seoul: { lat: 37.57, lon: 126.98, wide: true },
    tokyo: { lat: 35.68, lon: 139.69, avatar: "/assets/logo.svg", mark: true, ping: 4 },
    sydney: { lat: -33.87, lon: 151.21, ping: 5 },
  };

  const PIN_PATH =
    "M12 .8A11.2 11.2 0 0 0 .8 12c0 3.6 1.9 7.4 4.3 10.8 2.4 3.4 5.2 6.3 6.1 " +
    "7.2a1.1 1.1 0 0 0 1.6 0c.9-.9 3.7-3.8 6.1-7.2C21.3 19.4 23.2 15.6 23.2 " +
    "12A11.2 11.2 0 0 0 12 .8Z";

  const SVG_NS = "http://www.w3.org/2000/svg";
  const RADIANS = Math.PI / 180;
  const mercator = (latitude) => Math.log(Math.tan(Math.PI / 4 + (latitude * RADIANS) / 2));
  const TOP = mercator(LATITUDE_TOP);
  const SPAN = TOP - mercator(LATITUDE_BOTTOM);
  const ASPECT = (2 * Math.PI) / SPAN;

  function position(node) {
    const x = ((((node.lon - LONGITUDE_CENTER) / 360 + 0.5) % 1) + 1) % 1;
    return [x * 100, ((TOP - mercator(node.lat)) / SPAN) * 100];
  }

  function place(element, node) {
    const [x, y] = position(node);
    element.style.left = `${x.toFixed(3)}%`;
    element.style.top = `${y.toFixed(3)}%`;
  }

  function hash(x, y) {
    let h = Math.imul(x, 0x27d4eb2d) ^ Math.imul(y, 0x165667b1);
    h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
    h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  }

  function noise(x, y) {
    const ix = Math.floor(x);
    const iy = Math.floor(y);
    const fx = x - ix;
    const fy = y - iy;
    const sx = fx * fx * (3 - 2 * fx);
    const sy = fy * fy * (3 - 2 * fy);
    const a = hash(ix, iy);
    const b = hash(ix + 1, iy);
    const c = hash(ix, iy + 1);
    const d = hash(ix + 1, iy + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
  }

  function loadMask() {
    return new Promise((resolve, reject) => {
      const image = new Image();
      image.decoding = "async";
      image.onload = () => {
        const width = image.naturalWidth;
        const height = image.naturalHeight;
        const scratch = document.createElement("canvas");
        scratch.width = width;
        scratch.height = height;
        const context = scratch.getContext("2d", { willReadFrequently: true });
        context.drawImage(image, 0, 0);
        const pixels = context.getImageData(0, 0, width, height).data;
        const land = new Uint8Array(width * height);
        for (let index = 0; index < land.length; index += 1) {
          land[index] = pixels[index * 4] > 127 ? 1 : 0;
        }
        resolve({ land, width, height });
      };
      image.onerror = reject;
      image.src = MASK_SRC;
    });
  }

  function paint(view, mask) {
    const { mesh, stage, canvas } = view;
    const frame = mesh.getBoundingClientRect();
    const box = stage.getBoundingClientRect();
    const width = frame.width;
    const height = frame.height;
    if (!width || !height || !box.width || !box.height) return;

    const scale = Math.min(window.devicePixelRatio || 1, 2, Math.sqrt(MAX_PIXELS / (width * height)));
    canvas.width = Math.round(width * scale);
    canvas.height = Math.round(height * scale);
    const context = canvas.getContext("2d");
    context.setTransform(scale, 0, 0, scale, 0, 0);
    context.clearRect(0, 0, width, height);

    const left = box.left - frame.left;
    const top = box.top - frame.top;
    const cells = mask.width / 360;
    const firstColumn = Math.max(0, Math.ceil(-left / PITCH - 0.5));
    const lastColumn = Math.min(
      Math.floor(box.width / PITCH - 0.5),
      Math.floor((width - left) / PITCH - 0.5),
    );
    const firstRow = Math.max(0, Math.ceil(-top / PITCH - 0.5));
    const lastRow = Math.min(
      Math.floor(box.height / PITCH - 0.5),
      Math.floor((height - top) / PITCH - 0.5),
    );

    const columns = [];
    for (let column = firstColumn; column <= lastColumn; column += 1) {
      const longitude = LONGITUDE_CENTER + (((column + 0.5) * PITCH) / box.width - 0.5) * 360;
      const wrapped = (((longitude + 180) % 360) + 360) % 360;
      columns.push(Math.min(mask.width - 1, Math.floor(wrapped * cells)));
    }

    const shades = Array.from({ length: SHADES }, () => new Path2D());
    for (let row = firstRow; row <= lastRow; row += 1) {
      const projected = TOP - (((row + 0.5) * PITCH) / box.height) * SPAN;
      const latitude = Math.atan(Math.sinh(projected)) / RADIANS;
      const offset = Math.min(mask.height - 1, Math.floor((90 - latitude) * cells)) * mask.width;
      const y = top + (row + 0.5) * PITCH;
      for (let index = 0; index < columns.length; index += 1) {
        if (!mask.land[offset + columns[index]]) continue;
        const column = firstColumn + index;
        const x = left + (column + 0.5) * PITCH;
        const tone = noise(column / 26, row / 26) * 0.72 + noise(column / 7, row / 7) * 0.28;
        const path = shades[Math.min(SHADES - 1, Math.floor(tone * SHADES))];
        path.moveTo(x + RADIUS, y);
        path.arc(x, y, RADIUS, 0, Math.PI * 2);
      }
    }

    context.fillStyle = getComputedStyle(mesh).getPropertyValue("--auth-mesh-dot").trim() || "#5c5c5c";
    shades.forEach((path, shade) => {
      context.globalAlpha = FLOOR + ((1 - FLOOR) * (shade + 1)) / SHADES;
      context.fill(path);
    });
    context.globalAlpha = 1;
    canvas.classList.add("is-drawn");
  }

  function buildNode(node) {
    const dot = document.createElement("span");
    dot.className = "auth-mesh-node";
    if (node.wide) dot.dataset.wide = "";
    if (node.ping !== undefined) {
      dot.classList.add("auth-mesh-node-ping");
      dot.style.setProperty("--auth-mesh-delay", `${node.ping * 0.8}s`);
    }
    place(dot, node);
    return dot;
  }

  function buildPin(node) {
    const pin = document.createElement("span");
    pin.className = "auth-mesh-pin";
    place(pin, node);
    const teardrop = document.createElementNS(SVG_NS, "svg");
    teardrop.setAttribute("viewBox", "0 0 24 32");
    teardrop.setAttribute("aria-hidden", "true");
    teardrop.setAttribute("focusable", "false");
    const outline = document.createElementNS(SVG_NS, "path");
    outline.setAttribute("d", PIN_PATH);
    teardrop.append(outline);
    const avatar = document.createElement("span");
    avatar.className = node.mark ? "auth-mesh-avatar auth-mesh-avatar-mark" : "auth-mesh-avatar";
    const image = document.createElement("img");
    image.src = node.avatar;
    image.alt = "";
    image.width = 64;
    image.height = 64;
    image.decoding = "async";
    avatar.append(image);
    pin.append(teardrop, avatar);
    return pin;
  }

  function mount(target) {
    const mesh = document.createElement("div");
    mesh.className = "auth-mesh";
    mesh.setAttribute("aria-hidden", "true");
    const canvas = document.createElement("canvas");
    canvas.className = "auth-mesh-map";
    const stage = document.createElement("div");
    stage.className = "auth-mesh-stage";
    stage.style.setProperty("--auth-mesh-aspect", ASPECT.toFixed(4));
    const nodes = Object.values(NODES);
    nodes.forEach((node) => stage.append(buildNode(node)));
    nodes.filter((node) => node.avatar).forEach((node) => stage.append(buildPin(node)));
    mesh.append(canvas, stage);
    target.replaceWith(mesh);
    return { mesh, stage, canvas };
  }

  const views = Array.from(mounts, mount);
  let mask = null;
  let queued = false;

  function schedule() {
    if (!mask || queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      views.forEach((view) => paint(view, mask));
    });
  }

  loadMask()
    .then((loaded) => {
      mask = loaded;
      schedule();
    })
    .catch(() => {});

  if ("ResizeObserver" in window) {
    const observer = new ResizeObserver(schedule);
    views.forEach((view) => observer.observe(view.mesh));
  } else {
    window.addEventListener("resize", schedule);
  }
  new MutationObserver(schedule).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["class"],
  });
})();
