// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx AXI4-Stream Video Switch
 *
 * Copyright (C) 2018 Xilinx, Inc.
 *
 * Author: Vishal Sagar <vishal.sagar@xilinx.com>
 */

#include <linux/bitops.h>
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
 * @xvip: Xilinx Video IP device
 * @tdest_routing: Whether TDEST routing is enabled
 * @aclk: Video clock
 * @saxi_ctlclk: AXI-Lite control clock
 */
struct xvswitch_device {
	struct xvip_device xvip;
	bool tdest_routing;
	struct clk *aclk;
	struct clk *saxi_ctlclk;
};

static inline struct xvswitch_device *to_xvsw(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xvswitch_device, xvip.subdev);
}

static inline u32 xvswitch_read(struct xvswitch_device *xvsw, u32 addr)
{
	return xvip_read(&xvsw->xvip, addr);
}

static inline void xvswitch_write(struct xvswitch_device *xvsw, u32 addr,
				  u32 value)
{
	xvip_write(&xvsw->xvip, addr, value);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Video Operations
 */

static int xvsw_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_subdev_state *state;
	struct v4l2_subdev_route *route;
	unsigned long unused_sources;
	unsigned int i;

	/*
	 * In TDEST routing mode the hardware doesn't need to be configured.
	 *
	 * TODO: Validate the routing configuration by checking the frame
	 * descriptors (this requires specifying the TDEST routing table in the
	 * device tree).
	 */
	if (xvsw->tdest_routing)
		return 0;

	if (!enable) {
		/* In control reg routing, disable all master ports */
		for (i = 0; i < xvsw->xvip.num_sources; i++) {
			xvswitch_write(xvsw, XVSW_MI_MUX_REG_BASE + (i * 4),
				       XVSW_MI_MUX_DISABLE_MASK);
		}
		xvswitch_write(xvsw, XVSW_CTRL_REG, XVSW_CTRL_REG_UPDATE_MASK);
		return 0;
	}

	/*
	 * In case of control reg routing, from routing table write the values
	 * into respective reg and enable.
	 *
	 * Start by configuring the active routes, and record the unused
	 * sources. Avoid configuring the same output multiple times (in case
	 * multiple streams flow through the same source pad). Finally,
	 * configure Unused outputs as disabled.
	 */

	state = v4l2_subdev_lock_and_get_active_state(subdev);

	unused_sources = (1 << MAX_VSW_SRCS) - 1;

	for_each_active_route(&state->routing, route) {
		unsigned int source = route->source_pad;

		if (!(unused_sources & BIT(source)))
			continue;

		xvswitch_write(xvsw, XVSW_MI_MUX_REG_BASE + (source * 4),
			       route->sink_pad);

		unused_sources &= ~BIT(source);
	}

	for_each_set_bit(i, &unused_sources, MAX_VSW_SRCS)
		xvswitch_write(xvsw, XVSW_MI_MUX_REG_BASE + (i * 4),
			       XVSW_MI_MUX_DISABLE_MASK);

	v4l2_subdev_unlock_state(state);

	xvswitch_write(xvsw, XVSW_CTRL_REG, XVSW_CTRL_REG_UPDATE_MASK);

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
	 * In TDEST routing mode, we can't validate routes are, as the TDEST
	 * value isn't known. Only disable 1-to-N routing, as a stream is routed
	 * to a single output.
	 *
	 * In register-based mode, streams must be map 1-to-1, and can be mixed
	 * across different source pads.
	 */
	if (xvsw->tdest_routing)
		disallow = V4L2_SUBDEV_ROUTING_NO_1_TO_N;
	else
		disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1
			 | V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	ret = v4l2_subdev_routing_validate(subdev, routing, disallow);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(subdev, state, routing,
						&xvsw_default_format);
}

static int xvsw_init_cfg(struct v4l2_subdev *subdev,
			 struct v4l2_subdev_state *state)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_subdev_krouting routing = { };
	struct v4l2_subdev_route *routes;
	unsigned int num_routes;
	unsigned int i;
	int ret;

	num_routes = min(xvsw->xvip.num_sinks, xvsw->xvip.num_sources);
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
		route->source_pad = i + xvsw->xvip.num_sinks;
		route->flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	};

	routing.num_routes = num_routes;
	routing.routes = routes;

	ret = __xvsw_set_routing(subdev, state, &routing);

	kfree(routes);

	return ret;
}

static int xvsw_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_mbus_framefmt *source_fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&subdev->entity))
		return -EBUSY;

	/*
	 * The source pad format is always identical to the sink pad format and
	 * can't be modified.
	 */
	if (format->pad >= xvsw->xvip.num_sinks)
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

static int xvsw_set_routing(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_state *state,
			    enum v4l2_subdev_format_whence which,
			    struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_pipeline(&subdev->entity))
		return -EBUSY;

	return __xvsw_set_routing(subdev, state, routing);
}

static const struct v4l2_subdev_video_ops xvsw_video_ops = {
	.s_stream = xvsw_s_stream,
};

static const struct v4l2_subdev_pad_ops xvsw_pad_ops = {
	.init_cfg = xvsw_init_cfg,
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xvip_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xvsw_set_format,
	.link_validate = xvip_link_validate,
	.set_routing = xvsw_set_routing,
	.get_mbus_config = xvip_get_mbus_config,
};

