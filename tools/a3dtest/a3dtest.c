/*
 * a3dtest.c - controlled stimulus suite for Aureal Vortex 2 / A3D 2.0 measurements.
 *
 * Runs inside Windows 98 with the Aureal driver. Each test places a known sound with
 * known A3D parameters, holds it, and logs exactly what was requested and when, so the
 * register trace and line-in recording can be matched to the request.
 *
 * Build: see build.sh (MinGW cross compiler, no C runtime; needs ia3dapi.h from the
 *        A3D 2.0 SDK, which is not redistributed here).
 *
 * Usage (in Windows):  A3DTEST [test ...]
 *   no arguments = run-2 suite: sync xtalk ref hrtfmap glide2 reflect2 multi dsound sync
 *   v1 = the run-1 suite; all = everything
 *   run-1 tests: sync formats azimuth elevation distance gain eq doppler pitch occlusion
 *                reflections glide
 *   run-2 tests: xtalk ref hrtfmap glide2 reflect2 multi dsound
 *   noise2d = plain noise for 120 s (for vortex-poke.py eqsweep)
 *   wtpath = reflection rate and A3D-slot overflow (which sounds go to wavetable voices)
 *   click90 = one click source at 90 degrees for 150 s (for vortex-poke.py xtsweep)
 *   run3 = sync elev5 tcsweep (tcsweep is meant to run alongside vortex-poke.py tcsweep on the host)
 *   run12 = sync srcresp level midi hrtfgrid: converter response, levels and headroom, the MIDI
 *           synthesiser, and directions between the 15-degree table points (about 9 minutes)
 *   run13 = sync reflhf: reflection tone and level against wall reflectance (about 70 s)
 * Log: A3DTEST.LOG on the first writable hard disk other than C: (or C:\ if none).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <initguid.h>
#include <dsound.h>
#include "ia3dapi.h"

/* ------------------------------------------------------------------ minimal runtime */
void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) {
    while (*a && *b) { char x = *a++, y = *b++; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; }
    return *a == *b;
}

#define PI 3.14159265358979323846
static double dsin(double x) {               /* range-reduced Taylor series, |err| < 1e-9 */
    double x2, t, s; int i;
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    x2 = x * x; t = x; s = x;
    for (i = 1; i < 12; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
static double dcos(double x) { return dsin(x + PI / 2); }

/* "%.Nf"-style float formatting into buf; returns chars written */
static int ffmt(char *buf, double v, int dec) {
    char tmp[32]; int n = 0, i, neg = v < 0; unsigned long ip; double fp; unsigned long scale = 1;
    if (neg) v = -v;
    for (i = 0; i < dec; i++) scale *= 10;
    v += 0.5 / scale;
    ip = (unsigned long)v; fp = v - ip;
    if (neg) buf[n++] = '-';
    i = 0; do { tmp[i++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    while (i) buf[n++] = tmp[--i];
    if (dec) { buf[n++] = '.'; for (i = 0; i < dec; i++) { fp *= 10; buf[n++] = (char)('0' + ((int)fp) % 10); fp -= (int)fp; } }
    buf[n] = 0; return n;
}

/* ------------------------------------------------------------------ logging & timing */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static LARGE_INTEGER g_qf, g_q0;
static HWND g_wnd;
static char g_line[512];
static int  g_pos;

static double now_s(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (double)(q.QuadPart - g_q0.QuadPart) / (double)g_qf.QuadPart; }

static void L_s(const char *s) { while (*s && g_pos < 500) g_line[g_pos++] = *s++; }
static void L_f(double v, int d) { char b[40]; ffmt(b, v, d); L_s(b); }
static void L_i(long v) { char b[24]; wsprintfA(b, "%ld", v); L_s(b); }
static void L_x(unsigned long v) { char b[24]; wsprintfA(b, "0x%08lx", v); L_s(b); }
static void L_begin(void) { g_pos = 0; L_f(now_s(), 4); L_s(" "); }
static void L_end(void) {
    DWORD w; g_line[g_pos++] = '\r'; g_line[g_pos++] = '\n';
    if (g_log != INVALID_HANDLE_VALUE) { WriteFile(g_log, g_line, g_pos, &w, NULL); FlushFileBuffers(g_log); }
    g_line[g_pos - 2] = 0;
    if (g_wnd) SetWindowTextA(g_wnd, g_line);
}
static void logmsg(const char *s) { L_begin(); L_s(s); L_end(); }

static void open_log(void) {
    char path[] = "D:\\A3DTEST.LOG"; char root[] = "D:\\"; char c;
    for (c = 'D'; c <= 'Z'; c++) {
        root[0] = c;
        if (GetDriveTypeA(root) == DRIVE_FIXED) { path[0] = c; break; }
    }
    if (c > 'Z') path[0] = 'C';
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE) SetFilePointer(g_log, 0, NULL, FILE_END);
}

static void pump(void) { MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); } }

/* ------------------------------------------------------------------ A3D state */
static LPA3D4 a3d;
static LPA3DGEOM geom;
static LPA3DMATERIAL mat;

typedef struct {
    LPA3DSOURCE src;
    double x, y, z;            /* position, metres; listener at origin facing -Z, +X right, +Y up */
} SRC;

enum { WALL_NONE = 0, WALL_OCCLUDE = 1, WALL_RIGHT = 2, WALL_LEFT = 4, WALL_BACK = 8, WALL_REFLECT = WALL_RIGHT };
static int g_wall = WALL_NONE;
static double g_wall_d = 2.0;

static void frame(SRC *s, int n) {
    int i;
    IA3d4_Clear(a3d);
    IA3dGeom_LoadIdentity(geom);
    IA3dGeom_PushMatrix(geom); IA3dGeom_BindListener(geom); IA3dGeom_PopMatrix(geom);
    for (i = 0; i < n; i++) {
        if (!s[i].src) continue;
        IA3dGeom_PushMatrix(geom);
        IA3dGeom_Translate3f(geom, (A3DVAL)s[i].x, (A3DVAL)s[i].y, (A3DVAL)s[i].z);
        IA3dGeom_BindSource(geom, s[i].src);
        IA3dGeom_PopMatrix(geom);
    }
    if (g_wall != WALL_NONE) {
        IA3dGeom_BindMaterial(geom, mat);
        IA3dGeom_Begin(geom, A3D_QUADS);
        IA3dGeom_Tag(geom, 1);
        if (g_wall == WALL_OCCLUDE) {          /* 8 m x 8 m wall across the front, 2 m away */
            IA3dGeom_Vertex3f(geom, -4.0f, -4.0f, -2.0f);
            IA3dGeom_Vertex3f(geom,  4.0f, -4.0f, -2.0f);
            IA3dGeom_Vertex3f(geom,  4.0f,  4.0f, -2.0f);
            IA3dGeom_Vertex3f(geom, -4.0f,  4.0f, -2.0f);
        } else {                               /* reflective walls, g_wall_d metres from the listener */
            float d = (float)g_wall_d;
            if (g_wall & WALL_RIGHT) {
                IA3dGeom_Vertex3f(geom, d, -4.0f,  4.0f);  IA3dGeom_Vertex3f(geom, d, -4.0f, -12.0f);
                IA3dGeom_Vertex3f(geom, d,  4.0f, -12.0f); IA3dGeom_Vertex3f(geom, d,  4.0f,  4.0f);
            }
            if (g_wall & WALL_LEFT) {
                IA3dGeom_Vertex3f(geom, -d, -4.0f,  4.0f);  IA3dGeom_Vertex3f(geom, -d, 4.0f,  4.0f);
                IA3dGeom_Vertex3f(geom, -d,  4.0f, -12.0f); IA3dGeom_Vertex3f(geom, -d, -4.0f, -12.0f);
            }
            if (g_wall & WALL_BACK) {
                IA3dGeom_Vertex3f(geom, -8.0f, -4.0f, d); IA3dGeom_Vertex3f(geom, -8.0f, 4.0f, d);
                IA3dGeom_Vertex3f(geom,  8.0f,  4.0f, d); IA3dGeom_Vertex3f(geom,  8.0f, -4.0f, d);
            }
        }
        IA3dGeom_End(geom);
    }
    IA3d4_Flush(a3d);
}

/* hold the current scene for ms milliseconds, refreshing frames at ~33 Hz */
static void hold(SRC *s, int n, int ms) {
    double t_end = now_s() + ms / 1000.0;
    while (now_s() < t_end) { frame(s, n); pump(); Sleep(30); }
}

static void place(SRC *s, double az_deg, double el_deg, double r) {
    double az = az_deg * PI / 180, el = el_deg * PI / 180;
    s->x = r * dcos(el) * dsin(az);
    s->y = r * dsin(el);
    s->z = -r * dcos(el) * dcos(az);
}

/* ------------------------------------------------------------------ stimuli */
enum { STIM_CLICK, STIM_NOISE, STIM_TONE, STIM_CLICKNOISE, STIM_CLICK_L, STIM_CLICK_R, STIM_TONE250 };

static unsigned long rng = 12345;
static int noise16(void) { rng = rng * 1103515245UL + 12345UL; return (int)((rng >> 16) & 0x7fff) - 16384; }

/* Fill a buffer: rate, channels, bits; seconds long; kind */
static void *make_wave(int kind, int rate, int ch, int bits, double secs, DWORD *bytes, WAVEFORMATEX *wf) {
    int frames = (int)(rate * secs), i, c, v = 0;
    int bpf = ch * bits / 8;
    unsigned char *p = (unsigned char *)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, frames * bpf);
    double ph = 0, dph = 2 * PI * 1000.0 / rate;
    int click_every = rate / 4;                /* 4 clicks per second */
    for (i = 0; i < frames; i++) {
        switch (kind) {
        case STIM_CLICK:      v = (i % click_every == 0) ? 29000 : 0; break;
        case STIM_NOISE:      v = noise16() / 2; break;
        case STIM_TONE:       v = (int)(12000 * dsin(ph)); ph += dph; if (ph > 2 * PI) ph -= 2 * PI; break;
        case STIM_CLICKNOISE: v = (i % rate == 0) ? 29000 : ((i % rate) > rate / 2 ? noise16() / 2 : 0); break;
        case STIM_CLICK_L: case STIM_CLICK_R: v = (i % click_every == 0) ? 29000 : 0; break;
        case STIM_TONE250:    v = (int)(12000 * dsin(ph)); ph += dph / 4; if (ph > 2 * PI) ph -= 2 * PI; break;
        }
        for (c = 0; c < ch; c++) {
            int vc = (ch == 2 && c == 1 && kind == STIM_TONE) ? v / 2 : v;   /* right channel -6 dB, so L/R are distinguishable */
            if (ch == 2 && ((kind == STIM_CLICK_L && c == 1) || (kind == STIM_CLICK_R && c == 0))) vc = 0;
            if (bits == 16) { short sv = (short)vc; memcpy(p + i * bpf + c * 2, &sv, 2); }
            else p[i * bpf + c] = (unsigned char)((vc >> 8) + 128);
        }
    }
    wf->wFormatTag = WAVE_FORMAT_PCM; wf->nChannels = (WORD)ch; wf->nSamplesPerSec = rate;
    wf->wBitsPerSample = (WORD)bits; wf->nBlockAlign = (WORD)bpf; wf->nAvgBytesPerSec = rate * bpf; wf->cbSize = 0;
    *bytes = frames * bpf;
    return p;
}

