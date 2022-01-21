// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Xilinx MIPI CSI-2 Rx Subsystem
 *
 * Copyright (C) 2016 - 2020 Xilinx, Inc.
 *
 * Contacts: Vishal Sagar <vishal.sagar@xilinx.com>
 *
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/v4l2-subdev.h>

#include <media/media-entity.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "xilinx-vip.h"

/* Register register map */
#define XCSI_CCR_OFFSET		0x00
#define XCSI_CCR_SOFTRESET	BIT(1)
#define XCSI_CCR_ENABLE		BIT(0)

#define XCSI_PCR_OFFSET		0x04
#define XCSI_PCR_MAXLANES_MASK	GENMASK(4, 3)
#define XCSI_PCR_ACTLANES_MASK	GENMASK(1, 0)

#define XCSI_CSR_OFFSET		0x10
#define XCSI_CSR_PKTCNT		GENMASK(31, 16)
#define XCSI_CSR_SPFIFOFULL	BIT(3)
#define XCSI_CSR_SPFIFONE	BIT(2)
#define XCSI_CSR_SLBF		BIT(1)
#define XCSI_CSR_RIPCD		BIT(0)

#define XCSI_GIER_OFFSET	0x20
#define XCSI_GIER_GIE		BIT(0)

#define XCSI_ISR_OFFSET		0x24
#define XCSI_IER_OFFSET		0x28

#define XCSI_ISR_FR		BIT(31)
#define XCSI_ISR_VCXFE		BIT(30)
#define XCSI_ISR_YUV420		BIT(28)
#define XCSI_ISR_WCC		BIT(22)
#define XCSI_ISR_ILC		BIT(21)
#define XCSI_ISR_SPFIFOF	BIT(20)
#define XCSI_ISR_SPFIFONE	BIT(19)
#define XCSI_ISR_SLBF		BIT(18)
#define XCSI_ISR_STOP		BIT(17)
#define XCSI_ISR_SOTERR		BIT(13)
#define XCSI_ISR_SOTSYNCERR	BIT(12)
#define XCSI_ISR_ECC2BERR	BIT(11)
#define XCSI_ISR_ECC1BERR	BIT(10)
#define XCSI_ISR_CRCERR		BIT(9)
#define XCSI_ISR_DATAIDERR	BIT(8)
#define XCSI_ISR_VC3FSYNCERR	BIT(7)
#define XCSI_ISR_VC3FLVLERR	BIT(6)
#define XCSI_ISR_VC2FSYNCERR	BIT(5)
#define XCSI_ISR_VC2FLVLERR	BIT(4)
#define XCSI_ISR_VC1FSYNCERR	BIT(3)
#define XCSI_ISR_VC1FLVLERR	BIT(2)
#define XCSI_ISR_VC0FSYNCERR	BIT(1)
#define XCSI_ISR_VC0FLVLERR	BIT(0)

#define XCSI_ISR_ALLINTR_MASK	(0xd07e3fff)

/*
 * Removed VCXFE mask as it doesn't exist in IER
 * Removed STOP state irq as this will keep driver in irq handler only
 */
#define XCSI_IER_INTR_MASK	(XCSI_ISR_ALLINTR_MASK &\
				 ~(XCSI_ISR_STOP | XCSI_ISR_VCXFE))

#define XCSI_SPKTR_OFFSET	0x30
#define XCSI_SPKTR_DATA		GENMASK(23, 8)
#define XCSI_SPKTR_VC		GENMASK(7, 6)
#define XCSI_SPKTR_DT		GENMASK(5, 0)
#define XCSI_SPKT_FIFO_DEPTH	31

#define XCSI_VCXR_OFFSET	0x34
#define XCSI_VCXR_VCERR		GENMASK(23, 0)
#define XCSI_VCXR_FSYNCERR	BIT(1)
#define XCSI_VCXR_FLVLERR	BIT(0)

#define XCSI_CLKINFR_OFFSET	0x3C
#define XCSI_CLKINFR_STOP	BIT(1)

#define XCSI_DLXINFR_OFFSET	0x40
#define XCSI_DLXINFR_STOP	BIT(5)
#define XCSI_DLXINFR_SOTERR	BIT(1)
#define XCSI_DLXINFR_SOTSYNCERR	BIT(0)
#define XCSI_MAXDL_COUNT	0x4

#define XCSI_VCXINF1R_OFFSET		0x60
#define XCSI_VCXINF1R_LINECOUNT		GENMASK(31, 16)
#define XCSI_VCXINF1R_LINECOUNT_SHIFT	16
#define XCSI_VCXINF1R_BYTECOUNT		GENMASK(15, 0)

#define XCSI_VCXINF2R_OFFSET	0x64
#define XCSI_VCXINF2R_DT	GENMASK(5, 0)
#define XCSI_MAXVCX_COUNT	16

/*
 * Sink pad connected to sensor source pad.
 * Source pad connected to next module like demosaic.
 */
