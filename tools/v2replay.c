/*
 * v2replay - replay a traced register timeline into the Vortex 2 model.
 *
 * Feeds every register write from a timeline (made by vortex_trace.py from a
 * QEMU trace of the real card) into the chip model in core/, at the traced
 * times, and checks the model's answer to every register read the driver made
 * against what the real card returned. Optionally writes the model's output.
 *
 *   v2replay [options] TIMELINE
 *     -o FILE      write the model's output as raw 48 kHz stereo signed 16-bit
 *                  (front + rear mixed, as heard on a two-speaker system)
 *     -t SECONDS   stop at this trace time
 *     -a           compare every read, not just the deterministic registers
 *                  (position and status registers then differ with timing,
 *                  because the replay does not follow the guest's timing)
 *     -v           print each mismatch (up to 400), not just the summary
 *     -l           print the model's diagnostic log
 *     -d FILE      take the stream data from a vortex2-dma.bin written by the
 *                  86Box device (its trace option), so the output is the
 *                  audio the game played rather than noise
 *     -q           write four channels (front left, front right, rear left,
 *                  rear right) instead of front and rear mixed to stereo
 *     -s SRC[,SRC] write one ADB source (hex) instead of the mixed output,
 *                  e.g. -s a6,a7 for 3D slice 0 or -s 7e,7f for the EQ
 *
 * The trace holds no guest memory, so DMA reads return a fixed noise pattern;
 * the audio shows what the chip does with a stream, not what the game played.
 * With no -a the reads compared are the ones that do not depend on timing:
 * the ID word 0x29850, the ADB route table and the mixer targets.
 *
 * Exit status: 0 if every compared read matched, 1 otherwise, 2 on error.
 *
 * GPL-2.0-or-later.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "snd_vortex2_core.h"

#define MAX_REGS (V2_REG_SIZE / 4)

static double   timer_due = -1, timer_per;
static double   now_t;
static uint64_t frames;
static uint16_t codec[64];
static int      show_log;
static uint8_t *dma_log;
static size_t   dma_len;
static size_t  *dma_off;   /* offset of each record */
static uint8_t *dma_used;
static size_t   dma_n, dma_first; /* records, and the first one not yet served */
static long     dma_hits, dma_misses;

static void
dma_index(void)
{
    size_t p = 0, cap = 1024;
    dma_off = malloc(cap * sizeof(size_t));
    while (p + 8 <= dma_len) {
        uint32_t rl;
        memcpy(&rl, dma_log + p + 4, 4);
        if (p + 8 + rl > dma_len)
            break;
        if (dma_n == cap)
            dma_off = realloc(dma_off, (cap *= 2) * sizeof(size_t));
        dma_off[dma_n++] = p;
        p += 8 + rl;
    }
    dma_used = calloc(dma_n, 1);
}

/* Serve a fetch from the recorded data. The streams' fetches interleave
   slightly differently when the replay's block timing differs from the
   recording's, so look ahead a little from the first unserved record;
   records passed over stay available for the other streams. */
static int
dma_serve(uint32_t addr, uint8_t *buf, uint32_t len)
{
    /* Search around the last record served (dma_cur), not from the first
       unserved one: a fetch 86Box made that the replay never repeats would
       otherwise pin the window at that record for the rest of the run. */
    static size_t dma_cur;
    size_t        lo = dma_cur > 65536 ? dma_cur - 65536 : 0;
    size_t        hi = dma_cur + 262144 < dma_n ? dma_cur + 262144 : dma_n;

    for (size_t i = lo; i < hi; i++) {
        if (dma_used[i])
            continue;
        uint32_t ra, rl;
        memcpy(&ra, dma_log + dma_off[i], 4);
        memcpy(&rl, dma_log + dma_off[i] + 4, 4);
        if (ra == addr && rl == len) {
            memcpy(buf, dma_log + dma_off[i] + 8, len);
            dma_used[i] = 1;
            dma_cur     = i;
            return 1;
        }
    }
    /* No exact match: the replay's timing made it fetch with different
       boundaries. Take each byte from a nearby recorded fetch that covers
       it (the guest memory at about that time). */
    for (uint32_t b = 0; b < len;) {
        uint32_t a = addr + b, n = 0;
        for (size_t i = lo; i < hi; i++) {
            uint32_t ra, rl;
            memcpy(&ra, dma_log + dma_off[i], 4);
            memcpy(&rl, dma_log + dma_off[i] + 4, 4);
            if (a >= ra && a - ra < rl) {
                n = rl - (a - ra);
                if (n > len - b)
                    n = len - b;
                memcpy(buf + b, dma_log + dma_off[i] + 8 + (a - ra), n);
                break;
            }
        }
        if (!n)
            return 0;
        b += n;
    }
    return 1;
}

