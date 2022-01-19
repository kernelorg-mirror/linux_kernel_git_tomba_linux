/*
 * Xilinx Chroma Resampler
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
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "xilinx-vip.h"

#define XCRESAMPLE_ENCODING			0x100
#define XCRESAMPLE_ENCODING_FIELD		(1 << 7)
#define XCRESAMPLE_ENCODING_CHROMA		(1 << 8)

/**
 * struct xcresample_device - Xilinx CRESAMPLE device structure
 * @xvip: Xilinx Video IP device
 * @formats: V4L2 media bus formats at the sink and source pads
 * @default_formats: default V4L2 media bus formats
 * @ctrl_handler: control handler
 */
struct xcresample_device {
	struct xvip_device xvip;

	struct v4l2_mbus_framefmt formats[2];
	struct v4l2_mbus_framefmt default_formats[2];

	struct v4l2_ctrl_handler ctrl_handler;
};

static inline struct xcresample_device *to_cresample(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xcresample_device, xvip.subdev);
}

/* -----------------------------------------------------------------------------
 * xvip operations
 */

static int xcresample_enable_streams(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state, u32 pad,
				     u64 streams_mask)
{
	struct xcresample_device *xcresample = to_cresample(sd);

	xvip_set_frame_size(&xcresample->xvip,
			    &xcresample->formats[XVIP_PAD_SINK]);

	xvip_start(&xcresample->xvip);

	return 0;
}

static int xcresample_disable_streams(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state, u32 pad,
				      u64 streams_mask)
{
	struct xcresample_device *xcresample = to_cresample(sd);

	xvip_stop(&xcresample->xvip);

	return 0;
}

static const struct xvip_device_ops xcresample_xvip_device_ops = {
	.enable_streams = xcresample_enable_streams,
	.disable_streams = xcresample_disable_streams,
};

/*
 * V4L2 Subdevice Pad Operations
 */

