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
Text SES metrics retain an aggregate and also separate important-text
measurements from complete-text measurements. The profiler further separates
SES search/frontier work from explicit edit-script reconstruction and counts
the frontier diagonals examined. Additional counters attribute search setup,
comparison calls, allocation calls, and cumulative requested bytes.

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
`text_measurer::compute()`. Do not weaken matching thresholds or replace the
exact result with an approximation.

### Text SES split follow-up

A profiling-only follow-up split the eight text SES calls into the cached
important-text measurement and the complete-text measurement. It used the same
three inputs and one-warmup-plus-nine-measurement procedure. The rebuilt binary
SHA-256 was
`5c1a7346013b3251d977b43ef3a41552c3c878e9799f4297da840a8a473bb73e`.
Absolute times came from a separate run and should not be compared directly to
the earlier table; the comparison within each row is the relevant result.

| Case | Important calls | Important SES | Important terms (original / modified) | Complete calls | Complete SES | Complete terms (original / modified) |
|---|---:|---:|---:|---:|---:|---:|
| weak target | 4 | 55.615 ms | 1,562 / 216 | 4 | 60.162 ms | 2,948 / 366 |
| size-matched strong control | 4 | 56.647 ms | 1,136 / 1,152 | 4 | 185.822 ms | 2,006 / 2,032 |
| small baseline | 4 | 0.218 ms | 120 / 116 | 4 | 1.079 ms | 216 / 212 |

Complete-text SES accounts for about 77% of text SES time in the control and
83% in the baseline. The weak target is roughly even: about 48% important-text
and 52% complete-text. Complete-text inputs are larger in all three cases, but
input length alone does not explain the control's much larger complete-text
time because SES cost also depends on edit structure. The next profiling
question is which of the four complete-text calls dominates and what its two
sequence lengths and edit distance are. That per-call attribution should be
measured before selecting a semantics-preserving fast path.

### Slowest complete-text call follow-up

A second profiling-only follow-up retained only the slowest complete-text call
per process, recording its stable 1-based call index, duration, two sequence
lengths, and exact insert/delete distance. It again used one warmup and nine
measured runs. The rebuilt binary SHA-256 was
`ddc66532c2ca117d07cddaaa9c5ad0cbaf371b1c2147cd577da943c1816220f9`.

| Case | Complete SES total | Slowest call | Share | Repeated winner | Winner terms (original / modified) | Edit distance |
|---|---:|---:|---:|---|---:|---:|
| weak target | 24.439 ms | 11.907 ms | 49% | calls 3 and 4 alternated | 1,464 / 12 or 1,462 / 10 | 1,452 |
| size-matched strong control | 77.891 ms | 20.008 ms | 26% | call 2 won 6 of 9 | 10 / 1,004 | 994 |
| small baseline | 0.504 ms | 0.146 ms | 29% | call 3 won 7 of 9 | 98 / 12 | 86 |

No single complete-text comparison explains the cost. In the control, even the
largest call is only about one quarter of complete-text time, so the cost is
distributed across all four calls. In the weak target, two nearly equal calls
alternate as the maximum and together account for most complete-text time. The
repeatedly slow comparisons are highly unbalanced and have edit distances close
to the longer sequence length. The next profiling question is therefore not
which file pair is anomalous, but which SES algorithm path handles these
highly unbalanced, high-distance sequences and how its work scales with
sequence length and edit distance.

### SES search and reconstruction follow-up

A third profiling-only follow-up split each text SES computation into search
and explicit reconstruction. Reconstruction includes compacting the C edit
chain, converting it to the C++ edit list, and freeing the C chain. Search is
the remaining SES computation time, including frontier traversal, temporary
allocation and cleanup, and edit-node construction performed during hybrid
recursion. `frontier_steps` counts diagonal expansions rather than element
comparisons or matrix cells. The existing `text_ses_*_ms` timers also include
the later `process_edit_script()` call, so they are intentionally slightly
larger than search plus reconstruction. Reconstruction totals include the
fine-grained clock-read overhead and are therefore conservative at this scale.

