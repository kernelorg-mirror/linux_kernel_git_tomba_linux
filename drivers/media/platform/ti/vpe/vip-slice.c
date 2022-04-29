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


static int cal_camerarx_sd_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static int cal_camerarx_sd_enum_mbus_code(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *sd_state,
					  struct v4l2_subdev_mbus_code_enum *code)
{
	return 0;
}

static int cal_camerarx_sd_enum_frame_size(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *sd_state,
					   struct v4l2_subdev_frame_size_enum *fse)
{
	return 0;
}


static int cal_camerarx_sd_set_fmt(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *format)
{
	return 0;
}

static int cal_camerarx_sd_init_cfg(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state)
{
	int i;

	struct v4l2_subdev_format format = {
		.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY
		: V4L2_SUBDEV_FORMAT_ACTIVE,
		//.pad = CAL_CAMERARX_PAD_SINK,
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
		return cal_camerarx_sd_set_fmt(sd, sd_state, &format);
	}
}

static const struct v4l2_subdev_video_ops cal_camerarx_video_ops = {
	.s_stream = cal_camerarx_sd_s_stream,
};

static const struct v4l2_subdev_pad_ops cal_camerarx_pad_ops = {
	.init_cfg = cal_camerarx_sd_init_cfg,
	.enum_mbus_code = cal_camerarx_sd_enum_mbus_code,
	.enum_frame_size = cal_camerarx_sd_enum_frame_size,
	.get_fmt = cal_camerarx_sd_get_fmt,
	.set_fmt = cal_camerarx_sd_set_fmt,
};

static const struct v4l2_subdev_ops cal_camerarx_subdev_ops = {
	.video = &cal_camerarx_video_ops,
	.pad = &cal_camerarx_pad_ops,
};

static struct media_entity_operations cal_camerarx_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

int vip_create_slice_subdev(struct vip_slice *slice)
{
	struct v4l2_subdev *sd = &slice->sd;
	int ret;

	v4l2_subdev_init(sd, &cal_camerarx_subdev_ops);
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	strncpy(sd->name, slice->name, sizeof(sd->name));
	sd->dev = &slice->shared->pdev->dev;

	slice->pads[0].flags = MEDIA_PAD_FL_SINK;
	slice->pads[1].flags = MEDIA_PAD_FL_SINK;
	slice->pads[2].flags = MEDIA_PAD_FL_SOURCE;
	slice->pads[3].flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.ops = &cal_camerarx_media_ops;
	ret = media_entity_pads_init(&sd->entity, ARRAY_SIZE(slice->pads),
				     slice->pads);
	if (ret) {
		WARN_ON(1);
		return -EINVAL;
	}

	ret = cal_camerarx_sd_init_cfg(sd, NULL);
	if (ret) {
		WARN_ON(1);
		return -EINVAL;
	}

	ret = v4l2_device_register_subdev(&slice->shared->v4l2_dev, sd);
	if (ret) {
		WARN_ON(1);
		return -EINVAL;
	}

	return 0;
}

void vip_destroy_slice_subdev(struct vip_slice *slice)
{
	v4l2_device_unregister_subdev(&slice->sd);
	media_entity_cleanup(&slice->sd.entity);
}
