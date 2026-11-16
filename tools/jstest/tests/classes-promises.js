class Animal {
  constructor(name) { this.name = name; }
  speak() { return this.name + " makes a sound"; }
  get loud() { return this.speak().toUpperCase(); }
  static create(n) { return new Animal(n); }
}
class Cat extends Animal {
  speak() { return super.speak() + ": miaow"; }
}
var c = new Cat("Saffran");
check(c.speak(), "Saffran makes a sound: miaow", "super method");
check(c instanceof Animal, true, "extends instanceof");
check(Animal.create("Ozzy").name, "Ozzy", "static");
check(new Animal("x").loud, "X MAKES A SOUND", "class getter");
class Err extends Error { constructor(m) { super(m); this.extra = 1; } }
var e = new Err("oops");
check(e.message, "oops", "extends Error");
var order = [];
Promise.resolve(1).then(v => { order.push("then " + v); return v + 1; }).then(v => order.push("then " + v));
order.push("sync");
new Promise((res, rej) => rej(new Error("no"))).catch(err => order.push("caught " + err.message));
Promise.all([1, Promise.resolve(2), new Promise(r => setTimeout(() => r(3), 10))]).then(vs => {
  order.push("all " + vs.join("+"));
});
setTimeout(() => {
  check(order, ["sync", "then 1", "caught no", "then 2", "all 1+2+3"], "promise order");
  print("done");
}, 50);