static const struct v4l2_subdev_ops xvsw_ops = {
	.video = &xvsw_video_ops,
	.pad = &xvsw_pad_ops,
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

static int xvsw_parse_of(struct xvswitch_device *xvsw,
			 struct xvip_device_info *info)
{
	struct device_node *node = xvsw->xvip.dev->of_node;
	u32 routing_mode = 0;
	int ret;

	ret = of_property_read_u32(node, "xlnx,num-si-slots", &info->num_sinks);
	if (ret < 0 || info->num_sinks < MIN_VSW_SINKS ||
	    info->num_sinks > MAX_VSW_SINKS) {
		dev_err(xvsw->xvip.dev,
			"missing or invalid xlnx,num-si-slots property\n");
		return ret;
	}

	ret = of_property_read_u32(node, "xlnx,num-mi-slots", &info->num_sources);
	if (ret < 0 || info->num_sources < MIN_VSW_SRCS ||
	    info->num_sources > MAX_VSW_SRCS) {
		dev_err(xvsw->xvip.dev,
			"missing or invalid xlnx,num-mi-slots property\n");
		return ret;
	}

	ret = of_property_read_u32(node, "xlnx,routing-mode", &routing_mode);
	if (ret < 0 || routing_mode > 1) {
		dev_err(xvsw->xvip.dev,
			"missing or invalid xlnx,routing property\n");
		return ret;
	}

	if (!routing_mode)
		xvsw->tdest_routing = true;

	xvsw->aclk = devm_clk_get(xvsw->xvip.dev, "aclk");
	if (IS_ERR(xvsw->aclk)) {
		ret = PTR_ERR(xvsw->aclk);
		dev_err(xvsw->xvip.dev, "failed to get ap_clk (%d)\n", ret);
		return ret;
	}

	if (!xvsw->tdest_routing) {
		xvsw->saxi_ctlclk = devm_clk_get(xvsw->xvip.dev,
						 "s_axi_ctl_clk");
		if (IS_ERR(xvsw->saxi_ctlclk)) {
			ret = PTR_ERR(xvsw->saxi_ctlclk);
			dev_err(xvsw->xvip.dev,
				"failed to get s_axi_ctl_clk (%d)\n",
				ret);
			return ret;
		}
	}

	if (xvsw->tdest_routing && info->num_sinks > 1) {
		dev_err(xvsw->xvip.dev,
			"sinks = %d. Driver Limitation max 1 sink in TDEST routing mode\n",
			info->num_sinks);
		return -EINVAL;
	}

	info->has_axi_lite = !xvsw->tdest_routing;
	return 0;
}

static int xvsw_probe(struct platform_device *pdev)
{
	struct xvip_device_info xvsw_info = { };
	struct v4l2_subdev *subdev;
	struct xvswitch_device *xvsw;
	unsigned int npads;
	int ret;

	xvsw = devm_kzalloc(&pdev->dev, sizeof(*xvsw), GFP_KERNEL);
	if (!xvsw)
		return -ENOMEM;

	xvsw->xvip.dev = &pdev->dev;

	ret = xvsw_parse_of(xvsw, &xvsw_info);
	if (ret < 0)
		return ret;

	ret = xvip_device_init(&xvsw->xvip, &xvsw_info);
	if (ret < 0)
		return ret;

	/*
	 * Initialize V4L2 subdevice and media entity. Pad numbers depend on the
	 * number of pads.
	 */
	npads = xvsw->xvip.num_sinks + xvsw->xvip.num_sources;

	ret = clk_prepare_enable(xvsw->aclk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable aclk (%d)\n",
			ret);
		goto error_xvip;
	}

	if (!xvsw->tdest_routing) {
		ret = clk_prepare_enable(xvsw->saxi_ctlclk);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to enable s_axi_ctl_clk (%d)\n",
				ret);
			clk_disable_unprepare(xvsw->aclk);
			goto error_xvip;
		}
	}

	subdev = &xvsw->xvip.subdev;
	v4l2_subdev_init(subdev, &xvsw_ops);
	subdev->dev = &pdev->dev;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xvsw);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	subdev->entity.ops = &xvsw_media_ops;

	ret = media_entity_pads_init(&subdev->entity, npads, xvsw->xvip.pads);
	if (ret < 0)
		goto error_clk;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret)
		goto error;

	platform_set_drvdata(pdev, xvsw);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	dev_info(xvsw->xvip.dev, "Xilinx AXI4-Stream Switch found!\n");

	return 0;

error:
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
error_clk:
	if (!xvsw->tdest_routing)
		clk_disable_unprepare(xvsw->saxi_ctlclk);
	clk_disable_unprepare(xvsw->aclk);
error_xvip:
	xvip_device_cleanup(&xvsw->xvip);
	return ret;
}

static int xvsw_remove(struct platform_device *pdev)
{
	struct xvswitch_device *xvsw = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xvsw->xvip.subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	if (!xvsw->tdest_routing)
		clk_disable_unprepare(xvsw->saxi_ctlclk);
	clk_disable_unprepare(xvsw->aclk);
	xvip_device_cleanup(&xvsw->xvip);

	return 0;
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
MODULE_LICENSE("GPL v2");
