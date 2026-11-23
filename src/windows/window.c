/* window.c - floating windows and the stack that holds them. A window is not a widget: a widget is drawn into a rectangle the layout gave
 * it, a window brings its own rectangle, floats over everything, can be modal, raised and dragged by its title. What is inside is a pair of
 * callbacks (or nothing: the caller draws into lt_window_inner). */
#include "internal.h"

struct lt_window {
	char *title;
	int w, h, x, y;                  /* size asked for; origin once placed */
	bool open, modal, placed, centre, movable;
	lt_window_draw_fn draw;
	lt_window_event_fn on_event;
	void *user;
	lt_rect rect;                    /* where it was drawn last, clamped to the canvas */
};

lt_window *lt_window_new(const char *title, int w, int h)
{
	lt_window *win = calloc(1, sizeof *win);
	if (!win) return NULL;
	win->title = strdup(title ? title : "");
	win->w = w < 3 ? 3 : w; win->h = h < 3 ? 3 : h;
	win->centre = true; win->movable = true;
	return win;
}
void lt_window_free(lt_window *w) { if (!w) return; free(w->title); free(w); }
void lt_window_set_title(lt_window *w, const char *title) { char *t = strdup(title ? title : ""); if (t) { free(w->title); w->title = t; } }
void lt_window_set_content(lt_window *w, lt_window_draw_fn draw, lt_window_event_fn on_event, void *user) { w->draw = draw; w->on_event = on_event; w->user = user; }
void lt_window_set_modal(lt_window *w, bool modal) { w->modal = modal; }
void lt_window_set_movable(lt_window *w, bool movable) { w->movable = movable; }
void lt_window_resize(lt_window *w, int cols, int rows) { w->w = cols < 3 ? 3 : cols; w->h = rows < 3 ? 3 : rows; }
void lt_window_open(lt_window *w) { w->open = true; w->placed = false; w->centre = true; }
void lt_window_open_at(lt_window *w, int x, int y) { w->open = true; w->placed = true; w->centre = false; w->x = x; w->y = y; }
void lt_window_close(lt_window *w) { w->open = false; }
bool lt_window_is_open(const lt_window *w) { return w->open; }
bool lt_window_is_modal(const lt_window *w) { return w->modal; }
lt_rect lt_window_rect(const lt_window *w) { return w->rect; }
lt_rect lt_window_inner(const lt_window *w) { return lt_panel_inner(w->rect); }

/* size and place inside a canvas of cw x ch: never larger than the canvas, never off it */
static void place(lt_window *w, int cw, int ch)
{
	int ww = w->w > cw ? cw : w->w, hh = w->h > ch ? ch : w->h;
	if (w->centre && !w->placed) { w->x = (cw - ww) / 2; w->y = (ch - hh) / 2; w->placed = true; }
	w->x = lt_clampi(w->x, 0, cw > ww ? cw - ww : 0);
	w->y = lt_clampi(w->y, 0, ch > hh ? ch - hh : 0);
	w->rect = (lt_rect){w->x, w->y, ww, hh};
}

void lt_window_draw(lt_window *w, lt_canvas *c, const lt_theme *t)
{
	if (!w->open) return;
	place(w, lt_canvas_w(c), lt_canvas_h(c));
	lt_fill(c, w->rect, ' ', t->s[LT_T_TEXT]);                  /* opaque: what is under it must not show through */
	lt_panel(c, w->rect, t, w->title[0] ? w->title : NULL);
	if (w->draw) w->draw(w, c, lt_window_inner(w), t, w->user);
}

lt_wres lt_window_event(lt_window *w, const lt_event *ev)
{
	if (!w->open) return LT_W_IGNORED;
	if (ev->kind == LT_EV_KEY && ev->key == LT_K_ESC) { lt_window_close(w); return LT_W_CLOSED; }
	if (ev->kind == LT_EV_MOUSE && !lt_in(w->rect, ev->x, ev->y)) return LT_W_IGNORED;
	if (w->on_event) return w->on_event(w, lt_window_inner(w), ev, w->user);
	return ev->kind == LT_EV_KEY || ev->kind == LT_EV_MOUSE ? LT_W_HANDLED : LT_W_IGNORED;
}