#define XCSI_MEDIA_PADS		2
#define XCSI_DEFAULT_WIDTH	1920
#define XCSI_DEFAULT_HEIGHT	1080

#define XCSI_VCX_START		4
#define XCSI_MAX_VC		4
#define XCSI_MAX_VCX		16

#define XCSI_NEXTREG_OFFSET	4

/* There are 2 events frame sync and frame level error per VC */
#define XCSI_VCX_NUM_EVENTS	((XCSI_MAX_VCX - XCSI_MAX_VC) * 2)

/**
 * struct xcsi2rxss_event - Event log structure
 * @mask: Event mask
 * @name: Name of the event
 */
struct xcsi2rxss_event {
	u32 mask;
	const char *name;
};

static const struct xcsi2rxss_event xcsi2rxss_events[] = {
	{ XCSI_ISR_FR, "Frame Received" },
	{ XCSI_ISR_VCXFE, "VCX Frame Errors" },
	{ XCSI_ISR_YUV420, "YUV 420 Word Count Errors" },
	{ XCSI_ISR_WCC, "Word Count Errors" },
	{ XCSI_ISR_ILC, "Invalid Lane Count Error" },
	{ XCSI_ISR_SPFIFOF, "Short Packet FIFO OverFlow Error" },
	{ XCSI_ISR_SPFIFONE, "Short Packet FIFO Not Empty" },
	{ XCSI_ISR_SLBF, "Streamline Buffer Full Error" },
	{ XCSI_ISR_STOP, "Lane Stop State" },
	{ XCSI_ISR_SOTERR, "SOT Error" },
	{ XCSI_ISR_SOTSYNCERR, "SOT Sync Error" },
	{ XCSI_ISR_ECC2BERR, "2 Bit ECC Unrecoverable Error" },
	{ XCSI_ISR_ECC1BERR, "1 Bit ECC Recoverable Error" },
	{ XCSI_ISR_CRCERR, "CRC Error" },
	{ XCSI_ISR_DATAIDERR, "Data Id Error" },
	{ XCSI_ISR_VC3FSYNCERR, "Virtual Channel 3 Frame Sync Error" },
	{ XCSI_ISR_VC3FLVLERR, "Virtual Channel 3 Frame Level Error" },
	{ XCSI_ISR_VC2FSYNCERR, "Virtual Channel 2 Frame Sync Error" },
	{ XCSI_ISR_VC2FLVLERR, "Virtual Channel 2 Frame Level Error" },
	{ XCSI_ISR_VC1FSYNCERR, "Virtual Channel 1 Frame Sync Error" },
	{ XCSI_ISR_VC1FLVLERR, "Virtual Channel 1 Frame Level Error" },
	{ XCSI_ISR_VC0FSYNCERR, "Virtual Channel 0 Frame Sync Error" },
	{ XCSI_ISR_VC0FLVLERR, "Virtual Channel 0 Frame Level Error" }
};

#define XCSI_NUM_EVENTS		ARRAY_SIZE(xcsi2rxss_events)

/*
 * This table provides a mapping between CSI-2 Data type
 * and media bus formats
 */
static const u32 xcsi2dt_mbus_lut[][2] = {
	{ MIPI_CSI2_DT_YUV422_8B, MEDIA_BUS_FMT_UYVY8_1X16 },
	{ MIPI_CSI2_DT_YUV422_10B, MEDIA_BUS_FMT_UYVY10_1X20 },
	{ MIPI_CSI2_DT_RGB444, 0 },
	{ MIPI_CSI2_DT_RGB555, 0 },
	{ MIPI_CSI2_DT_RGB565, 0 },
	{ MIPI_CSI2_DT_RGB666, 0 },
	{ MIPI_CSI2_DT_RGB888, MEDIA_BUS_FMT_RBG888_1X24 },
	{ MIPI_CSI2_DT_RAW6, 0 },
	{ MIPI_CSI2_DT_RAW7, 0 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SRGGB8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SBGGR8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SGBRG8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SGRBG8_1X8 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SRGGB10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SBGGR10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SGBRG10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SGRBG10_1X10 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SRGGB12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SBGGR12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SGBRG12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SGRBG12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_Y12_1X12 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SRGGB16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SBGGR16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SGBRG16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SGRBG16_1X16 },
	{ MIPI_CSI2_DT_RAW20, 0 },
};

/**
 * struct xcsi2rxss_state - CSI-2 Rx Subsystem device structure
 * @xvip: Xilinx Video IP device
 * @clks: array of clocks
 * @rst_gpio: reset to video_aresetn
 * @lock: mutex for accessing this structure
 * @default_format: Default V4L2 format
 * @max_num_lanes: Maximum number of lanes present
 * @datatype: Data type filter
 * @enable_active_lanes: If number of active lanes can be modified
 * @en_vcx: If more than 4 VC are enabled
 * @@enabled_source_streams: Mask of enabled source streams
 * @events: counter for events
 * @vcx_events: counter for vcx_events
 *
 * This structure contains the device driver related parameters
 */
