// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Test Pattern Generator
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#include <dt-bindings/media/xilinx-vip.h>

#include "xilinx-vtc.h"

#define XTPG_MIN_WIDTH				32
#define XTPG_MAX_WIDTH				7680
#define XTPG_MIN_HEIGHT				32
#define XTPG_MAX_HEIGHT				7680

#define XTPG_CTRL_CONTROL			0x0000
#define XTPG_CTRL_CONTROL_SW_ENABLE		BIT(0)
#define XTPG_CTRL_CONTROL_REG_UPDATE		BIT(1)
#define XTPG_CTRL_CONTROL_SW_RESET		BIT(31)
#define XTPG_CTRL_STATUS_SLAVE_ERROR		BIT(16)
#define XTPG_CTRL_IRQ_SLAVE_ERROR		BIT(16)
#define XTPG_CTRL_VERSION			0x0010
#define XTPG_CTRL_VERSION_MAJOR_MASK		(0xff << 24)
#define XTPG_CTRL_VERSION_MAJOR_SHIFT		24
#define XTPG_CTRL_VERSION_MINOR_MASK		(0xff << 16)
#define XTPG_CTRL_VERSION_MINOR_SHIFT		16
#define XTPG_CTRL_VERSION_REVISION_MASK		(0xf << 12)
#define XTPG_CTRL_VERSION_REVISION_SHIFT	12

#define XTPG_ACTIVE_SIZE			0x0020
#define XTPG_ACTIVE_VSIZE_MASK			(0x7ff << 16)
#define XTPG_ACTIVE_VSIZE_SHIFT			16
#define XTPG_ACTIVE_HSIZE_MASK			(0x7ff << 0)
#define XTPG_ACTIVE_HSIZE_SHIFT			0

#define XTPG_PATTERN_CONTROL			0x0100
#define XTPG_PATTERN_MASK			(0xf << 0)
#define XTPG_PATTERN_CONTROL_CROSS_HAIRS	BIT(4)
#define XTPG_PATTERN_CONTROL_MOVING_BOX		BIT(5)
#define XTPG_PATTERN_CONTROL_COLOR_MASK_SHIFT	6
#define XTPG_PATTERN_CONTROL_COLOR_MASK_MASK	(0xf << 6)
#define XTPG_PATTERN_CONTROL_STUCK_PIXEL	BIT(9)
#define XTPG_PATTERN_CONTROL_NOISE		BIT(10)
#define XTPG_PATTERN_CONTROL_MOTION		BIT(12)
#define XTPG_MOTION_SPEED			0x0104
#define XTPG_CROSS_HAIRS			0x0108
#define XTPG_CROSS_HAIRS_ROW_SHIFT		0
#define XTPG_CROSS_HAIRS_ROW_MASK		(0xfff << 0)
#define XTPG_CROSS_HAIRS_COLUMN_SHIFT		16
#define XTPG_CROSS_HAIRS_COLUMN_MASK		(0xfff << 16)
#define XTPG_ZPLATE_HOR_CONTROL			0x010c
#define XTPG_ZPLATE_VER_CONTROL			0x0110
#define XTPG_ZPLATE_START_SHIFT			0
#define XTPG_ZPLATE_START_MASK			(0xffff << 0)
#define XTPG_ZPLATE_SPEED_SHIFT			16
#define XTPG_ZPLATE_SPEED_MASK			(0xffff << 16)
#define XTPG_BOX_SIZE				0x0114
#define XTPG_BOX_COLOR				0x0118
#define XTPG_STUCK_PIXEL_THRESH			0x011c
#define XTPG_NOISE_GAIN				0x0120
#define XTPG_BAYER_PHASE			0x0124
#define XTPG_BAYER_PHASE_RGGB			0
#define XTPG_BAYER_PHASE_GRBG			1
#define XTPG_BAYER_PHASE_GBRG			2
#define XTPG_BAYER_PHASE_BGGR			3
#define XTPG_BAYER_PHASE_OFF			4

/*
 * The minimum blanking value is one clock cycle for the front porch, one clock
 * cycle for the sync pulse and one clock cycle for the back porch.
 */
#define XTPG_MIN_HBLANK			3
#define XTPG_MAX_HBLANK			(XVTC_MAX_HSIZE - XTPG_MIN_WIDTH)
#define XTPG_MIN_VBLANK			3
#define XTPG_MAX_VBLANK			(XVTC_MAX_VSIZE - XTPG_MIN_HEIGHT)

/**
 * struct xtpg_video_format - AXI4-Stream video format description
 * @vf_code: AXI4 video format code
 * @width: AXI4 format width in bits per component
 * @pattern: CFA pattern for Mono/Sensor formats
 * @code: media bus format code
 */
