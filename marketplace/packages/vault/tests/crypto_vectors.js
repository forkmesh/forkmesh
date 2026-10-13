// Known-answer and property checks for content/crypto/vault-crypto.js.
// Started by test_vault_crypto.py; exits non-zero on the first failure.
"use strict";
const nodeCrypto = require("crypto");
const { execFileSync } = require("child_process");
const fs = require("fs"), os = require("os"), path = require("path");
const C = require("../content/crypto/vault-crypto.js");
const vectors = require("./fixtures/openssl-vectors.json");

let checks = 0;
function check(name, ok) {
  checks += 1;
  if (!ok) { console.error("FAIL " + name); process.exit(1); }
}
const same = (bytes, hexText) => C.toHex(bytes) === hexText;

// SHA-3, SHAKE and BLAKE2b against Node's OpenSSL, across block edges.
for (const size of [0, 1, 71, 72, 73, 135, 136, 137, 167, 168, 169, 500]) {
  const m = new Uint8Array(nodeCrypto.randomBytes(size));
  check("sha3-256/" + size, same(C.sha3_256(m), nodeCrypto.createHash("sha3-256").update(m).digest("hex")));
  check("sha3-512/" + size, same(C.sha3_512(m), nodeCrypto.createHash("sha3-512").update(m).digest("hex")));
  check("shake128/" + size, same(C.shake128(m, 400), nodeCrypto.createHash("shake128", { outputLength: 400 }).update(m).digest("hex")));
  check("shake256/" + size, same(C.shake256(m, 333), nodeCrypto.createHash("shake256", { outputLength: 333 }).update(m).digest("hex")));
  check("blake2b/" + size, same(C.blake2b(m, 64), nodeCrypto.createHash("blake2b512").update(m).digest("hex")));
}

// Argon2id against OpenSSL's answers, including the default 64 MiB setting.
for (const v of vectors.argon2id) {
  const key = C.argon2id(C.utf8(v.password), C.fromHex(v.salt),
    { memoryKiB: v.memoryKiB, passes: v.passes, parallelism: v.parallelism, length: v.length });
  check(`argon2id m=${v.memoryKiB} t=${v.passes} p=${v.parallelism}`, same(key, v.key));
}
check("the default stretch is the 64 MiB vector", vectors.argon2id.some((v) =>
  v.memoryKiB === C.KDF_DEFAULT.memoryKiB && v.passes === C.KDF_DEFAULT.passes && v.parallelism === C.KDF_DEFAULT.parallelism));

// ML-KEM-768 against OpenSSL: key generation from a seed, decapsulation of
// OpenSSL's ciphertext, and the implicit-rejection secret for a tampered one.
for (const v of vectors.mlkem768) {
  const pair = C.mlkemKeyGen(C.fromHex(v.seed));
  check("mlkem keygen public", same(pair.publicKey, v.publicKey));
  check("mlkem keygen secret", nodeCrypto.createHash("sha256").update(pair.secretKey).digest("hex") === v.secretKeySha256);
  check("mlkem decapsulate", same(C.mlkemDecapsulate(pair.secretKey, C.fromHex(v.ciphertext)), v.sharedSecret));
  check("mlkem implicit rejection", same(C.mlkemDecapsulate(pair.secretKey, C.fromHex(v.tamperedCiphertext)), v.rejectionSecret));
  const mine = C.mlkemEncapsulate(pair.publicKey);
  check("mlkem round trip", C.equalBytes(C.mlkemDecapsulate(pair.secretKey, mine.ciphertext), mine.sharedSecret));
  const unreduced = Uint8Array.from(pair.publicKey); unreduced[0] = 0xff; unreduced[1] |= 0x0f;
  let refused = false;
  try { C.mlkemEncapsulate(unreduced); } catch (error) { refused = error.code === "invalid_mlkem_public_key"; }
  check("mlkem refuses an unreduced public key", refused);
}

// When this machine's OpenSSL speaks ML-KEM, it must open what we seal.
function liveOpenssl() {
  try { return /ML-KEM-768/i.test(execFileSync("openssl", ["list", "-kem-algorithms"], { stdio: ["ignore", "pipe", "ignore"] }).toString()); }
  catch { return false; }
}
if (liveOpenssl()) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "fmv-live-"));
  const at = (name) => path.join(dir, name);
  execFileSync("openssl", ["genpkey", "-algorithm", "ML-KEM-768", "-out", at("sk.pem")]);
  const der = execFileSync("openssl", ["pkey", "-in", at("sk.pem"), "-pubout", "-outform", "DER"]);
  const sealed = C.mlkemEncapsulate(new Uint8Array(der.subarray(der.length - 1184)));
  fs.writeFileSync(at("ct"), sealed.ciphertext);
  execFileSync("openssl", ["pkeyutl", "-decap", "-inkey", at("sk.pem"), "-in", at("ct"), "-secret", at("ss")]);
  check("openssl opens our ciphertext", C.equalBytes(new Uint8Array(fs.readFileSync(at("ss"))), sealed.sharedSecret));
  fs.rmSync(dir, { recursive: true });
  console.log("live OpenSSL ML-KEM interop checked");
}