struct xcsi2rxss_state {
	struct xvip_device xvip;

	struct clk_bulk_data *clks;
	struct gpio_desc *rst_gpio;

	/* used to protect access to this struct */
	struct mutex lock;

	struct v4l2_mbus_framefmt default_format;
	u32 max_num_lanes;
	u32 datatype;
	bool enable_active_lanes;
	bool en_vcx;

	u64 enabled_source_streams;

	u32 events[XCSI_NUM_EVENTS];
	u32 vcx_events[XCSI_VCX_NUM_EVENTS];
};

static const struct clk_bulk_data xcsi2rxss_clks[] = {
	{ .id = "lite_aclk" },
	{ .id = "video_aclk" },
};

static inline struct xcsi2rxss_state *
to_xcsi2rxssstate(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xcsi2rxss_state, xvip.subdev);
}

/*
 * Register related operations
 */
static inline u32 xcsi2rxss_read(struct xcsi2rxss_state *csi2rx, u32 addr)
{
	return xvip_read(&csi2rx->xvip, addr);
}

static inline void xcsi2rxss_write(struct xcsi2rxss_state *csi2rx, u32 addr,
				   u32 value)
{
	xvip_write(&csi2rx->xvip, addr, value);
}

static inline void xcsi2rxss_clr(struct xcsi2rxss_state *csi2rx, u32 addr,
				 u32 clr)
{
	xcsi2rxss_write(csi2rx, addr,
			xcsi2rxss_read(csi2rx, addr) & ~clr);
}

static inline void xcsi2rxss_set(struct xcsi2rxss_state *csi2rx, u32 addr,
				 u32 set)
{
	xcsi2rxss_write(csi2rx, addr, xcsi2rxss_read(csi2rx, addr) | set);
}

/*
 * This function returns the nth mbus for a data type.
 * In case of error, mbus code returned is 0.
 */
static u32 xcsi2rxss_get_nth_mbus(u32 dt, u32 n)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xcsi2dt_mbus_lut); i++) {
		if (xcsi2dt_mbus_lut[i][0] == dt) {
			if (n-- == 0)
				return xcsi2dt_mbus_lut[i][1];
		}
	}

	return 0;
}

/* This returns the data type for a media bus format else 0 */
static u32 xcsi2rxss_get_dt(u32 mbus)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xcsi2dt_mbus_lut); i++) {
		if (xcsi2dt_mbus_lut[i][1] == mbus)
			return xcsi2dt_mbus_lut[i][0];
	}

	return 0;
}

/**
 * xcsi2rxss_soft_reset - Does a soft reset of the MIPI CSI-2 Rx Subsystem
 * @csi2rx: Xilinx CSI-2 Rx Subsystem structure pointer
 *
 * Core takes less than 100 video clock cycles to reset.
 * So a larger timeout value is chosen for margin.
 *
 * Return: 0 - on success OR -ETIME if reset times out
 */
static int xcsi2rxss_soft_reset(struct xcsi2rxss_state *csi2rx)
{
	u32 timeout = 1000; /* us */

	xcsi2rxss_set(csi2rx, XCSI_CCR_OFFSET, XCSI_CCR_SOFTRESET);

	while (xcsi2rxss_read(csi2rx, XCSI_CSR_OFFSET) & XCSI_CSR_RIPCD) {
		if (timeout == 0) {
			dev_err(csi2rx->xvip.dev, "soft reset timed out!\n");
			return -ETIME;
		}

		timeout--;
		udelay(1);
	}

	xcsi2rxss_clr(csi2rx, XCSI_CCR_OFFSET, XCSI_CCR_SOFTRESET);
	return 0;
}

static void xcsi2rxss_hard_reset(struct xcsi2rxss_state *csi2rx)
{
	if (!csi2rx->rst_gpio)
		return;

	/* minimum of 40 dphy_clk_200M cycles */
	gpiod_set_value_cansleep(csi2rx->rst_gpio, 1);
	usleep_range(1, 2);
	gpiod_set_value_cansleep(csi2rx->rst_gpio, 0);
}

static void xcsi2rxss_reset_event_counters(struct xcsi2rxss_state *csi2rx)
{
	unsigned int i;

	for (i = 0; i < XCSI_NUM_EVENTS; i++)
		csi2rx->events[i] = 0;

	for (i = 0; i < XCSI_VCX_NUM_EVENTS; i++)
		csi2rx->vcx_events[i] = 0;
}

static int xcsi2rxss_start_stream(struct xcsi2rxss_state *csi2rx)
{
	int ret;

	/* enable core */
	xcsi2rxss_set(csi2rx, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);

	ret = xcsi2rxss_soft_reset(csi2rx);
	if (ret) {
		/* disable core */
		xcsi2rxss_clr(csi2rx, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);
		return ret;
	}

	/* enable interrupts */
	xcsi2rxss_clr(csi2rx, XCSI_GIER_OFFSET, XCSI_GIER_GIE);
	xcsi2rxss_write(csi2rx, XCSI_IER_OFFSET, XCSI_IER_INTR_MASK);
	xcsi2rxss_set(csi2rx, XCSI_GIER_OFFSET, XCSI_GIER_GIE);

	return 0;
}

