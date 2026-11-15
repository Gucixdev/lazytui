/* filepicker.c - lt_filepicker: a directory browser. Entries of one directory (directories first, then files, each sorted without regard to
 * case), a path row on top, a type-to-filter row at the bottom, optional extension filter, hidden files off until '.' is pressed on an empty filter.
 * Directories are tracked by their canonical path (realpath), so a symlink loop never makes the path grow and "up" is always the real parent.
 * Needs: lazytui.h (the lib core) + internal.h (shared scrolling helpers). One widget, one file.
 * Layout, top to bottom: path row, [error row], entries (+ scrollbar column), filter row. Below 4 rows only the entries are drawn. */
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct { const char *name; bool dir, up; } fentry;     /* name lives in the picker's name arena; up = the ".." row */

struct lt_filepicker {
	char *dir;                           /* canonical absolute path of the directory shown */
	lt_arena *names;                     /* every entry name of the loaded directory; reset on each load */
	fentry *e; int n, cap;               /* loaded entries: [0] is ".." (unless at the root), the rest sorted */
	int *shown; int nshown;              /* indexes into e that pass the filters, in order */
	int cursor, offset;                  /* positions within shown */
	bool follow;                         /* the view follows the cursor (false after wheel / scrollbar scrolling) */
	bool hidden;                         /* dot files visible */
	char *exts;                          /* extension filter text, NULL = all files */
	lt_input q;                          /* the type-to-filter text */
	char *selected;                      /* path last chosen with Enter / double click on a file */
	char err[160];                       /* why the last read or change of directory failed; "" = fine */
	lt_filepicker_fn fn; void *user;
	lt_ptr ptr;
};

/* ------------------------------------------------------------------ loading */
static void set_err(lt_filepicker *f, const char *what, int en)
{
	snprintf(f->err, sizeof f->err, "%s: %s", what, strerror(en));
}

static bool add_entry(lt_filepicker *f, const char *name, bool dir, bool up)
{
	if (f->n == f->cap) {
		int cap = f->cap ? f->cap * 2 : 64;
		fentry *ne = realloc(f->e, (size_t)cap * sizeof *ne);
		if (!ne) return false;
		f->e = ne; f->cap = cap;
	}
	const char *copy = lt_arena_strdup(f->names, name);
	if (!copy) return false;
	f->e[f->n++] = (fentry){copy, dir, up};
	return true;
}

static int entry_cmp(const void *pa, const void *pb)
{
	const fentry *a = pa, *b = pb;
	if (a->dir != b->dir) return a->dir ? -1 : 1;
	int c = strcasecmp(a->name, b->name);
	return c ? c : strcmp(a->name, b->name);
}

static bool is_root(const char *dir) { return dir[0] == '/' && dir[1] == 0; }

static void load(lt_filepicker *f)
{
	lt_arena_reset(f->names);
	f->n = 0;
	f->err[0] = 0;
	if (!is_root(f->dir) && !add_entry(f, "..", true, true)) { set_err(f, "cannot read", ENOMEM); return; }
	DIR *d = opendir(f->dir);
	if (!d) { set_err(f, "cannot read", errno); return; }
	struct dirent *de;
	errno = 0;
	while ((de = readdir(d))) {
		const char *nm = de->d_name;
		if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) { errno = 0; continue; }
		bool dir = false;
#ifdef DT_DIR
		if (de->d_type == DT_DIR) dir = true;
		else if (de->d_type == DT_REG) dir = false;
		else
#endif
		{                                /* symlink or unknown type: ask what it points at; broken links and loops stay plain entries */
			struct stat st;
			if (fstatat(dirfd(d), nm, &st, 0) == 0 && S_ISDIR(st.st_mode)) dir = true;
		}
		if (!add_entry(f, nm, dir, false)) { set_err(f, "cannot read", ENOMEM); break; }
		errno = 0;
	}
	if (errno && !f->err[0]) set_err(f, "cannot read", errno);
	closedir(d);
	int skip = f->n && f->e[0].up ? 1 : 0;
	if (f->n - skip > 1) qsort(f->e + skip, (size_t)(f->n - skip), sizeof *f->e, entry_cmp);
}

/* ------------------------------------------------------------------ filtering */
static bool ci_contains(const char *s, const char *needle)
{
	size_t nl = strlen(needle);
	if (!nl) return true;
	for (; *s; s++) if (strncasecmp(s, needle, nl) == 0) return true;
	return false;
}

