/*
 * SpeedHQ encoder
 * Copyright (c) 2000, 2001 Fabrice Bellard
 * Copyright (c) 2003 Alex Beregszaszi
 * Copyright (c) 2003-2004 Michael Niedermayer
 * Copyright (c) 2020 FFmpeg
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * SpeedHQ encoder.
 */

#include "libavutil/thread.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "mpeg12data.h"
#include "mpeg12vlc.h"
#include "mpegvideo.h"
#include "mpegvideodata.h"
#include "mpegvideoenc.h"
#include "rl.h"
#include "speedhq.h"
#include "speedhqenc.h"

static uint8_t speedhq_max_level[MAX_LEVEL + 1];
static uint8_t speedhq_index_run[MAX_RUN   + 1];

/* Exactly the same as MPEG-2, except little-endian. */
static const uint16_t mpeg12_vlc_dc_lum_code_reversed[12] = {
    0x1, 0x0, 0x2, 0x5, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF
};
static const uint16_t mpeg12_vlc_dc_chroma_code_reversed[12] = {
    0x0, 0x2, 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF
};

/* simple include everything table for dc, first byte is bits
 * number next 3 are code */
static uint32_t speedhq_lum_dc_uni[512];
static uint32_t speedhq_chr_dc_uni[512];

static uint8_t uni_speedhq_ac_vlc_len[64 * 64 * 2];

typedef struct SpeedHQEncContext {
    MpegEncContext m;

    int slice_start;
} SpeedHQEncContext;

static av_cold void speedhq_init_static_data(void)
{
    ff_rl_init_level_run(speedhq_max_level, speedhq_index_run,
                         ff_speedhq_run, ff_speedhq_level, SPEEDHQ_RL_NB_ELEMS);

    /* build unified dc encoding tables */
    for (int i = -255; i < 256; i++) {
        int adiff, index;
        int bits, code;
        int diff = i;

        adiff = FFABS(diff);
        if (diff < 0)
            diff--;
        index = av_log2(2 * adiff);

        bits = ff_mpeg12_vlc_dc_lum_bits[index] + index;
        code = mpeg12_vlc_dc_lum_code_reversed[index] +
                (av_zero_extend(diff, index) << ff_mpeg12_vlc_dc_lum_bits[index]);
        speedhq_lum_dc_uni[i + 255] = bits + (code << 8);

        bits = ff_mpeg12_vlc_dc_chroma_bits[index] + index;
        code = mpeg12_vlc_dc_chroma_code_reversed[index] +
                (av_zero_extend(diff, index) << ff_mpeg12_vlc_dc_chroma_bits[index]);
        speedhq_chr_dc_uni[i + 255] = bits + (code << 8);
    }

    ff_mpeg1_init_uni_ac_vlc(speedhq_max_level, speedhq_index_run,
                             ff_speedhq_vlc_table, uni_speedhq_ac_vlc_len);
}

av_cold int ff_speedhq_encode_init(MpegEncContext *s)
{
    static AVOnce init_static_once = AV_ONCE_INIT;

    if (s->width > 65500 || s->height > 65500) {
        av_log(s->avctx, AV_LOG_ERROR, "SpeedHQ does not support resolutions above 65500x65500\n");
        return AVERROR(EINVAL);
    }

    // border is not implemented correctly at the moment, see ticket #10078
    if (s->width % 16) {
        av_log(s->avctx, AV_LOG_ERROR, "width must be a multiple of 16\n");
        return AVERROR_PATCHWELCOME;
    }

    s->min_qcoeff = -2048;
    s->max_qcoeff = 2047;

    ff_thread_once(&init_static_once, speedhq_init_static_data);

    s->intra_ac_vlc_length      =
    s->intra_ac_vlc_last_length =
    s->intra_chroma_ac_vlc_length      =
    s->intra_chroma_ac_vlc_last_length = uni_speedhq_ac_vlc_len;

    s->y_dc_scale_table =
    s->c_dc_scale_table = ff_mpeg12_dc_scale_table[3];

    switch (s->avctx->pix_fmt) {
    case AV_PIX_FMT_YUV420P:
        s->avctx->codec_tag = MKTAG('S','H','Q','0');
        break;
    case AV_PIX_FMT_YUV422P:
        s->avctx->codec_tag = MKTAG('S','H','Q','2');
        break;
    case AV_PIX_FMT_YUV444P:
        s->avctx->codec_tag = MKTAG('S','H','Q','4');
        break;
    default:
        av_assert0(0);
    }

    return 0;
}

void ff_speedhq_encode_picture_header(MpegEncContext *s)
{
    SpeedHQEncContext *ctx = (SpeedHQEncContext*)s;

    put_bits_le(&s->pb, 8, 100 - s->qscale * 2);  /* FIXME why doubled */
    put_bits_le(&s->pb, 24, 4);  /* no second field */

    ctx->slice_start = 4;
    /* length of first slice, will be filled out later */
    put_bits_le(&s->pb, 24, 0);
}

