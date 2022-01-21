/*
 * Xilinx HLS Core
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

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/xilinx-hls.h>
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "xilinx-hls-common.h"
#include "xilinx-vip.h"

/**
 * struct xhls_device - Xilinx HLS Core device structure
 * @xvip: Xilinx Video IP device
 * @compatible: first DT compatible string for the device
 * @formats: active V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @model: additional description of IP implementation if available
 * @ctrl_handler: control handler
 * @user_mem: user portion of the register space
 * @user_mem_size: size of the user portion of the register space
 */
struct xhls_device {
	struct xvip_device xvip;

	const char *compatible;

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *model;

	void __iomem *user_mem;
	size_t user_mem_size;
};

static inline struct xhls_device *to_hls(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xhls_device, xvip.subdev);
}

/* -----------------------------------------------------------------------------
 * xvip operations
 */

static int xhls_enable_streams(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct xhls_device *xhls = to_hls(sd);
	struct v4l2_mbus_framefmt *format = &xhls->formats[XVIP_PAD_SINK];

	xvip_write(&xhls->xvip, XHLS_REG_COLS, format->width);
	xvip_write(&xhls->xvip, XHLS_REG_ROWS, format->height);

	xvip_write(&xhls->xvip, XVIP_CTRL_CONTROL,
		   XHLS_REG_CTRL_AUTO_RESTART | XVIP_CTRL_CONTROL_SW_ENABLE);

	return 0;
}

static int xhls_disable_streams(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state, u32 pad,
				u64 streams_mask)
{
	struct xhls_device *xhls = to_hls(sd);

	xvip_write(&xhls->xvip, XVIP_CTRL_CONTROL, 0);

	return 0;
}

static const struct xvip_device_ops xhls_xvip_device_ops = {
	.enable_streams = xhls_enable_streams,
	.disable_streams = xhls_disable_streams,
};

/* -----------------------------------------------------------------------------
 * Controls
 */

static const struct v4l2_ctrl_config xhls_model_ctrl = {
	.id	= V4L2_CID_XILINX_HLS_MODEL,
	.name	= "HLS Model",
	.type	= V4L2_CTRL_TYPE_STRING,
	.step	= 1,
	.flags	= V4L2_CTRL_FLAG_READ_ONLY,
};

