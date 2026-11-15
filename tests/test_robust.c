/* test_robust.c - a randomized fuzz of the app layer: every widget kind in one spec, random canvas sizes, random key/mouse events,
 * ticks and page switches, thousands of iterations. The only assertion is "it never crashes" (run under ASan/UBSan by `make asan`). */
#include "lazytui.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static lt_caps UNI = {true, true, false, true, 256};

static const char *SPEC =
	"col\n"
	"  row size=1\n"
	"    buttonbar id=tabs group=nav items=\"main,extras\" active=tab size=12\n"
	"    buttonbar id=actions group=nav items=\"a,b,c\" size=fill\n"
	"  pages bind=tab\n"
	"    scroll id=page0\n"
	"      panel title=\"bars\" size=6\n"
	"        progressbar name=p bind=progress size=1\n"
	"        zonebar name=z zones=\"used=zu:err,free=zf:ok\" size=1\n"
	"        pacmanbar name=pb size=1\n"
	"      panel title=\"search\" size=10\n"
	"        searchbar id=q prompt=\"> \" bind=query size=1\n"
	"        list id=l\n"
	"      panel title=\"tree\" size=8\n"
	"        treeview id=t\n"
	"    scroll id=page1\n"
	"      panel title=\"graph\" size=8\n"
	"        custom id=g type=graph\n"
	"      rain\n"
	"  keybar hints=\"tab:focus;f1:x\" size=2\n";

static void draw_graph(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id; (void)focused;
	double *vals = ctx;
	lt_graph(c, r, t, vals, 20, 0, 100, LT_T_OK);
}
static lt_wres event_graph(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{
	(void)a; (void)id; (void)r; (void)ev; (void)ctx;
	return LT_W_IGNORED;
}

/* the filepicker fuzz works on a throwaway tree: directories, files, a link loop, a dangling link, an unreadable directory, a long name */
static void fp_touch(const char *dir, const char *name)
{
	char p[PATH_MAX + 300];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	int fd = open(p, O_CREAT | O_WRONLY, 0600);
	if (fd >= 0) close(fd);
}
static void fp_rm(const char *path)
{
	struct stat st;
	if (lstat(path, &st)) return;
	if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
	chmod(path, 0700);
	DIR *d = opendir(path);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		char p[PATH_MAX + 300];
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		snprintf(p, sizeof p, "%s/%s", path, de->d_name);
		fp_rm(p);
	}
	if (d) closedir(d);
	rmdir(path);
}