static LPA3DSOURCE new_source(int native, int kind, int rate, int ch, int bits, double secs) {
    LPA3DSOURCE s = NULL; WAVEFORMATEX wf; DWORD bytes, n1, n2; void *p1, *p2; void *data;
    HRESULT hr = IA3d4_NewSource(a3d, native ? A3DSOURCE_INITIAL_RENDERMODE_NATIVE : A3DSOURCE_INITIAL_RENDERMODE_A3D, &s);
    if (FAILED(hr)) { L_begin(); L_s("ERROR NewSource "); L_x(hr); L_end(); return NULL; }
    data = make_wave(kind, rate, ch, bits, secs, &bytes, &wf);
    IA3dSource_SetWaveFormat(s, &wf);
    hr = IA3dSource_AllocateWaveData(s, (INT)bytes);
    if (FAILED(hr)) { L_begin(); L_s("ERROR AllocateWaveData "); L_x(hr); L_end(); }
    if (SUCCEEDED(IA3dSource_Lock(s, 0, bytes, &p1, &n1, &p2, &n2, A3D_ENTIREBUFFER))) {
        memcpy(p1, data, n1); if (p2 && n2) memcpy(p2, (char *)data + n1, n2);
        IA3dSource_Unlock(s, p1, n1, p2, n2);
    } else logmsg("ERROR Lock");
    GlobalFree(data);
    if (!native) IA3dSource_SetMinMaxDistance(s, 1.0f, 100.0f, A3D_AUDIBLE);
    return s;
}

static void kill_source(SRC *s) { if (s->src) { IA3dSource_Stop(s->src); IA3dSource_Release(s->src); s->src = NULL; } }

static void step_log(const char *test, int step, SRC *s, const char *extra_k, double extra_v, int dec) {
    L_begin(); L_s("STEP "); L_s(test); L_s(" n="); L_i(step);
    if (s) { L_s(" x="); L_f(s->x, 3); L_s(" y="); L_f(s->y, 3); L_s(" z="); L_f(s->z, 3); }
    if (extra_k) { L_s(" "); L_s(extra_k); L_s("="); L_f(extra_v, dec); }
    L_end();
}

#define HOLD_MS 2000

/* ------------------------------------------------------------------ tests */
static void t_sync(void) {     /* 3 non-3D clicks 0.5 s apart: aligns log, trace and audio */
    SRC s = {0}; int i;
    logmsg("TEST sync begin");
    s.src = new_source(1, STIM_CLICK, 48000, 1, 16, 0.05);
    for (i = 0; i < 3; i++) {
        IA3dSource_Rewind(s.src); IA3dSource_Play(s.src, A3D_SINGLE);
        step_log("sync", i, NULL, NULL, 0, 0);
        hold(&s, 0, 500);
    }
    kill_source(&s);
    logmsg("TEST sync end");
}

static void t_formats(void) {  /* non-3D playback at each rate/format: converter ratios and DMA format codes */
    static const int rates[] = { 8000, 11025, 22050, 44100, 48000 };
    static const int fmts[][2] = { {1, 16}, {2, 16}, {1, 8}, {2, 8} };
    int r, f, n = 0; SRC s = {0};
    logmsg("TEST formats begin");
    for (r = 0; r < 5; r++) for (f = 0; f < 4; f++) {
        s.src = new_source(1, STIM_TONE, rates[r], fmts[f][0], fmts[f][1], 2.0);
        if (!s.src) continue;
        IA3dSource_Play(s.src, A3D_LOOPED);
        L_begin(); L_s("STEP formats n="); L_i(n++); L_s(" rate="); L_i(rates[r]); L_s(" ch="); L_i(fmts[f][0]); L_s(" bits="); L_i(fmts[f][1]); L_end();
        hold(&s, 0, 1500);
        kill_source(&s);
        hold(&s, 0, 300);
    }
    logmsg("TEST formats end");
}

static void t_azimuth(void) {
    SRC s = {0}; int a, n = 0;
    logmsg("TEST azimuth begin (click, r=1 m, elevation 0)");
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (a = 0; a < 360; a += 15) { place(&s, a, 0, 1.0); step_log("azimuth", n++, &s, "az", a, 0); hold(&s, 1, HOLD_MS); }
    kill_source(&s);
    logmsg("TEST azimuth end");
}

static void t_elevation(void) {
    SRC s = {0}; int e, n = 0, ai; static const int az[] = { 0, 90 };
    logmsg("TEST elevation begin (click, r=1 m)");
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (ai = 0; ai < 2; ai++)
        for (e = -45; e <= 90; e += 15) {
            place(&s, az[ai], e, 1.0);
            L_begin(); L_s("STEP elevation n="); L_i(n++); L_s(" az="); L_i(az[ai]); L_s(" el="); L_i(e); L_end();
            hold(&s, 1, HOLD_MS);
        }
    kill_source(&s);
    logmsg("TEST elevation end");
}

static void t_distance(void) {
    SRC s = {0}; int ai, d, n = 0; static const int az[] = { 0, 90 };
    static const double dist[] = { 0.5, 1, 2, 4, 8, 16, 32, 64 };
    logmsg("TEST distance begin (noise, min 1 m, max 100 m)");
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (ai = 0; ai < 2; ai++) for (d = 0; d < 8; d++) {
        place(&s, az[ai], 0, dist[d]);
        L_begin(); L_s("STEP distance n="); L_i(n++); L_s(" az="); L_i(az[ai]); L_s(" r="); L_f(dist[d], 1); L_end();
        hold(&s, 1, HOLD_MS);
    }
    kill_source(&s);
    logmsg("TEST distance end");
}

static void t_gain(void) {
    SRC s = {0}; int g, ai, n = 0; static const int az[] = { 0, 90 };
    static const double gains[] = { 1.0, 0.5, 0.25, 0.125, 0.0625, 0.0 };
    logmsg("TEST gain begin (noise, r=1 m)");
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (ai = 0; ai < 2; ai++) for (g = 0; g < 6; g++) {
        place(&s, az[ai], 0, 1.0);
        IA3dSource_SetGain(s.src, (A3DVAL)gains[g]);
        L_begin(); L_s("STEP gain n="); L_i(n++); L_s(" az="); L_i(az[ai]); L_s(" gain="); L_f(gains[g], 4); L_end();
        hold(&s, 1, HOLD_MS);
    }
    IA3dSource_SetGain(s.src, 1.0f);
    kill_source(&s);
    logmsg("TEST gain end");
}

static void t_eq(void) {
    SRC s = {0}; int e, n = 0;
    static const double eqs[] = { 1.0, 0.9, 0.75, 0.5, 0.3, 0.1, 0.0 };
    logmsg("TEST eq begin (noise, front, r=1 m)");
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 1.0);
    for (e = 0; e < 7; e++) {
        IA3dSource_SetEq(s.src, (A3DVAL)eqs[e]);
        step_log("eq", n++, &s, "eq", eqs[e], 2);
        hold(&s, 1, HOLD_MS);
    }
    kill_source(&s);
    logmsg("TEST eq end");
}

static void t_doppler(void) {
    SRC s = {0}; int v, n = 0;
    static const double vel[] = { 0, 5, 10, 20, 40, -5, -10, -20, -40 };
    logmsg("TEST doppler begin (1 kHz tone, front, r=4 m, velocity along line of sight, + = away)");
    s.src = new_source(0, STIM_TONE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_SetDopplerScale(s.src, 1.0f);
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 4.0);
    for (v = 0; v < 9; v++) {
        IA3dSource_SetVelocity3f(s.src, 0.0f, 0.0f, (A3DVAL)(-vel[v]));   /* -Z is away from the listener */
        step_log("doppler", n++, &s, "v", vel[v], 1);
        hold(&s, 1, HOLD_MS);
    }
    IA3dSource_SetVelocity3f(s.src, 0.0f, 0.0f, 0.0f);
    kill_source(&s);
    logmsg("TEST doppler end");
}

static void t_pitch(void) {
    SRC s = {0}; int p, n = 0; static const double pitch[] = { 1.0, 0.5, 0.75, 1.5, 2.0 };
    logmsg("TEST pitch begin (1 kHz tone, front, r=1 m)");
    s.src = new_source(0, STIM_TONE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 1.0);
    for (p = 0; p < 5; p++) {
        IA3dSource_SetPitch(s.src, (A3DVAL)pitch[p]);
        step_log("pitch", n++, &s, "pitch", pitch[p], 2);
        hold(&s, 1, HOLD_MS);
    }
    kill_source(&s);
    logmsg("TEST pitch end");
}

