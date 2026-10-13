#!/usr/bin/env node
// Regenerate tests/fixtures/openssl-vectors.json from the OpenSSL on this
// machine (3.5 or newer: it needs ML-KEM-768 and the ARGON2ID KDF). The
// vectors are committed so the suite checks the JavaScript against OpenSSL's
// answers on machines that have no such OpenSSL.
"use strict";
const { execFileSync } = require("child_process");
const crypto = require("crypto");
const fs = require("fs"), os = require("os"), path = require("path");

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "fmv-vectors-"));
const at = (name) => path.join(dir, name);
const hex = (buffer) => Buffer.from(buffer).toString("hex");
const openssl = (...args) => execFileSync("openssl", args);

const argon2id = [[19456, 2, 1, 32], [65536, 3, 1, 32], [1024, 3, 4, 64], [99, 2, 3, 100]].map(([memoryKiB, passes, parallelism, length], index) => {
  const password = `correct horse battery staple ${index} é`;
  const salt = crypto.randomBytes(16);
  const key = openssl("kdf", "-keylen", String(length), "-kdfopt", "hexpass:" + hex(Buffer.from(password, "utf8")),
    "-kdfopt", "hexsalt:" + hex(salt), "-kdfopt", "iter:" + passes, "-kdfopt", "memcost:" + memoryKiB,
    "-kdfopt", "lanes:" + parallelism, "ARGON2ID").toString().trim().replace(/:/g, "").toLowerCase();
  return { password, salt: hex(salt), memoryKiB, passes, parallelism, length, key };
});

const mlkem768 = [0, 1].map(() => {
  openssl("genpkey", "-algorithm", "ML-KEM-768", "-out", at("sk.pem"));
  openssl("pkey", "-in", at("sk.pem"), "-pubout", "-out", at("pk.pem"));
  const secretDer = openssl("pkey", "-in", at("sk.pem"), "-outform", "DER");
  const publicDer = openssl("pkey", "-in", at("pk.pem"), "-pubin", "-outform", "DER");
  // PKCS#8 "seed-priv": ... OCTET STRING(64) seed, OCTET STRING(2400) key.
  const secretKey = secretDer.subarray(secretDer.length - 2400);
  const seed = secretDer.subarray(secretDer.length - 2468, secretDer.length - 2404);
  openssl("pkeyutl", "-encap", "-inkey", at("pk.pem"), "-pubin", "-out", at("ct"), "-secret", at("ss"));
  const ciphertext = fs.readFileSync(at("ct"));
  const tampered = Buffer.from(ciphertext); tampered[7] ^= 0x20;
  fs.writeFileSync(at("bad"), tampered);
  openssl("pkeyutl", "-decap", "-inkey", at("sk.pem"), "-in", at("bad"), "-secret", at("ss-bad"));
  return {
    seed: hex(seed), publicKey: hex(publicDer.subarray(publicDer.length - 1184)),
    secretKeySha256: crypto.createHash("sha256").update(secretKey).digest("hex"),
    ciphertext: hex(ciphertext), sharedSecret: hex(fs.readFileSync(at("ss"))),
    tamperedCiphertext: hex(tampered), rejectionSecret: hex(fs.readFileSync(at("ss-bad"))),
  };
});

fs.rmSync(dir, { recursive: true });
const target = path.join(__dirname, "..", "tests", "fixtures", "openssl-vectors.json");
fs.writeFileSync(target, JSON.stringify({
  source: openssl("version").toString().trim(), argon2id, mlkem768,
}, null, 2) + "\n");
console.log("wrote " + target);
