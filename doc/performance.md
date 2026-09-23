# srcDiff Performance Notes

## Status

This document records verified implementation structure, measurements, and
candidate experiments. The optimization ideas are not implemented or proven.
The `profiler` branch is based on commit `f48b8180` and now has an opt-in
internal stage profiler.

## Profiling interface

`srcdiff --profile` writes stable aggregate `profile.*` values to standard
error. It is silent when disabled and does not change the XML stream. The
coarse metrics separate original and modified input conversion/materialization,
delta computation, serialization, archive writing, and total runtime. Focused
delta metrics cover structural shortest-edit scripts, edit correction, move
detection, change matching, nesting, and text/syntax shortest-edit scripts.

The profiler also reports scale counters for files, top-level constructs,
change matrices, measurement-cache use, shortest-edit-script inputs, and
nesting checks. `profile.pipeline.total_ms` is inclusive wall time.
`profile.delta.total_ms` subtracts the measured serialization calls from the
inclusive delta scope so that delta and serialization can be compared as major
phases. The focused delta timers are inclusive operation timers: text SES and
nesting can overlap `change_matcher`, and text SES can also overlap nesting.
They must not be added together.

The aggregates are intentionally unsynchronized. This is safe in the current
execution model because the main thread creates and immediately joins each
conversion thread, and all profiler mutations occur on the main thread. Any
future concurrent file differencing or instrumentation inside worker threads
must use per-thread data followed by a merge, or add synchronization. The
fine-grained serialization timer also reads the clock once per emitted node
when profiling is enabled, so profiled wall time includes some measurement
overhead. Disabled runs perform only the profiler-enabled checks at those
sites.

## 2026-09-23 retained-case profile

The measurements used GCC 13.3.0 and a `RelWithDebInfo` build of the dirty
`profiler` worktree based on `f48b8180`. The final binary SHA-256 was
`2adf7f7d684e32a5e3ff3b00d47b1a6365a1916d0b7afeb4f3ad64c3293d708b`.
Each case had one warmup followed by nine measured runs; the table contains
medians. Input is the sum of the four generated Java files.

| Case | Input | Output | External wall | Internal total | Original input | Modified input | Delta | Serialization | Archive write |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| weak target | 10,563 B | 129,453 B | 158 ms | 108.132 ms | 29.762 ms | 5.547 ms | 61.271 ms | 5.708 ms | 0.230 ms |
| size-matched strong control | 13,821 B | 170,687 B | 226 ms | 182.932 ms | 22.024 ms | 23.451 ms | 123.863 ms | 7.571 ms | 0.261 ms |
| small baseline | 1,246 B | 16,660 B | 56 ms | 19.171 ms | 5.739 ms | 4.694 ms | 2.085 ms | 0.910 ms | 0.109 ms |

Delta is the largest measured major phase for both large cases. Conversion is
the largest measured phase only for the small baseline. Direct execution did
not reproduce the earlier benchmark harness result in which the weak case was
roughly twice as slow as the control: in this build the control was slower.
Consequently, similarity alone is not a supported explanation for the earlier
gap.

Inside delta, `change_matcher` took 53.483 ms for the weak case and 114.015 ms
for the control. Text shortest-edit scripts nested inside that work took
49.629 ms and 108.964 ms respectively. Structural SES, edit correction, and
move detection were each below 1 ms. The representative cases therefore point
to text measurement, not the top-level change matrix, as the leading operation.

Each case made four `change_matcher` calls, but every matrix was only 1-by-1:
four cells and 112 allocated bytes in total. The same runs recorded eight text
SES calls, four measurement-cache misses, and four hits, consistent with the
important-text and complete-text measurements reached while evaluating those
cells. The maximum recorded text-sequence product was 17,568 terms for the weak
case, 12,072 for the control, and 1,176 for the baseline. The matrix remains a
credible scaling risk on other inputs, but it did not explain these timings.