static void t_occlusion(void) {
    SRC s = {0}; int m, n = 0;
    static const double tr[][2] = { {1, 1}, {0.5, 0.5}, {0.25, 0.25}, {0.1, 0.1}, {0.5, 0.1}, {0.1, 0.5}, {0, 0}, {1, 1} };
    logmsg("TEST occlusion begin (noise, front, r=4 m, 8x8 m wall at 2 m)");
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dGeom_Enable(geom, A3D_OCCLUSIONS);
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 4.0);
    g_wall = WALL_NONE; step_log("occlusion", n++, &s, "wall", 0, 0); hold(&s, 1, HOLD_MS);
    g_wall = WALL_OCCLUDE;
    for (m = 0; m < 8; m++) {
        IA3dMaterial_SetTransmittance(mat, (A3DVAL)tr[m][0], (A3DVAL)tr[m][1]);
        L_begin(); L_s("STEP occlusion n="); L_i(n++); L_s(" wall=1 trans_low="); L_f(tr[m][0], 2); L_s(" trans_high="); L_f(tr[m][1], 2); L_end();
        hold(&s, 1, HOLD_MS);
    }
    g_wall = WALL_NONE;
    IA3dGeom_Disable(geom, A3D_OCCLUSIONS);
    kill_source(&s);
    logmsg("TEST occlusion end");
}

static void t_reflections(void) {
    SRC s = {0}; int i, n = 0;
    static const double dscale[] = { 1.0, 0.5, 2.0, 4.0 };
    static const double gscale[] = { 1.0, 0.5, 2.0 };
    static const double maxdel[] = { 0.05, 0.1, 0.3, 1.0 };
    logmsg("TEST reflections begin (click, front, r=4 m, reflective wall 2 m to the right)");
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dMaterial_SetTransmittance(mat, 0.0f, 0.0f);
    IA3dMaterial_SetReflectance(mat, 1.0f, 1.0f);
    IA3dGeom_Enable(geom, A3D_1ST_REFLECTIONS);
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 4.0);
    g_wall = WALL_NONE; step_log("reflections", n++, &s, "wall", 0, 0); hold(&s, 1, HOLD_MS);
    g_wall = WALL_REFLECT; step_log("reflections", n++, &s, "wall", 1, 0); hold(&s, 1, HOLD_MS);
    for (i = 0; i < 4; i++) { IA3dGeom_SetReflectionDelayScale(geom, (A3DVAL)dscale[i]); step_log("reflections", n++, &s, "delay_scale", dscale[i], 2); hold(&s, 1, HOLD_MS); }
    IA3dGeom_SetReflectionDelayScale(geom, 1.0f);
    for (i = 0; i < 3; i++) { IA3dGeom_SetReflectionGainScale(geom, (A3DVAL)gscale[i]); step_log("reflections", n++, &s, "gain_scale", gscale[i], 2); hold(&s, 1, HOLD_MS); }
    IA3dGeom_SetReflectionGainScale(geom, 1.0f);
    for (i = 0; i < 4; i++) { IA3d4_SetMaxReflectionDelayTime(a3d, (A3DVAL)maxdel[i]); step_log("reflections", n++, &s, "max_delay", maxdel[i], 3); hold(&s, 1, HOLD_MS); }
    IA3dMaterial_SetReflectance(mat, 0.5f, 0.1f); step_log("reflections", n++, &s, "reflectance_low0.5_high", 0.1, 2); hold(&s, 1, HOLD_MS);
    g_wall = WALL_NONE;
    IA3dGeom_Disable(geom, A3D_1ST_REFLECTIONS);
    kill_source(&s);
    logmsg("TEST reflections end");
}

static void t_glide(void) {     /* abrupt parameter jumps: measures how fast current values chase targets */
    SRC s = {0}; int i, n = 0;
    logmsg("TEST glide begin (noise, r=1 m; abrupt jumps, 3 s holds)");
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (i = 0; i < 4; i++) { place(&s, (i & 1) ? -90 : 90, 0, 1.0); step_log("glide", n++, &s, "az", (i & 1) ? -90 : 90, 0); hold(&s, 1, 3000); }
    place(&s, 0, 0, 1.0);
    for (i = 0; i < 4; i++) { IA3dSource_SetGain(s.src, (i & 1) ? 0.1f : 1.0f); step_log("glide", n++, &s, "gain", (i & 1) ? 0.1 : 1.0, 2); hold(&s, 1, 3000); }
    IA3dSource_SetGain(s.src, 1.0f);
    for (i = 0; i < 4; i++) { IA3dSource_SetEq(s.src, (i & 1) ? 0.1f : 1.0f); step_log("glide", n++, &s, "eq", (i & 1) ? 0.1 : 1.0, 2); hold(&s, 1, 3000); }
    kill_source(&s);
    logmsg("TEST glide end");
}


/* ================================================================== run 2 tests */
static DWORD g_om0 = OUTPUT_MODE_STEREO, g_fx0 = OUTPUT_SPEAKERS_WIDE, g_rx0 = OUTPUT_SPEAKERS_WIDE;
static int g_om_saved;

static void set_output(DWORD xt, const char *name) {
    HRESULT hr = IA3d4_SetOutputMode(a3d, OUTPUT_MODE_STEREO, xt, xt);
    L_begin(); L_s("OUTPUT "); L_s(name); L_s(" hr="); L_x(hr); L_end();
    hold(NULL, 0, 400);
}
static void headphones(void) { set_output(OUTPUT_HEADPHONES, "headphones"); }

static void L_kv(const char *k, double v, int d) { L_s(" "); L_s(k); L_s("="); L_f(v, d); }

/* crosstalk: the same click positions under each output mode */
static void t_xtalk(void) {
    static const DWORD modes[] = { OUTPUT_HEADPHONES, OUTPUT_SPEAKERS_WIDE, OUTPUT_SPEAKERS_NARROW };
    static const char *names[] = { "headphones", "speakers_wide", "speakers_narrow" };
    static const int az[] = { 0, 30, 60, 90, 135, 180, 270 };
    int m, i, n = 0; SRC s = {0};
    logmsg("TEST xtalk begin (click, r=1 m, each output mode)");
    for (m = 0; m < 3; m++) {
        set_output(modes[m], names[m]);
        s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
        if (!s.src) return;
        IA3dSource_Play(s.src, A3D_LOOPED);
        for (i = 0; i < 7; i++) {
            place(&s, az[i], 0, 1.0);
            L_begin(); L_s("STEP xtalk n="); L_i(n++); L_s(" mode="); L_s(names[m]); L_kv("az", az[i], 0); L_end();
            hold(&s, 1, 2000);
        }
        kill_source(&s);
    }
    logmsg("TEST xtalk end");
}

/* references: native clicks per channel, and 3D clicks in headphone mode */
static void t_ref(void) {
    SRC s = {0}; int n = 0;
    static const int kinds[] = { STIM_CLICK_L, STIM_CLICK_R, STIM_CLICK };
    static const char *kn[] = { "native_stereo_L_only", "native_stereo_R_only", "native_stereo_both" };
    int k;
    logmsg("TEST ref begin (48 kHz clicks, 29000 peak)");
    headphones();
    for (k = 0; k < 3; k++) {
        s.src = new_source(1, kinds[k], 48000, 2, 16, 1.0);
        if (!s.src) continue;
        IA3dSource_Play(s.src, A3D_LOOPED);
        L_begin(); L_s("STEP ref n="); L_i(n++); L_s(" kind="); L_s(kn[k]); L_end();
        hold(&s, 0, 2000); kill_source(&s); hold(NULL, 0, 200);
    }
    s.src = new_source(1, STIM_CLICK, 48000, 1, 16, 1.0);
    if (s.src) {
        IA3dSource_Play(s.src, A3D_LOOPED);
        L_begin(); L_s("STEP ref n="); L_i(n++); L_s(" kind=native_mono"); L_end();
        hold(&s, 0, 2000); kill_source(&s); hold(NULL, 0, 200);
    }
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (s.src) {
        IA3dSource_Play(s.src, A3D_LOOPED);
        place(&s, 0, 0, 1.0);
        L_begin(); L_s("STEP ref n="); L_i(n++); L_s(" kind=a3d_front gain=1.0"); L_end();
        hold(&s, 1, 2000);
        IA3dSource_SetGain(s.src, 0.5f);
        L_begin(); L_s("STEP ref n="); L_i(n++); L_s(" kind=a3d_front gain=0.5"); L_end();
        hold(&s, 1, 2000);
        kill_source(&s);
    }
    logmsg("TEST ref end");
}

/* full direction map: HRTF taps and ITD for every 5 degrees of azimuth and 15 of elevation */
static void t_hrtfmap(void) {
    SRC s = {0}; int az, el, n = 0;
    logmsg("TEST hrtfmap begin (click, r=1 m, 200 ms per position)");
    headphones();
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (el = -45; el <= 90; el += 15) {
        for (az = 0; az < 360; az += 5) {
            if (el == 90 && az > 0) break;
            place(&s, az, el, 1.0);
            L_begin(); L_s("STEP hrtfmap n="); L_i(n++); L_kv("az", az, 0); L_kv("el", el, 0); L_end();
            hold(&s, 1, 200);
        }
    }
    for (az = 170; az <= 190; az++) {          /* the odd entry directly behind */
        place(&s, az, 0, 1.0);
        L_begin(); L_s("STEP hrtfmap n="); L_i(n++); L_kv("az", az, 0); L_kv("el", 0, 0); L_end();
        hold(&s, 1, 200);
    }
    kill_source(&s);
    logmsg("TEST hrtfmap end");
}

