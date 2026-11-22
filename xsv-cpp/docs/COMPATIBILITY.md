# Compatibility contract (P0)

The reference is `xsv 0.13.0` at commit `2b4cbaa0eecf7b507a612632fe00289b1b358c15`.
“Parity” below means: stdout bytes identical, exit code identical, output files
identical, help text identical, and stderr equal under the class rule in §13.
Where the reference has a bug, the bug is part of the contract.

## 0. P0 status

| command | C++ status | gate cases | notes |
| --- | --- | --- | --- |
| `count` | implemented | 32 | index fast path |
| `headers` | implemented | 12 | tabwriter cell-block emulation |
| `select` | implemented | 16 | full selector grammar |
| `slice` | implemented | 14 | indexed and streaming paths |
| `search` | implemented | 10 | RE2 backend |
| `index` | implemented | 12 | byte-identical `.idx` |
| `sort` | implemented | 12 | stable, numeric, reverse |
| `stats` | implemented | 17 | serial; `-j` accepted |
| `join` | implemented | 19 | all five join modes |

At the time of writing: **144/144 cases pass on `base` and `all`, and on the
ASan/UBSan and TSan builds.**

## 1. Input model

| item | behaviour |
| --- | --- |
| input argument | omitted or `-` means stdin; otherwise a file path |
| delimiter | `,`; a `.tsv` path defaults to `\t`; `-d`/`--delimiter` overrides |
| delimiter value | `\t` or exactly one ASCII character; anything else is a usage error |
| quote | `"` |
| double quote | on (`""` inside a quoted field is one quote) |
| escape | none by default |
| reader terminator | accepts `\n`, `\r\n`, and lone `\r`; empty lines are skipped |
| BOM | a UTF-8 BOM is stripped before the first record |
| headers | first record is the header; `-n`/`--no-headers` disables that |
| `XSV_TOGGLE_HEADERS=1` | inverts the requested header flag |
| ragged rows | error unless the command sets `flexible` |
| UTF-8 | bytes are never validated or transcoded; only commands that need text (`join` key trimming/case folding, `stats` type inference, `headers`) look at UTF-8 |

## 2. Index discovery

For an input path `p`, commands try `p + ".idx"`:

- missing/unreadable index: silently continue without one;
- index exists: compare **whole seconds** of `mtime`; if `mtime(p) > mtime(idx)`,
  fail with `The CSV file was modified after the index file. Please re-create
  the index.`;
- stdin never uses an index;
- an explicitly requested index path (`join` internal use) must open.

## 3. Index file format

Frozen, from `csv-index 0.1.5` `RandomAccessSimple`:

```
offset0  u64 big-endian
offset1  u64 big-endian
...
offsetN-1
N        u64 big-endian
```

- `offset_i` is the reference's seek position for record `i`; it is the scan
  position after the previous record's terminator, before blank lines are
  skipped. For CRLF input this is the LF of the previous pair (the reader
  skips it), not the first field. Reproducing this quirk is required for
  byte-identical indexes;
- the header is record 0 when headers are enabled;
- `N` counts every record written above, header included;
- file length is exactly `(N+1) * 8`;
- `Index::offset(i)` is valid for `0 <= i < N`; otherwise
  `invalid record index i (there are N records)`.

## 4. `count` (implemented)

Flags: `-h/--help`, `-n/--no-headers`, `-d/--delimiter`.

- no input → stdin;
- with a fresh index: `count = N - (headers && N > 0 ? 1 : 0)` without parsing
  the CSV;
- without an index: parse every record; header excluded unless `-n`;
- output: decimal count + `\n`;
- empty file → `0`;
- ragged file → CSV error with the reference's record/line/byte numbering.

Verified against the reference for: header/no-header, quoted newlines, CRLF,
BOM, empty file, stdin, index fast path, stale index, ragged rows.

## 4b. `index` (implemented)

Flags: `-o/--output`, `-d`, `-h`.

- the input must be a path; stdin is rejected (`Cannot use <stdin> here`);
- default output is `<input>.idx`; `-o` writes elsewhere;
- writes the frozen layout in §3, header included;
- the output is byte-compared against the reference for LF, CRLF, BOM, empty,
  quoted and explicit-output cases;
- a CRLF record ends at the CR, so the next record's stored offset points at
  the LF of the pair; this is the reference's behaviour and is reproduced.

## 5. `headers`

Flags: `-j/--just-names`, `--intersect`, `-d`.

- accepts multiple inputs; with more than one, `-j` is implied;
- one input and no `-j`: prints `index<TAB>name`, aligned by tabwriter
  (minwidth 2, padding 2);
- `--intersect` is **not** an intersection in 0.13.0: the code appends a header
  only when it has not appeared in an earlier input, i.e. it prints the
  first-seen union with duplicates removed. Match that;
- duplicate header names within one file are printed as-is;
- names are raw bytes; tabwriter counts Unicode display width.

## 6. `select`

Flags: `-o/--output`, `-n/--no-headers`, `-d`.

