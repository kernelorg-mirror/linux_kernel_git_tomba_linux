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
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/v4l2-subdev.h>
#include <media/media-entity.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define XCSI_PAD_SINK		0
#define XCSI_PAD_SOURCE		1
#define XCSI_PAD_SOURCE_EMB	2

/* Register map */
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

#define XCSI_VCSELR_OFFSET	0x2c
#define XCSI_VCSELR_VCSEL	GENMASK(15, 0)

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
 * The optional embedded data source pad carries the subsystem's embedded
 * data output.
 */
#define XCSI_MEDIA_PADS		3
#define XCSI_DEFAULT_WIDTH	1920
#define XCSI_DEFAULT_HEIGHT	1080

#define XCSI_VCX_START		4
#define XCSI_MAX_VC		4
#define XCSI_MAX_VCX		16

#define XCSI_NEXTREG_OFFSET	4

#define XCSI_PHY_MODE_CPHY	0
#define XCSI_PHY_MODE_DPHY	1

/* Feature flags */
#define XCSI_HAS_PHY_MODE	BIT(0)

/**
 * struct xcsi2rxss_feature - IP version feature configuration
 * @flags: Bitmask of features enabled in this IP version
 */
struct xcsi2rxss_feature {
	u32 flags;
};

static const struct xcsi2rxss_feature xcsi2rxss_cfg_v50 = { };

static const struct xcsi2rxss_feature xcsi2rxss_cfg_v60 = {
	.flags = XCSI_HAS_PHY_MODE,
};

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
	{ MIPI_CSI2_DT_YUV420_8B, MEDIA_BUS_FMT_VYYUYY8_1X24 },
	{ MIPI_CSI2_DT_YUV422_8B, MEDIA_BUS_FMT_UYVY8_1X16 },
	{ MIPI_CSI2_DT_YUV422_10B, MEDIA_BUS_FMT_UYVY10_1X20 },
	{ MIPI_CSI2_DT_RGB444, 0 },
	{ MIPI_CSI2_DT_RGB555, 0 },
	{ MIPI_CSI2_DT_RGB565, 0 },
	{ MIPI_CSI2_DT_RGB666, 0 },
	{ MIPI_CSI2_DT_RGB888, MEDIA_BUS_FMT_RBG888_1X24 },
	{ MIPI_CSI2_DT_RGB888, MEDIA_BUS_FMT_GBR888_1X24 },
	{ MIPI_CSI2_DT_RGB888, MEDIA_BUS_FMT_RGB888_1X24 },
	{ MIPI_CSI2_DT_RGB888, MEDIA_BUS_FMT_BGR888_1X24 },
	{ MIPI_CSI2_DT_RAW6, 0 },
	{ MIPI_CSI2_DT_RAW7, 0 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SRGGB8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SBGGR8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SGBRG8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_SGRBG8_1X8 },
	{ MIPI_CSI2_DT_RAW8, MEDIA_BUS_FMT_Y8_1X8 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SRGGB10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SBGGR10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SGBRG10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_SGRBG10_1X10 },
	{ MIPI_CSI2_DT_RAW10, MEDIA_BUS_FMT_Y10_1X10 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SRGGB12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SBGGR12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SGBRG12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_SGRBG12_1X12 },
	{ MIPI_CSI2_DT_RAW12, MEDIA_BUS_FMT_Y12_1X12 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SRGGB16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SBGGR16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SGBRG16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_SGRBG16_1X16 },
	{ MIPI_CSI2_DT_RAW16, MEDIA_BUS_FMT_Y16_1X16 },
	{ MIPI_CSI2_DT_RAW20, 0 },
	{ MIPI_CSI2_DT_EMBEDDED_8B, MEDIA_BUS_FMT_META_8 },
};

/**
 * struct xcsi2rxss_state - CSI-2 Rx Subsystem device structure
 * @subdev: The v4l2 subdev structure
 * @events: counter for events
 * @vcx_events: counter for vcx_events
 * @dev: Platform structure
 * @rst_gpio: reset to video_aresetn
 * @clks: array of clocks
 * @iomem: Base address of subsystem
 * @max_num_lanes: Maximum number of lanes present
 * @datatype: Data type filter
 * @pads: media pads
 * @enabled_sink_streams: sink streams currently enabled
 * @has_embdata: If the embedded data source port is present
 * @enable_active_lanes: If number of active lanes can be modified
 * @en_vcx: If more than 4 VC are enabled
 * @is_cphy: true if C-PHY mode, false if D-PHY mode
 * @cfg: Pointer to IP version feature config struct
 *
 * This structure contains the device driver related parameters
 */
