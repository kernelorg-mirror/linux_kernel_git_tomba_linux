// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx AXI4-Stream Video Broadcaster
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Author: Ronak Shah <ronak.shah@xilinx.com>
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#include "xilinx-vip.h"

#define MAX_VBR_SINKS			1
#define MIN_VBR_SRCS			2
#define MAX_VBR_SRCS			16

/**
 * struct xvbroadcaster_device - AXI4-Stream Broadcaster device structure
 * @dev: Platform structure
 * @subdev: The v4l2 subdev structure
 * @pads: media pads
 * @npads: number of pads
 * @enabled_pads: bitmask of the source pads that have their stream enabled
 */
struct xvbroadcaster_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	struct media_pad *pads;
	u32 npads;
	u64 enabled_pads;
};

static inline struct xvbroadcaster_device *to_xvbr(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xvbroadcaster_device, subdev);
}

/* -----------------------------------------------------------------------------
 * Streaming
 */

/*
 * The broadcaster duplicates the sink stream to all of its source pads at all
 * times, there is no register to program and no way to disable an individual
 * master port. As no Xilinx video IP can drop data, a copy that reaches a
 * consumer that hasn't been started yet backpressures the broadcaster, which
 * in turn stalls the whole pipeline, including the branches that are already
 * running. The upstream part of the pipeline must therefore only be started
 * once all the source pads that can be enabled have been enabled, and must be
 * stopped as soon as the first one is disabled.
 *
 * The source pads that can be enabled are those connected to an enabled link.
 * Links can't be enabled or disabled while the pipeline is streaming, so the
 * set is stable for the whole duration of a streaming session and can be
 * recomputed on each call.
 */

static u64 xvbr_linked_pads(struct xvbroadcaster_device *xvbr)
{
	u64 mask = 0;
	unsigned int i;

	for (i = XVIP_PAD_SINK + 1; i < xvbr->npads; ++i) {
		if (media_pad_remote_pad_first(&xvbr->pads[i]))
			mask |= BIT_ULL(i);
	}

	return mask;
}

static int xvbr_enable_streams(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct xvbroadcaster_device *xvbr = to_xvbr(subdev);
	int ret;

	xvbr->enabled_pads |= BIT_ULL(pad);

	if (xvbr->enabled_pads != xvbr_linked_pads(xvbr))
		return 0;

	ret = xvip_enable_remote_stream(subdev, XVIP_PAD_SINK, BIT_ULL(0));
	if (ret) {
		xvbr->enabled_pads &= ~BIT_ULL(pad);
		return ret;
	}

	return 0;
}

static int xvbr_disable_streams(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state, u32 pad,
				u64 streams_mask)
{
	struct xvbroadcaster_device *xvbr = to_xvbr(subdev);

	/*
	 * Stop the upstream part of the pipeline before removing one of its
	 * consumers. The set of enabled pads being complete means that it is
	 * currently running.
	 */
	if (xvbr->enabled_pads == xvbr_linked_pads(xvbr))
		xvip_disable_remote_stream(subdev, XVIP_PAD_SINK, BIT_ULL(0));

	xvbr->enabled_pads &= ~BIT_ULL(pad);

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int xvbr_init_state(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state)
{
	struct xvbroadcaster_device *xvbr = to_xvbr(subdev);
	unsigned int i;

	for (i = 0; i < xvbr->npads; ++i) {
		struct v4l2_mbus_framefmt *format;

		format = v4l2_subdev_state_get_format(sd_state, i);

		format->code = MEDIA_BUS_FMT_RGB888_1X24;
		format->field = V4L2_FIELD_NONE;
		format->colorspace = V4L2_COLORSPACE_SRGB;
		format->width = XVIP_MAX_WIDTH;
		format->height = XVIP_MAX_HEIGHT;
	}

	return 0;
}

static int xvbr_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvbroadcaster_device *xvbr = to_xvbr(subdev);
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	/*
	 * The source pads carry a copy of the sink stream, their format
	 * follows the sink pad format and can't be set.
	 */
	if (fmt->pad != XVIP_PAD_SINK) {
		fmt->format = *format;
		return 0;
	}

	*format = fmt->format;

	xvip_set_format_size(format, fmt);

	fmt->format = *format;

	for (i = XVIP_PAD_SINK + 1; i < xvbr->npads; ++i)
		*v4l2_subdev_state_get_format(sd_state, i) = *format;

	return 0;
}

