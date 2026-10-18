// End to end: the real client and crypto against the real Worker module.
// Started by test_vault_e2e.py; exits non-zero on the first failed check.
"use strict";
const path = require("path");
const { spawn } = require("child_process");
const readline = require("readline");
const C = require("../content/crypto/vault-crypto.js");
const Client = require("../content/crypto/vault-client.js");

const server = spawn(process.env.PYTHON || "python3", [path.join(__dirname, "vault_stdio_server.py")],
  { stdio: ["pipe", "pipe", "inherit"] });
const replies = [];
readline.createInterface({ input: server.stdout }).on("line", (line) => replies.shift()(JSON.parse(line)));
const send = (message) => new Promise((resolve) => {
  replies.push(resolve);
  server.stdin.write(JSON.stringify(message) + "\n");
});
const control = (name, extra = {}) => send({ control: name, ...extra }).then((reply) => reply.data);

// The smallest stretch the client accepts, so the suite stays quick. The
// 64 MiB default is checked against OpenSSL in test_vault_crypto.py.
const realNewKdf = C.newKdf;
C.newKdf = () => ({ ...realNewKdf(), ...C.KDF_FLOOR });

function clientFor(actor) {
  const request = async (apiPath, method = "GET", body = null) => {
    const reply = await send({ actor, method, path: apiPath, body });
    if (reply.status >= 400 || reply.data.ok === false) {
      const error = new Error(reply.data.error || "http_" + reply.status);
      error.code = reply.data.error; error.status = reply.status;
      throw error;
    }
    return reply.data;
  };
  return Client.create({ crypto: C, request });
}

let checks = 0;
function check(name, ok) {
  checks += 1;
  if (!ok) { console.error("FAIL " + name); server.kill(); process.exit(1); }
  console.log("ok   " + name);
}
async function refused(name, action, code) {
  try { await action(); } catch (error) { check(`${name} -> ${error.code}`, error.code === code); return; }
  check(name + " was accepted", false);
}