static struct v4l2_mbus_framefmt *
__xcresample_get_pad_format(struct xcresample_device *xcresample,
			    struct v4l2_subdev_state *sd_state,
			    unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&xcresample->xvip.subdev,
						    sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &xcresample->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int xcresample_get_format(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct xcresample_device *xcresample = to_cresample(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __xcresample_get_pad_format(xcresample, sd_state, fmt->pad,
					     fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int xcresample_set_format(struct v4l2_subdev *subdev,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct xcresample_device *xcresample = to_cresample(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __xcresample_get_pad_format(xcresample, sd_state, fmt->pad,
					     fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == XVIP_PAD_SOURCE) {
		fmt->format = *format;
		return 0;
	}

	xvip_set_format_size(format, fmt);

	fmt->format = *format;

	/* Propagate the format to the source pad. */
	format = __xcresample_get_pad_format(xcresample, sd_state,
					     XVIP_PAD_SOURCE, fmt->which);

	xvip_set_format_size(format, fmt);

	return 0;
}

/*
 * V4L2 Subdevice Operations
 */

static int xcresample_open(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_fh *fh)
{
	struct xcresample_device *xcresample = to_cresample(subdev);
	struct v4l2_mbus_framefmt *format;

	/* Initialize with default formats */
	format = v4l2_subdev_get_try_format(subdev, fh->state, XVIP_PAD_SINK);
	*format = xcresample->default_formats[XVIP_PAD_SINK];

	format = v4l2_subdev_get_try_format(subdev, fh->state, XVIP_PAD_SOURCE);
	*format = xcresample->default_formats[XVIP_PAD_SOURCE];

	return 0;
}

static int xcresample_close(struct v4l2_subdev *subdev,
			    struct v4l2_subdev_fh *fh)
{
	return 0;
}

static int xcresample_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xcresample_device *xcresample =
		container_of(ctrl->handler, struct xcresample_device,
			     ctrl_handler);
	switch (ctrl->id) {
	case V4L2_CID_XILINX_CRESAMPLE_FIELD_PARITY:
		xvip_clr_or_set(&xcresample->xvip, XCRESAMPLE_ENCODING,
				XCRESAMPLE_ENCODING_FIELD, ctrl->val);
		return 0;
	case V4L2_CID_XILINX_CRESAMPLE_CHROMA_PARITY:
		xvip_clr_or_set(&xcresample->xvip, XCRESAMPLE_ENCODING,
				XCRESAMPLE_ENCODING_CHROMA, ctrl->val);
		return 0;
	}

	return -EINVAL;

}

static const struct v4l2_ctrl_ops xcresample_ctrl_ops = {
	.s_ctrl	= xcresample_s_ctrl,
};

static struct v4l2_subdev_video_ops xcresample_video_ops = {
	.s_stream = xvip_s_stream,
};

static struct v4l2_subdev_pad_ops xcresample_pad_ops = {
	.enum_mbus_code		= xvip_enum_mbus_code,
	.enum_frame_size	= xvip_enum_frame_size,
	.get_fmt		= xcresample_get_format,
	.set_fmt		= xcresample_set_format,
	.enable_streams		= xvip_enable_streams,
	.disable_streams	= xvip_disable_streams,
};

static struct v4l2_subdev_ops xcresample_ops = {
	.video  = &xcresample_video_ops,
	.pad    = &xcresample_pad_ops,
};

static const struct v4l2_subdev_internal_ops xcresample_internal_ops = {
	.open	= xcresample_open,
	.close	= xcresample_close,
};

/*
 * Control Configs
 */

static const char *const xcresample_parity_string[] = {
	"Even",
	"Odd",
};

static struct v4l2_ctrl_config xcresample_field = {
	.ops	= &xcresample_ctrl_ops,
	.id	= V4L2_CID_XILINX_CRESAMPLE_FIELD_PARITY,
	.name	= "Chroma Resampler: Encoding Field Parity",
	.type	= V4L2_CTRL_TYPE_MENU,
	.min	= 0,
	.max	= 1,
	.qmenu	= xcresample_parity_string,
};

static struct v4l2_ctrl_config xcresample_chroma = {
	.ops	= &xcresample_ctrl_ops,
	.id	= V4L2_CID_XILINX_CRESAMPLE_CHROMA_PARITY,
	.name	= "Chroma Resampler: Encoding Chroma Parity",
	.type	= V4L2_CTRL_TYPE_MENU,
	.min	= 0,
	.max	= 1,
	.qmenu	= xcresample_parity_string,
};

/*
 * Media Operations
 */

static const struct media_entity_operations xcresample_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * Power Management
 */

static int __maybe_unused xcresample_pm_suspend(struct device *dev)
{
	struct xcresample_device *xcresample = dev_get_drvdata(dev);

	xvip_suspend(&xcresample->xvip);

	return 0;
}

static int __maybe_unused xcresample_pm_resume(struct device *dev)
{
	struct xcresample_device *xcresample = dev_get_drvdata(dev);

	xvip_resume(&xcresample->xvip);

	return 0;
}

/*
 * Platform Device Driver
 */

static const struct xvip_device_info xcresample_info = {
	.has_axi_lite = true,
	.has_port_formats = true,
	.num_sinks = 1,
	.num_sources = 1,
};

static int xcresample_probe(struct platform_device *pdev)
{
	struct xcresample_device *xcresample;
	struct v4l2_subdev *subdev;
	struct v4l2_mbus_framefmt *default_format;
	int ret;

	xcresample = devm_kzalloc(&pdev->dev, sizeof(*xcresample), GFP_KERNEL);
	if (!xcresample)
		return -ENOMEM;

	xcresample->xvip.dev = &pdev->dev;
	xcresample->xvip.ops = &xcresample_xvip_device_ops;

	ret = xvip_device_init(&xcresample->xvip, &xcresample_info);
	if (ret < 0)
		return ret;

	/* Reset and initialize the core */
	xvip_reset(&xcresample->xvip);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xcresample->xvip.subdev;
	v4l2_subdev_init(subdev, &xcresample_ops);
	subdev->dev = &pdev->dev;
	subdev->internal_ops = &xcresample_internal_ops;
	strlcpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, xcresample);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* Initialize default and active formats */
	default_format = &xcresample->default_formats[XVIP_PAD_SINK];
	default_format->code = xcresample->xvip.ports[XVIP_PAD_SINK].format->code;
	default_format->field = V4L2_FIELD_NONE;
	default_format->colorspace = V4L2_COLORSPACE_SRGB;
	xvip_get_frame_size(&xcresample->xvip, default_format);

	xcresample->formats[XVIP_PAD_SINK] = *default_format;

	default_format = &xcresample->default_formats[XVIP_PAD_SOURCE];
	*default_format = xcresample->default_formats[XVIP_PAD_SINK];
	default_format->code = xcresample->xvip.ports[XVIP_PAD_SOURCE].format->code;

	xcresample->formats[XVIP_PAD_SOURCE] = *default_format;

	subdev->entity.ops = &xcresample_media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, xcresample->xvip.pads);
	if (ret < 0)
		goto error;

	v4l2_ctrl_handler_init(&xcresample->ctrl_handler, 2);
	xcresample_field.def =
		(xvip_read(&xcresample->xvip, XCRESAMPLE_ENCODING) &
		 XCRESAMPLE_ENCODING_FIELD) ? 1 : 0;
	v4l2_ctrl_new_custom(&xcresample->ctrl_handler, &xcresample_field,
			     NULL);
	xcresample_chroma.def =
		(xvip_read(&xcresample->xvip, XCRESAMPLE_ENCODING) &
		 XCRESAMPLE_ENCODING_CHROMA) ? 1 : 0;
	v4l2_ctrl_new_custom(&xcresample->ctrl_handler, &xcresample_chroma,
			     NULL);
	if (xcresample->ctrl_handler.error) {
		dev_err(&pdev->dev, "failed to add controls\n");
		ret = xcresample->ctrl_handler.error;
		goto error;
	}
	subdev->ctrl_handler = &xcresample->ctrl_handler;

	platform_set_drvdata(pdev, xcresample);

	xvip_print_version(&xcresample->xvip);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	v4l2_ctrl_handler_free(&xcresample->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
	xvip_device_cleanup(&xcresample->xvip);
	return ret;
}

static int xcresample_remove(struct platform_device *pdev)
{
	struct xcresample_device *xcresample = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xcresample->xvip.subdev;

	v4l2_async_unregister_subdev(subdev);
	v4l2_ctrl_handler_free(&xcresample->ctrl_handler);
	media_entity_cleanup(&subdev->entity);

	xvip_device_cleanup(&xcresample->xvip);

	return 0;
}

static SIMPLE_DEV_PM_OPS(xcresample_pm_ops, xcresample_pm_suspend,
			 xcresample_pm_resume);

static const struct of_device_id xcresample_of_id_table[] = {
	{ .compatible = "xlnx,v-cresample-4.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, xcresample_of_id_table);

static struct platform_driver xcresample_driver = {
	.driver			= {
		.name		= "xilinx-cresample",
		.pm		= &xcresample_pm_ops,
		.of_match_table	= xcresample_of_id_table,
	},
	.probe			= xcresample_probe,
	.remove			= xcresample_remove,
};

module_platform_driver(xcresample_driver);

MODULE_DESCRIPTION("Xilinx Chroma Resampler Driver");
MODULE_LICENSE("GPL v2");
