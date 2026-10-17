export const MLKEM_PUBLIC_KEY_BYTES = 1184;
export const MLKEM_SECRET_KEY_BYTES = 2400;
export const MLKEM_CIPHERTEXT_BYTES = 1088;
export const MLKEM_SHARED_SECRET_BYTES = 32;
export const MLKEM_SEED_BYTES = 64;

const ROUND_LO = new Uint32Array(24);
const ROUND_HI = new Uint32Array(24);
const ROTATIONS = new Uint8Array(25);

(() => {
  let lfsr = 1;
  for (let round = 0; round < 24; round += 1) {
    for (let j = 0; j < 7; j += 1) {
      if (lfsr & 1) {
        const bit = (1 << j) - 1;
        if (bit < 32) ROUND_LO[round] ^= (1 << bit) >>> 0;
        else ROUND_HI[round] ^= (1 << (bit - 32)) >>> 0;
      }
      lfsr <<= 1;
      if (lfsr & 0x100) lfsr ^= 0x171;
    }
  }
  let x = 1;
  let y = 0;
  for (let t = 0; t < 24; t += 1) {
    ROTATIONS[x + 5 * y] = ((t + 1) * (t + 2) / 2) % 64;
    const nextY = (2 * x + 3 * y) % 5;
    x = y;
    y = nextY;
  }
})();

function keccakF(state, c, b) {
  for (let round = 0; round < 24; round += 1) {
    for (let x = 0; x < 5; x += 1) {
      let lo = 0;
      let hi = 0;
      for (let y = 0; y < 25; y += 5) {
        lo ^= state[2 * (x + y)];
        hi ^= state[2 * (x + y) + 1];
      }
      c[2 * x] = lo;
      c[2 * x + 1] = hi;
    }
    for (let x = 0; x < 5; x += 1) {
      const p = (x + 4) % 5;
      const n = (x + 1) % 5;
      const nlo = c[2 * n];
      const nhi = c[2 * n + 1];
      const dlo = c[2 * p] ^ ((nlo << 1) | (nhi >>> 31));
      const dhi = c[2 * p + 1] ^ ((nhi << 1) | (nlo >>> 31));
      for (let y = 0; y < 25; y += 5) {
        state[2 * (x + y)] ^= dlo;
        state[2 * (x + y) + 1] ^= dhi;
      }
    }
    for (let x = 0; x < 5; x += 1) {
      for (let y = 0; y < 5; y += 1) {
        const from = x + 5 * y;
        const to = y + 5 * ((2 * x + 3 * y) % 5);
        const lo = state[2 * from];
        const hi = state[2 * from + 1];
        const r = ROTATIONS[from];
        let olo;
        let ohi;
        if (r === 0) {
          olo = lo;
          ohi = hi;
        } else if (r < 32) {
          olo = (lo << r) | (hi >>> (32 - r));
          ohi = (hi << r) | (lo >>> (32 - r));
        } else if (r === 32) {
          olo = hi;
          ohi = lo;
        } else {
          const m = r - 32;
          olo = (hi << m) | (lo >>> (32 - m));
          ohi = (lo << m) | (hi >>> (32 - m));
        }
        b[2 * to] = olo;
        b[2 * to + 1] = ohi;
      }
    }
    for (let y = 0; y < 25; y += 5) {
      for (let x = 0; x < 5; x += 1) {
        const i1 = (x + 1) % 5 + y;
        const i2 = (x + 2) % 5 + y;
        state[2 * (x + y)] = b[2 * (x + y)] ^ (~b[2 * i1] & b[2 * i2]);
        state[2 * (x + y) + 1] = b[2 * (x + y) + 1] ^ (~b[2 * i1 + 1] & b[2 * i2 + 1]);
      }
    }
    state[0] ^= ROUND_LO[round];
    state[1] ^= ROUND_HI[round];
  }
}

export class Keccak {
  constructor(rate, domain) {
    this.rate = rate;
    this.domain = domain;
    this.state = new Uint32Array(50);
    this.c = new Uint32Array(10);
    this.b = new Uint32Array(50);
    this.offset = 0;
    this.squeezing = false;
  }

  static sha3_256() { return new Keccak(136, 0x06); }
  static sha3_512() { return new Keccak(72, 0x06); }
  static shake128() { return new Keccak(168, 0x1f); }
  static shake256() { return new Keccak(136, 0x1f); }

  xorByte(position, value) {
    const word = position >>> 2;
    this.state[word] ^= (value & 0xff) << ((position & 3) * 8);
  }

  update(data) {
    if (this.squeezing) throw new Error("Keccak already squeezing");
    for (let i = 0; i < data.length; i += 1) {
      this.xorByte(this.offset, data[i]);
      this.offset += 1;
      if (this.offset === this.rate) {
        keccakF(this.state, this.c, this.b);
        this.offset = 0;
      }
    }
    return this;
  }

