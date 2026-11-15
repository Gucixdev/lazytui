/* bench_load.c - allocations and time of lt_app_load: a 12-line spec and a generated 1000-node one, counted with the linker's --wrap.
 * Not part of make test. Build and run:
 *   cc -O2 -std=gnu99 -Iinclude -o /tmp/bench_load tests/bench_load.c liblazytui.a -lm -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -Wl,--wrap=free && /tmp/bench_load */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static long nm, nc, nr, ns, nf;
void *__real_malloc(size_t); void *__real_calloc(size_t, size_t); void *__real_realloc(void *, size_t); char *__real_strdup(const char *); void __real_free(void *);
void *__wrap_malloc(size_t n) { nm++; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { nc++; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { nr++; return __real_realloc(p, n); }
char *__wrap_strdup(const char *s) { ns++; return __real_strdup(s); }
void __wrap_free(void *p) { if (p) nf++; __real_free(p); }
static const char *SPEC =
	"# a small program\ncol\n  buttonbar id=top items=\"main,extras\" active=tab size=1\n  pages bind=tab\n    col\n"
	"      panel title=\"bars\" size=6\n        progressbar name=\"progress\" bind=progress size=1\n"
	"        searchbar id=q prompt=\"> \" bind=query size=1\n"
	"        dropdown id=mode items=\"filter,fuzzy,tag\" bind=mode name=\"mode\" size=1\n      list id=results\n"
	"    col\n      text text=\"the extras page\"\n  textbar bind=status size=1\n";
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void run(const char *name, const char *spec, int reps)
{
	char err[200];
	lt_app *a = lt_app_new();
	nm = nc = nr = ns = nf = 0;
	if (!lt_app_load(a, spec, err, sizeof err)) { printf("load failed: %s\n", err); exit(1); }
	long m = nm, c = nc, r = nr, s = ns, f = nf;
	lt_app_free(a);
	printf("%-12s allocs %ld (malloc %ld calloc %ld realloc %ld strdup %ld), frees %ld\n", name, m + c + r + s, m, c, r, s, nf);
	(void)f;
	double t0 = now();
	for (int i = 0; i < reps; i++) { lt_app *b = lt_app_new(); if (!lt_app_load(b, spec, err, sizeof err)) exit(1); lt_app_free(b); }
	printf("%-12s %d load+free: %.2f ms total, %.1f us each\n", name, reps, (now() - t0) * 1e3, (now() - t0) * 1e6 / reps);
}
int main(void)
{
	char *big = malloc(1 << 20), *p = big;
	p += sprintf(p, "col\n");
	int nodes = 1;
	for (int i = 0; nodes < 1000; i++) {
		p += sprintf(p, "  panel title=\"group %d\" size=30 gap=1\n", i); nodes++;
		for (int j = 0; j < 24 && nodes < 1000; j++, nodes++) p += sprintf(p, "    text text=\"item %d.%d\" bind=k%d size=1 name=\"n%d\"\n", i, j, j, j);
	}
	run("12-line", SPEC, 1000);
	run("1000-node", big, 1000);
	return 0;
}
