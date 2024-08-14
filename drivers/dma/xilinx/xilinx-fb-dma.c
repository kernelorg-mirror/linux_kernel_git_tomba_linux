// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Xilinx Framebuffer DMA
 *
 * Copyright (C) 2016 - 2024 Xilinx, Inc.
 *
 * Authors: Radhey Shyam Pandey <radheys@xilinx.com>
 *          John Nichols <jnichol@xilinx.com>
 *          Jeffrey Mouroux <jmouroux@xilinx.com>
 *
 * The AXI Framebuffer core is a soft Xilinx IP core that provides
 * high-bandwidth direct memory access between memory and AXI4-Stream.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma/xilinx-fb-dma.h>
#include <linux/videodev2.h>

#include <drm/drm_fourcc.h>

/* Register/Descriptor Offsets */
#define XILINX_FRMBUF_CTRL_OFFSET		0x00
#define XILINX_FRMBUF_GIE_OFFSET		0x04
#define XILINX_FRMBUF_IE_OFFSET			0x08
#define XILINX_FRMBUF_ISR_OFFSET		0x0c
#define XILINX_FRMBUF_WIDTH_OFFSET		0x10
#define XILINX_FRMBUF_HEIGHT_OFFSET		0x18
#define XILINX_FRMBUF_STRIDE_OFFSET		0x20
#define XILINX_FRMBUF_FMT_OFFSET		0x28
#define XILINX_FRMBUF_ADDR_OFFSET		0x30
#define XILINX_FRMBUF_ADDR2_OFFSET		0x3c
#define XILINX_FRMBUF_FID_OFFSET		0x48
#define XILINX_FRMBUF_FID_MODE_OFFSET		0x50
#define XILINX_FRMBUF_ADDR3_OFFSET		0x54
#define XILINX_FRMBUF_FID_ERR_OFFSET		0x58
#define XILINX_FRMBUF_FID_OUT_OFFSET		0x60
#define XILINX_FRMBUF_RD_ADDR3_OFFSET		0x74

/* Control Registers */
#define XILINX_FRMBUF_CTRL_AP_START		BIT(0)
#define XILINX_FRMBUF_CTRL_AP_DONE		BIT(1)
#define XILINX_FRMBUF_CTRL_AP_IDLE		BIT(2)
#define XILINX_FRMBUF_CTRL_AP_READY		BIT(3)
#define XILINX_FRMBUF_CTRL_FLUSH		BIT(5)
#define XILINX_FRMBUF_CTRL_FLUSH_DONE		BIT(6)
#define XILINX_FRMBUF_CTRL_AUTO_RESTART		BIT(7)
#define XILINX_FRMBUF_GIE_EN			BIT(0)

/* Interrupt Status and Control */
#define XILINX_FRMBUF_IE_AP_DONE		BIT(0)
#define XILINX_FRMBUF_IE_AP_READY		BIT(1)

#define XILINX_FRMBUF_ISR_AP_DONE_IRQ		BIT(0)
#define XILINX_FRMBUF_ISR_AP_READY_IRQ		BIT(1)

#define XILINX_FRMBUF_ISR_ALL_IRQ_MASK	\
		(XILINX_FRMBUF_ISR_AP_DONE_IRQ | \
		XILINX_FRMBUF_ISR_AP_READY_IRQ)

/* Video Format Register Settings */
#define XILINX_FRMBUF_FMT_RGBX8			10
#define XILINX_FRMBUF_FMT_YUVX8			11
#define XILINX_FRMBUF_FMT_YUYV8			12
#define XILINX_FRMBUF_FMT_RGBA8			13
#define XILINX_FRMBUF_FMT_YUVA8			14
#define XILINX_FRMBUF_FMT_RGBX10		15
#define XILINX_FRMBUF_FMT_YUVX10		16
#define XILINX_FRMBUF_FMT_Y_UV8			18
#define XILINX_FRMBUF_FMT_Y_UV8_420		19
#define XILINX_FRMBUF_FMT_RGB8			20
#define XILINX_FRMBUF_FMT_YUV8			21
#define XILINX_FRMBUF_FMT_Y_UV10		22
#define XILINX_FRMBUF_FMT_Y_UV10_420		23
#define XILINX_FRMBUF_FMT_Y8			24
#define XILINX_FRMBUF_FMT_Y10			25
#define XILINX_FRMBUF_FMT_BGRA8			26
#define XILINX_FRMBUF_FMT_BGRX8			27
#define XILINX_FRMBUF_FMT_UYVY8			28
#define XILINX_FRMBUF_FMT_BGR8			29
#define XILINX_FRMBUF_FMT_RGBX12		30
#define XILINX_FRMBUF_FMT_RGB16			35
#define XILINX_FRMBUF_FMT_Y_U_V8		42
#define XILINX_FRMBUF_FMT_Y_U_V10		43

/* FID Register */
#define XILINX_FRMBUF_FID_MASK			BIT(0)

/* FID ERR Register */
#define XILINX_FRMBUF_FID_ERR_MASK		BIT(0)
#define XILINX_FRMBUF_FID_OUT_MASK		BIT(0)

#define XILINX_FRMBUF_ALIGN_MUL			8

#define WAIT_FOR_FLUSH_DONE			25

/* Pixels per clock property flag */
#define XILINX_PPC_PROP				BIT(0)
#define XILINX_FLUSH_PROP			BIT(1)
#define XILINX_FID_PROP				BIT(2)
#define XILINX_CLK_PROP				BIT(3)
#define XILINX_THREE_PLANES_PROP		BIT(4)
#define XILINX_FID_ERR_DETECT_PROP		BIT(5)

#define XILINX_FRMBUF_MIN_HEIGHT		(64)
#define XILINX_FRMBUF_MIN_WIDTH			(64)

/**
 * struct xilinx_fb_dma_descriptor - Per Transaction structure
 * @node: Node in the channel descriptors list
 * @fid: Field ID of buffer
 * @earlycb_mode: Whether the callback should be called when in staged state
 * @luma_plane_addr: Luma or packed plane buffer address
 * @chroma_plane_addr: Chroma plane buffer address
 * @vsize: Vertical Size
 * @hsize: Horizontal Size
 * @stride: Number of bytes between the first
 *	    pixels of each horizontal line
 */
struct xilinx_fb_dma_descriptor {
	void (*callback)(void *data);
	void *callback_param;

	struct list_head node;
	u32 fid;
	enum xilinx_fb_dma_early_cb_mode earlycb_mode;

	dma_addr_t luma_plane_addr;
	dma_addr_t chroma_plane_addr[2];
	u32 vsize;
	u32 hsize;
	u32 stride;
};

/**
 * enum vid_frmwork_type - Linux video framework type
 * @XDMA_DRM: fourcc is of type DRM
 * @XDMA_V4L2: fourcc is of type V4L2
 */
enum xilinx_fb_dma_framework_type {
	XDMA_DRM = 0,
	XDMA_V4L2,
};

/**
 * enum fid_modes - FB IP fid mode register settings to select mode
 * @FID_MODE_0: carries the fid value shared by application
 * @FID_MODE_1: sets the fid after first frame
 * @FID_MODE_2: sets the fid after second frame
 */
enum xilinx_fb_dma_fid_modes {
	FID_MODE_0 = 0,
	FID_MODE_1 = 1,
	FID_MODE_2 = 2,
};

/**
 * struct xilinx_fb_dma_format_desc - lookup table to match fourcc to format
 * @dts_name: Device tree name for this entry.
 * @id: Format ID
 * @bpw: Bits of pixel data + padding in a 32-bit word (luma plane for semi-pl)
 * @ppw: Number of pixels represented in a 32-bit word (luma plane for semi-pl)
 * @num_planes: Expected number of plane buffers in framebuffer for this format
 * @drm_fmt: DRM video framework equivalent fourcc code
 * @v4l2_fmt: Video 4 Linux framework equivalent fourcc code
 * @fmt_bitmask: Flag identifying this format in device-specific "enabled"
 *	bitmap
 */
