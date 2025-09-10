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

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/platform_device.h>
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "xilinx-hls-common.h"
#include "xilinx-vip.h"
#include "xilinx-vtc.h"

/* TPG v8 is a completely redesigned IP using Vivado HLS
 * having a different AXI4-Lite interface
 */
#define XTPG_HLS_BG_PATTERN			0x0020
#define XTPG_HLS_FG_PATTERN			0x0028
#define XTPG_HLS_FG_PATTERN_CROSS_HAIR		(1 << 1)
#define XTPG_HLS_MASK_ID			0x0030
#define XTPG_HLS_MOTION_SPEED			0x0038
#define XTPG_HLS_COLOR_FORMAT			0x0040
#define XTPG_HLS_COLOR_FORMAT_RGB		0
#define XTPG_HLS_COLOR_FORMAT_YUV_444		1
#define XTPG_HLS_COLOR_FORMAT_YUV_422		2
#define XTPG_HLS_COLOR_FORMAT_YUV_420		3
#define XTPG_HLS_CROSS_HAIR_HOR			0x0048
#define XTPG_HLS_CROSS_HAIR_VER			0x0050
#define XTPG_HLS_ZPLATE_HOR_CNTL_START		0x0058
#define XTPG_HLS_ZPLATE_HOR_CNTL_DELTA		0x0060
#define XTPG_HLS_ZPLATE_VER_CNTL_START		0x0068
#define XTPG_HLS_ZPLATE_VER_CNTL_DELTA		0x0070
#define XTPG_HLS_BOX_SIZE			0x0078
#define XTPG_HLS_BOX_COLOR_RED_CB		0x0080
#define XTPG_HLS_BOX_COLOR_GREEN_CR		0x0088
#define XTPG_HLS_BOX_COLOR_BLUE_Y		0x0090
#define XTPG_HLS_ENABLE_INPUT			0x0098
#define XTPG_HLS_USE_INPUT_VID_STREAM		(1 << 0)
#define XTPG_HLS_PASS_THRU_START_X		0x00a0
#define XTPG_HLS_PASS_THRU_START_Y		0x00a8
#define XTPG_HLS_PASS_THRU_END_X		0x00b0
#define XTPG_HLS_PASS_THRU_END_Y		0x00b8

/*
 * The minimum blanking value is one clock cycle for the front porch, one clock
 * cycle for the sync pulse and one clock cycle for the back porch.
 */
#define XTPG_MIN_HBLANK			3
#define XTPG_MAX_HBLANK			(XVTC_MAX_HSIZE - XVIP_MIN_WIDTH)
#define XTPG_MIN_VBLANK			3
#define XTPG_MAX_VBLANK			(XVTC_MAX_VSIZE - XVIP_MIN_HEIGHT)

#define XTPG_MIN_WIDTH			(64)
#define XTPG_MIN_HEIGHT			(64)
#define XTPG_MAX_WIDTH			(10328)
#define XTPG_MAX_HEIGHT			(7760)

#define XTPG_MIN_PPC			1

/**
 * struct xtpg_device - Xilinx Test Pattern Generator device structure
 * @xvip: Xilinx Video IP device
 * @pads: media pads
 * @npads: number of pads (1 or 2)
 * @has_input: whether an input is connected to the sink pad
 * @default_format: default V4L2 media bus format
 * @ctrl_handler: control handler
 * @hblank: horizontal blanking control
 * @vblank: vertical blanking control
 * @pattern: test pattern control
 * @streaming: is the video stream active
 * @vtc: video timing controller
 * @vtmux_gpio: video timing mux GPIO
 * @rst_gpio: reset IP core GPIO
 * @max_width: Maximum width supported by this instance
 * @max_height: Maximum height supported by this instance
 * @ppc: Pixels per clock control
 */
struct xtpg_device {
	struct xvip_device xvip;

	struct media_pad pads[2];
	unsigned int npads;
	bool has_input;

	struct v4l2_mbus_framefmt default_format;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *pattern;
	bool streaming;

	struct xvtc_device *vtc;
	struct gpio_desc *vtmux_gpio;
	struct gpio_desc *rst_gpio;

