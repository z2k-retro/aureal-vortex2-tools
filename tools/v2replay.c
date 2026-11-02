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

static void
mem_read(void *p, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void) p;
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
    const char *outname = NULL;
    double      tmax    = 1e30;
    int         all = 0, verbose = 0, opt;

    while ((opt = getopt(argc, argv, "o:t:avl")) != -1) {
        switch (opt) {
            case 'o': outname = optarg; break;
            case 't': tmax = atof(optarg); break;
            case 'a': all = 1; break;
            case 'v': verbose = 1; break;
            case 'l': show_log = 1; break;
            default:
                fprintf(stderr, "usage: v2replay [-o out.raw] [-t seconds] [-a] [-v] [-l] TIMELINE\n");
                return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: v2replay [-o out.raw] [-t seconds] [-a] [-v] [-l] TIMELINE\n");
        return 2;
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
    int16_t         pcm[V2_BLOCK * 2];
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
                for (int n = 0; n < V2_BLOCK; n++)
                    for (int k = 0; k < 2; k++) {
                        int s = (int) lrintf((out[n * 4 + k] + out[n * 4 + 2 + k]) * 32768.0f);
                        pcm[n * 2 + k] = (int16_t) (s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
                    }
                fwrite(pcm, sizeof pcm, 1, o);
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