struct xilinx_fb_dma_format_desc {
	const char *dts_name;
	u32 id;
	u32 bpw;
	u32 ppw;
	u32 num_planes;
	u32 drm_fmt;
	u32 v4l2_fmt;
	u32 fmt_bitmask;
};

static const struct xilinx_fb_dma_format_desc xilinx_frmbuf_formats[] = {
	{
		.dts_name = "xbgr8888",
		.id = XILINX_FRMBUF_FMT_RGBX8,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_XBGR8888,
		.v4l2_fmt = V4L2_PIX_FMT_RGBX32,
		.fmt_bitmask = BIT(0),
	},
	{
		.dts_name = "xbgr2101010",
		.id = XILINX_FRMBUF_FMT_RGBX10,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_XBGR2101010,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(1),
	},
	{
		.dts_name = "xrgb8888",
		.id = XILINX_FRMBUF_FMT_BGRX8,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_XRGB8888,
		.v4l2_fmt = V4L2_PIX_FMT_XBGR32,
		.fmt_bitmask = BIT(2),
	},
	{
		.dts_name = "xvuy8888",
		.id = XILINX_FRMBUF_FMT_YUVX8,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_XVUY8888,
		.v4l2_fmt = V4L2_PIX_FMT_YUVX32,
		.fmt_bitmask = BIT(5),
	},
	{
		.dts_name = "vuy888",
		.id = XILINX_FRMBUF_FMT_YUV8,
		.bpw = 24,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_VUY888,
		.v4l2_fmt = V4L2_PIX_FMT_YUV24,
		.fmt_bitmask = BIT(6),
	},
	{
		.dts_name = "yuvx2101010",
		.id = XILINX_FRMBUF_FMT_YUVX10,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = 0,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(7),
	},
	{
		.dts_name = "yuyv",
		.id = XILINX_FRMBUF_FMT_YUYV8,
		.bpw = 32,
		.ppw = 2,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_YUYV,
		.v4l2_fmt = V4L2_PIX_FMT_YUYV,
		.fmt_bitmask = BIT(8),
	},
	{
		.dts_name = "uyvy",
		.id = XILINX_FRMBUF_FMT_UYVY8,
		.bpw = 32,
		.ppw = 2,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_UYVY,
		.v4l2_fmt = V4L2_PIX_FMT_UYVY,
		.fmt_bitmask = BIT(9),
	},
	{
		.dts_name = "nv16",
		.id = XILINX_FRMBUF_FMT_Y_UV8,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 2,
		.drm_fmt = DRM_FORMAT_NV16,
		.v4l2_fmt = V4L2_PIX_FMT_NV16M,
		.fmt_bitmask = BIT(11),
	},
	{
		.dts_name = "nv16",
		.id = XILINX_FRMBUF_FMT_Y_UV8,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 2,
		.drm_fmt = 0,
		.v4l2_fmt = V4L2_PIX_FMT_NV16,
		.fmt_bitmask = BIT(11),
	},
	{
		.dts_name = "nv12",
		.id = XILINX_FRMBUF_FMT_Y_UV8_420,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 2,
		.drm_fmt = DRM_FORMAT_NV12,
		.v4l2_fmt = V4L2_PIX_FMT_NV12M,
		.fmt_bitmask = BIT(12),
	},
	{
		.dts_name = "nv12",
		.id = XILINX_FRMBUF_FMT_Y_UV8_420,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 2,
		.drm_fmt = 0,
		.v4l2_fmt = V4L2_PIX_FMT_NV12,
		.fmt_bitmask = BIT(12),
	},
	{
		.dts_name = "xv15",
		.id = XILINX_FRMBUF_FMT_Y_UV10_420,
		.bpw = 32,
		.ppw = 3,
		.num_planes = 2,
		.drm_fmt = 0,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(13),
	},
	{
		.dts_name = "xv15",
		.id = XILINX_FRMBUF_FMT_Y_UV10_420,
		.bpw = 32,
		.ppw = 3,
		.num_planes = 2,
		.drm_fmt = 0,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(13),
	},
	{
		.dts_name = "xv20",
		.id = XILINX_FRMBUF_FMT_Y_UV10,
		.bpw = 32,
		.ppw = 3,
		.num_planes = 2,
		.drm_fmt = 0,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(14),
	},
	{
		.dts_name = "xv20",
		.id = XILINX_FRMBUF_FMT_Y_UV10,
		.bpw = 32,
		.ppw = 3,
		.num_planes = 2,
		.drm_fmt = 0,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(14),
	},
	{
		.dts_name = "bgr888",
		.id = XILINX_FRMBUF_FMT_RGB8,
		.bpw = 24,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_BGR888,
		.v4l2_fmt = V4L2_PIX_FMT_RGB24,
		.fmt_bitmask = BIT(15),
	},
	{
		.dts_name = "y8",
		.id = XILINX_FRMBUF_FMT_Y8,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 1,
		.drm_fmt = 0,
		.v4l2_fmt = V4L2_PIX_FMT_GREY,
		.fmt_bitmask = BIT(16),
	},
	{
		.dts_name = "y10",
		.id = XILINX_FRMBUF_FMT_Y10,
		.bpw = 32,
		.ppw = 3,
		.num_planes = 1,
		.drm_fmt = 0,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(17),
	},
	{
		.dts_name = "rgb888",
		.id = XILINX_FRMBUF_FMT_BGR8,
		.bpw = 24,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_RGB888,
		.v4l2_fmt = V4L2_PIX_FMT_BGR24,
		.fmt_bitmask = BIT(18),
	},
	{
		.dts_name = "abgr8888",
		.id = XILINX_FRMBUF_FMT_RGBA8,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_ABGR8888,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(19),
	},
	{
		.dts_name = "argb8888",
		.id = XILINX_FRMBUF_FMT_BGRA8,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_ARGB8888,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(20),
	},
	{
		.dts_name = "avuy8888",
		.id = XILINX_FRMBUF_FMT_YUVA8,
		.bpw = 32,
		.ppw = 1,
		.num_planes = 1,
		.drm_fmt = DRM_FORMAT_AVUY8888,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(21),
	},
	{
		.dts_name = "xbgr2121212",
		.id = XILINX_FRMBUF_FMT_RGBX12,
		.bpw = 40,
		.ppw = 1,
		.num_planes = 1,
		.v4l2_fmt = 0,
		.fmt_bitmask = BIT(22),
	},
	{
		.dts_name = "rgb16",
		.id = XILINX_FRMBUF_FMT_RGB16,
		.bpw = 48,
		.ppw = 1,
		.num_planes = 1,
		.v4l2_fmt = V4L2_PIX_FMT_BGR48,
		.fmt_bitmask = BIT(23),
	},
	{
		.dts_name = "y_u_v8",
		.id = XILINX_FRMBUF_FMT_Y_U_V8,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 3,
		.v4l2_fmt = V4L2_PIX_FMT_YUV444M,
		.drm_fmt = DRM_FORMAT_YUV444,
		.fmt_bitmask = BIT(24),
	},
	{
		.dts_name = "y_u_v8",
		.id = XILINX_FRMBUF_FMT_Y_U_V8,
		.bpw = 32,
		.ppw = 4,
		.num_planes = 3,
		.v4l2_fmt = 0,
		.drm_fmt = DRM_FORMAT_YUV444,
		.fmt_bitmask = BIT(24),
	},
	{
		.dts_name = "y_u_v10",
		.id = XILINX_FRMBUF_FMT_Y_U_V10,
		.bpw = 32,
		.ppw = 3,
		.num_planes = 3,
		.v4l2_fmt = 0,
		.drm_fmt = 0,
		.fmt_bitmask = BIT(25),
	},
};

