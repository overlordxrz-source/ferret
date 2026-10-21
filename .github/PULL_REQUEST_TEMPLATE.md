<!--
  Ferret PR template. Delete sections that genuinely do not apply, but do not
  mark a test as passing if you did not run it. Use NOT RUN instead.
-->

## What changed

<!-- One paragraph. Link the issue: "Part of #12" / "Fixes #12". -->

## What C implementation was replaced

| Item | Value |
| --- | --- |
| C source | `lib/...` |
| Rust destination | `lib/..._rust.rs` |
| Kconfig switch | `CONFIG_..._RUST` |
| Exported symbols | `...` |
| Export type preserved | `EXPORT_SYMBOL` / `EXPORT_SYMBOL_GPL` |

## Is the C object still compiled?

<!--
  State plainly. "No: lib/foo.o is absent from the Rust build tree and
  lib/foo_rust.o defines the symbols" is the expected answer. A wrapper that
  still calls the C implementation is not a migration; say so if that is what
  this PR does.
-->

## Tests performed

<!-- Paste commands and the relevant output lines, not just "tests pass". -->

- [ ] `tools/ferret/build.sh rust`
- [ ] `tools/ferret/boot.sh rust` (KUnit + userspace markers)
- [ ] `tools/ferret/linkage.sh rust` (C object absent, one definition, size match, ksymtab, export type)
- [ ] differential KUnit suite
- [ ] upstream KUnit suite for the component
- [ ] arm64 build/boot (if the component is arch-independent)
- [ ] Ubuntu shell and systemd boots (if the change can affect boot)

## Build / boot status

| Platform | Build | Boot | Notes |
| --- | --- | --- | --- |
| x86_64 | PASS / FAIL / NOT RUN | | |
| arm64 | PASS / FAIL / NOT RUN | | |

## Performance evidence (if relevant)

<!-- QEMU TCG numbers must be labelled as such. Do not present them as native. -->

## Safety invariants and unsafe justification

<!--
  List every unsafe block and the invariant that makes it sound, or state
  "no new unsafe". Explain allocation failure behaviour and panic-freedom.
-->

## Documentation / registry

- [ ] `registry/migrations.json` updated
- [ ] `python3 tools/ferret/registry.py --markdown` exits 0
- [ ] architecture / status docs updated if the public claims changed

## Checklist

- [ ] Commits are signed off (`git commit -s`)
- [ ] No unrelated reformatting or destructive history changes
- [ ] The C reference implementation is kept for the differential tests
- [ ] No fabricated results: anything not executed is marked NOT RUN
