// Syntax that sites' bundles use and the engine refused before 1.47.

// a number, then a property: 1..toString (core-js has it)
check(1..toString(), "1", "1..toString");
check(2e3 + .5e-1, 2000.05, "exponents");

// ...rest in an object pattern
var o = {a: 1, b: 2, c: 3, d: 4};
var {a, b: bee, ...rest} = o;
check(a + bee + JSON.stringify(rest), '3{"c":3,"d":4}', "object rest");
function keysBut({x, ...more}) { return Object.keys(more).join(); }
check(keysBut({x: 1, y: 2, z: 3}), "y,z", "object rest in a parameter");

// defaults on pattern parameters, in order
function f({a = 1, b} = {}, [c, d] = [3, 4], e = a + c) { return [a, b, c, d, e].join(); }
check(f(), "1,,3,4,4", "pattern defaults");
check(f({a: 7, b: 8}, [9]), "7,8,9,,16", "pattern defaults given");

// tagged templates and String.raw
function tag(s, ...v) { return s.join("|") + "/" + s.raw.join("|") + "/" + v.join(); }
check(tag`a${1}b${2 + 3}c`, "a|b|c/a|b|c/1,5", "tagged template");
check(tag`n\n`, "n\n/n\\n/", "tagged template raw");
check(String.raw`x\t${1 + 1}y`, "x\\t2y", "String.raw");

// class fields: instance, static, computed, private, static blocks
class A {
  x = 1; y; static s = "st"; #p = 7; ["c" + 1] = "comp";
  getP() { return this.#p; } has(q) { return #p in q; }
  #hid() { return "h"; } callHid() { return this.#hid(); }
  get ["g" + "x"]() { return "gx"; }
  static { this.blk = A.s + "!"; }
}
var i = new A();
check([i.x, i.y, A.s, i.getP(), i.c1, i.has(i), i.callHid(), i.gx, A.blk].join(), "1,,st,7,comp,true,h,gx,st!", "class fields");
check(Object.keys(i).join(), "x,y,c1", "private fields stay hidden");
class B extends A { z = this.x + 10; constructor(q) { super(); this.q = q; } }
var j = new B(5);
check([j.x, j.z, j.q, j.getP()].join(), "1,11,5,7", "fields after super()");
class C2 extends A { w = 3 }
check(new C2().w + new C2().x, 4, "fields with the default constructor");
class D { arrow = () => this.v; v = "dv"; }
check(new D().arrow(), "dv", "an arrow field sees this");
class E { get = 1; static = 2; async = 3 }
var e = new E();
check([e.get, e.static, e.async].join(), "1,2,3", "fields named like keywords");
class F { n = 0
  m = 1
  inc() { return ++this.n; } }
var ff = new F(); ff.inc();
check(ff.n + ff.m, 2, "fields without semicolons");

// computed getters and setters in an object
var g = { get ["a" + "b"]() { return "ab"; }, set ["q"](v) { this._q = v; } };
g.q = 4;
check(g.ab + g._q, "ab4", "computed getter and setter");

// for await over a list
var awaited = "";
async function fa() { for await (const v of [Promise.resolve(1), 2]) awaited += v; }
fa().then(function () { check(awaited, "12", "for await"); print("done"); });
