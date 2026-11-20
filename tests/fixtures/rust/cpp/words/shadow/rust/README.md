# Rust shadow for `words`

The original crate runs first; the C++ port runs second and disagreements go to
`shadow/words.shadow.jsonl`. Add the crate to the app:

```toml
[dependencies]
words_shadow = { path = "/mnt/data/Projects/OpenSource/Admin/cpp-anything/tests/fixtures/rust/cpp/words/shadow/rust" }
```

Build the port first (`cpp-anything shadow .` builds `build/libcppan_shadow.so`), and
set `LD_LIBRARY_PATH` to that build directory if the linker did not record the rpath.
Set `CPPAN_SHADOW=off` to bypass the comparison.
