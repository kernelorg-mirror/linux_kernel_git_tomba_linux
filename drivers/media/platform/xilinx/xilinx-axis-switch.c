// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx AXI4-Stream Video Switch
 *
 * Copyright (C) 2018 Xilinx, Inc.
 *
 * Author: Vishal Sagar <vishal.sagar@xilinx.com>
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

/*
 * The switch has a single slave (sink) interface and up to 16 master (source)
 * interfaces. Only the TDEST routing mode is supported: the hardware routes
 * each packet to the master interface selected by the packet's TDEST value
 * (the CSI-2 virtual channel in a MIPI capture pipeline), without any software
 * control. The switch has no registers to program in that mode, the driver
 * only models the routing with the V4L2 streams API so that userspace can
 * describe which sink streams reach which source pad.
 */

/* The sink pad is pad 0, the source pads follow it. */
#define XVSW_PAD_SINK			0
#define XVSW_PAD_SOURCE(n)		((n) + 1)

#define XVSW_NUM_SINKS			1
#define XVSW_MAX_NUM_SOURCES		16

/* Value of the xlnx,routing-mode property selecting TDEST-based routing. */
#define XVSW_ROUTING_MODE_TDEST		0

#define XVSW_DEFAULT_WIDTH		1920
#define XVSW_DEFAULT_HEIGHT		1080

/**
 * struct xvswitch_device - Xilinx AXI4-Stream Switch device structure
 * @dev: (OF) device
 * @subdev: V4L2 subdevice
 * @notifier: V4L2 async notifier for the upstream subdev
 * @pads: media pads, the sink pad followed by @nsources source pads
 * @nsources: number of source pads
 */
struct xvswitch_device {
	struct device *dev;
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct media_pad *pads;
	u32 nsources;
};

static inline struct xvswitch_device *to_xvsw(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xvswitch_device, subdev);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static const struct v4l2_mbus_framefmt xvsw_default_format = {
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.width = XVSW_DEFAULT_WIDTH,
	.height = XVSW_DEFAULT_HEIGHT,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SRGB,
};

static int __xvsw_set_routing(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_krouting *routing)
{
	int ret;

	/*
	 * The hardware routes each TDEST to exactly one master port, so a
	 * sink stream cannot be duplicated or merged. Multiple sink streams
	 * may however be routed to the same source pad (as separate source
	 * streams), as one virtual channel can carry several data types.
	 */
	ret = v4l2_subdev_routing_validate(subdev, routing,
					   V4L2_SUBDEV_ROUTING_ONLY_1_TO_1);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(subdev, state, routing,
						&xvsw_default_format);
}

static int xvsw_set_routing(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_state *state,
			    enum v4l2_subdev_format_whence which,
			    struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&subdev->entity))
		return -EBUSY;

	return __xvsw_set_routing(subdev, state, routing);
}

static int xvsw_init_state(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.sink_pad = XVSW_PAD_SINK,
			.sink_stream = 0,
			.source_pad = XVSW_PAD_SOURCE(0),
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};
	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};

	/*
	 * Default to a single route from the sink pad to the first source
	 * pad, matching a single-stream pipeline. This keeps the switch
	 * usable without routing setup, also with stream-unaware upstream
	 * and downstream entities.
	 */
	return __xvsw_set_routing(subdev, state, &routing);
}

static int xvsw_enum_mbus_code(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	const struct v4l2_mbus_framefmt *format;

	/*
	 * The switch is format-agnostic and has no format list of its own, so
	 * there's nothing to enumerate on the sink pad. The media bus code on
	 * the source pads is identical to the code of the routed sink stream,
	 * enumerate that single code.
	 */
	if (code->pad == XVSW_PAD_SINK)
		return -EINVAL;

	if (code->index > 0)
		return -EINVAL;

	format = v4l2_subdev_state_get_opposite_stream_format(state, code->pad,
							      code->stream);
	if (!format)
		return -EINVAL;

	code->code = format->code;

	return 0;
}

