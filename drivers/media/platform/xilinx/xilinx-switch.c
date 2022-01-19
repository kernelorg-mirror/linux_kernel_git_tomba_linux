/*
 * Xilinx Video Switch
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#include "xilinx-vip.h"

#define XSW_CORE_CH_CTRL			0x0100
#define XSW_CORE_CH_CTRL_FORCE			(1 << 3)

#define XSW_SWITCH_STATUS			0x0104

/**
 * struct xswitch_device - Xilinx Video Switch device structure
 * @xvip: Xilinx Video IP device
 */
struct xswitch_device {
	struct xvip_device xvip;
};

static inline struct xswitch_device *to_xsw(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xswitch_device, xvip.subdev);
}

/* -----------------------------------------------------------------------------
 * xvip operations
 */

static int xsw_enable_streams(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state, u32 pad,
			      u64 streams_mask)
{
	struct xswitch_device *xsw = to_xsw(sd);
	struct v4l2_subdev_route *route;
	unsigned long unused_outputs;
	u32 unused_inputs;
	unsigned int unused_input;
	u32 routing;
	unsigned int i;

	/*
	 * The hardware routing table stores in a register the input number at
	 * the output's position. All outputs must be connected, so unused
	 * outputs must be configured with an unused input. When the switch is
	 * synthesized with less than 8 inputs, the index of non-existing inputs
	 * may be used to configure unused outputs.
	 *
	 * Start with a first pass, iterating over all routes, to configure
	 * used outputs and to record the unused inputs and ouputs.
	 */
	unused_inputs = 0xff;
	unused_outputs = (1 << xsw->xvip.num_sources) - 1;
	routing = 0;

	for_each_active_route(&state->routing, route) {
		routing |= (XSW_CORE_CH_CTRL_FORCE | route->sink_pad)
			<< (route->source_pad * 4);

		unused_inputs &= ~BIT(route->sink_pad);
		unused_outputs &= ~BIT(route->source_pad);
	}

	/*
	 * Iterate over unused outputs and configure them with an unused input.
	 * If not unused input was found (implemented or non-implemented), it
	 * means that the switch is synthesized with 8 inputs and all of them
	 * are connected to different outputs. The unused_input value doesn't
	 * matter in that case, as there is no unused output.
	 */
	unused_input = unused_inputs ? ffs(unused_inputs) - 1 : 0;

	for_each_set_bit(i, &unused_outputs, 8)
		routing |= (XSW_CORE_CH_CTRL_FORCE | unused_input) << (i * 4);

	xvip_write(&xsw->xvip, XSW_CORE_CH_CTRL, routing);

	xvip_write(&xsw->xvip, XVIP_CTRL_CONTROL,
		   (((1 << xsw->xvip.num_sources) - 1) << 4) |
		   XVIP_CTRL_CONTROL_SW_ENABLE);

	return 0;
}

static int xsw_disable_streams(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct xswitch_device *xsw = to_xsw(sd);

	xvip_stop(&xsw->xvip);

	return 0;
}

static const struct xvip_device_ops xsw_xvip_device_ops = {
	.enable_streams = xsw_enable_streams,
	.disable_streams = xsw_disable_streams,
};

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static const struct v4l2_mbus_framefmt xsw_default_format = {
	.code = MEDIA_BUS_FMT_RGB888_1X24,
	.width = 1920,
	.height = 1080,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SRGB,
};

static int __xsw_set_routing(struct v4l2_subdev *subdev,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_krouting *routing)
{
	int ret;

	ret = v4l2_subdev_routing_validate(subdev, routing,
					   V4L2_SUBDEV_ROUTING_NO_N_TO_1 |
					   V4L2_SUBDEV_ROUTING_NO_STREAM_MIX);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(subdev, state, routing,
						&xsw_default_format);
}

static int xsw_init_cfg(struct v4l2_subdev *subdev,
			struct v4l2_subdev_state *state)
{
	struct xswitch_device *xsw = to_xsw(subdev);
	struct v4l2_subdev_krouting routing = { };
	struct v4l2_subdev_route *routes;
	unsigned int num_routes;
	unsigned int i;
	int ret;

	num_routes = min(xsw->xvip.num_sinks, xsw->xvip.num_sources);
	routes = kcalloc(num_routes, sizeof(*routes), GFP_KERNEL);
	if (!routes)
		return -ENOMEM;

	/*
	 * Set a 1:1 mapping between sinks and sources by default. If there
	 * are more sources than sinks, the last sources are not connected.
	 */
	for (i = 0; i < num_routes; ++i) {
		struct v4l2_subdev_route *route = &routes[i];

		route->sink_pad = i;
		route->source_pad = i + xsw->xvip.num_sinks;
		route->flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	};

	routing.num_routes = num_routes;
	routing.routes = routes;

	ret = __xsw_set_routing(subdev, state, &routing);

	kfree(routes);

	return ret;
}