/**
 * struct xilinx_fb_hw_info - dt or IP property structure
 * @direction: dma transfer mode and direction
 * @flags: Bitmask of properties enabled in IP or dt
 */
struct xilinx_fb_hw_info {
	enum xilinx_fb_dma_direction direction;
	u32 flags;
};

/**
 * struct xilinx_fb_dma_chan - Driver specific dma channel structure
 * @xdev: Driver specific device structure
 * @lock: Descriptor operation lock
 * @pending_list: Descriptors waiting
 * @done_list: Complete descriptors
 * @staged_desc: Next buffer to be programmed
 * @active_desc: Currently active buffer being read/written to
 * @dev: The dma device
 * @irq: Channel IRQ
 * @direction: Transfer direction
 * @idle: Channel idle state
 * @tasklet: Cleanup work after irq
 * @vid_fmt: Reference to currently assigned video format description
 * @hw_fid: FID enabled in hardware flag
 * @mode: Select operation mode
 * @fid_err_flag: Field id error detection flag
 * @fid_out_val: Field id out val
 * @fid_mode: Select fid mode
 */
struct xilinx_fb_dma_chan {
	struct xilinx_fb_dma_device *xdev;
	/* Descriptor operation lock */
	spinlock_t lock;
	struct list_head pending_list;
	struct list_head done_list;
	struct xilinx_fb_dma_descriptor *staged_desc;
	struct xilinx_fb_dma_descriptor *active_desc;
	int irq;
	enum xilinx_fb_dma_direction direction;
	bool idle;
	struct tasklet_struct tasklet;
	const struct xilinx_fb_dma_format_desc *vid_fmt;
	bool hw_fid;
	enum xilinx_fb_dma_operation_mode mode;
	u8 fid_err_flag;
	u8 fid_out_val;
	enum xilinx_fb_dma_fid_modes fid_mode;
};

/**
 * struct xilinx_fb_dma_device - dma device structure
 * @regs: I/O mapped base address
 * @dev: Device Structure
 * @common: DMA device structure
 * @chan: Driver specific dma channel
 * @rst_gpio: GPIO reset
 * @enabled_vid_fmts: Bitmask of video formats enabled in hardware
 * @drm_memory_fmts: Array of supported DRM fourcc codes
 * @drm_fmt_cnt: Count of supported DRM fourcc codes
 * @v4l2_memory_fmts: Array of supported V4L2 fourcc codes
 * @v4l2_fmt_cnt: Count of supported V4L2 fourcc codes
 * @cfg: Pointer to Framebuffer Feature config struct
 * @max_width: Maximum pixel width supported in IP.
 * @max_height: Maximum number of lines supported in IP.
 * @ppc: Pixels per clock supported in IP.
 * @ap_clk: Video core clock
 */
struct xilinx_fb_dma_device {
	void __iomem *regs;
	struct device *dev;
	struct xilinx_fb_dma dma_dev;
	struct xilinx_fb_dma_chan chan;
	struct gpio_desc *rst_gpio;

	u32 enabled_vid_fmts;
	u32 drm_memory_fmts[ARRAY_SIZE(xilinx_frmbuf_formats)];
	u32 drm_fmt_cnt;
	u32 v4l2_memory_fmts[ARRAY_SIZE(xilinx_frmbuf_formats)];
	u32 v4l2_fmt_cnt;

	const struct xilinx_fb_hw_info *hw_info;
	u32 max_width;
	u32 max_height;
	u32 ppc;
	struct clk *ap_clk;
	//struct clk *axi_clk;

	bool use_64_addr;

	struct list_head list;
	bool reserved;
};

static const struct xilinx_fb_hw_info xlnx_fbwr_hw_info_v20 = {
	.direction = XDMA_TO_MEM,
};

static const struct xilinx_fb_hw_info xlnx_fbwr_hw_info_v21 = {
	.direction = XDMA_TO_MEM,
	.flags = XILINX_PPC_PROP | XILINX_FLUSH_PROP | XILINX_FID_PROP |
		 XILINX_CLK_PROP,
};

static const struct xilinx_fb_hw_info xlnx_fbwr_hw_info_v22 = {
	.direction = XDMA_TO_MEM,
	.flags = XILINX_PPC_PROP | XILINX_FLUSH_PROP | XILINX_FID_PROP |
		 XILINX_CLK_PROP | XILINX_THREE_PLANES_PROP,
};

static const struct xilinx_fb_hw_info xlnx_fbrd_hw_info_v20 = {
	.direction = XDMA_FROM_MEM,
};

static const struct xilinx_fb_hw_info xlnx_fbrd_hw_info_v21 = {
	.direction = XDMA_FROM_MEM,
	.flags = XILINX_PPC_PROP | XILINX_FLUSH_PROP | XILINX_FID_PROP |
		 XILINX_CLK_PROP,
};

static const struct xilinx_fb_hw_info xlnx_fbrd_hw_info_v22 = {
	.direction = XDMA_FROM_MEM,
	.flags = XILINX_PPC_PROP | XILINX_FLUSH_PROP | XILINX_FID_PROP |
		 XILINX_CLK_PROP | XILINX_THREE_PLANES_PROP |
		 XILINX_FID_ERR_DETECT_PROP,
};

static const struct of_device_id xilinx_fb_dma_of_ids[] = {
	{ .compatible = "xlnx,axi-frmbuf-wr-v2",
	  .data = &xlnx_fbwr_hw_info_v20 },
	{ .compatible = "xlnx,axi-frmbuf-wr-v2.1",
	  .data = &xlnx_fbwr_hw_info_v21 },
	{ .compatible = "xlnx,axi-frmbuf-wr-v2.2",
	  .data = &xlnx_fbwr_hw_info_v22 },
	{ .compatible = "xlnx,axi-frmbuf-rd-v2",
	  .data = &xlnx_fbrd_hw_info_v20 },
	{ .compatible = "xlnx,axi-frmbuf-rd-v2.1",
	  .data = &xlnx_fbrd_hw_info_v21 },
	{ .compatible = "xlnx,axi-frmbuf-rd-v2.2",
	  .data = &xlnx_fbrd_hw_info_v22 },
	{ /* end of list */ }
};

/* Xilinx FB DMA registration / reservation */

static DEFINE_MUTEX(xilinx_fb_dma_dma_list_mutex);
static LIST_HEAD(xilinx_fb_dma_registered_dmas);

static inline struct xilinx_fb_dma_device *to_xilinx_fb_dma_device(struct xilinx_fb_dma *dma)
{
	return container_of(dma, struct xilinx_fb_dma_device, dma_dev);
}

static void xilinx_fb_dma_register(struct xilinx_fb_dma_device *dma)
{
	mutex_lock(&xilinx_fb_dma_dma_list_mutex);
	list_add(&dma->list, &xilinx_fb_dma_registered_dmas);
	mutex_unlock(&xilinx_fb_dma_dma_list_mutex);
}

static void xilinx_fb_dma_unregister(struct xilinx_fb_dma_device *dma)
{
	mutex_lock(&xilinx_fb_dma_dma_list_mutex);
	list_del(&dma->list);
	mutex_unlock(&xilinx_fb_dma_dma_list_mutex);
}

static struct xilinx_fb_dma_device *xilinx_fb_dma_find(struct of_phandle_args *dma_spec)
{
	struct xilinx_fb_dma_device *entry;

