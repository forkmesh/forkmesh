(() => {
  const config = window.__worldBenchConfig || {};
  const realNow = performance.now.bind(performance);
  const realRaf = window.requestAnimationFrame.bind(window);
  const clock = { mode: "live", shift: 0, frozen: 0 };
  const now = () => (clock.mode === "manual" ? clock.frozen : realNow() + clock.shift);
  performance.now = now;

  const fixedNow = Number(config.fixedNow) || Date.UTC(2026, 5, 21, 11, 0, 0);
  const RealDate = Date;
  const dateNow = () => fixedNow + Math.floor(now());
  function BenchDate(...args) {
    if (!new.target) return new RealDate(dateNow()).toString();
    return args.length ? new RealDate(...args) : new RealDate(dateNow());
  }
  BenchDate.prototype = RealDate.prototype;
  BenchDate.now = dateNow;
  BenchDate.parse = RealDate.parse;
  BenchDate.UTC = RealDate.UTC;
  window.Date = BenchDate;

  let state = (Number(config.seed) || 0x5eed1234) >>> 0;
  Math.random = () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  try {
    localStorage.clear();
    sessionStorage.setItem("forkmesh.world.guestId.v1", String(config.guestId || "world-bench-visitor"));
  } catch {}

  const pending = new Map();
  let nextId = 1;
  let scheduled = false;
  const frame = { calls: 0, triangles: 0, renders: 0, renderMs: 0 };
  const record = { on: false, frames: [], lastTs: 0 };
  const loafs = [];
  try {
    new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) loafs.push({ at: entry.startTime, ms: entry.duration });
    }).observe({ type: "long-animation-frame", buffered: true });
  } catch {}

  const runFrame = (ts) => {
    frame.calls = 0;
    frame.triangles = 0;
    frame.renders = 0;
    frame.renderMs = 0;
    const batch = [...pending.values()];
    pending.clear();
    for (const callback of batch) {
      try {
        callback(ts);
      } catch (error) {
        setTimeout(() => {
          throw error;
        });
      }
    }
  };
  const schedule = () => {
    if (scheduled || clock.mode !== "live") return;
    scheduled = true;
    realRaf(tick);
  };
  const tick = (realTs) => {
    scheduled = false;
    if (clock.mode !== "live") return;
    const start = realNow();
    runFrame(now());
    const work = realNow() - start;
    if (record.on && frame.renders > 0) {
      record.frames.push({
        work,
        render: frame.renderMs,
        interval: record.lastTs ? realTs - record.lastTs : 0,
      });
      record.lastTs = realTs;
    }
    if (pending.size) schedule();
  };
  window.requestAnimationFrame = (callback) => {
    const id = nextId++;
    pending.set(id, callback);
    schedule();
    return id;
  };
  window.cancelAnimationFrame = (id) => {
    pending.delete(id);
  };

  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const quantile = (values, q) => {
    if (!values.length) return 0;
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(q * (sorted.length - 1) + 0.5))];
  };
  const round = (value, places = 2) => {
    const scale = 10 ** places;
    return Math.round((Number(value) || 0) * scale) / scale;
  };

  const bench = {
    clock,
    frame,
    aim: null,
    world: () => document.querySelector("forkmesh-world")?.world || window.__world || null,

    freeze(at) {
      clock.frozen = at;
      clock.mode = "manual";
    },
    thaw() {
      clock.shift = clock.frozen - realNow();
      clock.mode = "live";
      schedule();
    },
    step(count, dt) {
      for (let index = 0; index < count; index += 1) {
        clock.frozen += dt;
        runFrame(clock.frozen);
      }
    },

    instrument(world) {
      const renderer = world.renderer;
      if (renderer.__benchWrapped) return;
      const camera = world.camera;
      const updateMatrixWorld = camera.updateMatrixWorld;
      let aiming = false;
      camera.updateMatrixWorld = function benchAimedMatrix(force) {
        if (bench.aim && !aiming) {
          aiming = true;
          try {
            this.position.set(...bench.aim.position);
            this.up.set(0, 1, 0);
            this.lookAt(...bench.aim.target);
          } finally {
            aiming = false;
          }
        }
        return updateMatrixWorld.call(this, force);
      };
      const render = renderer.render;
      renderer.render = function benchRender(scene, target) {
        if (bench.aim && scene === world.scene && target === world.camera) target.updateMatrixWorld(true);
        const start = realNow();
        const result = render.call(this, scene, target);
        frame.renderMs += realNow() - start;
        frame.calls += this.info.render.calls;
        frame.triangles += this.info.render.triangles;
        frame.renders += 1;
        return result;
      };
      renderer.__benchWrapped = true;
    },

    load() {
      const world = bench.world();
      bench.instrument(world);
      const measure = window.__forkmeshWorldBoot.measure;
      const long = measure.longFrames || [];
      return {
        firstFrameMs: round(measure.firstFrameMs, 1),
        interactiveMs: round(measure.interactiveMs, 1),
        interactiveFrameMs: round(measure.interactiveFrameMs, 1),
        loadLongTasks: long.length,
        loadLongTaskMs: round(long.reduce((sum, entry) => sum + (Number(entry.ms) || 0), 0), 1),
        shaderPrograms: measure.shader?.programs || 0,
        gpu: world.rendererGpu?.() || "",
      };
    },

    signature() {
      const world = bench.world();
      if (!world) return "";
      let objects = 0;
      world.scene.traverse(() => {
        objects += 1;
      });
      const info = world.renderer.info;
      return `${objects}/${info.memory.geometries}/${info.memory.textures}/${info.programs?.length || 0}/${frame.calls}`;
    },

    async place(pose) {
      const world = bench.world();
      world.setQualityTier?.(pose.qualityTier, { adaptive: false });
      world.setTheme?.("world");
      world.setDaylightMode?.("day");
      world.setLightLevel?.(100);
      world.setPaused?.(false);
      const place = pose.place;
      if (place.kind === "town") {
        if (world.isOfficeInterior?.()) world.leaveOfficeInterior?.();
        if (world.isBeachScene?.()) world.leaveBeachScene?.();
        world.setSpawn({ space: "town-square", x: place.at[0], z: place.at[2], heading: place.heading || 0 });
      } else if (place.kind === "office") {
        world.setOfficeAccess?.({ authenticated: true, account: "bench", allowedFloorIds: [place.floor] });
        world.setSpawn({ space: "town-square", x: 0, z: -168.8 });
        await wait(100);
        world.enterOfficeLobby();
        await wait(200);
        if (place.floor !== "lobby") world.enterOfficeLobby({ floorId: place.floor });
        await wait(200);
        world.player.position.set(...place.at);
      } else if (place.kind === "beach") {
        world.enterBeachScene();
        await wait(300);
      }
      const [px, py, pz] = pose.camera.position;
      const [tx, ty, tz] = pose.camera.target;
      const dx = tx - px;
      const dz = tz - pz;
      world.setCameraView?.({
        mode: "third-person",
        yaw: Math.atan2(-dx, -dz),
        pitch: Math.atan2(py - ty, Math.hypot(dx, dz)),
      });
      bench.aim = pose.camera;
      await wait(300);
      return world.getPosition();
    },

    audit() {
      try {
        const stats = bench.world().auditScene?.()?.stats || {};
        return {
          duplicateGeometries: stats.duplicateGeometries,
          duplicateMaterials: stats.duplicateMaterials,
          instancingCandidates: stats.instancingGroups,
          frustumCulledOff: stats.cullingOff,
          drawables: stats.drawables,
          visibleDrawables: stats.visibleDrawables,
          shadowCasters: stats.shadowCasters,
          textureMB: round(stats.textureMB, 1),
        };
      } catch (error) {
        return { error: String(error?.message || error).slice(0, 200) };
      }
    },

    async capture(pose, settings) {
      const world = bench.world();
      const hideMasked = () => {
        const hidden = [];
        for (const name of pose.mask) {
          const object = world.scene.getObjectByName(name);
          if (object) {
            object.visible = false;
            hidden.push(name);
          }
        }
        return hidden;
      };
      bench.aim = pose.camera;
      bench.freeze(settings.frozenAtMs);
      if (pose.place.kind === "office") world.player.position.set(...pose.place.at);
      hideMasked();
      bench.step(settings.freezeFrames, settings.frameDtMs);
      let signature = "";
      let settledFrames = 0;
      for (let attempt = 0; attempt < settings.freezeRounds; attempt += 1) {
        hideMasked();
        bench.step(settings.freezeFrames, settings.frameDtMs);
        settledFrames += settings.freezeFrames;
        const next = `${frame.calls}/${frame.triangles}`;
        if (next === signature) break;
        signature = next;
        await wait(50);
      }
      const masked = hideMasked();
      bench.step(1, settings.frameDtMs);
      const canvas = world.renderer.domElement;
      const shot = document.createElement("canvas");
      shot.width = settings.golden.width;
      shot.height = settings.golden.height;
      const context = shot.getContext("2d");
      context.imageSmoothingEnabled = true;
      context.imageSmoothingQuality = "high";
      context.drawImage(canvas, 0, 0, shot.width, shot.height);
      const golden = shot.toDataURL("image/png");
      const full = settings.fullShots ? canvas.toDataURL("image/png") : "";
      const info = world.renderer.info;
      const counts = {
        draws: frame.calls,
        triangles: frame.triangles,
        programs: info.programs?.length || 0,
        geometries: info.memory.geometries,
        textures: info.memory.textures,
      };
      const audit = bench.audit();
      for (const key of ["duplicateGeometries", "duplicateMaterials", "instancingCandidates", "frustumCulledOff"]) {
        if (Number.isFinite(audit[key])) counts[key] = audit[key];
      }
      return { masked, settledFrames, position: world.getPosition(), counts, audit, golden, full };
    },

    startRecording() {
      bench.thaw();
      record.frames = [];
      record.lastTs = 0;
      record.on = true;
      return realNow();
    },

    stopRecording(startedAt) {
      record.on = false;
      const frames = record.frames.slice();
      const work = frames.map((entry) => entry.work);
      const render = frames.map((entry) => entry.render);
      const intervals = frames.map((entry) => entry.interval).filter((value) => value > 0);
      return {
        frames: frames.length,
        renderMsP50: round(quantile(work, 0.5)),
        renderMsP90: round(quantile(work, 0.9)),
        submitMsP50: round(quantile(render, 0.5)),
        submitMsP90: round(quantile(render, 0.9)),
        frameIntervalP50: round(quantile(intervals, 0.5)),
        frameIntervalP90: round(quantile(intervals, 0.9)),
        longAnimationFrames: loafs.filter((entry) => entry.at >= startedAt && entry.ms >= 50).length,
      };
    },

    async compare(actual, baseline, tolerance) {
      const decode = async (url) => {
        const image = new Image();
        image.src = url;
        await image.decode();
        const canvas = document.createElement("canvas");
        canvas.width = image.naturalWidth;
        canvas.height = image.naturalHeight;
        const context = canvas.getContext("2d", { willReadFrequently: true });
        context.drawImage(image, 0, 0);
        return { canvas, data: context.getImageData(0, 0, canvas.width, canvas.height).data };
      };
      const a = await decode(actual);
      const b = await decode(baseline);
      if (a.canvas.width !== b.canvas.width || a.canvas.height !== b.canvas.height) {
        return { sameSize: false, changedPct: 100, maxDelta: 255, diff: "" };
      }
      const width = a.canvas.width;
      const height = a.canvas.height;
      const out = document.createElement("canvas");
      out.width = width;
      out.height = height;
      const outContext = out.getContext("2d");
      const diff = outContext.createImageData(width, height);
      const yiqMax = 35215;
      let changed = 0;
      let maxDelta = 0;
      for (let index = 0; index < a.data.length; index += 4) {
        const dr = a.data[index] - b.data[index];
        const dg = a.data[index + 1] - b.data[index + 1];
        const db = a.data[index + 2] - b.data[index + 2];
        const y = dr * 0.29889531 + dg * 0.58662247 + db * 0.11448223;
        const i = dr * 0.59597799 - dg * 0.2741761 - db * 0.32180189;
        const q = dr * 0.21147017 - dg * 0.52261711 + db * 0.31114694;
        const delta = Math.round(Math.sqrt((0.5053 * y * y + 0.299 * i * i + 0.1957 * q * q) / yiqMax) * 255);
        if (delta > maxDelta) maxDelta = delta;
        const gray = Math.round((b.data[index] * 0.3 + b.data[index + 1] * 0.59 + b.data[index + 2] * 0.11) * 0.35 + 160);
        if (delta > tolerance) {
          changed += 1;
          diff.data.set([255, 0, 40, 255], index);
        } else {
          diff.data.set([gray, gray, gray, 255], index);
        }
      }
      outContext.putImageData(diff, 0, 0);
      return {
        sameSize: true,
        changedPct: Math.round((changed / (width * height)) * 10000) / 100,
        maxDelta,
        diff: out.toDataURL("image/png"),
      };
    },
  };
  Object.defineProperty(window, "__worldBench", { value: bench });
})();