static int xhls_create_controls(struct xhls_device *xhls)
{
	struct v4l2_ctrl_config model = xhls_model_ctrl;
	struct v4l2_ctrl *ctrl;
	int ret;

	model.max = strlen(xhls->compatible);
	model.min = model.max;

	ret = v4l2_ctrl_handler_init(&xhls->ctrl_handler, 1);
	if (ret) {
		dev_err(xhls->xvip.dev,
			"failed to initializing controls (%d)\n", ret);
		return ret;
	}

	ctrl = v4l2_ctrl_new_custom(&xhls->ctrl_handler, &model, NULL);

	if (xhls->ctrl_handler.error || !ctrl) {
		dev_err(xhls->xvip.dev, "failed to add controls\n");
		v4l2_ctrl_handler_free(&xhls->ctrl_handler);
		return xhls->ctrl_handler.error;
	}

	v4l2_ctrl_s_ctrl_string(ctrl, xhls->compatible);

	xhls->xvip.subdev.ctrl_handler = &xhls->ctrl_handler;

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Core Operations
 */

static int xhls_user_read(struct xhls_device *xhls,
			  struct xilinx_axi_hls_registers *regs)
{
	unsigned int i;
	u32 offset;
	u32 value;

	if (regs->num_regs >= xhls->user_mem_size / 4)
		return -EINVAL;

	for (i = 0; i < regs->num_regs; ++i) {
		if (copy_from_user(&offset, &regs->regs[i].offset,
				   sizeof(offset)))
			return -EFAULT;

		if (offset >= xhls->user_mem_size || offset & 3)
			return -EINVAL;

		value = ioread32(xhls->user_mem + offset);

		if (copy_to_user(&regs->regs[i].value, &value, sizeof(value)))
			return -EFAULT;
	}

	return 0;
}

static int xhls_user_write(struct xhls_device *xhls,
			   struct xilinx_axi_hls_registers *regs)
{
	struct xilinx_axi_hls_register reg;
	unsigned int i;

	if (regs->num_regs >= xhls->user_mem_size / 4)
		return -EINVAL;

	for (i = 0; i < regs->num_regs; ++i) {
		if (copy_from_user(&reg, &regs->regs[i], sizeof(reg)))
			return -EFAULT;

		if (reg.offset >= xhls->user_mem_size || reg.offset & 3)
			return -EINVAL;

		iowrite32(reg.value, xhls->user_mem + reg.offset);
	}

	return 0;
}

static long xhls_ioctl(struct v4l2_subdev *subdev, unsigned int cmd, void *arg)
{
	struct xhls_device *xhls = to_hls(subdev);

	switch (cmd) {
	case XILINX_AXI_HLS_READ:
		return xhls_user_read(xhls, arg);
	case XILINX_AXI_HLS_WRITE:
		return xhls_user_write(xhls, arg);
	}

	return -ENOTTY;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static struct v4l2_mbus_framefmt *
__xhls_get_pad_format(struct xhls_device *xhls,
		      struct v4l2_subdev_state *sd_state,
		      unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&xhls->xvip.subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &xhls->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int xhls_get_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xhls_device *xhls = to_hls(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __xhls_get_pad_format(xhls, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int xhls_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xhls_device *xhls = to_hls(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __xhls_get_pad_format(xhls, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == XVIP_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	xvip_set_format_size(format, fmt);

	fmt->format = *format;

	/* Propagate the format to the source pad. */
	format = __xhls_get_pad_format(xhls, sd_state, XVIP_PAD_SOURCE,
					 fmt->which);
	if (!format)
		return -EINVAL;

	xvip_set_format_size(format, fmt);

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static int xhls_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct xhls_device *xhls = to_hls(subdev);
	struct v4l2_mbus_framefmt *format;

	/* Initialize with default formats */
	format = v4l2_subdev_get_try_format(subdev, fh->state, XVIP_PAD_SINK);
	*format = xhls->default_formats[XVIP_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, XVIP_PAD_SOURCE);
	*format = xhls->default_formats[XVIP_PAD_SOURCE];

	return 0;
}

static int xhls_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static struct v4l2_subdev_core_ops xhls_core_ops = {
	.ioctl = xhls_ioctl,
};

static struct v4l2_subdev_pad_ops xhls_pad_ops = {
	.enum_mbus_code = xvip_enum_mbus_code,
	.enum_frame_size = xvip_enum_frame_size,
	.get_fmt = xhls_get_format,
	.set_fmt = xhls_set_format,
	.enable_streams = xvip_enable_streams,
	.disable_streams = xvip_disable_streams,
};

static struct v4l2_subdev_ops xhls_ops = {
	.core   = &xhls_core_ops,
	.pad    = &xhls_pad_ops,
};

static const struct v4l2_subdev_internal_ops xhls_internal_ops = {
	.open = xhls_open,
	.close = xhls_close,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xhls_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static void xhls_init_formats(struct xhls_device *xhls)
{
	struct v4l2_mbus_framefmt *format;

	/* Initialize default and active formats */
	format = &xhls->default_formats[XVIP_PAD_SINK];
	format->code = xhls->xvip.ports[XVIP_PAD_SINK].format->code;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	format->width = xvip_read(&xhls->xvip, XHLS_REG_COLS);
	format->height = xvip_read(&xhls->xvip, XHLS_REG_ROWS);

	xhls->formats[XVIP_PAD_SINK] = *format;

	format = &xhls->default_formats[XVIP_PAD_SOURCE];
	*format = xhls->default_formats[XVIP_PAD_SINK];
	format->code = xhls->xvip.ports[XVIP_PAD_SOURCE].format->code;

	xhls->formats[XVIP_PAD_SOURCE] = *format;
}

static int xhls_parse_of(struct xhls_device *xhls)
{
	struct device_node *node = xhls->xvip.dev->of_node;
	int ret;

	ret = of_property_read_string(node, "compatible", &xhls->compatible);
	if (ret < 0)
		return -EINVAL;

	return 0;
}

static const struct xvip_device_info xhls_info = {
	.has_axi_lite = true,
	.has_port_formats = true,
	.num_sinks = 1,
	.num_sources = 1,
};

static int xhls_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xhls_device *xhls;
	struct resource *mem;
	int ret;

	xhls = devm_kzalloc(&pdev->dev, sizeof(*xhls), GFP_KERNEL);
	if (!xhls)
		return -ENOMEM;

	xhls->xvip.dev = &pdev->dev;
	xhls->xvip.ops = &xhls_xvip_device_ops;

	ret = xhls_parse_of(xhls);
	if (ret < 0)
		return ret;

	ret = xvip_device_init(&xhls->xvip, &xhls_info);
	if (ret < 0)
		return ret;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	xhls->user_mem = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(xhls->user_mem))
		return PTR_ERR(xhls->user_mem);
	xhls->user_mem_size = resource_size(mem);

	/* Reset and initialize the core */
	xvip_reset(&xhls->xvip);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xhls->xvip.subdev;
	v4l2_subdev_init(subdev, &xhls_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xhls_internal_ops;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xhls);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	xhls_init_formats(xhls);

	subdev->entity.ops = &xhls_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, xhls->xvip.pads);
	if (ret < 0)
		goto error;

	ret = xhls_create_controls(xhls);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, xhls);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	dev_info(xhls->xvip.dev, "device %s found\n", xhls->compatible);

	return 0;

error:
	v4l2_ctrl_handler_free(&xhls->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
	xvip_device_cleanup(&xhls->xvip);
	return ret;
}

static int xhls_remove(struct platform_device *pdev)
{
	struct xhls_device *xhls = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xhls->xvip.subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&xhls->ctrl_handler);
	media_entity_cleanup(&subdev->entity);

	xvip_device_cleanup(&xhls->xvip);

	return 0;
}

static const struct of_device_id xhls_of_id_table[] = {
	{ .compatible = "xlnx,v-hls" },
	{ }
};
MODULE_DEVICE_TABLE(of, xhls_of_id_table);

static struct platform_driver xhls_driver = {
	.driver = {
		.name = "xilinx-hls",
		.of_match_table = xhls_of_id_table,
	},
	.probe = xhls_probe,
	.remove = xhls_remove,
};

module_platform_driver(xhls_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Xilinx HLS Core Driver");
MODULE_LICENSE("GPL v2");
