globalThis.__forkmeshRedditWalk = async (options) => {
  const SETTLE = [320, 1100];
  const MAX_MS = 45000;
  const limit = Math.min(Math.max(Number(options?.ms) || 9000, 3000), MAX_MS);
  const deadline = Date.now() + limit;

  const pause = (ms) => new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
  const between = (low, high) => low + Math.random() * (high - low);
  const ease = (value) => (value < 0.5 ? 2 * value * value : 1 - ((-2 * value + 2) ** 2) / 2);
  let pointer = { x: innerWidth / 2, y: innerHeight / 2 };
  let hovered = null;

  function movePointer(x, y) {
    const clientX = Math.max(1, Math.min(innerWidth - 2, Math.round(x)));
    const clientY = Math.max(1, Math.min(innerHeight - 2, Math.round(y)));
    pointer = { x: clientX, y: clientY };
    const target = document.elementFromPoint(clientX, clientY);
    if (!target) return;
    const init = { bubbles: true, cancelable: true, composed: true, clientX, clientY,
      view: window, pointerType: "mouse", isPrimary: true };
    if (target !== hovered) {
      if (hovered?.isConnected) {
        hovered.dispatchEvent(new PointerEvent("pointerout", init));
        hovered.dispatchEvent(new MouseEvent("mouseout", init));
      }
      hovered = target;
      target.dispatchEvent(new PointerEvent("pointerover", init));
      target.dispatchEvent(new MouseEvent("mouseover", init));
    }
    target.dispatchEvent(new PointerEvent("pointermove", init));
    target.dispatchEvent(new MouseEvent("mousemove", init));
  }

  async function glide(toX, toY) {
    const from = pointer;
    const steps = Math.round(between(14, 26));
    const bowX = between(-60, 60);
    const bowY = between(-40, 40);
    for (let step = 1; step <= steps; step += 1) {
      const progress = ease(step / steps);
      const arc = Math.sin(Math.PI * (step / steps));
      movePointer(from.x + (toX - from.x) * progress + bowX * arc,
        from.y + (toY - from.y) * progress + bowY * arc);
      await pause(between(10, 26));
    }
  }

  function postBoxes() {
    const boxes = [];
    for (const node of document.querySelectorAll('shreddit-post, div.thing[data-fullname^="t3_"], article')) {
      const box = node.getBoundingClientRect?.();
      if (box && box.width > 40 && box.height > 20 && box.bottom > 0 && box.top < innerHeight) boxes.push(box);
      if (boxes.length >= 8) break;
    }
    return boxes;
  }

  async function readVisible() {
    const boxes = postBoxes();
    if (!boxes.length) {
      await glide(between(innerWidth * 0.3, innerWidth * 0.7), between(innerHeight * 0.3, innerHeight * 0.7));
      await pause(between(SETTLE[0], SETTLE[1]));
      return;
    }
    for (const box of boxes.slice(0, Math.round(between(1, 3)))) {
      await glide(box.left + between(0.2, 0.8) * box.width, box.top + between(0.2, 0.8) * box.height);
      await pause(between(SETTLE[0], SETTLE[1]));
    }
  }

  async function scrollBurst() {
    const target = Math.round(between(innerHeight * 0.45, innerHeight * 0.95));
    const steps = Math.round(between(5, 11));
    for (let step = 0; step < steps; step += 1) {
      scrollBy(0, Math.round(target / steps));
      await pause(between(28, 90));
    }
    if (Math.random() < 0.25) {
      scrollBy(0, -Math.round(between(40, 160)));
      await pause(between(180, 420));
    }
  }

  const atBottom = () =>
    innerHeight + scrollY >= (document.documentElement.scrollHeight || 0) - 2 * innerHeight;

  let bursts = 0;
  await pause(between(600, 1400));
  while (Date.now() < deadline) {
    await readVisible();
    if (atBottom()) break;
    await scrollBurst();
    bursts += 1;
    await pause(between(300, 900));
  }
  return { ok: true, bursts, url: location.href };
};
