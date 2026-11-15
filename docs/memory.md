# Memory

Where lazytui allocates, and why. Audited 2026-11-15 (`make test`: ASan + UBSan + LeakSanitizer on every test, clean).

## Already arena or static

- `core/io/arena.c`: chunked bump arena, `lt_scratch()` mark/restore for per-draw temporaries (piechart, histogram, piebar,
  ringbar, webchart, pixeleditor). The filepicker keeps all entry names in one arena that `load()` resets.
- Steady state: rendering the `tests/test_app.c` spec 1 and 1001 times gives the same malloc/calloc/realloc/free
  counts (217/39/184/364), so a frame allocates nothing (LD_PRELOAD counter).

## Justified heap

- Widget objects (`lt_tabs`, `lt_menu`, `lt_solver`, ...): caller-owned handles with `_new`/`_free`, lifetime set by the caller.
- Growable arrays (`lt_filepicker.e`, solver rows, canvas cells, hit list): doubling `realloc`, one owner.

## Spec loader on one arena (done 2026-11-15)

`lt_app_load` builds the whole spec tree in one `lt_arena` (`lt_app.ar`, 16 KB chunks): nodes, kind/id/attribute strings, the
attribute and child arrays (double from 4 instead of `realloc` by one), and the item/zone/hint tables `instantiate` makes. One call
(`lt_arena_free`) releases it on every error path and on reload/`lt_app_free`. A failed load leaves the app's previous tree untouched.
`node_destroy` only runs what has its own lifetime (kind `destroy`, widget objects, a scroll's layout, `last_text`).
Every arena allocation is checked: a NULL becomes the error "out of memory" and `lt_app_load` returns false. Public API unchanged.
Measured with `tests/bench_load.c` (counting `--wrap` allocator, 1000 load+free loops, -O2):

| | before | after |
|---|---|---|
| 12-line spec, allocations | 201 (79 malloc, 33 calloc, 64 realloc, 25 strdup) | 53 (4, 22, 10, 17) |
| 12-line spec, time per load+free | 8.6 us | 5.0 us |
| 1000-node spec, allocations | 22989 (9912, 2001, 9076, 2000) | 2205 (38, 1002, 165, 1000) |
| 1000-node spec, time per load+free | 937 us | 527 us |

What is left in those numbers is not the parser: the layout nodes (`layout.c`: one calloc + one name strdup + kid array per node),
the widget objects, and the focus list. Failure injected at the arena's chunk allocation (60 points over two specs, ASan+LSan) returns
"out of memory" with no leak; `lt_layout_new`/`lt_node_add` are still unchecked, so a failure there still crashes (UNSURE how likely
that matters: layout nodes are small and the chunk allocation fails first on a big spec, but it is not guaranteed).
Handlers/hotkeys/timers (`lt_app_on`, `lt_app_key`, `lt_app_every`) outlive a load and stay on the heap.

## Arena candidates, by payoff

1. `widgets/basic/search.c` `mcts()`: seven malloc/free temporaries per query, one lifetime: `lt_scratch()` mark/restore.
2. `core/io/layout.c`: per-node calloc + name strdup + doubling kid array; the largest share of what a load still allocates. A layout
   arena would make a layout one block, but `lt_node` pointers are handed out by `lt_node_add`, so check the users first.
3. `core/io/nav.c` breadcrumbs/tabs: one strdup per label; low payoff (a few labels).

## Known gap (not fixed)

`x = realloc(x, n)` without a NULL check remains in `nav.c`, `solver.c`, and in `app.c` outside the loader (`get_m`, `lt_app_on`/`key`/
`every`/`custom`, `collect`'s focus list, the hit list): on out-of-memory it loses the old block and crashes.
