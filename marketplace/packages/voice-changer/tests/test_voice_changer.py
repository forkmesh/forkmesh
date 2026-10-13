"""Voice Changer: the desktop engine and audio path, the Worker module, and
the free listing, held to one another."""

import asyncio
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import textwrap

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
DESKTOP = PACKAGE / "content" / "desktop"
PACKAGE_MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
CATALOG = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
FEATURE = (DESKTOP / "VoiceChangerFeature.cpp").read_text(encoding="utf-8")
AUDIO = (DESKTOP / "VoiceAudio.cpp").read_text(encoding="utf-8")
BUILD = json.loads((DESKTOP / "build.json").read_text(encoding="utf-8"))
CXX = shutil.which("g++")
MOC = next((str(p) for p in (Path("/usr/lib/qt6/libexec/moc"),
                             Path("/usr/lib/x86_64-linux-gnu/qt6/libexec/moc"))
            if p.is_file()), shutil.which("moc"))


def _qt_core_flags():
    if not CXX or not MOC or not shutil.which("pkg-config"):
        return None
    result = subprocess.run(["pkg-config", "--cflags", "--libs", "Qt6Core"],
                            capture_output=True, text=True)
    return result.stdout.split() if result.returncode == 0 else None


needs_cxx = pytest.mark.skipif(CXX is None, reason="g++ is not installed")
QT_CORE = _qt_core_flags()
needs_qt = pytest.mark.skipif(QT_CORE is None, reason="Qt6Core, moc or pkg-config missing")

sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import voice_changer  # noqa: E402


# --- listing ------------------------------------------------------------------

def test_listing_is_a_free_desktop_package():
    # Free, and with nothing to buy the package asks the host for no payment
    # capability at all.
    assert PACKAGE_MANIFEST["price"] == {"amount": 0, "currency": "USD"}
    assert PACKAGE_MANIFEST["license"]["model"] == "free-open-source"
    assert not [p for p in PACKAGE_MANIFEST["permissions"]
                if p["id"].startswith("payments.")]
    assert "package_owned" not in FEATURE and "package_checkout" not in FEATURE
    worker = (PACKAGE / "content" / "worker" / "voice_changer.py").read_text(encoding="utf-8")
    assert "package_owned" not in worker and "package_checkout" not in worker
    row = next(entry for entry in CATALOG["packages"] if entry["id"] == "voice-changer")
    assert row["version"] == PACKAGE_MANIFEST["version"]
    # Desktop only: a native rail page and its Worker API, no web page and no
    # browser extension.
    assert PACKAGE_MANIFEST["entrypoints"] == {
        "desktop": "builtin:voice-changer",
        "worker": "content/worker/voice_changer.py",
        "settings": "builtin:package",
    }
    assert "contributions" not in PACKAGE_MANIFEST
    assert not (PACKAGE / "content" / "extension").exists()
    assert not (PACKAGE / "content" / "web").exists()
    assert PACKAGE_MANIFEST["install"] == {"bundled": True, "default": False}
    assert PACKAGE_MANIFEST["worker"]["api"] == voice_changer.PREFIX
    assert len(PACKAGE_MANIFEST["summary"]) <= 240
    market = (REPO / "www" / "public" / "market.html").read_text(encoding="utf-8")
    assert "Voice Changer" in market


