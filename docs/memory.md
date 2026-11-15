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

## Arena candidates, by payoff

1. `core/io/app.c` spec loader (about 70 of the library's ~290 call sites). One 12-line spec costs 440 allocations
   (217 malloc, 39 calloc, 184 realloc): every node strdups kind/id/each attribute key and value, `realloc`s its attribute
   and child arrays by one element, and `node_free` walks it all. A per-`lt_app_load` arena would make the tree one
   block freed by one call, and `lt_app_load` could not leak on the error paths. Medium rewrite, not done.
   Handlers/hotkeys/timers (`lt_app_on`, `lt_app_key`, `lt_app_every`) outlive a load and stay on the heap.
2. `widgets/basic/search.c` `mcts()`: seven malloc/free temporaries per query, one lifetime: `lt_scratch()` mark/restore.
3. `core/io/nav.c` breadcrumbs/tabs: one strdup per label; low payoff (a few labels).

## Known gap (not fixed)

`x = realloc(x, n)` without a NULL check appears in `app.c`, `nav.c`, `solver.c`: on out-of-memory it loses the old block
and crashes. The arena would remove most of them.
