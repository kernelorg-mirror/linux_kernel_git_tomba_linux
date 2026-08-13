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
 * @routing: sink pad connected to each source pad (-1 if none)
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
	int routing[MAX_VSW_SRCS];
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
		for (i = 0; i < xvsw->nsources; i++) {
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

static int xvsw_init_state(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	unsigned int npads = xvsw->nsinks + xvsw->nsources;
	unsigned int i;

	for (i = 0; i < npads; i++) {
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

static int xvsw_get_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	int pad = fmt->pad;

	/*
	 * If control reg routing and pad is source pad then
	 * get corresponding sink pad. if no sink pad then
	 * clear the format and return
	 */

	if (!xvsw->tdest_routing && pad >= xvsw->nsinks) {
		pad = xvsw->routing[pad - xvsw->nsinks];
		if (pad < 0) {
			memset(&fmt->format, 0, sizeof(fmt->format));
			return 0;
		}
	}

	fmt->format = *v4l2_subdev_state_get_format(sd_state, pad);

	return 0;
}

static int xvsw_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xvswitch_device *xvsw = to_xvsw(subdev);
	struct v4l2_mbus_framefmt *format;

	if (!xvsw->tdest_routing && fmt->pad >= xvsw->nsinks) {
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

	if (xvsw->nsinks == 1 && fmt->pad != 0) {
		struct v4l2_mbus_framefmt *sinkformat;

		/*
		 * in tdest routing if there is only one sink then all the
		 * source pads will have same property as sink pad, assuming
		 * streams going to each source pad will have same
		 * properties.
		 */

		/* get sink pad format */
		sinkformat = v4l2_subdev_state_get_format(sd_state, 0);

		fmt->format = *sinkformat;

		/* set sink pad format on source pad */
		format = v4l2_subdev_state_get_format(sd_state, fmt->pad);

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
	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	format->code = fmt->format.code;
	format->width = fmt->format.width;
	format->height = fmt->format.height;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	fmt->format = *format;

	return 0;
}

static struct v4l2_subdev_video_ops xvsw_video_ops = {
	.s_stream = xvsw_s_stream,
};

static struct v4l2_subdev_pad_ops xvsw_pad_ops = {
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xvip_enum_frame_size,
	.get_fmt = xvsw_get_format,
	.set_fmt = xvsw_set_format,
};

static struct v4l2_subdev_ops xvsw_ops = {
	.video = &xvsw_video_ops,
	.pad = &xvsw_pad_ops,
};

static const struct v4l2_subdev_internal_ops xvsw_internal_ops = {
	.init_state = xvsw_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static bool xvsw_has_pad_interdep(struct media_entity *entity,
				  unsigned int pad0, unsigned int pad1)
{
	struct xvswitch_device *xvsw =
		container_of(entity, struct xvswitch_device, subdev.entity);
	unsigned int sink0, sink1;

	/* Two sinks are never connected together. */
	if (pad0 < xvsw->nsinks && pad1 < xvsw->nsinks)
		return false;

	/* In TDEST routing, assume all sinks and sources are connected */
	if (xvsw->tdest_routing)
		return true;

	sink0 = pad0 < xvsw->nsinks ? pad0 : xvsw->routing[pad0 - xvsw->nsinks];
	sink1 = pad1 < xvsw->nsinks ? pad1 : xvsw->routing[pad1 - xvsw->nsinks];

	return sink0 == sink1;
}

static const struct media_entity_operations xvsw_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = xvsw_has_pad_interdep,
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
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
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