	list_for_each_entry(entry, &xilinx_fb_dma_registered_dmas, list)
		if (entry->dev->of_node == dma_spec->np)
			return entry;

	pr_debug("%s: can't find Xilinx DMA controller %pOF\n", __func__,
		 dma_spec->np);

	return NULL;
}

struct xilinx_fb_dma *xilinx_fb_dma_request(struct device *dev,
					    const char *dma_name)
{
	struct device_node *np = dev->of_node;
	struct xilinx_fb_dma_device *xdev;
	struct of_phandle_args dma_spec;
	int index;

	if (!np || !dma_name)
		return ERR_PTR(-EINVAL);

	index = of_property_match_string(np, "dma-names", dma_name);
	if (index < 0) {
		pr_err("%s: no dma property for '%s' not found\n", __func__, dma_name);
		return ERR_PTR(index);
	}

	if (of_parse_phandle_with_args(np, "dmas", "#dma-cells", index,
				       &dma_spec)) {
		pr_err("%s: parse dmas phandle failed\n", __func__);
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&xilinx_fb_dma_dma_list_mutex);
	xdev = xilinx_fb_dma_find(&dma_spec);
	mutex_unlock(&xilinx_fb_dma_dma_list_mutex);

	of_node_put(dma_spec.np);

	if (IS_ERR_OR_NULL(xdev)) {
		pr_err("%s: xilinx_fb_dma %s not found\n", __func__, dma_name);
		return ERR_PTR(-ENODEV);
	}

	xdev->reserved = true;

	return &xdev->dma_dev;
}
EXPORT_SYMBOL_GPL(xilinx_fb_dma_request);

void xilinx_fb_dma_release(struct xilinx_fb_dma *dma)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	WARN_ON(!xdev->reserved);

	xdev->reserved = false;
}
EXPORT_SYMBOL_GPL(xilinx_fb_dma_release);

static inline u32 frmbuf_read(struct xilinx_fb_dma_chan *chan, u32 reg)
{
	return ioread32(chan->xdev->regs + reg);
}

static inline void frmbuf_write(struct xilinx_fb_dma_chan *chan, u32 reg,
				u32 value)
{
	iowrite32(value, chan->xdev->regs + reg);
}

static inline void frmbuf_writeq(struct xilinx_fb_dma_chan *chan, u32 reg,
				 u64 value)
{
	iowrite32(lower_32_bits(value), chan->xdev->regs + reg);
	iowrite32(upper_32_bits(value), chan->xdev->regs + reg + 4);
}

static void frmbuf_write_addr_64(struct xilinx_fb_dma_chan *chan, u32 reg,
				 dma_addr_t addr)
{
	frmbuf_writeq(chan, reg, (u64)addr);
}

static void frmbuf_write_addr_32(struct xilinx_fb_dma_chan *chan, u32 reg,
				 dma_addr_t addr)
{
	frmbuf_write(chan, reg, addr);
}

static void frmbuf_write_addr(struct xilinx_fb_dma_chan *chan, u32 reg,
			      dma_addr_t addr)
{
	if (chan->xdev->use_64_addr)
		frmbuf_write_addr_64(chan, reg, addr);
	else
		frmbuf_write_addr_32(chan, reg, addr);
}

static inline void frmbuf_clr(struct xilinx_fb_dma_chan *chan, u32 reg, u32 clr)
{
	frmbuf_write(chan, reg, frmbuf_read(chan, reg) & ~clr);
}

static inline void frmbuf_set(struct xilinx_fb_dma_chan *chan, u32 reg, u32 set)
{
	frmbuf_write(chan, reg, frmbuf_read(chan, reg) | set);
}

static void xilinx_fb_dma_init_format_array(struct xilinx_fb_dma_device *xdev)
{
	u32 i, cnt;

	for (i = 0; i < ARRAY_SIZE(xilinx_frmbuf_formats); i++) {
		if (!(xdev->enabled_vid_fmts &
		      xilinx_frmbuf_formats[i].fmt_bitmask))
			continue;

		if (xilinx_frmbuf_formats[i].drm_fmt) {
			cnt = xdev->drm_fmt_cnt++;
			xdev->drm_memory_fmts[cnt] =
				xilinx_frmbuf_formats[i].drm_fmt;
		}

		if (xilinx_frmbuf_formats[i].v4l2_fmt) {
			cnt = xdev->v4l2_fmt_cnt++;
			xdev->v4l2_memory_fmts[cnt] =
				xilinx_frmbuf_formats[i].v4l2_fmt;
		}
	}
}

static int xilinx_fb_dma_verify_format(struct xilinx_fb_dma_chan *xil_chan,
				       u32 fourcc, u32 type)
{
	for (u32 i = 0; i < ARRAY_SIZE(xilinx_frmbuf_formats); i++) {
		if ((type == XDMA_DRM &&
		     fourcc != xilinx_frmbuf_formats[i].drm_fmt) ||
		    (type == XDMA_V4L2 &&
		     fourcc != xilinx_frmbuf_formats[i].v4l2_fmt))
			continue;

		if (!(xilinx_frmbuf_formats[i].fmt_bitmask &
		      xil_chan->xdev->enabled_vid_fmts)) {
			return -EINVAL;
		}

		/*
		 * The Alpha color formats are supported in Framebuffer Read
		 * IP only as corresponding DRM formats.
		 */
		if (type == XDMA_DRM &&
		    (xilinx_frmbuf_formats[i].drm_fmt == DRM_FORMAT_ABGR8888 ||
		     xilinx_frmbuf_formats[i].drm_fmt == DRM_FORMAT_ARGB8888 ||
		     xilinx_frmbuf_formats[i].drm_fmt == DRM_FORMAT_AVUY8888) &&
		    xil_chan->direction != XDMA_FROM_MEM)
			return -EINVAL;

		xil_chan->vid_fmt = &xilinx_frmbuf_formats[i];

		return 0;
	}

	return -EINVAL;
}

static void xilinx_fb_dma_set_config(struct xilinx_fb_dma_chan *xil_chan,
				     u32 fourcc, u32 type)
{
	struct xilinx_fb_dma_device *xdev = xil_chan->xdev;
	const struct xilinx_fb_dma_format_desc *old_vid_fmt;
	int ret;

	/* Save old video format */
	old_vid_fmt = xil_chan->vid_fmt;

	ret = xilinx_fb_dma_verify_format(xil_chan, fourcc, type);
	if (ret == -EINVAL) {
		dev_err(xdev->dev,
			"Framebuffer not configured for fourcc %p4cc\n",
			&fourcc);
		return;
	}

	if ((!(xdev->hw_info->flags & XILINX_THREE_PLANES_PROP)) &&
	    (xil_chan->vid_fmt->id == XILINX_FRMBUF_FMT_Y_U_V8 ||
	     xil_chan->vid_fmt->id == XILINX_FRMBUF_FMT_Y_U_V10)) {
		dev_err(xdev->dev, "doesn't support %s format\n",
			xil_chan->vid_fmt->dts_name);
		/* Restore to old video format */
		xil_chan->vid_fmt = old_vid_fmt;
		return;
	}
}

/**
 * xilinx_fb_dma_free_desc_list - Free descriptors list
 * @chan: Driver specific dma channel
 * @list: List to parse and delete the descriptor
 */
static void xilinx_fb_dma_free_desc_list(struct xilinx_fb_dma_chan *chan,
					 struct list_head *list)
{
	struct xilinx_fb_dma_descriptor *desc, *next;

	list_for_each_entry_safe(desc, next, list, node) {
		list_del(&desc->node);
		kfree(desc);
	}
}

/**
 * xilinx_fb_dma_free_descriptors - Free channel descriptors
 * @chan: Driver specific dma channel
 */
