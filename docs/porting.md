# Porting rules

The generated project fixes the shape of a port; this page is what goes inside
`src/port.cpp`. It is the C++20/23 side of the Python behaviour you recorded.

## Signatures and types

| Python | C++ in | C++ out |
| --- | --- | --- |
| `str` | `std::string_view` | `std::string` |
| `int` | `std::int64_t` | `std::int64_t` |
| `float` | `double` | `double` |
| `bool` | `bool` | `bool` |
| `list[T]` | `std::span<const T>` | `std::vector<T>` |
| `dict[str, T]` | `const std::map<std::string, T>&` | `std::map<std::string, T>` |
| `tuple[...]` | `const std::tuple<...>&` | `std::tuple<...>` |
| `T \| None` | `std::optional<T>` | `std::optional<T>` |
| no annotation | `const cppan::Json&` | `cppan::Json` |

`std::string_view` points into the driver's decoded argument, which outlives the
call: do not store it. `std::span` is read-only and does not own: copy what you
need to keep.

Every function returns `cppan::Result<T>`: `std::expected<T, PortError>` on C++23
targets, and the same interface defined in `include/cppan/wire.hpp` on C++20 ones.

## Other source languages

The analyzer maps each language's types into the same dialect, so the table above
applies unchanged:

| Source | Maps to `str` / `int` / `float` / `bool` | Lists | No wire form (skipped) |
| --- | --- | --- | --- |
| TypeScript | `string` / `number` → `float` / `boolean` | `T[]`, `Array<T>` | interfaces, classes, generics (unless `--as-json T`) |
| JavaScript | `string` / `number` / `boolean` | arrays of those | untyped values become `Any`, which is `cppan::Json` |
| Go | `string` / `int…uint64`, `byte`, `rune` / `float32/64` / `bool` | `[]T` | structs, pointers, `any`, variadics, multiple results |
| Java | `String` / `int`, `long`, `short`, `byte` / `double`, `float` / `boolean` | `List<String>`, `Collection<String>` | arrays, custom types, generics other than the listed ones |
| Ruby | `String`, `Integer`, `Float`, `true`/`false` | `Array` | symbols, ranges, objects (recorded as `$opaque`/errors) |

Ruby and JavaScript without annotations give every parameter the type `Any`,
which crosses as `cppan::Json`: the body you write reads `is_string()`,
`as_string()`, `as_int()` and so on. Prefer annotations where the language has
them; `--as-json T` exists so a TypeScript generic that only holds data can do the
same deliberately.

Error types are part of the contract. `diff` compares the original's exception
class name with the port's `PortError` type, so raise the same name the runtime
would: `PortError{"NoMethodError", …}` for Ruby, `{"TypeError", …}` for
JavaScript, `{"IllegalArgumentException", …}` for Java.

## Failures are values

```cpp
if (limit < 0) {
  return std::unexpected(PortError{"ValueError", "limit must be >= 0, got " + std::to_string(limit)});
}
```

- The **type** is the Python exception class name. The diff compares it.
- The **message** is for humans; `--strict-errors` makes it part of the contract.
- Never throw for an expected failure: a thrown C++ exception becomes
  `RuntimeError` and will not match `ValueError`.
- Returning `std::unexpected` where the original returns a value is a mismatch, and
  so is the reverse. The diff says which side raised.

## Unicode: code points versus bytes

Python's `str` is a sequence of code points; `std::string` is bytes. Every offset
that reaches the output needs a helper:

```cpp
// count code points (lead bytes only)
std::size_t code_points(std::string_view text);
// byte offset of code point N
std::size_t byte_offset(std::string_view text, std::size_t points);
```

The example port (`examples/textkit-cpp/src/port.cpp`) has both, and uses them for
`truncate`, where `text[:limit]` in Python cuts between characters. Slicing,
`len()`, padding and column widths all hit this.

Unicode case mapping is the same story: `str.lower()` is unicode-aware;
`std::tolower` is ASCII. If the original lowercases non-ASCII text, either bring a
unicode library or record why the port diverges.

## Integers

- Python `int` is unbounded; `std::int64_t` is not. The analyzer flags arithmetic
  that can overflow; a port that must match must widen (`__int128` where available,
  a big-integer library, or an explicit `OverflowError` after agreeing with the
  caller that it is acceptable).
- `int(text)` accepts surrounding whitespace, a sign, underscores and unicode
  digits; `std::from_chars` accepts none of that beyond a sign and ASCII digits.
  Trim first, validate explicitly, and raise `ValueError` where Python does.
