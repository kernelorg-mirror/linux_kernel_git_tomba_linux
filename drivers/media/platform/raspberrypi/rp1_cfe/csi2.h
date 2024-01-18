/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RP1 CSI-2 driver.
 *
 * Copyright (c) 2021-2024 Raspberry Pi Ltd.
 * Copyright (c) 2023-2024 Ideas on Board Oy
 */

#ifndef _RP1_CSI2_
#define _RP1_CSI2_

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/types.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "dphy.h"

#define CSI2_NUM_CHANNELS 4

#define CSI2_PAD_SINK 0
#define CSI2_PAD_FIRST_SOURCE 1
#define CSI2_PAD_NUM_SOURCES 4
#define CSI2_NUM_PADS 5

#define DISCARDS_TABLE_NUM_VCS 4

enum csi2_mode {
	CSI2_MODE_NORMAL = 0,
	CSI2_MODE_REMAP = 1,
	CSI2_MODE_COMPRESSED = 2,
	CSI2_MODE_FE_STREAMING = 3,
};

enum csi2_compression_mode {
	CSI2_COMPRESSION_DELTA = 1,
	CSI2_COMPRESSION_SIMPLE = 2,
	CSI2_COMPRESSION_COMBINED = 3,
};

enum discards_table_index {
	DISCARDS_TABLE_OVERFLOW = 0,
	DISCARDS_TABLE_LENGTH_LIMIT,
	DISCARDS_TABLE_UNMATCHED,
	DISCARDS_TABLE_INACTIVE,
	DISCARDS_TABLE_NUM_ENTRIES,
};

struct csi2_device {
	/* Parent V4l2 device */
	struct v4l2_device *v4l2_dev;

	void __iomem *base;

	struct dphy_data dphy;

	enum v4l2_mbus_type bus_type;
	unsigned int bus_flags;
	bool multipacket_line;

	struct {
		bool enable;

		enum csi2_mode mode;
		bool auto_arm;
		bool pack_bytes;

		struct {
			enum csi2_compression_mode mode;
			u32 shift;
			u32 offset;
		} compression;

	} channel_configs[CSI2_NUM_CHANNELS];

	struct media_pad pad[CSI2_NUM_PADS];
	struct v4l2_subdev sd;

	struct v4l2_subdev *source_sd;
	u64 source_stream_mask;

	/* lock for csi2 errors counters */
	spinlock_t errors_lock;
	u32 overflows;
	u32 discards_table[DISCARDS_TABLE_NUM_VCS][DISCARDS_TABLE_NUM_ENTRIES];
	u32 discards_dt_table[DISCARDS_TABLE_NUM_ENTRIES];
};

void csi2_isr(struct csi2_device *csi2, bool *sof, bool *eof);
void csi2_set_buffer(struct csi2_device *csi2, unsigned int channel,
		     dma_addr_t dmaaddr, unsigned int stride,
		     unsigned int size);
int csi2_init(struct csi2_device *csi2, struct dentry *debugfs);
void csi2_uninit(struct csi2_device *csi2);

int csi2_configure(struct csi2_device *csi2, struct v4l2_subdev_state *state);
int csi2_start_streaming(struct csi2_device *csi2,
			 struct v4l2_subdev_state *state);
void csi2_stop_streaming(struct csi2_device *csi2,
			 struct v4l2_subdev_state *state);

#endif
