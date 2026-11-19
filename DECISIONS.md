# Decisions

Choices made so far, with the reason. Each one can be revisited.

## Product

| # | Decision | Reason |
| --- | --- | --- |
| D1 | A toolkit an agent drives, not a translator that calls a model. | No model cost or vendor lock-in, and the part an agent cannot do reliably is the checking, not the writing. |
| D2 | Target C++20/23, not "whatever the compiler accepts". | The user's stated policy: new language features first, C++20/23 baseline. Generated code says `std::span`, `std::string_view`, `[[nodiscard]]`, ranges; `std::expected` where the library has it. |
| D3 | Python is the first source language; the loop is language-independent. | Python is where most code that wants to move to C++ lives, and its annotations give the generator real types. `wire`, `diff`, `gate`, `bench` never mention Python; a new language is an analyzer, a recorder and a call mapping. |
| D4 | Written in Rust with zero dependencies. | One static binary that runs on a bare machine: no JSON crate, no argv crate, no TOML. JSON, the type model and hashing are a few hundred lines with unit tests; the build needs nothing from the network. |
| D5 | The matrix is embedded at compile time. | `matrix` answers identically offline and in CI; syncing the data and rendering the guides are explicit steps, not side effects of running the tool. |
| D6 | The guides are generated and drift-checked. | 651 rows of support data cannot be maintained by hand; `matrix docs --check` fails when a guide disagrees with `data/matrix.json`, and `rsi check` runs it. |

## The loop

| # | Decision | Reason |
| --- | --- | --- |
| L1 | The unit is one module. | Small enough to port in one sitting, to gate on its own, and to review: a per-function loop loses the ability to share helpers, a per-app loop hides which part is unproven. |
| L2 | `bridge` generates `port.hpp`, `dispatch.cpp` and the driver; only `src/port.cpp` is the user's. | A port is a signature contract: the analyzer owns the declarations, the user owns the bodies. `refresh` regenerates the contract after a signature change. |
| L3 | `bridge` refuses a non-empty directory without `--force`, and never overwrites `port.cpp` on `refresh`. | A port in progress is the user's work; regenerating scaffolding must not delete it. |
| L4 | Errors compare by exception type, not message. | Messages are rarely a contract and change between Python versions; `--strict-errors` is there when they are. |
| L5 | Floats compare bit-exactly by default; `--tolerance` opts into relative comparison. | `-0.0` and `NaN` are observable through formatting; a silent tolerance hides the bug the recording exists to find. |
| L6 | Objects compare unordered, arrays ordered. | A Python dict becomes `std::map`; its iteration order is an implementation detail the caller cannot rely on, unlike a list's order. |
| L7 | A case that produces no result is a failure, not a skip. | A driver that crashes halfway must not look like a partial pass. |
| L8 | Recording stays inside `int64`. | A value outside it cannot cross as `std::int64_t`, so the analyzer reports "Python ints are unbounded" as a risk instead of recording a case the generated signature can never satisfy. |
| L9 | The recorder also emits a few seeded random arguments. | Deterministic corpora alone let a port be tuned to the table; random cases catch the gaps without making runs unreproducible. |

## Generated C++

| # | Decision | Reason |
| --- | --- | --- |
| C1 | `cppan::Result<T>` is the port's return type: `std::expected` on C++23, a small equivalent on C++20. | The wire format needs value-or-error; requiring C++23 would have made "C++20 support" a lie, and forcing a fallback everywhere would have hidden `std::expected` from C++23 targets. |
| C2 | `std::string_view` in, `std::string` out, `std::span<const T>` for lists. | The driver owns the decoded arguments; the port must not copy them, and a span says "read-only view" in the signature. |
| C3 | The JSON codec is embedded and dependency-free. | Generated projects must build with a compiler and CMake only; pulling nlohmann into every port to replay cases would be a dependency the user never asked for. |
| C4 | `[[nodiscard]]`, `const`, `noexcept` where true, ranges instead of index loops. | The generated code is the style example for the port; a C++23 project that reads like C++11 teaches the wrong thing. |
| C5 | Warnings on (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, `/W4 /permissive-`), not `-Werror`. | `-Wconversion` catches the `size_t`/`int64_t` mistakes a Python port makes; `-Werror` would block a half-finished port for a warning that gate does not consider a failure. |
| C6 | `/Zc:__cplusplus` in the generated `CMakeLists.txt`. | Without it MSVC reports `199711L` and every `#if __cplusplus` check — including the profile's `static_assert` — lies. |
| C7 | `profile.hpp` claims only what the matrix records, per facility. | `__has_include` answers "is there a header", not "is this implemented"; the matrix answers the question the port actually has. |

## Matrix and RSI

| # | Decision | Reason |
| --- | --- | --- |
| M1 | The data is cppreference's wikitext, parsed by a script, with the page revision and a SHA-256 of what was read. | The rendered HTML changes; the templates are the table. Provenance makes a stale or wrong cell traceable and reproducible. |
| M2 | The MSVC alias table (`2022.7` → `19.37`) is applied to the MSVC column only. | ICC's `2022.2` is a real ICC version, not a Visual Studio alias; applying the table globally silently rewrote 31 ICC rows during development. |
| M3 | "First complete release" ignores features cppreference marks partial, and says so. | Modules are partial in every compiler; treating partial as missing would print `—` for every C++20 row and hide the useful answer. |
| M4 | `rsi check` = data + guides + CLI goldens + a rebuilt example replayed through its own diff. | The tool's claims are data, generated text, printed output and generated code; each is checked by the machinery it would use on a user's port. |
| M5 | MCP tools shell out to the CLI binary instead of re-implementing commands. | One implementation cannot drift from itself; the subprocess boundary also keeps the JSON-RPC stream clean of command output. |
| M6 | CLI goldens are re-recorded explicitly (`rsi record`), never auto-updated. | A golden that updates itself proves nothing; an intentional output change should be a reviewed diff in `tests/golden/cli.jsonl`. |

## Whole app and around the loop

| # | Decision | Reason |
| --- | --- | --- |
| W1 | Whole-app cases are JSONL of `{name, args, stdin, env, files, expect{stdout, stderr, exit, files, fallback}}`, stored in `cpp/app-cases/`. | One file a human can read and an agent can diff; binary or structured formats buy nothing the recording needs. |
| W2 | Every case runs twice; a case that differs is recorded but marked unstable and never fails replay. | A timestamp the normalisers miss should not make the gate red forever; the honest answer is "the app does not produce one output here". |
| W3 | The front door `exec`s the original, it does not wrap it. | exec gives identical stdout, stderr, exit code and signals; a wrapper changes the process a case observes. |
| W4 | `app lift` requires zero fallback across every case. | "Standalone" must mean no path to the original, not "mostly ported". |
| W5 | `record --from-tests` injects a `sitecustomize` hook through PYTHONPATH instead of rewriting the project. | It works for pytest, unittest and plain scripts, leaves no trace in the user's tree, and cannot corrupt sources it edits. |
| W6 | `map` writes `cpp/PORT_MAP.md` and is committed. | The drift it detects is upstream change; a committed map makes the next `map` a reviewable diff. |
| W7 | `setup` writes only its own entries, keeps key order, and backs up first. | The user's agent config is not ours; a tool that rewrites it wholesale loses settings it never understood. |
| W8 | MCP tools shell out to the CLI binary. | One implementation cannot drift from itself, and the subprocess boundary keeps JSON-RPC output clean. |
| W9 | `profile` advises batching when per-call cost is low and calls are many. | One call per item costs more than the work; the advice is the difference between a port that pays and one that does not. |
