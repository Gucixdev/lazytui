/* test_windows.c - lt_window / lt_windows (src/windows/window.c): placement, opaque drawing, modal, raise, drag, Esc, headless. */
#include "lazytui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

static int drawn, got;
static void draw_cb(lt_window *w, lt_canvas *c, lt_rect in, const lt_theme *t, void *u)
{
	drawn++;
	lt_text(c, in, in.x, in.y, (const char *)u, t->s[LT_T_TEXT], in.w);
}
static lt_wres ev_cb(lt_window *w, lt_rect in, const lt_event *ev, void *u)
{
	got++;
	return ev->kind == LT_EV_KEY && ev->key == 'x' ? LT_W_ACTIVATE : LT_W_HANDLED;
}

int main(void)
{
	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(40, 12, UNI);
	lt_window *a = lt_window_new("Alpha", 20, 6);
	lt_window *b = lt_window_new("Beta", 10, 5);
	lt_windows *s = lt_windows_new();
	CHECK(a && b && s, "created");
	lt_windows_add(s, a); lt_windows_add(s, b); lt_windows_add(s, a);
	lt_window_set_content(a, draw_cb, ev_cb, "hello");

	/* closed: nothing drawn, nothing handled, no top */
	lt_windows_draw(s, c, &th);
	CHECK(drawn == 0, "a closed window draws nothing");
	CHECK(lt_windows_top(s) == NULL && !lt_windows_modal(s), "no open window: no top, not modal");
	lt_event e = mouse(LT_K_CLICK, 5, 5);
	CHECK(lt_windows_event(s, &e) == LT_W_IGNORED, "no window open: the page gets the event");

	/* open centred, drawn with title and content, clamped inside the canvas */
	lt_window_open(a);
	lt_windows_draw(s, c, &th);
	lt_rect r = lt_window_rect(a);
	CHECK(r.w == 20 && r.h == 6 && r.x == 10 && r.y == 3, "opens centred");
	char *d = lt_canvas_dump(c);
	CHECK(strstr(d, "Alpha") && strstr(d, "hello") && drawn == 1, "title and content are drawn");
	free(d);
	lt_window_open_at(b, 38, 11);
	lt_windows_draw(s, c, &th);
	r = lt_window_rect(b);
	CHECK(r.x + r.w <= 40 && r.y + r.h <= 12 && r.x >= 0, "a window placed off the edge is clamped onto the canvas");

	/* opaque: text under a window does not show through */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_text(c, (lt_rect){0, 0, 40, 12}, 12, 5, "UNDER", th.s[LT_T_TEXT], 5);
	lt_window_close(b);
	lt_window_set_content(a, NULL, NULL, NULL);
	lt_windows_draw(s, c, &th);
	d = lt_canvas_dump(c);
	CHECK(!strstr(d, "UNDER"), "the window covers what is below it");
	free(d);

	/* events: inside -> callback, outside + non-modal -> ignored, modal -> swallowed */
	lt_window_set_content(a, draw_cb, ev_cb, "hello");
	e = mouse(LT_K_CLICK, 12, 5);
	CHECK(lt_windows_event(s, &e) == LT_W_HANDLED && got == 1, "a click inside reaches the callback");
	e = mouse(LT_K_CLICK, 0, 0);
	CHECK(lt_windows_event(s, &e) == LT_W_IGNORED, "a click outside a non-modal window is ignored");
	lt_window_set_modal(a, true);
	CHECK(lt_windows_modal(s), "modal is reported");
	CHECK(lt_windows_event(s, &e) == LT_W_HANDLED, "a modal window swallows a click outside");
	e = key('q');
	CHECK(lt_windows_event(s, &e) == LT_W_HANDLED, "a modal window swallows keys it does not use");
	e = key('x');
	CHECK(lt_windows_event(s, &e) == LT_W_ACTIVATE, "keys go to the top window's callback");

	/* Esc closes the top window */
	e = key(LT_K_ESC);
	CHECK(lt_windows_event(s, &e) == LT_W_CLOSED && !lt_window_is_open(a), "Esc closes the top window");
	CHECK(!lt_windows_modal(s), "a closed modal no longer blocks");

	/* raise: clicking a lower window brings it to the top */
	lt_window_set_modal(a, false);
	lt_window_open_at(a, 2, 2); lt_window_open_at(b, 8, 4);
	lt_windows_raise(s, b);
	lt_windows_draw(s, c, &th);
	CHECK(lt_windows_top(s) == b, "a raised window is on top");
	e = mouse(LT_K_CLICK, 3, 3);
	lt_windows_event(s, &e);
	CHECK(lt_windows_top(s) == a, "a click on the lower window raises it");

	/* drag by the title row */
	lt_windows_draw(s, c, &th);
	r = lt_window_rect(a);
	e = mouse(LT_K_CLICK, r.x + 3, r.y);
	CHECK(lt_windows_event(s, &e) == LT_W_HANDLED, "a click on the title starts a drag");
	e = mouse(LT_K_DRAG, r.x + 8, r.y + 3);
	lt_windows_event(s, &e);
	e = mouse(LT_K_RELEASE, r.x + 8, r.y + 3);
	lt_windows_event(s, &e);
	lt_windows_draw(s, c, &th);
	lt_rect r2 = lt_window_rect(a);
	CHECK(r2.x == r.x + 5 && r2.y == r.y + 3, "dragging moves the window by the mouse travel");
	lt_window_set_movable(a, false);
	e = mouse(LT_K_CLICK, r2.x + 3, r2.y);
	lt_windows_event(s, &e);
	e = mouse(LT_K_DRAG, r2.x + 9, r2.y + 1);
	lt_windows_event(s, &e);
	lt_windows_draw(s, c, &th);
	CHECK(lt_window_rect(a).x == r2.x, "a window that is not movable stays put");

	/* remove + free */
	lt_windows_remove(s, a);
	CHECK(lt_windows_top(s) == b, "a removed window leaves the stack");
	lt_windows_free(s); lt_window_free(a); lt_window_free(b);
	lt_canvas_free(c);
	if (!fails) printf("test_windows: all passed\n");
	return fails ? 1 : 0;
}
