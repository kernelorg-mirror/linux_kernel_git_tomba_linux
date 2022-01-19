/*
 * Xilinx Color Filter Array
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#include "xilinx-vip.h"

#define XCFA_BAYER_PHASE	0x100
#define XCFA_BAYER_PHASE_RGGB	0
#define XCFA_BAYER_PHASE_GRBG	1
#define XCFA_BAYER_PHASE_GBRG	2
#define XCFA_BAYER_PHASE_BGGR	3

/**
 * struct xcfa_device - Xilinx CFA device structure
 * @xvip: Xilinx Video IP device
 * @formats: V4L2 media bus formats
 * @default_formats: default V4L2 media bus formats
 */
struct xcfa_device {
	struct xvip_device xvip;

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];
};

static inline struct xcfa_device *to_cfa(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xcfa_device, xvip.subdev);
}

static int xcfa_get_bayer_phase(const unsigned int code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		return XCFA_BAYER_PHASE_RGGB;
	case MEDIA_BUS_FMT_SGRBG8_1X8:
		return XCFA_BAYER_PHASE_GRBG;
	case MEDIA_BUS_FMT_SGBRG8_1X8:
		return XCFA_BAYER_PHASE_GBRG;
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		return XCFA_BAYER_PHASE_BGGR;
	}

	return -EINVAL;
}

/* -----------------------------------------------------------------------------
 * xvip operations
 */

static int xcfa_enable_streams(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct xcfa_device *xcfa = to_cfa(sd);
	const unsigned int code = xcfa->formats[XVIP_PAD_SINK].code;
	u32 bayer_phase;

	/* This always returns the valid bayer phase value */
	bayer_phase = xcfa_get_bayer_phase(code);

	xvip_write(&xcfa->xvip, XCFA_BAYER_PHASE, bayer_phase);

	xvip_set_frame_size(&xcfa->xvip, &xcfa->formats[XVIP_PAD_SINK]);

	xvip_start(&xcfa->xvip);

	return 0;
}

static int xcfa_disable_streams(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state, u32 pad,
				u64 streams_mask)
{
	struct xcfa_device *xcfa = to_cfa(sd);

	xvip_stop(&xcfa->xvip);

	return 0;
}

static const struct xvip_device_ops xcfa_xvip_device_ops = {
	.enable_streams = xcfa_enable_streams,
	.disable_streams = xcfa_disable_streams,
};

/*
 * V4L2 Subdevice Pad Operations
 */