static void
mem_read(void *p, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void) p;
    if (dma_log) {
        if (dma_serve(addr, buf, len)) {
            dma_hits++;
            return;
        }
        dma_misses++;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = addr + i, h = (a >> 1) * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        int16_t s = (int16_t) (((int32_t) (h & 0xffff) - 32768) / 3);
        buf[i]    = (a & 1) ? (uint8_t) (s >> 8) : (uint8_t) s;
    }
}
static void mem_write(void *p, uint32_t a, const uint8_t *b, uint32_t l) { (void) p; (void) a; (void) b; (void) l; }
static void set_irq(void *p, int l) { (void) p; (void) l; }
static void
timer_arm(void *p, uint32_t n)
{
    (void) p;
    timer_per = n / (double) V2_RATE;
    timer_due = n ? now_t + timer_per : -1;
}
static uint32_t clk(void *p) { (void) p; return (uint32_t) (frames * 256); }
static uint16_t cread(void *p, uint8_t r) { (void) p; return codec[(r >> 1) & 63]; }
static void cwrite(void *p, uint8_t r, uint16_t v) { (void) p; codec[(r >> 1) & 63] = v; }
static void
logmsg(void *p, const char *m)
{
    (void) p;
    if (show_log)
        fputs(m, stderr);
}

static int
deterministic(uint32_t a)
{
    return a == 0x29850 || (a >= 0x28000 && a < 0x28310) || (a >= 0x21000 && a < 0x21c00);
}

