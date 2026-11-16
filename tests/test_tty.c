/* test_tty.c - the terminal decoder on a real pty: the child opens /dev/tty through lt_tty_open and reports every event on a pipe,
 * the parent types bytes into the pty master, with pauses, the way a terminal delivers one sequence in two reads. */
#include "lazytui.h"
#include <pty.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static void child(int out)
{
	lt_tty_opts o = {0};
	lt_tty *t = lt_tty_open(o);
	lt_event ev;
	char line[256];
	if (!t) { if (write(out, "E:open\n", 7) < 0) _exit(1); _exit(1); }
	if (write(out, "R\n", 2) < 0) _exit(1);
	while (lt_tty_wait(t, 3000, &ev)) {
		int n = 0;
		if (ev.kind == LT_EV_PASTE) n = snprintf(line, sizeof line, "P:%s\n", ev.paste ? ev.paste : "");
		else if (ev.kind == LT_EV_KEY) n = snprintf(line, sizeof line, "K:%x\n", (unsigned)ev.key);
		free(ev.paste);
		if (n > 0 && write(out, line, (size_t)n) < 0) break;
		if (ev.kind == LT_EV_KEY && ev.key == 'q') break;
	}
	lt_tty_close(t);
	_exit(0);
}

static void type(int fd, const char *s, size_t n) { if (write(fd, s, n) != (ssize_t)n) fails++; }

int main(void)
{
	int master, pp[2];
	char got[1024] = "";
	size_t len = 0;
	if (pipe(pp) < 0) { printf("tty: SKIP (no pipe)\n"); return 0; }
	setenv("TERM", "xterm-256color", 1);
	pid_t pid = forkpty(&master, NULL, NULL, NULL);
	if (pid < 0) { printf("tty: SKIP (no pty)\n"); return 0; }
	if (pid == 0) { close(pp[0]); child(pp[1]); }
	close(pp[1]);
	struct pollfd p = {pp[0], POLLIN, 0};
	if (poll(&p, 1, 5000) <= 0 || read(pp[0], got, 2) != 2 || got[0] != 'R') { printf("tty: SKIP (no terminal in the child)\n"); return 0; }
	got[0] = 0;

	type(master, "\033[200~abc\033[20", 13);               /* a paste whose end mark arrives in two reads */
	usleep(150000);
	type(master, "1~x", 3);
	usleep(150000);
	type(master, "\xc3", 1);                                /* a two-byte character cut the same way (the decoder waits 25 ms for the rest) */
	usleep(3000);
	type(master, "\xa9", 1);
	usleep(150000);
	type(master, "q", 1);
	for (;;) {
		struct pollfd q[2] = {{pp[0], POLLIN, 0}, {master, POLLIN, 0}};
		char junk[4096];
		if (poll(q, 2, 5000) <= 0) break;
		if (q[1].revents & POLLIN) { ssize_t jn = read(master, junk, sizeof junk); if (jn > 0 && getenv("LT_TTY_DEBUG")) fwrite(junk, 1, (size_t)jn, stderr); }
		if (q[0].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(pp[0], got + len, sizeof got - 1 - len);
			if (n <= 0) break;
			len += (size_t)n; got[len] = 0;
		} else if (q[1].revents & (POLLHUP | POLLERR)) break;
	}
	waitpid(pid, NULL, 0);
	CHECK(strstr(got, "P:abc\n") != NULL, "a paste ends at its end mark even when the mark comes in two reads");
	CHECK(strstr(got, "K:78\n") != NULL, "the key typed after the paste is a key, not paste text");
	CHECK(strstr(got, "K:e9\n") != NULL && strstr(got, "K:fffd\n") == NULL, "a character whose bytes come in two reads is one key");
	if (fails) printf("events were:\n%s", got);
	printf(fails ? "tty: %d failed\n" : "tty: ok\n", fails);
	return fails != 0;
}