/* glides, one parameter at a time, headphone mode */
static void t_glide2(void) {
    SRC s = {0}; int i, n = 0;
    logmsg("TEST glide2 begin (headphones, 2.5 s holds)");
    headphones();
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (i = 0; i < 4; i++) {                  /* elevation only: taps change, ITD stays 0 */
        int el = (i & 1) ? 60 : -45;
        place(&s, 0, el, 1.0);
        L_begin(); L_s("STEP glide2 n="); L_i(n++); L_s(" what=elevation"); L_kv("el", el, 0); L_end();
        hold(&s, 1, 2500);
    }
    place(&s, 0, 0, 1.0);
    for (i = 0; i < 4; i++) { double g = (i & 1) ? 0.5 : 1.0; IA3dSource_SetGain(s.src, (A3DVAL)g);
        L_begin(); L_s("STEP glide2 n="); L_i(n++); L_s(" what=gain"); L_kv("gain", g, 2); L_end(); hold(&s, 1, 2500); }
    IA3dSource_SetGain(s.src, 1.0f);
    for (i = 0; i < 4; i++) { double e = (i & 1) ? 0.5 : 1.0; IA3dSource_SetEq(s.src, (A3DVAL)e);
        L_begin(); L_s("STEP glide2 n="); L_i(n++); L_s(" what=eq"); L_kv("eq", e, 2); L_end(); hold(&s, 1, 2500); }
    IA3dSource_SetEq(s.src, 1.0f);
    for (i = 0; i < 4; i++) { double r = (i & 1) ? 8.0 : 1.0; place(&s, 0, 0, r);
        L_begin(); L_s("STEP glide2 n="); L_i(n++); L_s(" what=distance"); L_kv("r", r, 1); L_end(); hold(&s, 1, 2500); }
    kill_source(&s);
    s.src = new_source(0, STIM_TONE250, 48000, 1, 16, 2.0);   /* 250 Hz: phase tracks the ITD without wrapping */
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (i = 0; i < 4; i++) { int a = (i & 1) ? 270 : 90; place(&s, a, 0, 1.0);
        L_begin(); L_s("STEP glide2 n="); L_i(n++); L_s(" what=azimuth_tone250"); L_kv("az", a, 0); L_end(); hold(&s, 1, 2500); }
    kill_source(&s);
    logmsg("TEST glide2 end");
}

/* reflections: reflectance bands, wall distance and direction, several walls */
static void t_reflect2(void) {
    SRC s = {0}; int i, n = 0;
    static const double rl[][2] = { {1, 1}, {0.5, 1}, {0.25, 1}, {1, 0.5}, {1, 0.25}, {1, 0.1}, {1, 1} };
    static const double dist[] = { 1.0, 4.0, 8.0 };
    static const struct { int w; const char *name; } walls[] = {
        { WALL_LEFT, "left" }, { WALL_BACK, "back" }, { WALL_LEFT | WALL_RIGHT, "left+right" }, { WALL_LEFT | WALL_RIGHT | WALL_BACK, "left+right+back" } };
    logmsg("TEST reflect2 begin (headphones, click, front, r=4 m)");
    headphones();
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dMaterial_SetTransmittance(mat, 0.0f, 0.0f);
    IA3dGeom_Enable(geom, A3D_1ST_REFLECTIONS);
    IA3dGeom_SetReflectionDelayScale(geom, 1.0f); IA3dGeom_SetReflectionGainScale(geom, 1.0f);
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 4.0);
    g_wall = WALL_NONE; g_wall_d = 2.0;
    L_begin(); L_s("STEP reflect2 n="); L_i(n++); L_s(" walls=none"); L_end(); hold(&s, 1, 2000);
    g_wall = WALL_RIGHT;
    for (i = 0; i < 7; i++) {
        IA3dMaterial_SetReflectance(mat, (A3DVAL)rl[i][0], (A3DVAL)rl[i][1]);
        L_begin(); L_s("STEP reflect2 n="); L_i(n++); L_s(" walls=right d=2"); L_kv("refl_low", rl[i][0], 2); L_kv("refl_high", rl[i][1], 2); L_end();
        hold(&s, 1, 2000);
    }
    for (i = 0; i < 3; i++) {
        g_wall_d = dist[i];
        L_begin(); L_s("STEP reflect2 n="); L_i(n++); L_s(" walls=right"); L_kv("d", dist[i], 1); L_end();
        hold(&s, 1, 2000);
    }
    g_wall_d = 2.0;
    for (i = 0; i < 4; i++) {
        g_wall = walls[i].w;
        L_begin(); L_s("STEP reflect2 n="); L_i(n++); L_s(" walls="); L_s(walls[i].name); L_s(" d=2"); L_end();
        hold(&s, 1, 2000);
    }
    g_wall = WALL_NONE;
    IA3dGeom_Disable(geom, A3D_1ST_REFLECTIONS);
    IA3dMaterial_SetTransmittance(mat, 1.0f, 1.0f);
    IA3dMaterial_SetReflectance(mat, 0.0f, 0.0f);
    kill_source(&s);
    logmsg("TEST reflect2 end");
}

/* several sources at once: which slots the driver uses */
static void t_multi(void) {
    SRC s[16]; int i, n = 0;
    memset(s, 0, sizeof s);
    logmsg("TEST multi begin (noise sources added one per second, gain 0.15)");
    headphones();
    for (i = 0; i < 16; i++) {
        s[i].src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
        if (!s[i].src) { L_begin(); L_s("multi: source "); L_i(i); L_s(" failed"); L_end(); break; }
        IA3dSource_SetGain(s[i].src, 0.15f);
        place(&s[i], i * 22.5, 0, 1.0);
        IA3dSource_Play(s[i].src, A3D_LOOPED);
        L_begin(); L_s("STEP multi n="); L_i(n++); L_s(" add="); L_i(i); L_kv("az", i * 22.5, 1); L_end();
        hold(s, i + 1, 1000);
    }
    for (i = 15; i >= 0; i--) {
        if (!s[i].src) continue;
        kill_source(&s[i]);
        L_begin(); L_s("STEP multi n="); L_i(n++); L_s(" remove="); L_i(i); L_end();
        hold(s, 16, 500);
    }
    logmsg("TEST multi end");
}

/* DirectSound hardware buffers: formats, volume, pan, frequency (the non-A3D game path) */
typedef HRESULT (WINAPI *PDSCREATE)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
static void ds_wait(int ms) { double t = now_s() + ms / 1000.0; while (now_s() < t) { pump(); Sleep(20); } }

static LPDIRECTSOUNDBUFFER ds_buffer(LPDIRECTSOUND ds, int rate, int ch, int bits, DWORD *hwflag) {
    DSBUFFERDESC1 d; WAVEFORMATEX wf; DWORD bytes; void *data, *p1, *p2; DWORD n1, n2;
    LPDIRECTSOUNDBUFFER b = NULL; HRESULT hr;
    data = make_wave(STIM_TONE, rate, ch, bits, 1.0, &bytes, &wf);
    memset(&d, 0, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DSBCAPS_LOCHARDWARE | DSBCAPS_STATIC | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2;
    d.dwBufferBytes = bytes; d.lpwfxFormat = &wf;
    hr = IDirectSound_CreateSoundBuffer(ds, (LPCDSBUFFERDESC)&d, &b, NULL);
    *hwflag = 1;
    if (FAILED(hr)) {
        L_begin(); L_s("dsound hardware buffer failed "); L_x(hr); L_s(", retrying in software"); L_end();
        d.dwFlags &= ~DSBCAPS_LOCHARDWARE; *hwflag = 0;
        hr = IDirectSound_CreateSoundBuffer(ds, (LPCDSBUFFERDESC)&d, &b, NULL);
        if (FAILED(hr)) { L_begin(); L_s("dsound buffer failed "); L_x(hr); L_end(); GlobalFree(data); return NULL; }
    }
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, bytes, &p1, &n1, &p2, &n2, 0))) {
        memcpy(p1, data, n1); if (p2 && n2) memcpy(p2, (char *)data + n1, n2);
        IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    }
    GlobalFree(data);
    return b;
}

static void t_dsound(void) {
    static const int rates[] = { 8000, 11025, 22050, 44100, 48000 };
    static const int fmts[][2] = { {1, 16}, {2, 16}, {1, 8}, {2, 8} };
    static const long vols[] = { 0, -300, -600, -1200, -2400, -4800, -10000 };
    static const long pans[] = { -10000, -2400, -600, 0, 600, 2400, 10000 };
    static const DWORD freqs[] = { 48000, 24000, 32000, 44100, 96000 };
    HMODULE dll; PDSCREATE create; LPDIRECTSOUND ds = NULL; LPDIRECTSOUNDBUFFER b; HRESULT hr; DWORD hw;
    int r, f, i, n = 0;
    logmsg("TEST dsound begin (DirectSound buffers, 1 kHz tone)");
    dll = LoadLibraryA("dsound.dll");
    create = dll ? (PDSCREATE)GetProcAddress(dll, "DirectSoundCreate") : NULL;
    if (!create) { logmsg("ERROR dsound.dll / DirectSoundCreate not found"); return; }
    hr = create(NULL, &ds, NULL);
    if (FAILED(hr)) { L_begin(); L_s("ERROR DirectSoundCreate "); L_x(hr); L_end(); return; }
    hr = IDirectSound_SetCooperativeLevel(ds, g_wnd, DSSCL_PRIORITY);
    L_begin(); L_s("dsound cooperative level hr="); L_x(hr); L_end();
    for (r = 0; r < 5; r++) for (f = 0; f < 4; f++) {
        b = ds_buffer(ds, rates[r], fmts[f][0], fmts[f][1], &hw);
        if (!b) continue;
        IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
        L_begin(); L_s("STEP dsound n="); L_i(n++); L_s(" what=format rate="); L_i(rates[r]); L_s(" ch="); L_i(fmts[f][0]); L_s(" bits="); L_i(fmts[f][1]); L_s(" hw="); L_i(hw); L_end();
        ds_wait(1000);
        IDirectSoundBuffer_Stop(b); IDirectSoundBuffer_Release(b); ds_wait(200);
    }
    b = ds_buffer(ds, 48000, 1, 16, &hw);
    if (b) {
        IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
        for (i = 0; i < 7; i++) { IDirectSoundBuffer_SetVolume(b, vols[i]);
            L_begin(); L_s("STEP dsound n="); L_i(n++); L_s(" what=volume mb="); L_i(vols[i]); L_end(); ds_wait(1000); }
        IDirectSoundBuffer_SetVolume(b, 0);
        for (i = 0; i < 7; i++) { IDirectSoundBuffer_SetPan(b, pans[i]);
            L_begin(); L_s("STEP dsound n="); L_i(n++); L_s(" what=pan mb="); L_i(pans[i]); L_end(); ds_wait(1000); }
        IDirectSoundBuffer_SetPan(b, 0);
        for (i = 0; i < 5; i++) { hr = IDirectSoundBuffer_SetFrequency(b, freqs[i]);
            L_begin(); L_s("STEP dsound n="); L_i(n++); L_s(" what=frequency hz="); L_i((long)freqs[i]); L_s(" hr="); L_x(hr); L_end(); ds_wait(1000); }
        IDirectSoundBuffer_Stop(b); IDirectSoundBuffer_Release(b);
    }
    IDirectSound_Release(ds);
    logmsg("TEST dsound end");
}