Selection grammar is specified in ARCHITECTURE.md §7. Output:

- header row is projected and written unless `-n`;
- every data row is projected in selection order (duplicates and reordering are
  allowed);
- an empty selection (`--` + empty string) is all columns;
- `!` inverts the union;
- errors: index out of bounds, unknown name, `Name[k]` out of range, names with
  `-n`, unclosed quote/bracket.

## 7. `slice`

Flags: `-s/--start`, `-e/--end`, `-l/--len`, `-i/--index`, `-o`, `-n`, `-d`.

Range rules (from `util::range`):

| combination | result |
| --- | --- |
| only `-i N` | `[N, N+1)` |
| `-i` with `-s/-e/-l` | error: `--index cannot be used with --start, --end or --len` |
| `-e` and `-l` together | error: `--end and --len cannot be used at the same time.` |
| neither `-e` nor `-l` | `[start, usize::MAX)` |
| `-e E`, `start > E` | error: end must be >= start |
| `-l L` | `[start, start + L)` |

- no index: read and discard `start` records, write up to `end - start`;
- index: `seek(start)`, then read exactly `end - start` records;
- header is always written unless `-n`, even when the slice is empty;
- `end - start == 0` returns after writing the header.

## 8. `search`

Flags: `-i/--ignore-case`, `-v/--invert-match`, `-s/--select`, `-o`, `-n`, `-d`.

- the regex is matched against each selected field independently; a row is
  written if **any** field matches (before inversion);
- the full row, not the selected projection, is written on a match;
- header written unless `-n`;
- an empty pattern matches every field;
- matching is byte-oriented; UTF-8 mode is on in RE2, matching Rust's default;
- `-i` is equivalent to prefixing the pattern with `(?i)`.

RE2 vs Rust `regex` syntax (documented exclusions; the differential corpus
contains one case per row):

| construct | Rust regex | RE2 | policy |
| --- | --- | --- | --- |
| literals, `.`, `[...]`, `\d\w\s`, anchors | yes | yes | supported |
| `(?i)`, `(?s)`, `(?m)`, `(?U)` | yes | yes | supported |
| `\p{Greek}`, Unicode classes | yes | yes | supported |
| counted repetition `{m,n}` | yes | yes | supported |
| backreferences, lookaround | rejected | rejected | both error |
| `\A`, `\z` | yes | `\A`, `\z` | supported |
| byte-class differences (`\C`) | no | yes | excluded |
| regex error prose | Rust text | RE2 text | stderr compared by class only |

## 9. `sort`

Flags: `-s/--select`, `-N/--numeric`, `-R/--reverse`, `-o`, `-n`, `-d`.

- loads all records, then `sort_by` — a **stable** sort; equal keys keep input
  order;
- lexicographic comparison is on raw selected fields, field by field;
- numeric comparison parses each selected field as `i64`, else `f64`, else the
  value is unparseable; unparseable values sort before parseable ones; `NaN`
  compares equal to everything (`partial_cmp(...).unwrap_or(Equal)`);
- mixed int/float comparison promotes to `f64`;
- `-R` swaps comparator operands (`cmp(b, a)`), it does not reverse the output
  array; stable order of equal rows is therefore preserved;
- `NaN` makes numeric comparison non-transitive (the reference treats it as
  equal to every number). Forward `-N` matches the reference on the corpus;
  `-N -R` with `NaN` can produce a different permutation because Rust's
  driftsort and libstdc++'s stable merge sort resolve an inconsistent
  comparator differently. This is excluded from the gate and documented as a
  limitation; `stats` still consumes `NaN` as a value;
- header is written unless `-n`;
- empty fields and leading `+`/whitespace follow Rust `FromStr` (no whitespace,
  `+` allowed).

## 10. `stats`

Flags: `-s/--select`, `--everything`, `--mode`, `--median`, `--cardinality`,
`--nulls`, `-j/--jobs`, `-o`, `-n`, `-d`.

Columns: `field,type,sum,min,max,min_length,max_length,mean,stddev`, plus
`median`, `mode`, `cardinality` when requested.

Type lattice: `NULL < Integer <= Float <= Unicode`, `Unknown` absorbs
everything. Empty fields are `NULL` and do not degrade the type.

Per column:

- `sum`: integer sum until a float appears, then float sum;
- `min`/`max`: for numbers numeric, otherwise byte-lexicographic lossy UTF-8;
- `min_length`/`max_length`: byte lengths, including nulls;
- `mean`/`stddev`: only for numeric columns; the online algorithm is
  `mean += (x - oldmean)/n`, `variance = (prevq + (x-oldmean)*(x-mean))/n`;
  this is **population** variance (divide by n, not n−1);
- `--nulls` adds a zero sample for every null in a numeric column; without it,
  nulls are skipped;
- `median` stores all numeric samples; `mode` and `cardinality` store all raw
  field values; `--everything` enables all three. The mode path reproduces the
  reference's `stats::mode_on_sorted` run algorithm, including its surprising
  tie behaviour (a frequency-table mode would not match);
