// three.js links a shader program the first time a material is drawn and reads
// its uniforms straight away. Those reads are synchronous round trips to the
// GPU process, which answers only after every command queued ahead of them,
// every shader still compiling included. That wait froze the first render for
// over a second and cost 100-600ms whenever an object came into view while the
// idle warm-up had shaders in flight. KHR_parallel_shader_compile lets a
// program say whether it is done without a round trip, so a main-view draw
// whose program is compiling, or would be first used behind other programs
// that are, is skipped for that frame. It appears once the queue is clear,
// which also lets a fresh view appear in one piece instead of object by
// object. A draw is never held longer than PROGRAM_FIRST_USE_WAIT_MS behind
// other programs. Shadow passes, cube captures and environment bakes render
// into targets that are not redrawn every frame; they keep the blocking
// behaviour (isDeferrable returns false for them). Without the extension
// isReady() is always true and nothing changes.
const PROGRAM_PENDING = Symbol("forkmesh.programPending");
const GUARDED_PROGRAM = Symbol("forkmesh.guardedProgram");
const PROGRAM_FIRST_USE_WAIT_MS = 1_500;
const PROGRAM_FIRST_USE_FRAME_BUDGET_MS = 24;
export function warmMaterialsAsync(
  renderer,
  root,
  camera,
  scene,
  schedule = (callback) => setTimeout(callback, 10),
) {
  const pending = renderer.compile(root, camera, scene);
  return new Promise((resolve) => {
    const poll = () => {
      pending.forEach((material) => {
        const program = renderer.properties.get(material)?.currentProgram;
        if (!program || !program.program || program.isReady()) {
          pending.delete(material);
        }
      });
      if (pending.size === 0) {
        resolve(root);
        return;
      }
      schedule(poll);
    };
    poll();
  });
}
export function installShaderCompileGuard(
  renderer,
  isDeferrable,
  now = () => performance.now(),
) {
  let deferring = false;
  let warming = false;
  let warmVisit = null;
  let warmDeadline = Infinity;
  let warmSkipDrawn = false;
  let warmDrawn = new WeakMap();
  let hookedPrograms = null;
  let budgetFrame = -1;
  let budgetSpentMs = 0;
  const compiling = new Set();
  const unused = new Set();
  const pendingPrograms = () => {
    compiling.forEach((program) => {
      // A released program (its material disposed mid-compile) never
      // reports ready; three.js clears .program when it deletes it.
      if (!program.program || program.isReady()) compiling.delete(program);
    });
    return compiling.size;
  };
  const stats = {
    get warming() {
      return warming;
    },
    deferredDraws: 0,
    deferredShadowDraws: 0,
    budgetDeferredDraws: 0,
    pendingPrograms,
    compiledPrograms: 0,
    firstUseMs: 0,
    primedPrograms: 0,
    warmedDraws: 0,
    warmComplete: true,
    warm(render, visit = null, budgetMs = 0, resume = false) {
      warmVisit = visit;
      warming = true;
      warmSkipDrawn = resume === true;
      if (!warmSkipDrawn) warmDrawn = new WeakMap();
      warmDeadline = budgetMs > 0 ? now() + budgetMs : Infinity;
      stats.warmComplete = true;
      try {
        render();
      } finally {
        warming = false;
        warmVisit = null;
        warmSkipDrawn = false;
        warmDeadline = Infinity;
      }
      return stats.warmComplete;
    },
    primeReadyPrograms(budgetMs = 8) {
      const startedAt = now();
      let waiting = pendingPrograms();
      if (waiting > 0) return waiting;
      for (const program of [...unused]) {
        if (!program.program) {
          unused.delete(program);
          continue;
        }
        if (!program.isReady()) {
          waiting += 1;
          continue;
        }
        if (now() - startedAt >= budgetMs) {
          waiting += 1;
          continue;
        }
        program.getUniforms();
        stats.primedPrograms += 1;
      }
      return waiting;
    },
  };
  const guardProgram = (program) => {
    if (!program || program[GUARDED_PROGRAM]) return;
    program[GUARDED_PROGRAM] = true;
    unused.add(program);
    if (!program.isReady()) compiling.add(program);
    const getUniforms = program.getUniforms;
    let heldSince = -1;
    program.getUniforms = function guardedGetUniforms() {
      if (warming) throw PROGRAM_PENDING;
      if (deferring) {
        const ready = this.isReady();
        if (ready) compiling.delete(this);
        if (heldSince < 0) heldSince = now();
        if (
          !ready ||
          (pendingPrograms() > 0 && now() - heldSince < PROGRAM_FIRST_USE_WAIT_MS)
        ) {
          throw PROGRAM_PENDING;
        }
        const frame = Number(renderer.info?.render?.frame) || 0;
        if (frame !== budgetFrame) {
          budgetFrame = frame;
          budgetSpentMs = 0;
        } else if (budgetSpentMs >= PROGRAM_FIRST_USE_FRAME_BUDGET_MS) {
          stats.budgetDeferredDraws += 1;
          throw PROGRAM_PENDING;
        }
      }
      // The first use happens once; later draws skip the guard entirely.
      compiling.delete(this);
      unused.delete(this);
      this.getUniforms = getUniforms;
      const firstUseAt = now();
      const uniforms = getUniforms.call(this);
      const spent = now() - firstUseAt;
      stats.compiledPrograms += 1;
      stats.firstUseMs += spent;
      budgetSpentMs += spent;
      return uniforms;
    };
  };
  // info.programs is three.js's own program list; every new program is
  // pushed onto it. A restored context starts a fresh list.
  const hookPrograms = () => {
    const programs = renderer.info?.programs;
    if (!Array.isArray(programs) || programs === hookedPrograms) return;
    hookedPrograms = programs;
    compiling.clear();
    programs.forEach(guardProgram);
    const push = programs.push;
    programs.push = function pushGuardedPrograms(...items) {
      items.forEach(guardProgram);
      return push.apply(this, items);
    };
  };
  hookPrograms();
  const renderBufferDirect = renderer.renderBufferDirect;
  renderer.renderBufferDirect = function guardedRenderBufferDirect(
    camera,
    scene,
    geometry,
    material,
    object,
    group,
  ) {
    if (warming) {
      const drawnKey = object && typeof object === "object" ? object : null;
      let drawn = drawnKey ? warmDrawn.get(drawnKey) : null;
      if (warmSkipDrawn && drawn?.has(material)) return undefined;
      if (warmDeadline !== Infinity && now() >= warmDeadline) {
        stats.warmComplete = false;
        return undefined;
      }
      if (drawnKey) {
        if (!drawn) warmDrawn.set(drawnKey, (drawn = new Set()));
        drawn.add(material);
      }
      hookPrograms();
      warmVisit?.(material, object);
      try {
        return renderBufferDirect.call(
          this, camera, scene, geometry, material, object, group,
        );
      } catch (error) {
        if (error !== PROGRAM_PENDING) throw error;
        stats.warmedDraws += 1;
        return undefined;
      }
    }
    if (!isDeferrable(camera, scene)) {
      return renderBufferDirect.call(
        this, camera, scene, geometry, material, object, group,
      );
    }
    hookPrograms();
    deferring = true;
    try {
      return renderBufferDirect.call(
        this, camera, scene, geometry, material, object, group,
      );
    } catch (error) {
      // Thrown before the program is bound or any draw state is set.
      if (error !== PROGRAM_PENDING) throw error;
      stats.deferredDraws += 1;
      if (scene === null) stats.deferredShadowDraws += 1;
      return undefined;
    } finally {
      deferring = false;
    }
  };
  return stats;
}
