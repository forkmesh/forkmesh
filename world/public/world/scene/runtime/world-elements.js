import {
  loadStoreElementModule,
  normalizeElementParams,
} from "../../elements/index.js";
import { markWorldHTTPFailure, withWorldBackoff } from "../../world-backoff.js";
import { objectIsEffectivelyVisible } from "../object-utils.js";
import { ELEMENT_PART_LIMIT, SCENE_OBJECT_WALK_LIMIT } from "../quality.js";

export function functions({
  THREE,
  bootTrace,
  ctx,
  fns,
}) {
  // Purchased store elements are plugins from ./elements: nothing in the world
  // bundle knows their geometry, so they are built on demand and registered
  // through the same element registry as built-in content. That gives them the
  // Elements toggle, the diagnostics cost breakdown and disable-on-load for
  // free.
  const storeElementInstances = new Map();
  const deletedWorldObjectNodes = new Set();
  // Keys picked in this session, so a menu action deletes the node the visitor
  // actually pointed at rather than re-walking indices that a sibling deletion
  // may have shifted underneath it.
  const pickedWorldObjects = new Map();


  function worldElementEnabled(id) {
    return !ctx.disabledWorldElements.has(id);
  }

  function detachElementRoot(element, root) {
    if (!root || element.detached.has(root)) return;
    const parent = root.parent || null;
    const interactives = [];
    root.traverse?.((child) => {
      let index = ctx.interactive.indexOf(child);
      while (index >= 0) {
        interactives.push(child);
        ctx.interactive.splice(index, 1);
        index = ctx.interactive.indexOf(child);
      }
    });
    parent?.remove?.(root);
    element.detached.set(root, { parent, interactives });
  }

  function attachElementRoot(element, root) {
    const record = element.detached.get(root);
    if (!record) return;
    element.detached.delete(root);
    // A dynamic root (remote avatar, node cabinet) may have despawned while
    // it sat detached; its liveness probe keeps it from being resurrected.
    const live = element.liveness.get(root);
    if (live && live() !== true) {
      element.roots.delete(root);
      element.liveness.delete(root);
      return;
    }
    record.parent?.add?.(root);
    // An avatar that sat detached was swept out of the crowd batch, so its
    // legs, shoes and antenna rejoin it as it comes back into the game.
    ctx.avatarPartInstances.register(root);
    record.interactives.forEach((child) => {
      if (!ctx.interactive.includes(child)) ctx.interactive.push(child);
    });
  }

  function registerWorldElement(id, label, category, roots = [], live = null) {
    bootTrace?.sceneElement?.(id, label, roots);
    let element = ctx.worldElements.get(id);
    if (!element) {
      element = {
        id,
        label,
        category,
        roots: new Set(),
        detached: new Map(),
        liveness: new Map(),
        onToggle: null,
        systemOnly: false,
      };
      ctx.worldElements.set(id, element);
    }
    // Dynamic roots churn: node cabinets rebuild on every changed poll
    // record, visitor avatars despawn, the portal ring is replaced per
    // catalog reload. Pruning used to happen only from the diagnostics
    // panel, so every replaced root — with its canvases and liveness
    // closure — stayed in this registry for the life of the page (heap
    // profiling showed 13 dead cabinets per mirror node and ~200MB of
    // retained canvas backing stores). Every registration is a churn
    // point, so sweep the element's dead roots here.
    pruneDeadElementRoots(element);
    (Array.isArray(roots) ? roots : [roots])
      .filter(Boolean)
      .forEach((root) => {
        if (element.roots.has(root)) return;
        element.roots.add(root);
        root.traverse?.((child) => {
          if (child.isPointLight) ctx.localPointLights.add(child);
        });
        if (typeof live === "function") element.liveness.set(root, live);
        if (ctx.disabledWorldElements.has(id)) detachElementRoot(element, root);
      });
    // A piece an administrator deleted before the last reload comes out again
    // as soon as the element that owns it exists.
    applyStoredObjectDeletions(element);
    return element;
  }

  function registerWorldElementSystem(id, label, category, onToggle = null) {
    const element = registerWorldElement(id, label, category);
    element.systemOnly = true;
    element.onToggle = typeof onToggle === "function" ? onToggle : null;
    if (ctx.disabledWorldElements.has(id)) element.onToggle?.(false);
    return element;
  }

  function pruneDeadElementRoots(element) {
    element.roots.forEach((root) => {
      // A root an administrator right-click deleted is parentless on purpose
      // and must keep its registration, or restoring it would hand back a
      // root no element owns any more.
      if (deletedWorldObjectNodes.has(root)) return;
      const live = element.liveness.get(root);
      const detachedHere = element.detached.has(root);
      // Externally removed (a despawned avatar) or reported dead: forget it.
      if ((!detachedHere && !root.parent) || (live && live() !== true)) {
        element.roots.delete(root);
        element.detached.delete(root);
        element.liveness.delete(root);
      }
    });
  }

  function setWorldElementEnabled(id, enabled) {
    ctx.invalidateStaticMirrors();
    const element = ctx.worldElements.get(String(id || ""));
    if (!element) return false;
    const on = enabled !== false;
    if (on) ctx.disabledWorldElements.delete(element.id);
    else ctx.disabledWorldElements.add(element.id);
    pruneDeadElementRoots(element);
    element.roots.forEach((root) => {
      if (on) attachElementRoot(element, root);
      else detachElementRoot(element, root);
    });
    element.onToggle?.(on);
    ctx.officeFarProxyDirty = true;
    // The shadow atlas refreshes on a slow cadence; rebuild it now so a
    // removed caster's shadow does not linger on the ground.
    if (ctx.renderer.shadowMap.enabled) ctx.renderer.shadowMap.needsUpdate = true;
    return true;
  }

  function listWorldElements() {
    const interactiveSet = new Set(ctx.interactive);
    return [...ctx.worldElements.values()].map((element) => {
      pruneDeadElementRoots(element);
      let objects = 0;
      let drawables = 0;
      let triangles = 0;
      let interactives = 0;
      element.roots.forEach((root) => {
        root.traverse?.((child) => {
          objects += 1;
          if (interactiveSet.has(child)) interactives += 1;
          if (
            !child.isMesh &&
            !child.isPoints &&
            !child.isLine &&
            !child.isSprite
          ) {
            return;
          }
          if (child.userData?.raycastProxy === true) return;
          drawables += 1;
          const geometry = child.geometry;
          const vertices =
            geometry?.index?.count ??
            geometry?.attributes?.position?.count ??
            0;
          if (child.isMesh) triangles += Math.floor(vertices / 3);
        });
        interactives += element.detached.get(root)?.interactives?.length || 0;
      });
      return {
        id: element.id,
        label: element.label,
        category: element.category,
        enabled: worldElementEnabled(element.id),
        system: element.systemOnly === true,
        objects,
        drawables,
        triangles,
        interactives,
        parts: elementPartRoots(element).length,
      };
    });
  }

  // An element may only read the endpoints its manifest declared, which the
  // store disclosed before purchase. Anything else is refused here rather than
  // trusted to the element.
  function storeElementFetcher(manifest) {
    const allowed = new Set(manifest.network || []);
    return async (path) => {
      const target = String(path || "");
      if (!allowed.has(target)) {
        throw new Error(`${manifest.id} may not read ${target}`);
      }
      // An element owns its own refresh loop, so a purchased plugin polling a
      // failing endpoint is exactly the case the shared cooldown exists for.
      return withWorldBackoff(
        ctx.storeElementBackoff,
        `GET:${target}`,
        async () => {
          const response = await fetch(target, {
            credentials: "same-origin",
            headers: { accept: "application/json" },
          });
          if (!response.ok) {
            throw markWorldHTTPFailure(
              new Error(`${target} failed`),
              response,
            );
          }
          return response.json();
        },
      );
    };
  }

  async function installStoreElement(spec) {
    const id = String(spec?.id || "");
    if (!id) return false;
    removeStoreElement(id);
    const module = await loadStoreElementModule(id);
    const manifest = module.manifest;
    const params = normalizeElementParams(manifest, spec?.params);
    const instance = module.build({
      THREE,
      params,
      net: storeElementFetcher(manifest),
    });
    const group = instance?.group;
    if (!group) throw new Error(`store element ${id} built no group`);
    const placement = spec?.placement || {};
    group.name = `store-element-${id}`;
    group.position.set(Number(placement.x) || 0, 0, Number(placement.z) || 0);
    group.rotation.y = Number(placement.heading) || 0;
    ctx.world.add(group);
    storeElementInstances.set(id, instance);
    registerWorldElement(
      id,
      manifest.label || id,
      manifest.category || "Store",
      group,
      () => storeElementInstances.get(id) === instance,
    );
    if (spec?.enabled === false) setWorldElementEnabled(id, false);
    return true;
  }

  function removeStoreElement(id) {
    const instance = storeElementInstances.get(id);
    if (!instance) return false;
    storeElementInstances.delete(id);
    instance.group?.parent?.remove?.(instance.group);
    instance.dispose?.();
    ctx.worldElements.delete(id);
    return true;
  }

  function tickStoreElements(nowMs) {
    storeElementInstances.forEach((instance, id) => {
      if (typeof instance.tick !== "function") return;
      if (!worldElementEnabled(id)) return;
      try {
        instance.tick(nowMs);
      } catch {
        // One bad plugin frame must not stop the world; the element keeps its
        // last drawn state and the next frame tries again.
      }
    });
  }

  // An element row answers "what does this feature cost". Expanding it walks
  // one level further in: the cabinets inside Mirror node cabinets, the desks
  // and screens inside Office interior, the portals on the ring — and each of
  // those expands again, all the way down to a single mesh.
  //
  // A lone wrapper Group is scaffolding rather than a part, so the first level
  // opens straight into its children; elements registered with many roots (one
  // per cabinet, one per avatar) list those roots instead.
  function elementPartRoots(element) {
    const roots = [...element.roots];
    if (roots.length === 1 && roots[0]?.children?.length) {
      return [...roots[0].children];
    }
    return roots;
  }

  // Parts are addressed by child index from the element down, which stays
  // valid for as long as the subtree does. A churned root (a rebuilt cabinet,
  // a despawned avatar) simply resolves to whatever now sits at that index, or
  // to nothing — the panel re-reads on every render, so it never shows counts
  // for a node that is gone.
  function elementPartNodes(element, path) {
    let nodes = elementPartRoots(element);
    for (const index of path) {
      const node = nodes[index];
      if (!node) return [];
      nodes = node.children || [];
    }
    return nodes;
  }

  function elementPartKind(node) {
    if (node.isInstancedMesh) return "Instanced";
    if (node.isSkinnedMesh) return "Skinned";
    if (node.isMesh) return "Mesh";
    if (node.isPoints) return "Points";
    if (node.isLine) return "Line";
    if (node.isSprite) return "Sprite";
    if (node.isLight) return "Light";
    if (node.isCamera) return "Camera";
    if (node.children?.length) return "Group";
    return node.type || "Object";
  }

  // Most pieces of the town are unnamed meshes, so name them by what they are
  // and where they sit: "BoxGeometry #3" beats three identical "Mesh" rows.
  function elementPartLabel(node, index) {
    if (node.name) return node.name;
    const kind = node.isMesh
      ? node.geometry?.type || elementPartKind(node)
      : elementPartKind(node);
    return `${kind} #${index + 1}`;
  }

  // Counts use the same rules as listWorldElements — instanced batches count
  // once — so the numbers on a part row always add up to the row above it.
  function describeElementPart(node, index, path, interactiveSet) {
    let objects = 0;
    let drawables = 0;
    let triangles = 0;
    let interactives = 0;
    node.traverse?.((child) => {
      objects += 1;
      if (interactiveSet.has(child)) interactives += 1;
      if (!child.isMesh && !child.isPoints && !child.isLine && !child.isSprite) {
        return;
      }
      if (child.userData?.raycastProxy === true) return;
      drawables += 1;
      const geometry = child.geometry;
      const vertices =
        geometry?.index?.count ?? geometry?.attributes?.position?.count ?? 0;
      if (child.isMesh) triangles += Math.floor(vertices / 3);
    });
    return {
      path: [...path, index],
      label: elementPartLabel(node, index),
      type: elementPartKind(node),
      geometry: node.geometry?.type || "",
      instances: node.isInstancedMesh ? Math.max(0, Number(node.count) || 0) : 0,
      parts: node.children?.length || 0,
      visible: node.visible !== false,
      objects,
      drawables,
      triangles,
      interactives,
    };
  }

  function listWorldElementParts(elementId, path = []) {
    const element = ctx.worldElements.get(String(elementId || ""));
    if (!element) return { parts: [], total: 0 };
    pruneDeadElementRoots(element);
    const steps = (Array.isArray(path) ? path : [])
      .map((step) => Number(step))
      .filter((step) => Number.isInteger(step) && step >= 0);
    const nodes = elementPartNodes(element, steps).filter(Boolean);
    // A switched-off element has had its clickable children pulled out of the
    // live raycast list, so read those back from the detached record — the
    // click column stays truthful while the element is off.
    const interactiveSet = new Set(ctx.interactive);
    element.detached.forEach((record) => {
      record.interactives?.forEach((child) => interactiveSet.add(child));
    });
    return {
      total: nodes.length,
      parts: nodes
        .slice(0, ELEMENT_PART_LIMIT)
        .map((node, index) =>
          describeElementPart(node, index, steps, interactiveSet),
        ),
    };
  }

  function worldObjectKey(elementId, path) {
    return `${elementId}:${path.join(".")}`;
  }

  function parseWorldObjectKey(key) {
    const raw = String(key || "");
    const separator = raw.lastIndexOf(":");
    if (separator <= 0) return null;
    const elementId = raw.slice(0, separator);
    const path = raw
      .slice(separator + 1)
      .split(".")
      .map((step) => Number(step))
      .filter((step) => Number.isInteger(step) && step >= 0);
    if (!elementId || !path.length) return null;
    return { elementId, path };
  }

  // listWorldElementParts answers "what is inside this node"; deletion needs
  // the node itself, addressed by the same path.
  function elementPartNode(element, path) {
    let nodes = elementPartRoots(element);
    let node = null;
    for (const index of path) {
      node = nodes[index];
      if (!node) return null;
      nodes = node.children || [];
    }
    return node;
  }

  // The reverse walk: from a raycast hit back up to the element part root that
  // contains it, recording the child index at every step.
  function elementPartPathFor(element, object) {
    const partRoots = elementPartRoots(element);
    const descent = [];
    let current = object;
    while (current) {
      const index = partRoots.indexOf(current);
      if (index >= 0) {
        const path = [index];
        let parent = current;
        for (const node of descent) {
          const at = (parent.children || []).indexOf(node);
          if (at < 0) return null;
          path.push(at);
          parent = node;
        }
        return path;
      }
      descent.unshift(current);
      current = current.parent;
    }
    return null;
  }

  function elementOwning(object) {
    const owners = new Map();
    ctx.worldElements.forEach((element) => {
      element.roots.forEach((root) => {
        if (!owners.has(root)) owners.set(root, element);
      });
    });
    let current = object;
    while (current) {
      const element = owners.get(current);
      if (element) return element;
      current = current.parent;
    }
    return null;
  }

  function detachSceneNode(node) {
    const parent = node?.parent || null;
    if (!parent) return null;
    const index = parent.children.indexOf(node);
    const interactives = [];
    node.traverse?.((child) => {
      let at = ctx.interactive.indexOf(child);
      while (at >= 0) {
        interactives.push(child);
        ctx.interactive.splice(at, 1);
        at = ctx.interactive.indexOf(child);
      }
    });
    parent.remove(node);
    return { parent, index, interactives };
  }

  function attachSceneNode(node, record) {
    const parent = record?.parent;
    if (!parent || !node) return false;
    parent.add(node);
    // Index paths address every part of the world, so a restored node goes
    // back where it was rather than onto the end of its parent's children.
    const children = parent.children;
    const at = children.indexOf(node);
    if (at >= 0 && record.index >= 0 && record.index < children.length) {
      children.splice(at, 1);
      children.splice(record.index, 0, node);
    }
    record.interactives?.forEach((child) => {
      if (!ctx.interactive.includes(child)) ctx.interactive.push(child);
    });
    return true;
  }

  function deleteResolvedWorldObject(key, element, node) {
    if (!key || !node || ctx.deletedWorldObjects.has(key)) return false;
    const label = sceneObjectLabel(node);
    const record = detachSceneNode(node);
    if (!record) return false;
    deletedWorldObjectNodes.add(node);
    ctx.deletedWorldObjects.set(key, {
      ...record,
      key,
      node,
      label,
      elementId: element?.id || "",
      elementLabel: element?.label || "Unregistered",
    });
    ctx.invalidateStaticMirrors();
    ctx.officeFarProxyDirty = true;
    // Same reason the Elements toggle rebuilds it: a deleted caster must not
    // leave its shadow painted on the ground.
    if (ctx.renderer.shadowMap.enabled) ctx.renderer.shadowMap.needsUpdate = true;
    return true;
  }

  // Deletions restored from storage are resolved against the element as it was
  // just built — every path first, then every detach — so two deletions inside
  // one element cannot shift each other's indices.
  function applyStoredObjectDeletions(element) {
    if (!ctx.storedObjectDeletions.size) return;
    const resolved = [];
    ctx.storedObjectDeletions.forEach((key) => {
      const parsed = parseWorldObjectKey(key);
      if (!parsed || parsed.elementId !== element.id) return;
      const node = elementPartNode(element, parsed.path);
      if (node) resolved.push({ key, node });
    });
    resolved.forEach(({ key, node }) => {
      ctx.storedObjectDeletions.delete(key);
      deleteResolvedWorldObject(key, element, node);
    });
  }

  function describeDeletionTarget(scope, key, node, path, interactiveSet) {
    const index = path.length ? path[path.length - 1] : 0;
    return {
      ...describeElementPart(node, index, path.slice(0, -1), interactiveSet),
      scope,
      key,
      label: sceneObjectLabel(node),
    };
  }

  // What is under the cursor, offered at two levels: the exact object that was
  // hit, and the whole named piece it belongs to — a single plank is rarely
  // what someone means by "delete that bench".
  function pickWorldObject({ clientX, clientY } = {}) {
    if (!Number.isFinite(clientX) || !Number.isFinite(clientY)) return null;
    fns.pointerCoordinates({ clientX, clientY });
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    const hit = ctx.raycaster
      .intersectObject(ctx.scene, true)
      .find(
        ({ object }) =>
          object &&
          object.userData?.raycastProxy !== true &&
          objectIsEffectivelyVisible(object),
      );
    if (!hit) return null;
    pickedWorldObjects.clear();
    const element = elementOwning(hit.object);
    const interactiveSet = new Set(ctx.interactive);
    const targets = [];
    const remember = (target, node) => {
      pickedWorldObjects.set(target.key, { node, element });
      targets.push(target);
    };
    const path = element ? elementPartPathFor(element, hit.object) : null;
    if (element && path) {
      remember(
        describeDeletionTarget(
          "object",
          worldObjectKey(element.id, path),
          hit.object,
          path,
          interactiveSet,
        ),
        hit.object,
      );
      if (path.length > 1) {
        const groupPath = path.slice(0, 1);
        const groupNode = elementPartNode(element, groupPath);
        if (groupNode) {
          remember(
            describeDeletionTarget(
              "group",
              worldObjectKey(element.id, groupPath),
              groupNode,
              groupPath,
              interactiveSet,
            ),
            groupNode,
          );
        }
      }
    } else {
      // Scenery no element claims — the store plugin sandbox, a stray helper.
      // It can still be deleted for this session; only element-addressed
      // deletions can be written down and replayed after a reload.
      remember(
        describeDeletionTarget(
          "object",
          `scene:${hit.object.uuid}`,
          hit.object,
          [0],
          interactiveSet,
        ),
        hit.object,
      );
    }
    return {
      elementId: element?.id || "",
      elementLabel: element?.label || "Unregistered",
      elementCategory: element?.category || "",
      elementEnabled: element ? worldElementEnabled(element.id) : false,
      elementSystem: element?.systemOnly === true,
      persistent: Boolean(element && path),
      distance: Math.round((Number(hit.distance) || 0) * 10) / 10,
      targets,
    };
  }

  function deleteWorldObject(key) {
    const id = String(key || "");
    if (!id || ctx.deletedWorldObjects.has(id)) return false;
    const picked = pickedWorldObjects.get(id);
    let node = picked?.node || null;
    let element = picked?.element || null;
    if (!node) {
      const parsed = parseWorldObjectKey(id);
      element = parsed ? ctx.worldElements.get(parsed.elementId) || null : null;
      node = element ? elementPartNode(element, parsed.path) : null;
    }
    return deleteResolvedWorldObject(id, element, node);
  }

  function restoreWorldObject(key) {
    const id = String(key || "");
    const record = ctx.deletedWorldObjects.get(id);
    if (!record) {
      // Never resolved this session (its element has not been built yet):
      // dropping the stored key is still a restore.
      return ctx.storedObjectDeletions.delete(id);
    }
    ctx.deletedWorldObjects.delete(id);
    deletedWorldObjectNodes.delete(record.node);
    attachSceneNode(record.node, record);
    ctx.invalidateStaticMirrors();
    ctx.officeFarProxyDirty = true;
    if (ctx.renderer.shadowMap.enabled) ctx.renderer.shadowMap.needsUpdate = true;
    return true;
  }

  function restoreAllWorldObjects() {
    const keys = [...ctx.deletedWorldObjects.keys(), ...ctx.storedObjectDeletions];
    keys.forEach((key) => restoreWorldObject(key));
    return keys.length;
  }

  function listDeletedWorldObjects() {
    return [
      ...[...ctx.deletedWorldObjects.values()].map((record) => ({
        key: record.key,
        label: record.label,
        elementId: record.elementId,
        elementLabel: record.elementLabel,
        pending: false,
      })),
      ...[...ctx.storedObjectDeletions].map((key) => ({
        key,
        label: key,
        elementId: parseWorldObjectKey(key)?.elementId || "",
        elementLabel: "",
        pending: true,
      })),
    ];
  }

  // Unnamed meshes are the norm, so fall back to the nearest named ancestor
  // plus the geometry type: "forkmesh-operator-belt › BoxGeometry" locates a
  // row in the scene graph far better than a bare "Mesh".
  function sceneObjectLabel(object) {
    if (object.name) return object.name;
    let ancestor = object.parent;
    while (ancestor && !ancestor.name) ancestor = ancestor.parent;
    const geometryType = object.geometry?.type || "Mesh";
    return ancestor?.name ? `${ancestor.name} › ${geometryType}` : geometryType;
  }

  // Every individual mesh currently drawing triangles, tagged with the world
  // element that owns it. listWorldElements answers "which feature costs the
  // most"; this answers "which single object inside it does". Walked on
  // demand from the Debug tab, never per frame.
  function listSceneObjects() {
    const owners = new Map();
    ctx.worldElements.forEach((element) => {
      pruneDeadElementRoots(element);
      element.roots.forEach((root) => {
        root.traverse?.((child) => {
          if (!owners.has(child)) owners.set(child, element);
        });
      });
    });
    const interactiveSet = new Set(ctx.interactive);
    const objects = [];
    ctx.scene.traverse((child) => {
      if (!child.isMesh || objects.length >= SCENE_OBJECT_WALK_LIMIT) return;
      if (child.userData?.raycastProxy === true) return;
      const geometry = child.geometry;
      const vertices =
        geometry?.index?.count ?? geometry?.attributes?.position?.count ?? 0;
      const perInstance = Math.floor(vertices / 3);
      if (perInstance <= 0) return;
      const instances = child.isInstancedMesh
        ? Math.max(0, Number(child.count) || 0)
        : 1;
      // An instanced mesh parked at count 0 submits nothing this frame.
      const triangles = perInstance * instances;
      if (triangles <= 0) return;
      const owner = owners.get(child);
      objects.push({
        label: sceneObjectLabel(child),
        element: owner ? owner.label : "Unregistered",
        elementId: owner ? owner.id : "",
        type: child.isInstancedMesh
          ? "Instanced"
          : child.isSkinnedMesh
            ? "Skinned"
            : "Mesh",
        geometry: geometry?.type || "Geometry",
        triangles,
        instances,
        visible: objectIsEffectivelyVisible(child),
        interactive: interactiveSet.has(child),
      });
    });
    return objects;
  }

  return {
    worldElementEnabled,
    registerWorldElement,
    registerWorldElementSystem,
    pruneDeadElementRoots,
    setWorldElementEnabled,
    listWorldElements,
    installStoreElement,
    removeStoreElement,
    tickStoreElements,
    listWorldElementParts,
    pickWorldObject,
    deleteWorldObject,
    restoreWorldObject,
    restoreAllWorldObjects,
    listDeletedWorldObjects,
    listSceneObjects,
  };
}