struct xtpg_video_format {
	unsigned int vf_code;
	unsigned int width;
	const char *pattern;
	unsigned int code;
};

static const struct xtpg_video_format xtpg_video_formats[] = {
	{ XVIP_VF_YUV_422, 8, NULL, MEDIA_BUS_FMT_UYVY8_1X16, },
	{ XVIP_VF_YUV_444, 8, NULL, MEDIA_BUS_FMT_VUY8_1X24, },
	{ XVIP_VF_RBG, 8, NULL, MEDIA_BUS_FMT_RBG888_1X24, },
	{ XVIP_VF_MONO_SENSOR, 8, "mono", MEDIA_BUS_FMT_Y8_1X8, },
	{ XVIP_VF_MONO_SENSOR, 8, "rggb", MEDIA_BUS_FMT_SRGGB8_1X8, },
	{ XVIP_VF_MONO_SENSOR, 8, "grbg", MEDIA_BUS_FMT_SGRBG8_1X8, },
	{ XVIP_VF_MONO_SENSOR, 8, "gbrg", MEDIA_BUS_FMT_SGBRG8_1X8, },
	{ XVIP_VF_MONO_SENSOR, 8, "bggr", MEDIA_BUS_FMT_SBGGR8_1X8, },
	{ XVIP_VF_MONO_SENSOR, 12, "mono", MEDIA_BUS_FMT_Y12_1X12, },
	{ XVIP_VF_MONO_SENSOR, 10, "rggb", MEDIA_BUS_FMT_SRGGB10_1X10, },
};

/**
 * xtpg_of_get_format - Parse a device tree node and return format information
 * @node: the device tree node
 *
 * Read the xlnx,video-format, xlnx,video-width and xlnx,cfa-pattern properties
 * from the device tree @node passed as an argument and return the corresponding
 * format information.
 *
 * Return: a pointer to the format information structure corresponding to the
 * format name and width, or ERR_PTR if no corresponding format can be found.
 */
static const struct xtpg_video_format *
xtpg_of_get_format(struct device_node *node)
{
	const char *pattern = "mono";
	unsigned int vf_code;
	unsigned int i;
	u32 width;
	int ret;

	ret = of_property_read_u32(node, "xlnx,video-format", &vf_code);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = of_property_read_u32(node, "xlnx,video-width", &width);
	if (ret < 0)
		return ERR_PTR(ret);

	if (vf_code == XVIP_VF_MONO_SENSOR)
		of_property_read_string(node, "xlnx,cfa-pattern", &pattern);

	for (i = 0; i < ARRAY_SIZE(xtpg_video_formats); ++i) {
		const struct xtpg_video_format *format = &xtpg_video_formats[i];

		if (format->vf_code != vf_code || format->width != width)
			continue;

		if (vf_code == XVIP_VF_MONO_SENSOR &&
		    strcmp(pattern, format->pattern))
			continue;

		return format;
	}

	return ERR_PTR(-EINVAL);
}

/**
 * struct xtpg_device - Xilinx Test Pattern Generator device structure
 * @subdev: V4L2 subdevice
 * @notifier: Async notifier for the upstream subdev
 * @dev: (OF) device
 * @iomem: device I/O register space remapped to kernel virtual memory
 * @clk: video core clock
 * @saved_ctrl: saved control register for resume / suspend
 * @pads: media pads
 * @npads: number of pads (1 or 2)
 * @has_input: whether an input is connected to the sink pad
 * @default_format: default V4L2 media bus format
 * @vip_format: format information corresponding to the active format
 * @bayer: boolean flag if TPG is set to any bayer format
 * @ctrl_handler: control handler
 * @hblank: horizontal blanking control
 * @vblank: vertical blanking control
 * @pattern: test pattern control
 * @vtc: video timing controller
 * @vtmux_gpio: video timing mux GPIO
 */
struct xtpg_device {
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct device *dev;
	void __iomem *iomem;
	struct clk *clk;
	u32 saved_ctrl;

	struct media_pad pads[2];
	unsigned int npads;
	bool has_input;

	struct v4l2_mbus_framefmt default_format;
	const struct xtpg_video_format *vip_format;
	bool bayer;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *pattern;

	struct xvtc_device *vtc;
	struct gpio_desc *vtmux_gpio;
};

static inline struct xtpg_device *to_tpg(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xtpg_device, subdev);
}

/* -----------------------------------------------------------------------------
 * Register Access
 */

static inline u32 xtpg_read(struct xtpg_device *xtpg, u32 addr)
{
	return ioread32(xtpg->iomem + addr);
}

