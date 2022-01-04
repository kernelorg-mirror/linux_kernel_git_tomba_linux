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
 * @xvip: Xilinx Video IP device
 * @routing: sink pad connected to each source pad (-1 if none)
 * @formats: active V4L2 media bus formats on sink pads
 * @tdest_routing: Whether TDEST routing is enabled
 * @aclk: Video clock
 * @saxi_ctlclk: AXI-Lite control clock
 */
struct xvswitch_device {
	struct xvip_device xvip;
	int routing[MAX_VSW_SRCS];
	struct v4l2_mbus_framefmt *formats;
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
	unsigned int i;

	/* Nothing to be done in case of TDEST routing */
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
	 * In case of control reg routing,
	 * from routing table write the values into respective reg
	 * and enable
	 */
	for (i = 0; i < MAX_VSW_SRCS; i++) {
		u32 val;

		if (xvsw->routing[i] != -1)
			val = xvsw->routing[i];
		else
			val = XVSW_MI_MUX_DISABLE_MASK;

		xvswitch_write(xvsw, XVSW_MI_MUX_REG_BASE + (i * 4),
			       val);
	}

	xvswitch_write(xvsw, XVSW_CTRL_REG, XVSW_CTRL_REG_UPDATE_MASK);

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static struct v4l2_mbus_framefmt *
xvsw_get_pad_format(struct xvswitch_device *xvsw,
		    struct v4l2_subdev_state *sd_state,
		    unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *get_fmt;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		get_fmt = v4l2_subdev_get_try_format(&xvsw->xvip.subdev,
						     sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		get_fmt = &xvsw->formats[pad];
		break;
	default:
		get_fmt = NULL;
		break;
	}

	return get_fmt;
}

static int xvsw_get_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	int pad = fmt->pad;
	struct v4l2_mbus_framefmt *get_fmt;

	/*
	 * If control reg routing and pad is source pad then
	 * get corresponding sink pad. if no sink pad then
	 * clear the format and return
	 */

	if (!xvsw->tdest_routing && pad >= xvsw->xvip.num_sinks) {
		pad = xvsw->routing[pad - xvsw->xvip.num_sinks];
		if (pad < 0) {
			memset(&fmt->format, 0, sizeof(fmt->format));
			return 0;
		}
	}

	get_fmt = xvsw_get_pad_format(xvsw, sd_state, pad, fmt->which);
	if (!get_fmt)
		return -EINVAL;

	fmt->format = *get_fmt;

	return 0;
}

static int xvsw_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_mbus_framefmt *format;

	if (!xvsw->tdest_routing && fmt->pad >= xvsw->xvip.num_sinks) {
		/*
		 * In case of control reg routing,
		 * get the corresponding sink pad to source pad passed.
		 *
		 * The source pad format is always identical to the
		 * sink pad format and can't be modified.
		 *
		 * If sink pad found then get_format for that pad
		 * else clear the fmt->format as the source pad
		 * isn't connected and return.
		 */
		return xvsw_get_format(subdev, sd_state, fmt);
	}

	if (xvsw->xvip.num_sinks == 1 && fmt->pad != 0) {
		struct v4l2_mbus_framefmt *sinkformat;

		/*
		 * in tdest routing if there is only one sink then all the
		 * source pads will have same property as sink pad, assuming
		 * streams going to each source pad will have same
		 * properties.
		 */

		/* get sink pad format */
		sinkformat = xvsw_get_pad_format(xvsw, sd_state, 0, fmt->which);
		if (!sinkformat)
			return -EINVAL;

		fmt->format = *sinkformat;

		/* set sink pad format on source pad */
		format = xvsw_get_pad_format(xvsw, sd_state, fmt->pad, fmt->which);
		if (!format)
			return -EINVAL;

		*format = *sinkformat;

		return 0;
	}

	/*
	 * In TDEST routing mode, one can set any format on the pad as
	 * it can't be checked which pad's data will travel to
	 * which pad. E.g. In a system with 2 slaves and 4 masters,
	 * S0 or S1 data can reach M0 thru M3 based on TDEST
	 * S0 may have RBG and S1 may have YUV. M0, M1 stream RBG
	 * and M2, M3 stream YUV based on TDEST.
	 *
	 * In Control reg routing mode, set format only for sink pads.
	 */
	format = xvsw_get_pad_format(xvsw, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	format->code = fmt->format.code;
	format->width = clamp_t(unsigned int, fmt->format.width,
				XVIP_MIN_WIDTH, XVIP_MAX_WIDTH);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 XVIP_MIN_HEIGHT, XVIP_MAX_HEIGHT);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	fmt->format = *format;

	return 0;
}