/* ================================================================== run 3 tests */
/* elevation in 5-degree steps at four azimuths: the driver's elevation grid */
static void t_elev5(void) {
    SRC s = {0}; int ai, el, n = 0; static const int az[] = { 0, 45, 90, 135 };
    logmsg("TEST elev5 begin (click, r=1 m, 200 ms per position)");
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (ai = 0; ai < 4; ai++) for (el = -45; el <= 90; el += 5) {
        place(&s, az[ai], el, 1.0);
        L_begin(); L_s("STEP elev5 n="); L_i(n++); L_kv("az", az[ai], 0); L_kv("el", el, 0); L_end();
        hold(&s, 1, 200);
    }
    kill_source(&s);
    logmsg("TEST elev5 end");
}

/* one long-lived source with a fixed 9 s cycle of abrupt changes. The host script
   (vortex-poke.py tcsweep) rewrites the per-source time-constant words between cycles;
   the source is never restarted, so the driver does not overwrite them. */
#define TC_CYCLES 36
static void t_tcsweep(void) {
    SRC s = {0}; int c, n = 0;
    logmsg("TEST tcsweep begin (noise, r=1 m, 9 s cycles: gain 0.1, gain 1, eq 0.1, eq 1, az 90, az 270)");
    s.src = new_source(0, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 1.0); hold(&s, 1, 1500);
    for (c = 0; c < TC_CYCLES; c++) {
        IA3dSource_SetGain(s.src, 0.1f);
        L_begin(); L_s("STEP tcsweep n="); L_i(n++); L_s(" cycle="); L_i(c); L_s(" what=gain v=0.1"); L_end(); hold(&s, 1, 1500);
        IA3dSource_SetGain(s.src, 1.0f);
        L_begin(); L_s("STEP tcsweep n="); L_i(n++); L_s(" cycle="); L_i(c); L_s(" what=gain v=1.0"); L_end(); hold(&s, 1, 1500);
        IA3dSource_SetEq(s.src, 0.1f);
        L_begin(); L_s("STEP tcsweep n="); L_i(n++); L_s(" cycle="); L_i(c); L_s(" what=eq v=0.1"); L_end(); hold(&s, 1, 1500);
        IA3dSource_SetEq(s.src, 1.0f);
        L_begin(); L_s("STEP tcsweep n="); L_i(n++); L_s(" cycle="); L_i(c); L_s(" what=eq v=1.0"); L_end(); hold(&s, 1, 1500);
        place(&s, 90, 0, 1.0);
        L_begin(); L_s("STEP tcsweep n="); L_i(n++); L_s(" cycle="); L_i(c); L_s(" what=az v=90"); L_end(); hold(&s, 1, 1500);
        place(&s, 270, 0, 1.0);
        L_begin(); L_s("STEP tcsweep n="); L_i(n++); L_s(" cycle="); L_i(c); L_s(" what=az v=270"); L_end(); hold(&s, 1, 1500);
        place(&s, 0, 0, 1.0);
    }
    kill_source(&s);
    logmsg("TEST tcsweep end");
}


/* a click source held at 90 degrees for 150 s, for host-side canceller experiments
   (vortex-poke.py xtsweep rewrites the crosstalk canceller while this plays) */
static void t_click90(void) {
    SRC s = {0};
    logmsg("TEST click90 begin (click, az 90, r=1 m, 150 s)");
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 90, 0, 1.0);
    L_begin(); L_s("STEP click90 n=0"); L_kv("az", 90, 0); L_end();
    hold(&s, 1, 150000);
    kill_source(&s);
    logmsg("TEST click90 end");
}


/* plain (non-3D) noise for 120 s: the 2D mix passes the chip's EQ block (vortex-poke.py eqsweep) */
static void t_noise2d(void) {
    SRC s = {0};
    logmsg("TEST noise2d begin (native noise, 120 s)");
    s.src = new_source(1, STIM_NOISE, 48000, 1, 16, 2.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    L_begin(); L_s("STEP noise2d n=0"); L_end();
    hold(&s, 0, 120000);
    kill_source(&s);
    logmsg("TEST noise2d end");
}


/* wtpath: which sounds the driver puts on wavetable voices.
   Part A: one click source with a reflective wall at 48, 22.05 and 11.025 kHz, then at pitch 0.5;
           the reflection voice's rate field shows whether reflections run at the source's rate.
   Part B: 16 quiet noise sources fill the A3D slots, then extra click sources are added; an
           extra source is moved left/right and away to see whether overflow voices are panned
           and attenuated. Azimuths avoid 90 degrees so vortex-poke.py xtident waits for click90. */
static void t_wtpath(void) {
    static const int rates[] = { 48000, 22050, 11025 };
    SRC s = {0}, q[16], x[8]; int i, r, n = 0;
    logmsg("TEST wtpath begin");
    IA3dMaterial_SetTransmittance(mat, 0.0f, 0.0f);
    IA3dMaterial_SetReflectance(mat, 1.0f, 1.0f);
    IA3dGeom_SetReflectionDelayScale(geom, 1.0f); IA3dGeom_SetReflectionGainScale(geom, 1.0f);
    IA3dGeom_Enable(geom, A3D_1ST_REFLECTIONS);
    g_wall_d = 2.0;
    for (r = 0; r < 3; r++) {
        s.src = new_source(0, STIM_CLICK, rates[r], 1, 16, 1.0);
        if (!s.src) continue;
        IA3dSource_Play(s.src, A3D_LOOPED);
        place(&s, 0, 0, 4.0);
        g_wall = WALL_NONE;
        L_begin(); L_s("STEP wtpath n="); L_i(n++); L_kv("rate", rates[r], 0); L_s(" wall=none"); L_end();
        hold(&s, 1, 3000);
        g_wall = WALL_RIGHT;
        L_begin(); L_s("STEP wtpath n="); L_i(n++); L_kv("rate", rates[r], 0); L_s(" wall=right"); L_end();
        hold(&s, 1, 3000);
        if (r == 2) {
            IA3dSource_SetPitch(s.src, 0.5f);
            L_begin(); L_s("STEP wtpath n="); L_i(n++); L_kv("rate", rates[r], 0); L_s(" wall=right"); L_kv("pitch", 0.5, 2); L_end();
            hold(&s, 1, 3000);
        }
        g_wall = WALL_NONE;
        kill_source(&s);
        hold(NULL, 0, 500);
    }
    IA3dGeom_Disable(geom, A3D_1ST_REFLECTIONS);
    IA3dMaterial_SetTransmittance(mat, 1.0f, 1.0f);
    IA3dMaterial_SetReflectance(mat, 0.0f, 0.0f);

    memset(q, 0, sizeof q); memset(x, 0, sizeof x);
    for (i = 0; i < 16; i++) {                       /* quiet bed that occupies every A3D slot */
        q[i].src = new_source(0, STIM_NOISE, 22050, 1, 16, 2.0);
        if (!q[i].src) break;
        IA3dSource_SetGain(q[i].src, 0.03f);
        place(&q[i], i * 22.5 + 11.25, 0, 1.0);
        IA3dSource_Play(q[i].src, A3D_LOOPED);
        hold(q, i + 1, 250);
    }
    L_begin(); L_s("STEP wtpath n="); L_i(n++); L_s(" bed=16"); L_end();
    hold(q, 16, 2000);
    {   /* one extra source: left, right, front, then farther away */
        SRC all[17]; static const double az[] = { 300, 60, 0 }; static const double rr[] = { 2.0, 4.0, 8.0 };
        x[0].src = new_source(0, STIM_CLICK, 22050, 1, 16, 1.0);
        if (x[0].src) {
            IA3dSource_SetGain(x[0].src, 0.5f);
            IA3dSource_Play(x[0].src, A3D_LOOPED);
            for (i = 0; i < 3; i++) {
                place(&x[0], az[i], 0, 1.0);
                memcpy(all, q, sizeof q); all[16] = x[0];
                L_begin(); L_s("STEP wtpath n="); L_i(n++); L_s(" extra=1"); L_kv("az", az[i], 0); L_kv("r", 1.0, 1); L_end();
                hold(all, 17, 4000);
            }
            for (i = 0; i < 3; i++) {
                place(&x[0], 0, 0, rr[i]);
                memcpy(all, q, sizeof q); all[16] = x[0];
                L_begin(); L_s("STEP wtpath n="); L_i(n++); L_s(" extra=1"); L_kv("az", 0, 0); L_kv("r", rr[i], 1); L_end();
                hold(all, 17, 3000);
            }
            kill_source(&x[0]);
        }
    }
    {   /* eight extra sources, one per second, then removed */
        SRC all[24];
        for (i = 0; i < 8; i++) {
            x[i].src = new_source(0, STIM_CLICK, 22050, 1, 16, 1.0);
            if (!x[i].src) break;
            IA3dSource_SetGain(x[i].src, 0.3f);
            place(&x[i], i * 45 + 22.5, 0, 1.0);
            IA3dSource_Play(x[i].src, A3D_LOOPED);
            memcpy(all, q, sizeof q); memcpy(all + 16, x, sizeof x);
            L_begin(); L_s("STEP wtpath n="); L_i(n++); L_s(" extras="); L_i(i + 1); L_kv("az", i * 45 + 22.5, 1); L_end();
            hold(all, 16 + i + 1, 1000);
        }
        memcpy(all, q, sizeof q); memcpy(all + 16, x, sizeof x);
        hold(all, 24, 2000);
        for (i = 7; i >= 0; i--) kill_source(&x[i]);
    }
    for (i = 15; i >= 0; i--) kill_source(&q[i]);
    L_begin(); L_s("STEP wtpath n="); L_i(n++); L_s(" done"); L_end();
    hold(NULL, 0, 1000);
    logmsg("TEST wtpath end");
}


/* ================================================================== run 12 tests */
static double dexp(double x) {                 /* range-reduced Taylor series */
    int k = 0, i; double r = 1, t = 1;
    while (x > 0.5) { x /= 2; k++; }
    while (x < -0.5) { x /= 2; k++; }
    for (i = 1; i < 20; i++) { t *= x / i; r += t; }
    while (k--) r *= r;
    return r;
}
static double dln(double x) {                  /* ln via atanh series after scaling to [0.75,1.5) */
    double y, y2, t, r = 0; int k = 0, i;
    while (x >= 1.5) { x /= 2; k++; }
    while (x < 0.75) { x *= 2; k--; }
    y = (x - 1) / (x + 1); y2 = y * y; t = y;
    for (i = 1; i < 40; i += 2) { r += t / i; t *= y2; }
    return 2 * r + k * 0.69314718055994531;
}

enum { SIG_NOISE, SIG_SWEEP, SIG_IMPULSE, SIG_TONE };

/* mono 16-bit (or 8-bit) signal for the converter and level tests; amp is linear full scale */
static void *make_sig(int kind, int rate, int bits, double secs, double amp, double hz, DWORD *bytes, WAVEFORMATEX *wf) {
    int frames = (int)(rate * secs), i, v = 0, bpf = bits / 8;
    unsigned char *p = (unsigned char *)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, frames * bpf + 16);
    double ph = 0, f = 20.0, k = 1, a = amp * 32767.0;
    if (kind == SIG_SWEEP) k = dexp(dln((rate * 0.5) / 20.0) / frames);   /* 20 Hz to Nyquist, exponential */
    for (i = 0; i < frames; i++) {
        switch (kind) {
        case SIG_NOISE:   v = (int)(noise16() * 2 * amp); break;
        case SIG_SWEEP:   v = (int)(a * dsin(ph)); ph += 2 * PI * f / rate; f *= k; if (ph > 2 * PI) ph -= 2 * PI; break;
        case SIG_IMPULSE: v = (i % (rate / 4) == rate / 8) ? (int)a : 0; break;   /* 4 single-sample impulses per second */
        case SIG_TONE:    v = (int)(a * dsin(ph)); ph += 2 * PI * hz / rate; if (ph > 2 * PI) ph -= 2 * PI; break;
        }
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        if (bits == 16) { short sv = (short)v; memcpy(p + i * 2, &sv, 2); }
        else p[i] = (unsigned char)((v >> 8) + 128);
    }
    wf->wFormatTag = WAVE_FORMAT_PCM; wf->nChannels = 1; wf->nSamplesPerSec = rate;
    wf->wBitsPerSample = (WORD)bits; wf->nBlockAlign = (WORD)bpf; wf->nAvgBytesPerSec = rate * bpf; wf->cbSize = 0;
    *bytes = frames * bpf;
    return p;
}