(async () => {
  const alice = clientFor("alice"), bob = clientFor("bob"), carol = clientFor("carol"),
    dave = clientFor("dave"), erin = clientFor("erin");

  check("a new account has no vault", (await alice.status()).installed === false);
  const { recoveryCode } = await alice.setUp("alice master password");
  check("setting up yields a recovery code", /^([0-9A-Z]{4}-){12}[0-9A-Z]{4}$/.test(recoveryCode));
  await bob.setUp("bob master password");
  await refused("a second vault for one account", () => clientFor("alice").setUp("x".repeat(12)), "vault_exists");

  const loginId = await alice.save({ title: "Registrar", username: "ops@acme.test", password: "s3cret!", url: "https://registrar.example/login", notes: "2FA on the team phone" });
  const bankId = await alice.save({ title: "Bank", username: "alice", password: "only-mine", url: "https://bank.example" });
  await alice.load();
  check("the owner reads what she saved", alice.find(loginId).fields.password === "s3cret!");

  // A second session of the same account starts locked and unlocks itself.
  const aliceElsewhere = clientFor("alice");
  await refused("a wrong master password", () => aliceElsewhere.unlock("not the password"), "wrong_password");
  await aliceElsewhere.unlock("alice master password");
  check("another device unlocks with the password", (await aliceElsewhere.load()).length === 2);

  // The server holds no plaintext.
  const dump = JSON.stringify((await control("dump")).rows);
  for (const secret of ["s3cret!", "only-mine", "Registrar", "registrar.example", "ops@acme.test", "team phone", "master password"]) {
    check(`the database never sees "${secret}"`, !dump.includes(secret));
  }

  // Share with one user.
  const preview = await alice.previewShare({ kind: "user", name: "bob" });
  check("a first share shows a new fingerprint", preview[0].state === "new" && preview[0].fingerprint.length === 79);
  check("it is the fingerprint bob sees for himself", preview[0].fingerprint === await bob.myFingerprint());
  await refused("sharing with someone without a vault", () => alice.share(loginId, { kind: "user", name: "carol" }), "recipient_has_no_vault");
  await refused("sharing with nobody", () => alice.share(loginId, { kind: "user", name: "nobody" }), "recipient_not_found");
  await alice.share(loginId, { kind: "user", name: "bob", permission: "read" });
  await bob.load();
  check("bob opens the shared login", bob.find(loginId).fields.password === "s3cret!");
  check("bob sees only what was shared", bob.list().length === 1 && !bob.find(bankId));
  check("bob was pinged without the title", (await control("pings")).pings.some((p) => p.recipient === "bob" && !JSON.stringify(p).includes("Registrar")));
  await refused("a reader cannot edit", () => bob.save({ ...bob.find(loginId).fields, password: "changed" }, loginId), "read_only");
  check("bob's fingerprint is pinned now", (await alice.previewShare({ kind: "user", name: "bob" }))[0].state === "pinned");

  // Write permission.
  await alice.share(loginId, { kind: "user", name: "bob", permission: "write" });
  await bob.load();
  await bob.save({ ...bob.find(loginId).fields, password: "rotated-by-bob" }, loginId);
  await alice.load();
  check("a writer's change reaches the owner", alice.find(loginId).fields.password === "rotated-by-bob" && alice.find(loginId).updatedBy === "bob");
  await refused("a writer cannot share onward", () => bob.share(loginId, { kind: "user", name: "alice" }), "owner_required");

  // Organization share: carol has no vault yet, erin is not a member.
  const orgResult = await alice.share(bankId, { kind: "organization", name: "acme", permission: "read" });
  check("an organization share seals to members with a vault", orgResult.sealedTo.join() === "bob" && orgResult.withoutVault.join() === "carol");
  await refused("sharing with an organization you are not in", () => alice.share(bankId, { kind: "organization", name: "globex" }), "organization_not_found");
  await bob.load();
  check("bob reads the organization's login", bob.find(bankId).fields.password === "only-mine" && bob.find(bankId).via.name === "acme");
  await carol.setUp("carol master password");
  check("a late joiner cannot read before the owner seals to her", (await carol.load()).length === 0);
  await alice.load();
  check("the owner's next unlock seals to her", await alice.syncPending() === 1 && await alice.syncPending() === 0);
  check("carol reads it now", (await carol.load()).length === 1 && carol.find(bankId).fields.password === "only-mine");

  // Leaving the organization ends access at once, on the server.
  await control("leave", { organization: "acme", member: "bob" });
  await bob.load();
  check("a former member loses the organization's items", !bob.find(bankId) && Boolean(bob.find(loginId)));

  // Unsharing rotates the key: bob's old key opens nothing new.
  const oldBody = (await clientRaw("alice", "/api/vault/items")).items.find((row) => row.id === loginId).body;
  await alice.unshare(loginId, { kind: "user", name: "bob" });
  await alice.load();
  const newBody = (await clientRaw("alice", "/api/vault/items")).items.find((row) => row.id === loginId).body;
  check("unsharing re-encrypts the item", oldBody !== newBody && alice.find(loginId).fields.password === "rotated-by-bob");
  check("bob no longer sees it", (await bob.load()).length === 0);
  await alice.unshare(bankId, { kind: "organization", name: "acme" });
  check("unsharing an organization removes every member", (await carol.load()).length === 0);
  await control("join", { organization: "acme", member: "bob" });
  await alice.load();
  await alice.share(bankId, { kind: "organization", name: "acme", permission: "read" });
  await control("leave", { organization: "acme", member: "bob" });
  await alice.share(bankId, { kind: "user", name: "bob", permission: "read" });
  await alice.unshare(bankId, { kind: "user", name: "bob" });
  await carol.load();
  check("carol still reads after a rotation that removed bob", carol.find(bankId).fields.password === "only-mine");
  check("and bob does not", (await bob.load()).length === 0);

  // A server that publishes other keys under bob's name is caught: nothing
  // is sealed to them, and nothing "from" them is opened.
  const honestBundle = (await clientRaw("alice", "/api/vault/users/bob")).bundle;
  const impostor = await C.publicBundle(await C.newKeyset(), "bob");
  await control("set_bundle", { member: "bob", bundle: impostor });
  check("swapped keys show as changed", (await alice.previewShare({ kind: "user", name: "bob" }))[0].state === "changed");
  await refused("and are not sealed to", () => alice.share(loginId, { kind: "user", name: "bob" }), "recipient_keys_changed");
  await control("set_bundle", { member: "bob", bundle: honestBundle });
  const bobItem = await (async () => { await bob.load(); return bob.save({ title: "From bob", password: "pw" }); })();
  await bob.load();
  await bob.share(bobItem, { kind: "user", name: "alice" });
  check("alice opens bob's share under his pinned keys", (await alice.load()).find((i) => i.id === bobItem).fields.title === "From bob");
  await control("set_bundle", { member: "bob", bundle: impostor });
  const suspicious = (await alice.load()).find((item) => item.id === bobItem);
  check("a sender whose keys changed is not opened", suspicious.fields === null && suspicious.error === "sender_keys_changed");
  await control("set_bundle", { member: "bob", bundle: honestBundle });

  await control("delete_account", { member: "bob" });
  check("a purged account leaves no rows", !JSON.stringify((await control("dump")).rows).includes("bi:bob"));

  // Master password change and recovery.
  await alice.changePassword("alice second password");
  await refused("the old password stops working", () => clientFor("alice").unlock("alice master password"), "wrong_password");
  const recovered = clientFor("alice");
  await refused("a wrong recovery code", () => recovered.recover(C.newRecoveryCode(), "x".repeat(12)), "wrong_recovery_code");
  const renewed = await recovered.recover(recoveryCode, "alice third password");
  check("recovery opens the vault and issues a new code", (await recovered.load()).length === 2 && renewed.recoveryCode !== recoveryCode);
  await refused("a spent recovery code", () => clientFor("alice").recover(recoveryCode, "x".repeat(12)), "wrong_recovery_code");
  const again = clientFor("alice");
  await again.unlock("alice third password");
  check("the new password unlocks", (await again.load()).length === 2);

  // Key substitution: dave's bundle served under erin's name never verifies.
  await dave.setUp("dave master password");
  await erin.setUp("erin master password");
  const daveBundle = (await clientRaw("alice", "/api/vault/users/dave")).bundle;
  let substituted = false;
  try { await C.checkBundle({ ...daveBundle, account: "erin" }, "erin"); } catch { substituted = true; }
  check("a bundle cannot be re-labelled as another account's", substituted);

  const exported = Client.readLoginsCsv("Title,URL,Username,Password,Notes,OTPAuth\n"
    + "Mail,https://mail.example/,erin,first secret,,otpauth://totp/Mail:erin?secret=JBSWY3DPEHPK3PXP\n"
    + ",https://www.shop.example/,erin@example.com,second secret,,\n"
    + "Mail again,https://mail.example/,ERIN,first secret,,\n");
  const progress = [];
  const imported = await erin.importLogins(exported.logins, { onProgress: (done, total) => progress.push(`${done}/${total}`) });
  check("an Apple Passwords export is imported, duplicates folded",
    imported.added === 2 && imported.duplicates === 1 && imported.remaining === 0 && progress.join() === "1/2,2/2");
  const erinItems = await erin.load();
  check("imported logins open with the owner's keys", erinItems.length === 2
    && erinItems.some((item) => item.fields.title === "shop.example" && item.fields.password === "second secret")
    && erinItems.some((item) => item.fields.notes.startsWith("otpauth://totp/Mail:erin")));
  check("importing the same file again adds nothing", (await erin.importLogins(exported.logins)).added === 0);
  const stored = await clientRaw("erin", "/api/vault/items");
  check("the server never sees an imported password",
    JSON.stringify(stored).includes(erinItems[0].id) && !JSON.stringify(stored).includes("second secret"));
  erin.lock();
  check("a locked client imports nothing", (await erin.importLogins(exported.logins).catch((error) => error.code)) === "vault_locked");

  // Locking forgets everything.
  again.lock();
  check("a locked client holds nothing", again.list().length === 0 && !again.unlocked());
  await refused("a locked client cannot save", () => again.save({ title: "x" }), "vault_locked");

  // Deleting the vault removes it.
  const gone = await recovered.destroy();
  check("deleting returns an encrypted export", gone.export.items.length === 2 && !JSON.stringify(gone.export).includes("only-mine"));
  check("and the vault is gone", (await clientFor("alice").status()).installed === false);

  console.log(`${checks} checks passed`);
  server.stdin.end();
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });

async function clientRaw(actor, apiPath) {
  return (await send({ actor, method: "GET", path: apiPath, body: null })).data;
}