static int xvsw_get_routing(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_routing *route)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	unsigned int i;
	u32 min;

	/* In case of tdest routing, we can't get routing */
	if (xvsw->tdest_routing)
		return -EINVAL;

	mutex_lock(&subdev->entity.graph_obj.mdev->graph_mutex);

	if (xvsw->xvip.num_sources < route->num_routes)
		min = xvsw->xvip.num_sources;
	else
		min = route->num_routes;

	for (i = 0; i < min; ++i) {
		route->routes[i].sink = xvsw->routing[i];
		route->routes[i].source = i;
	}

	route->num_routes = xvsw->xvip.num_sources;

	mutex_unlock(&subdev->entity.graph_obj.mdev->graph_mutex);

	return 0;
}

static int xvsw_set_routing(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_routing *route)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	unsigned int i;
	int ret = 0;

	/* In case of tdest routing, we can't set routing */
	if (xvsw->tdest_routing)
		return -EINVAL;

	mutex_lock(&subdev->entity.graph_obj.mdev->graph_mutex);

	if (media_entity_pipeline(&subdev->entity)) {
		ret = -EBUSY;
		goto done;
	}

	for (i = 0; i < xvsw->xvip.num_sources; ++i)
		xvsw->routing[i] = -1;

	for (i = 0; i < route->num_routes; ++i)
		xvsw->routing[route->routes[i].source - xvsw->xvip.num_sinks] =
			route->routes[i].sink;

done:
	mutex_unlock(&subdev->entity.graph_obj.mdev->graph_mutex);
	return ret;
}

static int xvsw_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static int xvsw_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static const struct v4l2_subdev_video_ops xvsw_video_ops = {
	.s_stream = xvsw_s_stream,
};

static const struct v4l2_subdev_pad_ops xvsw_pad_ops = {
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xvip_enum_frame_size,
	.get_fmt = xvsw_get_format,
	.set_fmt = xvsw_set_format,
	.get_routing = xvsw_get_routing,
	.set_routing = xvsw_set_routing,
};

static const struct v4l2_subdev_ops xvsw_ops = {
	.video = &xvsw_video_ops,
	.pad = &xvsw_pad_ops,
};

static const struct v4l2_subdev_internal_ops xvsw_internal_ops = {
	.open = xvsw_open,
	.close = xvsw_close,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static bool xvsw_has_route(struct media_entity *entity, unsigned int pad0,
			   unsigned int pad1)
{
	struct xvswitch_device *xvsw =
		to_xvsw(media_entity_to_v4l2_subdev(entity));
	unsigned int sink0, sink1;

	/* Two sinks are never connected together. */
	if (pad0 < xvsw->xvip.num_sinks && pad1 < xvsw->xvip.num_sinks)
		return false;

	/* In TDEST routing, assume all sinks and sources are connected */
	if (xvsw->tdest_routing)
		return true;

	sink0 = pad0 < xvsw->xvip.num_sinks
	       ? pad0 : xvsw->routing[pad0 - xvsw->xvip.num_sinks];
	sink1 = pad1 < xvsw->xvip.num_sinks
	      ? pad1 : xvsw->routing[pad1 - xvsw->xvip.num_sinks];

	return sink0 == sink1;
}

static const struct media_entity_operations xvsw_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_route = xvsw_has_route,
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
	unsigned int i, padcount;
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
	padcount = xvsw->tdest_routing ? npads : xvsw->xvip.num_sinks;

	/*
	 * In case of tdest routing, allocate format per pad.
	 * source pad format has to match one of the sink pads in tdest routing.
	 *
	 * Otherwise only allocate for sinks as sources will
	 * get the same pad format and corresponding sink.
	 * set format on src pad will return corresponding sinks data.
	 */
	xvsw->formats = devm_kzalloc(&pdev->dev,
				     padcount * sizeof(*xvsw->formats),
				     GFP_KERNEL);
	if (!xvsw->formats) {
		ret = -ENOMEM;
		goto error_xvip;
	}

	for (i = 0; i < padcount; i++) {
		xvsw->formats[i].code = MEDIA_BUS_FMT_RGB888_1X24;
		xvsw->formats[i].field = V4L2_FIELD_NONE;
		xvsw->formats[i].colorspace = V4L2_COLORSPACE_SRGB;
		xvsw->formats[i].width = XVIP_MAX_WIDTH;
		xvsw->formats[i].height = XVIP_MAX_HEIGHT;
	}

	/*
	 * Initialize the routing table if none are connected.
	 * Routing table is valid only incase routing is not TDEST based.
	 */
	for (i = 0; i < MAX_VSW_SRCS; ++i)
		xvsw->routing[i] = -1;

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
	subdev->internal_ops = &xvsw_internal_ops;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xvsw);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xvsw_media_ops;

	ret = media_entity_pads_init(&subdev->entity, npads, xvsw->xvip.pads);
	if (ret < 0)
		goto error_clk;

	platform_set_drvdata(pdev, xvsw);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	dev_info(xvsw->xvip.dev, "Xilinx AXI4-Stream Switch found!\n");

	return 0;

error:
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