/* does `name` end in one of the extensions listed in exts ("c,h", ".md;txt", separated by comma, semicolon or space)? */
static bool ext_ok(const char *exts, const char *name)
{
	if (!exts) return true;
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name) return false;                   /* no extension (a leading dot alone is not one) */
	dot++;
	size_t dl = strlen(dot);
	const char *p = exts;
	while (*p) {
		while (*p == ',' || *p == ';' || *p == ' ') p++;
		if (*p == '.') p++;
		const char *s = p;
		while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
		if ((size_t)(p - s) == dl && dl && strncasecmp(s, dot, dl) == 0) return true;
	}
	return false;
}

static void first_match(lt_filepicker *f)
{
	f->cursor = 0;
	if (f->q.len && f->nshown > 1 && f->e[f->shown[0]].up) f->cursor = 1;       /* the first real hit, not the ".." row */
}

/* rebuilds shown; the cursor stays on the entry called `keep` when it is still there, else on the first match / the top */
static void refilter(lt_filepicker *f, const char *keep)
{
	free(f->shown);
	f->shown = malloc(((size_t)f->n + 1) * sizeof(int));
	f->nshown = 0;
	if (!f->shown) return;
	for (int i = 0; i < f->n; i++) {
		const fentry *en = &f->e[i];
		if (!en->up) {
			if (!f->hidden && en->name[0] == '.') continue;
			if (!en->dir && !ext_ok(f->exts, en->name)) continue;
			if (f->q.len && !ci_contains(en->name, f->q.text)) continue;
		}
		f->shown[f->nshown++] = i;
	}
	bool found = false;
	for (int i = 0; keep && i < f->nshown; i++)
		if (strcmp(f->e[f->shown[i]].name, keep) == 0) { f->cursor = i; found = true; break; }
	if (!found) first_match(f);
	f->follow = true;
}

/* ------------------------------------------------------------------ directory changes */
static char *parent_of(const char *dir)             /* canonical paths hold no symlinks, so the lexical parent is the real one */
{
	size_t l = strlen(dir);
	while (l > 1 && dir[l - 1] == '/') l--;
	while (l > 1 && dir[l - 1] != '/') l--;
	if (l > 1) l--;                                  /* drop the separator, but keep "/" itself */
	char *p = malloc(l + 1);
	if (p) { memcpy(p, dir, l); p[l] = 0; }
	return p;
}

/* make `path` the shown directory. keep: entry name to leave the cursor on. False (state untouched, error row set) when it cannot be entered. */
static bool go(lt_filepicker *f, const char *path, const char *keep, bool clear_query)
{
	char *real = realpath(path, NULL);
	if (!real) { set_err(f, "cannot open", errno); return false; }
	struct stat st;
	if (stat(real, &st) == 0 && !S_ISDIR(st.st_mode)) { free(real); set_err(f, "cannot open", ENOTDIR); return false; }
	free(f->dir);
	f->dir = real;
	if (clear_query) lt_input_set(&f->q, "");
	f->cursor = f->offset = 0;
	load(f);                                         /* an unreadable directory is entered all the same: error row + ".." to leave it */
	refilter(f, keep);
	return true;
}

static void go_up(lt_filepicker *f)
{
	if (is_root(f->dir)) return;
	const char *base = strrchr(f->dir, '/');
	char *keep = strdup(base ? base + 1 : f->dir);
	char *up = parent_of(f->dir);
	if (up) go(f, up, keep, true);
	free(up); free(keep);
}

static void go_down(lt_filepicker *f, const char *name)
{
	size_t dl = strlen(f->dir);
	char *p = malloc(dl + strlen(name) + 2);
	if (!p) return;
	sprintf(p, "%s%s%s", f->dir, is_root(f->dir) ? "" : "/", name);
	go(f, p, NULL, true);
	free(p);
}

/* ------------------------------------------------------------------ public state */
lt_filepicker *lt_filepicker_new(const char *dir)
{
	lt_filepicker *f = calloc(1, sizeof *f);
	if (!f) return NULL;
	f->names = lt_arena_new(64 * 1024);
	f->follow = true;
	f->ptr.last_row = -1;
	lt_input_init(&f->q);
	f->dir = strdup("/");
	if (!f->names || !f->dir) { lt_filepicker_free(f); return NULL; }
	if (!go(f, dir && *dir ? dir : ".", NULL, true) && !go(f, ".", NULL, true)) { load(f); refilter(f, NULL); }
	return f;
}

void lt_filepicker_free(lt_filepicker *f)
{
	if (!f) return;
	lt_arena_free(f->names);
	free(f->e); free(f->shown); free(f->dir); free(f->exts); free(f->selected);
	lt_input_free(&f->q);
	free(f);
}