  finish() {
    this.xorByte(this.offset, this.domain);
    this.xorByte(this.rate - 1, 0x80);
    keccakF(this.state, this.c, this.b);
    this.offset = 0;
    this.squeezing = true;
  }

  squeeze(out) {
    if (!this.squeezing) this.finish();
    for (let i = 0; i < out.length; i += 1) {
      if (this.offset === this.rate) {
        keccakF(this.state, this.c, this.b);
        this.offset = 0;
      }
      out[i] = this.state[this.offset >>> 2] >>> ((this.offset & 3) * 8);
      this.offset += 1;
    }
    return out;
  }

  digest(length) {
    return this.squeeze(new Uint8Array(length));
  }
}

function hashParts(make, length, parts) {
  const k = make();
  for (const part of parts) k.update(part);
  return k.digest(length);
}

export const sha3_256 = (...parts) => hashParts(Keccak.sha3_256, 32, parts);
export const sha3_512 = (...parts) => hashParts(Keccak.sha3_512, 64, parts);
export const shake128 = (length, ...parts) => hashParts(Keccak.shake128, length, parts);
export const shake256 = (length, ...parts) => hashParts(Keccak.shake256, length, parts);

const N = 256;
const Q = 3329;
const K = 3;
const ETA1 = 2;
const ETA2 = 2;
const DU = 10;
const DV = 4;
const POLY_BYTES = 384;
const U_BYTES = (N * DU) / 8;

function modPow(base, exponent) {
  let result = 1;
  let b = base % Q;
  let e = exponent;
  while (e > 0) {
    if (e & 1) result = (result * b) % Q;
    b = (b * b) % Q;
    e >>= 1;
  }
  return result;
}

function bitRev7(value) {
  let out = 0;
  for (let i = 0; i < 7; i += 1) if ((value >>> i) & 1) out |= 1 << (6 - i);
  return out;
}

const ZETAS = Int32Array.from({ length: 128 }, (_, i) => modPow(17, bitRev7(i)));
const GAMMAS = Int32Array.from({ length: 128 }, (_, i) => modPow(17, 2 * bitRev7(i) + 1));

function randomBytes(count) {
  return globalThis.crypto.getRandomValues(new Uint8Array(count));
}

function wipe(...arrays) {
  for (const array of arrays) array?.fill?.(0);
}

function ntt(f) {
  let k = 1;
  for (let len = 128; len >= 2; len >>= 1) {
    for (let start = 0; start < N; start += 2 * len) {
      const zeta = ZETAS[k++];
      for (let j = start; j < start + len; j += 1) {
        const t = (zeta * f[j + len]) % Q;
        const lo = f[j] - t;
        f[j + len] = lo < 0 ? lo + Q : lo;
        const hi = f[j] + t;
        f[j] = hi >= Q ? hi - Q : hi;
      }
    }
  }
}

function nttInverse(f) {
  let k = 127;
  for (let len = 2; len <= 128; len <<= 1) {
    for (let start = 0; start < N; start += 2 * len) {
      const zeta = ZETAS[k--];
      for (let j = start; j < start + len; j += 1) {
        const t = f[j];
        const sum = t + f[j + len];
        f[j] = sum >= Q ? sum - Q : sum;
        const diff = f[j + len] - t;
        f[j + len] = (zeta * (diff < 0 ? diff + Q : diff)) % Q;
      }
    }
  }
  for (let i = 0; i < N; i += 1) f[i] = (f[i] * 3303) % Q;
}

function multiplyNtts(f, g) {
  const h = new Int32Array(N);
  for (let i = 0; i < 128; i += 1) {
    const a0 = f[2 * i];
    const a1 = f[2 * i + 1];
    const b0 = g[2 * i];
    const b1 = g[2 * i + 1];
    h[2 * i] = (a0 * b0 + ((a1 * b1) % Q) * GAMMAS[i]) % Q;
    h[2 * i + 1] = (a0 * b1 + a1 * b0) % Q;
  }
  return h;
}

function addInto(acc, f) {
  for (let i = 0; i < N; i += 1) {
    const v = acc[i] + f[i];
    acc[i] = v >= Q ? v - Q : v;
  }
}

function compress(f, d) {
  const mask = (1 << d) - 1;
  for (let i = 0; i < N; i += 1) f[i] = Math.floor((f[i] * (1 << d) + 1664) / Q) & mask;
}

function decompress(f, d) {
  for (let i = 0; i < N; i += 1) f[i] = (f[i] * Q + (1 << (d - 1))) >>> d;
}

function byteEncode(f, d, out, offset) {
  let acc = 0;
  let bits = 0;
  let pos = offset;
  for (let i = 0; i < N; i += 1) {
    acc |= f[i] << bits;
    bits += d;
    while (bits >= 8) {
      out[pos++] = acc & 0xff;
      acc >>>= 8;
      bits -= 8;
    }
  }
}

