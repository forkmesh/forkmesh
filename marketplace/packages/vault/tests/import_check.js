"use strict";
const path = require("path");
const zlib = require("zlib");
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

function zip(entries) {
  const locals = [], centrals = [];
  let offset = 0;
  for (const { name, data, method = 8, flags = 0, crc = zlib.crc32(data) } of entries) {
    const body = method === 8 ? zlib.deflateRawSync(data) : data;
    const nameBytes = Buffer.from(name);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4); local.writeUInt16LE(flags, 6);
    local.writeUInt16LE(method, 8); local.writeUInt32LE(crc >>> 0, 14);
    local.writeUInt32LE(body.length, 18); local.writeUInt32LE(data.length, 22); local.writeUInt16LE(nameBytes.length, 26);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(20, 4); central.writeUInt16LE(20, 6);
    central.writeUInt16LE(flags, 8); central.writeUInt16LE(method, 10); central.writeUInt32LE(crc >>> 0, 16);
    central.writeUInt32LE(body.length, 20); central.writeUInt32LE(data.length, 24);
    central.writeUInt16LE(nameBytes.length, 28); central.writeUInt32LE(offset, 42);
    locals.push(local, nameBytes, body);
    centrals.push(central, nameBytes);
    offset += local.length + nameBytes.length + body.length;
  }
  const directory = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...locals, directory, end]));
}

const APPLE = "﻿Title,URL,Username,Password,Notes,OTPAuth\r\n"
  + "Registrar,https://registrar.example/login,ops@acme.test,\"pa,ss\"\"word\",\"line one\nline two\",otpauth://totp/Registrar:ops?secret=JBSWY3DPEHPK3PXP\r\n"
  + ",https://www.bank.example/,alice,only-mine,,\r\n"
  + "Router,http://192.168.1.1,admin,router-pass,,\r\n"
  + ",,,,,\r\n"
  + "Notes only,,,,a note,\r\n";

(async () => {
  const read = Client.readCsv(APPLE);
  check("an Apple Passwords export is recognised", read.source === "applePasswords" && read.sourceLabel === "Apple Passwords");
  check("quoted commas, quotes and newlines survive", read.logins[0].password === "pa,ss\"word" && read.logins[0].notes === "line one\nline two");
  check("blank rows are ignored and rows without credentials counted", read.logins.length === 3 && read.skippedRows === 1);
  const fields = Client.importFields(read.logins[0]);
  check("the one-time code travels in the notes", fields.notes.endsWith("\n\notpauth://totp/Registrar:ops?secret=JBSWY3DPEHPK3PXP"));
  check("an untitled login is named after its site", Client.importFields(read.logins[1]).title === "bank.example");
  check("other exporters are told apart",
    Client.readCsv("name,url,username,password,note\nA,https://a.example,u,p,\n").source === "chrome"
    && Client.readCsv("folder,favorite,type,name,notes,fields,reprompt,login_uri,login_username,login_password,login_totp\n,,login,B,,,,https://b.example,u,p,\n").source === "bitwarden");
  await refused("a file without a password column", async () => Client.readCsv("a,b\n1,2\n"), "csv_unknown_columns");
  await refused("an empty file", async () => Client.readImportFile(new Uint8Array()), "csv_empty");

  const csvBytes = new TextEncoder().encode(APPLE);
  check("a plain CSV file reads", (await Client.readImportFile(csvBytes)).logins.length === 3);
  const safari = zip([{ name: "__MACOSX/._Passwords.csv", data: Buffer.from("junk") }, { name: "Passwords.csv", data: Buffer.from(csvBytes) }]);
  check("Safari's ZIP export reads", (await Client.readImportFile(safari)).logins.length === 3);
  check("a stored ZIP entry reads", (await Client.readImportFile(zip([{ name: "Passwords.csv", data: Buffer.from(csvBytes), method: 0 }]))).logins.length === 3);
  await refused("a ZIP whose checksum is wrong", () => Client.readImportFile(zip([{ name: "Passwords.csv", data: Buffer.from(csvBytes), crc: 1 }])), "zip_unreadable");
  await refused("a password-protected ZIP", () => Client.readImportFile(zip([{ name: "Passwords.csv", data: Buffer.from(csvBytes), flags: 1 }])), "zip_encrypted");
  await refused("a ZIP without a CSV", () => Client.readImportFile(zip([{ name: "readme.txt", data: Buffer.from("hi") }])), "zip_no_csv");

  const alice = clientFor("alice");
  await alice.setUp("alice master password");
  await alice.save({ title: "Bank", username: "Alice", password: "only-mine", url: "https://bank.example" });
  await alice.load();
  check("the plan skips a login already in the vault", Client.planImport(read.logins, alice.list()).duplicates === 1);
  const progress = [];
  const report = await alice.importLogins(read.logins, (done, total) => progress.push(`${done}/${total}`));
  check("import adds only new logins", report.added === 2 && report.duplicates === 1 && report.remaining === 0 && !report.stoppedBy);
  check("import reports progress", progress.join(" ") === "1/2 2/2");
  const registrar = alice.list().find((item) => item.fields.title === "Registrar");
  check("imported logins are readable after reload", alice.list().length === 3 && registrar.fields.password === "pa,ss\"word");
  const again = await alice.importLogins((await Client.readImportFile(safari)).logins);
  check("importing the same export twice adds nothing", again.added === 0 && again.duplicates === 3);

  const elsewhere = clientFor("alice");
  await elsewhere.unlock("alice master password");
  check("another device opens the imported logins", (await elsewhere.load()).some((item) => item.fields.title === "Router"));

  await refused("import while locked", () => clientFor("alice").importLogins(read.logins), "vault_locked");

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