static void xcsi2rxss_stop_stream(struct xcsi2rxss_state *csi2rx)
{
	/* disable interrupts */
	xcsi2rxss_clr(csi2rx, XCSI_IER_OFFSET, XCSI_IER_INTR_MASK);
	xcsi2rxss_clr(csi2rx, XCSI_GIER_OFFSET, XCSI_GIER_GIE);

	/* disable core */
	xcsi2rxss_clr(csi2rx, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);
}

/**
 * xcsi2rxss_irq_handler - Interrupt handler for CSI-2
 * @irq: IRQ number
 * @data: Pointer to device state
 *
 * In the interrupt handler, a list of event counters are updated for
 * corresponding interrupts. This is useful to get status / debug.
 *
 * Return: IRQ_HANDLED after handling interrupts
 */
static irqreturn_t xcsi2rxss_irq_handler(int irq, void *data)
{
	struct xcsi2rxss_state *csi2rx = (struct xcsi2rxss_state *)data;
	struct device *dev = csi2rx->xvip.dev;
	u32 status;

	status = xcsi2rxss_read(csi2rx, XCSI_ISR_OFFSET)
	       & XCSI_ISR_ALLINTR_MASK;
	xcsi2rxss_write(csi2rx, XCSI_ISR_OFFSET, status);

	/* Received a short packet */
	if (status & XCSI_ISR_SPFIFONE) {
		u32 count = 0;

		/*
		 * Drain generic short packet FIFO by reading max 31
		 * (fifo depth) short packets from fifo or till fifo is empty.
		 */
		for (count = 0; count < XCSI_SPKT_FIFO_DEPTH; ++count) {
			u32 spfifostat, spkt;

			spkt = xcsi2rxss_read(csi2rx, XCSI_SPKTR_OFFSET);
			dev_dbg(dev, "Short packet = 0x%08x\n", spkt);
			spfifostat = xcsi2rxss_read(csi2rx, XCSI_ISR_OFFSET);
			spfifostat &= XCSI_ISR_SPFIFONE;
			if (!spfifostat)
				break;
			xcsi2rxss_write(csi2rx, XCSI_ISR_OFFSET, spfifostat);
		}
	}

	/* Short packet FIFO overflow */
	if (status & XCSI_ISR_SPFIFOF)
		dev_dbg_ratelimited(dev, "Short packet FIFO overflowed\n");

	/*
	 * Stream line buffer full
	 * This means there is a backpressure from downstream IP
	 */
	if (status & (XCSI_ISR_SLBF | XCSI_ISR_YUV420)) {
		if (status & XCSI_ISR_SLBF)
			dev_alert_ratelimited(dev, "Stream Line Buffer Full!\n");
		if (status & XCSI_ISR_YUV420)
			dev_alert_ratelimited(dev, "YUV 420 Word count error!\n");

		/* disable interrupts */
		xcsi2rxss_clr(csi2rx, XCSI_IER_OFFSET, XCSI_IER_INTR_MASK);
		xcsi2rxss_clr(csi2rx, XCSI_GIER_OFFSET, XCSI_GIER_GIE);

		/* disable core */
		xcsi2rxss_clr(csi2rx, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);

		/*
		 * The IP needs to be hard reset before it can be used now.
		 * This will be done in streamoff.
		 */

		/*
		 * TODO: Notify the whole pipeline with v4l2_subdev_notify() to
		 * inform userspace.
		 */
	}

	/* Increment event counters */
	if (status & XCSI_ISR_ALLINTR_MASK) {
		unsigned int i;

		for (i = 0; i < XCSI_NUM_EVENTS; i++) {
			if (!(status & xcsi2rxss_events[i].mask))
				continue;
			csi2rx->events[i]++;
			dev_dbg_ratelimited(dev, "%s: %u\n",
					    xcsi2rxss_events[i].name,
					    csi2rx->events[i]);
		}

		if (status & XCSI_ISR_VCXFE && csi2rx->en_vcx) {
			u32 vcxstatus;

			vcxstatus = xcsi2rxss_read(csi2rx, XCSI_VCXR_OFFSET);
			vcxstatus &= XCSI_VCXR_VCERR;
			for (i = 0; i < XCSI_VCX_NUM_EVENTS; i++) {
				if (!(vcxstatus & BIT(i)))
					continue;
				csi2rx->vcx_events[i]++;
			}
			xcsi2rxss_write(csi2rx, XCSI_VCXR_OFFSET, vcxstatus);
		}
	}

	return IRQ_HANDLED;
}

/* -----------------------------------------------------------------------------
 * xvip Operations
 */

