// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Video Demosaic IP
 *
 * Copyright (C) 2017 Xilinx, Inc.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <media/v4l2-async.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#define XDEMOSAIC_PAD_SINK		0
#define XDEMOSAIC_PAD_SOURCE		1
#define XDEMOSAIC_NUM_PADS		2

/* Register map */
#define XDEMOSAIC_AP_CTRL		0x00
#define XDEMOSAIC_AP_CTRL_START		BIT(0)
#define XDEMOSAIC_AP_CTRL_AUTO_RESTART	BIT(7)
#define XDEMOSAIC_WIDTH			0x10
#define XDEMOSAIC_HEIGHT		0x18
#define XDEMOSAIC_INPUT_BAYER_FORMAT	0x28

#define XDEMOSAIC_MIN_WIDTH		64U
#define XDEMOSAIC_MAX_WIDTH		8192U
#define XDEMOSAIC_DEF_WIDTH		1280U
#define XDEMOSAIC_MIN_HEIGHT		64U
#define XDEMOSAIC_MAX_HEIGHT		4320U
#define XDEMOSAIC_DEF_HEIGHT		720U

#define XDEMOSAIC_RESET_DEASSERT	0
#define XDEMOSAIC_RESET_ASSERT		1

/* Values of the XDEMOSAIC_INPUT_BAYER_FORMAT register. */
enum xdmsc_bayer_format {
	XDEMOSAIC_RGGB = 0,
	XDEMOSAIC_GRBG = 1,
	XDEMOSAIC_GBRG = 2,
	XDEMOSAIC_BGGR = 3,
};

/**
 * struct xdmsc_format_info - Sink pad format description
 * @code: media bus code of the Bayer format on the sink pad
 * @bayer_format: corresponding XDEMOSAIC_INPUT_BAYER_FORMAT register value
 */
struct xdmsc_format_info {
	u32 code;
	enum xdmsc_bayer_format bayer_format;
};

/*
 * Bayer formats supported on the sink pad. The first entry is used as the
 * default sink pad format.
 */
static const struct xdmsc_format_info xdmsc_formats[] = {
	{ MEDIA_BUS_FMT_SRGGB8_1X8,   XDEMOSAIC_RGGB },
	{ MEDIA_BUS_FMT_SGRBG8_1X8,   XDEMOSAIC_GRBG },
	{ MEDIA_BUS_FMT_SGBRG8_1X8,   XDEMOSAIC_GBRG },
	{ MEDIA_BUS_FMT_SBGGR8_1X8,   XDEMOSAIC_BGGR },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, XDEMOSAIC_RGGB },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, XDEMOSAIC_GRBG },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, XDEMOSAIC_GBRG },
	{ MEDIA_BUS_FMT_SBGGR10_1X10, XDEMOSAIC_BGGR },
	{ MEDIA_BUS_FMT_SRGGB12_1X12, XDEMOSAIC_RGGB },
	{ MEDIA_BUS_FMT_SGRBG12_1X12, XDEMOSAIC_GRBG },
	{ MEDIA_BUS_FMT_SGBRG12_1X12, XDEMOSAIC_GBRG },
	{ MEDIA_BUS_FMT_SBGGR12_1X12, XDEMOSAIC_BGGR },
	{ MEDIA_BUS_FMT_SRGGB16_1X16, XDEMOSAIC_RGGB },
	{ MEDIA_BUS_FMT_SGRBG16_1X16, XDEMOSAIC_GRBG },
	{ MEDIA_BUS_FMT_SGBRG16_1X16, XDEMOSAIC_GBRG },
	{ MEDIA_BUS_FMT_SBGGR16_1X16, XDEMOSAIC_BGGR },
};

/*
 * The demosaiced output is 8-bit RGB. Higher bit depths will be derived from
 * the sink pad code once their media bus codes are wired up.
 */
#define XDEMOSAIC_SOURCE_CODE		MEDIA_BUS_FMT_RBG888_1X24

/**
 * struct xdmsc_dev - Xilinx Video Demosaic device structure
 * @subdev: V4L2 subdevice
 * @notifier: async notifier for the upstream subdev
 * @dev: (OF) device
 * @iomem: device I/O register space remapped to kernel virtual memory
 * @pads: media pads
 * @rst_gpio: GPIO reset line to bring the IP out of reset
 * @max_width: maximum frame width supported by the IP
 * @max_height: maximum frame height supported by the IP
 */
