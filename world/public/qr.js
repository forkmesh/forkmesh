// ForkMesh's own QR-code generator — no third-party library.
//
// Scope: byte mode, error-correction level L, QR versions 1–5 (a single error
// block, which keeps placement simple). That covers up to 108 bytes — plenty
// for a "bitcoincash:<addr>?amount=..." URI. Exposes:
//   ForkMeshQR.toCanvas(text, pxSize) -> <canvas>
//   ForkMeshQR.render(text, containerEl, pxSize)
(function (global) {
  "use strict";

  // --- GF(256) tables (primitive polynomial 0x11d) --------------------------
  const EXP = new Array(512);
  const LOG = new Array(256);
  (function () {
    let x = 1;
    for (let i = 0; i < 255; i++) {
      EXP[i] = x;
      LOG[x] = i;
      x <<= 1;
      if (x & 0x100) x ^= 0x11d;
    }
    for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
  })();
  function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return EXP[LOG[a] + LOG[b]];
  }

  function rsGenPoly(degree) {
    let poly = [1];
    for (let i = 0; i < degree; i++) {
      const next = new Array(poly.length + 1).fill(0);
      for (let j = 0; j < poly.length; j++) {
        next[j] ^= poly[j]; // multiply by x
        next[j + 1] ^= gfMul(poly[j], EXP[i]); // times -α^i
      }
      poly = next;
    }
    return poly; // poly[0] === 1 (leading)
  }

  function rsEncode(data, ecLen) {
    const gen = rsGenPoly(ecLen);
    const res = data.slice().concat(new Array(ecLen).fill(0));
    for (let i = 0; i < data.length; i++) {
      const coef = res[i];
      if (coef !== 0)
        for (let j = 0; j < gen.length; j++)
          res[i + j] ^= gfMul(gen[j], coef);
    }
    return res.slice(data.length); // the ecLen remainder bytes
  }

  // --- Per-version capacity at EC level L -----------------------------------
  // { data: total data codewords, ec: EC codewords PER BLOCK, blocks }.
  // v1-5 are a single block; v6 (level L) is two equal blocks, so we interleave.
  // Versions >=7 are intentionally out of scope (they'd need version-info
  // modules and multiple alignment patterns).
  const CAP_L = {
    1: { data: 19, ec: 7, blocks: 1 },
    2: { data: 34, ec: 10, blocks: 1 },
    3: { data: 55, ec: 15, blocks: 1 },
    4: { data: 80, ec: 20, blocks: 1 },
    5: { data: 108, ec: 26, blocks: 1 },
    6: { data: 136, ec: 18, blocks: 2 },
  };
  // Single alignment-pattern center (only one for v2-6 at these sizes).
  const ALIGN = { 2: 18, 3: 22, 4: 26, 5: 30, 6: 34 };

  function chooseVersion(byteLen) {
    for (let v = 1; v <= 6; v++) {
      // 4-bit mode + 8-bit count + 8*len bits must fit in dataCodewords*8.
      if (4 + 8 + byteLen * 8 <= CAP_L[v].data * 8) return v;
    }
    throw new Error("data too long for QR v1–5");
  }

  // --- Bit stream -> data codewords -----------------------------------------
  function buildCodewords(bytes, version) {
    const cap = CAP_L[version];
    const bits = [];
    const push = (val, len) => {
      for (let i = len - 1; i >= 0; i--) bits.push((val >> i) & 1);
    };
    push(0b0100, 4); // byte mode
    push(bytes.length, 8); // char count (8 bits for v1–9)
    for (const b of bytes) push(b, 8);
    // Terminator (up to 4 zero bits), then pad to a byte boundary.
    const capBits = cap.data * 8;
    for (let i = 0; i < 4 && bits.length < capBits; i++) bits.push(0);
    while (bits.length % 8 !== 0) bits.push(0);
    const codewords = [];
    for (let i = 0; i < bits.length; i += 8) {
      let b = 0;
      for (let j = 0; j < 8; j++) b = (b << 1) | bits[i + j];
      codewords.push(b);
    }
    // Pad codewords with the standard 0xEC / 0x11 alternation.
    const pads = [0xec, 0x11];
    let p = 0;
    while (codewords.length < cap.data) codewords.push(pads[p++ % 2]);

    // Split into equal blocks, RS-encode each, then interleave data then EC.
    // For a single block (v1-5) this is just data followed by its EC bytes.
    const nBlocks = cap.blocks;
    const perBlock = cap.data / nBlocks;
    const dataBlocks = [];
    const ecBlocks = [];
    for (let b = 0; b < nBlocks; b++) {
      const block = codewords.slice(b * perBlock, (b + 1) * perBlock);
      dataBlocks.push(block);
      ecBlocks.push(rsEncode(block, cap.ec));
    }
    const out = [];
    for (let i = 0; i < perBlock; i++)
      for (let b = 0; b < nBlocks; b++) out.push(dataBlocks[b][i]);
    for (let i = 0; i < cap.ec; i++)
      for (let b = 0; b < nBlocks; b++) out.push(ecBlocks[b][i]);
    return out;
  }

  // --- Matrix ---------------------------------------------------------------
  function makeMatrix(version, allCodewords) {
    const size = 17 + 4 * version;
    const m = Array.from({ length: size }, () => new Array(size).fill(null));
    const reserved = Array.from({ length: size }, () =>
      new Array(size).fill(false));
    const set = (r, c, v) => {
      m[r][c] = v;
      reserved[r][c] = true;
    };

    function finder(r, c) {
      for (let dr = -1; dr <= 7; dr++)
        for (let dc = -1; dc <= 7; dc++) {
          const rr = r + dr, cc = c + dc;
          if (rr < 0 || rr >= size || cc < 0 || cc >= size) continue;
          const inRing =
            dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6 &&
            (dr === 0 || dr === 6 || dc === 0 || dc === 6);
          const inCore = dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4;
          set(rr, cc, inRing || inCore);
        }
    }
    finder(0, 0);
    finder(0, size - 7);
    finder(size - 7, 0);

    // Timing patterns.
    for (let i = 8; i < size - 8; i++) {
      if (!reserved[6][i]) set(6, i, i % 2 === 0);
      if (!reserved[i][6]) set(i, 6, i % 2 === 0);
    }
    // Dark module.
    set(4 * version + 9, 8, true);

    // Alignment pattern (v >= 2).
    if (ALIGN[version] !== undefined) {
      const ac = ALIGN[version];
      for (let dr = -2; dr <= 2; dr++)
        for (let dc = -2; dc <= 2; dc++) {
          const ring =
            Math.max(Math.abs(dr), Math.abs(dc)) !== 1; // center + outer ring
          set(ac + dr, ac + dc, ring);
        }
    }

    // Reserve the format-info regions (filled later, after masking).
    for (let i = 0; i < 9; i++) {
      if (!reserved[8][i]) reserved[8][i] = true;
      if (!reserved[i][8]) reserved[i][8] = true;
    }
    for (let i = 0; i < 8; i++) {
      reserved[8][size - 1 - i] = true;
      reserved[size - 1 - i][8] = true;
    }

    // Lay the data+EC bitstream in the upward/downward zigzag.
    const bits = [];
    for (const cw of allCodewords)
      for (let i = 7; i >= 0; i--) bits.push((cw >> i) & 1);
    let bi = 0;
    let upward = true;
    for (let col = size - 1; col > 0; col -= 2) {
      if (col === 6) col--; // skip the vertical timing column
      for (let i = 0; i < size; i++) {
        const row = upward ? size - 1 - i : i;
        for (let c = 0; c < 2; c++) {
          const cc = col - c;
          if (reserved[row][cc]) continue;
          m[row][cc] = bi < bits.length ? bits[bi++] === 1 : false;
        }
      }
      upward = !upward;
    }
    return { m, reserved, size };
  }

  const MASKS = [
    (r, c) => (r + c) % 2 === 0,
    (r, c) => r % 2 === 0,
    (r, c) => c % 3 === 0,
    (r, c) => (r + c) % 3 === 0,
    (r, c) => (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0,
    (r, c) => ((r * c) % 2) + ((r * c) % 3) === 0,
    (r, c) => (((r * c) % 2) + ((r * c) % 3)) % 2 === 0,
    (r, c) => (((r + c) % 2) + ((r * c) % 3)) % 2 === 0,
  ];

  function formatBits(mask) {
    // EC level L = 0b01.
    const data = (0b01 << 3) | mask;
    let d = data << 10;
    const g = 0b10100110111;
    for (let i = 14; i >= 10; i--) if ((d >> i) & 1) d ^= g << (i - 10);
    return ((data << 10) | d) ^ 0b101010000010010; // 15 bits, with mask
  }

  function applyFormat(m, size, mask) {
    const bits = formatBits(mask);
    const bit = (i) => ((bits >> i) & 1) === 1;
    // Around the top-left finder.
    for (let i = 0; i <= 5; i++) m[i][8] = bit(i);
    m[7][8] = bit(6);
    m[8][8] = bit(7);
    m[8][7] = bit(8);
    for (let i = 9; i <= 14; i++) m[8][14 - i] = bit(i);
    // Around the other two finders.
    for (let i = 0; i <= 7; i++) m[size - 1 - i][8] = bit(i);
    for (let i = 8; i <= 14; i++) m[8][size - 15 + i] = bit(i);
    m[size - 8][8] = true; // dark module stays set
  }

  function penalty(m, size) {
    let score = 0;
    // Rule 1: runs of 5+ same-color in rows and columns.
    for (let r = 0; r < size; r++) {
      for (const line of [m[r], m.map((row) => row[r])]) {
        let run = 1;
        for (let c = 1; c < size; c++) {
          if (line[c] === line[c - 1]) {
            run++;
            if (run === 5) score += 3;
            else if (run > 5) score += 1;
          } else run = 1;
        }
      }
    }
    // Rule 2: 2x2 blocks of same color.
    for (let r = 0; r < size - 1; r++)
      for (let c = 0; c < size - 1; c++) {
        const v = m[r][c];
        if (v === m[r][c + 1] && v === m[r + 1][c] && v === m[r + 1][c + 1])
          score += 3;
      }
    // Rule 3 (finder-like patterns) and Rule 4 (balance) are minor; we apply a
    // light balance penalty so the chosen mask isn't wildly skewed.
    let dark = 0;
    for (let r = 0; r < size; r++)
      for (let c = 0; c < size; c++) if (m[r][c]) dark++;
    const pct = (dark * 100) / (size * size);
    score += Math.floor(Math.abs(pct - 50) / 5) * 10;
    return score;
  }

  function generate(text) {
    const bytes = Array.from(new TextEncoder().encode(text));
    const version = chooseVersion(bytes.length);
    const codewords = buildCodewords(bytes, version);
    const base = makeMatrix(version, codewords);
    const size = base.size;

    let best = null;
    for (let mask = 0; mask < 8; mask++) {
      const m = base.m.map((row) => row.slice());
      for (let r = 0; r < size; r++)
        for (let c = 0; c < size; c++)
          if (!base.reserved[r][c] && MASKS[mask](r, c)) m[r][c] = !m[r][c];
      applyFormat(m, size, mask);
      const p = penalty(m, size);
      if (best === null || p < best.p) best = { m, p, mask };
    }
    return { modules: best.m, size };
  }

  function toCanvas(text, pxSize) {
    const { modules, size } = generate(text);
    const quiet = 4; // standard quiet-zone modules
    const total = size + quiet * 2;
    const scale = Math.max(1, Math.floor((pxSize || 220) / total));
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = total * scale;
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#000000";
    for (let r = 0; r < size; r++)
      for (let c = 0; c < size; c++)
        if (modules[r][c])
          ctx.fillRect((c + quiet) * scale, (r + quiet) * scale, scale, scale);
    return canvas;
  }

  function render(text, container, pxSize) {
    if (!container) return;
    container.innerHTML = "";
    try {
      container.appendChild(toCanvas(text, pxSize));
    } catch (e) {
      container.textContent = "(QR unavailable)";
    }
  }

  global.ForkMeshQR = { toCanvas, render, generate };
  // Internals exposed for the self-consistency test harness only.
  global.ForkMeshQR._test = {
    makeMatrix, buildCodewords, chooseVersion, rsEncode, MASKS, CAP_L, formatBits,
  };
})(typeof window !== "undefined" ? window : globalThis);