static int xcsi2rxss_enable_streams(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state, u32 pad,
				    u64 streams_mask)
{
	struct xcsi2rxss_state *csi2rx = to_xcsi2rxssstate(sd);
	int ret = 0;

	if (pad != XVIP_PAD_SOURCE)
		return -EINVAL;

	mutex_lock(&csi2rx->lock);

	/* Enable the HW if not yet enabled. */
	if (!csi2rx->enabled_source_streams) {
		xcsi2rxss_reset_event_counters(csi2rx);
		ret = xcsi2rxss_start_stream(csi2rx);
		if (ret)
			goto done;
	}

	csi2rx->enabled_source_streams |= streams_mask;

done:
	mutex_unlock(&csi2rx->lock);
	return ret;
}

static int xcsi2rxss_disable_streams(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state, u32 pad,
				     u64 streams_mask)
{
	struct xcsi2rxss_state *csi2rx = to_xcsi2rxssstate(sd);

	if (pad != XVIP_PAD_SOURCE)
		return -EINVAL;

	mutex_lock(&csi2rx->lock);

	/* Disable the HW is no streams are left enabled. */
	if (csi2rx->enabled_source_streams == streams_mask) {
		xcsi2rxss_stop_stream(csi2rx);
		xcsi2rxss_hard_reset(csi2rx);
	}

	csi2rx->enabled_source_streams &= ~streams_mask;

	mutex_unlock(&csi2rx->lock);

	return 0;
}

static const struct xvip_device_ops xcsi2rxss_xvip_device_ops = {
	.enable_streams = xcsi2rxss_enable_streams,
	.disable_streams = xcsi2rxss_disable_streams,
};

/* -----------------------------------------------------------------------------
 * V4L2 Subdev Operations
 */

/* Print event counters */
static void xcsi2rxss_log_counters(struct xcsi2rxss_state *csi2rx)
{
	struct device *dev = csi2rx->xvip.dev;
	unsigned int i;

	for (i = 0; i < XCSI_NUM_EVENTS; i++) {
		if (csi2rx->events[i] > 0) {
			dev_info(dev, "%s events: %d\n",
				 xcsi2rxss_events[i].name,
				 csi2rx->events[i]);
		}
	}

	if (csi2rx->en_vcx) {
		for (i = 0; i < XCSI_VCX_NUM_EVENTS; i++) {
			if (csi2rx->vcx_events[i] > 0) {
				dev_info(dev,
					 "VC %d Frame %s err vcx events: %d\n",
					 (i / 2) + XCSI_VCX_START,
					 i & 1 ? "Sync" : "Level",
					 csi2rx->vcx_events[i]);
			}
		}
	}
}

/**
 * xcsi2rxss_log_status - Logs the status of the CSI-2 Receiver
 * @sd: Pointer to V4L2 subdevice structure
 *
 * This function prints the current status of Xilinx MIPI CSI-2
 *
 * Return: 0 on success
 */
static int xcsi2rxss_log_status(struct v4l2_subdev *sd)
{
	struct xcsi2rxss_state *csi2rx = to_xcsi2rxssstate(sd);
	struct device *dev = csi2rx->xvip.dev;
	u32 reg, data;
	unsigned int i, max_vc;

	mutex_lock(&csi2rx->lock);

	xcsi2rxss_log_counters(csi2rx);

	dev_info(dev, "***** Core Status *****\n");
	data = xcsi2rxss_read(csi2rx, XCSI_CSR_OFFSET);
	dev_info(dev, "Short Packet FIFO Full = %s\n",
		 data & XCSI_CSR_SPFIFOFULL ? "true" : "false");
	dev_info(dev, "Short Packet FIFO Not Empty = %s\n",
		 data & XCSI_CSR_SPFIFONE ? "true" : "false");
	dev_info(dev, "Stream line buffer full = %s\n",
		 data & XCSI_CSR_SLBF ? "true" : "false");
	dev_info(dev, "Soft reset/Core disable in progress = %s\n",
		 data & XCSI_CSR_RIPCD ? "true" : "false");

	/* Clk & Lane Info  */
	dev_info(dev, "******** Clock Lane Info *********\n");
	data = xcsi2rxss_read(csi2rx, XCSI_CLKINFR_OFFSET);
	dev_info(dev, "Clock Lane in Stop State = %s\n",
		 data & XCSI_CLKINFR_STOP ? "true" : "false");

	dev_info(dev, "******** Data Lane Info *********\n");
	dev_info(dev, "Lane\tSoT Error\tSoT Sync Error\tStop State\n");
	reg = XCSI_DLXINFR_OFFSET;
	for (i = 0; i < XCSI_MAXDL_COUNT; i++) {
		data = xcsi2rxss_read(csi2rx, reg);

		dev_info(dev, "%d\t%s\t\t%s\t\t%s\n", i,
			 data & XCSI_DLXINFR_SOTERR ? "true" : "false",
			 data & XCSI_DLXINFR_SOTSYNCERR ? "true" : "false",
			 data & XCSI_DLXINFR_STOP ? "true" : "false");

		reg += XCSI_NEXTREG_OFFSET;
	}

	/* Virtual Channel Image Information */
	dev_info(dev, "********** Virtual Channel Info ************\n");
	dev_info(dev, "VC\tLine Count\tByte Count\tData Type\n");
	if (csi2rx->en_vcx)
		max_vc = XCSI_MAX_VCX;
	else
		max_vc = XCSI_MAX_VC;

	reg = XCSI_VCXINF1R_OFFSET;
	for (i = 0; i < max_vc; i++) {
		u32 line_count, byte_count, data_type;

		/* Get line and byte count from VCXINFR1 Register */
		data = xcsi2rxss_read(csi2rx, reg);
		byte_count = data & XCSI_VCXINF1R_BYTECOUNT;
		line_count = data & XCSI_VCXINF1R_LINECOUNT;
		line_count >>= XCSI_VCXINF1R_LINECOUNT_SHIFT;

		/* Get data type from VCXINFR2 Register */
		reg += XCSI_NEXTREG_OFFSET;
		data = xcsi2rxss_read(csi2rx, reg);
		data_type = data & XCSI_VCXINF2R_DT;

		dev_info(dev, "%d\t%d\t\t%d\t\t0x%x\n", i, line_count,
			 byte_count, data_type);

		/* Move to next pair of VC Info registers */
		reg += XCSI_NEXTREG_OFFSET;
	}

	mutex_unlock(&csi2rx->lock);

	return 0;
}