function byteDecode(bytes, offset, d) {
  const f = new Int32Array(N);
  const mask = (1 << d) - 1;
  let acc = 0;
  let bits = 0;
  let pos = offset;
  for (let i = 0; i < N; i += 1) {
    while (bits < d) {
      acc |= bytes[pos++] << bits;
      bits += 8;
    }
    f[i] = acc & mask;
    acc >>>= d;
    bits -= d;
  }
  return f;
}

function sampleNtt(rho, j, i) {
  const xof = Keccak.shake128().update(rho).update(Uint8Array.of(j, i));
  const a = new Int32Array(N);
  const block = new Uint8Array(168 * 3);
  let count = 0;
  let pos = block.length;
  while (count < N) {
    if (pos + 3 > block.length) {
      xof.squeeze(block);
      pos = 0;
    }
    const c0 = block[pos];
    const c1 = block[pos + 1];
    const c2 = block[pos + 2];
    pos += 3;
    const d1 = c0 + 256 * (c1 & 15);
    const d2 = (c1 >>> 4) + 16 * c2;
    if (d1 < Q) a[count++] = d1;
    if (d2 < Q && count < N) a[count++] = d2;
  }
  return a;
}

function sampleMatrix(rho) {
  return Array.from({ length: K }, (_, i) =>
    Array.from({ length: K }, (_, j) => sampleNtt(rho, j, i)));
}

function prf(seed, nonce, eta) {
  return shake256(64 * eta, seed, Uint8Array.of(nonce));
}

function cbd(bytes, eta) {
  const f = new Int32Array(N);
  for (let i = 0; i < N; i += 1) {
    let x = 0;
    let y = 0;
    const base = 2 * i * eta;
    for (let j = 0; j < eta; j += 1) {
      x += (bytes[(base + j) >>> 3] >>> ((base + j) & 7)) & 1;
      y += (bytes[(base + eta + j) >>> 3] >>> ((base + eta + j) & 7)) & 1;
    }
    const v = x - y;
    f[i] = v < 0 ? v + Q : v;
  }
  bytes.fill(0);
  return f;
}

function validatePublicKey(publicKey) {
  if (!(publicKey instanceof Uint8Array) || publicKey.length !== MLKEM_PUBLIC_KEY_BYTES) {
    throw new TypeError("Invalid ML-KEM public key length");
  }
  for (let i = 0; i < K; i += 1) {
    const t = byteDecode(publicKey, i * POLY_BYTES, 12);
    if (t.some((value) => value >= Q)) throw new TypeError("Invalid ML-KEM public key encoding");
  }
}

function pkeEncrypt(publicKey, message, r) {
  const t = Array.from({ length: K }, (_, i) => byteDecode(publicKey, i * POLY_BYTES, 12));
  const rho = publicKey.slice(K * POLY_BYTES, K * POLY_BYTES + 32);
  const a = sampleMatrix(rho);
  let nonce = 0;
  const y = Array.from({ length: K }, () => cbd(prf(r, nonce++, ETA1), ETA1));
  const e1 = Array.from({ length: K }, () => cbd(prf(r, nonce++, ETA2), ETA2));
  const e2 = cbd(prf(r, nonce, ETA2), ETA2);
  y.forEach(ntt);
  const out = new Uint8Array(MLKEM_CIPHERTEXT_BYTES);
  for (let i = 0; i < K; i += 1) {
    const u = new Int32Array(N);
    for (let j = 0; j < K; j += 1) addInto(u, multiplyNtts(a[j][i], y[j]));
    nttInverse(u);
    addInto(u, e1[i]);
    compress(u, DU);
    byteEncode(u, DU, out, i * U_BYTES);
  }
  const v = new Int32Array(N);
  for (let i = 0; i < K; i += 1) addInto(v, multiplyNtts(t[i], y[i]));
  nttInverse(v);
  addInto(v, e2);
  const mu = byteDecode(message, 0, 1);
  decompress(mu, 1);
  addInto(v, mu);
  compress(v, DV);
  byteEncode(v, DV, out, K * U_BYTES);
  y.forEach((poly) => poly.fill(0));
  return out;
}

function pkeDecrypt(secretKey, ciphertext) {
  const w = new Int32Array(N);
  for (let i = 0; i < K; i += 1) {
    const u = byteDecode(ciphertext, i * U_BYTES, DU);
    decompress(u, DU);
    ntt(u);
    const s = byteDecode(secretKey, i * POLY_BYTES, 12);
    if (s.some((value) => value >= Q)) throw new TypeError("Invalid ML-KEM secret key encoding");
    addInto(w, multiplyNtts(s, u));
    s.fill(0);
  }
  nttInverse(w);
  const v = byteDecode(ciphertext, K * U_BYTES, DV);
  decompress(v, DV);
  for (let i = 0; i < N; i += 1) {
    const diff = v[i] - w[i];
    w[i] = diff < 0 ? diff + Q : diff;
  }
  compress(w, 1);
  const message = new Uint8Array(32);
  byteEncode(w, 1, message, 0);
  return message;
}

