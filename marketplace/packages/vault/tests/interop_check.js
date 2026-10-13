// Every committed interop fixture, whichever implementation wrote it, must
// open under the JavaScript suite: the browser reads what the desktop and the
// phone write. Started by test_vault_crypto.py.
"use strict";
const fs = require("fs"), path = require("path");
const C = require("../content/crypto/vault-crypto.js");

let checks = 0;
function check(name, ok) {
  checks += 1;
  if (!ok) { console.error("FAIL " + name); process.exit(1); }
}

(async () => {
  const dir = path.join(__dirname, "fixtures");
  const files = fs.readdirSync(dir).filter((name) => /^interop-from-[a-z]+\.json$/.test(name)).sort();
  check("the JavaScript fixture is present", files.includes("interop-from-js.json"));
  for (const file of files) {
    const v = JSON.parse(fs.readFileSync(path.join(dir, file), "utf8"));
    const tag = `[${v.madeBy}] `;
    check(tag + "format", v.format === "forkmesh-vault-interop-v1" && v.suite === C.SUITE);
    const masterKey = C.stretchNow(v.password, v.kdf);
    check(tag + "Argon2id master key", C.toHex(masterKey) === v.masterKey);
    const alice = await C.unwrapKeyset(masterKey, "master", "alice", v.wrappedKeyset);
    check(tag + "keyset under the master password", JSON.stringify(alice) === JSON.stringify(C.checkKeyset(v.accounts.alice.keyset)));
    check(tag + "recovery code decodes", C.toHex(C.recoverySecret(v.recoveryCode)) === v.recoverySecret);
    const recovered = await C.unwrapKeyset(C.recoverySecret(v.recoveryCode), "recovery", "alice", v.wrappedRecovery);
    check(tag + "keyset under the recovery code", recovered.vaultKey === alice.vaultKey);
    for (const name of ["alice", "bob"]) {
      const account = v.accounts[name];
      await C.checkBundle(account.bundle, name);
      const mine = await C.publicBundle(account.keyset, name);
      check(tag + name + " public keys derive from the keyset", mine.x25519 === account.bundle.x25519
        && mine.mlkem768 === account.bundle.mlkem768 && mine.ed25519 === account.bundle.ed25519);
      check(tag + name + " fingerprint", await C.fingerprint(account.bundle) === account.fingerprint);
    }
    const itemKey = await C.unwrapItemKey(alice, v.item.id, v.item.wrappedKey);
    check(tag + "item key", C.toHex(itemKey) === v.item.key);
    check(tag + "item fields", JSON.stringify(await C.openItem(itemKey, v.item.id, v.item.body)) === JSON.stringify(v.item.fields));
    check(tag + "item padding", C.fromBase64(v.item.body).length % 256 === 12 + 16);
    check(tag + "trust record", JSON.stringify(await C.openPrivate(alice, "trust", v.trust.blob)) === JSON.stringify(v.trust.value));
    for (const permission of C.PERMISSIONS) {
      const opened = await C.openGrant({ keyset: v.accounts.bob.keyset, recipient: "bob", senderBundle: v.accounts.alice.bundle,
        itemId: v.item.id, grant: v.grants[permission], permission });
      check(tag + permission + " grant opens for bob", C.toHex(opened) === v.item.key);
    }
  }
  console.log(`${files.join(", ")}: ${checks} checks passed`);
})().catch((error) => { console.error(error); process.exit(1); });
