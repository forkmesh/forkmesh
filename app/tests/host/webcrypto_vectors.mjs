// WebCrypto reference vectors for app/tests/host/test_webcrypto_vectors.py.
//
//   node webcrypto_vectors.mjs            -> JSON vectors on stdout
//   node webcrypto_vectors.mjs verify     <- JSON {kind, ...} on stdin,
//                                            prints {"ok": bool}
//
// Keys and inputs are fixed so the Python host must reproduce the same
// bytes; ECDSA signatures are randomized, so those are cross-verified.
const subtle = globalThis.crypto.subtle;
const enc = new TextEncoder();
const hex = (buf) => Buffer.from(new Uint8Array(buf)).toString("hex");
const unhex = (text) => new Uint8Array(Buffer.from(text, "hex"));
const bytes = (n, seed) => Uint8Array.from({ length: n }, (_, i) => (i * 7 + seed) & 0xff);

async function vectors() {
  const message = enc.encode("ForkMesh host WebCrypto vector ✓");
  const big = bytes(200000, 3);
  const out = { message: hex(message), digests: {} };
  for (const name of ["SHA-1", "SHA-256", "SHA-384", "SHA-512"]) {
    out.digests[name] = {
      small: hex(await subtle.digest(name, message)),
      big: hex(await subtle.digest({ name }, big)),
    };
  }

  // HMAC
  const hmacKey = bytes(32, 11);
  out.hmac = { key: hex(hmacKey), sigs: {} };
  for (const hash of ["SHA-1", "SHA-256"]) {
    const key = await subtle.importKey("raw", hmacKey, { name: "HMAC", hash }, false, ["sign"]);
    out.hmac.sigs[hash] = hex(await subtle.sign("HMAC", key, message));
  }

  // Ed25519 from a fixed seed (PKCS#8 prefix + 32-byte seed).
  const seed = bytes(32, 42);
  const pkcs8 = new Uint8Array([...unhex("302e020100300506032b657004220420"), ...seed]);
  const edPriv = await subtle.importKey("pkcs8", pkcs8, { name: "Ed25519" }, true, ["sign"]);
  const jwk = await subtle.exportKey("jwk", edPriv);
  const edPubJwk = { kty: "OKP", crv: "Ed25519", x: jwk.x };
  const edPub = await subtle.importKey("jwk", edPubJwk, { name: "Ed25519" }, true, ["verify"]);
  out.ed25519 = {
    pkcs8: hex(pkcs8),
    jwk: { d: jwk.d, x: jwk.x },
    raw: hex(await subtle.exportKey("raw", edPub)),
    spki: hex(await subtle.exportKey("spki", edPub)),
    sig: hex(await subtle.sign({ name: "Ed25519" }, edPriv, message)),
  };

  // AES-GCM
  const aesKey = bytes(32, 5);
  const iv = bytes(12, 9);
  const aad = enc.encode("aad");
  const aes = await subtle.importKey("raw", aesKey, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
  out.aesgcm = {
    key: hex(aesKey), iv: hex(iv), aad: hex(aad),
    plain: hex(message),
    cipher: hex(await subtle.encrypt({ name: "AES-GCM", iv, tagLength: 128 }, aes, message)),
    cipherAad: hex(await subtle.encrypt({ name: "AES-GCM", iv, additionalData: aad }, aes, message)),
    cipher96: hex(await subtle.encrypt({ name: "AES-GCM", iv, tagLength: 96 }, aes, message)),
    bigCipherSha256: hex(await subtle.digest("SHA-256",
      await subtle.encrypt({ name: "AES-GCM", iv }, aes, big))),
  };

  // PBKDF2 (deriveBits and deriveKey -> AES-GCM)
  const password = enc.encode("correct horse battery staple");
  const salt = bytes(16, 1);
  const base = await subtle.importKey("raw", password, { name: "PBKDF2" }, false, ["deriveBits", "deriveKey"]);
  const bits = await subtle.deriveBits({ name: "PBKDF2", salt, iterations: 1000, hash: "SHA-256" }, base, 256);
  const derived = await subtle.deriveKey(
    { name: "PBKDF2", salt, iterations: 1000, hash: "SHA-256" }, base,
    { name: "AES-GCM", length: 256 }, true, ["encrypt"]);
  out.pbkdf2 = {
    password: hex(password), salt: hex(salt), iterations: 1000,
    bits: hex(bits),
    bitsSha1: hex(await subtle.deriveBits({ name: "PBKDF2", salt, iterations: 1000, hash: "SHA-1" }, base, 160)),
    derivedRaw: hex(await subtle.exportKey("raw", derived)),
    derivedCipher: hex(await subtle.encrypt({ name: "AES-GCM", iv }, derived, message)),
  };

  // ECDSA P-256: signatures are random, so export keys + one signature.
  const ecPair = await subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, ["sign", "verify"]);
  out.ecdsa = {
    pkcs8: hex(await subtle.exportKey("pkcs8", ecPair.privateKey)),
    spki: hex(await subtle.exportKey("spki", ecPair.publicKey)),
    raw: hex(await subtle.exportKey("raw", ecPair.publicKey)),
    jwk: await subtle.exportKey("jwk", ecPair.privateKey),
    sig: hex(await subtle.sign({ name: "ECDSA", hash: { name: "SHA-256" } }, ecPair.privateKey, message)),
  };

  // RSASSA-PKCS1-v1_5 is deterministic.
  const rsaPair = await subtle.generateKey(
    { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" },
    true, ["sign", "verify"]);
  out.rsa = {
    pkcs8: hex(await subtle.exportKey("pkcs8", rsaPair.privateKey)),
    spki: hex(await subtle.exportKey("spki", rsaPair.publicKey)),
    sig: hex(await subtle.sign({ name: "RSASSA-PKCS1-v1_5" }, rsaPair.privateKey, message)),
  };
  return out;
}

async function verify(input) {
  const message = unhex(input.message);
  if (input.kind === "ecdsa") {
    const key = await subtle.importKey("spki", unhex(input.spki), { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
    return subtle.verify({ name: "ECDSA", hash: "SHA-256" }, key, unhex(input.sig), message);
  }
  if (input.kind === "ed25519") {
    const key = await subtle.importKey("raw", unhex(input.raw), { name: "Ed25519" }, false, ["verify"]);
    return subtle.verify({ name: "Ed25519" }, key, unhex(input.sig), message);
  }
  if (input.kind === "rsa") {
    const key = await subtle.importKey("spki", unhex(input.spki), { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["verify"]);
    return subtle.verify("RSASSA-PKCS1-v1_5", key, unhex(input.sig), message);
  }
  throw new Error("unknown kind " + input.kind);
}

if (process.argv[2] === "verify") {
  let data = "";
  process.stdin.on("data", (chunk) => (data += chunk));
  process.stdin.on("end", async () => {
    const ok = await verify(JSON.parse(data));
    process.stdout.write(JSON.stringify({ ok }));
  });
} else {
  process.stdout.write(JSON.stringify(await vectors()));
}
