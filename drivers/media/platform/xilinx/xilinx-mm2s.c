// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Video MM2S DMA Subdevice
 *
 * Copyright (C) 2026 Ideas on Board
 */

#include <linux/of.h>
#include <linux/property.h>

#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#include "xilinx-dma.h"
#include "xilinx-mm2s.h"
#include "xilinx-vipp.h"

#define XVIP_MM2S_MIN_WIDTH		1U
#define XVIP_MM2S_MAX_WIDTH		65535U
#define XVIP_MM2S_MIN_HEIGHT		1U
#define XVIP_MM2S_MAX_HEIGHT		8191U

static inline struct xvip_mm2s *to_xvip_mm2s(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xvip_mm2s, subdev);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int xvip_mm2s_enum_mbus_code(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	/*
	 * Both pads expose the media bus codes supported by the DMA engine.
	 * The pipeline is what restricts them further, through link
	 * validation.
	 */
	return xvip_dma_enum_mbus_code(code->index, &code->code);
}

static int xvip_mm2s_enum_frame_size(struct v4l2_subdev *subdev,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	const struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(state, fse->pad);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	/* The source pad size is identical to the sink pad size. */
	if (fse->pad == XVIP_MM2S_PAD_SOURCE) {
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	} else {
		fse->min_width = XVIP_MM2S_MIN_WIDTH;
		fse->max_width = XVIP_MM2S_MAX_WIDTH;
		fse->min_height = XVIP_MM2S_MIN_HEIGHT;
		fse->max_height = XVIP_MM2S_MAX_HEIGHT;
	}

	return 0;
}

static int xvip_mm2s_set_format(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *format;

	/* The source pad format is always identical to the sink pad format. */
	if (fmt->pad == XVIP_MM2S_PAD_SOURCE) {
		fmt->format = *v4l2_subdev_state_get_format(state, fmt->pad);
		return 0;
	}

	format = v4l2_subdev_state_get_format(state, XVIP_MM2S_PAD_SINK);

	/*
	 * The DMA engine transfers the frame verbatim, so any media bus code
	 * is accepted here. Whether the code is one the DMA engine can produce
	 * is checked by the video node's .link_validate() operation, which
	 * compares this format against the active pixel format.
	 */
	format->code = fmt->format.code;
	format->width = clamp(fmt->format.width, XVIP_MM2S_MIN_WIDTH,
			      XVIP_MM2S_MAX_WIDTH);
	format->height = clamp(fmt->format.height, XVIP_MM2S_MIN_HEIGHT,
			       XVIP_MM2S_MAX_HEIGHT);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = fmt->format.colorspace;
	format->ycbcr_enc = fmt->format.ycbcr_enc;
	format->quantization = fmt->format.quantization;
	format->xfer_func = fmt->format.xfer_func;

	fmt->format = *format;

	/* Propagate the format to the source pad. */
	*v4l2_subdev_state_get_format(state, XVIP_MM2S_PAD_SOURCE) = *format;

	return 0;
}

/*
 * The DMA engine is started and stopped by videobuf2 through the video node,
 * there is nothing to do here. The operations are still needed, as the
 * downstream IP core propagates the stream state to its remote subdev, which
 * is this subdevice.
 */
static int xvip_mm2s_enable_streams(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_state *state,
				    u32 pad, u64 streams_mask)
{
	return 0;
}

static int xvip_mm2s_disable_streams(struct v4l2_subdev *subdev,
				     struct v4l2_subdev_state *state,
				     u32 pad, u64 streams_mask)
{
	return 0;
}

static int xvip_mm2s_init_state(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt format;

	/* Default to the same format as the DMA engine's video node. */
	xvip_dma_default_format(&format);

	*v4l2_subdev_state_get_format(state, XVIP_MM2S_PAD_SINK) = format;
	*v4l2_subdev_state_get_format(state, XVIP_MM2S_PAD_SOURCE) = format;

	return 0;
}

