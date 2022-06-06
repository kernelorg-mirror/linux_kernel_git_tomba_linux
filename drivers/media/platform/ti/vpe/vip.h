/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TI VIP capture driver
 *
 * Copyright (C) 2018 Texas Instruments Incorpated - http://www.ti.com/
 * David Griego, <dagriego@biglakesoftware.com>
 * Dale Farnsworth, <dale@farnsworth.org>
 * Nikhil Devshatwar, <nikhil.nd@ti.com>
 * Benoit Parrot, <bparrot@ti.com>
 */

#ifndef __TI_VIP_H
#define __TI_VIP_H

#include <linux/videodev2.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-rect.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-memops.h>
#include <media/media-device.h>

#include "csc.h"
#include "sc.h"
#include "vpdma.h"
#include "vpdma_priv.h"

#define VIP_SLICE1	0
#define VIP_SLICE2	1
#define VIP_NUM_SLICES	2

/*
 * Additionnal client identifiers used for VPDMA configuration descriptors
 */
#define VIP_SLICE1_CFD_SC_CLIENT	7
#define VIP_SLICE2_CFD_SC_CLIENT	8

#define VIP_PORTA	0
#define VIP_PORTB	1
#define VIP_NUM_PORTS	2

#define VIP_MAX_PLANES	2
#define	VIP_LUMA	0
#define VIP_CHROMA	1

#define VIP_CAP_STREAMS_PER_PORT	16
#define VIP_VBI_STREAMS_PER_PORT	16

#define VIP_MAX_SUBDEV			5

/*
 * Colorspace conversion unit can be in one of 3 modes:
 * NA  - Not Available on this port
 * Y2R - Needed for YUV to RGB on this port
 * R2Y - Needed for RGB to YUV on this port
 */
enum vip_csc_state {
	VIP_CSC_NA = 0,
	VIP_CSC_Y2R,
	VIP_CSC_R2Y,
};

/* buffer for one video frame */
struct vip_buffer {
	/* common v4l buffer stuff */
	struct vb2_v4l2_buffer	vb;
	struct list_head	list;
	bool			drop;
};

/*
 * struct vip_fmt - VIP media bus format information
 * @fourcc: V4L2 pixel format FCC identifier
 * @code: V4L2 media bus format code
 * @colorspace: V4L2 colorspace identifier
 * @coplanar: 1 if unpacked Luma and Chroma, 0 otherwise (packed/interleaved)
 * @vpdma_fmt: VPDMA data format per plane.
 * @finfo: Cache v4l2_format_info for associated fourcc
 */
struct vip_fmt {
	u32	fourcc;
	u32	code;
	u32	colorspace;
	u8	coplanar;
	const struct vpdma_data_format *vpdma_fmt[VIP_MAX_PLANES];
	const struct v4l2_format_info *finfo;
};

/*
 * The vip_clk_polarity structure contains the regmap, offset and bit field
 * definitions to control each port clock polarity.
 */
struct vip_clk_polarity {
	struct regmap	*rm_pol;
	u32		rm_offset;
	u32		rm_bit_field[4];
};

/*
 * The vip_shared structure contains data that is shared by both
 * the VIP1 and VIP2 slices.
 */
struct vip_shared {
	struct list_head	list;
	struct resource		*res;
	void __iomem		*base;
	struct platform_device	*pdev;
	struct vpdma_data	vpdma_data;
	struct vpdma_data	*vpdma;
	struct v4l2_device	v4l2_dev;
	struct vip_slice		*slices[VIP_NUM_SLICES];
	const char		*name;

	struct media_device	mdev;
};

/*
 * The vip_parser structures contains the memory mapped
 * info to access the parser registers.
 */
struct vip_parser {
	void __iomem		*base;
};

/*
 * There are two vip_slice structures, one for each vip slice: VIP1 & VIP2.
 */
struct vip_slice {
	struct vip_shared	*shared;
	struct resource		*res;
	struct vip_clk_polarity *pclk_pol;
	int			slice_id;
	int			num_ports;	/* count of open ports */
	struct mutex		mutex;
	spinlock_t		slock;

