// Run: node tests/recipe_test.js
// Host check of buildRecipe against the blob jura-connect brewed on a Z10 (PROTOCOL.md 5.9).
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const ctx = { HTMLElement: class {}, customElements: { define() {} }, window: {} };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(__dirname + '/../card/jura-z10-card.js', 'utf8'), ctx);
const p = (arg, tag, def, items = []) => ({ arg, tag, def, items, min: 0, max: 300, step: 1 });
const milkcoffee = { code: 0x05, double: 0, params: [p('F2_H', 'GRINDER_FREENESS', 0x20, [1]), p('F3', 'COFFEE_STRENGTH', 8, [1]),
  p('F4', 'WATER_AMOUNT', 90), p('F7', 'TEMPERATURE', 2, [1]), p('F6', 'MILK_FOAM_AMOUNT', 2), p('F14_1', 'MILK_FOAM_TEMP', 5),
  p('F5', 'MILK_AMOUNT', 3), p('F14_2', 'MILK_TEMP', 5)] };
const vals = Object.fromEntries(milkcoffee.params.map((q) => [q.arg, q.def]));
assert.strictEqual(ctx.buildRecipe(milkcoffee, vals, new Set(), new Set()), '05000812030202000100000000000000');
// test parameters go out only once changed
assert.strictEqual(ctx.buildRecipe(milkcoffee, vals, new Set(['F2_H', 'F14_1', 'F14_2']), new Set()), '05200812030202000100000000550000');
assert.strictEqual(ctx.buildRecipe(milkcoffee, vals, new Set(), new Set(['coldbrew'])), '05000812030280000100000000000000');
assert.strictEqual(ctx.buildRecipe(milkcoffee, vals, new Set(['F2_H']), new Set(['powder'])), '05000012030202000100000000000000');
assert.strictEqual(ctx.buildRecipe({ ...milkcoffee, double: 0x31 }, vals, new Set(), new Set(['double'])), '31000812030202000100000000000000');
assert.strictEqual(ctx.buildRecipe(milkcoffee, vals, new Set(), new Set(['xtrashot'])), '05000812030202020100000000000000');
// brewed on a Z10 as a Cappuccino: options write different bytes and combine
assert.strictEqual(ctx.buildRecipe(milkcoffee, vals, new Set(), new Set(['coldbrew', 'xtrashot'])), '05000812030280020100000000000000');
console.log('recipe ok');