static struct v4l2_mbus_framefmt *
__xcfa_get_pad_format(struct xcfa_device *xcfa,
		      struct v4l2_subdev_state *sd_state,
		      unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&xcfa->xvip.subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &xcfa->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int xcfa_get_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xcfa_device *xcfa = to_cfa(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __xcfa_get_pad_format(xcfa, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int xcfa_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct xcfa_device *xcfa = to_cfa(subdev);
	struct v4l2_mbus_framefmt *format;
	int bayer_phase;

	format = __xcfa_get_pad_format(xcfa, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == XVIP_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	bayer_phase = xcfa_get_bayer_phase(fmt->format.code);
	if (bayer_phase >= 0)
		format->code = fmt->format.code;

	xvip_set_format_size(format, fmt);

	fmt->format = *format;

	/* Propagate the format to the source pad */
	format = __xcfa_get_pad_format(xcfa, sd_state, XVIP_PAD_SOURCE,
				       fmt->which);

	xvip_set_format_size(format, fmt);

	return 0;
}

/*
 * V4L2 Subdevice Operations
 */

static int xcfa_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct xcfa_device *xcfa = to_cfa(subdev);
	struct v4l2_mbus_framefmt *format;

	/* Initialize with default formats */
	format = v4l2_subdev_get_try_format(subdev, fh->state, XVIP_PAD_SINK);
	*format = xcfa->default_formats[XVIP_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, XVIP_PAD_SOURCE);
	*format = xcfa->default_formats[XVIP_PAD_SOURCE];

	return 0;
}

static int xcfa_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static struct v4l2_subdev_video_ops xcfa_video_ops = {
	.s_stream = xvip_s_stream,
};

static struct v4l2_subdev_pad_ops xcfa_pad_ops = {
	.enum_mbus_code		= xvip_enum_mbus_code,
	.enum_frame_size	= xvip_enum_frame_size,
	.get_fmt		= xcfa_get_format,
	.set_fmt		= xcfa_set_format,
	.enable_streams		= xvip_enable_streams,
	.disable_streams	= xvip_disable_streams,
};

static struct v4l2_subdev_ops xcfa_ops = {
	.video  = &xcfa_video_ops,
	.pad    = &xcfa_pad_ops,
};

static const struct v4l2_subdev_internal_ops xcfa_internal_ops = {
	.open	= xcfa_open,
	.close	= xcfa_close,
};

/*
 * Media Operations
 */

static const struct media_entity_operations xcfa_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * Power Management
 */

static int __maybe_unused xcfa_pm_suspend(struct device *dev)
{
	struct xcfa_device *xcfa = dev_get_drvdata(dev);

	xvip_suspend(&xcfa->xvip);

	return 0;
}

static int __maybe_unused xcfa_pm_resume(struct device *dev)
{
	struct xcfa_device *xcfa = dev_get_drvdata(dev);

	xvip_resume(&xcfa->xvip);

	return 0;
}

/*
 * Platform Device Driver
 */

static const struct xvip_device_info xcfa_info = {
	.has_axi_lite = true,
	.has_port_formats = true,
	.num_sinks = 1,
	.num_sources = 1,
};

static int xcfa_probe(struct platform_device *pdev)
{
	struct xcfa_device *xcfa;
	struct v4l2_subdev *subdev;
	struct v4l2_mbus_framefmt *default_format;
	int ret;

	xcfa = devm_kzalloc(&pdev->dev, sizeof(*xcfa), GFP_KERNEL);
	if (!xcfa)
		return -ENOMEM;

	xcfa->xvip.dev = &pdev->dev;
	xcfa->xvip.ops = &xcfa_xvip_device_ops;

	ret = xvip_device_init(&xcfa->xvip, &xcfa_info);
	if (ret < 0)
		return ret;

	/* Reset and initialize the core */
	xvip_reset(&xcfa->xvip);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xcfa->xvip.subdev;
	v4l2_subdev_init(subdev, &xcfa_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xcfa_internal_ops;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xcfa);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* Initialize default and active formats */
	default_format = &xcfa->default_formats[XVIP_PAD_SINK];
	default_format->code = xcfa->xvip.ports[XVIP_PAD_SINK].format->code;
	default_format->field = V4L2_FIELD_NONE;
	default_format->colorspace = V4L2_COLORSPACE_SRGB;
	xvip_get_frame_size(&xcfa->xvip, default_format);

	xcfa->formats[XVIP_PAD_SINK] = *default_format;

	default_format = &xcfa->default_formats[XVIP_PAD_SOURCE];
	*default_format = xcfa->default_formats[XVIP_PAD_SINK];
	default_format->code = xcfa->xvip.ports[XVIP_PAD_SOURCE].format->code;

	xcfa->formats[XVIP_PAD_SOURCE] = *default_format;

	subdev->entity.ops = &xcfa_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, xcfa->xvip.pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, xcfa);

	xvip_print_version(&xcfa->xvip);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&subdev->entity);
	xvip_device_cleanup(&xcfa->xvip);
	return ret;
}

static int xcfa_remove(struct platform_device *pdev)
{
	struct xcfa_device *xcfa = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xcfa->xvip.subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);

	xvip_device_cleanup(&xcfa->xvip);

	return 0;
}

static SIMPLE_DEV_PM_OPS(xcfa_pm_ops, xcfa_pm_suspend, xcfa_pm_resume);

static const struct of_device_id xcfa_of_id_table[] = {
	{ .compatible = "xlnx,v-cfa-7.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, xcfa_of_id_table);

static struct platform_driver xcfa_driver = {
	.driver			= {
		.name		= "xilinx-cfa",
		.pm		= &xcfa_pm_ops,
		.of_match_table	= xcfa_of_id_table,
	},
	.probe			= xcfa_probe,
	.remove			= xcfa_remove,
};

module_platform_driver(xcfa_driver);

MODULE_DESCRIPTION("Xilinx Color Filter Array Driver");
MODULE_LICENSE("GPL v2");