/* ================================================================== the stack */
struct lt_windows { lt_window **w; int n, cap; lt_window *drag; int dx, dy; };

lt_windows *lt_windows_new(void) { return calloc(1, sizeof(lt_windows)); }
void lt_windows_free(lt_windows *s) { if (!s) return; free(s->w); free(s); }   /* the windows stay the caller's */

bool lt_windows_add(lt_windows *s, lt_window *w)
{
	for (int i = 0; i < s->n; i++) if (s->w[i] == w) return true;
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 4;
		lt_window **p = realloc(s->w, (size_t)cap * sizeof *p);
		if (!p) return false;
		s->w = p; s->cap = cap;
	}
	s->w[s->n++] = w;
	return true;
}

void lt_windows_remove(lt_windows *s, lt_window *w)
{
	for (int i = 0; i < s->n; i++) if (s->w[i] == w) { memmove(&s->w[i], &s->w[i + 1], (size_t)(s->n - i - 1) * sizeof *s->w); s->n--; break; }
	if (s->drag == w) s->drag = NULL;
}

void lt_windows_raise(lt_windows *s, lt_window *w)
{
	for (int i = 0; i < s->n; i++) if (s->w[i] == w) { memmove(&s->w[i], &s->w[i + 1], (size_t)(s->n - i - 1) * sizeof *s->w); s->w[s->n - 1] = w; return; }
}

lt_window *lt_windows_top(const lt_windows *s)
{
	for (int i = s->n - 1; i >= 0; i--) if (s->w[i]->open) return s->w[i];
	return NULL;
}

bool lt_windows_modal(const lt_windows *s)
{
	for (int i = 0; i < s->n; i++) if (s->w[i]->open && s->w[i]->modal) return true;
	return false;
}

void lt_windows_draw(lt_windows *s, lt_canvas *c, const lt_theme *t)
{
	for (int i = 0; i < s->n; i++) lt_window_draw(s->w[i], c, t);
}

/* Top window first. An open modal window takes every event that is not for a window above it (the page below is dead until it closes).
 * A click on a window raises it; the title row of a movable window drags it. Esc closes the top window. */
lt_wres lt_windows_event(lt_windows *s, const lt_event *ev)
{
	if (s->drag) {
		if (ev->kind == LT_EV_MOUSE && ev->key == LT_K_DRAG) { s->drag->x = ev->x - s->dx; s->drag->y = ev->y - s->dy; s->drag->placed = true; return LT_W_HANDLED; }
		s->drag = NULL;
		if (ev->kind == LT_EV_MOUSE && ev->key == LT_K_RELEASE) return LT_W_HANDLED;
	}
	lt_window *top = lt_windows_top(s);
	if (ev->kind == LT_EV_KEY) {
		if (!top) return LT_W_IGNORED;
		lt_wres r = lt_window_event(top, ev);
		return r != LT_W_IGNORED ? r : (lt_windows_modal(s) ? LT_W_HANDLED : LT_W_IGNORED);
	}
	if (ev->kind != LT_EV_MOUSE) return LT_W_IGNORED;
	for (int i = s->n - 1; i >= 0; i--) {
		lt_window *w = s->w[i];
		if (!w->open || !lt_in(w->rect, ev->x, ev->y)) continue;
		if (ev->key == LT_K_CLICK || ev->key == LT_K_DCLICK) lt_windows_raise(s, w);
		if (ev->key == LT_K_CLICK && w->movable && ev->y == w->rect.y) { s->drag = w; s->dx = ev->x - w->rect.x; s->dy = ev->y - w->rect.y; return LT_W_HANDLED; }
		return lt_window_event(w, ev);
	}
	return lt_windows_modal(s) ? LT_W_HANDLED : LT_W_IGNORED;
}
