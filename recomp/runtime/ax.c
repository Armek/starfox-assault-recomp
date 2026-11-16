/* ax.c - the DSP side of the AX audio library (an HLE of the AX microcode).
 *
 * Every 5 ms audio frame the SDK's __AXOutNewFrame mails 0xBABE0180 and the address of a command
 * list to the DSP. This file executes that list the way the microcode would: it walks the voice
 * parameter blocks (AXPB, 0xF4 bytes, big-endian), applies their per-millisecond parameter
 * updates, reads sample data from ARAM (DSP-ADPCM, PCM16, PCM8), resamples, applies the volume
 * envelope and low-pass filter, mixes into the main / aux A / aux B buses, swaps the aux buffers
 * with the CPU's effect code and writes the final 160-frame stereo buffer that the AI DMA plays.
 * Voice state (positions, ADPCM history, envelopes, ramps, stop on end) is written back so the
 * game's sound engine and CRI ADX streaming see playback progress.
 */
#include <stdio.h>
#include <string.h>
#include "funcs.h"
#include "runtime.h"

#define FRAME 160   /* samples per 5 ms frame at 32 kHz */
#define SUB 32      /* samples per 1 ms parameter-update slice */

static int32_t g_main[3][FRAME], g_auxa[3][FRAME], g_auxb[3][FRAME]; /* L, R, S */
static uint32_t g_pb_list;
unsigned long g_ax_frames, g_ax_voices_played;

/* ---- ARAM sample access -------------------------------------------------------------------------- */
static uint8_t aram8(uint32_t a) { const uint8_t *m = rt_aram(); return m ? m[a & (RT_ARAM_SIZE - 1)] : 0; }

/* ---- the voice parameter block as an array of big-endian u16 words ---------------------------------- */
#define PB_WORDS (0xF4 / 2)
enum {
    PB_NEXT_HI = 0x00, PB_NEXT_LO = 0x01, PB_SRC_SELECT = 0x04, PB_MIXER_CTRL = 0x06, PB_STATE = 0x07, PB_TYPE = 0x08,
    PB_MIX = 0x09,           /* 18 words: vL dL vR dR vAL dAL vAR dAR vBL dBL vBR dBR vBS dBS vS dS vAS dAS */
    PB_UPD_NUM = 0x22,       /* 5 words */
    PB_UPD_HI = 0x27, PB_UPD_LO = 0x28,
    PB_VE_VOL = 0x32, PB_VE_DELTA = 0x33,
    PB_LOOP_FLAG = 0x37, PB_FORMAT = 0x38, PB_LOOP_HI = 0x39, PB_LOOP_LO = 0x3A, PB_END_HI = 0x3B, PB_END_LO = 0x3C,
    PB_CUR_HI = 0x3D, PB_CUR_LO = 0x3E,
    PB_ADPCM_COEF = 0x3F,    /* 16 words */
    PB_ADPCM_GAIN = 0x4F, PB_PRED_SCALE = 0x50, PB_YN1 = 0x51, PB_YN2 = 0x52,
    PB_SRC_RATIO_HI = 0x53, PB_SRC_RATIO_LO = 0x54, PB_SRC_FRAC = 0x55, PB_SRC_LAST = 0x56, /* 4 words */
    PB_LOOP_PS = 0x5A, PB_LOOP_YN1 = 0x5B, PB_LOOP_YN2 = 0x5C,
    PB_LPF_ON = 0x5D, PB_LPF_YN1 = 0x5E, PB_LPF_A0 = 0x5F, PB_LPF_B0 = 0x60,
};

static int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static uint32_t u32w(const uint16_t *pb, int hi) { return (uint32_t)pb[hi] << 16 | pb[hi + 1]; }
static void set32w(uint16_t *pb, int hi, uint32_t v) { pb[hi] = (uint16_t)(v >> 16); pb[hi + 1] = (uint16_t)v; }

