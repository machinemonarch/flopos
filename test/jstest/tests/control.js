var s = 0;
for (var i = 0; i < 10; i++) { if (i == 3) continue; if (i == 8) break; s += i; }
check(s, 25, "for with break and continue");
var n = 0; while (n < 5) n++;
check(n, 5, "while");
var d = 0; do { d++; } while (d < 3);
check(d, 3, "do while");
function sw(x) {
  switch (x) { case 1: return "one"; case 2: case 3: return "two or three"; default: return "other"; }
}
check(sw(1), "one", "switch 1"); check(sw(3), "two or three", "switch 3"); check(sw(9), "other", "switch default");
var fall = [];
switch (2) { case 1: fall.push(1); case 2: fall.push(2); case 3: fall.push(3); break; case 4: fall.push(4); }
check(fall, [2, 3], "switch fallthrough");
var keys = []; for (var k in { a: 1, b: 2 }) keys.push(k);
check(keys, ["a", "b"], "for in");
var vals = []; for (var v of [5, 6]) vals.push(v);
check(vals, [5, 6], "for of");
var out = [];
outer: for (var a = 0; a < 3; a++) { for (var b = 0; b < 3; b++) { if (b == 1) continue outer; if (a == 2) break outer; out.push(a + "" + b); } }
check(out, ["00", "10"], "labels");
var t = [];
try { t.push("try"); throw new Error("boom"); } catch (e) { t.push(e.message); } finally { t.push("finally"); }
check(t, ["try", "boom", "finally"], "try catch finally");
function f() { try { return "from try"; } finally { t.push("ran"); } }
check(f(), "from try", "return through finally"); check(t[3], "ran", "finally on return");
var caught;
try { null.x; } catch (e) { caught = e instanceof TypeError; }
check(caught, true, "TypeError");
try { nothing_here(); } catch (e) { caught = e.name; }
check(caught, "ReferenceError", "ReferenceError");
var [p, q = 5, ...rest] = [1, undefined, 3, 4];
check([p, q, rest], [1, 5, [3, 4]], "array destructuring");
var { m, n: nn, z = "dz" } = { m: 1, n: 2 };
check([m, nn, z], [1, 2, "dz"], "object destructuring");
var aa = 1, bb = 2; [aa, bb] = [bb, aa];
check([aa, bb], [2, 1], "swap");
let blockLet = 4; const cc = 5;
check(blockLet + cc, 9, "let const");
print("done");
