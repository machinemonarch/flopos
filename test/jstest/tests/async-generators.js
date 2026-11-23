// Async generators and for await.
var log = [];
async function* count(n) { for (var i = 0; i < n; i++) { await null; yield i; } return 'end'; }
async function* agen2() { var got = yield 'a'; log.push('got ' + got); yield await Promise.resolve('b'); }
var obj = { async *m() { yield 'method'; } };
class K { static async *s() { yield 'static'; } }
var expr = async function* () { yield* [Promise.resolve('p1'), 'p2']; yield* count(2); };
(async function () {
    var seen = [];
    for await (var x of count(3)) seen.push(x);
    check(seen, [0, 1, 2], 'for await over an async generator');
    var g = count(1);
    check(typeof g.next, 'function', 'it has next');
    check(g['@@asyncIterator']() === g, true, 'it is its own async iterator');
    var r1 = await g.next(), r2 = await g.next(), r3 = await g.next();
    check([r1, r2, r3], [{value: 0, done: false}, {value: 'end', done: true}, {value: undefined, done: true}], 'next() by hand, to the end');
    var a = agen2();
    var pa = a.next(), pb = a.next('X');                     /* queued: the second waits for the first */
    check([await pa, await pb], [{value: 'a', done: false}, {value: 'b', done: false}], 'requests queue in order');
    check(log, ['got X'], 'the value given to next() comes back from yield');
    for await (var y of obj.m()) seen.push(y);
    for await (var z of K.s()) seen.push(z);
    check(seen.slice(3), ['method', 'static'], 'async generator methods');
    var all = []; for await (var w of expr()) all.push(w);
    check(all, ['p1', 'p2', 0, 1], 'yield* over a list and over an async generator');
    var plain = []; for await (var v of [Promise.resolve(1), 2]) plain.push(v);
    check(plain, [1, 2], 'for await over a plain list awaits each');
    async function* bad() { yield 1; throw new Error('boom'); }
    var caught = ''; try { for await (var q of bad()) {} } catch (e) { caught = e.message; }
    check(caught, 'boom', 'a throw inside reaches for await');
    var t = count(5); await t.next();
    check(await t.return('early'), {value: 'early', done: true}, 'return() ends it');
    check((await t.next()).done, true, 'and it stays ended');
    var threw = ''; try { for (var s of count(1)) {} } catch (e) { threw = e.constructor.name; }
    check(threw, 'TypeError', 'plain for-of over an async generator is refused');
    (async function* () {})().next().then(function (r) { check(r.done, true, 'an empty one'); print('done'); });
})().catch(function (e) { print('FAIL ' + e); });
