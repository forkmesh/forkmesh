// ForkMesh Vault cryptography (suite "fmv1"). One file, no dependencies.
//
// Runs unchanged in a page, an extension service worker, and Node (tests).
// The platform's WebCrypto supplies AES-256-GCM, HKDF-SHA-256, X25519 and
// Ed25519. What WebCrypto does not offer is written out here and checked
// against OpenSSL 3.5 vectors (tests/fixtures/openssl-vectors.json):
//
//   SHA-3 / SHAKE   FIPS 202
//   ML-KEM-768      FIPS 203, the post-quantum half of every share
//   BLAKE2b         RFC 7693, only as Argon2's hash
//   Argon2id v1.3   RFC 9106, the master password stretch
//
// A share is sealed to X25519 and ML-KEM-768 together and the two secrets are
// joined with the X-Wing combiner (draft-connolly-cfrg-xwing-kem), so opening
// one takes breaking both. Signatures stay Ed25519: a forged signature needs
// a quantum computer at the moment of the forgery, a recorded ciphertext does
// not, so only confidentiality has to hold against a future machine.
//
// JavaScript cannot promise constant-time arithmetic. Secrets here never
// select a branch or a table index on purpose, but a JIT may still vary.
//
// The same bytes are embedded in content/web/page.js and copied into the
// browser extension; tools/sync_crypto.py keeps them identical.