	int			irq;
	void __iomem		*base;

	struct vip_port		*ports[VIP_NUM_PORTS];

	char			name[16];
	/* parser data handle */
	struct vip_parser	*parser;
	/* scaler data handle */
	struct sc_data		*sc;
	/* scaler port assignation */
	int			sc_assigned;
	/* csc data handle */
	struct csc_data		*csc;
	/* csc port assignation */
	int			csc_assigned;

	struct v4l2_subdev	sd;
	struct media_pad	pads[4];
};

/*
 * There are two vip_port structures for each vip_slice, one for port A
 * and one for port B.
 */
struct vip_port {
	struct vip_slice		*slice;
	int			port_id;

	unsigned int		flags;
	struct v4l2_rect	c_rect;		/* crop rectangle */
	struct v4l2_mbus_framefmt mbus_framefmt;

	const char		*name;
	struct vip_fmt		*fmt;		/* current format info */
	int			num_streams;	/* count of open streams */
	struct vip_stream	*cap_streams[VIP_CAP_STREAMS_PER_PORT];

	struct v4l2_async_notifier notifier;
	struct v4l2_subdev	*remote_subdev;
	struct v4l2_fwnode_endpoint endpoint;
	/* have new shadow reg values */
	bool			load_mmrs;
	/* shadow reg addr/data block */
	struct vpdma_buf	mmr_adb;
	/* h coeff buffer */
	struct vpdma_buf	sc_coeff_h;
	/* v coeff buffer */
	struct vpdma_buf	sc_coeff_v;
	/* Show if scaler resource is available on this port */
	bool			scaler;
	/* Show the csc resource state on this port */
	enum vip_csc_state	csc;

};

/*
 * When handling multiplexed video, there can be multiple streams for each
 * port.  The vip_stream structure holds per-stream data.
 */
struct vip_stream {
	struct video_device	*vfd;
	struct vip_port		*port;
	int			stream_id;
	int			list_num;
	struct media_pad	pad;
	struct media_pipeline	pipe;
	char			name[16];
	struct work_struct	recovery_work;
	int			num_recovery;
	enum v4l2_field		field;		/* current field */
	unsigned int		sequence;	/* current frame/field seq */
	enum v4l2_field		sup_field;	/* supported field value */
	unsigned int		width;		/* frame width */
	unsigned int		height;		/* frame height */
	unsigned int		bytesperline;	/* bytes per line in memory */
	unsigned int		sizeimage;	/* image size in memory */
	struct list_head	vidq;		/* incoming vip_bufs queue */
	struct list_head	dropq;		/* drop vip_bufs queue */
	struct list_head	post_bufs;	/* vip_bufs to be DMAed */
	/* Maintain a list of used channels - Needed for VPDMA cleanup */
	int			vpdma_channels[VPDMA_MAX_CHANNELS];
	int			vpdma_channels_to_abort[VPDMA_MAX_CHANNELS];
	struct vpdma_desc_list	desc_list;	/* DMA descriptor list */
	struct vpdma_dtd	*write_desc;
	/* next unused desc_list addr */
	void			*desc_next;
	struct vb2_queue	vb_vidq;
};

// XXX
/*
 vip_port->fmt = DMA output format
 port->mbuf_framefmt = sensor mbus fmt
 stream->width & co = sensor mbus fmt
 */




/*
 * VIP Enumerations
 */
enum data_path_select {
	ALL_FIELDS_DATA_SELECT = 0,
	VIP_CSC_SRC_DATA_SELECT,
	VIP_SC_SRC_DATA_SELECT,
	VIP_RGB_SRC_DATA_SELECT,
	VIP_RGB_OUT_LO_DATA_SELECT,
	VIP_RGB_OUT_HI_DATA_SELECT,
	VIP_CHR_DS_1_SRC_DATA_SELECT,
	VIP_CHR_DS_2_SRC_DATA_SELECT,
	VIP_MULTI_CHANNEL_DATA_SELECT,
	VIP_CHR_DS_1_DATA_BYPASS,
	VIP_CHR_DS_2_DATA_BYPASS,
};


