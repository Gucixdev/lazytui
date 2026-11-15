/* test_filepicker.c - lt_filepicker on real directories: a temp tree with directories, files, symlink loops, a dangling link, an unreadable
 * directory, a very long name; ordering, navigation by key and mouse, filters, hidden files, selection, errors, and a directory of 100000 files.
 * Everything happens under one mkdtemp directory that is removed at the end. Headless: draw into a canvas and read it back as text. */
#include "lazytui.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_caps ASC = {false, false, false, true, 16};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

static char root[PATH_MAX], real[PATH_MAX];
static char picked[PATH_MAX];
static int picked_n;

static void on_pick(const char *path, void *user) { snprintf(picked, sizeof picked, "%s", path); picked_n += *(int *)user; }

static void touch(const char *dir, const char *name)
{
	char p[PATH_MAX + 300];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	int fd = open(p, O_CREAT | O_WRONLY, 0600);
	if (fd >= 0) close(fd);
}
static void mk(const char *dir, const char *name)
{
	char p[PATH_MAX + 300];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	mkdir(p, 0700);
}
static void lnk(const char *dir, const char *name, const char *to)
{
	char p[PATH_MAX + 300];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	if (symlink(to, p)) perror("symlink");
}

static void rm_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st)) return;
	if (S_ISDIR(st.st_mode)) {
		chmod(path, 0700);
		DIR *d = opendir(path);
		struct dirent *de;
		while (d && (de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
			char p[PATH_MAX + 300];
			snprintf(p, sizeof p, "%s/%s", path, de->d_name);
			rm_tree(p);
		}
		if (d) closedir(d);
		rmdir(path);
	} else unlink(path);
}

