import json
import re
import shutil
import subprocess
from pathlib import Path


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
IOS = REPO / "native" / "ios"
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
CATALOG = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
PAGE = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
VIEW = (PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")


def test_manifest_catalog_and_places_describe_the_cross_platform_studio():
    assert MANIFEST["id"] == "video-mix-dj-studio"
    assert MANIFEST["install"] == {"bundled": True, "default": False, "sync": "account", "places": ["mobile", "desktop", "web"]}
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:video-mix-dj-studio"
    assert MANIFEST["entrypoints"]["ios"] == "builtin:video-mix-dj-studio"
    assert MANIFEST["contributions"][0]["config"]["route"] == "/dashboard/video-mix-dj-studio"
    row = next(item for item in CATALOG["packages"] if item["id"] == MANIFEST["id"])
    assert row == {"id": MANIFEST["id"], "version": MANIFEST["version"], "manifest": "packages/video-mix-dj-studio/manifest.json"}


def test_dashboard_page_has_a_fixed_mobile_two_deck_surface_and_overlay_controls():
    for marker in ("data-vmdj-deck=\"a\"", "data-vmdj-deck=\"b\"", "data-vmdj=\"crossfader\"", "data-vmdj=\"sync\"", "data-vmdj=\"loop-a\"", "data-vmdj=\"capture-a\"", "data-vmdj=\"platter-a\"", "data-vmdj=\"art-a\"", "data-vmdj=\"section-0\"", "data-vmdj=\"save\"", "data-vmdj=\"modal\"", "data-vmdj=\"open-search\"", "data-vmdj-pad=\"kick\"", "data-vmdj-pad=\"riser\""):
        assert marker in VIEW
    assert "overflow:hidden" in VIEW
    assert "orientation:landscape" in VIEW
    assert "vmdj-platter.is-spinning" in VIEW
    assert "playsinline=1" in PAGE
    assert "vmdj-fullscreen" in VIEW
    assert "registerPage(\"video-mix-dj-studio\"" in PAGE


def test_overlay_keeps_keyboard_focus_and_announces_its_current_context():
    assert 'aria-labelledby="vmdj-modal-title"' in VIEW
    assert 'data-vmdj="modal-title"' in VIEW
    assert 'role="status" aria-live="polite"' in VIEW
    assert 'data-vmdj="search-target-a" class="vmdj-btn" type="button" aria-pressed="true"' in VIEW
    assert 'data-vmdj="search-target-b" class="vmdj-btn" type="button" aria-pressed="false"' in VIEW
    assert 'state.modalOpener = document.activeElement;' in PAGE
    assert 'if (opener instanceof HTMLElement && opener.isConnected) opener.focus();' in PAGE
    assert 'el("modal").addEventListener("keydown", (event) => {' in PAGE
    assert 'if (event.key !== "Tab") return;' in PAGE
    assert 'last.focus();' in PAGE
    assert 'first.focus();' in PAGE
    assert 'el("search-target-a").setAttribute("aria-pressed", String(state.searchTarget === "a"));' in PAGE


def test_mix_uses_youtube_embed_messages_local_storage_and_public_search_only():
    assert "www.youtube-nocookie.com/embed" in PAGE
    assert '"setVolume"' in PAGE
    assert '"setPlaybackRate"' in PAGE
    assert "AudioContext" in PAGE
    assert "triggerPad" in PAGE
    assert "captureSection" in PAGE
    assert "scratchStart" in PAGE
    assert "localStorage.setItem" in PAGE
    assert "https://api.invidious.io/instances.json" in PAGE
    assert "/api/v1/search" in PAGE
    assert "canonicalUrl" in PAGE
    assert "attribution_link" in PAGE
    assert "XMLHttpRequest" not in PAGE


def test_desktop_link_and_preview_are_packaged():
    link = (PACKAGE / "content" / "desktop" / "VideoMixDjStudioLink.cpp").read_text(encoding="utf-8")
    build = json.loads((PACKAGE / "content" / "desktop" / "build.json").read_text(encoding="utf-8"))
    assert 'QStringLiteral("builtin:video-mix-dj-studio")' in link
    assert 'QStringLiteral("/dashboard/video-mix-dj-studio")' in link
    assert 'QStringLiteral("play")' in link
    assert build == {"schemaVersion": 1, "sources": ["VideoMixDjStudioLink.cpp"], "mocHeaders": []}
    assert (PACKAGE / "preview" / "market.svg").is_file()
    assert (PACKAGE / "preview" / "market.webp").stat().st_size > 1000


def test_ios_studio_is_native_full_screen_and_rotates():
    packages = IOS / "ForkMesh" / "Features" / "Packages"
    screen = (packages / "VideoMixDjStudioScreen.swift").read_text(encoding="utf-8")
    player = (packages / "VideoMixDjStudioPlayer.swift").read_text(encoding="utf-8")
    model = (packages / "VideoMixDjStudioModel.swift").read_text(encoding="utf-8")
    delegate = (IOS / "ForkMesh" / "App" / "AppDelegate.swift").read_text(encoding="utf-8")
    routes = (IOS / "ForkMeshCore" / "Sources" / "FMPackages" / "PackageRoutes.swift").read_text(encoding="utf-8")
    parser = (IOS / "ForkMeshCore" / "Sources" / "FMApi" / "Navigation" / "RouteParser.swift").read_text(encoding="utf-8")
    assert ".fullScreenCover(isPresented: $presented" in screen
    assert "DjLandscape(model: model, size: geometry.size)" in screen
    assert "DjPortrait(model: model, size: geometry.size)" in screen
    assert ".persistentSystemOverlays(.hidden)" in screen
    assert "WKWebView" not in screen
    assert "allowsInlineMediaPlayback = true" in player
    assert 'host:"https://www.youtube-nocookie.com"' in player
    assert "webView.isUserInteractionEnabled = false" in player
    assert "FmOrientationLock.allowLandscape(true)" in model
    assert "FmOrientationLock.allowLandscape(false)" in model
    assert "supportedInterfaceOrientationsFor window: UIWindow?" in delegate
    assert 'case "videoMixDjStudio": return .videoMixDjStudio' in routes
    assert 'case "video-mix-dj-studio": return [.videoMixDjStudio]' in parser


def swift_literal(source, name):
    body = re.search(r"static let " + name + r' = """\n(.*?)\n\s*"""', source, re.S).group(1)
    return "\n".join(line[8:] for line in body.split("\n")).replace("\\\\", "\\")


def test_ios_decks_play_together_and_keep_playing_in_the_background(tmp_path):
    player = (IOS / "ForkMesh" / "Features" / "Packages" / "VideoMixDjStudioPlayer.swift").read_text(encoding="utf-8")
    plist = (IOS / "Config" / "ForkMesh-Info.plist").read_text(encoding="utf-8")
    modes = re.search(r"<key>UIBackgroundModes</key>\s*<array>(.*?)</array>", plist, re.S).group(1)
    assert "<string>audio</string>" in modes
    frame = swift_literal(player, "frameScript")
    host = swift_literal(player, "html")
    assert 'delete window[n]' in frame
    assert 'document.createElement("audio")' in frame
    assert "var m=want(v)||!solo(v)" in frame
    assert "function start(e){if(e.ended)return;" in frame
    assert "fmmse:M" in host
    assert "S.prototype.addSourceBuffer=function(type)" in frame
    assert "visibilitychange" in frame
    assert "function fmBackground(on)" in host
    assert 'fmdjFrame==="voice"' in host
    assert "func reset()" in player
    assert "didEnterBackgroundNotification" in player
    assert "willEnterForegroundNotification" in player
    assert 'run("fmAway(1)")' in player and 'run("fmAway(0)")' in player
    assert "function fmAway(on)" in host and "away:L" in host
    assert "function standby(v,a)" in frame
    assert "if(w.gain>=0.999&&Math.abs(d)<0.03){swap(v,a,true);return}" in frame
    node = shutil.which("node")
    if node:
        script = re.search(r"<script>(.*?)</script>", host, re.S).group(1)
        for name, text in (("frame.js", frame), ("host.js", script)):
            path = tmp_path / name
            path.write_text(text, encoding="utf-8")
            subprocess.run([node, "--check", str(path)], check=True)


def test_page_is_valid_javascript_when_node_is_available():
    node = shutil.which("node")
    if node:
        subprocess.run([node, "--check", str(PACKAGE / "content" / "web" / "page.js")], check=True)


def test_auto_dj_follows_each_song_with_a_related_song_people_liked():
    packages = IOS / "ForkMesh" / "Features" / "Packages"
    screen = (packages / "VideoMixDjStudioScreen.swift").read_text(encoding="utf-8")
    model = (packages / "VideoMixDjStudioModel.swift").read_text(encoding="utf-8")
    auto = (packages / "VideoMixDjStudioAutoDj.swift").read_text(encoding="utf-8")
    assert 'data-vmdj="auto-dj"' in VIEW and 'aria-pressed="false"' in VIEW
    assert "fields=recommendedVideos" in PAGE
    assert "fields=likeCount,viewCount" in PAGE
    assert "autoDj: state.autoDj" in PAGE
    assert 'el("auto-dj").addEventListener("click", toggleAutoDj);' in PAGE
    assert "DjAutoDjButton(model: model, size: 26 * u)" in screen
    assert "} else if autoDjOn, let pick = takeAutoDj() {" in model
    assert "saved.autoDj = autoDjOn" in model
    assert "DjAutoDj.likesURL(instance, id: id)" in auto


def test_auto_dj_plays_songs_and_prefers_original_uploads():
    assert '"got talent", "agt", "bgt", "x factor"' in PAGE
    assert "if (isSong(item)) found.push(item);" in PAGE
    assert "right.original - left.original" in PAGE
    core = (IOS / "ForkMeshCore" / "Sources" / "FMPackages" / "DjAutoDj.swift").read_text(encoding="utf-8")
    assert '"got talent", "agt", "bgt", "x factor"' in core
    assert "isSong(candidate)" in core


def test_a_push_tap_closes_the_studio_before_routing():
    shell = (IOS / "ForkMesh" / "Shell" / "HomeShell.swift").read_text(encoding="utf-8")
    screen = (IOS / "ForkMesh" / "Features" / "Packages" / "VideoMixDjStudioScreen.swift").read_text(encoding="utf-8")
    route = shell.split("private func routePushOpen()", 1)[1]
    assert route.index("DjStudioHub.shared.stepAside()") < route.index("await model.routePush(")
    assert "router.remove(.videoMixDjStudio)" in route
    assert "if route != .videoMixDjStudio, DjStudioHub.shared.stepAside() {" in shell
    assert "studio.onStepAside = { shown.wrappedValue = false }" in screen
    assert "guard leaving else { return }" in screen


def test_each_deck_can_repeat_its_song_and_auto_dj_leaves_it_alone():
    for name in ("a", "b"):
        assert f'data-vmdj="repeat-{name}"' in VIEW
    assert "repeat: deck.repeat, position: deck.time" in PAGE
    assert "decks[name].repeat = value.repeat === true;" in PAGE
    assert "if (playerState === 0 && deck.repeat) {" in PAGE
    assert 'command(deck, "seekTo", [0, true]);\n        command(deck, "playVideo");' in PAGE
    assert "if (!deck?.id || deck.repeat) return;" in PAGE


def test_ios_repeat_wraps_between_the_songs_silences_before_youtube_ends():
    packages = IOS / "ForkMesh" / "Features" / "Packages"
    model = (packages / "VideoMixDjStudioModel.swift").read_text(encoding="utf-8")
    player = (packages / "VideoMixDjStudioPlayer.swift").read_text(encoding="utf-8")
    frame = swift_literal(player, "frameScript")
    host = swift_literal(player, "html")
    assert "} else {\n                aimRepeat(deck, at: now)\n            }" in model
    assert "rewind(deck, to: span.start)" in model
    assert "rewind(deck, to: DjRepeat.span(deck.waveform, duration: deck.duration)?.start ?? 0)" in model
    assert 'else if(name==="seekTo"){d.w.from=+a[0]||0;d.w.seek++}' in host
    assert "else if(m.seek!==w.seek){" in frame