struct xcsi2rxss_state {
	struct v4l2_subdev subdev;
	u32 events[XCSI_NUM_EVENTS];
	u32 vcx_events[XCSI_VCX_NUM_EVENTS];
	struct device *dev;
	struct gpio_desc *rst_gpio;
	struct clk_bulk_data *clks;
	void __iomem *iomem;
	u32 max_num_lanes;
	u32 datatype;
	struct media_pad pads[XCSI_MEDIA_PADS];
	u64 enabled_sink_streams;
	bool has_embdata;
	bool enable_active_lanes;
	bool en_vcx;
	bool is_cphy;
	const struct xcsi2rxss_feature *cfg;
};

static const struct clk_bulk_data xcsi2rxss_clks[] = {
	{ .id = "lite_aclk" },
	{ .id = "video_aclk" },
};

static inline struct xcsi2rxss_state *
to_xcsi2rxssstate(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xcsi2rxss_state, subdev);
}

/*
 * Register related operations
 */
static inline u32 xcsi2rxss_read(struct xcsi2rxss_state *xcsi2rxss, u32 addr)
{
	return ioread32(xcsi2rxss->iomem + addr);
}

static inline void xcsi2rxss_write(struct xcsi2rxss_state *xcsi2rxss, u32 addr,
				   u32 value)
{
	iowrite32(value, xcsi2rxss->iomem + addr);
}

static inline void xcsi2rxss_clr(struct xcsi2rxss_state *xcsi2rxss, u32 addr,
				 u32 clr)
{
	xcsi2rxss_write(xcsi2rxss, addr,
			xcsi2rxss_read(xcsi2rxss, addr) & ~clr);
}