static struct v4l2_subdev_pad_ops xvbr_pad_ops = {
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xvip_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xvbr_set_format,
	.enable_streams = xvbr_enable_streams,
	.disable_streams = xvbr_disable_streams,
};

static struct v4l2_subdev_ops xvbr_ops = {
	.pad = &xvbr_pad_ops,
};

static const struct v4l2_subdev_internal_ops xvbr_internal_ops = {
	.init_state = xvbr_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xvbr_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xvbr_parse_of(struct xvbroadcaster_device *xvbr)
{
	struct device_node *node = xvbr->dev->of_node;
	struct device_node *ports;
	struct device_node *port;

	ports = of_get_child_by_name(node, "ports");
	if (!ports)
		ports = node;

	for_each_child_of_node(ports, port) {
		struct device_node *endpoint;

		if (!port->name || of_node_cmp(port->name, "port"))
			continue;

		endpoint = of_get_next_child(port, NULL);
		if (!endpoint) {
			dev_err(xvbr->dev, "No port at\n");
			return -EINVAL;
		}

		/* Count the number of ports. */
		xvbr->npads++;
	}

	/* validate number of ports */
	if ((xvbr->npads > (MAX_VBR_SINKS + MAX_VBR_SRCS)) ||
	    (xvbr->npads < (MAX_VBR_SINKS + MIN_VBR_SRCS))) {
		dev_err(xvbr->dev, "invalid number of ports %u\n", xvbr->npads);
		return -EINVAL;
	}

	return 0;
}

static int xvbr_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xvbroadcaster_device *xvbr;
	unsigned int i;
	int ret;

	xvbr = devm_kzalloc(&pdev->dev, sizeof(*xvbr), GFP_KERNEL);
	if (!xvbr)
		return -ENOMEM;

	xvbr->dev = &pdev->dev;

	ret = xvbr_parse_of(xvbr);
	if (ret < 0)
		return ret;

	/*
	 * Initialize V4L2 subdevice and media entity
	 */
	xvbr->pads = devm_kzalloc(&pdev->dev, xvbr->npads * sizeof(*xvbr->pads),
				  GFP_KERNEL);
	if (!xvbr->pads)
		return -ENOMEM;

	xvbr->pads[0].flags = MEDIA_PAD_FL_SINK;

	for (i = 1; i < xvbr->npads; ++i)
		xvbr->pads[i].flags = MEDIA_PAD_FL_SOURCE;

	subdev = &xvbr->subdev;
	v4l2_subdev_init(subdev, &xvbr_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xvbr_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xvbr);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xvbr_media_ops;

	ret = media_entity_pads_init(&subdev->entity, xvbr->npads, xvbr->pads);
	if (ret < 0)
		goto error;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, xvbr);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error_subdev;
	}

	dev_info(xvbr->dev, "Xilinx AXI4-Stream Broadcaster found!\n");

	return 0;

error_subdev:
	v4l2_subdev_cleanup(subdev);
error:
	media_entity_cleanup(&subdev->entity);

	return ret;
}

static void xvbr_remove(struct platform_device *pdev)
{
	struct xvbroadcaster_device *xvbr = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xvbr->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id xvbr_of_id_table[] = {
	{ .compatible = "xlnx,axis-broadcaster-1.1" },
	{ }
};
MODULE_DEVICE_TABLE(of, xvbr_of_id_table);

static struct platform_driver xvbr_driver = {
	.driver = {
		.name		= "xilinx-axis-broadcaster",
		.of_match_table	= xvbr_of_id_table,
	},
	.probe			= xvbr_probe,
	.remove		= xvbr_remove,
};

module_platform_driver(xvbr_driver);

MODULE_AUTHOR("Ronak Shah <ronak.shah@xilinx.com>");
MODULE_DESCRIPTION("Xilinx AXI4-Stream Broadcaster Driver");
MODULE_LICENSE("GPL");