static void xilinx_fb_dma_free_descriptors(struct xilinx_fb_dma_chan *chan)
{
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);

	xilinx_fb_dma_free_desc_list(chan, &chan->pending_list);
	xilinx_fb_dma_free_desc_list(chan, &chan->done_list);
	kfree(chan->active_desc);
	kfree(chan->staged_desc);

	chan->staged_desc = NULL;
	chan->active_desc = NULL;
	INIT_LIST_HEAD(&chan->pending_list);
	INIT_LIST_HEAD(&chan->done_list);

	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_fb_dma_chan_desc_cleanup - Clean channel descriptors
 * @chan: Driver specific dma channel
 */
static void xilinx_fb_dma_chan_desc_cleanup(struct xilinx_fb_dma_chan *chan)
{
	struct xilinx_fb_dma_descriptor *desc, *next;
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);

	list_for_each_entry_safe(desc, next, &chan->done_list, node) {
		xilinx_fb_dma_callback callback;
		void *callback_param;

		list_del(&desc->node);

		/* Run the link descriptor callback function */
		callback = desc->callback;
		callback_param = desc->callback_param;
		if (callback) {
			spin_unlock_irqrestore(&chan->lock, flags);
			callback(callback_param);
			spin_lock_irqsave(&chan->lock, flags);
		}

		/* free the descriptor */
		kfree(desc);
	}

	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_fb_dma_do_tasklet - Schedule completion tasklet
 * @data: Pointer to the Xilinx frmbuf channel structure
 */
static void xilinx_fb_dma_do_tasklet(unsigned long data)
{
	struct xilinx_fb_dma_chan *chan = (struct xilinx_fb_dma_chan *)data;

	xilinx_fb_dma_chan_desc_cleanup(chan);
}

/**
 * xilinx_fb_dma_halt - Halt frmbuf channel
 * @chan: Driver specific dma channel
 */
static void xilinx_fb_dma_halt(struct xilinx_fb_dma_chan *chan)
{
	frmbuf_clr(chan, XILINX_FRMBUF_CTRL_OFFSET,
		   XILINX_FRMBUF_CTRL_AP_START |
			   (chan->mode == XDMA_MODE_AUTO_RESTART ? BIT(7) : 0));
	chan->idle = true;
}

/**
 * xilinx_fb_dma_start - Start dma channel
 * @chan: Driver specific dma channel
 */
static void xilinx_fb_dma_start(struct xilinx_fb_dma_chan *chan)
{
	frmbuf_set(chan, XILINX_FRMBUF_CTRL_OFFSET,
		   XILINX_FRMBUF_CTRL_AP_START |
			   (chan->mode == XDMA_MODE_AUTO_RESTART ? BIT(7) : 0));

	chan->idle = false;
}

/**
 * xilinx_fb_dma_complete_descriptor - Mark the active descriptor as complete
 * This function is invoked with spinlock held
 * @chan : xilinx frmbuf channel
 *
 * CONTEXT: hardirq
 */
static void xilinx_fb_dma_complete_descriptor(struct xilinx_fb_dma_chan *chan)
{
	struct xilinx_fb_dma_descriptor *desc = chan->active_desc;

	/*
	 * In case of frame buffer write, read the fid register
	 * and associate it with descriptor
	 */
	if (chan->direction == XDMA_TO_MEM && chan->hw_fid)
		desc->fid = frmbuf_read(chan, XILINX_FRMBUF_FID_OFFSET) &
			    XILINX_FRMBUF_FID_MASK;

	list_add_tail(&desc->node, &chan->done_list);
}

/**
 * xilinx_fb_dma_start_transfer - Starts frmbuf transfer
 * @chan: Driver specific channel struct pointer
 */
static void xilinx_fb_dma_start_transfer(struct xilinx_fb_dma_chan *chan)
{
	struct xilinx_fb_dma_descriptor *desc;
	struct xilinx_fb_dma_device *xdev;

	xdev = container_of(chan, struct xilinx_fb_dma_device, chan);

	if (!chan->idle)
		return;

	if (chan->staged_desc) {
		chan->active_desc = chan->staged_desc;
		chan->staged_desc = NULL;
	}

	if (list_empty(&chan->pending_list))
		return;

	desc = list_first_entry(&chan->pending_list,
				struct xilinx_fb_dma_descriptor, node);

	if (desc->earlycb_mode == XDMA_EARLY_CALLBACK_START_DESC) {
		xilinx_fb_dma_callback callback;
		void *callback_param;

		callback = desc->callback;
		callback_param = desc->callback_param;
		if (callback) {
			callback(callback_param);
			desc->callback = NULL;
			chan->active_desc = desc;
		}
	}

	/* Start the transfer */
	frmbuf_write_addr(chan, XILINX_FRMBUF_ADDR_OFFSET,
			  desc->luma_plane_addr);
	frmbuf_write_addr(chan, XILINX_FRMBUF_ADDR2_OFFSET,
			  desc->chroma_plane_addr[0]);
	if (xdev->hw_info->flags & XILINX_THREE_PLANES_PROP) {
		if (chan->direction == XDMA_FROM_MEM)
			frmbuf_write_addr(chan, XILINX_FRMBUF_RD_ADDR3_OFFSET,
					  desc->chroma_plane_addr[1]);
		else
			frmbuf_write_addr(chan, XILINX_FRMBUF_ADDR3_OFFSET,
					  desc->chroma_plane_addr[1]);
	}

	/* HW expects these parameters to be same for one transaction */
	frmbuf_write(chan, XILINX_FRMBUF_WIDTH_OFFSET, desc->hsize);
	frmbuf_write(chan, XILINX_FRMBUF_STRIDE_OFFSET, desc->stride);
	frmbuf_write(chan, XILINX_FRMBUF_HEIGHT_OFFSET, desc->vsize);
	frmbuf_write(chan, XILINX_FRMBUF_FMT_OFFSET, chan->vid_fmt->id);

	/* If it is framebuffer read IP set the FID */
	if (chan->direction == XDMA_FROM_MEM && chan->hw_fid)
		frmbuf_write(chan, XILINX_FRMBUF_FID_OFFSET, desc->fid);

	/* Start the hardware */
	xilinx_fb_dma_start(chan);
	list_del(&desc->node);

	/* No staging descriptor required when auto restart is disabled */
	if (chan->mode == XDMA_MODE_AUTO_RESTART)
		chan->staged_desc = desc;
	else
		chan->active_desc = desc;
}

/**
 * xilinx_fb_dma_issue_pending - Issue pending transactions
 * @dchan: DMA channel
 */
static void xilinx_fb_dma_issue_pending(struct xilinx_fb_dma *dma)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);
	struct xilinx_fb_dma_chan *chan = &xdev->chan;
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);
	xilinx_fb_dma_start_transfer(chan);
	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_fb_dma_reset - Reset frmbuf channel
 * @chan: Driver specific dma channel
 */
static void xilinx_fb_dma_reset(struct xilinx_fb_dma_chan *chan)
{
	/* reset ip */
	gpiod_set_value(chan->xdev->rst_gpio, 1);
	udelay(1);
	gpiod_set_value(chan->xdev->rst_gpio, 0);
}

/**
 * xilinx_fb_dma_chan_reset - Reset frmbuf channel and enable interrupts
 * @chan: Driver specific frmbuf channel
 */
static void xilinx_fb_dma_chan_reset(struct xilinx_fb_dma_chan *chan)
{
	xilinx_fb_dma_reset(chan);
	frmbuf_write(chan, XILINX_FRMBUF_IE_OFFSET, XILINX_FRMBUF_IE_AP_DONE);
	frmbuf_write(chan, XILINX_FRMBUF_GIE_OFFSET, XILINX_FRMBUF_GIE_EN);
	chan->fid_err_flag = 0;
	chan->fid_out_val = 0;
}