static int __xcsi2rxss_set_routing(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_krouting *routing)
{
	struct xcsi2rxss_state *csi2rx = to_xcsi2rxssstate(sd);
	int ret;

	ret = v4l2_subdev_routing_validate(sd, routing, 0);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(sd, state, routing,
						&csi2rx->default_format);
}

/**
 * xcsi2rxss_init_cfg - Initialise the subdev state to default values
 * @sd: Pointer to V4L2 Sub device structure
 * @state: Pointer to sub device state structure
 *
 * Configure the CSI-2 RX state with a single route from the sink pad to the
 * source pad, using stream 0 on both sides. This is the most common use case.
 *
 * Return: 0 on success
 */
static int xcsi2rxss_init_cfg(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.sink_pad = XVIP_PAD_SINK,
			.sink_stream = 0,
			.source_pad = XVIP_PAD_SOURCE,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};
	struct v4l2_subdev_krouting routing = {
		.num_routes = 1,
		.routes = routes,
	};

	return __xcsi2rxss_set_routing(sd, state, &routing);
}

/*
 * xcsi2rxss_enum_mbus_code - Handle pixel format enumeration
 * @sd: pointer to v4l2 subdev structure
 * @state: Pointer to sub device state structure
 * @code: pointer to v4l2_subdev_mbus_code_enum structure
 *
 * Return: -EINVAL or zero on success
 */
static int xcsi2rxss_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	struct xcsi2rxss_state *csi2rx = to_xcsi2rxssstate(sd);
	u32 dt, n;
	int ret = 0;

	/* RAW8 dt packets are available in all DT configurations */
	if (code->index < 4) {
		n = code->index;
		dt = MIPI_CSI2_DT_RAW8;
	} else if (csi2rx->datatype != MIPI_CSI2_DT_RAW8) {
		n = code->index - 4;
		dt = csi2rx->datatype;
	} else {
		return -EINVAL;
	}

	code->code = xcsi2rxss_get_nth_mbus(dt, n);
	if (!code->code)
		ret = -EINVAL;

	return ret;
}

/**
 * xcsi2rxss_set_format - This is used to set the pad format
 * @sd: Pointer to V4L2 Sub device structure
 * @state: Pointer to sub device state structure
 * @fmt: Pointer to pad level media bus format
 *
 * This function is used to set the pad format. Since the pad format is fixed
 * in hardware, it can't be modified on run time. So when a format set is
 * requested by application, all parameters except the format type is saved
 * for the pad and the original pad format is sent back to the application.
 *
 * Return: 0 on success
 */
static int xcsi2rxss_set_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *format)
{
	struct xcsi2rxss_state *csi2rx = to_xcsi2rxssstate(sd);
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_mbus_framefmt *source_fmt;
	int ret = 0;
	u32 dt;

	/* No transcoding, source and sink formats must match. */
	if (format->pad != XVIP_PAD_SINK)
		return v4l2_subdev_get_fmt(sd, state, format);

	/*
	 * RAW8 is supported in all datatypes. So if requested media bus format
	 * is of RAW8 type, then allow to be set. In case core is configured to
	 * other RAW, YUV422 8/10 or RGB888, set appropriate media bus format.
	 */
	dt = xcsi2rxss_get_dt(format->format.code);
	if (dt != csi2rx->datatype && dt != MIPI_CSI2_DT_RAW8) {
		dev_dbg(csi2rx->xvip.dev, "Unsupported media bus format");
		/* set the default format for the data type */
		format->format.code = xcsi2rxss_get_nth_mbus(csi2rx->datatype,
							     0);
	}

	/*
	 * Set the format on the sink stream and propagate it to the source
	 * stream.
	 */
	sink_fmt = v4l2_subdev_state_get_stream_format(state, format->pad,
						       format->stream);
	source_fmt = v4l2_subdev_state_get_opposite_stream_format(state,
								  format->pad,
								  format->stream);
	if (!sink_fmt || !source_fmt)
		return -EINVAL;

	*sink_fmt = format->format;
	*source_fmt = format->format;

	return ret;
}

