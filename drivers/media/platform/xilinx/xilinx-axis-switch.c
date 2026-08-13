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
#include <linux/platform_device.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#include "xilinx-vip.h"

#define XVSW_CTRL_REG			0x00
#define XVSW_CTRL_REG_UPDATE_MASK	BIT(1)

#define XVSW_MI_MUX_REG_BASE		0x40
#define XVSW_MI_MUX_VAL_MASK		0xF
#define XVSW_MI_MUX_DISABLE_MASK	BIT(31)

#define MIN_VSW_SINKS			1
#define MAX_VSW_SINKS			16
#define MIN_VSW_SRCS			1
#define MAX_VSW_SRCS			16

/**
 * struct xvswitch_device - Xilinx AXI4-Stream Switch device structure
 * @dev: Platform structure
 * @iomem: Base address of IP
 * @subdev: The v4l2 subdev structure
 * @pads: media pads
 * @enabled_streams: mask of the enabled streams of each source pad
 * @nsinks: number of sink pads (1 to 8)
 * @nsources: number of source pads (2 to 8)
 * @tdest_routing: Whether TDEST routing is enabled
 * @aclk: Video clock
 * @saxi_ctlclk: AXI-Lite control clock
 */
struct xvswitch_device {
	struct device *dev;
	void __iomem *iomem;
	struct v4l2_subdev subdev;
	struct media_pad *pads;
	u64 enabled_streams[MAX_VSW_SRCS];
	u32 nsinks;
	u32 nsources;
	bool tdest_routing;
	struct clk *aclk;
	struct clk *saxi_ctlclk;
};

static inline struct xvswitch_device *to_xvsw(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xvswitch_device, subdev);
}

static inline u32 xvswitch_read(struct xvswitch_device *xvsw, u32 addr)
{
	return ioread32(xvsw->iomem + addr);
}

static inline void xvswitch_write(struct xvswitch_device *xvsw, u32 addr,
				  u32 value)
{
	iowrite32(value, xvsw->iomem + addr);
}

/* -----------------------------------------------------------------------------
 * Streaming
 */

/*
 * Get the sink pad that feeds the given source pad. All the streams of a source
 * pad come from the same sink pad: in TDEST routing mode because there is a
 * single sink pad, and in control register routing mode because the routing is
 * validated with V4L2_SUBDEV_ROUTING_NO_STREAM_MIX.
 */
static int xvsw_get_sink_pad(struct v4l2_subdev_state *state, u32 pad,
			     u32 *sink_pad)
{
	struct v4l2_subdev_route *route;

	for_each_active_route(&state->routing, route) {
		if (route->source_pad == pad) {
			*sink_pad = route->sink_pad;
			return 0;
		}
	}

	return -EPIPE;
}

static void xvsw_set_pad_stream(struct xvswitch_device *xvsw, u32 pad,
				u32 sink_pad, bool enable)
{
	/* Nothing to be done in case of TDEST routing */
	if (xvsw->tdest_routing)
		return;

	/*
	 * In control register routing mode, point the master port at the slave
	 * port it is routed from, or disable it.
	 */
	xvswitch_write(xvsw, XVSW_MI_MUX_REG_BASE + (pad - xvsw->nsinks) * 4,
		       enable ? sink_pad : XVSW_MI_MUX_DISABLE_MASK);
	xvswitch_write(xvsw, XVSW_CTRL_REG, XVSW_CTRL_REG_UPDATE_MASK);
}

static int xvsw_enable_streams(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	u64 *enabled = &xvsw->enabled_streams[pad - xvsw->nsinks];
	u64 sink_streams;
	u64 streams = streams_mask;
	u32 sink_pad;
	int ret;

	ret = xvsw_get_sink_pad(state, pad, &sink_pad);
	if (ret)
		return ret;

	sink_streams = v4l2_subdev_state_xlate_streams(state, pad, sink_pad,
						       &streams);

	/*
	 * The master port carries all the streams of the pad, it only needs to
	 * be programmed when the first one starts.
	 */
	if (!*enabled)
		xvsw_set_pad_stream(xvsw, pad, sink_pad, true);

	ret = xvip_enable_remote_stream(subdev, sink_pad, sink_streams);
	if (ret) {
		if (!*enabled)
			xvsw_set_pad_stream(xvsw, pad, sink_pad, false);
		return ret;
	}

	*enabled |= streams_mask;

	return 0;
}