The same three inputs were measured after one warmup followed by nine runs.
The rebuilt binary SHA-256 was
`e59623a0efbd11fa28567962e7f2a67d1e74b963573b4b19729f88b28467dd93`.
The table contains medians.

| Case | Important search | Important reconstruction | Important frontier steps | Complete search | Complete reconstruction | Complete frontier steps |
|---|---:|---:|---:|---:|---:|---:|
| weak target | 22.142 ms | 0.249 ms | 596,076 | 24.089 ms | 0.116 ms | 1,616,030 |
| size-matched strong control | 23.343 ms | 0.288 ms | 620,520 | 74.032 ms | 0.773 ms | 1,954,334 |
| small baseline | 0.096 ms | 0.004 ms | 3,874 | 0.447 ms | 0.010 ms | 14,794 |

Search dominates both text paths in every case; explicit reconstruction is at
most about 5% of their combined measured SES time and is about 1% or less for
the two large complete-text cases. The control performs about 21% more complete-text
frontier steps than the weak case but takes about three times as long in that
phase, so the diagonal count alone does not explain runtime. The next narrow
question is which part of frontier processing accounts for that per-step gap:
element comparison and diagonal sliding, or frontier allocation and update
overhead. That distinction should be measured before changing the algorithm.

### Frontier-detail follow-up

A fourth profiling-only follow-up measured search-storage allocation and
initialization directly, counted text comparisons and allocation requests, and
treated the remaining search time as traversal. Traversal includes frontier
updates, diagonal sliding, cleanup, and edit-node construction during hybrid
recursion. The allocation byte count is cumulative requested memory, not peak
resident memory. This split avoids clock reads inside the frontier loops; the
comparison callback adds only a counter increment when profiling is enabled.

The same one-warmup-plus-nine-run procedure produced the following medians.
The rebuilt binary SHA-256 was
`a8a184a395791ee980a692d86fce110f4fd64a85f316ecfe4c6e386f03da5ce4`.

| Case | Complete search | Setup | Traversal | Frontier steps | Comparisons | Allocations | Requested bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| weak target | 27.096 ms | 8.343 ms | 18.745 ms | 1,616,030 | 1,084 | 914 | 14,422,160 |
| size-matched strong control | 76.182 ms | 52.342 ms | 23.940 ms | 1,954,334 | 740 | 1,986 | 94,140,000 |
| small baseline | 0.491 ms | 0.210 ms | 0.257 ms | 14,794 | 272 | 180 | 734,880 |

Text comparison does not explain the control's extra complete-text cost: the
control made fewer comparisons than the weak case. Traversal was about 1.3
times slower, broadly consistent with its 1.2 times as many frontier steps.
Setup was over seven times slower, accompanied by about twice as many
allocation calls and 6.5 times as many cumulatively requested bytes. Repeated
allocation and zero-initialization in the quadratic-space leaves is therefore
the best-supported explanation for most of the remaining gap.

The next implementation question is whether the quadratic-space SES can reuse
or more narrowly allocate its temporary frontier and edit storage while
returning the identical shortest edit script. That should be designed and
tested as a separate optimization change, not folded into the profiler.

### Medium archive follow-up

Two real srcMove changes were profiled to check whether the retained-case
result extends to medium multi-file inputs. Commit `fcf1219f` is a mostly
additive change (five C++ files, 525 insertions and 43 deletions); its parent is
`e7863974`. Commit `e7863974` is a refactor (seven C++ files, 250 insertions and
221 deletions); its parent is `b99da1a0`. Each comparison was run both over the
complete 38-file `src/` tree and over only the changed files. The complete trees
contained about 150--166 KB of source. The same profiler binary as the
frontier-detail study was used.

Each workload had one warmup followed by nine measured runs. The table contains
medians. Profiled output was byte-identical across all nine runs and matched an
unprofiled run; unprofiled standard error was empty. The two full archives were
2,154,483 and 2,077,985 bytes respectively.

