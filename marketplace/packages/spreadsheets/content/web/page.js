(() => {
const FMSheets = (() => {
  const MAX_ROWS = 1048576;
  const MAX_COLS = 16384;
  const DAY_MS = 86400000;
  const EPOCH = Date.UTC(1899, 11, 30);

  class SheetError {
    constructor(code) { this.code = code; }
    toString() { return this.code; }
  }
  const ERROR_CODES = ["#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#SPILL!", "#CALC!", "#ERROR!"];
  const ERRORS = Object.fromEntries(ERROR_CODES.map((code) => [code, new SheetError(code)]));
  const isErr = (value) => value instanceof SheetError;
  const err = (code) => ERRORS[code] || ERRORS["#VALUE!"];

  function colName(c) {
    let s = "";
    let n = c + 1;
    while (n > 0) { const m = (n - 1) % 26; s = String.fromCharCode(65 + m) + s; n = Math.floor((n - 1) / 26); }
    return s;
  }
  function colIndex(letters) {
    let n = 0;
    for (const ch of letters.toUpperCase()) n = n * 26 + ch.charCodeAt(0) - 64;
    return n - 1;
  }
  const cellKey = (r, c) => r * MAX_COLS + c;
  const keyRow = (k) => Math.floor(k / MAX_COLS);
  const keyCol = (k) => k % MAX_COLS;
  const addr = (r, c) => colName(c) + (r + 1);

  function parseAddr(text) {
    const m = /^\$?([A-Za-z]{1,3})\$?(\d{1,7})$/.exec(String(text || "").trim());
    if (!m) return null;
    const r = Number(m[2]) - 1, c = colIndex(m[1]);
    return r >= 0 && r < MAX_ROWS && c >= 0 && c < MAX_COLS ? { r, c } : null;
  }
  function parseRange(text) {
    const value = String(text || "").trim();
    const parts = value.split(":");
    if (parts.length === 1) {
      const a = parseAddr(parts[0]);
      return a ? { r1: a.r, c1: a.c, r2: a.r, c2: a.c } : null;
    }
    if (parts.length !== 2) return null;
    const a = parseAddr(parts[0]), b = parseAddr(parts[1]);
    if (a && b) return normRange({ r1: a.r, c1: a.c, r2: b.r, c2: b.c });
    const colA = /^\$?([A-Za-z]{1,3})$/.exec(parts[0]), colB = /^\$?([A-Za-z]{1,3})$/.exec(parts[1]);
    if (colA && colB) return normRange({ r1: 0, c1: colIndex(colA[1]), r2: MAX_ROWS - 1, c2: colIndex(colB[1]) });
    const rowA = /^\$?(\d{1,7})$/.exec(parts[0]), rowB = /^\$?(\d{1,7})$/.exec(parts[1]);
    if (rowA && rowB) return normRange({ r1: Number(rowA[1]) - 1, c1: 0, r2: Number(rowB[1]) - 1, c2: MAX_COLS - 1 });
    return null;
  }
  function normRange(range) {
    return {
      r1: Math.min(range.r1, range.r2), c1: Math.min(range.c1, range.c2),
      r2: Math.max(range.r1, range.r2), c2: Math.max(range.c1, range.c2),
    };
  }
  function rangeText(range) {
    const r = normRange(range);
    if (r.r1 === 0 && r.r2 === MAX_ROWS - 1) return `${colName(r.c1)}:${colName(r.c2)}`;
    if (r.c1 === 0 && r.c2 === MAX_COLS - 1) return `${r.r1 + 1}:${r.r2 + 1}`;
    if (r.r1 === r.r2 && r.c1 === r.c2) return addr(r.r1, r.c1);
    return `${addr(r.r1, r.c1)}:${addr(r.r2, r.c2)}`;
  }
  const inRange = (range, r, c) => r >= range.r1 && r <= range.r2 && c >= range.c1 && c <= range.c2;
  const rangesOverlap = (a, b) => a.r1 <= b.r2 && b.r1 <= a.r2 && a.c1 <= b.c2 && b.c1 <= a.c2;

  function serialFromDate(y, m, d) {
    let year = Math.trunc(y);
    if (year >= 0 && year < 1900) year += 1900;
    const ms = Date.UTC(year, 0, 1);
    const date = new Date(ms);
    date.setUTCFullYear(year, Math.trunc(m) - 1, Math.trunc(d));
    let serial = Math.round((date.getTime() - EPOCH) / DAY_MS);
    if (serial < 61) serial -= 1;
    return serial;
  }
  function dateFromSerial(serial) {
    let whole = Math.floor(serial);
    let fraction = serial - whole;
    let ms = Math.round(fraction * DAY_MS);
    if (ms >= DAY_MS) { whole += 1; ms -= DAY_MS; }
    const adjusted = whole < 61 ? whole + 1 : whole;
    const date = new Date(EPOCH + adjusted * DAY_MS);
    const parts = {
      y: date.getUTCFullYear(), m: date.getUTCMonth() + 1, d: date.getUTCDate(), wd: date.getUTCDay(),
      H: Math.floor(ms / 3600000), M: Math.floor(ms / 60000) % 60, S: Math.floor(ms / 1000) % 60, ms: ms % 1000,
    };
    if (whole === 60) { parts.y = 1900; parts.m = 2; parts.d = 29; parts.wd = 3; }
    return parts;
  }
  function serialFromJsDate(date) {
    const local = Date.UTC(date.getFullYear(), date.getMonth(), date.getDate(), date.getHours(), date.getMinutes(), date.getSeconds(), date.getMilliseconds());
    return (local - EPOCH) / DAY_MS;
  }

  const MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"];
  const DAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
  const NAMED_COLORS = { black: "#000000", blue: "#0000FF", cyan: "#00FFFF", green: "#00FF00", magenta: "#FF00FF", red: "#FF0000", white: "#FFFFFF", yellow: "#FFFF00" };
  const PALETTE = [
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
    0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080,
    0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF,
    0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF,
    0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99,
    0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699, 0x969696,
    0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333,
  ].map((rgb) => "#" + rgb.toString(16).padStart(6, "0").toUpperCase());

  const BUILTIN_FORMATS = {
    0: "General", 1: "0", 2: "0.00", 3: "#,##0", 4: "#,##0.00",
    5: '"$"#,##0_);("$"#,##0)', 6: '"$"#,##0_);[Red]("$"#,##0)', 7: '"$"#,##0.00_);("$"#,##0.00)', 8: '"$"#,##0.00_);[Red]("$"#,##0.00)',
    9: "0%", 10: "0.00%", 11: "0.00E+00", 12: "# ?/?", 13: "# ??/??", 14: "m/d/yyyy", 15: "d-mmm-yy", 16: "d-mmm", 17: "mmm-yy",
    18: "h:mm AM/PM", 19: "h:mm:ss AM/PM", 20: "h:mm", 21: "h:mm:ss", 22: "m/d/yyyy h:mm",
    37: "#,##0_);(#,##0)", 38: "#,##0_);[Red](#,##0)", 39: "#,##0.00_);(#,##0.00)", 40: "#,##0.00_);[Red](#,##0.00)",
    41: '_(* #,##0_);_(* (#,##0);_(* "-"_);_(@_)', 42: '_("$"* #,##0_);_("$"* (#,##0);_("$"* "-"_);_(@_)',
    43: '_(* #,##0.00_);_(* (#,##0.00);_(* "-"??_);_(@_)', 44: '_("$"* #,##0.00_);_("$"* (#,##0.00);_("$"* "-"??_);_(@_)',
    45: "mm:ss", 46: "[h]:mm:ss", 47: "mm:ss.0", 48: "##0.0E+0", 49: "@",
  };

  function roundTo(value, digits) {
    if (!Number.isFinite(value)) return value;
    const sign = value < 0 ? -1 : 1;
    const shifted = Number(Math.abs(value) + "e" + digits);
    if (!Number.isFinite(shifted)) return value;
    return sign * Number(Math.round(shifted) + "e-" + digits);
  }
  function formatGeneral(n) {
    if (!Number.isFinite(n)) return "#NUM!";
    if (n === 0) return "0";
    const abs = Math.abs(n);
    if (abs >= 1e11 || abs < 1e-9) {
      const [mantissa, exponent] = n.toExponential(5).split("e");
      const m = mantissa.replace(/\.?0+$/, "");
      const e = Number(exponent);
      return `${m}E${e < 0 ? "-" : "+"}${String(Math.abs(e)).padStart(2, "0")}`;
    }
    const digits = Math.max(0, 10 - Math.floor(Math.log10(abs)));
    let text = roundTo(n, Math.min(digits, 15)).toString();
    if (text.includes("e")) text = roundTo(n, Math.min(digits, 15)).toFixed(Math.min(20, digits)).replace(/\.?0+$/, "");
    return text;
  }
  function valueToText(value) {
    if (value === null || value === undefined) return "";
    if (typeof value === "number") {
      if (!Number.isFinite(value)) return "#NUM!";
      const text = String(Number(value.toPrecision(15)));
      if (!text.includes("e")) return text;
      return formatGeneral(value);
    }
    if (typeof value === "boolean") return value ? "TRUE" : "FALSE";
    if (isErr(value)) return value.code;
    return String(value);
  }

  const formatCache = new Map();
  function tokenizeSection(source) {
    const tokens = [];
    let color = "", cond = null, i = 0;
    while (i < source.length) {
      const ch = source[i];
      if (ch === '"') {
        const end = source.indexOf('"', i + 1);
        const text = end < 0 ? source.slice(i + 1) : source.slice(i + 1, end);
        tokens.push({ t: "lit", v: text });
        i = end < 0 ? source.length : end + 1;
      } else if (ch === "\\") { tokens.push({ t: "lit", v: source[i + 1] || "" }); i += 2; }
      else if (ch === "_") { tokens.push({ t: "lit", v: " " }); i += 2; }
      else if (ch === "*") { i += 2; }
      else if (ch === "[") {
        const end = source.indexOf("]", i);
        const inner = end < 0 ? source.slice(i + 1) : source.slice(i + 1, end);
        i = end < 0 ? source.length : end + 1;
        const lower = inner.toLowerCase();
        if (NAMED_COLORS[lower]) color = NAMED_COLORS[lower];
        else if (/^color\s*\d+$/.test(lower)) color = PALETTE[Math.min(63, Number(lower.replace(/\D/g, "")) + 7)] || "";
        else if (/^(<=|>=|<>|<|>|=)\s*-?[\d.]+$/.test(inner)) { const m = /^(<=|>=|<>|<|>|=)\s*(-?[\d.]+)$/.exec(inner); cond = { op: m[1], v: Number(m[2]) }; }
        else if (/^(h+|m+|s+)$/i.test(inner)) tokens.push({ t: "elapsed", v: lower[0], n: inner.length });
        else if (inner.startsWith("$")) { const sym = inner.slice(1).split("-")[0]; if (sym) tokens.push({ t: "lit", v: sym }); }
      } else if (/^general/i.test(source.slice(i))) { tokens.push({ t: "general" }); i += 7; }
      else if (/^am\/pm/i.test(source.slice(i))) { tokens.push({ t: "ampm", v: "AM/PM" }); i += 5; }
      else if (/^a\/p/i.test(source.slice(i))) { tokens.push({ t: "ampm", v: "A/P" }); i += 3; }
      else if (/[ymdhse]/i.test(ch) && !(ch.toLowerCase() === "e" && /[+-]/.test(source[i + 1] || ""))) {
        let j = i;
        while (j < source.length && source[j].toLowerCase() === ch.toLowerCase()) j++;
        tokens.push({ t: "date", v: ch.toLowerCase(), n: j - i });
        i = j;
      } else if ((ch === "E" || ch === "e") && /[+-]/.test(source[i + 1] || "")) {
        let j = i + 2;
        let digits = 0;
        while (j < source.length && /[0#?]/.test(source[j])) { digits++; j++; }
        tokens.push({ t: "exp", sign: source[i + 1], n: Math.max(1, digits) });
        i = j;
      } else if (ch === "0" || ch === "#" || ch === "?") { tokens.push({ t: "digit", v: ch }); i++; }
      else if (ch === ".") { tokens.push({ t: "point" }); i++; }
      else if (ch === ",") { tokens.push({ t: "comma" }); i++; }
      else if (ch === "%") { tokens.push({ t: "percent" }); i++; }
      else if (ch === "@") { tokens.push({ t: "text" }); i++; }
      else if (ch === "/") { tokens.push({ t: "slash" }); i++; }
      else { tokens.push({ t: "lit", v: ch }); i++; }
    }
    const hasDate = tokens.some((tok) => tok.t === "date" || tok.t === "elapsed" || tok.t === "ampm");
    const hasDigit = tokens.some((tok) => tok.t === "digit");
    const kind = tokens.some((tok) => tok.t === "general") ? "general"
      : hasDate ? "date" : hasDigit ? "number" : tokens.some((tok) => tok.t === "text") ? "text" : "literal";
    if (kind === "date") {
      const dated = tokens.filter((tok) => tok.t === "date" || tok.t === "elapsed");
      dated.forEach((tok, index) => {
        if (tok.t !== "date" || tok.v !== "m" || tok.n > 2) return;
        const prev = dated[index - 1], next = dated[index + 1];
        if ((prev && (prev.v === "h")) || (next && next.v === "s")) tok.minute = true;
      });
      tokens.forEach((tok, index) => {
        if (tok.t === "point" && tokens[index - 1]?.t === "date" && tokens[index - 1].v === "s") {
          let n = 0;
          for (let j = index + 1; j < tokens.length && tokens[j].t === "digit" && tokens[j].v === "0"; j++) n++;
          if (n) { tok.t = "subsec"; tok.n = n; tokens.splice(index + 1, n); }
        }
      });
    }
    return { tokens, color, cond, kind, hasAmPm: tokens.some((tok) => tok.t === "ampm") };
  }
  function splitSections(code) {
    const sections = [];
    let current = "", quoted = false, bracket = false;
    for (let i = 0; i < code.length; i++) {
      const ch = code[i];
      if (ch === '"' && !bracket) quoted = !quoted;
      else if (ch === "[" && !quoted) bracket = true;
      else if (ch === "]" && !quoted) bracket = false;
      if (ch === "\\" && !quoted) { current += ch + (code[i + 1] || ""); i++; continue; }
      if (ch === ";" && !quoted && !bracket) { sections.push(current); current = ""; continue; }
      current += ch;
    }
    sections.push(current);
    return sections;
  }
  function compileFormat(code) {
    const source = code || "General";
    let compiled = formatCache.get(source);
    if (!compiled) {
      compiled = splitSections(source).slice(0, 4).map(tokenizeSection);
      formatCache.set(source, compiled);
      if (formatCache.size > 2000) formatCache.delete(formatCache.keys().next().value);
    }
    return compiled;
  }
  function isDateFormat(code) {
    if (!code) return false;
    return compileFormat(code)[0].kind === "date";
  }
  function bestFraction(value, maxDen) {
    let best = [0, 1], bestErr = Infinity;
    for (let den = 1; den <= maxDen; den++) {
      const num = Math.round(value * den);
      const diff = Math.abs(value - num / den);
      if (diff < bestErr - 1e-12) { best = [num, den]; bestErr = diff; if (diff === 0) break; }
    }
    return best;
  }
  function fillInteger(placeholders, digits) {
    let out = "";
    const n = placeholders.length;
    if (!n) return digits;
    const padded = digits.length >= n ? digits : " ".repeat(n - digits.length) + digits;
    const lead = padded.length - n;
    for (let i = 0; i < n; i++) {
      const piece = i === 0 ? padded.slice(0, lead + 1) : padded[lead + i];
      out += piece.replace(/ /g, () => placeholders[i] === "0" ? "0" : placeholders[i] === "?" ? " " : "");
    }
    return out;
  }
  function renderNumber(section, value, signed) {
    const tokens = section.tokens;
    const pointIndex = tokens.findIndex((tok) => tok.t === "point");
    const expIndex = tokens.findIndex((tok) => tok.t === "exp");
    const slashIndex = tokens.findIndex((tok) => tok.t === "slash");
    let n = Math.abs(value);
    const percents = tokens.filter((tok) => tok.t === "percent").length;
    n *= Math.pow(100, percents);
    const intEnd = pointIndex >= 0 ? pointIndex : expIndex >= 0 ? expIndex : tokens.length;
    let lastDigit = -1;
    for (let i = 0; i < intEnd; i++) if (tokens[i].t === "digit") lastDigit = i;
    let scale = 0;
    for (let i = lastDigit + 1; i < intEnd && tokens[i].t === "comma"; i++) scale++;
    if (pointIndex < 0 && lastDigit >= 0) {
      for (let i = lastDigit + 1; i < tokens.length && tokens[i].t === "comma"; i++) if (i >= intEnd) scale++;
    }
    n /= Math.pow(1000, scale);
    const firstDigit = tokens.findIndex((tok) => tok.t === "digit");
    const thousands = tokens.some((tok, i) => tok.t === "comma" && i > firstDigit && i < lastDigit);
    let negative = signed && value < 0;
    if (slashIndex >= 0) {
      const before = tokens.slice(0, slashIndex);
      const after = tokens.slice(slashIndex + 1);
      let numStart = before.length;
      while (numStart > 0 && before[numStart - 1].t === "digit") numStart--;
      const numPh = before.slice(numStart).map((tok) => tok.v);
      const wholeTokens = before.slice(0, numStart);
      const hasWhole = wholeTokens.some((tok) => tok.t === "digit");
      const denDigits = after.filter((tok) => tok.t === "digit");
      const fixed = after.map((tok) => tok.t === "lit" && /\d/.test(tok.v) ? tok.v : "").join("");
      let whole = hasWhole ? Math.floor(n) : 0;
      let fracValue = hasWhole ? n - whole : n;
      let num, den;
      if (fixed && !denDigits.length) { den = Number(fixed); num = Math.round(fracValue * den); }
      else [num, den] = bestFraction(fracValue, Math.pow(10, Math.max(1, denDigits.length)) - 1);
      if (hasWhole && num === den) { whole += 1; num = 0; }
      let out = negative ? "-" : "";
      for (const tok of wholeTokens) {
        if (tok.t === "digit") { if (tok === wholeTokens.find((t) => t.t === "digit")) out += whole || num === 0 ? String(whole) : ""; }
        else if (tok.t === "lit") out += tok.v;
      }
      if (hasWhole && num === 0) return out.trimEnd() || "0";
      out += fillInteger(numPh, String(num)) + "/";
      out += fixed && !denDigits.length ? fixed : String(den).padEnd(denDigits.length, " ");
      return out;
    }
    let exponent = 0;
    if (expIndex >= 0) {
      const intPh = tokens.slice(0, intEnd).filter((tok) => tok.t === "digit").length || 1;
      if (n !== 0) {
        exponent = Math.floor(Math.log10(n));
        if (intPh > 1 && tokens.slice(0, intEnd).some((tok) => tok.t === "digit" && tok.v === "#")) exponent = Math.floor(exponent / intPh) * intPh;
        else exponent -= intPh - 1;
        n /= Math.pow(10, exponent);
      }
    }
    const fracTokens = pointIndex >= 0 ? tokens.slice(pointIndex + 1, expIndex >= 0 ? expIndex : tokens.length) : [];
    const fracPh = fracTokens.filter((tok) => tok.t === "digit").map((tok) => tok.v);
    let rounded = roundTo(n, fracPh.length);
    if (expIndex >= 0 && rounded >= Math.pow(10, Math.max(1, tokens.slice(0, intEnd).filter((tok) => tok.t === "digit").length))) {
      rounded /= 10; exponent += 1;
    }
    if (negative && rounded === 0 && !fracPh.length) negative = false;
    if (negative && rounded === 0 && fracPh.length && Number(rounded.toFixed(fracPh.length)) === 0) negative = false;
    const fixedText = rounded.toFixed(fracPh.length);
    let [intDigits, fracDigits = ""] = fixedText.split(".");
    if (intDigits === "0") intDigits = "";
    const intTokens = tokens.slice(0, intEnd);
    const intPh = intTokens.filter((tok) => tok.t === "digit").map((tok) => tok.v);
    const literalsInside = intTokens.some((tok, i) => tok.t === "lit" && i > firstDigit && i < lastDigit);
    let intText;
    if (thousands && !literalsInside) {
      const filled = fillInteger(intPh, intDigits);
      const digitsOnly = filled.replace(/\D/g, "");
      const grouped = digitsOnly.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
      intText = filled.replace(/\d+/, grouped);
    } else intText = fillInteger(intPh, intDigits);
    let out = "";
    let intWritten = false;
    for (let i = 0; i < intEnd; i++) {
      const tok = tokens[i];
      if (tok.t === "digit") {
        if (!intWritten) {
          if (literalsInside) {
            let pos = 0;
            const lead = Math.max(0, intDigits.length - intPh.length);
            const padded = intDigits.length >= intPh.length ? intDigits : " ".repeat(intPh.length - intDigits.length) + intDigits;
            for (let j = i; j <= lastDigit; j++) {
              const t = tokens[j];
              if (t.t === "digit") {
                const piece = pos === 0 ? padded.slice(0, lead + 1) : padded[lead + pos];
                out += piece.replace(/ /g, () => t.v === "0" ? "0" : t.v === "?" ? " " : "");
                pos++;
              } else if (t.t === "lit") out += t.v;
            }
            i = lastDigit;
          } else out += intText;
          intWritten = true;
        }
      } else if (tok.t === "lit") out += tok.v;
      else if (tok.t === "percent") out += "%";
      else if (tok.t === "text") out += "";
    }
    if (pointIndex >= 0) {
      let frac = "";
      let digitIndex = 0;
      const chars = fracDigits.split("");
      let keep = chars.length;
      while (keep > 0 && chars[keep - 1] === "0" && fracPh[keep - 1] !== "0") keep--;
      for (const tok of fracTokens) {
        if (tok.t === "digit") {
          const d = chars[digitIndex];
          if (digitIndex < keep) frac += d;
          else frac += tok.v === "0" ? "0" : tok.v === "?" ? " " : "";
          digitIndex++;
        } else if (tok.t === "lit") frac += tok.v;
        else if (tok.t === "percent") frac += "%";
      }
      out += "." + frac;
    }
    if (expIndex >= 0) {
      const expTok = tokens[expIndex];
      const sign = exponent < 0 ? "-" : expTok.sign === "+" ? "+" : "";
      out += "E" + sign + String(Math.abs(exponent)).padStart(expTok.n, "0");
      for (let i = expIndex + 1; i < tokens.length; i++) {
        if (tokens[i].t === "lit") out += tokens[i].v;
        else if (tokens[i].t === "percent") out += "%";
      }
    }
    return (negative ? "-" : "") + out;
  }
  function renderDate(section, value) {
    if (value < 0 || value > 2958465.99999) return "#".repeat(8);
    const hasSub = section.tokens.find((tok) => tok.t === "subsec");
    const precision = hasSub ? Math.pow(10, hasSub.n) : 1;
    const roundedSerial = Math.round(value * 86400 * precision) / (86400 * precision);
    const p = dateFromSerial(roundedSerial);
    let out = "";
    for (const tok of section.tokens) {
      if (tok.t === "lit") out += tok.v;
      else if (tok.t === "date") {
        const n = tok.n;
        if (tok.v === "y" || tok.v === "e") out += n <= 2 && tok.v === "y" ? String(p.y % 100).padStart(2, "0") : String(p.y);
        else if (tok.v === "m" && tok.minute) out += n >= 2 ? String(p.M).padStart(2, "0") : String(p.M);
        else if (tok.v === "m") out += n === 1 ? String(p.m) : n === 2 ? String(p.m).padStart(2, "0") : n === 3 ? MONTHS[p.m - 1].slice(0, 3) : n === 5 ? MONTHS[p.m - 1][0] : MONTHS[p.m - 1];
        else if (tok.v === "d") out += n === 1 ? String(p.d) : n === 2 ? String(p.d).padStart(2, "0") : n === 3 ? DAYS[p.wd].slice(0, 3) : DAYS[p.wd];
        else if (tok.v === "h") {
          let h = p.H;
          if (section.hasAmPm) { h %= 12; if (h === 0) h = 12; }
          out += n >= 2 ? String(h).padStart(2, "0") : String(h);
        } else if (tok.v === "s") out += n >= 2 ? String(p.S).padStart(2, "0") : String(p.S);
      } else if (tok.t === "elapsed") {
        const total = tok.v === "h" ? Math.floor(roundedSerial * 24 + 1e-9) : tok.v === "m" ? Math.floor(roundedSerial * 1440 + 1e-9) : Math.floor(roundedSerial * 86400 + 1e-6);
        out += String(total).padStart(tok.n, "0");
      } else if (tok.t === "ampm") {
        const pm = p.H >= 12;
        out += tok.v === "A/P" ? (pm ? "P" : "A") : (pm ? "PM" : "AM");
      } else if (tok.t === "subsec") {
        out += "." + String(Math.round(p.ms / Math.pow(10, 3 - tok.n))).padStart(tok.n, "0").slice(0, tok.n);
      } else if (tok.t === "digit") out += tok.v === "0" ? "0" : "";
      else if (tok.t === "point") out += ".";
      else if (tok.t === "slash") out += "/";
      else if (tok.t === "comma") out += ",";
      else if (tok.t === "percent") out += "%";
    }
    return out;
  }
  function renderText(section, text) {
    let out = "";
    for (const tok of section.tokens) {
      if (tok.t === "text") out += text;
      else if (tok.t === "lit") out += tok.v;
    }
    return out;
  }
  function condMatch(cond, value) {
    switch (cond.op) {
      case "<": return value < cond.v;
      case "<=": return value <= cond.v;
      case ">": return value > cond.v;
      case ">=": return value >= cond.v;
      case "=": return value === cond.v;
      case "<>": return value !== cond.v;
      default: return false;
    }
  }
  function formatValue(value, code) {
    if (value === null || value === undefined) return { text: "", color: "" };
    if (isErr(value)) return { text: value.code, color: "" };
    if (typeof value === "boolean") return { text: value ? "TRUE" : "FALSE", color: "" };
    const sections = compileFormat(code);
    if (typeof value === "string") {
      const textSection = sections[3] || sections.find((section) => section.kind === "text");
      if (textSection) return { text: renderText(textSection, value), color: textSection.color };
      return { text: value, color: "" };
    }
    if (!Number.isFinite(value)) return { text: "#NUM!", color: "" };
    let section = sections[0], signed = true;
    if (sections.some((s) => s.cond)) {
      const numeric = sections.filter((s) => s.kind !== "text");
      section = numeric.find((s) => s.cond && condMatch(s.cond, value)) || numeric.find((s) => !s.cond) || numeric[numeric.length - 1];
      signed = !(section.cond && (section.cond.op === "<" || section.cond.op === "<=") && section.cond.v <= 0);
    } else if (sections.length >= 2 && sections[1].kind !== "text") {
      if (value < 0) { section = sections[1]; signed = false; }
      else if (value === 0 && sections.length >= 3 && sections[2].kind !== "text") section = sections[2];
    }
    if (section.kind === "general") {
      const text = formatGeneral(signed ? value : Math.abs(value));
      return { text: section.tokens.map((tok) => tok.t === "general" ? text : tok.t === "lit" ? tok.v : "").join(""), color: section.color };
    }
    if (section.kind === "date") return { text: renderDate(section, value), color: section.color };
    if (section.kind === "number") return { text: renderNumber(section, value, signed), color: section.color };
    if (section.kind === "text") return { text: formatGeneral(value), color: section.color };
    return { text: section.tokens.map((tok) => tok.t === "lit" ? tok.v : "").join(""), color: section.color };
  }

  const MONTH_LOOKUP = Object.fromEntries(MONTHS.flatMap((name, i) => [[name.toLowerCase(), i + 1], [name.slice(0, 3).toLowerCase(), i + 1]]));
  MONTH_LOOKUP.sept = 9;
  function parseTimeParts(text) {
    const m = /^(\d{1,2}):(\d{2})(?::(\d{2}(?:\.\d+)?))?\s*([ap]\.?m?\.?)?$/i.exec(text);
    if (!m) return null;
    let h = Number(m[1]);
    const min = Number(m[2]), sec = Number(m[3] || 0);
    if (m[4]) {
      if (h < 1 || h > 12) return null;
      const pm = m[4].toLowerCase().startsWith("p");
      h = (h % 12) + (pm ? 12 : 0);
    }
    if (h > 23 && !m[4]) { if (min > 59 || sec >= 60) return null; return { serial: (h * 3600 + min * 60 + sec) / 86400, format: "[h]:mm:ss" }; }
    if (min > 59 || sec >= 60) return null;
    const format = m[4] ? (m[3] ? "h:mm:ss AM/PM" : "h:mm AM/PM") : (m[3] ? "h:mm:ss" : "h:mm");
    return { serial: (h * 3600 + min * 60 + sec) / 86400, format };
  }
  function validDate(y, m, d) {
    if (m < 1 || m > 12 || d < 1 || y < 1900 || y > 9999) return false;
    return d <= new Date(Date.UTC(y, m, 0)).getUTCDate();
  }
  function parseDateText(text, dayFirst) {
    let m = /^(\d{4})[-/.](\d{1,2})[-/.](\d{1,2})(?:[ T](.+))?$/.exec(text);
    let y, mo, d, time = null, format;
    if (m) { y = +m[1]; mo = +m[2]; d = +m[3]; format = "yyyy-mm-dd"; if (m[4]) { time = parseTimeParts(m[4].trim()); if (!time) return null; } }
    else if ((m = /^(\d{1,2})[/.-](\d{1,2})[/.-](\d{2,4})(?:\s+(.+))?$/.exec(text))) {
      const a = +m[1], b = +m[2];
      y = +m[3]; if (m[3].length === 2) y += y < 30 ? 2000 : 1900;
      if (dayFirst) { d = a; mo = b; format = "dd/mm/yyyy"; } else { mo = a; d = b; format = "m/d/yyyy"; }
      if (m[4]) { time = parseTimeParts(m[4].trim()); if (!time) return null; }
    } else if ((m = /^(\d{1,2})[/.-](\d{1,2})$/.exec(text))) {
      const a = +m[1], b = +m[2];
      y = new Date().getFullYear();
      if (dayFirst) { d = a; mo = b; format = "d-mmm"; } else { mo = a; d = b; format = "d-mmm"; }
    } else if ((m = /^(\d{1,2})[\s-]+([A-Za-z]{3,9})\.?,?[\s-]*(\d{2,4})?$/.exec(text))) {
      mo = MONTH_LOOKUP[m[2].toLowerCase()]; d = +m[1];
      y = m[3] ? +m[3] : new Date().getFullYear(); if (m[3] && m[3].length === 2) y += y < 30 ? 2000 : 1900;
      format = m[3] ? "d-mmm-yyyy" : "d-mmm";
    } else if ((m = /^([A-Za-z]{3,9})\.?\s+(\d{1,2})(?:st|nd|rd|th)?,?\s*(\d{4})?$/.exec(text))) {
      mo = MONTH_LOOKUP[m[1].toLowerCase()]; d = +m[2];
      y = m[3] ? +m[3] : new Date().getFullYear();
      format = m[3] ? "mmm d, yyyy" : "mmm d";
    } else if ((m = /^([A-Za-z]{3,9})\.?[\s-]+(\d{4})$/.exec(text))) {
      mo = MONTH_LOOKUP[m[1].toLowerCase()]; d = 1; y = +m[2]; format = "mmm-yy";
    } else return null;
    if (!mo || !validDate(y, mo, d)) return null;
    let serial = serialFromDate(y, mo, d);
    if (time) { serial += time.serial; format += time.format.includes("AM") ? " h:mm AM/PM" : " h:mm"; }
    return { serial, format };
  }
  function parseNumberText(text) {
    let s = text.trim();
    if (!s) return null;
    let negative = false, percent = false, currency = "", format = "";
    if (/^\(.*\)$/.test(s)) { negative = true; s = s.slice(1, -1).trim(); }
    if (/^[+-]/.test(s)) { if (s[0] === "-") negative = !negative; s = s.slice(1).trim(); }
    const cur = /^([$€£¥₹₩]|R\$|CHF|kr)\s*/.exec(s) || /\s*([$€£¥₹₩]|CHF|kr)$/.exec(s);
    if (cur) { currency = cur[1]; s = s.replace(cur[0], "").trim(); }
    if (/^[+-]/.test(s)) { if (s[0] === "-") negative = !negative; s = s.slice(1).trim(); }
    if (s.endsWith("%")) { percent = true; s = s.slice(0, -1).trim(); }
    let grouped = false;
    if (/^\d{1,3}(,\d{3})+(\.\d*)?$/.test(s)) { grouped = true; s = s.replace(/,/g, ""); }
    if (!/^(\d+\.?\d*|\.\d+)(e[+-]?\d+)?$/i.test(s)) return null;
    let value = Number(s);
    if (!Number.isFinite(value)) return null;
    const decimals = (s.split(".")[1] || "").replace(/e.*/i, "").length;
    if (negative) value = -value;
    if (percent) { value /= 100; format = decimals ? "0.00%" : "0%"; }
    else if (currency) {
      const symbol = currency === "$" ? '"$"' : `"${currency}"`;
      format = decimals ? `${symbol}#,##0.00` : `${symbol}#,##0`;
      if (cur && cur.index > 0) format = decimals ? `#,##0.00 ${symbol}` : `#,##0 ${symbol}`;
    } else if (/e/i.test(s)) format = "0.00E+00";
    else if (grouped) format = decimals ? "#,##0.00" : "#,##0";
    return { value, format };
  }
  function parseInput(raw, options = {}) {
    const text = String(raw ?? "");
    if (text === "") return { value: null };
    if (text[0] === "'") return { value: text.slice(1), text: true };
    if (text[0] === "=" && text.length > 1) return { formula: text.slice(1) };
    if (options.textFormat) return { value: text };
    const trimmed = text.trim();
    if (/^(true|false)$/i.test(trimmed)) return { value: trimmed.toUpperCase() === "TRUE" };
    if (ERRORS[trimmed.toUpperCase()]) return { value: ERRORS[trimmed.toUpperCase()] };
    const number = parseNumberText(trimmed);
    if (number) return number;
    const time = parseTimeParts(trimmed);
    if (time) return { value: time.serial, format: time.format };
    const date = parseDateText(trimmed, options.dayFirst);
    if (date) return { value: date.serial, format: date.format };
    return { value: text };
  }
  function textToNumber(text) {
    const parsed = parseInput(String(text).trim());
    if (parsed.formula !== undefined || typeof parsed.value !== "number") return null;
    return parsed.value;
  }
  class FormulaSyntax extends Error {}
  const MISSING = Symbol("missing");
  const UNKNOWN_FUNCTION = Symbol("unknown-function");
  const SHEET_NAME_RE = /^[A-Za-z_À-￿][A-Za-z0-9_.À-￿]*$/;
  function quoteSheet(name) {
    const plain = SHEET_NAME_RE.test(name) && !/^[A-Za-z]{1,3}\d+$/.test(name) && !/^(TRUE|FALSE)$/i.test(name) && !/^R\d*C\d*$/i.test(name);
    return plain ? name : `'${String(name).replace(/'/g, "''")}'`;
  }
  const validCell = (letters, digits) => colIndex(letters) < MAX_COLS && Number(digits) >= 1 && Number(digits) <= MAX_ROWS;
  const FUNCTION_PREFIX_RE = /^(_xlfn\.)?(_xlws\.)?/i;

  function tokenize(src) {
    const tokens = [];
    let i = 0;
    const n = src.length;
    while (i < n) {
      const ch = src[i];
      if (ch === " " || ch === "\n" || ch === "\r" || ch === "\t" || ch === " ") { i++; continue; }
      const rest = src.slice(i);
      let m;
      if (ch === '"') {
        let j = i + 1, s = "";
        while (j < n) {
          if (src[j] === '"') { if (src[j + 1] === '"') { s += '"'; j += 2; continue; } break; }
          s += src[j++];
        }
        if (j >= n) throw new FormulaSyntax("A text value is missing its closing quote.");
        tokens.push({ t: "str", v: s }); i = j + 1; continue;
      }
      if (ch === "#") {
        m = /^#(NULL!|DIV\/0!|VALUE!|REF!|NAME\?|NUM!|N\/A|SPILL!|CALC!|ERROR!)/i.exec(rest);
        if (!m) throw new FormulaSyntax("Unknown error value.");
        tokens.push({ t: "err", v: m[0].toUpperCase() }); i += m[0].length; continue;
      }
      let sheet = null, sheetLen = 0;
      if (ch === "'") {
        m = /^'((?:[^']|'')+)'!/.exec(rest);
        if (!m) throw new FormulaSyntax("A sheet name is missing its closing quote.");
        sheet = m[1].replace(/''/g, "'"); sheetLen = m[0].length;
      } else if ((m = /^([A-Za-z_À-￿][A-Za-z0-9_.À-￿]*)!/.exec(rest))) { sheet = m[1]; sheetLen = m[0].length; }
      const body = sheetLen ? rest.slice(sheetLen) : rest;
      m = /^(\$?)([A-Za-z]{1,3})(\$?)(\d{1,7})(?::(\$?)([A-Za-z]{1,3})(\$?)(\d{1,7}))?(?![A-Za-z0-9_.(!])/.exec(body);
      if (m && validCell(m[2], m[4]) && (!m[6] || validCell(m[6], m[8]))) {
        if (m[6]) {
          tokens.push({ t: "node", node: { t: "rng", kind: "cells", sheet, r1: Number(m[4]) - 1, c1: colIndex(m[2]), r2: Number(m[8]) - 1, c2: colIndex(m[6]), ac1: !!m[1], ar1: !!m[3], ac2: !!m[5], ar2: !!m[7] } });
        } else {
          tokens.push({ t: "node", node: { t: "ref", sheet, r: Number(m[4]) - 1, c: colIndex(m[2]), ac: !!m[1], ar: !!m[3] } });
        }
        i += sheetLen + m[0].length; continue;
      }
      m = /^(\$?)([A-Za-z]{1,3}):(\$?)([A-Za-z]{1,3})(?![A-Za-z0-9_.(])/.exec(body);
      if (m && colIndex(m[2]) < MAX_COLS && colIndex(m[4]) < MAX_COLS) {
        tokens.push({ t: "node", node: { t: "rng", kind: "cols", sheet, r1: 0, c1: colIndex(m[2]), r2: MAX_ROWS - 1, c2: colIndex(m[4]), ac1: !!m[1], ar1: true, ac2: !!m[3], ar2: true } });
        i += sheetLen + m[0].length; continue;
      }
      m = /^(\$?)(\d{1,7}):(\$?)(\d{1,7})(?![\d.A-Za-z])/.exec(body);
      if (m && (sheetLen || !tokens.length || tokens[tokens.length - 1].t === "op") && Number(m[2]) >= 1 && Number(m[4]) >= 1) {
        tokens.push({ t: "node", node: { t: "rng", kind: "rows", sheet, r1: Number(m[2]) - 1, c1: 0, r2: Number(m[4]) - 1, c2: MAX_COLS - 1, ac1: true, ar1: !!m[1], ac2: true, ar2: !!m[3] } });
        i += sheetLen + m[0].length; continue;
      }
      if (sheetLen) {
        m = /^([A-Za-z_\\][A-Za-z0-9_.]*)/.exec(body);
        if (m) { tokens.push({ t: "name", v: m[1], sheet }); i += sheetLen + m[0].length; continue; }
        throw new FormulaSyntax("A sheet reference must point at a cell or range.");
      }
      m = /^(\d+\.?\d*|\.\d+)(e[+-]?\d+)?/i.exec(rest);
      if (m) { tokens.push({ t: "num", v: Number(m[0]) }); i += m[0].length; continue; }
      m = /^[A-Za-z_\\À-￿][A-Za-z0-9_.À-￿]*/.exec(rest);
      if (m) {
        let j = i + m[0].length;
        while (j < n && src[j] === " ") j++;
        if (src[j] === "(") tokens.push({ t: "fn", v: m[0].replace(FUNCTION_PREFIX_RE, "").toUpperCase() });
        else if (/^(TRUE|FALSE)$/i.test(m[0])) tokens.push({ t: "bool", v: m[0].toUpperCase() === "TRUE" });
        else tokens.push({ t: "name", v: m[0] });
        i += m[0].length; continue;
      }
      const two = src.slice(i, i + 2);
      if (two === "<=" || two === ">=" || two === "<>") { tokens.push({ t: "op", v: two }); i += 2; continue; }
      if ("+-*/^&=<>%(),;{}:".includes(ch)) { tokens.push({ t: "op", v: ch }); i++; continue; }
      throw new FormulaSyntax(`Unexpected character "${ch}".`);
    }
    return tokens;
  }

  function parseFormula(src) {
    const tokens = tokenize(src);
    let pos = 0;
    const peek = () => tokens[pos];
    const isOp = (v) => tokens[pos] && tokens[pos].t === "op" && tokens[pos].v === v;
    const expect = (v) => { if (!isOp(v)) throw new FormulaSyntax(`Expected "${v}".`); pos++; };
    const comparison = () => {
      let a = concat();
      while (peek() && peek().t === "op" && ["=", "<>", "<", ">", "<=", ">="].includes(peek().v)) {
        const op = tokens[pos++].v; a = { t: "op", op, a, b: concat() };
      }
      return a;
    };
    const concat = () => { let a = additive(); while (isOp("&")) { pos++; a = { t: "op", op: "&", a, b: additive() }; } return a; };
    const additive = () => { let a = term(); while (isOp("+") || isOp("-")) { const op = tokens[pos++].v; a = { t: "op", op, a, b: term() }; } return a; };
    const term = () => { let a = power(); while (isOp("*") || isOp("/")) { const op = tokens[pos++].v; a = { t: "op", op, a, b: power() }; } return a; };
    const power = () => { let a = unary(); while (isOp("^")) { pos++; a = { t: "op", op: "^", a, b: unary() }; } return a; };
    const unary = () => {
      if (isOp("-")) { pos++; return { t: "neg", a: unary() }; }
      if (isOp("+")) { pos++; return { t: "pos", a: unary() }; }
      return postfix();
    };
    const postfix = () => { let a = rangeOp(); while (isOp("%")) { pos++; a = { t: "pct", a }; } return a; };
    const rangeOp = () => {
      let a = primary();
      while (isOp(":")) {
        pos++;
        const b = primary();
        if (a.t === "ref" && b.t === "ref" && !b.sheet) {
          a = { t: "rng", kind: "cells", sheet: a.sheet, r1: a.r, c1: a.c, r2: b.r, c2: b.c, ar1: a.ar, ac1: a.ac, ar2: b.ar, ac2: b.ac };
        } else a = { t: "op", op: ":", a, b };
      }
      return a;
    };
    const constant = () => {
      const tok = tokens[pos++];
      if (!tok) throw new FormulaSyntax("The array is incomplete.");
      if (tok.t === "op" && tok.v === "-") { const next = tokens[pos++]; if (next?.t !== "num") throw new FormulaSyntax("Arrays hold constants only."); return { t: "n", v: -next.v }; }
      if (tok.t === "num") return { t: "n", v: tok.v };
      if (tok.t === "str") return { t: "s", v: tok.v };
      if (tok.t === "bool") return { t: "b", v: tok.v };
      if (tok.t === "err") return { t: "e", v: tok.v };
      throw new FormulaSyntax("Arrays hold constants only.");
    };
    const primary = () => {
      const tok = tokens[pos++];
      if (!tok) throw new FormulaSyntax("The formula is incomplete.");
      switch (tok.t) {
        case "num": return { t: "n", v: tok.v };
        case "str": return { t: "s", v: tok.v };
        case "bool": return { t: "b", v: tok.v };
        case "err": return { t: "e", v: tok.v };
        case "node": return tok.node;
        case "name": return { t: "name", v: tok.v, sheet: tok.sheet || null };
        case "fn": {
          expect("(");
          const args = [];
          if (isOp(")")) { pos++; return { t: "fn", name: tok.v, args }; }
          for (;;) {
            if (isOp(",") || isOp(";") || isOp(")")) args.push({ t: "miss" });
            else args.push(comparison());
            if (isOp(",") || isOp(";")) { pos++; continue; }
            expect(")");
            break;
          }
          return { t: "fn", name: tok.v, args };
        }
        case "op":
          if (tok.v === "(") { const a = comparison(); expect(")"); return { t: "paren", a }; }
          if (tok.v === "{") {
            const rows = [[]];
            for (;;) {
              rows[rows.length - 1].push(constant());
              if (isOp(",")) { pos++; continue; }
              if (isOp(";")) { pos++; rows.push([]); continue; }
              expect("}");
              break;
            }
            if (rows.some((row) => row.length !== rows[0].length)) throw new FormulaSyntax("Every row of an array needs the same number of values.");
            return { t: "arr", rows };
          }
          throw new FormulaSyntax(`Unexpected "${tok.v}".`);
        default: throw new FormulaSyntax("Unexpected token.");
      }
    };
    if (!tokens.length) throw new FormulaSyntax("The formula is empty.");
    const ast = comparison();
    if (pos < tokens.length) throw new FormulaSyntax(`Unexpected "${tokens[pos].v ?? "value"}".`);
    return ast;
  }

  function numberLiteral(v) {
    const text = String(v);
    return text.includes("e") ? text.toUpperCase() : text;
  }
  const refPart = (r, c, ar, ac) => `${ac ? "$" : ""}${colName(c)}${ar ? "$" : ""}${r + 1}`;
  function astText(node) {
    switch (node.t) {
      case "n": return numberLiteral(node.v);
      case "s": return `"${node.v.replace(/"/g, '""')}"`;
      case "b": return node.v ? "TRUE" : "FALSE";
      case "e": return node.v;
      case "miss": return "";
      case "ref": return (node.sheet ? quoteSheet(node.sheet) + "!" : "") + refPart(node.r, node.c, node.ar, node.ac);
      case "rng": {
        const prefix = node.sheet ? quoteSheet(node.sheet) + "!" : "";
        if (node.kind === "cols") return `${prefix}${node.ac1 ? "$" : ""}${colName(node.c1)}:${node.ac2 ? "$" : ""}${colName(node.c2)}`;
        if (node.kind === "rows") return `${prefix}${node.ar1 ? "$" : ""}${node.r1 + 1}:${node.ar2 ? "$" : ""}${node.r2 + 1}`;
        return `${prefix}${refPart(node.r1, node.c1, node.ar1, node.ac1)}:${refPart(node.r2, node.c2, node.ar2, node.ac2)}`;
      }
      case "fn": return `${node.name}(${node.args.map(astText).join(",")})`;
      case "op": return astText(node.a) + node.op + astText(node.b);
      case "neg": return "-" + astText(node.a);
      case "pos": return "+" + astText(node.a);
      case "pct": return astText(node.a) + "%";
      case "paren": return "(" + astText(node.a) + ")";
      case "name": return (node.sheet ? quoteSheet(node.sheet) + "!" : "") + node.v;
      case "arr": return "{" + node.rows.map((row) => row.map(astText).join(",")).join(";") + "}";
      default: return "";
    }
  }
  function mapAst(node, fn) {
    const mapped = fn(node);
    if (mapped !== node) return mapped;
    switch (node.t) {
      case "fn": return { ...node, args: node.args.map((arg) => mapAst(arg, fn)) };
      case "op": return { ...node, a: mapAst(node.a, fn), b: mapAst(node.b, fn) };
      case "neg": case "pos": case "pct": case "paren": return { ...node, a: mapAst(node.a, fn) };
      default: return node;
    }
  }
  function walkAst(node, fn) {
    fn(node);
    if (node.t === "fn") node.args.forEach((arg) => walkAst(arg, fn));
    else if (node.t === "op") { walkAst(node.a, fn); walkAst(node.b, fn); }
    else if (node.a) walkAst(node.a, fn);
  }
  const REF_ERROR = { t: "e", v: "#REF!" };
  function shiftNode(node, dr, dc) {
    if (node.t === "ref") {
      const r = node.ar ? node.r : node.r + dr, c = node.ac ? node.c : node.c + dc;
      if (r < 0 || c < 0 || r >= MAX_ROWS || c >= MAX_COLS) return REF_ERROR;
      return { ...node, r, c };
    }
    if (node.t === "rng") {
      const rowsMove = node.kind !== "cols", colsMove = node.kind !== "rows";
      const r1 = node.ar1 || !rowsMove ? node.r1 : node.r1 + dr, r2 = node.ar2 || !rowsMove ? node.r2 : node.r2 + dr;
      const c1 = node.ac1 || !colsMove ? node.c1 : node.c1 + dc, c2 = node.ac2 || !colsMove ? node.c2 : node.c2 + dc;
      if (Math.min(r1, r2, c1, c2) < 0 || Math.max(r1, r2) >= MAX_ROWS || Math.max(c1, c2) >= MAX_COLS) return REF_ERROR;
      return { ...node, r1, r2, c1, c2 };
    }
    return node;
  }
  function shiftFormula(text, dr, dc) {
    if (!dr && !dc) return text;
    try { return astText(mapAst(parseFormula(text), (node) => shiftNode(node, dr, dc))); }
    catch { return text; }
  }
  function adjustIndex(value, at, count) {
    if (count > 0) return value >= at ? value + count : value;
    const end = at - count;
    if (value >= end) return value + count;
    if (value >= at) return null;
    return value;
  }
  function adjustSpan(lo, hi, at, count, max) {
    if (count > 0) return [lo >= at ? lo + count : lo, hi >= at ? Math.min(max - 1, hi + count) : hi];
    const end = at - count;
    const nlo = lo >= end ? lo + count : lo >= at ? at : lo;
    const nhi = hi >= end ? hi + count : hi >= at ? at - 1 : hi;
    return nhi < nlo ? null : [nlo, nhi];
  }
  function structuralMapper(formulaSheet, targetSheet, axis, at, count) {
    const target = targetSheet.toLowerCase();
    const onTarget = (node) => (node.sheet || formulaSheet).toLowerCase() === target;
    return (node) => {
      if ((node.t !== "ref" && node.t !== "rng") || !onTarget(node)) return node;
      if (node.t === "ref") {
        if (axis === "row") { const r = adjustIndex(node.r, at, count); return r === null || r >= MAX_ROWS ? REF_ERROR : { ...node, r }; }
        const c = adjustIndex(node.c, at, count); return c === null || c >= MAX_COLS ? REF_ERROR : { ...node, c };
      }
      if (axis === "row") {
        if (node.kind === "cols") return node;
        const span = adjustSpan(node.r1, node.r2, at, count, MAX_ROWS);
        return span ? { ...node, r1: span[0], r2: span[1] } : REF_ERROR;
      }
      if (node.kind === "rows") return node;
      const span = adjustSpan(node.c1, node.c2, at, count, MAX_COLS);
      return span ? { ...node, c1: span[0], c2: span[1] } : REF_ERROR;
    };
  }
  function rewriteFormula(text, mapper) {
    try {
      const ast = parseFormula(text);
      let changed = false;
      const out = mapAst(ast, (node) => { const next = mapper(node); if (next !== node) changed = true; return next; });
      return changed ? astText(out) : text;
    } catch { return text; }
  }
  function formulaReferences(text) {
    const refs = [];
    try {
      walkAst(parseFormula(text), (node) => {
        if (node.t === "ref") refs.push({ sheet: node.sheet, r1: node.r, c1: node.c, r2: node.r, c2: node.c });
        else if (node.t === "rng") refs.push({ sheet: node.sheet, r1: Math.min(node.r1, node.r2), c1: Math.min(node.c1, node.c2), r2: Math.max(node.r1, node.r2), c2: Math.max(node.c1, node.c2) });
      });
    } catch { return refs; }
    return refs;
  }
  function referenceTokens(text) {
    const out = [];
    const re = /('(?:[^']|'')+'!|[A-Za-z_][A-Za-z0-9_.]*!)?(\$?[A-Za-z]{1,3}\$?\d{1,7}(?::\$?[A-Za-z]{1,3}\$?\d{1,7})?)(?![A-Za-z0-9_(!])/g;
    let quoted = false, last = 0, m;
    while ((m = re.exec(text))) {
      for (let i = last; i < m.index; i++) if (text[i] === '"') quoted = !quoted;
      last = m.index;
      if (quoted) continue;
      const before = text[m.index - 1];
      if (before && /[A-Za-z0-9_.$]/.test(before)) continue;
      const range = parseRange(m[2]);
      if (!range) continue;
      const sheet = m[1] ? m[1].slice(0, -1).replace(/^'|'$/g, "").replace(/''/g, "'") : null;
      out.push({ start: m.index, end: m.index + m[0].length, text: m[0], sheet, ...range });
    }
    return out;
  }
  function toggleAbsolute(ref) {
    const m = /^(\$?)([A-Za-z]{1,3})(\$?)(\d+)$/.exec(ref);
    if (!m) return ref;
    const state = (m[1] ? 2 : 0) + (m[3] ? 1 : 0);
    const next = { 0: [true, true], 3: [false, true], 1: [true, false], 2: [false, false] }[state];
    return `${next[0] ? "$" : ""}${m[2]}${next[1] ? "$" : ""}${m[4]}`;
  }

  class Range {
    constructor(sheet, r1, c1, r2, c2) {
      this.sheet = sheet;
      this.r1 = Math.min(r1, r2); this.c1 = Math.min(c1, c2);
      this.r2 = Math.max(r1, r2); this.c2 = Math.max(c1, c2);
    }
    get rows() { return this.r2 - this.r1 + 1; }
    get cols() { return this.c2 - this.c1 + 1; }
  }
  const isArrayLike = (v) => Array.isArray(v) || (v instanceof Range && (v.r1 !== v.r2 || v.c1 !== v.c2));
  const blank = (v) => v === null || v === undefined || v === MISSING;

  function compareValues(a, b) {
    if (blank(a)) a = typeof b === "string" ? "" : typeof b === "boolean" ? false : 0;
    if (blank(b)) b = typeof a === "string" ? "" : typeof a === "boolean" ? false : 0;
    const rank = (v) => typeof v === "number" ? 1 : typeof v === "string" ? 2 : typeof v === "boolean" ? 3 : 4;
    const ra = rank(a), rb = rank(b);
    if (ra !== rb) return ra < rb ? -1 : 1;
    if (ra === 2) { const x = a.toLowerCase(), y = b.toLowerCase(); return x < y ? -1 : x > y ? 1 : 0; }
    if (ra === 4) return 0;
    return a < b ? -1 : a > b ? 1 : 0;
  }
  function wildcardRegex(pattern) {
    let out = "";
    for (let i = 0; i < pattern.length; i++) {
      const ch = pattern[i];
      if (ch === "~" && i + 1 < pattern.length) { out += pattern[++i].replace(/[.*+?^${}()|[\]\\]/g, "\\$&"); }
      else if (ch === "*") out += "[\\s\\S]*";
      else if (ch === "?") out += "[\\s\\S]";
      else out += ch.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    }
    return new RegExp("^" + out + "$", "i");
  }
  function criterion(crit) {
    if (isErr(crit)) return () => false;
    if (blank(crit)) return (v) => blank(v) || v === "";
    if (typeof crit === "number") return (v) => typeof v === "number" ? v === crit : typeof v === "string" && textToNumber(v) === crit;
    if (typeof crit === "boolean") return (v) => v === crit;
    const m = /^(<=|>=|<>|<|>|=)?([\s\S]*)$/.exec(String(crit));
    const op = m[1] || "", rest = m[2];
    const num = rest === "" ? null : textToNumber(rest);
    const bool = /^(TRUE|FALSE)$/i.test(rest) ? rest.toUpperCase() === "TRUE" : null;
    if (op === "" || op === "=") {
      if (rest === "") return (v) => blank(v) || v === "";
      if (num !== null) return (v) => typeof v === "number" ? Math.abs(v - num) < 1e-12 : typeof v === "string" && textToNumber(v) === num;
      if (bool !== null) return (v) => v === bool;
      const re = wildcardRegex(rest);
      return (v) => typeof v === "string" && re.test(v);
    }
    if (op === "<>") {
      if (rest === "") return (v) => !(blank(v) || v === "");
      if (num !== null) return (v) => !(typeof v === "number" && Math.abs(v - num) < 1e-12);
      if (bool !== null) return (v) => v !== bool;
      const re = wildcardRegex(rest);
      return (v) => !(typeof v === "string" && re.test(v));
    }
    const test = (cmp) => op === "<" ? cmp < 0 : op === "<=" ? cmp <= 0 : op === ">" ? cmp > 0 : cmp >= 0;
    if (num !== null) return (v) => typeof v === "number" && test(v - num);
    const lower = rest.toLowerCase();
    return (v) => typeof v === "string" && test(v.toLowerCase() < lower ? -1 : v.toLowerCase() > lower ? 1 : 0);
  }

  class Calc {
    constructor(workbook) {
      this.wb = workbook;
      this.values = new Map();
      this.spills = new Map();
      this.previousSpills = new Map();
      this.active = new Set();
      this.astCache = new Map();
      this.nameCache = new Map();
      this.sheetIndex = new Map();
      this.bounds = new Map();
      this.now = null;
    }
    reset() {
      this.values = new Map();
      this.previousSpills = this.spills;
      this.spills = new Map();
      this.active = new Set();
      this.nameCache = new Map();
      this.sheetIndex = new Map(this.wb.sheets.map((sheet) => [sheet.name.toLowerCase(), sheet]));
      this.bounds = new Map();
      this.now = null;
      if (this.astCache.size > 50000) this.astCache = new Map();
    }
    recalc() {
      this.reset();
      this.pass();
      if (this.spills.size || this.previousSpills.size) {
        this.values = new Map();
        this.previousSpills = this.spills;
        this.spills = new Map();
        this.pass();
      }
      this.previousSpills = new Map();
    }
    pass() {
      for (const sheet of this.wb.sheets) {
        const keys = [];
        for (const [k, cell] of sheet.cells) if (cell.f !== undefined) keys.push(k);
        keys.sort((a, b) => a - b);
        for (const k of keys) this.formulaValue(sheet, k, sheet.cells.get(k));
      }
    }
    sheetBounds(sheet) {
      let b = this.bounds.get(sheet.id);
      if (!b) {
        let maxRow = -1, maxCol = -1;
        for (const k of sheet.cells.keys()) { const r = keyRow(k), c = keyCol(k); if (r > maxRow) maxRow = r; if (c > maxCol) maxCol = c; }
        const spill = this.spills.get(sheet.id);
        if (spill) for (const k of spill.keys()) { const r = keyRow(k), c = keyCol(k); if (r > maxRow) maxRow = r; if (c > maxCol) maxCol = c; }
        b = { maxRow, maxCol };
        this.bounds.set(sheet.id, b);
      }
      return b;
    }
    sheetFor(name, ctx) {
      if (!name) return ctx.sheet;
      if (!this.sheetIndex.size) this.sheetIndex = new Map(this.wb.sheets.map((sheet) => [sheet.name.toLowerCase(), sheet]));
      return this.sheetIndex.get(String(name).toLowerCase()) || null;
    }
    parsed(text) {
      let ast = this.astCache.get(text);
      if (ast === undefined) {
        try { ast = parseFormula(text); } catch { ast = null; }
        this.astCache.set(text, ast);
      }
      return ast;
    }
    cellValue(sheet, r, c) {
      const k = cellKey(r, c);
      const cell = sheet.cells.get(k);
      if (cell) {
        if (cell.f !== undefined) return this.formulaValue(sheet, k, cell);
        if (cell.v !== undefined && cell.v !== null) return cell.v;
      }
      const spill = this.spills.get(sheet.id);
      if (spill && spill.has(k)) return spill.get(k).value;
      const prev = this.previousSpills.get(sheet.id);
      if (prev && prev.has(k)) return prev.get(k).value;
      return null;
    }
    spillOwner(sheet, r, c) {
      const entry = this.spills.get(sheet.id)?.get(cellKey(r, c));
      return entry ? entry.owner : null;
    }
    spillRange(sheet, r, c) {
      const entry = this.spills.get(sheet.id)?.get(cellKey(r, c));
      return entry ? entry.range : null;
    }
    formulaValue(sheet, k, cell) {
      let store = this.values.get(sheet.id);
      if (!store) { store = new Map(); this.values.set(sheet.id, store); }
      if (store.has(k)) return store.get(k);
      const id = sheet.id + "|" + k;
      if (this.active.has(id)) return ERRORS["#REF!"];
      this.active.add(id);
      const r = keyRow(k), c = keyCol(k);
      let result;
      try {
        const ast = this.parsed(cell.f);
        result = ast ? this.finish(this.ev(ast, { sheet, r, c, cell }), sheet, r, c, k) : cell.v !== undefined && cell.v !== null ? cell.v : ERRORS["#ERROR!"];
      } catch (error) {
        if (error === UNKNOWN_FUNCTION) result = cell.v !== undefined && cell.v !== null ? cell.v : ERRORS["#NAME?"];
        else if (error instanceof RangeError) result = ERRORS["#CALC!"];
        else result = ERRORS["#VALUE!"];
      }
      this.active.delete(id);
      store.set(k, result);
      return result;
    }
    finish(result, sheet, r, c, k) {
      if (result instanceof Range) {
        if (result.rows === 1 && result.cols === 1) return this.plain(this.cellValue(result.sheet, result.r1, result.c1));
        result = this.matrix(result);
      }
      if (Array.isArray(result)) {
        const rows = result.length, cols = rows ? result[0].length : 0;
        if (!rows || !cols) return ERRORS["#CALC!"];
        if (rows === 1 && cols === 1) return this.plain(result[0][0]);
        if (r + rows > MAX_ROWS || c + cols > MAX_COLS) return ERRORS["#SPILL!"];
        let spill = this.spills.get(sheet.id);
        for (let i = 0; i < rows; i++) {
          for (let j = 0; j < cols; j++) {
            if (!i && !j) continue;
            const target = sheet.cells.get(cellKey(r + i, c + j));
            if (target && (target.f !== undefined || (target.v !== undefined && target.v !== null))) return ERRORS["#SPILL!"];
            const other = spill?.get(cellKey(r + i, c + j));
            if (other && other.owner !== k) return ERRORS["#SPILL!"];
          }
        }
        if (!spill) { spill = new Map(); this.spills.set(sheet.id, spill); }
        const range = { r1: r, c1: c, r2: r + rows - 1, c2: c + cols - 1 };
        for (let i = 0; i < rows; i++) for (let j = 0; j < cols; j++) {
          if (!i && !j) continue;
          spill.set(cellKey(r + i, c + j), { owner: k, value: this.plain(result[i][j]), range });
        }
        this.bounds.delete(sheet.id);
        spill.set(k, { owner: k, value: this.plain(result[0][0]), range, origin: true });
        return this.plain(result[0][0]);
      }
      return this.plain(result);
    }
    plain(v) {
      if (blank(v)) return 0;
      if (typeof v === "number" && !Number.isFinite(v)) return ERRORS["#NUM!"];
      return v;
    }
    evaluate(text, sheet, r, c) {
      const ast = this.parsed(text);
      if (!ast) return ERRORS["#ERROR!"];
      try {
        const v = this.ev(ast, { sheet, r, c, cell: null });
        return v instanceof Range ? (v.rows === 1 && v.cols === 1 ? this.cellValue(v.sheet, v.r1, v.c1) : this.matrix(v)) : v;
      } catch (error) { return error === UNKNOWN_FUNCTION ? ERRORS["#NAME?"] : ERRORS["#VALUE!"]; }
    }
    ev(node, ctx) {
      switch (node.t) {
        case "n": case "s": case "b": return node.v;
        case "e": return err(node.v);
        case "miss": return MISSING;
        case "ref": {
          const sheet = this.sheetFor(node.sheet, ctx);
          return sheet ? new Range(sheet, node.r, node.c, node.r, node.c) : ERRORS["#REF!"];
        }
        case "rng": {
          const sheet = this.sheetFor(node.sheet, ctx);
          return sheet ? new Range(sheet, node.r1, node.c1, node.r2, node.c2) : ERRORS["#REF!"];
        }
        case "paren": return this.ev(node.a, ctx);
        case "neg": return this.unary(this.ev(node.a, ctx), (x) => -x);
        case "pos": return this.ev(node.a, ctx);
        case "pct": return this.unary(this.ev(node.a, ctx), (x) => x / 100);
        case "op": {
          const a = this.ev(node.a, ctx), b = this.ev(node.b, ctx);
          if (node.op === ":") {
            if (a instanceof Range && b instanceof Range && a.sheet === b.sheet) return new Range(a.sheet, Math.min(a.r1, b.r1), Math.min(a.c1, b.c1), Math.max(a.r2, b.r2), Math.max(a.c2, b.c2));
            return ERRORS["#VALUE!"];
          }
          return this.binary(node.op, a, b);
        }
        case "name": return this.name(node, ctx);
        case "arr": return node.rows.map((row) => row.map((item) => this.ev(item, ctx)));
        case "fn": {
          const def = FUNCTIONS[node.name];
          if (!def) throw UNKNOWN_FUNCTION;
          if (def.lazy) return def.f(node.args, ctx, this);
          return def.f(node.args.map((arg) => this.ev(arg, ctx)), ctx, this);
        }
        default: return ERRORS["#VALUE!"];
      }
    }
    name(node, ctx) {
      const upper = node.v.toUpperCase();
      if (ctx.locals && ctx.locals.has(upper)) return ctx.locals.get(upper);
      const names = this.wb.names || new Map();
      const def = names.get(upper);
      if (!def) return ERRORS["#NAME?"];
      const key = upper;
      if (this.active.has("name|" + key)) return ERRORS["#REF!"];
      this.active.add("name|" + key);
      try {
        const ast = this.parsed(def.ref.replace(/^=/, ""));
        if (!ast) return ERRORS["#NAME?"];
        const defSheet = def.sheet ? this.sheetFor(def.sheet, ctx) : this.wb.sheets[0];
        return this.ev(ast, { ...ctx, sheet: defSheet || ctx.sheet });
      } finally { this.active.delete("name|" + key); }
    }
    matrix(v) {
      if (Array.isArray(v)) return v;
      if (v instanceof Range) {
        const b = this.sheetBounds(v.sheet);
        const r2 = v.r2 >= MAX_ROWS - 1 ? Math.max(v.r1, Math.min(v.r2, b.maxRow)) : v.r2;
        const c2 = v.c2 >= MAX_COLS - 1 ? Math.max(v.c1, Math.min(v.c2, b.maxCol)) : v.c2;
        const out = [];
        for (let r = v.r1; r <= r2; r++) {
          const row = [];
          for (let c = v.c1; c <= c2; c++) row.push(this.cellValue(v.sheet, r, c));
          out.push(row);
        }
        return out;
      }
      return [[v]];
    }
    clampRange(v) {
      const b = this.sheetBounds(v.sheet);
      return { r1: v.r1, c1: v.c1, r2: Math.min(v.r2, Math.max(b.maxRow, v.r1)), c2: Math.min(v.c2, Math.max(b.maxCol, v.c1)) };
    }
    scalar(v, ctx) {
      if (v instanceof Range) {
        if (v.r1 === v.r2 && v.c1 === v.c2) return this.cellValue(v.sheet, v.r1, v.c1);
        if (ctx && v.c1 === v.c2 && ctx.sheet === v.sheet && ctx.r >= v.r1 && ctx.r <= v.r2) return this.cellValue(v.sheet, ctx.r, v.c1);
        if (ctx && v.r1 === v.r2 && ctx.sheet === v.sheet && ctx.c >= v.c1 && ctx.c <= v.c2) return this.cellValue(v.sheet, v.r1, ctx.c);
        return ERRORS["#VALUE!"];
      }
      if (Array.isArray(v)) return v.length && v[0].length ? v[0][0] : ERRORS["#VALUE!"];
      return v;
    }
    each(v, fn) {
      if (v instanceof Range) {
        const b = this.clampRange(v);
        for (let r = b.r1; r <= b.r2; r++) for (let c = b.c1; c <= b.c2; c++) if (fn(this.cellValue(v.sheet, r, c), r, c) === false) return;
      } else if (Array.isArray(v)) {
        for (const row of v) for (const item of row) if (fn(item) === false) return;
      } else fn(v);
    }
    broadcast(values, fn) {
      const mats = values.map((v) => isArrayLike(v) ? this.matrix(v) : null);
      let rows = 1, cols = 1;
      for (const m of mats) if (m) { rows = Math.max(rows, m.length); cols = Math.max(cols, m[0]?.length || 0); }
      const out = [];
      for (let i = 0; i < rows; i++) {
        const row = [];
        for (let j = 0; j < cols; j++) {
          const args = values.map((v, index) => {
            const m = mats[index];
            if (!m) return v instanceof Range ? this.scalar(v) : v;
            const mi = m.length === 1 ? 0 : i, mj = m[0].length === 1 ? 0 : j;
            return mi < m.length && mj < m[0].length ? m[mi][mj] : ERRORS["#N/A"];
          });
          row.push(fn(...args));
        }
        out.push(row);
      }
      return out;
    }
    unary(v, fn) {
      if (isArrayLike(v)) return this.broadcast([v], (x) => this.unaryScalar(x, fn));
      return this.unaryScalar(this.scalar(v), fn);
    }
    unaryScalar(x, fn) {
      const n = toNumber(x);
      return isErr(n) ? n : fn(n);
    }
    binary(op, a, b) {
      if (isArrayLike(a) || isArrayLike(b)) return this.broadcast([a, b], (x, y) => binaryScalar(op, x, y));
      return binaryScalar(op, this.scalar(a), this.scalar(b));
    }
  }
  function toNumber(v) {
    if (blank(v)) return 0;
    if (typeof v === "number") return v;
    if (typeof v === "boolean") return v ? 1 : 0;
    if (isErr(v)) return v;
    if (typeof v === "string") { const n = textToNumber(v); return n === null ? ERRORS["#VALUE!"] : n; }
    return ERRORS["#VALUE!"];
  }
  function toText(v) {
    if (blank(v)) return "";
    if (isErr(v)) return v;
    return valueToText(v);
  }
  function toBool(v) {
    if (blank(v)) return false;
    if (typeof v === "boolean") return v;
    if (typeof v === "number") return v !== 0;
    if (isErr(v)) return v;
    const upper = String(v).toUpperCase();
    if (upper === "TRUE") return true;
    if (upper === "FALSE") return false;
    return ERRORS["#VALUE!"];
  }
  function binaryScalar(op, x, y) {
    if (isErr(x)) return x;
    if (isErr(y)) return y;
    if (op === "&") return toText(x) + toText(y);
    if (["=", "<>", "<", ">", "<=", ">="].includes(op)) {
      const cmp = compareValues(x, y);
      switch (op) {
        case "=": return cmp === 0;
        case "<>": return cmp !== 0;
        case "<": return cmp < 0;
        case ">": return cmp > 0;
        case "<=": return cmp <= 0;
        default: return cmp >= 0;
      }
    }
    const a = toNumber(x), b = toNumber(y);
    if (isErr(a)) return a;
    if (isErr(b)) return b;
    let out;
    switch (op) {
      case "+": out = a + b; break;
      case "-": out = a - b; break;
      case "*": out = a * b; break;
      case "/": if (b === 0) return ERRORS["#DIV/0!"]; out = a / b; break;
      case "^": if (a === 0 && b === 0) return ERRORS["#NUM!"]; out = Math.pow(a, b); break;
      default: return ERRORS["#VALUE!"];
    }
    return Number.isFinite(out) ? out : ERRORS["#NUM!"];
  }
  const FUNCTIONS = Object.create(null);
  const define = (names, f, lazy = false) => { for (const name of names.split(" ")) FUNCTIONS[name] = { f, lazy }; };
  const arg = (args, i, fallback) => (i < args.length && args[i] !== MISSING && args[i] !== undefined ? args[i] : fallback);
  const firstError = (list) => list.find(isErr);

  function numbers(args, c, mode = "plain") {
    const out = [];
    for (const a of args) {
      if (a === MISSING) continue;
      if (a instanceof Range || Array.isArray(a)) {
        let failure = null;
        c.each(a, (v) => {
          if (isErr(v)) { failure = v; return false; }
          if (typeof v === "number") out.push(v);
          else if (mode === "a" && typeof v === "boolean") out.push(v ? 1 : 0);
          else if (mode === "a" && typeof v === "string") out.push(0);
          return true;
        });
        if (failure) return failure;
      } else {
        const n = toNumber(a);
        if (isErr(n)) return n;
        out.push(n);
      }
    }
    return out;
  }
  const withNumbers = (fn, mode) => (args, ctx, c) => {
    const list = numbers(args, c, mode);
    return isErr(list) ? list : fn(list, args, ctx, c);
  };
  const lift = (fn) => (args, ctx, c) => {
    if (!args.some(isArrayLike)) return fn(args.map((a) => (a instanceof Range ? c.scalar(a, ctx) : a)), ctx, c);
    return c.broadcast(args, (...values) => fn(values, ctx, c));
  };
  const num1 = (fn) => lift(([x]) => { const n = toNumber(x); if (isErr(n)) return n; const out = fn(n); return Number.isFinite(out) ? out : ERRORS["#NUM!"]; });
  const num2 = (fn, fallback) => lift(([x, y]) => {
    const a = toNumber(x); if (isErr(a)) return a;
    const b = y === MISSING || y === undefined ? fallback : toNumber(y); if (isErr(b)) return b;
    const out = fn(a, b);
    return isErr(out) ? out : Number.isFinite(out) ? out : ERRORS["#NUM!"];
  });
  const text1 = (fn) => lift(([x, ...rest]) => { const s = toText(x); return isErr(s) ? s : fn(s, ...rest); });
  const sum = (list) => { let s = 0; for (const n of list) s += n; return s; };
  const mean = (list) => (list.length ? sum(list) / list.length : ERRORS["#DIV/0!"]);
  function variance(list, sample) {
    if (list.length < (sample ? 2 : 1)) return ERRORS["#DIV/0!"];
    const m = sum(list) / list.length;
    let s = 0;
    for (const n of list) s += (n - m) * (n - m);
    return s / (list.length - (sample ? 1 : 0));
  }
  function sortedNumbers(list) { return [...list].sort((a, b) => a - b); }
  function percentileInc(list, p) {
    if (!list.length || p < 0 || p > 1) return ERRORS["#NUM!"];
    const sorted = sortedNumbers(list);
    const pos = (sorted.length - 1) * p;
    const lo = Math.floor(pos), hi = Math.ceil(pos);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - lo);
  }
  function factorial(n) { let out = 1; for (let i = 2; i <= n; i++) out *= i; return out; }
  function gcd(a, b) { a = Math.abs(a); b = Math.abs(b); while (b) [a, b] = [b, a % b]; return a; }
  function vector(v, c) {
    const m = c.matrix(v);
    if (m.length === 1) return m[0];
    if (m[0] && m[0].length === 1) return m.map((row) => row[0]);
    return null;
  }
  function pairNumbers(xs, ys, c) {
    const a = c.matrix(xs).flat(), b = c.matrix(ys).flat();
    if (a.length !== b.length) return ERRORS["#N/A"];
    const x = [], y = [];
    for (let i = 0; i < a.length; i++) {
      if (isErr(a[i])) return a[i];
      if (isErr(b[i])) return b[i];
      if (typeof a[i] === "number" && typeof b[i] === "number") { x.push(a[i]); y.push(b[i]); }
    }
    return [x, y];
  }
  function regression(xs, ys, c) {
    const pair = pairNumbers(ys, xs, c);
    if (isErr(pair)) return pair;
    const [y, x] = pair;
    if (x.length < 2) return ERRORS["#DIV/0!"];
    const mx = sum(x) / x.length, my = sum(y) / y.length;
    let sxx = 0, sxy = 0, syy = 0;
    for (let i = 0; i < x.length; i++) { sxx += (x[i] - mx) ** 2; sxy += (x[i] - mx) * (y[i] - my); syy += (y[i] - my) ** 2; }
    if (sxx === 0) return ERRORS["#DIV/0!"];
    const slope = sxy / sxx;
    return { slope, intercept: my - slope * mx, r: syy === 0 ? ERRORS["#DIV/0!"] : sxy / Math.sqrt(sxx * syy), mx, my, sxx, sxy, syy, n: x.length };
  }
  function erf(x) {
    const t = 1 / (1 + 0.5 * Math.abs(x));
    const y = 1 - t * Math.exp(-x * x - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
    return x >= 0 ? y : -y;
  }
  const normCdf = (z) => 0.5 * (1 + erf(z / Math.SQRT2));
  function normInv(p) {
    if (p <= 0 || p >= 1) return ERRORS["#NUM!"];
    const a = [-39.69683028665376, 220.9460984245205, -275.9285104469687, 138.357751867269, -30.66479806614716, 2.506628277459239];
    const b = [-54.47609879822406, 161.5858368580409, -155.6989798598866, 66.80131188771972, -13.28068155288572];
    const cc = [-0.007784894002430293, -0.3223964580411365, -2.400758277161838, -2.549732539343734, 4.374664141464968, 2.938163982698783];
    const d = [0.007784695709041462, 0.3224671290700398, 2.445134137142996, 3.754408661907416];
    const low = 0.02425;
    let q, r;
    if (p < low) { q = Math.sqrt(-2 * Math.log(p)); return (((((cc[0] * q + cc[1]) * q + cc[2]) * q + cc[3]) * q + cc[4]) * q + cc[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    if (p > 1 - low) { q = Math.sqrt(-2 * Math.log(1 - p)); return -(((((cc[0] * q + cc[1]) * q + cc[2]) * q + cc[3]) * q + cc[4]) * q + cc[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    q = p - 0.5; r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
  }

  define("SUM", withNumbers((list) => sum(list)));
  define("PRODUCT", withNumbers((list) => list.reduce((a, b) => a * b, list.length ? 1 : 0)));
  define("SUMSQ", withNumbers((list) => sum(list.map((n) => n * n))));
  define("AVERAGE", withNumbers((list) => mean(list)));
  define("AVERAGEA", withNumbers((list) => mean(list), "a"));
  define("MAX", withNumbers((list) => (list.length ? Math.max(...list) : 0)));
  define("MAXA", withNumbers((list) => (list.length ? Math.max(...list) : 0), "a"));
  define("MIN", withNumbers((list) => (list.length ? Math.min(...list) : 0)));
  define("MINA", withNumbers((list) => (list.length ? Math.min(...list) : 0), "a"));
  define("MEDIAN", withNumbers((list) => (list.length ? percentileInc(list, 0.5) : ERRORS["#NUM!"])));
  define("MODE MODE.SNGL", withNumbers((list) => {
    const counts = new Map();
    let best = null, bestCount = 1;
    for (const n of list) { const k = (counts.get(n) || 0) + 1; counts.set(n, k); if (k > bestCount) { best = n; bestCount = k; } }
    return best === null ? ERRORS["#N/A"] : best;
  }));
  define("STDEV STDEV.S", withNumbers((list) => { const v = variance(list, true); return isErr(v) ? v : Math.sqrt(v); }));
  define("STDEVP STDEV.P", withNumbers((list) => { const v = variance(list, false); return isErr(v) ? v : Math.sqrt(v); }));
  define("STDEVA", withNumbers((list) => { const v = variance(list, true); return isErr(v) ? v : Math.sqrt(v); }, "a"));
  define("VAR VAR.S", withNumbers((list) => variance(list, true)));
  define("VARP VAR.P", withNumbers((list) => variance(list, false)));
  define("GEOMEAN", withNumbers((list) => (list.length && list.every((n) => n > 0) ? Math.exp(sum(list.map(Math.log)) / list.length) : ERRORS["#NUM!"])));
  define("HARMEAN", withNumbers((list) => (list.length && list.every((n) => n > 0) ? list.length / sum(list.map((n) => 1 / n)) : ERRORS["#NUM!"])));
  define("AVEDEV", withNumbers((list) => { if (!list.length) return ERRORS["#NUM!"]; const m = mean(list); return sum(list.map((n) => Math.abs(n - m))) / list.length; }));
  define("DEVSQ", withNumbers((list) => { if (!list.length) return 0; const m = mean(list); return sum(list.map((n) => (n - m) ** 2)); }));
  define("COUNT", (args, ctx, c) => {
    let n = 0;
    for (const a of args) {
      if (a === MISSING) continue;
      if (a instanceof Range || Array.isArray(a)) c.each(a, (v) => { if (typeof v === "number") n++; });
      else if (typeof a === "number" || (typeof a === "string" && textToNumber(a) !== null) || typeof a === "boolean") n++;
    }
    return n;
  });
  define("COUNTA", (args, ctx, c) => {
    let n = 0;
    for (const a of args) {
      if (a === MISSING) continue;
      if (a instanceof Range || Array.isArray(a)) c.each(a, (v) => { if (!blank(v)) n++; });
      else n++;
    }
    return n;
  });
  define("COUNTBLANK", (args, ctx, c) => {
    const range = args[0];
    if (!(range instanceof Range)) return ERRORS["#VALUE!"];
    let n = 0;
    for (let r = range.r1; r <= Math.min(range.r2, range.r1 + 1048575); r++) {
      for (let col = range.c1; col <= range.c2; col++) { const v = c.cellValue(range.sheet, r, col); if (blank(v) || v === "") n++; }
      if ((r - range.r1 + 1) * range.cols > 2000000) break;
    }
    return n;
  });
  define("LARGE", (args, ctx, c) => { const list = numbers([args[0]], c); if (isErr(list)) return list; const k = toNumber(args[1]); if (isErr(k)) return k; const s = sortedNumbers(list).reverse(); return k >= 1 && k <= s.length ? s[Math.ceil(k) - 1] : ERRORS["#NUM!"]; });
  define("SMALL", (args, ctx, c) => { const list = numbers([args[0]], c); if (isErr(list)) return list; const k = toNumber(args[1]); if (isErr(k)) return k; const s = sortedNumbers(list); return k >= 1 && k <= s.length ? s[Math.ceil(k) - 1] : ERRORS["#NUM!"]; });
  define("PERCENTILE PERCENTILE.INC", (args, ctx, c) => { const list = numbers([args[0]], c); if (isErr(list)) return list; const p = toNumber(args[1]); return isErr(p) ? p : percentileInc(list, p); });
  define("QUARTILE QUARTILE.INC", (args, ctx, c) => { const list = numbers([args[0]], c); if (isErr(list)) return list; const q = toNumber(args[1]); if (isErr(q)) return q; return q >= 0 && q <= 4 ? percentileInc(list, Math.floor(q) / 4) : ERRORS["#NUM!"]; });
  define("RANK RANK.EQ", (args, ctx, c) => {
    const n = toNumber(c.scalar(args[0], ctx)); if (isErr(n)) return n;
    const list = numbers([args[1]], c); if (isErr(list)) return list;
    const order = toNumber(arg(args, 2, 0));
    if (!list.includes(n)) return ERRORS["#N/A"];
    return 1 + list.filter((x) => (order ? x < n : x > n)).length;
  });
  define("CORREL PEARSON", (args, ctx, c) => { const r = regression(args[0], args[1], c); return isErr(r) ? r : r.r; });
  define("RSQ", (args, ctx, c) => { const r = regression(args[0], args[1], c); return isErr(r) ? r : isErr(r.r) ? r.r : r.r * r.r; });
  define("SLOPE", (args, ctx, c) => { const r = regression(args[1], args[0], c); return isErr(r) ? r : r.slope; });
  define("INTERCEPT", (args, ctx, c) => { const r = regression(args[1], args[0], c); return isErr(r) ? r : r.intercept; });
  define("FORECAST FORECAST.LINEAR", (args, ctx, c) => { const x = toNumber(c.scalar(args[0], ctx)); if (isErr(x)) return x; const r = regression(args[2], args[1], c); return isErr(r) ? r : r.intercept + r.slope * x; });
  define("COVAR COVARIANCE.P", (args, ctx, c) => { const p = pairNumbers(args[0], args[1], c); if (isErr(p)) return p; const [x, y] = p; if (!x.length) return ERRORS["#DIV/0!"]; const mx = mean(x), my = mean(y); return sum(x.map((v, i) => (v - mx) * (y[i] - my))) / x.length; });
  define("COVARIANCE.S", (args, ctx, c) => { const p = pairNumbers(args[0], args[1], c); if (isErr(p)) return p; const [x, y] = p; if (x.length < 2) return ERRORS["#DIV/0!"]; const mx = mean(x), my = mean(y); return sum(x.map((v, i) => (v - mx) * (y[i] - my))) / (x.length - 1); });
  define("STANDARDIZE", num2Three((x, m, s) => (s <= 0 ? ERRORS["#NUM!"] : (x - m) / s)));
  function num2Three(fn) { return lift((values) => { const n = values.map(toNumber); return firstError(n) || fn(...n); }); }
  define("NORM.DIST NORMDIST", lift(([x, m, s, cumulative]) => {
    const n = [x, m, s].map(toNumber); if (firstError(n)) return firstError(n);
    if (n[2] <= 0) return ERRORS["#NUM!"];
    const z = (n[0] - n[1]) / n[2];
    return toBool(cumulative) === true ? normCdf(z) : Math.exp(-0.5 * z * z) / (n[2] * Math.sqrt(2 * Math.PI));
  }));
  define("NORM.S.DIST", lift(([z, cumulative]) => { const n = toNumber(z); if (isErr(n)) return n; return toBool(arg([cumulative], 0, true)) === true ? normCdf(n) : Math.exp(-0.5 * n * n) / Math.sqrt(2 * Math.PI); }));
  define("NORMSDIST", num1((z) => normCdf(z)));
  define("NORM.INV NORMINV", lift(([p, m, s]) => { const n = [p, m, s].map(toNumber); if (firstError(n)) return firstError(n); if (n[2] <= 0) return ERRORS["#NUM!"]; const z = normInv(n[0]); return isErr(z) ? z : n[1] + n[2] * z; }));
  define("NORM.S.INV NORMSINV", num1((p) => { const z = normInv(p); return isErr(z) ? NaN : z; }));

  define("ABS", num1(Math.abs));
  define("SQRT", num1((n) => (n < 0 ? NaN : Math.sqrt(n))));
  define("EXP", num1(Math.exp));
  define("LN", num1((n) => (n <= 0 ? NaN : Math.log(n))));
  define("LOG10", num1((n) => (n <= 0 ? NaN : Math.log10(n))));
  define("LOG", num2((n, b) => (n <= 0 || b <= 0 || b === 1 ? NaN : Math.log(n) / Math.log(b)), 10));
  define("INT", num1(Math.floor));
  define("SIGN", num1(Math.sign));
  define("SIN", num1(Math.sin)); define("COS", num1(Math.cos)); define("TAN", num1(Math.tan));
  define("ASIN", num1((n) => (Math.abs(n) > 1 ? NaN : Math.asin(n)))); define("ACOS", num1((n) => (Math.abs(n) > 1 ? NaN : Math.acos(n)))); define("ATAN", num1(Math.atan));
  define("SINH", num1(Math.sinh)); define("COSH", num1(Math.cosh)); define("TANH", num1(Math.tanh));
  define("ASINH", num1(Math.asinh)); define("ACOSH", num1((n) => (n < 1 ? NaN : Math.acosh(n)))); define("ATANH", num1((n) => (Math.abs(n) >= 1 ? NaN : Math.atanh(n))));
  define("ATAN2", num2((x, y) => (x === 0 && y === 0 ? ERRORS["#DIV/0!"] : Math.atan2(y, x))));
  define("DEGREES", num1((n) => (n * 180) / Math.PI));
  define("RADIANS", num1((n) => (n * Math.PI) / 180));
  define("PI", () => Math.PI);
  define("RAND", () => Math.random());
  define("RANDBETWEEN", num2((lo, hi) => (hi < lo ? NaN : Math.floor(Math.random() * (Math.floor(hi) - Math.ceil(lo) + 1)) + Math.ceil(lo))));
  define("ROUND", num2((n, d) => roundTo(n, Math.trunc(d)), 0));
  define("ROUNDUP", num2((n, d) => { const f = Math.pow(10, Math.trunc(d)); return Math.sign(n) * Math.ceil(roundTo(Math.abs(n) * f, 9)) / f; }, 0));
  define("ROUNDDOWN", num2((n, d) => { const f = Math.pow(10, Math.trunc(d)); return Math.sign(n) * Math.floor(roundTo(Math.abs(n) * f, 9)) / f; }, 0));
  define("TRUNC", num2((n, d) => { const f = Math.pow(10, Math.trunc(d)); return Math.trunc(roundTo(n * f, 9)) / f; }, 0));
  define("MROUND", num2((n, m) => (m === 0 ? 0 : n * m < 0 ? NaN : roundTo(Math.round(roundTo(n / m, 9)) * m, 12))));
  define("CEILING", num2((n, s) => (s === 0 ? 0 : n > 0 && s < 0 ? NaN : roundTo(Math.ceil(roundTo(n / s, 9)) * s, 12)), 1));
  define("FLOOR", num2((n, s) => (s === 0 ? ERRORS["#DIV/0!"] : n > 0 && s < 0 ? NaN : roundTo(Math.floor(roundTo(n / s, 9)) * s, 12)), 1));
  define("CEILING.MATH CEILING.PRECISE ISO.CEILING", lift(([x, s, mode]) => {
    const n = toNumber(x), sig = Math.abs(toNumber(arg([s], 0, 1))); if (isErr(n)) return n; if (isErr(sig)) return sig;
    if (sig === 0) return 0;
    const away = n < 0 && toNumber(arg([mode], 0, 0)) !== 0;
    return roundTo((away ? Math.floor(roundTo(n / sig, 9)) : Math.ceil(roundTo(n / sig, 9))) * sig, 12);
  }));
  define("FLOOR.MATH FLOOR.PRECISE", lift(([x, s, mode]) => {
    const n = toNumber(x), sig = Math.abs(toNumber(arg([s], 0, 1))); if (isErr(n)) return n; if (isErr(sig)) return sig;
    if (sig === 0) return 0;
    const toward = n < 0 && toNumber(arg([mode], 0, 0)) !== 0;
    return roundTo((toward ? Math.ceil(roundTo(n / sig, 9)) : Math.floor(roundTo(n / sig, 9))) * sig, 12);
  }));
  define("EVEN", num1((n) => { const v = Math.ceil(Math.abs(n) / 2) * 2; return n < 0 ? -v : v; }));
  define("ODD", num1((n) => { let v = Math.ceil(Math.abs(n)); if (v % 2 === 0) v += 1; return n < 0 ? -v : v; }));
  define("MOD", num2((n, d) => (d === 0 ? ERRORS["#DIV/0!"] : roundTo(n - d * Math.floor(n / d), 12))));
  define("QUOTIENT", num2((n, d) => (d === 0 ? ERRORS["#DIV/0!"] : Math.trunc(n / d))));
  define("POWER", num2((a, b) => (a === 0 && b === 0 ? NaN : Math.pow(a, b))));
  define("FACT", num1((n) => (n < 0 ? NaN : factorial(Math.floor(n)))));
  define("COMBIN", num2((n, k) => { n = Math.floor(n); k = Math.floor(k); if (k < 0 || n < k) return NaN; return Math.round(factorial(n) / (factorial(k) * factorial(n - k))); }));
  define("PERMUT", num2((n, k) => { n = Math.floor(n); k = Math.floor(k); if (k < 0 || n < k) return NaN; return Math.round(factorial(n) / factorial(n - k)); }));
  define("GCD", withNumbers((list) => list.reduce((a, b) => gcd(Math.floor(a), Math.floor(b)), 0)));
  define("LCM", withNumbers((list) => list.reduce((a, b) => { a = Math.floor(a); b = Math.floor(b); return a === 0 || b === 0 ? 0 : Math.abs(a * b) / gcd(a, b); }, 1)));
  define("SUMPRODUCT", (args, ctx, c) => {
    const mats = args.map((a) => c.matrix(a));
    const rows = mats[0].length, cols = mats[0][0]?.length || 0;
    if (mats.some((m) => m.length !== rows || (m[0]?.length || 0) !== cols)) return ERRORS["#VALUE!"];
    let total = 0;
    for (let i = 0; i < rows; i++) for (let j = 0; j < cols; j++) {
      let p = 1;
      for (const m of mats) { const v = m[i][j]; if (isErr(v)) return v; p *= typeof v === "number" ? v : typeof v === "boolean" && mats.length === 1 ? 0 : 0; }
      total += p;
    }
    return total;
  });
  define("SUMX2MY2", (args, ctx, c) => { const p = pairNumbers(args[0], args[1], c); return isErr(p) ? p : sum(p[0].map((x, i) => x * x - p[1][i] * p[1][i])); });
  define("SUMX2PY2", (args, ctx, c) => { const p = pairNumbers(args[0], args[1], c); return isErr(p) ? p : sum(p[0].map((x, i) => x * x + p[1][i] * p[1][i])); });
  define("SUMXMY2", (args, ctx, c) => { const p = pairNumbers(args[0], args[1], c); return isErr(p) ? p : sum(p[0].map((x, i) => (x - p[1][i]) ** 2)); });
  define("MMULT", (args, ctx, c) => {
    const a = c.matrix(args[0]), b = c.matrix(args[1]);
    if (!a.length || !b.length || a[0].length !== b.length) return ERRORS["#VALUE!"];
    const out = [];
    for (let i = 0; i < a.length; i++) {
      const row = [];
      for (let j = 0; j < b[0].length; j++) {
        let s = 0;
        for (let k = 0; k < b.length; k++) { const x = a[i][k], y = b[k][j]; if (typeof x !== "number" || typeof y !== "number") return ERRORS["#VALUE!"]; s += x * y; }
        row.push(s);
      }
      out.push(row);
    }
    return out;
  });
  const SUBTOTALS = { 1: "AVERAGE", 2: "COUNT", 3: "COUNTA", 4: "MAX", 5: "MIN", 6: "PRODUCT", 7: "STDEV", 8: "STDEVP", 9: "SUM", 10: "VAR", 11: "VARP" };
  define("SUBTOTAL", (args, ctx, c) => {
    const code = toNumber(c.scalar(args[0], ctx));
    if (isErr(code)) return code;
    const name = SUBTOTALS[Math.trunc(code) % 100];
    if (!name) return ERRORS["#VALUE!"];
    const skipManual = code > 100;
    const values = args.slice(1).map((a) => {
      if (!(a instanceof Range)) return a;
      const hidden = a.sheet.filterRows || new Set();
      const manual = a.sheet.hiddenRows || new Set();
      const b = c.clampRange(a);
      const rows = [];
      for (let r = b.r1; r <= b.r2; r++) {
        if (hidden.has(r) || (skipManual && manual.has(r))) continue;
        const row = [];
        for (let col = b.c1; col <= b.c2; col++) {
          const cell = a.sheet.cells.get(cellKey(r, col));
          row.push(cell && cell.f && /^\s*SUBTOTAL\s*\(/i.test(cell.f) ? null : c.cellValue(a.sheet, r, col));
        }
        rows.push(row);
      }
      return rows.length ? rows : [[null]];
    });
    return FUNCTIONS[name].f(values, ctx, c);
  });
  function conditional(args, c, pairsStart, targetIndex) {
    const pairs = [];
    for (let i = pairsStart; i + 1 < args.length; i += 2) {
      if (!(args[i] instanceof Range) && !Array.isArray(args[i])) return ERRORS["#VALUE!"];
      pairs.push([args[i], criterion(c.scalar(args[i + 1]))]);
    }
    const first = pairs[0][0];
    const shape = first instanceof Range ? c.clampRange(first) : { r1: 0, c1: 0, r2: first.length - 1, c2: first[0].length - 1 };
    const rows = shape.r2 - shape.r1 + 1, cols = shape.c2 - shape.c1 + 1;
    const read = (source, i, j) => {
      if (source instanceof Range) return c.cellValue(source.sheet, source.r1 + i, source.c1 + j);
      return source[i]?.[j] ?? null;
    };
    const target = targetIndex === null ? null : args[targetIndex];
    const out = [];
    for (let i = 0; i < rows; i++) for (let j = 0; j < cols; j++) {
      if (pairs.every(([range, test]) => test(read(range, i, j)))) out.push(target === null ? 1 : read(target, i, j));
    }
    return out;
  }
  const numericOnly = (list) => { const e = list.find(isErr); return e || list.filter((v) => typeof v === "number"); };
  define("SUMIF", (args, ctx, c) => { const v = numericOnly(conditional([args[0], args[1], arg(args, 2, args[0])], c, 0, 2)); return isErr(v) ? v : sum(v); });
  define("SUMIFS", (args, ctx, c) => { const v = numericOnly(conditional(args, c, 1, 0)); return isErr(v) ? v : sum(v); });
  define("COUNTIF", (args, ctx, c) => conditional([args[0], args[1]], c, 0, null).length);
  define("COUNTIFS", (args, ctx, c) => conditional(args, c, 0, null).length);
  define("AVERAGEIF", (args, ctx, c) => { const v = numericOnly(conditional([args[0], args[1], arg(args, 2, args[0])], c, 0, 2)); return isErr(v) ? v : mean(v); });
  define("AVERAGEIFS", (args, ctx, c) => { const v = numericOnly(conditional(args, c, 1, 0)); return isErr(v) ? v : mean(v); });
  define("MAXIFS", (args, ctx, c) => { const v = numericOnly(conditional(args, c, 1, 0)); return isErr(v) ? v : v.length ? Math.max(...v) : 0; });
  define("MINIFS", (args, ctx, c) => { const v = numericOnly(conditional(args, c, 1, 0)); return isErr(v) ? v : v.length ? Math.min(...v) : 0; });

  const lazyValue = (node, ctx, c) => (node ? c.ev(node, ctx) : MISSING);
  define("IF", (nodes, ctx, c) => {
    const cond = lazyValue(nodes[0], ctx, c);
    if (isArrayLike(cond)) {
      const yes = nodes[1] ? lazyValue(nodes[1], ctx, c) : true, no = nodes[2] ? lazyValue(nodes[2], ctx, c) : false;
      return c.broadcast([cond, yes, no], (x, a, b) => { const t = toBool(x); return isErr(t) ? t : t ? (a === MISSING ? 0 : a) : (b === MISSING ? 0 : b); });
    }
    const test = toBool(c.scalar(cond, ctx));
    if (isErr(test)) return test;
    if (test) { const v = nodes.length > 1 ? lazyValue(nodes[1], ctx, c) : true; return v === MISSING ? 0 : v; }
    if (nodes.length < 3) return false;
    const v = lazyValue(nodes[2], ctx, c);
    return v === MISSING ? 0 : v;
  }, true);
  define("IFS", (nodes, ctx, c) => {
    for (let i = 0; i + 1 < nodes.length; i += 2) {
      const t = toBool(c.scalar(lazyValue(nodes[i], ctx, c), ctx));
      if (isErr(t)) return t;
      if (t) return lazyValue(nodes[i + 1], ctx, c);
    }
    return ERRORS["#N/A"];
  }, true);
  define("IFERROR", (nodes, ctx, c) => {
    const v = lazyValue(nodes[0], ctx, c);
    if (isArrayLike(v)) { const alt = lazyValue(nodes[1], ctx, c); return c.broadcast([v, alt], (x, a) => (isErr(x) ? (a === MISSING ? 0 : a) : x)); }
    const s = c.scalar(v, ctx);
    if (!isErr(s)) return v;
    const alt = nodes[1] ? lazyValue(nodes[1], ctx, c) : "";
    return alt === MISSING ? "" : alt;
  }, true);
  define("IFNA", (nodes, ctx, c) => {
    const v = lazyValue(nodes[0], ctx, c);
    const s = isArrayLike(v) ? null : c.scalar(v, ctx);
    if (s !== ERRORS["#N/A"]) return v;
    const alt = lazyValue(nodes[1], ctx, c);
    return alt === MISSING ? "" : alt;
  }, true);
  function booleans(args, c) {
    const out = [];
    for (const a of args) {
      if (a === MISSING) continue;
      if (a instanceof Range || Array.isArray(a)) {
        let failure = null;
        c.each(a, (v) => { if (isErr(v)) { failure = v; return false; } if (typeof v === "boolean") out.push(v); else if (typeof v === "number") out.push(v !== 0); return true; });
        if (failure) return failure;
      } else { const b = toBool(a); if (isErr(b)) return b; out.push(b); }
    }
    return out.length ? out : ERRORS["#VALUE!"];
  }
  define("AND", (args, ctx, c) => { const b = booleans(args, c); return isErr(b) ? b : b.every(Boolean); });
  define("OR", (args, ctx, c) => { const b = booleans(args, c); return isErr(b) ? b : b.some(Boolean); });
  define("XOR", (args, ctx, c) => { const b = booleans(args, c); return isErr(b) ? b : b.filter(Boolean).length % 2 === 1; });
  define("NOT", lift(([x]) => { const b = toBool(x); return isErr(b) ? b : !b; }));
  define("TRUE", () => true);
  define("FALSE", () => false);
  define("SWITCH", (nodes, ctx, c) => {
    const value = c.scalar(lazyValue(nodes[0], ctx, c), ctx);
    if (isErr(value)) return value;
    let i = 1;
    for (; i + 1 < nodes.length; i += 2) {
      const candidate = c.scalar(lazyValue(nodes[i], ctx, c), ctx);
      if (compareValues(value, candidate) === 0 && typeof value === typeof candidate) return lazyValue(nodes[i + 1], ctx, c);
    }
    return i < nodes.length ? lazyValue(nodes[i], ctx, c) : ERRORS["#N/A"];
  }, true);
  define("CHOOSE", (nodes, ctx, c) => {
    const index = toNumber(c.scalar(lazyValue(nodes[0], ctx, c), ctx));
    if (isErr(index)) return index;
    const i = Math.trunc(index);
    if (i < 1 || i >= nodes.length) return ERRORS["#VALUE!"];
    return lazyValue(nodes[i], ctx, c);
  }, true);
  define("LET", (nodes, ctx, c) => {
    if (nodes.length < 3 || nodes.length % 2 === 0) return ERRORS["#VALUE!"];
    const locals = new Map(ctx.locals || []);
    const inner = { ...ctx, locals };
    for (let i = 0; i + 1 < nodes.length; i += 2) {
      if (nodes[i].t !== "name") return ERRORS["#VALUE!"];
      locals.set(nodes[i].v.toUpperCase(), c.ev(nodes[i + 1], inner));
    }
    return c.ev(nodes[nodes.length - 1], inner);
  }, true);

  function lookupEquals(needle, value) {
    if (typeof needle === "string" && typeof value === "string") {
      if (/[*?~]/.test(needle)) return wildcardRegex(needle).test(value);
      return needle.toLowerCase() === value.toLowerCase();
    }
    if (typeof needle === "number" && typeof value === "number") return Math.abs(needle - value) < 1e-12 * Math.max(1, Math.abs(needle));
    return needle === value;
  }
  function approximateIndex(list, needle, descending = false) {
    let lo = 0, hi = list.length - 1, found = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const v = list[mid];
      if (blank(v)) { hi = mid - 1; continue; }
      const cmp = compareValues(v, needle);
      if (descending ? cmp >= 0 : cmp <= 0) { if (typeof v === typeof needle || descending) found = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    return found;
  }
  define("VLOOKUP", (args, ctx, c) => {
    const needle = c.scalar(args[0], ctx); if (isErr(needle)) return needle;
    const table = c.matrix(args[1]);
    const col = toNumber(c.scalar(args[2], ctx)); if (isErr(col)) return col;
    const approx = toBool(c.scalar(arg(args, 3, true), ctx));
    if (col < 1 || col > (table[0]?.length || 0)) return col < 1 ? ERRORS["#VALUE!"] : ERRORS["#REF!"];
    const keys = table.map((row) => row[0]);
    const index = approx === true ? approximateIndex(keys, needle) : keys.findIndex((v) => lookupEquals(needle, v));
    return index < 0 ? ERRORS["#N/A"] : table[index][Math.trunc(col) - 1];
  });
  define("HLOOKUP", (args, ctx, c) => {
    const needle = c.scalar(args[0], ctx); if (isErr(needle)) return needle;
    const table = c.matrix(args[1]);
    const row = toNumber(c.scalar(args[2], ctx)); if (isErr(row)) return row;
    const approx = toBool(c.scalar(arg(args, 3, true), ctx));
    if (row < 1 || row > table.length) return row < 1 ? ERRORS["#VALUE!"] : ERRORS["#REF!"];
    const keys = table[0] || [];
    const index = approx === true ? approximateIndex(keys, needle) : keys.findIndex((v) => lookupEquals(needle, v));
    return index < 0 ? ERRORS["#N/A"] : table[Math.trunc(row) - 1][index];
  });
  define("LOOKUP", (args, ctx, c) => {
    const needle = c.scalar(args[0], ctx); if (isErr(needle)) return needle;
    const m = c.matrix(args[1]);
    let keys, results;
    if (args.length > 2) { keys = vector(args[1], c); results = vector(args[2], c); }
    else if (m.length >= (m[0]?.length || 0)) { keys = m.map((row) => row[0]); results = m.map((row) => row[row.length - 1]); }
    else { keys = m[0]; results = m[m.length - 1]; }
    if (!keys || !results) return ERRORS["#N/A"];
    const index = approximateIndex(keys, needle);
    return index < 0 || index >= results.length ? ERRORS["#N/A"] : results[index];
  });
  define("MATCH", (args, ctx, c) => {
    const needle = c.scalar(args[0], ctx); if (isErr(needle)) return needle;
    const list = vector(args[1], c); if (!list) return ERRORS["#N/A"];
    const type = toNumber(c.scalar(arg(args, 2, 1), ctx));
    let index;
    if (type === 0) index = list.findIndex((v) => lookupEquals(needle, v));
    else if (type > 0) index = approximateIndex(list, needle);
    else { index = -1; for (let i = 0; i < list.length; i++) { if (compareValues(list[i], needle) >= 0) index = i; else break; } }
    return index < 0 ? ERRORS["#N/A"] : index + 1;
  });
  function xmatchIndex(list, needle, mode, search) {
    const order = search < 0 ? list.map((_, i) => list.length - 1 - i) : list.map((_, i) => i);
    if (mode === 2 || mode === 0) {
      for (const i of order) if (mode === 2 ? lookupEquals(needle, list[i]) : (typeof needle === "string" && typeof list[i] === "string" ? needle.toLowerCase() === list[i].toLowerCase() : lookupEquals(needle, list[i]))) return i;
      return -1;
    }
    let best = -1;
    for (const i of order) {
      const v = list[i];
      if (blank(v) || typeof v !== typeof needle) continue;
      const cmp = compareValues(v, needle);
      if (cmp === 0) return i;
      if (mode < 0 && cmp < 0 && (best < 0 || compareValues(v, list[best]) > 0)) best = i;
      if (mode > 0 && cmp > 0 && (best < 0 || compareValues(v, list[best]) < 0)) best = i;
    }
    return best;
  }
  define("XLOOKUP", (args, ctx, c) => {
    const needle = c.scalar(args[0], ctx); if (isErr(needle)) return needle;
    const keys = vector(args[1], c); if (!keys) return ERRORS["#VALUE!"];
    const ret = c.matrix(args[2]);
    const mode = toNumber(c.scalar(arg(args, 4, 0), ctx)), search = toNumber(c.scalar(arg(args, 5, 1), ctx));
    const index = xmatchIndex(keys, needle, mode, search);
    if (index < 0) return args.length > 3 && args[3] !== MISSING ? args[3] : ERRORS["#N/A"];
    const vertical = c.matrix(args[1]).length > 1 || keys.length === 1 && ret.length > 1;
    if (vertical) return ret[index] ? (ret[index].length === 1 ? ret[index][0] : [ret[index]]) : ERRORS["#REF!"];
    return ret.length === 1 ? ret[0][index] : ret.map((row) => [row[index]]);
  });
  define("XMATCH", (args, ctx, c) => {
    const needle = c.scalar(args[0], ctx); if (isErr(needle)) return needle;
    const list = vector(args[1], c); if (!list) return ERRORS["#VALUE!"];
    const index = xmatchIndex(list, needle, toNumber(c.scalar(arg(args, 2, 0), ctx)), toNumber(c.scalar(arg(args, 3, 1), ctx)));
    return index < 0 ? ERRORS["#N/A"] : index + 1;
  });
  define("INDEX", (args, ctx, c) => {
    const source = args[0];
    let row = toNumber(c.scalar(arg(args, 1, 0), ctx)), col = toNumber(c.scalar(arg(args, 2, 0), ctx));
    if (isErr(row)) return row;
    if (isErr(col)) return col;
    row = Math.trunc(row); col = Math.trunc(col);
    if (source instanceof Range) {
      if (args.length < 3 && source.rows === 1) { col = row; row = 1; }
      else if (args.length < 3 && source.cols === 1) col = 1;
      if (row < 0 || col < 0 || row > source.rows || col > source.cols) return ERRORS["#REF!"];
      if (row === 0 && col === 0) return source;
      if (row === 0) return new Range(source.sheet, source.r1, source.c1 + col - 1, source.r2, source.c1 + col - 1);
      if (col === 0) return new Range(source.sheet, source.r1 + row - 1, source.c1, source.r1 + row - 1, source.c2);
      return new Range(source.sheet, source.r1 + row - 1, source.c1 + col - 1, source.r1 + row - 1, source.c1 + col - 1);
    }
    const m = c.matrix(source);
    if (args.length < 3 && m.length === 1) { col = row; row = 1; }
    else if (args.length < 3 && m[0].length === 1) col = 1;
    if (row < 0 || col < 0 || row > m.length || col > m[0].length) return ERRORS["#REF!"];
    if (row === 0) return m.map((r) => [r[col - 1]]);
    if (col === 0) return [m[row - 1]];
    return m[row - 1][col - 1];
  });
  define("OFFSET", (args, ctx, c) => {
    const base = args[0];
    if (!(base instanceof Range)) return ERRORS["#VALUE!"];
    const n = [arg(args, 1, 0), arg(args, 2, 0), arg(args, 3, base.rows), arg(args, 4, base.cols)].map((v) => toNumber(c.scalar(v, ctx)));
    if (firstError(n)) return firstError(n);
    const r1 = base.r1 + Math.trunc(n[0]), c1 = base.c1 + Math.trunc(n[1]);
    const h = Math.trunc(n[2]), w = Math.trunc(n[3]);
    if (h < 1 || w < 1 || r1 < 0 || c1 < 0 || r1 + h > MAX_ROWS || c1 + w > MAX_COLS) return ERRORS["#REF!"];
    return new Range(base.sheet, r1, c1, r1 + h - 1, c1 + w - 1);
  });
  define("INDIRECT", (args, ctx, c) => {
    const text = toText(c.scalar(args[0], ctx));
    if (isErr(text)) return text;
    const a1 = toBool(c.scalar(arg(args, 1, true), ctx));
    let source = text.trim();
    let sheet = ctx.sheet;
    const bang = source.lastIndexOf("!");
    if (bang > 0) { sheet = c.sheetFor(source.slice(0, bang).replace(/^'|'$/g, "").replace(/''/g, "'"), ctx); source = source.slice(bang + 1); }
    if (!sheet) return ERRORS["#REF!"];
    let range = null;
    if (a1 === false) {
      const m = /^R(\d+)C(\d+)(?::R(\d+)C(\d+))?$/i.exec(source);
      if (m) range = { r1: +m[1] - 1, c1: +m[2] - 1, r2: +(m[3] || m[1]) - 1, c2: +(m[4] || m[2]) - 1 };
    } else range = parseRange(source);
    if (!range) {
      const named = (c.wb.names || new Map()).get(source.toUpperCase());
      if (named) return c.name({ v: source }, ctx);
      return ERRORS["#REF!"];
    }
    return new Range(sheet, range.r1, range.c1, range.r2, range.c2);
  });
  define("ROW", (args, ctx) => {
    const a = arg(args, 0, null);
    if (a === null) return ctx.r + 1;
    if (!(a instanceof Range)) return ERRORS["#VALUE!"];
    return a.rows === 1 ? a.r1 + 1 : Array.from({ length: Math.min(a.rows, 100000) }, (_, i) => [a.r1 + i + 1]);
  });
  define("COLUMN", (args, ctx) => {
    const a = arg(args, 0, null);
    if (a === null) return ctx.c + 1;
    if (!(a instanceof Range)) return ERRORS["#VALUE!"];
    return a.cols === 1 ? a.c1 + 1 : [Array.from({ length: a.cols }, (_, i) => a.c1 + i + 1)];
  });
  define("ROWS", (args, ctx, c) => (args[0] instanceof Range ? args[0].rows : c.matrix(args[0]).length));
  define("COLUMNS", (args, ctx, c) => (args[0] instanceof Range ? args[0].cols : c.matrix(args[0])[0]?.length || 0));
  define("AREAS", () => 1);
  define("ADDRESS", (args, ctx, c) => {
    const n = [args[0], args[1], arg(args, 2, 1)].map((v) => toNumber(c.scalar(v, ctx)));
    if (firstError(n)) return firstError(n);
    const [row, col, abs] = n.map(Math.trunc);
    if (row < 1 || col < 1 || row > MAX_ROWS || col > MAX_COLS) return ERRORS["#VALUE!"];
    const a1 = toBool(c.scalar(arg(args, 3, true), ctx));
    const sheet = arg(args, 4, null);
    let text;
    if (a1 === false) text = `R${abs === 1 || abs === 2 ? row : `[${row}]`}C${abs === 1 || abs === 3 ? col : `[${col}]`}`;
    else text = `${abs === 1 || abs === 3 ? "$" : ""}${colName(col - 1)}${abs === 1 || abs === 2 ? "$" : ""}${row}`;
    return sheet === null ? text : quoteSheet(toText(c.scalar(sheet, ctx))) + "!" + text;
  });
  define("TRANSPOSE", (args, ctx, c) => { const m = c.matrix(args[0]); return m[0].map((_, j) => m.map((row) => (blank(row[j]) ? 0 : row[j]))); });
  define("HYPERLINK", (args, ctx, c) => { const label = arg(args, 1, args[0]); return c.scalar(label, ctx); });
  define("FORMULATEXT", (args) => { const a = args[0]; if (!(a instanceof Range)) return ERRORS["#N/A"]; const cell = a.sheet.cells.get(cellKey(a.r1, a.c1)); return cell && cell.f !== undefined ? "=" + cell.f : ERRORS["#N/A"]; });
  define("ISFORMULA", (args) => { const a = args[0]; if (!(a instanceof Range)) return false; const cell = a.sheet.cells.get(cellKey(a.r1, a.c1)); return !!(cell && cell.f !== undefined); });
  define("ISREF", (args) => args[0] instanceof Range);

  define("SEQUENCE", (args, ctx, c) => {
    const n = [args[0], arg(args, 1, 1), arg(args, 2, 1), arg(args, 3, 1)].map((v) => toNumber(c.scalar(v, ctx)));
    if (firstError(n)) return firstError(n);
    const rows = Math.trunc(n[0]), cols = Math.trunc(n[1]);
    if (rows < 1 || cols < 1 || rows * cols > 1000000) return ERRORS["#VALUE!"];
    return Array.from({ length: rows }, (_, i) => Array.from({ length: cols }, (_, j) => n[2] + (i * cols + j) * n[3]));
  });
  define("SORT", (args, ctx, c) => {
    const m = c.matrix(args[0]).map((row) => [...row]);
    const index = Math.trunc(toNumber(c.scalar(arg(args, 1, 1), ctx))) - 1;
    const order = toNumber(c.scalar(arg(args, 2, 1), ctx)) < 0 ? -1 : 1;
    const byCol = toBool(c.scalar(arg(args, 3, false), ctx)) === true;
    if (byCol) {
      const t = m[0].map((_, j) => m.map((row) => row[j]));
      t.sort((a, b) => compareValues(a[index], b[index]) * order);
      return t[0].map((_, j) => t.map((row) => row[j]));
    }
    if (index < 0 || index >= (m[0]?.length || 0)) return ERRORS["#VALUE!"];
    return m.sort((a, b) => compareValues(a[index], b[index]) * order);
  });
  define("SORTBY", (args, ctx, c) => {
    const m = c.matrix(args[0]);
    const keys = [];
    for (let i = 1; i < args.length; i += 2) keys.push([vector(args[i], c), toNumber(c.scalar(arg(args, i + 1, 1), ctx)) < 0 ? -1 : 1]);
    const order = m.map((_, i) => i);
    order.sort((a, b) => { for (const [k, dir] of keys) { const cmp = compareValues(k?.[a], k?.[b]); if (cmp) return cmp * dir; } return 0; });
    return order.map((i) => m[i]);
  });
  define("FILTER", (args, ctx, c) => {
    const m = c.matrix(args[0]);
    const include = c.matrix(args[1]);
    let out;
    if (include.length === m.length && include[0].length === 1) out = m.filter((_, i) => toBool(include[i][0]) === true);
    else if (include.length === 1 && include[0].length === m[0].length) {
      const keep = include[0].map((v) => toBool(v) === true);
      out = m.map((row) => row.filter((_, j) => keep[j]));
      if (!out[0].length) out = [];
    } else return ERRORS["#VALUE!"];
    if (!out.length) return args.length > 2 ? args[2] : ERRORS["#CALC!"];
    return out;
  });
  define("UNIQUE", (args, ctx, c) => {
    const m = c.matrix(args[0]);
    const byCol = toBool(c.scalar(arg(args, 1, false), ctx)) === true;
    const once = toBool(c.scalar(arg(args, 2, false), ctx)) === true;
    const items = byCol ? m[0].map((_, j) => m.map((row) => row[j])) : m;
    const keyOf = (row) => JSON.stringify(row.map((v) => (typeof v === "string" ? "s:" + v.toLowerCase() : isErr(v) ? "e:" + v.code : v)));
    const counts = new Map();
    for (const row of items) { const k = keyOf(row); const entry = counts.get(k); if (entry) entry.n++; else counts.set(k, { row, n: 1 }); }
    const rows = [...counts.values()].filter((entry) => !once || entry.n === 1).map((entry) => entry.row);
    if (!rows.length) return ERRORS["#CALC!"];
    return byCol ? rows[0].map((_, i) => rows.map((row) => row[i])) : rows;
  });

  define("CONCATENATE", lift((values) => { const e = firstError(values); if (e) return e; return values.map(toText).join(""); }));
  define("CONCAT", (args, ctx, c) => {
    let out = "", failure = null;
    for (const a of args) c.each(a, (v) => { if (isErr(v)) { failure = v; return false; } out += toText(v); return true; });
    return failure || out;
  });
  define("TEXTJOIN", (args, ctx, c) => {
    const delim = toText(c.scalar(args[0], ctx)), skip = toBool(c.scalar(args[1], ctx));
    const parts = [];
    let failure = null;
    for (const a of args.slice(2)) c.each(a, (v) => { if (isErr(v)) { failure = v; return false; } const s = toText(v); if (!(skip && s === "")) parts.push(s); return true; });
    return failure || parts.join(delim);
  });
  define("JOIN", (args, ctx, c) => {
    const delim = toText(c.scalar(args[0], ctx));
    const parts = [];
    for (const a of args.slice(1)) c.each(a, (v) => { parts.push(toText(v)); });
    return parts.join(delim);
  });
  define("SPLIT", (args, ctx, c) => {
    const text = toText(c.scalar(args[0], ctx)), delim = toText(c.scalar(args[1], ctx));
    const each = toBool(c.scalar(arg(args, 2, true), ctx)) !== false, removeEmpty = toBool(c.scalar(arg(args, 3, true), ctx)) !== false;
    if (isErr(text)) return text;
    const pattern = each ? new RegExp("[" + delim.replace(/[\]\\^-]/g, "\\$&") + "]") : delim;
    let parts = delim ? text.split(pattern) : [text];
    if (removeEmpty) parts = parts.filter((p) => p !== "");
    return [parts.length ? parts.map((p) => textToNumber(p) ?? p) : [""]];
  });
  define("LEFT", lift(([x, n]) => { const s = toText(x); if (isErr(s)) return s; const k = toNumber(arg([n], 0, 1)); if (isErr(k)) return k; return k < 0 ? ERRORS["#VALUE!"] : s.slice(0, Math.trunc(k)); }));
  define("RIGHT", lift(([x, n]) => { const s = toText(x); if (isErr(s)) return s; const k = toNumber(arg([n], 0, 1)); if (isErr(k)) return k; return k < 0 ? ERRORS["#VALUE!"] : k === 0 ? "" : s.slice(-Math.trunc(k)); }));
  define("MID", lift(([x, start, n]) => { const s = toText(x); if (isErr(s)) return s; const a = toNumber(start), k = toNumber(n); if (isErr(a)) return a; if (isErr(k)) return k; if (a < 1 || k < 0) return ERRORS["#VALUE!"]; return s.substr(Math.trunc(a) - 1, Math.trunc(k)); }));
  define("LEN", text1((s) => s.length));
  define("LOWER", text1((s) => s.toLowerCase()));
  define("UPPER", text1((s) => s.toUpperCase()));
  define("PROPER", text1((s) => s.toLowerCase().replace(/(^|[^\p{L}'])(\p{L})/gu, (m, a, b) => a + b.toUpperCase())));
  define("TRIM", text1((s) => s.replace(/ +/g, " ").trim()));
  define("CLEAN", text1((s) => s.replace(/[\x00-\x1F]/g, "")));
  define("REPT", lift(([x, n]) => { const s = toText(x); if (isErr(s)) return s; const k = toNumber(n); if (isErr(k)) return k; return k < 0 || s.length * k > 32767 ? ERRORS["#VALUE!"] : s.repeat(Math.trunc(k)); }));
  define("EXACT", lift(([a, b]) => { const x = toText(a), y = toText(b); return firstError([x, y]) || x === y; }));
  define("SUBSTITUTE", lift(([x, from, to, which]) => {
    const s = toText(x), f = toText(from), t = toText(to);
    const e = firstError([s, f, t]); if (e) return e;
    if (!f) return s;
    if (which === undefined || which === MISSING) return s.split(f).join(t);
    const n = Math.trunc(toNumber(which));
    if (!(n >= 1)) return ERRORS["#VALUE!"];
    let index = -1;
    for (let i = 0; i < n; i++) { index = s.indexOf(f, index + 1); if (index < 0) return s; }
    return s.slice(0, index) + t + s.slice(index + f.length);
  }));
  define("REPLACE", lift(([x, start, n, to]) => { const s = toText(x), t = toText(to); const a = toNumber(start), k = toNumber(n); const e = firstError([s, t, a, k]); if (e) return e; if (a < 1 || k < 0) return ERRORS["#VALUE!"]; return s.slice(0, a - 1) + t + s.slice(a - 1 + k); }));
  define("FIND", lift(([needle, hay, start]) => { const n = toText(needle), h = toText(hay); const s = toNumber(arg([start], 0, 1)); const e = firstError([n, h, s]); if (e) return e; const i = h.indexOf(n, Math.trunc(s) - 1); return s < 1 || i < 0 ? ERRORS["#VALUE!"] : i + 1; }));
  define("SEARCH", lift(([needle, hay, start]) => {
    const n = toText(needle), h = toText(hay); const s = toNumber(arg([start], 0, 1)); const e = firstError([n, h, s]); if (e) return e;
    if (s < 1) return ERRORS["#VALUE!"];
    const source = wildcardRegex(n).source.slice(1, -1);
    const m = new RegExp(source, "i").exec(h.slice(Math.trunc(s) - 1));
    return m ? m.index + Math.trunc(s) : ERRORS["#VALUE!"];
  }));
  define("TEXTBEFORE", lift(([x, d, n]) => { const s = toText(x), delim = toText(d); const e = firstError([s, delim]); if (e) return e; const k = Math.trunc(toNumber(arg([n], 0, 1))); const parts = s.split(delim); if (parts.length - 1 < Math.abs(k)) return ERRORS["#N/A"]; return k > 0 ? parts.slice(0, k).join(delim) : parts.slice(0, parts.length + k).join(delim); }));
  define("TEXTAFTER", lift(([x, d, n]) => { const s = toText(x), delim = toText(d); const e = firstError([s, delim]); if (e) return e; const k = Math.trunc(toNumber(arg([n], 0, 1))); const parts = s.split(delim); if (parts.length - 1 < Math.abs(k)) return ERRORS["#N/A"]; return k > 0 ? parts.slice(k).join(delim) : parts.slice(parts.length + k).join(delim); }));
  define("CHAR UNICHAR", num1((n) => (n < 1 || n > 0x10FFFF ? NaN : String.fromCodePoint(Math.trunc(n)))));
  define("CODE UNICODE", text1((s) => (s ? s.codePointAt(0) : ERRORS["#VALUE!"])));
  define("T", lift(([x]) => (typeof x === "string" ? x : isErr(x) ? x : "")));
  define("N", lift(([x]) => (typeof x === "number" ? x : typeof x === "boolean" ? (x ? 1 : 0) : isErr(x) ? x : 0)));
  define("VALUE", lift(([x]) => { if (typeof x === "number") return x; if (blank(x)) return 0; if (isErr(x)) return x; const n = textToNumber(toText(x)); return n === null ? ERRORS["#VALUE!"] : n; }));
  define("NUMBERVALUE", lift(([x, dec, group]) => {
    if (typeof x === "number") return x;
    let s = toText(x); if (isErr(s)) return s;
    const d = dec === undefined || dec === MISSING ? "." : toText(dec), g = group === undefined || group === MISSING ? "," : toText(group);
    s = s.split(g).join("").split(d).join(".").replace(/\s+/g, "");
    const n = textToNumber(s);
    return n === null ? ERRORS["#VALUE!"] : n;
  }));
  define("TEXT", lift(([x, f]) => {
    const fmt = toText(f); if (isErr(fmt)) return fmt;
    if (isErr(x)) return x;
    const n = typeof x === "number" ? x : typeof x === "string" && textToNumber(x) !== null ? textToNumber(x) : x;
    return formatValue(blank(n) ? 0 : n, fmt).text;
  }));
  define("FIXED", lift(([x, d, noCommas]) => { const n = toNumber(x); if (isErr(n)) return n; const k = Math.trunc(toNumber(arg([d], 0, 2))); const decimals = Math.max(0, k); const rounded = roundTo(n, k); return formatValue(rounded, (toBool(arg([noCommas], 0, false)) === true ? "0" : "#,##0") + (decimals ? "." + "0".repeat(decimals) : "")).text; }));
  define("DOLLAR", lift(([x, d]) => { const n = toNumber(x); if (isErr(n)) return n; const k = Math.max(0, Math.trunc(toNumber(arg([d], 0, 2)))); const body = "#,##0" + (k ? "." + "0".repeat(k) : ""); return formatValue(n, `"$"${body};("$"${body})`).text; }));

  const today = () => Math.floor(serialFromJsDate(new Date()));
  define("TODAY", () => today());
  define("NOW", () => serialFromJsDate(new Date()));
  define("DATE", lift(([y, m, d]) => { const n = [y, m, d].map(toNumber); if (firstError(n)) return firstError(n); const s = serialFromDate(n[0], n[1], n[2]); return s < 0 ? ERRORS["#NUM!"] : s; }));
  define("TIME", lift(([h, m, s]) => { const n = [h, m, s].map(toNumber); if (firstError(n)) return firstError(n); const t = (Math.trunc(n[0]) * 3600 + Math.trunc(n[1]) * 60 + Math.trunc(n[2])) / 86400; return t < 0 ? ERRORS["#NUM!"] : t - Math.floor(t); }));
  const datePart = (fn) => lift(([x]) => { const n = typeof x === "string" ? (parseDateText(x)?.serial ?? textToNumber(x)) : toNumber(x); if (n === null) return ERRORS["#VALUE!"]; if (isErr(n)) return n; if (n < 0) return ERRORS["#NUM!"]; return fn(dateFromSerial(n), n); });
  define("YEAR", datePart((p) => p.y)); define("MONTH", datePart((p) => p.m)); define("DAY", datePart((p) => p.d));
  define("HOUR", datePart((p) => p.H)); define("MINUTE", datePart((p) => p.M)); define("SECOND", datePart((p) => p.S + (p.ms >= 500 ? 1 : 0)));
  define("WEEKDAY", lift(([x, t]) => {
    const n = toNumber(x); if (isErr(n)) return n;
    const type = Math.trunc(toNumber(arg([t], 0, 1)));
    const wd = dateFromSerial(n).wd;
    if (type === 1 || type === 17) return wd + 1;
    if (type === 2 || type === 11) return ((wd + 6) % 7) + 1;
    if (type === 3) return (wd + 6) % 7;
    if (type >= 12 && type <= 16) return ((wd - (type - 10) + 7) % 7) + 1;
    return ERRORS["#NUM!"];
  }));
  function isoWeek(serial) {
    const p = dateFromSerial(serial);
    const date = new Date(Date.UTC(p.y, p.m - 1, p.d));
    const day = (date.getUTCDay() + 6) % 7;
    date.setUTCDate(date.getUTCDate() - day + 3);
    const firstThursday = new Date(Date.UTC(date.getUTCFullYear(), 0, 4));
    return 1 + Math.round(((date - firstThursday) / DAY_MS - 3 + ((firstThursday.getUTCDay() + 6) % 7)) / 7);
  }
  define("ISOWEEKNUM", num1((n) => isoWeek(n)));
  define("WEEKNUM", lift(([x, t]) => {
    const n = toNumber(x); if (isErr(n)) return n;
    const type = Math.trunc(toNumber(arg([t], 0, 1)));
    if (type === 21) return isoWeek(n);
    const startDay = type === 1 || type === 17 ? 0 : type === 2 || type === 11 ? 1 : type >= 12 && type <= 16 ? type - 10 : -1;
    if (startDay < 0) return ERRORS["#NUM!"];
    const p = dateFromSerial(n);
    const jan1 = serialFromDate(p.y, 1, 1);
    const offset = (dateFromSerial(jan1).wd - startDay + 7) % 7;
    return Math.floor((Math.floor(n) - jan1 + offset) / 7) + 1;
  }));
  function addMonths(serial, months, endOfMonth) {
    const p = dateFromSerial(serial);
    const total = p.y * 12 + (p.m - 1) + Math.trunc(months);
    const y = Math.floor(total / 12), m = (total % 12) + 1;
    const last = new Date(Date.UTC(y, m, 0)).getUTCDate();
    return serialFromDate(y, m, endOfMonth ? last : Math.min(p.d, last));
  }
  define("EDATE", num2((s, m) => addMonths(s, m, false)));
  define("EOMONTH", num2((s, m) => addMonths(s, m, true)));
  define("DAYS", num2((end, start) => Math.floor(end) - Math.floor(start)));
  define("DAYS360", lift(([a, b, method]) => {
    const s = toNumber(a), e = toNumber(b); if (isErr(s)) return s; if (isErr(e)) return e;
    const p = dateFromSerial(s), q = dateFromSerial(e);
    let d1 = p.d, d2 = q.d;
    if (toBool(arg([method], 0, false)) === true) { if (d1 === 31) d1 = 30; if (d2 === 31) d2 = 30; }
    else { if (d1 === 31) d1 = 30; if (d2 === 31 && d1 >= 30) d2 = 30; }
    return (q.y - p.y) * 360 + (q.m - p.m) * 30 + (d2 - d1);
  }));
  define("DATEDIF", lift(([a, b, u]) => {
    const s = toNumber(a), e = toNumber(b), unit = toText(u).toUpperCase();
    const fail = firstError([s, e, unit]); if (fail) return fail;
    if (e < s) return ERRORS["#NUM!"];
    const p = dateFromSerial(s), q = dateFromSerial(e);
    let months = (q.y - p.y) * 12 + (q.m - p.m);
    if (q.d < p.d) months -= 1;
    switch (unit) {
      case "D": return Math.floor(e) - Math.floor(s);
      case "M": return months;
      case "Y": return Math.floor(months / 12);
      case "YM": return months % 12;
      case "MD": { let d = q.d - p.d; if (d < 0) d += new Date(Date.UTC(q.y, q.m - 1, 0)).getUTCDate(); return d; }
      case "YD": { const anniversary = serialFromDate(q.y - (q.m < p.m || (q.m === p.m && q.d < p.d) ? 1 : 0), p.m, p.d); return Math.floor(e) - anniversary; }
      default: return ERRORS["#NUM!"];
    }
  }));
  function holidaySet(arg0, c) { const set = new Set(); if (arg0 !== undefined && arg0 !== MISSING) c.each(arg0, (v) => { if (typeof v === "number") set.add(Math.floor(v)); }); return set; }
  define("NETWORKDAYS", (args, ctx, c) => {
    const s = toNumber(c.scalar(args[0], ctx)), e = toNumber(c.scalar(args[1], ctx)); if (isErr(s)) return s; if (isErr(e)) return e;
    const holidays = holidaySet(args[2], c);
    const a = Math.floor(Math.min(s, e)), b = Math.floor(Math.max(s, e));
    let n = 0;
    for (let d = a; d <= b; d++) { const wd = dateFromSerial(d).wd; if (wd !== 0 && wd !== 6 && !holidays.has(d)) n++; }
    return s > e ? -n : n;
  });
  define("WORKDAY", (args, ctx, c) => {
    const s = toNumber(c.scalar(args[0], ctx)), k = toNumber(c.scalar(args[1], ctx)); if (isErr(s)) return s; if (isErr(k)) return k;
    const holidays = holidaySet(args[2], c);
    let d = Math.floor(s), left = Math.abs(Math.trunc(k));
    const step = k < 0 ? -1 : 1;
    while (left > 0) { d += step; const wd = dateFromSerial(d).wd; if (wd !== 0 && wd !== 6 && !holidays.has(d)) left--; }
    return d;
  });
  define("YEARFRAC", lift(([a, b, basisArg]) => {
    let s = toNumber(a), e = toNumber(b); if (isErr(s)) return s; if (isErr(e)) return e;
    if (s > e) [s, e] = [e, s];
    const basis = Math.trunc(toNumber(arg([basisArg], 0, 0)));
    const p = dateFromSerial(s), q = dateFromSerial(e);
    if (basis === 0 || basis === 4) {
      let d1 = p.d, d2 = q.d;
      if (basis === 0) { if (d1 === 31) d1 = 30; if (d2 === 31 && d1 >= 30) d2 = 30; }
      else { if (d1 === 31) d1 = 30; if (d2 === 31) d2 = 30; }
      return ((q.y - p.y) * 360 + (q.m - p.m) * 30 + (d2 - d1)) / 360;
    }
    const days = Math.floor(e) - Math.floor(s);
    if (basis === 2) return days / 360;
    if (basis === 3) return days / 365;
    let yearDays = 0;
    for (let y = p.y; y <= q.y; y++) yearDays += (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 366 : 365;
    return days / (yearDays / (q.y - p.y + 1));
  }));
  define("DATEVALUE", text1((s) => { const d = parseDateText(s.trim()); return d ? Math.floor(d.serial) : ERRORS["#VALUE!"]; }));
  define("TIMEVALUE", text1((s) => { const t = parseTimeParts(s.trim()); if (t) return t.serial % 1; const d = parseDateText(s.trim()); return d ? d.serial % 1 : ERRORS["#VALUE!"]; }));

  const info = (test) => (args, ctx, c) => {
    const a = args[0];
    if (isArrayLike(a)) return c.broadcast([a], (v) => test(v));
    return test(a instanceof Range ? c.scalar(a, ctx) : a);
  };
  define("ISBLANK", info((v) => blank(v)));
  define("ISNUMBER", info((v) => typeof v === "number"));
  define("ISTEXT", info((v) => typeof v === "string"));
  define("ISNONTEXT", info((v) => typeof v !== "string"));
  define("ISLOGICAL", info((v) => typeof v === "boolean"));
  define("ISERROR", info((v) => isErr(v)));
  define("ISERR", info((v) => isErr(v) && v.code !== "#N/A"));
  define("ISNA", info((v) => v === ERRORS["#N/A"]));
  define("ISEVEN", info((v) => { const n = toNumber(v); return isErr(n) ? n : Math.trunc(n) % 2 === 0; }));
  define("ISODD", info((v) => { const n = toNumber(v); return isErr(n) ? n : Math.abs(Math.trunc(n)) % 2 === 1; }));
  define("NA", () => ERRORS["#N/A"]);
  define("TYPE", (args, ctx, c) => { const a = args[0]; if (Array.isArray(a) || (a instanceof Range && isArrayLike(a))) return 64; const v = c.scalar(a, ctx); return typeof v === "number" || blank(v) ? 1 : typeof v === "string" ? 2 : typeof v === "boolean" ? 4 : isErr(v) ? 16 : 1; });
  define("ERROR.TYPE", info((v) => (isErr(v) ? ({ "#NULL!": 1, "#DIV/0!": 2, "#VALUE!": 3, "#REF!": 4, "#NAME?": 5, "#NUM!": 6, "#N/A": 7, "#SPILL!": 9, "#CALC!": 14 }[v.code] || ERRORS["#N/A"]) : ERRORS["#N/A"])));

  function finance(fn, count) {
    return lift((values) => {
      const n = Array.from({ length: count.required + count.defaults.length }, (_, i) => (i >= count.required && (values[i] === undefined || values[i] === MISSING) ? count.defaults[i - count.required] : toNumber(values[i])));
      return firstError(n) || fn(...n);
    });
  }
  const fvOf = (r, n, pmt, pv, type) => (r === 0 ? -(pv + pmt * n) : -(pv * Math.pow(1 + r, n) + (pmt * (1 + r * type) * (Math.pow(1 + r, n) - 1)) / r));
  const pmtOf = (r, n, pv, fv, type) => (r === 0 ? -(pv + fv) / n : (-r * (fv + pv * Math.pow(1 + r, n))) / ((1 + r * type) * (Math.pow(1 + r, n) - 1)));
  function ipmtOf(r, per, n, pv, fv, type) {
    if (per < 1 || per > n) return ERRORS["#NUM!"];
    const pmt = pmtOf(r, n, pv, fv, type);
    if (per === 1 && type === 1) return 0;
    let interest = fvOf(r, per - 1, pmt, pv, type) * r;
    if (type === 1) interest /= 1 + r;
    return interest;
  }
  define("PMT", finance((r, n, pv, fv, type) => (n === 0 ? ERRORS["#NUM!"] : pmtOf(r, n, pv, fv, type ? 1 : 0)), { required: 3, defaults: [0, 0] }));
  define("FV", finance((r, n, pmt, pv, type) => fvOf(r, n, pmt, pv, type ? 1 : 0), { required: 3, defaults: [0, 0] }));
  define("PV", finance((r, n, pmt, fv, type) => (r === 0 ? -(fv + pmt * n) : -(fv + (pmt * (1 + r * (type ? 1 : 0)) * (Math.pow(1 + r, n) - 1)) / r) / Math.pow(1 + r, n)), { required: 3, defaults: [0, 0] }));
  define("NPER", finance((r, pmt, pv, fv, type) => {
    const t = type ? 1 : 0;
    if (r === 0) return pmt === 0 ? ERRORS["#NUM!"] : -(pv + fv) / pmt;
    const num = pmt * (1 + r * t) - fv * r, den = pmt * (1 + r * t) + pv * r;
    if (num / den <= 0) return ERRORS["#NUM!"];
    return Math.log(num / den) / Math.log(1 + r);
  }, { required: 3, defaults: [0, 0] }));
  define("IPMT", finance((r, per, n, pv, fv, type) => ipmtOf(r, per, n, pv, fv, type ? 1 : 0), { required: 4, defaults: [0, 0] }));
  define("PPMT", finance((r, per, n, pv, fv, type) => { const i = ipmtOf(r, per, n, pv, fv, type ? 1 : 0); return isErr(i) ? i : pmtOf(r, n, pv, fv, type ? 1 : 0) - i; }, { required: 4, defaults: [0, 0] }));
  define("RATE", finance((n, pmt, pv, fv, type, guess) => {
    const t = type ? 1 : 0;
    let r = guess;
    for (let i = 0; i < 100; i++) {
      const f = r === 0 ? pv + pmt * n + fv : pv * Math.pow(1 + r, n) + (pmt * (1 + r * t) * (Math.pow(1 + r, n) - 1)) / r + fv;
      const h = 1e-7;
      const r2 = r + h;
      const f2 = pv * Math.pow(1 + r2, n) + (pmt * (1 + r2 * t) * (Math.pow(1 + r2, n) - 1)) / r2 + fv;
      const d = (f2 - f) / h;
      if (!Number.isFinite(d) || d === 0) break;
      const next = r - f / d;
      if (Math.abs(next - r) < 1e-10) return next;
      r = next;
    }
    return ERRORS["#NUM!"];
  }, { required: 3, defaults: [0, 0, 0.1] }));
  define("NPV", (args, ctx, c) => {
    const r = toNumber(c.scalar(args[0], ctx)); if (isErr(r)) return r;
    const list = numbers(args.slice(1), c); if (isErr(list)) return list;
    return list.reduce((total, v, i) => total + v / Math.pow(1 + r, i + 1), 0);
  });
  define("IRR", (args, ctx, c) => {
    const list = numbers([args[0]], c); if (isErr(list)) return list;
    let r = toNumber(c.scalar(arg(args, 1, 0.1), ctx));
    for (let i = 0; i < 200; i++) {
      let f = 0, d = 0;
      list.forEach((v, k) => { f += v / Math.pow(1 + r, k); d -= (k * v) / Math.pow(1 + r, k + 1); });
      if (d === 0) break;
      const next = r - f / d;
      if (!Number.isFinite(next)) break;
      if (Math.abs(next - r) < 1e-10) return next;
      r = next;
    }
    return ERRORS["#NUM!"];
  });
  define("SLN", finance((cost, salvage, life) => (life === 0 ? ERRORS["#DIV/0!"] : (cost - salvage) / life), { required: 3, defaults: [] }));
  define("SYD", finance((cost, salvage, life, per) => (life <= 0 || per < 1 || per > life ? ERRORS["#NUM!"] : ((cost - salvage) * (life - per + 1) * 2) / (life * (life + 1))), { required: 4, defaults: [] }));

  const FUNCTION_NAMES = Object.keys(FUNCTIONS).sort();
  const FUTURE_FUNCTIONS = new Set(["IFS", "SWITCH", "XLOOKUP", "XMATCH", "CONCAT", "TEXTJOIN", "MAXIFS", "MINIFS", "IFNA", "UNIQUE", "SEQUENCE", "SORTBY", "STDEV.S", "STDEV.P", "VAR.S", "VAR.P", "MODE.SNGL", "PERCENTILE.INC", "QUARTILE.INC", "RANK.EQ", "CEILING.MATH", "FLOOR.MATH", "DAYS", "ISOWEEKNUM", "XOR", "TEXTBEFORE", "TEXTAFTER", "LET", "NORM.DIST", "NORM.S.DIST", "NORM.INV", "NORM.S.INV", "COVARIANCE.P", "COVARIANCE.S", "FORECAST.LINEAR", "NUMBERVALUE", "UNICHAR", "UNICODE", "FORMULATEXT", "ISFORMULA"]);
  const WORKSHEET_FUNCTIONS = new Set(["FILTER", "SORT"]);
  function excelFormulaText(text) {
    return text.replace(/("(?:[^"]|"")*")|\b([A-Z][A-Z0-9.]*)\s*\(/g, (m, quoted, name) => {
      if (quoted) return quoted;
      if (WORKSHEET_FUNCTIONS.has(name)) return `_xlfn._xlws.${m}`;
      if (FUTURE_FUNCTIONS.has(name)) return `_xlfn.${m}`;
      return m;
    });
  }
  let idCounter = 0;
  const uid = () => Date.now().toString(36) + (idCounter++).toString(36) + Math.random().toString(36).slice(2, 6);
  const DEFAULT_COL_WIDTH = 100;
  const DEFAULT_ROW_HEIGHT = 21;

  function createSheet(name, options = {}) {
    return {
      id: uid(), name, cells: new Map(),
      rowCount: options.rowCount || 1000, colCount: options.colCount || 26,
      colWidths: new Map(), rowHeights: new Map(), hiddenRows: new Set(), hiddenCols: new Set(),
      defaultColWidth: options.defaultColWidth || DEFAULT_COL_WIDTH, defaultRowHeight: options.defaultRowHeight || DEFAULT_ROW_HEIGHT,
      merges: [], freezeRows: 0, freezeCols: 0, color: "", showGrid: true, hidden: false,
      filter: null, filterRows: new Set(), conditional: [], validations: [], notes: new Map(), links: new Map(), charts: [],
    };
  }
  function createWorkbook() {
    return { sheets: [createSheet("Sheet1")], styles: [{}], styleIndex: new Map([["{}", 0]]), names: new Map(), active: 0, defaultFont: "Arial", defaultSize: 10 };
  }
  function uniqueSheetName(wb, base) {
    const taken = new Set(wb.sheets.map((sheet) => sheet.name.toLowerCase()));
    if (!taken.has(base.toLowerCase())) return base;
    const stem = base.replace(/\s*\(\d+\)$/, "").replace(/\d+$/, "") || "Sheet";
    for (let i = 2; ; i++) { const name = /\d$/.test(stem) ? `${stem} (${i})` : `${stem}${i}`; if (!taken.has(name.toLowerCase())) return name; }
  }
  function validSheetName(name) {
    const value = String(name || "").trim();
    return value.length > 0 && value.length <= 100 && !/[\\/?*[\]:]/.test(value) && !/^'|'$/.test(value);
  }

  const BORDER_KEYS = ["bt", "bb", "bl", "br"];
  function cleanStyle(style) {
    const out = {};
    for (const key of Object.keys(style || {}).sort()) {
      const v = style[key];
      if (v === undefined || v === null || v === false || v === "") continue;
      if (key === "fmt" && v === "General") continue;
      if (BORDER_KEYS.includes(key)) { if (v && v.w) out[key] = { w: String(v.w), c: String(v.c || "#000000") }; continue; }
      out[key] = v;
    }
    return out;
  }
  function styleKey(style) { return JSON.stringify(style); }
  function internStyle(wb, style) {
    const clean = cleanStyle(style);
    const key = styleKey(clean);
    let id = wb.styleIndex.get(key);
    if (id === undefined) { id = wb.styles.length; wb.styles.push(clean); wb.styleIndex.set(key, id); }
    return id;
  }
  const styleOf = (wb, cell) => (cell && cell.s ? wb.styles[cell.s] || {} : {});
  function makeCell(v, f, s) {
    const cell = {};
    if (f !== undefined && f !== null) cell.f = f;
    if (v !== undefined && v !== null && !(v === "" && cell.f === undefined)) cell.v = v;
    if (s) cell.s = s;
    return cell.f !== undefined || cell.v !== undefined || cell.s ? cell : null;
  }

  function cloneContainer(value) {
    if (value instanceof Map) return new Map(value);
    if (value instanceof Set) return new Set(value);
    if (Array.isArray(value)) return value.slice();
    return value;
  }
  class Transaction {
    constructor(wb, label) {
      this.wb = wb; this.label = label || "";
      this.cells = new Map(); this.props = new Map(); this.wbProps = new Map();
    }
    setCell(sheet, k, cell) {
      let store = this.cells.get(sheet);
      if (!store) { store = new Map(); this.cells.set(sheet, store); }
      if (!store.has(k)) store.set(k, sheet.cells.get(k) || null);
      if (cell) sheet.cells.set(k, cell); else sheet.cells.delete(k);
    }
    touch(sheet, prop) {
      let store = this.props.get(sheet);
      if (!store) { store = new Map(); this.props.set(sheet, store); }
      if (!store.has(prop)) store.set(prop, cloneContainer(sheet[prop]));
    }
    set(sheet, prop, value) { this.touch(sheet, prop); sheet[prop] = value; }
    touchWorkbook(prop) { if (!this.wbProps.has(prop)) this.wbProps.set(prop, cloneContainer(this.wb[prop])); }
    finish() {
      const cells = [], props = [], wbProps = [];
      for (const [sheet, store] of this.cells) for (const [k, before] of store) {
        const after = sheet.cells.get(k) || null;
        if (before !== after) cells.push({ sheet, k, before, after });
      }
      for (const [sheet, store] of this.props) for (const [prop, before] of store) props.push({ sheet, prop, before, after: cloneContainer(sheet[prop]) });
      for (const [prop, before] of this.wbProps) wbProps.push({ prop, before, after: cloneContainer(this.wb[prop]) });
      return cells.length || props.length || wbProps.length ? { label: this.label, cells, props, wbProps } : null;
    }
  }
  function applyHistory(wb, entry, direction) {
    const side = direction === "undo" ? "before" : "after";
    const list = direction === "undo" ? [...entry.cells].reverse() : entry.cells;
    for (const change of list) { const cell = change[side]; if (cell) change.sheet.cells.set(change.k, cell); else change.sheet.cells.delete(change.k); }
    for (const change of entry.props) change.sheet[change.prop] = cloneContainer(change[side]);
    for (const change of entry.wbProps) wb[change.prop] = cloneContainer(change[side]);
  }

  function remapKeyed(map, axis, at, count) {
    const out = new Map();
    for (const [k, v] of map) {
      const r = keyRow(k), c = keyCol(k);
      if (axis === "row") { const nr = adjustIndex(r, at, count); if (nr !== null && nr < MAX_ROWS) out.set(cellKey(nr, c), v); }
      else { const nc = adjustIndex(c, at, count); if (nc !== null && nc < MAX_COLS) out.set(cellKey(r, nc), v); }
    }
    return out;
  }
  function remapIndexMap(map, at, count, max) {
    const out = new Map();
    for (const [i, v] of map) { const n = adjustIndex(i, at, count); if (n !== null && n < max) out.set(n, v); }
    return out;
  }
  function remapIndexSet(set, at, count, max) {
    const out = new Set();
    for (const i of set) { const n = adjustIndex(i, at, count); if (n !== null && n < max) out.add(n); }
    return out;
  }
  function adjustRange(range, axis, at, count) {
    if (axis === "row") {
      if (range.r1 === 0 && range.r2 === MAX_ROWS - 1) return range;
      const span = adjustSpan(range.r1, range.r2, at, count, MAX_ROWS);
      return span ? { ...range, r1: span[0], r2: span[1] } : null;
    }
    if (range.c1 === 0 && range.c2 === MAX_COLS - 1) return range;
    const span = adjustSpan(range.c1, range.c2, at, count, MAX_COLS);
    return span ? { ...range, c1: span[0], c2: span[1] } : null;
  }
  function adjustRangeText(text, axis, at, count) {
    const range = parseRange(text);
    if (!range) return text;
    const next = adjustRange(range, axis, at, count);
    return next ? rangeText(next) : null;
  }
  function rewriteAllFormulas(tx, wb, mapperFor) {
    for (const sheet of wb.sheets) {
      const mapper = mapperFor(sheet);
      for (const [k, cell] of [...sheet.cells]) {
        if (cell.f === undefined) continue;
        const next = rewriteFormula(cell.f, mapper);
        if (next !== cell.f) tx.setCell(sheet, k, { ...cell, f: next });
      }
      if (sheet.conditional.some((rule) => rule.formula)) {
        tx.touch(sheet, "conditional");
        sheet.conditional = sheet.conditional.map((rule) => (rule.formula ? { ...rule, formula: rewriteFormula(rule.formula, mapper) } : rule));
      }
      if (sheet.validations.some((rule) => rule.source)) {
        tx.touch(sheet, "validations");
        sheet.validations = sheet.validations.map((rule) => (rule.source ? { ...rule, source: rewriteFormula(rule.source, mapper) } : rule));
      }
      if (sheet.charts.length) {
        tx.touch(sheet, "charts");
        sheet.charts = sheet.charts.map((chart) => ({ ...chart, range: rewriteFormula(chart.range, mapper) }));
      }
    }
    if (wb.names.size) {
      tx.touchWorkbook("names");
      const next = new Map();
      for (const [key, def] of wb.names) {
        const sheet = wb.sheets.find((s) => s.name === def.sheet) || wb.sheets[0];
        next.set(key, { ...def, ref: rewriteFormula(def.ref, mapperFor(sheet)) });
      }
      wb.names = next;
    }
  }
  function shiftAxis(tx, wb, sheet, axis, at, count) {
    const moved = [];
    for (const [k, cell] of sheet.cells) {
      const index = axis === "row" ? keyRow(k) : keyCol(k);
      if (index >= at) moved.push([k, cell]);
    }
    for (const [k] of moved) tx.setCell(sheet, k, null);
    for (const [k, cell] of moved) {
      const r = keyRow(k), c = keyCol(k);
      if (axis === "row") { const nr = adjustIndex(r, at, count); if (nr !== null && nr < MAX_ROWS) tx.setCell(sheet, cellKey(nr, c), cell); }
      else { const nc = adjustIndex(c, at, count); if (nc !== null && nc < MAX_COLS) tx.setCell(sheet, cellKey(r, nc), cell); }
    }
    rewriteAllFormulas(tx, wb, (formulaSheet) => structuralMapper(formulaSheet.name, sheet.name, axis, at, count));
    if (axis === "row") {
      tx.set(sheet, "rowHeights", remapIndexMap(sheet.rowHeights, at, count, MAX_ROWS));
      tx.set(sheet, "hiddenRows", remapIndexSet(sheet.hiddenRows, at, count, MAX_ROWS));
      tx.set(sheet, "rowCount", Math.max(1, Math.min(MAX_ROWS, sheet.rowCount + count)));
      if (count < 0 && sheet.freezeRows > at) tx.set(sheet, "freezeRows", Math.max(at, sheet.freezeRows + count));
      else if (count > 0 && sheet.freezeRows > at) tx.set(sheet, "freezeRows", sheet.freezeRows + count);
    } else {
      tx.set(sheet, "colWidths", remapIndexMap(sheet.colWidths, at, count, MAX_COLS));
      tx.set(sheet, "hiddenCols", remapIndexSet(sheet.hiddenCols, at, count, MAX_COLS));
      tx.set(sheet, "colCount", Math.max(1, Math.min(MAX_COLS, sheet.colCount + count)));
      if (count < 0 && sheet.freezeCols > at) tx.set(sheet, "freezeCols", Math.max(at, sheet.freezeCols + count));
      else if (count > 0 && sheet.freezeCols > at) tx.set(sheet, "freezeCols", sheet.freezeCols + count);
    }
    tx.set(sheet, "notes", remapKeyed(sheet.notes, axis, at, count));
    tx.set(sheet, "links", remapKeyed(sheet.links, axis, at, count));
    tx.set(sheet, "merges", sheet.merges.map((m) => adjustRange(m, axis, at, count)).filter((m) => m && (m.r1 !== m.r2 || m.c1 !== m.c2)));
    tx.set(sheet, "conditional", sheet.conditional.map((rule) => { const range = adjustRangeText(rule.range, axis, at, count); return range ? { ...rule, range } : null; }).filter(Boolean));
    tx.set(sheet, "validations", sheet.validations.map((rule) => { const range = adjustRangeText(rule.range, axis, at, count); return range ? { ...rule, range } : null; }).filter(Boolean));
    if (sheet.filter) {
      const range = adjustRange(sheet.filter.range, axis, at, count);
      let criteria = sheet.filter.criteria;
      if (axis === "col" && range) {
        criteria = new Map();
        for (const [col, rule] of sheet.filter.criteria) { const n = adjustIndex(col, at, count); if (n !== null) criteria.set(n, rule); }
      }
      tx.set(sheet, "filter", range ? { range, criteria } : null);
    }
    tx.set(sheet, "charts", sheet.charts.map((chart) => {
      const anchor = { ...chart.anchor };
      if (axis === "row") { const n = adjustIndex(anchor.r, at, count); anchor.r = n === null ? at : n; }
      else { const n = adjustIndex(anchor.c, at, count); anchor.c = n === null ? at : n; }
      return { ...chart, anchor };
    }));
  }
  const insertRows = (tx, wb, sheet, at, n) => shiftAxis(tx, wb, sheet, "row", at, n);
  const deleteRows = (tx, wb, sheet, at, n) => shiftAxis(tx, wb, sheet, "row", at, -n);
  const insertCols = (tx, wb, sheet, at, n) => shiftAxis(tx, wb, sheet, "col", at, n);
  const deleteCols = (tx, wb, sheet, at, n) => shiftAxis(tx, wb, sheet, "col", at, -n);

  function renameSheet(tx, wb, sheet, name) {
    const old = sheet.name;
    const target = old.toLowerCase();
    rewriteAllFormulas(tx, wb, () => (node) => ((node.t === "ref" || node.t === "rng" || node.t === "name") && node.sheet && node.sheet.toLowerCase() === target ? { ...node, sheet: name } : node));
    if (wb.names.size) {
      tx.touchWorkbook("names");
      wb.names = new Map([...wb.names].map(([key, def]) => [key, def.sheet === old ? { ...def, sheet: name } : def]));
    }
    tx.set(sheet, "name", name);
  }
  function removeSheet(tx, wb, sheet) {
    const target = sheet.name.toLowerCase();
    tx.touchWorkbook("sheets");
    wb.sheets = wb.sheets.filter((s) => s !== sheet);
    rewriteAllFormulas(tx, wb, (formulaSheet) => (node) => ((node.t === "ref" || node.t === "rng") && node.sheet && node.sheet.toLowerCase() === target ? REF_ERROR : node));
  }
  function cloneSheet(wb, sheet, name) {
    const copy = createSheet(name);
    for (const key of Object.keys(sheet)) {
      if (key === "id" || key === "name") continue;
      const value = sheet[key];
      copy[key] = value instanceof Map ? new Map(value) : value instanceof Set ? new Set(value) : Array.isArray(value) ? value.map((item) => (item && typeof item === "object" ? { ...item, id: item.id ? uid() : item.id } : item)) : value && typeof value === "object" ? { ...value } : value;
    }
    const source = sheet.name.toLowerCase();
    for (const [k, cell] of copy.cells) {
      if (cell.f === undefined) continue;
      const next = rewriteFormula(cell.f, (node) => ((node.t === "ref" || node.t === "rng") && node.sheet && node.sheet.toLowerCase() === source ? { ...node, sheet: name } : node));
      if (next !== cell.f) copy.cells.set(k, { ...cell, f: next });
    }
    return copy;
  }
  function mergeAt(sheet, r, c) {
    for (const m of sheet.merges) if (inRange(m, r, c)) return m;
    return null;
  }
  function expandToMerges(sheet, range) {
    let out = { ...range }, changed = true;
    while (changed) {
      changed = false;
      for (const m of sheet.merges) {
        if (rangesOverlap(m, out) && !(m.r1 >= out.r1 && m.r2 <= out.r2 && m.c1 >= out.c1 && m.c2 <= out.c2)) {
          out = { r1: Math.min(out.r1, m.r1), c1: Math.min(out.c1, m.c1), r2: Math.max(out.r2, m.r2), c2: Math.max(out.c2, m.c2) };
          changed = true;
        }
      }
    }
    return out;
  }
  function usedRange(sheet) {
    let r1 = Infinity, c1 = Infinity, r2 = -1, c2 = -1;
    for (const [k, cell] of sheet.cells) {
      if (cell.v === undefined && cell.f === undefined) continue;
      const r = keyRow(k), c = keyCol(k);
      if (r < r1) r1 = r; if (r > r2) r2 = r; if (c < c1) c1 = c; if (c > c2) c2 = c;
    }
    return r2 < 0 ? null : { r1, c1, r2, c2 };
  }
  function extentOf(sheet) {
    let r2 = -1, c2 = -1;
    for (const k of sheet.cells.keys()) { const r = keyRow(k), c = keyCol(k); if (r > r2) r2 = r; if (c > c2) c2 = c; }
    for (const m of sheet.merges) { if (m.r2 > r2) r2 = m.r2; if (m.c2 > c2) c2 = m.c2; }
    return { r2, c2 };
  }
  function dataRegion(sheet, calc, r, c) {
    const filled = (rr, cc) => rr >= 0 && cc >= 0 && !blank(calc.cellValue(sheet, rr, cc)) && calc.cellValue(sheet, rr, cc) !== "";
    let range = { r1: r, c1: c, r2: r, c2: c };
    for (let changed = true, guard = 0; changed && guard < 200; guard++) {
      changed = false;
      const grow = (test, apply) => { if (test()) { apply(); changed = true; } };
      const anyRow = (rr) => { for (let cc = range.c1 - 1; cc <= range.c2 + 1; cc++) if (filled(rr, cc)) return true; return false; };
      const anyCol = (cc) => { for (let rr = range.r1 - 1; rr <= range.r2 + 1; rr++) if (filled(rr, cc)) return true; return false; };
      grow(() => range.r1 > 0 && anyRow(range.r1 - 1), () => range.r1--);
      grow(() => anyRow(range.r2 + 1), () => range.r2++);
      grow(() => range.c1 > 0 && anyCol(range.c1 - 1), () => range.c1--);
      grow(() => anyCol(range.c2 + 1), () => range.c2++);
    }
    return range;
  }

  const SERIES_LISTS = [
    MONTHS, MONTHS.map((m) => m.slice(0, 3)), DAYS, DAYS.map((d) => d.slice(0, 3)),
  ];
  function seriesList(text) {
    for (const list of SERIES_LISTS) {
      const i = list.findIndex((item) => item.toLowerCase() === String(text).toLowerCase());
      if (i >= 0) return { list, index: i };
    }
    return null;
  }
  function matchCase(sample, word) {
    if (sample === sample.toUpperCase()) return word.toUpperCase();
    if (sample === sample.toLowerCase()) return word.toLowerCase();
    return word;
  }
  function fillValues(sources, count, wb, backwards = false) {
    const n = sources.length;
    const values = sources.map((cell) => (cell && cell.f === undefined ? cell.v : undefined));
    const out = [];
    const numeric = n > 0 && values.every((v) => typeof v === "number");
    let step = null, base = null;
    if (numeric) {
      if (n === 1) {
        const fmt = styleOf(wb, sources[0]).fmt;
        step = isDateFormat(fmt) ? 1 : 0;
      } else {
        const xs = values.map((_, i) => i), mx = (n - 1) / 2, my = sum(values) / n;
        let sxx = 0, sxy = 0;
        for (let i = 0; i < n; i++) { sxx += (xs[i] - mx) ** 2; sxy += (xs[i] - mx) * (values[i] - my); }
        step = sxy / sxx; base = my - step * mx;
      }
    }
    const textNumber = values.map((v) => (typeof v === "string" ? /^(.*?)(\d+)(\D*)$/.exec(v) : null));
    const textSeries = n > 0 && textNumber.every((m) => m && m[1] === textNumber[0][1] && m[3] === textNumber[0][3]);
    const named = n > 0 && values.every((v) => typeof v === "string" && seriesList(v)) ? values.map(seriesList) : null;
    const namedOk = named && named.every((item) => item.list === named[0].list);
    for (let t = 0; t < count; t++) {
      const p = backwards ? -(t + 1) : n + t;
      const sourceIndex = ((p % n) + n) % n;
      const source = sources[sourceIndex];
      const offset = p - sourceIndex;
      if (!source) { out.push({ cell: null, offset }); continue; }
      if (source.f !== undefined) { out.push({ cell: source, offset, formula: true }); continue; }
      let v = source.v;
      if (numeric && step !== null) v = n === 1 ? values[0] + step * p : roundTo(base + step * p, 12);
      else if (textSeries) {
        const nums = textNumber.map((m) => Number(m[2]));
        const d = n === 1 ? 1 : (nums[n - 1] - nums[0]) / (n - 1);
        const value = Math.round(nums[0] + d * p);
        const width = textNumber[0][2].startsWith("0") ? textNumber[0][2].length : 0;
        v = textNumber[0][1] + String(Math.abs(value)).padStart(width, "0") + textNumber[0][3];
      } else if (namedOk) {
        const { list } = named[0];
        const d = n === 1 ? 1 : named[n - 1].index - named[n - 2].index || 1;
        const index = (((named[0].index + d * p) % list.length) + list.length) % list.length;
        v = matchCase(String(values[0]), list[index]);
      }
      out.push({ cell: { ...source, v }, offset });
    }
    return out;
  }

  function sortRows(tx, wb, calc, sheet, range, keys) {
    const rows = [];
    for (let r = range.r1; r <= range.r2; r++) {
      const cells = [];
      for (let c = range.c1; c <= range.c2; c++) cells.push(sheet.cells.get(cellKey(r, c)) || null);
      rows.push({ r, cells, keys: keys.map((key) => calc.cellValue(sheet, r, key.col)) });
    }
    rows.sort((a, b) => {
      for (let i = 0; i < keys.length; i++) {
        const x = a.keys[i], y = b.keys[i];
        const xb = blank(x) || x === "", yb = blank(y) || y === "";
        if (xb || yb) { if (xb && yb) continue; return xb ? 1 : -1; }
        const cmp = compareValues(x, y);
        if (cmp) return keys[i].desc ? -cmp : cmp;
      }
      return a.r - b.r;
    });
    rows.forEach((row, i) => {
      const target = range.r1 + i;
      row.cells.forEach((cell, j) => {
        const k = cellKey(target, range.c1 + j);
        const moved = cell && cell.f !== undefined && target !== row.r ? { ...cell, f: shiftFormula(cell.f, target - row.r, 0) } : cell;
        tx.setCell(sheet, k, moved);
      });
    });
  }

  function computeFilterRows(calc, wb, sheet) {
    const hidden = new Set();
    const filter = sheet.filter;
    if (!filter || !filter.criteria || !filter.criteria.size) return hidden;
    const { range } = filter;
    for (let r = range.r1 + 1; r <= range.r2; r++) {
      for (const [col, rule] of filter.criteria) {
        const value = calc.cellValue(sheet, r, col);
        const text = displayText(wb, sheet, calc, r, col, value);
        if (rule.hidden && rule.hidden.has(text)) { hidden.add(r); break; }
        if (rule.contains && !text.toLowerCase().includes(String(rule.contains).toLowerCase())) { hidden.add(r); break; }
      }
    }
    return hidden;
  }
  function displayText(wb, sheet, calc, r, c, value = calc.cellValue(sheet, r, c)) {
    const cell = sheet.cells.get(cellKey(r, c));
    let fmt = styleOf(wb, cell).fmt;
    if (!fmt && !cell) {
      const owner = calc.spillOwner(sheet, r, c);
      if (owner !== null) fmt = styleOf(wb, sheet.cells.get(owner)).fmt;
    }
    return formatValue(value, fmt).text;
  }

  function conditionalContext(calc, wb, sheet) {
    const cache = new Map();
    const rules = sheet.conditional.map((rule) => ({ rule, range: parseRange(rule.range) })).filter((item) => item.range);
    const scaleStats = (item) => {
      let stats = cache.get(item.rule);
      if (!stats) {
        let min = Infinity, max = -Infinity;
        const b = calc.clampRange(new Range(sheet, item.range.r1, item.range.c1, item.range.r2, item.range.c2));
        for (let r = b.r1; r <= b.r2; r++) for (let c = b.c1; c <= b.c2; c++) {
          const v = calc.cellValue(sheet, r, c);
          if (typeof v === "number") { if (v < min) min = v; if (v > max) max = v; }
        }
        const counts = new Map();
        if (item.rule.type === "duplicate" || item.rule.type === "unique") {
          for (let r = b.r1; r <= b.r2; r++) for (let c = b.c1; c <= b.c2; c++) {
            const v = calc.cellValue(sheet, r, c);
            if (blank(v) || v === "") continue;
            const key = typeof v === "string" ? "s" + v.toLowerCase() : String(v);
            counts.set(key, (counts.get(key) || 0) + 1);
          }
        }
        stats = { min, max, counts };
        cache.set(item.rule, stats);
      }
      return stats;
    };
    const operand = (literal, formula, item) => {
      if (formula) return calc.evaluate(String(formula).replace(/^=/, ""), sheet, item.range.r1, item.range.c1);
      if (literal === undefined || literal === null || literal === "") return null;
      const parsed = parseInput(String(literal));
      return parsed.formula !== undefined ? calc.evaluate(parsed.formula, sheet, item.range.r1, item.range.c1) : parsed.value;
    };
    return (r, c) => {
      let overlay = null;
      for (const item of rules) {
        if (!inRange(item.range, r, c)) continue;
        const rule = item.rule;
        const value = calc.cellValue(sheet, r, c);
        let hit = false;
        if (rule.type === "scale") {
          if (typeof value !== "number") continue;
          const { min, max } = scaleStats(item);
          const t = max === min ? 0.5 : (value - min) / (max - min);
          const colors = rule.colors && rule.colors.length >= 2 ? rule.colors : ["#F8696B", "#FFEB84", "#63BE7B"];
          const fill = colors.length === 3 ? (t < 0.5 ? mixColors(colors[0], colors[1], t * 2) : mixColors(colors[1], colors[2], (t - 0.5) * 2)) : mixColors(colors[0], colors[1], t);
          overlay = { ...(overlay || {}), fill };
          continue;
        }
        if (rule.type === "cell") {
          if (blank(value) || value === "") continue;
          const a = operand(rule.value, rule.valueFormula, item), b = operand(rule.value2, rule.value2Formula, item);
          if (a === null || isErr(a)) continue;
          const x = compareValues(value, a), y = b === null || isErr(b) ? null : compareValues(value, b);
          const lo = y === null ? null : compareValues(a, b) <= 0 ? [x, y] : [y, x];
          hit = { gt: x > 0, lt: x < 0, ge: x >= 0, le: x <= 0, eq: x === 0, ne: x !== 0, between: !!lo && lo[0] >= 0 && lo[1] <= 0, notbetween: !!lo && (lo[0] < 0 || lo[1] > 0) }[rule.op] || false;
        } else if (rule.type === "text") {
          const text = valueToText(value).toLowerCase(), needle = String(rule.text || "").toLowerCase();
          hit = { contains: text.includes(needle), notcontains: !text.includes(needle), begins: text.startsWith(needle), ends: text.endsWith(needle), equals: text === needle }[rule.op || "contains"];
        } else if (rule.type === "blank") hit = blank(value) || value === "";
        else if (rule.type === "notblank") hit = !(blank(value) || value === "");
        else if (rule.type === "error") hit = isErr(value);
        else if (rule.type === "duplicate" || rule.type === "unique") {
          if (blank(value) || value === "") continue;
          const key = typeof value === "string" ? "s" + value.toLowerCase() : String(value);
          const n = scaleStats(item).counts.get(key) || 0;
          hit = rule.type === "duplicate" ? n > 1 : n === 1;
        } else if (rule.type === "formula" && rule.formula) {
          const text = shiftFormula(String(rule.formula).replace(/^=/, ""), r - item.range.r1, c - item.range.c1);
          const v = calc.evaluate(text, sheet, r, c);
          hit = toBool(v) === true;
        }
        if (hit) overlay = { ...(overlay || {}), ...rule.style };
        if (hit && rule.stop) break;
      }
      return overlay;
    };
  }
  function parseHex(color) {
    const m = /^#?([0-9a-f]{6})$/i.exec(String(color || ""));
    if (!m) return null;
    const n = parseInt(m[1], 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  function toHex(rgb) { return "#" + rgb.map((v) => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, "0")).join("").toUpperCase(); }
  function mixColors(a, b, t) {
    const x = parseHex(a) || [255, 255, 255], y = parseHex(b) || [255, 255, 255];
    return toHex(x.map((v, i) => v + (y[i] - v) * Math.max(0, Math.min(1, t))));
  }
  function luminance(color) { const rgb = parseHex(color); if (!rgb) return 1; return (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]) / 255; }

  function validationAt(sheet, r, c) {
    for (const rule of sheet.validations) { const range = parseRange(rule.range); if (range && inRange(range, r, c)) return rule; }
    return null;
  }
  function validationOptions(calc, sheet, rule) {
    if (rule.type !== "list") return [];
    if (rule.source) {
      const v = calc.evaluate(rule.source.replace(/^=/, ""), sheet, 0, 0);
      const list = [];
      const m = Array.isArray(v) ? v : [[v]];
      for (const row of m) for (const item of row) if (!blank(item) && item !== "") list.push(valueToText(item));
      return [...new Set(list)];
    }
    return (rule.values || []).map(String);
  }
  function validateValue(calc, sheet, rule, value) {
    if (!rule || blank(value) || value === "") return true;
    switch (rule.type) {
      case "list": return validationOptions(calc, sheet, rule).some((option) => option.toLowerCase() === valueToText(value).toLowerCase());
      case "checkbox": return typeof value === "boolean";
      case "number": case "date": {
        if (typeof value !== "number") return false;
        const min = rule.min === "" || rule.min === undefined ? null : Number(rule.min);
        const max = rule.max === "" || rule.max === undefined ? null : Number(rule.max);
        const op = rule.op || "between";
        if (op === "between") return (min === null || value >= min) && (max === null || value <= max);
        if (op === "notbetween") return !((min === null || value >= min) && (max === null || value <= max));
        if (op === "gt") return value > min;
        if (op === "lt") return value < min;
        if (op === "ge") return value >= min;
        if (op === "le") return value <= min;
        if (op === "eq") return value === min;
        if (op === "ne") return value !== min;
        return true;
      }
      case "length": { const n = valueToText(value).length; const min = Number(rule.min || 0), max = rule.max === "" || rule.max === undefined ? Infinity : Number(rule.max); return n >= min && n <= max; }
      case "formula": { const v = calc.evaluate(String(rule.formula || "TRUE").replace(/^=/, ""), sheet, 0, 0); return toBool(v) === true; }
      default: return true;
    }
  }

  function chartData(calc, wb, sheet, chart) {
    const refSheet = (() => { const refs = formulaReferences(chart.range); return refs.length && refs[0].sheet ? calc.sheetFor(refs[0].sheet, { sheet }) : sheet; })();
    const ref = formulaReferences(chart.range)[0];
    if (!ref || !refSheet) return null;
    const m = calc.matrix(new Range(refSheet, ref.r1, ref.c1, Math.min(ref.r2, ref.r1 + 5000), Math.min(ref.c2, ref.c1 + 200)));
    if (!m.length) return null;
    const byRows = chart.seriesBy === "rows";
    const grid = byRows ? m[0].map((_, j) => m.map((row) => row[j])) : m;
    const header = grid[0] || [];
    const hasHeader = chart.headers !== false && header.some((v) => typeof v === "string") && grid.length > 1;
    const body = hasHeader ? grid.slice(1) : grid;
    const firstIsCategory = body.some((row) => typeof row[0] === "string" || isDateFormat(styleOf(wb, refSheet.cells.get(cellKey(ref.r1 + (hasHeader ? 1 : 0), ref.c1))).fmt)) && (grid[0]?.length || 0) > 1;
    const categories = body.map((row, i) => (firstIsCategory ? displayFor(row[0]) : String(i + 1)));
    function displayFor(v) {
      if (typeof v === "number") { const fmt = styleOf(wb, refSheet.cells.get(cellKey(ref.r1 + (hasHeader ? 1 : 0), ref.c1))).fmt; return formatValue(v, fmt).text; }
      return valueToText(v);
    }
    const series = [];
    for (let j = firstIsCategory ? 1 : 0; j < (grid[0]?.length || 0); j++) {
      series.push({
        name: hasHeader ? valueToText(header[j]) || `Series ${series.length + 1}` : `Series ${series.length + 1}`,
        values: body.map((row) => (typeof row[j] === "number" ? row[j] : textToNumber(valueToText(row[j])))),
      });
    }
    return { categories, series };
  }

  const AGGREGATES = {
    sum: (list) => sum(list), count: (list) => list.length, average: (list) => (list.length ? sum(list) / list.length : 0),
    min: (list) => (list.length ? Math.min(...list) : 0), max: (list) => (list.length ? Math.max(...list) : 0),
    counta: (list) => list.length,
  };
  function pivotTable(calc, sheet, range, options) {
    const header = [];
    for (let c = range.c1; c <= range.c2; c++) header.push(valueToText(calc.cellValue(sheet, range.r1, c)) || colName(c));
    const rowIndex = options.rowField, colIndex2 = options.colField, valueIndex = options.valueField;
    const agg = AGGREGATES[options.aggregate] || AGGREGATES.sum;
    const groups = new Map(), rowKeys = new Map(), colKeys = new Map();
    const keyOf = (v) => (typeof v === "string" ? "s" + v.toLowerCase() : "v" + valueToText(v));
    for (let r = range.r1 + 1; r <= range.r2; r++) {
      const rv = calc.cellValue(sheet, r, range.c1 + rowIndex);
      if (blank(rv) && rowIndex !== undefined) continue;
      const cv = colIndex2 === null || colIndex2 === undefined || colIndex2 < 0 ? null : calc.cellValue(sheet, r, range.c1 + colIndex2);
      const vv = calc.cellValue(sheet, r, range.c1 + valueIndex);
      const rk = keyOf(rv), ck = cv === null ? "" : keyOf(cv);
      if (!rowKeys.has(rk)) rowKeys.set(rk, rv);
      if (cv !== null && !colKeys.has(ck)) colKeys.set(ck, cv);
      const g = rk + "\u0000" + ck;
      if (!groups.has(g)) groups.set(g, []);
      if (options.aggregate === "counta" ? !(blank(vv) || vv === "") : typeof vv === "number") groups.get(g).push(typeof vv === "number" ? vv : 1);
    }
    const sortKeys = (map) => [...map.entries()].sort((a, b) => compareValues(a[1], b[1]));
    const rows = sortKeys(rowKeys), cols = colKeys.size ? sortKeys(colKeys) : [["", null]];
    const label = `${options.aggregate === "average" ? "AVERAGE" : String(options.aggregate || "sum").toUpperCase()} of ${header[valueIndex]}`;
    const out = [[header[rowIndex], ...cols.map(([, v]) => (v === null ? label : valueToText(v))), ...(colKeys.size ? ["Grand total"] : [])]];
    const totals = cols.map(() => []);
    const grand = [];
    for (const [rk, rv] of rows) {
      const line = [rv];
      const rowAll = [];
      cols.forEach(([ck], j) => {
        const list = groups.get(rk + "\u0000" + ck) || [];
        line.push(list.length ? agg(list) : null);
        totals[j].push(...list); rowAll.push(...list); grand.push(...list);
      });
      if (colKeys.size) line.push(agg(rowAll));
      out.push(line);
    }
    out.push(["Grand total", ...totals.map((list) => agg(list)), ...(colKeys.size ? [agg(grand)] : [])]);
    return out;
  }

  function serializeRange(range) { return rangeText(range); }
  function encodeValue(v) { return isErr(v) ? { e: v.code } : v; }
  function decodeValue(v) {
    if (v && typeof v === "object" && typeof v.e === "string") return err(v.e);
    if (typeof v === "number" || typeof v === "string" || typeof v === "boolean") return v;
    return undefined;
  }
  function serializeWorkbook(wb, calc) {
    return {
      format: "forkmesh-sheets", version: 1,
      styles: wb.styles, active: wb.active, defaultFont: wb.defaultFont, defaultSize: wb.defaultSize,
      names: [...wb.names.values()].map((def) => ({ name: def.name, ref: def.ref, sheet: def.sheet || "" })),
      sheets: wb.sheets.map((sheet) => {
        const cells = {};
        for (const [k, cell] of sheet.cells) {
          const out = {};
          if (cell.f !== undefined) { out.f = cell.f; const v = calc ? calc.cellValue(sheet, keyRow(k), keyCol(k)) : cell.v; if (v !== undefined && v !== null) out.v = encodeValue(v); }
          else if (cell.v !== undefined) out.v = encodeValue(cell.v);
          if (cell.s) out.s = cell.s;
          cells[addr(keyRow(k), keyCol(k))] = out;
        }
        const keyed = (map) => Object.fromEntries([...map].map(([k, v]) => [addr(keyRow(k), keyCol(k)), v]));
        return {
          name: sheet.name, color: sheet.color, hidden: sheet.hidden, rows: sheet.rowCount, cols: sheet.colCount,
          dw: sheet.defaultColWidth, dh: sheet.defaultRowHeight, grid: sheet.showGrid, freeze: [sheet.freezeRows, sheet.freezeCols],
          cw: Object.fromEntries(sheet.colWidths), rh: Object.fromEntries(sheet.rowHeights),
          hr: [...sheet.hiddenRows], hc: [...sheet.hiddenCols], merges: sheet.merges.map(serializeRange),
          filter: sheet.filter ? { range: serializeRange(sheet.filter.range), criteria: [...sheet.filter.criteria].map(([col, rule]) => ({ col, hidden: rule.hidden ? [...rule.hidden] : undefined, contains: rule.contains })) } : null,
          cf: sheet.conditional, dv: sheet.validations, notes: keyed(sheet.notes), links: keyed(sheet.links), charts: sheet.charts, cells,
        };
      }),
    };
  }
  function deserializeWorkbook(data) {
    const wb = createWorkbook();
    if (!data || typeof data !== "object" || !Array.isArray(data.sheets) || !data.sheets.length) return wb;
    wb.styles = [{}];
    wb.styleIndex = new Map([["{}", 0]]);
    const styleMap = [0];
    (Array.isArray(data.styles) ? data.styles : []).forEach((style, i) => { if (i > 0) styleMap[i] = internStyle(wb, style && typeof style === "object" ? style : {}); });
    wb.defaultFont = typeof data.defaultFont === "string" ? data.defaultFont : "Arial";
    wb.defaultSize = Number(data.defaultSize) || 10;
    wb.names = new Map();
    for (const def of Array.isArray(data.names) ? data.names : []) if (def && def.name && def.ref) wb.names.set(String(def.name).toUpperCase(), { name: String(def.name), ref: String(def.ref), sheet: def.sheet ? String(def.sheet) : "" });
    const used = new Set();
    wb.sheets = data.sheets.slice(0, 500).map((raw, index) => {
      let name = validSheetName(raw.name) ? String(raw.name).trim() : `Sheet${index + 1}`;
      while (used.has(name.toLowerCase())) name = name + " " + (index + 1);
      used.add(name.toLowerCase());
      const sheet = createSheet(name, { rowCount: clampInt(raw.rows, 1, MAX_ROWS, 1000), colCount: clampInt(raw.cols, 1, MAX_COLS, 26), defaultColWidth: clampInt(raw.dw, 8, 2000, DEFAULT_COL_WIDTH), defaultRowHeight: clampInt(raw.dh, 4, 800, DEFAULT_ROW_HEIGHT) });
      sheet.color = typeof raw.color === "string" ? raw.color : "";
      sheet.hidden = !!raw.hidden;
      sheet.showGrid = raw.grid !== false;
      if (Array.isArray(raw.freeze)) { sheet.freezeRows = clampInt(raw.freeze[0], 0, 1000, 0); sheet.freezeCols = clampInt(raw.freeze[1], 0, 200, 0); }
      for (const [k, v] of Object.entries(raw.cw || {})) sheet.colWidths.set(Number(k), clampInt(v, 0, 4000, DEFAULT_COL_WIDTH));
      for (const [k, v] of Object.entries(raw.rh || {})) sheet.rowHeights.set(Number(k), clampInt(v, 0, 2000, DEFAULT_ROW_HEIGHT));
      for (const r of raw.hr || []) sheet.hiddenRows.add(Number(r));
      for (const c of raw.hc || []) sheet.hiddenCols.add(Number(c));
      sheet.merges = (raw.merges || []).map(parseRange).filter(Boolean);
      for (const [ref, cell] of Object.entries(raw.cells || {})) {
        const at = parseAddr(ref);
        if (!at || !cell || typeof cell !== "object") continue;
        const made = makeCell(decodeValue(cell.v), typeof cell.f === "string" ? cell.f : undefined, styleMap[cell.s] || 0);
        if (made) sheet.cells.set(cellKey(at.r, at.c), made);
      }
      if (raw.filter && raw.filter.range) {
        const range = parseRange(raw.filter.range);
        if (range) sheet.filter = { range, criteria: new Map((raw.filter.criteria || []).map((rule) => [Number(rule.col), { hidden: rule.hidden ? new Set(rule.hidden.map(String)) : undefined, contains: rule.contains }])) };
      }
      sheet.conditional = Array.isArray(raw.cf) ? raw.cf.filter((rule) => rule && rule.range) : [];
      sheet.validations = Array.isArray(raw.dv) ? raw.dv.filter((rule) => rule && rule.range) : [];
      for (const [ref, text] of Object.entries(raw.notes || {})) { const at = parseAddr(ref); if (at) sheet.notes.set(cellKey(at.r, at.c), String(text)); }
      for (const [ref, url] of Object.entries(raw.links || {})) { const at = parseAddr(ref); if (at) sheet.links.set(cellKey(at.r, at.c), String(url)); }
      sheet.charts = Array.isArray(raw.charts) ? raw.charts.filter((chart) => chart && chart.range && chart.anchor) : [];
      return sheet;
    });
    wb.active = clampInt(data.active, 0, wb.sheets.length - 1, 0);
    return wb;
  }
  function clampInt(value, min, max, fallback) {
    const n = Math.round(Number(value));
    return Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : fallback;
  }
  const utf8 = new TextDecoder("utf-8");
  const encoder = new TextEncoder();
  const MAX_UNPACKED = 400 * 1024 * 1024;
  async function inflateRaw(bytes) {
    if (typeof DecompressionStream === "undefined") throw new Error("This browser cannot open compressed workbooks. Update it and try again.");
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }
  async function deflateRaw(bytes) {
    if (typeof CompressionStream === "undefined") return null;
    const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }
  async function gzipText(text) {
    const stream = new Blob([encoder.encode(text)]).stream().pipeThrough(new CompressionStream("gzip"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }
  async function gunzipText(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
    return utf8.decode(new Uint8Array(await new Response(stream).arrayBuffer()));
  }
  function readZip(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let eocd = -1;
    for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) if (view.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error("This file is not a valid .xlsx workbook.");
    const count = view.getUint16(eocd + 10, true);
    let offset = view.getUint32(eocd + 16, true);
    const entries = new Map();
    for (let i = 0; i < count && offset + 46 <= bytes.length; i++) {
      if (view.getUint32(offset, true) !== 0x02014b50) break;
      const method = view.getUint16(offset + 10, true);
      const compressed = view.getUint32(offset + 20, true), size = view.getUint32(offset + 24, true);
      const nameLen = view.getUint16(offset + 28, true), extraLen = view.getUint16(offset + 30, true), commentLen = view.getUint16(offset + 32, true);
      const local = view.getUint32(offset + 42, true);
      const name = utf8.decode(bytes.subarray(offset + 46, offset + 46 + nameLen)).replace(/^\/+/, "");
      entries.set(name.toLowerCase(), { name, method, compressed, size, local });
      offset += 46 + nameLen + extraLen + commentLen;
    }
    let unpacked = 0;
    return {
      names: [...entries.values()].map((entry) => entry.name),
      has: (name) => entries.has(String(name).toLowerCase()),
      async read(name) {
        const e = entries.get(String(name).replace(/^\/+/, "").toLowerCase());
        if (!e) return null;
        if (view.getUint32(e.local, true) !== 0x04034b50) throw new Error("This workbook is damaged.");
        unpacked += e.size;
        if (unpacked > MAX_UNPACKED) throw new Error("This workbook is too large to open.");
        const start = e.local + 30 + view.getUint16(e.local + 26, true) + view.getUint16(e.local + 28, true);
        const data = bytes.subarray(start, start + e.compressed);
        if (e.method === 0) return data;
        if (e.method === 8) return inflateRaw(data);
        throw new Error("This workbook uses a compression method that is not supported.");
      },
      async text(name) { const data = await this.read(name); return data ? utf8.decode(data) : null; },
    };
  }
  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
  })();
  function crc32(bytes) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  async function writeZip(files) {
    const parts = [], central = [];
    let offset = 0;
    const now = new Date();
    const dosTime = (now.getHours() << 11) | (now.getMinutes() << 5) | Math.floor(now.getSeconds() / 2);
    const dosDate = ((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate();
    for (const file of files) {
      const raw = typeof file.data === "string" ? encoder.encode(file.data) : file.data;
      const name = encoder.encode(file.name);
      const packed = raw.length > 128 && !file.store ? await deflateRaw(raw) : null;
      const deflate = !!packed && packed.length < raw.length;
      const body = deflate ? packed : raw;
      const crc = crc32(raw);
      const header = new Uint8Array(30 + name.length);
      const h = new DataView(header.buffer);
      h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true); h.setUint16(8, deflate ? 8 : 0, true);
      h.setUint16(10, dosTime, true); h.setUint16(12, dosDate, true); h.setUint32(14, crc, true);
      h.setUint32(18, body.length, true); h.setUint32(22, raw.length, true); h.setUint16(26, name.length, true);
      header.set(name, 30);
      const record = new Uint8Array(46 + name.length);
      const r = new DataView(record.buffer);
      r.setUint32(0, 0x02014b50, true); r.setUint16(4, 20, true); r.setUint16(6, 20, true); r.setUint16(8, 0x0800, true); r.setUint16(10, deflate ? 8 : 0, true);
      r.setUint16(12, dosTime, true); r.setUint16(14, dosDate, true); r.setUint32(16, crc, true);
      r.setUint32(20, body.length, true); r.setUint32(24, raw.length, true); r.setUint16(28, name.length, true); r.setUint32(42, offset, true);
      record.set(name, 46);
      parts.push(header, body);
      central.push(record);
      offset += header.length + body.length;
    }
    const centralSize = central.reduce((n, part) => n + part.length, 0);
    const end = new Uint8Array(22);
    const e = new DataView(end.buffer);
    e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
    e.setUint32(12, centralSize, true); e.setUint32(16, offset, true);
    const all = [...parts, ...central, end];
    const out = new Uint8Array(all.reduce((n, part) => n + part.length, 0));
    let at = 0;
    for (const part of all) { out.set(part, at); at += part.length; }
    return out;
  }

  const XML_ENTITIES = { lt: "<", gt: ">", amp: "&", quot: '"', apos: "'" };
  function decodeXml(text) {
    if (text.indexOf("&") < 0) return text;
    return text.replace(/&(#x[0-9a-f]+|#\d+|\w+);/gi, (m, e) => {
      if (e[0] === "#") { const code = e[1].toLowerCase() === "x" ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10); return code > 0 && code <= 0x10FFFF ? String.fromCodePoint(code) : ""; }
      return XML_ENTITIES[e] ?? m;
    });
  }
  function parseAttrs(source) {
    const attrs = {};
    const re = /([\w:.-]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/g;
    let m;
    while ((m = re.exec(source))) {
      const value = decodeXml(m[2] !== undefined ? m[2] : m[3]);
      attrs[m[1]] = value;
      const colon = m[1].indexOf(":");
      if (colon >= 0 && !(m[1].slice(colon + 1) in attrs)) attrs[m[1].slice(colon + 1)] = value;
    }
    return attrs;
  }
  function parseXml(text) {
    const root = { name: "#root", attrs: {}, children: [] };
    const stack = [root];
    const re = /<!\[CDATA\[([\s\S]*?)\]\]>|<!--[\s\S]*?-->|<\?[\s\S]*?\?>|<!DOCTYPE[^>]*>|<(\/?)([A-Za-z_][\w:.-]*)((?:[^>"']|"[^"]*"|'[^']*')*?)(\/?)>|([^<]+)/g;
    let m;
    while ((m = re.exec(String(text || "")))) {
      const top = stack[stack.length - 1];
      if (m[1] !== undefined) { top.children.push({ name: "#text", text: m[1] }); continue; }
      if (m[3] !== undefined) {
        const local = m[3].includes(":") ? m[3].slice(m[3].indexOf(":") + 1) : m[3];
        if (m[2]) { for (let i = stack.length - 1; i > 0; i--) if (stack[i].name === local) { stack.length = i; break; } continue; }
        const node = { name: local, attrs: parseAttrs(m[4]), children: [] };
        top.children.push(node);
        if (!m[5]) stack.push(node);
        continue;
      }
      if (m[6] !== undefined && (top.name === "t" || top.name === "Data" || top.name === "v" || top.name === "f" || /\S/.test(m[6]))) top.children.push({ name: "#text", text: decodeXml(m[6]) });
    }
    return root;
  }
  const kids = (node, name) => (node ? node.children.filter((child) => child.name === name) : []);
  const kid = (node, name) => (node ? node.children.find((child) => child.name === name) || null : null);
  const nodePath = (node, ...names) => names.reduce((n, name) => kid(n, name), node);
  function textOf(node) {
    if (!node) return "";
    if (node.name === "#text") return node.text;
    let out = "";
    for (const child of node.children) if (child.name !== "rPh" && child.name !== "phoneticPr") out += textOf(child);
    return out;
  }
  const xmlEscape = (value) => String(value).replace(/[\x00-\x08\x0B\x0C\x0E-\x1F￾￿]/g, "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  const flag = (value) => value === "1" || value === "true" || value === "on";

  function finishSheetSize(sheet) {
    const { r2, c2 } = extentOf(sheet);
    sheet.rowCount = Math.min(MAX_ROWS, Math.max(1000, r2 + 101, sheet.freezeRows + 50));
    sheet.colCount = Math.min(MAX_COLS, Math.max(26, c2 + 6, sheet.freezeCols + 10));
  }
  function adjust1904(wb, sheet) {
    for (const [k, cell] of sheet.cells) {
      if (typeof cell.v === "number" && cell.f === undefined && isDateFormat(styleOf(wb, cell).fmt)) sheet.cells.set(k, { ...cell, v: cell.v + 1462 });
    }
  }

  const THEME_DEFAULT = ["FFFFFF", "000000", "E7E6E6", "44546A", "4472C4", "ED7D31", "A5A5A5", "FFC000", "5B9BD5", "70AD47", "0563C1", "954F72"];
  function applyTint(hex, tint) {
    const rgb = parseHex(hex);
    if (!rgb || !tint) return hex;
    let [r, g, b] = rgb.map((v) => v / 255);
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    let h = 0, s = 0, l = (max + min) / 2;
    if (max !== min) {
      const d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
      h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
      h /= 6;
    }
    l = tint < 0 ? l * (1 + tint) : l * (1 - tint) + tint;
    const hue = (p, q, t) => { if (t < 0) t += 1; if (t > 1) t -= 1; if (t < 1 / 6) return p + (q - p) * 6 * t; if (t < 1 / 2) return q; if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6; return p; };
    if (s === 0) { r = g = b = l; }
    else { const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q; r = hue(p, q, h + 1 / 3); g = hue(p, q, h); b = hue(p, q, h - 1 / 3); }
    return toHex([r * 255, g * 255, b * 255]);
  }
  function xlsxColor(node, theme, palette) {
    if (!node || node.attrs.auto === "1") return "";
    const a = node.attrs;
    let hex = "";
    if (a.rgb) hex = "#" + a.rgb.slice(-6).toUpperCase();
    else if (a.theme !== undefined) hex = "#" + (theme[Number(a.theme)] || "000000");
    else if (a.indexed !== undefined) { const i = Number(a.indexed); if (i >= 64) return ""; hex = palette[i] || ""; }
    if (!hex) return "";
    return a.tint ? applyTint(hex, Number(a.tint)) : hex;
  }
  const H_ALIGN = { left: "left", center: "center", right: "right", justify: "justify", fill: "left", centerContinuous: "center", distributed: "center" };
  const V_ALIGN = { top: "top", center: "middle", bottom: "bottom", justify: "middle", distributed: "middle" };
  function resolvePart(base, target) {
    if (target.startsWith("/")) return target.slice(1);
    const parts = base.split("/").slice(0, -1);
    for (const piece of target.split("/")) {
      if (piece === "..") parts.pop();
      else if (piece && piece !== ".") parts.push(piece);
    }
    return parts.join("/");
  }
  async function readXlsx(bytes) {
    const zip = readZip(bytes);
    const relsPath = (part) => { const i = part.lastIndexOf("/"); return part.slice(0, i + 1) + "_rels/" + part.slice(i + 1) + ".rels"; };
    async function rels(part) {
      const map = new Map();
      const xml = await zip.text(relsPath(part));
      if (!xml) return map;
      for (const rel of kids(kid(parseXml(xml), "Relationships"), "Relationship")) {
        const external = rel.attrs.TargetMode === "External";
        map.set(rel.attrs.Id, { target: external ? rel.attrs.Target : resolvePart(part, rel.attrs.Target || ""), type: rel.attrs.Type || "", external });
      }
      return map;
    }
    let workbookPath = "xl/workbook.xml";
    for (const rel of (await rels("")).values()) if (/\/officeDocument$/.test(rel.type)) workbookPath = rel.target;
    const workbookXml = await zip.text(workbookPath);
    if (!workbookXml) throw new Error("This file is not an Excel workbook.");
    const book = kid(parseXml(workbookXml), "workbook");
    const bookRels = await rels(workbookPath);
    const relOfType = (suffix) => [...bookRels.values()].find((rel) => rel.type.endsWith(suffix));
    const theme = THEME_DEFAULT.slice();
    const themeRel = relOfType("/theme");
    if (themeRel) {
      const scheme = nodePath(parseXml(await zip.text(themeRel.target) || ""), "theme", "themeElements", "clrScheme");
      const order = ["lt1", "dk1", "lt2", "dk2", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6", "hlink", "folHlink"];
      order.forEach((name, i) => {
        const node = kid(scheme, name);
        const value = kid(node, "srgbClr")?.attrs.val || kid(node, "sysClr")?.attrs.lastClr;
        if (value) theme[i] = value.toUpperCase();
      });
    }
    const palette = PALETTE.slice();
    const wb = createWorkbook();
    wb.sheets = [];
    const stylesRel = relOfType("/styles");
    const styleSheet = stylesRel ? kid(parseXml(await zip.text(stylesRel.target) || ""), "styleSheet") : null;
    const indexed = kids(nodePath(styleSheet, "colors", "indexedColors"), "rgbColor");
    indexed.forEach((node, i) => { if (node.attrs.rgb) palette[i] = "#" + node.attrs.rgb.slice(-6).toUpperCase(); });
    const numFmts = new Map(kids(kid(styleSheet, "numFmts"), "numFmt").map((n) => [Number(n.attrs.numFmtId), n.attrs.formatCode]));
    const fontOf = (n) => {
      if (!n) return {};
      const val = (name) => kid(n, name);
      const on = (name) => { const x = val(name); return !!x && x.attrs.val !== "0" && x.attrs.val !== "false"; };
      const u = val("u");
      return { name: val("name")?.attrs.val || val("rFont")?.attrs.val || "", size: Number(val("sz")?.attrs.val || 0), b: on("b"), i: on("i"), s: on("strike"), u: !!u && u.attrs.val !== "none", color: xlsxColor(val("color"), theme, palette) };
    };
    const fonts = kids(kid(styleSheet, "fonts"), "font").map(fontOf);
    const fillOf = (n, dxf) => {
      const pattern = kid(n, "patternFill");
      if (pattern) {
        const type = pattern.attrs.patternType || (dxf ? "solid" : "none");
        if (type === "none") return "";
        const fg = xlsxColor(kid(pattern, "fgColor"), theme, palette), bg = xlsxColor(kid(pattern, "bgColor"), theme, palette);
        if (dxf) return bg || fg;
        if (type === "solid") return fg || bg;
        return fg && bg ? mixColors(fg, bg, 0.5) : fg || bg;
      }
      const stop = kid(kid(n, "gradientFill"), "stop");
      return stop ? xlsxColor(kid(stop, "color"), theme, palette) : "";
    };
    const fills = kids(kid(styleSheet, "fills"), "fill").map((n) => fillOf(n, false));
    const borderOf = (n) => {
      const out = {};
      for (const [side, key] of [["left", "bl"], ["right", "br"], ["top", "bt"], ["bottom", "bb"]]) {
        const edge = kid(n, side);
        if (edge && edge.attrs.style && edge.attrs.style !== "none") out[key] = { w: edge.attrs.style, c: xlsxColor(kid(edge, "color"), theme, palette) || "#000000" };
      }
      return out;
    };
    const borders = kids(kid(styleSheet, "borders"), "border").map(borderOf);
    const baseFont = fonts[0] || { name: "Calibri", size: 11 };
    wb.defaultFont = baseFont.name || "Calibri";
    wb.defaultSize = baseFont.size || 11;
    const xfStyles = kids(kid(styleSheet, "cellXfs"), "xf").map((xf) => {
      const a = xf.attrs, style = {};
      const font = fonts[Number(a.fontId || 0)] || {};
      if (font.name && font.name !== wb.defaultFont) style.font = font.name;
      if (font.size && font.size !== wb.defaultSize) style.size = font.size;
      if (font.b) style.b = true;
      if (font.i) style.i = true;
      if (font.u) style.u = true;
      if (font.s) style.s = true;
      if (font.color && font.color !== "#000000") style.color = font.color;
      const fill = fills[Number(a.fillId || 0)];
      if (fill) style.fill = fill;
      Object.assign(style, borders[Number(a.borderId || 0)] || {});
      const id = Number(a.numFmtId || 0);
      const code = numFmts.get(id) || BUILTIN_FORMATS[id];
      if (code && code !== "General") style.fmt = code;
      const align = kid(xf, "alignment");
      if (align) {
        if (H_ALIGN[align.attrs.horizontal]) style.h = H_ALIGN[align.attrs.horizontal];
        if (V_ALIGN[align.attrs.vertical] && align.attrs.vertical !== "bottom") style.v = V_ALIGN[align.attrs.vertical];
        if (flag(align.attrs.wrapText)) style.wrap = true;
        if (Number(align.attrs.indent)) style.indent = Number(align.attrs.indent);
        if (Number(align.attrs.textRotation)) style.rot = Number(align.attrs.textRotation);
      }
      return internStyle(wb, style);
    });
    const dxfs = kids(kid(styleSheet, "dxfs"), "dxf").map((dxf) => {
      const font = fontOf(kid(dxf, "font")), style = {};
      if (font.b) style.b = true;
      if (font.i) style.i = true;
      if (font.u) style.u = true;
      if (font.s) style.s = true;
      if (font.color) style.color = font.color;
      const fill = kid(dxf, "fill") ? fillOf(kid(dxf, "fill"), true) : "";
      if (fill) style.fill = fill;
      return style;
    });
    const sharedRel = relOfType("/sharedStrings");
    const shared = sharedRel ? kids(kid(parseXml(await zip.text(sharedRel.target) || ""), "sst"), "si").map(textOf) : [];
    const date1904 = flag(kid(book, "workbookPr")?.attrs.date1904);
    const sheetNodes = kids(kid(book, "sheets"), "sheet");
    const sheetNames = sheetNodes.map((node) => node.attrs.name);
    for (const def of kids(kid(book, "definedNames"), "definedName")) {
      const name = def.attrs.name || "";
      if (!name || /^_xlnm\./i.test(name) || /\[\d+\]/.test(textOf(def))) continue;
      const local = def.attrs.localSheetId !== undefined ? sheetNames[Number(def.attrs.localSheetId)] : "";
      wb.names.set(name.toUpperCase(), { name, ref: textOf(def).replace(/_xlfn\.(_xlws\.)?/gi, ""), sheet: local || "" });
    }
    const used = new Set();
    for (const node of sheetNodes) {
      const rel = bookRels.get(node.attrs.id);
      if (!rel || !/\/worksheet$/.test(rel.type)) continue;
      let name = validSheetName(node.attrs.name) ? node.attrs.name : `Sheet${wb.sheets.length + 1}`;
      while (used.has(name.toLowerCase())) name += " (2)";
      used.add(name.toLowerCase());
      const sheet = createSheet(name, { defaultColWidth: 64, defaultRowHeight: 20 });
      sheet.hidden = node.attrs.state === "hidden" || node.attrs.state === "veryHidden";
      const xml = await zip.text(rel.target);
      const ws = kid(parseXml(xml || ""), "worksheet");
      if (!ws) { wb.sheets.push(sheet); continue; }
      const sheetRels = await rels(rel.target);
      const tab = nodePath(ws, "sheetPr", "tabColor");
      if (tab) sheet.color = xlsxColor(tab, theme, palette);
      const view = nodePath(ws, "sheetViews", "sheetView");
      if (view) {
        if (view.attrs.showGridLines === "0" || view.attrs.showGridLines === "false") sheet.showGrid = false;
        const pane = kid(view, "pane");
        if (pane && /frozen/.test(pane.attrs.state || "")) { sheet.freezeCols = Math.min(200, Math.round(Number(pane.attrs.xSplit || 0))); sheet.freezeRows = Math.min(1000, Math.round(Number(pane.attrs.ySplit || 0))); }
      }
      const format = kid(ws, "sheetFormatPr");
      if (format) {
        if (format.attrs.defaultColWidth) sheet.defaultColWidth = Math.max(8, Math.round(Number(format.attrs.defaultColWidth) * 7));
        else if (format.attrs.baseColWidth) sheet.defaultColWidth = Math.max(8, Math.round(Number(format.attrs.baseColWidth) * 7 + 5));
        if (format.attrs.defaultRowHeight) sheet.defaultRowHeight = Math.max(4, Math.round(Number(format.attrs.defaultRowHeight) * 4 / 3));
      }
      for (const col of kids(kid(ws, "cols"), "col")) {
        const min = Number(col.attrs.min) - 1, max = Math.min(MAX_COLS, Number(col.attrs.max)) - 1;
        const width = col.attrs.width !== undefined ? Math.round(Number(col.attrs.width) * 7) : null;
        const span = Math.min(max, min + 2000);
        for (let c = min; c <= span; c++) {
          if (width !== null && width !== sheet.defaultColWidth && (col.attrs.customWidth !== undefined || width !== sheet.defaultColWidth)) sheet.colWidths.set(c, width);
          if (flag(col.attrs.hidden)) sheet.hiddenCols.add(c);
        }
      }
      const sharedFormulas = new Map();
      const arrayFormulas = [];
      let rowIndex = -1;
      for (const row of kids(kid(ws, "sheetData"), "row")) {
        rowIndex = row.attrs.r ? Number(row.attrs.r) - 1 : rowIndex + 1;
        if (row.attrs.ht && flag(row.attrs.customHeight)) sheet.rowHeights.set(rowIndex, Math.round(Number(row.attrs.ht) * 4 / 3));
        else if (row.attrs.ht && Math.abs(Number(row.attrs.ht) * 4 / 3 - sheet.defaultRowHeight) > 2) sheet.rowHeights.set(rowIndex, Math.round(Number(row.attrs.ht) * 4 / 3));
        if (flag(row.attrs.hidden)) sheet.hiddenRows.add(rowIndex);
        let colPos = -1;
        for (const c of kids(row, "c")) {
          const at = c.attrs.r ? parseAddr(c.attrs.r) : null;
          const r = at ? at.r : rowIndex, col = at ? at.c : colPos + 1;
          colPos = col;
          const s = c.attrs.s ? xfStyles[Number(c.attrs.s)] || 0 : 0;
          const t = c.attrs.t || "n";
          const vNode = kid(c, "v");
          const raw = vNode ? textOf(vNode) : null;
          let value;
          if (t === "s") value = raw === null ? undefined : shared[Number(raw)] ?? "";
          else if (t === "str") value = raw ?? "";
          else if (t === "inlineStr") value = textOf(kid(c, "is"));
          else if (t === "b") value = raw === null ? undefined : raw === "1" || raw === "true";
          else if (t === "e") value = raw === null ? undefined : err(raw.trim());
          else if (t === "d") { const d = raw ? new Date(raw) : null; value = d && !Number.isNaN(d.getTime()) ? (Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds()) - EPOCH) / DAY_MS : undefined; }
          else value = raw === null || raw === "" ? undefined : Number(raw);
          if (typeof value === "number" && !Number.isFinite(value)) value = undefined;
          let formula;
          const f = kid(c, "f");
          if (f) {
            const text = textOf(f).replace(/_xlfn\.(_xlws\.)?/gi, "").replace(/^=/, "");
            if (f.attrs.t === "shared" && f.attrs.si !== undefined) {
              if (text) { sharedFormulas.set(f.attrs.si, { text, r, c: col }); formula = text; }
              else { const master = sharedFormulas.get(f.attrs.si); if (master) formula = shiftFormula(master.text, r - master.r, col - master.c); }
            } else if (f.attrs.t === "dataTable") formula = undefined;
            else if (text) {
              formula = text;
              if (f.attrs.t === "array" && f.attrs.ref) { const range = parseRange(f.attrs.ref); if (range && (range.r1 !== range.r2 || range.c1 !== range.c2)) arrayFormulas.push(range); }
            }
          }
          const cell = makeCell(value, formula, s);
          if (cell) sheet.cells.set(cellKey(r, col), cell);
        }
      }
      for (const range of arrayFormulas) {
        for (let r = range.r1; r <= range.r2; r++) for (let c = range.c1; c <= range.c2; c++) {
          if (r === range.r1 && c === range.c1) continue;
          const k = cellKey(r, c), cell = sheet.cells.get(k);
          if (cell && cell.f === undefined) { const next = makeCell(undefined, undefined, cell.s); if (next) sheet.cells.set(k, next); else sheet.cells.delete(k); }
        }
      }
      sheet.merges = kids(kid(ws, "mergeCells"), "mergeCell").map((m) => parseRange(m.attrs.ref)).filter(Boolean);
      const filter = kid(ws, "autoFilter");
      if (filter && parseRange(filter.attrs.ref)) sheet.filter = { range: parseRange(filter.attrs.ref), criteria: new Map() };
      const CELL_OPS = { greaterThan: "gt", lessThan: "lt", greaterThanOrEqual: "ge", lessThanOrEqual: "le", equal: "eq", notEqual: "ne", between: "between", notBetween: "notbetween" };
      for (const block of kids(ws, "conditionalFormatting")) {
        const ranges = String(block.attrs.sqref || "").split(/\s+/).filter((text) => parseRange(text));
        for (const rule of kids(block, "cfRule").sort((a, b) => Number(a.attrs.priority || 0) - Number(b.attrs.priority || 0))) {
          const formulas = kids(rule, "formula").map((n) => textOf(n).replace(/_xlfn\./gi, ""));
          const style = rule.attrs.dxfId !== undefined ? dxfs[Number(rule.attrs.dxfId)] || {} : {};
          let made = null;
          const type = rule.attrs.type;
          if (type === "cellIs" && CELL_OPS[rule.attrs.operator]) made = { type: "cell", op: CELL_OPS[rule.attrs.operator], valueFormula: formulas[0], value2Formula: formulas[1] };
          else if (type === "containsText" || type === "notContainsText" || type === "beginsWith" || type === "endsWith") made = { type: "text", op: { containsText: "contains", notContainsText: "notcontains", beginsWith: "begins", endsWith: "ends" }[type], text: rule.attrs.text || "" };
          else if (type === "expression" && formulas[0]) made = { type: "formula", formula: formulas[0] };
          else if (type === "containsBlanks") made = { type: "blank" };
          else if (type === "notContainsBlanks") made = { type: "notblank" };
          else if (type === "containsErrors") made = { type: "error" };
          else if (type === "duplicateValues") made = { type: "duplicate" };
          else if (type === "uniqueValues") made = { type: "unique" };
          else if (type === "colorScale") made = { type: "scale", colors: kids(kid(rule, "colorScale"), "color").map((n) => xlsxColor(n, theme, palette) || "#FFFFFF") };
          if (!made) continue;
          for (const range of ranges) sheet.conditional.push({ id: uid(), range: rangeText(parseRange(range)), ...made, style, stop: flag(rule.attrs.stopIfTrue) });
        }
      }
      const NUMBER_OPS = { between: "between", notBetween: "notbetween", equal: "eq", notEqual: "ne", greaterThan: "gt", lessThan: "lt", greaterThanOrEqual: "ge", lessThanOrEqual: "le" };
      for (const dv of kids(kid(ws, "dataValidations"), "dataValidation")) {
        const f1 = textOf(kid(dv, "formula1")), f2 = textOf(kid(dv, "formula2"));
        const type = dv.attrs.type || "any";
        let rule = null;
        if (type === "list") {
          if (/^".*"$/.test(f1)) rule = { type: "list", values: f1.slice(1, -1).replace(/""/g, '"').split(",").map((v) => v.trim()) };
          else if (f1) rule = { type: "list", source: f1 };
          if (rule) rule.dropdown = !flag(dv.attrs.showDropDown);
        } else if (type === "whole" || type === "decimal" || type === "date" || type === "time") rule = { type: type === "date" ? "date" : "number", op: NUMBER_OPS[dv.attrs.operator || "between"] || "between", min: f1, max: f2 };
        else if (type === "textLength") rule = { type: "length", min: f1 || "0", max: f2 || f1 };
        else if (type === "custom") rule = { type: "formula", formula: f1 };
        if (!rule) continue;
        rule.strict = (dv.attrs.errorStyle || "stop") === "stop" && dv.attrs.showErrorMessage !== "0";
        if (dv.attrs.promptTitle || dv.attrs.prompt) rule.help = dv.attrs.prompt || dv.attrs.promptTitle;
        for (const text of String(dv.attrs.sqref || "").split(/\s+/)) { const range = parseRange(text); if (range) sheet.validations.push({ id: uid(), range: rangeText(range), ...rule }); }
      }
      for (const link of kids(kid(ws, "hyperlinks"), "hyperlink")) {
        const range = parseRange(link.attrs.ref);
        if (!range) continue;
        const target = link.attrs.id && sheetRels.get(link.attrs.id) ? sheetRels.get(link.attrs.id).target : link.attrs.location ? "#" + link.attrs.location : "";
        if (target) sheet.links.set(cellKey(range.r1, range.c1), target);
      }
      for (const relItem of sheetRels.values()) {
        if (!/\/comments$/.test(relItem.type)) continue;
        const comments = kid(parseXml(await zip.text(relItem.target) || ""), "comments");
        for (const comment of kids(kid(comments, "commentList"), "comment")) {
          const at = parseAddr(comment.attrs.ref);
          if (at) sheet.notes.set(cellKey(at.r, at.c), textOf(kid(comment, "text")).trim());
        }
      }
      if (date1904) adjust1904(wb, sheet);
      finishSheetSize(sheet);
      wb.sheets.push(sheet);
    }
    if (!wb.sheets.length) wb.sheets.push(createSheet("Sheet1"));
    const activeTab = Number(nodePath(book, "bookViews", "workbookView")?.attrs.activeTab || 0);
    wb.active = Math.max(0, Math.min(wb.sheets.length - 1, activeTab));
    if (wb.sheets[wb.active].hidden) wb.active = Math.max(0, wb.sheets.findIndex((sheet) => !sheet.hidden));
    return wb;
  }

  const BUILTIN_FORMAT_IDS = new Map(Object.entries(BUILTIN_FORMATS).map(([id, code]) => [code, Number(id)]));
  const XLSX_BORDER_STYLES = new Set(["thin", "medium", "thick", "dashed", "dotted", "double", "hair", "mediumDashed", "dashDot", "mediumDashDot", "dashDotDot", "mediumDashDotDot", "slantDashDot"]);
  const argb = (hex) => "FF" + String(hex || "#000000").replace("#", "").toUpperCase().padStart(6, "0").slice(0, 6);
  async function writeXlsx(wb, calc) {
    const NS = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"';
    const REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    const fonts = [], fontIndex = new Map(), fills = ['<fill><patternFill patternType="none"/></fill>', '<fill><patternFill patternType="gray125"/></fill>'], fillIndex = new Map();
    const borders = ["<border><left/><right/><top/><bottom/><diagonal/></border>"], borderIndex = new Map();
    const numFmts = [], numFmtIndex = new Map(), xfs = [], xfIndex = new Map(), dxfs = [];
    const fontXml = (f) => `<font>${f.b ? "<b/>" : ""}${f.i ? "<i/>" : ""}${f.s ? "<strike/>" : ""}${f.u ? "<u/>" : ""}<sz val="${f.size}"/>${f.color ? `<color rgb="${argb(f.color)}"/>` : '<color theme="1"/>'}<name val="${xmlEscape(f.name)}"/></font>`;
    const intern = (list, index, key, xml) => { let i = index.get(key); if (i === undefined) { i = list.length; list.push(xml); index.set(key, i); } return i; };
    const fontId = (style) => { const f = { name: style.font || wb.defaultFont, size: style.size || wb.defaultSize, b: !!style.b, i: !!style.i, s: !!style.s, u: !!style.u, color: style.color || "" }; return intern(fonts, fontIndex, JSON.stringify(f), fontXml(f)); };
    fontId({});
    const xfOf = (styleId) => {
      if (!styleId) return 0;
      let i = xfIndex.get(styleId);
      if (i !== undefined) return i;
      const style = wb.styles[styleId] || {};
      const font = fontId(style);
      const fill = style.fill ? intern(fills, fillIndex, style.fill, `<fill><patternFill patternType="solid"><fgColor rgb="${argb(style.fill)}"/><bgColor indexed="64"/></patternFill></fill>`) : 0;
      const edge = (name, b) => (b && XLSX_BORDER_STYLES.has(b.w) ? `<${name} style="${b.w}"><color rgb="${argb(b.c)}"/></${name}>` : `<${name}/>`);
      const borderKey = JSON.stringify([style.bl, style.br, style.bt, style.bb]);
      const border = style.bl || style.br || style.bt || style.bb ? intern(borders, borderIndex, borderKey, `<border>${edge("left", style.bl)}${edge("right", style.br)}${edge("top", style.bt)}${edge("bottom", style.bb)}<diagonal/></border>`) : 0;
      let numFmt = 0;
      if (style.fmt) {
        numFmt = BUILTIN_FORMAT_IDS.get(style.fmt);
        if (numFmt === undefined || numFmt === 14 && style.fmt !== "m/d/yyyy") {
          numFmt = numFmtIndex.get(style.fmt);
          if (numFmt === undefined) { numFmt = 164 + numFmts.length; numFmts.push(`<numFmt numFmtId="${numFmt}" formatCode="${xmlEscape(style.fmt)}"/>`); numFmtIndex.set(style.fmt, numFmt); }
        }
      }
      const h = style.h ? ` horizontal="${style.h}"` : "", v = style.v ? ` vertical="${style.v === "middle" ? "center" : style.v}"` : "";
      const alignment = h || v || style.wrap || style.indent || style.rot ? `<alignment${h}${v}${style.wrap ? ' wrapText="1"' : ""}${style.indent ? ` indent="${style.indent}"` : ""}${style.rot ? ` textRotation="${style.rot}"` : ""}/>` : "";
      i = xfs.length + 1;
      xfs.push(`<xf numFmtId="${numFmt}" fontId="${font}" fillId="${fill}" borderId="${border}" xfId="0"${numFmt ? ' applyNumberFormat="1"' : ""}${font ? ' applyFont="1"' : ""}${fill ? ' applyFill="1"' : ""}${border ? ' applyBorder="1"' : ""}${alignment ? ` applyAlignment="1">${alignment}</xf>` : "/>"}`);
      xfIndex.set(styleId, i);
      return i;
    };
    const dxfOf = (style = {}) => {
      dxfs.push(`<dxf>${style.b || style.i || style.u || style.s || style.color ? `<font>${style.b ? "<b/>" : ""}${style.i ? "<i/>" : ""}${style.s ? "<strike/>" : ""}${style.u ? "<u/>" : ""}${style.color ? `<color rgb="${argb(style.color)}"/>` : ""}</font>` : ""}${style.fill ? `<fill><patternFill patternType="solid"><bgColor rgb="${argb(style.fill)}"/></patternFill></fill>` : ""}</dxf>`);
      return dxfs.length - 1;
    };
    const strings = [], stringIndex = new Map();
    const sst = (text) => intern(strings, stringIndex, text, `<si><t xml:space="preserve">${xmlEscape(text)}</t></si>`);
    const files = [];
    const contentOverrides = [];
    const sheetEntries = [];
    const definedNames = [];
    let vmlUsed = false;
    wb.sheets.forEach((sheet, index) => {
      const sheetRels = [];
      const out = [`<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet ${NS}>`];
      if (sheet.color) out.push(`<sheetPr><tabColor rgb="${argb(sheet.color)}"/></sheetPr>`);
      const used = extentOf(sheet);
      out.push(`<dimension ref="${used.r2 < 0 ? "A1" : rangeText({ r1: 0, c1: 0, r2: Math.max(0, used.r2), c2: Math.max(0, used.c2) })}"/>`);
      let pane = "";
      if (sheet.freezeRows || sheet.freezeCols) {
        const active = sheet.freezeRows && sheet.freezeCols ? "bottomRight" : sheet.freezeRows ? "bottomLeft" : "topRight";
        pane = `<pane${sheet.freezeCols ? ` xSplit="${sheet.freezeCols}"` : ""}${sheet.freezeRows ? ` ySplit="${sheet.freezeRows}"` : ""} topLeftCell="${addr(sheet.freezeRows, sheet.freezeCols)}" activePane="${active}" state="frozen"/><selection pane="${active}"/>`;
      }
      out.push(`<sheetViews><sheetView workbookViewId="0"${sheet.showGrid ? "" : ' showGridLines="0"'}${index === wb.active ? ' tabSelected="1"' : ""}>${pane}</sheetView></sheetViews>`);
      out.push(`<sheetFormatPr defaultRowHeight="${roundTo(sheet.defaultRowHeight * 0.75, 2)}"${sheet.defaultColWidth !== 64 ? ` defaultColWidth="${roundTo(sheet.defaultColWidth / 7, 4)}"` : ""}/>`);
      const colIds = [...new Set([...sheet.colWidths.keys(), ...sheet.hiddenCols])].sort((a, b) => a - b);
      if (colIds.length) out.push("<cols>" + colIds.map((c) => `<col min="${c + 1}" max="${c + 1}" width="${roundTo((sheet.colWidths.get(c) ?? sheet.defaultColWidth) / 7, 4)}" customWidth="1"${sheet.hiddenCols.has(c) ? ' hidden="1"' : ""}/>`).join("") + "</cols>");
      const rows = new Map();
      for (const [k, cell] of sheet.cells) { const r = keyRow(k); if (!rows.has(r)) rows.set(r, []); rows.get(r).push([keyCol(k), cell]); }
      for (const r of [...sheet.rowHeights.keys(), ...sheet.hiddenRows]) if (!rows.has(r)) rows.set(r, []);
      out.push("<sheetData>");
      for (const r of [...rows.keys()].sort((a, b) => a - b)) {
        const height = sheet.rowHeights.get(r);
        const attrs = `${height !== undefined ? ` ht="${roundTo(height * 0.75, 2)}" customHeight="1"` : ""}${sheet.hiddenRows.has(r) ? ' hidden="1"' : ""}`;
        const cellsXml = rows.get(r).sort((a, b) => a[0] - b[0]).map(([c, cell]) => {
          const ref = addr(r, c), s = cell.s ? ` s="${xfOf(cell.s)}"` : "";
          const value = cell.f !== undefined && calc ? calc.cellValue(sheet, r, c) : cell.v;
          const f = cell.f !== undefined ? `<f>${xmlEscape(excelFormulaText(cell.f))}</f>` : "";
          if (value === undefined || value === null) return f ? `<c r="${ref}"${s}>${f}</c>` : `<c r="${ref}"${s}/>`;
          if (typeof value === "number") return `<c r="${ref}"${s}>${f}<v>${Number.isFinite(value) ? value : 0}</v></c>`;
          if (typeof value === "boolean") return `<c r="${ref}"${s} t="b">${f}<v>${value ? 1 : 0}</v></c>`;
          if (isErr(value)) return `<c r="${ref}"${s} t="e">${f}<v>${["#SPILL!", "#CALC!", "#ERROR!"].includes(value.code) ? "#VALUE!" : value.code}</v></c>`;
          if (f) return `<c r="${ref}"${s} t="str">${f}<v>${xmlEscape(value)}</v></c>`;
          return `<c r="${ref}"${s} t="s"><v>${sst(String(value))}</v></c>`;
        }).join("");
        out.push(`<row r="${r + 1}"${attrs}>${cellsXml}</row>`);
      }
      out.push("</sheetData>");
      if (sheet.filter) {
        out.push(`<autoFilter ref="${rangeText(sheet.filter.range)}"/>`);
        definedNames.push(`<definedName name="_xlnm._FilterDatabase" localSheetId="${index}" hidden="1">${xmlEscape(quoteSheet(sheet.name))}!${xmlEscape(rangeText(sheet.filter.range).replace(/([A-Z]+)(\d+)/g, "$$$1$$$2"))}</definedName>`);
      }
      if (sheet.merges.length) out.push(`<mergeCells count="${sheet.merges.length}">${sheet.merges.map((m) => `<mergeCell ref="${rangeText(m)}"/>`).join("")}</mergeCells>`);
      let priority = 1;
      const CELL_OPS = { gt: "greaterThan", lt: "lessThan", ge: "greaterThanOrEqual", le: "lessThanOrEqual", eq: "equal", ne: "notEqual", between: "between", notbetween: "notBetween" };
      const literal = (text) => { const parsed = parseInput(String(text ?? "")); return typeof parsed.value === "number" ? String(parsed.value) : `"${String(text ?? "").replace(/"/g, '""')}"`; };
      for (const rule of sheet.conditional) {
        const range = parseRange(rule.range);
        if (!range) continue;
        const tl = addr(range.r1, range.c1);
        const needle = `"${String(rule.text || "").replace(/"/g, '""')}"`;
        let body = "";
        if (rule.type === "cell" && CELL_OPS[rule.op]) {
          const f1 = rule.valueFormula || literal(rule.value), f2 = rule.value2Formula || (rule.value2 !== undefined && rule.value2 !== "" ? literal(rule.value2) : "");
          body = `<cfRule type="cellIs" dxfId="${dxfOf(rule.style)}" priority="${priority++}" operator="${CELL_OPS[rule.op]}"><formula>${xmlEscape(f1)}</formula>${(rule.op === "between" || rule.op === "notbetween") && f2 ? `<formula>${xmlEscape(f2)}</formula>` : ""}</cfRule>`;
        } else if (rule.type === "text") {
          const spec = {
            contains: ["containsText", "containsText", `NOT(ISERROR(SEARCH(${needle},${tl})))`], notcontains: ["notContainsText", "notContains", `ISERROR(SEARCH(${needle},${tl}))`],
            begins: ["beginsWith", "beginsWith", `LEFT(${tl},LEN(${needle}))=${needle}`], ends: ["endsWith", "endsWith", `RIGHT(${tl},LEN(${needle}))=${needle}`],
            equals: ["expression", "", `LOWER(${tl})=LOWER(${needle})`],
          }[rule.op || "contains"];
          body = `<cfRule type="${spec[0]}" dxfId="${dxfOf(rule.style)}" priority="${priority++}"${spec[1] ? ` operator="${spec[1]}" text="${xmlEscape(rule.text || "")}"` : ""}><formula>${xmlEscape(spec[2])}</formula></cfRule>`;
        } else if (rule.type === "blank") body = `<cfRule type="containsBlanks" dxfId="${dxfOf(rule.style)}" priority="${priority++}"><formula>LEN(TRIM(${tl}))=0</formula></cfRule>`;
        else if (rule.type === "notblank") body = `<cfRule type="notContainsBlanks" dxfId="${dxfOf(rule.style)}" priority="${priority++}"><formula>LEN(TRIM(${tl}))&gt;0</formula></cfRule>`;
        else if (rule.type === "error") body = `<cfRule type="containsErrors" dxfId="${dxfOf(rule.style)}" priority="${priority++}"><formula>ISERROR(${tl})</formula></cfRule>`;
        else if (rule.type === "duplicate" || rule.type === "unique") body = `<cfRule type="${rule.type === "duplicate" ? "duplicateValues" : "uniqueValues"}" dxfId="${dxfOf(rule.style)}" priority="${priority++}"/>`;
        else if (rule.type === "formula" && rule.formula) body = `<cfRule type="expression" dxfId="${dxfOf(rule.style)}" priority="${priority++}"><formula>${xmlEscape(excelFormulaText(String(rule.formula).replace(/^=/, "")))}</formula></cfRule>`;
        else if (rule.type === "scale") {
          const colors = rule.colors && rule.colors.length >= 2 ? rule.colors : ["#F8696B", "#FFEB84", "#63BE7B"];
          const cfvo = colors.length === 3 ? '<cfvo type="min"/><cfvo type="percentile" val="50"/><cfvo type="max"/>' : '<cfvo type="min"/><cfvo type="max"/>';
          body = `<cfRule type="colorScale" priority="${priority++}"><colorScale>${cfvo}${colors.map((c) => `<color rgb="${argb(c)}"/>`).join("")}</colorScale></cfRule>`;
        }
        if (body) out.push(`<conditionalFormatting sqref="${rangeText(range)}">${body}</conditionalFormatting>`);
      }
      const NUMBER_OPS = { between: "between", notbetween: "notBetween", eq: "equal", ne: "notEqual", gt: "greaterThan", lt: "lessThan", ge: "greaterThanOrEqual", le: "lessThanOrEqual" };
      const validations = sheet.validations.map((rule) => {
        const range = parseRange(rule.range);
        if (!range) return "";
        const common = ` allowBlank="1" showErrorMessage="${rule.strict === false ? 0 : 1}"${rule.strict === false ? ' errorStyle="warning"' : ""}${rule.help ? ` showInputMessage="1" prompt="${xmlEscape(rule.help)}"` : ""} sqref="${rangeText(range)}"`;
        if (rule.type === "list") {
          const f1 = rule.source ? rule.source.replace(/^=/, "") : `"${(rule.values || []).join(",").replace(/"/g, '""')}"`;
          return `<dataValidation type="list"${rule.dropdown === false ? ' showDropDown="1"' : ""}${common}><formula1>${xmlEscape(f1)}</formula1></dataValidation>`;
        }
        if (rule.type === "number" || rule.type === "date") {
          const op = NUMBER_OPS[rule.op || "between"] || "between";
          return `<dataValidation type="${rule.type === "date" ? "date" : "decimal"}" operator="${op}"${common}><formula1>${xmlEscape(rule.min ?? "0")}</formula1>${op === "between" || op === "notBetween" ? `<formula2>${xmlEscape(rule.max ?? "0")}</formula2>` : ""}</dataValidation>`;
        }
        if (rule.type === "length") return `<dataValidation type="textLength" operator="between"${common}><formula1>${xmlEscape(rule.min || "0")}</formula1><formula2>${xmlEscape(rule.max || "32767")}</formula2></dataValidation>`;
        if (rule.type === "formula") return `<dataValidation type="custom"${common}><formula1>${xmlEscape(String(rule.formula || "TRUE").replace(/^=/, ""))}</formula1></dataValidation>`;
        return "";
      }).filter(Boolean);
      if (validations.length) out.push(`<dataValidations count="${validations.length}">${validations.join("")}</dataValidations>`);
      const links = [...sheet.links].filter(([, url]) => /^(https?:|mailto:|#)/i.test(url));
      if (links.length) {
        out.push("<hyperlinks>" + links.map(([k, url]) => {
          const ref = addr(keyRow(k), keyCol(k));
          if (url.startsWith("#")) return `<hyperlink ref="${ref}" location="${xmlEscape(url.slice(1))}"/>`;
          const id = `rId${sheetRels.length + 1}`;
          sheetRels.push(`<Relationship Id="${id}" Type="${REL}/hyperlink" Target="${xmlEscape(url)}" TargetMode="External"/>`);
          return `<hyperlink ref="${ref}" r:id="${id}"/>`;
        }).join("") + "</hyperlinks>");
      }
      out.push('<pageMargins left="0.7" right="0.7" top="0.75" bottom="0.75" header="0.3" footer="0.3"/>');
      if (sheet.notes.size) {
        vmlUsed = true;
        const n = index + 1;
        const notes = [...sheet.notes].sort((a, b) => a[0] - b[0]);
        files.push({ name: `xl/comments${n}.xml`, data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<comments xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><authors><author>ForkMesh</author></authors><commentList>${notes.map(([k, text]) => `<comment ref="${addr(keyRow(k), keyCol(k))}" authorId="0"><text><r><t xml:space="preserve">${xmlEscape(text)}</t></r></text></comment>`).join("")}</commentList></comments>` });
        const shapes = notes.map(([k], i) => `<v:shape id="_x0000_s${n * 1024 + i + 1}" type="#_x0000_t202" style="position:absolute;margin-left:59.25pt;margin-top:1.5pt;width:108pt;height:59.25pt;z-index:${i + 1};visibility:hidden" fillcolor="#ffffe1" o:insetmode="auto"><v:fill color2="#ffffe1"/><v:shadow on="t" color="black" obscured="t"/><v:path o:connecttype="none"/><v:textbox style="mso-direction-alt:auto"><div style="text-align:left"></div></v:textbox><x:ClientData ObjectType="Note"><x:MoveWithCells/><x:SizeWithCells/><x:AutoFill>False</x:AutoFill><x:Row>${keyRow(k)}</x:Row><x:Column>${keyCol(k)}</x:Column></x:ClientData></v:shape>`).join("");
        files.push({ name: `xl/drawings/vmlDrawing${n}.vml`, data: `<xml xmlns:v="urn:schemas-microsoft-com:vml" xmlns:o="urn:schemas-microsoft-com:office:office" xmlns:x="urn:schemas-microsoft-com:office:excel"><o:shapelayout v:ext="edit"><o:idmap v:ext="edit" data="${n}"/></o:shapelayout><v:shapetype id="_x0000_t202" coordsize="21600,21600" o:spt="202" path="m,l,21600r21600,l21600,xe"><v:stroke joinstyle="miter"/><v:path gradientshapeok="t" o:connecttype="rect"/></v:shapetype>${shapes}</xml>` });
        const vmlId = `rId${sheetRels.length + 1}`;
        sheetRels.push(`<Relationship Id="${vmlId}" Type="${REL}/vmlDrawing" Target="../drawings/vmlDrawing${n}.vml"/>`);
        sheetRels.push(`<Relationship Id="rId${sheetRels.length + 1}" Type="${REL}/comments" Target="../comments${n}.xml"/>`);
        out.push(`<legacyDrawing r:id="${vmlId}"/>`);
        contentOverrides.push(`<Override PartName="/xl/comments${n}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml"/>`);
      }
      out.push("</worksheet>");
      files.push({ name: `xl/worksheets/sheet${index + 1}.xml`, data: out.join("") });
      if (sheetRels.length) files.push({ name: `xl/worksheets/_rels/sheet${index + 1}.xml.rels`, data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">${sheetRels.join("")}</Relationships>` });
      contentOverrides.push(`<Override PartName="/xl/worksheets/sheet${index + 1}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>`);
      sheetEntries.push(`<sheet name="${xmlEscape(sheet.name)}" sheetId="${index + 1}"${sheet.hidden ? ' state="hidden"' : ""} r:id="rId${index + 1}"/>`);
    });
    for (const def of wb.names.values()) {
      const local = def.sheet ? wb.sheets.findIndex((sheet) => sheet.name === def.sheet) : -1;
      definedNames.push(`<definedName name="${xmlEscape(def.name)}"${local >= 0 ? ` localSheetId="${local}"` : ""}>${xmlEscape(excelFormulaText(def.ref.replace(/^=/, "")))}</definedName>`);
    }
    const n = wb.sheets.length;
    files.unshift({ name: "[Content_Types].xml", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>${vmlUsed ? '<Default Extension="vml" ContentType="application/vnd.openxmlformats-officedocument.vmlDrawing"/>' : ""}<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/><Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/><Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>${contentOverrides.join("")}</Types>` },
      { name: "_rels/.rels", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="${REL}/officeDocument" Target="xl/workbook.xml"/><Relationship Id="rId2" Type="${REL}/extended-properties" Target="docProps/app.xml"/></Relationships>` },
      { name: "docProps/app.xml", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>ForkMesh Spreadsheets</Application></Properties>` });
    files.push({ name: "xl/workbook.xml", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook ${NS}><workbookPr/><bookViews><workbookView activeTab="${wb.active}"/></bookViews><sheets>${sheetEntries.join("")}</sheets>${definedNames.length ? `<definedNames>${definedNames.join("")}</definedNames>` : ""}<calcPr calcId="191029" fullCalcOnLoad="1"/></workbook>` });
    files.push({ name: "xl/_rels/workbook.xml.rels", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">${wb.sheets.map((_, i) => `<Relationship Id="rId${i + 1}" Type="${REL}/worksheet" Target="worksheets/sheet${i + 1}.xml"/>`).join("")}<Relationship Id="rId${n + 1}" Type="${REL}/styles" Target="styles.xml"/><Relationship Id="rId${n + 2}" Type="${REL}/sharedStrings" Target="sharedStrings.xml"/></Relationships>` });
    files.push({ name: "xl/sharedStrings.xml", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="${strings.length}" uniqueCount="${strings.length}">${strings.join("")}</sst>` });
    files.push({ name: "xl/styles.xml", data: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">${numFmts.length ? `<numFmts count="${numFmts.length}">${numFmts.join("")}</numFmts>` : ""}<fonts count="${fonts.length}">${fonts.join("")}</fonts><fills count="${fills.length}">${fills.join("")}</fills><borders count="${borders.length}">${borders.join("")}</borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="${xfs.length + 1}"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>${xfs.join("")}</cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles><dxfs count="${dxfs.length}">${dxfs.join("")}</dxfs></styleSheet>` });
    return writeZip(files);
  }

  function readCfb(bytes) {
    const SIGNATURE = [0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1];
    if (bytes.length < 512 || SIGNATURE.some((b, i) => bytes[i] !== b)) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const sectorSize = 1 << view.getUint16(0x1E, true), miniSize = 1 << view.getUint16(0x20, true);
    if (sectorSize !== 512 && sectorSize !== 4096) throw new Error("This .xls file is damaged.");
    const dirStart = view.getUint32(0x30, true), miniCutoff = view.getUint32(0x38, true);
    const miniFatStart = view.getUint32(0x3C, true);
    let difatSector = view.getUint32(0x44, true);
    const END = 0xFFFFFFFA;
    const offsetOf = (n) => (n + 1) * sectorSize;
    const difat = [];
    for (let i = 0; i < 109; i++) { const s = view.getUint32(0x4C + i * 4, true); if (s < END) difat.push(s); }
    for (let guard = 0; difatSector < END && guard < 50000; guard++) {
      const off = offsetOf(difatSector);
      if (off + sectorSize > bytes.length) break;
      for (let i = 0; i < sectorSize / 4 - 1; i++) { const s = view.getUint32(off + i * 4, true); if (s < END) difat.push(s); }
      difatSector = view.getUint32(off + sectorSize - 4, true);
    }
    const per = sectorSize / 4;
    const fat = new Uint32Array(difat.length * per).fill(0xFFFFFFFE);
    difat.forEach((s, i) => { const off = offsetOf(s); if (off + sectorSize <= bytes.length) for (let j = 0; j < per; j++) fat[i * per + j] = view.getUint32(off + j * 4, true); });
    const readChain = (start, size) => {
      const sectors = [];
      const seen = new Set();
      for (let s = start; s < END && s < fat.length && !seen.has(s); s = fat[s]) { seen.add(s); sectors.push(s); }
      const out = new Uint8Array(sectors.length * sectorSize);
      sectors.forEach((s, i) => { const off = offsetOf(s); out.set(bytes.subarray(off, Math.min(off + sectorSize, bytes.length)), i * sectorSize); });
      return size === undefined ? out : out.subarray(0, Math.min(size, out.length));
    };
    const dir = readChain(dirStart);
    const dv = new DataView(dir.buffer, dir.byteOffset, dir.byteLength);
    const entries = [];
    for (let off = 0; off + 128 <= dir.length; off += 128) {
      const nameLen = dv.getUint16(off + 64, true);
      let name = "";
      for (let i = 0; i < Math.min(31, Math.max(0, nameLen / 2 - 1)); i++) name += String.fromCharCode(dv.getUint16(off + i * 2, true));
      entries.push({ name, type: dir[off + 66], start: dv.getUint32(off + 116, true), size: dv.getUint32(off + 120, true) });
    }
    const root = entries.find((e) => e.type === 5);
    let mini = null, miniFat = null;
    return {
      entries,
      stream(name) {
        const entry = entries.find((e) => e.type === 2 && e.name.toLowerCase() === name.toLowerCase());
        if (!entry) return null;
        if (entry.size >= miniCutoff) return readChain(entry.start, entry.size);
        if (!mini) {
          mini = root ? readChain(root.start, root.size) : new Uint8Array(0);
          const raw = readChain(miniFatStart);
          const mv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
          miniFat = new Uint32Array(raw.length / 4);
          for (let i = 0; i < miniFat.length; i++) miniFat[i] = mv.getUint32(i * 4, true);
        }
        const out = new Uint8Array(entry.size);
        const seen = new Set();
        for (let s = entry.start, pos = 0; s < END && pos < entry.size && !seen.has(s); s = miniFat[s], pos += miniSize) {
          seen.add(s);
          out.set(mini.subarray(s * miniSize, s * miniSize + Math.min(miniSize, entry.size - pos)), pos);
        }
        return out;
      },
    };
  }

  const BIFF_ERRORS = { 0x00: "#NULL!", 0x07: "#DIV/0!", 0x0F: "#VALUE!", 0x17: "#REF!", 0x1D: "#NAME?", 0x24: "#NUM!", 0x2A: "#N/A" };
  const BIFF_BORDERS = [null, "thin", "medium", "dashed", "dotted", "thick", "double", "hair", "mediumDashed", "dashDot", "mediumDashDot", "dashDotDot", "mediumDashDotDot", "slantDashDot"];
  const FTAB = (() => {
    const source = "0 COUNT|1 IF|2 ISNA 1|3 ISERROR 1|4 SUM|5 AVERAGE|6 MIN|7 MAX|8 ROW|9 COLUMN|10 NA 0|11 NPV|12 STDEV|13 DOLLAR|14 FIXED|15 SIN 1|16 COS 1|17 TAN 1|18 ATAN 1|19 PI 0|20 SQRT 1|21 EXP 1|22 LN 1|23 LOG10 1|24 ABS 1|25 INT 1|26 SIGN 1|27 ROUND 2|28 LOOKUP|29 INDEX|30 REPT 2|31 MID 3|32 LEN 1|33 VALUE 1|34 TRUE 0|35 FALSE 0|36 AND|37 OR|38 NOT 1|39 MOD 2|40 DCOUNT 3|41 DSUM 3|42 DAVERAGE 3|43 DMIN 3|44 DMAX 3|45 DSTDEV 3|46 VAR|47 DVAR 3|48 TEXT 2|49 LINEST|50 TREND|51 LOGEST|52 GROWTH|56 PV|57 FV|58 NPER|59 PMT|60 RATE|61 MIRR 3|62 IRR|63 RAND 0|64 MATCH|65 DATE 3|66 TIME 3|67 DAY 1|68 MONTH 1|69 YEAR 1|70 WEEKDAY|71 HOUR 1|72 MINUTE 1|73 SECOND 1|74 NOW 0|75 AREAS 1|76 ROWS 1|77 COLUMNS 1|78 OFFSET|82 SEARCH|83 TRANSPOSE 1|86 TYPE 1|97 ATAN2 2|98 ASIN 1|99 ACOS 1|100 CHOOSE|101 HLOOKUP|102 VLOOKUP|105 ISREF 1|109 LOG|111 CHAR 1|112 LOWER 1|113 UPPER 1|114 PROPER 1|115 LEFT|116 RIGHT|117 EXACT 2|118 TRIM 1|119 REPLACE 4|120 SUBSTITUTE|121 CODE 1|124 FIND|125 CELL|126 ISERR 1|127 ISTEXT 1|128 ISNUMBER 1|129 ISBLANK 1|130 T 1|131 N 1|140 DATEVALUE 1|141 TIMEVALUE 1|142 SLN 3|143 SYD 4|144 DDB|148 INDIRECT|162 CLEAN 1|163 MDETERM 1|164 MINVERSE 1|165 MMULT 2|167 IPMT|168 PPMT|169 COUNTA|183 PRODUCT|184 FACT 1|189 DPRODUCT 3|190 ISNONTEXT 1|193 STDEVP|194 VARP|195 DSTDEVP 3|196 DVARP 3|197 TRUNC|198 ISLOGICAL 1|199 DCOUNTA 3|212 ROUNDUP 2|213 ROUNDDOWN 2|216 RANK|219 ADDRESS|220 DAYS360|221 TODAY 0|222 VDB|227 MEDIAN|228 SUMPRODUCT|229 SINH 1|230 COSH 1|231 TANH 1|232 ASINH 1|233 ACOSH 1|234 ATANH 1|235 DGET 3|244 INFO 1|247 DB|252 FREQUENCY 2|261 ERROR.TYPE 1|269 AVEDEV|270 BETADIST|271 GAMMALN 1|272 BETAINV|273 BINOMDIST 4|274 CHIDIST 2|275 CHIINV 2|276 COMBIN 2|277 CONFIDENCE 3|278 CRITBINOM 3|279 EVEN 1|280 EXPONDIST 3|281 FDIST 3|282 FINV 3|283 FISHER 1|284 FISHERINV 1|285 FLOOR 2|286 GAMMADIST 4|287 GAMMAINV 3|288 CEILING 2|289 HYPGEOMDIST 4|290 LOGNORMDIST 3|291 LOGINV 3|292 NEGBINOMDIST 3|293 NORMDIST 4|294 NORMSDIST 1|295 NORMINV 3|296 NORMSINV 1|297 STANDARDIZE 3|298 ODD 1|299 PERMUT 2|300 POISSON 3|301 TDIST 3|302 WEIBULL 4|303 SUMXMY2 2|304 SUMX2MY2 2|305 SUMX2PY2 2|306 CHITEST 2|307 CORREL 2|308 COVAR 2|309 FORECAST 3|310 FTEST 2|311 INTERCEPT 2|312 PEARSON 2|313 RSQ 2|314 STEYX 2|315 SLOPE 2|316 TTEST 4|317 PROB|318 DEVSQ|319 GEOMEAN|320 HARMEAN|321 SUMSQ|322 KURT|323 SKEW|324 ZTEST|325 LARGE 2|326 SMALL 2|327 QUARTILE 2|328 PERCENTILE 2|329 PERCENTRANK|330 MODE|331 TRIMMEAN 2|332 TINV 2|336 CONCATENATE|337 POWER 2|342 RADIANS 1|343 DEGREES 1|344 SUBTOTAL|345 SUMIF|346 COUNTIF 2|347 COUNTBLANK 1|350 ISPMT 4|351 DATEDIF 3|354 ROMAN|359 HYPERLINK|361 AVERAGEA|362 MAXA|363 MINA|364 STDEVPA|365 VARPA|366 STDEVA|367 VARA";
    const map = new Map();
    for (const item of source.split("|")) { const [id, name, argc] = item.split(" "); map.set(Number(id), { name, argc: argc === undefined ? -1 : Number(argc) }); }
    return map;
  })();
  class BiffReader {
    constructor(segments) { this.segments = segments; this.seg = 0; this.pos = 0; }
    get data() { return this.segments[this.seg]; }
    ensure() { while (this.seg < this.segments.length && this.pos >= this.data.length) { this.seg++; this.pos = 0; } return this.seg < this.segments.length; }
    u8() { if (!this.ensure()) throw new Error("eof"); return this.data[this.pos++]; }
    u16() { return this.u8() | (this.u8() << 8); }
    u32() { return (this.u16() | (this.u16() << 16)) >>> 0; }
    skip(n) { let left = n; while (left > 0) { if (!this.ensure()) return; const take = Math.min(left, this.data.length - this.pos); this.pos += take; left -= take; } }
    chars(count, high) {
      let out = "", left = count;
      while (left > 0) {
        if (this.pos >= this.data.length) { this.seg++; this.pos = 0; if (this.seg >= this.segments.length) break; high = this.data[this.pos++] & 1; }
        const available = high ? Math.floor((this.data.length - this.pos) / 2) : this.data.length - this.pos;
        const take = Math.min(left, available);
        if (high) for (let i = 0; i < take; i++) { out += String.fromCharCode(this.data[this.pos] | (this.data[this.pos + 1] << 8)); this.pos += 2; }
        else for (let i = 0; i < take; i++) out += String.fromCharCode(this.data[this.pos++]);
        left -= take;
        if (take === 0 && left > 0) { this.pos = this.data.length; }
      }
      return out;
    }
    richString() {
      const cch = this.u16(), flags = this.u8();
      const runs = flags & 0x08 ? this.u16() : 0;
      const ext = flags & 0x04 ? this.u32() : 0;
      const text = this.chars(cch, flags & 1);
      this.skip(runs * 4 + ext);
      return text;
    }
  }
  function biffString(data, offset, cchSize, biff8, decoder) {
    const cch = cchSize === 1 ? data[offset] : data[offset] | (data[offset + 1] << 8);
    let pos = offset + cchSize;
    if (!biff8) return { text: decoder.decode(data.subarray(pos, pos + cch)), next: pos + cch };
    const flags = data[pos++];
    let runs = 0, ext = 0;
    if (flags & 0x08) { runs = data[pos] | (data[pos + 1] << 8); pos += 2; }
    if (flags & 0x04) { ext = (data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24)) >>> 0; pos += 4; }
    let text = "";
    if (flags & 1) { for (let i = 0; i < cch && pos + 1 < data.length; i++, pos += 2) text += String.fromCharCode(data[pos] | (data[pos + 1] << 8)); }
    else { for (let i = 0; i < cch && pos < data.length; i++) text += String.fromCharCode(data[pos++]); }
    return { text, next: pos + runs * 4 + ext };
  }
  function rkValue(rk) {
    let value;
    if (rk & 2) value = rk >> 2;
    else { const buf = new DataView(new ArrayBuffer(8)); buf.setUint32(4, rk & 0xFFFFFFFC, true); buf.setUint32(0, 0, true); value = buf.getFloat64(0, true); }
    return rk & 1 ? value / 100 : value;
  }
  function readXls(bytes) {
    const cfb = readCfb(bytes);
    if (!cfb) throw new Error("This file is not an Excel 97–2003 workbook.");
    if (cfb.entries.some((e) => e.name === "EncryptedPackage")) throw new Error("This workbook is protected with a password. Remove the password in Excel and try again.");
    const stream = cfb.stream("Workbook") || cfb.stream("Book");
    if (!stream) throw new Error("This .xls file holds no workbook.");
    const view = new DataView(stream.buffer, stream.byteOffset, stream.byteLength);
    const records = [];
    for (let pos = 0; pos + 4 <= stream.length;) {
      const type = view.getUint16(pos, true), len = view.getUint16(pos + 2, true);
      records.push({ type, offset: pos, data: stream.subarray(pos + 4, Math.min(stream.length, pos + 4 + len)) });
      pos += 4 + len;
    }
    const wb = createWorkbook();
    wb.sheets = [];
    let biff8 = true, date1904 = false, decoder = new TextDecoder("latin1"), activeTab = 0;
    const fonts = [], formats = new Map(), xfs = [], boundsheets = [], shared = [], names = [], supbooks = [], externSheets = [];
    const palette = PALETTE.slice();
    const u16 = (d, o) => d[o] | (d[o + 1] << 8);
    const u32 = (d, o) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
    const f64 = (d, o) => new DataView(d.buffer, d.byteOffset + o, 8).getFloat64(0, true);
    const colorAt = (i) => (i >= 8 && i < 64 ? palette[i] : i < 8 ? palette[i] : "");
    let index = 0;
    const continues = (i) => { const out = []; for (let j = i + 1; j < records.length && records[j].type === 0x003C; j++) out.push(records[j].data); return out; };
    for (; index < records.length; index++) {
      const { type, data } = records[index];
      if (type === 0x0809 && index > 0) break;
      if (type === 0x0809) { biff8 = u16(data, 0) === 0x0600; continue; }
      if (type === 0x002F) throw new Error("This workbook is protected with a password. Remove the password in Excel and try again.");
      if (type === 0x0042) { const cp = u16(data, 0); try { decoder = new TextDecoder(cp === 1200 ? "utf-16le" : cp === 10000 ? "macintosh" : `windows-${cp}`); } catch { decoder = new TextDecoder("latin1"); } }
      else if (type === 0x0022) date1904 = u16(data, 0) === 1;
      else if (type === 0x003D) activeTab = u16(data, 10);
      else if (type === 0x0085) {
        const pos = u32(data, 0), state = data[4], kind = data[5];
        const name = biff8 ? biffString(data, 6, 1, true, decoder).text : decoder.decode(data.subarray(7, 7 + data[6]));
        boundsheets.push({ pos, hidden: state !== 0, kind, name });
      } else if (type === 0x0031) {
        const height = u16(data, 0) / 20, grbit = u16(data, 2), icv = u16(data, 4), bls = u16(data, 6), uls = data[10];
        const name = biff8 ? biffString(data, 14, 1, true, decoder).text : decoder.decode(data.subarray(15, 15 + data[14]));
        fonts.push({ size: height, i: !!(grbit & 2), s: !!(grbit & 8), color: icv, b: bls >= 700, u: uls > 0, name });
      } else if (type === 0x041E) {
        const id = u16(data, 0);
        const code = biff8 ? biffString(data, 2, 2, true, decoder).text : decoder.decode(data.subarray(3, 3 + data[2]));
        formats.set(id, code);
      } else if (type === 0x00E0) xfs.push(data);
      else if (type === 0x0092) { const n = u16(data, 0); for (let i = 0; i < n && 2 + i * 4 + 3 <= data.length; i++) palette[8 + i] = toHex([data[2 + i * 4], data[3 + i * 4], data[4 + i * 4]]); }
      else if (type === 0x00FC) {
        const reader = new BiffReader([data, ...continues(index)]);
        reader.u32();
        const unique = reader.u32();
        for (let i = 0; i < unique; i++) { try { shared.push(reader.richString()); } catch { break; } }
      } else if (type === 0x01AE) {
        const ctab = u16(data, 0), cch = u16(data, 2);
        supbooks.push({ internal: cch === 0x0401, addin: cch === 0x3A01, ctab, names: [] });
      } else if (type === 0x0023 && supbooks.length) {
        const name = biff8 ? biffString(data, 6, 1, true, decoder).text : "";
        supbooks[supbooks.length - 1].names.push(name.replace(/^_xlfn\./i, ""));
      } else if (type === 0x0017 && biff8) {
        const n = u16(data, 0);
        for (let i = 0; i < n; i++) externSheets.push({ book: u16(data, 2 + i * 6), first: u16(data, 4 + i * 6), last: u16(data, 6 + i * 6) });
      } else if (type === 0x0018) {
        const grbit = u16(data, 0), cch = data[3], cce = u16(data, 4), itab = u16(data, 8);
        let name = "", next = 14;
        if (biff8) {
          const high = data[14] & 1;
          next = 15;
          for (let i = 0; i < cch; i++) { name += high ? String.fromCharCode(u16(data, next)) : String.fromCharCode(data[next]); next += high ? 2 : 1; }
        } else { name = decoder.decode(data.subarray(14, 14 + cch)); next = 14 + cch; }
        if (grbit & 0x20) name = { 0: "Consolidate_Area", 1: "Auto_Open", 2: "Auto_Close", 3: "Extract", 4: "Database", 5: "Criteria", 6: "Print_Area", 7: "Print_Titles", 8: "Recorder", 9: "Data_Form", 10: "Auto_Activate", 11: "Auto_Deactivate", 12: "Sheet_Title", 13: "_FilterDatabase" }[name.charCodeAt(0)] || name;
        names.push({ name, builtin: !!(grbit & 0x20), hidden: !!(grbit & 1), itab, rgce: data.subarray(next, next + cce) });
      }
    }
    const sheetNames = boundsheets.map((b) => b.name);
    const baseFont = fonts[0] || { name: "Arial", size: 10 };
    wb.defaultFont = baseFont.name || "Arial";
    wb.defaultSize = baseFont.size || 10;
    const styleCache = new Map();
    const styleFor = (ixfe) => {
      if (styleCache.has(ixfe)) return styleCache.get(ixfe);
      const data = xfs[ixfe];
      let id = 0;
      if (data) {
        const style = {};
        const ifnt = u16(data, 0), ifmt = u16(data, 2);
        const font = fonts[ifnt < 4 ? ifnt : ifnt - 1];
        if (font) {
          if (font.name && font.name !== wb.defaultFont) style.font = font.name;
          if (font.size && font.size !== wb.defaultSize) style.size = font.size;
          if (font.b) style.b = true;
          if (font.i) style.i = true;
          if (font.u) style.u = true;
          if (font.s) style.s = true;
          const color = font.color !== 0x7FFF && font.color < 64 ? colorAt(font.color) : "";
          if (color && color !== "#000000") style.color = color;
        }
        const code = formats.get(ifmt) || BUILTIN_FORMATS[ifmt];
        if (code && code !== "General") style.fmt = code;
        if (biff8 && data.length >= 20) {
          const align = data[6];
          const h = ["", "left", "center", "right", "left", "justify", "center", "center"][align & 7];
          if (h) style.h = h;
          if (align & 8) style.wrap = true;
          const v = ["top", "middle", "bottom", "middle", "middle"][(align >> 4) & 7];
          if (v && v !== "bottom") style.v = v;
          if (data[7] && data[7] !== 255) style.rot = data[7];
          if (data[8] & 15) style.indent = data[8] & 15;
          const b1 = u32(data, 10), b2 = u32(data, 14), b3 = u16(data, 18);
          const edge = (styleCode, icv) => (BIFF_BORDERS[styleCode] ? { w: BIFF_BORDERS[styleCode], c: (icv < 64 && colorAt(icv)) || "#000000" } : null);
          const left = edge(b1 & 15, (b1 >>> 16) & 127), right = edge((b1 >>> 4) & 15, (b1 >>> 23) & 127);
          const top = edge((b1 >>> 8) & 15, b2 & 127), bottom = edge((b1 >>> 12) & 15, (b2 >>> 7) & 127);
          if (left) style.bl = left;
          if (right) style.br = right;
          if (top) style.bt = top;
          if (bottom) style.bb = bottom;
          const pattern = (b2 >>> 26) & 63, fore = b3 & 127, back = (b3 >>> 7) & 127;
          if (pattern === 1 && fore < 64) style.fill = colorAt(fore);
          else if (pattern > 1 && fore < 64) style.fill = back < 64 ? mixColors(colorAt(fore), colorAt(back), 0.5) : colorAt(fore);
        }
        id = internStyle(wb, style);
      }
      styleCache.set(ixfe, id);
      return id;
    };
    const ptgSheet = (ixti) => {
      const x = externSheets[ixti];
      if (!x) return undefined;
      const book = supbooks[x.book];
      if (book && !book.internal) return undefined;
      if (x.first === 0xFFFE) return null;
      if (x.first !== x.last || x.first >= sheetNames.length) return undefined;
      return sheetNames[x.first];
    };
    const refText = (r, c, rowRel, colRel) => `${colRel ? "" : "$"}${colName(c)}${rowRel ? "" : "$"}${r + 1}`;
    const areaText = (r1, r2, c1, c2, f1, f2) => {
      if (r1 === 0 && r2 === 65535) return `${f1.col ? "" : "$"}${colName(c1)}:${f2.col ? "" : "$"}${colName(c2)}`;
      if (c1 === 0 && c2 === 255) return `${f1.row ? "" : "$"}${r1 + 1}:${f2.row ? "" : "$"}${r2 + 1}`;
      return `${refText(r1, c1, f1.row, f1.col)}:${refText(r2, c2, f2.row, f2.col)}`;
    };
    const decompile = (rgce, baseRow, baseCol) => {
      const stack = [];
      let i = 0;
      const pop = () => { if (!stack.length) throw new Error("stack"); return stack.pop(); };
      const relRow = (rw, rel) => (rel ? (baseRow + ((rw << 16) >> 16) + 65536) % 65536 : rw);
      const relCol = (col, rel) => (rel ? (baseCol + ((col << 24) >> 24) + 256) % 256 : col);
      try {
        while (i < rgce.length) {
          const ptg = rgce[i++];
          if (ptg >= 0x03 && ptg <= 0x11) {
            const b = pop(), a = pop();
            const op = { 0x03: "+", 0x04: "-", 0x05: "*", 0x06: "/", 0x07: "^", 0x08: "&", 0x09: "<", 0x0A: "<=", 0x0B: "=", 0x0C: ">=", 0x0D: ">", 0x0E: "<>", 0x0F: " ", 0x10: ",", 0x11: ":" }[ptg];
            if (ptg === 0x0F || ptg === 0x10) return null;
            stack.push(a + op + b);
            continue;
          }
          switch (ptg) {
            case 0x12: stack.push("+" + pop()); continue;
            case 0x13: stack.push("-" + pop()); continue;
            case 0x14: stack.push(pop() + "%"); continue;
            case 0x15: stack.push("(" + pop() + ")"); continue;
            case 0x16: stack.push(""); continue;
            case 0x17: { const s = biffString(rgce, i, 1, true, decoder); i = s.next; stack.push(`"${s.text.replace(/"/g, '""')}"`); continue; }
            case 0x19: {
              const grbit = rgce[i];
              if (grbit & 0x04) { const n = u16(rgce, i + 1); i += 3 + (n + 1) * 2; continue; }
              if (grbit & 0x10) { stack.push(`SUM(${pop()})`); }
              i += 3;
              continue;
            }
            case 0x1C: stack.push(BIFF_ERRORS[rgce[i]] || "#N/A"); i += 1; continue;
            case 0x1D: stack.push(rgce[i] ? "TRUE" : "FALSE"); i += 1; continue;
            case 0x1E: stack.push(String(u16(rgce, i))); i += 2; continue;
            case 0x1F: { const n = f64(rgce, i); stack.push(numberLiteral(n)); i += 8; continue; }
            default: break;
          }
          const base = ptg >= 0x20 ? (ptg & 0x1F) | 0x20 : ptg;
          switch (base) {
            case 0x21: {
              const id = u16(rgce, i); i += 2;
              const fn = FTAB.get(id);
              if (!fn || fn.argc < 0) return null;
              const args = [];
              for (let k = 0; k < fn.argc; k++) args.unshift(pop());
              stack.push(`${fn.name}(${args.join(",")})`);
              break;
            }
            case 0x22: {
              const argc = rgce[i] & 0x7F, id = u16(rgce, i + 1) & 0x7FFF; i += 3;
              const args = [];
              for (let k = 0; k < argc; k++) args.unshift(pop());
              if (id === 255) { const name = args.shift(); stack.push(`${String(name).replace(/^_xlfn\./i, "")}(${args.join(",")})`); break; }
              const fn = FTAB.get(id);
              if (!fn) return null;
              stack.push(`${fn.name}(${args.join(",")})`);
              break;
            }
            case 0x23: { const n = u16(rgce, i); i += 4; const def = names[n - 1]; if (!def) return null; stack.push(def.name); break; }
            case 0x24: case 0x2C: {
              const rw = u16(rgce, i), col = u16(rgce, i + 2); i += 4;
              const rowRel = !!(col & 0x8000), colRel = !!(col & 0x4000);
              const shared = base === 0x2C;
              stack.push(refText(shared ? relRow(rw, rowRel) : rw, shared ? relCol(col & 0xFF, colRel) : col & 0x3FFF, rowRel, colRel));
              break;
            }
            case 0x25: case 0x2D: {
              const r1 = u16(rgce, i), r2 = u16(rgce, i + 2), c1 = u16(rgce, i + 4), c2 = u16(rgce, i + 6); i += 8;
              const f1 = { row: !!(c1 & 0x8000), col: !!(c1 & 0x4000) }, f2 = { row: !!(c2 & 0x8000), col: !!(c2 & 0x4000) };
              const shared = base === 0x2D;
              stack.push(areaText(shared ? relRow(r1, f1.row) : r1, shared ? relRow(r2, f2.row) : r2, shared ? relCol(c1 & 0xFF, f1.col) : c1 & 0x3FFF, shared ? relCol(c2 & 0xFF, f2.col) : c2 & 0x3FFF, f1, f2));
              break;
            }
            case 0x26: case 0x27: case 0x28: i += 6; break;
            case 0x29: i += 2; break;
            case 0x2A: i += 4; stack.push("#REF!"); break;
            case 0x2B: i += 8; stack.push("#REF!"); break;
            case 0x39: {
              const ixti = u16(rgce, i), n = u16(rgce, i + 2); i += 6;
              const x = externSheets[ixti], book = x ? supbooks[x.book] : null;
              if (book && book.internal) { const def = names[n - 1]; if (!def) return null; stack.push(def.name); }
              else if (book && book.names[n - 1]) stack.push(book.names[n - 1]);
              else return null;
              break;
            }
            case 0x3A: {
              const ixti = u16(rgce, i), rw = u16(rgce, i + 2), col = u16(rgce, i + 4); i += 6;
              const sheet = ptgSheet(ixti);
              if (sheet === undefined) return null;
              stack.push((sheet ? quoteSheet(sheet) + "!" : "") + refText(rw, col & 0x3FFF, !!(col & 0x8000), !!(col & 0x4000)));
              break;
            }
            case 0x3B: {
              const ixti = u16(rgce, i), r1 = u16(rgce, i + 2), r2 = u16(rgce, i + 4), c1 = u16(rgce, i + 6), c2 = u16(rgce, i + 8); i += 10;
              const sheet = ptgSheet(ixti);
              if (sheet === undefined) return null;
              stack.push((sheet ? quoteSheet(sheet) + "!" : "") + areaText(r1, r2, c1 & 0x3FFF, c2 & 0x3FFF, { row: !!(c1 & 0x8000), col: !!(c1 & 0x4000) }, { row: !!(c2 & 0x8000), col: !!(c2 & 0x4000) }));
              break;
            }
            case 0x3C: i += 6; stack.push("#REF!"); break;
            case 0x3D: i += 10; stack.push("#REF!"); break;
            default: return null;
          }
        }
      } catch { return null; }
      return stack.length === 1 ? stack[0] : null;
    };
    if (biff8) {
      for (const def of names) {
        if (def.builtin || def.hidden || !def.rgce.length) continue;
        const ref = decompile(def.rgce, 0, 0);
        if (ref) wb.names.set(def.name.toUpperCase(), { name: def.name, ref, sheet: def.itab ? sheetNames[def.itab - 1] || "" : "" });
      }
    }
    const byOffset = new Map(boundsheets.map((b) => [b.pos, b]));
    const used = new Set();
    for (; index < records.length; index++) {
      const start = records[index];
      if (start.type !== 0x0809) continue;
      const info = byOffset.get(start.offset);
      const kind = u16(start.data, 2);
      if (!info || kind !== 0x0010 || info.kind !== 0) {
        let depth = 0;
        for (index++; index < records.length; index++) { if (records[index].type === 0x0809) depth++; if (records[index].type === 0x000A) { if (!depth) break; depth--; } }
        continue;
      }
      let name = validSheetName(info.name) ? info.name : `Sheet${wb.sheets.length + 1}`;
      while (used.has(name.toLowerCase())) name += " (2)";
      used.add(name.toLowerCase());
      const sheet = createSheet(name, { defaultColWidth: 64, defaultRowHeight: 17 });
      sheet.hidden = info.hidden;
      const pending = [], sharedFormulas = [], arrays = [];
      let lastFormula = null;
      const put = (r, c, value, ixfe) => { const cell = makeCell(value, undefined, styleFor(ixfe)); if (cell) sheet.cells.set(cellKey(r, c), cell); else sheet.cells.delete(cellKey(r, c)); };
      for (index++; index < records.length; index++) {
        const { type, data } = records[index];
        if (type === 0x000A) break;
        if (type === 0x0809) { let depth = 1; for (index++; index < records.length && depth; index++) { if (records[index].type === 0x0809) depth++; if (records[index].type === 0x000A) depth--; } index--; continue; }
        switch (type) {
          case 0x0203: put(u16(data, 0), u16(data, 2), f64(data, 6), u16(data, 4)); break;
          case 0x027E: put(u16(data, 0), u16(data, 2), rkValue(u32(data, 6)), u16(data, 4)); break;
          case 0x00BD: {
            const r = u16(data, 0), first = u16(data, 2), last = u16(data, data.length - 2);
            for (let c = first, o = 4; c <= last && o + 6 <= data.length - 2; c++, o += 6) put(r, c, rkValue(u32(data, o + 2)), u16(data, o));
            break;
          }
          case 0x00FD: put(u16(data, 0), u16(data, 2), shared[u32(data, 6)] ?? "", u16(data, 4)); break;
          case 0x0204: case 0x00D6: { const s = biffString(data, 6, 2, biff8, decoder); put(u16(data, 0), u16(data, 2), s.text, u16(data, 4)); break; }
          case 0x0205: put(u16(data, 0), u16(data, 2), data[7] ? err(BIFF_ERRORS[data[6]] || "#N/A") : !!data[6], u16(data, 4)); break;
          case 0x0201: put(u16(data, 0), u16(data, 2), undefined, u16(data, 4)); break;
          case 0x00BE: {
            const r = u16(data, 0), first = u16(data, 2), last = u16(data, data.length - 2);
            for (let c = first, o = 4; c <= last && o + 2 <= data.length - 2; c++, o += 2) put(r, c, undefined, u16(data, o));
            break;
          }
          case 0x0006: {
            const r = u16(data, 0), c = u16(data, 2), ixfe = u16(data, 4);
            let value;
            if (u16(data, 12) === 0xFFFF) {
              const kindCode = data[6];
              value = kindCode === 0 ? "" : kindCode === 1 ? !!data[8] : kindCode === 2 ? err(BIFF_ERRORS[data[8]] || "#N/A") : "";
            } else value = f64(data, 6);
            const cce = u16(data, 20);
            const rgce = data.subarray(22, 22 + cce);
            const entry = { r, c, ixfe, value, rgce };
            pending.push(entry);
            lastFormula = u16(data, 12) === 0xFFFF && data[6] === 0 ? entry : null;
            break;
          }
          case 0x0207: if (lastFormula) { lastFormula.value = biffString(data, 0, 2, biff8, decoder).text; lastFormula = null; } break;
          case 0x04BC: sharedFormulas.push({ r1: u16(data, 0), r2: u16(data, 2), c1: data[4], c2: data[5], rgce: data.subarray(10, 10 + u16(data, 8)) }); break;
          case 0x0221: arrays.push({ r1: u16(data, 0), r2: u16(data, 2), c1: data[4], c2: data[5], rgce: data.subarray(14, 14 + u16(data, 12)) }); break;
          case 0x0208: {
            const r = u16(data, 0), height = u16(data, 6) & 0x7FFF, grbit = u16(data, 12);
            if (grbit & 0x40) sheet.rowHeights.set(r, Math.round((height / 20) * 4 / 3));
            if (grbit & 0x20) sheet.hiddenRows.add(r);
            break;
          }
          case 0x007D: {
            const first = u16(data, 0), last = Math.min(u16(data, 2), first + 300), width = Math.round((u16(data, 4) / 256) * 7), hidden = u16(data, 8) & 1;
            for (let c = first; c <= last && c < MAX_COLS; c++) { if (width !== sheet.defaultColWidth) sheet.colWidths.set(c, width); if (hidden) sheet.hiddenCols.add(c); }
            break;
          }
          case 0x0055: sheet.defaultColWidth = Math.max(8, u16(data, 0) * 7 + 5); break;
          case 0x0099: sheet.defaultColWidth = Math.max(8, Math.round((u16(data, 0) / 256) * 7)); break;
          case 0x0225: sheet.defaultRowHeight = Math.max(4, Math.round((u16(data, 2) / 20) * 4 / 3)); break;
          case 0x00E5: { const n = u16(data, 0); for (let i = 0; i < n; i++) { const o = 2 + i * 8; if (o + 8 <= data.length) sheet.merges.push({ r1: u16(data, o), r2: u16(data, o + 2), c1: u16(data, o + 4), c2: u16(data, o + 6) }); } break; }
          case 0x023E: { const grbit = u16(data, 0); if (!(grbit & 0x0002)) sheet.showGrid = false; sheet.frozen = !!(grbit & 0x0008); break; }
          case 0x0041: if (sheet.frozen) { sheet.freezeCols = Math.min(200, u16(data, 0)); sheet.freezeRows = Math.min(1000, u16(data, 2)); } break;
          case 0x0862: if (data.length >= 20) { const icv = u32(data, 16) & 0x7F; if (icv < 64 && icv !== 0x7F) sheet.color = colorAt(icv); } break;
          default: break;
        }
      }
      delete sheet.frozen;
      const sharedAt = (r, c) => sharedFormulas.find((s) => r >= s.r1 && r <= s.r2 && c >= s.c1 && c <= s.c2);
      for (const entry of pending) {
        let formula = null;
        if (biff8 && entry.rgce[0] === 0x01) {
          const mr = u16(entry.rgce, 1), mc = u16(entry.rgce, 3);
          const array = arrays.find((a) => a.r1 === mr && a.c1 === mc);
          if (array) {
            if (entry.r === mr && entry.c === mc) formula = decompile(array.rgce, mr, mc);
            else { put(entry.r, entry.c, undefined, entry.ixfe); continue; }
          } else {
            const share = sharedAt(entry.r, entry.c) || sharedFormulas.find((s) => s.r1 === mr && s.c1 === mc);
            if (share) formula = decompile(share.rgce, entry.r, entry.c);
          }
        } else if (biff8) formula = decompile(entry.rgce, entry.r, entry.c);
        const cell = makeCell(entry.value, formula || undefined, styleFor(entry.ixfe));
        if (cell) sheet.cells.set(cellKey(entry.r, entry.c), cell);
      }
      if (date1904) adjust1904(wb, sheet);
      finishSheetSize(sheet);
      wb.sheets.push(sheet);
    }
    if (!wb.sheets.length) throw new Error("This .xls file holds no worksheets.");
    wb.active = Math.max(0, Math.min(wb.sheets.length - 1, activeTab));
    return wb;
  }

  const SPREADSHEETML_FORMATS = { "General Number": "General", General: "General", Fixed: "0.00", Standard: "#,##0.00", Percent: "0.00%", Scientific: "0.00E+00", Currency: '"$"#,##0.00', "Euro Currency": '"€"#,##0.00', "Short Date": "m/d/yyyy", "Medium Date": "d-mmm-yy", "Long Date": "dddd, mmmm d, yyyy", "General Date": "m/d/yyyy h:mm", "Short Time": "h:mm", "Medium Time": "h:mm AM/PM", "Long Time": "h:mm:ss", "Yes/No": '"Yes";"Yes";"No"', "True/False": '"True";"True";"False"', "On/Off": '"On";"On";"Off"', "@": "@" };
  function r1c1ToA1(formula, r, c) {
    let out = "", quoted = false;
    const re = /(?<![A-Za-z0-9_.$])R(\[-?\d+\]|\d+)?C(\[-?\d+\]|\d+)?(?![A-Za-z0-9_(])/g;
    let last = 0, m;
    while ((m = re.exec(formula))) {
      for (let i = last; i < m.index; i++) if (formula[i] === '"') quoted = !quoted;
      out += formula.slice(last, m.index);
      last = m.index + m[0].length;
      if (quoted) { out += m[0]; continue; }
      const part = (text, base) => (text === undefined ? [base, false] : text.startsWith("[") ? [base + Number(text.slice(1, -1)), false] : [Number(text) - 1, true]);
      const [row, absRow] = part(m[1], r), [col, absCol] = part(m[2], c);
      if (row < 0 || col < 0) { out += "#REF!"; continue; }
      out += `${absCol ? "$" : ""}${colName(col)}${absRow ? "$" : ""}${row + 1}`;
    }
    return out + formula.slice(last);
  }
  function readSpreadsheetMl(text) {
    const root = kid(parseXml(text), "Workbook");
    if (!root) throw new Error("This file is not an Excel workbook.");
    const wb = createWorkbook();
    wb.sheets = [];
    const styles = new Map();
    for (const style of kids(kid(root, "Styles"), "Style")) {
      const out = {};
      const font = kid(style, "Font"), interior = kid(style, "Interior"), align = kid(style, "Alignment"), number = kid(style, "NumberFormat");
      if (font) {
        if (flag(font.attrs.Bold)) out.b = true;
        if (flag(font.attrs.Italic)) out.i = true;
        if (font.attrs.Underline && font.attrs.Underline !== "None") out.u = true;
        if (flag(font.attrs.StrikeThrough)) out.s = true;
        if (/^#[0-9a-f]{6}$/i.test(font.attrs.Color || "") && font.attrs.Color !== "#000000") out.color = font.attrs.Color.toUpperCase();
        if (font.attrs.FontName && font.attrs.FontName !== wb.defaultFont) out.font = font.attrs.FontName;
        if (font.attrs.Size && Number(font.attrs.Size) !== wb.defaultSize) out.size = Number(font.attrs.Size);
      }
      if (interior && /^#[0-9a-f]{6}$/i.test(interior.attrs.Color || "") && (interior.attrs.Pattern || "Solid") !== "None") out.fill = interior.attrs.Color.toUpperCase();
      if (align) {
        const h = { Left: "left", Center: "center", Right: "right", Justify: "justify" }[align.attrs.Horizontal];
        const v = { Top: "top", Center: "middle" }[align.attrs.Vertical];
        if (h) out.h = h;
        if (v) out.v = v;
        if (flag(align.attrs.WrapText)) out.wrap = true;
      }
      if (number && number.attrs.Format) { const code = SPREADSHEETML_FORMATS[number.attrs.Format] || number.attrs.Format; if (code !== "General") out.fmt = code; }
      for (const border of kids(kid(style, "Borders"), "Border")) {
        const key = { Left: "bl", Right: "br", Top: "bt", Bottom: "bb" }[border.attrs.Position];
        if (!key || !border.attrs.LineStyle || border.attrs.LineStyle === "None") continue;
        const weight = Number(border.attrs.Weight || 1);
        const w = border.attrs.LineStyle === "Double" ? "double" : border.attrs.LineStyle === "Dash" ? "dashed" : border.attrs.LineStyle === "Dot" ? "dotted" : weight >= 3 ? "thick" : weight === 2 ? "medium" : "thin";
        out[key] = { w, c: /^#[0-9a-f]{6}$/i.test(border.attrs.Color || "") ? border.attrs.Color.toUpperCase() : "#000000" };
      }
      styles.set(style.attrs.ID, internStyle(wb, out));
    }
    const used = new Set();
    for (const ws of kids(root, "Worksheet")) {
      let name = validSheetName(ws.attrs.Name) ? ws.attrs.Name : `Sheet${wb.sheets.length + 1}`;
      while (used.has(name.toLowerCase())) name += " (2)";
      used.add(name.toLowerCase());
      const sheet = createSheet(name, { defaultColWidth: 64, defaultRowHeight: 20 });
      const table = kid(ws, "Table");
      let colIndex2 = -1;
      for (const col of kids(table, "Column")) {
        colIndex2 = col.attrs.Index ? Number(col.attrs.Index) - 1 : colIndex2 + 1;
        const span = Number(col.attrs.Span || 0);
        for (let c = colIndex2; c <= colIndex2 + span; c++) {
          if (col.attrs.Width) sheet.colWidths.set(c, Math.round(Number(col.attrs.Width) * 4 / 3));
          if (flag(col.attrs.Hidden)) sheet.hiddenCols.add(c);
        }
        colIndex2 += span;
      }
      let r = -1;
      for (const row of kids(table, "Row")) {
        r = row.attrs.Index ? Number(row.attrs.Index) - 1 : r + 1;
        if (row.attrs.Height) sheet.rowHeights.set(r, Math.round(Number(row.attrs.Height) * 4 / 3));
        if (flag(row.attrs.Hidden)) sheet.hiddenRows.add(r);
        let c = -1;
        for (const cell of kids(row, "Cell")) {
          c = cell.attrs.Index ? Number(cell.attrs.Index) - 1 : c + 1;
          const data = kid(cell, "Data");
          const type = data?.attrs.Type;
          const raw = data ? textOf(data) : null;
          let value;
          if (type === "Number") value = Number(raw);
          else if (type === "Boolean") value = raw === "1";
          else if (type === "DateTime") { const d = new Date(raw + (/[zZ]|[+-]\d\d:?\d\d$/.test(raw) ? "" : "Z")); value = Number.isNaN(d.getTime()) ? raw : (d.getTime() - EPOCH) / DAY_MS; }
          else if (type === "Error") value = err(raw);
          else if (raw !== null) value = raw;
          const formula = cell.attrs.Formula ? r1c1ToA1(cell.attrs.Formula.replace(/^=/, ""), r, c) : undefined;
          const made = makeCell(value, formula, styles.get(cell.attrs.StyleID) || 0);
          if (made) sheet.cells.set(cellKey(r, c), made);
          const across = Number(cell.attrs.MergeAcross || 0), down = Number(cell.attrs.MergeDown || 0);
          if (across || down) sheet.merges.push({ r1: r, c1: c, r2: r + down, c2: c + across });
          if (cell.attrs.HRef) sheet.links.set(cellKey(r, c), cell.attrs.HRef);
          const comment = kid(cell, "Comment");
          if (comment) sheet.notes.set(cellKey(r, c), textOf(comment).trim());
          c += across;
        }
      }
      const options = kid(ws, "WorksheetOptions");
      if (kid(options, "FreezePanes")) {
        sheet.freezeRows = Number(textOf(kid(options, "SplitHorizontal")) || 0);
        sheet.freezeCols = Number(textOf(kid(options, "SplitVertical")) || 0);
      }
      if (kid(options, "DoNotDisplayGridlines")) sheet.showGrid = false;
      finishSheetSize(sheet);
      wb.sheets.push(sheet);
    }
    if (!wb.sheets.length) wb.sheets.push(createSheet("Sheet1"));
    return wb;
  }

  function detectDelimiter(text) {
    const sample = text.slice(0, 20000).split(/\r?\n/).slice(0, 20).filter(Boolean);
    let best = ",", bestScore = -1;
    for (const d of [",", ";", "\t", "|"]) {
      const counts = sample.map((line) => { let n = 0, q = false; for (const ch of line) { if (ch === '"') q = !q; else if (ch === d && !q) n++; } return n; });
      const consistent = counts.filter((n) => n === counts[0]).length;
      const score = counts[0] ? consistent * 1000 + counts[0] : -1;
      if (score > bestScore) { best = d; bestScore = score; }
    }
    return best;
  }
  function parseCsv(text, delimiter) {
    const d = delimiter || detectDelimiter(text);
    const rows = [];
    let row = [], field = "", i = 0, quoted = false;
    const source = text.replace(/^﻿/, "");
    while (i < source.length) {
      const ch = source[i];
      if (quoted) {
        if (ch === '"') { if (source[i + 1] === '"') { field += '"'; i += 2; continue; } quoted = false; i++; continue; }
        field += ch; i++; continue;
      }
      if (ch === '"' && field === "") { quoted = true; i++; continue; }
      if (ch === d) { row.push(field); field = ""; i++; continue; }
      if (ch === "\r" || ch === "\n") { row.push(field); rows.push(row); row = []; field = ""; if (ch === "\r" && source[i + 1] === "\n") i++; i++; continue; }
      field += ch; i++;
    }
    if (field !== "" || row.length) { row.push(field); rows.push(row); }
    return rows;
  }
  function workbookFromRows(rows, name, options = {}) {
    const wb = createWorkbook();
    const sheet = wb.sheets[0];
    sheet.name = validSheetName(name) ? name : "Sheet1";
    rows.forEach((row, r) => row.forEach((text, c) => {
      if (r >= MAX_ROWS || c >= MAX_COLS) return;
      const parsed = parseInput(text, options);
      const s = parsed.format ? internStyle(wb, { fmt: parsed.format }) : 0;
      const cell = parsed.formula !== undefined ? makeCell(undefined, parsed.formula, s) : makeCell(parsed.value, undefined, s);
      if (cell) sheet.cells.set(cellKey(r, c), cell);
    }));
    finishSheetSize(sheet);
    return wb;
  }
  function sheetToCsv(wb, sheet, calc, delimiter = ",") {
    const used = usedRange(sheet);
    if (!used) return "";
    const lines = [];
    for (let r = 0; r <= used.r2; r++) {
      const fields = [];
      for (let c = 0; c <= used.c2; c++) {
        const text = displayText(wb, sheet, calc, r, c);
        fields.push(/["\r\n]/.test(text) || text.includes(delimiter) || /^\s|\s$/.test(text) ? `"${text.replace(/"/g, '""')}"` : text);
      }
      lines.push(fields.join(delimiter));
    }
    return lines.join("\r\n") + "\r\n";
  }
  function sniffFormat(bytes, filename = "") {
    const lower = filename.toLowerCase();
    if (bytes.length >= 8 && bytes[0] === 0xD0 && bytes[1] === 0xCF && bytes[2] === 0x11 && bytes[3] === 0xE0) return "xls";
    if (bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4B && bytes[2] === 0x03 && bytes[3] === 0x04) return "xlsx";
    const head = utf8.decode(bytes.subarray(0, 4096)).replace(/^﻿/, "").trimStart();
    if (/^<\?xml/i.test(head) && /urn:schemas-microsoft-com:office:spreadsheet/.test(head + utf8.decode(bytes.subarray(4096, 16384)))) return "spreadsheetml";
    if (/^</.test(head) && /<(html|table|!doctype html)/i.test(head + utf8.decode(bytes.subarray(4096, 65536)))) return "html";
    if (/\.(tsv|tab)$/.test(lower)) return "tsv";
    return "csv";
  }
  function decodeText(bytes) {
    if (bytes[0] === 0xFF && bytes[1] === 0xFE) return new TextDecoder("utf-16le").decode(bytes.subarray(2));
    if (bytes[0] === 0xFE && bytes[1] === 0xFF) return new TextDecoder("utf-16be").decode(bytes.subarray(2));
    try { return new TextDecoder("utf-8", { fatal: true }).decode(bytes); }
    catch { return new TextDecoder("windows-1252").decode(bytes); }
  }
  async function importWorkbook(bytes, filename = "", options = {}) {
    const kind = sniffFormat(bytes, filename);
    const base = filename.replace(/\.[^.]+$/, "").slice(0, 100) || "Sheet1";
    if (kind === "xls") return { wb: readXls(bytes), kind };
    if (kind === "xlsx") {
      const probe = readZip(bytes);
      if (!probe.has("[Content_Types].xml") && probe.has("content.xml")) return { wb: await readOds(bytes), kind: "ods" };
      return { wb: await readXlsx(bytes), kind };
    }
    if (kind === "spreadsheetml") return { wb: readSpreadsheetMl(decodeText(bytes)), kind };
    if (kind === "html") {
      if (!options.htmlTables) throw new Error("This file is a web page, not a workbook.");
      return { wb: options.htmlTables(decodeText(bytes), base), kind };
    }
    const text = decodeText(bytes);
    return { wb: workbookFromRows(parseCsv(text, kind === "tsv" ? "\t" : undefined), base, options), kind };
  }
  function bytesToBase64(bytes) {
    let out = "";
    for (let i = 0; i < bytes.length; i += 0x8000) out += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(out);
  }
  function base64ToBytes(text) {
    const raw = atob(text);
    const out = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
    return out;
  }
  async function packWorkbook(wb, calc) { return bytesToBase64(await gzipText(JSON.stringify(serializeWorkbook(wb, calc)))); }
  async function unpackWorkbook(text) { return deserializeWorkbook(JSON.parse(await gunzipText(base64ToBytes(text)))); }

  function writeCfb(stream) {
    const SECTOR = 512;
    const padded = new Uint8Array(Math.max(4096, Math.ceil(stream.length / SECTOR) * SECTOR));
    padded.set(stream);
    const streamSectors = padded.length / SECTOR;
    let fatSectors = 1;
    while (fatSectors * 128 < streamSectors + 1 + fatSectors) fatSectors++;
    if (fatSectors > 109) throw new Error("This workbook is too large for the Excel 97–2003 format. Download it as .xlsx instead.");
    const dirSector = fatSectors, firstStream = fatSectors + 1;
    const out = new Uint8Array(SECTOR * (1 + fatSectors + 1 + streamSectors));
    const v = new DataView(out.buffer);
    [0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1].forEach((b, i) => { out[i] = b; });
    v.setUint16(0x18, 0x003E, true); v.setUint16(0x1A, 3, true); v.setUint16(0x1C, 0xFFFE, true);
    v.setUint16(0x1E, 9, true); v.setUint16(0x20, 6, true);
    v.setUint32(0x2C, fatSectors, true); v.setUint32(0x30, dirSector, true); v.setUint32(0x38, 4096, true);
    v.setUint32(0x3C, 0xFFFFFFFE, true); v.setUint32(0x40, 0, true); v.setUint32(0x44, 0xFFFFFFFE, true); v.setUint32(0x48, 0, true);
    for (let i = 0; i < 109; i++) v.setUint32(0x4C + i * 4, i < fatSectors ? i : 0xFFFFFFFF, true);
    const fatBase = SECTOR;
    for (let i = 0; i < fatSectors * 128; i++) {
      let value = 0xFFFFFFFF;
      if (i < fatSectors) value = 0xFFFFFFFD;
      else if (i === dirSector) value = 0xFFFFFFFE;
      else if (i >= firstStream && i < firstStream + streamSectors) value = i === firstStream + streamSectors - 1 ? 0xFFFFFFFE : i + 1;
      v.setUint32(fatBase + i * 4, value, true);
    }
    const dirBase = SECTOR * (1 + dirSector);
    const entry = (index, name, type, child, start, size) => {
      const o = dirBase + index * 128;
      for (let i = 0; i < name.length; i++) v.setUint16(o + i * 2, name.charCodeAt(i), true);
      v.setUint16(o + 64, name ? (name.length + 1) * 2 : 0, true);
      out[o + 66] = type; out[o + 67] = 1;
      v.setUint32(o + 68, 0xFFFFFFFF, true); v.setUint32(o + 72, 0xFFFFFFFF, true); v.setUint32(o + 76, child, true);
      v.setUint32(o + 116, start, true); v.setUint32(o + 120, size, true);
    };
    entry(0, "Root Entry", 5, 1, 0xFFFFFFFE, 0);
    entry(1, "Workbook", 2, 0xFFFFFFFF, firstStream, padded.length);
    entry(2, "", 0, 0xFFFFFFFF, 0, 0);
    entry(3, "", 0, 0xFFFFFFFF, 0, 0);
    out.set(padded, SECTOR * (1 + firstStream));
    return out;
  }
  class ByteWriter {
    constructor() { this.chunks = []; this.length = 0; }
    record(type, body) {
      const head = new Uint8Array(4);
      head[0] = type & 255; head[1] = type >> 8; head[2] = body.length & 255; head[3] = body.length >> 8;
      this.chunks.push(head, body); this.length += 4 + body.length;
    }
    bytes() { const out = new Uint8Array(this.length); let at = 0; for (const c of this.chunks) { out.set(c, at); at += c.length; } return out; }
  }
  function bytes(...parts) {
    const list = [];
    for (const [kind, value] of parts) {
      if (kind === 8) list.push(value & 255);
      else if (kind === 16) list.push(value & 255, (value >> 8) & 255);
      else if (kind === 32) list.push(value & 255, (value >>> 8) & 255, (value >>> 16) & 255, (value >>> 24) & 255);
      else if (kind === "f") { const b = new Uint8Array(new Float64Array([value]).buffer); list.push(...b); }
      else if (kind === "raw") list.push(...value);
    }
    return new Uint8Array(list);
  }
  const utf16 = (text) => { const out = []; for (let i = 0; i < text.length; i++) { const c = text.charCodeAt(i); out.push(c & 255, c >> 8); } return out; };
  const shortString = (text) => { const s = String(text).slice(0, 255); return [s.length, 1, ...utf16(s)]; };
  const longString = (text) => { const s = String(text).slice(0, 32767); return [s.length & 255, s.length >> 8, 1, ...utf16(s)]; };
  const FTAB_BY_NAME = new Map([...FTAB].map(([id, fn]) => [fn.name, { id, argc: fn.argc }]));
  const BIFF_ERROR_CODES = Object.fromEntries(Object.entries(BIFF_ERRORS).map(([code, text]) => [text, Number(code)]));
  function compileBiffFormula(text, sheetName, sheetIndex) {
    const out = [];
    const flags = (rowRel, colRel) => (rowRel ? 0x8000 : 0) | (colRel ? 0x4000 : 0);
    const sheetOf = (name) => {
      if (!name || name.toLowerCase() === sheetName.toLowerCase()) return -1;
      const i = sheetIndex.get(name.toLowerCase());
      if (i === undefined) throw new Error("sheet");
      return i;
    };
    const emit = (node, asArg) => {
      switch (node.t) {
        case "n": if (Number.isInteger(node.v) && node.v >= 0 && node.v <= 65535) out.push(0x1E, node.v & 255, node.v >> 8); else out.push(0x1F, ...new Uint8Array(new Float64Array([node.v]).buffer)); return;
        case "s": if (node.v.length > 255) throw new Error("text"); out.push(0x17, ...shortString(node.v)); return;
        case "b": out.push(0x1D, node.v ? 1 : 0); return;
        case "e": if (BIFF_ERROR_CODES[node.v] === undefined) throw new Error("error"); out.push(0x1C, BIFF_ERROR_CODES[node.v]); return;
        case "miss": out.push(0x16); return;
        case "paren": emit(node.a, false); out.push(0x15); return;
        case "neg": emit(node.a, false); out.push(0x13); return;
        case "pos": emit(node.a, false); out.push(0x12); return;
        case "pct": emit(node.a, false); out.push(0x14); return;
        case "op": {
          emit(node.a, false); emit(node.b, false);
          const code = { "+": 0x03, "-": 0x04, "*": 0x05, "/": 0x06, "^": 0x07, "&": 0x08, "<": 0x09, "<=": 0x0A, "=": 0x0B, ">=": 0x0C, ">": 0x0D, "<>": 0x0E, ":": 0x11 }[node.op];
          if (!code) throw new Error("op");
          out.push(code);
          return;
        }
        case "ref": {
          if (node.r > 65535 || node.c > 255) throw new Error("bounds");
          const sheet = sheetOf(node.sheet);
          const col = node.c | flags(!node.ar, !node.ac);
          if (sheet < 0) out.push(asArg ? 0x24 : 0x44, node.r & 255, node.r >> 8, col & 255, col >> 8);
          else out.push(asArg ? 0x3A : 0x5A, sheet & 255, sheet >> 8, node.r & 255, node.r >> 8, col & 255, col >> 8);
          return;
        }
        case "rng": {
          const r1 = Math.min(node.r1, 65535), r2 = Math.min(node.r2, 65535);
          if (node.kind !== "rows" && (node.c1 > 255 || node.c2 > 255)) throw new Error("bounds");
          if (node.kind === "cells" && (node.r1 > 65535 || node.r2 > 65535)) throw new Error("bounds");
          const c1 = Math.min(node.c1, 255) | flags(!node.ar1 && node.kind !== "cols", !node.ac1 && node.kind !== "rows");
          const c2 = Math.min(node.c2, 255) | flags(!node.ar2 && node.kind !== "cols", !node.ac2 && node.kind !== "rows");
          const sheet = sheetOf(node.sheet);
          const body = [r1 & 255, r1 >> 8, r2 & 255, r2 >> 8, c1 & 255, c1 >> 8, c2 & 255, c2 >> 8];
          if (sheet < 0) out.push(0x25, ...body); else out.push(0x3B, sheet & 255, sheet >> 8, ...body);
          return;
        }
        case "fn": {
          const fn = FTAB_BY_NAME.get(node.name);
          if (!fn) throw new Error("function");
          node.args.forEach((a) => emit(a, true));
          if (fn.argc >= 0 && fn.argc === node.args.length) out.push(0x41, fn.id & 255, fn.id >> 8);
          else if (fn.argc >= 0) throw new Error("argc");
          else out.push(0x42, node.args.length, fn.id & 255, fn.id >> 8);
          return;
        }
        default: throw new Error("unsupported");
      }
    };
    try { emit(parseFormula(text), false); } catch { return null; }
    return out.length > 1800 ? null : new Uint8Array(out);
  }
  function nearestPaletteIndex(palette, color) {
    const rgb = parseHex(color);
    if (!rgb) return 0x7FFF;
    let best = 8, bestDistance = Infinity;
    for (let i = 8; i < 64; i++) {
      const p = parseHex(palette[i]);
      const d = (p[0] - rgb[0]) ** 2 + (p[1] - rgb[1]) ** 2 + (p[2] - rgb[2]) ** 2;
      if (d < bestDistance) { best = i; bestDistance = d; if (!d) break; }
    }
    return best;
  }
  function writeXls(wb, calc) {
    const palette = PALETTE.slice();
    const used = new Set();
    for (const style of wb.styles) for (const color of [style.color, style.fill, style.bl?.c, style.br?.c, style.bt?.c, style.bb?.c]) if (color) used.add(String(color).toUpperCase());
    let slot = 63;
    for (const color of used) {
      if (palette.slice(8).includes(color)) continue;
      if (slot < 8) break;
      palette[slot--] = color;
    }
    const colorIndex = (color) => (color ? nearestPaletteIndex(palette, color) : 0x7FFF);
    const fontKeys = new Map(), fontRecords = [];
    const fontOf = (style) => {
      const key = JSON.stringify([style.font || wb.defaultFont, style.size || wb.defaultSize, !!style.b, !!style.i, !!style.u, !!style.s, style.color || ""]);
      let i = fontKeys.get(key);
      if (i === undefined) {
        i = fontRecords.length;
        fontKeys.set(key, i);
        const size = Math.round((style.size || wb.defaultSize) * 20);
        fontRecords.push(bytes([16, size], [16, (style.i ? 2 : 0) | (style.s ? 8 : 0)], [16, colorIndex(style.color)], [16, style.b ? 700 : 400], [16, 0], [8, style.u ? 1 : 0], [8, 0], [8, 0], [8, 0], ["raw", shortString(style.font || wb.defaultFont)]));
      }
      return i < 4 ? i : i + 1;
    };
    fontOf({});
    const formatIds = new Map();
    const formatRecords = [];
    const formatOf = (code) => {
      if (!code) return 0;
      const builtin = BUILTIN_FORMAT_IDS.get(code);
      if (builtin !== undefined) return builtin;
      let id = formatIds.get(code);
      if (id === undefined) { id = 164 + formatIds.size; formatIds.set(code, id); formatRecords.push(bytes([16, id], ["raw", longString(code)])); }
      return id;
    };
    const BORDER_CODES = Object.fromEntries(BIFF_BORDERS.map((name, i) => [name, i]));
    const xfRecords = [];
    const xf = (font, format, style, cellXf) => {
      const h = { left: 1, center: 2, right: 3, justify: 5 }[style.h] || 0;
      const v = { top: 0, middle: 1, bottom: 2 }[style.v || "bottom"];
      const border = (b) => (b ? BORDER_CODES[b.w] ?? 1 : 0);
      const bc = (b) => (b ? colorIndex(b.c) & 127 : 0);
      const b1 = (border(style.bl) | (border(style.br) << 4) | (border(style.bt) << 8) | (border(style.bb) << 12) | (bc(style.bl) << 16) | (bc(style.br) << 23)) >>> 0;
      const fill = style.fill ? colorIndex(style.fill) & 127 : 64;
      const b2 = (bc(style.bt) | (bc(style.bb) << 7) | ((style.fill ? 1 : 0) << 26)) >>> 0;
      const b3 = (fill | (65 << 7)) & 0xFFFF;
      xfRecords.push(bytes([16, font], [16, format], [16, cellXf ? 0x0001 : 0xFFF5], [8, h | (style.wrap ? 8 : 0) | (v << 4)], [8, style.rot && style.rot <= 180 ? style.rot : 0], [8, Math.min(15, style.indent || 0)], [8, cellXf ? 0xF8 : 0xF4], [32, b1], [32, b2], [16, b3]));
    };
    for (let i = 0; i < 15; i++) xf(0, 0, {}, false);
    xf(0, 0, {}, true);
    const xfIndex = new Map();
    const xfOf = (styleId) => {
      if (!styleId) return 15;
      let i = xfIndex.get(styleId);
      if (i === undefined) { const style = wb.styles[styleId] || {}; i = xfRecords.length; xf(fontOf(style), formatOf(style.fmt), style, true); xfIndex.set(styleId, i); }
      return i;
    };
    const strings = [], stringIndex = new Map();
    const sst = (text) => { let i = stringIndex.get(text); if (i === undefined) { i = strings.length; strings.push(text); stringIndex.set(text, i); } return i; };
    const sheetIndex = new Map(wb.sheets.map((sheet, i) => [sheet.name.toLowerCase(), i]));
    const sheetStreams = wb.sheets.map((sheet, index) => {
      const w = new ByteWriter();
      w.record(0x0809, bytes([16, 0x0600], [16, 0x0010], [16, 0x0DBB], [16, 0x07CC], [32, 0], [32, 6]));
      w.record(0x0055, bytes([16, Math.max(1, Math.round((sheet.defaultColWidth - 5) / 7))]));
      for (const c of [...new Set([...sheet.colWidths.keys(), ...sheet.hiddenCols])].sort((a, b) => a - b)) {
        if (c > 255) continue;
        const width = Math.min(65535, Math.round(((sheet.colWidths.get(c) ?? sheet.defaultColWidth) / 7) * 256));
        w.record(0x007D, bytes([16, c], [16, c], [16, width], [16, 15], [16, sheet.hiddenCols.has(c) ? 1 : 0], [16, 0]));
      }
      const extent = extentOf(sheet);
      const lastRow = Math.min(65535, Math.max(0, extent.r2)), lastCol = Math.min(255, Math.max(0, extent.c2));
      w.record(0x0200, bytes([32, 0], [32, lastRow + 1], [16, 0], [16, lastCol + 1], [16, 0]));
      for (const r of [...new Set([...sheet.rowHeights.keys(), ...sheet.hiddenRows])].sort((a, b) => a - b)) {
        if (r > 65535) continue;
        const height = Math.min(8191, Math.round(((sheet.rowHeights.get(r) ?? sheet.defaultRowHeight) * 0.75) * 20));
        w.record(0x0208, bytes([16, r], [16, 0], [16, lastCol + 1], [16, height], [16, 0], [16, 0], [16, 0x0100 | (sheet.hiddenRows.has(r) ? 0x20 : 0) | (sheet.rowHeights.has(r) ? 0x40 : 0)], [16, 15]));
      }
      const keys = [...sheet.cells.keys()].filter((k) => keyRow(k) <= 65535 && keyCol(k) <= 255).sort((a, b) => a - b);
      for (const k of keys) {
        const cell = sheet.cells.get(k);
        const r = keyRow(k), c = keyCol(k), x = xfOf(cell.s);
        const value = cell.f !== undefined && calc ? calc.cellValue(sheet, r, c) : cell.v;
        const rgce = cell.f !== undefined ? compileBiffFormula(cell.f, sheet.name, sheetIndex) : null;
        if (rgce) {
          let result;
          if (typeof value === "number") result = ["f", Number.isFinite(value) ? value : 0];
          else if (typeof value === "boolean") result = ["raw", [1, 0, value ? 1 : 0, 0, 0, 0, 0xFF, 0xFF]];
          else if (isErr(value)) result = ["raw", [2, 0, BIFF_ERROR_CODES[value.code] ?? 0x0F, 0, 0, 0, 0xFF, 0xFF]];
          else if (typeof value === "string" && value !== "") result = ["raw", [0, 0, 0, 0, 0, 0, 0xFF, 0xFF]];
          else result = ["raw", [3, 0, 0, 0, 0, 0, 0xFF, 0xFF]];
          w.record(0x0006, bytes([16, r], [16, c], [16, x], result, [16, 0x0002], [32, 0], [16, rgce.length], ["raw", rgce]));
          if (typeof value === "string" && value !== "") w.record(0x0207, bytes(["raw", longString(value)]));
          continue;
        }
        if (value === undefined || value === null) w.record(0x0201, bytes([16, r], [16, c], [16, x]));
        else if (typeof value === "number") w.record(0x0203, bytes([16, r], [16, c], [16, x], ["f", Number.isFinite(value) ? value : 0]));
        else if (typeof value === "boolean") w.record(0x0205, bytes([16, r], [16, c], [16, x], [8, value ? 1 : 0], [8, 0]));
        else if (isErr(value)) w.record(0x0205, bytes([16, r], [16, c], [16, x], [8, BIFF_ERROR_CODES[value.code] ?? 0x0F], [8, 1]));
        else w.record(0x00FD, bytes([16, r], [16, c], [16, x], [32, sst(String(value))]));
      }
      const frozen = sheet.freezeRows > 0 || sheet.freezeCols > 0;
      const grbit = 0x0004 | 0x0010 | 0x0020 | 0x0080 | (sheet.showGrid ? 0x0002 : 0) | (frozen ? 0x0108 : 0) | (index === wb.active ? 0x0600 : 0);
      w.record(0x023E, bytes([16, grbit], [16, 0], [16, 0], [16, 64], [16, 0], [16, 0], [16, 0], [32, 0]));
      if (frozen) w.record(0x0041, bytes([16, Math.min(255, sheet.freezeCols)], [16, Math.min(65535, sheet.freezeRows)], [16, Math.min(65535, sheet.freezeRows)], [16, Math.min(255, sheet.freezeCols)], [8, sheet.freezeRows && sheet.freezeCols ? 0 : sheet.freezeRows ? 2 : 1], [8, 0]));
      const merges = sheet.merges.filter((m) => m.r2 <= 65535 && m.c2 <= 255);
      for (let i = 0; i < merges.length; i += 1000) {
        const part = merges.slice(i, i + 1000);
        w.record(0x00E5, bytes([16, part.length], ...part.flatMap((m) => [[16, m.r1], [16, m.r2], [16, m.c1], [16, m.c2]])));
      }
      w.record(0x000A, new Uint8Array(0));
      return w.bytes();
    });
    const globals = new ByteWriter();
    globals.record(0x0809, bytes([16, 0x0600], [16, 0x0005], [16, 0x0DBB], [16, 0x07CC], [32, 0], [32, 6]));
    globals.record(0x0042, bytes([16, 1200]));
    globals.record(0x003D, bytes([16, 0], [16, 0], [16, 0x3A5C], [16, 0x2328], [16, 0x0038], [16, wb.active], [16, 0], [16, 1], [16, 600]));
    globals.record(0x0022, bytes([16, 0]));
    const fontList = fontRecords.length >= 4 ? fontRecords : [...fontRecords, ...Array(4 - fontRecords.length).fill(fontRecords[0])];
    fontList.forEach((record) => globals.record(0x0031, record));
    formatRecords.forEach((record) => globals.record(0x041E, record));
    xfRecords.forEach((record) => globals.record(0x00E0, record));
    globals.record(0x0293, bytes([16, 0x8000], [8, 0], [8, 0xFF]));
    globals.record(0x0092, bytes([16, 56], ...palette.slice(8).flatMap((hex) => { const rgb = parseHex(hex) || [0, 0, 0]; return [[8, rgb[0]], [8, rgb[1]], [8, rgb[2]], [8, 0]]; })));
    const boundsheetAt = [];
    wb.sheets.forEach((sheet) => {
      boundsheetAt.push(globals.length + 4);
      globals.record(0x0085, bytes([32, 0], [8, sheet.hidden ? 1 : 0], [8, 0], ["raw", shortString(sheet.name.slice(0, 31))]));
    });
    globals.record(0x01AE, bytes([16, wb.sheets.length], [16, 0x0401]));
    globals.record(0x0017, bytes([16, wb.sheets.length], ...wb.sheets.flatMap((_, i) => [[16, 0], [16, i], [16, i]])));
    const LIMIT = 8224;
    const sstRecords = [];
    const putU32 = (arr, at, value) => { arr[at] = value & 255; arr[at + 1] = (value >>> 8) & 255; arr[at + 2] = (value >>> 16) & 255; arr[at + 3] = (value >>> 24) & 255; };
    let current = new Array(8).fill(0);
    putU32(current, 0, strings.length); putU32(current, 4, strings.length);
    for (const text of strings) {
      const s = text.slice(0, 32767);
      if (current.length + 3 + 2 > LIMIT) { sstRecords.push(current); current = []; }
      current.push(s.length & 255, s.length >> 8, 1);
      let i = 0;
      while (i < s.length) {
        const room = Math.floor((LIMIT - current.length) / 2);
        if (room <= 0) { sstRecords.push(current); current = [1]; continue; }
        const take = Math.min(room, s.length - i);
        for (let j = 0; j < take; j++) { const ch = s.charCodeAt(i + j); current.push(ch & 255, ch >> 8); }
        i += take;
      }
    }
    sstRecords.push(current);
    sstRecords.forEach((body, i) => globals.record(i === 0 ? 0x00FC : 0x003C, new Uint8Array(body)));
    globals.record(0x000A, new Uint8Array(0));
    const head = globals.bytes();
    let offset = head.length;
    const view = new DataView(head.buffer);
    sheetStreams.forEach((stream, i) => { view.setUint32(boundsheetAt[i], offset, true); offset += stream.length; });
    const all = new Uint8Array(offset);
    all.set(head, 0);
    let at = head.length;
    for (const stream of sheetStreams) { all.set(stream, at); at += stream.length; }
    return writeCfb(all);
  }

  const ODS_NS = 'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" xmlns:number="urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0" xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:dc="http://purl.org/dc/elements/1.1/" office:version="1.2"';
  function openFormulaText(text) {
    const ref = (sheet, r, c, ar, ac) => `${sheet ? "$" + quoteSheet(sheet).replace(/^'(.*)'$/, "'$1'") + "." : "."}${ac ? "$" : ""}${colName(c)}${ar ? "$" : ""}${r + 1}`;
    const walk = (node) => {
      switch (node.t) {
        case "ref": return `[${ref(node.sheet, node.r, node.c, node.ar, node.ac)}]`;
        case "rng": {
          const r2 = node.kind === "cols" ? MAX_ROWS - 1 : node.r2, c2 = node.kind === "rows" ? MAX_COLS - 1 : node.c2;
          return `[${ref(node.sheet, node.r1, node.c1, node.ar1, node.ac1)}:${ref(null, r2, c2, node.ar2, node.ac2)}]`;
        }
        case "fn": return `${node.name}(${node.args.map(walk).join(";")})`;
        case "op": return walk(node.a) + node.op + walk(node.b);
        case "neg": return "-" + walk(node.a);
        case "pos": return "+" + walk(node.a);
        case "pct": return walk(node.a) + "%";
        case "paren": return "(" + walk(node.a) + ")";
        case "b": return node.v ? "TRUE()" : "FALSE()";
        case "arr": return "{" + node.rows.map((row) => row.map(walk).join(";")).join("|") + "}";
        default: return astText(node);
      }
    };
    try { return "of:=" + walk(parseFormula(text)); } catch { return null; }
  }
  function fromOpenFormula(text) {
    let source = String(text || "").replace(/^(of|oooc|msoxl):/, "").replace(/^=/, "");
    let out = "", quoted = false;
    for (let i = 0; i < source.length; i++) {
      const ch = source[i];
      if (ch === '"') { quoted = !quoted; out += ch; continue; }
      if (quoted) { out += ch; continue; }
      if (ch === "[") {
        const end = source.indexOf("]", i);
        if (end < 0) { out += ch; continue; }
        const inner = source.slice(i + 1, end);
        const parts = inner.split(":").map((part) => {
          const m = /^\$?(?:('(?:[^']|'')+'|[^.]+))?\.(\$?[A-Za-z]+\$?\d+|\$?[A-Za-z]+|\$?\d+)$/.exec(part);
          return m ? { sheet: m[1] ? m[1].replace(/^'|'$/g, "").replace(/''/g, "'") : null, ref: m[2] } : null;
        });
        if (parts.some((p) => !p)) { out += inner; i = end; continue; }
        const sheet = parts[0].sheet;
        out += (sheet ? quoteSheet(sheet) + "!" : "") + parts.map((p) => p.ref).join(":");
        i = end;
        continue;
      }
      if (ch === ";") { out += ","; continue; }
      out += ch;
    }
    return out.replace(/\bTRUE\(\)/g, "TRUE").replace(/\bFALSE\(\)/g, "FALSE");
  }
  function odsLength(text) {
    const m = /^([\d.]+)(cm|mm|in|pt|pc|px)$/.exec(String(text || ""));
    if (!m) return null;
    const n = Number(m[1]);
    return Math.round({ cm: 37.795, mm: 3.7795, in: 96, pt: 4 / 3, pc: 16, px: 1 }[m[2]] * n);
  }
  function odsNumberFormat(node) {
    if (!node) return "";
    let code = "";
    for (const part of node.children) {
      const a = part.attrs || {};
      switch (part.name) {
        case "number": { const decimals = Number(a["decimal-places"] || 0); code += (flag(a.grouping) ? "#,##0" : "0") + (decimals ? "." + "0".repeat(decimals) : ""); break; }
        case "scientific-number": code += "0." + "0".repeat(Number(a["decimal-places"] || 2)) + "E+00"; break;
        case "fraction": code += "# ?/?"; break;
        case "text": code += textOf(part) === "%" ? "%" : `"${textOf(part).replace(/"/g, "")}"`; break;
        case "currency-symbol": code += `"${textOf(part)}"`; break;
        case "year": code += a.style === "long" ? "yyyy" : "yy"; break;
        case "month": code += flag(a.textual) ? (a.style === "long" ? "mmmm" : "mmm") : a.style === "long" ? "mm" : "m"; break;
        case "day": code += a.style === "long" ? "dd" : "d"; break;
        case "day-of-week": code += a.style === "long" ? "dddd" : "ddd"; break;
        case "hours": code += a.style === "long" ? "hh" : "h"; break;
        case "minutes": code += a.style === "long" ? "mm" : "m"; break;
        case "seconds": code += (a.style === "long" ? "ss" : "s") + (Number(a["decimal-places"]) ? "." + "0".repeat(Number(a["decimal-places"])) : ""); break;
        case "am-pm": code += "AM/PM"; break;
        default: break;
      }
    }
    if (node.name === "percentage-style" && !code.includes("%")) code += "%";
    if (node.name === "time-style" && node.attrs["truncate-on-overflow"] === "false") code = code.replace(/^h+/, "[h]");
    return code === "0" ? "" : code;
  }
  async function readOds(bytes) {
    const zip = readZip(bytes);
    const content = kid(parseXml(await zip.text("content.xml") || ""), "document-content");
    if (!content) throw new Error("This file is not an OpenDocument spreadsheet.");
    const stylesDoc = kid(parseXml(await zip.text("styles.xml") || ""), "document-styles");
    const wb = createWorkbook();
    wb.sheets = [];
    const dataStyles = new Map();
    const cellStyles = new Map(), columnWidths = new Map(), rowHeights = new Map();
    for (const container of [kid(stylesDoc, "styles"), kid(stylesDoc, "automatic-styles"), kid(content, "automatic-styles")]) {
      for (const node of container ? container.children : []) {
        if (/-style$/.test(node.name) && node.name !== "style" && node.attrs.name) dataStyles.set(node.attrs.name, odsNumberFormat(node));
      }
    }
    for (const container of [kid(stylesDoc, "styles"), kid(content, "automatic-styles")]) {
      for (const node of kids(container, "style")) {
        const family = node.attrs.family, name = node.attrs.name;
        if (family === "table-column") { const w = odsLength(kid(node, "table-column-properties")?.attrs["column-width"]); if (w) columnWidths.set(name, w); }
        else if (family === "table-row") { const h = odsLength(kid(node, "table-row-properties")?.attrs["row-height"]); if (h) rowHeights.set(name, h); }
        else if (family === "table-cell") {
          const style = {};
          const text = kid(node, "text-properties")?.attrs || {}, cell = kid(node, "table-cell-properties")?.attrs || {}, para = kid(node, "paragraph-properties")?.attrs || {};
          if (text["font-weight"] === "bold") style.b = true;
          if (text["font-style"] === "italic") style.i = true;
          if (text["text-underline-style"] && text["text-underline-style"] !== "none") style.u = true;
          if (text["text-line-through-style"] && text["text-line-through-style"] !== "none") style.s = true;
          if (/^#[0-9a-f]{6}$/i.test(text.color || "") && text.color.toLowerCase() !== "#000000") style.color = text.color.toUpperCase();
          if (text["font-size"] && /pt$/.test(text["font-size"])) style.size = Number(text["font-size"].replace("pt", ""));
          if (text["font-name"]) style.font = text["font-name"];
          if (/^#[0-9a-f]{6}$/i.test(cell["background-color"] || "")) style.fill = cell["background-color"].toUpperCase();
          if (cell["wrap-option"] === "wrap") style.wrap = true;
          const v = { top: "top", middle: "middle" }[cell["vertical-align"]];
          if (v) style.v = v;
          const h = { start: "left", left: "left", center: "center", end: "right", right: "right", justify: "justify" }[para["text-align"]];
          if (h) style.h = h;
          for (const [side, key] of [["border-left", "bl"], ["border-right", "br"], ["border-top", "bt"], ["border-bottom", "bb"]]) {
            const value = cell[side] || cell.border;
            const m = /([\d.]+)(pt|cm|mm|in)\s+(solid|dashed|dotted|double)\s+(#[0-9a-f]{6})/i.exec(value || "");
            if (m) { const pts = Number(m[1]) * ({ pt: 1, cm: 28.35, mm: 2.835, in: 72 }[m[2]]); style[key] = { w: m[3] === "double" ? "double" : m[3] === "dashed" ? "dashed" : m[3] === "dotted" ? "dotted" : pts > 2 ? "thick" : pts > 1 ? "medium" : "thin", c: m[4].toUpperCase() }; }
          }
          const fmt = dataStyles.get(node.attrs["data-style-name"]);
          if (fmt) style.fmt = fmt;
          cellStyles.set(name, internStyle(wb, style));
        }
      }
    }
    const spreadsheet = nodePath(content, "body", "spreadsheet");
    const used = new Set();
    for (const table of kids(spreadsheet, "table")) {
      let name = validSheetName(table.attrs.name) ? table.attrs.name : `Sheet${wb.sheets.length + 1}`;
      while (used.has(name.toLowerCase())) name += " (2)";
      used.add(name.toLowerCase());
      const sheet = createSheet(name, { defaultColWidth: 85, defaultRowHeight: 17 });
      const columnStyles = [];
      let c = 0;
      const columns = [];
      const collect = (node) => { for (const child of node.children) { if (child.name === "table-column") columns.push(child); else if (/^table-(header-)?columns$|^table-column-group$/.test(child.name)) collect(child); } };
      collect(table);
      for (const col of columns) {
        const repeat = Math.min(Number(col.attrs["number-columns-repeated"] || 1), MAX_COLS - c);
        const width = columnWidths.get(col.attrs["style-name"]);
        for (let i = 0; i < repeat && c < 1024; i++, c++) {
          if (width && width !== sheet.defaultColWidth && repeat < 1000) sheet.colWidths.set(c, width);
          if (col.attrs.visibility === "collapse") sheet.hiddenCols.add(c);
          columnStyles[c] = col.attrs["default-cell-style-name"];
        }
        if (repeat >= 1024) c += repeat;
      }
      const rows = [];
      const collectRows = (node) => { for (const child of node.children) { if (child.name === "table-row") rows.push(child); else if (/^table-(header-)?rows$|^table-row-group$/.test(child.name)) collectRows(child); } };
      collectRows(table);
      let r = 0;
      for (const row of rows) {
        const repeat = Math.min(Number(row.attrs["number-rows-repeated"] || 1), MAX_ROWS - r);
        const cells = row.children.filter((child) => child.name === "table-cell" || child.name === "covered-table-cell");
        const hasContent = cells.some((cell) => cell.attrs["value-type"] || cell.children.length || cell.attrs.formula);
        const height = rowHeights.get(row.attrs["style-name"]);
        if (!hasContent && repeat > 1) {
          if (height && repeat < 1000) for (let i = 0; i < repeat; i++) sheet.rowHeights.set(r + i, height);
          r += repeat;
          continue;
        }
        for (let rep = 0; rep < Math.min(repeat, 5000); rep++, r++) {
          if (height && height !== sheet.defaultRowHeight) sheet.rowHeights.set(r, height);
          if (row.attrs.visibility === "collapse" || row.attrs.visibility === "filter") sheet.hiddenRows.add(r);
          let col = 0;
          for (const cell of cells) {
            const span = Math.min(Number(cell.attrs["number-columns-repeated"] || 1), MAX_COLS - col);
            if (cell.name === "covered-table-cell") { col += span; continue; }
            const type = cell.attrs["value-type"];
            const paragraphs = kids(cell, "p").map(textOf);
            let value;
            if (type === "float" || type === "currency" || type === "percentage") value = Number(cell.attrs.value);
            else if (type === "boolean") value = flag(cell.attrs["boolean-value"]);
            else if (type === "date") { const d = new Date(cell.attrs["date-value"] + (cell.attrs["date-value"].length <= 10 ? "T00:00:00Z" : "Z")); value = Number.isNaN(d.getTime()) ? paragraphs.join("\n") : (d.getTime() - EPOCH) / DAY_MS; }
            else if (type === "time") { const m = /PT(\d+)H(\d+)M([\d.]+)S/.exec(cell.attrs["time-value"] || ""); value = m ? (Number(m[1]) * 3600 + Number(m[2]) * 60 + Number(m[3])) / 86400 : paragraphs.join("\n"); }
            else if (type === "string") value = cell.attrs["string-value"] ?? paragraphs.join("\n");
            else if (paragraphs.length) value = paragraphs.join("\n");
            const formula = cell.attrs.formula ? fromOpenFormula(cell.attrs.formula) : undefined;
            const styleName = cell.attrs["style-name"] || columnStyles[col];
            const s = cellStyles.get(styleName) || 0;
            const filled = value !== undefined || formula !== undefined;
            const limit = filled || s ? Math.min(span, 1024) : 0;
            for (let i = 0; i < limit; i++) {
              const made = makeCell(value, formula ? shiftFormula(formula, 0, i) : undefined, s);
              if (made) sheet.cells.set(cellKey(r, col + i), made);
            }
            const across = Number(cell.attrs["number-columns-spanned"] || 1), down = Number(cell.attrs["number-rows-spanned"] || 1);
            if (across > 1 || down > 1) sheet.merges.push({ r1: r, c1: col, r2: r + down - 1, c2: col + across - 1 });
            const note = kid(cell, "annotation");
            if (note) sheet.notes.set(cellKey(r, col), kids(note, "p").map(textOf).join("\n"));
            const link = nodePath(cell, "p", "a");
            if (link && link.attrs.href) sheet.links.set(cellKey(r, col), link.attrs.href);
            col += span;
          }
        }
        if (repeat > 5000) r += repeat - 5000;
      }
      finishSheetSize(sheet);
      wb.sheets.push(sheet);
    }
    if (!wb.sheets.length) wb.sheets.push(createSheet("Sheet1"));
    return wb;
  }
  function odsDataStyle(code, name) {
    const section = compileFormat(code)[0];
    if (!section || section.kind === "general") return "";
    if (section.kind === "date") {
      const parts = section.tokens.map((tok) => {
        if (tok.t === "lit") return `<number:text>${xmlEscape(tok.v)}</number:text>`;
        if (tok.t === "date") {
          const long = tok.n >= 2 ? ' number:style="long"' : "";
          if (tok.v === "y") return `<number:year${tok.n > 2 ? ' number:style="long"' : ""}/>`;
          if (tok.v === "m" && tok.minute) return `<number:minutes${long}/>`;
          if (tok.v === "m") return tok.n >= 3 ? `<number:month number:textual="true"${tok.n >= 4 ? ' number:style="long"' : ""}/>` : `<number:month${long}/>`;
          if (tok.v === "d") return tok.n >= 3 ? `<number:day-of-week${tok.n >= 4 ? ' number:style="long"' : ""}/>` : `<number:day${long}/>`;
          if (tok.v === "h") return `<number:hours${long}/>`;
          if (tok.v === "s") return `<number:seconds${long}/>`;
        }
        if (tok.t === "elapsed") return tok.v === "h" ? '<number:hours number:style="long"/>' : tok.v === "m" ? '<number:minutes number:style="long"/>' : '<number:seconds number:style="long"/>';
        if (tok.t === "ampm") return "<number:am-pm/>";
        if (tok.t === "point" || tok.t === "slash" || tok.t === "comma") return `<number:text>${{ point: ".", slash: "/", comma: "," }[tok.t]}</number:text>`;
        return "";
      }).join("");
      const elapsed = section.tokens.some((tok) => tok.t === "elapsed");
      const timeOnly = !section.tokens.some((tok) => tok.t === "date" && (tok.v === "y" || tok.v === "d" || (tok.v === "m" && !tok.minute)));
      return timeOnly ? `<number:time-style style:name="${name}"${elapsed ? ' number:truncate-on-overflow="false"' : ""}>${parts}</number:time-style>` : `<number:date-style style:name="${name}">${parts}</number:date-style>`;
    }
    if (section.kind !== "number") return "";
    const decimals = (() => { const p = section.tokens.findIndex((tok) => tok.t === "point"); if (p < 0) return 0; let n = 0; for (let i = p + 1; i < section.tokens.length && section.tokens[i].t === "digit"; i++) n++; return n; })();
    const grouping = section.tokens.some((tok) => tok.t === "comma");
    const prefix = [], suffix = [];
    let seenDigit = false;
    for (const tok of section.tokens) {
      if (tok.t === "digit") seenDigit = true;
      else if (tok.t === "lit" && tok.v.trim()) (seenDigit ? suffix : prefix).push(tok.v);
    }
    const number = `<number:number number:decimal-places="${decimals}" number:min-decimal-places="${decimals}" number:min-integer-digits="1"${grouping ? ' number:grouping="true"' : ""}/>`;
    const text = (list) => (list.length ? `<number:text>${xmlEscape(list.join(""))}</number:text>` : "");
    if (section.tokens.some((tok) => tok.t === "percent")) return `<number:percentage-style style:name="${name}">${text(prefix)}${number}<number:text>%</number:text></number:percentage-style>`;
    if (section.tokens.some((tok) => tok.t === "exp")) return `<number:number-style style:name="${name}"><number:scientific-number number:decimal-places="${decimals}" number:min-integer-digits="1" number:min-exponent-digits="2"/></number:number-style>`;
    return `<number:number-style style:name="${name}">${text(prefix)}${number}${text(suffix)}</number:number-style>`;
  }
  async function writeOds(wb, calc) {
    const automatic = [], dataStyles = [], cellStyleNames = new Map(), dataStyleNames = new Map(), columnStyles = new Map(), rowStyles = new Map();
    const borderCss = (b) => (b ? `${{ medium: "1.5pt", thick: "2.5pt", mediumDashed: "1.5pt", mediumDashDot: "1.5pt", mediumDashDotDot: "1.5pt", double: "2.5pt" }[b.w] || "0.75pt"} ${b.w === "double" ? "double" : /dash/i.test(b.w) ? "dashed" : /dot|hair/i.test(b.w) ? "dotted" : "solid"} ${b.c || "#000000"}` : "");
    const cellStyle = (id) => {
      if (!id) return "";
      if (cellStyleNames.has(id)) return cellStyleNames.get(id);
      const style = wb.styles[id] || {};
      const name = `ce${cellStyleNames.size + 1}`;
      let dataName = "";
      if (style.fmt) {
        if (!dataStyleNames.has(style.fmt)) { const n = `N${dataStyleNames.size + 1}`; const xml = odsDataStyle(style.fmt, n); dataStyleNames.set(style.fmt, xml ? n : ""); if (xml) dataStyles.push(xml); }
        dataName = dataStyleNames.get(style.fmt);
      }
      const cellProps = [style.fill ? `fo:background-color="${style.fill}"` : "", style.wrap ? 'fo:wrap-option="wrap"' : "", style.v ? `style:vertical-align="${style.v}"` : "",
        ...[["bl", "left"], ["br", "right"], ["bt", "top"], ["bb", "bottom"]].map(([key, side]) => (style[key] ? `fo:border-${side}="${borderCss(style[key])}"` : ""))].filter(Boolean).join(" ");
      const textProps = [style.b ? 'fo:font-weight="bold"' : "", style.i ? 'fo:font-style="italic"' : "", style.u ? 'style:text-underline-style="solid" style:text-underline-width="auto" style:text-underline-color="font-color"' : "", style.s ? 'style:text-line-through-style="solid"' : "", style.color ? `fo:color="${style.color}"` : "", style.size ? `fo:font-size="${style.size}pt"` : "", style.font ? `style:font-name="${xmlEscape(style.font)}" fo:font-family="${xmlEscape(style.font)}"` : ""].filter(Boolean).join(" ");
      const align = style.h ? `<style:paragraph-properties fo:text-align="${{ left: "start", center: "center", right: "end", justify: "justify" }[style.h] || "start"}"/>` : "";
      automatic.push(`<style:style style:name="${name}" style:family="table-cell" style:parent-style-name="Default"${dataName ? ` style:data-style-name="${dataName}"` : ""}>${cellProps ? `<style:table-cell-properties ${cellProps}/>` : ""}${align}${textProps ? `<style:text-properties ${textProps}/>` : ""}</style:style>`);
      cellStyleNames.set(id, name);
      return name;
    };
    const columnStyle = (px) => { if (!columnStyles.has(px)) { const name = `co${columnStyles.size + 1}`; columnStyles.set(px, name); automatic.push(`<style:style style:name="${name}" style:family="table-column"><style:table-column-properties style:column-width="${roundTo(px / 37.795, 3)}cm"/></style:style>`); } return columnStyles.get(px); };
    const rowStyle = (px) => { if (!rowStyles.has(px)) { const name = `ro${rowStyles.size + 1}`; rowStyles.set(px, name); automatic.push(`<style:style style:name="${name}" style:family="table-row"><style:table-row-properties style:row-height="${roundTo(px / 37.795, 3)}cm" style:use-optimal-row-height="false"/></style:style>`); } return rowStyles.get(px); };
    const tables = wb.sheets.map((sheet) => {
      const extent = extentOf(sheet);
      const lastRow = Math.max(0, extent.r2), lastCol = Math.max(0, extent.c2);
      const merged = new Map(), covered = new Set();
      for (const m of sheet.merges) { merged.set(cellKey(m.r1, m.c1), m); for (let r = m.r1; r <= m.r2; r++) for (let c = m.c1; c <= m.c2; c++) if (r !== m.r1 || c !== m.c1) covered.add(cellKey(r, c)); }
      const out = [`<table:table table:name="${xmlEscape(sheet.name)}">`];
      for (let c = 0; c <= lastCol; c++) out.push(`<table:table-column table:style-name="${columnStyle(sheet.colWidths.get(c) ?? sheet.defaultColWidth)}"${sheet.hiddenCols.has(c) ? ' table:visibility="collapse"' : ""}/>`);
      for (let r = 0; r <= lastRow; r++) {
        out.push(`<table:table-row table:style-name="${rowStyle(sheet.rowHeights.get(r) ?? sheet.defaultRowHeight)}"${sheet.hiddenRows.has(r) ? ' table:visibility="collapse"' : ""}>`);
        let empty = 0;
        const flush = () => { if (empty) { out.push(`<table:table-cell${empty > 1 ? ` table:number-columns-repeated="${empty}"` : ""}/>`); empty = 0; } };
        for (let c = 0; c <= lastCol; c++) {
          const k = cellKey(r, c);
          if (covered.has(k)) { flush(); out.push("<table:covered-table-cell/>"); continue; }
          const cell = sheet.cells.get(k);
          const note = sheet.notes.get(k), link = sheet.links.get(k);
          if (!cell && !note && !merged.has(k)) { empty++; continue; }
          flush();
          const value = cell ? (cell.f !== undefined && calc ? calc.cellValue(sheet, r, c) : cell.v) : undefined;
          const attrs = [];
          const style = cellStyle(cell?.s);
          if (style) attrs.push(`table:style-name="${style}"`);
          const m = merged.get(k);
          if (m) attrs.push(`table:number-columns-spanned="${m.c2 - m.c1 + 1}" table:number-rows-spanned="${m.r2 - m.r1 + 1}"`);
          if (cell && cell.f !== undefined) { const f = openFormulaText(cell.f); if (f) attrs.push(`table:formula="${xmlEscape(f)}"`); }
          const fmt = styleOf(wb, cell).fmt;
          if (typeof value === "number" && Number.isFinite(value)) {
            if (isDateFormat(fmt) && value >= 1) {
              const p = dateFromSerial(value);
              const iso = `${String(p.y).padStart(4, "0")}-${String(p.m).padStart(2, "0")}-${String(p.d).padStart(2, "0")}T${String(p.H).padStart(2, "0")}:${String(p.M).padStart(2, "0")}:${String(p.S).padStart(2, "0")}`;
              attrs.push(`office:value-type="date" office:date-value="${iso}"`);
            } else if (/%/.test(fmt || "")) attrs.push(`office:value-type="percentage" office:value="${value}"`);
            else attrs.push(`office:value-type="float" office:value="${value}"`);
          } else if (typeof value === "boolean") attrs.push(`office:value-type="boolean" office:boolean-value="${value}"`);
          else if (value !== undefined && value !== null) attrs.push('office:value-type="string"');
          const text = value === undefined || value === null ? "" : formatValue(value, fmt).text;
          const paragraphs = text ? text.split("\n").map((line) => `<text:p>${link && /^(https?:|mailto:)/i.test(link) ? `<text:a xlink:href="${xmlEscape(link)}" xlink:type="simple">${xmlEscape(line)}</text:a>` : xmlEscape(line)}</text:p>`).join("") : "";
          const annotation = note ? `<office:annotation>${note.split("\n").map((line) => `<text:p>${xmlEscape(line)}</text:p>`).join("")}</office:annotation>` : "";
          out.push(`<table:table-cell ${attrs.join(" ")}>${annotation}${paragraphs}</table:table-cell>`);
        }
        flush();
        out.push("</table:table-row>");
      }
      out.push("</table:table>");
      return out.join("");
    });
    const contentXml = `<?xml version="1.0" encoding="UTF-8"?>\n<office:document-content ${ODS_NS}><office:automatic-styles>${dataStyles.join("")}${automatic.join("")}</office:automatic-styles><office:body><office:spreadsheet>${tables.join("")}</office:spreadsheet></office:body></office:document-content>`;
    const stylesXml = `<?xml version="1.0" encoding="UTF-8"?>\n<office:document-styles ${ODS_NS}><office:styles><style:style style:name="Default" style:family="table-cell"><style:text-properties style:font-name="${xmlEscape(wb.defaultFont)}" fo:font-family="${xmlEscape(wb.defaultFont)}" fo:font-size="${wb.defaultSize}pt"/></style:style></office:styles></office:document-styles>`;
    const manifest = `<?xml version="1.0" encoding="UTF-8"?>\n<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.2"><manifest:file-entry manifest:full-path="/" manifest:version="1.2" manifest:media-type="application/vnd.oasis.opendocument.spreadsheet"/><manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/><manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/><manifest:file-entry manifest:full-path="meta.xml" manifest:media-type="text/xml"/></manifest:manifest>`;
    const meta = `<?xml version="1.0" encoding="UTF-8"?>\n<office:document-meta ${ODS_NS} xmlns:meta="urn:oasis:names:tc:opendocument:xmlns:meta:1.0"><office:meta><meta:generator>ForkMesh Spreadsheets</meta:generator></office:meta></office:document-meta>`;
    const mimetype = encoder.encode("application/vnd.oasis.opendocument.spreadsheet");
    return writeZip([{ name: "mimetype", data: mimetype, store: true }, { name: "META-INF/manifest.xml", data: manifest }, { name: "content.xml", data: contentXml }, { name: "styles.xml", data: stylesXml }, { name: "meta.xml", data: meta }]);
  }

  const htmlEscape = (value) => String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  function cssBorder(b) {
    if (!b) return "";
    const width = { medium: 2, mediumDashed: 2, mediumDashDot: 2, mediumDashDotDot: 2, thick: 3, double: 3 }[b.w] || 1;
    const kind = b.w === "double" ? "double" : /dash/i.test(b.w) ? "dashed" : /dot|hair/i.test(b.w) ? "dotted" : "solid";
    return `${width}px ${kind} ${b.c || "#000"}`;
  }
  function sheetHtmlTable(wb, sheet, calc, range = null) {
    const extent = range || (() => { const e = extentOf(sheet); return { r1: 0, c1: 0, r2: Math.max(0, e.r2), c2: Math.max(0, e.c2) }; })();
    const covered = new Set(), merged = new Map();
    for (const m of sheet.merges) { if (!rangesOverlap(m, extent)) continue; merged.set(cellKey(m.r1, m.c1), m); for (let r = m.r1; r <= m.r2; r++) for (let c = m.c1; c <= m.c2; c++) if (r !== m.r1 || c !== m.c1) covered.add(cellKey(r, c)); }
    const cols = [];
    for (let c = extent.c1; c <= extent.c2; c++) if (!sheet.hiddenCols.has(c)) cols.push(`<col style="width:${sheet.colWidths.get(c) ?? sheet.defaultColWidth}px">`);
    const rows = [];
    for (let r = extent.r1; r <= extent.r2; r++) {
      if (sheet.hiddenRows.has(r) || sheet.filterRows.has(r)) continue;
      const cells = [];
      for (let c = extent.c1; c <= extent.c2; c++) {
        if (sheet.hiddenCols.has(c)) continue;
        const k = cellKey(r, c);
        if (covered.has(k)) continue;
        const cell = sheet.cells.get(k);
        const style = styleOf(wb, cell);
        const value = calc.cellValue(sheet, r, c);
        const shown = formatValue(value, style.fmt);
        const css = [
          style.b ? "font-weight:bold" : "", style.i ? "font-style:italic" : "", style.u || style.s ? `text-decoration:${[style.u ? "underline" : "", style.s ? "line-through" : ""].join(" ").trim()}` : "",
          shown.color || style.color ? `color:${shown.color || style.color}` : "", style.fill ? `background:${style.fill}` : "", style.font ? `font-family:${htmlEscape(style.font)}` : "", style.size ? `font-size:${style.size}pt` : "",
          `text-align:${style.h || (typeof value === "number" ? "right" : typeof value === "boolean" || isErr(value) ? "center" : "left")}`, `vertical-align:${style.v || "bottom"}`, style.wrap ? "white-space:pre-wrap" : "white-space:pre",
          style.bl ? `border-left:${cssBorder(style.bl)}` : "", style.br ? `border-right:${cssBorder(style.br)}` : "", style.bt ? `border-top:${cssBorder(style.bt)}` : "", style.bb ? `border-bottom:${cssBorder(style.bb)}` : "",
        ].filter(Boolean).join(";");
        const m = merged.get(k);
        const span = m ? `${m.c2 > m.c1 ? ` colspan="${Math.min(m.c2, extent.c2) - c + 1}"` : ""}${m.r2 > m.r1 ? ` rowspan="${Math.min(m.r2, extent.r2) - r + 1}"` : ""}` : "";
        const link = sheet.links.get(k);
        const body = link && /^(https?:|mailto:)/i.test(link) ? `<a href="${htmlEscape(link)}">${htmlEscape(shown.text)}</a>` : htmlEscape(shown.text);
        cells.push(`<td${span} style="${css}">${body}</td>`);
      }
      rows.push(`<tr style="height:${sheet.rowHeights.get(r) ?? sheet.defaultRowHeight}px">${cells.join("")}</tr>`);
    }
    return `<table${sheet.showGrid ? ' class="grid"' : ""}><colgroup>${cols.join("")}</colgroup><tbody>${rows.join("")}</tbody></table>`;
  }
  function workbookHtml(wb, calc, title, options = {}) {
    const sheets = options.sheet ? [options.sheet] : wb.sheets.filter((sheet) => !sheet.hidden);
    const body = sheets.map((sheet) => `<section><h2>${htmlEscape(sheet.name)}</h2>${sheetHtmlTable(wb, sheet, calc, options.range)}</section>`).join("");
    return `<!doctype html><html><head><meta charset="utf-8"><title>${htmlEscape(title)}</title><style>body{font-family:${htmlEscape(wb.defaultFont)},Arial,sans-serif;font-size:${wb.defaultSize}pt;margin:24px;color:#000;background:#fff}h2{font-size:14px;margin:24px 0 8px}table{border-collapse:collapse;table-layout:fixed}td{padding:2px 4px;overflow:hidden;font-size:inherit}table.grid td{outline:1px solid #e2e2e2}section{break-after:page}section:last-child{break-after:auto}@media print{body{margin:0}h2{margin-top:0}}</style></head><body>${body}</body></html>`;
  }
  function sheetToMarkdown(wb, sheet, calc) {
    const used = usedRange(sheet);
    if (!used) return "";
    const lines = [];
    for (let r = 0; r <= used.r2; r++) {
      const cells = [];
      for (let c = 0; c <= used.c2; c++) cells.push(displayText(wb, sheet, calc, r, c).replace(/\|/g, "\\|").replace(/\n/g, "<br>"));
      lines.push(`| ${cells.join(" | ")} |`);
      if (r === 0) lines.push(`| ${cells.map(() => "---").join(" | ")} |`);
    }
    return lines.join("\n") + "\n";
  }
  function workbookJson(wb, calc) {
    return {
      sheets: wb.sheets.map((sheet) => {
        const used = usedRange(sheet);
        if (!used) return { name: sheet.name, rows: [] };
        const grid = [];
        for (let r = 0; r <= used.r2; r++) {
          const row = [];
          for (let c = 0; c <= used.c2; c++) { const v = calc.cellValue(sheet, r, c); row.push(isErr(v) ? v.code : v); }
          grid.push(row);
        }
        const header = grid[0];
        const keyed = header.every((v) => typeof v === "string" && v.trim()) && new Set(header).size === header.length && grid.length > 1;
        return keyed ? { name: sheet.name, rows: grid.slice(1).map((row) => Object.fromEntries(header.map((key, i) => [key, row[i]]))) } : { name: sheet.name, rows: grid };
      }),
    };
  }
  function tablesToWorkbook(tables, base) {
    const wb = createWorkbook();
    wb.sheets = [];
    tables.forEach((rows, index) => {
      const sheet = createSheet(uniqueSheetName(wb, tables.length > 1 ? `${base || "Sheet"} ${index + 1}` : base || "Sheet1"));
      rows.forEach((row, r) => {
        let c = 0;
        for (const item of row) {
          while (sheet.cells.has(cellKey(r, c)) || sheet.merges.some((m) => inRange(m, r, c))) c++;
          const parsed = parseInput(item.text);
          const style = { ...(item.style || {}) };
          if (parsed.format) style.fmt = parsed.format;
          const made = parsed.formula !== undefined ? makeCell(item.text, undefined, internStyle(wb, style)) : makeCell(parsed.value, undefined, internStyle(wb, style));
          if (made) sheet.cells.set(cellKey(r, c), made);
          if (item.colspan > 1 || item.rowspan > 1) sheet.merges.push({ r1: r, c1: c, r2: r + (item.rowspan || 1) - 1, c2: c + (item.colspan || 1) - 1 });
          if (item.href) sheet.links.set(cellKey(r, c), item.href);
          c += item.colspan || 1;
        }
      });
      finishSheetSize(sheet);
      wb.sheets.push(sheet);
    });
    if (!wb.sheets.length) wb.sheets.push(createSheet("Sheet1"));
    return wb;
  }

  const AI_ACTIONS = new Set(["write", "clear", "format", "insert_rows", "delete_rows", "insert_columns", "delete_columns", "add_sheet", "rename_sheet", "delete_sheet", "sort", "conditional", "chart", "freeze", "column_width", "merge", "unmerge", "filter", "validation", "note", "number_format", "select"]);
  function aiContext(wb, calc, sheet, selection, limit = 9000) {
    const lines = [];
    lines.push(`Workbook sheets: ${wb.sheets.map((s) => quoteSheet(s.name)).join(", ")}. Active sheet: ${quoteSheet(sheet.name)}.`);
    if (selection) lines.push(`Selected range: ${rangeText(selection)}.`);
    const used = usedRange(sheet);
    if (!used) { lines.push("The active sheet is empty."); return lines.join("\n"); }
    lines.push(`Used range on the active sheet: ${rangeText({ r1: 0, c1: 0, r2: used.r2, c2: used.c2 })}.`);
    lines.push("Cells (address=value, formulas shown after the value as {=formula}):");
    let size = lines.join("\n").length;
    const focus = selection ? { r1: Math.max(0, selection.r1 - 5), r2: selection.r2 + 5 } : { r1: 0, r2: used.r2 };
    const rowsInOrder = [];
    for (let r = 0; r <= Math.min(used.r2, 2); r++) rowsInOrder.push(r);
    for (let r = focus.r1; r <= Math.min(focus.r2, used.r2); r++) if (!rowsInOrder.includes(r)) rowsInOrder.push(r);
    for (let r = 0; r <= used.r2; r++) if (!rowsInOrder.includes(r)) rowsInOrder.push(r);
    let truncated = false;
    for (const r of rowsInOrder) {
      const parts = [];
      for (let c = 0; c <= Math.min(used.c2, 51); c++) {
        const cell = sheet.cells.get(cellKey(r, c));
        if (!cell || (cell.v === undefined && cell.f === undefined)) continue;
        const shown = displayText(wb, sheet, calc, r, c).slice(0, 60);
        parts.push(`${addr(r, c)}=${shown}${cell.f !== undefined ? ` {=${cell.f.slice(0, 120)}}` : ""}`);
      }
      if (!parts.length) continue;
      const line = parts.join(" | ");
      if (size + line.length > limit) { truncated = true; break; }
      lines.push(line);
      size += line.length + 1;
    }
    if (truncated) lines.push("(Some rows were left out to keep this short.)");
    return lines.join("\n");
  }
  function aiRange(text, fallback) {
    const value = String(text || "").replace(/\$/g, "").trim();
    const bang = value.lastIndexOf("!");
    const sheet = bang > 0 ? value.slice(0, bang).replace(/^'|'$/g, "").replace(/''/g, "'") : null;
    const range = parseRange(bang > 0 ? value.slice(bang + 1) : value);
    return range ? { sheet, range } : fallback ? { sheet: null, range: fallback } : null;
  }
  const AI_COLOR = (value) => { const v = String(value || "").trim(); if (/^#[0-9a-f]{6}$/i.test(v)) return v.toUpperCase(); if (/^#[0-9a-f]{3}$/i.test(v)) return ("#" + v.slice(1).split("").map((ch) => ch + ch).join("")).toUpperCase(); const named = { red: "#EA4335", green: "#34A853", blue: "#4285F4", yellow: "#FBBC04", orange: "#FF6D01", purple: "#9334E6", gray: "#9AA0A6", grey: "#9AA0A6", black: "#000000", white: "#FFFFFF", lightgreen: "#D9EAD3", lightred: "#F4CCCC", lightyellow: "#FFF2CC", lightblue: "#CFE2F3" }[v.toLowerCase().replace(/\s+/g, "")]; return named || ""; };
  function applyAiActions(tx, wb, calc, startSheet, actions, options = {}) {
    const notes = [];
    let sheet = startSheet;
    let selection = null;
    const sheetNamed = (name) => (name ? wb.sheets.find((s) => s.name.toLowerCase() === String(name).toLowerCase()) : null);
    const target = (action) => sheetNamed(action.sheet) || sheet;
    const setInput = (s, r, c, text) => {
      if (r >= MAX_ROWS || c >= MAX_COLS) return;
      const k = cellKey(r, c);
      const old = s.cells.get(k);
      const raw = text === null || text === undefined ? "" : typeof text === "string" ? text : typeof text === "boolean" ? (text ? "TRUE" : "FALSE") : String(text);
      const parsed = parseInput(raw, options);
      let styleId = old?.s || 0;
      if (parsed.format && !styleOf(wb, old).fmt) styleId = internStyle(wb, { ...styleOf(wb, old), fmt: parsed.format });
      const next = parsed.formula !== undefined ? makeCell(undefined, parsed.formula.replace(/_xlfn\./gi, ""), styleId) : makeCell(parsed.value, undefined, styleId);
      tx.setCell(s, k, next);
      if (r >= s.rowCount) tx.set(s, "rowCount", Math.min(MAX_ROWS, r + 100));
      if (c >= s.colCount) tx.set(s, "colCount", Math.min(MAX_COLS, c + 5));
    };
    const styleRange = (s, range, patch) => {
      const r2 = Math.min(range.r2, range.r1 + 5000), c2 = Math.min(range.c2, range.c1 + 200);
      for (let r = range.r1; r <= r2; r++) for (let c = range.c1; c <= c2; c++) {
        const k = cellKey(r, c), old = s.cells.get(k);
        const style = { ...styleOf(wb, old), ...patch };
        tx.setCell(s, k, makeCell(old?.v, old?.f, internStyle(wb, style)));
      }
    };
    for (const action of (actions || []).slice(0, 200)) {
      if (!action || !AI_ACTIONS.has(action.type)) continue;
      try {
        const s = target(action);
        switch (action.type) {
          case "write": {
            const at = parseAddr(String(action.cell || "A1").replace(/^.*!/, "")) || { r: 0, c: 0 };
            const rows = Array.isArray(action.rows) ? action.rows : action.value !== undefined ? [[action.value]] : [];
            let count = 0;
            rows.slice(0, 2000).forEach((row, i) => (Array.isArray(row) ? row : [row]).slice(0, 200).forEach((value, j) => { setInput(s, at.r + i, at.c + j, value); count++; }));
            if (count) notes.push(`Wrote ${count} cell${count === 1 ? "" : "s"} from ${quoteSheet(s.name)}!${addr(at.r, at.c)}`);
            break;
          }
          case "clear": {
            const ref = aiRange(action.range); if (!ref) break;
            const range = ref.range;
            for (const k of [...s.cells.keys()]) { const r = keyRow(k), c = keyCol(k); if (inRange(range, r, c)) { const old = s.cells.get(k); tx.setCell(s, k, action.formats ? null : makeCell(undefined, undefined, old.s)); } }
            notes.push(`Cleared ${rangeText(range)}`);
            break;
          }
          case "format": case "number_format": {
            const ref = aiRange(action.range); if (!ref) break;
            const patch = {};
            if (action.bold !== undefined) patch.b = !!action.bold;
            if (action.italic !== undefined) patch.i = !!action.italic;
            if (action.underline !== undefined) patch.u = !!action.underline;
            if (action.strikethrough !== undefined) patch.s = !!action.strikethrough;
            if (action.color) patch.color = AI_COLOR(action.color) || undefined;
            if (action.fill) patch.fill = AI_COLOR(action.fill) || undefined;
            if (action.align && ["left", "center", "right"].includes(action.align)) patch.h = action.align;
            if (action.valign && ["top", "middle", "bottom"].includes(action.valign)) patch.v = action.valign;
            if (action.wrap !== undefined) patch.wrap = !!action.wrap;
            if (action.fontSize) patch.size = Math.max(6, Math.min(72, Number(action.fontSize) || 10));
            if (action.font) patch.font = String(action.font).slice(0, 60);
            if (action.numberFormat) patch.fmt = String(action.numberFormat).slice(0, 120);
            if (action.border) { const b = { w: ["thin", "medium", "thick", "dashed", "dotted", "double"].includes(action.border) ? action.border : "thin", c: "#000000" }; Object.assign(patch, { bt: b, bb: b, bl: b, br: b }); }
            styleRange(s, ref.range, patch);
            notes.push(`Formatted ${rangeText(ref.range)}`);
            break;
          }
          case "insert_rows": { const at = Math.max(1, Math.trunc(Number(action.at) || 1)), n = Math.max(1, Math.min(1000, Math.trunc(Number(action.count) || 1))); insertRows(tx, wb, s, at - 1, n); notes.push(`Inserted ${n} row${n === 1 ? "" : "s"} at ${at}`); break; }
          case "delete_rows": { const at = Math.max(1, Math.trunc(Number(action.at) || 1)), n = Math.max(1, Math.min(1000, Math.trunc(Number(action.count) || 1))); deleteRows(tx, wb, s, at - 1, n); notes.push(`Deleted ${n} row${n === 1 ? "" : "s"} from ${at}`); break; }
          case "insert_columns": case "delete_columns": {
            const raw = String(action.at || "A");
            const at = /^\d+$/.test(raw) ? Number(raw) - 1 : colIndex(raw.replace(/[^A-Za-z]/g, "") || "A");
            const n = Math.max(1, Math.min(200, Math.trunc(Number(action.count) || 1)));
            (action.type === "insert_columns" ? insertCols : deleteCols)(tx, wb, s, Math.max(0, at), n);
            notes.push(`${action.type === "insert_columns" ? "Inserted" : "Deleted"} ${n} column${n === 1 ? "" : "s"} at ${colName(Math.max(0, at))}`);
            break;
          }
          case "add_sheet": {
            const name = uniqueSheetName(wb, validSheetName(action.name) ? String(action.name).trim() : "Sheet1");
            tx.touchWorkbook("sheets");
            const created = createSheet(name);
            wb.sheets = [...wb.sheets, created];
            sheet = created;
            notes.push(`Added sheet ${quoteSheet(name)}`);
            break;
          }
          case "rename_sheet": { const from = sheetNamed(action.sheet) || sheet; const name = String(action.name || "").trim(); if (validSheetName(name) && !sheetNamed(name)) { renameSheet(tx, wb, from, name); notes.push(`Renamed a sheet to ${quoteSheet(name)}`); } break; }
          case "delete_sheet": { const victim = sheetNamed(action.sheet); if (victim && wb.sheets.length > 1) { removeSheet(tx, wb, victim); if (sheet === victim) sheet = wb.sheets[0]; notes.push(`Deleted sheet ${quoteSheet(victim.name)}`); } break; }
          case "sort": {
            const ref = aiRange(action.range); if (!ref) break;
            const range = { ...ref.range };
            if (action.header) range.r1 += 1;
            const col = action.column ? colIndex(String(action.column).replace(/[^A-Za-z]/g, "") || "A") : range.c1;
            calc.recalc();
            sortRows(tx, wb, calc, s, range, [{ col, desc: !!action.descending }]);
            notes.push(`Sorted ${rangeText(range)} by column ${colName(col)}`);
            break;
          }
          case "conditional": {
            const ref = aiRange(action.range); if (!ref) break;
            const style = {};
            if (action.fill) style.fill = AI_COLOR(action.fill) || "#F4CCCC";
            if (action.color) style.color = AI_COLOR(action.color);
            if (action.bold) style.b = true;
            if (!style.fill && !style.color && !style.b) style.fill = "#F4CCCC";
            const rule = { id: uid(), range: rangeText(ref.range), style };
            const op = String(action.rule || "gt");
            if (op === "scale") Object.assign(rule, { type: "scale", colors: [AI_COLOR(action.low) || "#F8696B", AI_COLOR(action.mid) || "#FFEB84", AI_COLOR(action.high) || "#63BE7B"] });
            else if (["contains", "notcontains", "begins", "ends", "equals"].includes(op)) Object.assign(rule, { type: "text", op, text: String(action.value ?? "") });
            else if (op === "formula") Object.assign(rule, { type: "formula", formula: String(action.value || "").replace(/^=/, "") });
            else if (["blank", "notblank", "duplicate", "unique", "error"].includes(op)) rule.type = op;
            else Object.assign(rule, { type: "cell", op: ["gt", "lt", "ge", "le", "eq", "ne", "between", "notbetween"].includes(op) ? op : "gt", value: String(action.value ?? ""), value2: action.value2 === undefined ? "" : String(action.value2) });
            tx.set(s, "conditional", [...s.conditional, rule]);
            notes.push(`Added conditional formatting to ${rangeText(ref.range)}`);
            break;
          }
          case "chart": {
            const ref = aiRange(action.range); if (!ref) break;
            const type = ["column", "bar", "line", "area", "pie", "scatter"].includes(action.chartType) ? action.chartType : "column";
            const anchor = { r: ref.range.r1, c: ref.range.c2 + 2 };
            tx.set(s, "charts", [...s.charts, { id: uid(), type, range: (ref.sheet ? quoteSheet(ref.sheet) + "!" : "") + rangeText(ref.range), title: String(action.title || "").slice(0, 120), anchor, w: 480, h: 300, headers: true }]);
            notes.push(`Added a ${type} chart of ${rangeText(ref.range)}`);
            break;
          }
          case "freeze": tx.set(s, "freezeRows", Math.max(0, Math.min(100, Math.trunc(Number(action.rows) || 0)))); tx.set(s, "freezeCols", Math.max(0, Math.min(50, Math.trunc(Number(action.columns) || 0)))); notes.push("Changed frozen rows and columns"); break;
          case "column_width": {
            const c = colIndex(String(action.column || "A").replace(/[^A-Za-z]/g, "") || "A");
            const widths = new Map(s.colWidths);
            widths.set(c, Math.max(20, Math.min(1000, Math.trunc(Number(action.width) || 100))));
            tx.set(s, "colWidths", widths);
            break;
          }
          case "merge": { const ref = aiRange(action.range); if (!ref) break; tx.set(s, "merges", [...s.merges.filter((m) => !rangesOverlap(m, ref.range)), ref.range]); notes.push(`Merged ${rangeText(ref.range)}`); break; }
          case "unmerge": { const ref = aiRange(action.range); if (!ref) break; tx.set(s, "merges", s.merges.filter((m) => !rangesOverlap(m, ref.range))); break; }
          case "filter": { const ref = aiRange(action.range); if (!ref) break; tx.set(s, "filter", { range: ref.range, criteria: new Map() }); notes.push(`Added a filter to ${rangeText(ref.range)}`); break; }
          case "validation": {
            const ref = aiRange(action.range); if (!ref) break;
            const values = (Array.isArray(action.values) ? action.values : String(action.values || "").split(",")).map((v) => String(v).trim()).filter(Boolean).slice(0, 500);
            const rule = action.checkbox ? { id: uid(), range: rangeText(ref.range), type: "checkbox" } : { id: uid(), range: rangeText(ref.range), type: "list", values, dropdown: true, strict: true };
            tx.set(s, "validations", [...s.validations.filter((v) => !rangesOverlap(parseRange(v.range) || ref.range, ref.range)), rule]);
            notes.push(`Added ${rule.type === "checkbox" ? "checkboxes" : "a dropdown list"} to ${rangeText(ref.range)}`);
            break;
          }
          case "note": { const at = parseAddr(String(action.cell || "").replace(/^.*!/, "")); if (!at) break; const map = new Map(s.notes); if (action.text) map.set(cellKey(at.r, at.c), String(action.text).slice(0, 2000)); else map.delete(cellKey(at.r, at.c)); tx.set(s, "notes", map); break; }
          case "select": { const ref = aiRange(action.range); if (ref) selection = { sheet: sheetNamed(ref.sheet) || s, range: ref.range }; break; }
          default: break;
        }
      } catch (error) { notes.push(`Skipped one step: ${error.message}`); }
    }
    return { notes, sheet, selection };
  }
  return {
    MAX_ROWS, MAX_COLS, ERRORS, SheetError, isErr, colName, colIndex, cellKey, keyRow, keyCol, addr, parseAddr, parseRange, rangeText, normRange, inRange, rangesOverlap,
    serialFromDate, dateFromSerial, serialFromJsDate, formatValue, formatGeneral, valueToText, parseInput, isDateFormat, BUILTIN_FORMATS, PALETTE,
    parseFormula, astText, shiftFormula, rewriteFormula, formulaReferences, referenceTokens, toggleAbsolute, quoteSheet, FormulaSyntax,
    Calc, Range, FUNCTIONS, FUNCTION_NAMES, compareValues, criterion, excelFormulaText,
    createWorkbook, createSheet, uniqueSheetName, validSheetName, internStyle, styleOf, makeCell, cleanStyle, Transaction, applyHistory,
    insertRows, deleteRows, insertCols, deleteCols, renameSheet, removeSheet, cloneSheet, mergeAt, expandToMerges, usedRange, extentOf, dataRegion,
    fillValues, sortRows, computeFilterRows, displayText, conditionalContext, mixColors, luminance, parseHex, validationAt, validationOptions, validateValue,
    chartData, pivotTable, serializeWorkbook, deserializeWorkbook, uid, DEFAULT_COL_WIDTH, DEFAULT_ROW_HEIGHT,
    readZip, writeZip, parseXml, readXlsx, writeXlsx, readXls, readCfb, readSpreadsheetMl, r1c1ToA1, parseCsv, workbookFromRows, sheetToCsv, sniffFormat, importWorkbook,
    packWorkbook, unpackWorkbook, writeXls, writeCfb, compileBiffFormula, readOds, writeOds, openFormulaText, fromOpenFormula, workbookHtml, sheetHtmlTable, sheetToMarkdown, workbookJson, tablesToWorkbook, aiContext, applyAiActions, AI_ACTIONS, bytesToBase64, base64ToBytes, gzipText, gunzipText, MONTHS, DAYS,
  };
})();
if (typeof window === "undefined" || !window.ForkMeshDashboard) {
  if (typeof module !== "undefined" && module.exports) module.exports = FMSheets;
  return;
}
window.ForkMeshDashboard.registerPage("spreadsheets", ({ select: $, session, escapeHtml, relativeTimeLabel }) => {
  const S = FMSheets;
  const root = $('[data-view="spreadsheets"]');
  if (!root) return;
  const q = (selector) => root.querySelector(selector);
  const qa = (selector) => Array.from(root.querySelectorAll(selector));
  const API = "/api/spreadsheets";
  const AUTOSAVE_MS = 1500;
  const HEADER_H = 24;
  const MAX_UNDO = 200;
  const esc = (value) => escapeHtml(String(value ?? ""));
  const icons = () => window.lucide?.createIcons?.();
  const dayFirst = (() => {
    try {
      const parts = new Intl.DateTimeFormat(undefined).formatToParts(new Date(2026, 10, 2));
      return parts.findIndex((p) => p.type === "day") < parts.findIndex((p) => p.type === "month");
    } catch { return false; }
  })();
  const inputOptions = { dayFirst };
  const state = { installed: false, list: [], limits: {}, doc: null, aiAvailable: true };

  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const current = session();
    if (current?.sessionToken) headers.authorization = `Bearer ${current.sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload;
      throw error;
    }
    return payload;
  }
  function flash(kind, text) {
    const node = q(`[data-sheets-${kind}]`);
    if (!node) return;
    node.querySelector("[data-sheets-message-text]").textContent = text || "";
    node.classList.toggle("hidden", !text);
    if (kind === "notice" && text) { clearTimeout(flash.timer); flash.timer = setTimeout(() => flash("notice", ""), 5000); }
  }
  function toast(text) {
    const node = q("[data-sheets-toast]");
    if (!node) return;
    node.textContent = text;
    node.classList.remove("hidden");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => node.classList.add("hidden"), 3200);
  }
  function download(data, filename, type) {
    const blob = data instanceof Blob ? data : new Blob([data], { type });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url; link.download = filename;
    document.body.appendChild(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
  }
  const safeFilename = (title) => (String(title || "Spreadsheet").replace(/[\\/:*?"<>|\x00-\x1F]+/g, " ").trim().slice(0, 100) || "Spreadsheet");
  const formatBytes = (n) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : n >= 1024 ? `${Math.round(n / 1024)} KB` : `${n} B`);
  function when(ms) {
    if (!ms) return "";
    try { return relativeTimeLabel ? relativeTimeLabel(new Date(Number(ms)).toISOString()) : new Date(Number(ms)).toLocaleString(); }
    catch { return new Date(Number(ms)).toLocaleString(); }
  }
  function htmlTables(text, base) {
    const doc = new DOMParser().parseFromString(text, "text/html");
    const tables = [...doc.querySelectorAll("table")].map((table) => [...table.rows].map((row) => [...row.cells].map((cell) => {
      const style = {};
      const css = cell.getAttribute("style") || "";
      if (/font-weight:\s*(bold|[6-9]00)/i.test(css) || cell.querySelector("b,strong") || cell.tagName === "TH") style.b = true;
      if (/font-style:\s*italic/i.test(css) || cell.querySelector("i,em")) style.i = true;
      const color = /(?:^|;)\s*color:\s*(#[0-9a-f]{6})/i.exec(css), fill = /background(?:-color)?:\s*(#[0-9a-f]{6})/i.exec(css);
      if (color) style.color = color[1].toUpperCase();
      if (fill) style.fill = fill[1].toUpperCase();
      const align = /text-align:\s*(left|center|right)/i.exec(css);
      if (align) style.h = align[1].toLowerCase();
      const link = cell.querySelector("a[href]");
      return { text: cell.innerText ?? cell.textContent, style, colspan: cell.colSpan || 1, rowspan: cell.rowSpan || 1, href: link && /^(https?:|mailto:)/i.test(link.getAttribute("href")) ? link.getAttribute("href") : "" };
    })));
    if (!tables.length) throw new Error("This web page holds no tables.");
    return S.tablesToWorkbook(tables, base);
  }

  function renderInstall() {
    q("[data-sheets-install-panel]")?.classList.toggle("hidden", state.installed);
    q("[data-sheets-home]")?.classList.toggle("hidden", !state.installed);
    qa("[data-sheets-requires-install]").forEach((node) => node.classList.toggle("hidden", !state.installed));
  }
  function renderList() {
    const list = q("[data-sheets-items]");
    if (!list) return;
    const filter = String(q("[data-sheets-search]")?.value || "").toLowerCase();
    const items = state.list.filter((item) => !filter || String(item.title).toLowerCase().includes(filter));
    q("[data-sheets-count]").textContent = state.list.length ? `${state.list.length} spreadsheet${state.list.length === 1 ? "" : "s"}` : "";
    if (!state.list.length) {
      list.innerHTML = `<div class="fm-blankslate"><i data-lucide="sheet"></i><p class="fm-blankslate-title">No spreadsheets yet</p><p class="fm-blankslate-desc">Create a blank spreadsheet, open an Excel (.xls or .xlsx), OpenDocument or CSV file, or describe one for the AI assistant to build. You can also drop a file anywhere on this page.</p></div>`;
      icons();
      return;
    }
    list.innerHTML = items.map((item) => `<div class="sheets-row" data-sheets-row="${esc(item.id)}">
        <button type="button" class="sheets-row-main" data-sheets-open="${esc(item.id)}"><i data-lucide="sheet"></i><span class="sheets-row-title">${esc(item.title)}</span></button>
        <span class="sheets-row-meta">${esc(when(item.updatedAt))}</span>
        <span class="sheets-row-meta sheets-row-size">${esc(formatBytes(Number(item.bytes || 0)))}</span>
        <button type="button" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon" data-sheets-row-menu="${esc(item.id)}" aria-label="More actions for ${esc(item.title)}"><i data-lucide="ellipsis"></i></button>
      </div>`).join("") || `<p class="px-4 py-6 text-sm text-muted-foreground">No spreadsheet matches “${esc(filter)}”.</p>`;
    icons();
  }
  async function loadList() {
    try {
      const payload = await request(API);
      state.installed = payload.installed !== false;
      state.list = Array.isArray(payload.workbooks) ? payload.workbooks : [];
      state.limits = payload.limits || {};
      state.aiAvailable = payload.ai !== false;
      flash("error", "");
      renderInstall();
      renderList();
      const ai = q("[data-sheets-ai-create]");
      if (ai) ai.classList.toggle("hidden", !state.aiAvailable);
    } catch (error) {
      if (error.status === 409) { state.installed = false; renderInstall(); }
      else if (error.status === 401) flash("error", "Sign in to use Spreadsheets.");
      else flash("error", `Spreadsheets is unavailable: ${error.message}`);
    }
  }

  function newDoc(wb, meta) {
    const calc = new S.Calc(wb);
    const doc = {
      id: meta.id || "", title: meta.title || "Untitled spreadsheet", revision: Number(meta.revision || 0),
      wb, calc, undo: [], redo: [], dirty: false, saving: false, saveError: "", conflict: false,
      sel: { r: 0, c: 0, r1: 0, c1: 0, r2: 0, c2: 0 }, anchor: { r: 0, c: 0 }, editing: null, clipboard: null,
      zoom: 1, showFormulas: false, layoutVersion: 0, painter: null, scroll: new Map(), lastSaved: 0,
    };
    return doc;
  }
  const doc = () => state.doc;
  const sheet = () => state.doc.wb.sheets[state.doc.wb.active] || state.doc.wb.sheets[0];

  function recalc() {
    const d = doc();
    d.calc.recalc();
    let again = false;
    for (const s of d.wb.sheets) {
      const next = s.filter ? S.computeFilterRows(d.calc, d.wb, s) : new Set();
      const before = s.filterRows || new Set();
      if (next.size !== before.size || [...next].some((r) => !before.has(r))) again = true;
      s.filterRows = next;
    }
    if (again) d.calc.recalc();
    d.cf = null;
  }
  function act(label, fn, options = {}) {
    const d = doc();
    if (!d) return null;
    const tx = new S.Transaction(d.wb, label);
    let result;
    try { result = fn(tx); }
    catch (error) { const entry = tx.finish(); if (entry) S.applyHistory(d.wb, entry, "undo"); toast(error.message || "That did not work."); return null; }
    const entry = tx.finish();
    if (entry) {
      entry.sel = { ...d.sel, sheet: d.wb.active };
      entry.active = d.wb.active;
      d.undo.push(entry);
      if (d.undo.length > MAX_UNDO) d.undo.shift();
      d.redo = [];
      changed(options);
    }
    return result;
  }
  function changed(options = {}) {
    const d = doc();
    d.layoutVersion++;
    if (d.wb.active >= d.wb.sheets.length) d.wb.active = d.wb.sheets.length - 1;
    recalc();
    markDirty();
    if (!options.quiet) { renderTabs(); renderToolbarState(); }
    renderCharts();
    draw();
    updateFormulaBar();
    updateStatus();
  }
  function history(direction) {
    const d = doc();
    if (!d) return;
    if (d.editing) cancelEdit();
    const stack = direction === "undo" ? d.undo : d.redo;
    const entry = stack.pop();
    if (!entry) return;
    S.applyHistory(d.wb, entry, direction);
    (direction === "undo" ? d.redo : d.undo).push(entry);
    if (entry.sel && d.wb.sheets[entry.sel.sheet]) { d.wb.active = entry.sel.sheet; d.sel = { ...entry.sel }; delete d.sel.sheet; }
    changed();
    toast(`${direction === "undo" ? "Undid" : "Redid"} ${entry.label.toLowerCase()}`);
  }

  function markDirty() {
    const d = doc();
    d.dirty = true;
    renderSaveState();
    clearTimeout(d.saveTimer);
    d.saveTimer = setTimeout(() => save(), AUTOSAVE_MS);
  }
  function renderSaveState() {
    const d = doc();
    const node = q("[data-sheets-save-state]");
    if (!node || !d) return;
    node.textContent = d.conflict ? "Changed on another device" : d.saveError ? `Not saved: ${d.saveError}` : d.saving ? "Saving…" : d.dirty ? "Unsaved changes" : d.lastSaved ? "All changes saved" : "";
    node.classList.toggle("text-destructive", !!(d.saveError || d.conflict));
  }
  async function save(force = false) {
    const d = doc();
    if (!d || d.saving || (!d.dirty && !force) || (d.conflict && !force)) return;
    clearTimeout(d.saveTimer);
    d.saving = true; d.saveError = "";
    renderSaveState();
    const startedVersion = d.layoutVersion;
    try {
      const data = await S.packWorkbook(d.wb, d.calc);
      if (data.length > (state.limits.maxDataChars || 3000000)) throw new Error("this spreadsheet is too large to keep in your account; download it as .xlsx");
      const payload = d.id
        ? await request(`${API}/workbooks/${encodeURIComponent(d.id)}`, "PUT", { title: d.title, data, revision: d.revision, force })
        : await request(`${API}/workbooks`, "POST", { title: d.title, data });
      d.id = payload.workbook.id;
      d.revision = Number(payload.workbook.revision || 0);
      d.conflict = false;
      d.lastSaved = Date.now();
      if (d.layoutVersion === startedVersion) d.dirty = false;
      const row = state.list.find((item) => item.id === d.id);
      if (row) Object.assign(row, payload.workbook); else state.list.unshift(payload.workbook);
    } catch (error) {
      if (error.status === 409 && error.payload?.error === "workbook_changed") { d.conflict = true; showConflict(); }
      else d.saveError = error.message;
    } finally {
      d.saving = false;
      renderSaveState();
      if (d.dirty && !d.conflict && !d.saveError) d.saveTimer = setTimeout(() => save(), AUTOSAVE_MS);
    }
  }
  function showConflict() {
    const bar = q("[data-sheets-conflict]");
    bar?.classList.remove("hidden");
  }

  async function openWorkbook(id) {
    try {
      const payload = await request(`${API}/workbooks/${encodeURIComponent(id)}`);
      const wb = await S.unpackWorkbook(payload.workbook.data);
      openEditor(newDoc(wb, payload.workbook));
      history_replace(id);
    } catch (error) { flash("error", `Could not open that spreadsheet: ${error.message}`); }
  }
  function history_replace(id) {
    try { const url = new URL(location.href); if (id) url.searchParams.set("sheet", id); else url.searchParams.delete("sheet"); window.history.replaceState(null, "", url); } catch { return; }
  }
  async function createWorkbook(wb, title) {
    const d = newDoc(wb, { title });
    openEditor(d);
    d.dirty = true;
    await save(true);
    if (d.id) history_replace(d.id);
  }
  async function importFiles(files, intoCurrent = false) {
    for (const file of files) {
      try {
        if (file.size > 200 * 1024 * 1024) throw new Error("files over 200 MB cannot be opened here");
        const bytes = new Uint8Array(await file.arrayBuffer());
        const { wb } = await S.importWorkbook(bytes, file.name, { ...inputOptions, htmlTables });
        if (intoCurrent && doc()) {
          act("Import sheets", (tx) => {
            tx.touchWorkbook("sheets");
            tx.touchWorkbook("styles");
            const target = doc().wb;
            const added = wb.sheets.map((source) => {
              const copy = S.cloneSheet(wb, source, S.uniqueSheetName(target, source.name));
              for (const [k, cell] of copy.cells) if (cell.s) copy.cells.set(k, { ...cell, s: S.internStyle(target, wb.styles[cell.s] || {}) });
              return copy;
            });
            target.sheets = [...target.sheets, ...added];
            target.active = target.sheets.length - added.length;
          });
          toast(`Imported ${wb.sheets.length} sheet${wb.sheets.length === 1 ? "" : "s"} from ${file.name}`);
        } else {
          await createWorkbook(wb, file.name.replace(/\.[^.]+$/, "") || "Imported spreadsheet");
          toast(`Opened ${file.name}`);
        }
      } catch (error) {
        const text = `Could not open ${file.name}: ${error.message}`;
        if (doc()) toast(text); else flash("error", text);
      }
    }
  }

  function openEditor(d) {
    state.doc = d;
    recalc();
    q("[data-sheets-list]").classList.add("hidden");
    q("[data-sheets-editor]").classList.remove("hidden");
    q("[data-sheets-conflict]")?.classList.add("hidden");
    q("[data-sheets-title]").value = d.title;
    buildEditorOnce();
    renderTabs();
    renderToolbarState();
    resize();
    renderCharts();
    updateFormulaBar();
    updateStatus();
    renderSaveState();
    focusGrid();
  }
  async function closeEditor() {
    const d = doc();
    if (d) {
      if (d.editing) commitEdit();
      if (d.dirty) await save();
    }
    state.doc = null;
    closePopovers();
    q("[data-sheets-editor]").classList.add("hidden");
    q("[data-sheets-list]").classList.remove("hidden");
    history_replace("");
    renderList();
  }

  const grid = { built: false, colAxis: null, rowAxis: null, version: -1, measure: new Map(), colors: null, raf: 0, width: 0, height: 0, dpr: 1 };
  class Axis {
    constructor(count, def, sizes, hidden, extra) {
      this.count = count; this.def = def; this.sizes = sizes; this.hidden = hidden; this.extra = extra || new Set();
      const special = new Set();
      for (const i of sizes.keys()) if (i < count) special.add(i);
      for (const i of hidden) if (i < count) special.add(i);
      for (const i of this.extra) if (i < count) special.add(i);
      this.idx = [...special].sort((a, b) => a - b);
      this.prefix = [];
      let total = 0;
      for (const i of this.idx) { total += this.size(i) - def; this.prefix.push(total); }
    }
    size(i) { return this.hidden.has(i) || this.extra.has(i) ? 0 : this.sizes.get(i) ?? this.def; }
    offset(i) {
      let lo = 0, hi = this.idx.length;
      while (lo < hi) { const mid = (lo + hi) >> 1; if (this.idx[mid] < i) lo = mid + 1; else hi = mid; }
      return i * this.def + (lo ? this.prefix[lo - 1] : 0);
    }
    index(px) {
      if (px <= 0) return 0;
      let lo = 0, hi = this.count - 1;
      while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (this.offset(mid) <= px) lo = mid; else hi = mid - 1; }
      return lo;
    }
    total() { return this.offset(this.count); }
  }
  function axes() {
    const d = doc(), s = sheet();
    const key = `${d.layoutVersion}:${d.wb.active}`;
    if (grid.version !== key) {
      grid.colAxis = new Axis(s.colCount, s.defaultColWidth, s.colWidths, s.hiddenCols);
      grid.rowAxis = new Axis(s.rowCount, s.defaultRowHeight, s.rowHeights, s.hiddenRows, s.filterRows);
      grid.version = key;
    }
    return { cols: grid.colAxis, rows: grid.rowAxis };
  }
  const headerWidth = () => Math.max(46, 14 + String(sheet().rowCount).length * 8);
  function readColors() {
    const cs = getComputedStyle(document.documentElement);
    const v = (name, alpha = 1) => `rgb(${cs.getPropertyValue(`--dashboard-${name}-rgb`).trim() || "128 128 128"} / ${alpha})`;
    grid.colors = {
      bg: v("background"), fg: v("foreground"), muted: v("muted-foreground"), border: v("border"), gridline: v("border", 0.8),
      header: v("subtle"), headerActive: v("primary", 0.16), headerStrong: v("primary", 0.32), primary: v("primary"), primaryText: v("primary-foreground"),
      selection: v("primary", 0.12), handle: v("primary"), link: v("accent"), frozen: v("muted-foreground", 0.45), danger: v("destructive"),
      dark: document.documentElement.getAttribute("data-dashboard-theme") === "dark",
    };
  }
  function screenX(c) { const { cols } = axes(); const s = sheet(), z = doc().zoom; return headerWidth() + cols.offset(c) * z - (c < s.freezeCols ? 0 : grid.scroller.scrollLeft); }
  function screenY(r) { const { rows } = axes(); const s = sheet(), z = doc().zoom; return HEADER_H + rows.offset(r) * z - (r < s.freezeRows ? 0 : grid.scroller.scrollTop); }
  function cellRect(r, c) {
    const s = sheet(), z = doc().zoom, { rows, cols } = axes();
    const m = S.mergeAt(s, r, c);
    if (m) return { x: screenX(m.c1), y: screenY(m.r1), w: (cols.offset(m.c2 + 1) - cols.offset(m.c1)) * z, h: (rows.offset(m.r2 + 1) - rows.offset(m.r1)) * z, merge: m };
    return { x: screenX(c), y: screenY(r), w: cols.size(c) * z, h: rows.size(r) * z, merge: null };
  }
  function hitTest(x, y) {
    const s = sheet(), z = doc().zoom, { rows, cols } = axes();
    const hw = headerWidth();
    const frozenW = cols.offset(s.freezeCols) * z, frozenH = rows.offset(s.freezeRows) * z;
    const colAt = (px) => { const rel = px - hw; return Math.min(s.colCount - 1, cols.index((rel < frozenW ? rel : rel + grid.scroller.scrollLeft) / z)); };
    const rowAt = (py) => { const rel = py - HEADER_H; return Math.min(s.rowCount - 1, rows.index((rel < frozenH ? rel : rel + grid.scroller.scrollTop) / z)); };
    if (x < hw && y < HEADER_H) return { area: "corner" };
    if (y < HEADER_H) {
      const c = colAt(x);
      const left = screenX(c), right = left + cols.size(c) * z;
      if (Math.abs(x - right) <= 4) return { area: "colEdge", c };
      if (Math.abs(x - left) <= 3 && c > 0) { let p = c - 1; while (p > 0 && cols.size(p) === 0) p--; return { area: "colEdge", c: p }; }
      return { area: "colHeader", c };
    }
    if (x < hw) {
      const r = rowAt(y);
      const top = screenY(r), bottom = top + rows.size(r) * z;
      if (Math.abs(y - bottom) <= 3) return { area: "rowEdge", r };
      if (Math.abs(y - top) <= 2 && r > 0) { let p = r - 1; while (p > 0 && rows.size(p) === 0) p--; return { area: "rowEdge", r: p }; }
      return { area: "rowHeader", r };
    }
    const r = rowAt(y), c = colAt(x);
    const sel = doc().sel;
    const end = cellRect(sel.r2, sel.c2);
    const handleX = screenX(sel.c2) + cols.size(sel.c2) * z, handleY = screenY(sel.r2) + rows.size(sel.r2) * z;
    if (!doc().editing && Math.abs(x - (end.merge ? end.x + end.w : handleX)) <= 5 && Math.abs(y - (end.merge ? end.y + end.h : handleY)) <= 5) return { area: "handle", r, c };
    return { area: "cell", r, c };
  }

  function styleAt(s, r, c, cell) {
    const d = doc();
    let style = S.styleOf(d.wb, cell);
    if (s.conditional.length) {
      if (!d.cf || d.cf.sheet !== s) d.cf = { sheet: s, fn: S.conditionalContext(d.calc, d.wb, s) };
      const overlay = d.cf.fn(r, c);
      if (overlay) style = { ...style, ...overlay };
    }
    return style;
  }
  function fontFor(style, z) {
    const d = doc();
    const size = (style.size || d.wb.defaultSize) * (4 / 3) * z;
    const family = style.font || d.wb.defaultFont || "Arial";
    return `${style.i ? "italic " : ""}${style.b ? "bold " : ""}${size.toFixed(1)}px "${family.replace(/"/g, "")}", Arial, sans-serif`;
  }
  function measure(ctx, text) {
    const key = ctx.font + "\u0000" + text;
    let w = grid.measure.get(key);
    if (w === undefined) {
      w = ctx.measureText(text).width;
      if (grid.measure.size > 20000) grid.measure.clear();
      grid.measure.set(key, w);
    }
    return w;
  }
  function wrapLines(ctx, text, width) {
    const out = [];
    for (const paragraph of String(text).split("\n")) {
      const words = paragraph.split(/(\s+)/);
      let line = "";
      for (const word of words) {
        const next = line + word;
        if (line && measure(ctx, next.trimEnd()) > width) { out.push(line.trimEnd()); line = word.trimStart(); }
        else line = next;
        while (measure(ctx, line) > width && line.length > 1) {
          let cut = line.length - 1;
          while (cut > 1 && measure(ctx, line.slice(0, cut)) > width) cut--;
          out.push(line.slice(0, cut)); line = line.slice(cut);
        }
      }
      out.push(line);
    }
    return out;
  }
  function shownValue(s, r, c, cell, style) {
    const d = doc();
    if (d.showFormulas && cell && cell.f !== undefined) return { text: "=" + cell.f, color: "", formula: true };
    const value = d.calc.cellValue(s, r, c);
    let fmt = style.fmt;
    if (!cell) { const owner = d.calc.spillOwner(s, r, c); if (owner !== null) fmt = S.styleOf(d.wb, s.cells.get(owner)).fmt; }
    if (fmt === undefined && cell && cell.f !== undefined && typeof value === "number") {
      const refs = S.formulaReferences(cell.f);
      if (refs.length) {
        const ref = refs[0];
        const other = ref.sheet ? d.calc.sheetFor(ref.sheet, { sheet: s }) : s;
        const source = other?.cells.get(S.cellKey(ref.r1, ref.c1));
        const inherited = S.styleOf(d.wb, source).fmt;
        if (inherited && (S.isDateFormat(inherited) || /[$€£¥%]/.test(inherited))) fmt = inherited;
      }
    }
    const out = S.formatValue(value, fmt);
    return { ...out, value, fmt };
  }
  function borderStroke(ctx, b, z) {
    const width = { medium: 2, mediumDashed: 2, mediumDashDot: 2, mediumDashDotDot: 2, thick: 3, double: 3 }[b.w] || 1;
    ctx.lineWidth = Math.max(1, Math.round(width * Math.min(1.5, z)));
    ctx.strokeStyle = b.c || "#000";
    ctx.setLineDash(/dash/i.test(b.w) ? [4, 2] : /dot|hair/i.test(b.w) ? [1, 2] : []);
  }
  function scheduleDraw() { if (!grid.raf) grid.raf = requestAnimationFrame(() => { grid.raf = 0; drawNow(); }); }
  const draw = scheduleDraw;

  function drawNow() {
    const d = doc();
    if (!d || !grid.canvas) return;
    const ctx = grid.ctx, s = sheet(), z = d.zoom, colors = grid.colors;
    const { rows, cols } = axes();
    const W = grid.width, H = grid.height, hw = headerWidth();
    ctx.setTransform(grid.dpr, 0, 0, grid.dpr, 0, 0);
    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, W, H);
    const sx = grid.scroller.scrollLeft, sy = grid.scroller.scrollTop;
    const frozenW = cols.offset(s.freezeCols) * z, frozenH = rows.offset(s.freezeRows) * z;
    const lastCol = (left) => Math.min(s.colCount - 1, cols.index((left + W - hw) / z));
    const lastRow = (top) => Math.min(s.rowCount - 1, rows.index((top + H - HEADER_H) / z));
    const mainC0 = Math.max(s.freezeCols, cols.index((sx + frozenW) / z)), mainR0 = Math.max(s.freezeRows, rows.index((sy + frozenH) / z));
    const regions = [
      { r0: mainR0, r1: lastRow(sy), c0: mainC0, c1: lastCol(sx), x: hw + frozenW, y: HEADER_H + frozenH },
      { r0: 0, r1: s.freezeRows - 1, c0: mainC0, c1: lastCol(sx), x: hw + frozenW, y: HEADER_H },
      { r0: mainR0, r1: lastRow(sy), c0: 0, c1: s.freezeCols - 1, x: hw, y: HEADER_H + frozenH },
      { r0: 0, r1: s.freezeRows - 1, c0: 0, c1: s.freezeCols - 1, x: hw, y: HEADER_H },
    ];
    for (const region of regions) {
      if (region.r1 < region.r0 || region.c1 < region.c0) continue;
      ctx.save();
      ctx.beginPath();
      ctx.rect(region.x, region.y, W - region.x, H - region.y);
      ctx.clip();
      drawCells(ctx, s, region, z, colors);
      drawSelection(ctx, s, region, z, colors);
      ctx.restore();
    }
    drawHeaders(ctx, s, regions, z, colors, hw);
    if (s.freezeRows) { ctx.fillStyle = colors.frozen; ctx.fillRect(0, HEADER_H + frozenH - 1, W, 2); }
    if (s.freezeCols) { ctx.fillStyle = colors.frozen; ctx.fillRect(hw + frozenW - 1, 0, 2, H); }
    positionEditor();
    positionCharts();
  }
  function drawCells(ctx, s, region, z, colors) {
    const d = doc(), { rows, cols } = axes();
    const { r0, r1, c0, c1 } = region;
    const xs = [], ws = [];
    for (let c = c0; c <= c1 + 1; c++) xs[c - c0] = screenX(c);
    for (let c = c0; c <= c1; c++) ws[c - c0] = cols.size(c) * z;
    const merges = s.merges.filter((m) => m.r2 >= r0 && m.r1 <= r1 && m.c2 >= c0 && m.c1 <= c1);
    const covered = (r, c) => merges.some((m) => r >= m.r1 && r <= m.r2 && c >= m.c1 && c <= m.c2 && (r !== m.r1 || c !== m.c1));
    if (s.showGrid) {
      ctx.fillStyle = colors.gridline;
      for (let c = c0; c <= c1 + 1; c++) if (cols.size(c - 1) || c === c0) ctx.fillRect(Math.round(xs[c - c0]) - 1, region.y, 1, grid.height);
      for (let r = r0; r <= r1 + 1; r++) ctx.fillRect(region.x, Math.round(screenY(r)) - 1, grid.width, 1);
    }
    const textJobs = [];
    const overflowC0 = Math.max(0, c0 - 20);
    for (let r = r0; r <= r1; r++) {
      const h = rows.size(r) * z;
      if (!h) continue;
      const y = screenY(r);
      for (let c = overflowC0; c <= c1; c++) {
        const inView = c >= c0;
        if (inView && !ws[c - c0]) continue;
        const k = S.cellKey(r, c);
        const cell = s.cells.get(k);
        const hasSpill = !cell && d.calc.spillOwner(s, r, c) !== null;
        if (!cell && !hasSpill && !s.conditional.length && !s.notes.size && !s.validations.length) continue;
        if (inView && covered(r, c)) continue;
        const style = styleAt(s, r, c, cell);
        const m = merges.find((mm) => mm.r1 === r && mm.c1 === c);
        const rect = m ? cellRect(r, c) : { x: inView ? xs[c - c0] : screenX(c), y, w: cols.size(c) * z, h };
        if (inView && style.fill) { ctx.fillStyle = style.fill; ctx.fillRect(rect.x, rect.y, rect.w - 1, rect.h - 1); }
        else if (inView && m) { ctx.fillStyle = colors.bg; ctx.fillRect(rect.x, rect.y, rect.w - 1, rect.h - 1); }
        if (inView) {
          for (const [key, x1, y1, x2, y2] of [["bt", 0, 0, 1, 0], ["bb", 0, 1, 1, 1], ["bl", 0, 0, 0, 1], ["br", 1, 0, 1, 1]]) {
            const b = style[key];
            if (!b) continue;
            borderStroke(ctx, b, z);
            ctx.beginPath();
            const px = (fx) => Math.round(rect.x + fx * rect.w) - 0.5, py = (fy) => Math.round(rect.y + fy * rect.h) - 0.5;
            ctx.moveTo(px(x1), py(y1)); ctx.lineTo(px(x2), py(y2)); ctx.stroke();
            if (b.w === "double") { ctx.lineWidth = 1; }
          }
          ctx.setLineDash([]);
          if (s.notes.has(k)) { ctx.fillStyle = "#F4B400"; ctx.beginPath(); ctx.moveTo(rect.x + rect.w - 7, rect.y); ctx.lineTo(rect.x + rect.w - 1, rect.y); ctx.lineTo(rect.x + rect.w - 1, rect.y + 6); ctx.fill(); }
        }
        if (!cell && !hasSpill) { if (inView && s.validations.length) drawValidationMark(ctx, s, r, c, rect, null, colors, z); continue; }
        textJobs.push({ r, c, cell, style, rect, inView, merge: m });
      }
    }
    for (const job of textJobs) drawText(ctx, s, job, z, colors, region, c1);
    if (s.filter) drawFilterButtons(ctx, s, region, z, colors);
  }
  function drawValidationMark(ctx, s, r, c, rect, value, colors, z) {
    const rule = S.validationAt(s, r, c);
    if (!rule) return false;
    if (rule.type === "checkbox") {
      const size = Math.min(14 * z, rect.h - 4);
      const x = rect.x + (rect.w - size) / 2, y = rect.y + (rect.h - size) / 2;
      ctx.lineWidth = 1.5; ctx.strokeStyle = value === true ? colors.primary : colors.muted;
      ctx.strokeRect(x + 0.5, y + 0.5, size - 1, size - 1);
      if (value === true) {
        ctx.fillStyle = colors.primary; ctx.fillRect(x, y, size, size);
        ctx.strokeStyle = colors.primaryText; ctx.lineWidth = 2; ctx.beginPath();
        ctx.moveTo(x + size * 0.22, y + size * 0.52); ctx.lineTo(x + size * 0.42, y + size * 0.72); ctx.lineTo(x + size * 0.78, y + size * 0.3); ctx.stroke();
      }
      return true;
    }
    if (rule.type === "list" && rule.dropdown !== false) {
      const sel = doc().sel;
      if (sel.r === r && sel.c === c || rect.w > 60) {
        ctx.fillStyle = colors.muted;
        const cx = rect.x + rect.w - 10, cy = rect.y + rect.h / 2;
        ctx.beginPath(); ctx.moveTo(cx - 4, cy - 2); ctx.lineTo(cx + 4, cy - 2); ctx.lineTo(cx, cy + 3); ctx.fill();
      }
    }
    return false;
  }
  function drawText(ctx, s, job, z, colors, region, lastVisibleCol) {
    const d = doc(), { cols } = axes();
    const { r, c, cell, style, rect } = job;
    const shown = shownValue(s, r, c, cell, style);
    if (job.inView && drawValidationMark(ctx, s, r, c, rect, shown.value, colors, z)) return;
    let text = shown.text;
    if (!text) return;
    ctx.font = fontFor(style, z);
    const value = shown.value;
    const isNumber = typeof value === "number" && !shown.formula;
    const isLink = (typeof value === "string" && /^https?:\/\/\S+$/i.test(value)) || s.links.has(S.cellKey(r, c)) || (cell && cell.f && /^\s*HYPERLINK\s*\(/i.test(cell.f));
    let color = shown.color || style.color || "";
    if (isLink && !style.color) color = colors.link;
    if (!color) color = style.fill ? (S.luminance(style.fill) < 0.5 ? "#FFFFFF" : "#000000") : colors.fg;
    else if (!style.fill && colors.dark && S.luminance(color) < 0.1) color = colors.fg;
    if (S.isErr(value) && !shown.formula) color = style.color || colors.danger;
    const align = style.h || (shown.formula ? "left" : isNumber ? "right" : typeof value === "boolean" || S.isErr(value) ? "center" : "left");
    const pad = 3 * z + (style.indent || 0) * 9 * z;
    const lineHeight = (style.size || d.wb.defaultSize) * (4 / 3) * z * 1.2;
    ctx.fillStyle = color;
    ctx.textBaseline = "middle";
    let lines = [text];
    let clipX = rect.x, clipW = rect.w - 1;
    if (style.wrap && !job.merge?.c2 === undefined) lines = wrapLines(ctx, text, rect.w - pad * 2);
    else if (style.wrap) lines = wrapLines(ctx, text, rect.w - pad * 2);
    else if (text.includes("\n")) lines = text.split("\n");
    if (!style.wrap && lines.length === 1) {
      const width = measure(ctx, text);
      if (isNumber && width > rect.w - pad * 2 && !job.merge) {
        if (!shown.fmt || shown.fmt === "General") {
          let digits = 10;
          while (digits > 0 && measure(ctx, text) > rect.w - pad * 2) { text = S.formatGeneral(Number(value.toPrecision(digits))); digits--; }
          if (measure(ctx, text) > rect.w - pad * 2) text = "#".repeat(Math.max(1, Math.floor((rect.w - pad * 2) / measure(ctx, "#"))));
        } else text = "#".repeat(Math.max(1, Math.floor((rect.w - pad * 2) / measure(ctx, "#"))));
      } else if (!isNumber && width > rect.w - pad * 2 && !job.merge) {
        const empty = (cc) => { const v = d.calc.cellValue(s, r, cc); return (v === null || v === "") && !S.mergeAt(s, r, cc); };
        if (align === "left" || align === "justify") {
          let end = c + 1, extent = rect.w;
          while (extent < width + pad * 2 && end < s.colCount && end <= lastVisibleCol + 1 && empty(end)) { extent += cols.size(end) * z; end++; }
          clipW = extent - 1;
        } else if (align === "right") {
          let start = c - 1, extent = rect.w;
          while (extent < width + pad * 2 && start >= 0 && empty(start)) { extent += cols.size(start) * z; start--; }
          clipX = rect.x + rect.w - extent; clipW = extent - 1;
        } else {
          let left = c - 1, right = c + 1, extent = rect.w, lx = rect.x;
          while (extent < width + pad * 2 && ((left >= 0 && empty(left)) || (right < s.colCount && empty(right)))) {
            if (left >= 0 && empty(left)) { const w = cols.size(left) * z; extent += w; lx -= w; left--; }
            if (extent < width + pad * 2 && right < s.colCount && empty(right)) { extent += cols.size(right) * z; right++; }
            if (left < 0 && right >= s.colCount) break;
          }
          clipX = lx; clipW = extent - 1;
        }
      }
    }
    if (!job.inView && clipX + clipW < screenX(region.c0)) return;
    ctx.save();
    ctx.beginPath();
    ctx.rect(clipX, rect.y, clipW, rect.h - 1);
    ctx.clip();
    const totalH = lines.length * lineHeight;
    const v = style.v || "bottom";
    let y = v === "top" ? rect.y + 2 * z + lineHeight / 2 : v === "middle" ? rect.y + (rect.h - totalH) / 2 + lineHeight / 2 : rect.y + rect.h - totalH - 2 * z + lineHeight / 2;
    if (style.rot && lines.length === 1) {
      const angle = style.rot <= 90 ? -style.rot : style.rot - 90;
      ctx.translate(rect.x + rect.w / 2, rect.y + rect.h / 2);
      ctx.rotate((angle * Math.PI) / 180);
      ctx.textAlign = "center";
      ctx.fillText(text, 0, 0);
      ctx.restore();
      return;
    }
    for (const line of lines) {
      let x;
      const width = measure(ctx, line);
      if (align === "center") x = rect.x + (rect.w - width) / 2;
      else if (align === "right") x = rect.x + rect.w - pad - width;
      else x = rect.x + pad;
      ctx.textAlign = "left";
      ctx.fillText(line, x, y);
      if (style.u || isLink || style.s) {
        ctx.fillRect(x, style.s ? y : y + lineHeight * 0.36, width, Math.max(1, z));
        if (style.u && style.s) ctx.fillRect(x, y + lineHeight * 0.36, width, Math.max(1, z));
      }
      y += lineHeight;
    }
    ctx.restore();
  }
  function drawFilterButtons(ctx, s, region, z, colors) {
    const range = s.filter.range;
    if (range.r1 < region.r0 || range.r1 > region.r1) return;
    for (let c = Math.max(range.c1, region.c0); c <= Math.min(range.c2, region.c1); c++) {
      const rect = cellRect(range.r1, c);
      const size = Math.min(16 * z, rect.h - 4);
      const x = rect.x + rect.w - size - 3, y = rect.y + (rect.h - size) / 2;
      const active = s.filter.criteria.has(c);
      ctx.fillStyle = active ? colors.headerStrong : colors.header;
      ctx.fillRect(x, y, size, size);
      ctx.fillStyle = active ? colors.primary : colors.muted;
      ctx.beginPath(); ctx.moveTo(x + size * 0.2, y + size * 0.3); ctx.lineTo(x + size * 0.8, y + size * 0.3); ctx.lineTo(x + size * 0.56, y + size * 0.58); ctx.lineTo(x + size * 0.56, y + size * 0.8); ctx.lineTo(x + size * 0.44, y + size * 0.8); ctx.lineTo(x + size * 0.44, y + size * 0.58); ctx.closePath(); ctx.fill();
    }
  }
  const REF_COLORS = ["#4285F4", "#EA4335", "#A142F4", "#34A853", "#FA7B17", "#F439A0", "#24C1E0", "#9C27B0"];
  function drawSelection(ctx, s, region, z, colors) {
    const d = doc(), sel = d.sel;
    const rangeRect = (range) => {
      const a = cellRect(range.r1, range.c1), b = cellRect(range.r2, range.c2);
      const x = Math.min(a.x, b.x), y = Math.min(a.y, b.y);
      return { x, y, w: Math.max(a.x + a.w, b.x + b.w) - x, h: Math.max(a.y + a.h, b.y + b.h) - y };
    };
    if (d.editing && d.editing.text.startsWith("=")) {
      S.referenceTokens(d.editing.text).forEach((ref, i) => {
        const target = ref.sheet ? d.wb.sheets.find((x) => x.name.toLowerCase() === ref.sheet.toLowerCase()) : d.wb.sheets[d.editing.sheetIndex];
        if (target !== s) return;
        const rr = rangeRect({ r1: ref.r1, c1: ref.c1, r2: Math.min(ref.r2, s.rowCount - 1), c2: Math.min(ref.c2, s.colCount - 1) });
        const color = REF_COLORS[i % REF_COLORS.length];
        ctx.fillStyle = color + "22"; ctx.fillRect(rr.x, rr.y, rr.w - 1, rr.h - 1);
        ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.setLineDash([]); ctx.strokeRect(rr.x, rr.y, rr.w - 1, rr.h - 1);
      });
    }
    const spill = d.calc.spillRange(s, sel.r, sel.c);
    if (spill) { const sr = rangeRect(spill); ctx.strokeStyle = colors.primary; ctx.lineWidth = 1; ctx.setLineDash([4, 3]); ctx.strokeRect(sr.x + 0.5, sr.y + 0.5, sr.w - 2, sr.h - 2); ctx.setLineDash([]); }
    if (d.clipboard && d.clipboard.sheet === s && d.clipboard.marquee) {
      const cr = rangeRect(d.clipboard.range);
      ctx.strokeStyle = colors.primary; ctx.lineWidth = 2; ctx.setLineDash([5, 3]); ctx.strokeRect(cr.x, cr.y, cr.w - 1, cr.h - 1); ctx.setLineDash([]);
    }
    if (d.editing && d.editing.sheetIndex !== d.wb.active) return;
    const rr = rangeRect(sel);
    const single = sel.r1 === sel.r2 && sel.c1 === sel.c2 || (S.mergeAt(s, sel.r1, sel.c1) && S.expandToMerges(s, { r1: sel.r1, c1: sel.c1, r2: sel.r1, c2: sel.c1 }).r2 >= sel.r2 && S.expandToMerges(s, { r1: sel.r1, c1: sel.c1, r2: sel.r1, c2: sel.c1 }).c2 >= sel.c2);
    if (!single) {
      const active = cellRect(sel.r, sel.c);
      const right = rr.x + rr.w - 1, bottom = rr.y + rr.h - 1, aRight = active.x + active.w - 1, aBottom = active.y + active.h - 1;
      ctx.fillStyle = colors.selection;
      ctx.fillRect(rr.x, rr.y, rr.w - 1, Math.max(0, active.y - rr.y));
      ctx.fillRect(rr.x, aBottom, rr.w - 1, Math.max(0, bottom - aBottom));
      ctx.fillRect(rr.x, active.y, Math.max(0, active.x - rr.x), active.h - 1);
      ctx.fillRect(aRight, active.y, Math.max(0, right - aRight), active.h - 1);
    }
    if (d.fillPreview) {
      const fr = rangeRect(d.fillPreview);
      ctx.strokeStyle = colors.primary; ctx.lineWidth = 1; ctx.setLineDash([3, 3]); ctx.strokeRect(fr.x + 0.5, fr.y + 0.5, fr.w - 2, fr.h - 2); ctx.setLineDash([]);
    }
    const active = cellRect(sel.r, sel.c);
    ctx.strokeStyle = colors.primary;
    ctx.lineWidth = 2;
    ctx.strokeRect(rr.x, rr.y, rr.w - 1, rr.h - 1);
    if (!single) { ctx.lineWidth = 1; ctx.strokeRect(active.x + 0.5, active.y + 0.5, active.w - 2, active.h - 2); }
    if (!d.editing) { ctx.fillStyle = colors.handle; ctx.fillRect(rr.x + rr.w - 4, rr.y + rr.h - 4, 6, 6); ctx.fillStyle = colors.bg; ctx.fillRect(rr.x + rr.w - 4, rr.y + rr.h - 5, 6, 1); ctx.fillRect(rr.x + rr.w - 5, rr.y + rr.h - 4, 1, 6); }
  }
  function drawHeaders(ctx, s, regions, z, colors, hw) {
    const d = doc(), { rows, cols } = axes(), sel = d.sel;
    const W = grid.width, H = grid.height;
    ctx.fillStyle = colors.header;
    ctx.fillRect(0, 0, W, HEADER_H);
    ctx.fillRect(0, 0, hw, H);
    ctx.font = `${12}px system-ui, -apple-system, "Segoe UI", sans-serif`;
    ctx.textBaseline = "middle";
    ctx.textAlign = "center";
    const fullCols = sel.r1 === 0 && sel.r2 >= s.rowCount - 1, fullRows = sel.c1 === 0 && sel.c2 >= s.colCount - 1;
    const colRanges = [[regions[3].c0, regions[3].c1, hw], [regions[0].c0, regions[0].c1, hw + cols.offset(s.freezeCols) * z]];
    for (const [c0, c1, minX] of colRanges) {
      if (c1 < c0) continue;
      ctx.save(); ctx.beginPath(); ctx.rect(minX, 0, W - minX, HEADER_H); ctx.clip();
      for (let c = c0; c <= c1; c++) {
        const w = cols.size(c) * z;
        if (!w) { ctx.fillStyle = colors.muted; ctx.fillRect(screenX(c) - 2, 6, 3, HEADER_H - 12); continue; }
        const x = screenX(c);
        const inSel = c >= sel.c1 && c <= sel.c2;
        if (inSel) { ctx.fillStyle = fullCols ? colors.headerStrong : colors.headerActive; ctx.fillRect(x, 0, w, HEADER_H); }
        ctx.fillStyle = colors.gridline; ctx.fillRect(Math.round(x + w) - 1, 0, 1, HEADER_H);
        ctx.fillStyle = inSel ? colors.fg : colors.muted;
        ctx.fillText(S.colName(c), x + w / 2, HEADER_H / 2 + 1);
      }
      ctx.restore();
    }
    const rowRanges = [[0, s.freezeRows - 1, HEADER_H], [regions[0].r0, regions[0].r1, HEADER_H + rows.offset(s.freezeRows) * z]];
    for (const [r0, r1, minY] of rowRanges) {
      if (r1 < r0) continue;
      ctx.save(); ctx.beginPath(); ctx.rect(0, minY, hw, H - minY); ctx.clip();
      for (let r = r0; r <= r1; r++) {
        const h = rows.size(r) * z;
        if (!h) { if (s.hiddenRows.has(r)) { ctx.fillStyle = colors.muted; ctx.fillRect(6, screenY(r) - 2, hw - 12, 3); } continue; }
        const y = screenY(r);
        const inSel = r >= sel.r1 && r <= sel.r2;
        if (inSel) { ctx.fillStyle = fullRows ? colors.headerStrong : colors.headerActive; ctx.fillRect(0, y, hw, h); }
        ctx.fillStyle = colors.gridline; ctx.fillRect(0, Math.round(y + h) - 1, hw, 1);
        ctx.fillStyle = inSel ? colors.fg : s.filterRows.has(r) ? colors.primary : colors.muted;
        ctx.fillText(String(r + 1), hw / 2, y + h / 2 + 1);
      }
      ctx.restore();
    }
    ctx.fillStyle = colors.border;
    ctx.fillRect(0, HEADER_H - 1, W, 1);
    ctx.fillRect(hw - 1, 0, 1, H);
    ctx.fillStyle = colors.header;
    ctx.fillRect(0, 0, hw - 1, HEADER_H - 1);
    ctx.fillStyle = colors.muted;
    ctx.beginPath(); ctx.moveTo(hw - 5, HEADER_H - 5); ctx.lineTo(hw - 5, HEADER_H - 14); ctx.lineTo(hw - 14, HEADER_H - 5); ctx.fill();
  }

  function resize() {
    if (!grid.canvas || !doc()) return;
    const host = grid.host;
    const scroller = grid.scroller;
    grid.dpr = window.devicePixelRatio || 1;
    grid.width = scroller.clientWidth;
    grid.height = scroller.clientHeight;
    grid.canvas.width = Math.round(grid.width * grid.dpr);
    grid.canvas.height = Math.round(grid.height * grid.dpr);
    grid.canvas.style.width = grid.width + "px";
    grid.canvas.style.height = grid.height + "px";
    updateSizer();
    readColors();
    drawNow();
    if (host) host.dataset.ready = "1";
  }
  function updateSizer() {
    const d = doc(), s = sheet(), { rows, cols } = axes();
    grid.sizer.style.width = Math.ceil(headerWidth() + cols.total() * d.zoom + 120) + "px";
    grid.sizer.style.height = Math.ceil(HEADER_H + rows.total() * d.zoom + 120) + "px";
    if (s.rowCount < S.MAX_ROWS && grid.scroller.scrollTop + grid.height > HEADER_H + rows.total() * d.zoom - 400) {
      s.rowCount = Math.min(S.MAX_ROWS, s.rowCount + 1000);
      d.layoutVersion++;
      return updateSizer();
    }
    if (s.colCount < Math.min(S.MAX_COLS, 702) && grid.scroller.scrollLeft + grid.width > headerWidth() + cols.total() * d.zoom - 300) {
      s.colCount = Math.min(S.MAX_COLS, s.colCount + 10);
      d.layoutVersion++;
      return updateSizer();
    }
    return null;
  }
  function scrollIntoView(r, c) {
    const d = doc(), s = sheet(), z = d.zoom, { rows, cols } = axes();
    const hw = headerWidth();
    const frozenW = cols.offset(s.freezeCols) * z, frozenH = rows.offset(s.freezeRows) * z;
    if (c >= s.freezeCols) {
      const left = cols.offset(c) * z, right = left + cols.size(c) * z;
      const viewLeft = grid.scroller.scrollLeft + frozenW, viewRight = grid.scroller.scrollLeft + grid.width - hw;
      if (left < viewLeft) grid.scroller.scrollLeft = Math.max(0, left - frozenW);
      else if (right > viewRight) grid.scroller.scrollLeft = right - (grid.width - hw) + 2;
    }
    if (r >= s.freezeRows) {
      const top = rows.offset(r) * z, bottom = top + rows.size(r) * z;
      const viewTop = grid.scroller.scrollTop + frozenH, viewBottom = grid.scroller.scrollTop + grid.height - HEADER_H;
      if (top < viewTop) grid.scroller.scrollTop = Math.max(0, top - frozenH);
      else if (bottom > viewBottom) grid.scroller.scrollTop = bottom - (grid.height - HEADER_H) + 2;
    }
    updateSizer();
  }

  function setSelection(r, c, extend = false, options = {}) {
    const d = doc(), s = sheet();
    r = Math.max(0, Math.min(s.rowCount - 1, r));
    c = Math.max(0, Math.min(s.colCount - 1, c));
    if (r >= s.rowCount - 5 && s.rowCount < S.MAX_ROWS) { s.rowCount = Math.min(S.MAX_ROWS, s.rowCount + 1000); d.layoutVersion++; }
    if (c >= s.colCount - 2 && s.colCount < S.MAX_COLS) { s.colCount = Math.min(S.MAX_COLS, s.colCount + 10); d.layoutVersion++; }
    if (extend) {
      let range = S.normRange({ r1: d.anchor.r, c1: d.anchor.c, r2: r, c2: c });
      range = S.expandToMerges(s, range);
      d.sel = { ...d.sel, ...range };
      d.sel.focus = { r, c };
    } else {
      const m = S.mergeAt(s, r, c);
      if (m) { r = m.r1; c = m.c1; }
      d.anchor = { r, c };
      d.sel = { r, c, r1: m ? m.r1 : r, c1: m ? m.c1 : c, r2: m ? m.r2 : r, c2: m ? m.c2 : c };
    }
    if (!options.noScroll) scrollIntoView(extend ? r : d.sel.r, extend ? c : d.sel.c);
    updateFormulaBar();
    updateStatus();
    renderToolbarState();
    draw();
  }
  function selectRange(range, active) {
    const d = doc();
    const n = S.normRange(range);
    d.sel = { r: active ? active.r : n.r1, c: active ? active.c : n.c1, ...n };
    d.anchor = { r: d.sel.r, c: d.sel.c };
    updateFormulaBar(); updateStatus(); renderToolbarState(); draw();
  }
  function selectedRange() { const d = doc(); return { r1: d.sel.r1, c1: d.sel.c1, r2: d.sel.r2, c2: d.sel.c2 }; }
  function clampToUsed(range) {
    const s = sheet(), used = S.extentOf(s);
    return { r1: range.r1, c1: range.c1, r2: Math.min(range.r2, Math.max(range.r1, used.r2)), c2: Math.min(range.c2, Math.max(range.c1, used.c2)) };
  }
  function jumpEdge(dr, dc) {
    const d = doc(), s = sheet();
    const from = d.sel.focus && (d.sel.r1 !== d.sel.r2 || d.sel.c1 !== d.sel.c2) ? d.sel.focus : { r: d.sel.r, c: d.sel.c };
    const filled = (r, c) => { const v = d.calc.cellValue(s, r, c); return v !== null && v !== ""; };
    let r = from.r, c = from.c;
    const step = () => { r += dr; c += dc; return r >= 0 && c >= 0 && r < s.rowCount && c < s.colCount; };
    const inside = (rr, cc) => rr >= 0 && cc >= 0 && rr < s.rowCount && cc < s.colCount;
    if (filled(r, c) && inside(r + dr, c + dc) && filled(r + dr, c + dc)) { while (inside(r + dr, c + dc) && filled(r + dr, c + dc)) { r += dr; c += dc; } }
    else {
      let found = false;
      while (step()) { if (filled(r, c)) { found = true; break; } if ((dr && Math.abs(r - from.r) > 100000) || (dc && Math.abs(c - from.c) > 20000)) break; }
      if (!found) { const used = S.extentOf(s); r = dr > 0 ? Math.max(from.r, used.r2) : dr < 0 ? 0 : from.r; c = dc > 0 ? Math.max(from.c, used.c2) : dc < 0 ? 0 : from.c; }
    }
    return { r: Math.max(0, Math.min(s.rowCount - 1, r)), c: Math.max(0, Math.min(s.colCount - 1, c)) };
  }
  function moveBy(dr, dc, extend) {
    const d = doc(), s = sheet(), { rows, cols } = axes();
    const from = extend ? d.sel.focus || { r: d.sel.r, c: d.sel.c } : { r: d.sel.r, c: d.sel.c };
    let r = from.r, c = from.c;
    const m = !extend && S.mergeAt(s, r, c);
    if (m) { if (dr > 0) r = m.r2; if (dc > 0) c = m.c2; }
    r += dr; c += dc;
    while (dr && r > 0 && r < s.rowCount - 1 && rows.size(r) === 0) r += dr;
    while (dc && c > 0 && c < s.colCount - 1 && cols.size(c) === 0) c += dc;
    setSelection(r, c, extend);
  }
  function selectedCells(range = selectedRange(), limit = 2000000) {
    const out = [];
    const s = sheet(), r = clampToUsed(range);
    let n = 0;
    for (let row = r.r1; row <= r.r2; row++) for (let col = r.c1; col <= r.c2; col++) { if (++n > limit) return out; out.push([row, col]); }
    return out;
  }
  function updateStatus() {
    const d = doc(), node = q("[data-sheets-stats]");
    if (!d || !node) return;
    const range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) { node.textContent = ""; return; }
    let count = 0, numbers = 0, sum = 0, min = Infinity, max = -Infinity;
    const s = sheet(), r = clampToUsed(range);
    let visited = 0;
    for (let row = r.r1; row <= r.r2 && visited < 400000; row++) {
      if (s.hiddenRows.has(row) || s.filterRows.has(row)) continue;
      for (let col = r.c1; col <= r.c2; col++, visited++) {
        const v = d.calc.cellValue(s, row, col);
        if (v === null || v === "") continue;
        count++;
        if (typeof v === "number") { numbers++; sum += v; if (v < min) min = v; if (v > max) max = v; }
      }
    }
    const own = S.styleOf(d.wb, s.cells.get(S.cellKey(d.sel.r, d.sel.c))).fmt;
    const fmt = (n) => S.formatValue(n, own && own !== "@" ? own : Math.abs(n - Math.round(n)) < 1e-9 ? "#,##0" : "#,##0.########").text;
    node.textContent = numbers ? `Sum ${fmt(sum)} · Average ${fmt(sum / numbers)} · Min ${fmt(min)} · Max ${fmt(max)} · Count ${count}` : count ? `Count ${count}` : "";
  }
  function updateFormulaBar() {
    const d = doc();
    if (!d) return;
    const s = sheet();
    const name = q("[data-sheets-namebox]");
    const range = selectedRange();
    if (name && document.activeElement !== name) name.value = range.r1 === range.r2 && range.c1 === range.c2 || (S.mergeAt(s, range.r1, range.c1) && S.rangeText(S.mergeAt(s, range.r1, range.c1)) === S.rangeText(range)) ? S.addr(d.sel.r, d.sel.c) : S.rangeText(range);
    const bar = q("[data-sheets-formula]");
    if (bar && !(d.editing && d.editing.source === "bar")) bar.value = d.editing ? d.editing.text : cellInputText(s, d.sel.r, d.sel.c);
    const note = s.notes.get(S.cellKey(d.sel.r, d.sel.c));
    const hint = q("[data-sheets-cell-note]");
    if (hint) { hint.textContent = note || ""; hint.classList.toggle("hidden", !note || !!d.editing); positionNoteHint(); }
  }
  function cellInputText(s, r, c) {
    const d = doc();
    const cell = s.cells.get(S.cellKey(r, c));
    if (!cell) return "";
    if (cell.f !== undefined) return "=" + cell.f;
    const v = cell.v;
    if (v === undefined || v === null) return "";
    if (typeof v === "boolean") return v ? "TRUE" : "FALSE";
    if (S.isErr(v)) return v.code;
    if (typeof v === "number") {
      const fmt = S.styleOf(d.wb, cell).fmt;
      if (S.isDateFormat(fmt)) {
        const hasTime = Math.abs(v % 1) > 1e-9, hasDate = v >= 1;
        return S.formatValue(v, hasDate && hasTime ? (dayFirst ? "dd/mm/yyyy h:mm:ss" : "m/d/yyyy h:mm:ss") : hasDate ? (dayFirst ? "dd/mm/yyyy" : "m/d/yyyy") : "h:mm:ss AM/PM").text;
      }
      if (fmt && /%/.test(fmt) && !/"%"/.test(fmt)) return S.valueToText(S.formatGeneral(v * 100) * 1) + "%";
      return S.valueToText(v);
    }
    const text = String(v);
    const parsed = S.parseInput(text, inputOptions);
    return parsed.value !== text || parsed.formula !== undefined ? "'" + text : text;
  }
  function positionNoteHint() {
    const hint = q("[data-sheets-cell-note]");
    if (!hint || hint.classList.contains("hidden")) return;
    const d = doc(), rect = cellRect(d.sel.r, d.sel.c);
    hint.style.left = Math.min(grid.width - 220, rect.x + rect.w + 4) + "px";
    hint.style.top = Math.max(HEADER_H, rect.y) + "px";
  }

  function setCellInput(tx, s, r, c, text) {
    const d = doc();
    const k = S.cellKey(r, c);
    const old = s.cells.get(k);
    const style = S.styleOf(d.wb, old);
    const parsed = S.parseInput(text, { ...inputOptions, textFormat: style.fmt === "@" });
    let styleId = old?.s || 0;
    if (parsed.format && !style.fmt) styleId = S.internStyle(d.wb, { ...style, fmt: parsed.format });
    let next;
    if (parsed.formula !== undefined) {
      let formula = parsed.formula.trim();
      const opens = (formula.match(/\(/g) || []).length - (formula.match(/\)/g) || []).length;
      if (opens > 0 && opens < 10) formula += ")".repeat(opens);
      try { S.parseFormula(formula); }
      catch (error) { throw new Error(`There is a problem with this formula: ${error.message}`); }
      formula = formula.replace(/\b([a-z][a-z0-9.]*)\s*\(/gi, (m, name) => (S.FUNCTIONS[name.toUpperCase()] ? m.replace(name, name.toUpperCase()) : m)).replace(/(^|[^A-Za-z0-9_$"'!])(\$?[a-z]{1,3}\$?\d{1,7})(?![A-Za-z0-9_(!])/g, (m, pre, ref) => pre + ref.toUpperCase());
      next = S.makeCell(undefined, formula, styleId);
    } else next = S.makeCell(parsed.value, undefined, styleId);
    tx.setCell(s, k, next);
  }
  function validateInput(s, r, c, text) {
    const d = doc();
    const rule = S.validationAt(s, r, c);
    if (!rule || rule.strict === false || !text) return true;
    const parsed = S.parseInput(text, inputOptions);
    if (parsed.formula !== undefined) return true;
    if (S.validateValue(d.calc, s, rule, parsed.value)) return true;
    toast(rule.type === "list" ? `Pick one of: ${S.validationOptions(d.calc, s, rule).slice(0, 8).join(", ")}` : rule.help || "That value is not allowed in this cell.");
    return false;
  }

  function startEdit(initial, mode = "edit", source = "cell") {
    const d = doc(), s = sheet();
    if (d.editing) return;
    const cell = s.cells.get(S.cellKey(d.sel.r, d.sel.c));
    const rule = S.validationAt(s, d.sel.r, d.sel.c);
    if (rule && rule.type === "checkbox" && initial === undefined) { toggleCheckbox(); return; }
    const text = initial !== undefined ? initial : cellInputText(s, d.sel.r, d.sel.c);
    d.editing = { sheetIndex: d.wb.active, r: d.sel.r, c: d.sel.c, text, mode, source, original: cellInputText(s, d.sel.r, d.sel.c), point: null };
    const editor = grid.editor;
    const style = S.styleOf(d.wb, cell);
    editor.style.font = fontFor(style, d.zoom);
    editor.style.textAlign = style.h === "center" ? "center" : style.h === "right" ? "right" : "left";
    editor.style.color = style.color && !(grid.colors.dark && S.luminance(style.color) < 0.1) ? style.color : "";
    editor.style.background = style.fill || "";
    if (style.fill && !style.color) editor.style.color = S.luminance(style.fill) < 0.5 ? "#fff" : "#000";
    editor.value = text;
    editor.classList.add("editing");
    positionEditor();
    if (source === "bar") q("[data-sheets-formula]").focus();
    else { editor.focus({ preventScroll: true }); const end = editor.value.length; editor.setSelectionRange(end, end); }
    updateFormulaBar();
    draw();
    renderAutocomplete();
  }
  function positionEditor() {
    const d = doc();
    const editor = grid.editor;
    if (!editor || !d) return;
    if (!d.editing) {
      const rect = cellRect(d.sel.r, d.sel.c);
      editor.style.left = Math.max(headerWidth(), Math.min(grid.width - 4, rect.x)) + "px";
      editor.style.top = Math.max(HEADER_H, Math.min(grid.height - 4, rect.y)) + "px";
      editor.style.width = "2px"; editor.style.height = "2px";
      return;
    }
    if (d.editing.sheetIndex !== d.wb.active) { editor.style.left = "-9999px"; return; }
    const rect = cellRect(d.editing.r, d.editing.c);
    const minW = rect.w + 1;
    editor.style.left = rect.x - 1 + "px";
    editor.style.top = rect.y - 1 + "px";
    editor.style.minWidth = minW + "px";
    editor.style.width = "auto";
    editor.style.height = "auto";
    const ctx = grid.ctx;
    ctx.font = editor.style.font;
    const lines = editor.value.split("\n");
    const widest = Math.max(...lines.map((line) => measure(ctx, line))) + 16;
    editor.style.width = Math.min(Math.max(minW, widest), Math.max(minW, grid.width - rect.x - 8)) + "px";
    editor.style.height = Math.max(rect.h + 1, editor.scrollHeight) + "px";
  }
  function commitEdit(move = null) {
    const d = doc();
    if (!d || !d.editing) return true;
    const e = d.editing;
    const s = d.wb.sheets[e.sheetIndex];
    const text = e.source === "bar" ? q("[data-sheets-formula]").value : grid.editor.value;
    if (text !== e.original) {
      if (!validateInput(s, e.r, e.c, text)) return false;
      const ok = act("Edit cell", (tx) => {
        const range = d.fillSelection && e.sheetIndex === d.wb.active ? selectedRange() : null;
        if (range && (range.r1 !== range.r2 || range.c1 !== range.c2)) {
          for (let r = range.r1; r <= Math.min(range.r2, range.r1 + 20000); r++) for (let c = range.c1; c <= Math.min(range.c2, range.c1 + 500); c++) {
            const shifted = text.startsWith("=") ? "=" + S.shiftFormula(text.slice(1), r - e.r, c - e.c) : text;
            setCellInput(tx, s, r, c, shifted);
          }
        } else setCellInput(tx, s, e.r, e.c, text);
        return true;
      });
      if (ok === null) return false;
    }
    d.fillSelection = false;
    stopEdit();
    if (d.wb.active !== e.sheetIndex) { d.wb.active = e.sheetIndex; d.layoutVersion++; renderTabs(); }
    if (move) moveBy(move[0], move[1], false);
    else draw();
    return true;
  }
  function cancelEdit() {
    const d = doc();
    if (!d || !d.editing) return;
    const index = d.editing.sheetIndex;
    stopEdit();
    if (d.wb.active !== index) { d.wb.active = index; d.layoutVersion++; renderTabs(); resize(); }
    draw();
  }
  function stopEdit() {
    const d = doc();
    d.editing = null;
    grid.editor.classList.remove("editing");
    grid.editor.value = "";
    grid.editor.style.background = "";
    closeAutocomplete();
    updateFormulaBar();
    positionEditor();
    focusGrid();
  }
  function focusGrid() { if (grid.editor && document.activeElement !== grid.editor) grid.editor.focus({ preventScroll: true }); }
  function editorText() { const d = doc(); return d.editing?.source === "bar" ? q("[data-sheets-formula]") : grid.editor; }
  function syncEditing(from) {
    const d = doc();
    if (!d.editing) return;
    d.editing.text = from.value;
    d.editing.point = null;
    if (from === grid.editor) q("[data-sheets-formula]").value = from.value;
    else grid.editor.value = from.value;
    positionEditor();
    renderAutocomplete();
    draw();
  }
  function inPointMode() {
    const d = doc();
    if (!d.editing || !d.editing.text.startsWith("=")) return false;
    const input = editorText();
    const caret = input.selectionStart;
    if (d.editing.point && d.editing.point.end === caret) return true;
    const before = input.value.slice(0, caret).replace(/\s+$/, "");
    return /[=(,;+\-*/^&<>:]$/.test(before);
  }
  function insertReference(range, sheetIndex) {
    const d = doc();
    const input = editorText();
    const e = d.editing;
    const prefix = sheetIndex !== e.sheetIndex ? S.quoteSheet(d.wb.sheets[sheetIndex].name) + "!" : "";
    const text = prefix + S.rangeText(range);
    const start = e.point ? e.point.start : input.selectionStart;
    const end = e.point ? e.point.end : input.selectionEnd;
    input.value = input.value.slice(0, start) + text + input.value.slice(end);
    const caret = start + text.length;
    input.setSelectionRange(caret, caret);
    e.text = input.value;
    e.point = { start, end: caret };
    if (input === grid.editor) q("[data-sheets-formula]").value = input.value; else grid.editor.value = input.value;
    positionEditor();
    draw();
  }
  function toggleCheckbox() {
    const d = doc(), s = sheet();
    const cells = selectedCells();
    act("Toggle checkbox", (tx) => {
      const first = d.calc.cellValue(s, d.sel.r, d.sel.c) === true;
      for (const [r, c] of cells) {
        const rule = S.validationAt(s, r, c);
        if (!rule || rule.type !== "checkbox") continue;
        const old = s.cells.get(S.cellKey(r, c));
        tx.setCell(s, S.cellKey(r, c), S.makeCell(!first, undefined, old?.s));
      }
    });
  }
  function styleRange(tx, s, range, patcher) {
    const d = doc();
    const r = { ...range };
    const used = S.extentOf(s);
    if (r.r2 - r.r1 > 5000) r.r2 = Math.max(r.r1, Math.min(r.r2, Math.max(used.r2, s.rowCount - 1)));
    if (r.c2 - r.c1 > 500) r.c2 = Math.max(r.c1, Math.min(r.c2, Math.max(used.c2, s.colCount - 1)));
    if ((r.r2 - r.r1 + 1) * (r.c2 - r.c1 + 1) > 400000) throw new Error("Select fewer cells to format at once.");
    const cache = new Map();
    for (let row = r.r1; row <= r.r2; row++) for (let col = r.c1; col <= r.c2; col++) {
      const k = S.cellKey(row, col), old = s.cells.get(k);
      const from = old?.s || 0;
      const key = from + ":" + (patcher.length > 1 ? `${row - r.r1},${col - r.c1},${row === r.r2},${col === r.c2}` : "");
      let id = cache.get(key);
      if (id === undefined) { id = S.internStyle(d.wb, patcher({ ...S.styleOf(d.wb, old) }, { row, col, top: row === r.r1, bottom: row === r.r2, left: col === r.c1, right: col === r.c2 })); if (patcher.length < 2) cache.set(key, id); }
      if (id !== from) tx.setCell(s, k, S.makeCell(old?.v, old?.f, id));
    }
  }
  function applyStyle(patch, label = "Format cells") {
    act(label, (tx) => styleRange(tx, sheet(), selectedRange(), (style) => Object.assign(style, patch)));
  }
  function toggleStyle(key, label) {
    const d = doc(), s = sheet();
    const current = S.styleOf(d.wb, s.cells.get(S.cellKey(d.sel.r, d.sel.c)))[key];
    applyStyle({ [key]: !current }, label);
  }
  function applyBorders(kind) {
    const b = grid.borderStyle ? { w: grid.borderStyle, c: grid.borderColor || "#000000" } : { w: "thin", c: grid.borderColor || "#000000" };
    act("Borders", (tx) => styleRange(tx, sheet(), selectedRange(), (style, at) => {
      const set = (key, on) => { if (on) style[key] = b; };
      if (kind === "none") { delete style.bt; delete style.bb; delete style.bl; delete style.br; return style; }
      if (kind === "all") { style.bt = style.bb = style.bl = style.br = b; return style; }
      if (kind === "outer") { set("bt", at.top); set("bb", at.bottom); set("bl", at.left); set("br", at.right); return style; }
      if (kind === "inner") { set("bb", !at.bottom); set("br", !at.right); return style; }
      if (kind === "horizontal") { set("bb", !at.bottom); return style; }
      if (kind === "vertical") { set("br", !at.right); return style; }
      if (kind === "top") set("bt", at.top);
      if (kind === "bottom") set("bb", at.bottom);
      if (kind === "left") set("bl", at.left);
      if (kind === "right") set("br", at.right);
      return style;
    }));
  }
  function stepDecimals(delta) {
    const d = doc(), s = sheet();
    const cell = s.cells.get(S.cellKey(d.sel.r, d.sel.c));
    let fmt = S.styleOf(d.wb, cell).fmt || "";
    const value = d.calc.cellValue(s, d.sel.r, d.sel.c);
    if (!fmt || fmt === "General") {
      const text = typeof value === "number" ? S.formatGeneral(value) : "0";
      const places = (text.split(".")[1] || "").replace(/E.*/, "").length;
      fmt = places ? "0." + "0".repeat(places) : "0";
    }
    const next = fmt.split(";").map((section) => {
      if (/[ymdhs]/i.test(section.replace(/"[^"]*"/g, ""))) return section;
      if (delta > 0) return /\.0*/.test(section) ? section.replace(/(\.0*)/, "$10") : section.replace(/(0)(?![\s\S]*0)/, "$1.0");
      return section.replace(/\.(0+)/, (m, zeros) => (zeros.length > 1 ? "." + zeros.slice(1) : ""));
    }).join(";");
    applyStyle({ fmt: next }, delta > 0 ? "Increase decimals" : "Decrease decimals");
  }
  function clearRange(kind = "values") {
    const s = sheet(), range = selectedRange();
    act(kind === "all" ? "Clear" : kind === "formats" ? "Clear formatting" : "Delete values", (tx) => {
      for (const k of [...s.cells.keys()]) {
        const r = S.keyRow(k), c = S.keyCol(k);
        if (!S.inRange(range, r, c)) continue;
        const old = s.cells.get(k);
        if (kind === "values") tx.setCell(s, k, S.makeCell(undefined, undefined, old.s));
        else if (kind === "formats") tx.setCell(s, k, S.makeCell(old.v, old.f, 0));
        else tx.setCell(s, k, null);
      }
      if (kind !== "values") {
        tx.set(s, "conditional", s.conditional.filter((rule) => { const rr = S.parseRange(rule.range); return !rr || !(rr.r1 >= range.r1 && rr.r2 <= range.r2 && rr.c1 >= range.c1 && rr.c2 <= range.c2); }));
      }
      if (kind === "all") {
        const notes = new Map(s.notes); for (const k of notes.keys()) if (S.inRange(range, S.keyRow(k), S.keyCol(k))) notes.delete(k);
        tx.set(s, "notes", notes);
      }
    });
  }

  function clipboardSnapshot(range) {
    const s = sheet(), d = doc();
    const r = clampToUsed(range);
    const cells = [];
    for (let row = r.r1; row <= r.r2; row++) { const line = []; for (let col = r.c1; col <= r.c2; col++) line.push(s.cells.get(S.cellKey(row, col)) || null); cells.push(line); }
    const text = cells.map((line, i) => line.map((_, j) => S.displayText(d.wb, s, d.calc, r.r1 + i, r.c1 + j).replace(/\t/g, " ")).map((t) => (/["\n]/.test(t) ? `"${t.replace(/"/g, '""')}"` : t)).join("\t")).join("\n");
    const merges = s.merges.filter((m) => m.r1 >= r.r1 && m.r2 <= r.r2 && m.c1 >= r.c1 && m.c2 <= r.c2).map((m) => ({ r1: m.r1 - r.r1, c1: m.c1 - r.c1, r2: m.r2 - r.r1, c2: m.c2 - r.c1 }));
    const values = cells.map((line, i) => line.map((_, j) => d.calc.cellValue(s, r.r1 + i, r.c1 + j)));
    return { sheet: s, range: r, cells, values, text, merges, html: S.workbookHtml(d.wb, d.calc, "", { sheet: s, range: r }) };
  }
  function copySelection(data, cut) {
    const d = doc();
    const snap = clipboardSnapshot(selectedRange());
    d.clipboard = { ...snap, cut, marquee: true };
    if (data) { data.setData("text/plain", snap.text); data.setData("text/html", snap.html); }
    else navigator.clipboard?.writeText?.(snap.text).catch(() => {});
    draw();
    toast(cut ? "Cut. Paste to move the cells." : "Copied");
  }
  const normalizeClip = (text) => String(text || "").replace(/\r\n?/g, "\n").replace(/\n$/, "");
  function pasteInternal(mode = "all") {
    const d = doc(), s = sheet(), clip = d.clipboard;
    if (!clip) return;
    const rows = clip.cells.length, cols = clip.cells[0]?.length || 0;
    const sel = selectedRange();
    const transpose = mode === "transpose";
    const h = transpose ? cols : rows, w = transpose ? rows : cols;
    const tileR = (sel.r2 - sel.r1 + 1) % h === 0 && sel.r2 > sel.r1 ? (sel.r2 - sel.r1 + 1) / h : 1;
    const tileC = (sel.c2 - sel.c1 + 1) % w === 0 && sel.c2 > sel.c1 ? (sel.c2 - sel.c1 + 1) / w : 1;
    act(clip.cut ? "Move cells" : "Paste", (tx) => {
      const targets = new Set();
      for (let tr = 0; tr < tileR; tr++) for (let tc = 0; tc < tileC; tc++) for (let i = 0; i < h; i++) for (let j = 0; j < w; j++) {
        const si = transpose ? j : i, sj = transpose ? i : j;
        const source = clip.cells[si][sj];
        const r = sel.r1 + tr * h + i, c = sel.c1 + tc * w + j;
        if (r >= S.MAX_ROWS || c >= S.MAX_COLS) continue;
        const k = S.cellKey(r, c);
        targets.add(k);
        const old = s.cells.get(k);
        const styleId = source?.s || 0;
        if (mode === "values") tx.setCell(s, k, S.makeCell(clip.values[si][sj] ?? undefined, undefined, old?.s));
        else if (mode === "formats") tx.setCell(s, k, S.makeCell(old?.v, old?.f, styleId));
        else if (!source) tx.setCell(s, k, null);
        else if (source.f !== undefined) tx.setCell(s, k, S.makeCell(undefined, clip.cut ? source.f : S.shiftFormula(source.f, r - (clip.range.r1 + si), c - (clip.range.c1 + sj)), styleId));
        else tx.setCell(s, k, S.makeCell(source.v, undefined, styleId));
        if (r >= s.rowCount) tx.set(s, "rowCount", Math.min(S.MAX_ROWS, r + 100));
        if (c >= s.colCount) tx.set(s, "colCount", Math.min(S.MAX_COLS, c + 5));
      }
      if (mode === "all" && !transpose && clip.merges.length) {
        const added = [];
        for (let tr = 0; tr < tileR; tr++) for (let tc = 0; tc < tileC; tc++) for (const m of clip.merges) added.push({ r1: sel.r1 + tr * h + m.r1, c1: sel.c1 + tc * w + m.c1, r2: sel.r1 + tr * h + m.r2, c2: sel.c1 + tc * w + m.c2 });
        tx.set(s, "merges", [...s.merges.filter((m) => !added.some((a) => S.rangesOverlap(a, m))), ...added]);
      }
      if (clip.cut) {
        for (let i = 0; i < rows; i++) for (let j = 0; j < cols; j++) {
          const k = S.cellKey(clip.range.r1 + i, clip.range.c1 + j);
          if (clip.sheet === s && targets.has(k)) continue;
          tx.setCell(clip.sheet, k, null);
        }
      }
    });
    selectRange({ r1: sel.r1, c1: sel.c1, r2: sel.r1 + h * tileR - 1, c2: sel.c1 + w * tileC - 1 });
    if (clip.cut) d.clipboard = null; else clip.marquee = false;
    draw();
  }
  function pasteRows(rows, styles) {
    const d = doc(), s = sheet(), sel = selectedRange();
    if (!rows.length) return;
    const single = rows.length === 1 && rows[0].length === 1;
    act("Paste", (tx) => {
      if (single && (sel.r1 !== sel.r2 || sel.c1 !== sel.c2)) {
        const r = clampToUsed(sel);
        for (let row = sel.r1; row <= Math.max(r.r2, sel.r1); row++) for (let col = sel.c1; col <= Math.max(r.c2, sel.c1); col++) setCellInput(tx, s, row, col, rows[0][0]);
        return;
      }
      rows.forEach((line, i) => line.forEach((text, j) => {
        const r = sel.r1 + i, c = sel.c1 + j;
        if (r >= S.MAX_ROWS || c >= S.MAX_COLS) return;
        setCellInput(tx, s, r, c, text);
        const style = styles?.[i]?.[j];
        if (style && Object.keys(style).length) {
          const k = S.cellKey(r, c), cell = s.cells.get(k);
          tx.setCell(s, k, S.makeCell(cell?.v, cell?.f, S.internStyle(d.wb, { ...S.styleOf(d.wb, cell), ...style })));
        }
        if (r >= s.rowCount) tx.set(s, "rowCount", Math.min(S.MAX_ROWS, r + 100));
        if (c >= s.colCount) tx.set(s, "colCount", Math.min(S.MAX_COLS, c + 5));
      }));
    });
    selectRange({ r1: sel.r1, c1: sel.c1, r2: sel.r1 + rows.length - 1, c2: sel.c1 + Math.max(...rows.map((r) => r.length)) - 1 });
  }
  function pasteFrom(html, text) {
    const d = doc();
    if (d.clipboard && normalizeClip(text) === normalizeClip(d.clipboard.text)) return pasteInternal("all");
    if (html && /<table/i.test(html)) {
      try {
        const wb = htmlTables(html, "Paste");
        const source = wb.sheets[0];
        const used = S.extentOf(source);
        const rows = [], styles = [];
        for (let r = 0; r <= used.r2; r++) {
          const line = [], styleLine = [];
          for (let c = 0; c <= used.c2; c++) { const cell = source.cells.get(S.cellKey(r, c)); line.push(cell ? S.valueToText(cell.v) : ""); styleLine.push(S.styleOf(wb, cell)); }
          rows.push(line); styles.push(styleLine);
        }
        if (rows.length) return pasteRows(rows.map((line) => line.map((t, i) => t)), styles.map((line) => line.map((style) => { const out = { ...style }; delete out.fmt; return out; })));
      } catch { return pasteRows(S.parseCsv(normalizeClip(text), "\t")); }
    }
    if (text) pasteRows(S.parseCsv(normalizeClip(text), "\t"));
    return null;
  }
  async function pasteFromMenu(mode) {
    const d = doc();
    if (d.clipboard && mode !== "text") return pasteInternal(mode);
    try {
      const text = await navigator.clipboard.readText();
      pasteRows(S.parseCsv(normalizeClip(text), "\t"));
    } catch { toast("Use Ctrl+V (⌘V on a Mac) to paste from another app."); }
    return null;
  }

  function fillTo(source, target) {
    const d = doc(), s = sheet();
    const down = target.r2 > source.r2, up = target.r1 < source.r1, right = target.c2 > source.c2, left = target.c1 < source.c1;
    if (!down && !up && !right && !left) return;
    act("Fill", (tx) => {
      if (down || up) {
        const count = down ? target.r2 - source.r2 : source.r1 - target.r1;
        for (let c = source.c1; c <= source.c2; c++) {
          const sources = [];
          for (let r = source.r1; r <= source.r2; r++) sources.push(s.cells.get(S.cellKey(r, c)) || null);
          S.fillValues(sources, count, d.wb, up).forEach((item, t) => {
            const r = down ? source.r2 + 1 + t : source.r1 - 1 - t;
            const cell = item.cell && item.formula ? S.makeCell(undefined, S.shiftFormula(item.cell.f, r - (source.r1 + ((r - source.r1) % sources.length + sources.length) % sources.length), 0), item.cell.s) : item.cell;
            tx.setCell(s, S.cellKey(r, c), cell ? S.makeCell(cell.v, cell.f, cell.s) : null);
          });
        }
      } else {
        const count = right ? target.c2 - source.c2 : source.c1 - target.c1;
        for (let r = source.r1; r <= source.r2; r++) {
          const sources = [];
          for (let c = source.c1; c <= source.c2; c++) sources.push(s.cells.get(S.cellKey(r, c)) || null);
          S.fillValues(sources, count, d.wb, left).forEach((item, t) => {
            const c = right ? source.c2 + 1 + t : source.c1 - 1 - t;
            const cell = item.cell && item.formula ? S.makeCell(undefined, S.shiftFormula(item.cell.f, 0, c - (source.c1 + ((c - source.c1) % sources.length + sources.length) % sources.length)), item.cell.s) : item.cell;
            tx.setCell(s, S.cellKey(r, c), cell ? S.makeCell(cell.v, cell.f, cell.s) : null);
          });
        }
      }
    });
    selectRange({ r1: Math.min(source.r1, target.r1), c1: Math.min(source.c1, target.c1), r2: Math.max(source.r2, target.r2), c2: Math.max(source.c2, target.c2) }, { r: doc().sel.r, c: doc().sel.c });
  }
  function fillDirection(direction) {
    const sel = selectedRange();
    if (direction === "down") { if (sel.r1 === sel.r2) { if (sel.r1 === 0) return; fillTo({ ...sel, r1: sel.r1 - 1, r2: sel.r1 - 1 }, { ...sel, r1: sel.r1 - 1 }); return; } fillCopy({ ...sel, r2: sel.r1 }, sel); }
    else { if (sel.c1 === sel.c2) { if (sel.c1 === 0) return; fillTo({ ...sel, c1: sel.c1 - 1, c2: sel.c1 - 1 }, { ...sel, c1: sel.c1 - 1 }); return; } fillCopy({ ...sel, c2: sel.c1 }, sel); }
  }
  function fillCopy(source, target) {
    const s = sheet();
    act("Fill", (tx) => {
      for (let r = target.r1; r <= target.r2; r++) for (let c = target.c1; c <= target.c2; c++) {
        if (S.inRange(source, r, c)) continue;
        const sr = source.r1 === source.r2 ? source.r1 : r, sc = source.c1 === source.c2 ? source.c1 : c;
        const cell = s.cells.get(S.cellKey(sr, sc));
        tx.setCell(s, S.cellKey(r, c), cell ? S.makeCell(cell.v, cell.f !== undefined ? S.shiftFormula(cell.f, r - sr, c - sc) : undefined, cell.s) : null);
      }
    });
  }

  function structural(kind, before = true) {
    const d = doc(), s = sheet(), sel = selectedRange();
    const fullRows = sel.c1 === 0 && sel.c2 >= s.colCount - 1, fullCols = sel.r1 === 0 && sel.r2 >= s.rowCount - 1;
    const rowsCount = sel.r2 - sel.r1 + 1, colsCount = sel.c2 - sel.c1 + 1;
    if (kind === "insertRows") act(`Insert ${rowsCount > 1 ? rowsCount + " rows" : "row"}`, (tx) => S.insertRows(tx, d.wb, s, before ? sel.r1 : sel.r2 + 1, fullRows || rowsCount < 1000 ? rowsCount : 1));
    else if (kind === "insertCols") act(`Insert ${colsCount > 1 ? colsCount + " columns" : "column"}`, (tx) => S.insertCols(tx, d.wb, s, before ? sel.c1 : sel.c2 + 1, Math.min(colsCount, 500)));
    else if (kind === "deleteRows") { if (fullCols) return; act(`Delete ${rowsCount > 1 ? rowsCount + " rows" : "row"}`, (tx) => S.deleteRows(tx, d.wb, s, sel.r1, rowsCount)); }
    else if (kind === "deleteCols") { if (fullRows) return; act(`Delete ${colsCount > 1 ? colsCount + " columns" : "column"}`, (tx) => S.deleteCols(tx, d.wb, s, sel.c1, colsCount)); }
    setSelection(d.sel.r, d.sel.c);
  }
  function setHidden(axis, hide) {
    const s = sheet(), sel = selectedRange();
    act(hide ? `Hide ${axis === "row" ? "rows" : "columns"}` : `Unhide ${axis === "row" ? "rows" : "columns"}`, (tx) => {
      const prop = axis === "row" ? "hiddenRows" : "hiddenCols";
      const next = new Set(s[prop]);
      const [a, b] = axis === "row" ? [sel.r1, sel.r2] : [sel.c1, sel.c2];
      if (hide) for (let i = a; i <= Math.min(b, a + 100000); i++) next.add(i);
      else for (const i of [...next]) if (i >= a - 1 && i <= b + 1) next.delete(i);
      tx.set(s, prop, next);
    });
  }
  function resizeLines(axis, size) {
    const s = sheet(), sel = selectedRange();
    act(axis === "row" ? "Resize rows" : "Resize columns", (tx) => {
      const prop = axis === "row" ? "rowHeights" : "colWidths";
      const next = new Map(s[prop]);
      const [a, b] = axis === "row" ? [sel.r1, sel.r2] : [sel.c1, sel.c2];
      for (let i = a; i <= Math.min(b, a + 20000); i++) { if (size === null) next.delete(i); else next.set(i, size); }
      tx.set(s, prop, next);
    });
  }
  function autoFit(axis, index) {
    const d = doc(), s = sheet(), ctx = grid.ctx;
    if (axis === "col") {
      let best = 24;
      const used = S.extentOf(s);
      for (let r = 0; r <= Math.min(used.r2, 5000); r++) {
        const cell = s.cells.get(S.cellKey(r, index));
        if (!cell && d.calc.spillOwner(s, r, index) === null) continue;
        if (S.mergeAt(s, r, index)) continue;
        const style = S.styleOf(d.wb, cell);
        ctx.font = fontFor(style, 1);
        const text = shownValue(s, r, index, cell, style).text;
        for (const line of text.split("\n")) best = Math.max(best, measure(ctx, line) + 10);
      }
      return Math.min(800, Math.ceil(best));
    }
    let best = s.defaultRowHeight;
    const used = S.extentOf(s);
    for (let c = 0; c <= Math.min(used.c2, 300); c++) {
      const cell = s.cells.get(S.cellKey(index, c));
      if (!cell) continue;
      const style = S.styleOf(d.wb, cell);
      ctx.font = fontFor(style, 1);
      const text = shownValue(s, index, c, cell, style).text;
      const lines = style.wrap ? wrapLines(ctx, text, (s.colWidths.get(c) ?? s.defaultColWidth) - 6).length : text.split("\n").length;
      best = Math.max(best, Math.ceil(lines * (style.size || d.wb.defaultSize) * (4 / 3) * 1.2 + 6));
    }
    return Math.min(600, best);
  }
  function fitSelection(axis) {
    const s = sheet(), sel = selectedRange();
    act(axis === "row" ? "Fit rows to data" : "Fit columns to data", (tx) => {
      const prop = axis === "row" ? "rowHeights" : "colWidths";
      const next = new Map(s[prop]);
      const [a, b] = axis === "row" ? [sel.r1, sel.r2] : [sel.c1, sel.c2];
      const end = Math.min(b, a + (axis === "row" ? 2000 : 300), axis === "row" ? S.extentOf(s).r2 : S.extentOf(s).c2);
      for (let i = a; i <= end; i++) next.set(i, autoFit(axis, i));
      tx.set(s, prop, next);
    });
  }
  function mergeCells(kind) {
    const d = doc(), s = sheet(), sel = selectedRange();
    if (kind !== "unmerge" && sel.r1 === sel.r2 && sel.c1 === sel.c2) return;
    act(kind === "unmerge" ? "Unmerge" : "Merge cells", (tx) => {
      const keep = s.merges.filter((m) => !S.rangesOverlap(m, sel));
      if (kind === "unmerge") { tx.set(s, "merges", keep); return; }
      const added = [];
      if (kind === "all") added.push(sel);
      else if (kind === "horizontal") for (let r = sel.r1; r <= sel.r2; r++) added.push({ r1: r, c1: sel.c1, r2: r, c2: sel.c2 });
      else for (let c = sel.c1; c <= sel.c2; c++) added.push({ r1: sel.r1, c1: c, r2: sel.r2, c2: c });
      for (const m of added) for (let r = m.r1; r <= m.r2; r++) for (let c = m.c1; c <= m.c2; c++) {
        if (r === m.r1 && c === m.c1) continue;
        const k = S.cellKey(r, c), old = s.cells.get(k);
        if (old && (old.v !== undefined || old.f !== undefined)) tx.setCell(s, k, S.makeCell(undefined, undefined, old.s));
      }
      tx.set(s, "merges", [...keep, ...added.filter((m) => m.r1 !== m.r2 || m.c1 !== m.c2)]);
    });
    setSelection(sel.r1, sel.c1);
    if (kind !== "unmerge") selectRange(sel, { r: sel.r1, c: sel.c1 });
  }
  function sortSelection(desc, wholeSheet = false) {
    const d = doc(), s = sheet();
    let range = selectedRange();
    if (wholeSheet || (range.r1 === range.r2 && range.c1 === range.c2)) {
      const used = S.usedRange(s);
      if (!used) return;
      range = { r1: 0, c1: 0, r2: used.r2, c2: used.c2 };
      if (s.freezeRows) range.r1 = s.freezeRows;
      else {
        const header = [];
        for (let c = range.c1; c <= range.c2; c++) header.push(d.calc.cellValue(s, 0, c));
        const next = [];
        for (let c = range.c1; c <= range.c2; c++) next.push(d.calc.cellValue(s, 1, c));
        if (header.every((v) => v === null || typeof v === "string") && next.some((v) => typeof v === "number")) range.r1 = 1;
      }
    }
    range = clampToUsed(range);
    act(desc ? "Sort Z to A" : "Sort A to Z", (tx) => S.sortRows(tx, d.wb, d.calc, s, range, [{ col: d.sel.c >= range.c1 && d.sel.c <= range.c2 ? d.sel.c : range.c1, desc }]));
  }
  function toggleFilter() {
    const d = doc(), s = sheet();
    if (s.filter) { act("Remove filter", (tx) => tx.set(s, "filter", null)); return; }
    let range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) range = S.dataRegion(s, d.calc, d.sel.r, d.sel.c);
    range = clampToUsed(range);
    if (range.r1 === range.r2) { toast("Select a range with a header row to filter."); return; }
    act("Create a filter", (tx) => tx.set(s, "filter", { range, criteria: new Map() }));
  }
  function freeze(rows, cols) {
    const s = sheet();
    act("Freeze", (tx) => { if (rows !== null) tx.set(s, "freezeRows", rows); if (cols !== null) tx.set(s, "freezeCols", cols); });
    grid.scroller.scrollTop = 0; grid.scroller.scrollLeft = 0;
  }
  function autosum(fn) {
    const d = doc(), s = sheet(), sel = selectedRange();
    const numeric = (r, c) => typeof d.calc.cellValue(s, r, c) === "number";
    if (sel.r1 === sel.r2 && sel.c1 === sel.c2) {
      let r = sel.r1 - 1;
      while (r >= 0 && numeric(r, sel.c1)) r--;
      let guess = r < sel.r1 - 1 ? S.rangeText({ r1: r + 1, c1: sel.c1, r2: sel.r1 - 1, c2: sel.c1 }) : "";
      if (!guess) { let c = sel.c1 - 1; while (c >= 0 && numeric(sel.r1, c)) c--; if (c < sel.c1 - 1) guess = S.rangeText({ r1: sel.r1, c1: c + 1, r2: sel.r1, c2: sel.c1 - 1 }); }
      startEdit(`=${fn}(${guess})`, "edit");
      return;
    }
    act(`Insert ${fn}`, (tx) => {
      const below = Math.min(S.MAX_ROWS - 1, sel.r2 + 1);
      for (let c = sel.c1; c <= sel.c2; c++) setCellInput(tx, s, below, c, `=${fn}(${S.rangeText({ r1: sel.r1, c1: c, r2: sel.r2, c2: c })})`);
    });
  }
  function insertNow(kind) {
    const d = doc(), s = sheet();
    const now = new Date();
    const serial = S.serialFromJsDate(now);
    const value = kind === "date" ? Math.floor(serial) : kind === "time" ? serial - Math.floor(serial) : serial;
    const fmt = kind === "date" ? (dayFirst ? "dd/mm/yyyy" : "m/d/yyyy") : kind === "time" ? "h:mm:ss AM/PM" : dayFirst ? "dd/mm/yyyy h:mm:ss" : "m/d/yyyy h:mm:ss";
    act("Insert date", (tx) => {
      const k = S.cellKey(d.sel.r, d.sel.c), old = s.cells.get(k);
      tx.setCell(s, k, S.makeCell(value, undefined, S.internStyle(d.wb, { ...S.styleOf(d.wb, old), fmt })));
    });
  }

  function addSheet(after = true) {
    const d = doc();
    act("Add sheet", (tx) => {
      tx.touchWorkbook("sheets"); tx.touchWorkbook("active");
      const created = S.createSheet(S.uniqueSheetName(d.wb, `Sheet${d.wb.sheets.length + 1}`));
      const at = after ? d.wb.active + 1 : d.wb.sheets.length;
      d.wb.sheets = [...d.wb.sheets.slice(0, at), created, ...d.wb.sheets.slice(at)];
      d.wb.active = at;
    });
    switchSheet(d.wb.active, true);
  }
  function switchSheet(index, force = false) {
    const d = doc();
    if (!d.wb.sheets[index]) return;
    if (d.editing && !d.editing.text.startsWith("=")) { if (!commitEdit()) return; }
    if (index === d.wb.active && !force) return;
    d.scroll.set(d.wb.active, { left: grid.scroller.scrollLeft, top: grid.scroller.scrollTop, sel: { ...d.sel }, anchor: { ...d.anchor } });
    d.wb.active = index;
    d.layoutVersion++;
    const saved = d.scroll.get(index);
    if (!d.editing) { d.sel = saved ? saved.sel : { r: 0, c: 0, r1: 0, c1: 0, r2: 0, c2: 0 }; d.anchor = saved ? saved.anchor : { r: 0, c: 0 }; }
    d.cf = null;
    updateSizer();
    grid.scroller.scrollLeft = saved ? saved.left : 0;
    grid.scroller.scrollTop = saved ? saved.top : 0;
    renderTabs();
    renderCharts();
    updateFormulaBar();
    updateStatus();
    renderToolbarState();
    draw();
    focusGrid();
  }
  function sheetMove(delta) {
    const d = doc(), i = d.wb.active, j = i + delta;
    if (j < 0 || j >= d.wb.sheets.length) return;
    act("Move sheet", (tx) => { tx.touchWorkbook("sheets"); tx.touchWorkbook("active"); const list = [...d.wb.sheets]; [list[i], list[j]] = [list[j], list[i]]; d.wb.sheets = list; d.wb.active = j; });
  }
  async function renameSheetPrompt(index = doc().wb.active) {
    const d = doc(), target = d.wb.sheets[index];
    const name = await promptDialog("Rename sheet", "Sheet name", target.name);
    if (name === null) return;
    const clean = name.trim();
    if (!S.validSheetName(clean)) { toast("Sheet names cannot be empty or use \\ / ? * [ ] :"); return; }
    if (d.wb.sheets.some((s, i) => i !== index && s.name.toLowerCase() === clean.toLowerCase())) { toast("Another sheet already has that name."); return; }
    act("Rename sheet", (tx) => S.renameSheet(tx, d.wb, target, clean));
  }
  async function deleteSheet(index = doc().wb.active) {
    const d = doc();
    if (d.wb.sheets.filter((s) => !s.hidden).length <= 1) { toast("A spreadsheet needs at least one visible sheet."); return; }
    if (!(await confirmDialog("Delete sheet", `Delete “${d.wb.sheets[index].name}”? You can undo this.`, "Delete"))) return;
    act("Delete sheet", (tx) => { tx.touchWorkbook("active"); S.removeSheet(tx, d.wb, d.wb.sheets[index]); d.wb.active = Math.max(0, Math.min(index, d.wb.sheets.length - 1)); while (d.wb.sheets[d.wb.active]?.hidden && d.wb.active > 0) d.wb.active--; });
    switchSheet(d.wb.active, true);
  }
  function duplicateSheet(index = doc().wb.active) {
    const d = doc(), source = d.wb.sheets[index];
    act("Duplicate sheet", (tx) => {
      tx.touchWorkbook("sheets"); tx.touchWorkbook("active");
      const copy = S.cloneSheet(d.wb, source, S.uniqueSheetName(d.wb, `Copy of ${source.name}`));
      d.wb.sheets = [...d.wb.sheets.slice(0, index + 1), copy, ...d.wb.sheets.slice(index + 1)];
      d.wb.active = index + 1;
    });
    switchSheet(d.wb.active, true);
  }
  function renderTabs() {
    const d = doc(), list = q("[data-sheets-tabs]");
    if (!d || !list) return;
    list.innerHTML = d.wb.sheets.map((s, i) => (s.hidden ? "" : `<button type="button" role="tab" class="sheets-tab" data-sheets-tab="${i}" aria-selected="${i === d.wb.active}" title="${esc(s.name)}">${s.color ? `<span class="sheets-tab-color" style="background:${esc(s.color)}"></span>` : ""}<span>${esc(s.name)}</span><i data-lucide="chevron-down" class="sheets-tab-menu" data-sheets-tab-menu="${i}"></i></button>`)).join("");
    icons();
    const active = list.querySelector('[aria-selected="true"]');
    if (active) {
      const left = active.offsetLeft, right = left + active.offsetWidth;
      if (left < list.scrollLeft) list.scrollLeft = left;
      else if (right > list.scrollLeft + list.clientWidth) list.scrollLeft = right - list.clientWidth;
    }
  }
  const openMenus = [];
  function closePopovers(level = 0) {
    while (openMenus.length > level) openMenus.pop().remove();
    if (!level) { q("[data-sheets-popover]")?.remove(); grid.activeMenu = null; qa("[data-sheets-menubar] [aria-expanded='true']").forEach((b) => b.setAttribute("aria-expanded", "false")); }
  }
  function placeFloating(el, rect, prefer = "below") {
    const vw = window.innerWidth, vh = window.innerHeight;
    el.style.position = "fixed";
    el.style.visibility = "hidden";
    root.appendChild(el);
    const w = el.offsetWidth, h = el.offsetHeight;
    let left = prefer === "right" ? rect.right - 2 : rect.left;
    let top = prefer === "right" ? rect.top - 4 : rect.bottom + 2;
    if (left + w > vw - 8) left = prefer === "right" ? rect.left - w + 2 : vw - w - 8;
    if (top + h > vh - 8) top = Math.max(8, prefer === "right" ? vh - h - 8 : rect.top - h - 2);
    el.style.left = Math.max(8, left) + "px";
    el.style.top = Math.max(8, top) + "px";
    el.style.visibility = "";
  }
  function openMenu(rect, items, level = 0) {
    closePopovers(level);
    const el = document.createElement("div");
    el.className = "fm-popover sheets-menu";
    el.setAttribute("role", "menu");
    const list = typeof items === "function" ? items() : items;
    el.innerHTML = `<div class="fm-menu">${list.map((item, i) => {
      if (item === "-") return '<div class="fm-menu-divider"></div>';
      if (item.heading) return `<div class="fm-menu-heading">${esc(item.heading)}</div>`;
      if (item.html) return item.html;
      const checked = item.checked === undefined ? "" : ` aria-checked="${!!item.checked}"`;
      const icon = item.swatch ? `<span class="sheets-swatch" style="background:${esc(item.swatch)}"></span>` : item.icon ? `<i data-lucide="${item.icon}"></i>` : '<span class="sheets-menu-gap"></span>';
      const hint = item.hint ? `<span class="fm-menu-hint">${esc(item.hint)}</span>` : item.submenu ? '<i data-lucide="chevron-right" class="sheets-menu-sub"></i>' : "";
      return `<button type="button" role="menuitem" class="fm-menu-item${item.danger ? " fm-menu-item-danger" : ""}" data-menu-index="${i}"${checked}${item.disabled ? " disabled" : ""}>${icon}<span class="sheets-menu-label">${esc(item.label)}</span>${hint}</button>`;
    }).join("")}</div>`;
    placeFloating(el, rect, level ? "right" : "below");
    openMenus.push(el);
    icons();
    const itemAt = (target) => { const button = target.closest("[data-menu-index]"); return button ? [button, list[Number(button.dataset.menuIndex)]] : [null, null]; };
    el.addEventListener("mouseover", (event) => {
      const [button, item] = itemAt(event.target);
      if (!button) return;
      if (item.submenu) { if (openMenus[level + 1]?.dataset.owner !== button.dataset.menuIndex) { openMenu(button.getBoundingClientRect(), item.submenu, level + 1); openMenus[level + 1].dataset.owner = button.dataset.menuIndex; } }
      else closePopovers(level + 1);
    });
    el.addEventListener("click", (event) => {
      const [button, item] = itemAt(event.target);
      if (!button || item.disabled) return;
      if (item.submenu) { openMenu(button.getBoundingClientRect(), item.submenu, level + 1); return; }
      closePopovers();
      if (item.action) Promise.resolve(item.action()).catch((error) => toast(error.message));
      if (!doc()?.editing) focusGrid();
    });
    return el;
  }
  function openPanel(rect, html, bind) {
    closePopovers();
    const el = document.createElement("div");
    el.className = "fm-popover sheets-panel";
    el.dataset.sheetsPopover = "";
    el.innerHTML = html;
    placeFloating(el, rect);
    icons();
    bind?.(el);
    return el;
  }
  const COLOR_ROWS = [
    ["#000000", "#434343", "#666666", "#999999", "#B7B7B7", "#CCCCCC", "#D9D9D9", "#EFEFEF", "#F3F3F3", "#FFFFFF"],
    ["#980000", "#FF0000", "#FF9900", "#FFFF00", "#00FF00", "#00FFFF", "#4A86E8", "#0000FF", "#9900FF", "#FF00FF"],
    ["#E6B8AF", "#F4CCCC", "#FCE5CD", "#FFF2CC", "#D9EAD3", "#D0E0E3", "#C9DAF8", "#CFE2F3", "#D9D2E9", "#EAD1DC"],
    ["#DD7E6B", "#EA9999", "#F9CB9C", "#FFE599", "#B6D7A8", "#A2C4C9", "#A4C2F4", "#9FC5E8", "#B4A7D6", "#D5A6BD"],
    ["#CC4125", "#E06666", "#F6B26B", "#FFD966", "#93C47D", "#76A5AF", "#6D9EEB", "#6FA8DC", "#8E7CC3", "#C27BA0"],
    ["#A61C00", "#CC0000", "#E69138", "#F1C232", "#6AA84F", "#45818E", "#3C78D8", "#3D85C6", "#674EA7", "#A64D79"],
    ["#85200C", "#990000", "#B45F06", "#BF9000", "#38761D", "#134F5C", "#1155CC", "#0B5394", "#351C75", "#741B47"],
  ];
  function colorPicker(rect, title, current, onPick) {
    const swatches = COLOR_ROWS.map((row) => `<div class="sheets-color-row">${row.map((color) => `<button type="button" class="sheets-color" data-color="${color}" style="background:${color}" title="${color}" aria-label="${color}"${current && current.toUpperCase() === color ? ' aria-pressed="true"' : ""}></button>`).join("")}</div>`).join("");
    openPanel(rect, `<div class="fm-popover-header"><p class="fm-popover-title">${esc(title)}</p></div><div class="p-2">${swatches}<div class="mt-2 flex items-center gap-2"><button type="button" class="fm-btn fm-btn-sm" data-color="">Reset</button><label class="flex items-center gap-2 text-xs text-muted-foreground">Custom<input type="color" data-color-custom value="${esc(current || "#000000")}" class="h-6 w-8 cursor-pointer border-0 bg-transparent p-0"></label></div></div>`, (el) => {
      el.addEventListener("click", (event) => { const button = event.target.closest("[data-color]"); if (!button) return; closePopovers(); onPick(button.dataset.color || ""); focusGrid(); });
      el.querySelector("[data-color-custom]").addEventListener("change", (event) => { closePopovers(); onPick(event.target.value.toUpperCase()); focusGrid(); });
    });
  }

  function openDialog({ title, body, submit = "", cancel = "Cancel", danger = false, wide = false, extra = [], onOpen, onSubmit }) {
    closePopovers();
    const dialog = document.createElement("dialog");
    dialog.className = `sheets-dialog w-full ${wide ? "max-w-3xl" : "max-w-lg"} bg-transparent p-0 text-foreground backdrop:bg-background/80`;
    dialog.innerHTML = `<form method="dialog" class="fm-dialog"><div class="fm-dialog-header"><h2 class="fm-dialog-title">${esc(title)}</h2><button type="button" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon" data-dialog-close aria-label="Close"><i data-lucide="x"></i></button></div><div class="fm-dialog-body">${body}</div><div class="fm-dialog-footer">${extra.map((b, i) => `<button type="button" class="fm-btn fm-btn-sm${b.danger ? " fm-btn-danger" : ""}${b.left ? " mr-auto" : ""}" data-dialog-extra="${i}">${esc(b.label)}</button>`).join("")}${cancel ? `<button type="button" class="fm-btn fm-btn-sm" data-dialog-close>${esc(cancel)}</button>` : ""}${submit ? `<button type="submit" class="fm-btn fm-btn-sm${danger ? " fm-btn-danger" : ""}" data-dialog-submit>${esc(submit)}</button>` : ""}</div></form>`;
    root.appendChild(dialog);
    icons();
    const close = () => { dialog.close(); dialog.remove(); if (doc()) focusGrid(); };
    dialog.addEventListener("cancel", (event) => { event.preventDefault(); close(); });
    dialog.querySelectorAll("[data-dialog-close]").forEach((b) => b.addEventListener("click", close));
    extra.forEach((b, i) => dialog.querySelector(`[data-dialog-extra="${i}"]`).addEventListener("click", () => b.action(dialog, close)));
    dialog.querySelector("form").addEventListener("submit", async (event) => {
      event.preventDefault();
      if (!onSubmit) return close();
      const keep = await onSubmit(dialog, close);
      if (keep !== true) close();
      return null;
    });
    dialog.showModal();
    onOpen?.(dialog, close);
    dialog.querySelector("input:not([type=hidden]):not([type=checkbox]),textarea,select")?.focus();
    return { dialog, close };
  }
  const field = (label, control, hint = "") => `<label class="sheets-field"><span>${esc(label)}</span>${control}${hint ? `<small>${esc(hint)}</small>` : ""}</label>`;
  const textInput = (name, value = "", attrs = "") => `<input name="${name}" value="${esc(value)}" class="sheets-input" ${attrs}>`;
  const selectInput = (name, options, value) => `<select name="${name}" class="sheets-input">${options.map(([v, label]) => `<option value="${esc(v)}"${String(v) === String(value) ? " selected" : ""}>${esc(label)}</option>`).join("")}</select>`;
  const checkInput = (name, label, checked) => `<label class="sheets-check"><input type="checkbox" name="${name}"${checked ? " checked" : ""}> ${esc(label)}</label>`;
  const formValues = (dialog) => { const out = {}; for (const el of dialog.querySelectorAll("[name]")) out[el.name] = el.type === "checkbox" ? el.checked : el.value; return out; };
  function promptDialog(title, label, value = "", multiline = false) {
    return new Promise((resolve) => {
      let done = false;
      const { dialog } = openDialog({
        title, submit: "OK",
        body: field(label, multiline ? `<textarea name="value" rows="5" class="sheets-input">${esc(value)}</textarea>` : textInput("value", value)),
        onSubmit: (dlg) => { done = true; resolve(formValues(dlg).value); },
      });
      dialog.addEventListener("close", () => { if (!done) resolve(null); });
      const input = dialog.querySelector("[name=value]");
      input.select?.();
    });
  }
  function confirmDialog(title, text, label = "OK") {
    return new Promise((resolve) => {
      let done = false;
      const { dialog } = openDialog({ title, body: `<p class="text-sm text-muted-foreground">${esc(text)}</p>`, submit: label, danger: /delete|remove/i.test(label), onSubmit: () => { done = true; resolve(true); } });
      dialog.addEventListener("close", () => { if (!done) resolve(false); });
    });
  }

  const NUMBER_FORMATS = () => [
    ["Automatic", "", "1000.12"], ["Plain text", "@", "Text"], "-",
    ["Number", "#,##0.00", "1,000.12"], ["Percent", "0.00%", "10.12%"], ["Scientific", "0.00E+00", "1.01E+03"], "-",
    ["Accounting", '_("$"* #,##0.00_);_("$"* (#,##0.00);_("$"* "-"??_);_(@_)', "$ (1,000.12)"], ["Financial", "#,##0.00;(#,##0.00)", "(1,000.12)"],
    ["Currency", '"$"#,##0.00', "$1,000.12"], ["Currency rounded", '"$"#,##0', "$1,000"], ["Euro", '#,##0.00 "€"', "1.000,12 €"], "-",
    ["Date", dayFirst ? "dd/mm/yyyy" : "m/d/yyyy", dayFirst ? "26/09/2026" : "9/26/2026"], ["Time", "h:mm:ss AM/PM", "3:59:00 PM"], ["Date time", dayFirst ? "dd/mm/yyyy h:mm:ss" : "m/d/yyyy h:mm:ss", ""], ["Duration", "[h]:mm:ss", "24:01:00"], ["Long date", "dddd, mmmm d, yyyy", ""],
  ];
  const numberFormatItems = () => [...NUMBER_FORMATS().map((item) => (item === "-" ? "-" : { label: item[0], hint: item[2], action: () => applyStyle({ fmt: item[1] || undefined }, "Number format") })), "-", { label: "Custom number format…", action: customFormatDialog }];
  const FONTS = ["Arial", "Calibri", "Cambria", "Comic Sans MS", "Courier New", "Georgia", "Helvetica", "Impact", "Inter", "Roboto", "Segoe UI", "Times New Roman", "Trebuchet MS", "Verdana"];
  const SIZES = [6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72];
  const BORDER_STYLES = [["thin", "Thin"], ["medium", "Medium"], ["thick", "Thick"], ["dashed", "Dashed"], ["dotted", "Dotted"], ["double", "Double"]];
  const borderItems = () => [
    ...[["all", "All borders", "grid-3x3"], ["outer", "Outer borders", "square"], ["inner", "Inner borders", "plus"], ["horizontal", "Horizontal borders", "minus"], ["vertical", "Vertical borders", "separator-vertical"], ["top", "Top border", "panel-top"], ["bottom", "Bottom border", "panel-bottom"], ["left", "Left border", "panel-left"], ["right", "Right border", "panel-right"], ["none", "Clear borders", "square-dashed"]].map(([kind, label, icon]) => ({ label, icon, action: () => applyBorders(kind) })),
    "-",
    { label: "Border style", icon: "pen-line", submenu: () => BORDER_STYLES.map(([w, label]) => ({ label, checked: (grid.borderStyle || "thin") === w, action: () => { grid.borderStyle = w; } })) },
    { label: "Border colour", icon: "palette", swatch: grid.borderColor || "#000000", action: () => colorPicker(q("[data-tool='borders']").getBoundingClientRect(), "Border colour", grid.borderColor, (color) => { grid.borderColor = color || "#000000"; }) },
  ];
  const alignItems = () => [
    { label: "Left", icon: "align-left", action: () => applyStyle({ h: "left" }, "Align") }, { label: "Center", icon: "align-center", action: () => applyStyle({ h: "center" }, "Align") }, { label: "Right", icon: "align-right", action: () => applyStyle({ h: "right" }, "Align") }, { label: "Justify", icon: "align-justify", action: () => applyStyle({ h: "justify" }, "Align") }, "-",
    { label: "Top", icon: "align-vertical-justify-start", action: () => applyStyle({ v: "top" }, "Align") }, { label: "Middle", icon: "align-vertical-justify-center", action: () => applyStyle({ v: "middle" }, "Align") }, { label: "Bottom", icon: "align-vertical-justify-end", action: () => applyStyle({ v: undefined }, "Align") }, "-",
    { label: "Increase indent", icon: "indent-increase", action: () => indent(1) }, { label: "Decrease indent", icon: "indent-decrease", action: () => indent(-1) },
  ];
  function indent(delta) { act("Indent", (tx) => styleRange(tx, sheet(), selectedRange(), (style) => { const n = Math.max(0, Math.min(15, (style.indent || 0) + delta)); style.indent = n || undefined; if (n && !style.h) style.h = "left"; return style; })); }
  const rotationItems = () => [[0, "None"], [45, "Tilt up"], [135, "Tilt down"], [90, "Rotate up"], [180, "Rotate down"]].map(([rot, label]) => ({ label, action: () => applyStyle({ rot: rot || undefined }, "Rotate text") }));
  const mergeItems = () => [{ label: "Merge all", icon: "table-cells-merge", action: () => mergeCells("all") }, { label: "Merge horizontally", action: () => mergeCells("horizontal") }, { label: "Merge vertically", action: () => mergeCells("vertical") }, { label: "Unmerge", icon: "table-cells-split", action: () => mergeCells("unmerge") }];
  const functionItems = () => [...["SUM", "AVERAGE", "COUNT", "MAX", "MIN"].map((fn) => ({ label: fn, action: () => autosum(fn) })), "-", { label: "All functions…", icon: "square-function", action: functionListDialog }];
  const freezeItems = () => {
    const s = sheet(), d = doc();
    return [
      { label: "No rows", checked: s.freezeRows === 0, action: () => freeze(0, null) }, { label: "1 row", checked: s.freezeRows === 1, action: () => freeze(1, null) }, { label: "2 rows", checked: s.freezeRows === 2, action: () => freeze(2, null) }, { label: `Up to row ${d.sel.r + 1}`, action: () => freeze(d.sel.r + 1, null) }, "-",
      { label: "No columns", checked: s.freezeCols === 0, action: () => freeze(null, 0) }, { label: "1 column", checked: s.freezeCols === 1, action: () => freeze(null, 1) }, { label: "2 columns", checked: s.freezeCols === 2, action: () => freeze(null, 2) }, { label: `Up to column ${S.colName(d.sel.c)}`, action: () => freeze(null, d.sel.c + 1) },
    ];
  };
  const zoomItems = () => [50, 75, 90, 100, 125, 150, 200].map((z) => ({ label: `${z}%`, checked: Math.round(doc().zoom * 100) === z, action: () => setZoom(z / 100) }));
  function setZoom(z) { const d = doc(); d.zoom = Math.max(0.25, Math.min(4, z)); grid.measure.clear(); renderToolbarState(); updateSizer(); renderCharts(); draw(); }
  const exportItems = () => [
    { label: "Microsoft Excel (.xlsx)", icon: "file-spreadsheet", action: () => exportAs("xlsx") }, { label: "Excel 97–2003 (.xls)", icon: "file-spreadsheet", action: () => exportAs("xls") },
    { label: "OpenDocument (.ods)", icon: "file-spreadsheet", action: () => exportAs("ods") }, "-",
    { label: "CSV, this sheet (.csv)", icon: "file-text", action: () => exportAs("csv") }, { label: "Tab separated, this sheet (.tsv)", icon: "file-text", action: () => exportAs("tsv") },
    { label: "PDF (print to PDF)", icon: "printer", action: () => exportAs("pdf") }, { label: "Web page (.html)", icon: "file-code", action: () => exportAs("html") },
    { label: "JSON (.json)", icon: "braces", action: () => exportAs("json") }, { label: "Markdown table, this sheet (.md)", icon: "file-text", action: () => exportAs("md") },
  ];
  function menus() {
    const d = doc(), s = sheet();
    return {
      File: [
        { label: "New spreadsheet", icon: "file-plus", action: () => createWorkbook(S.createWorkbook(), "Untitled spreadsheet") },
        { label: "Open a file…", icon: "folder-open", hint: "xls, xlsx, ods, csv", action: () => pickFiles(false) },
        { label: "Import into this spreadsheet…", icon: "file-input", action: () => pickFiles(true) },
        { label: "Make a copy", icon: "copy", action: makeCopy }, { label: "Rename…", icon: "pencil", action: renameWorkbook }, "-",
        { label: "Download", icon: "download", submenu: exportItems }, { label: "Print", icon: "printer", hint: "Ctrl+P", action: () => printSheet() }, "-",
        { label: "Version saved", icon: "save", hint: "Ctrl+S", action: () => save(true) },
        { label: "Delete spreadsheet…", icon: "trash-2", danger: true, action: () => deleteWorkbook(d.id, d.title) },
        { label: "All spreadsheets", icon: "arrow-left", action: closeEditor },
      ],
      Edit: [
        { label: "Undo", icon: "undo-2", hint: "Ctrl+Z", disabled: !d.undo.length, action: () => history("undo") }, { label: "Redo", icon: "redo-2", hint: "Ctrl+Y", disabled: !d.redo.length, action: () => history("redo") }, "-",
        { label: "Cut", icon: "scissors", hint: "Ctrl+X", action: () => copySelection(null, true) }, { label: "Copy", icon: "copy", hint: "Ctrl+C", action: () => copySelection(null, false) }, { label: "Paste", icon: "clipboard", hint: "Ctrl+V", action: () => pasteFromMenu("all") },
        { label: "Paste special", icon: "clipboard-paste", submenu: () => [{ label: "Values only", hint: "Ctrl+Shift+V", action: () => pasteFromMenu("values") }, { label: "Format only", action: () => pasteFromMenu("formats") }, { label: "Transposed", action: () => pasteFromMenu("transpose") }] }, "-",
        { label: "Find and replace", icon: "search", hint: "Ctrl+H", action: () => findDialog(true) }, { label: "Delete values", icon: "eraser", hint: "Delete", action: () => clearRange("values") }, { label: "Clear all", icon: "eraser", action: () => clearRange("all") },
        { label: "Fill down", hint: "Ctrl+D", action: () => fillDirection("down") }, { label: "Fill right", hint: "Ctrl+R", action: () => fillDirection("right") }, "-",
        { label: "Select all", hint: "Ctrl+A", action: () => selectRange({ r1: 0, c1: 0, r2: s.rowCount - 1, c2: s.colCount - 1 }, { r: 0, c: 0 }) },
      ],
      View: [
        { label: "Freeze", icon: "snowflake", submenu: freezeItems }, { label: "Gridlines", checked: s.showGrid, action: () => act("Gridlines", (tx) => tx.set(s, "showGrid", !s.showGrid)) },
        { label: "Formulas", checked: d.showFormulas, hint: "Ctrl+`", action: () => { d.showFormulas = !d.showFormulas; draw(); renderToolbarState(); } },
        { label: "Zoom", icon: "zoom-in", submenu: zoomItems }, "-",
        { label: "Hidden sheets", icon: "eye", submenu: () => { const hidden = d.wb.sheets.map((x, i) => [x, i]).filter(([x]) => x.hidden); return hidden.length ? hidden.map(([x, i]) => ({ label: x.name, action: () => { act("Unhide sheet", (tx) => tx.set(x, "hidden", false)); switchSheet(i, true); } })) : [{ label: "No hidden sheets", disabled: true }]; } },
        { label: "AI assistant", icon: "sparkles", checked: !q("[data-sheets-ai]").classList.contains("hidden"), action: toggleAi },
      ],
      Insert: [
        { label: `Row above`, icon: "arrow-up-to-line", action: () => structural("insertRows", true) }, { label: "Row below", icon: "arrow-down-to-line", action: () => structural("insertRows", false) },
        { label: "Column left", icon: "arrow-left-to-line", action: () => structural("insertCols", true) }, { label: "Column right", icon: "arrow-right-to-line", action: () => structural("insertCols", false) }, "-",
        { label: "Sheet", icon: "plus", action: () => addSheet(true) }, { label: "Chart…", icon: "chart-column", action: () => chartDialog() }, { label: "Pivot table…", icon: "table-2", action: pivotDialog },
        { label: "Function", icon: "sigma", submenu: functionItems }, "-",
        { label: "Link…", icon: "link", hint: "Ctrl+K", action: linkDialog }, { label: "Note…", icon: "sticky-note", hint: "Shift+F2", action: noteDialog },
        { label: "Checkbox", icon: "square-check", action: () => addValidation({ type: "checkbox" }) }, { label: "Dropdown…", icon: "list", action: () => validationDialog("list") },
        { label: "Date and time", icon: "calendar", submenu: () => [{ label: "Today's date", hint: "Ctrl+;", action: () => insertNow("date") }, { label: "Current time", hint: "Ctrl+Shift+;", action: () => insertNow("time") }, { label: "Date and time", action: () => insertNow("both") }] },
      ],
      Format: [
        { label: "Number", icon: "hash", submenu: numberFormatItems }, { label: "Text", icon: "bold", submenu: () => [{ label: "Bold", hint: "Ctrl+B", action: () => toggleStyle("b", "Bold") }, { label: "Italic", hint: "Ctrl+I", action: () => toggleStyle("i", "Italic") }, { label: "Underline", hint: "Ctrl+U", action: () => toggleStyle("u", "Underline") }, { label: "Strikethrough", hint: "Ctrl+5", action: () => toggleStyle("s", "Strikethrough") }] },
        { label: "Font size", icon: "a-large-small", submenu: () => SIZES.map((size) => ({ label: String(size), action: () => applyStyle({ size: size === doc().wb.defaultSize ? undefined : size }, "Font size") })) },
        { label: "Alignment", icon: "align-left", submenu: alignItems }, { label: "Wrapping", icon: "wrap-text", submenu: () => [{ label: "Overflow", action: () => applyStyle({ wrap: false }, "Wrapping") }, { label: "Wrap", action: () => applyStyle({ wrap: true }, "Wrapping") }] },
        { label: "Rotation", icon: "rotate-ccw", submenu: rotationItems }, { label: "Merge cells", icon: "table-cells-merge", submenu: mergeItems }, { label: "Borders", icon: "grid-3x3", submenu: borderItems }, "-",
        { label: "Conditional formatting…", icon: "paint-bucket", action: conditionalDialog }, { label: "Alternating colours…", icon: "rows-3", action: alternatingDialog }, "-",
        { label: "Row height…", action: () => sizeDialog("row") }, { label: "Column width…", action: () => sizeDialog("col") }, { label: "Fit columns to data", action: () => fitSelection("col") }, "-",
        { label: "Clear formatting", icon: "remove-formatting", hint: "Ctrl+\\", action: () => clearRange("formats") },
      ],
      Data: [
        { label: "Sort sheet A → Z", icon: "arrow-down-a-z", action: () => sortSelection(false, true) }, { label: "Sort sheet Z → A", icon: "arrow-up-z-a", action: () => sortSelection(true, true) }, { label: "Sort range…", icon: "arrow-down-up", action: sortDialog }, "-",
        { label: s.filter ? "Remove filter" : "Create a filter", icon: "filter", action: toggleFilter }, { label: "Data validation…", icon: "list-checks", action: () => validationDialog() },
        { label: "Named ranges…", icon: "tag", action: namedRangesDialog }, { label: "Pivot table…", icon: "table-2", action: pivotDialog }, "-",
        { label: "Remove duplicates…", icon: "copy-x", action: duplicatesDialog }, { label: "Trim whitespace", icon: "scissors", action: trimWhitespace }, { label: "Split text to columns…", icon: "columns-3", action: splitDialog },
      ],
      Help: [{ label: "Keyboard shortcuts", icon: "keyboard", hint: "Ctrl+/", action: shortcutsDialog }, { label: "Functions", icon: "square-function", action: functionListDialog }, { label: "Ask the AI assistant", icon: "sparkles", action: toggleAi }],
    };
  }
  function buildMenubar() {
    const bar = q("[data-sheets-menubar]");
    bar.innerHTML = Object.keys(menus()).map((name) => `<button type="button" class="sheets-menubar-item" data-menubar="${name}" aria-haspopup="menu" aria-expanded="false">${name}</button>`).join("");
    bar.addEventListener("mousedown", (event) => event.preventDefault());
    bar.addEventListener("click", (event) => {
      const button = event.target.closest("[data-menubar]");
      if (!button) return;
      if (doc().editing && !doc().editing.text.startsWith("=")) commitEdit();
      const open = button.getAttribute("aria-expanded") === "true";
      closePopovers();
      if (open) return;
      openMenu(button.getBoundingClientRect(), menus()[button.dataset.menubar]);
      button.setAttribute("aria-expanded", "true");
      grid.activeMenu = button.dataset.menubar;
    });
    bar.addEventListener("mouseover", (event) => {
      const button = event.target.closest("[data-menubar]");
      if (!button || !grid.activeMenu || grid.activeMenu === button.dataset.menubar) return;
      closePopovers();
      openMenu(button.getBoundingClientRect(), menus()[button.dataset.menubar]);
      button.setAttribute("aria-expanded", "true");
      grid.activeMenu = button.dataset.menubar;
    });
  }
  const TOOLS = [
    ["undo", "undo-2", "Undo (Ctrl+Z)"], ["redo", "redo-2", "Redo (Ctrl+Y)"], ["print", "printer", "Print (Ctrl+P)"], ["painter", "paintbrush", "Paint format"], "|",
    ["zoom", null, "Zoom"], "|",
    ["currency", "dollar-sign", "Format as currency"], ["percent", "percent", "Format as percent"], ["decDown", null, "Decrease decimal places"], ["decUp", null, "Increase decimal places"], ["numfmt", null, "More formats"], "|",
    ["font", null, "Font"], ["size", null, "Font size"], "|",
    ["bold", "bold", "Bold (Ctrl+B)"], ["italic", "italic", "Italic (Ctrl+I)"], ["underline", "underline", "Underline (Ctrl+U)"], ["strike", "strikethrough", "Strikethrough (Ctrl+5)"], ["color", "baseline", "Text colour"], "|",
    ["fill", "paint-bucket", "Fill colour"], ["borders", "grid-3x3", "Borders"], ["merge", "table-cells-merge", "Merge cells"], "|",
    ["align", "align-left", "Horizontal align"], ["valign", "align-vertical-justify-end", "Vertical align"], ["wrap", "wrap-text", "Text wrapping"], ["rotate", "rotate-ccw", "Text rotation"], "|",
    ["link", "link", "Insert link (Ctrl+K)"], ["note", "sticky-note", "Insert note"], ["chart", "chart-column", "Insert chart"], ["filter", "filter", "Create a filter"], ["functions", "sigma", "Functions"], "|",
    ["ai", "sparkles", "AI assistant"],
  ];
  function buildToolbar() {
    const bar = q("[data-sheets-toolbar]");
    bar.innerHTML = TOOLS.map((tool) => {
      if (tool === "|") return '<span class="sheets-tool-sep" aria-hidden="true"></span>';
      const [id, icon, title] = tool;
      if (id === "zoom") return `<button type="button" class="sheets-tool sheets-tool-text" data-tool="zoom" title="${title}"><span data-tool-label>100%</span><i data-lucide="chevron-down"></i></button>`;
      if (id === "decDown" || id === "decUp") return `<button type="button" class="sheets-tool sheets-tool-text" data-tool="${id}" title="${title}" aria-label="${title}"><span>${id === "decDown" ? ".0←" : ".00→"}</span></button>`;
      if (id === "numfmt") return `<button type="button" class="sheets-tool sheets-tool-text" data-tool="numfmt" title="${title}"><span>123</span><i data-lucide="chevron-down"></i></button>`;
      if (id === "font") return `<select class="sheets-select sheets-font" data-tool="font" title="${title}">${FONTS.map((f) => `<option>${esc(f)}</option>`).join("")}</select>`;
      if (id === "size") return `<input class="sheets-select sheets-size" data-tool="size" title="${title}" list="sheets-sizes" inputmode="numeric"><datalist id="sheets-sizes">${SIZES.map((n) => `<option value="${n}">`).join("")}</datalist>`;
      if (id === "ai") return `<button type="button" class="sheets-tool sheets-tool-text" data-tool="ai" title="${title}"><i data-lucide="${icon}"></i><span>Ask AI</span></button>`;
      const swatch = id === "color" || id === "fill" ? '<span class="sheets-tool-swatch" data-tool-swatch></span>' : "";
      return `<button type="button" class="sheets-tool" data-tool="${id}" title="${title}" aria-label="${title}"><i data-lucide="${icon}"></i>${swatch}</button>`;
    }).join("");
    icons();
    bar.addEventListener("mousedown", (event) => { if (event.target.closest("button")) event.preventDefault(); });
    bar.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-tool]");
      if (!button) return;
      const rect = button.getBoundingClientRect();
      const d = doc(), s = sheet();
      const style = S.styleOf(d.wb, s.cells.get(S.cellKey(d.sel.r, d.sel.c)));
      if (d.editing && !["undo", "redo"].includes(button.dataset.tool)) commitEdit();
      const actions = {
        undo: () => history("undo"), redo: () => history("redo"), print: () => printSheet(),
        painter: () => { d.painter = d.painter ? null : { style: { ...style } }; renderToolbarState(); toast(d.painter ? "Select the cells to paint this format onto." : "Paint format off"); },
        zoom: () => openMenu(rect, zoomItems()), currency: () => applyStyle({ fmt: '"$"#,##0.00' }, "Currency"), percent: () => applyStyle({ fmt: "0.00%" }, "Percent"),
        decDown: () => stepDecimals(-1), decUp: () => stepDecimals(1), numfmt: () => openMenu(rect, numberFormatItems()),
        bold: () => toggleStyle("b", "Bold"), italic: () => toggleStyle("i", "Italic"), underline: () => toggleStyle("u", "Underline"), strike: () => toggleStyle("s", "Strikethrough"),
        color: () => colorPicker(rect, "Text colour", style.color, (color) => applyStyle({ color: color || undefined }, "Text colour")),
        fill: () => colorPicker(rect, "Fill colour", style.fill, (color) => applyStyle({ fill: color || undefined }, "Fill colour")),
        borders: () => openMenu(rect, borderItems()), merge: () => openMenu(rect, mergeItems()),
        align: () => openMenu(rect, alignItems().slice(0, 4)), valign: () => openMenu(rect, alignItems().slice(5, 8)),
        wrap: () => openMenu(rect, [{ label: "Overflow", checked: !style.wrap, action: () => applyStyle({ wrap: false }, "Wrapping") }, { label: "Wrap", checked: !!style.wrap, action: () => applyStyle({ wrap: true }, "Wrapping") }]),
        rotate: () => openMenu(rect, rotationItems()), link: linkDialog, note: noteDialog, chart: () => chartDialog(), filter: toggleFilter, functions: () => openMenu(rect, functionItems()), ai: toggleAi,
      };
      actions[button.dataset.tool]?.();
    });
    bar.querySelector("[data-tool=font]").addEventListener("change", (event) => applyStyle({ font: event.target.value === doc().wb.defaultFont ? undefined : event.target.value }, "Font"));
    const size = bar.querySelector("[data-tool=size]");
    const applySize = () => { const n = Number(size.value); if (n >= 1 && n <= 400) applyStyle({ size: n === doc().wb.defaultSize ? undefined : n }, "Font size"); focusGrid(); };
    size.addEventListener("change", applySize);
    size.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); applySize(); } });
  }
  function renderToolbarState() {
    const d = doc();
    if (!d || !grid.built) return;
    const s = sheet();
    const style = S.styleOf(d.wb, s.cells.get(S.cellKey(d.sel.r, d.sel.c)));
    const pressed = { bold: style.b, italic: style.i, underline: style.u, strike: style.s, painter: !!d.painter, filter: !!s.filter, ai: !q("[data-sheets-ai]").classList.contains("hidden") };
    for (const [id, on] of Object.entries(pressed)) q(`[data-tool="${id}"]`)?.setAttribute("aria-pressed", on ? "true" : "false");
    const undo = q('[data-tool="undo"]'), redo = q('[data-tool="redo"]');
    if (undo) undo.disabled = !d.undo.length;
    if (redo) redo.disabled = !d.redo.length;
    const zoom = q('[data-tool="zoom"] [data-tool-label]');
    if (zoom) zoom.textContent = `${Math.round(d.zoom * 100)}%`;
    const font = q('[data-tool="font"]');
    if (font) { const name = style.font || d.wb.defaultFont; if (![...font.options].some((o) => o.value === name)) font.add(new Option(name, name)); font.value = name; }
    const size = q('[data-tool="size"]');
    if (size && document.activeElement !== size) size.value = String(style.size || d.wb.defaultSize);
    const swatches = qa("[data-tool-swatch]");
    if (swatches[0]) swatches[0].style.background = style.color || "currentColor";
    if (swatches[1]) swatches[1].style.background = style.fill || "transparent";
    const alignIcon = q('[data-tool="align"] i, [data-tool="align"] svg');
    if (alignIcon && style.h && alignIcon.dataset.lucide !== `align-${style.h === "justify" ? "justify" : style.h}`) { q('[data-tool="align"]').innerHTML = `<i data-lucide="align-${style.h}"></i>`; icons(); }
  }
  function customFormatDialog() {
    const d = doc(), s = sheet();
    const cell = s.cells.get(S.cellKey(d.sel.r, d.sel.c));
    const current = S.styleOf(d.wb, cell).fmt || "General";
    const sample = d.calc.cellValue(s, d.sel.r, d.sel.c);
    const examples = ["0", "0.00", "#,##0", "#,##0.00", "0%", "0.00%", '"$"#,##0.00', '"$"#,##0.00_);[Red]("$"#,##0.00)', "0.00E+00", "# ?/?", "yyyy-mm-dd", "dd/mm/yyyy", "mmm d, yyyy", "dddd", "h:mm AM/PM", "[h]:mm:ss", '0 "units"', "@"];
    openDialog({
      title: "Custom number format", submit: "Apply",
      body: `${field("Format code", textInput("code", current, 'data-format-code autocomplete="off" spellcheck="false"'), "Sections: positive;negative;zero;text. Colours like [Red], dates like yyyy-mm-dd.")}<p class="mt-2 text-sm">Preview: <strong data-format-preview></strong></p><div class="mt-3 flex flex-wrap gap-1">${examples.map((code) => `<button type="button" class="fm-btn fm-btn-sm" data-format-example="${esc(code)}">${esc(code)}</button>`).join("")}</div>`,
      onOpen: (dialog) => {
        const input = dialog.querySelector("[data-format-code]");
        const preview = () => { dialog.querySelector("[data-format-preview]").textContent = S.formatValue(typeof sample === "number" || typeof sample === "string" ? sample : 1234.5, input.value).text; };
        input.addEventListener("input", preview);
        dialog.addEventListener("click", (event) => { const b = event.target.closest("[data-format-example]"); if (b) { input.value = b.dataset.formatExample; preview(); } });
        preview();
      },
      onSubmit: (dialog) => applyStyle({ fmt: formValues(dialog).code.trim() || undefined }, "Number format"),
    });
  }
  const FUNCTION_HELP = {
    SUM: "SUM(value1, [value2, …]) — adds numbers", AVERAGE: "AVERAGE(value1, [value2, …]) — the mean", COUNT: "COUNT(value1, …) — counts numbers", COUNTA: "COUNTA(value1, …) — counts non-empty cells",
    MAX: "MAX(value1, …)", MIN: "MIN(value1, …)", IF: "IF(test, value_if_true, [value_if_false])", IFS: "IFS(test1, value1, [test2, value2, …])", IFERROR: "IFERROR(value, value_if_error)",
    AND: "AND(test1, [test2, …])", OR: "OR(test1, [test2, …])", NOT: "NOT(test)", SWITCH: "SWITCH(expression, case1, value1, …, [default])",
    VLOOKUP: "VLOOKUP(search_key, range, index, [is_sorted])", HLOOKUP: "HLOOKUP(search_key, range, index, [is_sorted])", XLOOKUP: "XLOOKUP(search_key, lookup_range, result_range, [missing_value], [match_mode], [search_mode])",
    INDEX: "INDEX(range, row, [column])", MATCH: "MATCH(search_key, range, [search_type])", OFFSET: "OFFSET(reference, rows, cols, [height], [width])", INDIRECT: "INDIRECT(reference_text)",
    SUMIF: "SUMIF(range, criterion, [sum_range])", SUMIFS: "SUMIFS(sum_range, range1, criterion1, …)", COUNTIF: "COUNTIF(range, criterion)", COUNTIFS: "COUNTIFS(range1, criterion1, …)", AVERAGEIF: "AVERAGEIF(range, criterion, [average_range])",
    ROUND: "ROUND(value, [places])", ROUNDUP: "ROUNDUP(value, [places])", ROUNDDOWN: "ROUNDDOWN(value, [places])", ABS: "ABS(value)", MOD: "MOD(dividend, divisor)", POWER: "POWER(base, exponent)", SQRT: "SQRT(value)",
    CONCATENATE: "CONCATENATE(text1, [text2, …])", CONCAT: "CONCAT(text1, …)", TEXTJOIN: "TEXTJOIN(delimiter, ignore_empty, text1, …)", LEFT: "LEFT(text, [count])", RIGHT: "RIGHT(text, [count])", MID: "MID(text, start, length)",
    LEN: "LEN(text)", TRIM: "TRIM(text)", UPPER: "UPPER(text)", LOWER: "LOWER(text)", PROPER: "PROPER(text)", SUBSTITUTE: "SUBSTITUTE(text, search_for, replace_with, [occurrence])", FIND: "FIND(search_for, text, [start])", SEARCH: "SEARCH(search_for, text, [start])",
    TEXT: "TEXT(number, format)", VALUE: "VALUE(text)", SPLIT: "SPLIT(text, delimiter)", TODAY: "TODAY()", NOW: "NOW()", DATE: "DATE(year, month, day)", TIME: "TIME(hour, minute, second)",
    YEAR: "YEAR(date)", MONTH: "MONTH(date)", DAY: "DAY(date)", WEEKDAY: "WEEKDAY(date, [type])", EDATE: "EDATE(start_date, months)", EOMONTH: "EOMONTH(start_date, months)", DATEDIF: "DATEDIF(start_date, end_date, unit)",
    NETWORKDAYS: "NETWORKDAYS(start_date, end_date, [holidays])", WORKDAY: "WORKDAY(start_date, days, [holidays])", PMT: "PMT(rate, periods, present_value, [future_value], [type])", FV: "FV(rate, periods, payment, [present_value], [type])",
    PV: "PV(rate, periods, payment, [future_value], [type])", NPV: "NPV(rate, value1, …)", IRR: "IRR(values, [guess])", RATE: "RATE(periods, payment, present_value, …)", MEDIAN: "MEDIAN(value1, …)", STDEV: "STDEV(value1, …)",
    SUMPRODUCT: "SUMPRODUCT(array1, [array2, …])", UNIQUE: "UNIQUE(range)", SORT: "SORT(range, [column], [ascending])", FILTER: "FILTER(range, condition)", SEQUENCE: "SEQUENCE(rows, [columns], [start], [step])", TRANSPOSE: "TRANSPOSE(range)",
    LET: "LET(name, value, …, expression)", SUBTOTAL: "SUBTOTAL(function_code, range1, …)", RANK: "RANK(value, data, [descending])", LARGE: "LARGE(data, n)", SMALL: "SMALL(data, n)", HYPERLINK: "HYPERLINK(url, [label])",
  };
  function functionListDialog() {
    const names = S.FUNCTION_NAMES;
    openDialog({
      title: "Functions", cancel: "Close", wide: true,
      body: `${textInput("search", "", 'placeholder="Search functions" data-fn-search autocomplete="off"')}<div class="sheets-fn-list" data-fn-list></div><p class="mt-2 text-xs text-muted-foreground">${names.length} functions. Click one to start a formula in the selected cell.</p>`,
      onOpen: (dialog, close) => {
        const list = dialog.querySelector("[data-fn-list]");
        const render = () => {
          const term = dialog.querySelector("[data-fn-search]").value.trim().toUpperCase();
          list.innerHTML = names.filter((n) => !term || n.includes(term) || (FUNCTION_HELP[n] || "").toUpperCase().includes(term)).map((n) => `<button type="button" class="sheets-fn-item" data-fn="${n}"><strong>${n}</strong><span>${esc(FUNCTION_HELP[n] || `${n}(…)`)}</span></button>`).join("");
        };
        dialog.querySelector("[data-fn-search]").addEventListener("input", render);
        list.addEventListener("click", (event) => { const b = event.target.closest("[data-fn]"); if (!b) return; close(); startEdit(`=${b.dataset.fn}(`, "edit"); });
        render();
      },
    });
  }
  function shortcutsDialog() {
    const rows = [["Ctrl+C / X / V", "Copy, cut, paste"], ["Ctrl+Shift+V", "Paste values only"], ["Ctrl+Z / Ctrl+Y", "Undo, redo"], ["Ctrl+B / I / U / 5", "Bold, italic, underline, strikethrough"], ["F2 or Enter", "Edit the cell"], ["Alt+Enter", "New line in a cell"], ["Ctrl+Enter", "Fill the selection with what you typed"], ["F4", "Cycle $ in the reference at the cursor"], ["Ctrl+arrows", "Jump to the edge of the data"], ["Shift+arrows", "Extend the selection"], ["Ctrl+Space / Shift+Space", "Select the column / row"], ["Ctrl+A", "Select all"], ["Ctrl+D / Ctrl+R", "Fill down / right"], ["Ctrl+; / Ctrl+Shift+;", "Insert date / time"], ["Ctrl+F / Ctrl+H", "Find / find and replace"], ["Ctrl+K", "Insert link"], ["Shift+F2", "Insert note"], ["Ctrl+`", "Show formulas"], ["Ctrl+Shift+4 / 5", "Currency / percent format"], ["Ctrl+PageUp / PageDown", "Previous / next sheet"], ["Ctrl+S", "Save now"], ["Ctrl+P", "Print or save as PDF"], ["Delete", "Delete values"]];
    openDialog({ title: "Keyboard shortcuts", cancel: "Close", body: `<div class="sheets-shortcuts">${rows.map(([k, v]) => `<kbd>${esc(k)}</kbd><span>${esc(v)}</span>`).join("")}</div><p class="mt-3 text-xs text-muted-foreground">On a Mac use ⌘ for Ctrl.</p>` });
  }
  function findDialog(replace) {
    const d = doc();
    openDialog({
      title: replace ? "Find and replace" : "Find", cancel: "Close",
      body: `${field("Find", textInput("find", d.lastFind || "", 'autocomplete="off"'))}${replace ? field("Replace with", textInput("replace", "", 'autocomplete="off"')) : ""}<div class="mt-2 grid gap-1">${checkInput("matchCase", "Match case", false)}${checkInput("entire", "Match entire cell contents", false)}${checkInput("formulas", "Also search within formulas", false)}${checkInput("allSheets", "Search all sheets", false)}</div><p class="mt-2 text-xs text-muted-foreground" data-find-status></p>`,
      extra: [{ label: "Find next", action: (dialog) => findNext(dialog) }, ...(replace ? [{ label: "Replace", action: (dialog) => replaceOne(dialog) }, { label: "Replace all", action: (dialog) => replaceAll(dialog) }] : [])],
      onOpen: (dialog) => dialog.querySelector("[name=find]").addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); findNext(dialog); } }),
    });
  }
  function findMatcher(opts) {
    const flags = opts.matchCase ? "g" : "gi";
    const escaped = opts.find.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    return new RegExp(opts.entire ? `^${escaped}$` : escaped, flags);
  }
  function findCells(opts) {
    const d = doc();
    const re = findMatcher(opts);
    const out = [];
    const sheets = opts.allSheets ? d.wb.sheets.map((s, i) => [s, i]) : [[sheet(), d.wb.active]];
    for (const [s, index] of sheets) {
      const keys = [...s.cells.keys()].sort((a, b) => a - b);
      for (const k of keys) {
        const cell = s.cells.get(k), r = S.keyRow(k), c = S.keyCol(k);
        const text = opts.formulas && cell.f !== undefined ? "=" + cell.f : S.displayText(d.wb, s, d.calc, r, c);
        re.lastIndex = 0;
        if (opts.find && re.test(text)) out.push({ index, r, c, k });
      }
    }
    return out;
  }
  function findNext(dialog) {
    const d = doc(), opts = formValues(dialog);
    d.lastFind = opts.find;
    const hits = findCells(opts);
    const status = dialog.querySelector("[data-find-status]");
    if (!hits.length) { status.textContent = "No matches."; return; }
    const pos = hits.findIndex((h) => h.index > d.wb.active || (h.index === d.wb.active && (h.r > d.sel.r || (h.r === d.sel.r && h.c > d.sel.c))));
    const hit = hits[pos < 0 ? 0 : pos];
    if (hit.index !== d.wb.active) switchSheet(hit.index);
    setSelection(hit.r, hit.c);
    status.textContent = `${hits.length} match${hits.length === 1 ? "" : "es"}.`;
  }
  function replaceIn(cell, s, r, c, opts) {
    const re = findMatcher(opts);
    const source = cell.f !== undefined ? (opts.formulas ? "=" + cell.f : null) : cellInputText(s, r, c);
    if (source === null) return null;
    const next = source.replace(re, () => opts.replace);
    return next === source ? null : next;
  }
  function replaceOne(dialog) {
    const d = doc(), opts = formValues(dialog), s = sheet();
    const cell = s.cells.get(S.cellKey(d.sel.r, d.sel.c));
    const next = cell ? replaceIn(cell, s, d.sel.r, d.sel.c, opts) : null;
    if (next !== null) act("Replace", (tx) => setCellInput(tx, s, d.sel.r, d.sel.c, next));
    findNext(dialog);
  }
  function replaceAll(dialog) {
    const d = doc(), opts = formValues(dialog);
    const hits = findCells(opts);
    let n = 0;
    act("Replace all", (tx) => {
      for (const hit of hits) {
        const s = d.wb.sheets[hit.index], cell = s.cells.get(hit.k);
        const next = replaceIn(cell, s, hit.r, hit.c, opts);
        if (next !== null) { setCellInput(tx, s, hit.r, hit.c, next); n++; }
      }
    });
    dialog.querySelector("[data-find-status]").textContent = `Replaced ${n} cell${n === 1 ? "" : "s"}.`;
  }

  const CF_TYPES = [["gt", "Greater than"], ["ge", "Greater than or equal to"], ["lt", "Less than"], ["le", "Less than or equal to"], ["eq", "Is equal to"], ["ne", "Is not equal to"], ["between", "Is between"], ["notbetween", "Is not between"], ["contains", "Text contains"], ["notcontains", "Text does not contain"], ["begins", "Text starts with"], ["ends", "Text ends with"], ["equals", "Text is exactly"], ["blank", "Is empty"], ["notblank", "Is not empty"], ["error", "Is an error"], ["duplicate", "Duplicate values"], ["unique", "Unique values"], ["formula", "Custom formula is"], ["scale", "Colour scale"]];
  function describeRule(rule) {
    if (rule.type === "scale") return `Colour scale ${(rule.colors || []).join(" → ")}`;
    if (rule.type === "cell") { const label = CF_TYPES.find(([v]) => v === rule.op)?.[1] || rule.op; const a = rule.value ?? (rule.valueFormula ? "=" + rule.valueFormula : ""); const b = rule.value2 ?? (rule.value2Formula ? "=" + rule.value2Formula : ""); return `${label} ${a}${(rule.op === "between" || rule.op === "notbetween") ? ` and ${b}` : ""}`; }
    if (rule.type === "text") return `${CF_TYPES.find(([v]) => v === (rule.op === "equals" ? "equals" : rule.op))?.[1] || "Text"} “${rule.text}”`;
    if (rule.type === "formula") return `Formula =${rule.formula}`;
    return CF_TYPES.find(([v]) => v === rule.type)?.[1] || rule.type;
  }
  function conditionalDialog() {
    const s = sheet();
    const list = () => s.conditional.length ? s.conditional.map((rule, i) => `<div class="sheets-rule"><span class="sheets-rule-sample" style="${rule.style?.fill ? `background:${esc(rule.style.fill)};` : ""}${rule.style?.color ? `color:${esc(rule.style.color)};` : ""}${rule.style?.b ? "font-weight:600;" : ""}">${rule.type === "scale" ? `<span style="background:linear-gradient(90deg,${esc((rule.colors || []).join(","))});display:block;width:100%;height:100%"></span>` : "Aa"}</span><span class="min-w-0 flex-1"><strong>${esc(rule.range)}</strong><br><span class="text-xs text-muted-foreground">${esc(describeRule(rule))}</span></span><button type="button" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon" data-rule-delete="${i}" aria-label="Delete rule"><i data-lucide="trash-2"></i></button></div>`).join("") : '<p class="text-sm text-muted-foreground">No rules on this sheet yet.</p>';
    openDialog({
      title: "Conditional formatting", submit: "Add rule", cancel: "Done", wide: true,
      body: `<div data-rule-list class="sheets-rules">${list()}</div><div class="sheets-form-grid mt-4">
        ${field("Apply to range", textInput("range", S.rangeText(selectedRange())))}
        ${field("Format cells if", selectInput("type", CF_TYPES, "gt"))}
        ${field("Value or formula", textInput("value", "", 'placeholder="e.g. 100, Done or =$B2>10"'))}
        ${field("Second value", textInput("value2", "", 'placeholder="for between"'))}
        ${field("Fill colour", '<input type="color" name="fill" value="#f4cccc" class="sheets-input h-control p-1">')}
        ${field("Text colour", '<input type="color" name="color" value="#990000" class="sheets-input h-control p-1">')}
        ${field("Scale low", '<input type="color" name="low" value="#f8696b" class="sheets-input h-control p-1">')}
        ${field("Scale middle", '<input type="color" name="mid" value="#ffeb84" class="sheets-input h-control p-1">')}
        ${field("Scale high", '<input type="color" name="high" value="#63be7b" class="sheets-input h-control p-1">')}
        <div class="sheets-field">${checkInput("bold", "Bold", false)}${checkInput("useColor", "Change text colour", false)}${checkInput("stop", "Stop if true", false)}</div></div>`,
      onOpen: (dialog) => dialog.querySelector("[data-rule-list]").addEventListener("click", (event) => {
        const b = event.target.closest("[data-rule-delete]");
        if (!b) return;
        const index = Number(b.dataset.ruleDelete);
        act("Delete formatting rule", (tx) => tx.set(s, "conditional", s.conditional.filter((_, i) => i !== index)));
        dialog.querySelector("[data-rule-list]").innerHTML = list(); icons();
      }),
      onSubmit: (dialog) => {
        const v = formValues(dialog);
        const range = S.parseRange(v.range.replace(/\$/g, ""));
        if (!range) { toast("Enter a range like A1:C20."); return true; }
        const style = { fill: v.fill.toUpperCase() };
        if (v.useColor) style.color = v.color.toUpperCase();
        if (v.bold) style.b = true;
        const rule = { id: S.uid(), range: S.rangeText(range), style, stop: v.stop };
        if (v.type === "scale") Object.assign(rule, { type: "scale", colors: [v.low.toUpperCase(), v.mid.toUpperCase(), v.high.toUpperCase()] });
        else if (["contains", "notcontains", "begins", "ends", "equals"].includes(v.type)) Object.assign(rule, { type: "text", op: v.type, text: v.value });
        else if (v.type === "formula") Object.assign(rule, { type: "formula", formula: v.value.replace(/^=/, "") });
        else if (["blank", "notblank", "error", "duplicate", "unique"].includes(v.type)) rule.type = v.type;
        else Object.assign(rule, { type: "cell", op: v.type, value: v.value, value2: v.value2 });
        act("Add formatting rule", (tx) => tx.set(s, "conditional", [...s.conditional, rule]));
        dialog.querySelector("[data-rule-list]").innerHTML = list(); icons();
        return true;
      },
    });
    icons();
  }
  function alternatingDialog() {
    const s = sheet(), d = doc();
    let range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) range = S.dataRegion(s, d.calc, d.sel.r, d.sel.c);
    openDialog({
      title: "Alternating colours", submit: "Apply",
      body: `${field("Range", textInput("range", S.rangeText(clampToUsed(range))))}${field("Header colour", '<input type="color" name="header" value="#4472c4" class="sheets-input h-control p-1">')}${field("Band colour", '<input type="color" name="band" value="#d9e1f2" class="sheets-input h-control p-1">')}${checkInput("hasHeader", "First row is a header", true)}`,
      onSubmit: (dialog) => {
        const v = formValues(dialog), r = S.parseRange(v.range);
        if (!r) return true;
        act("Alternating colours", (tx) => styleRange(tx, s, r, (style, at) => {
          if (v.hasHeader && at.row === r.r1) { style.fill = v.header.toUpperCase(); style.b = true; style.color = S.luminance(v.header) < 0.5 ? "#FFFFFF" : "#000000"; }
          else style.fill = (at.row - r.r1 - (v.hasHeader ? 1 : 0)) % 2 === 1 ? v.band.toUpperCase() : undefined;
          return style;
        }));
        return false;
      },
    });
  }
  function sizeDialog(axis) {
    const s = sheet(), d = doc();
    const current = axis === "row" ? s.rowHeights.get(d.sel.r) ?? s.defaultRowHeight : s.colWidths.get(d.sel.c) ?? s.defaultColWidth;
    openDialog({
      title: axis === "row" ? "Row height" : "Column width", submit: "OK",
      body: `${field("Size in pixels", textInput("size", current, 'type="number" min="2" max="2000"'))}${checkInput("fit", "Fit to data", false)}`,
      onSubmit: (dialog) => { const v = formValues(dialog); if (v.fit) fitSelection(axis); else resizeLines(axis, Math.max(2, Math.min(2000, Number(v.size) || current))); },
    });
  }
  function headerOptions(range) {
    const d = doc(), s = sheet();
    const out = [];
    for (let c = range.c1; c <= Math.min(range.c2, range.c1 + 200); c++) { const label = S.valueToText(d.calc.cellValue(s, range.r1, c)); out.push([String(c), `Column ${S.colName(c)}${label ? ` — ${label.slice(0, 30)}` : ""}`]); }
    return out;
  }
  function sortDialog() {
    const d = doc(), s = sheet();
    let range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) range = S.dataRegion(s, d.calc, d.sel.r, d.sel.c);
    range = clampToUsed(range);
    const options = headerOptions(range);
    openDialog({
      title: "Sort range", submit: "Sort",
      body: `${field("Range", textInput("range", S.rangeText(range)))}${checkInput("header", "Data has a header row", true)}<div class="sheets-form-grid mt-2">${field("Sort by", selectInput("col", options, String(d.sel.c)))}${field("Order", selectInput("order", [["asc", "A → Z"], ["desc", "Z → A"]], "asc"))}${field("Then by", selectInput("col2", [["", "(none)"], ...options], ""))}${field("Order", selectInput("order2", [["asc", "A → Z"], ["desc", "Z → A"]], "asc"))}</div>`,
      onSubmit: (dialog) => {
        const v = formValues(dialog), r = S.parseRange(v.range);
        if (!r) return true;
        if (v.header) r.r1 += 1;
        const keys = [{ col: Number(v.col), desc: v.order === "desc" }];
        if (v.col2 !== "") keys.push({ col: Number(v.col2), desc: v.order2 === "desc" });
        act("Sort range", (tx) => S.sortRows(tx, d.wb, d.calc, s, r, keys));
        return false;
      },
    });
  }
  function pivotDialog() {
    const d = doc(), s = sheet();
    let range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) range = S.dataRegion(s, d.calc, d.sel.r, d.sel.c);
    range = clampToUsed(range);
    const options = headerOptions(range).map(([c, label], i) => [String(i), label]);
    openDialog({
      title: "Pivot table", submit: "Create",
      body: `${field("Source range (first row is the header)", textInput("range", S.rangeText(range)))}<div class="sheets-form-grid">${field("Rows", selectInput("row", options, "0"))}${field("Columns", selectInput("col", [["", "(none)"], ...options], ""))}${field("Values", selectInput("value", options, String(Math.min(options.length - 1, 1))))}${field("Summarise by", selectInput("agg", [["sum", "SUM"], ["count", "COUNT"], ["counta", "COUNTA"], ["average", "AVERAGE"], ["min", "MIN"], ["max", "MAX"]], "sum"))}</div><p class="text-xs text-muted-foreground">The pivot table is written to a new sheet as values. Run it again after the data changes.</p>`,
      onSubmit: (dialog) => {
        const v = formValues(dialog), r = S.parseRange(v.range);
        if (!r || r.r2 <= r.r1) { toast("Pick a range with a header row and data."); return true; }
        const out = S.pivotTable(d.calc, s, r, { rowField: Number(v.row), colField: v.col === "" ? null : Number(v.col), valueField: Number(v.value), aggregate: v.agg });
        act("Pivot table", (tx) => {
          tx.touchWorkbook("sheets"); tx.touchWorkbook("active");
          const created = S.createSheet(S.uniqueSheetName(d.wb, "Pivot table 1"));
          d.wb.sheets = [...d.wb.sheets, created];
          d.wb.active = d.wb.sheets.length - 1;
          const bold = S.internStyle(d.wb, { b: true, fill: "#D9E1F2", bb: { w: "thin", c: "#8EA9DB" } });
          out.forEach((row, i) => row.forEach((value, j) => tx.setCell(created, S.cellKey(i, j), S.makeCell(value === null ? undefined : value, undefined, i === 0 || i === out.length - 1 || j === 0 ? bold : 0))));
          const widths = new Map(); widths.set(0, 160); tx.set(created, "colWidths", widths);
        });
        switchSheet(d.wb.active, true);
        return false;
      },
    });
  }
  function namedRangesDialog() {
    const d = doc();
    const list = () => d.wb.names.size ? [...d.wb.names.values()].map((def) => `<div class="sheets-rule"><span class="min-w-0 flex-1"><strong>${esc(def.name)}</strong><br><span class="text-xs text-muted-foreground">${esc(def.ref)}</span></span><button type="button" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon" data-name-delete="${esc(def.name.toUpperCase())}" aria-label="Delete name"><i data-lucide="trash-2"></i></button></div>`).join("") : '<p class="text-sm text-muted-foreground">No named ranges yet.</p>';
    openDialog({
      title: "Named ranges", submit: "Add", cancel: "Done",
      body: `<div data-name-list class="sheets-rules">${list()}</div><div class="sheets-form-grid mt-3">${field("Name", textInput("name", "", 'placeholder="Sales_2026"'))}${field("Range", textInput("ref", `${S.quoteSheet(sheet().name)}!${S.rangeText(selectedRange()).replace(/([A-Z]+)(\d+)/g, "$$$1$$$2")}`))}</div>`,
      onOpen: (dialog) => dialog.querySelector("[data-name-list]").addEventListener("click", (event) => {
        const b = event.target.closest("[data-name-delete]");
        if (!b) return;
        act("Delete named range", (tx) => { tx.touchWorkbook("names"); const next = new Map(d.wb.names); next.delete(b.dataset.nameDelete); d.wb.names = next; });
        dialog.querySelector("[data-name-list]").innerHTML = list(); icons();
      }),
      onSubmit: (dialog) => {
        const v = formValues(dialog), name = v.name.trim();
        if (!/^[A-Za-z_\\][A-Za-z0-9_.]{0,254}$/.test(name) || /^[A-Za-z]{1,3}\d+$/.test(name) || /^(TRUE|FALSE)$/i.test(name)) { toast("Names start with a letter and cannot look like a cell address."); return true; }
        try { S.parseFormula(v.ref.replace(/^=/, "")); } catch { toast("That range is not valid."); return true; }
        act("Add named range", (tx) => { tx.touchWorkbook("names"); const next = new Map(d.wb.names); next.set(name.toUpperCase(), { name, ref: v.ref.replace(/^=/, ""), sheet: "" }); d.wb.names = next; });
        dialog.querySelector("[data-name-list]").innerHTML = list(); icons();
        return true;
      },
    });
    icons();
  }
  function duplicatesDialog() {
    const d = doc(), s = sheet();
    let range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) range = S.dataRegion(s, d.calc, d.sel.r, d.sel.c);
    range = clampToUsed(range);
    openDialog({
      title: "Remove duplicates", submit: "Remove duplicates",
      body: `${field("Range", textInput("range", S.rangeText(range)))}${checkInput("header", "Data has a header row", true)}<p class="mt-2 text-xs text-muted-foreground">Rows whose values match an earlier row in every column are removed; the rows below move up inside the range.</p>`,
      onSubmit: (dialog) => {
        const v = formValues(dialog), r = S.parseRange(v.range);
        if (!r) return true;
        const start = v.header ? r.r1 + 1 : r.r1;
        const seen = new Set(), keep = [];
        for (let row = start; row <= r.r2; row++) {
          const key = JSON.stringify(Array.from({ length: r.c2 - r.c1 + 1 }, (_, j) => { const value = d.calc.cellValue(s, row, r.c1 + j); return typeof value === "string" ? value.toLowerCase().trim() : S.valueToText(value); }));
          if (seen.has(key)) continue;
          seen.add(key); keep.push(row);
        }
        const removed = r.r2 - start + 1 - keep.length;
        act("Remove duplicates", (tx) => {
          const rows = keep.map((row) => Array.from({ length: r.c2 - r.c1 + 1 }, (_, j) => s.cells.get(S.cellKey(row, r.c1 + j)) || null));
          for (let row = start; row <= r.r2; row++) for (let c = r.c1; c <= r.c2; c++) {
            const i = row - start;
            const cell = i < rows.length ? rows[i][c - r.c1] : null;
            tx.setCell(s, S.cellKey(row, c), cell && cell.f !== undefined ? { ...cell, f: S.shiftFormula(cell.f, row - keep[i], 0) } : cell);
          }
        });
        toast(`Removed ${removed} duplicate row${removed === 1 ? "" : "s"}; ${keep.length} left.`);
        return false;
      },
    });
  }
  function trimWhitespace() {
    const s = sheet();
    let n = 0;
    act("Trim whitespace", (tx) => {
      for (const [r, c] of selectedCells()) {
        const k = S.cellKey(r, c), cell = s.cells.get(k);
        if (!cell || typeof cell.v !== "string" || cell.f !== undefined) continue;
        const next = cell.v.replace(/\s+/g, " ").trim();
        if (next !== cell.v) { tx.setCell(s, k, S.makeCell(next, undefined, cell.s)); n++; }
      }
    });
    toast(`Trimmed ${n} cell${n === 1 ? "" : "s"}`);
  }
  function splitDialog() {
    const s = sheet(), d = doc();
    openDialog({
      title: "Split text to columns", submit: "Split",
      body: `${field("Separator", selectInput("sep", [["detect", "Detect automatically"], [",", "Comma"], [";", "Semicolon"], [".", "Period"], [" ", "Space"], ["\t", "Tab"], ["custom", "Custom"]], "detect"))}${field("Custom separator", textInput("custom", ""))}<p class="text-xs text-muted-foreground">Splits the first selected column into the columns to its right; existing values there are replaced.</p>`,
      onSubmit: (dialog) => {
        const v = formValues(dialog), range = clampToUsed(selectedRange());
        act("Split text", (tx) => {
          for (let r = range.r1; r <= range.r2; r++) {
            const value = d.calc.cellValue(s, r, range.c1);
            if (typeof value !== "string") continue;
            let sep = v.sep === "custom" ? v.custom : v.sep;
            if (sep === "detect") sep = [",", ";", "\t", "|", " "].find((x) => value.includes(x)) || ",";
            if (!sep) continue;
            value.split(sep).forEach((part, j) => setCellInput(tx, s, r, range.c1 + j, part.trim()));
          }
        });
        return false;
      },
    });
  }
  function addValidation(rule) {
    const s = sheet(), range = selectedRange();
    act(rule.type === "checkbox" ? "Insert checkboxes" : "Data validation", (tx) => {
      tx.set(s, "validations", [...s.validations.filter((v) => { const r = S.parseRange(v.range); return !r || !S.rangesOverlap(r, range); }), { id: S.uid(), range: S.rangeText(clampToUsed(range).r2 < range.r2 && range.r2 - range.r1 > 5000 ? clampToUsed(range) : range), ...rule }]);
      if (rule.type === "checkbox") for (const [r, c] of selectedCells(range, 50000)) { const k = S.cellKey(r, c), old = s.cells.get(k); if (!old || typeof old.v !== "boolean") tx.setCell(s, k, S.makeCell(false, undefined, old?.s)); }
    });
  }
  function validationDialog(preset) {
    const d = doc(), s = sheet();
    const existing = S.validationAt(s, d.sel.r, d.sel.c);
    const rule = existing || { type: preset || "list", values: [], strict: true, dropdown: true };
    openDialog({
      title: "Data validation", submit: "Save",
      body: `${field("Apply to range", textInput("range", existing ? existing.range : S.rangeText(selectedRange())))}
        ${field("Criteria", selectInput("type", [["list", "Dropdown from a list of items"], ["source", "Dropdown from a range"], ["checkbox", "Checkbox"], ["number", "Number"], ["date", "Date"], ["length", "Text length"], ["formula", "Custom formula is true"]], rule.source ? "source" : rule.type))}
        ${field("Items or range", textInput("values", rule.source ? rule.source : (rule.values || []).join(", ") || rule.formula || "", 'placeholder="Yes, No, Maybe  or  Sheet2!A1:A20  or  =A1>0"'), "Comma-separated items, a range, or a formula.")}
        <div class="sheets-form-grid">${field("Condition", selectInput("op", [["between", "between"], ["notbetween", "not between"], ["gt", "greater than"], ["ge", "greater than or equal"], ["lt", "less than"], ["le", "less than or equal"], ["eq", "equal to"], ["ne", "not equal to"]], rule.op || "between"))}${field("Minimum / value", textInput("min", rule.min ?? ""))}${field("Maximum", textInput("max", rule.max ?? ""))}${field("Help text", textInput("help", rule.help || ""))}</div>
        ${checkInput("strict", "Reject invalid input", rule.strict !== false)}${checkInput("dropdown", "Show a dropdown arrow", rule.dropdown !== false)}`,
      extra: existing ? [{ label: "Remove rule", left: true, danger: true, action: (dialog, close) => { act("Remove validation", (tx) => tx.set(s, "validations", s.validations.filter((v) => v !== existing))); close(); } }] : [],
      onSubmit: (dialog) => {
        const v = formValues(dialog);
        const range = S.parseRange(v.range.replace(/\$/g, ""));
        if (!range) { toast("Enter a range like B2:B50."); return true; }
        let next = { id: existing?.id || S.uid(), range: S.rangeText(range), strict: v.strict, help: v.help || undefined };
        if (v.type === "list") next = { ...next, type: "list", values: v.values.split(",").map((x) => x.trim()).filter(Boolean), dropdown: v.dropdown };
        else if (v.type === "source") next = { ...next, type: "list", source: v.values.replace(/^=/, ""), dropdown: v.dropdown };
        else if (v.type === "checkbox") next = { ...next, type: "checkbox" };
        else if (v.type === "formula") next = { ...next, type: "formula", formula: v.values.replace(/^=/, "") };
        else {
          const toNum = (text) => { const p = S.parseInput(text, inputOptions); return typeof p.value === "number" ? String(p.value) : text; };
          next = { ...next, type: v.type, op: v.op, min: toNum(v.min), max: toNum(v.max) };
        }
        act("Data validation", (tx) => {
          tx.set(s, "validations", [...s.validations.filter((x) => x !== existing && (() => { const r = S.parseRange(x.range); return !r || !S.rangesOverlap(r, range); })()), next]);
          if (next.type === "checkbox") for (const [r, c] of selectedCells(range, 50000)) { const k = S.cellKey(r, c), old = s.cells.get(k); if (!old || typeof old.v !== "boolean") tx.setCell(s, k, S.makeCell(false, undefined, old?.s)); }
        });
        return false;
      },
    });
  }
  async function noteDialog() {
    const d = doc(), s = sheet(), k = S.cellKey(d.sel.r, d.sel.c);
    const text = await promptDialog(`Note on ${S.addr(d.sel.r, d.sel.c)}`, "Note (leave empty to remove)", s.notes.get(k) || "", true);
    if (text === null) return;
    act(text.trim() ? "Edit note" : "Delete note", (tx) => { const next = new Map(s.notes); if (text.trim()) next.set(k, text.trim()); else next.delete(k); tx.set(s, "notes", next); });
  }
  function linkDialog() {
    const d = doc(), s = sheet(), k = S.cellKey(d.sel.r, d.sel.c);
    const current = s.links.get(k) || "";
    const label = S.displayText(d.wb, s, d.calc, d.sel.r, d.sel.c);
    openDialog({
      title: "Insert link", submit: "Apply",
      body: `${field("Text", textInput("text", label))}${field("Link", textInput("url", current, 'placeholder="https://… or #Sheet2!A1"'))}`,
      extra: current ? [{ label: "Remove link", left: true, action: (dialog, close) => { act("Remove link", (tx) => { const next = new Map(s.links); next.delete(k); tx.set(s, "links", next); }); close(); } }] : [],
      onSubmit: (dialog) => {
        const v = formValues(dialog);
        let url = v.url.trim();
        if (url && !/^(https?:|mailto:|#)/i.test(url)) url = /^[\w.-]+@[\w.-]+$/.test(url) ? "mailto:" + url : "https://" + url;
        act("Insert link", (tx) => {
          const next = new Map(s.links); if (url) next.set(k, url); else next.delete(k); tx.set(s, "links", next);
          if (v.text !== label) setCellInput(tx, s, d.sel.r, d.sel.c, v.text || url);
        });
        return false;
      },
    });
  }
  function openLink(url) {
    if (!url) return;
    if (url.startsWith("#")) {
      const target = url.slice(1);
      const bang = target.lastIndexOf("!");
      const name = bang > 0 ? target.slice(0, bang).replace(/^'|'$/g, "").replace(/''/g, "'") : sheet().name;
      const index = doc().wb.sheets.findIndex((s) => s.name.toLowerCase() === name.toLowerCase());
      const range = S.parseRange(bang > 0 ? target.slice(bang + 1) : target);
      if (index >= 0) switchSheet(index);
      if (range) selectRange(range);
      return;
    }
    if (/^(https?:|mailto:)/i.test(url)) window.open(url, "_blank", "noopener,noreferrer");
  }
  async function renameWorkbook() {
    const d = doc();
    const title = await promptDialog("Rename spreadsheet", "Name", d.title);
    if (title === null || !title.trim()) return;
    d.title = title.trim().slice(0, 120);
    q("[data-sheets-title]").value = d.title;
    markDirty();
  }
  async function makeCopy() {
    const d = doc();
    if (d.dirty) await save();
    const copy = S.deserializeWorkbook(S.serializeWorkbook(d.wb, d.calc));
    await createWorkbook(copy, `Copy of ${d.title}`.slice(0, 120));
    toast("Made a copy");
  }
  async function deleteWorkbook(id, title) {
    if (!(await confirmDialog("Delete spreadsheet", `Delete “${title}” from your account on every device? This cannot be undone. Download a copy first if you need one.`, "Delete"))) return;
    if (doc() && doc().id === id) { clearTimeout(doc().saveTimer); state.doc.dirty = false; }
    try {
      if (id) await request(`${API}/workbooks/${encodeURIComponent(id)}`, "DELETE");
      state.list = state.list.filter((item) => item.id !== id);
      if (doc() && (doc().id === id || !id)) { state.doc = null; q("[data-sheets-editor]").classList.add("hidden"); q("[data-sheets-list]").classList.remove("hidden"); history_replace(""); }
      renderList();
      flash("notice", `Deleted “${title}”.`);
    } catch (error) { toast(`Could not delete: ${error.message}`); }
  }
  function pickFiles(intoCurrent) {
    const input = q("[data-sheets-file]");
    input.dataset.into = intoCurrent ? "1" : "";
    input.value = "";
    input.click();
  }
  function printSheet() {
    const d = doc();
    recalc();
    const range = selectedRange();
    const useSelection = range.r1 !== range.r2 || range.c1 !== range.c2;
    const html = S.workbookHtml(d.wb, d.calc, d.title, useSelection ? { sheet: sheet(), range: clampToUsed(range) } : {});
    const frame = document.createElement("iframe");
    frame.className = "sheets-print-frame";
    frame.setAttribute("aria-hidden", "true");
    frame.srcdoc = html;
    frame.onload = () => { try { frame.contentWindow.focus(); frame.contentWindow.print(); } catch { download(html, safeFilename(d.title) + ".html", "text/html"); } setTimeout(() => frame.remove(), 60000); };
    document.body.appendChild(frame);
    toast(useSelection ? "Printing the selected cells. Choose “Save as PDF” to make a PDF." : "Printing every visible sheet. Choose “Save as PDF” to make a PDF.");
  }
  async function exportAs(kind) {
    const d = doc();
    if (d.editing) commitEdit();
    recalc();
    const name = safeFilename(d.title), s = sheet();
    try {
      if (kind === "xlsx") download(await S.writeXlsx(d.wb, d.calc), `${name}.xlsx`, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
      else if (kind === "xls") download(S.writeXls(d.wb, d.calc), `${name}.xls`, "application/vnd.ms-excel");
      else if (kind === "ods") download(await S.writeOds(d.wb, d.calc), `${name}.ods`, "application/vnd.oasis.opendocument.spreadsheet");
      else if (kind === "csv") download("﻿" + S.sheetToCsv(d.wb, s, d.calc, ","), `${name} - ${safeFilename(s.name)}.csv`, "text/csv;charset=utf-8");
      else if (kind === "tsv") download("﻿" + S.sheetToCsv(d.wb, s, d.calc, "\t"), `${name} - ${safeFilename(s.name)}.tsv`, "text/tab-separated-values;charset=utf-8");
      else if (kind === "html") download(S.workbookHtml(d.wb, d.calc, d.title), `${name}.html`, "text/html;charset=utf-8");
      else if (kind === "json") download(JSON.stringify(S.workbookJson(d.wb, d.calc), null, 2), `${name}.json`, "application/json");
      else if (kind === "md") download(S.sheetToMarkdown(d.wb, s, d.calc), `${name} - ${safeFilename(s.name)}.md`, "text/markdown;charset=utf-8");
      else if (kind === "pdf") printSheet();
      if (kind === "xls" && d.wb.sheets.some((x) => S.extentOf(x).r2 > 65535 || S.extentOf(x).c2 > 255)) toast("Excel 97–2003 holds 65,536 rows and 256 columns; cells beyond that were left out.");
    } catch (error) { toast(`Could not export: ${error.message}`); }
  }
  const CHART_COLORS = ["#4285F4", "#EA4335", "#FBBC04", "#34A853", "#FF6D01", "#46BDC6", "#7BAAF7", "#F07B72", "#FCD04F", "#71C287"];
  function chartSvg(data, chart, w, h) {
    const fg = grid.colors.fg, muted = grid.colors.muted, line = grid.colors.gridline;
    const title = chart.title ? `<text x="${w / 2}" y="20" text-anchor="middle" font-size="14" font-weight="600" fill="${fg}">${esc(chart.title)}</text>` : "";
    if (!data || !data.series.length || !data.categories.length) return `<svg viewBox="0 0 ${w} ${h}" width="${w}" height="${h}">${title}<text x="${w / 2}" y="${h / 2}" text-anchor="middle" font-size="12" fill="${muted}">No numbers to chart in ${esc(chart.range)}</text></svg>`;
    const top = chart.title ? 36 : 14, legendH = data.series.length > 1 || chart.type === "pie" ? 22 : 0;
    if (chart.type === "pie") {
      const values = data.series[0].values.map((v) => (typeof v === "number" && v > 0 ? v : 0));
      const total = values.reduce((a, b) => a + b, 0) || 1;
      const r = Math.max(10, Math.min(w, h - top - legendH) / 2 - 12), cx = w / 2, cy = top + (h - top - legendH) / 2;
      let angle = -Math.PI / 2;
      const slices = values.map((v, i) => {
        const a = (v / total) * Math.PI * 2;
        const x1 = cx + r * Math.cos(angle), y1 = cy + r * Math.sin(angle);
        angle += a;
        const x2 = cx + r * Math.cos(angle), y2 = cy + r * Math.sin(angle);
        const color = CHART_COLORS[i % CHART_COLORS.length];
        if (a >= Math.PI * 2 - 1e-9) return `<circle cx="${cx}" cy="${cy}" r="${r}" fill="${color}"><title>${esc(data.categories[i])}: ${v}</title></circle>`;
        return `<path d="M${cx},${cy} L${x1.toFixed(2)},${y1.toFixed(2)} A${r},${r} 0 ${a > Math.PI ? 1 : 0} 1 ${x2.toFixed(2)},${y2.toFixed(2)} Z" fill="${color}" stroke="${grid.colors.bg}" stroke-width="1.5"><title>${esc(data.categories[i])}: ${v} (${Math.round((v / total) * 100)}%)</title></path>`;
      }).join("");
      const legend = data.categories.slice(0, 10).map((c, i) => `<g transform="translate(${8 + (i % 5) * (w / 5)},${h - 14 - (i >= 5 ? 0 : 0)})"><rect width="9" height="9" rx="2" fill="${CHART_COLORS[i % CHART_COLORS.length]}"/><text x="13" y="8" font-size="10" fill="${muted}">${esc(String(c).slice(0, 14))}</text></g>`).join("");
      return `<svg viewBox="0 0 ${w} ${h}" width="${w}" height="${h}" role="img" aria-label="${esc(chart.title || "Pie chart")}">${title}${slices}${legend}</svg>`;
    }
    const all = data.series.flatMap((s) => s.values).filter((v) => typeof v === "number");
    let min = Math.min(0, ...all), max = Math.max(0, ...all);
    if (min === max) max = min + 1;
    const step = Math.pow(10, Math.floor(Math.log10((max - min) / 4 || 1)));
    const nice = [1, 2, 2.5, 5, 10].map((m) => m * step).find((s) => (max - min) / s <= 5) || step * 10;
    min = Math.floor(min / nice) * nice; max = Math.ceil(max / nice) * nice;
    const left = 46, right = 12, bottom = 26 + legendH, plotW = w - left - right, plotH = h - top - bottom;
    const horizontal = chart.type === "bar";
    const scale = (v) => (horizontal ? left + ((v - min) / (max - min)) * plotW : top + plotH - ((v - min) / (max - min)) * plotH);
    const ticks = [];
    for (let v = min; v <= max + nice / 2; v += nice) ticks.push(v);
    const label = (v) => S.formatValue(v, Math.abs(max) >= 10000 ? "#,##0" : "General").text;
    const axis = ticks.map((v) => horizontal
      ? `<line x1="${scale(v)}" x2="${scale(v)}" y1="${top}" y2="${top + plotH}" stroke="${line}"/><text x="${scale(v)}" y="${top + plotH + 14}" text-anchor="middle" font-size="10" fill="${muted}">${esc(label(v))}</text>`
      : `<line x1="${left}" x2="${w - right}" y1="${scale(v)}" y2="${scale(v)}" stroke="${line}"/><text x="${left - 6}" y="${scale(v) + 3}" text-anchor="end" font-size="10" fill="${muted}">${esc(label(v))}</text>`).join("");
    const n = data.categories.length;
    const band = (horizontal ? plotH : plotW) / n;
    const every = Math.max(1, Math.ceil(n / Math.max(1, Math.floor((horizontal ? plotH : plotW) / 48))));
    const cats = data.categories.map((c, i) => (i % every ? "" : horizontal
      ? `<text x="${left - 6}" y="${top + band * i + band / 2 + 3}" text-anchor="end" font-size="10" fill="${muted}">${esc(String(c).slice(0, 10))}</text>`
      : `<text x="${left + band * i + band / 2}" y="${top + plotH + 14}" text-anchor="middle" font-size="10" fill="${muted}">${esc(String(c).slice(0, 12))}</text>`)).join("");
    let marks = "";
    const zero = scale(Math.max(min, Math.min(max, 0)));
    if (chart.type === "column" || chart.type === "bar") {
      const k = data.series.length, gap = band * 0.2, bw = Math.max(1, (band - gap) / k);
      data.series.forEach((s, j) => s.values.forEach((v, i) => {
        if (typeof v !== "number") return;
        const color = CHART_COLORS[j % CHART_COLORS.length];
        const tip = `<title>${esc(s.name)} · ${esc(data.categories[i])}: ${esc(label(v))}</title>`;
        if (horizontal) { const y = top + band * i + gap / 2 + bw * j, x = Math.min(zero, scale(v)); marks += `<rect x="${x}" y="${y}" width="${Math.max(1, Math.abs(scale(v) - zero))}" height="${bw - 1}" rx="2" fill="${color}">${tip}</rect>`; }
        else { const x = left + band * i + gap / 2 + bw * j, y = Math.min(zero, scale(v)); marks += `<rect x="${x}" y="${y}" width="${bw - 1}" height="${Math.max(1, Math.abs(scale(v) - zero))}" rx="2" fill="${color}">${tip}</rect>`; }
      }));
    } else if (chart.type === "scatter") {
      const xs = data.categories.map((c, i) => S.parseInput(String(c)).value).map((v, i) => (typeof v === "number" ? v : i + 1));
      let xmin = Math.min(...xs), xmax = Math.max(...xs); if (xmin === xmax) xmax = xmin + 1;
      const sx = (v) => left + ((v - xmin) / (xmax - xmin)) * plotW;
      data.series.forEach((s, j) => s.values.forEach((v, i) => { if (typeof v === "number") marks += `<circle cx="${sx(xs[i])}" cy="${scale(v)}" r="4" fill="${CHART_COLORS[j % CHART_COLORS.length]}"><title>${esc(xs[i])}, ${esc(label(v))}</title></circle>`; }));
    } else {
      data.series.forEach((s, j) => {
        const color = CHART_COLORS[j % CHART_COLORS.length];
        const points = s.values.map((v, i) => (typeof v === "number" ? [left + band * i + band / 2, scale(v), v, i] : null)).filter(Boolean);
        if (!points.length) return;
        const path = points.map((p, i) => `${i ? "L" : "M"}${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(" ");
        if (chart.type === "area") marks += `<path d="${path} L${points[points.length - 1][0].toFixed(1)},${zero} L${points[0][0].toFixed(1)},${zero} Z" fill="${color}" opacity=".25"/>`;
        marks += `<path d="${path}" fill="none" stroke="${color}" stroke-width="2"/>` + points.map((p) => `<circle cx="${p[0]}" cy="${p[1]}" r="3" fill="${color}"><title>${esc(s.name)} · ${esc(data.categories[p[3]])}: ${esc(label(p[2]))}</title></circle>`).join("");
      });
    }
    const legend = legendH ? data.series.slice(0, 6).map((s, j) => `<g transform="translate(${left + j * Math.max(70, plotW / Math.min(6, data.series.length))},${h - 12})"><rect width="9" height="9" rx="2" fill="${CHART_COLORS[j % CHART_COLORS.length]}"/><text x="13" y="8" font-size="10" fill="${muted}">${esc(s.name.slice(0, 16))}</text></g>`).join("") : "";
    return `<svg viewBox="0 0 ${w} ${h}" width="${w}" height="${h}" role="img" aria-label="${esc(chart.title || "Chart")}">${title}${axis}${cats}${marks}${legend}</svg>`;
  }
  function chartDialog(existing = null) {
    const d = doc(), s = sheet();
    let range = selectedRange();
    if (range.r1 === range.r2 && range.c1 === range.c2) range = S.dataRegion(s, d.calc, d.sel.r, d.sel.c);
    const chart = existing || { type: "column", range: S.rangeText(clampToUsed(range)), title: "", headers: true, seriesBy: "cols" };
    openDialog({
      title: existing ? "Edit chart" : "Insert chart", submit: existing ? "Save" : "Insert",
      body: `<div class="sheets-form-grid">${field("Chart type", selectInput("type", [["column", "Column"], ["bar", "Bar"], ["line", "Line"], ["area", "Area"], ["pie", "Pie"], ["scatter", "Scatter"]], chart.type))}${field("Data range", textInput("range", chart.range))}${field("Title", textInput("title", chart.title))}${field("Series in", selectInput("seriesBy", [["cols", "Columns"], ["rows", "Rows"]], chart.seriesBy || "cols"))}</div>${checkInput("headers", "Use the first row as series names", chart.headers !== false)}<div class="sheets-chart-preview mt-3" data-chart-preview></div>`,
      onOpen: (dialog) => {
        const preview = () => {
          const v = formValues(dialog);
          const candidate = { ...chart, ...v, headers: v.headers };
          let data = null;
          try { data = S.chartData(d.calc, d.wb, s, candidate); } catch { data = null; }
          dialog.querySelector("[data-chart-preview]").innerHTML = chartSvg(data, candidate, 520, 260);
        };
        dialog.addEventListener("input", preview); dialog.addEventListener("change", preview); preview();
      },
      onSubmit: (dialog) => {
        const v = formValues(dialog);
        if (!S.formulaReferences(v.range).length) { toast("Enter a data range like A1:C12."); return true; }
        const next = { ...chart, type: v.type, range: v.range.replace(/^=/, ""), title: v.title, headers: v.headers, seriesBy: v.seriesBy };
        if (!existing) { const ref = S.formulaReferences(next.range)[0]; Object.assign(next, { id: S.uid(), anchor: { r: ref.r1, c: Math.min(s.colCount - 1, ref.c2 + 2) }, w: 480, h: 300 }); }
        act(existing ? "Edit chart" : "Insert chart", (tx) => tx.set(s, "charts", existing ? s.charts.map((c) => (c.id === existing.id ? next : c)) : [...s.charts, next]));
        return false;
      },
    });
  }
  function renderCharts() {
    const d = doc(), layer = grid.charts;
    if (!d || !layer) return;
    const s = sheet();
    layer.innerHTML = s.charts.map((chart) => `<div class="sheets-chart" data-chart="${esc(chart.id)}"><div class="sheets-chart-bar" data-chart-drag><span>${esc(chart.title || chart.type[0].toUpperCase() + chart.type.slice(1) + " chart")}</span><button type="button" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon" data-chart-menu aria-label="Chart options"><i data-lucide="ellipsis-vertical"></i></button></div><div class="sheets-chart-body" data-chart-body></div><span class="sheets-chart-resize" data-chart-resize></span></div>`).join("");
    for (const chart of s.charts) {
      const el = layer.querySelector(`[data-chart="${CSS.escape(chart.id)}"]`);
      let data = null;
      try { data = S.chartData(d.calc, d.wb, s, chart); } catch { data = null; }
      el.querySelector("[data-chart-body]").innerHTML = chartSvg(data, { ...chart, title: "" }, chart.w * d.zoom, chart.h * d.zoom - 30);
    }
    icons();
    positionCharts();
  }
  function positionCharts() {
    const d = doc(), layer = grid.charts;
    if (!d || !layer || !layer.children.length) return;
    const s = sheet(), z = d.zoom, { rows, cols } = axes();
    layer.style.left = headerWidth() + "px";
    layer.style.top = HEADER_H + "px";
    layer.style.width = grid.width - headerWidth() + "px";
    layer.style.height = grid.height - HEADER_H + "px";
    for (const chart of s.charts) {
      const el = layer.querySelector(`[data-chart="${CSS.escape(chart.id)}"]`);
      if (!el) continue;
      el.style.left = cols.offset(chart.anchor.c) * z + (chart.anchor.dx || 0) * z - grid.scroller.scrollLeft + "px";
      el.style.top = rows.offset(chart.anchor.r) * z + (chart.anchor.dy || 0) * z - grid.scroller.scrollTop + "px";
      el.style.width = chart.w * z + "px";
      el.style.height = chart.h * z + "px";
    }
  }
  function chartPointer(event) {
    const el = event.target.closest("[data-chart]");
    if (!el) return;
    const s = sheet(), chart = s.charts.find((c) => c.id === el.dataset.chart);
    if (!chart) return;
    if (event.target.closest("[data-chart-menu]")) {
      openMenu(event.target.closest("[data-chart-menu]").getBoundingClientRect(), [
        { label: "Edit chart…", icon: "pencil", action: () => chartDialog(chart) },
        { label: "Download as SVG", icon: "download", action: () => { const svg = el.querySelector("svg"); download(new Blob([svg.outerHTML.replace("<svg", '<svg xmlns="http://www.w3.org/2000/svg"')], { type: "image/svg+xml" }), safeFilename(chart.title || "chart") + ".svg"); } },
        { label: "Delete chart", icon: "trash-2", danger: true, action: () => act("Delete chart", (tx) => tx.set(s, "charts", s.charts.filter((c) => c.id !== chart.id))) },
      ]);
      return;
    }
    const resizing = !!event.target.closest("[data-chart-resize]");
    if (!resizing && !event.target.closest("[data-chart-drag]") && !event.target.closest("[data-chart-body]")) return;
    event.preventDefault();
    const start = { x: event.clientX, y: event.clientY, left: parseFloat(el.style.left), top: parseFloat(el.style.top), w: chart.w, h: chart.h };
    const z = doc().zoom;
    const move = (e) => {
      const dx = e.clientX - start.x, dy = e.clientY - start.y;
      if (resizing) { el.style.width = Math.max(160, start.w * z + dx) + "px"; el.style.height = Math.max(120, start.h * z + dy) + "px"; }
      else { el.style.left = start.left + dx + "px"; el.style.top = start.top + dy + "px"; }
    };
    const up = (e) => {
      window.removeEventListener("mousemove", move); window.removeEventListener("mouseup", up);
      const dx = e.clientX - start.x, dy = e.clientY - start.y;
      if (Math.abs(dx) < 2 && Math.abs(dy) < 2) return;
      if (resizing) act("Resize chart", (tx) => tx.set(s, "charts", s.charts.map((c) => (c.id === chart.id ? { ...c, w: Math.round(Math.max(160, start.w * z + dx) / z), h: Math.round(Math.max(120, start.h * z + dy) / z) } : c))));
      else {
        const { rows, cols } = axes();
        const px = (start.left + dx + grid.scroller.scrollLeft) / z, py = (start.top + dy + grid.scroller.scrollTop) / z;
        const c = cols.index(Math.max(0, px)), r = rows.index(Math.max(0, py));
        act("Move chart", (tx) => tx.set(s, "charts", s.charts.map((x) => (x.id === chart.id ? { ...x, anchor: { r, c, dx: Math.max(0, Math.round(px - cols.offset(c))), dy: Math.max(0, Math.round(py - rows.offset(r))) } } : x))));
      }
    };
    window.addEventListener("mousemove", move); window.addEventListener("mouseup", up);
  }

  function closeAutocomplete() { const el = q("[data-sheets-autocomplete]"); if (el) { el.classList.add("hidden"); el.innerHTML = ""; } grid.suggestions = null; }
  function renderAutocomplete() {
    const d = doc(), el = q("[data-sheets-autocomplete]");
    if (!d?.editing || !el || !d.editing.text.startsWith("=")) { closeAutocomplete(); return; }
    const input = editorText();
    const before = input.value.slice(0, input.selectionStart ?? input.value.length);
    if ((before.match(/"/g) || []).length % 2) { closeAutocomplete(); return; }
    const token = /(?:^=|[^A-Za-z0-9_.$!"'])([A-Za-z][A-Za-z0-9.]*)$/.exec(before);
    const COMMON = ["SUM", "AVERAGE", "COUNT", "IF", "VLOOKUP", "XLOOKUP", "MAX", "MIN", "ROUND", "SUMIF", "COUNTIF", "IFERROR", "INDEX", "MATCH", "CONCATENATE", "TODAY", "TEXT", "LEFT", "LEN", "AND", "OR"];
    const rank = (n) => (COMMON.includes(n) ? COMMON.indexOf(n) : 100 + n.length);
    let names = token ? S.FUNCTION_NAMES.filter((n) => n.startsWith(token[1].toUpperCase())).sort((a, b) => rank(a) - rank(b) || a.localeCompare(b)).slice(0, 8) : [];
    if (token && names.length === 1 && names[0] === token[1].toUpperCase() && /\($/.test(before)) names = [];
    let depth = 0, fn = "";
    for (let i = before.length - 1; i >= 0; i--) {
      const ch = before[i];
      if (ch === ")") depth++;
      else if (ch === "(") { if (depth === 0) { const m = /([A-Za-z][A-Za-z0-9.]*)$/.exec(before.slice(0, i)); fn = m ? m[1].toUpperCase() : ""; break; } depth--; }
    }
    if (!names.length && !fn) { closeAutocomplete(); return; }
    grid.suggestions = names.length ? { names, index: grid.suggestions?.index && grid.suggestions.index < names.length ? grid.suggestions.index : 0, token: token[1] } : null;
    el.innerHTML = names.length
      ? names.map((n, i) => `<button type="button" class="fm-menu-item" data-suggest="${n}"${i === grid.suggestions.index ? ' data-active="true"' : ""}><strong>${n}</strong><span class="fm-menu-hint">${esc((FUNCTION_HELP[n] || "").replace(/^[^—]*— ?/, ""))}</span></button>`).join("")
      : `<div class="px-3 py-2 text-xs">${esc(FUNCTION_HELP[fn] || (S.FUNCTIONS[fn] ? `${fn}(…)` : ""))}</div>`;
    if (!el.innerHTML.trim() || el.textContent.trim() === "") { closeAutocomplete(); return; }
    el.classList.remove("hidden");
    const anchor = input.getBoundingClientRect(), host = root.getBoundingClientRect();
    el.style.left = anchor.left - host.left + "px";
    el.style.top = anchor.bottom - host.top + 2 + "px";
  }
  function acceptSuggestion(name) {
    const input = editorText();
    const caret = input.selectionStart;
    const before = input.value.slice(0, caret), after = input.value.slice(caret);
    const start = before.length - (grid.suggestions?.token.length || 0);
    input.value = before.slice(0, start) + name + "(" + after;
    const pos = start + name.length + 1;
    input.setSelectionRange(pos, pos);
    syncEditing(input);
  }

  function openFilterMenu(c, rect) {
    const d = doc(), s = sheet(), range = s.filter.range;
    const counts = new Map();
    for (let r = range.r1 + 1; r <= Math.min(range.r2, range.r1 + 100000); r++) { const text = S.displayText(d.wb, s, d.calc, r, c); counts.set(text, (counts.get(text) || 0) + 1); }
    const values = [...counts.keys()].sort((a, b) => S.compareValues(S.parseInput(a).value ?? a, S.parseInput(b).value ?? b)).slice(0, 2000);
    const current = s.filter.criteria.get(c);
    const hidden = current?.hidden || new Set();
    openPanel({ left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom }, `<div class="fm-menu"><button type="button" class="fm-menu-item" data-filter-sort="asc"><i data-lucide="arrow-down-a-z"></i>Sort A → Z</button><button type="button" class="fm-menu-item" data-filter-sort="desc"><i data-lucide="arrow-up-z-a"></i>Sort Z → A</button><div class="fm-menu-divider"></div></div><div class="px-2 pb-2"><input class="sheets-input" data-filter-search placeholder="Search values"><div class="mt-1 flex gap-2 text-xs"><button type="button" class="text-accent" data-filter-all>Select all</button><button type="button" class="text-accent" data-filter-none>Clear</button></div><div class="sheets-filter-list" data-filter-list>${values.map((v, i) => `<label class="sheets-check" data-filter-value="${esc(v.toLowerCase())}"><input type="checkbox" data-filter-index="${i}"${hidden.has(v) ? "" : " checked"}> ${esc(v === "" ? "(Blanks)" : v)} <span class="text-muted-foreground">${counts.get(v)}</span></label>`).join("")}</div></div><div class="fm-popover-footer flex justify-end gap-2"><button type="button" class="fm-btn fm-btn-sm" data-filter-cancel>Cancel</button><button type="button" class="fm-btn fm-btn-sm" data-filter-ok>OK</button></div>`, (el) => {
      el.querySelector("[data-filter-search]").addEventListener("input", (event) => { const term = event.target.value.toLowerCase(); el.querySelectorAll("[data-filter-value]").forEach((row) => row.classList.toggle("hidden", !!term && !row.dataset.filterValue.includes(term))); });
      el.addEventListener("click", (event) => {
        if (event.target.closest("[data-filter-all]")) el.querySelectorAll("[data-filter-index]").forEach((box) => { if (!box.closest(".hidden")) box.checked = true; });
        if (event.target.closest("[data-filter-none]")) el.querySelectorAll("[data-filter-index]").forEach((box) => { if (!box.closest(".hidden")) box.checked = false; });
        if (event.target.closest("[data-filter-cancel]")) closePopovers();
        const sortButton = event.target.closest("[data-filter-sort]");
        if (sortButton) { closePopovers(); act("Sort", (tx) => S.sortRows(tx, d.wb, d.calc, s, { ...range, r1: range.r1 + 1 }, [{ col: c, desc: sortButton.dataset.filterSort === "desc" }])); }
        if (event.target.closest("[data-filter-ok]")) {
          const off = new Set();
          el.querySelectorAll("[data-filter-index]").forEach((box) => { if (!box.checked) off.add(values[Number(box.dataset.filterIndex)]); });
          closePopovers();
          act("Filter", (tx) => { const criteria = new Map(s.filter.criteria); if (off.size) criteria.set(c, { hidden: off }); else criteria.delete(c); tx.set(s, "filter", { ...s.filter, criteria }); });
        }
      });
    });
  }
  function openDropdown(r, c, rect) {
    const d = doc(), s = sheet(), rule = S.validationAt(s, r, c);
    const options = S.validationOptions(d.calc, s, rule);
    const current = S.valueToText(d.calc.cellValue(s, r, c));
    openMenu({ left: rect.left, right: rect.right, top: rect.top, bottom: rect.bottom }, options.length ? [...options.slice(0, 200).map((option) => ({ label: option, checked: option === current, action: () => act("Choose value", (tx) => setCellInput(tx, s, r, c, option)) })), "-", { label: "Clear", action: () => act("Clear", (tx) => { const old = s.cells.get(S.cellKey(r, c)); tx.setCell(s, S.cellKey(r, c), S.makeCell(undefined, undefined, old?.s)); }) }] : [{ label: "No items in this list", disabled: true }]);
  }
  function contextMenu(hit, event) {
    const d = doc(), s = sheet(), rect = { left: event.clientX, right: event.clientX, top: event.clientY, bottom: event.clientY };
    const sel = selectedRange();
    const n = (a, b) => (b - a + 1 > 1 ? ` ${b - a + 1}` : "");
    if (hit.area === "colHeader") {
      openMenu(rect, [{ label: "Cut", icon: "scissors", action: () => copySelection(null, true) }, { label: "Copy", icon: "copy", action: () => copySelection(null, false) }, { label: "Paste", icon: "clipboard", action: () => pasteFromMenu("all") }, "-",
        { label: `Insert${n(sel.c1, sel.c2)} column${sel.c2 > sel.c1 ? "s" : ""} left`, icon: "arrow-left-to-line", action: () => structural("insertCols", true) }, { label: `Insert${n(sel.c1, sel.c2)} column${sel.c2 > sel.c1 ? "s" : ""} right`, icon: "arrow-right-to-line", action: () => structural("insertCols", false) },
        { label: `Delete column${sel.c2 > sel.c1 ? "s" : ""} ${S.colName(sel.c1)}${sel.c2 > sel.c1 ? "–" + S.colName(sel.c2) : ""}`, icon: "trash-2", action: () => structural("deleteCols") }, { label: "Clear column", icon: "eraser", action: () => clearRange("values") }, "-",
        { label: "Hide column", icon: "eye-off", action: () => setHidden("col", true) }, { label: "Unhide columns", icon: "eye", action: () => setHidden("col", false) }, { label: "Resize column…", action: () => sizeDialog("col") }, { label: "Fit to data", action: () => fitSelection("col") }, "-",
        { label: "Sort sheet A → Z", icon: "arrow-down-a-z", action: () => sortSelection(false, true) }, { label: "Sort sheet Z → A", icon: "arrow-up-z-a", action: () => sortSelection(true, true) }, { label: s.filter ? "Remove filter" : "Create a filter", icon: "filter", action: toggleFilter }]);
      return;
    }
    if (hit.area === "rowHeader") {
      openMenu(rect, [{ label: "Cut", icon: "scissors", action: () => copySelection(null, true) }, { label: "Copy", icon: "copy", action: () => copySelection(null, false) }, { label: "Paste", icon: "clipboard", action: () => pasteFromMenu("all") }, "-",
        { label: `Insert${n(sel.r1, sel.r2)} row${sel.r2 > sel.r1 ? "s" : ""} above`, icon: "arrow-up-to-line", action: () => structural("insertRows", true) }, { label: `Insert${n(sel.r1, sel.r2)} row${sel.r2 > sel.r1 ? "s" : ""} below`, icon: "arrow-down-to-line", action: () => structural("insertRows", false) },
        { label: `Delete row${sel.r2 > sel.r1 ? "s" : ""} ${sel.r1 + 1}${sel.r2 > sel.r1 ? "–" + (sel.r2 + 1) : ""}`, icon: "trash-2", action: () => structural("deleteRows") }, { label: "Clear row", icon: "eraser", action: () => clearRange("values") }, "-",
        { label: "Hide row", icon: "eye-off", action: () => setHidden("row", true) }, { label: "Unhide rows", icon: "eye", action: () => setHidden("row", false) }, { label: "Resize row…", action: () => sizeDialog("row") }, { label: "Fit to data", action: () => fitSelection("row") }]);
      return;
    }
    const link = s.links.get(S.cellKey(d.sel.r, d.sel.c)) || (() => { const v = d.calc.cellValue(s, d.sel.r, d.sel.c); return typeof v === "string" && /^https?:\/\/\S+$/i.test(v) ? v : ""; })();
    openMenu(rect, [{ label: "Cut", icon: "scissors", hint: "Ctrl+X", action: () => copySelection(null, true) }, { label: "Copy", icon: "copy", hint: "Ctrl+C", action: () => copySelection(null, false) }, { label: "Paste", icon: "clipboard", hint: "Ctrl+V", action: () => pasteFromMenu("all") },
      { label: "Paste special", icon: "clipboard-paste", submenu: () => [{ label: "Values only", action: () => pasteFromMenu("values") }, { label: "Format only", action: () => pasteFromMenu("formats") }, { label: "Transposed", action: () => pasteFromMenu("transpose") }] }, "-",
      { label: "Insert row above", icon: "arrow-up-to-line", action: () => structural("insertRows", true) }, { label: "Insert column left", icon: "arrow-left-to-line", action: () => structural("insertCols", true) },
      { label: "Delete row", icon: "trash-2", action: () => structural("deleteRows") }, { label: "Delete column", icon: "trash-2", action: () => structural("deleteCols") }, { label: "Delete values", icon: "eraser", hint: "Delete", action: () => clearRange("values") }, "-",
      ...(link ? [{ label: "Open link", icon: "external-link", action: () => openLink(link) }] : []),
      { label: "Insert link…", icon: "link", action: linkDialog }, { label: "Insert note…", icon: "sticky-note", action: noteDialog }, { label: "Insert chart…", icon: "chart-column", action: () => chartDialog() }, "-",
      { label: "Conditional formatting…", icon: "paint-bucket", action: conditionalDialog }, { label: "Data validation…", icon: "list-checks", action: () => validationDialog() }, { label: "Ask AI about this", icon: "sparkles", action: () => { toggleAi(true); q("[data-sheets-ai-input]").focus(); } }]);
  }

  let drag = null;
  function pointerAt(event) { const r = grid.host.getBoundingClientRect(); return { x: event.clientX - r.left, y: event.clientY - r.top }; }
  function onMouseDown(event) {
    const d = doc();
    if (!d || event.button > 1) return;
    const { x, y } = pointerAt(event);
    if (x >= grid.width || y >= grid.height) return;
    closePopovers();
    const s = sheet();
    const hit = hitTest(x, y);
    event.preventDefault();
    if (d.editing && hit.area === "cell" && inPointMode()) {
      insertReference({ r1: hit.r, c1: hit.c, r2: hit.r, c2: hit.c }, d.wb.active);
      drag = { type: "point", anchor: { r: hit.r, c: hit.c } };
      return;
    }
    if (d.editing && !commitEdit()) return;
    focusGrid();
    if (hit.area === "cell") {
      const rect = cellRect(hit.r, hit.c);
      if (s.filter && hit.r === s.filter.range.r1 && hit.c >= s.filter.range.c1 && hit.c <= s.filter.range.c2 && x >= rect.x + rect.w - 20) {
        setSelection(hit.r, hit.c);
        const host = grid.host.getBoundingClientRect();
        openFilterMenu(hit.c, { left: host.left + rect.x + rect.w - 20, right: host.left + rect.x + rect.w, top: host.top + rect.y, bottom: host.top + rect.y + rect.h });
        return;
      }
      const rule = S.validationAt(s, hit.r, hit.c);
      if (rule && rule.type === "checkbox" && !event.shiftKey) { setSelection(hit.r, hit.c); toggleCheckbox(); return; }
      if (rule && rule.type === "list" && rule.dropdown !== false && x >= rect.x + rect.w - 20 && !event.shiftKey) {
        setSelection(hit.r, hit.c);
        const host = grid.host.getBoundingClientRect();
        openDropdown(hit.r, hit.c, { left: host.left + rect.x, right: host.left + rect.x + rect.w, top: host.top + rect.y, bottom: host.top + rect.y + rect.h });
        return;
      }
      if ((event.ctrlKey || event.metaKey) && !event.shiftKey) {
        const link = s.links.get(S.cellKey(hit.r, hit.c)) || (() => { const v = d.calc.cellValue(s, hit.r, hit.c); return typeof v === "string" && /^https?:\/\/\S+$/i.test(v) ? v : ""; })();
        if (link) { openLink(link); return; }
      }
      setSelection(hit.r, hit.c, event.shiftKey);
      drag = { type: "cells" };
    } else if (hit.area === "handle") drag = { type: "fill", source: selectedRange() };
    else if (hit.area === "colHeader") {
      if (event.shiftKey) { const a = d.anchor.c; selectRange({ r1: 0, c1: Math.min(a, hit.c), r2: s.rowCount - 1, c2: Math.max(a, hit.c) }, { r: d.sel.r, c: d.sel.c }); }
      else if (!(d.sel.r1 === 0 && d.sel.r2 >= s.rowCount - 1 && hit.c >= d.sel.c1 && hit.c <= d.sel.c2 && event.button === 0 && false)) { selectRange({ r1: 0, c1: hit.c, r2: s.rowCount - 1, c2: hit.c }, { r: Math.max(grid.firstVisibleRow ?? 0, 0), c: hit.c }); d.anchor = { r: 0, c: hit.c }; }
      drag = { type: "cols", anchor: d.anchor.c };
    } else if (hit.area === "rowHeader") {
      if (event.shiftKey) { const a = d.anchor.r; selectRange({ r1: Math.min(a, hit.r), c1: 0, r2: Math.max(a, hit.r), c2: s.colCount - 1 }, { r: d.sel.r, c: d.sel.c }); }
      else { selectRange({ r1: hit.r, c1: 0, r2: hit.r, c2: s.colCount - 1 }, { r: hit.r, c: 0 }); d.anchor = { r: hit.r, c: 0 }; }
      drag = { type: "rows", anchor: d.anchor.r };
    } else if (hit.area === "colEdge") drag = { type: "colResize", index: hit.c, start: x, size: axes().cols.size(hit.c) / d.zoom || s.defaultColWidth };
    else if (hit.area === "rowEdge") drag = { type: "rowResize", index: hit.r, start: y, size: axes().rows.size(hit.r) / d.zoom || s.defaultRowHeight };
    else if (hit.area === "corner") selectRange({ r1: 0, c1: 0, r2: s.rowCount - 1, c2: s.colCount - 1 }, { r: 0, c: 0 });
    if (drag) { drag.lastEvent = event; drag.timer = setInterval(() => { if (drag?.lastEvent) onMouseMove(drag.lastEvent, true); }, 60); }
  }
  function onMouseMove(event, auto = false) {
    if (!grid.host || !doc()) return;
    const { x, y } = pointerAt(event);
    if (!drag) {
      if (event.target !== grid.scroller && !grid.scroller.contains(event.target)) return;
      const hit = x < grid.width && y < grid.height ? hitTest(x, y) : null;
      grid.scroller.style.cursor = !hit ? "" : hit.area === "colEdge" ? "col-resize" : hit.area === "rowEdge" ? "row-resize" : hit.area === "handle" ? "crosshair" : hit.area === "cell" ? "cell" : "default";
      return;
    }
    drag.lastEvent = event;
    const d = doc(), s = sheet();
    if (!auto) {
      const edge = 24;
      grid.autoScroll = { dx: x > grid.width - edge ? 24 : x < headerWidth() ? -24 : 0, dy: y > grid.height - edge ? 24 : y < HEADER_H ? -24 : 0 };
    }
    if (auto && grid.autoScroll && (drag.type === "cells" || drag.type === "fill" || drag.type === "point" || drag.type === "rows" || drag.type === "cols")) {
      grid.scroller.scrollLeft += grid.autoScroll.dx; grid.scroller.scrollTop += grid.autoScroll.dy;
    }
    const clampX = Math.max(headerWidth() + 1, Math.min(grid.width - 1, x)), clampY = Math.max(HEADER_H + 1, Math.min(grid.height - 1, y));
    const hit = hitTest(clampX, clampY);
    if (drag.type === "cells" && hit.area !== "colHeader") setSelection(hit.r, hit.c, true, { noScroll: true });
    else if (drag.type === "point") {
      const range = S.normRange({ r1: drag.anchor.r, c1: drag.anchor.c, r2: hit.r, c2: hit.c });
      insertReference(range, d.wb.active);
    } else if (drag.type === "fill") {
      const src = drag.source;
      const dr = hit.r > src.r2 ? hit.r - src.r2 : hit.r < src.r1 ? hit.r - src.r1 : 0;
      const dc = hit.c > src.c2 ? hit.c - src.c2 : hit.c < src.c1 ? hit.c - src.c1 : 0;
      if (Math.abs(dr) >= Math.abs(dc)) d.fillPreview = dr > 0 ? { ...src, r2: hit.r } : dr < 0 ? { ...src, r1: hit.r } : null;
      else d.fillPreview = dc > 0 ? { ...src, c2: hit.c } : { ...src, c1: hit.c };
      draw();
    } else if (drag.type === "cols") selectRange({ r1: 0, c1: Math.min(drag.anchor, hit.c), r2: s.rowCount - 1, c2: Math.max(drag.anchor, hit.c) }, { r: d.sel.r, c: d.sel.c });
    else if (drag.type === "rows") selectRange({ r1: Math.min(drag.anchor, hit.r), c1: 0, r2: Math.max(drag.anchor, hit.r), c2: s.colCount - 1 }, { r: d.sel.r, c: d.sel.c });
    else if (drag.type === "colResize" || drag.type === "rowResize") {
      const delta = (drag.type === "colResize" ? x - drag.start : y - drag.start) / d.zoom;
      drag.next = Math.max(drag.type === "colResize" ? 8 : 6, Math.round(drag.size + delta));
      const prop = drag.type === "colResize" ? "colWidths" : "rowHeights";
      if (!drag.original) drag.original = new Map(s[prop]);
      s[prop].set(drag.index, drag.next);
      d.layoutVersion++;
      updateSizer(); renderCharts(); draw();
    }
  }
  function onMouseUp(event) {
    if (!drag) return;
    const d = doc(), s = sheet(), done = drag;
    clearInterval(done.timer);
    drag = null; grid.autoScroll = null;
    if (done.type === "fill") { const target = d.fillPreview; d.fillPreview = null; if (target) fillTo(done.source, target); else draw(); }
    if ((done.type === "colResize" || done.type === "rowResize") && done.next !== undefined) {
      const prop = done.type === "colResize" ? "colWidths" : "rowHeights";
      const next = new Map(s[prop]);
      s[prop] = done.original;
      const sel = selectedRange();
      const multi = done.type === "colResize" ? sel.r1 === 0 && sel.r2 >= s.rowCount - 1 && done.index >= sel.c1 && done.index <= sel.c2 : sel.c1 === 0 && sel.c2 >= s.colCount - 1 && done.index >= sel.r1 && done.index <= sel.r2;
      if (multi) for (let i = done.type === "colResize" ? sel.c1 : sel.r1; i <= (done.type === "colResize" ? sel.c2 : sel.r2); i++) next.set(i, done.next);
      act(done.type === "colResize" ? "Resize column" : "Resize row", (tx) => tx.set(s, prop, next));
    }
    if (d.painter && done.type === "cells") {
      const style = d.painter.style;
      d.painter = null;
      act("Paint format", (tx) => styleRange(tx, s, selectedRange(), () => ({ ...style })));
      renderToolbarState();
    }
    if (done.type === "point") editorText().focus({ preventScroll: true });
  }
  function onDoubleClick(event) {
    const d = doc();
    if (!d) return;
    const { x, y } = pointerAt(event);
    const hit = hitTest(x, y);
    if (hit.area === "colEdge") { const width = autoFit("col", hit.c); act("Fit column", (tx) => { const next = new Map(sheet().colWidths); next.set(hit.c, width); tx.set(sheet(), "colWidths", next); }); return; }
    if (hit.area === "rowEdge") { const height = autoFit("row", hit.r); act("Fit row", (tx) => { const next = new Map(sheet().rowHeights); next.set(hit.r, height); tx.set(sheet(), "rowHeights", next); }); return; }
    if (hit.area === "cell" && !d.editing) { setSelection(hit.r, hit.c); const rule = S.validationAt(sheet(), hit.r, hit.c); if (!(rule && rule.type === "checkbox")) startEdit(undefined, "edit"); }
  }

  function onKeyDown(event) {
    const d = doc();
    if (!d) return;
    const mod = event.ctrlKey || event.metaKey, key = event.key, s = sheet();
    if (d.editing) {
      const input = editorText();
      if (grid.suggestions && ["ArrowDown", "ArrowUp", "Tab", "Enter"].includes(key) && !event.shiftKey && !mod && !event.altKey) {
        event.preventDefault();
        if (key === "ArrowDown") grid.suggestions.index = (grid.suggestions.index + 1) % grid.suggestions.names.length;
        else if (key === "ArrowUp") grid.suggestions.index = (grid.suggestions.index - 1 + grid.suggestions.names.length) % grid.suggestions.names.length;
        else { acceptSuggestion(grid.suggestions.names[grid.suggestions.index]); return; }
        renderAutocomplete();
        return;
      }
      if (key === "Escape") { event.preventDefault(); cancelEdit(); return; }
      if (key === "Enter" && event.altKey) {
        event.preventDefault();
        const pos = input.selectionStart;
        input.value = input.value.slice(0, pos) + "\n" + input.value.slice(input.selectionEnd);
        input.setSelectionRange(pos + 1, pos + 1);
        syncEditing(input);
        return;
      }
      if (key === "Enter") { event.preventDefault(); if (mod) d.fillSelection = true; commitEdit(mod ? null : [event.shiftKey ? -1 : 1, 0]); return; }
      if (key === "Tab") { event.preventDefault(); commitEdit([0, event.shiftKey ? -1 : 1]); return; }
      if (key === "F4") {
        event.preventDefault();
        const caret = input.selectionStart;
        const ref = S.referenceTokens(input.value).find((t) => caret >= t.start && caret <= t.end);
        if (ref) {
          const body = ref.text.replace(/^.*!/, "");
          const prefix = ref.text.slice(0, ref.text.length - body.length);
          const toggled = prefix + body.split(":").map(S.toggleAbsolute).join(":");
          input.value = input.value.slice(0, ref.start) + toggled + input.value.slice(ref.end);
          const pos = ref.start + toggled.length;
          input.setSelectionRange(pos, pos);
          syncEditing(input);
        }
        return;
      }
      if (key.startsWith("Arrow") && d.editing.mode === "enter" && !event.shiftKey && !mod) {
        if (inPointMode()) {
          event.preventDefault();
          const last = d.editing.point ? S.parseRange(input.value.slice(d.editing.point.start, d.editing.point.end).replace(/^.*!/, "")) : null;
          const base = last ? { r: last.r1, c: last.c1 } : { r: d.editing.r, c: d.editing.c };
          const step = { ArrowDown: [1, 0], ArrowUp: [-1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1] }[key];
          const r = Math.max(0, base.r + step[0]), c = Math.max(0, base.c + step[1]);
          insertReference({ r1: r, c1: c, r2: r, c2: c }, d.wb.active);
          return;
        }
        event.preventDefault();
        const step = { ArrowDown: [1, 0], ArrowUp: [-1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1] }[key];
        commitEdit(step);
        return;
      }
      return;
    }
    const sel = selectedRange();
    if (mod && !event.altKey) {
      const lower = key.toLowerCase();
      const run = (fn) => { event.preventDefault(); fn(); };
      if (lower === "z" && !event.shiftKey) return run(() => history("undo"));
      if (lower === "y" || (lower === "z" && event.shiftKey)) return run(() => history("redo"));
      if (lower === "b") return run(() => toggleStyle("b", "Bold"));
      if (lower === "i") return run(() => toggleStyle("i", "Italic"));
      if (lower === "u") return run(() => toggleStyle("u", "Underline"));
      if (key === "5" && !event.shiftKey) return run(() => toggleStyle("s", "Strikethrough"));
      if (lower === "a") return run(() => selectRange({ r1: 0, c1: 0, r2: s.rowCount - 1, c2: s.colCount - 1 }, { r: d.sel.r, c: d.sel.c }));
      if (lower === "f") return run(() => findDialog(false));
      if (lower === "h") return run(() => findDialog(true));
      if (lower === "s") return run(() => { save(true); toast("Saved"); });
      if (lower === "p") return run(() => printSheet());
      if (lower === "d") return run(() => fillDirection("down"));
      if (lower === "r") return run(() => fillDirection("right"));
      if (lower === "k") return run(linkDialog);
      if (key === "`") return run(() => { d.showFormulas = !d.showFormulas; draw(); });
      if (key === "/") return run(shortcutsDialog);
      if (key === "\\") return run(() => clearRange("formats"));
      if (key === ";" || key === ":") return run(() => insertNow(event.shiftKey ? "time" : "date"));
      if (event.shiftKey && (key === "$" || key === "4")) return run(() => applyStyle({ fmt: '"$"#,##0.00' }, "Currency"));
      if (event.shiftKey && (key === "%" || key === "5")) return run(() => applyStyle({ fmt: "0%" }, "Percent"));
      if (event.shiftKey && (key === "!" || key === "1")) return run(() => applyStyle({ fmt: "#,##0.00" }, "Number format"));
      if (key === " ") return run(() => selectRange({ r1: 0, c1: sel.c1, r2: s.rowCount - 1, c2: sel.c2 }, { r: d.sel.r, c: d.sel.c }));
      if (key === "PageDown" || key === "PageUp") return run(() => { const list = d.wb.sheets.map((x, i) => [x, i]).filter(([x]) => !x.hidden).map(([, i]) => i); const at = list.indexOf(d.wb.active); const next = list[(at + (key === "PageDown" ? 1 : -1) + list.length) % list.length]; switchSheet(next); });
      if (key === "Home") return run(() => setSelection(0, 0));
      if (key === "End") return run(() => { const e = S.extentOf(s); setSelection(Math.max(0, e.r2), Math.max(0, e.c2)); });
      if (key.startsWith("Arrow")) {
        return run(() => {
          const step = { ArrowDown: [1, 0], ArrowUp: [-1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1] }[key];
          const to = jumpEdge(step[0], step[1]);
          if (event.shiftKey) setSelection(to.r, to.c, true); else setSelection(to.r, to.c);
        });
      }
      return;
    }
    const step = { ArrowDown: [1, 0], ArrowUp: [-1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1] }[key];
    if (step) { event.preventDefault(); moveBy(step[0], step[1], event.shiftKey); return; }
    if (key === "Enter") {
      event.preventDefault();
      if (event.altKey) return;
      const multi = sel.r1 !== sel.r2 || sel.c1 !== sel.c2;
      if (multi) {
        let { r, c } = d.sel;
        if (event.shiftKey) { r--; if (r < sel.r1) { r = sel.r2; c--; if (c < sel.c1) c = sel.c2; } }
        else { r++; if (r > sel.r2) { r = sel.r1; c++; if (c > sel.c2) c = sel.c1; } }
        d.sel = { ...d.sel, r, c }; updateFormulaBar(); draw(); return;
      }
      const rule = S.validationAt(s, d.sel.r, d.sel.c);
      if (rule && rule.type === "list" && rule.dropdown !== false) { const rect = cellRect(d.sel.r, d.sel.c), host = grid.host.getBoundingClientRect(); openDropdown(d.sel.r, d.sel.c, { left: host.left + rect.x, right: host.left + rect.x + rect.w, top: host.top + rect.y, bottom: host.top + rect.y + rect.h }); return; }
      if (event.shiftKey) moveBy(-1, 0, false); else startEdit(undefined, "edit");
      return;
    }
    if (key === "Tab") { event.preventDefault(); moveBy(0, event.shiftKey ? -1 : 1, false); return; }
    if (key === "F2" && event.shiftKey) { event.preventDefault(); noteDialog(); return; }
    if (key === "F2") { event.preventDefault(); startEdit(undefined, "edit"); return; }
    if (key === "Delete" || key === "Backspace") { event.preventDefault(); clearRange("values"); return; }
    if (key === "Escape") { if (d.clipboard) { d.clipboard.marquee = false; draw(); } if (d.painter) { d.painter = null; renderToolbarState(); } return; }
    if (key === " " && event.shiftKey) { event.preventDefault(); selectRange({ r1: sel.r1, c1: 0, r2: sel.r2, c2: s.colCount - 1 }, { r: d.sel.r, c: d.sel.c }); return; }
    if (key === " " && S.validationAt(s, d.sel.r, d.sel.c)?.type === "checkbox") { event.preventDefault(); toggleCheckbox(); return; }
    if (key === "Home") { event.preventDefault(); setSelection(d.sel.r, 0, event.shiftKey); return; }
    if (key === "End") { event.preventDefault(); const e = S.extentOf(s); setSelection(d.sel.r, Math.max(0, e.c2), event.shiftKey); return; }
    if (key === "PageDown" || key === "PageUp") {
      event.preventDefault();
      const rows = Math.max(1, Math.floor((grid.height - HEADER_H) / (s.defaultRowHeight * d.zoom)) - 1);
      moveBy(key === "PageDown" ? rows : -rows, 0, event.shiftKey);
    }
  }

  function toggleAi(force) {
    const panel = q("[data-sheets-ai]");
    const show = typeof force === "boolean" ? force : panel.classList.contains("hidden");
    panel.classList.toggle("hidden", !show);
    renderToolbarState();
    requestAnimationFrame(() => { resize(); if (show) q("[data-sheets-ai-input]")?.focus(); });
  }
  function aiMessage(role, html) {
    const list = q("[data-sheets-ai-log]");
    const el = document.createElement("div");
    el.className = `sheets-ai-msg sheets-ai-${role}`;
    el.innerHTML = html;
    list.appendChild(el);
    list.scrollTop = list.scrollHeight;
    icons();
    return el;
  }
  async function askAi(prompt, mode = "edit") {
    const d = doc();
    if (!d || !prompt.trim()) return;
    if (d.editing) commitEdit();
    recalc();
    aiMessage("user", esc(prompt));
    const pending = aiMessage("assistant", '<span class="text-muted-foreground">Working on it…</span>');
    const button = q("[data-sheets-ai-send]");
    if (button) button.disabled = true;
    try {
      const context = mode === "create" ? `A new, empty spreadsheet named "${d.title}". Active sheet: ${S.quoteSheet(sheet().name)}.` : S.aiContext(d.wb, d.calc, sheet(), selectedRange());
      const payload = await request(`${API}/ai`, "POST", { prompt, context, mode });
      const actions = Array.isArray(payload.actions) ? payload.actions : [];
      let result = { notes: [] };
      const undoBefore = d.undo.length;
      if (actions.length) {
        result = act(`AI: ${prompt.slice(0, 40)}`, (tx) => S.applyAiActions(tx, d.wb, d.calc, sheet(), actions, inputOptions)) || { notes: [] };
        if (result.sheet) { const index = d.wb.sheets.indexOf(result.sheet); if (index >= 0 && index !== d.wb.active) switchSheet(index, true); }
        if (result.selection) { const index = d.wb.sheets.indexOf(result.selection.sheet); if (index >= 0) { switchSheet(index); selectRange(result.selection.range); } }
      }
      const changedSomething = d.undo.length > undoBefore;
      pending.innerHTML = `<p>${esc(payload.reply || (changedSomething ? "Done." : "I could not find anything to change."))}</p>${result.notes?.length ? `<ul>${result.notes.map((n) => `<li>${esc(n)}</li>`).join("")}</ul>` : ""}${changedSomething ? '<button type="button" class="fm-btn fm-btn-sm mt-2" data-sheets-ai-undo><i data-lucide="undo-2"></i>Undo</button>' : ""}`;
      icons();
      const undo = pending.querySelector("[data-sheets-ai-undo]");
      undo?.addEventListener("click", () => { if (d.undo.length && d.undo[d.undo.length - 1].label.startsWith("AI:")) history("undo"); undo.disabled = true; });
    } catch (error) {
      const reason = error.status === 429 ? "The AI assistant is busy for your account right now. Try again in a minute." : error.status === 503 ? "The AI assistant is not available on this server right now." : `The AI assistant could not answer: ${error.message}`;
      pending.innerHTML = `<p class="text-destructive">${esc(reason)}</p>`;
    } finally { if (button) button.disabled = false; }
  }

  function buildEditorOnce() {
    if (grid.built) return;
    grid.built = true;
    grid.host = q("[data-sheets-grid]");
    grid.scroller = q("[data-sheets-scroller]");
    grid.sizer = q("[data-sheets-sizer]");
    grid.canvas = q("[data-sheets-canvas]");
    grid.ctx = grid.canvas.getContext("2d");
    grid.editor = q("[data-sheets-cell-input]");
    grid.charts = q("[data-sheets-charts]");
    readColors();
    buildMenubar();
    buildToolbar();
    grid.scroller.addEventListener("scroll", () => { updateSizer(); drawNow(); positionNoteHint(); });
    new ResizeObserver(() => resize()).observe(grid.host);
    new MutationObserver(() => { readColors(); if (doc()) { renderCharts(); draw(); } }).observe(document.documentElement, { attributes: true, attributeFilter: ["data-dashboard-theme", "class", "style"] });
    grid.scroller.addEventListener("mousedown", onMouseDown);
    grid.scroller.addEventListener("dblclick", onDoubleClick);
    grid.scroller.addEventListener("contextmenu", (event) => {
      const d = doc();
      if (!d) return;
      event.preventDefault();
      if (d.editing) commitEdit();
      const { x, y } = pointerAt(event);
      const hit = hitTest(x, y), s = sheet();
      if (hit.area === "cell" && !S.inRange(selectedRange(), hit.r, hit.c)) setSelection(hit.r, hit.c);
      if (hit.area === "colHeader" && !(d.sel.r1 === 0 && d.sel.r2 >= s.rowCount - 1 && hit.c >= d.sel.c1 && hit.c <= d.sel.c2)) selectRange({ r1: 0, c1: hit.c, r2: s.rowCount - 1, c2: hit.c }, { r: 0, c: hit.c });
      if (hit.area === "rowHeader" && !(d.sel.c1 === 0 && d.sel.c2 >= s.colCount - 1 && hit.r >= d.sel.r1 && hit.r <= d.sel.r2)) selectRange({ r1: hit.r, c1: 0, r2: hit.r, c2: s.colCount - 1 }, { r: hit.r, c: 0 });
      contextMenu(hit, event);
    });
    window.addEventListener("mousemove", (event) => onMouseMove(event));
    window.addEventListener("mouseup", onMouseUp);
    grid.charts.addEventListener("mousedown", chartPointer);
    grid.charts.addEventListener("dblclick", (event) => { const el = event.target.closest("[data-chart]"); if (el) chartDialog(sheet().charts.find((c) => c.id === el.dataset.chart)); });
    const editor = grid.editor;
    editor.addEventListener("keydown", onKeyDown);
    editor.addEventListener("input", () => {
      const d = doc();
      if (!d) return;
      if (!d.editing) { const typed = editor.value; startEdit(typed, "enter"); return; }
      syncEditing(editor);
    });
    editor.addEventListener("compositionstart", () => { const d = doc(); if (d && !d.editing) startEdit("", "enter"); });
    editor.addEventListener("click", () => renderAutocomplete());
    editor.addEventListener("keyup", (event) => { if (!["ArrowDown", "ArrowUp", "Enter", "Tab"].includes(event.key) || !grid.suggestions) renderAutocomplete(); });
    editor.addEventListener("copy", (event) => { if (doc()?.editing) return; event.preventDefault(); copySelection(event.clipboardData, false); });
    editor.addEventListener("cut", (event) => { if (doc()?.editing) return; event.preventDefault(); copySelection(event.clipboardData, true); });
    editor.addEventListener("paste", (event) => {
      const d = doc();
      if (!d || d.editing) return;
      event.preventDefault();
      const html = event.clipboardData.getData("text/html"), text = event.clipboardData.getData("text/plain");
      if (grid.pasteValues) { grid.pasteValues = false; if (d.clipboard && normalizeClip(text) === normalizeClip(d.clipboard.text)) pasteInternal("values"); else pasteRows(S.parseCsv(normalizeClip(text), "\t")); return; }
      pasteFrom(html, text);
    });
    editor.addEventListener("keydown", (event) => { if ((event.ctrlKey || event.metaKey) && event.shiftKey && event.key.toLowerCase() === "v") grid.pasteValues = true; }, true);
    const bar = q("[data-sheets-formula]");
    bar.addEventListener("focus", () => { const d = doc(); if (d && !d.editing) startEdit(undefined, "edit", "bar"); else if (d?.editing) d.editing.source = "bar"; });
    bar.addEventListener("input", () => syncEditing(bar));
    bar.addEventListener("keydown", onKeyDown);
    bar.addEventListener("keyup", () => renderAutocomplete());
    const nameBox = q("[data-sheets-namebox]");
    nameBox.addEventListener("focus", () => nameBox.select());
    nameBox.addEventListener("keydown", (event) => {
      if (event.key === "Escape") { updateFormulaBar(); focusGrid(); return; }
      if (event.key !== "Enter") return;
      event.preventDefault();
      const d = doc(), text = nameBox.value.trim();
      const named = d.wb.names.get(text.toUpperCase());
      const target = named ? named.ref : text;
      const bang = target.lastIndexOf("!");
      const sheetName = bang > 0 ? target.slice(0, bang).replace(/^'|'$/g, "").replace(/''/g, "'") : null;
      const range = S.parseRange((bang > 0 ? target.slice(bang + 1) : target).replace(/\$/g, ""));
      if (!range) { toast("Type a cell like B12, a range like A1:D20, or a named range."); return; }
      if (sheetName) { const index = d.wb.sheets.findIndex((s) => s.name.toLowerCase() === sheetName.toLowerCase()); if (index >= 0) switchSheet(index); }
      if (range.r1 === range.r2 && range.c1 === range.c2) setSelection(range.r1, range.c1); else { selectRange(range); scrollIntoView(range.r1, range.c1); }
      focusGrid();
    });
    q("[data-sheets-tabs]").addEventListener("click", (event) => {
      const menuButton = event.target.closest("[data-sheets-tab-menu]");
      const tab = event.target.closest("[data-sheets-tab]");
      if (!tab) return;
      const index = Number(tab.dataset.sheetsTab);
      if (menuButton) { switchSheet(index); tabMenu(index, tab.getBoundingClientRect()); return; }
      const d = doc();
      if (d.editing && d.editing.text.startsWith("=") && inPointMode()) { switchSheet(index); return; }
      switchSheet(index);
    });
    q("[data-sheets-tabs]").addEventListener("dblclick", (event) => { const tab = event.target.closest("[data-sheets-tab]"); if (tab) renameSheetPrompt(Number(tab.dataset.sheetsTab)); });
    q("[data-sheets-tabs]").addEventListener("contextmenu", (event) => { const tab = event.target.closest("[data-sheets-tab]"); if (!tab) return; event.preventDefault(); const index = Number(tab.dataset.sheetsTab); switchSheet(index); tabMenu(index, { left: event.clientX, right: event.clientX, top: event.clientY - 300, bottom: event.clientY }); });
    q("[data-sheets-add-sheet]").addEventListener("click", () => addSheet(false));
    q("[data-sheets-all-sheets]").addEventListener("click", (event) => openMenu(event.currentTarget.getBoundingClientRect(), doc().wb.sheets.map((s, i) => ({ label: s.name + (s.hidden ? " (hidden)" : ""), checked: i === doc().wb.active, action: () => { if (s.hidden) act("Unhide sheet", (tx) => tx.set(s, "hidden", false)); switchSheet(i, true); } }))));
    q("[data-sheets-back]").addEventListener("click", closeEditor);
    const title = q("[data-sheets-title]");
    title.addEventListener("change", () => { const d = doc(); const next = title.value.trim().slice(0, 120); if (next && next !== d.title) { d.title = next; markDirty(); } else title.value = d.title; });
    title.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); title.blur(); focusGrid(); } });
    q("[data-sheets-download]").addEventListener("click", (event) => openMenu(event.currentTarget.getBoundingClientRect(), exportItems()));
    q("[data-sheets-ai-toggle]").addEventListener("click", () => toggleAi());
    q("[data-sheets-ai-close]").addEventListener("click", () => toggleAi(false));
    q("[data-sheets-ai-form]").addEventListener("submit", (event) => { event.preventDefault(); const input = q("[data-sheets-ai-input]"); const text = input.value; input.value = ""; askAi(text); });
    q("[data-sheets-ai-input]").addEventListener("keydown", (event) => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); q("[data-sheets-ai-form]").requestSubmit(); } });
    q("[data-sheets-ai-examples]").addEventListener("click", (event) => { const b = event.target.closest("[data-ai-example]"); if (b) askAi(b.textContent); });
    q("[data-sheets-conflict-reload]").addEventListener("click", async () => { const d = doc(); q("[data-sheets-conflict]").classList.add("hidden"); d.dirty = false; await openWorkbook(d.id); });
    q("[data-sheets-conflict-keep]").addEventListener("click", async () => { const d = doc(); q("[data-sheets-conflict]").classList.add("hidden"); d.conflict = false; d.dirty = true; await save(true); });
    q("[data-sheets-zoom-out]").addEventListener("click", () => setZoom(doc().zoom - 0.1));
    q("[data-sheets-zoom-in]").addEventListener("click", () => setZoom(doc().zoom + 0.1));
  }
  function tabMenu(index, rect) {
    const d = doc(), s = d.wb.sheets[index];
    openMenu(rect, [
      { label: "Rename…", icon: "pencil", action: () => renameSheetPrompt(index) }, { label: "Duplicate", icon: "copy", action: () => duplicateSheet(index) },
      { label: "Change colour", icon: "palette", swatch: s.color || undefined, action: () => colorPicker(rect, "Tab colour", s.color, (color) => act("Tab colour", (tx) => tx.set(s, "color", color))) },
      { label: "Move left", icon: "arrow-left", disabled: index === 0, action: () => sheetMove(-1) }, { label: "Move right", icon: "arrow-right", disabled: index === d.wb.sheets.length - 1, action: () => sheetMove(1) },
      { label: "Hide sheet", icon: "eye-off", action: () => { if (d.wb.sheets.filter((x) => !x.hidden).length <= 1) { toast("At least one sheet has to stay visible."); return; } act("Hide sheet", (tx) => tx.set(s, "hidden", true)); const next = d.wb.sheets.findIndex((x) => !x.hidden); switchSheet(next, true); } }, "-",
      { label: "Delete", icon: "trash-2", danger: true, action: () => deleteSheet(index) },
    ]);
  }

  function bindPage() {
    q("[data-sheets-new]")?.addEventListener("click", () => createWorkbook(S.createWorkbook(), "Untitled spreadsheet"));
    q("[data-sheets-import]")?.addEventListener("click", () => pickFiles(false));
    q("[data-sheets-file]")?.addEventListener("change", (event) => { const files = [...event.target.files]; const into = event.target.dataset.into === "1"; event.target.value = ""; importFiles(files, into); });
    q("[data-sheets-search]")?.addEventListener("input", renderList);
    q("[data-sheets-install]")?.addEventListener("click", async () => {
      try { await request(`${API}/install`, "POST", {}); flash("notice", "Spreadsheets is installed for your account."); await loadList(); }
      catch (error) { flash("error", `Could not install: ${error.message}`); }
    });
    q("[data-sheets-items]")?.addEventListener("click", (event) => {
      const open = event.target.closest("[data-sheets-open]");
      if (open) { openWorkbook(open.dataset.sheetsOpen); return; }
      const menu = event.target.closest("[data-sheets-row-menu]");
      if (!menu) return;
      const item = state.list.find((x) => x.id === menu.dataset.sheetsRowMenu);
      if (!item) return;
      const withBook = async (fn) => { const payload = await request(`${API}/workbooks/${encodeURIComponent(item.id)}`); const wb = await S.unpackWorkbook(payload.workbook.data); const calc = new S.Calc(wb); calc.recalc(); return fn(wb, calc); };
      openMenu(menu.getBoundingClientRect(), [
        { label: "Open", icon: "sheet", action: () => openWorkbook(item.id) },
        { label: "Download .xlsx", icon: "download", action: () => withBook(async (wb, calc) => download(await S.writeXlsx(wb, calc), `${safeFilename(item.title)}.xlsx`, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")) },
        { label: "Download .xls", icon: "download", action: () => withBook((wb, calc) => download(S.writeXls(wb, calc), `${safeFilename(item.title)}.xls`, "application/vnd.ms-excel")) },
        { label: "Download .ods", icon: "download", action: () => withBook(async (wb, calc) => download(await S.writeOds(wb, calc), `${safeFilename(item.title)}.ods`, "application/vnd.oasis.opendocument.spreadsheet")) },
        { label: "Make a copy", icon: "copy", action: () => withBook(async (wb) => { const payload = await request(`${API}/workbooks`, "POST", { title: `Copy of ${item.title}`.slice(0, 120), data: await S.packWorkbook(wb, null) }); state.list.unshift(payload.workbook); renderList(); }) },
        { label: "Rename…", icon: "pencil", action: async () => { const title = await promptDialog("Rename spreadsheet", "Name", item.title); if (!title || !title.trim()) return; const payload = await request(`${API}/workbooks/${encodeURIComponent(item.id)}`, "PUT", { title: title.trim().slice(0, 120), revision: item.revision }); Object.assign(item, payload.workbook); renderList(); } },
        "-", { label: "Delete…", icon: "trash-2", danger: true, action: () => deleteWorkbook(item.id, item.title) },
      ]);
    });
    q("[data-sheets-ai-create-form]")?.addEventListener("submit", async (event) => {
      event.preventDefault();
      const input = q("[data-sheets-ai-create-input]");
      const prompt = input.value.trim();
      if (!prompt) return;
      input.value = "";
      const title = prompt.replace(/^(make|create|build|start)\s+(me\s+)?(a|an)\s+/i, "").replace(/[.!?].*$/, "").slice(0, 60) || "AI spreadsheet";
      await createWorkbook(S.createWorkbook(), title.charAt(0).toUpperCase() + title.slice(1));
      toggleAi(true);
      await askAi(prompt, "create");
    });
    q("[data-sheets-manage]")?.addEventListener("click", () => q("[data-sheets-manage-dialog]").showModal());
    q("[data-sheets-dialog-close]")?.addEventListener("click", () => q("[data-sheets-manage-dialog]").close());
    q("[data-sheets-export-data]")?.addEventListener("click", async () => {
      try { const payload = await request(`${API}/export`); download(JSON.stringify(payload.export, null, 2), payload.filename || "forkmesh-spreadsheets.json", "application/json"); }
      catch (error) { flash("error", `Could not export: ${error.message}`); }
    });
    q("[data-sheets-uninstall]")?.addEventListener("click", async () => {
      if (!(await confirmDialog("Uninstall Spreadsheets", "This downloads every spreadsheet as one JSON file, then deletes them from your account on every device.", "Uninstall"))) return;
      try {
        const exported = await request(`${API}/export`);
        download(JSON.stringify(exported.export, null, 2), exported.filename || "forkmesh-spreadsheets.json", "application/json");
        await request(`${API}/uninstall`, "POST", {});
        q("[data-sheets-manage-dialog]").close();
        state.list = [];
        await loadList();
        flash("notice", "Spreadsheets was uninstalled. Your export was downloaded.");
      } catch (error) { flash("error", `Could not uninstall: ${error.message}`); }
    });
    root.addEventListener("dragover", (event) => { if (event.dataTransfer?.types?.includes("Files")) { event.preventDefault(); root.classList.add("sheets-dropping"); } });
    root.addEventListener("dragleave", (event) => { if (event.target === root || !root.contains(event.relatedTarget)) root.classList.remove("sheets-dropping"); });
    root.addEventListener("drop", (event) => {
      if (!event.dataTransfer?.files?.length) return;
      event.preventDefault();
      root.classList.remove("sheets-dropping");
      if (!state.installed) { flash("error", "Install Spreadsheets first, then drop the file again."); return; }
      importFiles([...event.dataTransfer.files], !!doc());
    });
    document.addEventListener("mousedown", (event) => { if (!event.target.closest(".sheets-menu, .sheets-panel, [data-menubar], [data-sheets-autocomplete]")) closePopovers(); }, true);
    q("[data-sheets-autocomplete]")?.addEventListener("mousedown", (event) => { const b = event.target.closest("[data-suggest]"); if (b) { event.preventDefault(); acceptSuggestion(b.dataset.suggest); } });
    document.addEventListener("keydown", (event) => { if (event.key === "Escape" && openMenus.length) { closePopovers(); focusGrid(); } });
    window.addEventListener("beforeunload", (event) => { const d = doc(); if (d && (d.dirty || d.saving)) { save(); event.preventDefault(); event.returnValue = ""; } });
    document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden" && doc()?.dirty) save(); });
  }

  bindPage();
  loadList().then(() => {
    const id = new URL(location.href).searchParams.get("sheet");
    if (id && state.installed) openWorkbook(id);
  });
});
})();
