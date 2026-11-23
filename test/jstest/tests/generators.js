// Generators: function*, yield, yield*, next/return/throw, for-of, spread, methods.
function* count(n) { for (let i = 0; i < n; i++) yield i; return 'end'; }
var g = count(3);
check(g.next(), {value: 0, done: false}, 'first');
check(g.next(), {value: 1, done: false}, 'second');
check(g.next(), {value: 2, done: false}, 'third');
check(g.next(), {value: 'end', done: true}, 'its return');
check(g.next(), {done: true}, 'after the end');
check([...count(4)], [0, 1, 2, 3], 'spread');
var sum = 0; for (const x of count(5)) sum += x; check(sum, 10, 'for-of');
function* echo() { var got = yield 'ready'; while (true) got = yield 'got ' + got; }
var e = echo(); e.next(); check(e.next('a').value, 'got a', 'next(v) is yield\'s value');
function* outer() { yield 1; yield* count(2); yield* [7, 8]; yield 9; }
check([...outer()], [1, 0, 1, 7, 8, 9], 'yield*');
function* guarded() { try { yield 1; yield 2; } catch (err) { yield 'caught ' + err; } }
var t = guarded(); t.next(); check(t.throw('x').value, 'caught x', 'throw() into a try');
var r = count(10); r.next(); check(r.return(42), {value: 42, done: true}, 'return()');
check(r.next().done, true, 'done after return()');
var o = { *items() { yield 'a'; yield 'b'; } }; check([...o.items()].join(''), 'ab', 'a generator method');
class Bag { constructor() { this.xs = [3, 4]; } *each() { for (const x of this.xs) yield x * 2; } }
check([...new Bag().each()], [6, 8], 'a generator method in a class, with this');
var lazy = (function* () { let a = 0, b = 1; while (true) { yield a; [a, b] = [b, a + b]; } })();
var fib = []; for (const v of lazy) { if (v > 20) break; fib.push(v); } check(fib.join(','), '0,1,1,2,3,5,8,13', 'endless, stopped by break');
function* args(a, b = a * 2) { yield a + b; } check(args(1).next().value, 3, 'parameters before the first next');
var it = { '@@iterator': function () { return count(2); } };
check([...it], [0, 1], 'an object that gives its own iterator');
function* thrower() { yield 1; throw new Error('x'); }
var th = thrower(), thm = ''; th.next();
try { th.next(); } catch (e) { thm = e.message; }
check([thm, th.next().done], ['x', true], 'a throw out of a generator, which is then finished');
print('done');
