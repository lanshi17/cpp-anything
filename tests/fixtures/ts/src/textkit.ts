// textkit: a tiny text surface, written the way a small TypeScript module is.
// Every function is synchronous and exported by name; `parseCount` is the one
// that raises, so the recording contains failures as well as values.

/** Collapse whitespace runs and trim the ends. */
export function squish(text: string): string {
  return text.replace(/\s+/g, " ").trim();
}

/** Parse a signed decimal count; anything else is a TypeError. */
export function parseCount(text: string): number {
  if (typeof text !== "string") throw new TypeError("parseCount expects a string");
  if (!/^-?\d+$/.test(text)) throw new TypeError("parseCount expects a decimal count");
  return Number.parseInt(text, 10);
}

/** Length of the shared prefix of two strings. */
export function commonPrefix(left: string, right: string): number {
  const limit = Math.min(left.length, right.length);
  let index = 0;
  while (index < limit && left[index] === right[index]) index += 1;
  return index;
}

/** -1, 0 or 1: the usual ordering of two numbers. */
export function compare(left: number, right: number): number {
  return left < right ? -1 : left > right ? 1 : 0;
}