static int xsw_set_format(struct v4l2_subdev *subdev,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct xswitch_device *xsw = to_xsw(subdev);
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_mbus_framefmt *source_fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&subdev->entity))
		return -EBUSY;

	/*
	 * The source pad format is always identical to the sink pad format and
	 * can't be modified.
	 */
	if (format->pad >= xsw->xvip.num_sinks)
		return v4l2_subdev_get_fmt(subdev, state, format);

	/* Validate the requested format. */
	format->format.width = clamp_t(unsigned int, format->format.width,
				       XVIP_MIN_WIDTH, XVIP_MAX_WIDTH);
	format->format.height = clamp_t(unsigned int, format->format.height,
					XVIP_MIN_HEIGHT, XVIP_MAX_HEIGHT);
	format->format.field = V4L2_FIELD_NONE;

	/*
	 * Set the format on the sink stream and propagate it to the source
	 * stream.
	 */
	sink_fmt = v4l2_subdev_state_get_stream_format(state, format->pad,
						       format->stream);
	source_fmt = v4l2_subdev_state_get_opposite_stream_format(state,
								  format->pad,
								  format->stream);
	if (!sink_fmt || !source_fmt)
		return -EINVAL;

	*sink_fmt = format->format;
	*source_fmt = format->format;

	return 0;
}

static int xsw_set_routing(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state,
			   enum v4l2_subdev_format_whence which,
			   struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&subdev->entity))
		return -EBUSY;

	return __xsw_set_routing(subdev, state, routing);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static const struct v4l2_subdev_video_ops xsw_video_ops = {
	.s_stream = xvip_s_stream,
};

static const struct v4l2_subdev_pad_ops xsw_pad_ops = {
	.init_cfg = xsw_init_cfg,
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xvip_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xsw_set_format,
	.set_routing = xsw_set_routing,
	.enable_streams = xvip_enable_streams,
	.disable_streams = xvip_disable_streams,
};

static const struct v4l2_subdev_ops xsw_ops = {
	.video = &xsw_video_ops,
	.pad = &xsw_pad_ops,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xsw_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xsw_parse_of(struct xswitch_device *xsw,
			struct xvip_device_info *info)
{
	struct device_node *node = xsw->xvip.dev->of_node;
	int ret;

	ret = of_property_read_u32(node, "#xlnx,inputs", &info->num_sinks);
	if (ret < 0) {
		dev_err(xsw->xvip.dev, "missing or invalid #xlnx,%s property\n",
			"inputs");
		return ret;
	}

	ret = of_property_read_u32(node, "#xlnx,outputs", &info->num_sources);
	if (ret < 0) {
		dev_err(xsw->xvip.dev, "missing or invalid #xlnx,%s property\n",
			"outputs");
		return ret;
	}

	return 0;
}

static int xsw_probe(struct platform_device *pdev)
{
	struct xvip_device_info xsw_info = {
		.has_axi_lite = true,
	};
	struct v4l2_subdev *subdev;
	struct xswitch_device *xsw;
	unsigned int npads;
	int ret;

	xsw = devm_kzalloc(&pdev->dev, sizeof(*xsw), GFP_KERNEL);
	if (!xsw)
		return -ENOMEM;

	xsw->xvip.dev = &pdev->dev;
	xsw->xvip.ops = &xsw_xvip_device_ops;

	ret = xsw_parse_of(xsw, &xsw_info);
	if (ret < 0)
		return ret;

	ret = xvip_device_init(&xsw->xvip, &xsw_info);
	if (ret < 0)
		return ret;

	/* Initialize V4L2 subdevice and media entity. Pad numbers depend on the
	 * number of pads.
	 */
	npads = xsw->xvip.num_sinks + xsw->xvip.num_sources;

	subdev = &xsw->xvip.subdev;
	v4l2_subdev_init(subdev, &xsw_ops);
	subdev->dev = &pdev->dev;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xsw);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	subdev->entity.ops = &xsw_media_ops;

	ret = media_entity_pads_init(&subdev->entity, npads, xsw->xvip.pads);
	if (ret < 0)
		goto error;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret)
		goto error;

	platform_set_drvdata(pdev, xsw);

	xvip_print_version(&xsw->xvip);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	xvip_device_cleanup(&xsw->xvip);
	return ret;
}

static int xsw_remove(struct platform_device *pdev)
{
	struct xswitch_device *xsw = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xsw->xvip.subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);

	xvip_device_cleanup(&xsw->xvip);

	return 0;
}

static const struct of_device_id xsw_of_id_table[] = {
	{ .compatible = "xlnx,v-switch-1.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, xsw_of_id_table);

static struct platform_driver xsw_driver = {
	.driver = {
		.name		= "xilinx-switch",
		.of_match_table	= xsw_of_id_table,
	},
	.probe			= xsw_probe,
	.remove			= xsw_remove,
};

module_platform_driver(xsw_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Xilinx Video Switch Driver");
MODULE_LICENSE("GPL v2");