	u32 max_width;
	u32 max_height;
	u32 ppc;

	const struct xtpg_device_info *info;
};

static inline struct xtpg_device *to_tpg(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xtpg_device, xvip.subdev);
}

static void xtpg_config_vtc(struct xtpg_device *xtpg,
			    const struct v4l2_mbus_framefmt *format)
{
	struct xvtc_config config = {
		.hblank_start = format->width / xtpg->ppc,
		.hsync_start = format->width / xtpg->ppc + 1,
		.vblank_start = format->height,
		.vsync_start = format->height + 1,
	};
	unsigned int htotal;
	unsigned int vtotal;

	htotal = min_t(unsigned int, XVTC_MAX_HSIZE,
		       (xtpg->hblank->val + format->width) / xtpg->ppc);
	vtotal = min_t(unsigned int, XVTC_MAX_VSIZE,
		       xtpg->vblank->val + format->height);

	config.hsync_end = htotal - 1;
	config.hsize = htotal;
	config.vsync_end = vtotal - 1;
	config.vsize = vtotal;

	xvtc_generator_start(xtpg->vtc, &config);
}

static void xtpg_update_pattern_control(struct xtpg_device *xtpg,
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

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Video Operations
 */

static int xtpg_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct xtpg_device *xtpg = to_tpg(subdev);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	bool passthrough;
	u32 fmt;

	state = v4l2_subdev_lock_and_get_active_state(subdev);

	if (!enable) {
		if (xtpg->vtc)
			xvtc_generator_stop(xtpg->vtc);

		xtpg_update_pattern_control(xtpg, true, true);
		xtpg->streaming = false;

		gpiod_set_value_cansleep(xtpg->rst_gpio, 1);

		goto unlock;
	}

	/* Release the reset */
	gpiod_set_value_cansleep(xtpg->rst_gpio, 0);

	format = v4l2_subdev_state_get_format(state, 0);

	switch (format->code) {
	case MEDIA_BUS_FMT_VYUY8_1X16:
	case MEDIA_BUS_FMT_UYVY10_1X20:
		fmt = XTPG_HLS_COLOR_FORMAT_YUV_422;
		break;
	case MEDIA_BUS_FMT_VUY8_1X24:
		fmt = XTPG_HLS_COLOR_FORMAT_YUV_444;
		break;
	case MEDIA_BUS_FMT_RBG888_1X24:
		fmt = XTPG_HLS_COLOR_FORMAT_RGB;
		break;
	default:
		fmt = 0;
		break;
	}

	xvip_write(&xtpg->xvip, XTPG_HLS_COLOR_FORMAT, fmt);
	xvip_write(&xtpg->xvip, XHLS_REG_COLS, format->width);
	xvip_write(&xtpg->xvip, XHLS_REG_ROWS, format->height);

	if (xtpg->vtc)
		xtpg_config_vtc(xtpg, format);

	xvip_write(&xtpg->xvip, XTPG_HLS_BG_PATTERN, xtpg->pattern->cur.val);

	/*
	 * Switching between passthrough and test pattern generation modes isn't
	 * allowed during streaming, update the control range accordingly.
	 */
	passthrough = xtpg->pattern->cur.val == 0;
	xtpg_update_pattern_control(xtpg, passthrough, !passthrough);

	xtpg->streaming = true;

	if (xtpg->vtmux_gpio)
		gpiod_set_value_cansleep(xtpg->vtmux_gpio, !passthrough);

	xvip_set(&xtpg->xvip, XTPG_HLS_ENABLE_INPUT,
		 XTPG_HLS_USE_INPUT_VID_STREAM);
	xvip_set(&xtpg->xvip, XVIP_CTRL_CONTROL,
		 XHLS_REG_CTRL_AUTO_RESTART | XVIP_CTRL_CONTROL_SW_ENABLE);

unlock:
	v4l2_subdev_unlock_state(state);

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
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(sd_state, 0);

	/* In two pads mode the source pad format is always identical to the
	 * sink pad format.
	 */
	if (xtpg->npads == 2 && fmt->pad == 1) {
		fmt->format = *format;
		return 0;
	}

	switch (fmt->format.code) {
	// YUV422
	case MEDIA_BUS_FMT_VYUY8_1X16:
	case MEDIA_BUS_FMT_UYVY10_1X20:
	// YUV444
	case MEDIA_BUS_FMT_VUY8_1X24:
	// RGB
	case MEDIA_BUS_FMT_RBG888_1X24:
		format->code = fmt->format.code;
		break;
	default:
		format->code = xtpg->default_format.code;
	}

	format->width = clamp_t(unsigned int, fmt->format.width, XTPG_MIN_WIDTH,
				xtpg->max_width);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 XTPG_MIN_HEIGHT, xtpg->max_height);

	fmt->format = *format;

	/* Propagate the format to the source pad. */
	if (xtpg->npads == 2) {
		format = v4l2_subdev_state_get_format(sd_state, 1);
		*format = fmt->format;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static int xtpg_enum_frame_size(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_frame_size_enum *fse)
{
	struct v4l2_mbus_framefmt *format;
	struct xtpg_device *xtpg = to_tpg(subdev);

	// XXX what is this trying to do?

	format = v4l2_subdev_state_get_format(sd_state, fse->pad);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	/* Min / max values for pad 0 is always fixed in both one and two pads
	 * modes. In two pads mode, the source pad(= 1) size is identical to
	 * the sink pad size */
	if (fse->pad == 0) {
		fse->min_width = XTPG_MIN_WIDTH;
		fse->max_width = xtpg->max_width;
		fse->min_height = XTPG_MIN_HEIGHT;
		fse->max_height = xtpg->max_height;
	} else {
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}

static int xtpg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xtpg_device *xtpg =
		container_of(ctrl->handler, struct xtpg_device, ctrl_handler);
	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		xvip_write(&xtpg->xvip, XTPG_HLS_BG_PATTERN, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_COLOR_MASK:
		xvip_write(&xtpg->xvip, XTPG_HLS_MASK_ID, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_MOTION_SPEED:
		xvip_write(&xtpg->xvip, XTPG_HLS_MOTION_SPEED, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_CROSS_HAIR_ROW:
		xvip_write(&xtpg->xvip, XTPG_HLS_CROSS_HAIR_HOR, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_CROSS_HAIR_COLUMN:
		xvip_write(&xtpg->xvip, XTPG_HLS_CROSS_HAIR_VER, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_HOR_START:
		xvip_write(&xtpg->xvip, XTPG_HLS_ZPLATE_HOR_CNTL_START,
			   ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_HOR_SPEED:
		xvip_write(&xtpg->xvip, XTPG_HLS_ZPLATE_HOR_CNTL_DELTA,
			   ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_VER_START:
		xvip_write(&xtpg->xvip, XTPG_HLS_ZPLATE_VER_CNTL_START,
			   ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_ZPLATE_VER_SPEED:
		xvip_write(&xtpg->xvip, XTPG_HLS_ZPLATE_VER_CNTL_DELTA,
			   ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_BOX_SIZE:
		xvip_write(&xtpg->xvip, XTPG_HLS_BOX_SIZE, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_BOX_COLOR:
		xvip_write(&xtpg->xvip, XTPG_HLS_BOX_COLOR_RED_CB,
			   ctrl->val >> 16);
		xvip_write(&xtpg->xvip, XTPG_HLS_BOX_COLOR_GREEN_CR,
			   ctrl->val >> 8);
		xvip_write(&xtpg->xvip, XTPG_HLS_BOX_COLOR_BLUE_Y, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_TPG_HLS_FG_PATTERN:
		xvip_write(&xtpg->xvip, XTPG_HLS_FG_PATTERN, ctrl->val);
		return 0;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Internal Operations
 */

static int xtpg_init_state(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state)
{
	struct xtpg_device *xtpg = to_tpg(sd);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(state, 0);
	*format = xtpg->default_format;

	if (xtpg->npads == 2) {
		format = v4l2_subdev_state_get_format(state, 1);
		*format = xtpg->default_format;
	}

	return 0;
}

static const struct v4l2_ctrl_ops xtpg_ctrl_ops = {
	.s_ctrl = xtpg_s_ctrl,
};

static const struct v4l2_subdev_core_ops xtpg_core_ops = {};

static const struct v4l2_subdev_video_ops xtpg_video_ops = {
	.s_stream = xtpg_s_stream,
};

static const struct v4l2_subdev_pad_ops xtpg_pad_ops = {
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xtpg_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xtpg_set_format,
};

static const struct v4l2_subdev_ops xtpg_ops = {
	.core = &xtpg_core_ops,
	.video = &xtpg_video_ops,
	.pad = &xtpg_pad_ops,
};

static const struct v4l2_subdev_internal_ops xtpg_internal_ops = {
	.init_state = xtpg_init_state,
};

/*
 * Control Config
 */

static const char *const xtpg_hls_pattern_strings[] = {
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
	"Color Sweep",
	"Vertical/Horizontal Ramps",
	"Black/White Checker Board",
	"PseudoRandom",
};

static const char *const xtpg_hls_fg_strings[] = {
	"No Overlay",
	"Moving Box",
	"Cross Hairs",
};

static const struct v4l2_ctrl_config xtpg_hls_fg_ctrl = {
	.ops = &xtpg_ctrl_ops,
	.id = V4L2_CID_XILINX_TPG_HLS_FG_PATTERN,
	.name = "Test Pattern: Foreground Pattern",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = 0,
	.max = ARRAY_SIZE(xtpg_hls_fg_strings) - 1,
	.qmenu = xtpg_hls_fg_strings,
};

static struct v4l2_ctrl_config xtpg_common_ctrls[] = {
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_COLOR_MASK,
		.name = "Test Pattern: Color Mask",
		.type = V4L2_CTRL_TYPE_BITMASK,
		.min = 0,
		.max = 0x7,
		.def = 0,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_MOTION_SPEED,
		.name = "Test Pattern: Motion Speed",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 8) - 1,
		.step = 1,
		.def = 4,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_CROSS_HAIR_ROW,
		.name = "Test Pattern: Cross Hairs Row",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 12) - 1,
		.step = 1,
		.def = 0x64,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_CROSS_HAIR_COLUMN,
		.name = "Test Pattern: Cross Hairs Column",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 12) - 1,
		.step = 1,
		.def = 0x64,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_ZPLATE_HOR_START,
		.name = "Test Pattern: Zplate Horizontal Start Pos",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 16) - 1,
		.step = 1,
		.def = 0x1e,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_ZPLATE_HOR_SPEED,
		.name = "Test Pattern: Zplate Horizontal Speed",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 16) - 1,
		.step = 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_ZPLATE_VER_START,
		.name = "Test Pattern: Zplate Vertical Start Pos",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 16) - 1,
		.step = 1,
		.def = 1,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_ZPLATE_VER_SPEED,
		.name = "Test Pattern: Zplate Vertical Speed",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 16) - 1,
		.step = 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_BOX_SIZE,
		.name = "Test Pattern: Box Size",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 12) - 1,
		.step = 1,
		.def = 0x32,
		.flags = V4L2_CTRL_FLAG_SLIDER,
	},
	{
		.ops = &xtpg_ctrl_ops,
		.id = V4L2_CID_XILINX_TPG_BOX_COLOR,
		.name = "Test Pattern: Box Color(RGB/YCbCr)",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = (1 << 24) - 1,
		.step = 1,
		.def = 0,
	},
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xtpg_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int __maybe_unused xtpg_pm_suspend(struct device *dev)
{
	struct xtpg_device *xtpg = dev_get_drvdata(dev);

	xvip_suspend(&xtpg->xvip);

	return 0;
}

static int __maybe_unused xtpg_pm_resume(struct device *dev)
{
	struct xtpg_device *xtpg = dev_get_drvdata(dev);

	xvip_resume(&xtpg->xvip);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xtpg_parse_ports(struct xtpg_device *xtpg)
{
	struct device *dev = xtpg->xvip.dev;
	struct fwnode_handle *ports;
	struct fwnode_handle *port;
	unsigned int nports = 0;
	bool has_endpoint = false;
	int ret;

	ret = device_property_read_u32(dev, "xlnx,max-height",
				       &xtpg->max_height);
	if (ret < 0) {
		dev_err(dev, "xlnx,max-height dt property is missing!");
		return -EINVAL;
	} else if (xtpg->max_height > XTPG_MAX_HEIGHT ||
		   xtpg->max_height < XTPG_MIN_HEIGHT) {
		dev_err(dev, "Invalid height in dt");
		return -EINVAL;
	}

	ret = device_property_read_u32(dev, "xlnx,max-width", &xtpg->max_width);
	if (ret < 0) {
		dev_err(dev, "xlnx,max-width dt property is missing!");
		return -EINVAL;
	} else if (xtpg->max_width > XTPG_MAX_WIDTH ||
		   xtpg->max_width < XTPG_MIN_WIDTH) {
		dev_err(dev, "Invalid width in dt");
		return -EINVAL;
	}

	ret = device_property_read_u32(dev, "xlnx,ppc", &xtpg->ppc);
	if (ret < 0) {
		xtpg->ppc = XTPG_MIN_PPC;
		dev_dbg(dev, "failed to read ppc in dt\n");
	} else if ((xtpg->ppc != 1) && (xtpg->ppc != 2) && (xtpg->ppc != 4) &&
		   (xtpg->ppc != 8)) {
		dev_err(dev, "Invalid ppc config in dt\n");
		return -EINVAL;
	}

	ports = device_get_named_child_node(dev, "ports");
	if (!ports) {
		dev_err(dev, "ports node not present");
		return -EINVAL;
	}

	fwnode_for_each_child_node(ports, port) {
		struct fwnode_handle *endpoint;

		if (nports == 0) {
			endpoint = fwnode_get_next_child_node(port, NULL);
			if (endpoint)
				has_endpoint = true;
			fwnode_handle_put(endpoint);
		}

		/* Count the number of ports. */
		nports++;
	}

	if (nports != 1 && nports != 2) {
		dev_err(dev, "invalid number of ports %u\n", nports);
		ret = -EINVAL;
		goto out;
	}

	xtpg->npads = nports;
	if (nports == 2 && has_endpoint)
		xtpg->has_input = true;

out:
	fwnode_handle_put(ports);
	return ret;
}

static int xtpg_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xtpg_device *xtpg;
	u32 i;
	int ret;

	xtpg = devm_kzalloc(&pdev->dev, sizeof(*xtpg), GFP_KERNEL);
	if (!xtpg)
		return -ENOMEM;

	xtpg->info = of_device_get_match_data(&pdev->dev);

	xtpg->xvip.dev = &pdev->dev;

	ret = xtpg_parse_ports(xtpg);
	if (ret < 0)
		return ret;

	ret = xvip_init_resources(&xtpg->xvip);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to init xvip resources\n");
		return ret;
	}

	xtpg->vtmux_gpio =
		devm_gpiod_get_optional(&pdev->dev, "timing", GPIOD_OUT_HIGH);
	if (IS_ERR(xtpg->vtmux_gpio)) {
		ret = PTR_ERR(xtpg->vtmux_gpio);
		goto error_resource;
	}

	xtpg->rst_gpio =
		devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xtpg->rst_gpio)) {
		dev_err(&pdev->dev, "failed to get reset gpio\n");
		ret = PTR_ERR(xtpg->rst_gpio);
		goto error_resource;
	}

	msleep(10);

	/* Release the reset */
	gpiod_set_value_cansleep(xtpg->rst_gpio, 0);

	xtpg->vtc = xvtc_fwnode_get(dev_fwnode(&pdev->dev));
	if (IS_ERR(xtpg->vtc)) {
		ret = PTR_ERR(xtpg->vtc);
		goto error_resource;
	}

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
	xtpg->default_format.code = MEDIA_BUS_FMT_RBG888_1X24;
	xtpg->default_format.field = V4L2_FIELD_NONE;
	xtpg->default_format.colorspace = V4L2_COLORSPACE_SRGB;
	xtpg->default_format.width = xvip_read(&xtpg->xvip, XHLS_REG_COLS);
	xtpg->default_format.height = xvip_read(&xtpg->xvip, XHLS_REG_ROWS);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xtpg->xvip.subdev;
	v4l2_subdev_init(subdev, &xtpg_ops);
	subdev->internal_ops = &xtpg_internal_ops;
	subdev->dev = &pdev->dev;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xtpg);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_COMPOSER;
	subdev->entity.ops = &xtpg_media_ops;

	ret = media_entity_pads_init(&subdev->entity, xtpg->npads, xtpg->pads);
	if (ret < 0)
		goto error;

	v4l2_ctrl_handler_init(&xtpg->ctrl_handler,
			       4 + ARRAY_SIZE(xtpg_common_ctrls));

	xtpg->vblank = v4l2_ctrl_new_std(&xtpg->ctrl_handler, &xtpg_ctrl_ops,
					 V4L2_CID_VBLANK, XTPG_MIN_VBLANK,
					 XTPG_MAX_VBLANK, 1, 100);
	xtpg->hblank = v4l2_ctrl_new_std(&xtpg->ctrl_handler, &xtpg_ctrl_ops,
					 V4L2_CID_HBLANK, XTPG_MIN_HBLANK,
					 XTPG_MAX_HBLANK, 1, 100);

	xtpg->pattern = v4l2_ctrl_new_std_menu_items(
		&xtpg->ctrl_handler, &xtpg_ctrl_ops, V4L2_CID_TEST_PATTERN,
		ARRAY_SIZE(xtpg_hls_pattern_strings) - 1, 1, 9,
		xtpg_hls_pattern_strings);
	v4l2_ctrl_new_custom(&xtpg->ctrl_handler, &xtpg_hls_fg_ctrl, NULL);

	for (i = 0; i < ARRAY_SIZE(xtpg_common_ctrls); i++)
		v4l2_ctrl_new_custom(&xtpg->ctrl_handler, &xtpg_common_ctrls[i],
				     NULL);

	if (xtpg->ctrl_handler.error) {
		dev_err(&pdev->dev, "failed to add controls\n");
		ret = xtpg->ctrl_handler.error;
		goto error;
	}
	subdev->ctrl_handler = &xtpg->ctrl_handler;

	subdev->state_lock = subdev->ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret)
		goto error;

	mutex_lock(xtpg->ctrl_handler.lock);
	xtpg_update_pattern_control(xtpg, true, true);
	mutex_unlock(xtpg->ctrl_handler.lock);

	ret = v4l2_ctrl_handler_setup(&xtpg->ctrl_handler);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to set controls\n");
		goto error;
	}

	platform_set_drvdata(pdev, xtpg);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	// XXX FIX error handling
	v4l2_ctrl_handler_free(&xtpg->ctrl_handler);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	xvtc_put(xtpg->vtc);