static long fuzz_filepicker(unsigned *rngp)
{
	unsigned rng = *rngp;
	const char *tmp = getenv("TMPDIR");
	char root[PATH_MAX], real[PATH_MAX], p[PATH_MAX + 300];
	snprintf(root, sizeof root, "%s/lt_robust_XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(root) || !realpath(root, real)) { printf("robust: FAIL mkdtemp\n"); exit(1); }
	static const char *dn[] = {"a", "B", "sub", "sub/deep", ".hid", "locked", "\x01" "ctl\x1b[31m", "caf\xC3\xA9", "bad\xFF\xFE"};
	for (size_t i = 0; i < sizeof dn / sizeof *dn; i++) { snprintf(p, sizeof p, "%s/%s", real, dn[i]); mkdir(p, 0700); }
	static const char *fn[] = {"x.c", "X.H", "y.txt", ".dot", "no_ext", "sub/deep/f.txt", "locked/s", "wide\xE6\x97\xA5\xE6\x9C\xAC.txt",
	                           "an_extremely_long_file_name_that_will_never_fit_into_a_narrow_column_of_any_picker_at_all.txt"};
	for (size_t i = 0; i < sizeof fn / sizeof *fn; i++) fp_touch(real, fn[i]);
	snprintf(p, sizeof p, "%s/loop", real); if (symlink(".", p)) perror("symlink");
	snprintf(p, sizeof p, "%s/up", real); if (symlink("..", p)) perror("symlink");
	snprintf(p, sizeof p, "%s/self", real); if (symlink("self", p)) perror("symlink");
	snprintf(p, sizeof p, "%s/dangling", real); if (symlink("nowhere", p)) perror("symlink");
	snprintf(p, sizeof p, "%s/locked", real); chmod(p, 0);

	static const int ks[] = {LT_K_UP, LT_K_DOWN, LT_K_LEFT, LT_K_RIGHT, LT_K_ENTER, LT_K_ESC, LT_K_TAB, LT_K_PGUP, LT_K_PGDN, LT_K_HOME, LT_K_END,
	                         LT_K_BSPACE, LT_K_DEL, LT_K_F5, '.', '.', 'x', 'a', 'C', ' ', 0x65E5, LT_MOD_CTRL | 'u', LT_MOD_CTRL | 'w', LT_MOD_ALT | 'b', 0x1b, 0x7f, 0};
	static const int ms[] = {LT_K_CLICK, LT_K_CLICK, LT_K_CLICK, LT_K_DCLICK, LT_K_DRAG, LT_K_RELEASE, LT_K_WHEEL_UP, LT_K_WHEEL_DOWN, LT_K_RCLICK};
	static const char *pastes[] = {"", "x", "a\nb\tc", "\x1b[2Jboom", "\xFF\xFE", "very long pasted text very long pasted text very long pasted text"};
	static const char *exts[] = {NULL, "c,h", ".txt;C", " ", ",,,", "x"};
#define FR(n) (rng = rng * 1664525u + 1013904223u, (int)((rng >> 8) % (unsigned)(n)))
	lt_filepicker *f = lt_filepicker_new(real);
	size_t rl = strlen(real);
	long calls = 0, moves = 0, picks = 0;
	char prev[PATH_MAX];
	snprintf(prev, sizeof prev, "%s", real);
	for (int it = 0; it < 20000; it++) {
		int w = 1 + FR(70), h = 1 + FR(16);
		lt_canvas *fc = lt_canvas_new(w, h, UNI);
		lt_theme th2 = FR(2) ? lt_theme_matrix() : lt_theme_default();
		lt_rect r = {FR(5) - 2, FR(5) - 2, FR(w + 6), FR(h + 6)};
		if (FR(4)) r = (lt_rect){0, 0, w, h};
		lt_filepicker_draw(f, fc, r, &th2, FR(2));
		for (int k = 0; k < 3; k++) {
			lt_event fe = {0};
			char pb[80];
			int kind = FR(10);
			if (kind < 6) { fe.kind = LT_EV_KEY; fe.key = ks[FR((int)(sizeof ks / sizeof *ks))]; }
			else if (kind < 9) { fe.kind = LT_EV_MOUSE; fe.key = ms[FR((int)(sizeof ms / sizeof *ms))]; fe.x = FR(80) - 5; fe.y = FR(20) - 2; }
			else { fe.kind = LT_EV_PASTE; snprintf(pb, sizeof pb, "%s", pastes[FR((int)(sizeof pastes / sizeof *pastes))]); fe.paste = pb; }
			if (lt_filepicker_event(f, r, &fe) == LT_W_ACTIVATE) picks++;
			calls++;
			if (strncmp(lt_filepicker_dir(f), real, rl) != 0) lt_filepicker_set_dir(f, real);      /* never wander outside the tree */
			if (strcmp(prev, lt_filepicker_dir(f)) != 0) { moves++; snprintf(prev, sizeof prev, "%s", lt_filepicker_dir(f)); }
			int n = lt_filepicker_count(f), cur = lt_filepicker_cursor(f);
			if ((n == 0) != (cur < 0) || cur >= n) { printf("robust: FAIL filepicker cursor %d of %d\n", cur, n); exit(1); }
		}
		if (FR(40) == 0) lt_filepicker_set_extensions(f, exts[FR((int)(sizeof exts / sizeof *exts))]);
		if (FR(60) == 0) lt_filepicker_set_hidden(f, FR(2));
		if (FR(80) == 0) lt_filepicker_set_dir(f, real);
		lt_canvas_free(fc);
	}
	lt_filepicker_free(f);
	fp_rm(real);
	if (moves < 100 || picks < 10) { printf("robust: FAIL filepicker fuzz barely moved (%ld directory changes, %ld files chosen)\n", moves, picks); exit(1); }
	*rngp = rng;
	return calls;
}

int main(void)
{
	char err[300];
	lt_app *a = lt_app_new();
	if (!lt_app_load(a, SPEC, err, sizeof err)) { printf("robust: FAIL spec: %s\n", err); return 1; }
	double vals[20];
	for (int i = 0; i < 20; i++) vals[i] = i * 5;
	lt_app_custom(a, "graph", draw_graph, event_graph, vals);

	lt_list *fl = lt_app_list(a, "l");
	lt_tree *ft = lt_app_tree(a, "t");
	for (int i = 0; i < 50; i++) { char b[24]; snprintf(b, sizeof b, "row %d", i); lt_list_add(fl, b, i); lt_tree_add(ft, i > 3 ? i / 4 - 1 : -1, b, i); }
	lt_tree_expand_all(ft, true);
	lt_app_key(a, "ctrl-x", "boom");
	lt_app_set(a, "zu", 30); lt_app_set(a, "zf", 70);
	lt_app_set(a, "done", 10); lt_app_set(a, "total", 100); lt_app_set(a, "rate", 5);

	unsigned rng = 99;
	static const int ks[] = {LT_K_UP, LT_K_DOWN, LT_K_LEFT, LT_K_RIGHT, LT_K_ENTER, LT_K_ESC, LT_K_TAB, LT_K_BTAB, LT_K_PGUP, LT_K_PGDN,
	                          LT_K_HOME, LT_K_END, ' ', 'a', 'x', LT_K_BSPACE, LT_MOD_CTRL | 'x', LT_MOD_ALT | 'b', LT_K_F1, LT_K_F6};
	static const int ms[] = {LT_K_CLICK, LT_K_CLICK, LT_K_DRAG, LT_K_RELEASE, LT_K_WHEEL_UP, LT_K_WHEEL_DOWN, LT_K_RCLICK, LT_K_DCLICK};
#define R(n) (rng = rng * 1664525u + 1013904223u, (int)((rng >> 8) % (unsigned)(n)))
	long widget_calls = 0;
	for (int it = 0; it < 6000; it++) {
		int w = 1 + R(90), h = 1 + R(30);
		lt_canvas *fc = lt_canvas_new(w, h, UNI);
		lt_theme th2 = lt_theme_matrix();
		lt_app_render(a, fc, &th2);
		for (int k = 0; k < 3; k++) {
			lt_event fe = {0};
			if (R(3)) { fe.kind = LT_EV_KEY; fe.key = ks[R(20)]; } else { fe.kind = LT_EV_MOUSE; fe.key = ms[R(8)]; fe.x = R(100) - 5; fe.y = R(34) - 2; }
			lt_app_dispatch(a, &fe, &UNI, 0);
			widget_calls++;
		}
		if (R(50) == 0) lt_app_tick(a, 0.05);
		if (R(40) == 0) lt_app_set(a, "tab", R(2));
		if (R(30) == 0) lt_app_set(a, "progress", (double)R(100) / 100.0);
		lt_canvas_free(fc);
	}
	lt_app_free(a);

	long fp_calls = fuzz_filepicker(&rng);
	printf("robust: ok (%ld widget calls, %ld filepicker calls)\n", widget_calls, fp_calls);
	return 0;
}
