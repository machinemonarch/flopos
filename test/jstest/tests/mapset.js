// Map, Set, WeakMap, WeakSet: the standard's behaviour, natively.
var m = new Map([[1, 'a'], ['1', 'b'], [NaN, 'n'], [-0, 'z']]);
check([m.size, m.get(1), m.get('1'), m.get(NaN), m.get(0), m.has(-0)], [4, 'a', 'b', 'n', 'z', true], 'keys by SameValueZero');
var o = {}; m.set(o, 'obj'); check(m.get(o), 'obj', 'an object key'); check(m.get({}), undefined, 'another object');
check(m.set('x', 1), m, 'set gives the map back');
var seen = []; for (var [k, v] of m) seen.push(k); check(seen.length, 6, 'for-of over a Map gives entries');
check([...m.keys()].slice(0, 2), [1, '1'], 'keys in insertion order');
check(Array.from(m.values()).pop(), 1, 'Array.from over values');
m.delete('1'); check([m.size, m.has('1'), [...m.keys()][1] !== m.keys()], [5, false, true], 'delete');
var order = [], mm = new Map([[1, 1], [2, 2], [3, 3]]);
for (var [k2] of mm) { order.push(k2); if (k2 === 1) { mm.delete(2); mm.set(4, 4); } }
check(order, [1, 3, 4], 'changes during for-of: deleted skipped, added visited');
var fe = []; mm.forEach(function (v, k, map) { fe.push(k + ':' + v + (map === mm)); }); check(fe, ['1:1true', '3:3true', '4:4true'], 'forEach');
mm.clear(); check([mm.size, mm.get(1)], [0, undefined], 'clear');
var big = new Map(); for (var i = 0; i < 5000; i++) big.set('k' + i, i);
for (var i = 0; i < 5000; i += 2) big.delete('k' + i);
check([big.size, big.get('k4999'), big.get('k4998')], [2500, 4999, undefined], 'five thousand keys, half deleted');
var s = new Set('hello'); check([...s].join(''), 'helo', 'a Set from a string');
check([...new Set([3, 1, 3, 2, 1])], [3, 1, 2], 'removing duplicates by spreading a Set');
check([s.has('l'), s.add('z') === s, s.size, s.delete('h'), s.size], [true, true, 5, true, 4], 'Set methods');
check([...s.entries()][0], ['e', 'e'], 'Set entries are pairs of the value');
check(new Set([1]).keys().next(), {value: 1, done: false}, 'next() by hand');
var it = new Set([1]).values(); it.next(); check(it.next().done, true, 'a finished iterator');
check(it['@@iterator']() === it, true, 'an iterator is its own iterable');
check(Object.prototype.toString.call(new Map()), '[object Map]', 'toString tag');
var wm = new WeakMap(), key = {}; wm.set(key, 5);
check([wm.get(key), wm.has({}), wm.delete(key), wm.has(key)], [5, false, true, false], 'WeakMap');
var threw = false; try { wm.set('str', 1); } catch (e) { threw = e instanceof TypeError; } check(threw, true, 'WeakMap takes objects only');
var ws = new WeakSet([key]); check(ws.has(key), true, 'WeakSet');
threw = false; try { Map(); } catch (e) { threw = true; } check(threw, true, 'Map without new');
class Counter extends Map { inc(k) { this.set(k, (this.get(k) || 0) + 1); return this; } }
var c = new Counter(); c.inc('a').inc('a'); check([c.get('a'), c instanceof Map, c.size], [2, true, 1], 'class extends Map');
check([...c], [['a', 2]], 'spreading a Map subclass');
var g = Map.groupBy([1, 2, 3, 4], function (x) { return x % 2 ? 'odd' : 'even'; });
check([g.get('odd'), g.get('even')], [[1, 3], [2, 4]], 'Map.groupBy');
var og = Object.groupBy(['ab', 'c', 'de'], function (x) { return x.length; });
check([og[2], og[1], Object.getPrototypeOf(og)], [['ab', 'de'], ['c'], null], 'Object.groupBy');
var m2 = new Map(new Map([[1, 2]])); check(m2.get(1), 2, 'a Map from a Map');
var [first] = new Set(['only']); check(first, 'only', 'destructuring a Set');
function* gen() { yield 1; yield 2; }
check(Array.from(gen(), function (x) { return x * this.k; }, {k: 10}), [10, 20], 'Array.from over a generator, with this');
check(Array.from('a\ud83d\ude00b').length, 3, 'Array.from a string by characters');
check(Array.from({length: 2, 0: 'x'}), ['x', undefined], 'Array.from an array-like');
print('done');