(function (root) {
  const factory = forkMeshVaultCryptoFactory;
  const api = factory();
  // The factory's own text starts the key-stretch worker, so a page that may
  // only load one script still keeps Argon2 off its main thread.
  api.workerSource = "(" + factory.toString() + ")();";
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.ForkMeshVaultCrypto = api;

function forkMeshVaultCryptoFactory() {
  "use strict";

  const SUITE = "fmv1";
  const subtle = globalThis.crypto.subtle;
  const encoder = new TextEncoder();
  const decoder = new TextDecoder("utf-8", { fatal: true });

  // ---- bytes ---------------------------------------------------------------

  function utf8(text) { return encoder.encode(String(text)); }
  function text(bytes) { return decoder.decode(bytes); }
  function randomBytes(length) {
    const out = new Uint8Array(length);
    globalThis.crypto.getRandomValues(out);
    return out;
  }
  function concat(...parts) {
    let total = 0;
    for (const part of parts) total += part.length;
    const out = new Uint8Array(total);
    let at = 0;
    for (const part of parts) { out.set(part, at); at += part.length; }
    return out;
  }
  function toBase64(bytes) {
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }
  function fromBase64(value) {
    const clean = String(value || "");
    if (!/^[A-Za-z0-9_-]*$/.test(clean)) throw new VaultCryptoError("invalid_encoding");
    const binary = atob(clean.replace(/-/g, "+").replace(/_/g, "/"));
    const out = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
    return out;
  }
  function toHex(bytes) {
    let out = "";
    for (const byte of bytes) out += byte.toString(16).padStart(2, "0");
    return out;
  }
  function fromHex(value) {
    const clean = String(value || "");
    if (clean.length % 2 || !/^[0-9a-fA-F]*$/.test(clean)) throw new VaultCryptoError("invalid_encoding");
    const out = new Uint8Array(clean.length / 2);
    for (let i = 0; i < out.length; i += 1) out[i] = parseInt(clean.substr(i * 2, 2), 16);
    return out;
  }
  // Every byte is visited whatever the first difference is.
  function equalBytes(a, b) {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i += 1) diff |= a[i] ^ b[i];
    return diff === 0;
  }
  function wipe(...buffers) {
    for (const buffer of buffers) if (buffer && buffer.fill) buffer.fill(0);
  }

  class VaultCryptoError extends Error {
    constructor(code) {
      super(String(code).replaceAll("_", " "));
      this.name = "VaultCryptoError";
      this.code = String(code);
    }
  }

  // ---- SHA-3 and SHAKE (FIPS 202) -------------------------------------------

  const MASK64 = (1n << 64n) - 1n;
  const KECCAK_RC = (() => {
    // The round constants come from the degree-8 LFSR in FIPS 202 3.2.5.
    const constants = [];
    let lfsr = 1;
    for (let round = 0; round < 24; round += 1) {
      let value = 0n;
      for (let j = 0; j < 7; j += 1) {
        if (lfsr & 1) value |= 1n << BigInt((1 << j) - 1);
        lfsr = lfsr & 0x80 ? ((lfsr << 1) ^ 0x71) & 0xff : lfsr << 1;
      }
      constants.push(value);
    }
    return constants;
  })();

  function rotl64(value, shift) {
    const by = BigInt(shift);
    return by === 0n ? value : ((value << by) | (value >> (64n - by))) & MASK64;
  }

  function keccakF1600(state) {
    const c = new Array(5);
    for (let round = 0; round < 24; round += 1) {
      for (let x = 0; x < 5; x += 1) {
        c[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
      }
      for (let x = 0; x < 5; x += 1) {
        const d = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1);
        for (let y = 0; y < 25; y += 5) state[x + y] ^= d;
      }
      let x = 1, y = 0, current = state[1];
      for (let t = 0; t < 24; t += 1) {
        const nextX = y, nextY = (2 * x + 3 * y) % 5;
        x = nextX; y = nextY;
        const held = state[x + 5 * y];
        state[x + 5 * y] = rotl64(current, ((t + 1) * (t + 2) / 2) % 64);
        current = held;
      }
      for (let row = 0; row < 25; row += 5) {
        const a0 = state[row], a1 = state[row + 1], a2 = state[row + 2],
          a3 = state[row + 3], a4 = state[row + 4];
        state[row] = a0 ^ (~a1 & MASK64 & a2);
        state[row + 1] = a1 ^ (~a2 & MASK64 & a3);
        state[row + 2] = a2 ^ (~a3 & MASK64 & a4);
        state[row + 3] = a3 ^ (~a4 & MASK64 & a0);
        state[row + 4] = a4 ^ (~a0 & MASK64 & a1);
      }
      state[0] ^= KECCAK_RC[round];
    }
  }

  // A sponge that absorbs one message and then hands out bytes on demand.
  function sponge(rate, suffix, message) {
    const state = new Array(25).fill(0n);
    const block = new Uint8Array(rate);
    const absorbBlock = () => {
      for (let lane = 0; lane < rate / 8; lane += 1) {
        let word = 0n;
        for (let b = 7; b >= 0; b -= 1) word = (word << 8n) | BigInt(block[lane * 8 + b]);
        state[lane] ^= word;
      }
      keccakF1600(state);
    };
    let at = 0;
    for (; at + rate <= message.length; at += rate) {
      block.set(message.subarray(at, at + rate));
      absorbBlock();
    }
    block.fill(0);
    block.set(message.subarray(at));
    block[message.length - at] ^= suffix;
    block[rate - 1] ^= 0x80;
    absorbBlock();
    let offset = 0;
    const extract = () => {
      for (let lane = 0; lane < rate / 8; lane += 1) {
        let word = state[lane];
        for (let b = 0; b < 8; b += 1) { block[lane * 8 + b] = Number(word & 0xffn); word >>= 8n; }
      }
    };
    extract();
    return {
      read(length) {
        const out = new Uint8Array(length);
        let written = 0;
        while (written < length) {
          if (offset === rate) { keccakF1600(state); extract(); offset = 0; }
          const take = Math.min(rate - offset, length - written);
          out.set(block.subarray(offset, offset + take), written);
          offset += take; written += take;
        }
        return out;
      },
    };
  }

  function sha3_256(message) { return sponge(136, 0x06, message).read(32); }
  function sha3_512(message) { return sponge(72, 0x06, message).read(64); }
  function shake128(message, length) { return sponge(168, 0x1f, message).read(length); }
  function shake256(message, length) { return sponge(136, 0x1f, message).read(length); }

  // ---- ML-KEM-768 (FIPS 203) ------------------------------------------------

  const Q = 3329, N = 256, K = 3, ETA1 = 2, ETA2 = 2, DU = 10, DV = 4;
  const MLKEM_PUBLIC_BYTES = 384 * K + 32;          // 1184
  const MLKEM_SECRET_BYTES = 768 * K + 96;          // 2400
  const MLKEM_CIPHERTEXT_BYTES = 32 * (DU * K + DV); // 1088

  function bitReverse7(value) {
    let out = 0;
    for (let bit = 0; bit < 7; bit += 1) out |= ((value >> bit) & 1) << (6 - bit);
    return out;
  }
  function powMod(base, exponent) {
    let result = 1, b = base % Q;
    for (let e = exponent; e > 0; e >>= 1) {
      if (e & 1) result = (result * b) % Q;
      b = (b * b) % Q;
    }
    return result;
  }
  const ZETAS = Array.from({ length: 128 }, (_, i) => powMod(17, bitReverse7(i)));
  const GAMMAS = Array.from({ length: 128 }, (_, i) => powMod(17, 2 * bitReverse7(i) + 1));

  function ntt(poly) {
    const f = Int32Array.from(poly);
    let i = 1;
    for (let len = 128; len >= 2; len >>= 1) {
      for (let start = 0; start < N; start += 2 * len) {
        const zeta = ZETAS[i]; i += 1;
        for (let j = start; j < start + len; j += 1) {
          const t = (zeta * f[j + len]) % Q;
          f[j + len] = (f[j] - t + Q) % Q;
          f[j] = (f[j] + t) % Q;
        }
      }
    }
    return f;
  }
  function inverseNtt(poly) {
    const f = Int32Array.from(poly);
    let i = 127;
    for (let len = 2; len <= 128; len <<= 1) {
      for (let start = 0; start < N; start += 2 * len) {
        const zeta = ZETAS[i]; i -= 1;
        for (let j = start; j < start + len; j += 1) {
          const t = f[j];
          f[j] = (t + f[j + len]) % Q;
          f[j + len] = (zeta * ((f[j + len] - t + Q) % Q)) % Q;
        }
      }
    }
    for (let j = 0; j < N; j += 1) f[j] = (f[j] * 3303) % Q;
    return f;
  }
  function multiplyNtts(f, g) {
    const h = new Int32Array(N);
    for (let i = 0; i < 128; i += 1) {
      const a0 = f[2 * i], a1 = f[2 * i + 1], b0 = g[2 * i], b1 = g[2 * i + 1];
      h[2 * i] = (a0 * b0 + ((a1 * b1) % Q) * GAMMAS[i]) % Q;
      h[2 * i + 1] = (a0 * b1 + a1 * b0) % Q;
    }
    return h;
  }
  function addPoly(f, g) {
    const h = new Int32Array(N);
    for (let i = 0; i < N; i += 1) h[i] = (f[i] + g[i]) % Q;
    return h;
  }
  function subPoly(f, g) {
    const h = new Int32Array(N);
    for (let i = 0; i < N; i += 1) h[i] = (f[i] - g[i] + Q) % Q;
    return h;
  }

  function byteEncode(poly, d) {
    const out = new Uint8Array(32 * d);
    let bitAt = 0;
    for (let i = 0; i < N; i += 1) {
      for (let bit = 0; bit < d; bit += 1, bitAt += 1) {
        out[bitAt >> 3] |= ((poly[i] >> bit) & 1) << (bitAt & 7);
      }
    }
    return out;
  }
  function byteDecode(bytes, d) {
    const poly = new Int32Array(N);
    let bitAt = 0;
    for (let i = 0; i < N; i += 1) {
      let value = 0;
      for (let bit = 0; bit < d; bit += 1, bitAt += 1) {
        value |= ((bytes[bitAt >> 3] >> (bitAt & 7)) & 1) << bit;
      }
      poly[i] = value;
    }
    return poly;
  }
  function compress(poly, d) {
    const out = new Int32Array(N);
    const modulus = 1 << d;
    for (let i = 0; i < N; i += 1) {
      out[i] = Math.floor((2 * poly[i] * modulus + Q) / (2 * Q)) % modulus;
    }
    return out;
  }
  function decompress(poly, d) {
    const out = new Int32Array(N);
    const half = 1 << (d - 1);
    for (let i = 0; i < N; i += 1) out[i] = (poly[i] * Q + half) >> d;
    return out;
  }

  function sampleNtt(rho, j, i) {
    const xof = sponge(168, 0x1f, concat(rho, Uint8Array.of(j, i)));
    const poly = new Int32Array(N);
    let filled = 0;
    while (filled < N) {
      const c = xof.read(3);
      const d1 = c[0] + 256 * (c[1] & 15);
      const d2 = (c[1] >> 4) + 16 * c[2];
      if (d1 < Q) { poly[filled] = d1; filled += 1; }
      if (d2 < Q && filled < N) { poly[filled] = d2; filled += 1; }
    }
    return poly;
  }
  function samplePolyCbd(seed, counter, eta) {
    const bytes = shake256(concat(seed, Uint8Array.of(counter)), 64 * eta);
    const bitAt = (index) => (bytes[index >> 3] >> (index & 7)) & 1;
    const poly = new Int32Array(N);
    for (let i = 0; i < N; i += 1) {
      let x = 0, y = 0;
      for (let j = 0; j < eta; j += 1) {
        x += bitAt(2 * i * eta + j);
        y += bitAt(2 * i * eta + eta + j);
      }
      poly[i] = (x - y + Q) % Q;
    }
    wipe(bytes);
    return poly;
  }
  function expandMatrix(rho) {
    const matrix = [];
    for (let i = 0; i < K; i += 1) {
      matrix.push([]);
      for (let j = 0; j < K; j += 1) matrix[i].push(sampleNtt(rho, j, i));
    }
    return matrix;
  }

  function pkeKeyGen(d) {
    // The module rank rides along as a domain separator (checked against
    // OpenSSL 3.5 and Go's crypto/mlkem, which agree on every seed).
    const g = sha3_512(concat(d, Uint8Array.of(K)));
    const rho = g.slice(0, 32), sigma = g.slice(32);
    const matrix = expandMatrix(rho);
    const s = [], e = [];
    for (let i = 0; i < K; i += 1) s.push(ntt(samplePolyCbd(sigma, i, ETA1)));
    for (let i = 0; i < K; i += 1) e.push(ntt(samplePolyCbd(sigma, K + i, ETA1)));
    const parts = [], secretParts = [];
    for (let i = 0; i < K; i += 1) {
      let t = e[i];
      for (let j = 0; j < K; j += 1) t = addPoly(t, multiplyNtts(matrix[i][j], s[j]));
      parts.push(byteEncode(t, 12));
      secretParts.push(byteEncode(s[i], 12));
    }
    wipe(g, sigma);
    return { ek: concat(...parts, rho), dk: concat(...secretParts) };
  }
  function pkeEncrypt(ek, message, randomness) {
    const t = [];
    for (let i = 0; i < K; i += 1) t.push(byteDecode(ek.subarray(384 * i, 384 * (i + 1)), 12));
    const matrix = expandMatrix(ek.subarray(384 * K));
    const y = [], e1 = [];
    for (let i = 0; i < K; i += 1) y.push(ntt(samplePolyCbd(randomness, i, ETA1)));
    for (let i = 0; i < K; i += 1) e1.push(samplePolyCbd(randomness, K + i, ETA2));
    const e2 = samplePolyCbd(randomness, 2 * K, ETA2);
    const c1 = [];
    let v = new Int32Array(N);
    for (let i = 0; i < K; i += 1) {
      let u = new Int32Array(N);
      for (let j = 0; j < K; j += 1) u = addPoly(u, multiplyNtts(matrix[j][i], y[j]));
      c1.push(byteEncode(compress(addPoly(inverseNtt(u), e1[i]), DU), DU));
      v = addPoly(v, multiplyNtts(t[i], y[i]));
    }
    const mu = decompress(byteDecode(message, 1), 1);
    const c2 = byteEncode(compress(addPoly(addPoly(inverseNtt(v), e2), mu), DV), DV);
    return concat(...c1, c2);
  }
  function pkeDecrypt(dk, ciphertext) {
    let inner = new Int32Array(N);
    for (let i = 0; i < K; i += 1) {
      const u = decompress(byteDecode(ciphertext.subarray(32 * DU * i, 32 * DU * (i + 1)), DU), DU);
      const s = byteDecode(dk.subarray(384 * i, 384 * (i + 1)), 12);
      inner = addPoly(inner, multiplyNtts(s, ntt(u)));
    }
    const v = decompress(byteDecode(ciphertext.subarray(32 * DU * K), DV), DV);
    return byteEncode(compress(subPoly(v, inverseNtt(inner)), 1), 1);
  }

  // seed is d || z (64 bytes), the form FIPS 203 and OpenSSL both store.
  function mlkemKeyGen(seed) {
    if (!(seed instanceof Uint8Array) || seed.length !== 64) throw new VaultCryptoError("invalid_mlkem_seed");
    const pke = pkeKeyGen(seed.subarray(0, 32));
    const dk = concat(pke.dk, pke.ek, sha3_256(pke.ek), seed.subarray(32));
    wipe(pke.dk);
    return { publicKey: pke.ek, secretKey: dk };
  }
  function mlkemCheckPublicKey(ek) {
    if (!(ek instanceof Uint8Array) || ek.length !== MLKEM_PUBLIC_BYTES) {
      throw new VaultCryptoError("invalid_mlkem_public_key");
    }
    // FIPS 203 7.2 modulus check: every coefficient is already reduced.
    for (let i = 0; i < K; i += 1) {
      const slice = ek.subarray(384 * i, 384 * (i + 1));
      if (!equalBytes(byteEncode(byteDecode(slice, 12).map((c) => c % Q), 12), slice)) {
        throw new VaultCryptoError("invalid_mlkem_public_key");
      }
    }
  }
  function mlkemEncapsulate(ek, message = randomBytes(32)) {
    mlkemCheckPublicKey(ek);
    const g = sha3_512(concat(message, sha3_256(ek)));
    const ciphertext = pkeEncrypt(ek, message, g.subarray(32));
    const sharedSecret = g.slice(0, 32);
    wipe(g, message);
    return { ciphertext, sharedSecret };
  }
  function mlkemDecapsulate(dk, ciphertext) {
    if (!(dk instanceof Uint8Array) || dk.length !== MLKEM_SECRET_BYTES) {
      throw new VaultCryptoError("invalid_mlkem_secret_key");
    }
    if (!(ciphertext instanceof Uint8Array) || ciphertext.length !== MLKEM_CIPHERTEXT_BYTES) {
      throw new VaultCryptoError("invalid_mlkem_ciphertext");
    }
    const dkPke = dk.subarray(0, 384 * K);
    const ek = dk.subarray(384 * K, 768 * K + 32);
    const h = dk.subarray(768 * K + 32, 768 * K + 64);
    const z = dk.subarray(768 * K + 64);
    // FIPS 203 7.3 hash check: the stored H(ek) belongs to the stored ek.
    if (!equalBytes(sha3_256(ek), h)) throw new VaultCryptoError("invalid_mlkem_secret_key");
    const message = pkeDecrypt(dkPke, ciphertext);
    const g = sha3_512(concat(message, h));
    const rejected = shake256(concat(z, ciphertext), 32);
    const again = pkeEncrypt(ek, message, g.subarray(32));
    // Implicit rejection: a tampered ciphertext yields a key nobody knows,
    // chosen by mask rather than by branch.
    let diff = 0;
    for (let i = 0; i < again.length; i += 1) diff |= again[i] ^ ciphertext[i];
    const keep = ((diff | -diff) >> 31) & 0xff;   // 0x00 when equal, 0xff when not
    const out = new Uint8Array(32);
    for (let i = 0; i < 32; i += 1) out[i] = (g[i] & ~keep) | (rejected[i] & keep);
    wipe(message, g, rejected);
    return out;
  }

  // ---- 64-bit words as two Uint32 halves -------------------------------------
  // BLAKE2b and Argon2 live on 64-bit words; BigInt is far too slow for the
  // ~10^8 operations a 64 MiB Argon2 pass needs, so words sit in Uint32Array
  // pairs: index 2i is the low half, 2i+1 the high half.

  const BLAKE2B_IV = new Uint32Array([
    0xf3bcc908, 0x6a09e667, 0x84caa73b, 0xbb67ae85, 0xfe94f82b, 0x3c6ef372,
    0x5f1d36f1, 0xa54ff53a, 0xade682d1, 0x510e527f, 0x2b3e6c1f, 0x9b05688c,
    0xfb41bd6b, 0x1f83d9ab, 0x137e2179, 0x5be0cd19,
  ]);
  const BLAKE2B_SIGMA = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
    [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
    [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
    [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
    [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
  ];

  function blake2bCompress(h, block, counter, last) {
    const v = new Uint32Array(32);
    const m = new Uint32Array(32);
    v.set(h); v.set(BLAKE2B_IV, 16);
    v[24] ^= counter >>> 0;
    v[25] ^= Math.floor(counter / 0x100000000) >>> 0;
    if (last) { v[28] = ~v[28]; v[29] = ~v[29]; }
    for (let i = 0; i < 32; i += 1) {
      m[i] = block[i * 4] | (block[i * 4 + 1] << 8) | (block[i * 4 + 2] << 16) | (block[i * 4 + 3] << 24);
    }
    const mix = (a, b, c, d, x, y) => {
      let lo, hi, tl, th;
      // a = a + b + x
      lo = (v[a] >>> 0) + (v[b] >>> 0) + (m[x] >>> 0);
      hi = (v[a + 1] >>> 0) + (v[b + 1] >>> 0) + (m[x + 1] >>> 0) + Math.floor(lo / 0x100000000);
      v[a] = lo; v[a + 1] = hi;
      // d = rotr(d ^ a, 32)
      tl = v[d] ^ v[a]; th = v[d + 1] ^ v[a + 1];
      v[d] = th; v[d + 1] = tl;
      // c = c + d
      lo = (v[c] >>> 0) + (v[d] >>> 0);
      hi = (v[c + 1] >>> 0) + (v[d + 1] >>> 0) + Math.floor(lo / 0x100000000);
      v[c] = lo; v[c + 1] = hi;
      // b = rotr(b ^ c, 24)
      tl = v[b] ^ v[c]; th = v[b + 1] ^ v[c + 1];
      v[b] = (tl >>> 24) | (th << 8); v[b + 1] = (th >>> 24) | (tl << 8);
      // a = a + b + y
      lo = (v[a] >>> 0) + (v[b] >>> 0) + (m[y] >>> 0);
      hi = (v[a + 1] >>> 0) + (v[b + 1] >>> 0) + (m[y + 1] >>> 0) + Math.floor(lo / 0x100000000);
      v[a] = lo; v[a + 1] = hi;
      // d = rotr(d ^ a, 16)
      tl = v[d] ^ v[a]; th = v[d + 1] ^ v[a + 1];
      v[d] = (tl >>> 16) | (th << 16); v[d + 1] = (th >>> 16) | (tl << 16);
      // c = c + d
      lo = (v[c] >>> 0) + (v[d] >>> 0);
      hi = (v[c + 1] >>> 0) + (v[d + 1] >>> 0) + Math.floor(lo / 0x100000000);
      v[c] = lo; v[c + 1] = hi;
      // b = rotr(b ^ c, 63)
      tl = v[b] ^ v[c]; th = v[b + 1] ^ v[c + 1];
      v[b] = (tl << 1) | (th >>> 31); v[b + 1] = (th << 1) | (tl >>> 31);
    };
    for (let round = 0; round < 12; round += 1) {
      const s = BLAKE2B_SIGMA[round];
      mix(0, 8, 16, 24, 2 * s[0], 2 * s[1]);
      mix(2, 10, 18, 26, 2 * s[2], 2 * s[3]);
      mix(4, 12, 20, 28, 2 * s[4], 2 * s[5]);
      mix(6, 14, 22, 30, 2 * s[6], 2 * s[7]);
      mix(0, 10, 20, 30, 2 * s[8], 2 * s[9]);
      mix(2, 12, 22, 24, 2 * s[10], 2 * s[11]);
      mix(4, 14, 16, 26, 2 * s[12], 2 * s[13]);
      mix(6, 8, 18, 28, 2 * s[14], 2 * s[15]);
    }
    for (let i = 0; i < 16; i += 1) h[i] ^= v[i] ^ v[i + 16];
    wipe(v, m);
  }

  function blake2b(message, outLength) {
    if (outLength < 1 || outLength > 64) throw new VaultCryptoError("invalid_hash_length");
    const h = Uint32Array.from(BLAKE2B_IV);
    h[0] ^= 0x01010000 ^ outLength;
    const block = new Uint8Array(128);
    let at = 0;
    while (message.length - at > 128) {
      block.set(message.subarray(at, at + 128));
      at += 128;
      blake2bCompress(h, block, at, false);
    }
    block.fill(0);
    block.set(message.subarray(at));
    blake2bCompress(h, block, message.length, true);
    const out = new Uint8Array(64);
    for (let i = 0; i < 16; i += 1) {
      out[i * 4] = h[i]; out[i * 4 + 1] = h[i] >>> 8;
      out[i * 4 + 2] = h[i] >>> 16; out[i * 4 + 3] = h[i] >>> 24;
    }
    wipe(block);
    return out.slice(0, outLength);
  }

  // ---- Argon2id v1.3 (RFC 9106) ---------------------------------------------

  function le32(value) {
    return Uint8Array.of(value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff);
  }
  // H' of RFC 9106 3.3: BLAKE2b stretched to any length.
  function argon2Hash(input, length) {
    const prefixed = concat(le32(length), input);
    if (length <= 64) return blake2b(prefixed, length);
    const out = new Uint8Array(length);
    const rounds = Math.ceil(length / 32) - 2;
    let v = blake2b(prefixed, 64);
    out.set(v.subarray(0, 32), 0);
    for (let i = 1; i < rounds; i += 1) {
      v = blake2b(v, 64);
      out.set(v.subarray(0, 32), 32 * i);
    }
    // The tail is a BLAKE2b of the remaining length, not a truncated 64.
    out.set(blake2b(v, length - 32 * rounds), 32 * rounds);
    return out;
  }

  // One Argon2 block is 128 words = 256 Uint32 halves.
  function argon2Permute(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) {
    argon2Mix(v, a, e, i, m); argon2Mix(v, b, f, j, n);
    argon2Mix(v, c, g, k, o); argon2Mix(v, d, h, l, p);
    argon2Mix(v, a, f, k, p); argon2Mix(v, b, g, l, m);
    argon2Mix(v, c, h, i, n); argon2Mix(v, d, e, j, o);
  }
  // a + b + 2 * lo(a) * lo(b) over 64 bits, written into v[a].
  function argon2Add(v, a, b) {
    const x = v[a], y = v[b];
    const x0 = x & 0xffff, x1 = x >>> 16, y0 = y & 0xffff, y1 = y >>> 16;
    // 32x32 -> 64 from 16-bit limbs; the low half is what Math.imul keeps.
    const cross = x1 * y0 + x0 * y1 + ((x0 * y0) >>> 16);   // below 2^34, exact
    const productLo = Math.imul(x, y) >>> 0;
    const productHi = x1 * y1 + Math.floor(cross / 0x10000);   // below 2^32
    const lo = x + y + ((productLo << 1) >>> 0);
    const hi = v[a + 1] + v[b + 1] + (((productHi << 1) | (productLo >>> 31)) >>> 0)
      + (lo >= 0x200000000 ? 2 : lo >= 0x100000000 ? 1 : 0);
    v[a] = lo; v[a + 1] = hi;
  }
  function argon2Mix(v, a, b, c, d) {
    let tl, th;
    argon2Add(v, a, b);
    tl = v[d] ^ v[a]; th = v[d + 1] ^ v[a + 1];
    v[d] = th; v[d + 1] = tl;
    argon2Add(v, c, d);
    tl = v[b] ^ v[c]; th = v[b + 1] ^ v[c + 1];
    v[b] = (tl >>> 24) | (th << 8); v[b + 1] = (th >>> 24) | (tl << 8);
    argon2Add(v, a, b);
    tl = v[d] ^ v[a]; th = v[d + 1] ^ v[a + 1];
    v[d] = (tl >>> 16) | (th << 16); v[d + 1] = (th >>> 16) | (tl << 16);
    argon2Add(v, c, d);
    tl = v[b] ^ v[c]; th = v[b + 1] ^ v[c + 1];
    v[b] = (tl << 1) | (th >>> 31); v[b + 1] = (th << 1) | (tl >>> 31);
  }

  const ARGON2_BLOCK = 256;   // Uint32 halves per 1 KiB block
  // G of RFC 9106 3.5: R = X xor Y, Z = P(R) by rows then columns, out = Z xor R,
  // xored over what was there before on every pass after the first.
  function argon2Compress(memory, x, y, target, xorIn, r, z) {
    for (let i = 0; i < ARGON2_BLOCK; i += 1) r[i] = memory[x + i] ^ memory[y + i];
    z.set(r);
    for (let row = 0; row < 8; row += 1) {
      const o = row * 32;
      argon2Permute(z, o, o + 2, o + 4, o + 6, o + 8, o + 10, o + 12, o + 14,
        o + 16, o + 18, o + 20, o + 22, o + 24, o + 26, o + 28, o + 30);
    }
    for (let column = 0; column < 8; column += 1) {
      const o = column * 4;
      argon2Permute(z, o, o + 2, o + 32, o + 34, o + 64, o + 66, o + 96, o + 98,
        o + 128, o + 130, o + 160, o + 162, o + 192, o + 194, o + 224, o + 226);
    }
    if (xorIn) for (let i = 0; i < ARGON2_BLOCK; i += 1) memory[target + i] ^= z[i] ^ r[i];
    else for (let i = 0; i < ARGON2_BLOCK; i += 1) memory[target + i] = z[i] ^ r[i];
  }

  // Blocks into memory: bytes are little-endian words, so a Uint32 view of
  // the same buffer is the halves array on every platform JS runs on.
  function argon2id(password, salt, { memoryKiB, passes, parallelism = 1, length = 32 }) {
    if (!(password instanceof Uint8Array) || !(salt instanceof Uint8Array) || salt.length < 8) {
      throw new VaultCryptoError("invalid_kdf_input");
    }
    const lanes = parallelism;
    if (!Number.isInteger(lanes) || lanes < 1 || lanes > 16
        || !Number.isInteger(passes) || passes < 1 || passes > 64
        || !Number.isInteger(memoryKiB) || memoryKiB < 8 * lanes || memoryKiB > 1048576
        || !Number.isInteger(length) || length < 4 || length > 1024) {
      throw new VaultCryptoError("invalid_kdf_parameters");
    }
    const segmentLength = Math.floor(memoryKiB / (4 * lanes));
    const laneLength = segmentLength * 4;
    const blocks = laneLength * lanes;
    const h0 = blake2b(concat(
      le32(lanes), le32(length), le32(memoryKiB), le32(passes), le32(0x13), le32(2),
      le32(password.length), password, le32(salt.length), salt, le32(0), le32(0)), 64);
    const memory = new Uint32Array(blocks * ARGON2_BLOCK);
    const loadBlock = (index, bytes) => {
      const view = new DataView(bytes.buffer, bytes.byteOffset, 1024);
      for (let i = 0; i < ARGON2_BLOCK; i += 1) memory[index * ARGON2_BLOCK + i] = view.getUint32(i * 4, true);
    };
    for (let lane = 0; lane < lanes; lane += 1) {
      loadBlock(lane * laneLength, argon2Hash(concat(h0, le32(0), le32(lane)), 1024));
      loadBlock(lane * laneLength + 1, argon2Hash(concat(h0, le32(1), le32(lane)), 1024));
    }
    const r = new Uint32Array(ARGON2_BLOCK), z = new Uint32Array(ARGON2_BLOCK);
    // Scratch for the data-independent address stream: [zero, input, address].
    const scratch = new Uint32Array(3 * ARGON2_BLOCK);
    const nextAddresses = () => {
      scratch[ARGON2_BLOCK + 12] += 1;     // the counter word; never reaches 2^32
      argon2Compress(scratch, 0, ARGON2_BLOCK, 2 * ARGON2_BLOCK, false, r, z);
      argon2Compress(scratch, 0, 2 * ARGON2_BLOCK, 2 * ARGON2_BLOCK, false, r, z);
    };
    for (let pass = 0; pass < passes; pass += 1) {
      for (let slice = 0; slice < 4; slice += 1) {
        // Argon2id: the first half of the first pass is data-independent.
        const independent = pass === 0 && slice < 2;
        for (let lane = 0; lane < lanes; lane += 1) {
          if (independent) {
            scratch.fill(0);
            scratch[ARGON2_BLOCK] = pass; scratch[ARGON2_BLOCK + 2] = lane;
            scratch[ARGON2_BLOCK + 4] = slice; scratch[ARGON2_BLOCK + 6] = blocks;
            scratch[ARGON2_BLOCK + 8] = passes; scratch[ARGON2_BLOCK + 10] = 2;
          }
          const first = pass === 0 && slice === 0 ? 2 : 0;
          if (independent && first) nextAddresses();
          for (let index = first; index < segmentLength; index += 1) {
            const column = slice * segmentLength + index;
            const current = lane * laneLength + column;
            const previous = column === 0 ? current + laneLength - 1 : current - 1;
            let j1, j2;
            if (independent) {
              if (index % 128 === 0) nextAddresses();
              j1 = scratch[2 * ARGON2_BLOCK + 2 * (index % 128)];
              j2 = scratch[2 * ARGON2_BLOCK + 2 * (index % 128) + 1];
            } else {
              j1 = memory[previous * ARGON2_BLOCK];
              j2 = memory[previous * ARGON2_BLOCK + 1];
            }
            const refLane = pass === 0 && slice === 0 ? lane : j2 % lanes;
            let area;
            if (pass === 0) {
              area = refLane === lane ? column - 1
                : slice * segmentLength - (index === 0 ? 1 : 0);
            } else {
              area = refLane === lane ? laneLength - segmentLength + index - 1
                : laneLength - segmentLength - (index === 0 ? 1 : 0);
            }
            // area - 1 - floor(area * floor(j1^2 / 2^32) / 2^32), in exact parts.
            const j1Lo = j1 & 0xffff, j1Hi = j1 >>> 16;
            const squareHigh = j1Hi * j1Hi + Math.floor((2 * j1Hi * j1Lo + Math.floor((j1Lo * j1Lo) / 0x10000)) / 0x10000);
            const xLo = squareHigh % 0x10000, xHi = Math.floor(squareHigh / 0x10000);
            const scaled = area * xHi + Math.floor((area * xLo) / 0x10000);
            const relative = area - 1 - Math.floor(scaled / 0x10000);
            const start = pass === 0 || slice === 3 ? 0 : (slice + 1) * segmentLength;
            const reference = refLane * laneLength + (start + relative) % laneLength;
            argon2Compress(memory, previous * ARGON2_BLOCK, reference * ARGON2_BLOCK,
              current * ARGON2_BLOCK, pass > 0, r, z);
          }
        }
      }
    }
    const last = new Uint32Array(ARGON2_BLOCK);
    for (let lane = 0; lane < lanes; lane += 1) {
      const at = (lane * laneLength + laneLength - 1) * ARGON2_BLOCK;
      for (let i = 0; i < ARGON2_BLOCK; i += 1) last[i] ^= memory[at + i];
    }
    const lastBytes = new Uint8Array(1024);
    const view = new DataView(lastBytes.buffer);
    for (let i = 0; i < ARGON2_BLOCK; i += 1) view.setUint32(i * 4, last[i], true);
    const out = argon2Hash(lastBytes, length);
    wipe(memory, last, lastBytes, scratch, r, z, h0);
    return out;
  }

  // ---- WebCrypto helpers ------------------------------------------------------

  const CONTEXT = "forkmesh-vault/" + SUITE + "/";
  const PKCS8_X25519 = fromHex("302e020100300506032b656e04220420");
  const PKCS8_ED25519 = fromHex("302e020100300506032b657004220420");
  // XWingLabel, the six bytes \.//^\ that close the X-Wing combiner input.
  const XWING_LABEL = fromHex("5c2e2f2f5e5c");

  async function hkdf(secret, info, length = 32) {
    const key = await subtle.importKey("raw", secret, "HKDF", false, ["deriveBits"]);
    return new Uint8Array(await subtle.deriveBits(
      { name: "HKDF", hash: "SHA-256", salt: new Uint8Array(32), info: utf8(CONTEXT + info) },
      key, length * 8));
  }
  async function aesKey(raw) {
    if (!(raw instanceof Uint8Array) || raw.length !== 32) throw new VaultCryptoError("invalid_key");
    return subtle.importKey("raw", raw, "AES-GCM", false, ["encrypt", "decrypt"]);
  }
  // iv || ciphertext || tag, base64url. `context` is bound as associated data,
  // so a blob moved to another row, item, or account fails to open.
  async function seal(rawKey, context, plaintext) {
    const iv = randomBytes(12);
    const sealed = new Uint8Array(await subtle.encrypt(
      { name: "AES-GCM", iv, additionalData: utf8(CONTEXT + context), tagLength: 128 },
      await aesKey(rawKey), plaintext));
    return toBase64(concat(iv, sealed));
  }
  async function open(rawKey, context, blob) {
    const bytes = fromBase64(blob);
    if (bytes.length < 12 + 16) throw new VaultCryptoError("invalid_ciphertext");
    try {
      return new Uint8Array(await subtle.decrypt(
        { name: "AES-GCM", iv: bytes.subarray(0, 12), additionalData: utf8(CONTEXT + context), tagLength: 128 },
        await aesKey(rawKey), bytes.subarray(12)));
    } catch {
      throw new VaultCryptoError("decryption_failed");
    }
  }
  function cleanName(value) {
    const name = String(value || "").trim().toLowerCase();
    if (!/^[a-z0-9](?:[a-z0-9-]{0,62})$/.test(name)) throw new VaultCryptoError("invalid_account");
    return name;
  }
  function cleanId(value) {
    const id = String(value || "");
    if (!/^[0-9a-f]{32}$/.test(id)) throw new VaultCryptoError("invalid_item_id");
    return id;
  }

  // ---- master password ---------------------------------------------------------

  // RFC 9106's second recommended setting. The floor below is the recommended minimum:
  // a server that hands back weaker numbers is refused, never obeyed.
  const KDF_DEFAULT = Object.freeze({ memoryKiB: 65536, passes: 3, parallelism: 1 });
  const KDF_FLOOR = Object.freeze({ memoryKiB: 19456, passes: 2 });
  const KDF_CEILING = Object.freeze({ memoryKiB: 1048576, passes: 16, parallelism: 16 });

  function newKdf() {
    return { algorithm: "argon2id", version: 19, ...KDF_DEFAULT, salt: toBase64(randomBytes(16)) };
  }
  function checkKdf(kdf) {
    const value = kdf || {};
    const salt = fromBase64(value.salt);
    if (value.algorithm !== "argon2id" || value.version !== 19 || salt.length < 16 || salt.length > 64
        || !Number.isInteger(value.memoryKiB) || !Number.isInteger(value.passes)
        || !Number.isInteger(value.parallelism)
        || value.memoryKiB < KDF_FLOOR.memoryKiB || value.memoryKiB > KDF_CEILING.memoryKiB
        || value.passes < KDF_FLOOR.passes || value.passes > KDF_CEILING.passes
        || value.parallelism < 1 || value.parallelism > KDF_CEILING.parallelism) {
      throw new VaultCryptoError("unsafe_kdf_parameters");
    }
    return { memoryKiB: value.memoryKiB, passes: value.passes, parallelism: value.parallelism, salt };
  }
  function stretchNow(password, kdf) {
    const checked = checkKdf(kdf);
    const secret = utf8(String(password).normalize("NFKC"));
    const out = argon2id(secret, checked.salt, { ...checked, length: 32 });
    wipe(secret);
    return out;
  }
  // The stretch takes seconds, so it runs in a worker wherever one can start.
  // `workerUrl` names a packaged copy of this file (an extension page, whose
  // policy forbids blob scripts); a page falls back to a blob of workerSource.
  function stretch(password, kdf, { workerUrl = "" } = {}) {
    checkKdf(kdf);
    if (typeof Worker !== "function" || typeof URL === "undefined") {
      return Promise.resolve().then(() => stretchNow(password, kdf));
    }
    return new Promise((resolve, reject) => {
      let worker, blobUrl = "";
      const finish = (settle, value) => {
        if (worker) worker.terminate();
        if (blobUrl) URL.revokeObjectURL(blobUrl);
        settle(value);
      };
      try {
        if (!workerUrl) {
          blobUrl = URL.createObjectURL(new Blob([api.workerSource], { type: "text/javascript" }));
        }
        worker = new Worker(workerUrl || blobUrl, { name: KDF_WORKER });
      } catch {
        if (blobUrl) URL.revokeObjectURL(blobUrl);
        resolve(stretchNow(password, kdf));
        return;
      }
      worker.onmessage = (event) => {
        const data = event.data || {};
        if (data.key instanceof Uint8Array && data.key.length === 32) finish(resolve, data.key);
        else finish(reject, new VaultCryptoError(data.error || "kdf_failed"));
      };
      // A worker the policy refuses fires error before any message: do the
      // work here instead, so unlocking never depends on a worker.
      worker.onerror = (event) => {
        if (event && event.preventDefault) event.preventDefault();
        try { finish(resolve, stretchNow(password, kdf)); } catch (error) { finish(reject, error); }
      };
      worker.postMessage({ type: "forkmesh-vault-stretch", password: String(password), kdf });
    });
  }
  const KDF_WORKER = "forkmesh-vault-kdf";

  // ---- the account keyset ----------------------------------------------------
  // vaultKey   wraps the owner's own item keys (AES-256-GCM)
  // x25519     classical half of the share KEM
  // mlkemSeed  d || z of the ML-KEM-768 key, the post-quantum half
  // ed25519    signs the public bundle and every share this account makes

  async function newKeyset() {
    const x = await subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
    const ed = await subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
    const xPkcs8 = new Uint8Array(await subtle.exportKey("pkcs8", x.privateKey));
    const edPkcs8 = new Uint8Array(await subtle.exportKey("pkcs8", ed.privateKey));
    return {
      v: 1,
      vaultKey: toBase64(randomBytes(32)),
      x25519: toBase64(xPkcs8.subarray(xPkcs8.length - 32)),
      mlkemSeed: toBase64(randomBytes(64)),
      ed25519: toBase64(edPkcs8.subarray(edPkcs8.length - 32)),
    };
  }
  function checkKeyset(keyset) {
    const value = keyset || {};
    const sizes = { vaultKey: 32, x25519: 32, mlkemSeed: 64, ed25519: 32 };
    if (value.v !== 1) throw new VaultCryptoError("unsupported_keyset");
    for (const [field, size] of Object.entries(sizes)) {
      if (fromBase64(value[field]).length !== size) throw new VaultCryptoError("invalid_keyset");
    }
    return value;
  }
  async function x25519Private(raw) {
    return subtle.importKey("pkcs8", concat(PKCS8_X25519, raw), { name: "X25519" }, true, ["deriveBits"]);
  }
  async function ed25519Private(raw) {
    return subtle.importKey("pkcs8", concat(PKCS8_ED25519, raw), { name: "Ed25519" }, true, ["sign"]);
  }
  // A private key's JWK carries its public half as `x`.
  async function publicHalf(privateKey) {
    return fromBase64((await subtle.exportKey("jwk", privateKey)).x);
  }

  function bundleMessage(account, x, m, ed) {
    return utf8([CONTEXT + "identity", account, toBase64(x), toBase64(m), toBase64(ed)].join("\n"));
  }
  // What an account publishes: three public keys, self-signed with the account
  // name inside the signature so one account's bundle cannot be served as
  // another's.
  async function publicBundle(keyset, account) {
    checkKeyset(keyset);
    const name = cleanName(account);
    const x = await publicHalf(await x25519Private(fromBase64(keyset.x25519)));
    const edKey = await ed25519Private(fromBase64(keyset.ed25519));
    const ed = await publicHalf(edKey);
    const m = mlkemKeyGen(fromBase64(keyset.mlkemSeed)).publicKey;
    const signature = new Uint8Array(await subtle.sign("Ed25519", edKey, bundleMessage(name, x, m, ed)));
    return {
      v: 1, suite: SUITE, account: name,
      x25519: toBase64(x), mlkem768: toBase64(m), ed25519: toBase64(ed),
      signature: toBase64(signature),
    };
  }
  async function checkBundle(bundle, account) {
    const value = bundle || {};
    const name = cleanName(account);
    if (value.v !== 1 || value.suite !== SUITE || value.account !== name) {
      throw new VaultCryptoError("invalid_public_keys");
    }
    const x = fromBase64(value.x25519), m = fromBase64(value.mlkem768), ed = fromBase64(value.ed25519);
    if (x.length !== 32 || ed.length !== 32) throw new VaultCryptoError("invalid_public_keys");
    mlkemCheckPublicKey(m);
    const verifier = await subtle.importKey("raw", ed, { name: "Ed25519" }, true, ["verify"]);
    const good = await subtle.verify("Ed25519", verifier, fromBase64(value.signature), bundleMessage(name, x, m, ed));
    if (!good) throw new VaultCryptoError("invalid_public_keys");
    return { account: name, x, m, ed, verifier };
  }
  // Read aloud or compared out of band before trusting a first share.
  async function fingerprint(bundle) {
    const checked = await checkBundle(bundle, (bundle || {}).account);
    const digest = new Uint8Array(await subtle.digest("SHA-256",
      concat(utf8(CONTEXT + "fingerprint\n" + checked.account + "\n"), checked.x, checked.m, checked.ed)));
    return toHex(digest).match(/.{4}/g).join(" ");
  }

  async function wrapKeyset(wrappingSecret, purpose, account, keyset) {
    checkKeyset(keyset);
    const name = cleanName(account);
    const kek = await hkdf(wrappingSecret, "keyset-kek|" + purpose);
    const blob = await seal(kek, "keyset|" + purpose + "|" + name, utf8(JSON.stringify(keyset)));
    wipe(kek);
    return blob;
  }
  async function unwrapKeyset(wrappingSecret, purpose, account, blob) {
    const name = cleanName(account);
    const kek = await hkdf(wrappingSecret, "keyset-kek|" + purpose);
    try {
      return checkKeyset(JSON.parse(text(await open(kek, "keyset|" + purpose + "|" + name, blob))));
    } catch (error) {
      // A wrong master password and a corrupted blob look the same on purpose.
      throw new VaultCryptoError(error instanceof VaultCryptoError && error.code !== "decryption_failed"
        ? error.code : "wrong_password");
    } finally {
      wipe(kek);
    }
  }

  // The recovery code is 256 random bits, so it needs no stretching. It is
  // shown once, as Crockford base32 in groups, and opens a second wrapped copy
  // of the keyset when the master password is forgotten.
  const BASE32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  function newRecoveryCode() {
    const bytes = randomBytes(32);
    let bits = 0n;
    for (const byte of bytes) bits = (bits << 8n) | BigInt(byte);
    bits <<= 4n;                                  // 256 -> 260 bits, 52 symbols
    let out = "";
    for (let i = 51; i >= 0; i -= 1) out += BASE32[Number((bits >> BigInt(i * 5)) & 31n)];
    wipe(bytes);
    return out.match(/.{4}/g).join("-");
  }
  function recoverySecret(code) {
    const clean = String(code || "").toUpperCase().replace(/[\s-]/g, "")
      .replace(/O/g, "0").replace(/[IL]/g, "1");
    if (clean.length !== 52 || [...clean].some((c) => !BASE32.includes(c))) {
      throw new VaultCryptoError("invalid_recovery_code");
    }
    let bits = 0n;
    for (const c of clean) bits = (bits << 5n) | BigInt(BASE32.indexOf(c));
    bits >>= 4n;
    const out = new Uint8Array(32);
    for (let i = 31; i >= 0; i -= 1) { out[i] = Number(bits & 0xffn); bits >>= 8n; }
    return out;
  }

  // ---- items -------------------------------------------------------------------

  function newItemId() { return toHex(randomBytes(16)); }
  function newItemKey() { return randomBytes(32); }

  // Padded to a 256-byte step so a ciphertext's size says little about the
  // password inside it. JSON ignores the trailing spaces.
  async function sealItem(itemKey, itemId, fields) {
    const body = utf8(JSON.stringify(fields));
    const padded = new Uint8Array(Math.ceil((body.length + 1) / 256) * 256).fill(0x20);
    padded.set(body);
    const blob = await seal(itemKey, "item|" + cleanId(itemId), padded);
    wipe(body, padded);
    return blob;
  }
  async function openItem(itemKey, itemId, blob) {
    const value = JSON.parse(text(await open(itemKey, "item|" + cleanId(itemId), blob)));
    if (!value || typeof value !== "object" || Array.isArray(value)) throw new VaultCryptoError("invalid_item");
    return value;
  }
  async function wrapItemKey(keyset, itemId, itemKey) {
    return seal(fromBase64(checkKeyset(keyset).vaultKey), "itemkey|" + cleanId(itemId), itemKey);
  }
  async function unwrapItemKey(keyset, itemId, blob) {
    return open(fromBase64(checkKeyset(keyset).vaultKey), "itemkey|" + cleanId(itemId), blob);
  }
  // Small owner-only records (the pinned fingerprints of people shared with).
  async function sealPrivate(keyset, label, value) {
    return seal(fromBase64(checkKeyset(keyset).vaultKey), "private|" + label, utf8(JSON.stringify(value)));
  }
  async function openPrivate(keyset, label, blob) {
    return JSON.parse(text(await open(fromBase64(checkKeyset(keyset).vaultKey), "private|" + label, blob)));
  }

  // ---- shares: X25519 + ML-KEM-768, X-Wing combiner ------------------------------

  const PERMISSIONS = Object.freeze(["read", "write"]);

  async function hybridEncapsulate(recipient) {
    const ephemeral = await subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
    const ctX = new Uint8Array(await subtle.exportKey("raw", ephemeral.publicKey));
    const theirs = await subtle.importKey("raw", recipient.x, { name: "X25519" }, true, []);
    const ssX = new Uint8Array(await subtle.deriveBits({ name: "X25519", public: theirs }, ephemeral.privateKey, 256));
    if (equalBytes(ssX, new Uint8Array(32))) throw new VaultCryptoError("invalid_public_keys");
    const kem = mlkemEncapsulate(recipient.m);
    const shared = sha3_256(concat(kem.sharedSecret, ssX, ctX, recipient.x, XWING_LABEL));
    wipe(ssX, kem.sharedSecret);
    return { ctX, ctM: kem.ciphertext, shared };
  }
  // Expanding the ML-KEM seed costs as much as a decapsulation, so the
  // expanded key is kept while a vault is unlocked and dropped on lock.
  let expanded = { seed: "", secretKey: null };
  function expandedMlkem(keyset) {
    if (expanded.seed !== keyset.mlkemSeed) {
      forgetKeys();
      expanded = { seed: keyset.mlkemSeed, secretKey: mlkemKeyGen(fromBase64(keyset.mlkemSeed)).secretKey };
    }
    return expanded.secretKey;
  }
  function forgetKeys() {
    wipe(expanded.secretKey);
    expanded = { seed: "", secretKey: null };
  }
  async function hybridDecapsulate(keyset, ctX, ctM) {
    if (ctX.length !== 32) throw new VaultCryptoError("invalid_share");
    const mine = await x25519Private(fromBase64(keyset.x25519));
    const pkX = await publicHalf(mine);
    const theirs = await subtle.importKey("raw", ctX, { name: "X25519" }, true, []);
    const ssX = new Uint8Array(await subtle.deriveBits({ name: "X25519", public: theirs }, mine, 256));
    if (equalBytes(ssX, new Uint8Array(32))) throw new VaultCryptoError("invalid_share");
    const ssM = mlkemDecapsulate(expandedMlkem(keyset), ctM);
    const shared = sha3_256(concat(ssM, ssX, ctX, pkX, XWING_LABEL));
    wipe(ssX, ssM);
    return shared;
  }
  function grantContext(itemId, sender, recipient, permission) {
    return ["grant", cleanId(itemId), cleanName(sender), cleanName(recipient), permission].join("|");
  }
  async function grantMessage(context, recipientEd, ctX, ctM, wrapped) {
    const digest = new Uint8Array(await subtle.digest("SHA-256", concat(ctX, ctM, fromBase64(wrapped))));
    return utf8([CONTEXT + context, toBase64(recipientEd), toBase64(digest)].join("\n"));
  }

  // Seal an item key to one recipient and sign the result as the sender.
  async function sealGrant({ keyset, sender, recipientBundle, itemId, itemKey, permission }) {
    if (!PERMISSIONS.includes(permission)) throw new VaultCryptoError("invalid_permission");
    const recipient = await checkBundle(recipientBundle, (recipientBundle || {}).account);
    const context = grantContext(itemId, sender, recipient.account, permission);
    const kem = await hybridEncapsulate(recipient);
    const wrapKey = await hkdf(kem.shared, context);
    const wrapped = await seal(wrapKey, context, itemKey);
    wipe(kem.shared, wrapKey);
    const signer = await ed25519Private(fromBase64(checkKeyset(keyset).ed25519));
    const signature = new Uint8Array(await subtle.sign("Ed25519", signer,
      await grantMessage(context, recipient.ed, kem.ctX, kem.ctM, wrapped)));
    return {
      v: 1, suite: SUITE, x25519: toBase64(kem.ctX), mlkem768: toBase64(kem.ctM),
      wrapped, signature: toBase64(signature),
    };
  }
  // Returns the item key, or throws: a share is only opened after its
  // signature checks out against the sender's published bundle.
  async function openGrant({ keyset, recipient, senderBundle, itemId, grant, permission }) {
    const value = grant || {};
    if (value.v !== 1 || value.suite !== SUITE || !PERMISSIONS.includes(permission)) {
      throw new VaultCryptoError("invalid_share");
    }
    const sender = await checkBundle(senderBundle, (senderBundle || {}).account);
    const context = grantContext(itemId, sender.account, recipient, permission);
    const ctX = fromBase64(value.x25519), ctM = fromBase64(value.mlkem768);
    const myEd = await publicHalf(await ed25519Private(fromBase64(checkKeyset(keyset).ed25519)));
    const signed = await subtle.verify("Ed25519", sender.verifier, fromBase64(value.signature),
      await grantMessage(context, myEd, ctX, ctM, value.wrapped));
    if (!signed) throw new VaultCryptoError("invalid_share_signature");
    const shared = await hybridDecapsulate(keyset, ctX, ctM);
    const wrapKey = await hkdf(shared, context);
    try {
      const itemKey = await open(wrapKey, context, value.wrapped);
      if (itemKey.length !== 32) throw new VaultCryptoError("invalid_share");
      return itemKey;
    } finally {
      wipe(shared, wrapKey);
    }
  }

  // ---- password generator ------------------------------------------------------

  const CLASSES = Object.freeze({
    lower: "abcdefghijkmnopqrstuvwxyz",       // no l
    upper: "ABCDEFGHJKLMNPQRSTUVWXYZ",        // no I, O
    digits: "23456789",                       // no 0, 1
    symbols: "!@#$%^&*-_=+?",
  });
  // Uniform over [0, bound): bytes that would bias the result are thrown away.
  function randomBelow(bound) {
    const limit = 256 - (256 % bound);
    for (;;) {
      const byte = randomBytes(1)[0];
      if (byte < limit) return byte % bound;
    }
  }
  function generatePassword({ length = 24, lower = true, upper = true, digits = true, symbols = true } = {}) {
    const chosen = Object.entries({ lower, upper, digits, symbols })
      .filter(([, on]) => on).map(([name]) => CLASSES[name]);
    const size = Math.max(8, Math.min(128, Math.floor(Number(length)) || 24));
    if (!chosen.length) throw new VaultCryptoError("no_character_classes");
    const alphabet = chosen.join("");
    for (;;) {
      let out = "";
      for (let i = 0; i < size; i += 1) out += alphabet[randomBelow(alphabet.length)];
      // Redraw the whole password rather than patch one in: every accepted
      // password is then equally likely.
      if (chosen.every((set) => [...out].some((c) => set.includes(c)))) return out;
    }
  }

  // ---- key-stretch worker ---------------------------------------------------------

  if (typeof WorkerGlobalScope !== "undefined" && typeof self !== "undefined"
      && self instanceof WorkerGlobalScope && self.name === KDF_WORKER) {
    self.onmessage = (event) => {
      const job = event.data || {};
      if (job.type !== "forkmesh-vault-stretch") return;
      try {
        const key = stretchNow(job.password, job.kdf);
        self.postMessage({ key }, [key.buffer]);
      } catch (error) {
        self.postMessage({ error: error && error.code ? error.code : "kdf_failed" });
      }
    };
  }

  const api = {
    SUITE, VaultCryptoError, KDF_DEFAULT, KDF_FLOOR, PERMISSIONS,
    utf8, text, concat, randomBytes, toBase64, fromBase64, toHex, fromHex, equalBytes, wipe,
    sha3_256, sha3_512, shake128, shake256, blake2b, argon2id,
    mlkemKeyGen, mlkemEncapsulate, mlkemDecapsulate, mlkemCheckPublicKey,
    MLKEM_PUBLIC_BYTES, MLKEM_SECRET_BYTES, MLKEM_CIPHERTEXT_BYTES,
    newKdf, checkKdf, stretch, stretchNow,
    newKeyset, checkKeyset, publicBundle, checkBundle, fingerprint, wrapKeyset, unwrapKeyset,
    newRecoveryCode, recoverySecret,
    newItemId, newItemKey, sealItem, openItem, wrapItemKey, unwrapItemKey, sealPrivate, openPrivate,
    sealGrant, openGrant, forgetKeys, generatePassword,
  };
  return api;
}
})(typeof globalThis !== "undefined" ? globalThis : this);
