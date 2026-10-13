import { wrapText } from './model.js';

export const BUBBLE_W = 58;
const HEAD = '600 5px system-ui, sans-serif';
const BODY = '4.5px system-ui, sans-serif';

export function bubbleLines(ctx, b) {
  ctx.font = HEAD;
  const head = wrapText(b.title, BUBBLE_W - 6, (t) => ctx.measureText(t).width, 2);
  ctx.font = BODY;
  const body = b.prose ? wrapText(b.prose, BUBBLE_W - 6, (t) => ctx.measureText(t).width, 2) : [];
  return { head, body };
}

export function paintBubble(ctx, { head, body }, s, cx, bottom, now) {
  if (!head.length && !body.length) return;
  const h = 5 + head.length * 6 + body.length * 5.2;
  const x = Math.round(cx - BUBBLE_W / 2), y = Math.round(bottom - h);
  ctx.globalAlpha = s.live ? 0.96 : 0.78;
  ctx.fillStyle = '#ffffff';
  ctx.strokeStyle = s.color;
  ctx.lineWidth = 0.75;
  ctx.beginPath();
  ctx.roundRect(x, y, BUBBLE_W, h, 3);
  ctx.moveTo(cx - 2.5, y + h);
  ctx.lineTo(cx, y + h + 2.5);
  ctx.lineTo(cx + 2.5, y + h);
  ctx.fill();
  ctx.stroke();
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  let ty = y + 2.5;
  ctx.fillStyle = '#1d2030';
  ctx.font = HEAD;
  for (const line of head) { ctx.fillText(line, x + 3, ty); ty += 6; }
  ctx.fillStyle = '#4b5068';
  ctx.font = BODY;
  for (const line of body) { ctx.fillText(line, x + 3, ty); ty += 5.2; }
  if (s.pulse) {
    ctx.fillStyle = s.color;
    ctx.globalAlpha = 0.5 + 0.5 * Math.sin(now / 200);
    ctx.beginPath();
    ctx.arc(x + BUBBLE_W - 4, y + 4, 1.4, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.globalAlpha = 1;
}