error_resource:
	xvip_cleanup_resources(&xtpg->xvip);
	dev_err(&pdev->dev, "tpg probe failed: %d\n", ret);
	return ret;
}

static void xtpg_remove(struct platform_device *pdev)
{
	struct xtpg_device *xtpg = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xtpg->xvip.subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	v4l2_ctrl_handler_free(&xtpg->ctrl_handler);
	media_entity_cleanup(&subdev->entity);

	xvip_cleanup_resources(&xtpg->xvip);
}

static SIMPLE_DEV_PM_OPS(xtpg_pm_ops, xtpg_pm_suspend, xtpg_pm_resume);

static const struct of_device_id xtpg_of_id_table[] = {
	{ .compatible = "xlnx,v-tpg-8.0", .data = NULL },
	{}
};
MODULE_DEVICE_TABLE(of, xtpg_of_id_table);

static struct platform_driver xtpg_driver = {
	.driver = {
		.name		= "xilinx-hls-tpg",
		.pm		= &xtpg_pm_ops,
		.of_match_table	= of_match_ptr(xtpg_of_id_table),
	},
	.probe			= xtpg_probe,
	.remove			= xtpg_remove,
};

module_platform_driver(xtpg_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>");
MODULE_DESCRIPTION("Xilinx HLS Test Pattern Generator Driver");
MODULE_LICENSE("GPL v2");