static inline void xcsi2rxss_set(struct xcsi2rxss_state *xcsi2rxss, u32 addr,
				 u32 set)
{
	xcsi2rxss_write(xcsi2rxss, addr, xcsi2rxss_read(xcsi2rxss, addr) | set);
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
 * @state: Xilinx CSI-2 Rx Subsystem structure pointer
 *
 * Core takes less than 100 video clock cycles to reset.
 * So a larger timeout value is chosen for margin.
 *
 * Return: 0 - on success OR -ETIME if reset times out
 */
static int xcsi2rxss_soft_reset(struct xcsi2rxss_state *state)
{
	u32 timeout = 1000; /* us */

	xcsi2rxss_set(state, XCSI_CCR_OFFSET, XCSI_CCR_SOFTRESET);

	while (xcsi2rxss_read(state, XCSI_CSR_OFFSET) & XCSI_CSR_RIPCD) {
		if (timeout == 0) {
			dev_err(state->dev, "soft reset timed out!\n");
			return -ETIME;
		}

		timeout--;
		udelay(1);
	}

	xcsi2rxss_clr(state, XCSI_CCR_OFFSET, XCSI_CCR_SOFTRESET);
	return 0;
}

static void xcsi2rxss_hard_reset(struct xcsi2rxss_state *state)
{
	if (!state->rst_gpio)
		return;

	/* minimum of 40 dphy_clk_200M cycles */
	gpiod_set_value_cansleep(state->rst_gpio, 1);
	usleep_range(1, 2);
	gpiod_set_value_cansleep(state->rst_gpio, 0);
}

static void xcsi2rxss_reset_event_counters(struct xcsi2rxss_state *state)
{
	unsigned int i;

	for (i = 0; i < XCSI_NUM_EVENTS; i++)
		state->events[i] = 0;

	for (i = 0; i < XCSI_VCX_NUM_EVENTS; i++)
		state->vcx_events[i] = 0;
}

/* Print event counters */
static void xcsi2rxss_log_counters(struct xcsi2rxss_state *state)
{
	struct device *dev = state->dev;
	unsigned int i;

	for (i = 0; i < XCSI_NUM_EVENTS; i++) {
		if (state->events[i] > 0) {
			dev_info(dev, "%s events: %d\n",
				 xcsi2rxss_events[i].name,
				 state->events[i]);
		}
	}

	if (state->en_vcx) {
		for (i = 0; i < XCSI_VCX_NUM_EVENTS; i++) {
			if (state->vcx_events[i] > 0) {
				dev_info(dev,
					 "VC %d Frame %s err vcx events: %d\n",
					 (i / 2) + XCSI_VCX_START,
					 i & 1 ? "Sync" : "Level",
					 state->vcx_events[i]);
			}
		}
	}
}

static int xcsi2rxss_log_status(struct v4l2_subdev *sd)
{
	struct xcsi2rxss_state *xcsi2rxss = to_xcsi2rxssstate(sd);
	struct device *dev = xcsi2rxss->dev;
	struct v4l2_subdev_state *state;
	u32 reg, data;
	unsigned int i, max_vc;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	xcsi2rxss_log_counters(xcsi2rxss);

	dev_info(dev, "***** Core Status *****\n");
	data = xcsi2rxss_read(xcsi2rxss, XCSI_CSR_OFFSET);
	dev_info(dev, "Short Packet FIFO Full = %s\n",
		 data & XCSI_CSR_SPFIFOFULL ? "true" : "false");
	dev_info(dev, "Short Packet FIFO Not Empty = %s\n",
		 data & XCSI_CSR_SPFIFONE ? "true" : "false");
	dev_info(dev, "Stream line buffer full = %s\n",
		 data & XCSI_CSR_SLBF ? "true" : "false");
	dev_info(dev, "Soft reset/Core disable in progress = %s\n",
		 data & XCSI_CSR_RIPCD ? "true" : "false");

	if (!xcsi2rxss->is_cphy) {
		dev_info(dev, "******** Clock Lane Info *********\n");
		data = xcsi2rxss_read(xcsi2rxss, XCSI_CLKINFR_OFFSET);
		dev_info(dev, "Clock Lane in Stop State = %s\n",
			 data & XCSI_CLKINFR_STOP ? "true" : "false");
	}

	dev_info(dev, "******** Data %s Info *********\n",
		 xcsi2rxss->is_cphy ? "Trio" : "Lane");
	dev_info(dev, "%s\tSoT Error\tSoT Sync Error\tStop State\n",
		 xcsi2rxss->is_cphy ? "Trio" : "Lane");

	reg = XCSI_DLXINFR_OFFSET;
	for (i = 0; i < xcsi2rxss->max_num_lanes; i++) {
		data = xcsi2rxss_read(xcsi2rxss, reg);

		dev_info(dev, "%d\t%s\t\t%s\t\t%s\n", i,
			 data & XCSI_DLXINFR_SOTERR ? "true" : "false",
			 data & XCSI_DLXINFR_SOTSYNCERR ? "true" : "false",
			 data & XCSI_DLXINFR_STOP ? "true" : "false");

		reg += XCSI_NEXTREG_OFFSET;
	}

	/* Virtual Channel Image Information */
	dev_info(dev, "********** Virtual Channel Info ************\n");
	dev_info(dev, "VC\tLine Count\tByte Count\tData Type\n");
	if (xcsi2rxss->en_vcx)
		max_vc = XCSI_MAX_VCX;
	else
		max_vc = XCSI_MAX_VC;

	reg = XCSI_VCXINF1R_OFFSET;
	for (i = 0; i < max_vc; i++) {
		u32 line_count, byte_count, data_type;

		/* Get line and byte count from VCXINFR1 Register */
		data = xcsi2rxss_read(xcsi2rxss, reg);
		byte_count = data & XCSI_VCXINF1R_BYTECOUNT;
		line_count = data & XCSI_VCXINF1R_LINECOUNT;
		line_count >>= XCSI_VCXINF1R_LINECOUNT_SHIFT;

		/* Get data type from VCXINFR2 Register */
		reg += XCSI_NEXTREG_OFFSET;
		data = xcsi2rxss_read(xcsi2rxss, reg);
		data_type = data & XCSI_VCXINF2R_DT;

		dev_info(dev, "%d\t%d\t\t%d\t\t0x%x\n", i, line_count,
			 byte_count, data_type);

		/* Move to next pair of VC Info registers */
		reg += XCSI_NEXTREG_OFFSET;
	}

	v4l2_subdev_unlock_state(state);

	return 0;
}

static int xcsi2rxss_start_core(struct xcsi2rxss_state *state)
{
	int ret;

	/* enable core */
	xcsi2rxss_set(state, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);

	ret = xcsi2rxss_soft_reset(state);
	if (ret) {
		xcsi2rxss_clr(state, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);
		xcsi2rxss_hard_reset(state);
		return ret;
	}

	/* enable interrupts */
	xcsi2rxss_clr(state, XCSI_GIER_OFFSET, XCSI_GIER_GIE);
	xcsi2rxss_write(state, XCSI_IER_OFFSET, XCSI_IER_INTR_MASK);
	xcsi2rxss_set(state, XCSI_GIER_OFFSET, XCSI_GIER_GIE);

	return 0;
}

static void xcsi2rxss_stop_core(struct xcsi2rxss_state *state)
{
	/* disable interrupts */
	xcsi2rxss_clr(state, XCSI_IER_OFFSET, XCSI_IER_INTR_MASK);
	xcsi2rxss_clr(state, XCSI_GIER_OFFSET, XCSI_GIER_GIE);

	/* disable core */
	xcsi2rxss_clr(state, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);

	xcsi2rxss_hard_reset(state);
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
	struct xcsi2rxss_state *state = (struct xcsi2rxss_state *)data;
	struct device *dev = state->dev;
	u32 status;

	status = xcsi2rxss_read(state, XCSI_ISR_OFFSET) & XCSI_ISR_ALLINTR_MASK;
	xcsi2rxss_write(state, XCSI_ISR_OFFSET, status);

	/* Received a short packet */
	if (status & XCSI_ISR_SPFIFONE) {
		u32 count = 0;

		/*
		 * Drain generic short packet FIFO by reading max 31
		 * (fifo depth) short packets from fifo or till fifo is empty.
		 */
		for (count = 0; count < XCSI_SPKT_FIFO_DEPTH; ++count) {
			u32 spfifostat, spkt;

			spkt = xcsi2rxss_read(state, XCSI_SPKTR_OFFSET);
			dev_dbg(dev, "Short packet = 0x%08x\n", spkt);
			spfifostat = xcsi2rxss_read(state, XCSI_ISR_OFFSET);
			spfifostat &= XCSI_ISR_SPFIFONE;
			if (!spfifostat)
				break;
			xcsi2rxss_write(state, XCSI_ISR_OFFSET, spfifostat);
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
		xcsi2rxss_clr(state, XCSI_IER_OFFSET, XCSI_IER_INTR_MASK);
		xcsi2rxss_clr(state, XCSI_GIER_OFFSET, XCSI_GIER_GIE);

		/* disable core */
		xcsi2rxss_clr(state, XCSI_CCR_OFFSET, XCSI_CCR_ENABLE);

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
			state->events[i]++;
			dev_dbg_ratelimited(dev, "%s: %u\n",
					    xcsi2rxss_events[i].name,
					    state->events[i]);
		}

		if (status & XCSI_ISR_VCXFE && state->en_vcx) {
			u32 vcxstatus;

			vcxstatus = xcsi2rxss_read(state, XCSI_VCXR_OFFSET);
			vcxstatus &= XCSI_VCXR_VCERR;
			for (i = 0; i < XCSI_VCX_NUM_EVENTS; i++) {
				if (!(vcxstatus & BIT(i)))
					continue;
				state->vcx_events[i]++;
			}
			xcsi2rxss_write(state, XCSI_VCXR_OFFSET, vcxstatus);
		}
	}

	return IRQ_HANDLED;
}

/*
 * Get the mask of the virtual channels that carry the given sink streams, from
 * the frame descriptor of the source. Default to all the virtual channels if
 * the source doesn't report one, or if it doesn't describe every stream: the
 * core must not filter out data that a stream carries.
 */
static u16 xcsi2rxss_get_vc_mask(struct xcsi2rxss_state *state,
				 u64 streams_mask)
{
	struct v4l2_mbus_frame_desc fd;
	struct v4l2_subdev *subdev;
	struct media_pad *remote;
	u64 found_streams = 0;
	u16 vc_mask = 0;
	unsigned int i;
	int ret;

	remote = media_pad_remote_pad_first(&state->pads[XCSI_PAD_SINK]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return XCSI_VCSELR_VCSEL;

	subdev = media_entity_to_v4l2_subdev(remote->entity);

	ret = v4l2_subdev_call(subdev, pad, get_frame_desc, remote->index, &fd);
	if (ret || fd.type != V4L2_MBUS_FRAME_DESC_TYPE_CSI2)
		return XCSI_VCSELR_VCSEL;

	for (i = 0; i < fd.num_entries; ++i) {
		const struct v4l2_mbus_frame_desc_entry *entry = &fd.entry[i];

		if (!(streams_mask & BIT_ULL(entry->stream)))
			continue;

		found_streams |= BIT_ULL(entry->stream);
		vc_mask |= BIT(entry->bus.csi2.vc);
	}

	if (found_streams != streams_mask) {
		dev_dbg(state->dev,
			"streams %#llx not described by the frame descriptor\n",
			streams_mask & ~found_streams);
		return XCSI_VCSELR_VCSEL;
	}

	return vc_mask;
}

/*
 * Program the virtual channels the core accepts. The packets of the other
 * virtual channels are dropped, instead of filling the stream line buffer up
 * and taking the core down, when no entity downstream consumes them.
 *
 * This only has an effect on cores synthesized to allow all the virtual
 * channels. The others filter by virtual channel on their own, and the register
 * reads back its reset value.
 */
static void xcsi2rxss_set_vc_mask(struct xcsi2rxss_state *state,
				  u64 streams_mask)
{
	u16 vc_mask = xcsi2rxss_get_vc_mask(state, streams_mask);

	dev_dbg(state->dev, "streams %#llx: accepting virtual channels %#x\n",
		streams_mask, vc_mask);

	xcsi2rxss_write(state, XCSI_VCSELR_OFFSET, vc_mask);
}

static int xcsi2rxss_enable_streams(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    u32 pad, u64 streams_mask)
{
	struct xcsi2rxss_state *xcsi2rxss = to_xcsi2rxssstate(sd);
	struct media_pad *remote;
	struct v4l2_subdev *subdev;
	u64 streams = streams_mask;
	u64 sink_streams;
	int ret;

	remote = media_pad_remote_pad_first(&xcsi2rxss->pads[XCSI_PAD_SINK]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return -EPIPE;

	subdev = media_entity_to_v4l2_subdev(remote->entity);

	sink_streams = v4l2_subdev_state_xlate_streams(sd_state, pad,
						       XCSI_PAD_SINK,
						       &streams);

	if (!xcsi2rxss->enabled_sink_streams) {
		xcsi2rxss_reset_event_counters(xcsi2rxss);

		ret = xcsi2rxss_start_core(xcsi2rxss);
		if (ret)
			return ret;
	}

	/* Accept the packets of the new streams before the source sends them. */
	xcsi2rxss_set_vc_mask(xcsi2rxss,
			      xcsi2rxss->enabled_sink_streams | sink_streams);

	ret = v4l2_subdev_enable_streams(subdev, remote->index, sink_streams);
	if (ret) {
		if (!xcsi2rxss->enabled_sink_streams)
			xcsi2rxss_stop_core(xcsi2rxss);
		else
			xcsi2rxss_set_vc_mask(xcsi2rxss,
					      xcsi2rxss->enabled_sink_streams);

		return ret;
	}

	xcsi2rxss->enabled_sink_streams |= sink_streams;

	return 0;
}

static int xcsi2rxss_disable_streams(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     u32 pad, u64 streams_mask)
{
	struct xcsi2rxss_state *xcsi2rxss = to_xcsi2rxssstate(sd);
	struct media_pad *remote;
	u64 streams = streams_mask;
	u64 sink_streams;

	sink_streams = v4l2_subdev_state_xlate_streams(sd_state, pad,
						       XCSI_PAD_SINK,
						       &streams);

	/*
	 * Drop the packets of the streams being disabled before the entities
	 * downstream of them stop, as they have nothing left to consume them.
	 * A virtual channel is kept as long as any of the streams it carries is
	 * still enabled.
	 */
	xcsi2rxss->enabled_sink_streams &= ~sink_streams;
	xcsi2rxss_set_vc_mask(xcsi2rxss, xcsi2rxss->enabled_sink_streams);

	remote = media_pad_remote_pad_first(&xcsi2rxss->pads[XCSI_PAD_SINK]);
	if (remote && is_media_entity_v4l2_subdev(remote->entity)) {
		struct v4l2_subdev *subdev;

		subdev = media_entity_to_v4l2_subdev(remote->entity);
		v4l2_subdev_disable_streams(subdev, remote->index,
					    sink_streams);
	}

	if (!xcsi2rxss->enabled_sink_streams)
		xcsi2rxss_stop_core(xcsi2rxss);

	return 0;
}

static int __xcsi2rxss_set_routing(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_krouting *routing)
{
	struct xcsi2rxss_state *xcsi2rxss = to_xcsi2rxssstate(sd);
	const struct v4l2_mbus_framefmt format = {
		.code = xcsi2rxss_get_nth_mbus(xcsi2rxss->datatype, 0),
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.width = XCSI_DEFAULT_WIDTH,
		.height = XCSI_DEFAULT_HEIGHT,
	};
	int ret;

	/*
	 * The subsystem passes every stream through unmodified, so routes
	 * map sink streams 1:1 to source streams. The hardware routes
	 * embedded data packets to the embedded data output on its own;
	 * nothing to configure for routes to the embedded data pad either.
	 */
	ret = v4l2_subdev_routing_validate(sd, routing,
					   V4L2_SUBDEV_ROUTING_ONLY_1_TO_1);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(sd, sd_state, routing,
						&format);
}

static int xcsi2rxss_set_routing(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 enum v4l2_subdev_format_whence which,
				 struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&sd->entity))
		return -EBUSY;

	return __xcsi2rxss_set_routing(sd, sd_state, routing);
}

static int xcsi2rxss_init_state(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.sink_pad = XCSI_PAD_SINK,
			.sink_stream = 0,
			.source_pad = XCSI_PAD_SOURCE,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};
	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};

	/*
	 * Default to a single route for a single-stream (virtual channel 0)
	 * pipeline. This keeps the subsystem usable without routing setup,
	 * also with stream-unaware upstream and downstream entities.
	 */
	return __xcsi2rxss_set_routing(sd, sd_state, &routing);
}

