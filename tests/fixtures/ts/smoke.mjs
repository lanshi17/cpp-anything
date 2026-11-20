// Smoke test for the generated TypeScript shadow: every call returns what the
// original returns, and the port is compared on the side.
import * as textkit from "./cpp/textkit/shadow/textkit_shadow.mjs";

const results = {
  squish: textkit.squish("  Hello   World  "),
  parseCount: textkit.parseCount("42"),
  commonPrefix: textkit.commonPrefix("prefix", "presto"),
  compare: textkit.compare(2, 7),
};

let threw = null;
try {
  textkit.parseCount("not a count");
} catch (error) {
  threw = `${error.constructor.name}: ${error.message}`;
}

console.log(JSON.stringify({ results, threw }, null, 2));