| Change and input | Total | Input conversion | Delta | All text SES | Complete search | Archive write |
|---|---:|---:|---:|---:|---:|---:|
| `fcf1219f`, 38 files | 1,847.140 ms | 950.334 ms | 579.785 ms | 389.705 ms | 247.568 ms | 203.973 ms |
| `fcf1219f`, 5 changed files | 740.607 ms | 215.841 ms | 457.610 ms | 388.413 ms | 246.975 ms | 38.559 ms |
| `e7863974`, 38 files | 2,041.060 ms | 895.210 ms | 817.582 ms | 572.396 ms | 387.890 ms | 208.498 ms |
| `e7863974`, 7 changed files | 1,136.340 ms | 296.748 ms | 726.817 ms | 575.625 ms | 390.220 ms | 58.275 ms |

The unchanged files add conversion, structural differencing, serialization, and
archive-writing cost, but do not materially change the text SES totals. Text SES
accounts for 67--70% of delta time in the full archives and 79--85% when only
changed files are supplied. It is therefore the leading measured delta
operation on both real changes, but it is only about 21--28% of complete
pipeline time for the full archives. The broader claim that text SES dominates
all srcDiff execution does not hold for these archive runs.

The complete-search split also varies materially with edit shape:

| Change | Complete calls | Search | Setup | Traversal | Frontier steps | Comparisons | Allocations | Requested bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `fcf1219f` | 58 | 247.568 ms | 12.627 ms | 234.881 ms | 5,807,930 | 1,788,098 | 1,325 | 28,578,992 |
| `e7863974` | 270 | 387.890 ms | 89.255 ms | 298.404 ms | 5,668,740 | 2,442,106 | 7,267 | 201,343,000 |

Setup is only 5% of complete search for the mostly additive change, where
traversal dominates. It is 23% for the refactor and requests about 201 MB
cumulatively. The synthetic control's setup-heavy result is therefore real but
not universal; both setup and traversal remain plausible optimization targets,
depending on workload.

### srcML conversion share

A narrow input timer around `converter.convert()` separates the libsrcml
conversion from srcDiff's later XML-reader and node-graph materialization. It
includes creation of the srcML unit, parsing through the input callbacks,
serialization of the unit into the in-memory srcML buffer, and associated
libsrcml archive setup and teardown. It excludes opening the input stream,
reparsing that buffer with libxml2, building srcDiff nodes, and thread startup.
The rebuilt binary SHA-256 was
`62a7f2b06473e7c304eee545b08917b45d45b94be5b95e8b2feb8a04e9d65535`.

One warmup and nine measured runs produced these medians:

| Change and input | Total | Complete input phase | libsrcml conversion | libsrcml / total | libsrcml / input |
|---|---:|---:|---:|---:|---:|
| `fcf1219f`, 38 files | 2,117.858 ms | 1,196.607 ms | 849.524 ms | 40.1% | 71.0% |
| `fcf1219f`, 5 changed files | 783.563 ms | 224.637 ms | 152.723 ms | 19.5% | 68.0% |
| `fcf1219f`, `region_filter.cpp` only | 594.356 ms | 143.212 ms | 88.906 ms | 15.0% | 62.1% |
| `e7863974`, 38 files | 2,113.376 ms | 944.430 ms | 640.171 ms | 30.3% | 67.8% |
| `e7863974`, 7 changed files | 1,162.523 ms | 308.586 ms | 211.712 ms | 18.2% | 68.6% |

On these runs, libsrcml accounts for about 30--40% of complete archive time
and 18--20% of changed-files-only time. It consistently accounts for about
68--71% of srcDiff's input phase; XML reparsing, node construction, file open,
and thread overhead make up the remainder. Eliminating all libsrcml cost would
therefore have a theoretical ceiling of about 1.2--1.7 times on these
workloads, while eliminating duplicate conversion for unchanged pairs targets
the full-archive cases specifically.