static int xvsw_disable_streams(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state, u32 pad,
				u64 streams_mask)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	u64 *enabled = &xvsw->enabled_streams[pad - xvsw->nsinks];
	u64 streams = streams_mask;
	u64 sink_streams;
	u32 sink_pad;
	int ret;

	ret = xvsw_get_sink_pad(state, pad, &sink_pad);
	if (ret)
		return ret;

	sink_streams = v4l2_subdev_state_xlate_streams(state, pad, sink_pad,
						       &streams);

	/*
	 * Stopping is best effort: a failure would leave the streams marked as
	 * enabled in the core while the switch has stopped forwarding them.
	 */
	ret = xvip_disable_remote_stream(subdev, sink_pad, sink_streams);
	if (ret)
		dev_err(xvsw->dev, "failed to stop the source of pad %u: %d\n",
			pad, ret);

	*enabled &= ~streams_mask;

	if (!*enabled)
		xvsw_set_pad_stream(xvsw, pad, sink_pad, false);

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static const struct v4l2_mbus_framefmt xvsw_default_format = {
	.code = MEDIA_BUS_FMT_RGB888_1X24,
	.width = XVIP_MAX_WIDTH,
	.height = XVIP_MAX_HEIGHT,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SRGB,
};

static int __xvsw_set_routing(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_krouting *routing)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	enum v4l2_subdev_routing_restriction disallow;
	int ret;

	/*
	 * The switch can neither duplicate nor merge streams, so the routing is
	 * always one to one.
	 *
	 * In TDEST routing mode the sink pad carries the streams of all the
	 * source pads, and the fabric demultiplexes them based on their TDEST
	 * value, so the streams of the sink pad may end up on different source
	 * pads.
	 *
	 * In control register routing mode each master port selects a single
	 * slave port and forwards all of its data, so the sink and source pads
	 * are connected as a whole.
	 */
	disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1;

	if (!xvsw->tdest_routing)
		disallow |= V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	ret = v4l2_subdev_routing_validate(subdev, routing, disallow);
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
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_subdev_route routes[MAX_VSW_SRCS] = { };
	struct v4l2_subdev_krouting routing = {
		.routes = routes,
	};
	unsigned int i;

	/*
	 * In TDEST routing mode default to a single route, from the first sink
	 * stream to the first source pad. The upstream subdev produces a single
	 * stream by default, and declaring more sink streams than the upstream
	 * produces makes link validation fail with dangling sink streams.
	 * Multi-stream operation requires configuring the routing on both
	 * subdevs.
	 *
	 * In control register routing mode a source pad gets its data from a
	 * single sink pad, so route the sink pads one to one to the first
	 * source pads.
	 */
	routing.num_routes = xvsw->tdest_routing ?
			     1 : min(xvsw->nsinks, xvsw->nsources);

	for (i = 0; i < routing.num_routes; ++i) {
		routes[i].sink_pad = xvsw->tdest_routing ? 0 : i;
		routes[i].sink_stream = 0;
		routes[i].source_pad = xvsw->nsinks + i;
		routes[i].source_stream = 0;
		routes[i].flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	}

	return __xvsw_set_routing(subdev, state, &routing);
}

static int xvsw_enum_mbus_code(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	const struct v4l2_mbus_framefmt *format;

	/*
	 * The switch passes the data through without interpreting it, there's
	 * no list of formats to enumerate. Report the format of the stream.
	 */
	if (code->index)
		return -EINVAL;

	format = v4l2_subdev_state_get_format(state, code->pad, code->stream);
	if (!format)
		return -EINVAL;

	code->code = format->code;

	return 0;
}