enum data_interface_modes {
	SINGLE_24B_INTERFACE = 0,
	SINGLE_16B_INTERFACE = 1,
	DUAL_8B_INTERFACE = 2,
};

enum sync_types {
	EMBEDDED_SYNC_SINGLE_YUV422 = 0,
	EMBEDDED_SYNC_2X_MULTIPLEXED_YUV422 = 1,
	EMBEDDED_SYNC_4X_MULTIPLEXED_YUV422 = 2,
	EMBEDDED_SYNC_LINE_MULTIPLEXED_YUV422 = 3,
	DISCRETE_SYNC_SINGLE_YUV422 = 4,
	EMBEDDED_SYNC_SINGLE_RGB_OR_YUV444 = 5,
	DISCRETE_SYNC_SINGLE_RGB_24B = 10,
};

#define VIP_NOT_ASSIGNED	-1

u32 vip_port_to_slice_sink_pad(struct vip_port *port);
u32 vip_port_to_slice_source_pad(struct vip_port *port);

/*
 * Register offsets and field selectors
 */

/* VIP TOP registers */

#define VIP_PID_FUNC			0xf02

#define VIP_PID				0x0000
#define VIP_PID_MINOR_MASK              0x3f
#define VIP_PID_MINOR_SHIFT             0
#define VIP_PID_CUSTOM_MASK             0x03
#define VIP_PID_CUSTOM_SHIFT            6
#define VIP_PID_MAJOR_MASK              0x07
#define VIP_PID_MAJOR_SHIFT             8
#define VIP_PID_RTL_MASK                0x1f
#define VIP_PID_RTL_SHIFT               11
#define VIP_PID_FUNC_MASK               0xfff
#define VIP_PID_FUNC_SHIFT              16
#define VIP_PID_SCHEME_MASK             0x03
#define VIP_PID_SCHEME_SHIFT            30

#define VIP_SYSCONFIG			0x0010
#define VIP_SYSCONFIG_IDLE_MASK         0x03
#define VIP_SYSCONFIG_IDLE_SHIFT        2
#define VIP_SYSCONFIG_STANDBY_MASK      0x03
#define VIP_SYSCONFIG_STANDBY_SHIFT     4
#define VIP_FORCE_IDLE_MODE             0
#define VIP_NO_IDLE_MODE                1
#define VIP_SMART_IDLE_MODE             2
#define VIP_SMART_IDLE_WAKEUP_MODE      3
#define VIP_FORCE_STANDBY_MODE          0
#define VIP_NO_STANDBY_MODE             1
#define VIP_SMART_STANDBY_MODE          2
#define VIP_SMART_STANDBY_WAKEUP_MODE   3

#define VIP_INTC_INTRx_STATUS_RAWy(x, y)	(0x0020 + 0x20 * (x) + 4 * (y))
#define VIP_INTC_INTRx_STATUS_ENAy(x, y)	(0x0028 + 0x20 * (x) + 4 * (y))
#define VIP_INTC_INTRx_ENA_SETy(x, y)		(0x0030 + 0x20 * (x) + 4 * (y))
#define VIP_INTC_INTRx_ENA_CLRy(x, y)		(0x0038 + 0x20 * (x) + 4 * (y))

#define VIP_INT0_LIST0_COMPLETE         BIT(0)
#define VIP_INT0_LIST0_NOTIFY           BIT(1)
#define VIP_INT0_LIST1_COMPLETE         BIT(2)
#define VIP_INT0_LIST1_NOTIFY           BIT(3)
#define VIP_INT0_LIST2_COMPLETE         BIT(4)
#define VIP_INT0_LIST2_NOTIFY           BIT(5)
#define VIP_INT0_LIST3_COMPLETE         BIT(6)
#define VIP_INT0_LIST3_NOTIFY           BIT(7)
#define VIP_INT0_LIST4_COMPLETE         BIT(8)
#define VIP_INT0_LIST4_NOTIFY           BIT(9)
#define VIP_INT0_LIST5_COMPLETE         BIT(10)
#define VIP_INT0_LIST5_NOTIFY           BIT(11)
#define VIP_INT0_LIST6_COMPLETE         BIT(12)
#define VIP_INT0_LIST6_NOTIFY           BIT(13)
#define VIP_INT0_LIST7_COMPLETE         BIT(14)
#define VIP_INT0_LIST7_NOTIFY           BIT(15)
#define VIP_INT0_DESCRIPTOR             BIT(16)
#define VIP_VIP1_PARSER_INT		BIT(20)
#define VIP_VIP2_PARSER_INT		BIT(21)

