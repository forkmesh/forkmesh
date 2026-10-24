#!/usr/bin/env python3
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
FRAGMENT = (JS_DIR / "09-post-translate.js").read_text("utf-8")
HELPERS = (JS_DIR / "02-helpers.js").read_text("utf-8")
BOOT = (JS_DIR / "10-boot.js").read_text("utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text("utf-8")
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

sys.path.insert(0, str(ROOT / "src"))
import dashboard_bundle  # noqa: E402


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    assert match, name
    cursor = text.index("{", text.index(")", match.start()))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():cursor + 1]
        cursor += 1


DOM = r"""
class FakeText {
  constructor(value) { this.nodeType = 3; this.nodeValue = value; this.parentNode = null; }
}
class FakeElement {
  constructor(tag, attrs = {}, children = []) {
    this.nodeType = 1;
    this.tagName = tag.toUpperCase();
    this.attrs = { ...attrs };
    this.childNodes = [];
    children.forEach((child) => this.append(child));
  }
  append(child) {
    const node = typeof child === "string" ? new FakeText(child) : child;
    node.parentNode = this;
    this.childNodes.push(node);
    return node;
  }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  removeAttribute(name) { delete this.attrs[name]; }
  hasAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attrs, name); }
  get text() { return this.childNodes.map((node) => node.nodeType === 3 ? node.nodeValue : node.text).join(""); }
}
const h = (tag, attrs, ...children) => new FakeElement(tag, attrs || {}, children);
class MemoryStorage {
  constructor() { this.items = {}; }
  getItem(key) { return Object.prototype.hasOwnProperty.call(this.items, key) ? this.items[key] : null; }
  setItem(key, value) { this.items[key] = String(value); }
}
function fakeTranslator(answers, calls) {
  return {
    async availability(options) {
      calls.push(["availability", options.sourceLanguage, options.targetLanguage]);
      return answers[`${options.sourceLanguage}>${options.targetLanguage}`] || "unavailable";
    },
    async create(options) {
      calls.push(["create", options.sourceLanguage, options.targetLanguage]);
      const listeners = [];
      options.monitor?.({ addEventListener: (type, fn) => listeners.push([type, fn]) });
      listeners.forEach(([type, fn]) => { if (type === "downloadprogress") fn({ loaded: 0.5 }); });
      return { async translate(text) { calls.push(["translate", text]); return `[${options.targetLanguage}] ${text}`; } };
    },
  };
}
function fakeDetector(language, confidence, calls, state = "available") {
  return {
    async availability() { return state; },
    async create() {
      calls.push(["detector"]);
      return { async detect(text) { calls.push(["detect", text]); return [{ detectedLanguage: language, confidence }, { detectedLanguage: "und", confidence: 0.01 }]; } };
    },
  };
}
"""


