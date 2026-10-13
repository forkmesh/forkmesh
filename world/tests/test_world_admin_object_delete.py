#!/usr/bin/env python3
"""Contracts for right-click deletion in the World.

The issue asks that an administrator — or anyone running the debug panel —
can right-click on something in the world and delete it. Deleting is the same
kind of local render experiment the Elements tab already offers, one object at
a time instead of one feature at a time: the piece leaves the scene graph and
the raycast list, the choice is remembered on this device, and everything
deleted can be put back.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (WORLD / "world.css").read_text(encoding="utf-8")


def test_scene_picks_what_is_under_the_pointer_and_names_who_owns_it():
    for contract in (
        "function pickWorldObject({ clientX, clientY } = {}) {",
        "raycaster.setFromCamera(pointer, camera);",
        ".intersectObject(scene, true)",
        "function elementOwning(object)",
        "function elementPartPathFor(element, object)",
        "function worldObjectKey(elementId, path)",
        "function parseWorldObjectKey(key)",
    ):
        assert contract in SCENE, f"missing pick contract: {contract}"
    pick = SCENE.split("function pickWorldObject", 1)[1].split(
        "function deleteWorldObject", 1
    )[0]
    # A raycast proxy is invisible geometry that exists to be clicked, and a
    # hidden or detached subtree is not on screen: neither is what the pointer
    # is aimed at.
    assert "object.userData?.raycastProxy !== true" in pick
    assert "objectIsEffectivelyVisible(object)" in pick
    # Two levels are offered, because a single plank is rarely what someone
    # means by "delete that bench".
    assert '"object"' in pick and '"group"' in pick
    # Only an element-addressed pick can be written down and replayed.
    assert "persistent: Boolean(element && path)," in pick


def test_deleting_removes_the_object_from_the_scene_and_from_raycasting():
    detach = SCENE.split("function detachSceneNode", 1)[1].split(
        "function attachSceneNode", 1
    )[0]
    assert "interactive.splice(at, 1);" in detach
    assert "parent.remove(node);" in detach
    delete = SCENE.split("function deleteResolvedWorldObject", 1)[1].split(
        "function applyStoredObjectDeletions", 1
    )[0]
    # A deleted caster must not leave its shadow painted on the ground.
    assert "renderer.shadowMap.needsUpdate = true;" in delete
    assert "deletedWorldObjectNodes.add(node);" in delete
    # The element registry keeps owning a deleted root, or restoring it would
    # hand back a root no element could switch off again.
    prune = SCENE.split("function pruneDeadElementRoots", 1)[1].split(
        "function setWorldElementEnabled", 1
    )[0]
    assert "if (deletedWorldObjectNodes.has(root)) return;" in prune


def test_restoring_puts_the_object_back_where_it_was():
    attach = SCENE.split("function attachSceneNode", 1)[1].split(
        "function deleteResolvedWorldObject", 1
    )[0]
    # Index paths address every part of the world, so a restored node goes back
    # at its own index rather than onto the end of its parent's children.
    assert "children.splice(record.index, 0, node);" in attach
    assert "if (!interactive.includes(child)) interactive.push(child);" in attach
    for contract in (
        "function restoreWorldObject(key)",
        "function restoreAllWorldObjects()",
        "function listDeletedWorldObjects()",
    ):
        assert contract in SCENE
    # Detached geometry is not reachable from the scene, so it is put back
    # before the teardown sweep instead of outliving the renderer.
    dispose = SCENE.split("  function dispose() {", 1)[1].split("\n  }", 1)[0]
    assert (
        "[...deletedWorldObjects.keys()].forEach((key) => restoreWorldObject(key));"
        in dispose
    )
    assert dispose.index("restoreWorldObject(key)") < dispose.index(
        "scene.traverse"
    )


def test_deletions_survive_a_reload_and_cannot_shift_each_other():
    assert "initialDeletedObjects = []," in SCENE
    assert "initialDeletedObjects: this.deletedWorldObjects.map(" in APP
    # Applied as each owning element is built…
    register = SCENE.split("function registerWorldElement(", 1)[1].split(
        "function registerWorldElementSystem", 1
    )[0]
    assert "applyStoredObjectDeletions(element);" in register
    # …resolving every stored path against the element as it was just built,
    # and only then detaching, so two deletions inside one element cannot move
    # each other's indices.
    apply = SCENE.split("function applyStoredObjectDeletions", 1)[1].split(
        "function describeDeletionTarget", 1
    )[0]
    assert apply.index("const node = elementPartNode(element, parsed.path);") < apply.index(
        "deleteResolvedWorldObject(key, element, node);"
    )
    # Stored on the device only, exactly like the Elements switches.
    assert (
        'const DELETED_OBJECTS_KEY = "forkmesh.world.deletedObjects.v1";' in APP
    )
    assert "function storedDeletedWorldObjects()" in APP
    assert "const DELETED_OBJECT_KEY_RE = /^[a-z0-9-]+:\\d+(?:\\.\\d+)*$/;" in APP
    # A piece no element claims has no address after a reload, so it is never
    # written down.
    persist = APP.split("  persistDeletedWorldObjects() {", 1)[1].split(
        "\n  deleteWorldObject(", 1
    )[0]
    assert "DELETED_OBJECT_KEY_RE.test(entry.key)" in persist


def test_the_menu_opens_on_right_click_for_admins_and_debug_panel_users():
    available = APP.split("  worldObjectDeletionAvailable() {", 1)[1].split(
        "\n  }", 1
    )[0]
    assert "this.identity?.isAdmin === true" in available
    assert "this.settings?.debugPanel === true" in available
    handler = APP.split("  handleWorldContextMenu(event) {", 1)[1].split(
        "\n  openWorldObjectMenu(", 1
    )[0]
    # Everyone else keeps the browser's own context menu, and so does an empty
    # pick.
    assert "if (!this.worldObjectDeletionAvailable()) return;" in handler
    assert handler.index("if (!pick?.targets?.length)") < handler.index(
        "event.preventDefault();"
    )
    # Bound to the canvas only: the HUD, chat, and links keep their own menu.
    assert (
        'this.$("[data-world-canvas-wrap]")?.addEventListener(\n      "contextmenu",'
        in APP
    )
    for contract in (
        "data-world-object-menu",
        "data-world-object-delete=",
        "data-world-object-delete-element=",
        "data-world-object-restore=",
        "data-world-object-restore-all",
        "openWorldObjectMenu(pick, event.clientX, event.clientY);",
        "positionWorldObjectMenu(menu, clientX, clientY)",
    ):
        assert contract in APP, f"missing menu contract: {contract}"
    # Whole-element removal reuses the Elements switch rather than a second
    # mechanism.
    assert "this.setWorldElementEnabled(\n          elementDelete.dataset" in APP


def test_the_menu_closes_on_escape_and_on_the_next_click_outside_it():
    assert (
        'this.$("[data-world-object-menu]")?.dataset.open === "true" &&\n'
        '        !event.target.closest("[data-world-object-menu]")' in APP
    )
    escape = APP.split('if (event.code !== "Escape") return;\n      if (this.$', 1)[
        1
    ].split("\n    });", 1)[0]
    assert 'this.closeWorldObjectMenu();' in escape


def test_everything_deleted_is_listed_with_one_click_to_restore_it():
    assert "data-world-deleted-group" in APP
    assert "data-world-deleted-list" in APP
    assert "renderDeletedWorldObjects()" in APP
    listing = APP.split("  renderDeletedWorldObjects() {", 1)[1].split(
        "\n  // One card per purchasable element", 1
    )[0]
    assert "group.hidden = deleted.length === 0;" in listing
    # A deletion whose element has not loaded this session says so instead of
    # looking like a piece that was never there.
    assert "listDeletedWorldObjects?.()" in listing
    assert "waiting for its element to load" in listing
    # The Elements tab renders it, so opening that tab always shows the truth.
    assert (
        "  renderWorldElementsPane(statusMessage = \"\") {\n"
        "    this.renderDeletedWorldObjects();" in APP
    )
    assert "restoreAllDeletedWorldThings()" in APP
    assert "this.disabledWorldElements = [];" in APP


def test_the_menu_and_the_restore_list_are_styled():
    for selector in (
        ".world-object-menu {",
        ".world-object-menu[hidden] {",
        ".world-object-menu-head {",
        ".world-object-menu-note {",
        ".world-deleted-list {",
        ".world-deleted-row {",
        ".world-deleted-name {",
    ):
        assert selector in CSS, f"missing style: {selector}"
