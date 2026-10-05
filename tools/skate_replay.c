/* Plays one scripted ride through the Skate 3 simulation library and writes
 * every STEP reply to a file, so builds can be compared byte for byte (e.g.
 * skate3.dll under Wine against libskate3.so: whether Windows and Linux
 * players can predict against each other).
 *
 *   skate_replay <library> <asset root> <map.bsp> <x> <y> <z> <yaw> <seconds> <out file>
 *
 * Build: cc -O2 -o skate_replay tools/skate_replay.c -ldl
 *        x86_64-w64-mingw32-gcc -O2 -o skate_replay.exe tools/skate_replay.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define LOAD(path) ((void *)LoadLibraryA(path))
#define SYM(lib, name) ((void *)GetProcAddress((HMODULE)(lib), name))
#else
#include <dlfcn.h>
#define LOAD(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define SYM(lib, name) dlsym(lib, name)
#endif

typedef void *(*OpenFn)(const char *, char *, size_t);
typedef int (*RequestFn)(void *, unsigned char, const unsigned char *, size_t, unsigned char **, size_t *);
typedef void (*FreeFn)(unsigned char *, size_t);

enum { WORLD = 2, SPAWN = 3, STEP = 4 };
static RequestFn request_fn;
static FreeFn free_fn;
static void *sim;

typedef struct { unsigned char *data; size_t size, cap; } Buf;
static void put(Buf *b, const void *p, size_t n) {
	if (b->size + n > b->cap) { b->cap = (b->size + n) * 2; b->data = realloc(b->data, b->cap); }
	memcpy(b->data + b->size, p, n); b->size += n;
}
static void put_u32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void put_f32(Buf *b, float v) { put(b, &v, 4); }
static void put_str(Buf *b, const char *s) { put_u32(b, (uint32_t)strlen(s)); put(b, s, strlen(s)); }

/* Returns the reply (after the ok byte); exits on failure. */
static unsigned char *call(unsigned char kind, Buf *payload, size_t *size) {
	unsigned char *reply = NULL; size_t n = 0;
	if (request_fn(sim, kind, payload->data, payload->size, &reply, &n) != 0 || !reply || n < 1) { fprintf(stderr, "request %d failed\n", kind); exit(1); }
	if (reply[0] == 0) { uint32_t len; memcpy(&len, reply + 1, 4); fprintf(stderr, "request %d: %.*s\n", kind, (int)len, reply + 5); exit(1); }
	unsigned char *copy = malloc(n - 1); memcpy(copy, reply + 1, n - 1); *size = n - 1;
	free_fn(reply, n);
	payload->size = 0;
	return copy;
}

static uint32_t rng = 12345;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float unit(void) { return (float)(next() & 0xFFFFFF) / (float)0xFFFFFF; }

int main(int argc, char **argv) {
	if (argc != 10) { fprintf(stderr, "usage: %s <library> <asset root> <map.bsp> <x> <y> <z> <yaw> <seconds> <out>\n", argv[0]); return 2; }
	void *lib = LOAD(argv[1]);
	if (!lib) { fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
	OpenFn open_fn = (OpenFn)SYM(lib, "skate3_open");
	request_fn = (RequestFn)SYM(lib, "skate3_request");
	free_fn = (FreeFn)SYM(lib, "skate3_free");
	char error[1024] = "";
	sim = open_fn(argv[2], error, sizeof error);
	if (!sim) { fprintf(stderr, "open: %s\n", error); return 1; }

	FILE *f = fopen(argv[3], "rb");
	if (!f) { fprintf(stderr, "can't read %s\n", argv[3]); return 1; }
	fseek(f, 0, SEEK_END); long bsp_size = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *bsp = malloc(bsp_size);
	if (fread(bsp, 1, bsp_size, f) != (size_t)bsp_size) return 1;
	fclose(f);

	Buf b = {0}; size_t n;
	put_str(&b, "replay"); put_f32(&b, 0.0254f); put_u32(&b, (uint32_t)bsp_size); put(&b, bsp, bsp_size);
	put_u32(&b, 0); put_u32(&b, 0);
	free(call(WORLD, &b, &n));

	put_u32(&b, 1); put_f32(&b, (float)atof(argv[4])); put_f32(&b, (float)atof(argv[5])); put_f32(&b, (float)atof(argv[6]));
	put_f32(&b, (float)atof(argv[7])); put_str(&b, "");
	free(call(SPAWN, &b, &n));

	/* Wait for loading without stepping: a zero-length STEP answers LOADING. */
	for (;;) {
		put_u32(&b, 1); put_f32(&b, 0.0f); put_u32(&b, 0);
		for (int i = 0; i < 4; ++i) put_f32(&b, 0.0f);
		put_u32(&b, 0); put_f32(&b, 0.0f); put_f32(&b, 0.0f);
		unsigned char *r = call(STEP, &b, &n);
		uint32_t state; memcpy(&state, r, 4); free(r);
		if (state != 0xFFFFFFFFu) break;
	}

	FILE *out = fopen(argv[9], "wb");
	int steps = (int)(atof(argv[8]) / 0.015);
	uint32_t buttons = 0; float forward = 1.0f, side = 0.0f;
	for (int i = 0; i < steps; ++i) {
		float mx = 0.0f, my = 0.0f; uint32_t flags = 0;
		if (unit() < 0.04f) {
			static const uint32_t choices[] = { 0, 0, 1u << 1, 1u << 2, 1u << 0, 1u << 11 };
			buttons = choices[next() % 6];
			forward = (float)((int)(next() % 3) - 1);
			side = (float)((int)(next() % 3) - 1);
		}
		if (unit() < 0.05f) { mx = unit() * 300.0f - 150.0f; my = unit() * 400.0f - 200.0f; }
		if (i == steps / 2) flags = 1;	/* a forced bail */
		put_u32(&b, 1); put_f32(&b, 0.015f); put_u32(&b, buttons); put_f32(&b, forward); put_f32(&b, side);
		put_f32(&b, mx); put_f32(&b, my); put_u32(&b, flags); put_f32(&b, 0.0f); put_f32(&b, 0.0f);
		unsigned char *r = call(STEP, &b, &n);
		uint32_t len = (uint32_t)n;
		fwrite(&len, 4, 1, out); fwrite(r, 1, n, out);
		if (i == steps - 1) {
			float o[3]; uint32_t state; memcpy(&state, r, 4); memcpy(o, r + 8, 12);
			printf("%d steps, last state %u at (%.3f %.3f %.3f)\n", steps, state, o[0], o[1], o[2]);
		}
		free(r);
	}
	fclose(out);
	return 0;
}
