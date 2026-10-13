// A provider login terminal, streamed from the account's own node to the web.
//
// The dashboard used to offer this as a `forkmesh://agent-login/<node>` link.
// That only ever worked on a machine with the ForkMesh desktop installed AND
// the node saved as an SSH host in it; anywhere else the browser handed the
// URL to the operating system, which answered "No Apps Available" and left
// the sign-in with nowhere to go.
//
// So the terminal comes to the browser instead. Both halves hold a WebSocket
// to one per-owner ForkMeshNodes Durable Object — the node with its signed
// drain token, this page with an event ticket — and the relay forwards
// {"type":"terminal"} frames between them without keeping a copy
// (app/src/node_terminal.py). Which object that is depends on the node: a
// managed mirror is its own node account and sits on its own, a desktop sits
// on the account's, so this page names the machine it wants and the relay
// joins it to the right one. The node runs exactly one thing, the provider's
// own login command, and it decides whether to run it at all.
//
// What this module owns: the pty stream's client half. It is a small VT100/
// xterm subset — enough for the provider CLIs' full-screen redraws — because
// the alternative, appending raw output to a log, renders a cursor-addressed
// login prompt as garbage.

// The account channel is imported when a session actually starts, not at
// module load: this module is itself loaded on demand, and keeping the import
// dynamic also lets the emulator below be exercised on its own.
const ACCOUNT_CHANNEL = "./account-events.js";

// The node coalesces output, so these are redraw bursts rather than keystrokes.
const MAX_SCROLLBACK = 1000;
// Keystrokes are batched at roughly a frame so a fast typist costs one frame
// per burst instead of one per character.
const INPUT_FLUSH_MS = 25;
// How long to wait for the node to answer an open before saying it is not
// there. The relay does not tell us whether anything received the frame — it
// is a wire, not a party — so silence is the only signal, and it has to be
// distinguishable from a slow start.
const OPEN_TIMEOUT_MS = 9000;
// An OSC string longer than this is dropped whole. A sign-in URL is ~450
// characters; nothing a login prompt sends comes close to the cap.
const MAX_OSC_CHARS = 4096;

// Both defaults render as "inherit": ordinary text carries no inline colour at
// all, which is most of a sign-in screen. The concrete values are only needed
// to resolve an inverse cell, and they match the stylesheet's own.
const DEFAULT_FG = -1;
const DEFAULT_BG = -1;
const DEFAULT_FG_CSS = "#c7c9cf";
const DEFAULT_BG_CSS = "#0d0d10";

// xterm's 16 base colours, then the 6x6x6 cube and greyscale ramp on demand.
const BASE_COLORS = [
  "#1e1e21", "#e05561", "#8cc265", "#d5a24a", "#4d9fe8", "#c162de",
  "#42b3c2", "#c7c9cf", "#5c6370", "#ff6d78", "#a5e075", "#f0c674",
  "#61afef", "#d982f0", "#56cfe1", "#f2f3f5",
];

function colorOf(index) {
  if (index < 0) return "";
  if (index < 16) return BASE_COLORS[index];
  if (index < 232) {
    const value = index - 16;
    const level = (n) => [0, 95, 135, 175, 215, 255][n];
    return `rgb(${level(Math.floor(value / 36) % 6)},${
      level(Math.floor(value / 6) % 6)},${level(value % 6)})`;
  }
  if (index < 256) {
    const grey = 8 + (index - 232) * 10;
    return `rgb(${grey},${grey},${grey})`;
  }
  // 24-bit colours are packed as 0x1000000 | rgb so one number describes a cell.
  const rgb = index - 0x1000000;
  return `rgb(${(rgb >> 16) & 255},${(rgb >> 8) & 255},${rgb & 255})`;
}

