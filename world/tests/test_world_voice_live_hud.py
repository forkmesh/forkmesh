import functools
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
WORLD_JS = (WORLD / "world.js").read_text(encoding="utf-8")
VOICE = (WORLD / "world-voice.js").read_text(encoding="utf-8")
PUPPETS = (WORLD / "world-avatar-puppets.js").read_text(encoding="utf-8")
RIG = (WORLD / "scene" / "avatar-rig.js").read_text(encoding="utf-8")
AVATAR_VOICE = (WORLD / "scene" / "runtime" / "avatar-voice.js").read_text(encoding="utf-8")
CORE = (WORLD / "scene" / "core.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[1];
const chart = await import(pathToFileURL(base + "/world-voice-chart.js").href);
const pops = await import(pathToFileURL(base + "/world-reaction-pops.js").href);
const out = {};
const samples = [];
for (let i = 0; i < 320; i += 1) chart.pushVoiceSample(samples, { fps: 60 - (i % 5), level: i % 2 ? 2 : -1, trackingMs: i < 300 ? null : 20, rttMs: "x" });
out.window = samples.length;
out.levels = [samples[0].level, samples[1].level];
out.missing = [samples[0].trackingMs, samples[299].trackingMs, samples[5].rttMs];
out.ceiling = [chart.seriesCeiling(samples, "fps", 30), chart.seriesCeiling([], "rttMs", 80)];
out.summary = chart.voiceChartSummary(samples);
const calls = [];
const context = new Proxy({}, {
  get(target, name) {
    if (name in target) return target[name];
    return (...args) => { calls.push(name); return { width: 10 }; };
  },
  set(target, name, value) { target[name] = value; return true; },
});
out.drawn = chart.drawVoiceChart(context, samples, { ratio: 2 });
out.drawnEmpty = chart.drawVoiceChart(context, [], {});
out.labels = calls.filter((name) => name === "fillText").length;
out.burst = pops.reactionBurst(8, 42);
out.burstAgain = pops.reactionBurst(8, 42);
out.placements = [
  pops.popPlacement(null, { width: 800, height: 600 }),
  pops.popPlacement({ x: -50, y: 9999, onScreen: false }, { width: 800, height: 600 }),
  pops.popPlacement({ x: 400, y: 300, onScreen: true }, { width: 800, height: 600 }),
];
function node(tag) {
  return {
    tag, children: [], style: { props: {}, setProperty(k, v) { this.props[k] = v; } }, dataset: {}, className: "", textContent: "", removed: false,
    append(...items) { this.children.push(...items); },
    remove() { this.removed = true; },
  };
}
const layer = node("div");
const timers = [];
const fake = { createElement: node };
const scope = { innerWidth: 800, innerHeight: 600, setTimeout: (fn, ms) => timers.push([fn, ms]) };
const popper = pops.createReactionPops({ layer, documentRef: fake, scope });
const first = popper.pop("👍", { point: { x: 100, y: 200 }, label: "You · Thumbs up", big: true });
out.pop = {
  size: first.dataset.size,
  x: first.style.props["--pop-x"],
  emoji: first.children[0].textContent,
  sparks: first.children.filter((child) => child.className === "world-reaction-spark").length,
  label: first.children.at(-1).textContent,
  timer: timers[0][1],
};
for (let i = 0; i < 20; i += 1) popper.pop("🔥", {});
out.capped = popper.count();
timers.forEach(([fn]) => fn());
out.cleared = popper.count();
const calm = pops.createReactionPops({ layer: node("div"), documentRef: fake, scope, reducedMotion: true });
out.calmChildren = calm.pop("❤️", {}).children.length;
console.log(JSON.stringify(out));
'''


@functools.cache
def _run():
    done = subprocess.run(["node", "--input-type=module", "-e", HARNESS, str(WORLD)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    return json.loads(done.stdout)


def test_the_live_chart_keeps_thirty_seconds_and_breaks_lines_on_missing_values():
    data = _run()
    assert data["window"] == 300
    assert data["levels"] == [0, 1]
    assert data["missing"] == [None, 20, None]
    assert data["ceiling"][0] == 60 * 1.15 and data["ceiling"][1] == 80
    assert data["summary"] == "Frames 56 fps, Mic level 100%, Tracking 20 ms, Round trip not measured"
    assert data["drawn"] is True and data["drawnEmpty"] is True
    assert data["labels"] == 16


def test_the_live_chart_shows_while_voice_or_video_is_on_and_can_be_hidden():
    assert "timers.push(scope.setInterval(sampleChart, VOICE_CHART_SAMPLE_MS));" in VOICE
    assert 'export const WORLD_VOICE_CHART_KEY = "forkmesh.world.voiceChart";' in VOICE
    assert " Show the live diagnostics chart" in VOICE
    assert "return Boolean(state.room || state.mic || state.cam);" in VOICE
    assert '<figure class="world-voice-chart" data-world-voice-chart hidden>' in WORLD_JS
    assert 'chartElement: () => this.$("[data-world-voice-chart]"),' in WORLD_JS
    assert ".fm-world .world-voice-chart {" in CSS


def test_reactions_pop_on_screen_with_a_burst_for_everyone():
    data = _run()
    assert data["burst"] == data["burstAgain"] and len(data["burst"]) == 8
    assert data["placements"][0] == {"x": 400, "y": 330, "anchored": False}
    assert data["placements"][1] == {"x": 40, "y": 480, "anchored": False}
    assert data["placements"][2] == {"x": 400, "y": 300, "anchored": True}
    assert data["pop"] == {"size": "big", "x": "100px", "emoji": "👍", "sparks": 8, "label": "You · Thumbs up", "timer": 1900}
    assert data["capped"] == 14 and data["cleared"] == 0
    assert data["calmChildren"] == 1
    assert 'host.popReaction?.(RIG_REACTIONS[kind].emoji, `You · ${RIG_REACTIONS[kind].label}`);' in VOICE
    assert 'this.popWorldReaction(emoji, from, name, { big: toSelf });' in WORLD_JS
    assert "avatarScreenPoint," in AVATAR_VOICE and CORE.count("avatarScreenPoint,") == 2
    assert "@keyframes world-reaction-pop" in CSS


def test_an_on_air_sign_blinks_under_the_corner_face_and_on_the_avatar():
    assert '<span class="world-on-air" data-world-on-air aria-hidden="true" hidden>' in WORLD_JS
    assert "const live = Boolean(mic || cam);" in WORLD_JS
    assert 'this.world?.setAvatarOnAir?.("self", live);' in WORLD_JS
    assert "const live = Boolean(player.mic || player.cam);" in WORLD_JS
    assert "export function syncAvatarOnAir(THREE, avatar, on)" in RIG
    assert "avatar.userData.onAirSign?.visible === true" in CORE
    assert "@keyframes world-on-air-blink" in CSS


def test_the_uploaded_avatar_photo_is_the_live_face_on_the_body_and_in_the_corner():
    assert "export function portraitFaceAnchors()" in PUPPETS
    assert "anchors: peer.anchors || fallbackAnchors" in PUPPETS
    assert '(self ? ownPhoto() : null)' in PUPPETS
    assert "if (self) onSelfFace(peer.canvas);" in PUPPETS
    assert 'host.puppets()?.local("self", state.anchors ? { ...rig, anchors: state.anchors } : rig);' in VOICE
    assert '<canvas class="world-shirt-live" data-world-shirt-live' in WORLD_JS
    assert "onSelfFace: (canvas) => this.paintHudLiveFace(canvas)," in WORLD_JS
    assert 'if (this.settings?.faceImagePreference === "generated") return null;' in WORLD_JS
