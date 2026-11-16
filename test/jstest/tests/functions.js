function add(a, b) { return a + b; }
check(add(2, 3), 5, "call");
check(hoisted(), "up", "hoisted before its line");
function hoisted() { return "up"; }
var counter = (function () { var n = 0; return function () { return ++n; }; })();
counter(); counter();
check(counter(), 3, "closure");
var sq = x => x * x;
check(sq(7), 49, "arrow");
var sum = (...xs) => xs.reduce((a, b) => a + b, 0);
check(sum(1, 2, 3, 4), 10, "rest");
function defaults(a, b = 10) { return a + b; }
check(defaults(1), 11, "default");
function args() { return arguments.length; }
check(args(1, 2, 3), 3, "arguments");
var fact = function f(n) { return n <= 1 ? 1 : n * f(n - 1); };
check(fact(10), 3628800, "named expression recursion");
var obj = { v: 42, get() { return this.v; }, arrow: function () { return (() => this.v)(); } };
check(obj.get(), 42, "method this");
check(obj.arrow(), 42, "arrow this");
check(add.call(null, 1, 2), 3, "call");
check(add.apply(null, [4, 5]), 9, "apply");
var bound = add.bind(null, 10);
check(bound(5), 15, "bind");
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.len = function () { return Math.sqrt(this.x * this.x + this.y * this.y); };
var p = new Point(3, 4);
check(p.len(), 5, "constructor and prototype");
check(p instanceof Point, true, "instanceof");
check(Math.max(...[1, 9, 3]), 9, "spread call");
function deep(n) { return n ? deep(n - 1) + 1 : 0; }
check(deep(200), 200, "recursion");
var r;
try { deep(100000); } catch (e) { r = e instanceof RangeError; }
check(r, true, "too much recursion is a RangeError");
var log = [];
for (var i = 0; i < 3; i++) log.push(((j) => () => j)(i)());
check(log, [0, 1, 2], "closures in a loop");
print("done");