The change matrix requires time and space proportional to the product of the
unmatched construct counts, in addition to the cost of measuring each cell.
The text SES implementation is input-length and edit-distance sensitive; its
Myers-style work can grow with both sequence length and edit distance. The
profile shows that this nested SES cost can dominate even when matrix space is
negligible.

Peak RSS was not recorded because GNU `time` was unavailable in the Docker
image; adding monitoring infrastructure was not justified for this focused
run. Profiled and unprofiled XML matched byte-for-byte for all three cases, all
nine outputs per case had one checksum, and the delta suite passed all 564
cases.

The narrow optimization target for a later task is the complete-text SES path
entered by `check_match()` through `construct::is_match_similar()` and
`text_measurer::compute()`. Before changing it, distinguish important-text and
complete-text calls and test a semantics-preserving reuse or rejection fast
path. Do not weaken matching thresholds or replace the exact result with an
approximation.

## Pipeline and cost model

For each local file pair, srcDiff currently:

1. converts each source through srcML;
2. reparses the two srcML buffers into custom node graphs;
3. builds syntactic constructs;
4. performs hierarchical sequence differencing and refinement;
5. serializes a complete srcDiff unit; and
6. writes the unit through the srcML archive API.

`change_matcher::match_differences()` allocates an
`original_count * modified_count` matrix. Each matrix cell may request text and
syntax measurement, refinement rules, and nested matching. This is a likely
time and space hotspot for large unmatched change blocks, but it must be
measured before its rules are changed.

The input manager also creates one thread for the original conversion, joins it
immediately, then repeats that sequence for the modified conversion. The work
is serial, so small files pay two thread-lifecycle costs without concurrency.

The shared converter overwrites its owned XML buffer for the second conversion
and frees only the last buffer in its destructor. This appears to leak the
original-side generated XML buffer for each pair. Confirm with a memory profiler
before fixing ownership because `convert(srcml_unit*)` also stores a borrowed
pointer in the same fields.

## Ranked experiments

1. **Exact-identical file fast path.** Check size and bytes before conversion.
   Parse once and emit the normal common unit so complete-document semantics are
   preserved. Measure the hit rate on real directory/history workloads.
2. **Remove serial thread creation.** Call the two conversions directly and
   compare small-file medians. True parallel conversion requires separate
   converter state and an audit of srcML, namespace, move-ID, and output globals.
3. **Fix converter buffer ownership.** Use explicit owned buffers per
   conversion and validate archive RSS over many file pairs.
4. **Remove duplicate or avoidable measurements.** The retained-case profile
   makes the complete-text SES path the first target. Add only conservative
   reuse or prefilters whose bounds cannot reject a valid result.
5. **Batch serialization writes.** `common_stream` paths can emit text a
   character at a time and allocate a node per character. Batch contiguous runs
   after profiling proves this phase material.
6. **Use caller knowledge.** When a repository client already knows changed
   paths, pass only those paths with `--files-from` rather than scanning and
   processing known-identical files.

The remote `origin/speed-fixes` branch is not a safe optimization source. Its
five-construct lookahead bound changes matching semantics. Its large-input
unigram fallback compares indices from different node arrays, omits unmatched
tails, and comes from code already described as unused or incorrect. Treat
either only as an approximate-mode research experiment, not a default fix.

## Output constraints

Normal srcDiff XML contains enough information to reconstruct both revisions.
An optimization must preserve archive metadata, namespaces, positions,
whitespace changes, nested diff wrappers, replacement grouping, and existing
move annotations.

A regions-only format could serve srcMove more cheaply, but it is a separate
lossy contract. Likewise, byte-range splicing of long common regions is
promising only with XML-aware offsets and namespace handling; it should not be
implemented as a naive text scan.

Validate optimizations on identical, whitespace-only, attribute-only, nesting,
conversion, move, archive, addition/deletion, and malformed-input cases. Any
approximate mode also needs downstream srcMove accuracy comparison.