/**
 * xilinx_fb_dma_irq_handler - frmbuf Interrupt handler
 * @irq: IRQ number
 * @data: Pointer to the Xilinx frmbuf channel structure
 *
 * Return: IRQ_HANDLED/IRQ_NONE
 */
static irqreturn_t xilinx_fb_dma_irq_handler(int irq, void *data)
{
	struct xilinx_fb_dma_chan *chan = data;
	u32 status;
	xilinx_fb_dma_callback callback = NULL;
	void *callback_param;
	struct xilinx_fb_dma_descriptor *desc;

	status = frmbuf_read(chan, XILINX_FRMBUF_ISR_OFFSET);
	if (!(status & XILINX_FRMBUF_ISR_ALL_IRQ_MASK))
		return IRQ_NONE;

	frmbuf_write(chan, XILINX_FRMBUF_ISR_OFFSET,
		     status & XILINX_FRMBUF_ISR_ALL_IRQ_MASK);

	/* Check if callback function needs to be called early */
	desc = chan->staged_desc;
	if (desc && desc->earlycb_mode == XDMA_EARLY_CALLBACK) {
		callback = desc->callback;
		callback_param = desc->callback_param;
		if (callback) {
			callback(callback_param);
			desc->callback = NULL;
		}
	}

	if (status & XILINX_FRMBUF_ISR_AP_DONE_IRQ) {
		spin_lock(&chan->lock);
		chan->idle = true;
		if (chan->active_desc) {
			xilinx_fb_dma_complete_descriptor(chan);
			chan->active_desc = NULL;
		}

		/* Update fid err detect flag and out value */
		if (chan->direction == XDMA_FROM_MEM && chan->hw_fid &&
		    chan->idle &&
		    chan->xdev->hw_info->flags & XILINX_FID_ERR_DETECT_PROP) {
			if (chan->mode == XDMA_MODE_AUTO_RESTART)
				chan->fid_mode = FID_MODE_2;
			else
				chan->fid_mode = FID_MODE_1;

			frmbuf_write(chan, XILINX_FRMBUF_FID_MODE_OFFSET,
				     chan->fid_mode);
			dev_dbg(chan->xdev->dev, "fid mode = %d\n",
				frmbuf_read(chan, XILINX_FRMBUF_FID_MODE_OFFSET));

			chan->fid_err_flag = frmbuf_read(chan,
							 XILINX_FRMBUF_FID_ERR_OFFSET) &
							XILINX_FRMBUF_FID_ERR_MASK;
			chan->fid_out_val = frmbuf_read(chan,
							XILINX_FRMBUF_FID_OUT_OFFSET) &
							XILINX_FRMBUF_FID_OUT_MASK;
			dev_dbg(chan->xdev->dev, "fid err cnt = 0x%x\n",
				frmbuf_read(chan, XILINX_FRMBUF_FID_ERR_OFFSET));
		}

		xilinx_fb_dma_start_transfer(chan);
		spin_unlock(&chan->lock);
	}

	tasklet_schedule(&chan->tasklet);
	return IRQ_HANDLED;
}

/*
 * Xilinx DMA ops
 */

static struct xilinx_fb_dma_descriptor *
xilinx_fb_dma_prepare(struct xilinx_fb_dma *dma,
		      struct xilinx_fb_dma_params *params)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);
	struct xilinx_fb_dma_chan *chan = &xdev->chan;
	struct xilinx_fb_dma_descriptor *desc;
	u32 vsize, hsize;

	if (params->direction != chan->direction) {
		dev_err(chan->xdev->dev, "Bad direction\n");
		return ERR_PTR(-EINVAL);
	}

	if (!chan->vid_fmt) {
		dev_err(chan->xdev->dev, "No fmt selected\n");
		return ERR_PTR(-EINVAL);
	}

	if (params->num_planes != chan->vid_fmt->num_planes) {
		dev_err(chan->xdev->dev, "Bad number of planes\n");
		return ERR_PTR(-EINVAL);
	}

	vsize = params->height;
	//hsize = (xt->sgl[0].size * chan->vid_fmt->ppw * 8) /
	//	 chan->vid_fmt->bpw;
	hsize = params->width;
	/* hsize calc should not have resulted in an odd number */
	if (hsize & 1) {
		dev_err(chan->xdev->dev, "Odd width\n");
		return ERR_PTR(-EINVAL);
	}

	if (hsize < XILINX_FRMBUF_MIN_WIDTH ||
	    vsize < XILINX_FRMBUF_MIN_HEIGHT) {
		dev_err(chan->xdev->dev, "Size too small\n");
		return ERR_PTR(-EINVAL);
	}

	if (vsize > chan->xdev->max_height || hsize > chan->xdev->max_width) {
		dev_dbg(chan->xdev->dev,
			"vsize %d max vsize %d hsize %d max hsize %d\n",
			vsize, chan->xdev->max_height, hsize,
			chan->xdev->max_width);
		dev_err(chan->xdev->dev, "Requested size not supported!\n");
		return ERR_PTR(-EINVAL);
	}

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	desc->vsize = params->height;
	desc->stride = params->bytesperline; // XXX
	//desc->hsize = (xt->sgl[0].size * chan->vid_fmt->ppw * 8) /
	//	     chan->vid_fmt->bpw;
	desc->hsize = params->width;

	/* hsize calc should not have resulted in an odd number */
	if (desc->hsize & 1) // XXX
		desc->hsize++;

	desc->luma_plane_addr = params->planes[0].addr;

	if (params->num_planes > 1)
		desc->chroma_plane_addr[0] = params->planes[1].addr;
	if (params->num_planes > 2)
		desc->chroma_plane_addr[1] = params->planes[2].addr;

	desc->callback = params->callback;
	desc->callback_param = params->callback_data;

	return desc;
}

static void xilinx_fb_dma_submit(struct xilinx_fb_dma *dma,
				 struct xilinx_fb_dma_descriptor *desc)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);
	struct xilinx_fb_dma_chan *chan = &xdev->chan;
	unsigned long flags;

	spin_lock_irqsave(&chan->lock, flags);
	list_add_tail(&desc->node, &chan->pending_list);
	spin_unlock_irqrestore(&chan->lock, flags);
}

/**
 * xilinx_frmbuf_terminate_all - Halt the channel and free descriptors
 * @dchan: Driver specific dma channel pointer
 */
static void xilinx_fb_dma_terminate_all(struct xilinx_fb_dma *dma)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);
	struct xilinx_fb_dma_chan *chan = &xdev->chan;

	xilinx_fb_dma_halt(chan);
	xilinx_fb_dma_free_descriptors(chan);
	/* worst case frame-to-frame boundary; ensure frame output complete */
	msleep(50);

	if (chan->xdev->hw_info->flags & XILINX_FLUSH_PROP) {
		u8 count;

		/*
		 * Flush the framebuffer FIFO and
		 * wait for max 50ms for flush done
		 */
		frmbuf_set(chan, XILINX_FRMBUF_CTRL_OFFSET,
			   XILINX_FRMBUF_CTRL_FLUSH);
		for (count = WAIT_FOR_FLUSH_DONE; count > 0; count--) {
			if (frmbuf_read(chan, XILINX_FRMBUF_CTRL_OFFSET) &
			    XILINX_FRMBUF_CTRL_FLUSH_DONE)
				break;
			usleep_range(2000, 2100);
		}

		if (!count)
			dev_err(chan->xdev->dev, "Framebuffer Flush not done!\n");
	}

	xilinx_fb_dma_chan_reset(chan);
}

/**
 * xilinx_fb_dma_synchronize - kill tasklet to stop further descr processing
 * @dchan: Driver specific dma channel pointer
 */