/* One input sample from the voice's accelerator; returns 0 and stops the voice past the end. */
static int16_t next_sample(uint16_t *pb, int *ended) {
    if (*ended) return 0;
    uint32_t cur = u32w(pb, PB_CUR_HI), end = u32w(pb, PB_END_HI);
    int format = pb[PB_FORMAT];
    int16_t s = 0;
    if (format == 0) { /* DSP-ADPCM: addresses are in nibbles, 8-byte frames with a header byte */
        if ((cur & 15) == 0) { pb[PB_PRED_SCALE] = aram8(cur >> 1); cur += 2; }
        int nib = (aram8(cur >> 1) >> ((cur & 1) ? 0 : 4)) & 15;
        if (nib & 8) nib -= 16;
        int ps = pb[PB_PRED_SCALE], pred = (ps >> 4) & 7, scale = ps & 15;
        int32_t c1 = (int16_t)pb[PB_ADPCM_COEF + pred * 2], c2 = (int16_t)pb[PB_ADPCM_COEF + pred * 2 + 1];
        int32_t yn1 = (int16_t)pb[PB_YN1], yn2 = (int16_t)pb[PB_YN2];
        int32_t v = ((nib * (1 << scale)) << 11) + c1 * yn1 + c2 * yn2;
        s = clamp16((v + 1024) >> 11);
        pb[PB_YN2] = (uint16_t)yn1;
        pb[PB_YN1] = (uint16_t)s;
    } else if (format == 0x0A) { /* PCM16: addresses in samples */
        s = (int16_t)(aram8(cur * 2) << 8 | aram8(cur * 2 + 1));
    } else if (format == 0x19) { /* PCM8: addresses in bytes */
        s = (int16_t)((int8_t)aram8(cur) << 8);
    }
    cur++;
    if (cur > end) { /* the end address is the last sample played */
        if (pb[PB_LOOP_FLAG]) {
            cur = u32w(pb, PB_LOOP_HI);
            if (format == 0 && pb[PB_TYPE] != 1) { /* not a stream: restore the loop's ADPCM context */
                pb[PB_PRED_SCALE] = pb[PB_LOOP_PS];
                pb[PB_YN1] = pb[PB_LOOP_YN1];
                pb[PB_YN2] = pb[PB_LOOP_YN2];
            }
        } else {
            pb[PB_STATE] = 0;
            *ended = 1;
        }
    }
    set32w(pb, PB_CUR_HI, cur);
    return s;
}

/* Mix `in` into `out` with a volume (1.15) and optional per-sample ramp, writing the volume back. */
static void mix(int32_t *out, const int32_t *in, int n, uint16_t *vol, uint16_t *delta, int ramp) {
    int32_t v = *vol, d = ramp ? (int16_t)*delta : 0;
    for (int i = 0; i < n; i++) {
        out[i] += (int32_t)(((int64_t)in[i] * v) >> 15);
        v += d;
        if (v < 0) v = 0;
        if (v > 0xFFFF) v = 0xFFFF;
    }
    *vol = (uint16_t)v;
}

static void process_voice(uint32_t addr) {
    uint16_t pb[PB_WORDS];
    for (int i = 0; i < PB_WORDS; i++) pb[i] = rd16(addr + (uint32_t)i * 2);
    if (pb[PB_STATE] != 1) {
        for (int i = 0; i < PB_WORDS; i++) wr16(addr + (uint32_t)i * 2, pb[i]);
        return;
    }
    g_ax_voices_played++;
    int ended = 0;
    uint32_t upd_addr = u32w(pb, PB_UPD_HI), upd_pos = 0;
    for (int ms = 0; ms < FRAME / SUB; ms++) {
        /* parameter updates for this millisecond: (word index, value) pairs */
        for (uint32_t k = 0; k < pb[PB_UPD_NUM + ms] && k < 64; k++, upd_pos++) {
            uint32_t idx = rd16(upd_addr + upd_pos * 4), val = rd16(upd_addr + upd_pos * 4 + 2);
            if (idx < PB_WORDS) pb[idx] = (uint16_t)val;
        }
        if (pb[PB_STATE] != 1 && !ended) ended = 1;

        /* resample 32 output samples */
        int32_t buf[SUB];
        uint32_t ratio = pb[PB_SRC_SELECT] == 2 ? 0x10000 : u32w(pb, PB_SRC_RATIO_HI);
        uint32_t frac = pb[PB_SRC_FRAC];
        int16_t *last = (int16_t *)&pb[PB_SRC_LAST];
        for (int i = 0; i < SUB; i++) {
            frac += ratio;
            while (frac >= 0x10000) {
                frac -= 0x10000;
                last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
                last[3] = next_sample(pb, &ended);
            }
            buf[i] = last[2] + (int32_t)(((int64_t)(last[3] - last[2]) * (int32_t)frac) >> 16);
        }
        pb[PB_SRC_FRAC] = (uint16_t)frac;

        /* volume envelope */
        int32_t vol = pb[PB_VE_VOL], dv = (int16_t)pb[PB_VE_DELTA];
        for (int i = 0; i < SUB; i++) {
            buf[i] = (int32_t)(((int64_t)buf[i] * vol) >> 15);
            vol += dv;
            if (vol < 0) vol = 0;
            if (vol > 0x7FFF) vol = 0x7FFF;
        }
        pb[PB_VE_VOL] = (uint16_t)vol;

        /* one-pole low-pass filter */
        if (pb[PB_LPF_ON]) {
            int32_t y = (int16_t)pb[PB_LPF_YN1], a0 = pb[PB_LPF_A0], b0 = pb[PB_LPF_B0];
            for (int i = 0; i < SUB; i++) {
                y = (int32_t)(((int64_t)a0 * buf[i] + (int64_t)b0 * y) >> 15);
                buf[i] = y;
            }
            pb[PB_LPF_YN1] = (uint16_t)clamp16(y);
        }

        /* mixer */
        int off = ms * SUB;
        uint16_t mc = pb[PB_MIXER_CTRL], *m = &pb[PB_MIX];
        int ramp = (mc >> 3) & 1, ra_lr = (mc >> 6) & 1, ra_s = (mc >> 8) & 1, rb_lr = (mc >> 11) & 1, rb_s = (mc >> 13) & 1;
        if (mc & 0x0001) mix(g_main[0] + off, buf, SUB, &m[0], &m[1], ramp);
        if (mc & 0x0002) mix(g_main[1] + off, buf, SUB, &m[2], &m[3], ramp);
        if (mc & 0x0004) mix(g_main[2] + off, buf, SUB, &m[14], &m[15], ramp);
        if (mc & 0x0010) mix(g_auxa[0] + off, buf, SUB, &m[4], &m[5], ra_lr);
        if (mc & 0x0020) mix(g_auxa[1] + off, buf, SUB, &m[6], &m[7], ra_lr);
        if (mc & 0x0080) mix(g_auxa[2] + off, buf, SUB, &m[16], &m[17], ra_s);
        if (mc & 0x0200) mix(g_auxb[0] + off, buf, SUB, &m[8], &m[9], rb_lr);
        if (mc & 0x0400) mix(g_auxb[1] + off, buf, SUB, &m[10], &m[11], rb_lr);
        if (mc & 0x1000) mix(g_auxb[2] + off, buf, SUB, &m[12], &m[13], rb_s);
    }
    for (int i = 0; i < PB_WORDS; i++) wr16(addr + (uint32_t)i * 2, pb[i]);
}