/**
 * xcsi2rxss_set_format - This is used to set the pad format
 * @sd: Pointer to V4L2 Sub device structure
 * @sd_state: Pointer to sub device state structure
 * @fmt: Pointer to pad level media bus format
 *
 * This function is used to set the pad format for the CSI-2 Rx subsystem.
 * Only the sink pad format can be set: the function validates the requested
 * format code against the hardware's configured datatype, applies the requested
 * format and propagates it to the source pad. Setting the source pad format is
 * a no-op, as the source pad always mirrors the sink pad.
 *
 * Return: 0 on success
 */
static int xcsi2rxss_set_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct xcsi2rxss_state *xcsi2rxss = to_xcsi2rxssstate(sd);
	struct v4l2_mbus_framefmt *__format;
	u32 dt;

	/*
	 * Only the sink pad format can be set. The source pads always mirror
	 * the routed sink stream, so report their current format unchanged.
	 */
	if (fmt->pad != XCSI_PAD_SINK)
		return v4l2_subdev_get_fmt(sd, sd_state, fmt);

	/*
	 * Only the format->code parameter matters for CSI as the CSI format
	 * cannot be changed at runtime.
	 *
	 * RAW8 is supported in all datatypes. So if requested media bus format
	 * is of RAW8 type, then allow to be set. In case core is configured to
	 * other RAW, YUV422 8/10 or RGB888, set appropriate media bus format.
	 * Embedded data packets pass through regardless of the configured
	 * pixel data type, so metadata formats are always allowed too.
	 */
	dt = xcsi2rxss_get_dt(fmt->format.code);
	if (dt != xcsi2rxss->datatype && dt != MIPI_CSI2_DT_RAW8 &&
	    dt != MIPI_CSI2_DT_EMBEDDED_8B) {
		dev_dbg(xcsi2rxss->dev, "Unsupported media bus format");
		/* set the default format for the data type */
		fmt->format.code = xcsi2rxss_get_nth_mbus(xcsi2rxss->datatype,
							  0);
	}

	/*
	 * Store the format on the sink stream and propagate it to the routed
	 * source stream.
	 */
	__format = v4l2_subdev_state_get_format(sd_state, fmt->pad,
						fmt->stream);
	if (!__format)
		return -EINVAL;

	*__format = fmt->format;

	__format = v4l2_subdev_state_get_opposite_stream_format(sd_state,
								fmt->pad,
								fmt->stream);
	if (!__format)
		return -EINVAL;

	*__format = fmt->format;

	return 0;
}