export function mlkemKeyPair(seed = randomBytes(MLKEM_SEED_BYTES)) {
  if (!(seed instanceof Uint8Array) || seed.length !== MLKEM_SEED_BYTES) {
    throw new TypeError("ML-KEM seed must be 64 bytes (d || z)");
  }
  const d = seed.slice(0, 32);
  const z = seed.slice(32, 64);
  const rhoSigma = sha3_512(d, Uint8Array.of(K));
  const rho = rhoSigma.slice(0, 32);
  const sigma = rhoSigma.slice(32, 64);
  const a = sampleMatrix(rho);
  let nonce = 0;
  const s = Array.from({ length: K }, () => cbd(prf(sigma, nonce++, ETA1), ETA1));
  const e = Array.from({ length: K }, () => cbd(prf(sigma, nonce++, ETA1), ETA1));
  s.forEach(ntt);
  e.forEach(ntt);
  const publicKey = new Uint8Array(MLKEM_PUBLIC_KEY_BYTES);
  const secretKey = new Uint8Array(MLKEM_SECRET_KEY_BYTES);
  for (let i = 0; i < K; i += 1) {
    const t = new Int32Array(N);
    for (let j = 0; j < K; j += 1) addInto(t, multiplyNtts(a[i][j], s[j]));
    addInto(t, e[i]);
    byteEncode(t, 12, publicKey, i * POLY_BYTES);
    byteEncode(s[i], 12, secretKey, i * POLY_BYTES);
  }
  publicKey.set(rho, K * POLY_BYTES);
  let offset = K * POLY_BYTES;
  secretKey.set(publicKey, offset);
  offset += MLKEM_PUBLIC_KEY_BYTES;
  secretKey.set(sha3_256(publicKey), offset);
  offset += 32;
  secretKey.set(z, offset);
  s.forEach((poly) => poly.fill(0));
  wipe(d, z, rhoSigma, sigma);
  return { publicKey, secretKey };
}

export function mlkemEncapsulate(publicKey, message = randomBytes(32)) {
  validatePublicKey(publicKey);
  if (!(message instanceof Uint8Array) || message.length !== 32) {
    throw new TypeError("ML-KEM message must be 32 bytes");
  }
  const kr = sha3_512(message, sha3_256(publicKey));
  const r = kr.slice(32, 64);
  const ciphertext = pkeEncrypt(publicKey, message, r);
  const sharedSecret = kr.slice(0, 32);
  wipe(kr, r);
  return { ciphertext, sharedSecret };
}

export function mlkemDecapsulate(secretKey, ciphertext) {
  if (!(secretKey instanceof Uint8Array) || secretKey.length !== MLKEM_SECRET_KEY_BYTES) {
    throw new TypeError("Invalid ML-KEM secret key length");
  }
  if (!(ciphertext instanceof Uint8Array) || ciphertext.length !== MLKEM_CIPHERTEXT_BYTES) {
    throw new TypeError("Invalid ML-KEM ciphertext length");
  }
  const ekOffset = K * POLY_BYTES;
  const hOffset = ekOffset + MLKEM_PUBLIC_KEY_BYTES;
  const zOffset = hOffset + 32;
  const publicKey = secretKey.slice(ekOffset, hOffset);
  validatePublicKey(publicKey);
  const h = secretKey.slice(hOffset, zOffset);
  const check = sha3_256(publicKey);
  let hashDiff = 0;
  for (let i = 0; i < 32; i += 1) hashDiff |= check[i] ^ h[i];
  if (hashDiff) throw new TypeError("Invalid ML-KEM secret key hash");
  const z = secretKey.slice(zOffset, MLKEM_SECRET_KEY_BYTES);
  const message = pkeDecrypt(secretKey, ciphertext);
  const kr = sha3_512(message, h);
  const r = kr.slice(32, 64);
  const reencrypted = pkeEncrypt(publicKey, message, r);
  const rejection = shake256(32, z, ciphertext);
  let diff = 0;
  for (let i = 0; i < MLKEM_CIPHERTEXT_BYTES; i += 1) diff |= ciphertext[i] ^ reencrypted[i];
  const equal = -((diff - 1) >>> 31);
  const out = new Uint8Array(MLKEM_SHARED_SECRET_BYTES);
  for (let i = 0; i < MLKEM_SHARED_SECRET_BYTES; i += 1) {
    out[i] = (kr[i] & equal) | (rejection[i] & ~equal);
  }
  wipe(message, kr, r, rejection, z);
  return out;
}
