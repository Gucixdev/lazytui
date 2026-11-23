/* menu.c - menu bar, pull-down menus and context menus. A window layer: it draws over the widgets, takes the mouse while open. */
#include "internal.h"
#include <stdio.h>

/* ================================================================== menu */
typedef struct { char *label, *shortcut; int id; bool sep, check, checked, enabled; } mitem;
typedef struct { char *title; mitem *it; int n, cap; } mmenu;
struct lt_menu {
	mmenu *m; int n;
	int open;                        /* -1 closed, else the open menu */
	int cursor;                      /* item under the cursor in the open menu */
	int last_id;
	int *tx, *tw;                    /* title positions on the bar */
	lt_rect bar, popup;
	int px, py;                      /* popup origin */
	bool context;
};

lt_menu *lt_menu_new(void) { lt_menu *m = calloc(1, sizeof *m); m->open = -1; m->last_id = -1; return m; }
void lt_menu_free(lt_menu *m)
{
	if (!m) return;
	for (int i = 0; i < m->n; i++) { for (int k = 0; k < m->m[i].n; k++) { free(m->m[i].it[k].label); free(m->m[i].it[k].shortcut); } free(m->m[i].it); free(m->m[i].title); }
	free(m->m); free(m->tx); free(m->tw); free(m);
}
int lt_menu_add(lt_menu *m, const char *title)
{
	m->m = realloc(m->m, (size_t)(m->n + 1) * sizeof *m->m);
	m->m[m->n] = (mmenu){strdup(title ? title : ""), NULL, 0, 0};
	m->tx = realloc(m->tx, (size_t)(m->n + 1) * sizeof(int)); m->tw = realloc(m->tw, (size_t)(m->n + 1) * sizeof(int));
	m->tx[m->n] = m->tw[m->n] = 0;
	return m->n++;
}
static void add_item(lt_menu *m, int menu, mitem it)
{
	if (menu < 0 || menu >= m->n) return;
	mmenu *mm = &m->m[menu];
	if (mm->n == mm->cap) { mm->cap = mm->cap ? mm->cap * 2 : 8; mm->it = realloc(mm->it, (size_t)mm->cap * sizeof *mm->it); }
	mm->it[mm->n++] = it;
}
void lt_menu_item(lt_menu *m, int menu, const char *label, const char *sc, int id) { add_item(m, menu, (mitem){strdup(label ? label : ""), strdup(sc ? sc : ""), id, false, false, false, true}); }
void lt_menu_check(lt_menu *m, int menu, const char *label, const char *sc, int id, bool on) { add_item(m, menu, (mitem){strdup(label ? label : ""), strdup(sc ? sc : ""), id, false, true, on, true}); }
void lt_menu_sep(lt_menu *m, int menu) { add_item(m, menu, (mitem){strdup(""), strdup(""), -1, true, false, false, false}); }
static mitem *find_item(const lt_menu *m, int id) { for (int i = 0; i < m->n; i++) for (int k = 0; k < m->m[i].n; k++) if (!m->m[i].it[k].sep && m->m[i].it[k].id == id) return &m->m[i].it[k]; return NULL; }
void lt_menu_set_checked(lt_menu *m, int id, bool on) { mitem *it = find_item(m, id); if (it) it->checked = on; }
void lt_menu_set_enabled(lt_menu *m, int id, bool on) { mitem *it = find_item(m, id); if (it) it->enabled = on; }
bool lt_menu_checked(const lt_menu *m, int id) { mitem *it = find_item(m, id); return it && it->checked; }
int lt_menu_id(const lt_menu *m) { return m->last_id; }
bool lt_menu_is_open(const lt_menu *m) { return m->open >= 0; }
void lt_menu_close(lt_menu *m) { m->open = -1; m->context = false; }

static int first_enabled(const mmenu *mm, int from, int dir)
{
	for (int i = from; i >= 0 && i < mm->n; i += dir) if (!mm->it[i].sep && mm->it[i].enabled) return i;
	return -1;
}

void lt_menu_open_at(lt_menu *m, int menu, int x, int y)
{
	if (menu < 0 || menu >= m->n) return;
	m->open = menu; m->px = x; m->py = y; m->context = true;
	int f = first_enabled(&m->m[menu], 0, 1);
	m->cursor = f < 0 ? 0 : f;
}

void lt_menu_draw_bar(lt_menu *m, lt_canvas *c, lt_rect r, const lt_theme *t)
{
	if (r.w < 1) return;
	r.h = 1;
	m->bar = r;
	lt_rect all = lt_full(c);
	lt_fill(c, r, ' ', t->s[LT_T_TEXT]);
	int x = r.x + 1;
	for (int i = 0; i < m->n; i++) {
		int w = lt_utf8_width(m->m[i].title) + 2;
		m->tx[i] = x; m->tw[i] = w;
		lt_style st = (i == m->open && !m->context) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		if (x + w > r.x + r.w) { m->tw[i] = 0; continue; }
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', st);
		lt_text(c, all, x + 1, r.y, m->m[i].title, st, w - 2);
		x += w + 1;
	}
}

static void popup_geom(const lt_menu *m, int canvas_w, int canvas_h, lt_rect *out)
{
	const mmenu *mm = &m->m[m->open];
	int w = 8;
	for (int i = 0; i < mm->n; i++) { int l = lt_utf8_width(mm->it[i].label) + 6 + (mm->it[i].shortcut[0] ? lt_utf8_width(mm->it[i].shortcut) + 3 : 0); if (l > w) w = l; }
	int h = mm->n + 2;
	int x = m->context ? m->px : m->tx[m->open], y = m->context ? m->py : m->bar.y + 1;
	if (x + w > canvas_w) x = canvas_w - w;
	if (y + h > canvas_h) y = canvas_h - h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	*out = (lt_rect){x, y, w, h};
}