/**
 * xcsi2rxss_enum_mbus_code - Handle pixel format enumeration
 * @sd: pointer to v4l2 subdev structure
 * @sd_state: V4L2 subdev pad configuration
 * @code: pointer to v4l2_subdev_mbus_code_enum structure
 *
 * Return: -EINVAL or zero on success
 */
static int xcsi2rxss_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	struct xcsi2rxss_state *state = to_xcsi2rxssstate(sd);
	u32 dt, n;
	int ret = 0;

	/*
	 * The media bus code on the source pads is identical to the routed
	 * sink stream.
	 */
	if (code->pad != XCSI_PAD_SINK) {
		const struct v4l2_mbus_framefmt *format;

		if (code->index > 0)
			return -EINVAL;

		format = v4l2_subdev_state_get_opposite_stream_format(sd_state,
								      code->pad,
								      code->stream);
		if (!format)
			return -EINVAL;

		code->code = format->code;

		return 0;
	}

	/* RAW8 dt packets are available in all DT configurations */
	if (code->index < 4) {
		n = code->index;
		dt = MIPI_CSI2_DT_RAW8;
	} else if (state->datatype != MIPI_CSI2_DT_RAW8) {
		n = code->index - 4;
		dt = state->datatype;
	} else {
		return -EINVAL;
	}

	code->code = xcsi2rxss_get_nth_mbus(dt, n);
	if (!code->code)
		ret = -EINVAL;

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations xcsi2rxss_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

