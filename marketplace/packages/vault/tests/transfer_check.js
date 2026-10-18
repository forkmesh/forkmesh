"use strict";
const zlib = require("zlib");
const Client = require("../content/crypto/vault-client.js");

let checks = 0;
function check(name, ok) {
  checks += 1;
  if (!ok) { console.error("FAILED: " + name); process.exit(1); }
}
async function refused(name, work, code) {
  try { await work(); } catch (error) { check(name, error.code === code); return; }
  check(name + " (did not throw)", false);
}

const crc32 = (bytes) => {
  let crc = ~0;
  for (const byte of bytes) {
    crc ^= byte;
    for (let k = 0; k < 8; k += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return ~crc >>> 0;
};
function zip(entries) {
  const locals = [], centrals = [];
  let offset = 0;
  for (const { name, data, deflate, flags = 0, crc } of entries) {
    const raw = Buffer.from(data), packed = deflate ? zlib.deflateRawSync(raw) : raw;
    const sum = crc ?? crc32(raw);
    const fileName = Buffer.from(name);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(20, 4); local.writeUInt16LE(flags, 6); local.writeUInt16LE(deflate ? 8 : 0, 8);
    local.writeUInt32LE(sum, 14); local.writeUInt32LE(packed.length, 18); local.writeUInt32LE(raw.length, 22);
    local.writeUInt16LE(fileName.length, 26);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(20, 4); central.writeUInt16LE(20, 6);
    central.writeUInt16LE(flags, 8); central.writeUInt16LE(deflate ? 8 : 0, 10); central.writeUInt32LE(sum, 16);
    central.writeUInt32LE(packed.length, 20); central.writeUInt32LE(raw.length, 24);
    central.writeUInt16LE(fileName.length, 28); central.writeUInt32LE(offset, 42);
    locals.push(local, fileName, packed); centrals.push(central, fileName);
    offset += local.length + fileName.length + packed.length;
  }
  const directory = Buffer.concat(centrals), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...locals, directory, end]));
}

const APPLE = "﻿Title,URL,Username,Password,Notes,OTPAuth\r\n"
  + "example.com (ana),https://example.com/,ana@example.com,\"pa,ss\"\"word\",\"line one\nline two\",otpauth://totp/Example:ana?secret=JBSWY3DPEHPK3PXP&issuer=Example\r\n"
  + ",https://www.shop.example/login,bo,=-leading,,\r\n"
  + "Wi-Fi note,,,,just a note,\r\n"
  + "\r\n";

(async () => {
  const apple = Client.readLoginsCsv(APPLE);
  check("an Apple Passwords export is recognised", apple.source === "applePasswords" && apple.sourceLabel === "Apple Passwords");
  check("rows without a username or password are counted, not imported", apple.logins.length === 2 && apple.skippedRows === 1);
  const [first, second] = apple.logins;
  check("quoted commas, quotes and newlines survive", first.password === 'pa,ss"word' && first.notes === "line one\nline two");
  check("a leading = or - is part of the password", second.password === "=-leading");
  const fields = Client.importFields(first);
  check("the one-time code is kept in the notes", fields.notes === "line one\nline two\n\notpauth://totp/Example:ana?secret=JBSWY3DPEHPK3PXP&issuer=Example");
  check("an untitled login is named after its site", Client.importFields(second).title === "shop.example");
  check("then its username, then a placeholder",
    Client.importFields({ username: "solo", password: "x" }).title === "solo"
    && Client.importFields({ password: "x" }).title === "Imported login");
  check("the fields are what save() accepts", Client.cleanFields(fields).title === "example.com (ana)");

  const sources = {
    chrome: "name,url,username,password,note\nsite,https://a.example,u,p,\n",
    firefox: "url,username,password,httpRealm,formActionOrigin,guid,timeCreated\nhttps://a.example,u,p,,,{1},0\n",
    bitwarden: "folder,favorite,type,name,notes,fields,reprompt,login_uri,login_username,login_password,login_totp\n,,login,site,,,0,https://a.example,u,p,\n",
    onePassword: "Title,Url,Username,Password,OTPAuth,Favorite,Archived,Tags,Notes\nsite,https://a.example,u,p,,false,false,,\n",
    duckDuckGo: "title,url,username,password,notes\nsite,https://a.example,u,p,\n",
  };
  for (const [source, text] of Object.entries(sources)) {
    const read = Client.readLoginsCsv(text);
    check(`${source} columns are read`, read.source === source && read.logins[0].password === "p" && read.logins[0].username === "u");
  }
  check("Bitwarden's URL column is used", Client.readLoginsCsv(sources.bitwarden).logins[0].url === "https://a.example");

  await refused("a file without a password column", () => Client.readLoginsCsv("a,b\n1,2\n"), "csv_unknown_columns");
  await refused("an empty file", () => Client.readLoginsCsv(""), "csv_empty");
  await refused("a file with only notes", () => Client.readLoginsCsv("Title,Password,Username,Notes\nx,,,note\n"), "csv_no_logins");

  const encoder = new TextEncoder();
  check("bytes are read as UTF-8", (await Client.readLoginsFile(encoder.encode(APPLE))).logins.length === 2);
  const latin = Buffer.from("Title,URL,Username,Password\nCaf\xe9,https://a.example,u,p\n", "latin1");
  check("a Latin-1 export still opens", (await Client.readLoginsFile(latin)).logins[0].title === "Café");
  const archive = zip([
    { name: "__MACOSX/._Passwords.csv", data: "junk" },
    { name: "notes.txt", data: "hello" },
    { name: "Passwords.csv", data: APPLE, deflate: true },
  ]);
  check("Safari's ZIP export opens (deflated)", (await Client.readLoginsFile(archive)).logins.length === 2);
  check("a stored ZIP entry opens", (await Client.readLoginsFile(zip([{ name: "p.csv", data: APPLE }]).buffer)).logins.length === 2);
  await refused("a ZIP without a CSV", () => Client.readLoginsFile(zip([{ name: "a.txt", data: "x" }])), "zip_no_csv");
  await refused("a truncated ZIP", () => Client.readLoginsFile(archive.subarray(0, 40)), "zip_unreadable");
  await refused("a ZIP whose checksum does not match", () => Client.readLoginsFile(zip([{ name: "p.csv", data: APPLE, deflate: true, crc: 1 }])), "zip_unreadable");
  await refused("a password-protected ZIP", () => Client.readLoginsFile(zip([{ name: "p.csv", data: APPLE, flags: 1 }])), "zip_encrypted");
  const bomb = zip([{ name: "p.csv", data: Buffer.alloc(17 * 1024 * 1024, 0x61), deflate: true }]);
  bomb.set([0, 0, 0, 0], bomb.length - 22 - 46 - 5 + 24);
  await refused("a ZIP that inflates past 16 MB, whatever size it claims", () => Client.readLoginsFile(bomb), "csv_too_large");

  const existing = [
    { fields: { title: "Example", url: "https://example.com", username: "ANA@example.com", password: 'pa,ss"word' } },
    { error: "sender_keys_changed" },
  ];
  const changed = { ...first, password: "new password" };
  const plan = Client.planImport([first, second, second, changed], existing);
  check("a login already in the vault is skipped (host and case folded)", plan.duplicates === 2);
  check("a changed password is imported beside the old one", plan.fresh.length === 2 && plan.fresh[1].password === "new password");

  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); process.exit(1); });
