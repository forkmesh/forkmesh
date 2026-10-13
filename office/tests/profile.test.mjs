import { test } from 'node:test';
import assert from 'node:assert/strict';
import { cleanName, cleanTz, cleanPic, picUrl, localTime, tzOffset, fmtOffset, tzList, fitSquare, b64Bytes, handleOf, NAME_MAX } from '../public/features/profile/logic.js';

test('display names are trimmed, stripped of markup characters and capped', () => {
  assert.equal(cleanName('  Kaif   Ansari \n'), 'Kaif Ansari');
  assert.equal(cleanName('<b>x</b>'), 'b x /b');
  assert.equal(cleanName('x'.repeat(40)).length, NAME_MAX);
  assert.equal(cleanName(null), '');
  assert.equal(cleanName('\u0000\u0007'), '');
});

test('time zones must look like IANA names', () => {
  assert.equal(cleanTz('Asia/Calcutta'), 'Asia/Calcutta');
  assert.equal(cleanTz(' America/Argentina/Buenos_Aires '), 'America/Argentina/Buenos_Aires');
  assert.equal(cleanTz('Etc/GMT+5'), 'Etc/GMT+5');
  assert.equal(cleanTz('UTC'), 'UTC');
  assert.equal(cleanTz('a/b/c/d'), '');
  assert.equal(cleanTz('Asia/Cal cutta'), '');
  assert.equal(cleanTz('<script>'), '');
  assert.equal(cleanTz('x'.repeat(80)), '');
  assert.equal(cleanTz(undefined), '');
});

test('local time and offsets follow the zone', () => {
  const noonUtc = Date.UTC(2026, 0, 15, 12, 0);
  assert.equal(localTime('UTC', noonUtc), '12:00');
  assert.equal(localTime('Asia/Kolkata', noonUtc), '17:30');
  assert.equal(localTime('Not/AZone', noonUtc), '');
  assert.equal(localTime('', noonUtc), '');
  assert.equal(tzOffset('Asia/Kolkata', noonUtc), 330);
  assert.equal(tzOffset('America/New_York', noonUtc), -300);
  assert.equal(tzOffset('UTC', noonUtc), 0);
  assert.equal(tzOffset('Not/AZone', noonUtc), null);
  assert.equal(fmtOffset(330), 'UTC+05:30');
  assert.equal(fmtOffset(-300), 'UTC-05:00');
  assert.equal(fmtOffset(0), 'UTC+00:00');
});

test('the zone list is sorted by offset, unique and always has UTC and the current zone', () => {
  const list = tzList(['Asia/Kolkata', 'Asia/Kolkata', 'bad zone'], Date.UTC(2026, 0, 15));
  const names = list.map((z) => z.tz);
  assert.ok(names.includes('UTC') && names.includes('Asia/Kolkata'));
  assert.equal(new Set(names).size, names.length);
  for (let i = 1; i < list.length; i++) assert.ok(list[i - 1].off <= list[i].off);
  assert.equal(list.find((z) => z.tz === 'Asia/Kolkata').label, '(UTC+05:30) Asia/Kolkata');
});

test('picture urls need an account and a digest', () => {
  assert.equal(picUrl('kaif', '0123456789abcdef'), '/api/accounts/kaif/avatar?v=0123456789abcdef');
  assert.equal(picUrl('a b', '0123456789abcdef'), '/api/accounts/a%20b/avatar?v=0123456789abcdef');
  assert.equal(picUrl('kaif', 'nope'), '');
  assert.equal(picUrl('', '0123456789abcdef'), '');
  assert.equal(cleanPic('0123456789ABCDEF'), '');
  assert.equal(handleOf({ ex: { fm: { a: 'kaif' } } }), 'kaif');
  assert.equal(handleOf({}), '');
});

test('picture helpers crop to a centred square and measure base64', () => {
  assert.deepEqual(fitSquare(400, 200), { sx: 100, sy: 0, side: 200 });
  assert.deepEqual(fitSquare(100, 300), { sx: 0, sy: 100, side: 100 });
  assert.equal(b64Bytes('data:image/png;base64,QUJD'), 3);
  assert.equal(b64Bytes('data:image/png;base64,QUI='), 2);
  assert.equal(b64Bytes('data:image/png;base64,QQ=='), 1);
  assert.equal(b64Bytes(''), 0);
});