static const struct v4l2_subdev_core_ops xcsi2rxss_core_ops = {
	.log_status = xcsi2rxss_log_status,
};

static const struct v4l2_subdev_pad_ops xcsi2rxss_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xcsi2rxss_set_format,
	.enum_mbus_code = xcsi2rxss_enum_mbus_code,
	.link_validate = v4l2_subdev_link_validate_default,
	.set_routing = xcsi2rxss_set_routing,
	.enable_streams = xcsi2rxss_enable_streams,
	.disable_streams = xcsi2rxss_disable_streams,
};

static const struct v4l2_subdev_ops xcsi2rxss_ops = {
	.core = &xcsi2rxss_core_ops,
	.pad = &xcsi2rxss_pad_ops
};

static const struct v4l2_subdev_internal_ops xcsi2rxss_internal_ops = {
	.init_state = xcsi2rxss_init_state,
};

static int xcsi2rxss_parse_of(struct xcsi2rxss_state *xcsi2rxss)
{
	struct device *dev = xcsi2rxss->dev;
	struct device_node *node = dev->of_node;
	struct v4l2_fwnode_endpoint vep = { };
	struct fwnode_handle *ep;
	bool en_csi_v20;
	u32 phy_mode;
	int ret;

	if (xcsi2rxss->cfg->flags & XCSI_HAS_PHY_MODE) {
		ret = device_property_read_u32(dev, "xlnx,mipi-rx-phy-mode",
					       &phy_mode);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing xlnx,mipi-rx-phy-mode property\n");

		if (phy_mode == XCSI_PHY_MODE_CPHY) {
			xcsi2rxss->is_cphy = true;
			vep.bus_type = V4L2_MBUS_CSI2_CPHY;
		} else if (phy_mode == XCSI_PHY_MODE_DPHY) {
			xcsi2rxss->is_cphy = false;
			vep.bus_type = V4L2_MBUS_CSI2_DPHY;
		} else {
			return dev_err_probe(dev, -EINVAL,
					     "invalid xlnx,mipi-rx-phy-mode %u\n",
					     phy_mode);
		}
	} else {
		xcsi2rxss->is_cphy = false;
		vep.bus_type = V4L2_MBUS_CSI2_DPHY;
	}

	en_csi_v20 = of_property_read_bool(node, "xlnx,en-csi-v2-0");
	if (en_csi_v20)
		xcsi2rxss->en_vcx = of_property_read_bool(node, "xlnx,en-vcx");

	xcsi2rxss->enable_active_lanes =
		of_property_read_bool(node, "xlnx,en-active-lanes");

	ret = of_property_read_u32(node, "xlnx,csi-pxl-format",
				   &xcsi2rxss->datatype);
	if (ret < 0) {
		dev_err(dev, "missing xlnx,csi-pxl-format property\n");
		return ret;
	}

	switch (xcsi2rxss->datatype) {
	case MIPI_CSI2_DT_YUV420_8B:
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

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     XCSI_PAD_SINK, 0,
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

	dev_dbg(dev, "mipi number %s = %d\n",
		xcsi2rxss->is_cphy ? "trios" : "lanes",
		vep.bus.mipi_csi2.num_data_lanes);

	xcsi2rxss->max_num_lanes = vep.bus.mipi_csi2.num_data_lanes;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     XCSI_PAD_SOURCE, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		dev_err(dev, "no source port found");
		return -EINVAL;
	}

	fwnode_handle_put(ep);

	/* The embedded data source port is optional. */
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
					     XCSI_PAD_SOURCE_EMB, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (ep) {
		xcsi2rxss->has_embdata = true;
		fwnode_handle_put(ep);
	}

	dev_dbg(dev, "phy mode %s, vcx %s, %u data %s (%s), data type 0x%02x\n",
		xcsi2rxss->is_cphy ? "C-PHY" : "D-PHY",
		xcsi2rxss->en_vcx ? "enabled" : "disabled",
		xcsi2rxss->max_num_lanes,
		xcsi2rxss->is_cphy ? "trios" : "lanes",
		xcsi2rxss->enable_active_lanes ? "dynamic" : "static",
		xcsi2rxss->datatype);

	return 0;
}