/* Aux bus: hand this frame's send to the CPU's effects and add the processed return. */
static void aux_exchange(int32_t bus[3][FRAME], uint32_t send, uint32_t ret) {
    for (int ch = 0; ch < 3; ch++)
        for (int i = 0; i < FRAME; i++) {
            uint32_t o = (uint32_t)(ch * FRAME + i) * 4;
            if (send) wr32(send + o, (uint32_t)bus[ch][i]);
            if (ret) g_main[ch][i] += (int32_t)rd32(ret + o);
        }
}

void rt_ax_command_list(uint32_t cl) {
    memset(g_main, 0, sizeof g_main);
    memset(g_auxa, 0, sizeof g_auxa);
    memset(g_auxb, 0, sizeof g_auxb);
    g_ax_frames++;
    for (int guard = 0; guard < 256; guard++) {
        uint16_t cmd = rd16(cl);
        cl += 2;
#define ARG32(i) ((uint32_t)rd16(cl + (i) * 4) << 16 | rd16(cl + (i) * 4 + 2))
        switch (cmd) {
        case 0x00: cl += 4; break;                 /* studio setup */
        case 0x07: case 0x11: cl += 4; break;      /* surround buffer */
        case 0x02: g_pb_list = ARG32(0); cl += 4; break;
        case 0x03:                                 /* process all voices */
            for (uint32_t pb = g_pb_list, n = 0; pb && n < 256; n++) {
                uint32_t next = (uint32_t)rd16(pb) << 16 | rd16(pb + 2);
                process_voice(pb);
                pb = next;
            }
            break;
        case 0x04: aux_exchange(g_auxa, ARG32(0), ARG32(1)); cl += 8; break;
        case 0x05: aux_exchange(g_auxb, ARG32(0), ARG32(1)); cl += 8; break;
        case 0x10: cl += 8; break;                 /* Dolby Pro Logic II aux B */
        case 0x13: cl += 24; break;                /* Dolby Pro Logic II aux A */
        case 0x12: cl += 8; break;                 /* compressor */
        case 0x0E: {                               /* final output: interleaved big-endian stereo */
            uint32_t sbuf = ARG32(0), out = ARG32(1);
            for (int i = 0; i < FRAME; i++) {
                wr16(out + (uint32_t)i * 4, (uint16_t)clamp16(g_main[0][i]));
                wr16(out + (uint32_t)i * 4 + 2, (uint16_t)clamp16(g_main[1][i]));
                if (sbuf) wr32(sbuf + (uint32_t)i * 4, (uint32_t)g_main[2][i]);
            }
            cl += 8;
            break;
        }
        case 0x0F: return;                         /* end of list */
        default:
            fprintf(stderr, "[ax] unknown command list entry 0x%04X\n", cmd);
            return;
        }
#undef ARG32
    }
}