void ff_speedhq_end_slice(MpegEncContext *s)
{
    SpeedHQEncContext *ctx = (SpeedHQEncContext*)s;
    int slice_len;

    flush_put_bits_le(&s->pb);
    slice_len = put_bytes_output(&s->pb) - ctx->slice_start;
    AV_WL24(s->pb.buf + ctx->slice_start, slice_len);

    /* length of next slice, will be filled out later */
    ctx->slice_start = put_bytes_output(&s->pb);
    put_bits_le(&s->pb, 24, 0);
}

static inline void encode_dc(PutBitContext *pb, int diff, int component)
{
    unsigned int diff_u = diff + 255;
    if (diff_u >= 511) {
        int index;

        if (diff < 0) {
            index = av_log2_16bit(-2 * diff);
            diff--;
        } else {
            index = av_log2_16bit(2 * diff);
        }
        if (component == 0)
            put_bits_le(pb,
                        ff_mpeg12_vlc_dc_lum_bits[index] + index,
                        mpeg12_vlc_dc_lum_code_reversed[index] +
                        (av_zero_extend(diff, index) << ff_mpeg12_vlc_dc_lum_bits[index]));
        else
            put_bits_le(pb,
                        ff_mpeg12_vlc_dc_chroma_bits[index] + index,
                        mpeg12_vlc_dc_chroma_code_reversed[index] +
                        (av_zero_extend(diff, index) << ff_mpeg12_vlc_dc_chroma_bits[index]));
    } else {
        if (component == 0)
            put_bits_le(pb,
                        speedhq_lum_dc_uni[diff + 255] & 0xFF,
                        speedhq_lum_dc_uni[diff + 255] >> 8);
        else
            put_bits_le(pb,
                        speedhq_chr_dc_uni[diff + 255] & 0xFF,
                        speedhq_chr_dc_uni[diff + 255] >> 8);
    }
}

static void encode_block(MpegEncContext *s, int16_t *block, int n)
{
    int alevel, level, last_non_zero, dc, i, j, run, last_index, sign;
    int code;
    int component, val;

    /* DC coef */
    component = (n <= 3 ? 0 : (n&1) + 1);
    dc = block[0]; /* overflow is impossible */
    val = s->last_dc[component] - dc;  /* opposite of most codecs */
    encode_dc(&s->pb, val, component);
    s->last_dc[component] = dc;

    /* now quantify & encode AC coefs */
    last_non_zero = 0;
    last_index = s->block_last_index[n];

    for (i = 1; i <= last_index; i++) {
        j     = s->intra_scantable.permutated[i];
        level = block[j];

        /* encode using VLC */
        if (level != 0) {
            run = i - last_non_zero - 1;

            alevel = level;
            MASK_ABS(sign, alevel);
            sign &= 1;

            if (alevel <= speedhq_max_level[run]) {
                code = speedhq_index_run[run] + alevel - 1;
                /* store the VLC & sign at once */
                put_bits_le(&s->pb, ff_speedhq_vlc_table[code][1] + 1,
                            ff_speedhq_vlc_table[code][0] | (sign << ff_speedhq_vlc_table[code][1]));
            } else {
                /* escape seems to be pretty rare <5% so I do not optimize it;
                 * the values correspond to ff_speedhq_vlc_table[121] */
                put_bits_le(&s->pb, 6, 32);
                /* escape: only clip in this case */
                put_bits_le(&s->pb, 6, run);
                put_bits_le(&s->pb, 12, level + 2048);
            }
            last_non_zero = i;
        }
    }
    /* end of block; the values correspond to ff_speedhq_vlc_table[122] */
    put_bits_le(&s->pb, 4, 6);
}

void ff_speedhq_encode_mb(MpegEncContext *s, int16_t block[12][64])
{
    int i;
    for(i=0;i<6;i++) {
        encode_block(s, block[i], i);
    }
    if (s->chroma_format == CHROMA_444) {
        encode_block(s, block[8], 8);
        encode_block(s, block[9], 9);

        encode_block(s, block[6], 6);
        encode_block(s, block[7], 7);

        encode_block(s, block[10], 10);
        encode_block(s, block[11], 11);
    } else if (s->chroma_format == CHROMA_422) {
        encode_block(s, block[6], 6);
        encode_block(s, block[7], 7);
    }

    s->i_tex_bits += get_bits_diff(s);
}

const FFCodec ff_speedhq_encoder = {
    .p.name         = "speedhq",
    CODEC_LONG_NAME("NewTek SpeedHQ"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_SPEEDHQ,
    .p.priv_class   = &ff_mpv_enc_class,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(SpeedHQEncContext),
    .init           = ff_mpv_encode_init,
    FF_CODEC_ENCODE_CB(ff_mpv_encode_picture),
    .close          = ff_mpv_encode_end,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .color_ranges   = AVCOL_RANGE_MPEG,
    .p.pix_fmts     = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_NONE
    },
};
