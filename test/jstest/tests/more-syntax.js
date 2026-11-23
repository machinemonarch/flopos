// new with ...args, destructuring into properties, top-level parts.
class P { constructor(a, b, c) { this.s = a + b + c; } }
var xs = [1, 2];
check(new P(...xs, 3).s, 6, 'new with ...');
check(new P(0, ...[5, 5]).s, 10, 'new with ... after an argument');
check(new Date(...[2020, 0, 2]).getDate(), 2, 'new Date(...)');
var o = {}, arr = [];
[o.a, o['b'], arr[0] = 'd'] = [1, 2];
check([o.a, o.b, arr[0]], [1, 2, 'd'], 'array pattern into properties, with a default');
({x: o.x, y: arr[1] = 'dy'} = {x: 'X'});
check([o.x, arr[1]], ['X', 'dy'], 'object pattern into properties');
var a, b; [a, b] = [b, a] = [1, 2]; check([a, b], [1, 2], 'chained patterns: the right one first');
print('done');