bool lt_filepicker_set_dir(lt_filepicker *f, const char *dir) { return go(f, dir && *dir ? dir : ".", NULL, true); }
const char *lt_filepicker_dir(const lt_filepicker *f) { return f->dir; }

void lt_filepicker_reload(lt_filepicker *f)
{
	char *keep = f->nshown ? strdup(f->e[f->shown[lt_clampi(f->cursor, 0, f->nshown - 1)]].name) : NULL;
	load(f);
	refilter(f, keep);
	free(keep);
}

void lt_filepicker_set_extensions(lt_filepicker *f, const char *exts)
{
	free(f->exts);
	f->exts = exts && *exts ? strdup(exts) : NULL;
	refilter(f, NULL);
}

void lt_filepicker_set_hidden(lt_filepicker *f, bool show)
{
	if (f->hidden == show) return;
	f->hidden = show;
	refilter(f, f->nshown ? f->e[f->shown[lt_clampi(f->cursor, 0, f->nshown - 1)]].name : NULL);
}
bool lt_filepicker_hidden(const lt_filepicker *f) { return f->hidden; }

void lt_filepicker_set_query(lt_filepicker *f, const char *text)
{
	lt_input_set(&f->q, text ? text : "");
	refilter(f, NULL);
}
const char *lt_filepicker_query(const lt_filepicker *f) { return f->q.text; }

void lt_filepicker_on_select(lt_filepicker *f, lt_filepicker_fn fn, void *user) { f->fn = fn; f->user = user; }
const char *lt_filepicker_selected(const lt_filepicker *f) { return f->selected; }
const char *lt_filepicker_error(const lt_filepicker *f) { return f->err[0] ? f->err : NULL; }

int lt_filepicker_count(const lt_filepicker *f) { return f->nshown; }
int lt_filepicker_total(const lt_filepicker *f) { return f->n - (f->n && f->e[0].up ? 1 : 0); }
const char *lt_filepicker_name(const lt_filepicker *f, int i) { return i >= 0 && i < f->nshown ? f->e[f->shown[i]].name : NULL; }
bool lt_filepicker_is_dir(const lt_filepicker *f, int i) { return i >= 0 && i < f->nshown && f->e[f->shown[i]].dir; }
int lt_filepicker_cursor(const lt_filepicker *f) { return f->nshown ? lt_clampi(f->cursor, 0, f->nshown - 1) : -1; }
void lt_filepicker_set_cursor(lt_filepicker *f, int i)
{
	if (f->nshown) f->cursor = lt_clampi(i, 0, f->nshown - 1);
	f->follow = true;
}

/* ------------------------------------------------------------------ layout */
typedef struct { lt_rect head, err, list, foot; } lay;

static lay layout(const lt_filepicker *f, lt_rect r)
{
	lay l = {{r.x, r.y, 0, 0}, {r.x, r.y, 0, 0}, r, {r.x, r.y, 0, 0}};
	if (r.h >= 4 && r.w >= 8) {
		l.head = (lt_rect){r.x, r.y, r.w, 1};
		l.foot = (lt_rect){r.x, r.y + r.h - 1, r.w, 1};
		l.list = (lt_rect){r.x, r.y + 1, r.w, r.h - 2};
	}
	if (f->err[0] && l.list.h >= 2) {
		l.err = (lt_rect){l.list.x, l.list.y, l.list.w, 1};
		l.list.y++; l.list.h--;
	}
	return l;
}

static void fix_offset(lt_filepicker *f, int rows)
{
	if (rows < 1) rows = 1;
	f->offset = f->follow ? lt_scroll_to(f->offset, f->cursor, rows, f->nshown)
	                      : lt_clampi(f->offset, 0, f->nshown > rows ? f->nshown - rows : 0);
}

int lt_filepicker_entry_at(const lt_filepicker *f, lt_rect r, int x, int y)
{
	lay l = layout(f, r);
	lt_rect body = lt_view_content(l.list, f->nshown);
	if (!lt_in(body, x, y)) return -1;
	int rows = l.list.h < 1 ? 1 : l.list.h;
	int off = f->follow ? lt_scroll_to(f->offset, f->cursor, rows, f->nshown) : lt_clampi(f->offset, 0, f->nshown > rows ? f->nshown - rows : 0);
	int pos = off + (y - l.list.y);
	return pos >= 0 && pos < f->nshown ? pos : -1;
}

/* ------------------------------------------------------------------ drawing */
static void put_ellipsis_text(lt_canvas *c, lt_rect clip, int x, int y, const char *s, lt_style st, int maxw)
{
	if (maxw < 1) return;
	if (lt_utf8_width(s) <= maxw) { lt_text(c, clip, x, y, s, st, maxw); return; }
	int used = lt_text(c, clip, x, y, s, st, maxw - 1);
	lt_put(c, clip, x + used, y, lt_canvas_caps(c)->unicode ? 0x2026 : '.', st);
}

