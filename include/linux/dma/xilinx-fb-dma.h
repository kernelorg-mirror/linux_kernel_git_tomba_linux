/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx Framebuffer DMA
 *
 * Copyright (C) 2017 - 2024 Xilinx, Inc.
 */

#ifndef __XILINX_FB_DMA_H
#define __XILINX_FB_DMA_H

#include <linux/types.h>
#include <linux/bits.h>

struct device;
struct xilinx_fb_dma;
struct xilinx_fb_dma_descriptor;

typedef void (*xilinx_fb_dma_callback)(void *data);

/**
 * enum xilinx_fb_dma_early_cb_mode - Modes to enable early callback
 * @XDMA_NO_EARLY_CALLBACK : No early callback.
 * @XDMA_EARLY_CALLBACK : To avoid first frame delay.
 * @XDMA_EARLY_CALLBACK_START_DESC : Give callback at start of descriptor processing.
 */
enum xilinx_fb_dma_early_cb_mode {
	XDMA_NO_EARLY_CALLBACK = 0,
	XDMA_EARLY_CALLBACK,
	XDMA_EARLY_CALLBACK_START_DESC,
};

/**
 * enum xilinx_fb_dma_operation_mode - FB IP control register field settings to select mode
 * @DEFAULT : Use default mode, No explicit bit field settings required.
 * @AUTO_RESTART : Use auto-restart mode by setting BIT(7) of control register.
 */
enum xilinx_fb_dma_operation_mode {
	XDMA_MODE_DEFAULT = 0,
	XDMA_MODE_AUTO_RESTART,
};

enum xilinx_fb_dma_direction {
	XDMA_TO_MEM,
	XDMA_FROM_MEM,
};

struct xilinx_fb_dma_plane {
	dma_addr_t addr;
};

struct xilinx_fb_dma_params {
	void (*callback)(void *data);
	void *callback_data;

	enum xilinx_fb_dma_direction direction;
	// XXX add fmt here?

	u32 width;
	u32 height;
	u32 bpp;
	u32 bytesperline;

	unsigned int num_planes;
	struct xilinx_fb_dma_plane planes[3];
};

struct xilinx_fb_dma_ops {
	struct xilinx_fb_dma_descriptor *(*prepare)(struct xilinx_fb_dma *dma,
						    struct xilinx_fb_dma_params *params);
	void (*submit)(struct xilinx_fb_dma *dma,
		       struct xilinx_fb_dma_descriptor *desc);

	void (*async_issue_pending)(struct xilinx_fb_dma *dma);
	void (*terminate_all)(struct xilinx_fb_dma *dma);
	void (*synchronize)(struct xilinx_fb_dma *dma);

	void (*set_mode)(struct xilinx_fb_dma *dma,
			 enum xilinx_fb_dma_operation_mode mode);

	void (*drm_config)(struct xilinx_fb_dma *dma, u32 drm_fourcc);
	void (*v4l2_config)(struct xilinx_fb_dma *dma, u32 v4l2_fourcc);
	int (*get_drm_vid_fmts)(struct xilinx_fb_dma *dma, u32 *fmt_cnt,
				u32 **fmts);
	int (*get_v4l2_vid_fmts)(struct xilinx_fb_dma *dma, u32 *fmt_cnt,
				 u32 **fmts);

	int (*get_fid)(struct xilinx_fb_dma *dma,
		       struct xilinx_fb_dma_descriptor *desc, u32 *fid);
	int (*set_fid)(struct xilinx_fb_dma *dma,
		       struct xilinx_fb_dma_descriptor *desc, u32 fid);
	int (*get_fid_err_flag)(struct xilinx_fb_dma *dma, u32 *fid_err_flag);
	int (*get_fid_out)(struct xilinx_fb_dma *dma, u32 *fid_out_val);

	int (*get_earlycb)(struct xilinx_fb_dma *dma,
			   struct xilinx_fb_dma_descriptor *desc,
			   enum xilinx_fb_dma_early_cb_mode *earlycb);
	int (*set_earlycb)(struct xilinx_fb_dma *dma,
			   struct xilinx_fb_dma_descriptor *desc,
			   enum xilinx_fb_dma_early_cb_mode earlycb);
};

struct xilinx_fb_dma {
	const struct xilinx_fb_dma_ops *ops;
	u32 copy_align;
	u32 directions;
	u32 ppc;
};

/* Exported functions for DMA consumers */

struct xilinx_fb_dma *xilinx_fb_dma_request(struct device *dev,
					    const char *dma_name);
void xilinx_fb_dma_release(struct xilinx_fb_dma *dma);

#endif /* __XILINX_FB_DMA_H */
