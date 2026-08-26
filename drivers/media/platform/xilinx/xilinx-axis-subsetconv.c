// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Axis Subset Converter Driver
 *
 * Copyright (C) 2022 Xilinx, Inc.
 *
 * Authors: Anil Kumar M <anil.mamidal@xilinx.com>
 *          Karthikeyan T <karthikeyan.thangavel@xilinx.com>
 *
 * This converter driver is for matching the format of source pad
 * and sink pad in the media pipeline. The format of a source does
 * not match the sink pad if it is converted by a non-memory mapped
 * hardware IP. This subset converter driver is for non-memory mapped
 * axi stream subset converter which converts the format of the stream.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/v4l2-subdev.h>
#include <media/media-entity.h>
#include <media/v4l2-subdev.h>
#include "xilinx-vip.h"

/* Number of media pads */
#define XSUBSETCONV_MEDIA_PADS		(2)

#define XSUBSETCONV_DEFAULT_WIDTH	(1920)
#define XSUBSETCONV_DEFAULT_HEIGHT	(1080)

/**
 * struct xsubsetconv_state - SW format converter device structure
 * @dev: Core structure for SW format converter
 * @subdev: The v4l2 subdev structure
 * @pads: media pads
 *
 * This structure contains the device driver related parameters
 */
struct xsubsetconv_state {
	struct device *dev;
	struct v4l2_subdev subdev;
	struct media_pad pads[XSUBSETCONV_MEDIA_PADS];
};

static const struct of_device_id xsubsetconv_of_id_table[] = {
	{ .compatible = "xlnx,axis-subsetconv-1.1"},
	{ }
};
MODULE_DEVICE_TABLE(of, xsubsetconv_of_id_table);

/**
 * xsubsetconv_init_state - Initialise the default pad formats
 * @sd: pointer to v4l2 sub device structure
 * @state: pointer to the sub device state to initialise
 *
 * Return: 0 on success
 */
static int xsubsetconv_init_state(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state)
{
	unsigned int pad;

	for (pad = 0; pad < XSUBSETCONV_MEDIA_PADS; pad++) {
		struct v4l2_mbus_framefmt *format;

		format = v4l2_subdev_state_get_format(state, pad);

		format->code = MEDIA_BUS_FMT_RGB888_1X24;
		format->field = V4L2_FIELD_NONE;
		format->colorspace = V4L2_COLORSPACE_SRGB;
		format->width = XSUBSETCONV_DEFAULT_WIDTH;
		format->height = XSUBSETCONV_DEFAULT_HEIGHT;
	}

	return 0;
}

/**
 * xsubsetconv_set_format - This is used to set the pad format
 * @sd: Pointer to V4L2 Sub device structure
 * @state: Pointer to sub device pad information structure
 * @fmt: Pointer to pad level media bus format
 *
 * This function is used to set the pad format.
 * Since the pad format is converted in hardware which is not
 * memory based IP, this driver will convert the source pad format
 * to the hardware outputting sink pad format. It actually cannot
 * convert any format.
 *
 * Return: 0 on success
 */
static int xsubsetconv_set_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *format;
	unsigned int src_code;

	format = v4l2_subdev_state_get_format(state, fmt->pad);

	/* Restore the original pad format code */
	if (fmt->pad == XVIP_PAD_SOURCE) {
		struct v4l2_mbus_framefmt *sink_fmt;

		sink_fmt = v4l2_subdev_state_get_format(state, XVIP_PAD_SINK);
		/*
		 * TODO: Need to add a check to compare sink format and possible
		 *		 src format supported by subset converter
		 */
		*format = *sink_fmt;
		format->code = fmt->format.code;

	} else {
		struct v4l2_mbus_framefmt *src_fmt;

		src_fmt = v4l2_subdev_state_get_format(state, XVIP_PAD_SOURCE);

		*format = fmt->format;
		src_code = src_fmt->code;
		*src_fmt = *format;
		src_fmt->code = src_code;
	}

	return 0;
}

static int xsubsetconv_enable_streams(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state, u32 pad,
				      u64 streams_mask)
{
	/* Nothing to program, only propagate the stream state upstream. */
	return xvip_enable_remote_stream(sd, XVIP_PAD_SINK, BIT_ULL(0));
}