For an intentionally imperfect CLI comparison, unprofiled srcDiff and
standalone srcML were each run after one warmup for nine trials over the same
two source trees. Both wrote their XML archives to `/dev/null`, and srcML used
one worker to match srcDiff's sequential conversion behavior.

| Change and input | srcDiff wall | srcML wall | srcDiff / srcML |
|---|---:|---:|---:|
| `fcf1219f`, 38 files | 2,073.644 ms | 688.866 ms | 3.01 times |
| `fcf1219f`, 5 changed files | 896.145 ms | 205.000 ms | 4.37 times |
| `fcf1219f`, `region_filter.cpp` only | 655.423 ms | 145.944 ms | 4.49 times |
| `e7863974`, 38 files | 2,414.787 ms | 623.807 ms | 3.87 times |
| `e7863974`, 7 changed files | 1,208.868 ms | 237.238 ms | 5.10 times |

This comparison is descriptive, not an efficiency verdict: srcML only creates
the source representation, whereas srcDiff converts both sides, builds its own
graphs, computes a semantic diff, and emits a substantially different archive.
The CLI timing also includes process and archive overhead and showed wider
run-to-run variation than the internal phase measurements.

The allocation mechanism gives a strong explanation for the synthetic
control's 94 MB result. The quadratic routine allocates three zero-initialized
arrays sized from the two sequence lengths, then allocates blocks of
`edit_t` records while expanding the frontier. The linear middle-snake routine
instead allocates two point arrays per recursive call and does not retain an
edit record for every explored state. The hybrid threshold is 1,000. At entry
it is applied to the combined unmatched-tail length after removing a common
prefix; during hybrid recursion it is applied to the parent edit distance when
choosing how to solve the two children.

### SES allocation-path follow-up

A final attribution split counted linear middle-snake storage, quadratic base
storage, and quadratic edit blocks separately. It adds counters only at
allocation sites, with no additional clock reads or hot-loop work. The rebuilt
binary SHA-256 was
`d5e7c7d92e9ec5913a0ca7cb6905355a5573d47d2d813d12ee006f8c74b6a6c6`.
The counts and byte totals are deterministic, so one run of each previously
measured workload was sufficient for this attribution check.

| Complete-text workload | Linear calls / bytes | Quadratic calls / base bytes | Edit blocks / bytes | Total allocations / bytes |
|---|---:|---:|---:|---:|
| weak target | 4 / 279,488 | 6 / 85,968 | 888 / 14,056,704 | 914 / 14,422,160 |
| size-matched control | 0 / 0 | 4 / 190,272 | 1,974 / 93,949,728 | 1,986 / 94,140,000 |
| small baseline | 0 / 0 | 4 / 16,992 | 168 / 717,888 | 180 / 734,880 |
| `fcf1219f` changed files | 6 / 1,035,680 | 60 / 599,136 | 1,133 / 26,944,176 | 1,325 / 28,578,992 |
| `e7863974` changed files | 13 / 2,190,512 | 281 / 2,966,328 | 6,398 / 196,185,792 | 7,267 / 201,342,632 |

Quadratic edit blocks account for 99.8% of the control's requested bytes and
97.5% of the weak case's. The control runs all four complete-text comparisons
directly through the quadratic routine. The weak case first makes four linear
middle-snake calls, splits the large unmatched tails, and reaches six smaller
quadratic leaves. The control therefore allocates both more edit blocks and
larger blocks. This directly confirms that reusable linear frontier arrays or
the three quadratic base arrays cannot materially reduce the reported 94 MB.

The medium refactor shows the same mechanism at a larger aggregate scale:
quadratic edit blocks account for 196.2 MB of its 201.3 MB. Buffer reuse could
reduce allocator calls, and replacing zero-initialized edit blocks with fully
initialized-on-write storage might reduce setup time, but neither changes the
amount of retained quadratic search state. A deeper change to quadratic path
reconstruction or to the hybrid selection policy is required to materially
reduce cumulative storage. Either can change shortest-script tie-breaking, so
the next optimization must first define and test exact XML equivalence across
the complete suite and these retained workloads.

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