#define VIP_INT0_CHANNEL_GROUP0		BIT(0)
#define VIP_INT0_CHANNEL_GROUP1		BIT(1)
#define VIP_INT0_CHANNEL_GROUP2		BIT(2)
#define VIP_INT0_CHANNEL_GROUP3		BIT(3)
#define VIP_INT0_CHANNEL_GROUP4		BIT(4)
#define VIP_INT0_CHANNEL_GROUP5		BIT(5)
#define VIP_INT0_CLIENT			BIT(7)
#define VIP_VIP1_DS1_UV_ERROR_INT	BIT(22)
#define VIP_VIP1_DS2_UV_ERROR_INT	BIT(23)
#define VIP_VIP2_DS1_UV_ERROR_INT	BIT(24)
#define VIP_VIP2_DS2_UV_ERROR_INT	BIT(25)

#define VIP_INTC_EOI			0x00a0

#define VIP_CLKC_CLKEN			0x0100
#define VIP_VPDMA_CLK_ENABLE		BIT(0)
#define VIP_VIP1_DATA_PATH_CLK_ENABLE	BIT(16)
#define VIP_VIP2_DATA_PATH_CLK_ENABLE	BIT(17)

#define VIP_CLKC_RST			0x0104
#define VIP_VPDMA_RESET			BIT(0)
#define VIP_VPDMA_CLK_RESET_MASK	0x1
#define VIP_VPDMA_CLK_RESET_SHIFT	0
#define VIP_DATA_PATH_CLK_RESET_MASK	0x1
#define VIP_VIP1_DATA_PATH_RESET_SHIFT	16
#define VIP_VIP2_DATA_PATH_RESET_SHIFT	17
#define VIP_VIP1_DATA_PATH_RESET	BIT(16)
#define VIP_VIP2_DATA_PATH_RESET	BIT(17)
#define VIP_VIP1_PARSER_RESET		BIT(18)
#define VIP_VIP2_PARSER_RESET		BIT(19)
#define VIP_VIP1_CSC_RESET		BIT(20)
#define VIP_VIP2_CSC_RESET		BIT(21)
#define VIP_VIP1_SC_RESET		BIT(22)
#define VIP_VIP2_SC_RESET		BIT(23)
#define VIP_VIP1_DS1_RESET		BIT(25)
#define VIP_VIP2_DS1_RESET		BIT(26)
#define VIP_VIP1_DS2_RESET		BIT(27)
#define VIP_VIP2_DS2_RESET		BIT(28)
#define VIP_MAIN_RESET			BIT(31)

#define VIP_CLKC_DPS			0x0108
#define VIP_CLKC_VIP_DPS(slice)		(0x010c + 4 * (slice))
#define VIP_CSC_SRC_SELECT_MASK		0x07
#define VIP_CSC_SRC_SELECT_SHFT		0
#define VIP_SC_SRC_SELECT_MASK		0x07
#define VIP_SC_SRC_SELECT_SHFT		3
#define VIP_RGB_SRC_SELECT		BIT(6)
#define VIP_RGB_OUT_LO_SRC_SELECT	BIT(7)
#define VIP_RGB_OUT_HI_SRC_SELECT	BIT(8)
#define VIP_DS1_SRC_SELECT_MASK		0x07
#define VIP_DS1_SRC_SELECT_SHFT		9
#define VIP_DS2_SRC_SELECT_MASK		0x07
#define VIP_DS2_SRC_SELECT_SHFT		12
#define VIP_MULTI_CHANNEL_SELECT	BIT(15)
#define VIP_DS1_BYPASS			BIT(16)
#define VIP_DS2_BYPASS			BIT(17)
#define VIP_TESTPORT_B_SELECT		BIT(26)
#define VIP_TESTPORT_A_SELECT		BIT(27)
#define VIP_DATAPATH_SELECT_MASK	0x0f
#define VIP_DATAPATH_SELECT_SHFT	28

