import { test } from 'node:test';
import assert from 'node:assert/strict';
import { COFFEE, coffeeFood, coffeeEntry, localDay, logError } from '../public/features/play/coffee-log.js';

test('the coffee uses the member\'s own quick food first, then the catalog, then a default', () => {
  assert.deepEqual(coffeeFood(null), COFFEE);
  const common = [{ name: 'Orange juice', calories: 112 }, { name: 'Latte', portion: '12 oz (355 ml)', calories: 190, proteinG: 12, carbsG: 18, fatG: 7 }];
  assert.equal(coffeeFood({ commonFoods: common }).name, 'Latte');
  const mine = [{ name: 'Oat flat white', portion: 'cup', calories: 95.4, fatG: -1 }];
  assert.deepEqual(coffeeFood({ quickFoods: mine, commonFoods: common }), { name: 'Oat flat white', portion: 'cup', calories: 95 });
  assert.deepEqual(coffeeFood({ quickFoods: [{ name: 'Coffee cake', calories: 'lots' }, { name: 'tea', calories: 2 }] }), COFFEE);
});

test('the entry is a quick snack on the member\'s local day', () => {
  const e = coffeeEntry({}, new Date(2026, 0, 5, 23, 30));
  assert.deepEqual(e, { ...COFFEE, day: '2026-01-05', meal: 'snack', source: 'quick' });
  assert.equal(localDay(new Date(2026, 11, 31)), '2026-12-31');
});

test('server refusals become plain messages', () => {
  assert.match(logError(409, { error: 'plugin_not_installed' }), /Install/);
  assert.match(logError(409, { error: 'day_full' }), /full/);
  assert.match(logError(401, {}), /Sign in/);
  assert.match(logError(500, null), /Try again/);
});
