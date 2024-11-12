// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stringify.h>

#include <media/mipi-csi2.h>

#include <video/videomode.h>

#include <uapi/linux/media-bus-format.h>

#include "max_serdes.h"

int max_get_fd_stream_entry(struct v4l2_subdev *sd,
			    unsigned int pad, unsigned int stream,
			    struct v4l2_mbus_frame_desc_entry *entry)
{
	struct v4l2_mbus_frame_desc fd;
	unsigned int i;
	int ret;

	ret = v4l2_subdev_call(sd, pad, get_frame_desc, pad, &fd);
	if (ret)
		return ret;

	if (fd.type != V4L2_MBUS_FRAME_DESC_TYPE_CSI2)
		return -EOPNOTSUPP;

	for (i = 0; i < fd.num_entries; i++) {
		if (fd.entry[i].stream == stream) {
			*entry = fd.entry[i];
			return 0;
		}
	}

	return -ENOENT;
}
EXPORT_SYMBOL(max_get_fd_stream_entry);

static const struct videomode max_serdes_tpg_videomodes[] = {
	{
		.pixelclock = 25000000,

		.hactive = 640,
		.hfront_porch = 10,
		.hsync_len = 96,
		.hback_porch = 40,

		.vactive = 480,
		.vfront_porch = 2,
		.vsync_len = 24,
		.vback_porch = 24,
	}, {
		.pixelclock = 75000000,

		.hactive = 1920,
		.hfront_porch = 88,
		.hsync_len = 44,
		.hback_porch = 148,

		.vactive = 1080,
		.vfront_porch = 4,
		.vsync_len = 16,
		.vback_porch = 36,
	},
};

const struct videomode *max_serdes_find_tpg_videomode(u32 width, u32 height)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(max_serdes_tpg_videomodes); ++i) {
		if (max_serdes_tpg_videomodes[i].hactive == width &&
		    max_serdes_tpg_videomodes[i].vactive == height)
			return &max_serdes_tpg_videomodes[i];
	}

	return NULL;
}
EXPORT_SYMBOL(max_serdes_find_tpg_videomode);

MODULE_DESCRIPTION("Maxim GMSL2 Serializer/Deserializer Driver");
MODULE_AUTHOR("Cosmin Tanislav <cosmin.tanislav@analog.com>");
MODULE_LICENSE("GPL");