static int xvsw_enum_frame_size(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_frame_size_enum *fse)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	const struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(state, fse->pad, fse->stream);
	if (!format)
		return -EINVAL;

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	if (fse->pad < xvsw->nsinks) {
		fse->min_width = 1;
		fse->max_width = XVIP_MAX_WIDTH;
		fse->min_height = 1;
		fse->max_height = XVIP_MAX_HEIGHT;
	} else {
		/*
		 * The size on the source pads is fixed and always identical to
		 * the size of the routed sink stream.
		 */
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}

static int xvsw_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_mbus_framefmt *format;

	/*
	 * The source pad formats are always identical to the format of the
	 * routed sink stream and can't be modified.
	 */
	if (fmt->pad >= xvsw->nsinks)
		return v4l2_subdev_get_fmt(subdev, state, fmt);

	format = v4l2_subdev_state_get_format(state, fmt->pad, fmt->stream);
	if (!format)
		return -EINVAL;

	format->code = fmt->format.code;
	format->width = fmt->format.width;
	format->height = fmt->format.height;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	fmt->format = *format;

	/* Propagate the format to the routed source stream. */
	format = v4l2_subdev_state_get_opposite_stream_format(state, fmt->pad,
							      fmt->stream);
	if (format)
		*format = fmt->format;

	return 0;
}

static struct v4l2_subdev_pad_ops xvsw_pad_ops = {
	.enum_mbus_code = xvsw_enum_mbus_code,
	.enum_frame_size = xvsw_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xvsw_set_format,
	.set_routing = xvsw_set_routing,
	.enable_streams = xvsw_enable_streams,
	.disable_streams = xvsw_disable_streams,
};

static struct v4l2_subdev_ops xvsw_ops = {
	.pad = &xvsw_pad_ops,
};