static int xcsi2rxss_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct xcsi2rxss_state *xcsi2rxss;
	int num_clks = ARRAY_SIZE(xcsi2rxss_clks);
	struct device *dev = &pdev->dev;
	int irq, ret;

	xcsi2rxss = devm_kzalloc(dev, sizeof(*xcsi2rxss), GFP_KERNEL);
	if (!xcsi2rxss)
		return -ENOMEM;

	xcsi2rxss->dev = dev;

	xcsi2rxss->clks = devm_kmemdup(dev, xcsi2rxss_clks,
				       sizeof(xcsi2rxss_clks), GFP_KERNEL);
	if (!xcsi2rxss->clks)
		return -ENOMEM;

	/* Reset GPIO */
	xcsi2rxss->rst_gpio = devm_gpiod_get_optional(dev, "video-reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(xcsi2rxss->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(xcsi2rxss->rst_gpio),
				     "Video Reset GPIO not setup in DT\n");

	xcsi2rxss->cfg = device_get_match_data(dev);
	if (!xcsi2rxss->cfg)
		return dev_err_probe(dev, -ENODEV, "no match data found\n");

	ret = xcsi2rxss_parse_of(xcsi2rxss);
	if (ret < 0)
		return ret;

	xcsi2rxss->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xcsi2rxss->iomem))
		return PTR_ERR(xcsi2rxss->iomem);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(dev, irq, NULL,
					xcsi2rxss_irq_handler, IRQF_ONESHOT,
					dev_name(dev), xcsi2rxss);
	if (ret) {
		dev_err(dev, "Err = %d Interrupt handler reg failed!\n", ret);
		return ret;
	}

	ret = clk_bulk_get(dev, num_clks, xcsi2rxss->clks);
	if (ret)
		return ret;

	/* TODO: Enable/disable clocks at stream on/off time. */
	ret = clk_bulk_prepare_enable(num_clks, xcsi2rxss->clks);
	if (ret)
		goto err_clk_put;

	xcsi2rxss_hard_reset(xcsi2rxss);
	xcsi2rxss_soft_reset(xcsi2rxss);

	/* Initialize media pads */
	xcsi2rxss->pads[XCSI_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	xcsi2rxss->pads[XCSI_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	xcsi2rxss->pads[XCSI_PAD_SOURCE_EMB].flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize V4L2 subdevice and media entity */
	subdev = &xcsi2rxss->subdev;
	v4l2_subdev_init(subdev, &xcsi2rxss_ops);
	subdev->internal_ops = &xcsi2rxss_internal_ops;
	subdev->dev = dev;
	strscpy(subdev->name, dev_name(dev), sizeof(subdev->name));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_EVENTS | V4L2_SUBDEV_FL_HAS_DEVNODE |
			 V4L2_SUBDEV_FL_STREAMS;
	subdev->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	subdev->entity.ops = &xcsi2rxss_media_ops;
	v4l2_set_subdevdata(subdev, xcsi2rxss);

	ret = media_entity_pads_init(&subdev->entity,
				     xcsi2rxss->has_embdata ? 3 : 2,
				     xcsi2rxss->pads);
	if (ret < 0)
		goto err_clk_disable;

	/* Allocate the subdev active state and populate the default format. */
	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_media_cleanup;

	platform_set_drvdata(pdev, xcsi2rxss);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto error_subdev_cleanup;
	}

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(subdev);
error_media_cleanup:
	media_entity_cleanup(&subdev->entity);
err_clk_disable:
	clk_bulk_disable_unprepare(num_clks, xcsi2rxss->clks);
err_clk_put:
	clk_bulk_put(num_clks, xcsi2rxss->clks);
	return ret;
}

static void xcsi2rxss_remove(struct platform_device *pdev)
{
	struct xcsi2rxss_state *xcsi2rxss = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xcsi2rxss->subdev;
	int num_clks = ARRAY_SIZE(xcsi2rxss_clks);

	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	media_entity_cleanup(&subdev->entity);
	clk_bulk_disable_unprepare(num_clks, xcsi2rxss->clks);
	clk_bulk_put(num_clks, xcsi2rxss->clks);
}

static const struct of_device_id xcsi2rxss_of_id_table[] = {
	{ .compatible = "xlnx,mipi-csi2-rx-subsystem-6.0",
	  .data = &xcsi2rxss_cfg_v60 },
	{ .compatible = "xlnx,mipi-csi2-rx-subsystem-5.0",
	  .data = &xcsi2rxss_cfg_v50 },
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
