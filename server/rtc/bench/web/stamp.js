export const WIDTH = 640;
export const HEIGHT = 360;
export const FPS = 24;
export const BLOCK = 20;
export const COLUMNS = 24;
export const VALUE_BITS = 40;
export const CHECK_BITS = 8;
const SPAN = 2 ** VALUE_BITS;
const MAX_DELAY_MS = 60000;

export function check(value) {
  let sum = 0x5a;
  for (let i = 0; i < 5; i += 1) sum = (sum * 31 + (Math.floor(value / 2 ** (8 * i)) % 256)) % 256;
  return sum;
}

export function stampBits(now) {
  const value = now % SPAN;
  const bits = [];
  for (let i = 0; i < VALUE_BITS; i += 1) bits.push(Math.floor(value / 2 ** i) % 2);
  const sum = check(value);
  for (let i = 0; i < CHECK_BITS; i += 1) bits.push((sum >> i) & 1);
  return bits;
}

export function paintStamp(context, now) {
  const bits = stampBits(now);
  context.fillStyle = "#000";
  context.fillRect(0, 0, BLOCK * COLUMNS, BLOCK * Math.ceil(bits.length / COLUMNS));
  context.fillStyle = "#fff";
  bits.forEach((bit, i) => {
    if (bit) context.fillRect((i % COLUMNS) * BLOCK, Math.floor(i / COLUMNS) * BLOCK, BLOCK, BLOCK);
  });
}

export function decodeStamp(pixels, width, now) {
  const total = VALUE_BITS + CHECK_BITS;
  let value = 0;
  let sum = 0;
  for (let i = 0; i < total; i += 1) {
    const x = (i % COLUMNS) * BLOCK + BLOCK / 2;
    const y = Math.floor(i / COLUMNS) * BLOCK + BLOCK / 2;
    let luma = 0;
    for (let dy = -3; dy <= 3; dy += 3) {
      for (let dx = -3; dx <= 3; dx += 3) {
        const offset = ((y + dy) * width + (x + dx)) * 4;
        luma += pixels[offset] + pixels[offset + 1] + pixels[offset + 2];
      }
    }
    const bit = luma > 9 * 3 * 128 ? 1 : 0;
    if (i < VALUE_BITS) value += bit * 2 ** i;
    else sum |= bit << (i - VALUE_BITS);
  }
  if (check(value) !== sum) return null;
  const delay = ((now % SPAN) - value + SPAN) % SPAN;
  return delay <= MAX_DELAY_MS ? delay : null;
}

export function readStamp(canvas, context, video, now) {
  const width = video.videoWidth;
  const height = video.videoHeight;
  if (!width || !height) return null;
  const rows = Math.ceil((VALUE_BITS + CHECK_BITS) / COLUMNS);
  if (canvas.width !== BLOCK * COLUMNS) canvas.width = BLOCK * COLUMNS;
  if (canvas.height !== BLOCK * rows) canvas.height = BLOCK * rows;
  context.drawImage(video, 0, 0, (width * canvas.width) / WIDTH, (height * canvas.height) / HEIGHT, 0, 0, canvas.width, canvas.height);
  return decodeStamp(context.getImageData(0, 0, canvas.width, canvas.height).data, canvas.width, now);
}