static int xcsi2rxss_set_routing(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 enum v4l2_subdev_format_whence which,
				 struct v4l2_subdev_krouting *routing)
{
	return __xcsi2rxss_set_routing(sd, state, routing);
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xcsi2rxss_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

static const struct v4l2_subdev_core_ops xcsi2rxss_core_ops = {
	.log_status = xcsi2rxss_log_status,
};

static const struct v4l2_subdev_pad_ops xcsi2rxss_pad_ops = {
	.init_cfg = xcsi2rxss_init_cfg,
	.enum_mbus_code = xcsi2rxss_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xcsi2rxss_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
	.set_routing = xcsi2rxss_set_routing,
	.enable_streams = xvip_enable_streams,
	.disable_streams = xvip_disable_streams,
};

static const struct v4l2_subdev_ops xcsi2rxss_ops = {
	.core = &xcsi2rxss_core_ops,
	.pad = &xcsi2rxss_pad_ops
};

static int xcsi2rxss_parse_of(struct xcsi2rxss_state *csi2rx)
{
	struct device *dev = csi2rx->xvip.dev;
	struct device_node *node = dev->of_node;

	struct fwnode_handle *ep;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	bool en_csi_v20, vfb;
	int ret;

	en_csi_v20 = of_property_read_bool(node, "xlnx,en-csi-v2-0");
	if (en_csi_v20)
		csi2rx->en_vcx = of_property_read_bool(node, "xlnx,en-vcx");

	csi2rx->enable_active_lanes =
		of_property_read_bool(node, "xlnx,en-active-lanes");

	ret = of_property_read_u32(node, "xlnx,csi-pxl-format",
				   &csi2rx->datatype);
	if (ret < 0) {
		dev_err(dev, "missing xlnx,csi-pxl-format property\n");
		return ret;
	}

	switch (csi2rx->datatype) {
	case MIPI_CSI2_DT_YUV422_8B:
	case MIPI_CSI2_DT_RGB444:
	case MIPI_CSI2_DT_RGB555:
	case MIPI_CSI2_DT_RGB565:
	case MIPI_CSI2_DT_RGB666:
	case MIPI_CSI2_DT_RGB888:
	case MIPI_CSI2_DT_RAW6:
	case MIPI_CSI2_DT_RAW7:
	case MIPI_CSI2_DT_RAW8:
	case MIPI_CSI2_DT_RAW10:
	case MIPI_CSI2_DT_RAW12:
	case MIPI_CSI2_DT_RAW14:
		break;
	case MIPI_CSI2_DT_YUV422_10B:
	case MIPI_CSI2_DT_RAW16:
	case MIPI_CSI2_DT_RAW20:
		if (!en_csi_v20) {
			ret = -EINVAL;
			dev_dbg(dev, "enable csi v2 for this pixel format");
		}
		break;
	default:
		ret = -EINVAL;
	}
	if (ret < 0) {
		dev_err(dev, "invalid csi-pxl-format property!\n");
		return ret;
	}

	vfb = of_property_read_bool(node, "xlnx,vfb");
	if (!vfb) {
		dev_err(dev, "operation without VFB is not supported\n");
		return -EINVAL;
	}

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     XVIP_PAD_SINK, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		dev_err(dev, "no sink port found");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret) {
		dev_err(dev, "error parsing sink port");
		return ret;
	}

	dev_dbg(dev, "mipi number lanes = %d\n",
		vep.bus.mipi_csi2.num_data_lanes);

	csi2rx->max_num_lanes = vep.bus.mipi_csi2.num_data_lanes;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     XVIP_PAD_SOURCE, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		dev_err(dev, "no source port found");
		return -EINVAL;
	}

	fwnode_handle_put(ep);

	dev_dbg(dev, "vcx %s, %u data lanes (%s), data type 0x%02x\n",
		csi2rx->en_vcx ? "enabled" : "disabled",
		csi2rx->max_num_lanes,
		csi2rx->enable_active_lanes ? "dynamic" : "static",
		csi2rx->datatype);

	return 0;
}

static const struct xvip_device_info xcsi2rxss_info = {
	.has_axi_lite = true,
	.num_sinks = 1,
	.num_sources = 1,
};

