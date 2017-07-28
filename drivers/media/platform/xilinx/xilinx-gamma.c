// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Gamma Correction LUT
 *
 * Copyright (C) 2017 Xilinx, Inc.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#include "xilinx-gamma-coeff.h"

#define XGAMMA_PAD_SINK			0
#define XGAMMA_PAD_SOURCE		1

#define XGAMMA_MIN_WIDTH		64
#define XGAMMA_MAX_WIDTH		8192
#define XGAMMA_DEF_WIDTH		1280
#define XGAMMA_MIN_HEIGHT		64
#define XGAMMA_MAX_HEIGHT		4320
#define XGAMMA_DEF_HEIGHT		720

#define XGAMMA_AP_CTRL			0x0000
#define XGAMMA_AP_CTRL_START		BIT(0)
#define XGAMMA_AP_CTRL_AUTO_RESTART	BIT(7)
#define XGAMMA_WIDTH			0x0010
#define XGAMMA_HEIGHT			0x0018
#define XGAMMA_VIDEO_FORMAT		0x0020
#define XGAMMA_VIDEO_FORMAT_RGB		0
/* Look-up table @n, one 32-bit register per two entries. */
#define XGAMMA_GAMMA_LUT(n)		(0x0800 + (n) * 0x0800)

/*
 * Gamma look-up tables, in hardware order. The IP applies one table per
 * colour component.
 */
enum xgamma_lut {
	XGAMMA_LUT_RED,
	XGAMMA_LUT_GREEN,
	XGAMMA_LUT_BLUE,
	XGAMMA_NUM_LUTS,
};

/**
 * struct xgamma_dev - Xilinx Video Gamma LUT device structure
 * @subdev: V4L2 subdevice
 * @notifier: V4L2 async notifier for the upstream subdev
 * @dev: (OF) device
 * @iomem: device I/O register space remapped to kernel virtual memory
 * @clk: video core clock
 * @rst_gpio: GPIO reset line to bring the IP out of reset
 * @pads: media pads
 * @ctrl_handler: V4L2 control handler for the R, G and B gamma controls
 * @curves: gamma curves for the colour depth of this instance
 * @luts: gamma curve selected by the control of each colour component
 * @lut_length: number of entries in a gamma curve
 * @max_width: maximum width supported by this instance
 * @max_height: maximum height supported by this instance
 */
struct xgamma_dev {
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct device *dev;
	void __iomem *iomem;
	struct clk *clk;
	struct gpio_desc *rst_gpio;
	struct media_pad pads[2];
	struct v4l2_ctrl_handler ctrl_handler;

	const u16 * const *curves;
	const u16 *luts[XGAMMA_NUM_LUTS];
	unsigned int lut_length;
	u32 max_width;
	u32 max_height;
};

static inline struct xgamma_dev *to_xg(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xgamma_dev, subdev);
}

static inline void xg_write(struct xgamma_dev *xg, u32 reg, u32 data)
{
	iowrite32(data, xg->iomem + reg);
}

/*
 * Program the gamma curve currently selected for @lut into the corresponding
 * hardware look-up table. Two consecutive entries are packed in each 32-bit
 * register.
 */
static void xg_set_lut(struct xgamma_dev *xg, unsigned int lut)
{
	const u16 *entries = xg->luts[lut];
	unsigned int i;

	for (i = 0; i < xg->lut_length / 2; i++)
		xg_write(xg, XGAMMA_GAMMA_LUT(lut) + i * 4,
			 (entries[2 * i + 1] << 16) | entries[2 * i]);
}

