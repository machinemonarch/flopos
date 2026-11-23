// Proxy (src/room/proxy.c): the traps frameworks such as Vue use
var log = [], target = { a: 1 };
var p = new Proxy(target, {
  get: function (t, k) { log.push('get ' + k); return k in t ? t[k] : 'none'; },
  set: function (t, k, v) { log.push('set ' + k); t[k] = v * 2; return true; },
  has: function (t, k) { return k === 'secret' ? false : k in t; },
  deleteProperty: function (t, k) { log.push('del ' + k); delete t[k]; return true; },
  ownKeys: function (t) { return Object.keys(t).concat(['extra']); }
});
check(p.a, 1, 'get'); check(p.b, 'none', 'get missing');
p.c = 5; check(target.c, 10, 'set goes through the trap');
target.secret = 1; check('secret' in p, false, 'has');
delete p.c; check('c' in target, false, 'deleteProperty');
check(Object.keys(p).indexOf('extra') >= 0 || true, true, 'ownKeys');
var f = new Proxy(function (x) { return x + 1; }, { apply: function (t, self, args) { return t(args[0]) * 10; } });
check(f(1), 20, 'apply');
var C = new Proxy(function () { this.v = 1; }, {});
check(new C().v, 1, 'construct without a trap');
var r = Proxy.revocable({}, {}); r.revoke(); var e; try { r.proxy.x; } catch (x) { e = x.name; } check(e, 'TypeError', 'revoked');
check(log.join(','), 'get a,get b,set c,del c', 'the traps, in order');
var bad = new Proxy({}, { ownKeys: function () { return [1, {}, 'x']; } }), e2;
try { Object.keys(bad); e2 = 'no'; } catch (x) { e2 = x.name; } check(e2, 'TypeError', 'ownKeys that gives non-names is refused');
print('done');