def _run(body):
    names = [
        "postTranslateApi", "postTranslateReadPrefs", "postTranslateWritePrefs", "postTranslateCode",
        "postTranslateSame", "postTranslateReaderLanguage", "postTranslateLanguageName",
        "postTranslateNativeName", "postTranslateSkipped", "postTranslateSegments", "postTranslateWorthy",
        "postTranslatePlainText", "postTranslateProse", "postTranslateScriptGuess", "postTranslateWordGuess",
        "postTranslateGuess", "postTranslateWithin",
        "postTranslateDetectorInstance", "postTranslateDetect",
        "postTranslateAvailability", "postTranslateRecordAvailability", "postTranslateTranslator", "postTranslateText",
        "postTranslateApply", "postTranslateRestore", "postTranslateStatusText", "postTranslateBarHtml",
        "postTranslateHoverable", "postTranslateCanHover",
    ]
    consts = FRAGMENT[:FRAGMENT.index("  function postTranslateApi(")]
    code = "\n".join([consts, _function(HELPERS, "escapeHtml")] + [_function(FRAGMENT, n) for n in names])
    script = "%s\n%s\n(async () => {\nconst out = {};\n%s\nprocess.stdout.write(JSON.stringify(out));\n})().catch((error) => { console.error(error); process.exit(1); });\n" % (DOM, code, body)
    result = subprocess.run(["node", "-"], input=script, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


@needs_node
def test_the_fragment_is_bundled_and_started_at_boot():
    assert "09-post-translate.js" in dashboard_bundle.FRAGMENTS
    assert "09-post-translate.js" not in dashboard_bundle.LAZY_CHUNKS.values()
    assert "initPostTranslate();" in BOOT
    assert ".fm-xl-bar" in SHELL and ".fm-xl-pop" in SHELL


@needs_node
def test_code_blocks_links_and_mentions_are_not_translated():
    out = _run(r"""
const body = h("div", {},
  h("p", {}, "Use the ", h("code", {}, "--tls"), " flag ", h("strong", {}, "only"), " when needed."),
  h("pre", {}, h("code", {}, "curl https://example.com")),
  h("p", {}, "See ", h("a", { href: "/x" }, "the report"), " and @alice."),
  h("p", {}, h("a", { href: "https://x.test" }, "https://x.test/long")),
  h("p", {}, h("span", { translate: "no" }, "keep me")),
  h("ul", {}, h("li", {}, "first item"), h("li", {}, "#42")));
out.segments = postTranslateSegments(body).map((nodes) => nodes.map((node) => node.nodeValue));
const calls = [];
const translator = await fakeTranslator({}, calls).create({ sourceLanguage: "fa", targetLanguage: "en" });
const record = { source: "fa", target: "en", showing: "original", saved: [] };
await postTranslateApply(body, record, translator);
out.translated = body.text;
out.lang = body.getAttribute("lang");
out.showing = record.showing;
out.code = body.childNodes[1].text;
postTranslateRestore(body, record);
out.restored = body.text;
out.langAfter = body.getAttribute("lang");
out.showingAfter = record.showing;
""")
    assert out["segments"] == [
        ["Use the "], [" flag ", "only", " when needed."], ["See "], ["the report"],
        [" and @alice."], ["first item"]]
    assert "--tls" in out["translated"] and "curl https://example.com" in out["translated"]
    assert "[en] flag only when needed." in out["translated"]
    assert "[en] the report" in out["translated"] and "#42" in out["translated"]
    assert "keep me" in out["translated"] and "[en] keep me" not in out["translated"]
    assert out["code"] == "curl https://example.com"
    assert out["lang"] == "en" and out["showing"] == "translation"
    assert out["restored"] == ("Use the --tls flag only when needed.curl https://example.com"
                               "See the report and @alice.https://x.test/longkeep mefirst item#42")
    assert out["langAfter"] == "fa" and out["showingAfter"] == "original"


@needs_node
def test_availability_covers_available_downloadable_unavailable_and_no_api():
    out = _run(r"""
const calls = [];
const api = { translator: fakeTranslator({ "fa>en": "available", "ru>en": "downloadable", "zh>en": "downloading" }, calls), detector: null };
out.available = await postTranslateAvailability("fa", "en", api);
out.downloadable = await postTranslateAvailability("ru", "en", api);
out.downloading = await postTranslateAvailability("zh", "en", api);
out.unavailable = await postTranslateAvailability("tk", "en", api);
out.same = await postTranslateAvailability("en", "en-US", api);
out.unknown = await postTranslateAvailability("", "en", api);
out.noApi = await postTranslateAvailability("fa", "en", { translator: null, detector: null });
const hanging = { translator: { availability: () => new Promise(() => {}), create() {} }, detector: null };
const started = Date.now();
out.hanging = await postTranslateAvailability("fa", "en", hanging);
out.waited = Date.now() - started;
out.apiFromScope = [postTranslateApi({}).translator, Boolean(postTranslateApi({ Translator: api.translator }).translator)];
const progress = [];
const translator = await postTranslateTranslator("ru", "en", api, (loaded) => progress.push(loaded));
const again = await postTranslateTranslator("ru", "en", api);
out.reused = translator === again;
out.progress = progress;
out.creates = calls.filter((call) => call[0] === "create").length;
await postTranslateText(translator, "ru", "en", "Привет");
await postTranslateText(translator, "ru", "en", "Привет");
out.translateCalls = calls.filter((call) => call[0] === "translate").length;
const base = { source: "fa", target: "en", showing: "original" };
out.bars = {};
for (const availability of ["available", "downloadable", "unavailable", "no-api", "unknown-source", "same"]) {
  out.bars[availability] = postTranslateBarHtml({ ...base, availability, hintShown: true });
}
""")
    assert out["available"] == "available"
    assert out["downloadable"] == "downloadable"
    assert out["downloading"] == "downloading"
    assert out["unavailable"] == "unavailable"
    assert out["same"] == "same" and out["unknown"] == "unknown-source"
    assert out["noApi"] == "no-api"
    assert out["hanging"] == "unknown" and out["waited"] < 10000
    assert out["apiFromScope"] == [None, True]
    assert out["reused"] is True and out["creates"] == 1 and out["progress"] == [0.5]
    assert out["translateCalls"] == 1
    bars = out["bars"]
    assert "Translate to English" in bars["available"] and "aria-disabled" not in bars["available"]
    assert "aria-disabled" not in bars["downloadable"]
    assert "aria-disabled=\"true\"" in bars["no-api"]
    assert "Your browser can&#039;t translate on-device; use its Translate page option." in bars["no-api"]
    assert "This post is marked as Persian" in bars["no-api"]
    assert "from Persian to English isn&#039;t available in this browser" in bars["unavailable"]
    # A post already in the reader's language gets no button and no menu.
    assert "data-post-translate-toggle" not in bars["same"]
    assert "data-post-translate-menu" not in bars["same"] and "data-post-translations" in bars["same"]
    assert "Nothing is sent to a translation service." in bars["available"]


@needs_node
def test_language_detection_prefers_the_detector_then_script_heuristics():
    out = _run(r"""
const calls = [];
out.detector = await postTranslateDetect("Это сообщение о блокировке.", { translator: null, detector: fakeDetector("ru", 0.93, calls) });
out.weak = await postTranslateDetect("ok", { translator: null, detector: fakeDetector("en", 0.2, calls) });
out.persian = await postTranslateDetect("این یک گزارش درباره فیلترینگ پیام رسان های ایرانی است", { translator: null, detector: null });
out.arabic = await postTranslateDetect("هذه مدينة كبيرة في المنطقة العربية التي يحكي عنها", { translator: null, detector: null });
out.ambiguous = postTranslateScriptGuess("سلام");
out.chinese = postTranslateScriptGuess("这是一个关于审查的报告");
out.japanese = postTranslateScriptGuess("これは検閲についての報告です");
out.latin = postTranslateScriptGuess("Turkmenistan blocks this protocol");
out.empty = await postTranslateDetect("   ", { translator: null, detector: null });
postTranslateUi.detector = null;
const createdBefore = calls.filter((call) => call[0] === "detector").length;
const later = { translator: null, detector: fakeDetector("en", 0.9, calls, "downloadable") };
out.later = await postTranslateDetect("Turkmenistan blocks this protocol and the other ones", later);
out.laterCreates = calls.filter((call) => call[0] === "detector").length - createdBefore;
out.gesture = await postTranslateDetect("Turkmenistan blocks this protocol", later, true);
out.laterAvailability = await postTranslateAvailability("", "en", { translator: fakeTranslator({}, calls), detector: null }, "later");
out.codes = ["zh-TW", "zh-Hans-CN", "en-US", "pt_BR", "iw", "x", "<b>"].map(postTranslateCode);
out.names = [postTranslateLanguageName("fa"), postTranslateNativeName("zh"), postTranslateLanguageName("zh-Hant")];
""")
    assert out["detector"] == {"language": "ru", "confidence": 0.93, "source": "detector"}
    assert out["weak"]["source"] == "none"
    assert out["persian"]["language"] == "fa" and out["persian"]["source"] == "script"
    assert out["arabic"]["language"] == "ar"
    assert out["ambiguous"] == ""
    assert out["chinese"] == "zh" and out["japanese"] == "ja" and out["latin"] == ""
    assert out["empty"]["language"] == ""
    # Without the browser's model yet, common words still name the language.
    assert out["later"] == {"language": "en", "confidence": 0.55, "source": "words"}
    assert out["laterCreates"] == 0
    assert out["gesture"]["language"] == "en" and out["gesture"]["source"] == "detector"
    assert out["laterAvailability"] == "unknown"
    assert out["codes"] == ["zh-Hant", "zh", "en", "pt", "he", "", ""]
    assert out["names"][0] == "Persian" and out["names"][1] == "中文"


@needs_node
def test_the_reader_language_is_remembered_and_overrides_the_browser():
    out = _run(r"""
const storage = new MemoryStorage();
const nav = { languages: ["fa-IR", "en"], language: "fa-IR" };
out.first = postTranslateReaderLanguage(postTranslateReadPrefs(storage), nav);
postTranslateWritePrefs({ target: "zh-TW", auto: true }, storage);
out.saved = JSON.parse(storage.getItem(POST_TRANSLATE_PREFS_KEY));
const prefs = postTranslateReadPrefs(storage);
out.prefs = prefs;
out.second = postTranslateReaderLanguage(prefs, nav);
storage.setItem(POST_TRANSLATE_PREFS_KEY, "{broken");
out.broken = postTranslateReadPrefs(storage);
out.fallback = postTranslateReaderLanguage({ target: "" }, { languages: [] });
const throwing = { getItem() { throw new Error("denied"); }, setItem() { throw new Error("denied"); } };
out.blocked = postTranslateReadPrefs(throwing);
out.written = postTranslateWritePrefs({ target: "ru" }, throwing);
""")
    assert out["first"] == "fa"
    assert out["saved"] == {"target": "zh-Hant", "auto": True, "machine": True}
    assert out["prefs"] == {"target": "zh-Hant", "auto": True, "machine": True}
    assert out["second"] == "zh-Hant"
    assert out["broken"] == {"target": "", "auto": False, "machine": True}
    assert out["fallback"] == "en"
    assert out["blocked"] == {"target": "", "auto": False, "machine": True}
    assert out["written"] == {"target": "ru", "auto": False, "machine": True}


@needs_node
def test_a_failed_translation_leaves_the_original_in_place():
    out = _run(r"""
const body = h("div", {}, h("p", {}, "one"), h("p", {}, "two"));
let n = 0;
const translator = { async translate(text) { n += 1; if (n === 2) throw new Error("boom"); return "uno"; } };
const record = { source: "en", target: "es", showing: "original", saved: [] };
try { await postTranslateApply(body, record, translator); out.threw = false; } catch (_) { out.threw = true; }
out.text = body.text;
out.showing = record.showing;
""")
    assert out["threw"] is True
    assert out["text"] == "onetwo"
    assert out["showing"] == "original"


def _run_community(body):
    names = [
        "postTranslateCode", "postTranslateLanguageName", "postTranslateNativeName",
        "postTranslationParts", "postTranslationRepo", "postTranslationSigningRepo", "postTranslationSigningText",
        "postTranslationCleanBody", "postTranslationLangCode", "postTranslationSourceText",
        "postTranslationLanguages", "postTranslationChipsHtml", "postTranslationEntryHtml",
        "postTranslationFormHtml", "postTranslationErrorText",
    ]
    consts = FRAGMENT[:FRAGMENT.index("  function postTranslateApi(")]
    community = FRAGMENT[FRAGMENT.index("  const POST_TRANSLATION_PLUGIN"):FRAGMENT.index("  function postTranslationParts(")]
    code = "\n".join([consts, community, _function(HELPERS, "escapeHtml")] + [_function(FRAGMENT, n) for n in names])
    script = """
const MD_BODY_CLASS = "md";
const state = { repoRecordDetail: { repo: { owner: "example", name: "forum" }, parsed: {
  body: "opening", issueEvents: [
    { type: "comment", id: "c1", body: "first" },
    { type: "edit", target: "c1", body: "first, edited" },
    { type: "comment", id: "c2", body: "second" }] } } };
function formatTimeAgo() { return "2 days ago"; }
function renderRecordMarkdown(text) { return "<p>" + escapeHtml(text) + "</p>"; }
%s
(async () => {
const out = {};
%s
process.stdout.write(JSON.stringify(out));
})().catch((error) => { console.error(error); process.exit(1); });
""" % (code, body)
    result = subprocess.run(["node", "-"], input=script, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


@needs_node
def test_community_translation_signing_matches_the_server():
    import hashlib
    import post_translations
    out = _run_community(r"""
out.text = postTranslationSigningText("/api/repo/Example/BBS%20x", "issue:7", "c1", "fa", "PUB", 1234, "abc");
out.clean = postTranslationCleanBody("line\r\nnext\u0007\u200c  \n\n");
out.langs = ["zh-hant", "EN", "pt_BR", "x", "<b>"].map(postTranslationLangCode);
out.parts = [postTranslationParts("issue:7"), postTranslationParts("pull:3"), postTranslationParts("wiki:1")];
out.sources = [postTranslationSourceText("body"), postTranslationSourceText("c1"), postTranslationSourceText("c2"), postTranslationSourceText("nope")];
""")
    assert out["text"] == post_translations.signing_text(
        "Example", "BBS x", "issue", 7, "c1", "fa", "PUB", 1234, "abc")
    raw = "line\r\nnext\u0007\u200c  \n\n"
    assert out["clean"] == post_translations.clean_body(raw) == "line\nnext\u200c"
    assert out["langs"] == [post_translations.clean_lang(v) for v in ["zh-hant", "EN", "pt_BR", "x", "<b>"]]
    assert out["langs"][:3] == ["zh-hant", "en", "pt-BR"]
    assert out["parts"] == [{"kind": "issue", "number": 7}, {"kind": "pull", "number": 3}, None]
    assert out["sources"] == ["opening", "first, edited", "second", ""]
    assert hashlib.sha256(b"x").hexdigest()


@needs_node
def test_community_chips_entries_and_forms_escape_and_never_replace_the_post():
    out = _run_community(r"""
const entries = [
  { lang: "en", name: "carol", mine: false, updatedAt: 1, source: "aaa" },
  { lang: "en", name: "dave", mine: true, updatedAt: 1, source: "" },
  { lang: "zh", name: "erin", mine: false, updatedAt: 1, source: "" }];
out.chips = postTranslationChipsHtml(entries, true, "zh");
out.signedOut = postTranslationChipsHtml([], false);
out.signedInEmpty = postTranslationChipsHtml([], true);
out.entry = postTranslationEntryHtml({ lang: "fa", name: "<img src=x>", agent: true, mine: false, updatedAt: 5, source: "old",
  body: "<script>alert(1)</script>" }, { sourceHash: "new", canModerate: true });
out.mine = postTranslationEntryHtml({ lang: "en", name: "dave", mine: true, updatedAt: 5, source: "same", body: "hi" }, { sourceHash: "same" });
out.form = postTranslationFormHtml("fa", "</textarea><b>x</b>");
out.errors = ["rate_limited", "bad_signature", "plugin_disabled", "weird"].map(postTranslationErrorText);
""")
    chips = out["chips"]
    assert "Community translations:" in chips
    assert 'data-post-translation-chip="en"' in chips and "· 2" in chips
    assert 'data-post-translation-chip="zh" aria-pressed="true"' in chips
    assert "Add yours" in chips
    assert out["signedOut"] == ""
    assert "Add a translation" in out["signedInEmpty"]
    entry = out["entry"]
    assert "<script>" not in entry and "&lt;script&gt;" in entry
    assert "<img" not in entry and "&lt;img src=x&gt; (agent)" in entry
    assert "the original above is the source of truth" in entry
    assert "the original was edited after this translation" in entry
    assert 'lang="fa" dir="auto"' in entry
    assert 'data-post-translation-remove="fa" data-post-translation-account="&lt;img src=x&gt;"' in entry
    assert "data-post-translation-edit" not in entry
    assert "data-post-translation-edit=\"en\"" in out["mine"] and "was edited" not in out["mine"]
    assert "</textarea><b>" not in out["form"] and "&lt;/textarea&gt;" in out["form"]
    assert '<option value="fa" selected>' in out["form"]
    assert out["errors"][0].startswith("Slow down") and "sign" in out["errors"][1]
    assert out["errors"][2] == "Translations are switched off here."
    assert out["errors"][3] == "The translation didn't save. Try again."


@needs_node
def test_languages_are_guessed_without_the_browsers_model():
    out = _run(r"""
const samples = {
  ru: "Свой VPN-сервис для конечных пользователей в России. Протокол работает, но мобильный интернет режут.",
  uk: "Це повідомлення про блокування. Їх провайдер змінює налаштування щодня, і ми шукаємо рішення.",
  kk: "Қазақстанда интернет бұғатталды, біз шешім іздейміз және көмек сұраймыз.",
  en: "The provider started blocking this protocol last week and we are looking for a way around it.",
  es: "El proveedor bloquea el protocolo desde la semana pasada y no hay una solución para los usuarios.",
  de: "Der Anbieter blockiert das Protokoll seit letzter Woche und es gibt keine Lösung für die Nutzer.",
  fr: "Le fournisseur bloque le protocole depuis la semaine dernière et il n'y a pas de solution pour les gens.",
  vi: "Nhà mạng đã chặn giao thức này từ tuần trước và chúng tôi đang tìm cách vượt qua.",
  zh: "这个协议上周开始被封锁了，我们正在寻找绕过的方法。",
};
out.guesses = {};
for (const [code, text] of Object.entries(samples)) out.guesses[code] = postTranslateGuess(text);
out.short = postTranslateGuess("ok thanks");
out.code = postTranslateGuess("curl -v https://example.com --resolve");
const detected = await postTranslateDetect(samples.ru, { translator: null, detector: null });
out.detectedSource = detected.source;
""")
    assert out["guesses"] == {code: code for code in out["guesses"]}
    assert out["short"] == "" and out["code"] == ""
    assert out["detectedSource"] == "script"


@needs_node
def test_a_model_that_never_starts_gives_up_and_is_not_offered_again():
    out = _run(r"""
const calls = [];
const stuck = { availability: async () => "downloadable", create: () => new Promise(() => {}) };
const api = { translator: stuck, detector: null };
const started = Date.now();
try { await postTranslateTranslator("ru", "en", api, null, 50); out.threw = false; } catch (error) { out.threw = error.message; }
out.waited = Date.now() - started;
out.after = await postTranslateAvailability("ru", "en", api);
const counted = { calls: 0, availability: async () => { counted.calls += 1; return "available"; }, create() {} };
await postTranslateAvailability("fa", "en", { translator: counted, detector: null });
await postTranslateAvailability("fa", "en", { translator: counted, detector: null });
out.asked = counted.calls;
""")
    assert out["threw"] == "translator_stalled" and out["waited"] < 5000
    assert out["after"] == "unavailable"
    assert out["asked"] == 1


@needs_node
def test_a_post_written_twice_needs_no_translation_for_its_second_language():
    out = _run(r"""
const calls = [];
const api = { translator: fakeTranslator({ "zh>en": "available", "zh>fa": "available" }, calls), detector: null };
const text = "这个协议上周开始被封锁了，我们正在寻找绕过的方法。 This protocol was blocked last week and we are looking for a way around it.";
const record = { source: postTranslateGuess(text), alsoIn: postTranslateWordGuess(text), target: "en" };
out.source = record.source;
out.forEnglish = await postTranslateRecordAvailability(record, api);
out.forPersian = await postTranslateRecordAvailability({ ...record, target: "fa" }, api);
out.bar = postTranslateBarHtml({ ...record, availability: "unknown-source", showing: "original" });
""")
    assert out["source"] == "zh"
    assert out["forEnglish"] == "same" and out["forPersian"] == "available"
    assert "data-post-translate-toggle" not in out["bar"]


@needs_node
def test_the_sites_own_translation_is_labelled_and_can_be_switched_off():
    out = _run(r"""
const base = { source: "ru", target: "en", availability: "machine", machineAuto: true };
out.ready = postTranslateBarHtml({ ...base, showing: "original" });
out.shown = postTranslateBarHtml({ ...base, showing: "translation", machine: { source: "ru", text: "x" } });
out.off = postTranslateBarHtml({ ...base, machineAuto: false, showing: "original" });
const storage = new MemoryStorage();
postTranslateWritePrefs({ target: "en", machine: false }, storage);
out.prefs = postTranslateReadPrefs(storage);
""")
    assert "Translate to English" in out["ready"] and "translation ready" in out["ready"]
    assert "aria-disabled" not in out["ready"]
    assert "Show original" in out["shown"] and "Machine translation from Russian" in out["shown"]
    assert "data-post-translate-machine checked" in out["shown"]
    assert "data-post-translate-machine checked" not in out["off"]
    assert out["prefs"]["machine"] is False


@needs_node
def test_hovering_reveals_only_posts_with_a_translation_ready():
    out = _run(r"""
const base = { source: "ru", target: "en", showing: "original", hover: true };
out.machine = postTranslateHoverable({ ...base, availability: "machine", machineEntry: { text: "x" } });
out.onDevice = postTranslateHoverable({ ...base, availability: "available" });
out.download = postTranslateHoverable({ ...base, availability: "downloadable" });
out.touch = postTranslateHoverable({ ...base, hover: false, availability: "machine", machineEntry: { text: "x" } });
out.shown = postTranslateHoverable({ ...base, showing: "translation", availability: "machine", machineEntry: { text: "x" } });
out.dismissed = postTranslateHoverable({ ...base, hoverOff: true, availability: "machine", machineEntry: { text: "x" } });
out.status = postTranslateStatusText({ ...base }, "machine");
out.touchStatus = postTranslateStatusText({ ...base, hover: false }, "machine");
out.canHover = [postTranslateCanHover({ matchMedia: () => ({ matches: true }) }), postTranslateCanHover({}),
  postTranslateCanHover({ matchMedia() { throw new Error("no"); } })];
""")
    assert out["machine"] is True and out["onDevice"] is True
    assert out["download"] is False and out["touch"] is False
    assert out["shown"] is False and out["dismissed"] is False
    assert out["status"] == "Written in Russian · hover to translate"
    assert out["touchStatus"] == "Written in Russian · translation ready"
    assert out["canHover"] == [True, False, False]


def test_the_translate_bar_starts_only_with_the_plugin_on():
    assert 'if (instancePluginOn("post-translate")) initPostTranslate();' in BOOT


@needs_node
def test_an_idle_slice_that_timed_out_still_draws_one_bar():
    script = """
const postTranslateUi = { scheduled: false, pending: [], posts: new Map() };
const drawn = [];
async function postTranslateDecorate(body) { drawn.push(body.id); }
const queue = [];
function requestIdleCallback(fn) { queue.push(fn); }
%s
postTranslateUi.pending.push({ id: 1, isConnected: true }, { id: 2, isConnected: true });
postTranslateSchedule();
for (let round = 0; round < 4 && queue.length; round += 1) {
  queue.shift()({ didTimeout: true, timeRemaining: () => 0 });
}
console.log(JSON.stringify(drawn));
""" % _function(FRAGMENT, "postTranslateSchedule")
    result = subprocess.run(["node", "-"], input=script, capture_output=True, text=True, check=True)
    assert json.loads(result.stdout) == [1, 2]
