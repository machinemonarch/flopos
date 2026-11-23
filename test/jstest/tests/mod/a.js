export const one = 1;
export let counter = 0;
export function bump() { counter++; return counter; }
export default class Thing { hi() { return 'thing'; } }
export { one as uno, two };
var two = 2;
export const { p, q: queue = 'Q' } = { p: 'P' };