static void xilinx_fb_dma_synchronize(struct xilinx_fb_dma *dma)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	tasklet_kill(&xdev->chan.tasklet);
}

static void xilinx_fb_dma_set_mode(struct xilinx_fb_dma *dma,
				   enum xilinx_fb_dma_operation_mode mode)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	if (WARN_ON(mode != XDMA_MODE_DEFAULT &&
		    mode != XDMA_MODE_AUTO_RESTART))
		return;

	xdev->chan.mode = mode;
}

static void xilinx_fb_dma_drm_config(struct xilinx_fb_dma *dma, u32 drm_fourcc)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	xilinx_fb_dma_set_config(&xdev->chan, drm_fourcc, XDMA_DRM);
}

static void xilinx_fb_dma_v4l2_config(struct xilinx_fb_dma *dma,
				      u32 v4l2_fourcc)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	xilinx_fb_dma_set_config(&xdev->chan, v4l2_fourcc, XDMA_V4L2);
}

static int xilinx_fb_dma_get_drm_vid_fmts(struct xilinx_fb_dma *dma,
					  u32 *fmt_cnt, u32 **fmts)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	*fmt_cnt = xdev->drm_fmt_cnt;
	*fmts = xdev->drm_memory_fmts;

	return 0;
}

static int xilinx_fb_dma_get_v4l2_vid_fmts(struct xilinx_fb_dma *dma,
					   u32 *fmt_cnt, u32 **fmts)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	*fmt_cnt = xdev->v4l2_fmt_cnt;
	*fmts = xdev->v4l2_memory_fmts;

	return 0;
}

static int xilinx_fb_dma_get_fid(struct xilinx_fb_dma *dma,
				 struct xilinx_fb_dma_descriptor *desc,
				 u32 *fid)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	if (!desc || !fid)
		return -EINVAL;

	if (xdev->chan.direction != XDMA_TO_MEM)
		return -EINVAL;

	*fid = desc->fid;
	return 0;
}

static int xilinx_fb_dma_set_fid(struct xilinx_fb_dma *dma,
				 struct xilinx_fb_dma_descriptor *desc, u32 fid)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	if (fid > 1 || !desc)
		return -EINVAL;

	if (xdev->chan.direction != XDMA_FROM_MEM)
		return -EINVAL;

	desc->fid = fid;
	return 0;
}

static int xilinx_fb_dma_get_fid_err_flag(struct xilinx_fb_dma *dma,
					  u32 *fid_err_flag)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	if (xdev->chan.direction != XDMA_FROM_MEM || xdev->chan.idle)
		return -EINVAL;

	*fid_err_flag = xdev->chan.fid_err_flag;

	return 0;
}

static int xilinx_fb_dma_get_fid_out(struct xilinx_fb_dma *dma,
				     u32 *fid_out_val)
{
	struct xilinx_fb_dma_device *xdev = to_xilinx_fb_dma_device(dma);

	if (xdev->chan.direction != XDMA_FROM_MEM || xdev->chan.idle)
		return -EINVAL;

	*fid_out_val = xdev->chan.fid_out_val;

	return 0;
}

static int xilinx_fb_dma_get_earlycb(struct xilinx_fb_dma *dma,
				     struct xilinx_fb_dma_descriptor *desc,
				     enum xilinx_fb_dma_early_cb_mode *earlycb)
{
	if (!desc || !earlycb)
		return -EINVAL;

	*earlycb = desc->earlycb_mode;
	return 0;
}

static int xilinx_fb_dma_set_earlycb(struct xilinx_fb_dma *dma,
				     struct xilinx_fb_dma_descriptor *desc,
				     enum xilinx_fb_dma_early_cb_mode earlycb)
{
	if (!desc)
		return -EINVAL;

	desc->earlycb_mode = earlycb;
	return 0;
}

static const struct xilinx_fb_dma_ops xilinx_dma_ops = {
	.prepare = xilinx_fb_dma_prepare,
	.submit = xilinx_fb_dma_submit,
	.async_issue_pending = xilinx_fb_dma_issue_pending,
	.terminate_all = xilinx_fb_dma_terminate_all,
	.synchronize = xilinx_fb_dma_synchronize,

	.set_mode = xilinx_fb_dma_set_mode,

	.drm_config = xilinx_fb_dma_drm_config,
	.v4l2_config = xilinx_fb_dma_v4l2_config,
	.get_drm_vid_fmts = xilinx_fb_dma_get_drm_vid_fmts,
	.get_v4l2_vid_fmts = xilinx_fb_dma_get_v4l2_vid_fmts,

	.get_fid = xilinx_fb_dma_get_fid,
	.set_fid = xilinx_fb_dma_set_fid,
	.get_fid_err_flag = xilinx_fb_dma_get_fid_err_flag,
	.get_fid_out = xilinx_fb_dma_get_fid_out,

	.get_earlycb = xilinx_fb_dma_get_earlycb,
	.set_earlycb = xilinx_fb_dma_set_earlycb,
};

/* -----------------------------------------------------------------------------
 * Probe and remove
 */

/**
 * xilinx_fb_dma_chan_remove - Per Channel remove function
 * @chan: Driver specific dma channel
 */
static void xilinx_fb_dma_chan_remove(struct xilinx_fb_dma_chan *chan)
{
	/* Disable all interrupts */
	frmbuf_clr(chan, XILINX_FRMBUF_IE_OFFSET,
		   XILINX_FRMBUF_ISR_ALL_IRQ_MASK);

	tasklet_kill(&chan->tasklet);
}

/**
 * xilinx_fb_dma_chan_probe - Per Channel Probing
 * It get channel features from the device tree entry and
 * initialize special channel handling routines
 *
 * @xdev: Driver specific device structure
 * @node: Device node
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_fb_dma_chan_probe(struct xilinx_fb_dma_device *xdev,
				    struct device_node *node)
{
	struct xilinx_fb_dma_chan *chan;
	int err;
	u32 dma_addr_size = 0;

	chan = &xdev->chan;

	chan->xdev = xdev;
	chan->idle = true;
	chan->fid_err_flag = 0;
	chan->fid_out_val = 0;
	chan->mode = XDMA_MODE_AUTO_RESTART;

	err = of_property_read_u32(node, "xlnx,dma-addr-width", &dma_addr_size);
	if (err || (dma_addr_size != 32 && dma_addr_size != 64)) {
		dev_err(xdev->dev, "missing or invalid addr width dts prop\n");
		return err;
	}

	xdev->use_64_addr = dma_addr_size == 64 &&
			    sizeof(dma_addr_t) == sizeof(u64);

	if (xdev->hw_info->flags & XILINX_FID_PROP)
		chan->hw_fid = of_property_read_bool(node, "xlnx,fid");

	spin_lock_init(&chan->lock);
	INIT_LIST_HEAD(&chan->pending_list);
	INIT_LIST_HEAD(&chan->done_list);

	chan->irq = irq_of_parse_and_map(node, 0);
	err = devm_request_irq(xdev->dev, chan->irq, xilinx_fb_dma_irq_handler,
			       IRQF_SHARED, "xilinx_framebuffer", chan);

	if (err) {
		dev_err(xdev->dev, "unable to request IRQ %d\n", chan->irq);
		return err;
	}

	tasklet_init(&chan->tasklet, xilinx_fb_dma_do_tasklet,
		     (unsigned long)chan);

	xilinx_fb_dma_chan_reset(chan);

	return 0;
}

/**
 * xilinx_fb_dma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 *
 * Return: '0' on success and failure value on error
 */
