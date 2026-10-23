# FVS-AUDIT-001 evidence (first round), preserved

These are the audit artifacts as they stood at commit `084d98cbda66`, before
FVS-AUDIT-001-R2 and before the provenance binding. They are kept here
unchanged so that the first round of evidence stays readable after the
current artifacts are regenerated; the current files are in the parent
directory and are the ones the gate reads.

The mutation campaign is the exception: `artifacts/audit/mutation-test.txt`
is append-only, so it already contains the first-round runs, in order, next
to the later ones; nothing was copied here.

Why these are not evidence for the current tree: they carry no provenance
stamp, they were produced before the retry path was counted and asserted, and
their published form (`docs/ferret/evidence/`) was extracted from an older
revision. They are history, not a current result.