- median with `NaN` uses a different sort for the same non-total-order reason as
  `sort -N -R`; it is outside the gate. Non-NaN medians are bit-equal;
- `-j` requires an index in the reference; chunks are computed in worker threads
  and merged in chunk order with the standard parallel-variance formula;
  `-j 1` is serial. The C++ implementation uses the same index chunking when
  `-j > 1` and an index exists; chunks are merged in fixed order. Serial `-j 1`
  is bit-equal to the reference. Parallel float results can differ in the last
  bits from the reference because the reference merges channel arrivals rather
  than a fixed order; the report states this explicitly;
- field names: header bytes, or the 1-based column number with `-n`;
- float formatting follows Rust `Display` (see §12).

## 11. `join`

Flags: `--left`, `--right`, `--full`, `--cross`, `--no-case`, `--nulls`, `-o`,
`-n`, `-d`.

- keys are selected fields, then normalized:
  - valid UTF-8: `str::trim()` on the whole field (Unicode whitespace both
    ends), then if `--no-case`, each character is lowercased and only the
    **first** lowercase character is kept (`c.to_lowercase().next()`); this is
    the reference's simplification, including its quirk for characters whose
    lowercase expands to multiple chars;
  - invalid UTF-8: bytes are used unchanged, and trimming/case folding are
    skipped;
- without `--nulls`, a row with any empty key field is excluded from the value
  index and therefore matches nothing;
- the second input is the build side; its rows are stored as a hash map from key
  to insertion-ordered row ordinals;
- output row = left fields followed by right fields; missing side padded with
  empty fields of that side's header width;
- inner: only matches;
- left: unmatched left row + padding;
- right: implemented by swapping the two inputs, so output is right fields then
  left fields (the reference's behavior);
- full: left pass writes matches and unmatched left; then every unmatched right
  row is written as padding + right row, in right input order;
- cross: for each left row, re-reads the right input from the beginning; output
  order is left-major, right-minor;
- headers: left header concatenated with right header, written unless `-n`;
- column selections must have equal length;
- case folding uses the first character of Rust's lowercase expansion. The C++
  implementation covers ASCII, Latin-1, Greek and Cyrillic; characters whose
  lowercase expands to multiple code points outside that table (for example
  `İ`) are outside the P0 corpus. Invalid UTF-8 keys are used as raw bytes,
  exactly like the reference.

## 12. Number formatting

Rust `Display` for `f64` never uses exponent notation and prints the shortest
round-trip digits:

| value | output |
| --- | --- |
| `0.0` | `0` |
| `-0.0` | `-0` |
| `1.5` | `1.5` |
| `1e20` | `100000000000000000000` |
| `1e-7` | `0.0000001` |
| infinity | `inf` / `-inf` |
| NaN | `NaN` |

C++ `std::to_chars` provides the digits; `format_f64` expands the exponent. A
100k-value randomized corpus against a Rust helper is part of the stats test.

## 13. Differential oracle

| surface | rule |
| --- | --- |
| stdout | byte-identical |
| exit code | identical |
| output file (`-o`) | byte-identical |
| help text (`-h`) | byte-identical |
| stderr | same class: leading phrase before `:` / number; exact text is not required |
| `--version` | excluded from replay (C++ adds variant/upstream text) |
| index file | byte-identical |
| stats numeric cells | exact by default; a `--float-tolerance 1e-12` mode exists for cross-variant comparisons only |
| nondeterministic P1 commands | out of scope for P0 |

Every case is replayed against both binaries in a fresh working directory, with
a timeout, and the report names the first differing byte.

## 14. Known, intentional deviations

1. `--version` prints `xsv-cpp <ver> (xsv 0.13.0 compatible, variant <name>)`.
2. Unknown command prose differs; exit code 1 is the contract.
3. SIGPIPE is ignored and a closed stdout exits 1; the Rust binary panics with
   exit 101. This is measured separately and never mixed into semantic replay.
4. RE2 error prose differs; `search` error cases compare by class.
5. `headers --intersect` matches the reference's actual union-with-dedup
   behavior, not the flag's documentation.
6. The C++ `stats` may use a different parallel merge order across variants, so
   cross-variant numeric equality uses the documented tolerance; against the
   reference the serial default must still be bit-exact.
7. `--list` prints the nine P0 commands, not the reference's twenty; P1/P2
   commands are outside v1.0.
8. Parallel `stats -j` merges fixed chunks; float means/stddev can differ in the
   last bits from the reference's channel-order merge. Serial `-j 1` is exact.
9. Numeric sort with `NaN` under `-N -R` is not byte-stable across sort
   implementations (see §9); it is excluded from the gate.
10. Join case folding outside the documented script ranges is not implemented
    (see §11); ASCII/Latin-1/Greek/Cyrillic keys are covered.
11. `headers` reproduces tabwriter's cell-block widths for tabs and newlines in
    header names; exotic control characters are not part of the gate.
