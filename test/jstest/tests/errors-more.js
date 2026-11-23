// Error causes, URIError, AggregateError.
check(new Error('m', {cause: 'c'}).cause, 'c', 'new Error with a cause');
check('cause' in new Error('m'), false, 'no cause, no property');
check(new TypeError('t', {cause: 0}).cause, 0, 'a cause that is falsy');
var u = new URIError('bad'); check([u instanceof URIError, u instanceof Error, u.name, u.message], [true, true, 'URIError', 'bad'], 'URIError');
var ag = new AggregateError([1, 2], 'all', {cause: 'x'});
check([ag.errors, ag.message, ag.name, ag.cause, ag instanceof Error], [[1, 2], 'all', 'AggregateError', 'x', true], 'AggregateError');
class E2 extends Error { constructor(m) { super(m, {cause: 'inner'}); } }
check(new E2('z').cause, 'inner', 'super with a cause');
print('done');
