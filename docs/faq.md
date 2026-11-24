# FAQ

**Does cpp-anything translate my Python to C++?**
No. It records what the code does, generates a project that must reproduce it, and
tells you — precisely — when the port does not. You or your agent write the C++.

**Why C++ and not Rust?**
Because that is the migration you asked for. The mechanism is language-agnostic:
record behaviour, replay it, gate on agreement. The target's idioms are the only
part that changes. Compile speed depends on the protocol, and the C++ advantage
is in the loop you run while developing: in the shipped
[xsv-cpp](../xsv-cpp/report/REPORT.md) comparison, touching one source file and
rebuilding takes 1.53 s in C++ against 1.67 s with cargo, because Ninja
recompiles one translation unit while cargo rebuilds the crate. If compile
latency drives your choice, measure your own edit loop; cpp-anything's claim is
the proven porting loop, not a compiler race.

**Is `diff` passing the same as "equivalent"?**
No. It means "matches the original on every recorded case". Branches the corpus does
not reach are unverified; add real inputs with `record --inputs`. This is the honest
limit of any behavioural check, and it is why the corpus is biased towards edges.

**Why does my port need C++23? Must I upgrade my compiler?**
You need C++20 at minimum; `cpp-anything` defaults to C++23 because the language and
library there are what the generated code is written for. On a C++20 target the same
port source compiles: `CPPAN_HAS_EXPECTED` selects `std::expected` or the equivalent
`cppan::Result`. What you cannot do is target C++17: the wire format needs a
value-or-error type and the code uses `std::span` and ranges.

**Which compiler version do I need?**
`cpp-anything matrix features --toolchain gcc --version 13 --std c++23` answers for
one release; [selecting-a-toolchain](guides/selecting-a-toolchain.md) has the minimum
per standard. Defaults: GCC 13, Clang 17 with libc++ 16+, MSVC 19.33.

**The matrix says my compiler supports `std::print`, but it does not compile.**
The matrix describes the *library* that ships with the compiler (libstdc++, libc++,
MSVC STL), and cppreference's tables can lag a patch release. `profile.hpp` is a
claim, not a guarantee; if the compiler disagrees, fix the data
(`scripts/sync_matrix.py`, then `matrix docs`) rather than the guide. The compiler
wins.

**Why are error messages not compared?**
Because they are rarely a contract and change between Python patch releases;
comparing them would fail ports for wording. `--strict-errors` opts in when your
callers do depend on the text.

**Why is `-0.0` not equal to `0.0`?**
Because formatting exposes it (`-0.0` prints with a sign), so a port that loses the
sign changed behaviour. `--tolerance` relaxes numeric comparison when you decide the
sign does not matter.

**Why does my returned dict order not matter, but my list order does?**
A Python `dict` becomes `std::map`, which iterates sorted. The diff compares objects
as maps (key order is not observable through a mapping) and arrays in order.

**Can I port a whole app, not one module?**
Yes, one unit at a time: port the units the app's hot path calls first, wire each
ported unit in where the Python call was, and keep `gate` in CI for every unit. A
single "port the app" command would hide which parts are proven and which are not.

**Where do the compiler numbers come from, and how fresh are they?**
From cppreference's compiler support templates, fetched by `scripts/sync_matrix.py`;
`data/matrix.json` records the page revision and a SHA-256 of what was read, and
`docs/guides/README.md` shows it. `rsi check` fails if the guides drift from the data.

**Does it work offline?**
Yes. The matrix is embedded in the binary; `analyze`/`record`/`bench` need only the
local Python interpreter; `build`/`diff`/`gate` need CMake and a C++ compiler. Only
`scripts/sync_matrix.py` needs the network, and `--offline` parses its cache.

**Windows?**
Generated projects pass `/Zc:__cplusplus`, `/W4` and `/permissive-`; the driver and
the codec are portable C++23. `doctor` reports the toolset it finds. macOS works with
Apple Clang's libc++; `--toolchain apple-clang` uses the matrix column for it.

**How do I update cpp-anything without breaking my ports?**
`bridge` refuses to overwrite a non-empty project without `--force`, and `refresh`
never touches `src/port.cpp`. Re-run `gate` after upgrading: it re-checks the
toolchain against the new matrix.
