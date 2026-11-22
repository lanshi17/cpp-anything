# Evaluation artifacts

## Correctness (`evaluation/correctness/`)

- `cases.jsonl` — one JSON object per line. Fields:
  - `id`, `command`, `tags`;
  - `argv` — command plus arguments; `{fixtures}`, `{data}`, `{work}`, `{tmp}`
    are substituted by the runner;
  - `stdin_file` or `stdin_text`;
  - `copy` — files copied into the case work directory;
  - `prepare` — `index`, `index_stale`, `index_corrupt`;
  - `compare_stdout` (default true), `output_file`;
  - `stderr_contains` (both binaries must contain the text) or
    `stderr_class` (default true);
  - `env`, `timeout`.
- `fixtures/` — handcrafted CSV inputs shared by both binaries.

Run:

```sh
python3 scripts/run_correctness.py \
  --cpp-bin build/all/xsv \
  --rust-bin upstream/xsv/target/release/xsv \
  --filter count --verbose
```

`--gate p0` additionally requires at least one case for every P0 command. The
current corpus has 144 cases and the gate is green on `base` and `all`.

The oracle is defined in `docs/COMPATIBILITY.md` §11.

## Performance (`evaluation/performance/`)

- `matrix.json` — benchmark cases. `status` is `implemented`, `planned`, or
  `deferred-p1`. Planned/deferred cases are skipped unless
  `--include-planned` is passed.
- Datasets are referenced by name; sizes come from `datasets/manifests/`.

Run:

```sh
python3 scripts/run_benchmarks.py \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant base=build/base/xsv \
  --cpp-variant all=build/all/xsv \
  --profile smoke --memory
python3 scripts/generate_report.py
```

The runner writes append-only JSONL with an environment fingerprint. A different
CPU, binary hash or dataset index refuses to append: results from different
environments are never silently mixed.

## Adding a case

1. Add the fixture (correctness) or dataset (performance).
2. Add one JSON line / matrix entry with a stable `id`.
3. Run the case against both binaries; a new case is not accepted until the
   reference and the candidate agree.
4. If the case reveals a reference bug, encode the bug; do not “fix” it in the
   C++ side. See `docs/DECISIONS.md` D6.