static int xilinx_fb_dma_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct xilinx_fb_dma_device *xdev;
	struct resource *io;
	enum xilinx_fb_dma_direction dma_dir;
	const struct of_device_id *match;
	int err;
	u32 i, j, align, max_width, max_height;
	int hw_vid_fmt_cnt;
	const char *vid_fmts[ARRAY_SIZE(xilinx_frmbuf_formats)];

	xdev = devm_kzalloc(&pdev->dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	xdev->dev = &pdev->dev;

	match = of_match_node(xilinx_fb_dma_of_ids, node);
	if (!match)
		return -ENODEV;

	xdev->hw_info = match->data;

	dma_dir = xdev->hw_info->direction;

	if (xdev->hw_info->flags & XILINX_CLK_PROP) {
		xdev->ap_clk = devm_clk_get(xdev->dev, "ap_clk");
		if (IS_ERR(xdev->ap_clk)) {
			err = PTR_ERR(xdev->ap_clk);
			dev_err(xdev->dev, "failed to get ap_clk (%d)\n", err);
			return err;
		}

		//xdev->axi_clk = devm_clk_get(xdev->dev, "axi_clk");
		//if (IS_ERR(xdev->axi_clk)) {
		//	err = PTR_ERR(xdev->axi_clk);
		//	dev_err(xdev->dev, "failed to get axi_clk (%d)\n", err);
		//	return err;
		//}
	} else {
		dev_info(xdev->dev, "assuming clock is enabled!\n");
	}

	xdev->rst_gpio = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xdev->rst_gpio)) {
		err = PTR_ERR(xdev->rst_gpio);
		if (err == -EPROBE_DEFER)
			dev_info(&pdev->dev,
				 "Probe deferred due to GPIO reset defer\n");
		else
			dev_err(&pdev->dev,
				"Unable to locate reset property in dt\n");
		return err;
	}

	gpiod_set_value_cansleep(xdev->rst_gpio, 0x0);

	io = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xdev->regs = devm_ioremap_resource(&pdev->dev, io);
	if (IS_ERR(xdev->regs))
		return PTR_ERR(xdev->regs);

	if (xdev->hw_info->flags & XILINX_THREE_PLANES_PROP)
		max_height = 8640;
	else
		max_height = 4320;

	err = of_property_read_u32(node, "xlnx,max-height", &xdev->max_height);
	if (err < 0) {
		dev_err(xdev->dev, "xlnx,max-height is missing");
		return -EINVAL;
	} else if (xdev->max_height > max_height ||
		   xdev->max_height < XILINX_FRMBUF_MIN_HEIGHT) {
		dev_err(&pdev->dev, "Invalid height in dt");
		return -EINVAL;
	}

	if (xdev->hw_info->flags & XILINX_THREE_PLANES_PROP)
		max_width = 15360;
	else
		max_width = 8192;

	err = of_property_read_u32(node, "xlnx,max-width", &xdev->max_width);
	if (err < 0) {
		dev_err(xdev->dev, "xlnx,max-width is missing");
		return -EINVAL;
	} else if (xdev->max_width > max_width ||
		   xdev->max_width < XILINX_FRMBUF_MIN_WIDTH) {
		dev_err(&pdev->dev, "Invalid width in dt");
		return -EINVAL;
	}

	/* Initialize the DMA engine */
	if (xdev->hw_info->flags & XILINX_PPC_PROP) {
		err = of_property_read_u32(node, "xlnx,pixels-per-clock", &xdev->ppc);
		if (err || (xdev->ppc != 1 && xdev->ppc != 2 &&
			    xdev->ppc != 4 && xdev->ppc != 8)) {
			dev_err(&pdev->dev, "missing or invalid pixels per clock dts prop\n");
			return err;
		}
		err = of_property_read_u32(node, "xlnx,dma-align", &align);
		if (err)
			align = xdev->ppc * XILINX_FRMBUF_ALIGN_MUL;

		if (align < (xdev->ppc * XILINX_FRMBUF_ALIGN_MUL) ||
		    ffs(align) != fls(align)) {
			dev_err(&pdev->dev, "invalid dma align dts prop\n");
			return -EINVAL;
		}
	} else {
		align = 16;
	}

	xdev->dma_dev.ppc = xdev->ppc;
	xdev->dma_dev.copy_align = fls(align) - 1;

	if (xdev->hw_info->flags & XILINX_CLK_PROP) {
		err = clk_prepare_enable(xdev->ap_clk);
		if (err) {
			dev_err(&pdev->dev, " failed to enable ap_clk (%d)\n",
				err);
			return err;
		}

		//err = clk_prepare_enable(xdev->axi_clk);
		//if (err) {
		//	dev_err(&pdev->dev, " failed to enable axi_clk (%d)\n",
		//		err);
		//	return err;
		//}
	}

	/* Initialize the channels */
	err = xilinx_fb_dma_chan_probe(xdev, node);
	if (err < 0)
		goto disable_clk;

	xdev->chan.direction = dma_dir;

	if (xdev->chan.direction == XDMA_TO_MEM) {
		xdev->dma_dev.directions = BIT(XDMA_TO_MEM);
	} else if (xdev->chan.direction == XDMA_FROM_MEM) {
		xdev->dma_dev.directions = BIT(XDMA_FROM_MEM);
	} else {
		err = -EINVAL;
		goto remove_chan;
	}

	/* read supported video formats and update internal table */
	hw_vid_fmt_cnt = of_property_count_strings(node, "xlnx,vid-formats");

	err = of_property_read_string_array(node, "xlnx,vid-formats",
					    vid_fmts, hw_vid_fmt_cnt);
	if (err < 0) {
		dev_err(&pdev->dev,
			"Missing or invalid xlnx,vid-formats dts prop\n");
		goto remove_chan;
	}

	for (i = 0; i < hw_vid_fmt_cnt; i++) {
		const char *vid_fmt_name = vid_fmts[i];

		for (j = 0; j < ARRAY_SIZE(xilinx_frmbuf_formats); j++) {
			const char *dts_name =
				xilinx_frmbuf_formats[j].dts_name;

			if (strcmp(vid_fmt_name, dts_name))
				continue;

			xdev->enabled_vid_fmts |=
				xilinx_frmbuf_formats[j].fmt_bitmask;
		}
	}

	/* Determine supported vid framework formats */
	xilinx_fb_dma_init_format_array(xdev);

	xdev->dma_dev.ops = &xilinx_dma_ops;

	platform_set_drvdata(pdev, xdev);

	/* Register the DMA engine with the core */
	xilinx_fb_dma_register(xdev);

	return 0;

remove_chan:
	xilinx_fb_dma_chan_remove(&xdev->chan);
disable_clk:
	//clk_disable_unprepare(xdev->axi_clk);
	clk_disable_unprepare(xdev->ap_clk);
	return err;
}

/**
 * xilinx_fb_dma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 *
 * Return: Always '0'
 */
static void xilinx_fb_dma_remove(struct platform_device *pdev)
{
	struct xilinx_fb_dma_device *xdev = platform_get_drvdata(pdev);

	xilinx_fb_dma_unregister(xdev);
	xilinx_fb_dma_chan_remove(&xdev->chan);
	//clk_disable_unprepare(xdev->axi_clk);
	clk_disable_unprepare(xdev->ap_clk);
}

MODULE_DEVICE_TABLE(of, xilinx_fb_dma_of_ids);

static struct platform_driver xilinx_fb_dma_driver = {
	.driver = {
		.name = "xilinx-fb-dma",
		.of_match_table = xilinx_fb_dma_of_ids,
	},
	.probe = xilinx_fb_dma_probe,
	.remove = xilinx_fb_dma_remove,
};

module_platform_driver(xilinx_fb_dma_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx Framebuffer DMA driver");
MODULE_LICENSE("GPL v2");
