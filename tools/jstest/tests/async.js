// async and await: the order things happen in, values, throws, try,
// loops, methods, arrows, and values waited on mid-expression.
var order = [];
async function one() { order.push("one starts"); var v = await 1; order.push("one got " + v); return v + 1; }
var p = one();
order.push("after the call");
check(p instanceof Promise, true, "an async function returns a promise");

async function sum(xs) { var t = 0; for (var i = 0; i < xs.length; i++) t += await xs[i]; return t; }
async function mid() { return 10 + await Promise.resolve(5) * 2; }
async function thrower() { await null; throw new Error("boom"); }
async function catcher() { try { await thrower(); return "no"; } catch (e) { return "caught " + e.message; } finally { order.push("finally"); } }
async function rejects() { try { await Promise.reject("nope"); } catch (e) { return "inner " + e; } }
async function early() { throw new TypeError("before any await"); }
var arrow = async x => (await x) * 3;
var arrow2 = async (a, b) => { var r = await a; return r + b; };
var obj = { k: 7, async get() { return this.k + await 1; } };
class C { constructor() { this.v = 2; } async twice() { return (await this.v) * 2; } static async s() { return "static"; } }
var expr = async function named(n) { return n <= 0 ? 0 : n + await named(n - 1); };
async function waits() { var a = await new Promise(r => setTimeout(() => r("later"), 5)); return a; }
async function nested() { var inner = async () => { await null; return this; }; return await inner(); }
function plain() { return "async"; }

Promise.all([p, sum([1, Promise.resolve(2), 3]), mid(), catcher(), rejects(), arrow(Promise.resolve(4)),
             arrow2(Promise.resolve(1), 2), obj.get(), new C().twice(), C.s(), expr(3), waits(),
             early().catch(e => e.name + ": " + e.message), Promise.resolve(plain())]).then(function (r) {
  check(r[0], 2, "value after await");
  check(r[1], 6, "await in a loop");
  check(r[2], 20, "await mid-expression");
  check(r[3], "caught boom", "throw after await, caught by the caller's try");
  check(r[4], "inner nope", "rejected promise, caught inside");
  check(r[5], 12, "async arrow, one parameter");
  check(r[6], 3, "async arrow, two parameters");
  check(r[7], 8, "async method, this");
  check(r[8], 4, "async class method");
  check(r[9], "static", "static async method");
  check(r[10], 6, "named async function expression, recursive");
  check(r[11], "later", "await a timer");
  check(r[12], "TypeError: before any await", "throw before any await rejects");
  check(r[13], "async", "async is still a name");
  check(order.slice(0, 3), ["one starts", "after the call", "one got 1"], "await lets the caller go on");
  check(order.indexOf("finally") >= 0, true, "finally after await");
  print("done");
});
