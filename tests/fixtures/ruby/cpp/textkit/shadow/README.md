# Shadow for `textkit` (ruby)

The original Ruby runs first, the C++ port runs second, and every
disagreement is appended to `shadow/textkit.shadow.jsonl`; callers only ever see the
original's result. The binding talks to the port through the C ABI
(`cppan_shadow_call` / `cppan_shadow_free`) in `build/libcppan_shadow.so`.

Build the port first: `cpp-anything build <project>`.

Environment:
- `CPPAN_SHADOW=off` calls only the original.
- `CPPAN_SHADOW_LOG=...` writes the log elsewhere.