void lt_menu_draw_popup(lt_menu *m, lt_canvas *c, const lt_theme *t)
{
	if (m->open < 0 || m->open >= m->n) return;
	lt_rect p;
	popup_geom(m, lt_canvas_w(c), lt_canvas_h(c), &p);
	m->popup = p;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_fill(c, p, ' ', t->s[LT_T_TEXT]);
	lt_panel(c, p, t, NULL);
	const mmenu *mm = &m->m[m->open];
	for (int i = 0; i < mm->n; i++) {
		const mitem *it = &mm->it[i];
		int y = p.y + 1 + i;
		if (it->sep) { for (int k = 1; k < p.w - 1; k++) lt_put(c, all, p.x + k, y, uni ? 0x2500 : '-', t->s[LT_T_BORDER]); continue; }
		lt_style st = i == m->cursor ? t->s[LT_T_SELECT] : t->s[it->enabled ? LT_T_TEXT : LT_T_DIM];
		lt_fill(c, (lt_rect){p.x + 1, y, p.w - 2, 1}, ' ', st);
		if (it->check) lt_put(c, all, p.x + 2, y, it->checked ? (uni ? 0x2713 : 'x') : ' ', st);
		lt_text(c, all, p.x + 4, y, it->label, st, p.w - 6);
		if (it->shortcut[0]) { int sw = lt_utf8_width(it->shortcut); lt_text(c, all, p.x + p.w - 2 - sw, y, it->shortcut, i == m->cursor ? st : t->s[LT_T_DIM], sw); }
	}
}

static lt_wres activate(lt_menu *m)
{
	mmenu *mm = &m->m[m->open];
	if (m->cursor < 0 || m->cursor >= mm->n) return LT_W_HANDLED;
	mitem *it = &mm->it[m->cursor];
	if (it->sep || !it->enabled) return LT_W_HANDLED;
	if (it->check) it->checked = !it->checked;
	m->last_id = it->id;
	lt_menu_close(m);
	return LT_W_ACTIVATE;
}

lt_wres lt_menu_event(lt_menu *m, lt_rect bar, const lt_event *ev, int cw, int ch)
{
	if (m->n == 0) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF;
	bar.h = 1;
	if (ev->kind == LT_EV_MOUSE) {
		if (m->open >= 0) {
			lt_rect p;
			popup_geom(m, cw, ch, &p);
			if (base == LT_K_CLICK && lt_in(p, ev->x, ev->y)) {
				int i = ev->y - p.y - 1;
				if (i >= 0 && i < m->m[m->open].n) { m->cursor = i; return activate(m); }
				return LT_W_HANDLED;
			}
			if (base == LT_K_CLICK) {                                                       /* outside: another title switches, anything else closes */
				for (int i = 0; !m->context && i < m->n; i++) if (m->tw[i] && lt_in((lt_rect){m->tx[i], bar.y, m->tw[i], 1}, ev->x, ev->y)) { if (i == m->open) { lt_menu_close(m); return LT_W_CLOSED; } m->open = i; m->cursor = first_enabled(&m->m[i], 0, 1); if (m->cursor < 0) m->cursor = 0; return LT_W_HANDLED; }
				lt_menu_close(m);
				return LT_W_CLOSED;
			}
			return LT_W_HANDLED;
		}
		if (base != LT_K_CLICK) return LT_W_IGNORED;
		for (int i = 0; i < m->n; i++) if (m->tw[i] && lt_in((lt_rect){m->tx[i], bar.y, m->tw[i], 1}, ev->x, ev->y)) { m->open = i; m->context = false; m->cursor = first_enabled(&m->m[i], 0, 1); if (m->cursor < 0) m->cursor = 0; return LT_W_HANDLED; }
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	if (m->open < 0) {
		if (base == LT_K_DOWN || base == LT_K_ENTER) { m->open = 0; m->context = false; m->cursor = first_enabled(&m->m[0], 0, 1); if (m->cursor < 0) m->cursor = 0; return LT_W_HANDLED; }
		return LT_W_IGNORED;
	}
	mmenu *mm = &m->m[m->open];
	int f;
	switch (base) {
	case LT_K_ESC: lt_menu_close(m); return LT_W_CLOSED;
	case LT_K_DOWN: f = first_enabled(mm, m->cursor + 1, 1); if (f >= 0) m->cursor = f; else { f = first_enabled(mm, 0, 1); if (f >= 0) m->cursor = f; } return LT_W_HANDLED;
	case LT_K_UP: f = first_enabled(mm, m->cursor - 1, -1); if (f >= 0) m->cursor = f; else { f = first_enabled(mm, mm->n - 1, -1); if (f >= 0) m->cursor = f; } return LT_W_HANDLED;
	case LT_K_LEFT: if (!m->context) { m->open = (m->open + m->n - 1) % m->n; f = first_enabled(&m->m[m->open], 0, 1); m->cursor = f < 0 ? 0 : f; } return LT_W_HANDLED;
	case LT_K_RIGHT: if (!m->context) { m->open = (m->open + 1) % m->n; f = first_enabled(&m->m[m->open], 0, 1); m->cursor = f < 0 ? 0 : f; } return LT_W_HANDLED;
	case LT_K_ENTER: case ' ': return activate(m);
	}
	return LT_W_HANDLED;
}

