(function (root) {
  "use strict";

  var DROP = [
    [0x0000, 0x0008], [0x000b, 0x000c], [0x000e, 0x001f], [0x007f, 0x009f],
    [0x00ad, 0x00ad], [0x034f, 0x034f], [0x115f, 0x1160], [0x17b4, 0x17b5],
    [0x180b, 0x180f], [0x200b, 0x200b], [0x202a, 0x202e], [0x2060, 0x206f],
    [0x3164, 0x3164], [0xd800, 0xdfff], [0xfdd0, 0xfdef], [0xfe00, 0xfe0d],
    [0xfeff, 0xfeff], [0xffa0, 0xffa0], [0xfff9, 0xfffb], [0x1bca0, 0x1bca3],
    [0x1d173, 0x1d17a], [0xe0000, 0xe007f], [0xe0100, 0xe01ef]
  ];
  var EMOJI = [
    [0x00a9, 0x00a9], [0x00ae, 0x00ae], [0x203c, 0x203c], [0x2049, 0x2049],
    [0x2122, 0x2122], [0x2139, 0x2139], [0x2190, 0x2bff], [0x3030, 0x3030],
    [0x303d, 0x303d], [0x3297, 0x3297], [0x3299, 0x3299], [0x1f000, 0x1faff]
  ];
  var JOINING = [
    [0x0600, 0x08ff], [0x0900, 0x0dff], [0x0f00, 0x109f], [0x1780, 0x18af],
    [0xfb50, 0xfdff], [0xfe70, 0xfefc]
  ];
  var RTL = [
    [0x0590, 0x08ff], [0xfb1d, 0xfdff], [0xfe70, 0xfefc], [0x10800, 0x10fff],
    [0x1e800, 0x1efff]
  ];
  var ZWNJ = 0x200c;
  var ZWJ = 0x200d;
  var BLACK_FLAG = 0x1f3f4;
  var CANCEL_TAG = 0xe007f;
  var QUICK = /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f-\u009f\u00ad\u034f\u061c\u115f\u1160\u17b4\u17b5\u180b-\u180f\u200b-\u200f\u2028-\u202e\u2060-\u206f\u3164\ud800-\udfff\ufdd0-\ufdef\ufe00-\ufe0f\ufeff\uffa0\ufff9-\ufffb\ufffe\uffff]/;

  function within(cp, ranges) {
    for (var i = 0; i < ranges.length; i++) {
      if (cp >= ranges[i][0] && cp <= ranges[i][1]) return true;
    }
    return false;
  }

  function isWhitespace(cp) {
    return cp === 0x09 || cp === 0x0a || cp === 0x0d || cp === 0x20;
  }

  function isSelector(cp) {
    return cp === 0xfe0e || cp === 0xfe0f;
  }

  function codePoints(text) {
    var out = [];
    for (var i = 0; i < text.length; i++) {
      var unit = text.charCodeAt(i);
      if (unit >= 0xd800 && unit <= 0xdbff && i + 1 < text.length) {
        var low = text.charCodeAt(i + 1);
        if (low >= 0xdc00 && low <= 0xdfff) {
          out.push(((unit - 0xd800) << 10) + (low - 0xdc00) + 0x10000);
          i++;
          continue;
        }
      }
      out.push(unit);
    }
    return out;
  }

  function flagTagLength(cps, start) {
    var index = start;
    while (index < cps.length && index - start < 8) {
      var cp = cps[index];
      if (cp === CANCEL_TAG) {
        var run = index - start;
        return run >= 2 && run <= 7 ? run + 1 : 0;
      }
      if (!((cp >= 0xe0030 && cp <= 0xe0039) || (cp >= 0xe0061 && cp <= 0xe007a))) return 0;
      index++;
    }
    return 0;
  }

  function keepMask(cps) {
    var keep = new Array(cps.length);
    var prev = -1;
    var index = 0;
    while (index < cps.length) {
      var cp = cps[index];
      var following = index + 1 < cps.length ? cps[index + 1] : -1;
      var at = index;
      index++;
      var kept = true;
      if (cp === 0x2028 || cp === 0x2029) {
        keep[at] = "\n";
        prev = 0x0a;
        continue;
      }
      if (isSelector(cp)) {
        kept = prev >= 0 && !isWhitespace(prev) && !isSelector(prev) && prev !== ZWJ && prev !== ZWNJ;
      } else if (cp === ZWJ) {
        kept = ((prev === 0xfe0f || within(prev, EMOJI)) && within(following, EMOJI)) ||
          (within(prev, JOINING) && within(following, JOINING));
      } else if (cp === ZWNJ) {
        kept = within(prev, JOINING) && within(following, JOINING);
      } else if (cp === 0x061c || cp === 0x200e || cp === 0x200f) {
        kept = within(prev, RTL) || within(following, RTL);
      } else if (cp === BLACK_FLAG) {
        var run = flagTagLength(cps, index);
        keep[at] = true;
        for (var t = 0; t < run; t++) keep[index + t] = true;
        if (run) prev = cps[index + run - 1];
        else prev = cp;
        index += run;
        continue;
      } else if (within(cp, DROP) || (cp & 0xfffe) === 0xfffe) {
        kept = false;
      }
      keep[at] = kept;
      if (kept) prev = cp;
    }
    return keep;
  }

  function clean(text) {
    if (typeof text !== "string" || !text || !hasCandidate(text)) return text;
    var cps = codePoints(text);
    var keep = keepMask(cps);
    var out = "";
    for (var i = 0; i < cps.length; i++) {
      if (keep[i] === true) out += String.fromCodePoint(cps[i]);
      else if (typeof keep[i] === "string") out += keep[i];
    }
    return out;
  }

  function hasCandidate(text) {
    return QUICK.test(text);
  }

  function hasHidden(text) {
    return typeof text === "string" && clean(text) !== text;
  }

  var api = { policy: "forkmesh-hidden-unicode-v1", clean: clean, hasHidden: hasHidden };
  if (typeof module === "object" && module && module.exports) module.exports = api;
  root.ForkMeshSafeInput = api;

  var doc = root.document;
  if (!doc || !doc.addEventListener) return;

  var SKIPPED_TYPES = {
    password: 1, file: 1, checkbox: 1, radio: 1, range: 1, color: 1, hidden: 1,
    submit: 1, button: 1, reset: 1, image: 1, date: 1, time: 1, "datetime-local": 1,
    month: 1, week: 1, number: 1
  };

  function editableTarget(event) {
    var path = typeof event.composedPath === "function" ? event.composedPath() : null;
    var target = path && path.length ? path[0] : event.target;
    if (!target || target.nodeType !== 1) {
      target = target && target.parentElement;
    }
    return target || null;
  }

  function cleanField(field) {
    var tag = field.tagName;
    if (tag === "TEXTAREA" || (tag === "INPUT" && !SKIPPED_TYPES[String(field.type || "text").toLowerCase()])) {
      var value = field.value;
      if (!hasCandidate(value)) return;
      var next = clean(value);
      if (next === value) return;
      var caret = null;
      try { caret = field.selectionEnd; } catch (_) { caret = null; }
      field.value = next;
      if (caret !== null && caret !== undefined) {
        var position = clean(value.slice(0, caret)).length;
        try { field.setSelectionRange(position, position); } catch (_) {}
      }
      return;
    }
    var host = field.isContentEditable ? field : null;
    while (host && host.parentElement && host.parentElement.isContentEditable) host = host.parentElement;
    if (host) cleanEditable(host);
  }

  function cleanEditable(host) {
    if (!hasCandidate(host.textContent || "")) return;
    var selection = root.getSelection ? root.getSelection() : null;
    var anchorNode = selection && selection.rangeCount ? selection.focusNode : null;
    var anchorOffset = selection && selection.rangeCount ? selection.focusOffset : 0;
    var walker = doc.createTreeWalker(host, 4, null);
    var node;
    while ((node = walker.nextNode())) {
      var data = node.data;
      if (!hasCandidate(data)) continue;
      var next = clean(data);
      if (next === data) continue;
      var restore = node === anchorNode;
      var offset = restore ? clean(data.slice(0, anchorOffset)).length : 0;
      node.data = next;
      if (restore && selection) {
        try { selection.collapse(node, Math.min(offset, next.length)); } catch (_) {}
      }
    }
  }

  function onInput(event) {
    if (event.isComposing) return;
    var target = editableTarget(event);
    if (target) cleanField(target);
  }

  doc.addEventListener("input", onInput, true);
  doc.addEventListener("compositionend", onInput, true);
  doc.addEventListener("change", onInput, true);
})(typeof window !== "undefined" ? window : globalThis);