static int xvsw_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_mbus_framefmt *source_fmt;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_pad_is_streaming(&xvsw->pads[fmt->pad]))
		return -EBUSY;

	/*
	 * The source pad formats are always identical to the routed sink
	 * stream format and can't be modified.
	 */
	if (fmt->pad != XVSW_PAD_SINK)
		return v4l2_subdev_get_fmt(subdev, state, fmt);

	/*
	 * The switch is a pure stream interconnect with no notion of frame
	 * geometry, accept any media bus code and frame size.
	 */
	fmt->format.field = V4L2_FIELD_NONE;

	sink_fmt = v4l2_subdev_state_get_format(state, fmt->pad, fmt->stream);
	if (!sink_fmt)
		return -EINVAL;

	*sink_fmt = fmt->format;

	/* Propagate the format to the routed source stream. */
	source_fmt = v4l2_subdev_state_get_opposite_stream_format(state,
								  fmt->pad,
								  fmt->stream);
	if (!source_fmt)
		return -EINVAL;

	*source_fmt = fmt->format;

	return 0;
}

static int xvsw_set_remote_streams(struct xvswitch_device *xvsw,
				   struct v4l2_subdev_state *state,
				   u32 pad, u64 streams_mask, bool enable)
{
	struct media_pad *remote;
	struct v4l2_subdev *subdev;
	u64 sink_streams;
	u64 streams = streams_mask;

	sink_streams = v4l2_subdev_state_xlate_streams(state, pad,
						       XVSW_PAD_SINK,
						       &streams);

	remote = media_pad_remote_pad_first(&xvsw->pads[XVSW_PAD_SINK]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return -EPIPE;

	subdev = media_entity_to_v4l2_subdev(remote->entity);

	if (enable)
		return v4l2_subdev_enable_streams(subdev, remote->index,
						  sink_streams);

	return v4l2_subdev_disable_streams(subdev, remote->index,
					   sink_streams);
}

static int xvsw_enable_streams(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state,
			       u32 pad, u64 streams_mask)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);

	/*
	 * The hardware routes by TDEST without software control, so there is
	 * nothing to configure. Propagate the enabled streams, translated to
	 * the sink side, to the upstream subdevice.
	 */
	return xvsw_set_remote_streams(xvsw, state, pad, streams_mask, true);
}

static int xvsw_disable_streams(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state,
				u32 pad, u64 streams_mask)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);

	return xvsw_set_remote_streams(xvsw, state, pad, streams_mask, false);
}

static const struct v4l2_subdev_pad_ops xvsw_pad_ops = {
	.enum_mbus_code = xvsw_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xvsw_set_format,
	.set_routing = xvsw_set_routing,
	.enable_streams = xvsw_enable_streams,
	.disable_streams = xvsw_disable_streams,
};

static const struct v4l2_subdev_ops xvsw_ops = {
	.pad = &xvsw_pad_ops,
};