/* VIP PARSER registers */

#define VIP_PARSER_MAIN_CFG		0x0000
#define VIP_DATA_INTERFACE_MODE_MASK	0x03
#define VIP_DATA_INTERFACE_MODE_SHFT	0
#define VIP_CLIP_BLANK			BIT(4)
#define VIP_CLIP_ACTIVE			BIT(5)

#define VIP_PARSER_PORT(p)		(0x0004 + (p) * 8)
#define VIP_SYNC_TYPE_MASK		0x0f
#define VIP_SYNC_TYPE_SHFT		0
#define VIP_CTRL_CHANNEL_SEL_MASK	0x03
#define VIP_CTRL_CHANNEL_SEL_SHFT	4
#define VIP_ASYNC_FIFO_WR		BIT(6)
#define VIP_ASYNC_FIFO_RD		BIT(7)
#define VIP_PORT_ENABLE			BIT(8)
#define VIP_FID_POLARITY		BIT(9)
#define VIP_PIXCLK_EDGE_POLARITY	BIT(10)
#define VIP_HSYNC_POLARITY		BIT(11)
#define VIP_VSYNC_POLARITY		BIT(12)
#define VIP_ACTVID_POLARITY		BIT(13)
#define VIP_FID_DETECT_MODE		BIT(14)
#define VIP_USE_ACTVID_HSYNC_ONLY	BIT(15)
#define VIP_FID_SKEW_PRECOUNT_MASK	0x3f
#define VIP_FID_SKEW_PRECOUNT_SHFT	16
#define VIP_DISCRETE_BASIC_MODE		BIT(22)
#define VIP_SW_RESET			BIT(23)
#define VIP_FID_SKEW_POSTCOUNT_MASK	0x3f
#define VIP_FID_SKEW_POSTCOUNT_SHFT	24
#define VIP_ANALYZER_2X4X_SRCNUM_POS	BIT(30)
#define VIP_ANALYZER_FVH_ERR_COR_EN	BIT(31)

#define VIP_PARSER_EXTRA_PORT(p)	(0x0008 + (p) * 8)
#define VIP_SRC0_NUMLINES_MASK		0x0fff
#define VIP_SRC0_NUMLINES_SHFT		0
#define VIP_ANC_CHAN_SEL_8B_MASK	0x03
#define VIP_ANC_CHAN_SEL_8B_SHFT	13
#define VIP_SRC0_NUMPIX_MASK		0x0fff
#define VIP_SRC0_NUMPIX_SHFT		16
#define VIP_REPACK_SEL_MASK		0x07
#define VIP_REPACK_SEL_SHFT		28

#define VIP_PARSER_FIQ_MASK		0x0014
#define VIP_PARSER_FIQ_CLR		0x0018
#define VIP_PARSER_FIQ_STATUS		0x001c
#define VIP_PORTA_VDET			BIT(0)
#define VIP_PORTB_VDET			BIT(1)
#define VIP_PORTA_ASYNC_FIFO_OF		BIT(2)
#define VIP_PORTB_ASYNC_FIFO_OF		BIT(3)
#define VIP_PORTA_OUTPUT_FIFO_YUV	BIT(4)
#define VIP_PORTA_OUTPUT_FIFO_ANC	BIT(6)
#define VIP_PORTB_OUTPUT_FIFO_YUV	BIT(7)
#define VIP_PORTB_OUTPUT_FIFO_ANC	BIT(9)
#define VIP_PORTA_CONN			BIT(10)
#define VIP_PORTA_DISCONN		BIT(11)
#define VIP_PORTB_CONN			BIT(12)
#define VIP_PORTB_DISCONN		BIT(13)
#define VIP_PORTA_SRC0_SIZE		BIT(14)
#define VIP_PORTB_SRC0_SIZE		BIT(15)
#define VIP_PORTA_YUV_PROTO_VIOLATION	BIT(16)
#define VIP_PORTA_ANC_PROTO_VIOLATION	BIT(17)
#define VIP_PORTB_YUV_PROTO_VIOLATION	BIT(18)
#define VIP_PORTB_ANC_PROTO_VIOLATION	BIT(19)
#define VIP_PORTA_CFG_DISABLE_COMPLETE	BIT(20)
#define VIP_PORTB_CFG_DISABLE_COMPLETE	BIT(21)