static inline void xtpg_write(struct xtpg_device *xtpg, u32 addr, u32 value)
{
	iowrite32(value, xtpg->iomem + addr);
}

static inline void xtpg_clr(struct xtpg_device *xtpg, u32 addr, u32 clr)
{
	xtpg_write(xtpg, addr, xtpg_read(xtpg, addr) & ~clr);
}

static inline void xtpg_set(struct xtpg_device *xtpg, u32 addr, u32 set)
{
	xtpg_write(xtpg, addr, xtpg_read(xtpg, addr) | set);
}

static void xtpg_clr_or_set(struct xtpg_device *xtpg, u32 addr, u32 mask,
			    bool set)
{
	u32 reg;

	reg = xtpg_read(xtpg, addr);
	reg = set ? reg | mask : reg & ~mask;
	xtpg_write(xtpg, addr, reg);
}

static void xtpg_clr_and_set(struct xtpg_device *xtpg, u32 addr, u32 clr,
			     u32 set)
{
	u32 reg;

	reg = xtpg_read(xtpg, addr);
	reg &= ~clr;
	reg |= set;
	xtpg_write(xtpg, addr, reg);
}

static inline void xtpg_reset(struct xtpg_device *xtpg)
{
	xtpg_write(xtpg, XTPG_CTRL_CONTROL, XTPG_CTRL_CONTROL_SW_RESET);
}

static inline void xtpg_start(struct xtpg_device *xtpg)
{
	xtpg_set(xtpg, XTPG_CTRL_CONTROL,
		 XTPG_CTRL_CONTROL_SW_ENABLE | XTPG_CTRL_CONTROL_REG_UPDATE);
}

static inline void xtpg_stop(struct xtpg_device *xtpg)
{
	xtpg_clr(xtpg, XTPG_CTRL_CONTROL, XTPG_CTRL_CONTROL_SW_ENABLE);
}

static void xtpg_print_version(struct xtpg_device *xtpg)
{
	u32 version;

	version = xtpg_read(xtpg, XTPG_CTRL_VERSION);

	dev_info(xtpg->dev, "device found, version %u.%02x%x\n",
		 (version & XTPG_CTRL_VERSION_MAJOR_MASK) >>
		 XTPG_CTRL_VERSION_MAJOR_SHIFT,
		 (version & XTPG_CTRL_VERSION_MINOR_MASK) >>
		 XTPG_CTRL_VERSION_MINOR_SHIFT,
		 (version & XTPG_CTRL_VERSION_REVISION_MASK) >>
		 XTPG_CTRL_VERSION_REVISION_SHIFT);
}

static u32 xtpg_get_bayer_phase(unsigned int code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		return XTPG_BAYER_PHASE_RGGB;
	case MEDIA_BUS_FMT_SGRBG8_1X8:
		return XTPG_BAYER_PHASE_GRBG;
	case MEDIA_BUS_FMT_SGBRG8_1X8:
		return XTPG_BAYER_PHASE_GBRG;
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		return XTPG_BAYER_PHASE_BGGR;
	default:
		return XTPG_BAYER_PHASE_OFF;
	}
}

static void __xtpg_update_pattern_control(struct xtpg_device *xtpg,
					  bool passthrough, bool pattern)
{
	u32 pattern_mask = (1 << (xtpg->pattern->maximum + 1)) - 1;

	/*
	 * If the TPG has no sink pad or no input connected to its sink pad
	 * passthrough mode can't be enabled.
	 */
	if (xtpg->npads == 1 || !xtpg->has_input)
		passthrough = false;

	/* If passthrough mode is allowed unmask bit 0. */
	if (passthrough)
		pattern_mask &= ~1;

	/* If test pattern mode is allowed unmask all other bits. */
	if (pattern)
		pattern_mask &= 1;

	__v4l2_ctrl_modify_range(xtpg->pattern, 0, xtpg->pattern->maximum,
				 pattern_mask, pattern ? 9 : 0);
}

