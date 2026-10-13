export const COFFEE = { name: 'Coffee with milk', portion: '1 mug (240 ml)', calories: 30, proteinG: 1.6, carbsG: 2.4, fatG: 1.5 };
export const ASK_EVERY_MS = 3 * 60 * 1000;
export const PACKAGE_URL = '/packages/calories';
const COFFEE_RE = /\b(coffee|latte|cappuccino|espresso|americano|flat white|mocha)\b/i;

export function localDay(d) {
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

function pick(food) {
  if (!food || typeof food !== 'object') return null;
  const name = String(food.name ?? '').trim().slice(0, 120);
  const calories = Number(food.calories);
  if (!name || !COFFEE_RE.test(name) || !Number.isFinite(calories) || calories < 0 || calories > 2000) return null;
  const out = { name, calories: Math.round(calories) };
  const portion = String(food.portion ?? '').trim().slice(0, 80);
  if (portion) out.portion = portion;
  for (const k of ['proteinG', 'carbsG', 'fatG']) {
    const v = Number(food[k]);
    if (Number.isFinite(v) && v >= 0 && v <= 500) out[k] = v;
  }
  return out;
}

export function coffeeFood(overview) {
  for (const list of [overview?.quickFoods, overview?.commonFoods]) {
    for (const food of Array.isArray(list) ? list : []) {
      const found = pick(food);
      if (found) return found;
    }
  }
  return { ...COFFEE };
}

export function coffeeEntry(overview, date) {
  return { ...coffeeFood(overview), day: localDay(date), meal: 'snack', source: 'quick' };
}

export function logError(status, body) {
  const code = String(body?.error || '');
  if (status === 409 && code === 'plugin_not_installed') return 'Install the Calorie Counter plugin first.';
  if (status === 409 && code === 'day_full') return 'Your diary is full for today (100 entries).';
  if (status === 401) return 'Sign in again to log it.';
  return 'Could not log it. Try again from the Calorie Counter.';
}