- `//` floors towards negative infinity in Python and truncates in C++. `%` follows.
- `/` is always a float in Python; use `double` explicitly in C++.

## Floats

- Python formats floats with the shortest round-trip repr; `std::to_chars` does the
  same, `std::format("{}")` as well, `std::to_string` does not (it uses `%f`).
  A port that formats numbers must use `to_chars`/`format`, never `to_string`.
- `NaN != NaN`, but the recorder and the diff treat `NaN` as equal to `NaN`: the
  same input produced the same value.
- A captured `-0.0` must come back as `-0.0`; the diff is bit-exact by default.

## Containers and order

- A Python `dict` becomes `std::map`: sorted iteration, not insertion order. If the
  original's order is observable (it usually is not, for a returned dict; it is for
  serialized output), use `std::vector<std::pair<…>>` and say so in the port.
- `list.sort()` is stable; `std::sort` is not. Use `std::stable_sort` where ties can
  be observed.
- Sets come back as `{"$set":[…]}` sorted; if the port returns a
  `std::vector` of a set's elements, the diff will not accept it — return
  `cppan::Json` with the `$set` tag instead, and document it.

## Text processing

- `str.split()` with no argument splits on unicode whitespace. `Trim` + a hand-rolled
  splitter using `std::isspace` handles ASCII only; the analyzer flags it.
- `re` and `std::regex`: Python supports lookaround, backreferences and lazy
  quantifiers that `std::regex` does not implement (or implements differently), and
  `\w`/`\b`/case-insensitivity treat non-ASCII text differently. Prefer a small
  hand-written matcher for simple patterns, and record real patterns with
  `record --inputs` rather than trusting generated strings.
- `str.format` and f-strings format numbers with Python's rules; `std::format` is
  close but not identical for padding, alignment and `{}` of floats.

## What the C++ may use

`include/cppan/profile.hpp` says what the target toolchain has. Use the facility if
the macro is 1, and provide a fallback if it is 0:

```cpp
#if CPPAN_HAS_PRINT
  std::println("{}", value);
#else
  std::cout << value << '\n';
#endif
```

`cppan::Result` is always available: `CPPAN_HAS_EXPECTED` decides whether it is
`std::expected` or the C++20 fallback, and the port source is identical. See
[selecting a toolchain](guides/selecting-a-toolchain.md).

Style the generated code already uses, and a port should keep:

- `[[nodiscard]]` on everything that returns a value the caller must look at.
- `const`, `noexcept` where true, `std::string_view` for read-only text.
- Ranges (`std::views::take`, `std::ranges::count_if`) instead of index loops.
- No `unsafe`, no C casts, no `malloc`: the driver owns memory, the port owns logic.
- Warnings on (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, `/W4 /permissive-`).
  `-Wconversion` is what catches the `size_t`/`int64_t` mistakes a Python port makes.

## C++20 modules

`bridge --modules on` generates `src/port.cppm` instead of a header:

```cpp
module;
#include <string>
#include <string_view>
#include "cppan/wire.hpp"

export module textkit;

export namespace textkit {
  [[nodiscard]] cppan::Result<std::int64_t> word_count(std::string_view text);
}
```

The runtime headers stay headers, included in the global module fragment, and every
translation unit that imports the module includes `cppan/wire.hpp` first — the
declarations and the importers then agree on `cppan::Result` and the other types.
`src/port.cpp` becomes a module implementation unit (`module textkit;`) and must not
include the old port header, which no longer exists.

What decides the default:

| Input | Result |
| --- | --- |
| `--modules off`, or a standard below C++20 | header (`include/<ns>/port.hpp`) |
| `--modules auto` and the toolchain is gcc 15+, clang 17+, apple-clang 16+ or msvc 19.36+, and Ninja is installed | module unit |
| `--modules on` and the toolchain or Ninja is missing | an error that names the missing piece |
| `--modules auto` and the toolchain is older | header, with a one-line note at bridge time |

The minimums are curated and ahead of the compiler-support matrix: the matrix
records GCC 11 as the start of module support, and GCC 13 still fails on a two-file
example, so the tool only generates a module unit where it has built one.

Building modules needs Ninja (`python3 -m pip install --user ninja`); `doctor`
reports whether it is on PATH. `import std;` is not emitted: GCC 15 does not ship
a prebuilt std module, so the standard headers stay `#include`s.

## Tests

`diff` is the test suite. Add C++ unit tests only for the reasoning the recording
cannot see: helper functions, invariants, the branch you know the corpus misses.
Keep them in `src/port.cpp` under `#ifdef CPPAN_PORT_TESTS` if you add them, so the
driver build stays clean.