static const struct v4l2_subdev_internal_ops xvsw_internal_ops = {
	.init_state = xvsw_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xvsw_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xvsw_parse_of(struct xvswitch_device *xvsw)
{
	struct device *dev = xvsw->dev;
	struct device_node *node = dev->of_node;
	unsigned int nports;
	u32 routing_mode;
	u32 nsinks;
	int ret;

	ret = of_property_read_u32(node, "xlnx,routing-mode", &routing_mode);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read xlnx,routing-mode\n");

	if (routing_mode != XVSW_ROUTING_MODE_TDEST)
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "only TDEST routing mode is supported\n");

	ret = of_property_read_u32(node, "xlnx,num-si-slots", &nsinks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read xlnx,num-si-slots\n");

	if (nsinks != XVSW_NUM_SINKS)
		return dev_err_probe(dev, -EINVAL,
				     "invalid number of slave interfaces %u\n",
				     nsinks);

	ret = of_property_read_u32(node, "xlnx,num-mi-slots", &xvsw->nsources);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read xlnx,num-mi-slots\n");

	if (xvsw->nsources < 1 || xvsw->nsources > XVSW_MAX_NUM_SOURCES)
		return dev_err_probe(dev, -EINVAL,
				     "invalid number of master interfaces %u\n",
				     xvsw->nsources);

	nports = of_graph_get_port_count(node);
	if (nports != XVSW_NUM_SINKS + xvsw->nsources)
		return dev_err_probe(dev, -EINVAL,
				     "invalid number of ports %u\n", nports);

	return 0;
}

static int xvsw_notify_bound(struct v4l2_async_notifier *notifier,
			     struct v4l2_subdev *sd,
			     struct v4l2_async_connection *asc)
{
	struct xvswitch_device *xvsw =
		container_of(notifier, struct xvswitch_device, notifier);

	return v4l2_create_fwnode_links_to_pad(sd,
					       &xvsw->pads[XVSW_PAD_SINK],
					       MEDIA_LNK_FL_ENABLED |
					       MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations xvsw_notify_ops = {
	.bound = xvsw_notify_bound,
};

static int xvsw_register_notifier(struct xvswitch_device *xvsw)
{
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	/*
	 * The entity connected to the slave interface is mandatory. Bind to it
	 * through an async notifier and create the link to the sink pad when it
	 * probes.
	 */
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(xvsw->dev),
					     XVSW_PAD_SINK, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return dev_err_probe(xvsw->dev, -ENODEV,
				     "no endpoint found on the sink port\n");

	v4l2_async_subdev_nf_init(&xvsw->notifier, &xvsw->subdev);
	xvsw->notifier.ops = &xvsw_notify_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&xvsw->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		v4l2_async_nf_cleanup(&xvsw->notifier);
		return PTR_ERR(asc);
	}

	ret = v4l2_async_nf_register(&xvsw->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&xvsw->notifier);

	return ret;
}

static int xvsw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xvswitch_device *xvsw;
	struct v4l2_subdev *subdev;
	struct clk *aclk;
	unsigned int npads;
	unsigned int i;
	int ret;

	xvsw = devm_kzalloc(dev, sizeof(*xvsw), GFP_KERNEL);
	if (!xvsw)
		return -ENOMEM;

	xvsw->dev = dev;

	ret = xvsw_parse_of(xvsw);
	if (ret)
		return ret;

	aclk = devm_clk_get_enabled(dev, "aclk");
	if (IS_ERR(aclk))
		return dev_err_probe(dev, PTR_ERR(aclk),
				     "failed to get aclk\n");

	npads = XVSW_NUM_SINKS + xvsw->nsources;
	xvsw->pads = devm_kcalloc(dev, npads, sizeof(*xvsw->pads), GFP_KERNEL);
	if (!xvsw->pads)
		return -ENOMEM;

	xvsw->pads[XVSW_PAD_SINK].flags = MEDIA_PAD_FL_SINK |
					  MEDIA_PAD_FL_MUST_CONNECT;
	for (i = 0; i < xvsw->nsources; ++i)
		xvsw->pads[XVSW_PAD_SOURCE(i)].flags = MEDIA_PAD_FL_SOURCE;

	subdev = &xvsw->subdev;
	v4l2_subdev_init(subdev, &xvsw_ops);
	subdev->dev = dev;
	subdev->internal_ops = &xvsw_internal_ops;
	strscpy(subdev->name, dev_name(dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xvsw);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	subdev->entity.function = MEDIA_ENT_F_VID_MUX;
	subdev->entity.ops = &xvsw_media_ops;

	ret = media_entity_pads_init(&subdev->entity, npads, xvsw->pads);
	if (ret)
		return ret;

	/* Allocate the subdev active state and set the default routing. */
	ret = v4l2_subdev_init_finalize(subdev);
	if (ret)
		goto error_entity;

	platform_set_drvdata(pdev, xvsw);

	ret = xvsw_register_notifier(xvsw);
	if (ret)
		goto error_subdev;

	ret = v4l2_async_register_subdev(subdev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register subdev\n");
		goto error_notifier;
	}

	return 0;

error_notifier:
	v4l2_async_nf_unregister(&xvsw->notifier);
	v4l2_async_nf_cleanup(&xvsw->notifier);
error_subdev:
	v4l2_subdev_cleanup(subdev);
error_entity:
	media_entity_cleanup(&subdev->entity);
	return ret;
}

static void xvsw_remove(struct platform_device *pdev)
{
	struct xvswitch_device *xvsw = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xvsw->subdev;

	v4l2_async_nf_unregister(&xvsw->notifier);
	v4l2_async_nf_cleanup(&xvsw->notifier);
	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id xvsw_of_id_table[] = {
	{ .compatible = "xlnx,axis-switch-1.1" },
	{ }
};
MODULE_DEVICE_TABLE(of, xvsw_of_id_table);

static struct platform_driver xvsw_driver = {
	.driver = {
		.name		= "xilinx-axis-switch",
		.of_match_table	= xvsw_of_id_table,
	},
	.probe			= xvsw_probe,
	.remove			= xvsw_remove,
};

module_platform_driver(xvsw_driver);

MODULE_AUTHOR("Vishal Sagar <vishal.sagar@xilinx.com>");
MODULE_DESCRIPTION("Xilinx AXI4-Stream Switch Driver");
MODULE_LICENSE("GPL");
