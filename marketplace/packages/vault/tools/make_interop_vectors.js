#!/usr/bin/env node
// Regenerate tests/fixtures/interop-from-js.json: a small vault made by the
// JavaScript suite, with its TEST-ONLY secrets written next to it, so every
// other implementation of suite fmv1 (the desktop's C++, the mobile app's
// Dart) can prove it opens exactly what the browser writes. Those ports commit
// the mirror image (interop-from-cpp.json, interop-from-dart.json) and
// tests/interop_check.js proves the browser opens what they write.
"use strict";
const fs = require("fs"), path = require("path");
const C = require("../content/crypto/vault-crypto.js");

(async () => {
  const password = "correct horse battery staple Å";   // NFKC folds the Angstrom sign
  const kdf = { ...C.newKdf(), ...C.KDF_FLOOR };
  const masterKey = await C.stretch(password, kdf);
  const alice = await C.newKeyset(), bob = await C.newKeyset();
  const recoveryCode = C.newRecoveryCode();
  const itemId = C.newItemId(), itemKey = C.newItemKey();
  const fields = { title: "Registrar — prod", username: "ops@acme.test", password: "päss \"quoted\" \\ wörd", url: "https://registrar.example/login", notes: "line one\nline two" };
  const aliceBundle = await C.publicBundle(alice, "alice"), bobBundle = await C.publicBundle(bob, "bob");
  const out = {
    format: "forkmesh-vault-interop-v1", suite: C.SUITE, madeBy: "javascript",
    warning: "Test keys only. Nothing here protects anything.",
    password, kdf, masterKey: C.toHex(masterKey),
    accounts: {
      alice: { keyset: alice, bundle: aliceBundle, fingerprint: await C.fingerprint(aliceBundle) },
      bob: { keyset: bob, bundle: bobBundle, fingerprint: await C.fingerprint(bobBundle) },
    },
    wrappedKeyset: await C.wrapKeyset(masterKey, "master", "alice", alice),
    recoveryCode, recoverySecret: C.toHex(C.recoverySecret(recoveryCode)),
    wrappedRecovery: await C.wrapKeyset(C.recoverySecret(recoveryCode), "recovery", "alice", alice),
    item: {
      id: itemId, key: C.toHex(itemKey), fields,
      body: await C.sealItem(itemKey, itemId, fields),
      wrappedKey: await C.wrapItemKey(alice, itemId, itemKey),
    },
    trust: { value: { pins: { bob: await C.fingerprint(bobBundle) } } },
    grants: {},
  };
  out.trust.blob = await C.sealPrivate(alice, "trust", out.trust.value);
  for (const permission of C.PERMISSIONS) {
    out.grants[permission] = await C.sealGrant({ keyset: alice, sender: "alice", recipientBundle: bobBundle, itemId, itemKey, permission });
  }
  const target = path.join(__dirname, "..", "tests", "fixtures", "interop-from-js.json");
  fs.writeFileSync(target, JSON.stringify(out, null, 2) + "\n");
  console.log("wrote " + target);
})().catch((error) => { console.error(error); process.exit(1); });