static int xsubsetconv_disable_streams(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state, u32 pad,
				       u64 streams_mask)
{
	struct xsubsetconv_state *xsubsetconv = v4l2_get_subdevdata(sd);
	int ret;

	/*
	 * Stopping is best effort: a failure would leave the streams marked as
	 * enabled in the core while the source has stopped.
	 */
	ret = xvip_disable_remote_stream(sd, XVIP_PAD_SINK, BIT_ULL(0));
	if (ret)
		dev_err(xsubsetconv->dev, "failed to stop the source of pad %u: %d\n",
			pad, ret);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xsubsetconv_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static struct v4l2_subdev_pad_ops xsubsetconv_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xsubsetconv_set_format,
	.enable_streams = xsubsetconv_enable_streams,
	.disable_streams = xsubsetconv_disable_streams,
};

static struct v4l2_subdev_ops xsubsetconv_ops = {
	.pad = &xsubsetconv_pad_ops
};

static const struct v4l2_subdev_internal_ops xsubsetconv_internal_ops = {
	.init_state = xsubsetconv_init_state,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xsubsetconv_parse_of(struct xsubsetconv_state *xsubsetconv)
{
	struct device_node *node = xsubsetconv->dev->of_node;
	struct device_node *ports = NULL;
	struct device_node *port = NULL;
	unsigned int nports = 0;

	ports = of_get_child_by_name(node, "ports");
	if (!ports)
		ports = node;

	for_each_child_of_node(ports, port) {
		struct device_node *endpoint;

		if (!port->name || of_node_cmp(port->name, "port"))
			continue;

		endpoint = of_get_next_child(port, NULL);
		if (!endpoint) {
			dev_err(xsubsetconv->dev, "No port at\n");
			return -EINVAL;
		}

		dev_dbg(xsubsetconv->dev, "%s : port %d\n", __func__, nports);

		/* Count the number of ports. */
		nports++;
	}

	if (nports != XSUBSETCONV_MEDIA_PADS) {
		dev_err(xsubsetconv->dev, "invalid number of ports %u\n", nports);
		return -EINVAL;
	}

	return 0;
}

static int xsubsetconv_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xsubsetconv_state *xsubsetconv;
	int ret;

	xsubsetconv = devm_kzalloc(&pdev->dev, sizeof(*xsubsetconv), GFP_KERNEL);
	if (!xsubsetconv)
		return -ENOMEM;

	xsubsetconv->dev = &pdev->dev;

	ret = xsubsetconv_parse_of(xsubsetconv);
	if (ret < 0) {
		dev_err(&pdev->dev, "xsubsetconv_parse_of ret = %d\n", ret);
		return ret;
	}

	/* Initialize V4L2 subdevice and media entity */
	xsubsetconv->pads[XVIP_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	xsubsetconv->pads[XVIP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xsubsetconv->subdev;

	v4l2_subdev_init(subdev, &xsubsetconv_ops);

	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xsubsetconv_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));

	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	subdev->entity.ops = &xsubsetconv_media_ops;

	v4l2_set_subdevdata(subdev, xsubsetconv);

	ret = media_entity_pads_init(&subdev->entity, XSUBSETCONV_MEDIA_PADS,
				     xsubsetconv->pads);
	if (ret < 0) {
		dev_err(&pdev->dev, "media pad init failed = %d\n", ret);
		return ret;
	}

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_media;

	platform_set_drvdata(pdev, xsubsetconv);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error_subdev;
	}

	dev_info(&pdev->dev, "Xilinx AXI4-Stream Subset Converter found!\n");

	return 0;

error_subdev:
	v4l2_subdev_cleanup(subdev);
error_media:
	media_entity_cleanup(&subdev->entity);

	return ret;
}

static void xsubsetconv_remove(struct platform_device *pdev)
{
	struct xsubsetconv_state *xsubsetconv = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xsubsetconv->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
}

static struct platform_driver xsubsetconv_driver = {
	.driver = {
		.name		= "xlnx,axis-subsetconv-1.1",
		.of_match_table	= xsubsetconv_of_id_table,
	},
	.probe			= xsubsetconv_probe,
	.remove			= xsubsetconv_remove,
};

module_platform_driver(xsubsetconv_driver);

MODULE_AUTHOR("Anil Kumar M <anil.mamidal@xilinx.com>");
MODULE_AUTHOR("Karthikeyan T <karthikeyan.thangavel@xilinx.com>");
MODULE_DESCRIPTION("Xilinx AXI4-Stream Subset Converter Driver");
MODULE_LICENSE("GPL");