/* the shown names, joined with '|' (dirs get a trailing '/') */
static void names(const lt_filepicker *f, char *out, size_t n)
{
	out[0] = 0;
	for (int i = 0; i < lt_filepicker_count(f); i++) {
		size_t l = strlen(out);
		snprintf(out + l, n - l, "%s%s%s", i ? "|" : "", lt_filepicker_name(f, i), lt_filepicker_is_dir(f, i) && strcmp(lt_filepicker_name(f, i), "..") ? "/" : "");
	}
}
static int row_of(const lt_filepicker *f, const char *name)
{
	for (int i = 0; i < lt_filepicker_count(f); i++) if (!strcmp(lt_filepicker_name(f, i), name)) return i;
	return -1;
}
static const char *cur(const lt_filepicker *f) { return lt_filepicker_name(f, lt_filepicker_cursor(f)); }
static char *dump(lt_filepicker *f, lt_canvas *c, lt_rect r, const lt_theme *th)
{
	lt_canvas_clear(c, th->s[LT_T_TEXT]);
	lt_filepicker_draw(f, c, r, th, true);
	return lt_canvas_dump(c);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(root, sizeof root, "%s/lt_filepicker_XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
	if (!realpath(root, real)) { perror("realpath"); return 1; }

	char longname[160];
	snprintf(longname, sizeof longname, "very_long_%.90s.txt", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
	const char *dirs[] = {"zeta", "Alpha", "beta", "empty", "sub", "locked", ".hidden"};
	for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) mk(root, dirs[i]);
	const char *files[] = {"b.txt", "A.C", "a.c", "c.h", "README", "spaced name", ".dot", longname};
	for (size_t i = 0; i < sizeof files / sizeof *files; i++) touch(root, files[i]);
	lnk(root, "loop", ".");                        /* a directory link that points at its own parent */
	lnk(root, "loop2", "loop2");                   /* a link to itself: ELOOP, so a plain entry */
	lnk(root, "dangling", "does-not-exist");
	char sub[PATH_MAX + 8], inner[PATH_MAX + 16], locked[PATH_MAX + 16];
	snprintf(sub, sizeof sub, "%s/sub", root);
	mk(sub, "inner");
	snprintf(inner, sizeof inner, "%s/inner", sub);
	touch(inner, "deep.txt");
	snprintf(locked, sizeof locked, "%s/locked", root);
	touch(locked, "secret");
	chmod(locked, 0);

	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(40, 12, UNI);
	lt_rect r = {0, 0, 40, 12};                    /* path row, 10 entry rows (y 1..10), filter row */
	char buf[PATH_MAX + 4096];
	lt_event e;

	/* ---- ordering: directories first, case-insensitive, ".." leads, dot files hidden ---- */
	lt_filepicker *f = lt_filepicker_new(root);
	CHECK(f && !strcmp(lt_filepicker_dir(f), real), "the directory is kept as its canonical path");
	CHECK(lt_filepicker_error(f) == NULL, "a readable directory has no error");
	names(f, buf, sizeof buf);
	char want[PATH_MAX + 4096];
	snprintf(want, sizeof want, "..|Alpha/|beta/|empty/|locked/|loop/|sub/|zeta/|A.C|a.c|b.txt|c.h|dangling|loop2|README|spaced name|%s", longname);
	CHECK(!strcmp(buf, want), "directories first, then files, each sorted ignoring case; links to directories are directories, loops and dangling links plain entries");
	if (strcmp(buf, want)) printf("  got: %s\n", buf);
	CHECK(lt_filepicker_total(f) == 18 && lt_filepicker_count(f) == 17, "total counts every entry except .., count the shown rows with ..");
	CHECK(lt_filepicker_cursor(f) == 0 && !strcmp(cur(f), ".."), "the cursor starts on the first row");

	/* ---- keys: arrows, PgUp/PgDn, Home/End ---- */
	e = key(LT_K_DOWN);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && !strcmp(cur(f), "Alpha"), "Down moves to the next row");
	e = key(LT_K_UP); lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_HANDLED && lt_filepicker_cursor(f) == 0, "Up at the top stays and is handled");
	e = key(LT_K_END);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && lt_filepicker_cursor(f) == 16, "End goes to the last row");
	e = key(LT_K_PGUP); lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_cursor(f) == 16 - 9, "PgUp moves a page minus one (10 rows -> 9)");
	e = key(LT_K_HOME); lt_filepicker_event(f, r, &e);
	e = key(LT_K_PGDN); lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_cursor(f) == 9, "PgDn moves a page minus one");
	char *d = dump(f, c, r, &th);
	CHECK(strstr(d, "locked") != NULL && strstr(d, "> ") != NULL, "the draw shows the entries and the filter row");
	free(d);

	/* ---- Enter on a directory, Backspace / Left up, the cursor returns to where we came from ---- */
	lt_filepicker_set_cursor(f, row_of(f, "sub"));
	e = key(LT_K_ENTER);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED, "Enter on a directory descends");
	char want_sub[PATH_MAX + 8];
	snprintf(want_sub, sizeof want_sub, "%s/sub", real);
	CHECK(!strcmp(lt_filepicker_dir(f), want_sub), "the directory is now sub");
	names(f, buf, sizeof buf);
	CHECK(!strcmp(buf, "..|inner/"), "sub holds inner");
	e = key(LT_K_BSPACE);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && !strcmp(lt_filepicker_dir(f), real), "Backspace on an empty filter goes up");
	CHECK(!strcmp(cur(f), "sub"), "and the cursor is on the directory we left");
	e = key(LT_K_RIGHT); lt_filepicker_event(f, r, &e);
	CHECK(!strcmp(lt_filepicker_dir(f), want_sub), "Right also opens a directory");
	e = key(LT_K_DOWN); lt_filepicker_event(f, r, &e);
	e = key(LT_K_ENTER); lt_filepicker_event(f, r, &e);
	e = key(LT_K_DOWN); lt_filepicker_event(f, r, &e);
	e = key(LT_K_ENTER);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_ACTIVATE && lt_filepicker_selected(f) != NULL, "Enter on a file chooses it (inner/deep.txt)");
	char want_deep[PATH_MAX + 32];
	snprintf(want_deep, sizeof want_deep, "%s/inner/deep.txt", want_sub);
	CHECK(lt_filepicker_selected(f) && !strcmp(lt_filepicker_selected(f), want_deep), "selected is the absolute path");
	e = key(LT_K_LEFT); lt_filepicker_event(f, r, &e);
	e = key(LT_K_LEFT);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && !strcmp(lt_filepicker_dir(f), real), "Left goes up, twice back to the root of the tree");
	lt_filepicker_free(f);

	/* ---- callback, and a file picked from the tree root ---- */
	int one = 1;
	f = lt_filepicker_new(root);
	lt_filepicker_on_select(f, on_pick, &one);
	lt_filepicker_set_cursor(f, row_of(f, "b.txt"));
	e = key(LT_K_ENTER);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_ACTIVATE && picked_n == 1, "the callback runs once per chosen file");
	char want_b[PATH_MAX + 16];
	snprintf(want_b, sizeof want_b, "%s/b.txt", real);
	CHECK(!strcmp(picked, want_b) && !strcmp(lt_filepicker_selected(f), want_b), "callback and selected carry the same absolute path");
	lt_filepicker_set_cursor(f, row_of(f, "Alpha"));
	e = key(LT_K_ENTER); lt_filepicker_event(f, r, &e);
	CHECK(picked_n == 1, "descending into a directory does not call the callback");
	e = key(LT_K_LEFT); lt_filepicker_event(f, r, &e);

	/* ---- hidden files: '.' on an empty filter ---- */
	CHECK(row_of(f, ".hidden") < 0 && row_of(f, ".dot") < 0, "dot files start hidden");
	e = key('.');
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && lt_filepicker_hidden(f), "'.' shows dot files");
	CHECK(row_of(f, ".hidden") >= 0 && row_of(f, ".dot") >= 0 && lt_filepicker_count(f) == 19, "both the hidden directory and the hidden file appear");
	CHECK(lt_filepicker_is_dir(f, row_of(f, ".hidden")) && row_of(f, ".hidden") < row_of(f, "Alpha") + 1 + 7, "a hidden directory still sorts among the directories");
	e = key('.'); lt_filepicker_event(f, r, &e);
	CHECK(!lt_filepicker_hidden(f) && row_of(f, ".dot") < 0, "'.' again hides them");

	/* ---- type to filter ---- */
	e = key('x'); lt_filepicker_event(f, r, &e);
	e = key('X');
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED, "typing edits the filter");
	CHECK(!strcmp(lt_filepicker_query(f), "xX"), "the filter text is what was typed");
	names(f, buf, sizeof buf);
	snprintf(want, sizeof want, "..|%s", longname);
	CHECK(!strcmp(buf, want), "filter is a case-insensitive substring; the .. row stays, directories are filtered too");
	CHECK(!strcmp(cur(f), longname), "the cursor jumps to the first real match");
	e = key('.'); lt_filepicker_event(f, r, &e);
	CHECK(!lt_filepicker_hidden(f) && !strcmp(lt_filepicker_query(f), "xX."), "'.' with a non-empty filter is a filter character, not the hidden toggle");
	e = key(LT_K_BSPACE); lt_filepicker_event(f, r, &e);
	e = key(LT_K_BSPACE); lt_filepicker_event(f, r, &e);
	CHECK(!strcmp(lt_filepicker_query(f), "x") && !strcmp(lt_filepicker_dir(f), real), "Backspace with a filter erases a character instead of going up");
	e = key(LT_K_ESC);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && lt_filepicker_query(f)[0] == 0 && lt_filepicker_count(f) == 17, "Esc clears the filter");
	e = key(LT_K_ESC);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_IGNORED, "Esc with nothing to clear is left to the parent");
	lt_filepicker_set_query(f, "zzz-nothing");
	CHECK(lt_filepicker_count(f) == 1 && lt_filepicker_cursor(f) == 0, "no match leaves only ..");
	d = dump(f, c, r, &th);
	CHECK(strstr(d, "0/18") != NULL, "the filter row counts matches");
	free(d);
	e = key(LT_K_ENTER);
	lt_filepicker_event(f, r, &e);
	CHECK(strcmp(lt_filepicker_dir(f), real) != 0, "Enter on .. goes up even with a filter");
	lt_filepicker_set_dir(f, root);
	e = (lt_event){LT_EV_PASTE, 0, 0, 0, 0, "a\n\x1b[31m.c"};
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED, "paste goes into the filter");
	CHECK(strchr(lt_filepicker_query(f), '\x1b') == NULL && strchr(lt_filepicker_query(f), '\n') == NULL, "control bytes in a paste are replaced");
	lt_filepicker_set_query(f, "");

	/* ---- extension filter ---- */
	lt_filepicker_set_extensions(f, "C, .h;");
	names(f, buf, sizeof buf);
	CHECK(!strcmp(buf, "..|Alpha/|beta/|empty/|locked/|loop/|sub/|zeta/|A.C|a.c|c.h"), "only .c and .h files remain (any case), directories always show");
	lt_filepicker_set_extensions(f, "txt");
	names(f, buf, sizeof buf);
	CHECK(strstr(buf, "b.txt") && strstr(buf, longname) && !strstr(buf, "c.h") && !strstr(buf, "README"), "another extension list replaces the first");
	lt_filepicker_set_extensions(f, "");
	CHECK(lt_filepicker_count(f) == 17, "an empty list shows everything again");
	lt_filepicker_set_extensions(f, "c,h");
	d = dump(f, c, r, &th);
	CHECK(strstr(d, "[c,h]") != NULL, "the active extension list is shown in the filter row");
	free(d);
	lt_filepicker_set_extensions(f, NULL);

	/* ---- long names: cut with an ellipsis, in unicode and in ASCII ---- */
	/* the long name is the last row: scroll to it */
	e = key(LT_K_END); lt_filepicker_event(f, r, &e);
	d = dump(f, c, r, &th);
	CHECK(strstr(d, "xxxxxxxxxx\xE2\x80\xA6") != NULL, "a long name ends in an ellipsis");
	CHECK(strstr(d, ".txt") == NULL || strstr(d, "b.txt") != NULL, "the tail of a long name is cut off");
	free(d);
	lt_canvas *ca = lt_canvas_new(40, 12, ASC);
	d = dump(f, ca, r, &th);
	CHECK(strstr(d, "xxxxxxxxxx.") != NULL && strstr(d, "\xE2") == NULL, "without unicode the ellipsis is a dot and no multibyte glyph is drawn");
	free(d);
	lt_canvas_free(ca);
	lt_filepicker_set_cursor(f, 0);

	/* ---- the path row keeps the tail of a long path ---- */
	lt_filepicker_set_dir(f, inner);
	lt_canvas *cn = lt_canvas_new(20, 6, UNI);
	d = dump(f, cn, (lt_rect){0, 0, 20, 6}, &th);
	CHECK(strstr(d, "\xE2\x80\xA6") != NULL && strstr(d, "sub/inner") != NULL, "a long path is cut from the left, the end stays");
	free(d);
	lt_canvas_free(cn);
	lt_filepicker_set_dir(f, root);

	/* ---- mouse: click, double click, wheel (rows start at y=1) ---- */
	lt_filepicker_set_cursor(f, 0);
	e = mouse(LT_K_CLICK, 5, 1 + 2);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && !strcmp(cur(f), "beta"), "a click moves the cursor");
	CHECK(lt_filepicker_entry_at(f, r, 5, 3) == 2 && lt_filepicker_entry_at(f, r, 5, 0) == -1 && lt_filepicker_entry_at(f, r, 5, 11) == -1, "entry_at maps rows and rejects the path and filter rows");
	e = mouse(LT_K_CLICK, 5, 1 + 3);
	lt_filepicker_event(f, r, &e);
	e = mouse(LT_K_CLICK, 5, 1 + 3);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && strstr(lt_filepicker_dir(f), "/empty") != NULL, "a double click opens the directory");
	e = key(LT_K_LEFT); lt_filepicker_event(f, r, &e);
	lt_filepicker_set_cursor(f, 0);
	e = mouse(LT_K_DCLICK, 5, 1);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && strcmp(lt_filepicker_dir(f), real) != 0, "a DCLICK event on the .. row goes up");
	lt_filepicker_set_dir(f, root);
	e = mouse(LT_K_CLICK, 5, 1 + 12);                                            /* below the list: the filter row */
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_IGNORED, "a click outside the entry rows is ignored");
	e = mouse(LT_K_WHEEL_DOWN, 5, 4);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_HANDLED && lt_filepicker_entry_at(f, r, 5, 1) == 3 && lt_filepicker_cursor(f) == 0, "the wheel scrolls the view three rows, not the cursor");
	e = mouse(LT_K_WHEEL_UP, 5, 4); lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_entry_at(f, r, 5, 1) == 0, "wheel up scrolls back");
	e = mouse(LT_K_WHEEL_DOWN, 100, 100);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_IGNORED, "a wheel outside the widget is not taken");
	e = mouse(LT_K_CLICK, 39, 10);                                               /* scrollbar column, bottom: scrolls to the end */
	lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_entry_at(f, r, 5, 10) == 16, "a click on the scrollbar scrolls the view");
	e = mouse(LT_K_RELEASE, 39, 10); lt_filepicker_event(f, r, &e);
	lt_filepicker_set_cursor(f, 0);

	/* ---- symlink loops: the path never grows ---- */
	size_t base_len = strlen(lt_filepicker_dir(f));
	for (int i = 0; i < 40; i++) {
		lt_filepicker_set_cursor(f, row_of(f, "loop"));
		e = key(LT_K_ENTER); lt_filepicker_event(f, r, &e);
	}
	CHECK(strlen(lt_filepicker_dir(f)) == base_len && !strcmp(lt_filepicker_dir(f), real), "entering a link that points at its own parent 40 times stays in the same directory");
	lt_filepicker_set_dir(f, root);
	CHECK(row_of(f, "loop2") >= 0 && !lt_filepicker_is_dir(f, row_of(f, "loop2")), "a link to itself is a plain entry");
	lt_filepicker_set_cursor(f, row_of(f, "loop2"));
	e = key(LT_K_ENTER);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_ACTIVATE, "a plain entry that is a broken link can still be chosen");
	CHECK(!lt_filepicker_set_dir(f, "loop2") && lt_filepicker_error(f) != NULL && !strcmp(lt_filepicker_dir(f), real), "set_dir on a looping link fails and keeps the old directory");

	/* ---- errors: unreadable, missing, not a directory, removed under us ---- */
	CHECK(!lt_filepicker_set_dir(f, "/nonexistent-lt-filepicker-dir") && lt_filepicker_error(f) && !strcmp(lt_filepicker_dir(f), real), "a missing directory is refused, the old one stays");
	snprintf(buf, sizeof buf, "%s/b.txt", root);
	CHECK(!lt_filepicker_set_dir(f, buf) && lt_filepicker_error(f), "a file is not a directory");
	CHECK(lt_filepicker_set_dir(f, root) && lt_filepicker_error(f) == NULL, "a good directory clears the error");
	if (geteuid() != 0) {
		lt_filepicker_set_cursor(f, row_of(f, "locked"));
		e = key(LT_K_ENTER);
		CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED, "Enter on an unreadable directory still enters it");
		CHECK(lt_filepicker_error(f) != NULL && strstr(lt_filepicker_error(f), "denied") != NULL, "and reports why");
		CHECK(lt_filepicker_count(f) == 1 && !strcmp(cur(f), ".."), "only .. is left to leave it");
		d = dump(f, c, r, &th);
		CHECK(strstr(d, "denied") != NULL, "the error row is drawn");
		free(d);
		e = mouse(LT_K_CLICK, 5, 2);                                             /* the error row sits at y=1, the list starts below it */
		CHECK(lt_filepicker_event(f, r, &e) != LT_W_ACTIVATE && strstr(lt_filepicker_dir(f), "locked") != NULL, "a click on the error row chooses nothing");
		e = key(LT_K_LEFT); lt_filepicker_event(f, r, &e);
		CHECK(lt_filepicker_error(f) == NULL && !strcmp(cur(f), "locked"), "going up clears the error and lands on the directory");
	} else printf("filepicker: running as root, unreadable-directory case skipped\n");
	lt_filepicker_free(f);

	/* a directory removed while it is shown */
	char gone[PATH_MAX + 8];
	snprintf(gone, sizeof gone, "%s/gone", root);
	mkdir(gone, 0700);
	f = lt_filepicker_new(gone);
	rmdir(gone);
	e = key(LT_K_F5);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && lt_filepicker_error(f) != NULL, "F5 on a vanished directory shows an error row instead of crashing");
	d = dump(f, c, r, &th);
	free(d);
	e = key(LT_K_LEFT); lt_filepicker_event(f, r, &e);
	CHECK(!strcmp(lt_filepicker_dir(f), real) && lt_filepicker_error(f) == NULL, "and Left still leads out");
	lt_filepicker_free(f);

	/* a missing start directory falls back to the current one; NULL means the current one */
	f = lt_filepicker_new("/nonexistent-lt-filepicker-dir");
	CHECK(f && lt_filepicker_dir(f)[0] == '/', "an unusable start directory falls back");
	lt_filepicker_free(f);
	f = lt_filepicker_new(NULL);
	CHECK(f && lt_filepicker_dir(f)[0] == '/', "NULL means the current directory");
	lt_filepicker_free(f);

	/* ---- the root has no .. row and Left stays ---- */
	f = lt_filepicker_new("/");
	CHECK(!strcmp(lt_filepicker_dir(f), "/") && (lt_filepicker_count(f) == 0 || strcmp(lt_filepicker_name(f, 0), "..") != 0), "the root has no .. row");
	e = key(LT_K_LEFT); lt_filepicker_event(f, r, &e);
	CHECK(!strcmp(lt_filepicker_dir(f), "/"), "Left at the root stays");
	e = key(LT_K_BSPACE); lt_filepicker_event(f, r, &e);
	CHECK(!strcmp(lt_filepicker_dir(f), "/"), "Backspace at the root stays");
	lt_filepicker_free(f);

	/* ---- an empty directory: only the .. row; empty filter results; tiny rectangles ---- */
	char empty[PATH_MAX + 8];
	snprintf(empty, sizeof empty, "%s/empty", root);
	f = lt_filepicker_new(empty);
	CHECK(lt_filepicker_count(f) == 1 && lt_filepicker_total(f) == 0 && lt_filepicker_error(f) == NULL, "an empty directory shows only ..");
	for (int k = 0; k < 8; k++) { int ks[] = {LT_K_DOWN, LT_K_UP, LT_K_PGDN, LT_K_PGUP, LT_K_HOME, LT_K_END, 'x', LT_K_BSPACE}; e = key(ks[k]); lt_filepicker_event(f, r, &e); }
	lt_filepicker_free(f);
	f = lt_filepicker_new(root);
	for (int w = 0; w < 12; w++) for (int h = 0; h < 7; h++) {
		lt_canvas *tc = lt_canvas_new(w ? w : 1, h ? h : 1, UNI);
		lt_filepicker_draw(f, tc, (lt_rect){0, 0, w, h}, &th, w & 1);
		lt_filepicker_draw(f, tc, (lt_rect){-3, -2, w + 9, h + 6}, &th, true);
		e = mouse(LT_K_CLICK, 1, 1); lt_filepicker_event(f, (lt_rect){0, 0, w, h}, &e);
		lt_canvas_free(tc);
	}
	lt_filepicker_free(f);

	/* ---- 100000 entries ---- */
	char big[PATH_MAX + 8];
	snprintf(big, sizeof big, "%s/big", root);
	mkdir(big, 0700);
	for (int i = 0; i < 10; i++) { char n[16]; snprintf(n, sizeof n, "d%d", i); mk(big, n); }
	for (int i = 0; i < 100000; i++) { char n[24]; snprintf(n, sizeof n, "f%06d.dat", i); touch(big, n); }
	f = lt_filepicker_new(big);
	CHECK(f && lt_filepicker_error(f) == NULL && lt_filepicker_total(f) == 100010 && lt_filepicker_count(f) == 100011, "100000 files and 10 directories are all listed");
	CHECK(!strcmp(lt_filepicker_name(f, 1), "d0") && lt_filepicker_is_dir(f, 10) && !lt_filepicker_is_dir(f, 11) && !strcmp(lt_filepicker_name(f, 11), "f000000.dat") && !strcmp(lt_filepicker_name(f, 100010), "f099999.dat"), "directories first, then files in order, at both ends");
	d = dump(f, c, r, &th);
	CHECK(strstr(d, "100000/100010") != NULL || strstr(d, "100010/100010") != NULL, "the filter row shows the counts");
	free(d);
	e = key(LT_K_END);
	CHECK(lt_filepicker_event(f, r, &e) == LT_W_CHANGED && lt_filepicker_cursor(f) == 100010, "End goes to the last of 100010");
	d = dump(f, c, r, &th);
	CHECK(strstr(d, "f099999.dat") != NULL, "and it is drawn");
	free(d);
	e = key(LT_K_PGUP); lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_cursor(f) == 100010 - 9, "PgUp works at that size");
	e = mouse(LT_K_CLICK, 39, 5);                                                /* the middle of the scrollbar */
	lt_filepicker_event(f, r, &e);
	int mid = lt_filepicker_entry_at(f, r, 5, 1);
	CHECK(mid > 30000 && mid < 70000, "a click in the middle of the scrollbar lands near the middle");
	e = mouse(LT_K_RELEASE, 39, 5); lt_filepicker_event(f, r, &e);
	e = mouse(LT_K_WHEEL_UP, 5, 4); lt_filepicker_event(f, r, &e);
	CHECK(lt_filepicker_entry_at(f, r, 5, 1) == mid - 3, "the wheel still moves three rows");
	int expect = 0;
	for (int i = 0; i < 100000; i++) { char n[24]; snprintf(n, sizeof n, "f%06d.dat", i); if (strstr(n, "00999")) expect++; }
	lt_filepicker_set_query(f, "00999");
	CHECK(lt_filepicker_count(f) == expect + 1 && expect > 0, "the filter narrows 100000 entries correctly");
	CHECK(lt_filepicker_cursor(f) == 1 && strstr(cur(f), "00999") != NULL, "and the cursor sits on the first match");
	lt_filepicker_set_query(f, "");
	lt_filepicker_set_extensions(f, "dat");
	CHECK(lt_filepicker_count(f) == 100011, "the extension filter keeps all files");
	lt_filepicker_set_extensions(f, "nope");
	CHECK(lt_filepicker_count(f) == 11, "a non-matching extension leaves the directories and ..");
	lt_filepicker_free(f);

	lt_canvas_free(c);
	rm_tree(root);
	CHECK(access(root, F_OK) != 0, "the temp tree is gone");
	printf(fails ? "filepicker: %d failed\n" : "filepicker: ok\n", fails);
	return fails != 0;
}