/* The IP is stopped by pulsing its reset line. */
static void xg_stop(struct xgamma_dev *xg)
{
	/*
	 * Clear ap_start and auto-restart. The core finishes the frame in
	 * flight and then stops. This must be done unconditionally: the reset
	 * GPIO below is optional, as some designs reset the IP through a
	 * reset line shared with the rest of the pipeline.
	 */
	xg_write(xg, XGAMMA_AP_CTRL, 0);

	gpiod_set_value_cansleep(xg->rst_gpio, 1);
	gpiod_set_value_cansleep(xg->rst_gpio, 0);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int xg_set_remote_streams(struct xgamma_dev *xg, bool enable)
{
	struct media_pad *remote;
	struct v4l2_subdev *subdev;

	remote = media_pad_remote_pad_first(&xg->pads[XGAMMA_PAD_SINK]);

	/*
	 * The upstream entity may be a video node (an MM2S DMA), whose
	 * streaming is controlled by the DMA engine. Nothing to propagate.
	 */
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return 0;

	subdev = media_entity_to_v4l2_subdev(remote->entity);

	if (enable)
		return v4l2_subdev_enable_streams(subdev, remote->index,
						  BIT_ULL(0));

	return v4l2_subdev_disable_streams(subdev, remote->index, BIT_ULL(0));
}

static int xg_enable_streams(struct v4l2_subdev *subdev,
			     struct v4l2_subdev_state *state,
			     u32 pad, u64 streams_mask)
{
	struct xgamma_dev *xg = to_xg(subdev);
	const struct v4l2_mbus_framefmt *format;
	unsigned int i;
	int ret;

	format = v4l2_subdev_state_get_format(state, XGAMMA_PAD_SINK);

	xg_write(xg, XGAMMA_WIDTH, format->width);
	xg_write(xg, XGAMMA_HEIGHT, format->height);
	xg_write(xg, XGAMMA_VIDEO_FORMAT, XGAMMA_VIDEO_FORMAT_RGB);

	/* The look-up tables are cleared by the reset performed at stream off. */
	for (i = 0; i < XGAMMA_NUM_LUTS; i++)
		xg_set_lut(xg, i);

	xg_write(xg, XGAMMA_AP_CTRL,
		 XGAMMA_AP_CTRL_START | XGAMMA_AP_CTRL_AUTO_RESTART);

	/* Start the upstream subdev. */
	ret = xg_set_remote_streams(xg, true);
	if (ret < 0) {
		xg_stop(xg);
		return ret;
	}

	return 0;
}

static int xg_disable_streams(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_state *state,
			      u32 pad, u64 streams_mask)
{
	struct xgamma_dev *xg = to_xg(subdev);

	/* Stop the upstream subdev. */
	xg_set_remote_streams(xg, false);

	xg_stop(xg);

	return 0;
}

static int xg_enum_mbus_code(struct v4l2_subdev *subdev,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_RBG888_1X24;

	return 0;
}

static int xg_enum_frame_size(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_frame_size_enum *fse)
{
	struct xgamma_dev *xg = to_xg(subdev);

	if (fse->index || fse->code != MEDIA_BUS_FMT_RBG888_1X24)
		return -EINVAL;

	if (fse->pad == XGAMMA_PAD_SINK) {
		fse->min_width = XGAMMA_MIN_WIDTH;
		fse->max_width = xg->max_width;
		fse->min_height = XGAMMA_MIN_HEIGHT;
		fse->max_height = xg->max_height;
	} else {
		const struct v4l2_mbus_framefmt *format;

		/*
		 * The size on the source pad is fixed and always identical to
		 * the size on the sink pad.
		 */
		format = v4l2_subdev_state_get_format(sd_state, fse->pad);

		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}

static int xg_set_format(struct v4l2_subdev *subdev,
			 struct v4l2_subdev_state *sd_state,
			 struct v4l2_subdev_format *fmt)
{
	struct xgamma_dev *xg = to_xg(subdev);
	struct v4l2_mbus_framefmt *sink;

	/*
	 * The source pad format is derived from the sink pad format and cannot
	 * be set directly. Return the current format.
	 */
	if (fmt->pad == XGAMMA_PAD_SOURCE)
		return v4l2_subdev_get_fmt(subdev, sd_state, fmt);

	sink = v4l2_subdev_state_get_format(sd_state, XGAMMA_PAD_SINK);

	/* The gamma correction LUT only supports progressive RGB frames. */
	sink->code = MEDIA_BUS_FMT_RBG888_1X24;
	sink->field = V4L2_FIELD_NONE;
	sink->width = clamp_t(unsigned int, fmt->format.width,
			      XGAMMA_MIN_WIDTH, xg->max_width);
	sink->height = clamp_t(unsigned int, fmt->format.height,
			       XGAMMA_MIN_HEIGHT, xg->max_height);

	fmt->format = *sink;

	/*
	 * Propagate the format to the source pad. Gamma correction does not
	 * alter the format, so the two pads share it.
	 */
	*v4l2_subdev_state_get_format(sd_state, XGAMMA_PAD_SOURCE) = *sink;

	return 0;
}

static int xg_init_state(struct v4l2_subdev *subdev,
			 struct v4l2_subdev_state *sd_state)
{
	struct xgamma_dev *xg = to_xg(subdev);
	struct v4l2_mbus_framefmt *sink;

	/* The gamma correction LUT only supports RGB, identical on both pads. */
	sink = v4l2_subdev_state_get_format(sd_state, XGAMMA_PAD_SINK);
	sink->code = MEDIA_BUS_FMT_RBG888_1X24;
	sink->field = V4L2_FIELD_NONE;
	sink->colorspace = V4L2_COLORSPACE_SRGB;
	sink->width = min_t(u32, XGAMMA_DEF_WIDTH, xg->max_width);
	sink->height = min_t(u32, XGAMMA_DEF_HEIGHT, xg->max_height);

	*v4l2_subdev_state_get_format(sd_state, XGAMMA_PAD_SOURCE) = *sink;

	return 0;
}

static const struct v4l2_subdev_pad_ops xg_pad_ops = {
	.enum_mbus_code = xg_enum_mbus_code,
	.enum_frame_size = xg_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xg_set_format,
	.enable_streams = xg_enable_streams,
	.disable_streams = xg_disable_streams,
};

static const struct v4l2_subdev_ops xg_ops = {
	.pad = &xg_pad_ops,
};

static const struct v4l2_subdev_internal_ops xg_internal_ops = {
	.init_state = xg_init_state,
};

static const struct media_entity_operations xg_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * V4L2 Controls
 */

static int xg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xgamma_dev *xg =
		container_of(ctrl->handler, struct xgamma_dev, ctrl_handler);
	enum xgamma_lut lut;

	switch (ctrl->id) {
	case V4L2_CID_XILINX_GAMMA_CORR_RED_GAMMA:
		lut = XGAMMA_LUT_RED;
		break;
	case V4L2_CID_XILINX_GAMMA_CORR_GREEN_GAMMA:
		lut = XGAMMA_LUT_GREEN;
		break;
	case V4L2_CID_XILINX_GAMMA_CORR_BLUE_GAMMA:
		lut = XGAMMA_LUT_BLUE;
		break;
	default:
		return -EINVAL;
	}

	/* The control value is the gamma value multiplied by 10. */
	xg->luts[lut] = xg->curves[ctrl->val - 1];
	xg_set_lut(xg, lut);

	return 0;
}

static const struct v4l2_ctrl_ops xg_ctrl_ops = {
	.s_ctrl = xg_s_ctrl,
};

static const struct v4l2_ctrl_config xg_ctrls[] = {
	{
		.ops	= &xg_ctrl_ops,
		.id	= V4L2_CID_XILINX_GAMMA_CORR_RED_GAMMA,
		.name	= "Red Gamma Correction",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 1,
		.max	= XGAMMA_NUM_CURVES,
		.step	= 1,
		.def	= 10,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xg_ctrl_ops,
		.id	= V4L2_CID_XILINX_GAMMA_CORR_GREEN_GAMMA,
		.name	= "Green Gamma Correction",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 1,
		.max	= XGAMMA_NUM_CURVES,
		.step	= 1,
		.def	= 10,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	}, {
		.ops	= &xg_ctrl_ops,
		.id	= V4L2_CID_XILINX_GAMMA_CORR_BLUE_GAMMA,
		.name	= "Blue Gamma Correction",
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.min	= 1,
		.max	= XGAMMA_NUM_CURVES,
		.step	= 1,
		.def	= 10,
		.flags	= V4L2_CTRL_FLAG_SLIDER,
	},
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xg_parse_of(struct xgamma_dev *xg)
{
	struct device *dev = xg->dev;
	u32 color_depth;
	int ret;

	ret = device_property_read_u32(dev, "xlnx,max-width", &xg->max_width);
	if (ret < 0)
		return dev_err_probe(dev, ret, "xlnx,max-width is missing\n");

	if (xg->max_width < XGAMMA_MIN_WIDTH ||
	    xg->max_width > XGAMMA_MAX_WIDTH)
		return dev_err_probe(dev, -EINVAL, "invalid xlnx,max-width %u\n",
				     xg->max_width);

	ret = device_property_read_u32(dev, "xlnx,max-height", &xg->max_height);
	if (ret < 0)
		return dev_err_probe(dev, ret, "xlnx,max-height is missing\n");

	if (xg->max_height < XGAMMA_MIN_HEIGHT ||
	    xg->max_height > XGAMMA_MAX_HEIGHT)
		return dev_err_probe(dev, -EINVAL,
				     "invalid xlnx,max-height %u\n",
				     xg->max_height);

	ret = device_property_read_u32(dev, "xlnx,video-width", &color_depth);
	if (ret < 0)
		return dev_err_probe(dev, ret, "xlnx,video-width is missing\n");

	switch (color_depth) {
	case XGAMMA_BPC_8:
		xg->curves = xgamma8_curves;
		xg->lut_length = XGAMMA_LUT8_LENGTH;
		break;
	case XGAMMA_BPC_10:
		xg->curves = xgamma10_curves;
		xg->lut_length = XGAMMA_LUT10_LENGTH;
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "unsupported colour depth %u\n",
				     color_depth);
	}

	return 0;
}

static int xg_notify_bound(struct v4l2_async_notifier *notifier,
			   struct v4l2_subdev *sd,
			   struct v4l2_async_connection *asc)
{
	struct xgamma_dev *xg =
		container_of(notifier, struct xgamma_dev, notifier);

	return v4l2_create_fwnode_links_to_pad(sd,
					       &xg->pads[XGAMMA_PAD_SINK],
					       MEDIA_LNK_FL_ENABLED |
					       MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations xg_notify_ops = {
	.bound = xg_notify_bound,
};

static int xg_register_notifier(struct xgamma_dev *xg)
{
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(xg->dev),
					     XGAMMA_PAD_SINK, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return 0;

	v4l2_async_subdev_nf_init(&xg->notifier, &xg->subdev);
	xg->notifier.ops = &xg_notify_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&xg->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		v4l2_async_nf_cleanup(&xg->notifier);
		return PTR_ERR(asc);
	}

	ret = v4l2_async_nf_register(&xg->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&xg->notifier);

	return ret;
}

static int xg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_subdev *subdev;
	struct xgamma_dev *xg;
	unsigned int i;
	int ret;

	xg = devm_kzalloc(dev, sizeof(*xg), GFP_KERNEL);
	if (!xg)
		return -ENOMEM;

	xg->dev = dev;

	ret = xg_parse_of(xg);
	if (ret < 0)
		return ret;

	xg->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xg->iomem))
		return PTR_ERR(xg->iomem);

	xg->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(xg->clk))
		return dev_err_probe(dev, PTR_ERR(xg->clk),
				     "failed to get core clock\n");

	xg->rst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xg->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(xg->rst_gpio),
				     "failed to get reset GPIO\n");

	/* Bring the IP out of reset. */
	gpiod_set_value_cansleep(xg->rst_gpio, 0);

	/* Initialize the V4L2 subdevice and media entity. */
	subdev = &xg->subdev;
	v4l2_subdev_init(subdev, &xg_ops);
	subdev->dev = dev;
	subdev->internal_ops = &xg_internal_ops;
	strscpy(subdev->name, dev_name(dev));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xg_media_ops;
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_LUT;

	xg->pads[XGAMMA_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	xg->pads[XGAMMA_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&subdev->entity, ARRAY_SIZE(xg->pads),
				     xg->pads);
	if (ret < 0)
		return ret;

	v4l2_ctrl_handler_init(&xg->ctrl_handler, ARRAY_SIZE(xg_ctrls));
	for (i = 0; i < ARRAY_SIZE(xg_ctrls); i++)
		v4l2_ctrl_new_custom(&xg->ctrl_handler, &xg_ctrls[i], NULL);
	if (xg->ctrl_handler.error) {
		ret = xg->ctrl_handler.error;
		dev_err(dev, "failed to add controls\n");
		goto error_ctrl;
	}
	subdev->ctrl_handler = &xg->ctrl_handler;

	ret = v4l2_ctrl_handler_setup(&xg->ctrl_handler);
	if (ret < 0) {
		dev_err(dev, "failed to set controls\n");
		goto error_ctrl;
	}

	/*
	 * Share the control handler lock with the subdev state, as both the
	 * controls and the stream start program the gamma look-up tables.
	 */
	subdev->state_lock = subdev->ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_ctrl;

	platform_set_drvdata(pdev, xg);

	ret = xg_register_notifier(xg);
	if (ret < 0)
		goto error_subdev;

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto error_notifier;
	}

	return 0;

error_notifier:
	v4l2_async_nf_unregister(&xg->notifier);
	v4l2_async_nf_cleanup(&xg->notifier);
error_subdev:
	v4l2_subdev_cleanup(subdev);
error_ctrl:
	v4l2_ctrl_handler_free(&xg->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
	return ret;
}

static void xg_remove(struct platform_device *pdev)
{
	struct xgamma_dev *xg = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xg->subdev;

	v4l2_async_nf_unregister(&xg->notifier);
	v4l2_async_nf_cleanup(&xg->notifier);
	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	v4l2_ctrl_handler_free(&xg->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id xg_of_id_table[] = {
	{ .compatible = "xlnx,v-gamma-lut" },
	{ }
};
MODULE_DEVICE_TABLE(of, xg_of_id_table);

static struct platform_driver xg_driver = {
	.driver = {
		.name = "xilinx-gamma-lut",
		.of_match_table = xg_of_id_table,
	},
	.probe = xg_probe,
	.remove = xg_remove,
};

module_platform_driver(xg_driver);

MODULE_DESCRIPTION("Xilinx Video Gamma Correction LUT Driver");
MODULE_LICENSE("GPL");