static LPDIRECTSOUND g_ds;
static int ds_open(void) {
    HMODULE dll; PDSCREATE create; HRESULT hr;
    if (g_ds) return 1;
    dll = LoadLibraryA("dsound.dll");
    create = dll ? (PDSCREATE)GetProcAddress(dll, "DirectSoundCreate") : NULL;
    if (!create) { logmsg("ERROR dsound.dll / DirectSoundCreate not found"); return 0; }
    hr = create(NULL, &g_ds, NULL);
    if (FAILED(hr)) { L_begin(); L_s("ERROR DirectSoundCreate "); L_x(hr); L_end(); g_ds = NULL; return 0; }
    IDirectSound_SetCooperativeLevel(g_ds, g_wnd, DSSCL_PRIORITY);
    return 1;
}

/* a hardware (or, failing that, software) DirectSound buffer holding the given signal */
static LPDIRECTSOUNDBUFFER ds_sig(int kind, int rate, int bits, double secs, double amp, double hz, DWORD *hw) {
    DSBUFFERDESC1 d; WAVEFORMATEX wf; DWORD bytes, n1, n2; void *data, *p1, *p2; LPDIRECTSOUNDBUFFER b = NULL; HRESULT hr;
    data = make_sig(kind, rate, bits, secs, amp, hz, &bytes, &wf);
    memset(&d, 0, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DSBCAPS_LOCHARDWARE | DSBCAPS_STATIC | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2;
    d.dwBufferBytes = bytes; d.lpwfxFormat = &wf;
    *hw = 1;
    hr = IDirectSound_CreateSoundBuffer(g_ds, (LPCDSBUFFERDESC)&d, &b, NULL);
    if (FAILED(hr)) {
        d.dwFlags &= ~DSBCAPS_LOCHARDWARE; *hw = 0;
        hr = IDirectSound_CreateSoundBuffer(g_ds, (LPCDSBUFFERDESC)&d, &b, NULL);
        if (FAILED(hr)) { L_begin(); L_s("ERROR dsound buffer "); L_x(hr); L_end(); GlobalFree(data); return NULL; }
    }
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, bytes, &p1, &n1, &p2, &n2, 0))) {
        memcpy(p1, data, n1); if (p2 && n2) memcpy(p2, (char *)data + n1, n2);
        IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    }
    GlobalFree(data);
    return b;
}

static void ds_play_once(const char *what, int kind, int rate, int bits, double secs, double amp, double hz, DWORD setfreq, int *n) {
    DWORD hw; LPDIRECTSOUNDBUFFER b = ds_sig(kind, rate, bits, secs, amp, hz, &hw);
    if (!b) return;
    if (setfreq) IDirectSoundBuffer_SetFrequency(b, setfreq);
    L_begin(); L_s("STEP srcresp n="); L_i((*n)++); L_s(" sig="); L_s(what); L_s(" rate="); L_i(rate);
    L_s(" bits="); L_i(bits); if (setfreq) { L_s(" playfreq="); L_i((long)setfreq); }
    L_kv("secs", secs, 1); L_kv("amp", amp, 3); L_s(" hw="); L_i(hw); L_end();
    IDirectSoundBuffer_Play(b, 0, 0, 0);
    ds_wait((int)(secs * 1000 * (setfreq ? (double)rate / setfreq : 1.0)) + 400);
    IDirectSoundBuffer_Stop(b); IDirectSoundBuffer_Release(b);
    ds_wait(300);
}

/* 1. the sample-rate converter: noise, a sweep and impulses at each common rate */
static void t_srcresp(void) {
    static const int rates[] = { 48000, 44100, 32000, 22050, 11025, 8000 };
    int r, n = 0;
    logmsg("TEST srcresp begin (DirectSound, mono, each played once)");
    if (!ds_open()) return;
    for (r = 0; r < 6; r++) {
        ds_play_once("noise",   SIG_NOISE,   rates[r], 16, 4.0, 0.25, 0, 0, &n);
        ds_play_once("sweep",   SIG_SWEEP,   rates[r], 16, 4.0, 0.5,  0, 0, &n);
        ds_play_once("impulse", SIG_IMPULSE, rates[r], 16, 2.0, 0.9,  0, 0, &n);
    }
    ds_play_once("noise", SIG_NOISE, 22050, 8, 4.0, 0.25, 0, 0, &n);      /* 8-bit path */
    ds_play_once("noise", SIG_NOISE, 11025, 8, 4.0, 0.25, 0, 0, &n);
    ds_play_once("sweep", SIG_SWEEP, 22050, 16, 4.0, 0.5, 0, 30000, &n); /* a non-standard ratio */
    ds_play_once("noise", SIG_NOISE, 22050, 16, 4.0, 0.25, 0, 30000, &n);
    logmsg("TEST srcresp end");
}