int
main(int argc, char **argv)
{
    const char *outname = NULL, *dmaname = NULL;
    double      tmax    = 1e30;
    int         all = 0, verbose = 0, quad = 0, opt, solo[2] = { -1, -1 };
    const char *usage = "usage: v2replay [-o out.raw] [-t seconds] [-a] [-v] [-l] [-d dma.bin] [-s src[,src]] [-q] TIMELINE\n";

    while ((opt = getopt(argc, argv, "o:t:avld:s:q")) != -1) {
        switch (opt) {
            case 'o': outname = optarg; break;
            case 't': tmax = atof(optarg); break;
            case 'a': all = 1; break;
            case 'v': verbose = 1; break;
            case 'l': show_log = 1; break;
            case 'd': dmaname = optarg; break;
            case 'q':
                quad = 1;
                break;
            case 's':
                solo[0] = (int) strtol(optarg, NULL, 16);
                solo[1] = strchr(optarg, ',') ? (int) strtol(strchr(optarg, ',') + 1, NULL, 16) : solo[0];
                break;
            default:
                fputs(usage, stderr);
                return 2;
        }
    }
    if (optind >= argc) {
        fputs(usage, stderr);
        return 2;
    }
    if (dmaname) {
        FILE *d = fopen(dmaname, "rb");
        if (!d) {
            perror(dmaname);
            return 2;
        }
        fseek(d, 0, SEEK_END);
        dma_len = (size_t) ftell(d);
        fseek(d, 0, SEEK_SET);
        dma_log = malloc(dma_len);
        if (!dma_log || fread(dma_log, 1, dma_len, d) != dma_len) {
            fprintf(stderr, "could not load %s\n", dmaname);
            return 2;
        }
        fclose(d);
        dma_index();
    }
    FILE *f = fopen(argv[optind], "r");
    if (!f) {
        perror(argv[optind]);
        return 2;
    }
    FILE *o = outname ? fopen(outname, "wb") : NULL;
    if (outname && !o) {
        perror(outname);
        return 2;
    }

    /* The SQ2500's STAC9708 identity, as the driver reads it at start-up. */
    codec[0x7c >> 1] = 0x8384;
    codec[0x7e >> 1] = 0x7608;
    codec[0x28 >> 1] = 0x0080;
    codec[0x26 >> 1] = 0x000f;

    v2_host_t  h = { NULL, mem_read, mem_write, set_irq, timer_arm, clk, cread, cwrite, logmsg };
    v2_chip_t *c = v2_create(&h);

    static uint32_t mism[MAX_REGS];
    char            line[256];
    float           out[V2_BLOCK * 4];
    int16_t         pcm[V2_BLOCK * 4];
    const int       nch = quad ? 4 : 2;
    const double    block_dt = V2_BLOCK / (double) V2_RATE;
    long            nw = 0, nr = 0, compared = 0, mismatches = 0;

    while (fgets(line, sizeof line, f)) {
        double   t;
        char     rw[4], as[32], via[8];
        unsigned val;
        int      rep = 1;

        if (sscanf(line, "%lf %3s %31s %7s %x", &t, rw, as, via, &val) < 5)
            continue;
        char *x = strstr(line, " x");
        if (x)
            rep = atoi(x + 2);
        if (t > tmax)
            break;

        while (now_t + block_dt <= t) {
            if (timer_due >= 0 && timer_due <= now_t + block_dt) {
                timer_due = timer_per > 0 ? timer_due + timer_per : -1;
                v2_timer_expired(c);
            }
            v2_run_block(c, out);
            frames += V2_BLOCK;
            now_t += block_dt;
            if (o) {
                const float *sl = solo[0] >= 0 ? v2_debug_source(c, solo[0]) : NULL;
                const float *sr = solo[0] >= 0 ? v2_debug_source(c, solo[1]) : NULL;
                for (int n = 0; n < V2_BLOCK; n++)
                    for (int k = 0; k < nch; k++) {
                        float v = sl ? (k & 1 ? sr[n] : sl[n])
                                     : quad ? out[n * 4 + k] : out[n * 4 + k] + out[n * 4 + 2 + k];
                        int   s = (int) lrintf(v * 32768.0f);
                        pcm[n * nch + k] = (int16_t) (s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
                    }
                fwrite(pcm, sizeof(int16_t), (size_t) V2_BLOCK * nch, o);
            }
        }

        uint32_t a = (uint32_t) strtoul(as, NULL, 16);
        for (int i = 0; i < rep; i++) {
            if (rw[0] == 'W') {
                v2_write(c, a, val);
                nw++;
                continue;
            }
            uint32_t v = v2_read(c, a);
            nr++;
            /* The sample counter and the codec data port never repeat. */
            if (!(all ? (a != 0x29198 && a != 0x29188) : deterministic(a)))
                continue;
            compared++;
            if (v != val) {
                if (verbose && mismatches < 400)
                    fprintf(stderr, "mismatch t=%.4f %05x model %08x card %08x\n", t, a, v, val);
                mismatches++;
                if (a < V2_REG_SIZE)
                    mism[a >> 2]++;
            }
        }
    }

    printf("%ld writes, %ld reads, %.1f s replayed; %ld reads compared, %ld differ\n",
           nw, nr, now_t, compared, mismatches);
    if (dma_log)
        printf("stream data: %ld fetches from the recording, %ld not found (noise)\n", dma_hits, dma_misses);
    if (mismatches) {
        printf("registers that differ most often:\n");
        for (int k = 0; k < 12; k++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t r = 0; r < MAX_REGS; r++)
                if (mism[r] > best) {
                    best = mism[r];
                    bi   = r;
                }
            if (!best)
                break;
            printf("  %05x  %u\n", bi << 2, best);
            mism[bi] = 0;
        }
    }
    if (o)
        fclose(o);
    fclose(f);
    v2_destroy(c);
    return mismatches ? 1 : 0;
}