static int xcsi2rxss_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xcsi2rxss_state *csi2rx;
	int num_clks = ARRAY_SIZE(xcsi2rxss_clks);
	struct device *dev = &pdev->dev;
	int irq, ret;

	csi2rx = devm_kzalloc(dev, sizeof(*csi2rx), GFP_KERNEL);
	if (!csi2rx)
		return -ENOMEM;

	csi2rx->xvip.dev = dev;
	csi2rx->xvip.ops = &xcsi2rxss_xvip_device_ops;

	ret = xvip_device_init(&csi2rx->xvip, &xcsi2rxss_info);
	if (ret)
		return ret;

	csi2rx->clks = devm_kmemdup(dev, xcsi2rxss_clks,
				       sizeof(xcsi2rxss_clks), GFP_KERNEL);
	if (!csi2rx->clks) {
		ret = -ENOMEM;
		goto err_xvip;
	}

	/* Reset GPIO */
	csi2rx->rst_gpio = devm_gpiod_get_optional(dev, "video-reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(csi2rx->rst_gpio)) {
		if (PTR_ERR(csi2rx->rst_gpio) != -EPROBE_DEFER)
			dev_err(dev, "Video Reset GPIO not setup in DT");
		ret = PTR_ERR(csi2rx->rst_gpio);
		goto err_xvip;
	}

	ret = xcsi2rxss_parse_of(csi2rx);
	if (ret < 0)
		goto err_xvip;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_xvip;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL,
					xcsi2rxss_irq_handler, IRQF_ONESHOT,
					dev_name(dev), csi2rx);
	if (ret) {
		dev_err(dev, "Err = %d Interrupt handler reg failed!\n", ret);
		goto err_xvip;
	}

	ret = clk_bulk_get(dev, num_clks, csi2rx->clks);
	if (ret)
		goto err_xvip;

	/* TODO: Enable/disable clocks at stream on/off time. */
	ret = clk_bulk_prepare_enable(num_clks, csi2rx->clks);
	if (ret)
		goto err_clk_put;

	mutex_init(&csi2rx->lock);

	xcsi2rxss_hard_reset(csi2rx);
	xcsi2rxss_soft_reset(csi2rx);

	/* Initialize the default format */
	csi2rx->default_format.code =
		xcsi2rxss_get_nth_mbus(csi2rx->datatype, 0);
	csi2rx->default_format.field = V4L2_FIELD_NONE;
	csi2rx->default_format.colorspace = V4L2_COLORSPACE_SRGB;
	csi2rx->default_format.width = XCSI_DEFAULT_WIDTH;
	csi2rx->default_format.height = XCSI_DEFAULT_HEIGHT;

	/* Initialize V4L2 subdevice and media entity */
	subdev = &csi2rx->xvip.subdev;
	v4l2_subdev_init(subdev, &xcsi2rxss_ops);
	subdev->dev = dev;
	strscpy(subdev->name, dev_name(dev), sizeof(subdev->name));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_EVENTS | V4L2_SUBDEV_FL_HAS_DEVNODE
		      |  V4L2_SUBDEV_FL_STREAMS;
	subdev->entity.ops = &xcsi2rxss_media_ops;
	v4l2_set_subdevdata(subdev, csi2rx);

	ret = media_entity_pads_init(&subdev->entity, XCSI_MEDIA_PADS,
				     csi2rx->xvip.pads);
	if (ret < 0)
		goto err_cleanup;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret)
		goto err_cleanup;

	platform_set_drvdata(pdev, csi2rx);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto err_cleanup;
	}

	return 0;

err_cleanup:
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	mutex_destroy(&csi2rx->lock);
	clk_bulk_disable_unprepare(num_clks, csi2rx->clks);
err_clk_put:
	clk_bulk_put(num_clks, csi2rx->clks);
err_xvip:
	xvip_device_cleanup(&csi2rx->xvip);
	return ret;
}

static int xcsi2rxss_remove(struct platform_device *pdev)
{
	struct xcsi2rxss_state *csi2rx = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &csi2rx->xvip.subdev;
	int num_clks = ARRAY_SIZE(xcsi2rxss_clks);

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	mutex_destroy(&csi2rx->lock);
	clk_bulk_disable_unprepare(num_clks, csi2rx->clks);
	clk_bulk_put(num_clks, csi2rx->clks);
	xvip_device_cleanup(&csi2rx->xvip);

	return 0;
}

static const struct of_device_id xcsi2rxss_of_id_table[] = {
	{ .compatible = "xlnx,mipi-csi2-rx-subsystem-5.0", },
	{ }
};
MODULE_DEVICE_TABLE(of, xcsi2rxss_of_id_table);

static struct platform_driver xcsi2rxss_driver = {
	.driver = {
		.name		= "xilinx-csi2rxss",
		.of_match_table	= xcsi2rxss_of_id_table,
	},
	.probe			= xcsi2rxss_probe,
	.remove			= xcsi2rxss_remove,
};

module_platform_driver(xcsi2rxss_driver);

MODULE_AUTHOR("Vishal Sagar <vsagar@xilinx.com>");
MODULE_DESCRIPTION("Xilinx MIPI CSI-2 Rx Subsystem Driver");
MODULE_LICENSE("GPL v2");