static const struct v4l2_subdev_internal_ops xvsw_internal_ops = {
	.init_state = xvsw_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xvsw_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xvsw_parse_of(struct xvswitch_device *xvsw)
{
	struct device_node *node = xvsw->dev->of_node;
	struct device_node *ports;
	struct device_node *port;
	unsigned int nports = 0;
	u32 routing_mode = 0;
	int ret;

	ret = of_property_read_u32(node, "xlnx,num-si-slots", &xvsw->nsinks);
	if (ret < 0 || xvsw->nsinks < MIN_VSW_SINKS ||
	    xvsw->nsinks > MAX_VSW_SINKS) {
		dev_err(xvsw->dev, "missing or invalid xlnx,num-si-slots property\n");
		return ret;
	}

	ret = of_property_read_u32(node, "xlnx,num-mi-slots", &xvsw->nsources);
	if (ret < 0 || xvsw->nsources < MIN_VSW_SRCS ||
	    xvsw->nsources > MAX_VSW_SRCS) {
		dev_err(xvsw->dev, "missing or invalid xlnx,num-mi-slots property\n");
		return ret;
	}

	ret = of_property_read_u32(node, "xlnx,routing-mode", &routing_mode);
	if (ret < 0 || routing_mode > 1) {
		dev_err(xvsw->dev, "missing or invalid xlnx,routing property\n");
		return ret;
	}

	if (!routing_mode)
		xvsw->tdest_routing = true;

	xvsw->aclk = devm_clk_get(xvsw->dev, "aclk");
	if (IS_ERR(xvsw->aclk)) {
		ret = PTR_ERR(xvsw->aclk);
		dev_err(xvsw->dev, "failed to get ap_clk (%d)\n", ret);
		return ret;
	}

	if (!xvsw->tdest_routing) {
		xvsw->saxi_ctlclk = devm_clk_get(xvsw->dev,
						 "s_axi_ctrl_aclk");
		if (IS_ERR(xvsw->saxi_ctlclk)) {
			ret = PTR_ERR(xvsw->saxi_ctlclk);
			dev_err(xvsw->dev,
				"failed to get s_axi_ctrl_aclk (%d)\n",
				ret);
			return ret;
		}
	}

	if (xvsw->tdest_routing && xvsw->nsinks > 1) {
		dev_err(xvsw->dev, "sinks = %d. Driver Limitation max 1 sink in TDEST routing mode\n",
			xvsw->nsinks);
		return -EINVAL;
	}

	ports = of_get_child_by_name(node, "ports");
	if (!ports)
		ports = node;

	for_each_child_of_node(ports, port) {
		struct device_node *endpoint;

		if (!port->name || of_node_cmp(port->name, "port"))
			continue;

		endpoint = of_get_next_child(port, NULL);
		if (!endpoint) {
			dev_err(xvsw->dev, "No port at\n");
			return -EINVAL;
		}

		/* Count the number of ports. */
		nports++;
	}

	/* validate number of ports */
	if (nports != (xvsw->nsinks + xvsw->nsources)) {
		dev_err(xvsw->dev, "invalid number of ports %u\n", nports);
		return -EINVAL;
	}

	return 0;
}

static int xvsw_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xvswitch_device *xvsw;
	struct resource *res;
	unsigned int npads;
	unsigned int i;
	int ret;

	xvsw = devm_kzalloc(&pdev->dev, sizeof(*xvsw), GFP_KERNEL);
	if (!xvsw)
		return -ENOMEM;

	xvsw->dev = &pdev->dev;

	ret = xvsw_parse_of(xvsw);
	if (ret < 0)
		return ret;

	/* ioremap only if control reg based routing */
	if (!xvsw->tdest_routing) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		xvsw->iomem = devm_ioremap_resource(xvsw->dev, res);
		if (IS_ERR(xvsw->iomem))
			return PTR_ERR(xvsw->iomem);
	}

	/*
	 * Initialize V4L2 subdevice and media entity. Pad numbers depend on the
	 * number of pads.
	 */
	npads = xvsw->nsinks + xvsw->nsources;
	xvsw->pads = devm_kzalloc(&pdev->dev, npads * sizeof(*xvsw->pads),
				  GFP_KERNEL);
	if (!xvsw->pads)
		return -ENOMEM;

	for (i = 0; i < xvsw->nsinks; ++i)
		xvsw->pads[i].flags = MEDIA_PAD_FL_SINK;

	for (; i < npads; ++i)
		xvsw->pads[i].flags = MEDIA_PAD_FL_SOURCE;

	ret = clk_prepare_enable(xvsw->aclk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable aclk (%d)\n",
			ret);
		return ret;
	}

	if (!xvsw->tdest_routing) {
		ret = clk_prepare_enable(xvsw->saxi_ctlclk);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to enable s_axi_ctl_clk (%d)\n",
				ret);
			clk_disable_unprepare(xvsw->aclk);
			return ret;
		}
	}

	subdev = &xvsw->subdev;
	v4l2_subdev_init(subdev, &xvsw_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xvsw_internal_ops;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xvsw);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	subdev->entity.function = MEDIA_ENT_F_VID_MUX;
	subdev->entity.ops = &xvsw_media_ops;

	ret = media_entity_pads_init(&subdev->entity, npads, xvsw->pads);
	if (ret < 0)
		goto clk_error;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_media;

	platform_set_drvdata(pdev, xvsw);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error_subdev;
	}

	dev_info(xvsw->dev, "Xilinx AXI4-Stream Switch found!\n");

	return 0;

error_subdev:
	v4l2_subdev_cleanup(subdev);
error_media:
	media_entity_cleanup(&subdev->entity);
clk_error:
	if (!xvsw->tdest_routing)
		clk_disable_unprepare(xvsw->saxi_ctlclk);
	clk_disable_unprepare(xvsw->aclk);
	return ret;
}

static void xvsw_remove(struct platform_device *pdev)
{
	struct xvswitch_device *xvsw = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xvsw->subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	if (!xvsw->tdest_routing)
		clk_disable_unprepare(xvsw->saxi_ctlclk);
	clk_disable_unprepare(xvsw->aclk);
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