#define VIP_PARSER_PORTA_SOURCE_FID	0x0020
#define VIP_PARSER_PORTA_ENCODER_FID	0x0024
#define VIP_PARSER_PORTB_SOURCE_FID	0x0028
#define VIP_PARSER_PORTB_ENCODER_FID	0x002c

#define VIP_PARSER_PORTA_SRC0_SIZE	0x0030
#define VIP_PARSER_PORTB_SRC0_SIZE	0x0070
#define VIP_SOURCE_HEIGHT_MASK		0x0fff
#define VIP_SOURCE_HEIGHT_SHFT		0
#define VIP_SOURCE_WIDTH_MASK		0x0fff
#define VIP_SOURCE_WIDTH_SHFT		16

#define VIP_PARSER_PORTA_VDET_VEC	0x00b0
#define VIP_PARSER_PORTB_VDET_VEC	0x00b4

#define VIP_PARSER_ANC_CROP_H_PORT(p)	(0x00b8 + (p) * 0x10)
#define VIP_ANC_SKIP_NUMPIX_MASK	0x0fff
#define VIP_ANC_SKIP_NUMPIX_SHFT	0
#define VIP_ANC_BYPASS			BIT(15)
#define VIP_ANC_USE_NUMPIX_MASK		0x0fff
#define VIP_ANC_USE_NUMPIX_SHFT		16
#define VIP_ANC_TARGET_SRCNUM_MASK	0x0f
#define VIP_ANC_TARGET_SRCNUM_SHFT	28

#define VIP_PARSER_ANC_CROP_V_PORT(p)	(0x00bc + (p) * 0x10)
#define VIP_ANC_SKIP_NUMLINES_MASK	0x0fff
#define VIP_ANC_SKIP_NUMLINES_SHFT	0
#define VIP_ANC_USE_NUMLINES_MASK	0x0fff
#define VIP_ANC_USE_NUMLINES_SHFT	16

#define VIP_PARSER_CROP_H_PORT(p)	(0x00c0 + (p) * 0x10)
#define VIP_ACT_SKIP_NUMPIX_MASK	0x0fff
#define VIP_ACT_SKIP_NUMPIX_SHFT	0
#define VIP_ACT_BYPASS			BIT(15)
#define VIP_ACT_USE_NUMPIX_MASK		0x0fff
#define VIP_ACT_USE_NUMPIX_SHFT		16
#define VIP_ACT_TARGET_SRCNUM_MASK	0x0f
#define VIP_ACT_TARGET_SRCNUM_SHFT	28

#define VIP_PARSER_CROP_V_PORT(p)	(0x00c4 + (p) * 0x10)
#define VIP_ACT_SKIP_NUMLINES_MASK	0x0fff
#define VIP_ACT_SKIP_NUMLINES_SHFT	0
#define VIP_ACT_USE_NUMLINES_MASK	0x0fff
#define VIP_ACT_USE_NUMLINES_SHFT	16

#define VIP_PARSER_STOP_IMM_PORT(p)	(0x00d8 + (p) * 0x4)
#define VIP_ANC_SRCNUM_STOP_IMM_SHFT	0
#define VIP_YUV_SRCNUM_STOP_IMM_SHFT	16


void vip_destroy_slice_subdev(struct vip_slice *slice);
int vip_create_slice_subdev(struct vip_slice *slice);


#endif /* __TI_VIP_H */
