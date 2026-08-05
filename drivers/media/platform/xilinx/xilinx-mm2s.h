/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx Video MM2S DMA Subdevice
 *
 * Copyright (C) 2026 Ideas on Board
 */

#ifndef __XILINX_MM2S_H__
#define __XILINX_MM2S_H__

#include <media/media-entity.h>
#include <media/v4l2-subdev.h>

struct xvip_composite_device;

#define XVIP_MM2S_PAD_SINK	0
#define XVIP_MM2S_PAD_SOURCE	1
#define XVIP_MM2S_NUM_PADS	2

/**
 * struct xvip_mm2s - MM2S DMA engine subdevice
 * @subdev: V4L2 subdevice
 * @pads: media pads, sink for the DMA video node, source for the IP core
 * @xdev: composite device the MM2S DMA engine belongs to
 * @port: composite device DT node port number for the DMA engine
 * @ep: the port's endpoint, claimed for async matching. A reference is held
 *	for the subdevice's lifetime, as v4l2_async_subdev_endpoint_add() does
 *	not take one.
 *
 * MM2S DMA engines feed a video IP core from memory. The DMA engine itself is
 * exposed to userspace as a video output node owned by the composite device,
 * but the IP core drivers connect to subdevs, not to video nodes. This
 * subdevice represents the MM2S DMA engine in the media graph, so that IP core
 * drivers can treat an MM2S source exactly like any other subdev source.
 *
 * The subdevice is a pass-through: it carries no hardware state, its source pad
 * format mirrors its sink pad format, and its stream operations are no-ops as
 * the DMA engine is started by videobuf2 through the video node.
 */
struct xvip_mm2s {
	struct v4l2_subdev subdev;
	struct media_pad pads[XVIP_MM2S_NUM_PADS];

	struct xvip_composite_device *xdev;
	unsigned int port;
	struct fwnode_handle *ep;
};

int xvip_mm2s_init(struct xvip_composite_device *xdev, struct xvip_mm2s *mm2s,
		   unsigned int port);
void xvip_mm2s_cleanup(struct xvip_mm2s *mm2s);

#endif /* __XILINX_MM2S_H__ */