/* 3. levels: tones at known levels through each path, then summing for headroom */
static void t_level(void) {
    static const double amps[] = { 1.0, 0.5, 0.1 };
    LPDIRECTSOUNDBUFFER b[4]; DWORD hw; int i, n = 0; SRC s = {0};
    logmsg("TEST level begin (1 kHz tones)");
    if (!ds_open()) return;
    for (i = 0; i < 3; i++) {
        b[0] = ds_sig(SIG_TONE, 48000, 16, 1.0, amps[i], 1000, &hw);
        if (!b[0]) return;
        IDirectSoundBuffer_Play(b[0], 0, 0, DSBPLAY_LOOPING);
        L_begin(); L_s("STEP level n="); L_i(n++); L_s(" path=dsound48"); L_kv("amp", amps[i], 2); L_s(" hw="); L_i(hw); L_end();
        ds_wait(2000); IDirectSoundBuffer_Stop(b[0]); IDirectSoundBuffer_Release(b[0]); ds_wait(300);
    }
    b[0] = ds_sig(SIG_TONE, 22050, 16, 1.0, 0.5, 1000, &hw);
    if (b[0]) {
        IDirectSoundBuffer_Play(b[0], 0, 0, DSBPLAY_LOOPING);
        L_begin(); L_s("STEP level n="); L_i(n++); L_s(" path=dsound22"); L_kv("amp", 0.5, 2); L_end();
        ds_wait(2000); IDirectSoundBuffer_Stop(b[0]); IDirectSoundBuffer_Release(b[0]); ds_wait(300);
    }
    /* headroom: 1-4 half-scale tones at different pitches; their peaks add up to 2x full scale */
    for (i = 0; i < 4; i++) b[i] = ds_sig(SIG_TONE, 48000, 16, 1.0, 0.5, 1000 + 250 * i, &hw);
    for (i = 0; i < 4; i++) {
        if (!b[i]) break;
        IDirectSoundBuffer_Play(b[i], 0, 0, DSBPLAY_LOOPING);
        L_begin(); L_s("STEP level n="); L_i(n++); L_s(" path=sum tones="); L_i(i + 1); L_s(" hz=1000+250k"); L_kv("amp", 0.5, 2); L_end();
        ds_wait(2500);
    }
    for (i = 0; i < 4; i++) if (b[i]) { IDirectSoundBuffer_Stop(b[i]); IDirectSoundBuffer_Release(b[i]); }
    ds_wait(300);
    /* A3D: native (2D) source and a 3D source straight ahead at the minimum distance */
    s.src = new_source(1, STIM_TONE, 48000, 1, 16, 1.0);
    if (s.src) {
        IA3dSource_Play(s.src, A3D_LOOPED);
        L_begin(); L_s("STEP level n="); L_i(n++); L_s(" path=a3d_native amp=0.37"); L_end();
        hold(&s, 0, 2000); kill_source(&s); hold(NULL, 0, 300);
    }
    headphones();
    s.src = new_source(0, STIM_TONE, 48000, 1, 16, 1.0);
    if (s.src) {
        IA3dSource_Play(s.src, A3D_LOOPED);
        place(&s, 0, 0, 1.0);
        L_begin(); L_s("STEP level n="); L_i(n++); L_s(" path=a3d_front_1m_headphones amp=0.37"); L_end();
        hold(&s, 1, 2500); kill_source(&s); hold(NULL, 0, 300);
    }
    logmsg("TEST level end");
}

/* 2. MIDI through the Aureal synthesiser */
static HMIDIOUT g_mo;
static void mo(int st, int d1, int d2) { if (g_mo) midiOutShortMsg(g_mo, (DWORD)(st | (d1 << 8) | (d2 << 16))); }
static void midi_note(const char *what, int ch, int prog, int note, int vel, int on_ms, int off_ms, int *n) {
    if (prog >= 0) mo(0xc0 | ch, prog, 0);
    L_begin(); L_s("STEP midi n="); L_i((*n)++); L_s(" what="); L_s(what); L_s(" ch="); L_i(ch + 1);
    L_s(" prog="); L_i(prog); L_s(" note="); L_i(note); L_s(" vel="); L_i(vel); L_end();
    mo(0x90 | ch, note, vel); ds_wait(on_ms); mo(0x80 | ch, note, 0); ds_wait(off_ms);
}
static void midi_reset_ctl(void) {
    int c; for (c = 0; c < 16; c++) { mo(0xb0 | c, 121, 0); mo(0xb0 | c, 7, 100); mo(0xb0 | c, 10, 64); mo(0xe0 | c, 0, 64); }
}