/* a long path keeps its tail: "...ts/sub/dir" */
static void put_path(lt_canvas *c, lt_rect clip, int x, int y, const char *s, lt_style st, int maxw)
{
	if (maxw < 2) return;
	int w = lt_utf8_width(s);
	if (w <= maxw) { lt_text(c, clip, x, y, s, st, maxw); return; }
	int skip = w - (maxw - 1), got = 0;
	while (*s && got < skip) {
		uint32_t q;
		size_t l = lt_utf8_decode(s, &q);
		char tmp[5]; memcpy(tmp, s, l); tmp[l] = 0;
		got += lt_utf8_width(tmp);
		s += l;
	}
	lt_put(c, clip, x, y, lt_canvas_caps(c)->unicode ? 0x2026 : '.', st);
	lt_text(c, clip, x + 1, y, s, st, maxw - 1);
}

void lt_filepicker_draw(lt_filepicker *f, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 1 || r.h < 1) return;
	lt_rect all = lt_full(c);
	lay l = layout(f, r);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_fill(c, r, ' ', t->s[LT_T_TEXT]);
	fix_offset(f, l.list.h);
	if (l.head.h) put_path(c, all, l.head.x, l.head.y, f->dir, t->s[LT_T_TITLE], l.head.w);
	if (l.err.h) put_ellipsis_text(c, all, l.err.x, l.err.y, f->err, t->s[LT_T_ERR], l.err.w);
	lt_rect body = lt_view_content(l.list, f->nshown);
	for (int row = 0; row < l.list.h; row++) {
		int pos = f->offset + row;
		if (pos >= f->nshown) break;
		const fentry *en = &f->e[f->shown[pos]];
		bool cur = pos == f->cursor;
		lt_style st = en->dir ? t->s[LT_T_ACCENT] : t->s[LT_T_TEXT];
		if (cur) st = focused ? t->s[LT_T_SELECT] : st;
		if (cur && !focused) st.attr |= LT_BOLD;
		int y = body.y + row;
		lt_fill(c, (lt_rect){body.x, y, body.w, 1}, ' ', st);
		if (cur) lt_put(c, all, body.x, y, uni ? 0x258C : '>', focused ? st : t->s[LT_T_ACCENT]);
		char buf[NAME_MAX + 8];
		size_t k = 0;
		for (const char *s = en->name; *s && k < NAME_MAX + 1; s++) buf[k++] = ((unsigned char)*s < 32 || *s == 127) ? '?' : *s;
		if (en->dir && !en->up) buf[k++] = '/';
		buf[k] = 0;
		put_ellipsis_text(c, all, body.x + 2, y, buf, st, body.w - 2);
	}
	lt_scrollbar(c, (lt_rect){l.list.x + l.list.w - 1, l.list.y, 1, l.list.h}, t, f->nshown, f->offset, l.list.h);
	if (l.foot.h) {
		char info[96];
		int len = snprintf(info, sizeof info, "%d/%d%s%s%s%s", f->nshown - (f->nshown && f->e[f->shown[0]].up ? 1 : 0), lt_filepicker_total(f),
		                   f->hidden ? " [hidden]" : "", f->exts ? " [" : "", f->exts ? f->exts : "", f->exts ? "]" : "");
		if (len >= (int)sizeof info) len = (int)sizeof info - 1;
		int iw = lt_utf8_width(info);
		bool room = l.foot.w >= iw + 12;
		lt_searchbar(c, (lt_rect){l.foot.x, l.foot.y, room ? l.foot.w - iw - 1 : l.foot.w, 1}, t, "> ", f->q.text, focused ? f->q.cursor : -1);
		if (room) lt_text(c, all, l.foot.x + l.foot.w - iw, l.foot.y, info, t->s[LT_T_DIM], iw);
	}
}

/* ------------------------------------------------------------------ events */
/* Enter / double click on the entry under the cursor */
static lt_wres activate(lt_filepicker *f)
{
	if (!f->nshown) return LT_W_HANDLED;
	const fentry *en = &f->e[f->shown[lt_clampi(f->cursor, 0, f->nshown - 1)]];
	if (en->up) { go_up(f); return LT_W_CHANGED; }
	if (en->dir) { go_down(f, en->name); return LT_W_CHANGED; }
	size_t dl = strlen(f->dir);
	char *p = malloc(dl + strlen(en->name) + 2);
	if (!p) return LT_W_HANDLED;
	sprintf(p, "%s%s%s", f->dir, is_root(f->dir) ? "" : "/", en->name);
	free(f->selected);
	f->selected = p;
	if (f->fn) f->fn(p, f->user);
	return LT_W_ACTIVATE;
}

