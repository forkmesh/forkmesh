  const POST_TRANSLATE_PREFS_KEY = "forkmesh.postTranslate.v1";
  const POST_TRANSLATE_SKIP = new Set(["PRE", "CODE", "KBD", "SAMP", "VAR", "SCRIPT", "STYLE", "TEXTAREA", "INPUT", "SELECT", "BUTTON", "SVG", "MATH", "IMG", "VIDEO", "AUDIO"]);
  const POST_TRANSLATE_INLINE = new Set(["B", "STRONG", "I", "EM", "U", "S", "DEL", "INS", "MARK", "SMALL", "SUB", "SUP", "SPAN", "ABBR", "Q", "CITE", "DFN", "BDI", "BDO", "FONT"]);
  const POST_TRANSLATE_NO_TRANSLATE = "pre, code, kbd, samp, var";
  const POST_TRANSLATE_LANGUAGES = Object.freeze([
    "en", "zh", "zh-Hant", "fa", "ar", "ru", "tk", "tr", "uk", "es", "fr", "de", "pt", "it", "ja", "ko",
    "vi", "id", "hi", "bn", "ur", "he", "pl", "nl", "sv", "my", "th", "kk", "uz", "az",
  ]);
  const POST_TRANSLATE_DETECT_CHARS = 2000;
  const POST_TRANSLATE_MIN_CONFIDENCE = 0.5;
  const POST_TRANSLATE_CHECK_MS = 4000;
  const POST_TRANSLATE_STALL_MS = 20000;
  const POST_TRANSLATE_DETECTOR_MS = 8000;
  const POST_TRANSLATE_WORDS = Object.freeze({
    en: "the and is are was of to in that it for this with not on be you have but can from they will what about there a an as at by or if has we so do which would their been more some all just also only than how who",
    es: "el la los las de que y en por para con una es no se del como pero más este esta también",
    fr: "le la les de des et est une que pour dans pas sur avec il ce qui du ne sont mais aussi",
    de: "der die das und ist nicht ein eine zu mit von den auf für ich es sich auch dass wird",
    pt: "o os de que e do da em um uma para com não por mais se é dos das mas também",
    it: "il la di che e è un una per non con del della sono si anche gli più come",
    tr: "ve bir bu için ile de da çok ne olarak değil gibi ama daha var ben",
    id: "yang dan di ini itu dengan untuk tidak ada dari saya akan juga bisa atau",
    nl: "de het een en van is dat niet op te met voor zijn ook maar",
    pl: "i w nie na się z że do to jest jak co ale czy przez",
    tk: "we bilen üçin bu hem däl bolsa ýa-da ýaly bar",
  });
  const POST_TRANSLATE_LETTERS = Object.freeze({
    de: /[ß]/g, es: /[ñ¿¡]/g, pt: /[ãõ]/g, tr: /[ğı]/g, pl: /[ąęłśźżń]/g, tk: /[ňýž]/g,
  });
  const POST_TRANSLATE_RUNNABLE = new Set(["available", "downloadable", "downloading", "unknown", "pending", "machine"]);
  const POST_MACHINE_SCHEMA = "forkmesh-machine-translations-issue-v1";
  const POST_TRANSLATE_HOVER_MS = 350;
  const POST_TRANSLATE_SPARKLES = 12;
  const POST_TRANSLATE_STYLE = `
    .fm-xl-sparkle { position: fixed; z-index: 90; width: 16px; height: 16px; margin: -8px 0 0 -8px; pointer-events: none; color: rgb(var(--dashboard-primary-rgb, 9 105 218)); filter: drop-shadow(0 0 4px currentColor); animation: fm-xl-spark var(--fm-xl-life, 800ms) ease-out var(--fm-xl-delay, 0ms) both; }
    .fm-xl-sparkle::before { content: ""; position: absolute; inset: 0; background: currentColor; clip-path: polygon(50% 0, 61% 39%, 100% 50%, 61% 61%, 50% 100%, 39% 61%, 0 50%, 39% 39%); }
    .fm-xl-sparkle:nth-child(3n) { color: rgb(var(--dashboard-accent-rgb, 130 80 223)); }
    @keyframes fm-xl-spark { 0% { opacity: 0; transform: translate(0, 0) scale(.2) rotate(0deg); } 25% { opacity: 1; } 100% { opacity: 0; transform: translate(var(--fm-xl-dx), var(--fm-xl-dy)) scale(var(--fm-xl-size, 1)) rotate(120deg); } }
    .fm-xl-reveal { animation: fm-xl-reveal 650ms ease-out both; }
    @keyframes fm-xl-reveal { 0% { opacity: .2; filter: blur(4px); } 60% { opacity: 1; filter: blur(0); } 100% { opacity: 1; filter: none; } }
    [data-post-translate-hoverable] { transition: background-color .2s; border-radius: 6px; }
    @media (hover: hover) { [data-post-translate-hoverable]:hover { background: rgb(var(--dashboard-primary-rgb, 9 105 218) / .04); } }
    @media (prefers-reduced-motion: reduce) { .fm-xl-sparkle { display: none; } .fm-xl-reveal { animation: none; } }
  `;
  const POST_TRANSLATE_BODIES = "[data-repo-record-detail] [data-repo-record-body], [data-repo-record-detail] [data-repo-issue-comment-text]";
  const postTranslateUi = {
    posts: new WeakMap(),
    translated: new Map(),
    translators: new Map(),
    cache: new Map(),
    detector: null,
    availability: new Map(),
    failed: new Set(),
    thread: "",
    originals: false,
    machine: new Map(),
    hover: null,
    pending: [],
    scheduled: false,
    iconsFrame: 0,
    words: null,
    names: new Map(),
    observer: null,
    queue: Promise.resolve(),
  };

  function postTranslateCanHover(scope = globalThis) {
    try {
      return scope.matchMedia?.("(hover: hover) and (pointer: fine)")?.matches === true;
    } catch (_) {
      return false;
    }
  }

  function postTranslateApi(scope = globalThis) {
    const translator = scope?.Translator;
    const detector = scope?.LanguageDetector;
    return {
      translator: translator && typeof translator.create === "function" && typeof translator.availability === "function" ? translator : null,
      detector: detector && typeof detector.create === "function" ? detector : null,
    };
  }

  function postTranslateReadPrefs(storage = globalThis.localStorage) {
    let saved = null;
    try { saved = JSON.parse(storage?.getItem(POST_TRANSLATE_PREFS_KEY) || "null"); } catch (_) { saved = null; }
    const target = postTranslateCode(saved?.target);
    return { target, auto: saved?.auto === true, machine: saved?.machine !== false };
  }

  function postTranslateWritePrefs(prefs, storage = globalThis.localStorage) {
    const clean = { target: postTranslateCode(prefs?.target), auto: prefs?.auto === true, machine: prefs?.machine !== false };
    try { storage?.setItem(POST_TRANSLATE_PREFS_KEY, JSON.stringify(clean)); } catch (_) {}
    return clean;
  }

  function postTranslateCode(value) {
    const raw = String(value || "").trim().replace(/_/g, "-");
    if (!/^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8}){0,3}$/.test(raw)) return "";
    const parts = raw.split("-");
    const primary = parts[0].toLowerCase();
    if (primary === "zh") {
      const rest = parts.slice(1).map((part) => part.toLowerCase());
      return rest.some((part) => ["hant", "tw", "hk", "mo"].includes(part)) ? "zh-Hant" : "zh";
    }
    if (primary === "iw") return "he";
    if (primary === "pes" || primary === "prs") return "fa";
    return primary;
  }

  function postTranslateSame(a, b) {
    const left = postTranslateCode(a);
    const right = postTranslateCode(b);
    return Boolean(left) && left === right;
  }

  function postTranslateReaderLanguage(prefs = postTranslateReadPrefs(), nav = globalThis.navigator) {
    if (prefs.target) return prefs.target;
    const list = Array.isArray(nav?.languages) && nav.languages.length ? nav.languages : [nav?.language];
    for (const item of list) {
      const code = postTranslateCode(item);
      if (code) return code;
    }
    return "en";
  }

  function postTranslateLanguageName(code, displayIn = "en") {
    const clean = postTranslateCode(code);
    if (!clean) return "";
    const key = `${displayIn || "en"}\u0000${clean}`;
    const names = postTranslateUi.names;
    if (names.has(key)) return names.get(key);
    let found = clean;
    try {
      const name = new Intl.DisplayNames([displayIn || "en"], { type: "language" }).of(clean);
      if (name && name !== clean) found = name;
    } catch (_) {}
    names.set(key, found);
    return found;
  }

  function postTranslateNativeName(code) {
    return postTranslateLanguageName(code, postTranslateCode(code) || "en");
  }

  function postTranslateSkipped(element) {
    if (!element || element.nodeType !== 1) return false;
    if (POST_TRANSLATE_SKIP.has(String(element.tagName || "").toUpperCase())) return true;
    if (element.getAttribute?.("translate") === "no") return true;
    return element.hasAttribute?.("data-post-translate") === true;
  }

  function postTranslateSegments(root) {
    const segments = [];
    let open = false;
    const walk = (element) => {
      for (const child of Array.from(element.childNodes || [])) {
        if (child.nodeType === 3) {
          if (!open) {
            segments.push([]);
            open = true;
          }
          segments[segments.length - 1].push(child);
          continue;
        }
        if (child.nodeType !== 1) continue;
        if (postTranslateSkipped(child)) {
          open = false;
          continue;
        }
        if (POST_TRANSLATE_INLINE.has(String(child.tagName || "").toUpperCase())) {
          walk(child);
          continue;
        }
        open = false;
        walk(child);
        open = false;
      }
    };
    if (root) walk(root);
    return segments.filter((nodes) => postTranslateWorthy(nodes.map((node) => node.nodeValue || "").join("")));
  }

  function postTranslateWorthy(text) {
    const value = String(text || "").trim();
    if (!value || !/\p{L}/u.test(value)) return false;
    if (/^(?:https?:\/\/|www\.)\S+$/i.test(value)) return false;
    return !/^[@#][\w.-]+$/.test(value);
  }

  function postTranslatePlainText(root, limit = POST_TRANSLATE_DETECT_CHARS) {
    let text = "";
    for (const nodes of postTranslateSegments(root)) {
      text += `${nodes.map((node) => node.nodeValue || "").join("").trim()}\n`;
      if (text.length >= limit) break;
    }
    return text.slice(0, limit).trim();
  }

  function postTranslateProse(text) {
    return String(text || "").slice(0, POST_TRANSLATE_DETECT_CHARS)
      .replace(/(?:https?:\/\/|www\.)\S+|[@#][\w.-]+|\S*[\/_.]\S*[\/_.]\S*|\S*\d\S*/gi, " ");
  }

  function postTranslateScriptGuess(text) {
    const sample = postTranslateProse(text);
    const count = (pattern) => (sample.match(pattern) || []).length;
    const letters = count(/\p{L}/gu);
    if (!letters) return "";
    const kana = count(/[぀-ヿ]/g);
    const han = count(/\p{Script=Han}/gu);
    const hangul = count(/\p{Script=Hangul}/gu);
    const arabic = count(/\p{Script=Arabic}/gu);
    const share = (value) => value / letters;
    if (share(kana) > 0.1) return "ja";
    if (share(hangul) > 0.3) return "ko";
    if (share(han) > 0.3 || (han >= 12 && share(han) > 0.12)) return "zh";
    if (share(arabic) > 0.5) {
      const persian = count(/[پچژگکی]/g);
      const arabicOnly = count(/[ةيىأإ]/g);
      if (persian >= 2 && persian > arabicOnly) return "fa";
      if (arabicOnly >= 2 && persian === 0) return "ar";
      return "";
    }
    const cyrillic = count(/\p{Script=Cyrillic}/gu);
    if (share(cyrillic) > 0.5) {
      if (count(/[әғқңөұүһ]/gi) >= 2) return "kk";
      const ukrainian = count(/[іїєґ]/gi);
      const russian = count(/[ыэъё]/gi);
      if (ukrainian >= 2 && ukrainian > russian) return "uk";
      return ukrainian && !russian ? "" : "ru";
    }
    if (count(/[ăđơư]/gi) >= 2 && count(/[ạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ]/gi) >= 2) return "vi";
    const scripts = [["th", /\p{Script=Thai}/gu], ["he", /\p{Script=Hebrew}/gu], ["el", /\p{Script=Greek}/gu],
      ["my", /\p{Script=Myanmar}/gu], ["hy", /\p{Script=Armenian}/gu], ["ka", /\p{Script=Georgian}/gu]];
    for (const [code, pattern] of scripts) {
      if (share(count(pattern)) > 0.5) return code;
    }
    return "";
  }

  function postTranslateWordGuess(text) {
    const words = postTranslateProse(text).toLowerCase().match(/\p{L}[\p{L}'’-]*/gu) || [];
    if (words.length < 4) return "";
    if (!postTranslateUi.words) {
      postTranslateUi.words = Object.entries(POST_TRANSLATE_WORDS).map(([code, list]) => [code, new Set(list.split(" "))]);
    }
    const scores = postTranslateUi.words.map(([code, known]) => {
      let hits = words.filter((word) => known.has(word)).length;
      const letters = POST_TRANSLATE_LETTERS[code];
      if (letters) hits += Math.min(3, (postTranslateProse(text).match(letters) || []).length);
      return [code, hits / words.length, hits];
    }).sort((a, b) => b[1] - a[1]);
    const [best, second] = scores;
    if (best[1] < 0.06 || best[2] < 2) return "";
    return best[1] >= second[1] * 1.6 ? best[0] : "";
  }

  function postTranslateGuess(text) {
    return postTranslateScriptGuess(text) || postTranslateWordGuess(text);
  }

  function postTranslateWithin(promise, ms, fallback) {
    return new Promise((resolve) => {
      const timer = setTimeout(() => resolve(fallback), ms);
      Promise.resolve(promise).then((value) => {
        clearTimeout(timer);
        resolve(value);
      }, () => {
        clearTimeout(timer);
        resolve(fallback);
      });
    });
  }

  async function postTranslateDetectorInstance(api, gesture) {
    const cached = postTranslateUi.detector;
    if (cached?.api === api.detector && cached.instance) return cached.instance;
    const availability = typeof api.detector.availability === "function"
      ? await postTranslateWithin(api.detector.availability(), POST_TRANSLATE_CHECK_MS, "unknown") : "available";
    if (availability === "unavailable") return null;
    if (availability !== "available" && !gesture) return "later";
    if (cached?.api === api.detector && cached.failed) return null;
    postTranslateUi.detector = { api: api.detector, instance: null };
    const instance = await postTranslateWithin(api.detector.create(), POST_TRANSLATE_DETECTOR_MS, null);
    postTranslateUi.detector = { api: api.detector, instance, failed: !instance };
    return instance;
  }

  async function postTranslateDetect(text, api = postTranslateApi(), gesture = false) {
    const sample = String(text || "").trim();
    if (!sample) return { language: "", confidence: 0, source: "none" };
    let later = false;
    if (api.detector) {
      try {
        const instance = await postTranslateDetectorInstance(api, gesture);
        later = instance === "later";
        if (instance && !later) {
          const results = await instance.detect(sample);
          const best = Array.isArray(results) ? results[0] : null;
          const language = postTranslateCode(best?.detectedLanguage);
          const confidence = Number(best?.confidence) || 0;
          if (language && language !== "und" && confidence >= POST_TRANSLATE_MIN_CONFIDENCE) {
            return { language, confidence, source: "detector" };
          }
        }
      } catch (_) {}
    }
    const script = postTranslateScriptGuess(sample);
    if (script) return { language: script, confidence: 0.6, source: "script" };
    const words = postTranslateWordGuess(sample);
    if (words) return { language: words, confidence: 0.55, source: "words" };
    return { language: "", confidence: 0, source: later ? "later" : "none" };
  }

  async function postTranslateAvailability(source, target, api = postTranslateApi(), detection = "") {
    if (!api.translator) return "no-api";
    if (!target) return "unknown-source";
    if (!source) return detection === "later" ? "unknown" : "unknown-source";
    if (postTranslateSame(source, target)) return "same";
    const key = `${source}>${target}`;
    if (postTranslateUi.failed.has(key)) return "unavailable";
    if (postTranslateUi.translators.has(key)) return "available";
    const cached = postTranslateUi.availability.get(key);
    if (cached?.api === api.translator) return cached.answer;
    const answer = (async () => {
      try {
        const value = await postTranslateWithin(api.translator.availability({ sourceLanguage: source, targetLanguage: target }), POST_TRANSLATE_CHECK_MS, "unknown");
        return ["available", "downloadable", "downloading", "unavailable", "unknown"].includes(value) ? value : "unavailable";
      } catch (_) {
        return "unavailable";
      }
    })();
    postTranslateUi.availability.set(key, { api: api.translator, answer });
    const value = await answer;
    if (value === "unknown") postTranslateUi.availability.delete(key);
    return value;
  }

  async function postTranslateSha256(text) {
    const data = await globalThis.crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
    return Array.from(new Uint8Array(data), (byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  function postMachineDoc() {
    const detail = state.repoRecordDetail;
    if (!detail?.repo || detail.kind !== "issues" || !Number(detail.number)) return Promise.resolve(null);
    const key = `${repoKey(detail.repo)}#${Number(detail.number)}`;
    if (!postTranslateUi.machine.has(key)) {
      postTranslateUi.machine.set(key, (async () => {
        const path = `.forkmesh/translations/issues/${Number(detail.number)}.json`;
        try {
          const blobs = await fetchRepoBlobs(detail.repo, [path]);
          const doc = blobs[path] ? JSON.parse(blobText(blobs[path]) || "null") : null;
          return doc?.schema === POST_MACHINE_SCHEMA && doc.posts && typeof doc.posts === "object" ? doc : null;
        } catch (_) {
          return null;
        }
      })());
    }
    return postTranslateUi.machine.get(key);
  }

  async function postMachineEntry(record) {
    if (!record?.comment || !globalThis.crypto?.subtle) return null;
    for (let attempt = 0; attempt < 20 && !state.repoRecordDetail?.parsed; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
    const doc = await postMachineDoc();
    const entry = doc?.posts?.[record.comment];
    if (!entry || typeof entry.text !== "string" || !postTranslateSame(doc.target, record.target)) return null;
    if (!state.repoRecordDetail?.parsed) return null;
    const text = postTranslationSourceText(record.comment);
    if (!text || await postTranslateSha256(text) !== entry.hash) return null;
    return { source: postTranslateCode(entry.source), text: entry.text };
  }

  function postTranslateStyleOnce() {
    if (document.querySelector("style[data-post-translate-style]")) return;
    const style = document.createElement("style");
    style.setAttribute("data-post-translate-style", "");
    style.textContent = POST_TRANSLATE_STYLE;
    (document.head || document.documentElement).append(style);
  }

  function postTranslateSparkle(body, point) {
    if (!body?.isConnected || globalThis.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches) return;
    postTranslateStyleOnce();
    const box = body.getBoundingClientRect();
    const x = Number.isFinite(point?.x) ? point.x : box.left + Math.min(box.width, 240) / 2;
    const y = Number.isFinite(point?.y) ? point.y : box.top + Math.min(box.height, 60) / 2;
    const layer = document.createElement("div");
    layer.setAttribute("aria-hidden", "true");
    for (let index = 0; index < POST_TRANSLATE_SPARKLES; index += 1) {
      const spark = document.createElement("span");
      const angle = (Math.PI * 2 * index) / POST_TRANSLATE_SPARKLES + Math.random() * 0.6;
      const reach = 30 + Math.random() * 42;
      spark.className = "fm-xl-sparkle";
      spark.style.left = `${Math.round(x)}px`;
      spark.style.top = `${Math.round(y)}px`;
      spark.style.setProperty("--fm-xl-dx", `${Math.round(Math.cos(angle) * reach)}px`);
      spark.style.setProperty("--fm-xl-dy", `${Math.round(Math.sin(angle) * reach - 10)}px`);
      spark.style.setProperty("--fm-xl-size", (0.7 + Math.random() * 0.9).toFixed(2));
      spark.style.setProperty("--fm-xl-delay", `${Math.round(Math.random() * 120)}ms`);
      spark.style.setProperty("--fm-xl-life", `${Math.round(650 + Math.random() * 350)}ms`);
      layer.append(spark);
    }
    document.body.append(layer);
    setTimeout(() => layer.remove(), 1300);
  }

  function postTranslateReveal(body, point) {
    if (!body?.isConnected) return;
    postTranslateStyleOnce();
    postTranslateSparkle(body, point);
    body.classList.remove("fm-xl-reveal");
    void body.offsetWidth;
    body.classList.add("fm-xl-reveal");
    setTimeout(() => body.classList.remove("fm-xl-reveal"), 800);
  }

  function postTranslateHoverable(record) {
    return Boolean(record?.hover && record.showing === "original" && !record.busy && !record.hoverOff
      && (record.machineEntry || record.availability === "available"));
  }

  function postTranslateMarkHover(body, record) {
    if (postTranslateHoverable(record)) {
      postTranslateStyleOnce();
      body.setAttribute("data-post-translate-hoverable", "");
    } else {
      body.removeAttribute("data-post-translate-hoverable");
    }
  }

  function postTranslateHoverStart(body, point) {
    const record = postTranslateUi.posts.get(body);
    if (!postTranslateHoverable(record)) return;
    if (postTranslateUi.hover?.body === body) {
      postTranslateUi.hover.point = point;
      return;
    }
    postTranslateHoverStop();
    const hover = { body, point, timer: 0 };
    hover.timer = setTimeout(() => {
      if (postTranslateUi.hover !== hover) return;
      postTranslateUi.hover = null;
      const current = postTranslateUi.posts.get(body);
      if (!postTranslateHoverable(current)) return;
      if (current.machineEntry) {
        postMachineApply(body, current);
        postTranslateReveal(body, hover.point);
      } else {
        void postTranslateRun(body, false).then((done) => {
          if (done?.showing === "translation") postTranslateReveal(body, hover.point);
        });
      }
    }, POST_TRANSLATE_HOVER_MS);
    postTranslateUi.hover = hover;
  }

  function postTranslateHoverStop(body = null) {
    const hover = postTranslateUi.hover;
    if (!hover || (body && hover.body !== body)) return;
    clearTimeout(hover.timer);
    postTranslateUi.hover = null;
  }

  function postMachineApply(body, record) {
    const entry = record.machineEntry;
    if (!entry || record.showing === "translation") return;
    record.originalHtml = body.innerHTML;
    body.innerHTML = renderRecordMarkdown(entry.text, state.repoRecordDetail?.repo);
    body.setAttribute("lang", record.target);
    record.machine = entry;
    record.showing = "translation";
    record.saved = [];
    body.removeAttribute("data-post-translate-hoverable");
    postTranslatePaint(body);
  }

  async function postTranslateRecordAvailability(record, api) {
    if (record.alsoIn && postTranslateSame(record.alsoIn, record.target)) return "same";
    return postTranslateAvailability(record.source, record.target, api, record.detection);
  }

  async function postTranslateTranslator(source, target, api = postTranslateApi(), onProgress = null, stallMs = POST_TRANSLATE_STALL_MS) {
    const key = `${source}>${target}`;
    if (postTranslateUi.translators.has(key)) return postTranslateUi.translators.get(key);
    let stall = null;
    let fail = null;
    const watch = () => {
      clearTimeout(stall);
      stall = setTimeout(() => fail?.(new Error("translator_stalled")), stallMs);
    };
    const created = new Promise((resolve, reject) => {
      fail = reject;
      watch();
      Promise.resolve(api.translator.create({
        sourceLanguage: source,
        targetLanguage: target,
        monitor(monitor) {
          monitor?.addEventListener?.("downloadprogress", (event) => {
            watch();
            onProgress?.(Math.max(0, Math.min(1, Number(event?.loaded) || 0)));
          });
        },
      })).then(resolve, reject);
    }).finally(() => clearTimeout(stall));
    postTranslateUi.translators.set(key, created);
    try {
      return await created;
    } catch (error) {
      postTranslateUi.translators.delete(key);
      postTranslateUi.availability.delete(key);
      postTranslateUi.failed.add(key);
      throw error;
    }
  }

  async function postTranslateText(translator, source, target, text) {
    const key = `${source}>${target}\u0000${text}`;
    if (postTranslateUi.cache.has(key)) return postTranslateUi.cache.get(key);
    const result = String(await translator.translate(text));
    postTranslateUi.cache.set(key, result);
    return result;
  }

  async function postTranslateApply(root, record, translator) {
    const saved = [];
    try {
      for (const nodes of postTranslateSegments(root)) {
        const original = nodes.map((node) => node.nodeValue || "");
        const joined = original.join("");
        const lead = /^\s*/.exec(joined)[0];
        const trail = /\s*$/.exec(joined.slice(lead.length))[0];
        const core = joined.slice(lead.length, joined.length - trail.length);
        const translated = await postTranslateText(translator, record.source, record.target, core);
        nodes.forEach((node, index) => saved.push([node, original[index]]));
        nodes[0].nodeValue = `${lead}${translated}${trail}`;
        nodes.slice(1).forEach((node) => { node.nodeValue = ""; });
      }
    } catch (error) {
      saved.forEach(([node, value]) => { node.nodeValue = value; });
      throw error;
    }
    record.saved = saved;
    record.showing = "translation";
    root.setAttribute?.("lang", record.target);
    return record;
  }

  function postTranslateRestore(root, record) {
    (record.saved || []).forEach(([node, value]) => { node.nodeValue = value; });
    record.saved = [];
    if (record.machine && typeof record.originalHtml === "string") root.innerHTML = record.originalHtml;
    record.machine = null;
    record.originalHtml = null;
    record.showing = "original";
    if (record.source) root.setAttribute?.("lang", record.source);
    else root.removeAttribute?.("lang");
    return record;
  }

  function postTranslatePostKey(body) {
    const card = body.closest?.("[data-repo-issue-comment]") || body.closest?.("[data-repo-record-description]") || body.parentElement;
    const bar = card?.querySelector?.("[data-comment-reactions][data-comment-id]");
    const article = body.closest?.("[data-repo-record-detail]");
    const subject = bar?.getAttribute("data-comment-reactions") || `${article?.getAttribute("data-repo-record-detail") || "record"}:${location.pathname}`;
    const comment = bar?.getAttribute("data-comment-id") || (body.hasAttribute("data-repo-record-body") ? "body" : "");
    return { subject, comment, key: comment ? `${location.pathname}\u0000${subject}\u0000${comment}` : "" };
  }

  function postTranslateStatusText(record, availability) {
    const from = postTranslateLanguageName(record.source);
    if (record.showing === "translation" && record.machine) return `Machine translation from ${from || "another language"}`;
    if (record.showing === "translation") return `Translated from ${from || "another language"} · on your device`;
    if ((availability === "machine" || availability === "available") && record.hover && !record.hoverOff) {
      return from ? `Written in ${from} · hover to translate` : "Hover to translate";
    }
    if (availability === "machine") return from ? `Written in ${from} · translation ready` : "Translation ready";
    if (record.busy) return record.progress > 0 && record.progress < 1 ? `Downloading the ${from} model… ${Math.round(record.progress * 100)}%` : "Translating on your device…";
    if (record.error) return record.error;
    if (availability === "downloadable") return `${from} · the translation model downloads once to your device`;
    return from ? `Written in ${from}` : "";
  }

  function postTranslateBarHtml(record) {
    const target = postTranslateLanguageName(record.target);
    const availability = record.availability;
    const hidden = availability === "same" || availability === "pending" || availability === "unknown-source"
      || (availability === "no-api" && !record.source);
    const unsupported = availability === "no-api" || availability === "unavailable" || availability === "unknown-source";
    const from = postTranslateLanguageName(record.source);
    const marked = from ? ` This post is marked as ${from}, so the browser knows what to translate.` : "";
    const hint = availability === "no-api"
      ? `Your browser can't translate on-device; use its Translate page option.${marked}`
      : availability === "unavailable"
        ? `On-device translation from ${from || "this language"} to ${target} isn't available in this browser; use its Translate page option.${marked}`
        : availability === "unknown-source"
          ? "Couldn't tell which language this post is in; use your browser's Translate page option."
          : "";
    if (hidden) return `<div class="fm-xl-row"><span data-post-translations class="fm-xl-community"></span></div>`;
    const thread = postTranslateUi.thread
      ? `<button type="button" data-post-translate-originals class="fm-xl-button">Show all originals</button>`
      : `<button type="button" data-post-translate-all class="fm-xl-button">Translate every post on this page</button>`;
    const label = record.showing === "translation" ? "Show original" : `Translate to ${target}`;
    const options = Array.from(new Set([record.target, ...POST_TRANSLATE_LANGUAGES])).map((code) => `<option value="${escapeHtml(code)}"${code === record.target ? " selected" : ""}>${escapeHtml(postTranslateNativeName(code))}${postTranslateNativeName(code) === postTranslateLanguageName(code) ? "" : ` · ${escapeHtml(postTranslateLanguageName(code))}`}</option>`).join("");
    return `<div class="fm-xl-row"><button type="button" data-post-translate-toggle class="fm-xl-button"${unsupported ? ` aria-disabled="true" title="${escapeHtml(hint)}"` : ""}${record.busy ? " disabled" : ""}><i data-lucide="languages" class="h-4 w-4"></i><span>${escapeHtml(label)}</span></button>
      <span data-post-translate-status class="fm-xl-note" role="status" aria-live="polite">${escapeHtml(record.hintShown && hint ? hint : postTranslateStatusText(record, availability))}</span>
      <details data-post-translate-menu class="fm-xl-menu">
        <summary aria-label="Translation settings" title="Translation settings" class="fm-xl-icon"><i data-lucide="settings-2" class="h-4 w-4"></i></summary>
        <div class="fm-xl-pop" role="group" aria-label="Translation settings">
          <label class="fm-xl-field"><span>Translate posts to</span><select data-post-translate-target translate="no" class="fm-xl-select">${options}</select></label>
          <label class="fm-xl-check"><input type="checkbox" data-post-translate-machine${record.machineAuto !== false ? " checked" : ""} /><span>Use this site's translations</span></label>
          <label class="fm-xl-check"><input type="checkbox" data-post-translate-auto${record.auto ? " checked" : ""} /><span>Translate right away instead of on hover</span></label>
          ${thread}
          <p class="fm-xl-fine">This site's translations are made on its own server with open translation models. Other translations run on your device with your browser's built-in translator. Nothing is sent to a translation service.</p>
        </div>
      </details>
      <span data-post-translations class="fm-xl-community"></span></div>`;
  }

  function postTranslateRecordFor(body) {
    let record = postTranslateUi.posts.get(body);
    if (!record) {
      const prefs = postTranslateReadPrefs();
      const { key, subject, comment } = postTranslatePostKey(body);
      record = { key, subject, comment, source: "", target: postTranslateReaderLanguage(prefs), auto: prefs.auto, machineAuto: prefs.machine, hover: postTranslateCanHover(), availability: "pending", showing: "original", saved: [] };
      postTranslateUi.posts.set(body, record);
    }
    return record;
  }

  function postTranslateBarFor(body) {
    const next = body.nextElementSibling;
    return next?.hasAttribute?.("data-post-translate") ? next : null;
  }

  function postTranslatePaint(body) {
    const record = postTranslateUi.posts.get(body);
    const bar = postTranslateBarFor(body);
    if (!record || !bar) return;
    const open = bar.querySelector("[data-post-translate-menu]")?.open === true;
    const community = bar.querySelector("[data-post-translations]")?.innerHTML || "";
    const row = bar.querySelector(".fm-xl-row");
    if (row) row.outerHTML = postTranslateBarHtml(record);
    else bar.insertAdjacentHTML("afterbegin", postTranslateBarHtml(record));
    const menu = bar.querySelector("[data-post-translate-menu]");
    if (menu && open) menu.open = true;
    const slot = bar.querySelector("[data-post-translations]");
    if (slot) slot.innerHTML = community;
    postTranslateMarkHover(body, record);
    if (bar.querySelector("i[data-lucide]")) postTranslateIcons();
  }

  function postTranslateIcons() {
    if (postTranslateUi.iconsFrame) return;
    const run = () => {
      postTranslateUi.iconsFrame = 0;
      window.lucide?.createIcons?.();
    };
    postTranslateUi.iconsFrame = typeof requestAnimationFrame === "function" ? requestAnimationFrame(run) : setTimeout(run, 16);
  }

  function postTranslatePrepareBody(body) {
    if (!body.hasAttribute("dir")) body.setAttribute("dir", "auto");
    body.setAttribute("translate", "yes");
    body.querySelectorAll(POST_TRANSLATE_NO_TRANSLATE).forEach((node) => node.setAttribute("translate", "no"));
  }

  async function postTranslateDecorate(body) {
    if (!body?.isConnected || postTranslateUi.posts.has(body)) return;
    const record = postTranslateRecordFor(body);
    postTranslatePrepareBody(body);
    if (!postTranslateBarFor(body)) {
      const bar = document.createElement("div");
      bar.setAttribute("data-post-translate", "");
      bar.className = "fm-xl-bar px-4 pb-2";
      body.insertAdjacentElement("afterend", bar);
    }
    postTranslatePaint(body);
    void postTranslationAttach(body);
    const api = postTranslateApi();
    record.machineEntry = record.machineAuto === false ? null : await postMachineEntry(record);
    if (record.busy || record.showing === "translation") return;
    if (record.machineEntry) {
      record.source = record.machineEntry.source;
      record.availability = "machine";
      if (record.source && !body.hasAttribute("lang")) body.setAttribute("lang", record.source);
      postTranslatePaint(body);
      if (record.machineAuto !== false && !postTranslateUi.originals
          && (record.auto || !record.hover || postTranslateUi.thread === record.target)) {
        postMachineApply(body, record);
      }
      return;
    }
    const text = postTranslatePlainText(body);
    const detected = await postTranslateDetect(text, api);
    if (record.busy || record.showing === "translation") return;
    record.source = detected.language;
    record.detection = detected.source;
    const whole = postTranslatePlainText(body, POST_TRANSLATE_DETECT_CHARS * 10);
    record.alsoIn = "";
    for (let start = 0; start < whole.length && !record.alsoIn; start += POST_TRANSLATE_DETECT_CHARS) {
      const found = postTranslateWordGuess(whole.slice(start, start + POST_TRANSLATE_DETECT_CHARS));
      if (postTranslateSame(found, record.target)) record.alsoIn = found;
    }
    if (record.source && !body.hasAttribute("lang")) body.setAttribute("lang", record.source);
    const short = !record.source && (postTranslateProse(text).match(/\p{L}+/gu) || []).length < 12;
    const availability = short ? "unknown-source" : await postTranslateRecordAvailability(record, api);
    if (record.busy || record.showing === "translation") return;
    record.availability = availability;
    postTranslatePaint(body);
    const remembered = record.key ? postTranslateUi.translated.get(record.key) : "";
    const ready = record.availability === "available" || postTranslateUi.translators.has(`${record.source}>${record.target}`);
    const thread = postTranslateUi.thread === record.target && record.availability !== "unavailable";
    if (ready && (remembered === record.target || record.auto)) void postTranslateRun(body, false);
    else if (thread && POST_TRANSLATE_RUNNABLE.has(record.availability)) void postTranslateRun(body, false);
  }

  function postTranslateRun(body, gesture = true) {
    const record = postTranslateUi.posts.get(body);
    if (!record || record.busy || record.showing === "translation") return Promise.resolve(record);
    if (record.machineEntry) {
      postMachineApply(body, record);
      if (gesture && postTranslateUi.thread !== record.target) postTranslateThread(record.target);
      return Promise.resolve(record);
    }
    const api = postTranslateApi();
    if (!api.translator || !POST_TRANSLATE_RUNNABLE.has(record.availability)) {
      record.hintShown = true;
      postTranslatePaint(body);
      return Promise.resolve(record);
    }
    record.busy = true;
    record.error = "";
    record.progress = 0;
    postTranslatePaint(body);
    const onProgress = (loaded) => {
      record.progress = loaded;
      const status = postTranslateBarFor(body)?.querySelector("[data-post-translate-status]");
      if (status) status.textContent = postTranslateStatusText(record, record.availability);
    };
    const stallMs = record.availability === "unknown" ? POST_TRANSLATE_CHECK_MS * 2 : POST_TRANSLATE_STALL_MS;
    const start = record.source
      ? Promise.resolve(postTranslateTranslator(record.source, record.target, api, onProgress, stallMs))
      : postTranslateResolve(body, record, api, gesture, onProgress);
    start.catch(() => {});
    const job = postTranslateUi.queue.then(async () => {
      try {
        const translator = await start;
        if (!translator || !body.isConnected) return record;
        await postTranslateApply(body, record, translator);
        record.availability = "available";
        if (record.key) postTranslateUi.translated.set(record.key, record.target);
        if (gesture && postTranslateUi.thread !== record.target) postTranslateThread(record.target);
      } catch (error) {
        const stalled = String(error?.message || "") === "translator_stalled";
        if (stalled || postTranslateUi.failed.has(`${record.source}>${record.target}`)) {
          record.availability = "unavailable";
          record.hintShown = true;
          postTranslateRepaintPair(record.source, record.target, body);
        }
        record.error = gesture && !record.hintShown ? "The on-device translation didn't finish. Use your browser's Translate page option." : "";
      } finally {
        record.busy = false;
        postTranslatePaint(body);
      }
      return record;
    });
    postTranslateUi.queue = job.catch(() => {});
    return job;
  }

  async function postTranslateResolve(body, record, api, gesture, onProgress) {
    if (!record.source) {
      const detected = await postTranslateDetect(postTranslatePlainText(body), api, gesture);
      record.source = detected.language;
      record.detection = detected.source;
      if (record.source && !body.hasAttribute("lang")) body.setAttribute("lang", record.source);
    }
    record.availability = await postTranslateRecordAvailability(record, api);
    if (record.availability === "unknown" && record.source) record.availability = "downloadable";
    if (!["available", "downloadable", "downloading"].includes(record.availability)) {
      record.hintShown = record.availability !== "same";
      return null;
    }
    return postTranslateTranslator(record.source, record.target, api, onProgress);
  }

  function postTranslateToggle(body) {
    const record = postTranslateUi.posts.get(body);
    if (!record) return;
    if (record.showing === "translation") {
      postTranslateRestore(body, record);
      record.hoverOff = true;
      if (record.key) postTranslateUi.translated.delete(record.key);
      postTranslatePaint(body);
      return;
    }
    record.hoverOff = false;
    void postTranslateRun(body, true);
  }

  function postTranslateThread(target) {
    postTranslateUi.thread = target;
    postTranslateUi.originals = false;
    postTranslateBodies().forEach((body) => {
      const record = postTranslateUi.posts.get(body);
      if (!record) return;
      postTranslatePaint(body);
      if (record.showing !== "translation" && record.source && record.target === target
          && POST_TRANSLATE_RUNNABLE.has(record.availability) && record.availability !== "pending") {
        void postTranslateRun(body, false);
      }
    });
  }

  function postTranslateOriginals() {
    postTranslateUi.thread = "";
    postTranslateUi.originals = true;
    postTranslateBodies().forEach((body) => {
      const record = postTranslateUi.posts.get(body);
      if (!record) return;
      if (record.showing === "translation") postTranslateRestore(body, record);
      if (record.key) postTranslateUi.translated.delete(record.key);
      postTranslatePaint(body);
    });
  }

  function postTranslateRepaintPair(source, target, except) {
    postTranslateBodies().forEach((body) => {
      const record = postTranslateUi.posts.get(body);
      if (!record || body === except || record.source !== source || record.target !== target) return;
      if (record.showing === "translation" || record.busy) return;
      record.availability = "unavailable";
      postTranslatePaint(body);
    });
  }

  function postTranslateBodies(root = document) {
    return Array.from(root.querySelectorAll?.(POST_TRANSLATE_BODIES) || []);
  }

  function postTranslateAll() {
    const first = postTranslateBodies().map((body) => postTranslateUi.posts.get(body))
      .find((record) => record && record.source && record.availability !== "same");
    if (first) postTranslateUi.thread = first.target;
    postTranslateBodies().forEach((body) => {
      const record = postTranslateUi.posts.get(body);
      if (record && record.showing !== "translation" && record.availability !== "same") void postTranslateRun(body, true);
    });
    postTranslateUi.originals = false;
    postTranslateBodies().forEach((body) => {
      if (postTranslateUi.posts.has(body)) postTranslatePaint(body);
    });
  }

  async function postTranslateRetarget(target, auto, machine = postTranslateReadPrefs().machine) {
    const prefs = postTranslateWritePrefs({ ...postTranslateReadPrefs(), target, auto, machine });
    const api = postTranslateApi();
    for (const body of postTranslateBodies()) {
      const record = postTranslateUi.posts.get(body);
      if (!record) continue;
      const changed = record.target !== postTranslateReaderLanguage(prefs);
      if (changed) postTranslateUi.thread = "";
      if (changed && record.showing === "translation") postTranslateRestore(body, record);
      record.target = postTranslateReaderLanguage(prefs);
      record.auto = prefs.auto;
      record.machineAuto = prefs.machine;
      record.hintShown = false;
      if (changed) {
        record.availability = await postTranslateRecordAvailability(record, api);
        record.machineEntry = record.availability === "same" || !prefs.machine ? null : await postMachineEntry(record);
        if (record.machineEntry) record.availability = "machine";
      }
      if (!prefs.machine && record.machine) postTranslateRestore(body, record);
      postTranslatePaint(body);
      if (prefs.machine && record.machineEntry && !postTranslateUi.originals && (prefs.auto || !record.hover)) {
        postMachineApply(body, record);
        continue;
      }
      if (record.auto && record.availability === "available") void postTranslateRun(body, false);
    }
  }

  function postTranslateScan(root = document) {
    const scope = root?.querySelectorAll ? root : document;
    const bodies = scope.matches?.("[data-repo-record-body], [data-repo-issue-comment-text]") ? [scope] : [];
    bodies.concat(postTranslateBodies(scope)).forEach((body) => {
      if (body.closest("[data-repo-record-detail]") && !postTranslateUi.posts.has(body)) postTranslateUi.pending.push(body);
    });
    postTranslateSchedule();
  }

  function postTranslateSchedule() {
    if (postTranslateUi.scheduled || !postTranslateUi.pending.length) return;
    postTranslateUi.scheduled = true;
    const slice = (deadline) => {
      postTranslateUi.scheduled = false;
      const started = Date.now();
      let handled = 0;
      while (postTranslateUi.pending.length) {
        const left = typeof deadline?.timeRemaining === "function" ? deadline.timeRemaining() : 8 - (Date.now() - started);
        if (left <= 1 && (handled || !deadline?.didTimeout)) break;
        handled += 1;
        const body = postTranslateUi.pending.shift();
        if (body?.isConnected && !postTranslateUi.posts.has(body)) void postTranslateDecorate(body);
      }
      postTranslateSchedule();
    };
    if (typeof requestIdleCallback === "function") requestIdleCallback(slice, { timeout: 500 });
    else setTimeout(slice, 16);
  }

  function postTranslatePlaceMenu(menu) {
    const summary = menu.querySelector("summary");
    const pop = menu.querySelector(".fm-xl-pop");
    if (!summary || !pop) return;
    const anchor = summary.getBoundingClientRect();
    const width = pop.offsetWidth || 280;
    const height = pop.offsetHeight || 200;
    const left = Math.max(8, Math.min(anchor.left, window.innerWidth - width - 8));
    const below = anchor.bottom + 4;
    const top = below + height > window.innerHeight - 8 ? Math.max(8, anchor.top - height - 4) : below;
    pop.style.left = `${Math.round(left)}px`;
    pop.style.top = `${Math.round(top)}px`;
  }

  function initPostTranslate() {
    if (postTranslateUi.observer || typeof MutationObserver !== "function" || !document.body) return;
    postTranslateScan(document);
    postTranslateUi.observer = new MutationObserver((records) => {
      for (const record of records) {
        for (const node of record.addedNodes) {
          if (node.nodeType === 1 && !node.hasAttribute("data-post-translate") && !node.closest("[data-post-translate]")) postTranslateScan(node);
        }
      }
    });
    postTranslateUi.observer.observe(document.body, { childList: true, subtree: true });
    document.addEventListener("click", (event) => {
      const bar = event.target.closest?.("[data-post-translate]");
      if (!bar) {
        document.querySelectorAll("[data-post-translate-menu][open]").forEach((menu) => { menu.open = false; });
        return;
      }
      const body = bar.previousElementSibling;
      if (postTranslationClick(event, bar, body)) return;
      if (event.target.closest("[data-post-translate-toggle]")) {
        event.preventDefault();
        postTranslateToggle(body);
      } else if (event.target.closest("[data-post-translate-all]")) {
        event.preventDefault();
        bar.querySelector("[data-post-translate-menu]")?.removeAttribute("open");
        postTranslateAll();
      } else if (event.target.closest("[data-post-translate-originals]")) {
        event.preventDefault();
        bar.querySelector("[data-post-translate-menu]")?.removeAttribute("open");
        postTranslateOriginals();
      }
    });
    document.addEventListener("pointerover", (event) => {
      if (event.pointerType && event.pointerType !== "mouse") return;
      const body = event.target.closest?.("[data-post-translate-hoverable]");
      if (body) postTranslateHoverStart(body, { x: event.clientX, y: event.clientY });
    });
    document.addEventListener("pointermove", (event) => {
      const hover = postTranslateUi.hover;
      if (hover) hover.point = { x: event.clientX, y: event.clientY };
    }, { passive: true });
    document.addEventListener("pointerout", (event) => {
      const body = event.target.closest?.("[data-post-translate-hoverable]");
      if (body && !body.contains(event.relatedTarget)) postTranslateHoverStop(body);
    });
    document.addEventListener("toggle", (event) => {
      const menu = event.target;
      if (menu?.matches?.("[data-post-translate-menu]") && menu.open) postTranslatePlaceMenu(menu);
    }, true);
    const closeMenus = () => document.querySelectorAll("[data-post-translate-menu][open]").forEach((menu) => { menu.open = false; });
    window.addEventListener("resize", closeMenus);
    window.addEventListener("scroll", closeMenus, { passive: true });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") closeMenus();
    });
    document.addEventListener("submit", (event) => {
      const form = event.target.closest?.("[data-post-translation-form]");
      if (!form) return;
      event.preventDefault();
      void postTranslationSubmit(form);
    });
    document.addEventListener("change", (event) => {
      const langSelect = event.target.closest?.("[data-post-translation-lang]");
      if (langSelect) {
        langSelect.closest("form")?.querySelector("[data-post-translation-body]")?.setAttribute("lang", langSelect.value);
        return;
      }
      const control = event.target.closest?.("[data-post-translate-target], [data-post-translate-auto], [data-post-translate-machine]");
      if (!control) return;
      const bar = control.closest("[data-post-translate]");
      const target = bar?.querySelector("[data-post-translate-target]")?.value || "";
      const auto = bar?.querySelector("[data-post-translate-auto]")?.checked === true;
      const machine = bar?.querySelector("[data-post-translate-machine]")?.checked !== false;
      void postTranslateRetarget(target, auto, machine);
    });
  }

  const POST_TRANSLATION_PLUGIN = "community-translations";
  const POST_TRANSLATION_MAX_BODY = 20000;
  const postTranslationUi = { data: new Map(), loads: new Map() };

  function postTranslationParts(subject) {
    const match = /^(issue|pull|discussion):([1-9]\d{0,9})$/.exec(String(subject || ""));
    return match ? { kind: match[1], number: Number(match[2]) } : null;
  }

  function postTranslationRepo() {
    return state.repoRecordDetail?.repo || state.selectedRepo || null;
  }

  function postTranslationBase(repo) {
    return repo ? repoApiBase(repo) : "";
  }

  function postTranslationSigningRepo(base) {
    const parts = String(base || "").split("/");
    return { owner: decodeURIComponent(parts[3] || ""), name: decodeURIComponent(parts[4] || "") };
  }

  function postTranslationSigningText(base, subject, comment, lang, pub, ts, bodyHash) {
    const repo = postTranslationSigningRepo(base);
    return ["forkmesh-translation-v1", `${repo.owner.trim().toLowerCase()}/${repo.name.trim().toLowerCase()}`,
      subject, comment, lang, pub, String(ts), bodyHash].join("\n");
  }

  function postTranslationCleanBody(value) {
    return String(value || "").replace(/\r\n?/g, "\n").replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "").replace(/\s+$/, "");
  }

  function postTranslationLangCode(value) {
    const raw = String(value || "").trim().replace(/_/g, "-");
    if (!raw) return "";
    const parts = raw.split("-");
    parts[0] = parts[0].toLowerCase();
    const code = parts.join("-");
    return /^[a-z]{2,3}(?:-[A-Za-z0-9]{2,8}){0,2}$/.test(code) ? code : "";
  }

  function postTranslationSourceText(comment, parsed = state.repoRecordDetail?.parsed) {
    if (!parsed) return "";
    if (comment === "body") return String(parsed.body || "");
    const events = Array.isArray(parsed.issueEvents) ? parsed.issueEvents : [];
    const original = events.find((event) => event && String(event.id || "") === comment);
    if (!original) return "";
    const edits = events.filter((event) => event?.type === "edit" && String(event.target || "") === comment);
    return String((edits.length ? edits[edits.length - 1].body : original.body) || "");
  }

  function postTranslationLanguages(entries) {
    const counts = new Map();
    (entries || []).forEach((entry) => counts.set(entry.lang, (counts.get(entry.lang) || 0) + 1));
    return Array.from(counts, ([lang, count]) => ({ lang, count }));
  }

  function postTranslationChipsHtml(entries, signedIn, openLang = "") {
    const languages = postTranslationLanguages(entries);
    const chips = languages.map(({ lang, count }) => `<button type="button" data-post-translation-chip="${escapeHtml(lang)}" aria-pressed="${lang === openLang ? "true" : "false"}" class="fm-xl-chip" lang="${escapeHtml(lang)}">${escapeHtml(postTranslateNativeName(lang) || lang)}${count > 1 ? ` <span aria-label="${count} translations">· ${count}</span>` : ""}</button>`).join("");
    const label = languages.length ? `<span class="fm-xl-note">Community translations:</span>${chips}` : "";
    const add = signedIn ? `<button type="button" data-post-translation-add class="fm-xl-link">${languages.length ? "Add yours" : "Add a translation"}</button>` : "";
    return label + add;
  }

  function postTranslationEntryHtml(entry, options = {}) {
    const lang = escapeHtml(entry.lang);
    const name = postTranslateLanguageName(entry.lang) || entry.lang;
    const stale = options.sourceHash && entry.source && entry.source !== options.sourceHash;
    const when = entry.updatedAt ? formatTimeAgo(entry.updatedAt) : "";
    const actions = [
      entry.mine ? `<button type="button" data-post-translation-edit="${lang}" class="fm-xl-link">Edit</button>` : "",
      entry.mine || options.canModerate ? `<button type="button" data-post-translation-remove="${lang}" data-post-translation-account="${escapeHtml(entry.mine ? "" : entry.name)}" class="fm-xl-link fm-xl-danger">Remove</button>` : "",
    ].join("");
    return `<div data-post-translation-entry="${lang}" class="grid gap-2">
        <p class="fm-xl-meta"><span>Community translation into ${escapeHtml(name)} by ${escapeHtml(entry.name)}${entry.agent ? " (agent)" : ""}${when ? ` · ${escapeHtml(when)}` : ""}</span><span>· the original above is the source of truth</span>${stale ? `<span class="text-attention">· the original was edited after this translation</span>` : ""}${actions}</p>
        <div class="${MD_BODY_CLASS}" lang="${lang}" dir="auto" translate="yes">${renderRecordMarkdown(entry.body, postTranslationRepo())}</div>
      </div>`;
  }

  function postTranslationFormHtml(lang, body = "") {
    const options = Array.from(new Set([lang, ...POST_TRANSLATE_LANGUAGES].filter(Boolean))).map((code) => `<option value="${escapeHtml(code)}"${code === lang ? " selected" : ""}>${escapeHtml(postTranslateNativeName(code))}</option>`).join("");
    return `<form data-post-translation-form class="grid gap-2">
        <label class="fm-xl-field"><span class="fm-xl-meta">Translate this post into</span><select data-post-translation-lang translate="no" class="fm-xl-select">${options}</select></label>
        <textarea data-post-translation-body rows="8" maxlength="${POST_TRANSLATION_MAX_BODY}" dir="auto" lang="${escapeHtml(lang)}" class="fm-xl-textarea" aria-label="Translation (Markdown)" placeholder="Markdown. Keep links and code as they are.">${escapeHtml(body)}</textarea>
        <p class="fm-xl-fine">Shown beside the original with your name, signed by this browser. It never replaces the post.</p>
        <p data-post-translation-hint class="fm-xl-fine" role="status" aria-live="polite"></p>
        <div class="fm-xl-actions"><button type="button" data-post-translation-cancel class="fm-xl-button">Cancel</button><button type="submit" class="fm-xl-button">Publish translation</button></div>
      </form>`;
  }

  async function postTranslationLoad(subject, fresh = false) {
    const parts = postTranslationParts(subject);
    const base = postTranslationBase(postTranslationRepo());
    if (!parts || !base) return null;
    await instanceInfo();
    if (instancePluginOff(POST_TRANSLATION_PLUGIN)) return null;
    const key = `${base.toLowerCase()}|${subject}`;
    if (!fresh && postTranslationUi.data.has(key)) return postTranslationUi.data.get(key);
    if (!fresh && postTranslationUi.loads.has(key)) return postTranslationUi.loads.get(key);
    const load = (async () => {
      try {
        const data = await fetchJson(`${base}/translations?kind=${parts.kind}&number=${parts.number}`, { fresh: fresh || Boolean(state.session?.sessionToken) });
        if (!data || data.ok === false || typeof data.translations !== "object") return null;
        const entry = { translations: data.translations || {}, signedIn: data.signedIn === true || Boolean(state.session?.sessionToken) };
        postTranslationUi.data.set(key, entry);
        return entry;
      } catch (_) {
        return null;
      } finally {
        postTranslationUi.loads.delete(key);
      }
    })();
    postTranslationUi.loads.set(key, load);
    return load;
  }

  function postTranslationPaintChips(body, entry) {
    const record = postTranslateUi.posts.get(body);
    const slot = postTranslateBarFor(body)?.querySelector("[data-post-translations]");
    if (!record || !slot || !entry) return;
    const open = postTranslateBarFor(body)?.querySelector("[data-post-translation-panel]")?.getAttribute("data-post-translation-panel") || "";
    slot.innerHTML = postTranslationChipsHtml(entry.translations[record.comment] || [], entry.signedIn, open);
  }

  async function postTranslationAttach(body) {
    const record = postTranslateUi.posts.get(body);
    if (!record?.comment || !postTranslationParts(record.subject)) return;
    for (let attempt = 0; attempt < 20 && !postTranslationRepo(); attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 250));
      if (!body.isConnected) return;
    }
    const entry = await postTranslationLoad(record.subject);
    if (entry && body.isConnected) postTranslationPaintChips(body, entry);
  }

  function postTranslationRefreshSubject(subject, entry) {
    postTranslateBodies().forEach((body) => {
      if (postTranslateUi.posts.get(body)?.subject === subject) postTranslationPaintChips(body, entry);
    });
  }

  function postTranslationPanel(bar, mode) {
    let panel = bar.querySelector("[data-post-translation-panel]");
    if (!panel) {
      panel = document.createElement("div");
      panel.className = "fm-xl-panel";
      bar.appendChild(panel);
    }
    panel.setAttribute("data-post-translation-panel", mode);
    return panel;
  }

  function postTranslationClose(bar) {
    bar.querySelector("[data-post-translation-panel]")?.remove();
    bar.querySelectorAll("[data-post-translation-chip]").forEach((chip) => chip.setAttribute("aria-pressed", "false"));
  }

  async function postTranslationShow(body, lang) {
    const record = postTranslateUi.posts.get(body);
    const bar = postTranslateBarFor(body);
    const parts = postTranslationParts(record?.subject);
    const base = postTranslationBase(postTranslationRepo());
    if (!record || !bar || !parts || !base) return;
    if (bar.querySelector("[data-post-translation-panel]")?.getAttribute("data-post-translation-panel") === lang) {
      postTranslationClose(bar);
      return;
    }
    const panel = postTranslationPanel(bar, lang);
    bar.querySelectorAll("[data-post-translation-chip]").forEach((chip) => chip.setAttribute("aria-pressed", chip.getAttribute("data-post-translation-chip") === lang ? "true" : "false"));
    panel.innerHTML = `<p class="fm-xl-fine">Loading the translation…</p>`;
    try {
      const data = await fetchJson(`${base}/translations?kind=${parts.kind}&number=${parts.number}&comment=${encodeURIComponent(record.comment)}&lang=${encodeURIComponent(lang)}`, { fresh: true });
      const entries = data?.translations?.[record.comment] || [];
      const source = postTranslationSourceText(record.comment);
      const sourceHash = source ? await sha256HexLower(source) : "";
      const canModerate = Boolean(state.repoRecordDetail?.parsed?.issueMutationAuthorized);
      if (panel.getAttribute("data-post-translation-panel") !== lang) return;
      panel.innerHTML = entries.length
        ? entries.map((entry) => postTranslationEntryHtml(entry, { sourceHash, canModerate })).join("")
        : `<p class="fm-xl-fine">This translation was removed.</p>`;
      panel._entries = entries;
      window.lucide?.createIcons?.();
    } catch (_) {
      panel.innerHTML = `<p class="fm-xl-fine">The translation could not be loaded. Try again.</p>`;
    }
  }

  function postTranslationOpenForm(body, lang = "", text = "") {
    const record = postTranslateUi.posts.get(body);
    const bar = postTranslateBarFor(body);
    if (!record || !bar) return;
    if (!state.session?.sessionToken) return;
    const panel = postTranslationPanel(bar, "form");
    const fallback = postTranslateSame(record.source, record.target) ? (postTranslateSame(record.source, "en") ? "zh" : "en") : record.target;
    panel.innerHTML = postTranslationFormHtml(lang || fallback, text);
    panel.querySelector("[data-post-translation-body]")?.focus();
  }

  function postTranslationErrorText(code) {
    if (code === "rate_limited") return "Slow down a little and try again in a minute.";
    if (code === "daily_limit") return "You've reached today's translation limit.";
    if (code === "too_many_translations" || code === "record_full") return "This thread has reached its translation limit.";
    if (code === "unauthorized") return "Sign in again to publish a translation.";
    if (code === "bad_signature") return "This browser couldn't sign the translation.";
    if (code === "invalid_body") return `Write the translation first (up to ${POST_TRANSLATION_MAX_BODY} characters).`;
    if (code === "feature_disabled" || code === "plugin_disabled") return "Translations are switched off here.";
    return "The translation didn't save. Try again.";
  }

  async function postTranslationSign(base, subject, comment, lang, text) {
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Date.now();
    const canonical = postTranslationSigningText(base, subject, comment, lang, pub, ts, await sha256HexLower(text));
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    return { pub, sig, ts };
  }

  async function postTranslationSend(payload) {
    const base = postTranslationBase(postTranslationRepo());
    const response = await fetch(`${base}/translations`, {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ ...payload, sessionToken: state.session?.sessionToken || "" }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) throw new Error(String(data.error || `HTTP ${response.status}`));
    return data;
  }

  async function postTranslationSubmit(form) {
    const bar = form.closest("[data-post-translate]");
    const body = bar?.previousElementSibling;
    const record = body ? postTranslateUi.posts.get(body) : null;
    const parts = postTranslationParts(record?.subject);
    const base = postTranslationBase(postTranslationRepo());
    const hint = form.querySelector("[data-post-translation-hint]");
    const lang = postTranslationLangCode(form.querySelector("[data-post-translation-lang]")?.value);
    const text = postTranslationCleanBody(form.querySelector("[data-post-translation-body]")?.value);
    if (!record || !parts || !base || !lang) return;
    if (!text.trim() || text.length > POST_TRANSLATION_MAX_BODY) {
      if (hint) hint.textContent = postTranslationErrorText("invalid_body");
      return;
    }
    const buttons = form.querySelectorAll("button");
    buttons.forEach((button) => { button.disabled = true; });
    if (hint) hint.textContent = "Signing and publishing…";
    try {
      const source = postTranslationSourceText(record.comment);
      const signature = await postTranslationSign(base, record.subject, record.comment, lang, text);
      await postTranslationSend({ kind: parts.kind, number: parts.number, comment: record.comment, lang, body: text,
        source: source ? await sha256HexLower(source) : "", ...signature });
      const entry = await postTranslationLoad(record.subject, true);
      postTranslationClose(bar);
      if (entry) postTranslationRefreshSubject(record.subject, entry);
      await postTranslationShow(body, lang);
    } catch (error) {
      buttons.forEach((button) => { button.disabled = false; });
      if (hint) hint.textContent = postTranslationErrorText(error?.message === "signing_unavailable" ? "bad_signature" : String(error?.message || ""));
    }
  }

  async function postTranslationRemove(body, lang, account) {
    const record = postTranslateUi.posts.get(body);
    const parts = postTranslationParts(record?.subject);
    const bar = postTranslateBarFor(body);
    if (!record || !parts || !bar) return;
    try {
      await postTranslationSend({ kind: parts.kind, number: parts.number, comment: record.comment, lang, remove: true, ...(account ? { account } : {}) });
      postTranslationClose(bar);
      const entry = await postTranslationLoad(record.subject, true);
      if (entry) postTranslationRefreshSubject(record.subject, entry);
    } catch (error) {
      const panel = bar.querySelector("[data-post-translation-panel]");
      if (panel) panel.insertAdjacentHTML("beforeend", `<p class="fm-xl-fine" role="status">${escapeHtml(postTranslationErrorText(String(error?.message || "")))}</p>`);
    }
  }

  function postTranslationClick(event, bar, body) {
    const chip = event.target.closest("[data-post-translation-chip]");
    if (chip) {
      event.preventDefault();
      void postTranslationShow(body, chip.getAttribute("data-post-translation-chip") || "");
      return true;
    }
    if (event.target.closest("[data-post-translation-add]")) {
      event.preventDefault();
      postTranslationOpenForm(body);
      return true;
    }
    if (event.target.closest("[data-post-translation-cancel]")) {
      event.preventDefault();
      postTranslationClose(bar);
      return true;
    }
    const edit = event.target.closest("[data-post-translation-edit]");
    if (edit) {
      event.preventDefault();
      const lang = edit.getAttribute("data-post-translation-edit") || "";
      const mine = (bar.querySelector("[data-post-translation-panel]")?._entries || []).find((entry) => entry.mine && entry.lang === lang);
      postTranslationOpenForm(body, lang, mine?.body || "");
      return true;
    }
    const remove = event.target.closest("[data-post-translation-remove]");
    if (remove) {
      event.preventDefault();
      void postTranslationRemove(body, remove.getAttribute("data-post-translation-remove") || "", remove.getAttribute("data-post-translation-account") || "");
      return true;
    }
    return false;
  }

