/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Register offsets used by the direct-MMIO paths of the driver
 *              (everything else goes through AtomBIOS command tables).
 *              All values are transcribed from the Linux v6.6 radeon driver
 *              headers named next to each block; do not edit by memory.
 * COPYRIGHT:   Copyright 2008-2013 Advanced Micro Devices, Inc.
 *              Copyright 2008 Red Hat Inc.
 *              Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#ifndef _RADEON_REGS_H_
#define _RADEON_REGS_H_

/* --- drivers/gpu/drm/radeon/radeon_reg.h --- */
#define RADEON_MM_INDEX                     0x0000
#define RADEON_MM_DATA                      0x0004

/* --- drivers/gpu/drm/radeon/rv515d.h ---
 * MC indirect access pair; still routed this way by the r5xx+ register
 * accessor init in radeon_asic.c (used by ATOM_ARG_MC interpreter args). */
#define MC_IND_INDEX                        0x0070
#define        MC_IND_WR_EN                 (1 << 24)
#define MC_IND_DATA                         0x0074

/* --- drivers/gpu/drm/radeon/avivod.h --- */
#define VGA_RENDER_CONTROL                  0x0300
#define        VGA_VSTATUS_CNTL_MASK        0x00030000
#define D1VGA_CONTROL                       0x0330
#define        DVGA_CONTROL_MODE_ENABLE     (1 << 0)
#define        DVGA_CONTROL_TIMING_SELECT   (1 << 8)
#define D2VGA_CONTROL                       0x0338

/* --- drivers/gpu/drm/radeon/evergreen_reg.h --- */
#define EVERGREEN_D3VGA_CONTROL                         0x3e0
#define EVERGREEN_D4VGA_CONTROL                         0x3e4
#define EVERGREEN_D5VGA_CONTROL                         0x3e8
#define EVERGREEN_D6VGA_CONTROL                         0x3ec

#define EVERGREEN_GRPH_ENABLE                           0x6800
#define EVERGREEN_GRPH_CONTROL                          0x6804
#define        EVERGREEN_GRPH_DEPTH(x)                  (((x) & 0x3) << 0)
#define        EVERGREEN_GRPH_DEPTH_8BPP                0
#define        EVERGREEN_GRPH_DEPTH_16BPP               1
#define        EVERGREEN_GRPH_DEPTH_32BPP               2
#define        EVERGREEN_GRPH_NUM_BANKS(x)              (((x) & 0x3) << 2)
#define        EVERGREEN_ADDR_SURF_2_BANK               0
#define        EVERGREEN_ADDR_SURF_4_BANK               1
#define        EVERGREEN_ADDR_SURF_8_BANK               2
#define        EVERGREEN_ADDR_SURF_16_BANK              3
#define        EVERGREEN_GRPH_FORMAT(x)                 (((x) & 0x7) << 8)
/* 16 BPP */
#define        EVERGREEN_GRPH_FORMAT_ARGB1555           0
#define        EVERGREEN_GRPH_FORMAT_ARGB565            1
#define        EVERGREEN_GRPH_FORMAT_ARGB4444           2
/* 32 BPP */
#define        EVERGREEN_GRPH_FORMAT_ARGB8888           0
#define        EVERGREEN_GRPH_FORMAT_ARGB2101010        1
#define        EVERGREEN_GRPH_ARRAY_MODE(x)             (((x) & 0x7) << 20)
#define        EVERGREEN_GRPH_ARRAY_LINEAR_GENERAL      0
#define        EVERGREEN_GRPH_ARRAY_LINEAR_ALIGNED      1
#define EVERGREEN_GRPH_LUT_10BIT_BYPASS_CONTROL         0x6808
#define        EVERGREEN_LUT_10BIT_BYPASS_EN            (1 << 8)
#define EVERGREEN_GRPH_SWAP_CONTROL                     0x680c
#define        EVERGREEN_GRPH_ENDIAN_SWAP(x)            (((x) & 0x3) << 0)
#define        EVERGREEN_GRPH_ENDIAN_NONE               0
#define EVERGREEN_GRPH_PRIMARY_SURFACE_ADDRESS          0x6810
#define EVERGREEN_GRPH_SECONDARY_SURFACE_ADDRESS        0x6814
#define        EVERGREEN_GRPH_SURFACE_ADDRESS_MASK      0xffffff00
#define EVERGREEN_GRPH_PITCH                            0x6818
#define EVERGREEN_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH     0x681c
#define EVERGREEN_GRPH_SECONDARY_SURFACE_ADDRESS_HIGH   0x6820
#define EVERGREEN_GRPH_SURFACE_OFFSET_X                 0x6824
#define EVERGREEN_GRPH_SURFACE_OFFSET_Y                 0x6828
#define EVERGREEN_GRPH_X_START                          0x682c
#define EVERGREEN_GRPH_Y_START                          0x6830
#define EVERGREEN_GRPH_X_END                            0x6834
#define EVERGREEN_GRPH_Y_END                            0x6838
#define EVERGREEN_GRPH_FLIP_CONTROL                     0x6848

#define EVERGREEN_DATA_FORMAT                           0x6b00
#define        EVERGREEN_INTERLEAVE_EN                  (1 << 0)
#define EVERGREEN_DESKTOP_HEIGHT                        0x6b04

#define EVERGREEN_VIEWPORT_START                        0x6d70
#define EVERGREEN_VIEWPORT_SIZE                         0x6d74

#define EVERGREEN_CRTC0_REGISTER_OFFSET                 (0x6df0 - 0x6df0)
#define EVERGREEN_CRTC1_REGISTER_OFFSET                 (0x79f0 - 0x6df0)
#define EVERGREEN_CRTC2_REGISTER_OFFSET                 (0x105f0 - 0x6df0)
#define EVERGREEN_CRTC3_REGISTER_OFFSET                 (0x111f0 - 0x6df0)
#define EVERGREEN_CRTC4_REGISTER_OFFSET                 (0x11df0 - 0x6df0)
#define EVERGREEN_CRTC5_REGISTER_OFFSET                 (0x129f0 - 0x6df0)

#define EVERGREEN_CRTC_CONTROL                          0x6e70
#define        EVERGREEN_CRTC_MASTER_EN                 (1 << 0)
#define EVERGREEN_MASTER_UPDATE_MODE                    0x6ef8

/* --- drivers/gpu/drm/radeon/si_reg.h --- */
#define SI_GRPH_PIPE_CONFIG(x)                          (((x) & 0x1f) << 24)

/* --- drivers/gpu/drm/radeon/sid.h --- */
#define SI_ADDR_SURF_P4_8x16                            4
#define SI_ADDR_SURF_P8_32x32_8x16                      10

/* sid.h/evergreend.h agree on these offsets for SI and evergreen/NI(ARUBA) */
#define MC_VM_FB_LOCATION                               0x2024
#define CONFIG_MEMSIZE                                  0x5428

#endif /* _RADEON_REGS_H_ */