static void t_midi(void) {
    static const int notes[] = { 24, 36, 48, 60, 72, 84, 96, 108 };
    static const int vels[] = { 127, 100, 64, 32, 16 };
    static const int progs[] = { 0, 24, 33, 40, 48, 56, 73, 80 };
    static const int drums[] = { 35, 36, 38, 42, 46, 49, 51 };
    MIDIOUTCAPSA caps; UINT i, ndev = midiOutGetNumDevs(), pick = (UINT)-1; int n = 0, k; char b[200];
    logmsg("TEST midi begin");
    for (i = 0; i < ndev; i++) {
        if (midiOutGetDevCapsA(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        L_begin(); L_s("midi device "); L_i(i); L_s(": "); L_s(caps.szPname); L_s(" tech="); L_i(caps.wTechnology); L_s(" voices="); L_i(caps.wVoices); L_end();
        if (pick == (UINT)-1 && caps.wTechnology != MOD_MIDIPORT) pick = i;   /* first internal synth */
    }
    if (pick == (UINT)-1) pick = 0;
    if (midiOutOpen(&g_mo, pick, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { logmsg("ERROR midiOutOpen"); g_mo = NULL; }
    else { L_begin(); L_s("midi using device "); L_i(pick); L_end(); }
    if (g_mo) {
        midi_reset_ctl(); ds_wait(500);
        for (k = 0; k < 8; k++) midi_note("pitch", 0, 0, notes[k], 100, 1500, 500, &n);         /* piano across the range */
        for (k = 0; k < 5; k++) midi_note("velocity", 0, 0, 60, vels[k], 1500, 500, &n);
        for (k = 0; k < 8; k++) midi_note("program", 0, progs[k], 60, 100, 2500, 700, &n);      /* incl. sustained loops */
        midi_note("sustain", 0, 48, 48, 100, 6000, 1500, &n);                                  /* strings: loop and release */
        for (k = 0; k < 7; k++) midi_note("drum", 9, -1, drums[k], 110, 600, 400, &n);
        mo(0xc0, 80, 0);                                                                     /* square lead for controllers */
        mo(0x90, 69, 100);
        for (k = 0; k < 4; k++) { static const int v[] = { 127, 64, 32, 100 }; mo(0xb0, 7, v[k]);
            L_begin(); L_s("STEP midi n="); L_i(n++); L_s(" what=cc7 value="); L_i(v[k]); L_end(); ds_wait(1200); }
        for (k = 0; k < 4; k++) { static const int v[] = { 0, 32, 96, 127 }; mo(0xb0, 10, v[k]);
            L_begin(); L_s("STEP midi n="); L_i(n++); L_s(" what=cc10 value="); L_i(v[k]); L_end(); ds_wait(1200); }
        mo(0xb0, 10, 64);
        for (k = 0; k < 4; k++) { static const int v[] = { 8192 + 4096, 16383, 0, 8192 }; mo(0xe0, v[k] & 127, v[k] >> 7);
            L_begin(); L_s("STEP midi n="); L_i(n++); L_s(" what=bend value="); L_i(v[k]); L_end(); ds_wait(1200); }
        mo(0x80, 69, 0); ds_wait(500);
        mo(0xc0, 0, 0);
        for (k = 0; k < 2; k++) { mo(0xb0, 91, k ? 127 : 0); mo(0xb0, 93, 0);
            midi_note(k ? "reverb127" : "reverb0", 0, -1, 60, 110, 800, 1700, &n); }
        for (k = 0; k < 2; k++) { mo(0xb0, 91, 0); mo(0xb0, 93, k ? 127 : 0);
            midi_note(k ? "chorus127" : "chorus0", 0, -1, 60, 110, 1500, 1000, &n); }
        mo(0xb0, 93, 0);
        L_begin(); L_s("STEP midi n="); L_i(n++); L_s(" what=chord prog=0 notes=48,52,55,60,64,67,72"); L_end();
        for (k = 0; k < 7; k++) { static const int c[] = { 48, 52, 55, 60, 64, 67, 72 }; mo(0x90, c[k], 100); }
        ds_wait(2500);
        for (k = 0; k < 7; k++) { static const int c[] = { 48, 52, 55, 60, 64, 67, 72 }; mo(0x80, c[k], 0); }
        ds_wait(1000);
        midi_reset_ctl();
        midiOutReset(g_mo); midiOutClose(g_mo); g_mo = NULL;
    }
    /* a real piece through the MIDI mapper, as games play it */
    {
        char path[MAX_PATH], cmd[MAX_PATH + 64]; MCIERROR e;
        GetWindowsDirectoryA(path, MAX_PATH);
        wsprintfA(cmd, "open \"%s\\MEDIA\\CANYON.MID\" type sequencer alias song", path);
        e = mciSendStringA(cmd, NULL, 0, NULL);
        if (e) { L_begin(); L_s("canyon.mid open failed, mci error "); L_i((long)e); L_end(); }
        else {
            L_begin(); L_s("STEP midi n="); L_i(n++); L_s(" what=canyon.mid (MIDI mapper, 40 s)"); L_end();
            mciSendStringA("play song", NULL, 0, NULL);
            ds_wait(40000);
            mciSendStringA("stop song", NULL, 0, NULL);
            mciSendStringA("close song", NULL, 0, NULL);
        }
        (void)b;
    }
    ds_wait(500);
    logmsg("TEST midi end");
}

/* 4. directions between table points in both azimuth and elevation (3.75-degree grid) */
static void t_hrtfgrid(void) {
    static const struct { int az0, el0; } box[] = { { 0, 0 }, { 90, -45 }, { 195, 30 } };
    SRC s = {0}; int bi, i, j, n = 0;
    logmsg("TEST hrtfgrid begin (headphones, click, r=1 m, 250 ms per position)");
    headphones();
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dSource_Play(s.src, A3D_LOOPED);
    for (bi = 0; bi < 3; bi++) for (j = 0; j <= 8; j++) for (i = 0; i <= 12; i++) {
        double az = box[bi].az0 + i * 3.75, el = box[bi].el0 + j * 3.75;
        place(&s, az, el, 1.0);
        L_begin(); L_s("STEP hrtfgrid n="); L_i(n++); L_kv("az", az, 2); L_kv("el", el, 2); L_end();
        hold(&s, 1, 250);
    }
    kill_source(&s);
    logmsg("TEST hrtfgrid end");
}


/* run 13: the reflection voice's tone and level. One click source 4 m ahead, one wall
   4 m to the right, so the echo arrives about 14 ms after the click, well clear of it;
   high- then low-frequency reflectance stepped, 4 s per step, no-wall references at
   both ends. */
static void t_reflhf(void) {
    static const double hf[] = { 1.0, 0.75, 0.5, 0.35, 0.25, 0.15, 0.1, 0.05, 0.0 };
    static const double lf[] = { 0.75, 0.5, 0.35, 0.25, 0.1 };
    SRC s = {0}; int i, n = 0;
    logmsg("TEST reflhf begin (headphones, click, front r=4 m, wall right d=4 m, 4 s per step)");
    headphones();
    s.src = new_source(0, STIM_CLICK, 48000, 1, 16, 1.0);
    if (!s.src) return;
    IA3dMaterial_SetTransmittance(mat, 0.0f, 0.0f);
    IA3dGeom_Enable(geom, A3D_1ST_REFLECTIONS);
    IA3dGeom_SetReflectionDelayScale(geom, 1.0f); IA3dGeom_SetReflectionGainScale(geom, 1.0f);
    IA3dSource_Play(s.src, A3D_LOOPED);
    place(&s, 0, 0, 4.0);
    g_wall = WALL_NONE; g_wall_d = 4.0;
    L_begin(); L_s("STEP reflhf n="); L_i(n++); L_s(" walls=none"); L_end(); hold(&s, 1, 4000);
    g_wall = WALL_RIGHT;
    for (i = 0; i < 9; i++) {
        IA3dMaterial_SetReflectance(mat, 1.0f, (A3DVAL)hf[i]);
        L_begin(); L_s("STEP reflhf n="); L_i(n++); L_s(" walls=right d=4"); L_kv("refl_low", 1.0, 2); L_kv("refl_high", hf[i], 2); L_end();
        hold(&s, 1, 4000);
    }
    for (i = 0; i < 5; i++) {
        IA3dMaterial_SetReflectance(mat, (A3DVAL)lf[i], 1.0f);
        L_begin(); L_s("STEP reflhf n="); L_i(n++); L_s(" walls=right d=4"); L_kv("refl_low", lf[i], 2); L_kv("refl_high", 1.0, 2); L_end();
        hold(&s, 1, 4000);
    }
    g_wall = WALL_NONE;
    L_begin(); L_s("STEP reflhf n="); L_i(n++); L_s(" walls=none"); L_end(); hold(&s, 1, 4000);
    IA3dGeom_Disable(geom, A3D_1ST_REFLECTIONS);
    IA3dMaterial_SetTransmittance(mat, 1.0f, 1.0f);
    IA3dMaterial_SetReflectance(mat, 0.0f, 0.0f);
    kill_source(&s);
    logmsg("TEST reflhf end");
}

/* ------------------------------------------------------------------ main */
typedef void (*TESTFN)(void);
static const struct { const char *name; TESTFN fn; } tests[] = {
    {"sync", t_sync}, {"formats", t_formats}, {"azimuth", t_azimuth}, {"elevation", t_elevation},
    {"distance", t_distance}, {"gain", t_gain}, {"eq", t_eq}, {"doppler", t_doppler}, {"pitch", t_pitch},
    {"occlusion", t_occlusion}, {"reflections", t_reflections}, {"glide", t_glide},
    {"xtalk", t_xtalk}, {"ref", t_ref}, {"hrtfmap", t_hrtfmap}, {"glide2", t_glide2}, {"reflect2", t_reflect2},
    {"multi", t_multi}, {"dsound", t_dsound}, {"elev5", t_elev5}, {"tcsweep", t_tcsweep}, {"click90", t_click90}, {"noise2d", t_noise2d}, {"wtpath", t_wtpath},
    {"srcresp", t_srcresp}, {"midi", t_midi}, {"level", t_level}, {"hrtfgrid", t_hrtfgrid}, {"reflhf", t_reflhf},
};
/* default (no arguments): the run-2 suite */
static const char *suite2[] = { "xtalk", "ref", "hrtfmap", "glide2", "reflect2", "multi", "dsound" };
#define NTESTS (sizeof(tests) / sizeof(tests[0]))

static int init_a3d(void) {
    HRESULT hr; A3DCAPS_HARDWARE hw; DWORD feat = A3D_OCCLUSIONS | A3D_1ST_REFLECTIONS;
    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_A3dApi, NULL, CLSCTX_INPROC_SERVER, &IID_IA3d4, (void **)&a3d);
    if (FAILED(hr)) { L_begin(); L_s("ERROR CoCreateInstance(A3dApi) "); L_x(hr); L_end(); return 0; }
    hr = IA3d4_QueryInterface(a3d, &IID_IA3dGeom, (void **)&geom);
    if (FAILED(hr)) { L_begin(); L_s("ERROR IA3dGeom "); L_x(hr); L_end(); return 0; }
    hr = IA3d4_Init(a3d, NULL, feat, A3DRENDERPREFS_DEFAULT);
    if (FAILED(hr)) { L_begin(); L_s("ERROR Init "); L_x(hr); L_end(); return 0; }
    IA3d4_SetCooperativeLevel(a3d, g_wnd, A3D_CL_NORMAL);
    L_begin(); L_s("A3D ready: occlusions="); L_i(IA3d4_IsFeatureAvailable(a3d, A3D_OCCLUSIONS));
    L_s(" reflections="); L_i(IA3d4_IsFeatureAvailable(a3d, A3D_1ST_REFLECTIONS));
    memset(&hw, 0, sizeof hw); hw.dwSize = sizeof hw;
    if (SUCCEEDED(IA3d4_GetHardwareCaps(a3d, &hw))) { L_s(" hw3d="); L_i(hw.dwMax3DBuffers); L_s(" hw2d="); L_i(hw.dwMax2DBuffers); L_s(" hwflags="); L_x(hw.dwFlags); }
    L_end();
    if (SUCCEEDED(IA3d4_GetOutputMode(a3d, &g_om0, &g_fx0, &g_rx0))) {
        g_om_saved = 1;
        L_begin(); L_s("output mode at start: mode="); L_i((long)g_om0); L_s(" front_xtalk="); L_i((long)g_fx0); L_s(" rear_xtalk="); L_i((long)g_rx0);
        L_s(" (1=stereo 2=quad; xtalk 1=headphones 2=speakers wide 3=speakers narrow)"); L_end();
    }
    IA3dGeom_NewMaterial(geom, &mat);
    IA3dMaterial_SetTransmittance(mat, 1.0f, 1.0f);
    IA3dMaterial_SetReflectance(mat, 0.0f, 0.0f);
    return 1;
}

static void run_named(const char *name) {
    unsigned i;
    for (i = 0; i < NTESTS; i++) if (seq(name, tests[i].name)) { tests[i].fn(); return; }
    L_begin(); L_s("unknown test '"); L_s(name); L_s("'"); L_end();
}

void entry(void) {
    char *cmd = GetCommandLineA(), tok[32]; int ran = 0, k; SYSTEMTIME st; unsigned i;
    QueryPerformanceFrequency(&g_qf); QueryPerformanceCounter(&g_q0);
    g_wnd = CreateWindowExA(0, "STATIC", "a3dtest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 720, 90, NULL, NULL, GetModuleHandleA(NULL), NULL);
    open_log();
    GetLocalTime(&st);
    L_begin(); L_s("a3dtest start; guest local time ");
    { char b[64]; wsprintfA(b, "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds); L_s(b); }
    L_s(" qpc_hz="); L_i((long)g_qf.QuadPart); L_s(" tick="); L_i((long)GetTickCount()); L_end();
#ifdef SMOKE
    if (!init_a3d()) { logmsg("SMOKE: continuing without A3D"); t_srcresp(); t_midi(); ExitProcess(0); }
#endif
    if (!init_a3d()) { MessageBoxA(g_wnd, "A3D initialisation failed - see A3DTEST.LOG", "a3dtest", MB_OK); ExitProcess(1); }
    hold(NULL, 0, 1000);

    /* skip program name (possibly quoted) */
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    for (;;) {
        while (*cmd == ' ') cmd++;
        if (!*cmd) break;
        k = 0; while (*cmd && *cmd != ' ' && k < 31) tok[k++] = *cmd++;
        tok[k] = 0;
        if (seq(tok, "all")) { for (i = 0; i < NTESTS; i++) tests[i].fn(); }
        else if (seq(tok, "v1")) { for (i = 0; i < 12; i++) tests[i].fn(); }
        else if (seq(tok, "run3")) { t_sync(); run_named("elev5"); run_named("tcsweep"); }
        else if (seq(tok, "run13")) { t_sync(); t_reflhf(); }
        else if (seq(tok, "run12")) { t_sync(); t_srcresp(); t_level(); t_midi(); t_hrtfgrid(); }
        else run_named(tok);
        ran = 1;
    }
    if (!ran) { t_sync(); for (i = 0; i < sizeof suite2 / sizeof suite2[0]; i++) run_named(suite2[i]); }
    t_sync();                                   /* closing sync marks the end */
    if (g_om_saved) {
        HRESULT hr = IA3d4_SetOutputMode(a3d, g_om0, g_fx0, g_rx0);
        L_begin(); L_s("output mode restored hr="); L_x(hr); L_end();
    }
    logmsg("a3dtest done");
    if (g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
    /* Run 1 crashed during process exit after CoUninitialize. The A3D objects are left for
       Windows to clean up instead; the log is already closed. */
    MessageBoxA(g_wnd, "a3dtest finished. Shut Windows down to end the session.", "a3dtest", MB_OK);
    ExitProcess(0);
}
