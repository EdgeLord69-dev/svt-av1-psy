/*
* Copyright (c) 2024, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at www.aomedia.org/license/software. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/

#include <arm_neon.h>

#include "common_dsp_rtcd.h"
#include "definitions.h"
#include "filter.h"
#include "highbd_convolve_sve2.h"
#include "inter_prediction.h"
#include "mem_neon.h"
#include "neon_sve_bridge.h"
#include "neon_sve2_bridge.h"
#include "sum_neon.h"
#include "utility.h"

static INLINE uint16x4_t highbd_convolve8_4_y(int16x8_t samples_lo[2], int16x8_t samples_hi[2], int16x8_t filter,
                                              uint16x4_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(vdupq_n_s64(0), samples_lo[0], filter, 0);
    sum01           = svt_svdot_lane_s16(sum01, samples_hi[0], filter, 1);

    int64x2_t sum23 = svt_svdot_lane_s16(vdupq_n_s64(0), samples_lo[1], filter, 0);
    sum23           = svt_svdot_lane_s16(sum23, samples_hi[1], filter, 1);

    int32x4_t  sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    uint16x4_t res     = vqrshrun_n_s32(sum0123, FILTER_BITS);
    return vmin_u16(res, max);
}

static INLINE uint16x8_t highbd_convolve8_8_y(int16x8_t samples_lo[4], int16x8_t samples_hi[4], int16x8_t filter,
                                              uint16x8_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(vdupq_n_s64(0), samples_lo[0], filter, 0);
    sum01           = svt_svdot_lane_s16(sum01, samples_hi[0], filter, 1);

    int64x2_t sum23 = svt_svdot_lane_s16(vdupq_n_s64(0), samples_lo[1], filter, 0);
    sum23           = svt_svdot_lane_s16(sum23, samples_hi[1], filter, 1);

    int64x2_t sum45 = svt_svdot_lane_s16(vdupq_n_s64(0), samples_lo[2], filter, 0);
    sum45           = svt_svdot_lane_s16(sum45, samples_hi[2], filter, 1);

    int64x2_t sum67 = svt_svdot_lane_s16(vdupq_n_s64(0), samples_lo[3], filter, 0);
    sum67           = svt_svdot_lane_s16(sum67, samples_hi[3], filter, 1);

    int32x4_t  sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    int32x4_t  sum4567 = vcombine_s32(vmovn_s64(sum45), vmovn_s64(sum67));
    uint16x8_t res     = vcombine_u16(vqrshrun_n_s32(sum0123, FILTER_BITS), vqrshrun_n_s32(sum4567, FILTER_BITS));
    return vminq_u16(res, max);
}

static void highbd_convolve_y_sr_8tap_sve2(const uint16_t *src, ptrdiff_t src_stride, uint16_t *dst,
                                           ptrdiff_t dst_stride, int width, int height, const int16_t *filter_y,
                                           int bd) {
    assert(width >= 4 && height >= 4);

    const int16x8_t y_filter = vld1q_s16(filter_y);

    uint16x8x3_t merge_block_tbl = vld1q_u16_x3(kDotProdMergeBlockTbl);
    // Scale indices by size of the true vector length to avoid reading from an
    // 'undefined' portion of a vector on a system with SVE vectors > 128-bit.
    uint16x8_t correction0 = vreinterpretq_u16_u64(vdupq_n_u64(svcnth() * 0x0001000000000000ULL));
    merge_block_tbl.val[0] = vaddq_u16(merge_block_tbl.val[0], correction0);

    uint16x8_t correction1 = vreinterpretq_u16_u64(vdupq_n_u64(svcnth() * 0x0001000100000000ULL));
    merge_block_tbl.val[1] = vaddq_u16(merge_block_tbl.val[1], correction1);

    uint16x8_t correction2 = vreinterpretq_u16_u64(vdupq_n_u64(svcnth() * 0x0001000100010000ULL));
    merge_block_tbl.val[2] = vaddq_u16(merge_block_tbl.val[2], correction2);

    if (width == 4) {
        const uint16x4_t max = vdup_n_u16((1 << bd) - 1);
        int16_t         *s   = (int16_t *)src;

        int16x4_t s0, s1, s2, s3, s4, s5, s6;
        load_s16_4x7(s, src_stride, &s0, &s1, &s2, &s3, &s4, &s5, &s6);
        s += 7 * src_stride;

        // This operation combines a conventional transpose and the sample permute
        // required before computing the dot product.
        int16x8_t s0123[2], s1234[2], s2345[2], s3456[2];
        transpose_concat_4x4(s0, s1, s2, s3, s0123);
        transpose_concat_4x4(s1, s2, s3, s4, s1234);
        transpose_concat_4x4(s2, s3, s4, s5, s2345);
        transpose_concat_4x4(s3, s4, s5, s6, s3456);

        do {
            int16x4_t s7, s8, s9, s10;
            load_s16_4x4(s, src_stride, &s7, &s8, &s9, &s10);

            int16x8_t s4567[2], s5678[2], s6789[2], s789A[2];
            // Transpose and shuffle the 4 lines that were loaded.
            transpose_concat_4x4(s7, s8, s9, s10, s789A);

            // Merge new data into block from previous iteration.
            svt_tbl2x2_s16(s3456, s789A, merge_block_tbl.val[0], s4567);
            svt_tbl2x2_s16(s3456, s789A, merge_block_tbl.val[1], s5678);
            svt_tbl2x2_s16(s3456, s789A, merge_block_tbl.val[2], s6789);

            uint16x4_t d0 = highbd_convolve8_4_y(s0123, s4567, y_filter, max);
            uint16x4_t d1 = highbd_convolve8_4_y(s1234, s5678, y_filter, max);
            uint16x4_t d2 = highbd_convolve8_4_y(s2345, s6789, y_filter, max);
            uint16x4_t d3 = highbd_convolve8_4_y(s3456, s789A, y_filter, max);

            store_u16_4x4(dst, dst_stride, d0, d1, d2, d3);

            // Prepare block for next iteration - re-using as much as possible.
            // Shuffle everything up four rows.
            s0123[0] = s4567[0];
            s0123[1] = s4567[1];
            s1234[0] = s5678[0];
            s1234[1] = s5678[1];
            s2345[0] = s6789[0];
            s2345[1] = s6789[1];
            s3456[0] = s789A[0];
            s3456[1] = s789A[1];
            s += 4 * src_stride;
            dst += 4 * dst_stride;
            height -= 4;
        } while (height != 0);
    } else {
        const uint16x8_t max = vdupq_n_u16((1 << bd) - 1);

        do {
            int       h = height;
            int16_t  *s = (int16_t *)src;
            uint16_t *d = dst;

            int16x8_t s0, s1, s2, s3, s4, s5, s6;
            load_s16_8x7(s, src_stride, &s0, &s1, &s2, &s3, &s4, &s5, &s6);
            s += 7 * src_stride;

            // This operation combines a conventional transpose and the sample permute
            // required before computing the dot product.
            int16x8_t s0123[4], s1234[4], s2345[4], s3456[4];
            transpose_concat_8x4(s0, s1, s2, s3, s0123);
            transpose_concat_8x4(s1, s2, s3, s4, s1234);
            transpose_concat_8x4(s2, s3, s4, s5, s2345);
            transpose_concat_8x4(s3, s4, s5, s6, s3456);

            do {
                int16x8_t s7, s8, s9, s10;
                load_s16_8x4(s, src_stride, &s7, &s8, &s9, &s10);

                int16x8_t s4567[4], s5678[4], s6789[4], s789A[4];
                // Transpose and shuffle the 4 lines that were loaded.
                transpose_concat_8x4(s7, s8, s9, s10, s789A);

                // Merge new data into block from previous iteration.
                svt_tbl2x4_s16(s3456, s789A, merge_block_tbl.val[0], s4567);
                svt_tbl2x4_s16(s3456, s789A, merge_block_tbl.val[1], s5678);
                svt_tbl2x4_s16(s3456, s789A, merge_block_tbl.val[2], s6789);

                uint16x8_t d0 = highbd_convolve8_8_y(s0123, s4567, y_filter, max);
                uint16x8_t d1 = highbd_convolve8_8_y(s1234, s5678, y_filter, max);
                uint16x8_t d2 = highbd_convolve8_8_y(s2345, s6789, y_filter, max);
                uint16x8_t d3 = highbd_convolve8_8_y(s3456, s789A, y_filter, max);

                store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

                // Prepare block for next iteration - re-using as much as possible.
                // Shuffle everything up four rows.
                s0123[0] = s4567[0];
                s0123[1] = s4567[1];
                s0123[2] = s4567[2];
                s0123[3] = s4567[3];
                s1234[0] = s5678[0];
                s1234[1] = s5678[1];
                s1234[2] = s5678[2];
                s1234[3] = s5678[3];
                s2345[0] = s6789[0];
                s2345[1] = s6789[1];
                s2345[2] = s6789[2];
                s2345[3] = s6789[3];
                s3456[0] = s789A[0];
                s3456[1] = s789A[1];
                s3456[2] = s789A[2];
                s3456[3] = s789A[3];

                s += 4 * src_stride;
                d += 4 * dst_stride;
                h -= 4;
            } while (h != 0);
            src += 8;
            dst += 8;
            width -= 8;
        } while (width != 0);
    }
}

static INLINE uint16x4_t highbd_convolve4_4_y(int16x8_t samples[2], int16x8_t filter, uint16x4_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(vdupq_n_s64(0), samples[0], filter, 0);
    int64x2_t sum23 = svt_svdot_lane_s16(vdupq_n_s64(0), samples[1], filter, 0);

    int32x4_t  sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    uint16x4_t res     = vqrshrun_n_s32(sum0123, FILTER_BITS);
    return vmin_u16(res, max);
}

static INLINE uint16x8_t highbd_convolve4_8_y(int16x8_t samples[4], int16x8_t filter, uint16x8_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(vdupq_n_s64(0), samples[0], filter, 0);
    int64x2_t sum23 = svt_svdot_lane_s16(vdupq_n_s64(0), samples[1], filter, 0);
    int64x2_t sum45 = svt_svdot_lane_s16(vdupq_n_s64(0), samples[2], filter, 0);
    int64x2_t sum67 = svt_svdot_lane_s16(vdupq_n_s64(0), samples[3], filter, 0);

    int32x4_t  sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    int32x4_t  sum4567 = vcombine_s32(vmovn_s64(sum45), vmovn_s64(sum67));
    uint16x8_t res     = vcombine_u16(vqrshrun_n_s32(sum0123, FILTER_BITS), vqrshrun_n_s32(sum4567, FILTER_BITS));
    return vminq_u16(res, max);
}

static void highbd_convolve_y_sr_4tap_sve2(const uint16_t *src, ptrdiff_t src_stride, uint16_t *dst,
                                           ptrdiff_t dst_stride, int width, int height, const int16_t *filter_y,
                                           int bd) {
    assert(width >= 4 && height >= 4);

    const int16x8_t y_filter = vcombine_s16(vld1_s16(filter_y + 2), vdup_n_s16(0));

    if (width == 4) {
        const uint16x4_t max = vdup_n_u16((1 << bd) - 1);
        int16_t         *s   = (int16_t *)src;

        int16x4_t s0, s1, s2;
        load_s16_4x3(s, src_stride, &s0, &s1, &s2);
        s += 3 * src_stride;

        do {
            int16x4_t s3, s4, s5, s6;
            load_s16_4x4(s, src_stride, &s3, &s4, &s5, &s6);

            // This operation combines a conventional transpose and the sample permute
            // required before computing the dot product.
            int16x8_t s0123[2], s1234[2], s2345[2], s3456[2];
            transpose_concat_4x4(s0, s1, s2, s3, s0123);
            transpose_concat_4x4(s1, s2, s3, s4, s1234);
            transpose_concat_4x4(s2, s3, s4, s5, s2345);
            transpose_concat_4x4(s3, s4, s5, s6, s3456);

            uint16x4_t d0 = highbd_convolve4_4_y(s0123, y_filter, max);
            uint16x4_t d1 = highbd_convolve4_4_y(s1234, y_filter, max);
            uint16x4_t d2 = highbd_convolve4_4_y(s2345, y_filter, max);
            uint16x4_t d3 = highbd_convolve4_4_y(s3456, y_filter, max);

            store_u16_4x4(dst, dst_stride, d0, d1, d2, d3);

            // Shuffle everything up four rows.
            s0 = s4;
            s1 = s5;
            s2 = s6;

            s += 4 * src_stride;
            dst += 4 * dst_stride;
            height -= 4;
        } while (height != 0);
    } else {
        const uint16x8_t max = vdupq_n_u16((1 << bd) - 1);

        do {
            int       h = height;
            int16_t  *s = (int16_t *)src;
            uint16_t *d = dst;

            int16x8_t s0, s1, s2;
            load_s16_8x3(s, src_stride, &s0, &s1, &s2);
            s += 3 * src_stride;

            do {
                int16x8_t s3, s4, s5, s6;
                load_s16_8x4(s, src_stride, &s3, &s4, &s5, &s6);

                // This operation combines a conventional transpose and the sample
                // permute required before computing the dot product.
                int16x8_t s0123[4], s1234[4], s2345[4], s3456[4];
                transpose_concat_8x4(s0, s1, s2, s3, s0123);
                transpose_concat_8x4(s1, s2, s3, s4, s1234);
                transpose_concat_8x4(s2, s3, s4, s5, s2345);
                transpose_concat_8x4(s3, s4, s5, s6, s3456);

                uint16x8_t d0 = highbd_convolve4_8_y(s0123, y_filter, max);
                uint16x8_t d1 = highbd_convolve4_8_y(s1234, y_filter, max);
                uint16x8_t d2 = highbd_convolve4_8_y(s2345, y_filter, max);
                uint16x8_t d3 = highbd_convolve4_8_y(s3456, y_filter, max);

                store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

                // Shuffle everything up four rows.
                s0 = s4;
                s1 = s5;
                s2 = s6;

                s += 4 * src_stride;
                d += 4 * dst_stride;
                h -= 4;
            } while (h != 0);
            src += 8;
            dst += 8;
            width -= 8;
        } while (width != 0);
    }
}

void svt_av1_highbd_convolve_y_sr_sve2(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h,
                                       const InterpFilterParams *filter_params_x,
                                       const InterpFilterParams *filter_params_y, const int subpel_x_qn,
                                       const int subpel_y_qn, ConvolveParams *conv_params, int bd) {
    if (w == 2 || h == 2) {
        svt_av1_highbd_convolve_y_sr_c(src,
                                       src_stride,
                                       dst,
                                       dst_stride,
                                       w,
                                       h,
                                       filter_params_x,
                                       filter_params_y,
                                       subpel_x_qn,
                                       subpel_y_qn,
                                       conv_params,
                                       bd);
        return;
    }
    const int y_filter_taps = get_filter_tap(filter_params_y, subpel_y_qn);

    if (y_filter_taps == 6) {
        svt_av1_highbd_convolve_y_sr_neon(src,
                                          src_stride,
                                          dst,
                                          dst_stride,
                                          w,
                                          h,
                                          filter_params_x,
                                          filter_params_y,
                                          subpel_x_qn,
                                          subpel_y_qn,
                                          conv_params,
                                          bd);
        return;
    }

    const int      vert_offset  = filter_params_y->taps / 2 - 1;
    const int16_t *y_filter_ptr = av1_get_interp_filter_subpel_kernel(*filter_params_y, subpel_y_qn & SUBPEL_MASK);

    src -= vert_offset * src_stride;

    if (y_filter_taps == 4) {
        highbd_convolve_y_sr_4tap_sve2(src + 2 * src_stride, src_stride, dst, dst_stride, w, h, y_filter_ptr, bd);
        return;
    }

    highbd_convolve_y_sr_8tap_sve2(src, src_stride, dst, dst_stride, w, h, y_filter_ptr, bd);
}

static INLINE uint16x8_t convolve8_8_2d_h(int16x8_t s0[8], int16x8_t filter, int64x2_t offset) {
    int64x2_t sum[8];
    sum[0] = svt_sdotq_s16(offset, s0[0], filter);
    sum[1] = svt_sdotq_s16(offset, s0[1], filter);
    sum[2] = svt_sdotq_s16(offset, s0[2], filter);
    sum[3] = svt_sdotq_s16(offset, s0[3], filter);
    sum[4] = svt_sdotq_s16(offset, s0[4], filter);
    sum[5] = svt_sdotq_s16(offset, s0[5], filter);
    sum[6] = svt_sdotq_s16(offset, s0[6], filter);
    sum[7] = svt_sdotq_s16(offset, s0[7], filter);

    sum[0] = vpaddq_s64(sum[0], sum[1]);
    sum[2] = vpaddq_s64(sum[2], sum[3]);
    sum[4] = vpaddq_s64(sum[4], sum[5]);
    sum[6] = vpaddq_s64(sum[6], sum[7]);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum[0]), vmovn_s64(sum[2]));
    int32x4_t sum4567 = vcombine_s32(vmovn_s64(sum[4]), vmovn_s64(sum[6]));

    return vcombine_u16(vqrshrun_n_s32(sum0123, ROUND0_BITS), vqrshrun_n_s32(sum4567, ROUND0_BITS));
}

static INLINE void highbd_convolve_2d_sr_horiz_8tap_sve2(const uint16_t *src, int src_stride, uint16_t *dst,
                                                         int dst_stride, int width, int height,
                                                         const int16_t *y_filter_ptr, const int x_offset) {
    const int64x2_t offset = vcombine_s64(vdup_n_s64(x_offset), vdup_n_s64(0));

    const int16x8_t filter = vld1q_s16(y_filter_ptr);

    do {
        const int16_t *s = (const int16_t *)src;
        uint16_t      *d = dst;
        int            w = width;

        do {
            int16x8_t s0[8], s1[8], s2[8], s3[8];
            load_s16_8x8(s + 0 * src_stride, 1, &s0[0], &s0[1], &s0[2], &s0[3], &s0[4], &s0[5], &s0[6], &s0[7]);
            load_s16_8x8(s + 1 * src_stride, 1, &s1[0], &s1[1], &s1[2], &s1[3], &s1[4], &s1[5], &s1[6], &s1[7]);
            load_s16_8x8(s + 2 * src_stride, 1, &s2[0], &s2[1], &s2[2], &s2[3], &s2[4], &s2[5], &s2[6], &s2[7]);
            load_s16_8x8(s + 3 * src_stride, 1, &s3[0], &s3[1], &s3[2], &s3[3], &s3[4], &s3[5], &s3[6], &s3[7]);

            uint16x8_t d0 = convolve8_8_2d_h(s0, filter, offset);
            uint16x8_t d1 = convolve8_8_2d_h(s1, filter, offset);
            uint16x8_t d2 = convolve8_8_2d_h(s2, filter, offset);
            uint16x8_t d3 = convolve8_8_2d_h(s3, filter, offset);

            store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

            s += 8;
            d += 8;
            w -= 8;
        } while (w != 0);
        src += 4 * src_stride;
        dst += 4 * dst_stride;
        height -= 4;
    } while (height > 0);
}

static INLINE uint16x4_t convolve4_4_2d_h(int16x8_t s0, int16x8_t filter, int64x2_t offset, uint16x8x2_t permute_tbl) {
    int16x8_t permuted_samples0 = svt_tbl_s16(s0, permute_tbl.val[0]);
    int16x8_t permuted_samples1 = svt_tbl_s16(s0, permute_tbl.val[1]);

    int64x2_t sum01 = svt_svdot_lane_s16(offset, permuted_samples0, filter, 0);
    int64x2_t sum23 = svt_svdot_lane_s16(offset, permuted_samples1, filter, 0);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    return vqrshrun_n_s32(sum0123, ROUND0_BITS);
}

static INLINE uint16x8_t convolve4_8_2d_h(int16x8_t s0[8], int16x8_t filter, int64x2_t offset, uint16x8_t tbl) {
    int64x2_t sum04 = svt_svdot_lane_s16(offset, s0[0], filter, 0);
    int64x2_t sum15 = svt_svdot_lane_s16(offset, s0[1], filter, 0);
    int64x2_t sum26 = svt_svdot_lane_s16(offset, s0[2], filter, 0);
    int64x2_t sum37 = svt_svdot_lane_s16(offset, s0[3], filter, 0);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum04), vmovn_s64(sum15));
    int32x4_t sum4567 = vcombine_s32(vmovn_s64(sum26), vmovn_s64(sum37));

    uint16x8_t res = vcombine_u16(vqrshrun_n_s32(sum0123, ROUND0_BITS), vqrshrun_n_s32(sum4567, ROUND0_BITS));
    return svt_tbl_u16(res, tbl);
}

static INLINE void highbd_convolve_2d_sr_horiz_4tap_sve2(const uint16_t *src, int src_stride, uint16_t *dst,
                                                         int dst_stride, int width, int height,
                                                         const int16_t *x_filter_ptr, const int x_offset) {
    const int64x2_t offset = vdupq_n_s64(x_offset);

    const int16x4_t x_filter = vld1_s16(x_filter_ptr + 2);
    const int16x8_t filter   = vcombine_s16(x_filter, vdup_n_s16(0));

    if (width == 4) {
        const int16_t *s = (const int16_t *)(src);

        uint16x8x2_t permute_tbl = vld1q_u16_x2(kDotProdTbl);

        do {
            int16x8_t s0, s1, s2, s3;
            load_s16_8x4(s, src_stride, &s0, &s1, &s2, &s3);

            uint16x4_t d0 = convolve4_4_2d_h(s0, filter, offset, permute_tbl);
            uint16x4_t d1 = convolve4_4_2d_h(s1, filter, offset, permute_tbl);
            uint16x4_t d2 = convolve4_4_2d_h(s2, filter, offset, permute_tbl);
            uint16x4_t d3 = convolve4_4_2d_h(s3, filter, offset, permute_tbl);

            store_u16_4x4(dst, dst_stride, d0, d1, d2, d3);

            s += 4 * src_stride;
            dst += 4 * dst_stride;
            height -= 4;
        } while (height > 0);
    } else {
        uint16x8_t idx = vld1q_u16(kDeinterleaveTbl);

        do {
            const int16_t *s = (const int16_t *)(src);
            uint16_t      *d = dst;
            int            w = width;

            do {
                int16x8_t s0[8], s1[8], s2[8], s3[8];
                load_s16_8x8(s + 0 * src_stride, 1, &s0[0], &s0[1], &s0[2], &s0[3], &s0[4], &s0[5], &s0[6], &s0[7]);
                load_s16_8x8(s + 1 * src_stride, 1, &s1[0], &s1[1], &s1[2], &s1[3], &s1[4], &s1[5], &s1[6], &s1[7]);
                load_s16_8x8(s + 2 * src_stride, 1, &s2[0], &s2[1], &s2[2], &s2[3], &s2[4], &s2[5], &s2[6], &s2[7]);
                load_s16_8x8(s + 3 * src_stride, 1, &s3[0], &s3[1], &s3[2], &s3[3], &s3[4], &s3[5], &s3[6], &s3[7]);

                uint16x8_t d0 = convolve4_8_2d_h(s0, filter, offset, idx);
                uint16x8_t d1 = convolve4_8_2d_h(s1, filter, offset, idx);
                uint16x8_t d2 = convolve4_8_2d_h(s2, filter, offset, idx);
                uint16x8_t d3 = convolve4_8_2d_h(s3, filter, offset, idx);

                store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

                s += 8;
                d += 8;
                w -= 8;
            } while (w != 0);
            src += 4 * src_stride;
            dst += 4 * dst_stride;
            height -= 4;
        } while (height > 0);
    }
}

static INLINE uint16x4_t highbd_convolve8_4_2d_v(int16x8_t samples_lo[2], int16x8_t samples_hi[2], int16x8_t filter,
                                                 int64x2_t offset, uint16x4_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(offset, samples_lo[0], filter, 0);
    sum01           = svt_svdot_lane_s16(sum01, samples_hi[0], filter, 1);

    int64x2_t sum23 = svt_svdot_lane_s16(offset, samples_lo[1], filter, 0);
    sum23           = svt_svdot_lane_s16(sum23, samples_hi[1], filter, 1);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));

    uint16x4_t res = vqshrun_n_s32(sum0123, 2 * FILTER_BITS - ROUND0_BITS);
    return vmin_u16(res, max);
}

static INLINE uint16x8_t highbd_convolve8_8_2d_v(int16x8_t samples_lo[4], int16x8_t samples_hi[4], int16x8_t filter,
                                                 int64x2_t offset, uint16x8_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(offset, samples_lo[0], filter, 0);
    sum01           = svt_svdot_lane_s16(sum01, samples_hi[0], filter, 1);

    int64x2_t sum23 = svt_svdot_lane_s16(offset, samples_lo[1], filter, 0);
    sum23           = svt_svdot_lane_s16(sum23, samples_hi[1], filter, 1);

    int64x2_t sum45 = svt_svdot_lane_s16(offset, samples_lo[2], filter, 0);
    sum45           = svt_svdot_lane_s16(sum45, samples_hi[2], filter, 1);

    int64x2_t sum67 = svt_svdot_lane_s16(offset, samples_lo[3], filter, 0);
    sum67           = svt_svdot_lane_s16(sum67, samples_hi[3], filter, 1);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    int32x4_t sum4567 = vcombine_s32(vmovn_s64(sum45), vmovn_s64(sum67));

    uint16x8_t res = vcombine_u16(vqshrun_n_s32(sum0123, 2 * FILTER_BITS - ROUND0_BITS),
                                  vqshrun_n_s32(sum4567, 2 * FILTER_BITS - ROUND0_BITS));
    return vminq_u16(res, max);
}

static void highbd_convolve_2d_sr_vert_8tap_sve2(const uint16_t *src, ptrdiff_t src_stride, uint16_t *dst,
                                                 ptrdiff_t dst_stride, int width, int height, const int16_t *filter_y,
                                                 int bd, const int y_offset) {
    assert(width >= 4 && height >= 4);
    const int64x2_t offset   = vdupq_n_s64(y_offset);
    const int16x8_t y_filter = vld1q_s16(filter_y);

    uint16x8x3_t merge_block_tbl = vld1q_u16_x3(kDotProdMergeBlockTbl);
    // Scale indices by size of the true vector length to avoid reading from an
    // 'undefined' portion of a vector on a system with SVE vectors > 128-bit.
    uint16x8_t correction0 = vreinterpretq_u16_u64(vdupq_n_u64(svcnth() * 0x0001000000000000ULL));
    merge_block_tbl.val[0] = vaddq_u16(merge_block_tbl.val[0], correction0);

    uint16x8_t correction1 = vreinterpretq_u16_u64(vdupq_n_u64(svcnth() * 0x0001000100000000ULL));
    merge_block_tbl.val[1] = vaddq_u16(merge_block_tbl.val[1], correction1);

    uint16x8_t correction2 = vreinterpretq_u16_u64(vdupq_n_u64(svcnth() * 0x0001000100010000ULL));
    merge_block_tbl.val[2] = vaddq_u16(merge_block_tbl.val[2], correction2);

    if (width == 4) {
        const uint16x4_t max = vdup_n_u16((1 << bd) - 1);
        int16_t         *s   = (int16_t *)src;

        int16x4_t s0, s1, s2, s3, s4, s5, s6;
        load_s16_4x7(s, src_stride, &s0, &s1, &s2, &s3, &s4, &s5, &s6);
        s += 7 * src_stride;

        // This operation combines a conventional transpose and the sample permute
        // required before computing the dot product.
        int16x8_t s0123[2], s1234[2], s2345[2], s3456[2];
        transpose_concat_4x4(s0, s1, s2, s3, s0123);
        transpose_concat_4x4(s1, s2, s3, s4, s1234);
        transpose_concat_4x4(s2, s3, s4, s5, s2345);
        transpose_concat_4x4(s3, s4, s5, s6, s3456);

        do {
            int16x4_t s7, s8, s9, s10;
            load_s16_4x4(s, src_stride, &s7, &s8, &s9, &s10);

            int16x8_t s4567[2], s5678[2], s6789[2], s789A[2];
            // Transpose and shuffle the 4 lines that were loaded.
            transpose_concat_4x4(s7, s8, s9, s10, s789A);

            // Merge new data into block from previous iteration.
            svt_tbl2x2_s16(s3456, s789A, merge_block_tbl.val[0], s4567);
            svt_tbl2x2_s16(s3456, s789A, merge_block_tbl.val[1], s5678);
            svt_tbl2x2_s16(s3456, s789A, merge_block_tbl.val[2], s6789);

            uint16x4_t d0 = highbd_convolve8_4_2d_v(s0123, s4567, y_filter, offset, max);
            uint16x4_t d1 = highbd_convolve8_4_2d_v(s1234, s5678, y_filter, offset, max);
            uint16x4_t d2 = highbd_convolve8_4_2d_v(s2345, s6789, y_filter, offset, max);
            uint16x4_t d3 = highbd_convolve8_4_2d_v(s3456, s789A, y_filter, offset, max);

            store_u16_4x4(dst, dst_stride, d0, d1, d2, d3);

            // Prepare block for next iteration - re-using as much as possible.
            // Shuffle everything up four rows.
            s0123[0] = s4567[0];
            s0123[1] = s4567[1];
            s1234[0] = s5678[0];
            s1234[1] = s5678[1];
            s2345[0] = s6789[0];
            s2345[1] = s6789[1];
            s3456[0] = s789A[0];
            s3456[1] = s789A[1];

            s += 4 * src_stride;
            dst += 4 * dst_stride;
            height -= 4;
        } while (height != 0);
    } else {
        const uint16x8_t max = vdupq_n_u16((1 << bd) - 1);

        do {
            int       h = height;
            int16_t  *s = (int16_t *)src;
            uint16_t *d = dst;

            int16x8_t s0, s1, s2, s3, s4, s5, s6;
            load_s16_8x7(s, src_stride, &s0, &s1, &s2, &s3, &s4, &s5, &s6);
            s += 7 * src_stride;

            // This operation combines a conventional transpose and the sample permute
            // required before computing the dot product.
            int16x8_t s0123[4], s1234[4], s2345[4], s3456[4];
            transpose_concat_8x4(s0, s1, s2, s3, s0123);
            transpose_concat_8x4(s1, s2, s3, s4, s1234);
            transpose_concat_8x4(s2, s3, s4, s5, s2345);
            transpose_concat_8x4(s3, s4, s5, s6, s3456);

            do {
                int16x8_t s7, s8, s9, s10;
                load_s16_8x4(s, src_stride, &s7, &s8, &s9, &s10);

                int16x8_t s4567[4], s5678[4], s6789[4], s789A[4];
                // Transpose and shuffle the 4 lines that were loaded.
                transpose_concat_8x4(s7, s8, s9, s10, s789A);

                // Merge new data into block from previous iteration.
                svt_tbl2x4_s16(s3456, s789A, merge_block_tbl.val[0], s4567);
                svt_tbl2x4_s16(s3456, s789A, merge_block_tbl.val[1], s5678);
                svt_tbl2x4_s16(s3456, s789A, merge_block_tbl.val[2], s6789);

                uint16x8_t d0 = highbd_convolve8_8_2d_v(s0123, s4567, y_filter, offset, max);
                uint16x8_t d1 = highbd_convolve8_8_2d_v(s1234, s5678, y_filter, offset, max);
                uint16x8_t d2 = highbd_convolve8_8_2d_v(s2345, s6789, y_filter, offset, max);
                uint16x8_t d3 = highbd_convolve8_8_2d_v(s3456, s789A, y_filter, offset, max);

                store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

                // Prepare block for next iteration - re-using as much as possible.
                // Shuffle everything up four rows.
                s0123[0] = s4567[0];
                s0123[1] = s4567[1];
                s0123[2] = s4567[2];
                s0123[3] = s4567[3];
                s1234[0] = s5678[0];
                s1234[1] = s5678[1];
                s1234[2] = s5678[2];
                s1234[3] = s5678[3];
                s2345[0] = s6789[0];
                s2345[1] = s6789[1];
                s2345[2] = s6789[2];
                s2345[3] = s6789[3];
                s3456[0] = s789A[0];
                s3456[1] = s789A[1];
                s3456[2] = s789A[2];
                s3456[3] = s789A[3];

                s += 4 * src_stride;
                d += 4 * dst_stride;
                h -= 4;
            } while (h != 0);
            src += 8;
            dst += 8;
            width -= 8;
        } while (width != 0);
    }
}

static INLINE uint16x4_t highbd_convolve4_4_2d_v(int16x8_t samples[2], int16x8_t filter, int64x2_t offset,
                                                 uint16x4_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(offset, samples[0], filter, 0);
    int64x2_t sum23 = svt_svdot_lane_s16(offset, samples[1], filter, 0);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));

    uint16x4_t res = vqshrun_n_s32(sum0123, 2 * FILTER_BITS - ROUND0_BITS);
    return vmin_u16(res, max);
}

static INLINE uint16x8_t highbd_convolve4_8_2d_v(int16x8_t samples[4], int16x8_t filter, int64x2_t offset,
                                                 uint16x8_t max) {
    int64x2_t sum01 = svt_svdot_lane_s16(offset, samples[0], filter, 0);
    int64x2_t sum23 = svt_svdot_lane_s16(offset, samples[1], filter, 0);
    int64x2_t sum45 = svt_svdot_lane_s16(offset, samples[2], filter, 0);
    int64x2_t sum67 = svt_svdot_lane_s16(offset, samples[3], filter, 0);

    int32x4_t sum0123 = vcombine_s32(vmovn_s64(sum01), vmovn_s64(sum23));
    int32x4_t sum4567 = vcombine_s32(vmovn_s64(sum45), vmovn_s64(sum67));

    uint16x8_t res = vcombine_u16(vqshrun_n_s32(sum0123, 2 * FILTER_BITS - ROUND0_BITS),
                                  vqshrun_n_s32(sum4567, 2 * FILTER_BITS - ROUND0_BITS));
    return vminq_u16(res, max);
}

static void highbd_convolve_2d_sr_vert_4tap_sve2(const uint16_t *src, ptrdiff_t src_stride, uint16_t *dst,
                                                 ptrdiff_t dst_stride, int width, int height, const int16_t *filter_y,
                                                 int bd, const int y_offset) {
    assert(width >= 4 && height >= 4);
    const int64x2_t offset = vdupq_n_s64(y_offset);

    const int16x8_t y_filter = vcombine_s16(vld1_s16(filter_y + 2), vdup_n_s16(0));

    if (width == 4) {
        const uint16x4_t max = vdup_n_u16((1 << bd) - 1);
        int16_t         *s   = (int16_t *)(src);

        int16x4_t s0, s1, s2;
        load_s16_4x3(s, src_stride, &s0, &s1, &s2);
        s += 3 * src_stride;

        do {
            int16x4_t s3, s4, s5, s6;
            load_s16_4x4(s, src_stride, &s3, &s4, &s5, &s6);

            // This operation combines a conventional transpose and the sample permute
            // required before computing the dot product.
            int16x8_t s0123[2], s1234[2], s2345[2], s3456[2];
            transpose_concat_4x4(s0, s1, s2, s3, s0123);
            transpose_concat_4x4(s1, s2, s3, s4, s1234);
            transpose_concat_4x4(s2, s3, s4, s5, s2345);
            transpose_concat_4x4(s3, s4, s5, s6, s3456);

            uint16x4_t d0 = highbd_convolve4_4_2d_v(s0123, y_filter, offset, max);
            uint16x4_t d1 = highbd_convolve4_4_2d_v(s1234, y_filter, offset, max);
            uint16x4_t d2 = highbd_convolve4_4_2d_v(s2345, y_filter, offset, max);
            uint16x4_t d3 = highbd_convolve4_4_2d_v(s3456, y_filter, offset, max);

            store_u16_4x4(dst, dst_stride, d0, d1, d2, d3);

            // Shuffle everything up four rows.
            s0 = s4;
            s1 = s5;
            s2 = s6;

            s += 4 * src_stride;
            dst += 4 * dst_stride;
            height -= 4;
        } while (height != 0);
    } else {
        const uint16x8_t max = vdupq_n_u16((1 << bd) - 1);

        do {
            int       h = height;
            int16_t  *s = (int16_t *)(src);
            uint16_t *d = dst;

            int16x8_t s0, s1, s2;
            load_s16_8x3(s, src_stride, &s0, &s1, &s2);
            s += 3 * src_stride;

            do {
                int16x8_t s3, s4, s5, s6;
                load_s16_8x4(s, src_stride, &s3, &s4, &s5, &s6);

                // This operation combines a conventional transpose and the sample
                // permute required before computing the dot product.
                int16x8_t s0123[4], s1234[4], s2345[4], s3456[4];
                transpose_concat_8x4(s0, s1, s2, s3, s0123);
                transpose_concat_8x4(s1, s2, s3, s4, s1234);
                transpose_concat_8x4(s2, s3, s4, s5, s2345);
                transpose_concat_8x4(s3, s4, s5, s6, s3456);

                uint16x8_t d0 = highbd_convolve4_8_2d_v(s0123, y_filter, offset, max);
                uint16x8_t d1 = highbd_convolve4_8_2d_v(s1234, y_filter, offset, max);
                uint16x8_t d2 = highbd_convolve4_8_2d_v(s2345, y_filter, offset, max);
                uint16x8_t d3 = highbd_convolve4_8_2d_v(s3456, y_filter, offset, max);

                store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

                // Shuffle everything up four rows.
                s0 = s4;
                s1 = s5;
                s2 = s6;

                s += 4 * src_stride;
                d += 4 * dst_stride;
                h -= 4;
            } while (h != 0);
            src += 8;
            dst += 8;
            width -= 8;
        } while (width != 0);
    }
}

void svt_av1_highbd_convolve_2d_sr_sve2(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w,
                                        int h, const InterpFilterParams *filter_params_x,
                                        const InterpFilterParams *filter_params_y, const int subpel_x_qn,
                                        const int subpel_y_qn, ConvolveParams *conv_params, int bd) {
    if (w == 2 || h == 2) {
        svt_av1_highbd_convolve_2d_sr_c(src,
                                        src_stride,
                                        dst,
                                        dst_stride,
                                        w,
                                        h,
                                        filter_params_x,
                                        filter_params_y,
                                        subpel_x_qn,
                                        subpel_y_qn,
                                        conv_params,
                                        bd);
        return;
    }

    DECLARE_ALIGNED(16, uint16_t, im_block[(MAX_SB_SIZE + MAX_FILTER_TAP) * MAX_SB_SIZE]);
    const int x_filter_taps = get_filter_tap(filter_params_x, subpel_x_qn);
    const int y_filter_taps = get_filter_tap(filter_params_y, subpel_y_qn);

    if (x_filter_taps == 6 || y_filter_taps == 6) {
        svt_av1_highbd_convolve_2d_sr_neon(src,
                                           src_stride,
                                           dst,
                                           dst_stride,
                                           w,
                                           h,
                                           filter_params_x,
                                           filter_params_y,
                                           subpel_x_qn,
                                           subpel_y_qn,
                                           conv_params,
                                           bd);
        return;
    }

    const int clamped_x_taps = x_filter_taps < 4 ? 4 : x_filter_taps;
    const int clamped_y_taps = y_filter_taps < 4 ? 4 : y_filter_taps;

    const int im_stride     = MAX_SB_SIZE;
    const int vert_offset   = clamped_y_taps / 2 - 1;
    const int horiz_offset  = clamped_x_taps / 2 - 1;
    const int x_offset      = (1 << (bd + FILTER_BITS - 1));
    const int y_offset_bits = bd + 2 * FILTER_BITS - ROUND0_BITS;
    // The extra shim of (1 << (conv_params->round_1 - 1)) allows us to do a
    // simple shift left instead of a rounding saturating shift left.
    const int y_offset = (1 << (2 * FILTER_BITS - ROUND0_BITS - 1)) - (1 << (y_offset_bits - 1));

    const uint16_t *src_ptr = src - vert_offset * src_stride - horiz_offset;

    const int16_t *x_filter_ptr = av1_get_interp_filter_subpel_kernel(*filter_params_x, subpel_x_qn & SUBPEL_MASK);
    const int16_t *y_filter_ptr = av1_get_interp_filter_subpel_kernel(*filter_params_y, subpel_y_qn & SUBPEL_MASK);
    const int      im_h         = h + clamped_y_taps - 1;

    if (x_filter_taps <= 4) {
        highbd_convolve_2d_sr_horiz_4tap_sve2(
            src_ptr, src_stride, im_block, im_stride, w, im_h, x_filter_ptr, x_offset);
    } else {
        highbd_convolve_2d_sr_horiz_8tap_sve2(
            src_ptr, src_stride, im_block, im_stride, w, im_h, x_filter_ptr, x_offset);
    }

    if (y_filter_taps <= 4) {
        highbd_convolve_2d_sr_vert_4tap_sve2(im_block, im_stride, dst, dst_stride, w, h, y_filter_ptr, bd, y_offset);
    } else {
        highbd_convolve_2d_sr_vert_8tap_sve2(im_block, im_stride, dst, dst_stride, w, h, y_filter_ptr, bd, y_offset);
    }
}