function escapeHtml(value) {
  return value.replace(/[&<>"]/g, (char) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[char]);
}

// Only web links become clickable: the target comes from the remote pty, and
// a javascript: or data: URL must stay inert text.
function safeLink(uri) {
  return /^https?:\/\/[\x21-\x7e]+$/i.test(uri) ? uri : "";
}

function randomSessionId() {
  const bytes = new Uint8Array(16);
  (globalThis.crypto || {}).getRandomValues?.(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

function encodeBase64(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

function decodeBase64(text) {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

// --- screen model ----------------------------------------------------------

function blankCell() {
  return { ch: " ", fg: DEFAULT_FG, bg: DEFAULT_BG, bold: false,
    inverse: false, underline: false, dim: false, link: "" };
}

function blankRow(cols) {
  const row = new Array(cols);
  for (let i = 0; i < cols; i += 1) row[i] = blankCell();
  return row;
}

// A minimal VT parser over decoded text. Everything it does not understand is
// dropped rather than printed: a stray escape sequence painted literally is
// worse than a missing one, and the provider CLIs stay inside this subset.
export function createScreen(rows, cols) {
  const screen = {
    rows,
    cols,
    grid: [],
    alt: null,
    scrollback: [],
    cx: 0,
    cy: 0,
    savedCx: 0,
    savedCy: 0,
    scrollTop: 0,
    scrollBottom: rows - 1,
    fg: DEFAULT_FG,
    bg: DEFAULT_BG,
    bold: false,
    dim: false,
    inverse: false,
    underline: false,
    wrap: true,
    wrapPending: false,
    cursorVisible: true,
    appCursorKeys: false,
    bracketedPaste: false,
    state: "ground",
    params: "",
    intermediate: "",
    oscBuffer: "",
    link: "",
  };
  for (let i = 0; i < rows; i += 1) screen.grid.push(blankRow(cols));
  return screen;
}

function clampCursor(s) {
  s.cx = Math.max(0, Math.min(s.cols - 1, s.cx));
  s.cy = Math.max(0, Math.min(s.rows - 1, s.cy));
}

function scrollUp(s, top, bottom, count) {
  for (let n = 0; n < count; n += 1) {
    const removed = s.grid.splice(top, 1)[0];
    // Only the primary screen keeps history; an alternate-screen redraw is
    // transient by definition and would otherwise flood the scrollback.
    if (!s.alt && top === 0) {
      s.scrollback.push(removed);
      if (s.scrollback.length > MAX_SCROLLBACK) s.scrollback.shift();
    }
    s.grid.splice(bottom, 0, blankRow(s.cols));
  }
}

function scrollDown(s, top, bottom, count) {
  for (let n = 0; n < count; n += 1) {
    s.grid.splice(bottom, 1);
    s.grid.splice(top, 0, blankRow(s.cols));
  }
}

function newLine(s) {
  if (s.cy === s.scrollBottom) scrollUp(s, s.scrollTop, s.scrollBottom, 1);
  else if (s.cy < s.rows - 1) s.cy += 1;
}

function putChar(s, ch) {
  if (s.wrapPending && s.wrap) {
    // Remembered so a URL the grid wrapped can be found again as one string.
    s.grid[s.cy].wrapped = true;
    s.cx = 0;
    newLine(s);
  }
  s.wrapPending = false;
  clampCursor(s);
  s.grid[s.cy][s.cx] = {
    ch,
    fg: s.fg,
    bg: s.bg,
    bold: s.bold,
    dim: s.dim,
    inverse: s.inverse,
    underline: s.underline,
    link: s.link,
  };
  if (s.cx + 1 >= s.cols) s.wrapPending = true;
  else s.cx += 1;
}

function eraseCells(row, from, to, s) {
  if (from <= 0 && to >= row.length - 1) row.wrapped = false;
  for (let i = from; i <= to && i < row.length; i += 1) {
    const cell = blankCell();
    cell.bg = s.bg;
    row[i] = cell;
  }
}

function sgrCodes(raw) {
  return raw.split(";").flatMap((group) => {
    const parts = group.split(":").map((part) => parseInt(part, 10) || 0);
    if (parts.length === 1) return parts;
    if (parts[0] === 4) return [parts[1] === 0 ? 24 : 4];
    if ((parts[0] === 38 || parts[0] === 48) && parts[1] === 5 && parts.length >= 3) return parts.slice(0, 3);
    if ((parts[0] === 38 || parts[0] === 48) && parts[1] === 2 && parts.length >= 5) return [parts[0], 2, ...parts.slice(-3)];
    return [];
  });
}

function applySgr(s, params) {
  const codes = params.length ? params : [0];
  for (let i = 0; i < codes.length; i += 1) {
    const code = codes[i];
    if (code === 0) {
      s.fg = DEFAULT_FG;
      s.bg = DEFAULT_BG;
      s.bold = s.dim = s.inverse = s.underline = false;
    } else if (code === 1) s.bold = true;
    else if (code === 2) s.dim = true;
    else if (code === 4) s.underline = true;
    else if (code === 7) s.inverse = true;
    else if (code === 22) s.bold = s.dim = false;
    else if (code === 24) s.underline = false;
    else if (code === 27) s.inverse = false;
    else if (code >= 30 && code <= 37) s.fg = code - 30;
    else if (code === 39) s.fg = DEFAULT_FG;
    else if (code >= 40 && code <= 47) s.bg = code - 40;
    else if (code === 49) s.bg = DEFAULT_BG;
    else if (code >= 90 && code <= 97) s.fg = code - 90 + 8;
    else if (code >= 100 && code <= 107) s.bg = code - 100 + 8;
    else if (code === 38 || code === 48) {
      const target = code === 38 ? "fg" : "bg";
      if (codes[i + 1] === 5) {
        s[target] = Math.max(0, Math.min(255, codes[i + 2] || 0));
        i += 2;
      } else if (codes[i + 1] === 2) {
        const r = Math.max(0, Math.min(255, codes[i + 2] || 0));
        const g = Math.max(0, Math.min(255, codes[i + 3] || 0));
        const b = Math.max(0, Math.min(255, codes[i + 4] || 0));
        s[target] = 0x1000000 + ((r << 16) | (g << 8) | b);
        i += 4;
      }
    }
  }
}

function useAltScreen(s, on) {
  if (on && !s.alt) {
    s.alt = s.grid;
    s.grid = [];
    for (let i = 0; i < s.rows; i += 1) s.grid.push(blankRow(s.cols));
    s.cx = s.cy = 0;
  } else if (!on && s.alt) {
    s.grid = s.alt;
    s.alt = null;
  }
}

function handleMode(s, params, enable, isPrivate) {
  if (!isPrivate) return;
  for (const code of params) {
    if (code === 1) s.appCursorKeys = enable;
    else if (code === 7) s.wrap = enable;
    else if (code === 25) s.cursorVisible = enable;
    else if (code === 2004) s.bracketedPaste = enable;
    else if (code === 47 || code === 1047 || code === 1049) {
      if (code === 1049 && enable) {
        s.savedCx = s.cx;
        s.savedCy = s.cy;
      }
      useAltScreen(s, enable);
      if (code === 1049 && !enable) {
        s.cx = s.savedCx;
        s.cy = s.savedCy;
      }
    }
  }
}

function handleCsi(s, finalByte) {
  const isPrivate = s.intermediate === "?";
  if (s.intermediate !== "" && !(isPrivate && "hlJK".includes(finalByte))) return;
  const params = s.params
    .split(";")
    .map((part) => (part === "" ? 0 : parseInt(part, 10) || 0));
  const first = params[0] || 0;
  const count = Math.max(1, first);
  switch (finalByte) {
    case "A": s.cy = Math.max(s.scrollTop, s.cy - count); break;
    case "B": s.cy = Math.min(s.scrollBottom, s.cy + count); break;
    case "C": s.cx = Math.min(s.cols - 1, s.cx + count); s.wrapPending = false;
      break;
    case "D": s.cx = Math.max(0, s.cx - count); s.wrapPending = false; break;
    case "E": s.cx = 0; s.cy = Math.min(s.rows - 1, s.cy + count); break;
    case "F": s.cx = 0; s.cy = Math.max(0, s.cy - count); break;
    case "G": case "`": s.cx = count - 1; s.wrapPending = false; break;
    case "d": s.cy = count - 1; break;
    case "H": case "f":
      s.cy = (params[0] || 1) - 1;
      s.cx = (params[1] || 1) - 1;
      s.wrapPending = false;
      break;
    case "J": {
      clampCursor(s);
      if (first === 0) {
        eraseCells(s.grid[s.cy], s.cx, s.cols - 1, s);
        for (let y = s.cy + 1; y < s.rows; y += 1)
          eraseCells(s.grid[y], 0, s.cols - 1, s);
      } else if (first === 1) {
        for (let y = 0; y < s.cy; y += 1)
          eraseCells(s.grid[y], 0, s.cols - 1, s);
        eraseCells(s.grid[s.cy], 0, s.cx, s);
      } else {
        for (let y = 0; y < s.rows; y += 1)
          eraseCells(s.grid[y], 0, s.cols - 1, s);
      }
      break;
    }
    case "K": {
      clampCursor(s);
      if (first === 0) eraseCells(s.grid[s.cy], s.cx, s.cols - 1, s);
      else if (first === 1) eraseCells(s.grid[s.cy], 0, s.cx, s);
      else eraseCells(s.grid[s.cy], 0, s.cols - 1, s);
      break;
    }
    case "L": scrollDown(s, s.cy, s.scrollBottom, count); break;
    case "M": scrollUp(s, s.cy, s.scrollBottom, count); break;
    case "P": {
      clampCursor(s);
      const row = s.grid[s.cy];
      row.splice(s.cx, Math.min(count, row.length - s.cx));
      while (row.length < s.cols) row.push(blankCell());
      break;
    }
    case "@": {
      clampCursor(s);
      const row = s.grid[s.cy];
      for (let n = 0; n < count; n += 1) row.splice(s.cx, 0, blankCell());
      row.length = s.cols;
      break;
    }
    case "X": {
      clampCursor(s);
      eraseCells(s.grid[s.cy], s.cx, s.cx + count - 1, s);
      break;
    }
    case "S": scrollUp(s, s.scrollTop, s.scrollBottom, count); break;
    case "T": scrollDown(s, s.scrollTop, s.scrollBottom, count); break;
    case "r":
      s.scrollTop = Math.max(0, (params[0] || 1) - 1);
      s.scrollBottom = Math.min(s.rows - 1, (params[1] || s.rows) - 1);
      if (s.scrollBottom <= s.scrollTop) {
        s.scrollTop = 0;
        s.scrollBottom = s.rows - 1;
      }
      s.cx = 0;
      s.cy = s.scrollTop;
      break;
    case "h": handleMode(s, params, true, isPrivate); break;
    case "l": handleMode(s, params, false, isPrivate); break;
    case "m": applySgr(s, s.params === "" ? [] : sgrCodes(s.params)); break;
    case "s": s.savedCx = s.cx; s.savedCy = s.cy; break;
    case "u": s.cx = s.savedCx; s.cy = s.savedCy; break;
    default: break;
  }
  clampCursor(s);
}

// OSC 8 (`8;params;URI`) opens or closes a hyperlink over the cells that
// follow. Claude Code prints its sign-in URL that way, and the link target is
// the one copy of the URL no row wrap can split. Every other OSC (window
// titles and the like) addresses chrome this emulator does not have.
function endOsc(s) {
  const text = s.oscBuffer;
  s.oscBuffer = "";
  if (text.length > MAX_OSC_CHARS || !text.startsWith("8;")) return;
  const split = text.indexOf(";", 2);
  if (split < 0) return;
  s.link = safeLink(text.slice(split + 1));
}

export function feed(s, text) {
  for (const ch of text) {
    const code = ch.codePointAt(0);
    if (s.state === "osc") {
      if (code === 0x07) {
        endOsc(s);
        s.state = "ground";
      } else if (code === 0x1b) {
        s.state = "osc-esc";
      } else if (s.oscBuffer.length <= MAX_OSC_CHARS) {
        s.oscBuffer += ch;
      }
      continue;
    }
    if (s.state === "osc-esc") {
      if (ch === "\\") {
        endOsc(s);
        s.state = "ground";
      } else {
        s.state = "osc";
      }
      continue;
    }
    if (s.state === "esc") {
      if (ch === "[") {
        s.state = "csi";
        s.params = "";
        s.intermediate = "";
      } else if (ch === "]") {
        s.state = "osc";
        s.oscBuffer = "";
      } else if (ch === "(" || ch === ")" || ch === "#" || ch === "%") {
        s.state = "esc-charset";
      } else {
        if (ch === "7") { s.savedCx = s.cx; s.savedCy = s.cy; }
        else if (ch === "8") { s.cx = s.savedCx; s.cy = s.savedCy; }
        else if (ch === "D") newLine(s);
        else if (ch === "E") { s.cx = 0; newLine(s); }
        else if (ch === "M") {
          if (s.cy === s.scrollTop) scrollDown(s, s.scrollTop, s.scrollBottom, 1);
          else s.cy = Math.max(0, s.cy - 1);
        } else if (ch === "c") {
          const fresh = createScreen(s.rows, s.cols);
          Object.assign(s, fresh, { scrollback: s.scrollback });
        }
        s.state = "ground";
      }
      continue;
    }
    if (s.state === "esc-charset") {
      s.state = "ground";
      continue;
    }
    if (s.state === "csi") {
      if (code >= 0x30 && code <= 0x3f) {
        if (ch === "?" || ch === "<" || ch === "=" || ch === ">") {
          s.intermediate += ch;
        } else {
          s.params += ch;
        }
        continue;
      }
      if (code >= 0x20 && code <= 0x2f) {
        s.intermediate += ch;
        continue;
      }
      handleCsi(s, ch);
      s.state = "ground";
      continue;
    }
    if (code === 0x1b) {
      s.state = "esc";
      continue;
    }
    if (code === 0x0d) { s.cx = 0; s.wrapPending = false; continue; }
    if (code === 0x0a || code === 0x0b || code === 0x0c) {
      newLine(s);
      s.wrapPending = false;
      continue;
    }
    if (code === 0x08) {
      s.cx = Math.max(0, s.cx - 1);
      s.wrapPending = false;
      continue;
    }
    if (code === 0x09) {
      const next = Math.min(s.cols - 1, (Math.floor(s.cx / 8) + 1) * 8);
      s.cx = next;
      s.wrapPending = false;
      continue;
    }
    if (code < 0x20 || code === 0x7f) continue;
    putChar(s, ch);
  }
}

// Plain-text URLs, found across rows the grid soft-wrapped. A URL that wraps
// in a narrow pane cannot be copied out whole (the selection carries a line
// break per row), and pasting the first rows of a sign-in URL is exactly how a
// browser ends up at "Missing code_challenge parameter". Each wrapped piece
// links to the whole URL instead.
function wrappedUrlTargets(rows) {
  const targets = new Array(rows.length).fill(null);
  for (let start = 0; start < rows.length;) {
    let end = start;
    while (end < rows.length - 1 && rows[end].wrapped) end += 1;
    let text = "";
    const where = [];
    for (let r = start; r <= end; r += 1) {
      rows[r].forEach((cell, col) => {
        // One string index per cell: a URL is ASCII, so a wider character can
        // stand in as a blank without shifting any match.
        text += cell.ch.length === 1 ? cell.ch : " ";
        where.push([r, col]);
      });
    }
    if (text.includes("://")) {
      for (const match of text.matchAll(/https?:\/\/[\x21-\x7e]+/g)) {
        const url = safeLink(match[0].replace(/[.,;:!?)\]}>'"]+$/, ""));
        for (let i = 0; url && i < url.length; i += 1) {
          const [r, col] = where[match.index + i];
          (targets[r] ||= [])[col] = url;
        }
      }
    }
    start = end + 1;
  }
  return targets;
}

export function renderScreen(s) {
  const lines = [];
  const rows = s.scrollback.concat(s.grid);
  const urlTargets = wrappedUrlTargets(rows);
  // Where the operator is typing. A login prompt asks for a pasted code, and
  // a terminal that does not show its own cursor makes that guesswork.
  const cursorRow = s.cursorVisible ? s.scrollback.length + s.cy : -1;
  for (let rowIndex = 0; rowIndex < rows.length; rowIndex += 1) {
    const row = rows[rowIndex];
    const cursorCol = rowIndex === cursorRow ? s.cx : -1;
    let html = "";
    let run = "";
    let style = null;
    let openLink = "";
    const flush = () => {
      if (!run) return;
      html += style
        ? `<span style="${style}">${escapeHtml(run)}</span>`
        : escapeHtml(run);
      run = "";
    };
    // Trailing blanks are dropped so a selection copies the visible line
    // rather than the full-width padding behind it.
    let width = row.length;
    while (width > 0) {
      const cell = row[width - 1];
      if (cell.ch !== " " || cell.bg !== DEFAULT_BG || cell.underline) break;
      width -= 1;
    }
    // The cursor usually sits past the last written cell, so the trimmed line
    // has to grow back far enough to carry it.
    width = Math.max(width, cursorCol + 1);
    for (let i = 0; i < width; i += 1) {
      const cell = row[i];
      const link = cell.link || urlTargets[rowIndex]?.[i] || "";
      if (link !== openLink) {
        flush();
        if (openLink) html += "</a>";
        if (link) {
          html += `<a href="${escapeHtml(link)}" target="_blank" ` +
            'rel="noopener noreferrer" ' +
            'style="color:inherit;text-decoration:underline">';
        }
        openLink = link;
      }
      const parts = [];
      // Inverse swaps the pair, and a swapped default has to become a real
      // colour or the highlight the remote program drew would disappear.
      const fgColor = colorOf(cell.inverse ? cell.bg : cell.fg) ||
        (cell.inverse ? DEFAULT_BG_CSS : "");
      if (fgColor) parts.push(`color:${fgColor}`);
      const bgColor = colorOf(cell.inverse ? cell.fg : cell.bg) ||
        (cell.inverse ? DEFAULT_FG_CSS : "");
      if (bgColor) parts.push(`background:${bgColor}`);
      if (cell.bold) parts.push("font-weight:600");
      if (cell.dim) parts.push("opacity:.7");
      if (cell.underline) parts.push("text-decoration:underline");
      if (i === cursorCol) {
        parts.push(`background:${DEFAULT_FG_CSS}`, `color:${DEFAULT_BG_CSS}`);
      }
      const cellStyle = parts.join(";");
      if (cellStyle !== style) {
        flush();
        style = cellStyle;
      }
      run += cell.ch;
    }
    flush();
    if (openLink) html += "</a>";
    lines.push(html || "&nbsp;");
  }
  return lines.join("\n");
}

// --- keyboard --------------------------------------------------------------

export function keyToBytes(event, appCursorKeys) {
  const { key, ctrlKey, altKey, metaKey } = event;
  if (metaKey) return "";
  const ESC = "\x1b";
  // DECCKM: the cursor keys change their own prefix when the remote program
  // asks for application mode, which the provider CLIs' menus rely on.
  const cursor = (letter) => ESC + (appCursorKeys ? "O" : "[") + letter;
  switch (key) {
    case "Enter": return "\r";
    case "Backspace": return "\x7f";
    case "Tab": return "\t";
    case "Escape": return ESC;
    case "ArrowUp": return cursor("A");
    case "ArrowDown": return cursor("B");
    case "ArrowRight": return cursor("C");
    case "ArrowLeft": return cursor("D");
    case "Home": return cursor("H");
    case "End": return cursor("F");
    case "PageUp": return ESC + "[5~";
    case "PageDown": return ESC + "[6~";
    case "Insert": return ESC + "[2~";
    case "Delete": return ESC + "[3~";
    default: break;
  }
  if (ctrlKey && key.length === 1) {
    const upper = key.toUpperCase();
    if (upper >= "A" && upper <= "Z") {
      return String.fromCharCode(upper.charCodeAt(0) - 64);
    }
    const controls = { " ": "\x00", "[": "\x1b", "\\": "\x1c", "]": "\x1d" };
    return controls[key] || "";
  }
  if (key.length === 1) return altKey ? ESC + key : key;
  return "";
}

// --- view ------------------------------------------------------------------

function mountTerminalView({
  surface,
  label,
  onData,
  onResize,
  requestAnimationFrameImpl,
  setTimeoutImpl,
  clearTimeoutImpl,
}) {
  const decoder = new TextDecoder("utf-8");
  const encoder = new TextEncoder();
  const view = document.createElement("pre");
  view.className = "fm-agent-terminal";
  view.tabIndex = 0;
  view.setAttribute("role", "log");
  view.setAttribute("aria-label", label);
  surface.textContent = "";
  surface.style.display = "flex";
  surface.style.flexDirection = "column";
  view.style.cssText = "flex:1;min-height:80px;height:auto";
  surface.appendChild(view);

  let screen = createScreen(24, 80);
  let pendingInput = "";
  let inputTimer = null;
  let renderQueued = false;
  let resizeTimer = null;
  let disposed = false;

  function measure() {
    // One character's box, read from the live element so the grid matches the
    // font the page actually loaded rather than an assumed monospace metric.
    const probe = document.createElement("span");
    probe.textContent = "0".repeat(10);
    probe.style.cssText = "position:absolute;visibility:hidden;white-space:pre";
    view.appendChild(probe);
    const rect = probe.getBoundingClientRect();
    probe.remove();
    const cellWidth = rect.width / 10 || 8;
    const cellHeight = rect.height || 16;
    const box = view.getBoundingClientRect();
    return {
      cols: Math.max(20, Math.min(500, Math.floor((box.width - 8) / cellWidth) || 80)),
      rows: Math.max(5, Math.min(200, Math.floor((box.height - 8) / cellHeight) || 24)),
    };
  }

  function scheduleRender() {
    if (renderQueued) return;
    renderQueued = true;
    const paint = () => {
      renderQueued = false;
      view.innerHTML = renderScreen(screen);
      view.scrollTop = view.scrollHeight;
    };
    if (requestAnimationFrameImpl) requestAnimationFrameImpl(paint);
    else paint();
  }

  function flushInput() {
    inputTimer = null;
    if (!pendingInput) return;
    const data = encodeBase64(encoder.encode(pendingInput));
    pendingInput = "";
    onData(data);
  }

  function queueInput(text) {
    if (!text || disposed) return;
    pendingInput += text;
    // A paste can outrun one frame's budget; hand it over immediately rather
    // than growing a buffer the relay would reject whole.
    if (pendingInput.length > 4096) {
      if (inputTimer !== null) clearTimeoutImpl(inputTimer);
      flushInput();
      return;
    }
    if (inputTimer === null) inputTimer = setTimeoutImpl(flushInput, INPUT_FLUSH_MS);
  }

  view.addEventListener("keydown", (event) => {
    // Copy and paste stay the browser's: Ctrl/Cmd+V has to reach the `paste`
    // listener below to carry the clipboard at all, and a terminal that eats
    // the copy shortcut makes the one thing people need from this window —
    // taking a sign-in URL out of it — impossible.
    const editing = event.ctrlKey || event.metaKey;
    const lower = event.key.toLowerCase();
    if (editing && (lower === "v" || (lower === "c" && event.shiftKey))) return;
    if (event.metaKey) return;
    const bytes = keyToBytes(event, screen.appCursorKeys);
    if (!bytes) return;
    event.preventDefault();
    queueInput(bytes);
  });

  view.addEventListener("paste", (event) => {
    const text = event.clipboardData?.getData("text") || "";
    if (!text) return;
    event.preventDefault();
    queueInput(
      screen.bracketedPaste ? `[200~${text}[201~` : text,
    );
  });

  const controls = document.createElement("div");
  controls.className = "fm-agent-terminal-controls";
  controls.style.cssText = "display:flex;flex-wrap:wrap;gap:8px;padding:8px;align-items:center";
  controls.setAttribute("role", "group");
  controls.setAttribute("aria-label", "Terminal input");
  for (const [label, key] of [["↑", "ArrowUp"], ["↓", "ArrowDown"], ["←", "ArrowLeft"], ["→", "ArrowRight"], ["Tab", "Tab"], ["Enter", "Enter"], ["Escape", "Escape"]]) {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = label;
    button.setAttribute("aria-label", key);
    button.style.cssText = "min-width:44px;min-height:44px;touch-action:manipulation";
    button.addEventListener("click", () => queueInput(keyToBytes({ key }, screen.appCursorKeys)));
    controls.appendChild(button);
  }
  const form = document.createElement("form");
  form.style.cssText = "display:flex;flex:1 1 240px;gap:8px;min-width:0";
  const input = document.createElement("input");
  input.type = "text";
  input.setAttribute("aria-label", "Terminal response");
  input.placeholder = "Type a response";
  input.autocomplete = "off";
  input.autocapitalize = "off";
  input.spellcheck = false;
  input.style.cssText = "min-width:0;flex:1;min-height:44px;font-size:16px";
  const send = document.createElement("button");
  send.type = "submit";
  send.textContent = "Send";
  send.style.cssText = "min-width:44px;min-height:44px;touch-action:manipulation";
  form.append(input, send);
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    queueInput(input.value + "\r");
    input.value = "";
  });
  controls.appendChild(form);
  surface.appendChild(controls);

  const observer = globalThis.ResizeObserver
    ? new ResizeObserver(() => {
      if (resizeTimer !== null) clearTimeoutImpl(resizeTimer);
      resizeTimer = setTimeoutImpl(() => {
        resizeTimer = null;
        if (disposed) return;
        const { cols, rows } = measure();
        if (cols === screen.cols && rows === screen.rows) return;
        // The remote program redraws after SIGWINCH, so a resized grid is
        // rebuilt rather than reflowed — reflow would only invent a layout the
        // program is about to replace.
        const scrollback = screen.scrollback;
        screen = createScreen(rows, cols);
        screen.scrollback = scrollback;
        onResize(cols, rows);
        scheduleRender();
      }, 150);
    })
    : null;
  observer?.observe(view);

  return {
    view,
    measure,
    get screen() {
      return screen;
    },
    reset(rows = screen.rows, cols = screen.cols) {
      screen = createScreen(rows, cols);
      scheduleRender();
    },
    write(base64) {
      let bytes;
      try {
        bytes = decodeBase64(String(base64 || ""));
      } catch (_) {
        return;
      }
      if (!bytes.length) return;
      feed(screen, decoder.decode(bytes, { stream: true }));
      scheduleRender();
    },
    focus() {
      view.focus();
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      observer?.disconnect();
      if (inputTimer !== null) clearTimeoutImpl(inputTimer);
      if (resizeTimer !== null) clearTimeoutImpl(resizeTimer);
    },
  };
}

// --- session ---------------------------------------------------------------

/**
 * Stream `provider`'s login on `node` into `surface`.
 *
 * `surface` is any element; a <pre> is created inside it and focused for
 * keystrokes. `onStatus(text, level)` reports connection state — the caller
 * owns how that reads in its own chrome. `close()` tells the node to end the
 * session and drops the socket; nothing survives it on either side.
 */
export function createAgentLoginTerminal({
  surface,
  node,
  provider,
  sessionToken,
  onStatus = () => {},
  onExit = () => {},
  channelFactory = null,
  requestAnimationFrameImpl = globalThis.requestAnimationFrame?.bind(globalThis),
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
} = {}) {
  const session = randomSessionId();
  let openTimer = null;
  let answered = false;
  let closed = false;
  let channel = null;

  const terminal = mountTerminalView({
    surface,
    label: `${provider} sign-in terminal on ${node}`,
    onData: (data) => send({ action: "input", data }),
    onResize: (cols, rows) => send({ action: "resize", cols, rows }),
    requestAnimationFrameImpl,
    setTimeoutImpl,
    clearTimeoutImpl,
  });

  const channelOptions = {
    sessionToken,
    // Ask to be accepted onto this machine's own relay object rather than the
    // account's. A managed mirror is its own node account and holds its socket
    // there, so a terminal that stayed on the account's object reached desktop
    // nodes only and left every mirror answering nothing at all.
    nodeName: node,
    onConnected: () => {
      // Re-announce on every (re)connect: the node keeps the pty alive across
      // a browser reconnect and replays what was drawn while we were gone, so
      // a dropped socket costs the redraw and not the sign-in.
      onStatus(`Connecting to ${node}…`, "pending");
      sendOpen();
    },
    onFrame: (frame) => {
      if (!frame || frame.type !== "terminal") return;
      if (frame.session !== session || frame.from !== "node") return;
      handleNodeFrame(frame);
    },
    onSignedOut: () => onStatus("Sign in again to continue.", "error"),
  };

  function send(frame) {
    if (!channel || closed) return false;
    return channel.send({ type: "terminal", session, node, ...frame });
  }

  function sendOpen() {
    const { cols, rows } = terminal.measure();
    terminal.reset(rows, cols);
    if (!send({ action: "open", provider, cols, rows })) return;
    armOpenTimer();
  }

  function armOpenTimer() {
    if (openTimer !== null) clearTimeoutImpl(openTimer);
    openTimer = setTimeoutImpl(() => {
      openTimer = null;
      if (answered || closed) return;
      onStatus(
        `${node} did not answer. It is reachable here only while it is ` +
          "online and configured to accept agent work from the website.",
        "error",
      );
    }, OPEN_TIMEOUT_MS);
  }

  function handleNodeFrame(frame) {
    answered = true;
    if (openTimer !== null) {
      clearTimeoutImpl(openTimer);
      openTimer = null;
    }
    if (frame.action === "ready") {
      onStatus(
        `Live on ${node}. Complete the provider's own sign-in below.`,
        "ok",
      );
      return;
    }
    if (frame.action === "output") {
      terminal.write(frame.data);
      return;
    }
    if (frame.action === "error") {
      onStatus(String(frame.message || "The node refused the session."), "error");
      return;
    }
    if (frame.action === "exit") {
      const code = Number(frame.code || 0);
      onStatus(
        code === 0
          ? "The sign-in command finished."
          : `The sign-in command exited with status ${code}.`,
        code === 0 ? "ok" : "error",
      );
      onExit(code);
    }
  }

  return {
    session,
    async start() {
      onStatus(`Connecting to ${node}…`, "pending");
      const factory = channelFactory ||
        (await import(ACCOUNT_CHANNEL)).createAccountEventChannel;
      if (closed) return;
      channel = factory(channelOptions);
      channel.start();
      if (!answered) armOpenTimer();
      terminal.focus();
    },
    focus() {
      terminal.focus();
    },
    close() {
      if (closed) return;
      send({ action: "close" });
      closed = true;
      terminal.dispose();
      if (openTimer !== null) clearTimeoutImpl(openTimer);
      channel?.dispose();
    },
  };
}

export function createDesktopControlTerminal({
  surface,
  connection,
  label = "Terminal",
  start = null,
  attach = "",
  detachOnClose = false,
  onStatus = () => {},
  onExit = () => {},
  requestAnimationFrameImpl = globalThis.requestAnimationFrame?.bind(globalThis),
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
} = {}) {
  let terminalId = String(attach || "");
  let starting = false;
  let early = [];
  let closed = false;
  let exited = false;
  const offs = [];

  function call(method, params) {
    if (!terminalId || closed || exited || !connection.isOpen) return;
    connection.request(method, { terminal: terminalId, ...params }).catch(() => {});
  }

  const terminal = mountTerminalView({
    surface,
    label,
    onData: (data) => call("terminal.input", { data }),
    onResize: (cols, rows) => call("terminal.resize", { cols, rows }),
    requestAnimationFrameImpl,
    setTimeoutImpl,
    clearTimeoutImpl,
  });

  function output(data) {
    if (data.reset) terminal.reset();
    terminal.write(data.data);
  }

  function exit(data) {
    exited = true;
    const code = Number(data.code ?? 0);
    onStatus(
      code === 0 ? "The command finished." : `The command exited with status ${code}.`,
      code === 0 ? "ok" : "error",
    );
    onExit(code);
  }

  function route(kind, data) {
    if (closed) return;
    const id = String(data?.terminal || "");
    if (!terminalId && starting) {
      early.push([kind, data]);
      return;
    }
    if (!id || id !== terminalId) return;
    if (kind === "output") output(data);
    else exit(data);
  }

  offs.push(connection.on("terminal.output", (data) => route("output", data)));
  offs.push(connection.on("terminal.exit", (data) => route("exit", data)));
  offs.push(connection.onClose(() => {
    if (!closed && !exited) onStatus("The channel to the desktop closed.", "error");
  }));

  return {
    get terminal() {
      return terminalId;
    },
    async start() {
      const { cols, rows } = terminal.measure();
      terminal.reset(rows, cols);
      starting = !terminalId;
      let result;
      try {
        result = terminalId
          ? await connection.request("terminal.attach", { terminal: terminalId, cols, rows })
          : await connection.request("terminal.start", { ...(start || {}), cols, rows });
      } finally {
        starting = false;
      }
      terminalId = String(result?.terminal || terminalId);
      if (closed) {
        if (terminalId && connection.isOpen) {
          connection.request(detachOnClose ? "terminal.detach" : "terminal.close", { terminal: terminalId })
            .catch(() => {});
        }
        return result;
      }
      if (result?.replay) terminal.write(result.replay);
      const queued = early;
      early = [];
      for (const [kind, data] of queued) route(kind, data);
      terminal.focus();
      return result;
    },
    focus() {
      terminal.focus();
    },
    close({ end = !detachOnClose } = {}) {
      if (closed) return;
      if (terminalId && !exited && connection.isOpen) {
        connection.request(end ? "terminal.close" : "terminal.detach", { terminal: terminalId })
          .catch(() => {});
      }
      closed = true;
      for (const off of offs) off();
      terminal.dispose();
    },
  };
}