def test_desktop_page_registers_itself_with_the_worker_proof():
    assert 'QStringLiteral("builtin:voice-changer")' in FEATURE
    assert 'QStringLiteral("/api/voice-changer")' in FEATURE
    assert f'QStringLiteral("{voice_changer.REQUEST_PROOF}")' in FEATURE
    assert "NativePackageRegistration voiceChangerRegistration" in FEATURE
    # Q_OBJECT lives only in the audio pipeline, and the build knows it.
    assert BUILD["mocHeaders"] == ["VoiceAudio.h"]
    assert "Q_OBJECT" not in (DESKTOP / "VoiceChangerFeature.h").read_text(encoding="utf-8")
    icons = (REPO / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    assert "icons/unmute" in icons


def test_desktop_page_answers_the_top_search_socket():
    assert "searchHits(const QString &query, int limit) const" in FEATURE
    assert "void VoiceChangerFeature::activateSearchHit" in FEATURE


def test_settings_live_under_the_package_data_prefix_the_host_removes():
    assert 'QStringLiteral("market/packages/%1/data/voice.%2")' in FEATURE


def test_audio_path_makes_a_virtual_microphone_and_never_records_it():
    # PipeWire: one pw-loopback exposes a sink and a "ForkMesh Voice" source.
    assert "media.class=Audio/Sink node.name=%1" in AUDIO
    assert "media.class=Audio/Source node.name=%1" in AUDIO
    # PulseAudio: a null sink plus a source remapped from its monitor.
    assert "module-null-sink" in AUDIO and "module-remap-source" in AUDIO
    # Our own microphone is never offered or used as the input (feedback loop).
    assert "if (!input.isEmpty() && !ours(input))" in AUDIO
    # Helpers die with the app rather than keeping a microphone alive.
    assert "PR_SET_PDEATHSIG" in AUDIO


def test_audio_path_keeps_the_player_fed_without_crackle():
    # 10 ms helper streams dropped blocks on a desktop even when fed from a file.
    assert 'QStringLiteral("10ms")' not in AUDIO and "--latency-msec=10\"" not in AUDIO
    assert 'kStreamLatency = QStringLiteral("40ms")' in AUDIO
    # Every write measures the player's queue and goes through a RateMatcher.
    assert "FIONREAD" in AUDIO and "matcher->process(" in AUDIO
    # A player that died must not kill the app with SIGPIPE.
    assert "pthread_sigmask(SIG_BLOCK" in AUDIO


def test_background_noise_reduction_is_on_unless_turned_off():
    assert 'setting(QStringLiteral("noiseReduction"), QStringLiteral("true"))' in FEATURE
    assert '{QStringLiteral("noiseReduction"), m_denoise->isChecked()}' in FEATURE
    assert "pipeline->setNoiseReduction(on)" in FEATURE
    assert 'config.value(QStringLiteral("noiseReduction"), true)' in AUDIO


def test_voice_matching_needs_consent():
    body = FEATURE.split("void VoiceChangerFeature::learn()", 1)[1]
    body = body.split("\nvoid VoiceChangerFeature::", 1)[0]
    assert body.index("m_consent->isChecked()") < body.index("learnVoice(")


# --- engine (C++) ------------------------------------------------------------------

@pytest.fixture(scope="module")
def engine(tmp_path_factory):
    if CXX is None:
        pytest.skip("g++ is not installed")
    binary = tmp_path_factory.mktemp("engine") / "test_voice_engine"
    subprocess.run(
        [CXX, "-std=c++20", "-O2", "-o", str(binary), str(DESKTOP / "VoiceEngine.cpp"),
         str(DESKTOP / "tests" / "test_voice_engine.cpp")],
        check=True, capture_output=True, text=True)
    result = subprocess.run([str(binary), "--json"], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@needs_cxx
def test_engine_bounds_match_the_worker(engine):
    assert {name: list(bounds) for name, bounds in voice_changer.PARAMS.items()} == engine["params"]


@needs_cxx
def test_engine_is_a_clean_delay_when_nothing_changes(engine):
    assert 0 < engine["latencySamples"] <= 2400  # under 50 ms at 48 kHz
    assert engine["identityCorrelation"] > 0.9999


@needs_cxx
def test_engine_moves_pitch_and_keeps_level(engine):
    expected = 120 * 2 ** (-5 / 12)
    assert abs(engine["downPitch"] - expected) < 0.03 * expected
    assert 0.8 < engine["downLevelRatio"] < 1.25
    # The lowered voice stays one clean periodic tone, not a smeared one.
    assert engine["downClarity"] > 0.9
    assert abs(engine["learnedSpeakerPitch"] - 120) <= 3
    assert abs(engine["targetPitch"] - 220) < 0.06 * 220


@needs_cxx
def test_every_preset_is_stable(engine):
    assert all(result["finite"] for result in engine["presets"].values())
    assert engine["presets"]["deep-baritone"]["pitch"] < 100
    assert engine["presets"]["clear-soprano"]["pitch"] > 220


@needs_cxx
def test_noise_reduction_quiets_the_room_and_keeps_the_voice(engine):
    assert engine["noiseCutDb"] > 15
    # Someone already talking when it starts is not learned as the noise.
    assert engine["talkingLevel"] > 0.9
    assert 0.8 < engine["cleanSpeechLevel"] < 1.25
    expected = 120 * 2 ** (-4 / 12)
    assert abs(engine["cleanPitch"] - expected) < 0.03 * expected


@needs_cxx
def test_player_queue_follows_clock_drift_without_gaps(engine):
    queue = engine["queue"]
    assert queue["driftGaps"] == 0 and queue["driftUnderruns"] == 0
    assert queue["driftCorrection"] == pytest.approx(0.003, abs=0.0015)
    assert queue["driftMaxMs"] < 90 and queue["steadyGaps"] == 0
    # A 300 ms stall costs one gap and one cut-back, then no more.
    assert queue["stallUnderruns"] <= 1 and queue["stallDrops"] == 1
    assert queue["stallGaps"] == 0 and queue["stallMaxMs"] < 200


@needs_cxx
def test_engine_runs_well_inside_real_time(engine):
    assert engine["realtimeFactor"] < 0.25


@needs_cxx
def test_learning_a_voice_recovers_pitch_and_vocal_tract(engine):
    assert engine["fitted"]["targetPitchHz"] == pytest.approx(230, abs=6)
    assert engine["fitted"]["formant"] == pytest.approx(1.18, abs=0.03)


# --- audio pipeline against stand-in tools ------------------------------------------------

FAKE_PW_CAT = textwrap.dedent('''\
    #!/usr/bin/env python3
    # Stand-in pw-cat: --record plays a ~120 Hz synthetic voice, --playback keeps
    # what it is sent. Both run at real time, as the sound server would: the
    # pipeline measures how far ahead of the player it is.
    import math, os, struct, sys, time
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "calls.log"), "a") as log:
        log.write("pw-cat " + " ".join(sys.argv[1:]) + "\\n")
    if "--playback" in sys.argv:
        started = time.monotonic()
        taken = 0
        with open(os.environ["FAKE_CAPTURED"], "ab") as out:
            while True:
                chunk = sys.stdin.buffer.read(1024)
                if not chunk:
                    break
                out.write(chunk)
                out.flush()
                taken += len(chunk) // 4
                time.sleep(max(0.0, started + taken / 48000 - time.monotonic()))
        sys.exit(0)
    rate = 48000
    bank = []
    for i, hz in enumerate((700, 1220, 2600)):
        r = math.exp(-math.pi * (80 + 40 * i) / rate)
        bank.append([2 * r * math.cos(2 * math.pi * hz / rate), -r * r, 0.0, 0.0])
    phase, samples = 0.0, []
    for i in range(rate * 3):
        # Speech is never perfectly steady: a slow 3% vibrato, as in the engine test.
        phase += 120.0 * (1 + 0.03 * math.sin(2 * math.pi * 3 * i / rate)) / rate
        pulse = 0.0
        if phase >= 1:
            phase -= 1
            pulse = 1.0
        y = 0.0
        for f in bank:
            v = pulse + f[0] * f[2] + f[1] * f[3]
            f[3], f[2] = f[2], v
            y += v
        samples.append(y * 0.05)
    data = b"".join(struct.pack("<f", v) for v in samples)
    started = time.monotonic()
    for start in range(0, len(data), 3840):
        sys.stdout.buffer.write(data[start:start + 3840])
        sys.stdout.buffer.flush()
        time.sleep(max(0.0, started + (start + 3840) / 4 / rate - time.monotonic()))
    time.sleep(60)
''')

FAKE_PW_LOOPBACK = textwrap.dedent('''\
    #!/usr/bin/env python3
    import os, signal, sys, time
    here = os.path.dirname(os.path.abspath(__file__))
    def log(line):
        with open(os.path.join(here, "calls.log"), "a") as f:
            f.write(line + "\\n")
    log("pw-loopback " + " ".join(sys.argv[1:]))
    signal.signal(signal.SIGTERM, lambda *_: (log("loopback-stopped"), sys.exit(0)))
    while True:
        time.sleep(0.1)
''')

FAKE_PW_DUMP = textwrap.dedent('''\
    #!/usr/bin/env python3
    import json
    print(json.dumps([
        {"id": 7, "info": {"props": {"media.class": "Audio/Source", "node.name": "alsa_input.usb",
                                      "node.description": "USB Microphone"}}},
        {"id": 9, "info": {"props": {"media.class": "Audio/Sink",
                                      "node.name": "forkmesh_voice_sink"}}},
    ]))
''')


@needs_qt
def test_audio_pipeline_end_to_end_with_stand_in_tools(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    for name, body in (("pw-cat", FAKE_PW_CAT), ("pw-loopback", FAKE_PW_LOOPBACK),
                       ("pw-dump", FAKE_PW_DUMP)):
        script = tools / name
        script.write_text(body, encoding="utf-8")
        script.chmod(0o755)
    moc_out = tmp_path / "moc_VoiceAudio.cpp"
    subprocess.run([MOC, str(DESKTOP / "VoiceAudio.h"), "-o", str(moc_out)], check=True)
    binary = tmp_path / "test_voice_audio"
    build = subprocess.run(
        [CXX, "-std=c++20", "-fPIC", "-O2", "-I", str(DESKTOP), "-o", str(binary),
         str(DESKTOP / "tests" / "test_voice_audio.cpp"), str(DESKTOP / "VoiceAudio.cpp"),
         str(DESKTOP / "VoiceEngine.cpp"), str(moc_out), *QT_CORE],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    captured = tmp_path / "captured.raw"
    env = dict(os.environ, FAKE_CAPTURED=str(captured), QT_QPA_PLATFORM="offscreen")
    result = subprocess.run([str(binary), str(tools), str(captured)], env=env,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr + result.stdout
    measured = json.loads(result.stdout.strip().splitlines()[-1])
    assert measured["started"] and measured["stoppedCleanly"]
    assert abs(measured["outputPitch"] - 220) < 0.07 * 220


# --- Worker module ----------------------------------------------------------------------------

class Runtime:
    def __init__(self, actor="alice", method="GET", body=None, ai=True, origin_ok=True):
        self.actor = actor
        self.method_value = method
        self.body = body if body is not None else {}
        self.origin_ok = origin_ok
        self.prompts = []
        if ai:
            self.ai_json = self._ai_json
        self.ai_reply = {"ok": True, "data": {
            "name": "Night radio", "note": "",
            "params": {"targetPitchHz": 95, "formant": 3.0, "breath": -2, "mix": 0.2, "extra": 5}}}

    def method(self):
        return self.method_value

    def same_origin(self):
        return self.origin_ok

    async def json_body(self, limit):
        del limit
        return self.body, ""

    async def ensure_schema(self):
        return None

    async def session(self, data):
        del data
        return (("bi:" + self.actor, {"name": self.actor, "email": "a@example.com"})
                if self.actor else ("", None))

    async def _ai_json(self, account_bi, system, prompt, schema, image_data_url="", max_tokens=700):
        self.prompts.append((account_bi, system, prompt, schema, max_tokens))
        return self.ai_reply

    def response(self, payload, status=200, cache_control="", extra_headers=None):
        return {"status": status, "payload": payload, "headers": extra_headers or {}}


def _call(runtime, path="/api/voice-changer"):
    return asyncio.run(voice_changer.handle(runtime, path))


def test_status_says_who_is_signed_in_and_whether_ai_is_there():
    result = _call(Runtime())
    assert result["status"] == 200
    assert result["payload"]["actor"] == "alice" and result["payload"]["ai"] is True
    assert "price" not in result["payload"] and "owned" not in result["payload"]
    assert _call(Runtime(ai=False))["payload"]["ai"] is False
    assert _call(Runtime(actor=""))["status"] == 401


def test_design_clamps_what_the_model_returns():
    runtime = Runtime(method="POST", body={"prompt": "A calm, deep late-night radio host"})
    result = _call(runtime, "/api/voice-changer/design")
    assert result["status"] == 200
    voice = result["payload"]["voice"]
    assert voice["params"]["formant"] == 1.45 and voice["params"]["breath"] == 0
    assert voice["params"]["mix"] == 1 and "extra" not in voice["params"]
    account, system, prompt, schema, _tokens = runtime.prompts[0]
    assert account == "bi:alice" and "real, named person" in system
    assert "<description>A calm, deep late-night radio host</description>" in prompt
    assert set(schema["properties"]["params"]["properties"]) == set(voice_changer.PARAMS)


def test_design_errors():
    assert _call(Runtime(method="POST", body={"prompt": ""}),
                 "/api/voice-changer/design")["status"] == 400
    assert _call(Runtime(method="POST", body={"prompt": "deep"}, ai=False),
                 "/api/voice-changer/design")["status"] == 503
    limited = Runtime(method="POST", body={"prompt": "deep"})
    limited.ai_reply = {"ok": False, "error": "rate_limited", "retryAfterMs": 3000}
    result = _call(limited, "/api/voice-changer/design")
    assert result["status"] == 429 and result["payload"]["retryAfterMs"] == 3000
    assert _call(Runtime(method="POST", body={"prompt": "deep"}, origin_ok=False),
                 "/api/voice-changer/design")["status"] == 403


def test_routes_and_methods():
    assert _call(Runtime(), "/api/voice-changer/nope")["status"] == 404
    assert _call(Runtime(), "/api/voice-changer/design/x")["status"] == 404
    assert _call(Runtime(method="POST"))["status"] == 405
    assert _call(Runtime(method="GET"), "/api/voice-changer/design")["status"] == 405
    # Nothing is sold any more, so there is no checkout route at all.
    assert _call(Runtime(method="POST"), "/api/voice-changer/checkout")["status"] == 404
