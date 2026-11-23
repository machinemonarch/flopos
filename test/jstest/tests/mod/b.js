import Thing, { bump as up, counter } from './a.js';
import * as A from './a.js';
export * from './a.js';
export * as all from './a.js';
export { default as Renamed } from './a.js';
export function seen() { return counter; }
export const viaNs = A.one + A.uno;
export default function () { return new Thing().hi() + up(); }