static char *sanitized(const char *s)
{
	char *c = strdup(s);
	if (c) for (char *p = c; *p; p++) if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
	return c;
}

lt_wres lt_filepicker_event(lt_filepicker *f, lt_rect r, const lt_event *ev)
{
	if (r.w < 1 || r.h < 1) return LT_W_IGNORED;
	lay l = layout(f, r);
	int base = ev->key & 0xFFFFFF, mods = ev->key & ~0xFFFFFF;
	int page = l.list.h > 1 ? l.list.h - 1 : 1, last = f->nshown - 1, old = f->cursor;
	if (ev->kind == LT_EV_PASTE) {
		char *clean = ev->paste ? sanitized(ev->paste) : NULL;
		bool ch = clean && lt_input_paste(&f->q, clean);
		free(clean);
		if (ch) { refilter(f, NULL); }
		return ch ? LT_W_CHANGED : LT_W_HANDLED;
	}
	if (ev->kind == LT_EV_MOUSE) {
		fix_offset(f, l.list.h);
		bool bar;
		if (lt_view_mouse(&f->ptr, l.list, f->nshown, &f->offset, ev, &bar)) { f->follow = false; return LT_W_HANDLED; }
		lt_rect body = lt_view_content(l.list, f->nshown);
		if ((base == LT_K_CLICK || base == LT_K_DCLICK) && lt_in(body, ev->x, ev->y)) {
			int pos = f->offset + (ev->y - l.list.y);
			if (pos >= f->nshown) return LT_W_HANDLED;
			bool dbl = base == LT_K_DCLICK || lt_ptr_double(&f->ptr, pos);
			f->cursor = pos;
			f->follow = true;
			if (dbl) return activate(f);
			return pos != old ? LT_W_CHANGED : LT_W_HANDLED;
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY) return LT_W_IGNORED;
	if (mods & (LT_MOD_CTRL | LT_MOD_ALT)) {                 /* the editing keys of the filter row (ctrl-u, ctrl-w, alt-b ...) */
		char *before = strdup(f->q.text);
		bool ch = lt_input_key(&f->q, ev->key);
		bool text = before && strcmp(before, f->q.text) != 0;
		free(before);
		if (text) { refilter(f, NULL); return LT_W_CHANGED; }
		return ch ? LT_W_HANDLED : LT_W_IGNORED;
	}
	f->follow = true;
	switch (base) {
	case LT_K_UP: f->cursor = lt_clampi(f->cursor - 1, 0, last < 0 ? 0 : last); break;
	case LT_K_DOWN: f->cursor = lt_clampi(f->cursor + 1, 0, last < 0 ? 0 : last); break;
	case LT_K_PGUP: f->cursor = lt_clampi(f->cursor - page, 0, last < 0 ? 0 : last); break;
	case LT_K_PGDN: f->cursor = lt_clampi(f->cursor + page, 0, last < 0 ? 0 : last); break;
	case LT_K_HOME: f->cursor = 0; break;
	case LT_K_END: f->cursor = last < 0 ? 0 : last; break;
	case LT_K_ENTER: return activate(f);
	case LT_K_RIGHT:
		if (f->nshown && f->e[f->shown[f->cursor]].dir) return activate(f);
		return LT_W_HANDLED;
	case LT_K_LEFT: go_up(f); return LT_W_CHANGED;
	case LT_K_F5: lt_filepicker_reload(f); return LT_W_CHANGED;
	case LT_K_ESC:
		if (!f->q.len) return LT_W_IGNORED;
		lt_filepicker_set_query(f, "");
		return LT_W_CHANGED;
	case LT_K_BSPACE:
		if (!f->q.len) { go_up(f); return LT_W_CHANGED; }
		/* fall through */
	case LT_K_DEL:
		if (lt_input_key(&f->q, base)) { refilter(f, NULL); return LT_W_CHANGED; }
		return LT_W_HANDLED;
	default:
		if (base == '.' && !f->q.len) { lt_filepicker_set_hidden(f, !f->hidden); return LT_W_CHANGED; }
		if (base >= 32 && base != 127 && base < LT_K_ENTER) {
			if (lt_input_key(&f->q, base)) { refilter(f, NULL); return LT_W_CHANGED; }
			return LT_W_HANDLED;
		}
		return LT_W_IGNORED;
	}
	fix_offset(f, l.list.h);
	return f->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
}