static const struct v4l2_subdev_pad_ops xvip_mm2s_pad_ops = {
	.enum_mbus_code		= xvip_mm2s_enum_mbus_code,
	.enum_frame_size	= xvip_mm2s_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= xvip_mm2s_set_format,
	.enable_streams		= xvip_mm2s_enable_streams,
	.disable_streams	= xvip_mm2s_disable_streams,
};

static const struct v4l2_subdev_ops xvip_mm2s_ops = {
	.pad = &xvip_mm2s_pad_ops,
};

static const struct v4l2_subdev_internal_ops xvip_mm2s_internal_ops = {
	.init_state = xvip_mm2s_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static int xvip_mm2s_get_fwnode_pad(struct media_entity *entity,
				    struct fwnode_endpoint *endpoint)
{
	struct v4l2_subdev *subdev = media_entity_to_v4l2_subdev(entity);
	struct xvip_mm2s *mm2s = to_xvip_mm2s(subdev);

	/*
	 * All the MM2S subdevices of a composite device share the composite
	 * device's fwnode, as the DMA engines have no device node of their
	 * own. Claim the source pad for the endpoints of our own port only.
	 */
	if (endpoint->port != mm2s->port)
		return -ENXIO;

	return XVIP_MM2S_PAD_SOURCE;
}

static const struct media_entity_operations xvip_mm2s_media_ops = {
	.get_fwnode_pad = xvip_mm2s_get_fwnode_pad,
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * Init and Cleanup
 */

int xvip_mm2s_init(struct xvip_composite_device *xdev, struct xvip_mm2s *mm2s,
		   unsigned int port)
{
	struct v4l2_subdev *subdev = &mm2s->subdev;
	int ret;

	mm2s->xdev = xdev;
	mm2s->port = port;

	v4l2_subdev_init(subdev, &xvip_mm2s_ops);
	subdev->dev = xdev->dev;
	subdev->internal_ops = &xvip_mm2s_internal_ops;
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xvip_mm2s_media_ops;
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	snprintf(subdev->name, sizeof(subdev->name), "%pOFn mm2s %u",
		 xdev->dev->of_node, port);

	/*
	 * The DMA engine has no device node of its own, so use the composite
	 * device's node. Endpoint-based async matching keeps the MM2S
	 * subdevices of the composite device distinct.
	 */
	subdev->fwnode = dev_fwnode(xdev->dev);

	mm2s->pads[XVIP_MM2S_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	mm2s->pads[XVIP_MM2S_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&subdev->entity, XVIP_MM2S_NUM_PADS,
				     mm2s->pads);
	if (ret < 0)
		return ret;

	/*
	 * Claim the port's endpoint for async matching. The endpoint list
	 * takes precedence over the fwnode in match_fwnode(), and is what
	 * tells the MM2S subdevices of a composite device apart.
	 *
	 * v4l2_async_subdev_endpoint_add() doesn't take a reference to the
	 * fwnode, keep ours until cleanup.
	 */
	mm2s->ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(xdev->dev), port,
						   0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!mm2s->ep) {
		dev_err(xdev->dev, "no endpoint found for port %u\n", port);
		ret = -EINVAL;
		goto error_entity_cleanup;
	}

	ret = v4l2_async_subdev_endpoint_add(subdev, mm2s->ep);
	if (ret < 0)
		goto error_fwnode_put;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_subdev_cleanup;

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0)
		goto error_subdev_cleanup;

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(subdev);
error_fwnode_put:
	fwnode_handle_put(mm2s->ep);
error_entity_cleanup:
	media_entity_cleanup(&subdev->entity);
	return ret;
}

void xvip_mm2s_cleanup(struct xvip_mm2s *mm2s)
{
	v4l2_async_unregister_subdev(&mm2s->subdev);
	v4l2_subdev_cleanup(&mm2s->subdev);
	fwnode_handle_put(mm2s->ep);
	media_entity_cleanup(&mm2s->subdev.entity);
}