struct xdmsc_dev {
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct device *dev;
	void __iomem *iomem;
	struct media_pad pads[XDEMOSAIC_NUM_PADS];
	struct gpio_desc *rst_gpio;
	u32 max_width;
	u32 max_height;
};

static inline struct xdmsc_dev *to_xdmsc(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xdmsc_dev, subdev);
}

static inline void xdmsc_write(struct xdmsc_dev *xdmsc, u32 reg, u32 data)
{
	iowrite32(data, xdmsc->iomem + reg);
}

static const struct xdmsc_format_info *xdmsc_get_format_info(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xdmsc_formats); ++i) {
		if (xdmsc_formats[i].code == code)
			return &xdmsc_formats[i];
	}

	return NULL;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int xdmsc_set_remote_streams(struct xdmsc_dev *xdmsc, bool enable)
{
	struct media_pad *remote;
	struct v4l2_subdev *subdev;

	remote = media_pad_remote_pad_first(&xdmsc->pads[XDEMOSAIC_PAD_SINK]);

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

static void xdmsc_stop(struct xdmsc_dev *xdmsc)
{
	/*
	 * Clear ap_start and auto-restart. The core finishes the frame in
	 * flight and then stops. This must be done unconditionally: the reset
	 * GPIO below is optional, as some designs reset the IP through a
	 * reset line shared with the rest of the pipeline.
	 */
	xdmsc_write(xdmsc, XDEMOSAIC_AP_CTRL, 0);

	gpiod_set_value_cansleep(xdmsc->rst_gpio, XDEMOSAIC_RESET_ASSERT);
	gpiod_set_value_cansleep(xdmsc->rst_gpio, XDEMOSAIC_RESET_DEASSERT);
}

static int xdmsc_enable_streams(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state,
				u32 pad, u64 streams_mask)
{
	struct xdmsc_dev *xdmsc = to_xdmsc(subdev);
	const struct xdmsc_format_info *info;
	const struct v4l2_mbus_framefmt *format;
	int ret;

	/*
	 * The sink pad format is restricted to the supported Bayer patterns by
	 * .init_state() and .set_fmt(), the lookup can't fail.
	 */
	format = v4l2_subdev_state_get_format(state, XDEMOSAIC_PAD_SINK);
	info = xdmsc_get_format_info(format->code);

	xdmsc_write(xdmsc, XDEMOSAIC_WIDTH, format->width);
	xdmsc_write(xdmsc, XDEMOSAIC_HEIGHT, format->height);
	xdmsc_write(xdmsc, XDEMOSAIC_INPUT_BAYER_FORMAT, info->bayer_format);

	/* Start the Demosaic IP in free-running mode. */
	xdmsc_write(xdmsc, XDEMOSAIC_AP_CTRL,
		    XDEMOSAIC_AP_CTRL_AUTO_RESTART | XDEMOSAIC_AP_CTRL_START);

	/* Start the upstream subdev. */
	ret = xdmsc_set_remote_streams(xdmsc, true);
	if (ret < 0) {
		xdmsc_stop(xdmsc);
		return ret;
	}

	return 0;
}

static int xdmsc_disable_streams(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct xdmsc_dev *xdmsc = to_xdmsc(subdev);

	/* Stop the upstream subdev. */
	xdmsc_set_remote_streams(xdmsc, false);

	xdmsc_stop(xdmsc);

	return 0;
}

static int xdmsc_set_format(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct xdmsc_dev *xdmsc = to_xdmsc(subdev);
	struct v4l2_mbus_framefmt *sink, *source;

	/*
	 * The source pad format is derived from the sink pad format and cannot
	 * be set directly. Return the current format.
	 */
	if (fmt->pad == XDEMOSAIC_PAD_SOURCE)
		return v4l2_subdev_get_fmt(subdev, sd_state, fmt);

	/* Fall back to the default Bayer pattern for unsupported codes. */
	if (!xdmsc_get_format_info(fmt->format.code))
		fmt->format.code = xdmsc_formats[0].code;

	fmt->format.width = clamp_t(unsigned int, fmt->format.width,
				    XDEMOSAIC_MIN_WIDTH, xdmsc->max_width);
	fmt->format.height = clamp_t(unsigned int, fmt->format.height,
				     XDEMOSAIC_MIN_HEIGHT, xdmsc->max_height);
	fmt->format.field = V4L2_FIELD_NONE;

	sink = v4l2_subdev_state_get_format(sd_state, XDEMOSAIC_PAD_SINK);
	*sink = fmt->format;

	/*
	 * Propagate the format to the source pad. The frame size and ancillary
	 * parameters are identical; only the media bus code differs, becoming
	 * the demosaiced RGB code.
	 */
	source = v4l2_subdev_state_get_format(sd_state, XDEMOSAIC_PAD_SOURCE);
	*source = *sink;
	source->code = XDEMOSAIC_SOURCE_CODE;

	return 0;
}

static int xdmsc_init_state(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_state *sd_state)
{
	struct xdmsc_dev *xdmsc = to_xdmsc(subdev);
	struct v4l2_mbus_framefmt *sink, *source;

	/*
	 * The sink pad can be any Bayer format, the default is RGGB. The source
	 * pad has a fixed RGB media bus format. Both pads share the same default
	 * frame size.
	 */
	sink = v4l2_subdev_state_get_format(sd_state, XDEMOSAIC_PAD_SINK);
	sink->code = xdmsc_formats[0].code;
	sink->width = min(XDEMOSAIC_DEF_WIDTH, xdmsc->max_width);
	sink->height = min(XDEMOSAIC_DEF_HEIGHT, xdmsc->max_height);
	sink->field = V4L2_FIELD_NONE;
	sink->colorspace = V4L2_COLORSPACE_SRGB;

	source = v4l2_subdev_state_get_format(sd_state, XDEMOSAIC_PAD_SOURCE);
	*source = *sink;
	source->code = XDEMOSAIC_SOURCE_CODE;

	return 0;
}

static int xdmsc_enum_mbus_code(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_mbus_code_enum *code)
{
	/* The source pad exposes the single demosaiced RGB code. */
	if (code->pad == XDEMOSAIC_PAD_SOURCE) {
		if (code->index)
			return -EINVAL;

		code->code = XDEMOSAIC_SOURCE_CODE;

		return 0;
	}

	if (code->index >= ARRAY_SIZE(xdmsc_formats))
		return -EINVAL;

	code->code = xdmsc_formats[code->index].code;

	return 0;
}

static int xdmsc_enum_frame_size(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_frame_size_enum *fse)
{
	struct xdmsc_dev *xdmsc = to_xdmsc(subdev);
	const struct v4l2_mbus_framefmt *format;

	if (fse->index)
		return -EINVAL;

	/*
	 * The frame size on the source pad is fixed and always identical to the
	 * frame size on the sink pad.
	 */
	if (fse->pad == XDEMOSAIC_PAD_SOURCE) {
		format = v4l2_subdev_state_get_format(sd_state,
						      XDEMOSAIC_PAD_SOURCE);
		if (fse->code != format->code)
			return -EINVAL;

		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;

		return 0;
	}

	if (!xdmsc_get_format_info(fse->code))
		return -EINVAL;

	fse->min_width = XDEMOSAIC_MIN_WIDTH;
	fse->max_width = xdmsc->max_width;
	fse->min_height = XDEMOSAIC_MIN_HEIGHT;
	fse->max_height = xdmsc->max_height;

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static const struct v4l2_subdev_pad_ops xdmsc_pad_ops = {
	.enum_mbus_code = xdmsc_enum_mbus_code,
	.enum_frame_size = xdmsc_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xdmsc_set_format,
	.enable_streams = xdmsc_enable_streams,
	.disable_streams = xdmsc_disable_streams,
};

static const struct v4l2_subdev_ops xdmsc_ops = {
	.pad = &xdmsc_pad_ops,
};

static const struct v4l2_subdev_internal_ops xdmsc_internal_ops = {
	.init_state = xdmsc_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xdmsc_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xdmsc_parse_of(struct xdmsc_dev *xdmsc)
{
	struct device *dev = xdmsc->dev;
	struct device_node *node = dev->of_node;
	int ret;

	ret = of_property_read_u32(node, "xlnx,max-height", &xdmsc->max_height);
	if (ret < 0) {
		dev_err(dev, "missing xlnx,max-height property\n");
		return ret;
	}

	if (xdmsc->max_height > XDEMOSAIC_MAX_HEIGHT ||
	    xdmsc->max_height < XDEMOSAIC_MIN_HEIGHT) {
		dev_err(dev, "invalid xlnx,max-height value %u\n",
			xdmsc->max_height);
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "xlnx,max-width", &xdmsc->max_width);
	if (ret < 0) {
		dev_err(dev, "missing xlnx,max-width property\n");
		return ret;
	}

	if (xdmsc->max_width > XDEMOSAIC_MAX_WIDTH ||
	    xdmsc->max_width < XDEMOSAIC_MIN_WIDTH) {
		dev_err(dev, "invalid xlnx,max-width value %u\n",
			xdmsc->max_width);
		return -EINVAL;
	}

	return 0;
}

static int xdmsc_notify_bound(struct v4l2_async_notifier *notifier,
			      struct v4l2_subdev *sd,
			      struct v4l2_async_connection *asc)
{
	struct xdmsc_dev *xdmsc =
		container_of(notifier, struct xdmsc_dev, notifier);

	return v4l2_create_fwnode_links_to_pad(sd,
					       &xdmsc->pads[XDEMOSAIC_PAD_SINK],
					       MEDIA_LNK_FL_ENABLED |
					       MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations xdmsc_notify_ops = {
	.bound = xdmsc_notify_bound,
};

static int xdmsc_register_notifier(struct xdmsc_dev *xdmsc)
{
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(xdmsc->dev),
					     XDEMOSAIC_PAD_SINK, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return 0;

	v4l2_async_subdev_nf_init(&xdmsc->notifier, &xdmsc->subdev);
	xdmsc->notifier.ops = &xdmsc_notify_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&xdmsc->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		v4l2_async_nf_cleanup(&xdmsc->notifier);
		return PTR_ERR(asc);
	}

	ret = v4l2_async_nf_register(&xdmsc->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&xdmsc->notifier);

	return ret;
}

static int xdmsc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_subdev *subdev;
	struct xdmsc_dev *xdmsc;
	struct clk *clk;
	int ret;

	xdmsc = devm_kzalloc(dev, sizeof(*xdmsc), GFP_KERNEL);
	if (!xdmsc)
		return -ENOMEM;

	xdmsc->dev = dev;

	ret = xdmsc_parse_of(xdmsc);
	if (ret < 0)
		return ret;

	/* Keep the IP in reset until the clock is running. */
	xdmsc->rst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xdmsc->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(xdmsc->rst_gpio),
				     "failed to get reset GPIO\n");

	xdmsc->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xdmsc->iomem))
		return PTR_ERR(xdmsc->iomem);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "failed to get clock\n");

	gpiod_set_value_cansleep(xdmsc->rst_gpio, XDEMOSAIC_RESET_DEASSERT);

	/* Initialize the media pads. */
	xdmsc->pads[XDEMOSAIC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	xdmsc->pads[XDEMOSAIC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize the V4L2 subdev and media entity. */
	subdev = &xdmsc->subdev;
	v4l2_subdev_init(subdev, &xdmsc_ops);
	subdev->dev = dev;
	subdev->internal_ops = &xdmsc_internal_ops;
	strscpy(subdev->name, dev_name(dev));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xdmsc_media_ops;
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV;

	ret = media_entity_pads_init(&subdev->entity, XDEMOSAIC_NUM_PADS,
				     xdmsc->pads);
	if (ret < 0)
		return ret;

	/* Allocate the subdev active state and populate the default format. */
	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_media_cleanup;

	platform_set_drvdata(pdev, xdmsc);

	ret = xdmsc_register_notifier(xdmsc);
	if (ret < 0)
		goto error_subdev_cleanup;

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto error_notifier_cleanup;
	}

	return 0;

error_notifier_cleanup:
	v4l2_async_nf_unregister(&xdmsc->notifier);
	v4l2_async_nf_cleanup(&xdmsc->notifier);
error_subdev_cleanup:
	v4l2_subdev_cleanup(subdev);
error_media_cleanup:
	media_entity_cleanup(&subdev->entity);
	return ret;
}

static void xdmsc_remove(struct platform_device *pdev)
{
	struct xdmsc_dev *xdmsc = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xdmsc->subdev;

	v4l2_async_nf_unregister(&xdmsc->notifier);
	v4l2_async_nf_cleanup(&xdmsc->notifier);
	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id xdmsc_of_id_table[] = {
	{ .compatible = "xlnx,v-demosaic" },
	{ }
};
MODULE_DEVICE_TABLE(of, xdmsc_of_id_table);

static struct platform_driver xdmsc_driver = {
	.driver = {
		.name		= "xilinx-demosaic",
		.of_match_table	= xdmsc_of_id_table,
	},
	.probe			= xdmsc_probe,
	.remove			= xdmsc_remove,
};

module_platform_driver(xdmsc_driver);

MODULE_DESCRIPTION("Xilinx Video Demosaic IP Driver");
MODULE_LICENSE("GPL");