static void xtpg_update_pattern_control(struct xtpg_device *xtpg,
					bool passthrough, bool pattern)
{
	mutex_lock(xtpg->ctrl_handler.lock);
	__xtpg_update_pattern_control(xtpg, passthrough, pattern);
	mutex_unlock(xtpg->ctrl_handler.lock);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Video Operations
 */

static int xtpg_set_remote_stream(struct xtpg_device *xtpg, bool enable)
{
	struct media_pad *remote;
	struct v4l2_subdev *subdev;
	int ret;

	if (xtpg->npads == 1)
		return 0;

	remote = media_pad_remote_pad_first(&xtpg->pads[0]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return 0;

	subdev = media_entity_to_v4l2_subdev(remote->entity);

	if (enable)
		ret = v4l2_subdev_enable_streams(subdev, remote->index,
						 BIT_ULL(0));
	else
		ret = v4l2_subdev_disable_streams(subdev, remote->index,
						  BIT_ULL(0));

	if (ret < 0 && ret != -ENOIOCTLCMD)
		return ret;

	return 0;
}

static int xtpg_enable_streams(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       u32 pad, u64 streams_mask)
{
	struct xtpg_device *xtpg = to_tpg(subdev);
	const struct v4l2_mbus_framefmt *format;
	unsigned int width;
	unsigned int height;
	bool passthrough;
	u32 bayer_phase;
	int ret;

	format = v4l2_subdev_state_get_format(sd_state, 0);
	width = format->width;
	height = format->height;

	xtpg_write(xtpg, XTPG_ACTIVE_SIZE,
		   (height << XTPG_ACTIVE_VSIZE_SHIFT) |
		   (width << XTPG_ACTIVE_HSIZE_SHIFT));

	if (xtpg->vtc) {
		struct xvtc_config config = {
			.hblank_start = width,
			.hsync_start = width + 1,
			.vblank_start = height,
			.vsync_start = height + 1,
		};
		unsigned int htotal;
		unsigned int vtotal;

		htotal = min_t(unsigned int, XVTC_MAX_HSIZE,
			       xtpg->hblank->cur.val + width);
		vtotal = min_t(unsigned int, XVTC_MAX_VSIZE,
			       xtpg->vblank->cur.val + height);

		config.hsync_end = htotal - 1;
		config.hsize = htotal;
		config.vsync_end = vtotal - 1;
		config.vsize = vtotal;

		xvtc_generator_start(xtpg->vtc, &config);
	}

	/*
	 * Configure the bayer phase and video timing mux based on the
	 * operation mode (passthrough or test pattern generation).
	 */
	xtpg_clr_and_set(xtpg, XTPG_PATTERN_CONTROL,
			 XTPG_PATTERN_MASK, xtpg->pattern->cur.val);

	/*
	 * Switching between passthrough and test pattern generation modes isn't
	 * allowed during streaming, update the control range accordingly.
	 */
	passthrough = xtpg->pattern->cur.val == 0;
	__xtpg_update_pattern_control(xtpg, passthrough, !passthrough);

	/*
	 * For TPG v5.0, the bayer phase needs to be off for the pass through
	 * mode, otherwise the external input would be subsampled.
	 */
	bayer_phase = passthrough ? XTPG_BAYER_PHASE_OFF
		    : xtpg_get_bayer_phase(format->code);
	xtpg_write(xtpg, XTPG_BAYER_PHASE, bayer_phase);

	if (xtpg->vtmux_gpio)
		gpiod_set_value_cansleep(xtpg->vtmux_gpio, !passthrough);

	xtpg_start(xtpg);

	ret = xtpg_set_remote_stream(xtpg, true);
	if (ret) {
		xtpg_stop(xtpg);
		if (xtpg->vtc)
			xvtc_generator_stop(xtpg->vtc);

		__xtpg_update_pattern_control(xtpg, true, true);

		return ret;
	}

	return 0;
}

static int xtpg_disable_streams(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				u32 pad, u64 streams_mask)
{
	struct xtpg_device *xtpg = to_tpg(subdev);

	xtpg_set_remote_stream(xtpg, false);

	xtpg_stop(xtpg);
	if (xtpg->vtc)
		xvtc_generator_stop(xtpg->vtc);

	__xtpg_update_pattern_control(xtpg, true, true);

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int xtpg_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xtpg_device *xtpg = to_tpg(subdev);
	struct v4l2_mbus_framefmt *__format;
	u32 bayer_phase;

	__format = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	/* In two pads mode the source pad format is always identical to the
	 * sink pad format.
	 */
	if (xtpg->npads == 2 && fmt->pad == 1) {
		fmt->format = *__format;
		return 0;
	}

	/* Bayer phase is configurable at runtime */
	if (xtpg->bayer) {
		bayer_phase = xtpg_get_bayer_phase(fmt->format.code);
		if (bayer_phase != XTPG_BAYER_PHASE_OFF)
			__format->code = fmt->format.code;
	}

	__format->width = clamp_t(unsigned int, fmt->format.width,
				  XTPG_MIN_WIDTH, XTPG_MAX_WIDTH);
	__format->height = clamp_t(unsigned int, fmt->format.height,
				   XTPG_MIN_HEIGHT, XTPG_MAX_HEIGHT);

	fmt->format = *__format;

	/* Propagate the format to the source pad. */
	if (xtpg->npads == 2) {
		__format = v4l2_subdev_state_get_format(sd_state, 1);
		*__format = fmt->format;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static int xtpg_enum_mbus_code(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	struct v4l2_mbus_framefmt *format;

	/* Enumerating media bus codes based on the active configuration isn't
	 * supported yet.
	 */
	if (code->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	if (code->index)
		return -EINVAL;

	format = v4l2_subdev_state_get_format(sd_state, code->pad);

	code->code = format->code;

	return 0;
}

static int xtpg_enum_frame_size(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_frame_size_enum *fse)
{
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(sd_state, fse->pad);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	/* Min / max values for pad 0 is always fixed in both one and two pads
	 * modes. In two pads mode, the source pad(= 1) size is identical to
	 * the sink pad size
	 */
	if (fse->pad == 0) {
		fse->min_width = XTPG_MIN_WIDTH;
		fse->max_width = XTPG_MAX_WIDTH;
		fse->min_height = XTPG_MIN_HEIGHT;
		fse->max_height = XTPG_MAX_HEIGHT;
	} else {
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}

static int xtpg_init_state(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state)
{
	struct xtpg_device *xtpg = to_tpg(subdev);
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	for (i = 0; i < xtpg->npads; i++) {
		format = v4l2_subdev_state_get_format(sd_state, i);
		*format = xtpg->default_format;
	}

	return 0;
}

static int xtpg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xtpg_device *xtpg = container_of(ctrl->handler,
						struct xtpg_device,
						ctrl_handler);
	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		xtpg_clr_and_set(xtpg, XTPG_PATTERN_CONTROL,
				 XTPG_PATTERN_MASK, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_CROSS_HAIRS:
		xtpg_clr_or_set(xtpg, XTPG_PATTERN_CONTROL,
				XTPG_PATTERN_CONTROL_CROSS_HAIRS, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_MOVING_BOX:
		xtpg_clr_or_set(xtpg, XTPG_PATTERN_CONTROL,
				XTPG_PATTERN_CONTROL_MOVING_BOX, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_COLOR_MASK:
		xtpg_clr_and_set(xtpg, XTPG_PATTERN_CONTROL,
				 XTPG_PATTERN_CONTROL_COLOR_MASK_MASK,
				 ctrl->val <<
				 XTPG_PATTERN_CONTROL_COLOR_MASK_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_STUCK_PIXEL:
		xtpg_clr_or_set(xtpg, XTPG_PATTERN_CONTROL,
				XTPG_PATTERN_CONTROL_STUCK_PIXEL, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_NOISE:
		xtpg_clr_or_set(xtpg, XTPG_PATTERN_CONTROL,
				XTPG_PATTERN_CONTROL_NOISE, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_MOTION:
		xtpg_clr_or_set(xtpg, XTPG_PATTERN_CONTROL,
				XTPG_PATTERN_CONTROL_MOTION, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_MOTION_SPEED:
		xtpg_write(xtpg, XTPG_MOTION_SPEED, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_CROSS_HAIR_ROW:
		xtpg_clr_and_set(xtpg, XTPG_CROSS_HAIRS,
				 XTPG_CROSS_HAIRS_ROW_MASK,
				 ctrl->val << XTPG_CROSS_HAIRS_ROW_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_CROSS_HAIR_COLUMN:
		xtpg_clr_and_set(xtpg, XTPG_CROSS_HAIRS,
				 XTPG_CROSS_HAIRS_COLUMN_MASK,
				 ctrl->val << XTPG_CROSS_HAIRS_COLUMN_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_HOR_START:
		xtpg_clr_and_set(xtpg, XTPG_ZPLATE_HOR_CONTROL,
				 XTPG_ZPLATE_START_MASK,
				 ctrl->val << XTPG_ZPLATE_START_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_HOR_SPEED:
		xtpg_clr_and_set(xtpg, XTPG_ZPLATE_HOR_CONTROL,
				 XTPG_ZPLATE_SPEED_MASK,
				 ctrl->val << XTPG_ZPLATE_SPEED_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_VER_START:
		xtpg_clr_and_set(xtpg, XTPG_ZPLATE_VER_CONTROL,
				 XTPG_ZPLATE_START_MASK,
				 ctrl->val << XTPG_ZPLATE_START_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_VER_SPEED:
		xtpg_clr_and_set(xtpg, XTPG_ZPLATE_VER_CONTROL,
				 XTPG_ZPLATE_SPEED_MASK,
				 ctrl->val << XTPG_ZPLATE_SPEED_SHIFT);
		return 0;
	case V4L2_CID_XILINX_TPG_BOX_SIZE:
		xtpg_write(xtpg, XTPG_BOX_SIZE, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_BOX_COLOR:
		xtpg_write(xtpg, XTPG_BOX_COLOR, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_STUCK_PIXEL_THRESH:
		xtpg_write(xtpg, XTPG_STUCK_PIXEL_THRESH, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_NOISE_GAIN:
		xtpg_write(xtpg, XTPG_NOISE_GAIN, ctrl->val);
		return 0;
	}

	return 0;
}

static const struct v4l2_ctrl_ops xtpg_ctrl_ops = {
	.s_ctrl	= xtpg_s_ctrl,
};

static const struct v4l2_subdev_core_ops xtpg_core_ops = {
};

static const struct v4l2_subdev_video_ops xtpg_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops xtpg_pad_ops = {
	.enum_mbus_code		= xtpg_enum_mbus_code,
	.enum_frame_size	= xtpg_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= xtpg_set_format,
	.enable_streams		= xtpg_enable_streams,
	.disable_streams	= xtpg_disable_streams,
};

static const struct v4l2_subdev_ops xtpg_ops = {
	.core   = &xtpg_core_ops,
	.video  = &xtpg_video_ops,
	.pad    = &xtpg_pad_ops,
};

static const struct v4l2_subdev_internal_ops xtpg_internal_ops = {
	.init_state	= xtpg_init_state,
};

/*
 * Control Config
 */

static const char *const xtpg_pattern_strings[] = {
	"Passthrough",
	"Horizontal Ramp",
	"Vertical Ramp",
	"Temporal Ramp",
	"Solid Red",
	"Solid Green",
	"Solid Blue",
	"Solid Black",
	"Solid White",
	"Color Bars",
	"Zone Plate",
	"Tartan Color Bars",
	"Cross Hatch",
	"None",
	"Vertical/Horizontal Ramps",
	"Black/White Checker Board",
};

static struct v4l2_ctrl_config xtpg_ctrls[] = {
	{
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_CROSS_HAIRS,
		.name	= "Test Pattern: Cross Hairs",
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.min	= false,
		.max	= true,
		.step	= 1,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_MOVING_BOX,
		.name	= "Test Pattern: Moving Box",
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.min	= false,
		.max	= true,
		.step	= 1,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_COLOR_MASK,
		.name	= "Test Pattern: Color Mask",
		.type	= V4L2_CTRL_TYPE_BITMASK,
		.min	= 0,
		.max	= 0xf,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_STUCK_PIXEL,
		.name	= "Test Pattern: Stuck Pixel",
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.min	= false,
		.max	= true,
		.step	= 1,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_NOISE,
		.name	= "Test Pattern: Noise",
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.min	= false,
		.max	= true,
		.step	= 1,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_MOTION,
		.name	= "Test Pattern: Motion",
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.min	= false,
		.max	= true,
		.step	= 1,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_MOTION_SPEED,
		.name	= "Test Pattern: Motion Speed",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 8) - 1,
		.step	= 1,
		.def	= 4,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_CROSS_HAIR_ROW,
		.name	= "Test Pattern: Cross Hairs Row",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 12) - 1,
		.step	= 1,
		.def	= 0x64,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_CROSS_HAIR_COLUMN,
		.name	= "Test Pattern: Cross Hairs Column",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 12) - 1,
		.step	= 1,
		.def	= 0x64,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_ZPLATE_HOR_START,
		.name	= "Test Pattern: Zplate Horizontal Start Pos",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 16) - 1,
		.step	= 1,
		.def	= 0x1e,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_ZPLATE_HOR_SPEED,
		.name	= "Test Pattern: Zplate Horizontal Speed",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 16) - 1,
		.step	= 1,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_ZPLATE_VER_START,
		.name	= "Test Pattern: Zplate Vertical Start Pos",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 16) - 1,
		.step	= 1,
		.def	= 1,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_ZPLATE_VER_SPEED,
		.name	= "Test Pattern: Zplate Vertical Speed",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 16) - 1,
		.step	= 1,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_BOX_SIZE,
		.name	= "Test Pattern: Box Size",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 12) - 1,
		.step	= 1,
		.def	= 0x32,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_BOX_COLOR,
		.name	= "Test Pattern: Box Color(RGB)",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 24) - 1,
		.step	= 1,
		.def	= 0,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_STUCK_PIXEL_THRESH,
		.name	= "Test Pattern: Stuck Pixel threshold",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 16) - 1,
		.step	= 1,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xtpg_ctrl_ops,
		.id	= V4L2_CID_XILINX_TPG_NOISE_GAIN,
		.name	= "Test Pattern: Noise Gain",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 0,
		.max	= (1 << 8) - 1,
		.step	= 1,
		.def	= 0,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	},
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xtpg_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int __maybe_unused xtpg_pm_suspend(struct device *dev)
{
	struct xtpg_device *xtpg = dev_get_drvdata(dev);

	xtpg->saved_ctrl = xtpg_read(xtpg, XTPG_CTRL_CONTROL);
	xtpg_write(xtpg, XTPG_CTRL_CONTROL,
		   xtpg->saved_ctrl & ~XTPG_CTRL_CONTROL_SW_ENABLE);

	return 0;
}

static int __maybe_unused xtpg_pm_resume(struct device *dev)
{
	struct xtpg_device *xtpg = dev_get_drvdata(dev);

	xtpg_write(xtpg, XTPG_CTRL_CONTROL,
		   xtpg->saved_ctrl | XTPG_CTRL_CONTROL_SW_ENABLE);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xtpg_parse_of(struct xtpg_device *xtpg)
{
	struct device *dev = xtpg->dev;
	struct device_node *node = xtpg->dev->of_node;
	unsigned int nports = 0;
	bool has_endpoint = false;

	for_each_of_graph_port(node, port) {
		const struct xtpg_video_format *format;
		struct device_node *endpoint;

		format = xtpg_of_get_format(port);
		if (IS_ERR(format)) {
			dev_err(dev, "invalid format in DT");
			return PTR_ERR(format);
		}

		/* Get and check the format description */
		if (!xtpg->vip_format) {
			xtpg->vip_format = format;
		} else if (xtpg->vip_format != format) {
			dev_err(dev, "in/out format mismatch in DT");
			return -EINVAL;
		}

		if (nports == 0) {
			endpoint = of_graph_get_next_port_endpoint(port, NULL);
			if (endpoint)
				has_endpoint = true;
			of_node_put(endpoint);
		}

		/* Count the number of ports. */
		nports++;
	}

	if (nports != 1 && nports != 2) {
		dev_err(dev, "invalid number of ports %u\n", nports);
		return -EINVAL;
	}

	xtpg->npads = nports;
	if (nports == 2 && has_endpoint)
		xtpg->has_input = true;

	return 0;
}

static int xtpg_notify_bound(struct v4l2_async_notifier *notifier,
			     struct v4l2_subdev *sd,
			     struct v4l2_async_connection *asc)
{
	struct xtpg_device *xtpg =
		container_of(notifier, struct xtpg_device, notifier);

	return v4l2_create_fwnode_links_to_pad(sd, &xtpg->pads[0],
					       MEDIA_LNK_FL_ENABLED |
					       MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations xtpg_notify_ops = {
	.bound = xtpg_notify_bound,
};

static int xtpg_register_notifier(struct xtpg_device *xtpg)
{
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	/* Port 0 is the sink port only when the TPG has an input port. */
	if (xtpg->npads != 2)
		return 0;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(xtpg->dev), 0, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return 0;

	v4l2_async_subdev_nf_init(&xtpg->notifier, &xtpg->subdev);
	xtpg->notifier.ops = &xtpg_notify_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&xtpg->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		v4l2_async_nf_cleanup(&xtpg->notifier);
		return PTR_ERR(asc);
	}

	ret = v4l2_async_nf_register(&xtpg->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&xtpg->notifier);

	return ret;
}

static int xtpg_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xtpg_device *xtpg;
	u32 i, bayer_phase;
	u32 size;
	int ret;

	xtpg = devm_kzalloc(&pdev->dev, sizeof(*xtpg), GFP_KERNEL);
	if (!xtpg)
		return -ENOMEM;

	xtpg->dev = &pdev->dev;

	ret = xtpg_parse_of(xtpg);
	if (ret < 0)
		return ret;

	xtpg->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xtpg->iomem))
		return PTR_ERR(xtpg->iomem);

	xtpg->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(xtpg->clk))
		return PTR_ERR(xtpg->clk);

	xtpg->vtmux_gpio = devm_gpiod_get_optional(&pdev->dev, "timing",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(xtpg->vtmux_gpio))
		return PTR_ERR(xtpg->vtmux_gpio);

	xtpg->vtc = xvtc_of_get(pdev->dev.of_node);
	if (IS_ERR(xtpg->vtc))
		return PTR_ERR(xtpg->vtc);

	/* Reset and initialize the core */
	xtpg_reset(xtpg);

	/* Initialize V4L2 subdevice and media entity. Pad numbers depend on the
	 * number of pads.
	 */
	if (xtpg->npads == 2) {
		xtpg->pads[0].flags = MEDIA_PAD_FL_SINK;
		xtpg->pads[1].flags = MEDIA_PAD_FL_SOURCE;
	} else {
		xtpg->pads[0].flags = MEDIA_PAD_FL_SOURCE;
	}

	/* Initialize the default format */
	xtpg->default_format.code = xtpg->vip_format->code;
	xtpg->default_format.field = V4L2_FIELD_NONE;
	xtpg->default_format.colorspace = V4L2_COLORSPACE_SRGB;

	size = xtpg_read(xtpg, XTPG_ACTIVE_SIZE);
	xtpg->default_format.width = (size & XTPG_ACTIVE_HSIZE_MASK) >>
				     XTPG_ACTIVE_HSIZE_SHIFT;
	xtpg->default_format.height = (size & XTPG_ACTIVE_VSIZE_MASK) >>
				      XTPG_ACTIVE_VSIZE_SHIFT;

	bayer_phase = xtpg_get_bayer_phase(xtpg->vip_format->code);
	if (bayer_phase != XTPG_BAYER_PHASE_OFF)
		xtpg->bayer = true;

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xtpg->subdev;
	v4l2_subdev_init(subdev, &xtpg_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xtpg_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xtpg);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xtpg_media_ops;

	ret = media_entity_pads_init(&subdev->entity, xtpg->npads, xtpg->pads);
	if (ret < 0)
		goto error;

	v4l2_ctrl_handler_init(&xtpg->ctrl_handler, 3 + ARRAY_SIZE(xtpg_ctrls));

	xtpg->vblank = v4l2_ctrl_new_std(&xtpg->ctrl_handler, &xtpg_ctrl_ops,
					 V4L2_CID_VBLANK, XTPG_MIN_VBLANK,
					 XTPG_MAX_VBLANK, 1, 100);
	xtpg->hblank = v4l2_ctrl_new_std(&xtpg->ctrl_handler, &xtpg_ctrl_ops,
					 V4L2_CID_HBLANK, XTPG_MIN_HBLANK,
					 XTPG_MAX_HBLANK, 1, 100);
	xtpg->pattern = v4l2_ctrl_new_std_menu_items(&xtpg->ctrl_handler,
						     &xtpg_ctrl_ops, V4L2_CID_TEST_PATTERN,
						     ARRAY_SIZE(xtpg_pattern_strings) - 1,
						     1, 9, xtpg_pattern_strings);

	for (i = 0; i < ARRAY_SIZE(xtpg_ctrls); i++)
		v4l2_ctrl_new_custom(&xtpg->ctrl_handler, &xtpg_ctrls[i], NULL);

	if (xtpg->ctrl_handler.error) {
		dev_err(&pdev->dev, "failed to add controls\n");
		ret = xtpg->ctrl_handler.error;
		goto error;
	}
	subdev->ctrl_handler = &xtpg->ctrl_handler;

	xtpg_update_pattern_control(xtpg, true, true);

	ret = v4l2_ctrl_handler_setup(&xtpg->ctrl_handler);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to set controls\n");
		goto error;
	}

	subdev->state_lock = subdev->ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, xtpg);

	xtpg_print_version(xtpg);

	ret = xtpg_register_notifier(xtpg);
	if (ret < 0)
		goto error;

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	v4l2_async_nf_unregister(&xtpg->notifier);
	v4l2_async_nf_cleanup(&xtpg->notifier);
	v4l2_ctrl_handler_free(&xtpg->ctrl_handler);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	xvtc_put(xtpg->vtc);
	return ret;
}

static void xtpg_remove(struct platform_device *pdev)
{
	struct xtpg_device *xtpg = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xtpg->subdev;

	v4l2_async_nf_unregister(&xtpg->notifier);
	v4l2_async_nf_cleanup(&xtpg->notifier);
	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&xtpg->ctrl_handler);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
}

static SIMPLE_DEV_PM_OPS(xtpg_pm_ops, xtpg_pm_suspend, xtpg_pm_resume);

static const struct of_device_id xtpg_of_id_table[] = {
	{ .compatible = "xlnx,v-tpg-5.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, xtpg_of_id_table);

static struct platform_driver xtpg_driver = {
	.driver = {
		.name		= "xilinx-tpg",
		.pm		= &xtpg_pm_ops,
		.of_match_table	= xtpg_of_id_table,
	},
	.probe			= xtpg_probe,
	.remove			= xtpg_remove,
};

module_platform_driver(xtpg_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Xilinx Test Pattern Generator Driver");
MODULE_LICENSE("GPL");
