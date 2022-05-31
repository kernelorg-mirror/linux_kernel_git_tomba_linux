// SPDX-License-Identifier: GPL-2.0
/*
 * TI VIP capture driver
 *
 * Copyright (C) 2018 Texas Instruments Incorporated -  http://www.ti.com/
 * David Griego, <dagriego@biglakesoftware.com>
 * Dale Farnsworth, <dale@farnsworth.org>
 * Nikhil Devshatwar, <nikhil.nd@ti.com>
 * Benoit Parrot, <bparrot@ti.com>
 */

#include "media/v4l2-subdev.h"
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include "vip.h"

#define VIP_SLICE_NUM_PADS 4

static bool vip_slice_pad_is_sink(u32 pad)
{
	return pad == 0 || pad == 1;
}

static bool vip_slice_pad_is_source(u32 pad)
{
	return pad == 2 || pad == 3;
}

static u32 vip_slice_invert_pad(u32 pad)
{
	if (vip_slice_pad_is_source(pad))
		return pad - 2;
	else
		return pad + 2;
}

static struct vip_port *vip_slice_pad_to_port(struct vip_slice *slice, u32 pad)
{
	if (WARN_ON(pad >= VIP_SLICE_NUM_PADS))
		return NULL;

	return slice->ports[pad % 2];
}

u32 vip_port_to_slice_source_pad(struct vip_port *port)
{
	return port->port_id + 2;
}

static inline struct vip_slice *to_vip_slice(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vip_slice, sd);
}

static int vip_slice_enum_mbus_code(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *state,
					  struct v4l2_subdev_mbus_code_enum *code)
{
	return 0;
}

static int vip_slice_enum_frame_size(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *state,
					   struct v4l2_subdev_frame_size_enum *fse)
{
	return 0;
}


static int vip_slice_set_fmt(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_get_pad_format(sd, state, format->pad);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	if (vip_slice_pad_is_sink(format->pad)) {
		/* propagate to source pad */

		fmt = v4l2_subdev_get_pad_format(sd, state, vip_slice_invert_pad(format->pad));
		if (!fmt)
			return -EINVAL;

		*fmt = format->format;
	}

	return 0;
}

static int vip_slice_init_cfg(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state)
{
	int i;

	struct v4l2_subdev_format format = {
		.which = state ? V4L2_SUBDEV_FORMAT_TRY
		: V4L2_SUBDEV_FORMAT_ACTIVE,
		//.pad = vip_slice_PAD_SINK,
		.format = {
			.width = 640,
			.height = 480,
			.code = MEDIA_BUS_FMT_UYVY8_2X8,
			.field = V4L2_FIELD_NONE,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.ycbcr_enc = V4L2_YCBCR_ENC_601,
			.quantization = V4L2_QUANTIZATION_LIM_RANGE,
			.xfer_func = V4L2_XFER_FUNC_SRGB,
		},
	};

	for (i = 0; i < 2; ++i) {
		format.pad = i;
		return vip_slice_set_fmt(sd, state, &format);
	}

	return 0;
}

static int vip_slice_enable_streams(struct v4l2_subdev *sd,
		      struct v4l2_subdev_state *state, u32 pad,
		      u64 streams_mask)
{
	struct vip_slice *slice = to_vip_slice(sd);
	struct vip_port *port = vip_slice_pad_to_port(slice, pad);
	int ret;

	if (!vip_slice_pad_is_source(pad))
		return -EINVAL;

	if (streams_mask != BIT(0))
		return -EINVAL;

	ret = v4l2_subdev_call(port->remote_subdev, video, s_stream, 1);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		WARN_ON(1);
		return ret;
	}

	return 0;
}

static int vip_slice_disable_streams(struct v4l2_subdev *sd,
		       struct v4l2_subdev_state *state, u32 pad,
		       u64 streams_mask)
{
	struct vip_slice *slice = to_vip_slice(sd);
	struct vip_port *port = vip_slice_pad_to_port(slice, pad);
	int ret;

	if (!vip_slice_pad_is_source(pad))
		return -EINVAL;

	if (streams_mask != BIT(0))
		return -EINVAL;

	ret = v4l2_subdev_call(port->remote_subdev, video, s_stream, 0);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		WARN_ON(1);
		return ret;
	}

	return 0;
}

static const struct v4l2_subdev_pad_ops vip_slice_pad_ops = {
	.init_cfg = vip_slice_init_cfg,
	.enum_mbus_code = vip_slice_enum_mbus_code,
	.enum_frame_size = vip_slice_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = vip_slice_set_fmt,
	.enable_streams = vip_slice_enable_streams,
	.disable_streams = vip_slice_disable_streams,
};

static const struct v4l2_subdev_ops vip_slice_subdev_ops = {
	.pad = &vip_slice_pad_ops,
};

static struct media_entity_operations vip_slice_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

int vip_create_slice_subdev(struct vip_slice *slice)
{
	struct v4l2_subdev *sd = &slice->sd;
	int ret;

	v4l2_subdev_init(sd, &vip_slice_subdev_ops);
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	strncpy(sd->name, slice->name, sizeof(sd->name));
	sd->dev = &slice->shared->pdev->dev;

	slice->pads[0].flags = MEDIA_PAD_FL_SINK;
	slice->pads[1].flags = MEDIA_PAD_FL_SINK;
	slice->pads[2].flags = MEDIA_PAD_FL_SOURCE;
	slice->pads[3].flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.ops = &vip_slice_media_ops;
	ret = media_entity_pads_init(&sd->entity, ARRAY_SIZE(slice->pads),
				     slice->pads);
	if (ret) {
		WARN_ON(1);
		return ret;
	}

	ret = v4l2_subdev_init_finalize(sd);
	if (ret) {
		WARN_ON(1);
		return ret;
	}

	ret = v4l2_device_register_subdev(&slice->shared->v4l2_dev, sd);
	if (ret) {
		WARN_ON(1);
		return ret;
	}

	return 0;
}

void vip_destroy_slice_subdev(struct vip_slice *slice)
{
	v4l2_device_unregister_subdev(&slice->sd);
	v4l2_subdev_cleanup(&slice->sd);
	media_entity_cleanup(&slice->sd.entity);
}