(async () => {
  // The protocol layer: every binding a blob carries is enforced.
  const alice = await C.newKeyset(), bob = await C.newKeyset();
  const aliceBundle = await C.publicBundle(alice, "alice"), bobBundle = await C.publicBundle(bob, "bob");
  const id = C.newItemId(), otherId = C.newItemId(), itemKey = C.newItemKey();
  const refused = async (name, action, code) => {
    try { await action(); } catch (error) { check(`${name} -> ${error.code}`, error.code === code); return; }
    check(name + " was accepted", false);
  };

  const body = await C.sealItem(itemKey, id, { title: "t", password: "p" });
  check("an item round trips", (await C.openItem(itemKey, id, body)).password === "p");
  check("ciphertext length hides the password length",
    (await C.sealItem(itemKey, id, { title: "t", password: "p".repeat(60) })).length === body.length);
  await refused("an item blob moved to another id", () => C.openItem(itemKey, otherId, body), "decryption_failed");
  const flipped = C.fromBase64(body); flipped[flipped.length - 1] ^= 1;
  await refused("a tampered item blob", () => C.openItem(itemKey, id, C.toBase64(flipped)), "decryption_failed");
  const wrapped = await C.wrapItemKey(alice, id, itemKey);
  await refused("an item key moved to another id", () => C.unwrapItemKey(alice, otherId, wrapped), "decryption_failed");
  await refused("an item key under another keyset", () => C.unwrapItemKey(bob, id, wrapped), "decryption_failed");

  const grant = await C.sealGrant({ keyset: alice, sender: "alice", recipientBundle: bobBundle, itemId: id, itemKey, permission: "read" });
  const open = (over = {}) => C.openGrant({ keyset: bob, recipient: "bob", senderBundle: aliceBundle, itemId: id, grant, permission: "read", ...over });
  check("a grant opens for its recipient", C.equalBytes(await open(), itemKey));
  await refused("a grant upgraded to write", () => open({ permission: "write" }), "invalid_share_signature");
  await refused("a grant moved to another item", () => open({ itemId: otherId }), "invalid_share_signature");
  await refused("a grant replayed to another recipient", () => open({ keyset: alice, recipient: "alice" }), "invalid_share_signature");
  await refused("a grant from a forged sender", async () => open({ senderBundle: await C.publicBundle(await C.newKeyset(), "alice") }), "invalid_share_signature");
  for (const field of ["x25519", "mlkem768", "wrapped"]) {
    const bytes = C.fromBase64(grant[field]); bytes[3] ^= 1;
    await refused(`a grant with a tampered ${field}`, () => open({ grant: { ...grant, [field]: C.toBase64(bytes) } }), "invalid_share_signature");
  }
  await refused("a bundle re-labelled for another account", () => C.checkBundle({ ...bobBundle, account: "mallory" }, "mallory"), "invalid_public_keys");
  await refused("a bundle with a swapped ML-KEM key", () => C.checkBundle({ ...bobBundle, mlkem768: aliceBundle.mlkem768 }, "bob"), "invalid_public_keys");
  check("fingerprints differ per account", await C.fingerprint(aliceBundle) !== await C.fingerprint(bobBundle));

  // Master password wrapping and the downgrade floor.
  const kdf = { ...C.newKdf(), ...C.KDF_FLOOR };
  const masterKey = await C.stretch("paßword Ω", kdf);
  check("stretching normalizes Unicode (NFKC)", C.equalBytes(masterKey, C.stretchNow("paßword Ω", kdf)));
  const blob = await C.wrapKeyset(masterKey, "master", "alice", alice);
  check("a keyset round trips", (await C.unwrapKeyset(masterKey, "master", "alice", blob)).vaultKey === alice.vaultKey);
  await refused("a keyset blob served to another account", () => C.unwrapKeyset(masterKey, "master", "bob", blob), "wrong_password");
  await refused("a master blob offered as the recovery blob", () => C.unwrapKeyset(masterKey, "recovery", "alice", blob), "wrong_password");
  for (const weak of [{ memoryKiB: 1024 }, { passes: 1 }, { algorithm: "pbkdf2" }, { version: 16 }, { salt: "AAAA" }]) {
    let code = "";
    try { C.checkKdf({ ...kdf, ...weak }); } catch (error) { code = error.code; }
    check("weak stretch settings are refused: " + JSON.stringify(weak), code === "unsafe_kdf_parameters" || code === "invalid_encoding");
  }
  const code = C.newRecoveryCode();
  check("a recovery code survives being typed loosely",
    C.equalBytes(C.recoverySecret(code), C.recoverySecret(code.toLowerCase().replace(/-/g, " ").replace(/0/g, "o").replace(/1/g, "l"))));

  // Generator: every class present, nothing outside the alphabet, no repeats.
  const seen = new Set();
  for (let i = 0; i < 200; i += 1) {
    const password = C.generatePassword({ length: 16 });
    check("generated password shape", password.length === 16 && /[a-z]/.test(password) && /[A-Z]/.test(password)
      && /[0-9]/.test(password) && /[^A-Za-z0-9]/.test(password) && !/[lIO01]/.test(password));
    seen.add(password);
  }
  check("generated passwords do not repeat", seen.size === 200);
  check("digits only when asked", /^[2-9]{8}$/.test(C.generatePassword({ length: 8, lower: false, upper: false, symbols: false })));

  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); process.exit(1); });
