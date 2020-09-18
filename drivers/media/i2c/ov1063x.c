// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV10633/OV10635 Camera Driver
 *
 * Based on the original driver written by Phil Edworthy.
 * Copyright (C) 2013 Phil Edworthy
 * Copyright (C) 2013 Renesas Electronics
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
 * Copyright (C) 2020 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This driver has been tested at QVGA, VGA and 720p, and 1280x800 at up to
 * 30fps and it should work at any resolution in between and any frame rate
 * up to 30fps.
 */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/v4l2-mediabus.h>
#include <linux/videodev2.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Register definitions */
#define OV1063X_REG_8BIT(n)			((1 << 16) | (n))
#define OV1063X_REG_16BIT(n)			((2 << 16) | (n))
#define OV1063X_REG_24BIT(n)			((3 << 16) | (n))
#define OV1063X_REG_32BIT(n)			((4 << 16) | (n))
#define OV1063X_REG_SIZE_SHIFT			16
#define OV1063X_REG_ADDR_MASK			0xffff

#define OV1063X_STREAM_MODE			OV1063X_REG_8BIT(0x0100)
#define OV1063X_STREAM_MODE_ON			BIT(0)
#define OV1063X_SOFTWARE_RESET			OV1063X_REG_8BIT(0x0103)

#define OV1063X_SC_CMMN_PLL_CTRL0		OV1063X_REG_8BIT(0x3003)
#define OV1063X_SC_CMMN_PLL_SCLK_CP(n)		((n) << 6)
#define OV1063X_SC_CMMN_PLL_SCLK_MULTI(n)	((n) << 0)
#define OV1063X_SC_CMMN_PLL_CTRL1		OV1063X_REG_8BIT(0x3004)
#define OV1063X_SC_CMMN_PLL_SCLK_BYPASS		BIT(7)
#define OV1063X_SC_CMMN_PLL_SCLK_PRE_DIV(n)	((n) << 4)	/* /1, /1.5, /2, /3, /4, /5, /6, /7 */
#define OV1063X_SC_CMMN_PLL_SCLK_CP2(n)		((n) << 3)
#define OV1063X_SC_CMMN_PLL_SCLK_DIV(n)		((n) << 0)	/* Divider = 2 * (1 + n) */
#define OV1063X_SC_CMMN_PLL_CTRL2		OV1063X_REG_8BIT(0x3005)
#define OV1063X_SC_CMMN_PLL_PCLK_CP(n)		((n) << 6)
#define OV1063X_SC_CMMN_PLL_PCLK_MULTI(n)	((n) << 0)
#define OV1063X_SC_CMMN_PLL_CTRL3		OV1063X_REG_8BIT(0x3006)
#define OV1063X_SC_CMMN_PLL_PCLK_BYPASS		BIT(7)
#define OV1063X_SC_CMMN_PLL_PCLK_PRE_DIV(n)	((n) << 4)	/* /1, /1.5, /2, /3, /4, /5, /6, /7 */
#define OV1063X_SC_CMMN_PLL_PCLK_CP2(n)		((n) << 3)
#define OV1063X_SC_CMMN_PLL_PCLK_DIV(n)		((n) << 0)	/* Divider = 2 * (1 + n) */
#define OV1063X_SC_CMMN_PCLK_DIV_CTRL		OV1063X_REG_8BIT(0x3007)
#define OV1063X_PID				OV1063X_REG_8BIT(0x300a)
#define OV1063X_VER				OV1063X_REG_8BIT(0x300b)
#define OV1063X_SC_CMMN_SCCB_ID			OV1063X_REG_8BIT(0x300c)
#define OV1063X_SC_CMMN_SCCB_ID_ADDR(n)		((n) << 1)
#define OV1063X_SC_CMMN_SCCB_ID_SEL		BIT(0)
#define OV1063X_SC_CMMN_PAD			OV1063X_REG_8BIT(0x3011)
#define OV1063X_SC_CMMN_PAD_DRIVE(n)		((n) << 6)
#define OV1063X_SC_CMMN_CLKRST0			OV1063X_REG_8BIT(0x301a)
#define OV1063X_SC_CMMN_CLKRST0_SCLK		GENMASK(7, 4)
#define OV1063X_SC_CMMN_CLKRST0_RST		GENMASK(3, 0)
#define OV1063X_SC_CMMN_CLKRST1			OV1063X_REG_8BIT(0x301b)
#define OV1063X_SC_CMMN_CLKRST1_SCLK		GENMASK(7, 4)
#define OV1063X_SC_CMMN_CLKRST1_RST		GENMASK(3, 0)
#define OV1063X_SC_CMMN_CLKRST2			OV1063X_REG_8BIT(0x301c)
#define OV1063X_SC_CMMN_CLKRST2_PCLK_DVP	BIT(7)
#define OV1063X_SC_CMMN_CLKRST2_SCLK		GENMASK(6, 4)
#define OV1063X_SC_CMMN_CLKRST2_RST_DVP		BIT(3)
#define OV1063X_SC_CMMN_CLKRST2_RST		GENMASK(2, 0)
#define OV1063X_SC_CMMN_CLOCK_SEL		OV1063X_REG_8BIT(0x3020)
#define OV1063X_SC_CMMN_MISC_CTRL		OV1063X_REG_8BIT(0x3021)
#define OV1063X_SC_CMMN_MISC_CTRL_PCLK_INV	BIT(7)
#define OV1063X_SC_CMMN_MISC_CTRL_SCLK_INV	BIT(6)
#define OV1063X_SC_CMMN_MISC_CTRL_SCLK2X_INV	BIT(5)
#define OV1063X_SC_CMMN_MISC_CTRL_CEN_GLOBAL_O	BIT(0)
#define OV1063X_SC_CMMN_CORE_CTRL		OV1063X_REG_8BIT(0x3024)
#define OV1063X_SC_CMMN_CORE_CTRL_RAW_LONG	(0U << 4)
#define OV1063X_SC_CMMN_CORE_CTRL_RAW_SHORT	(1U << 4)
#define OV1063X_SC_CMMN_CORE_CTRL_RAW_LONG_SHORT	(2U << 4)
#define OV1063X_SC_CMMN_CORE_CTRL_RAW_COMBINED	(0U << 4)
#define OV1063X_SC_CMMN_CORE_CTRL_YUV_LONG	(1U << 1)
#define OV1063X_SC_CMMN_CORE_CTRL_YUV_SHORT	(2U << 1)
#define OV1063X_SC_CMMN_CORE_CTRL_PCLK_SYS	(0U << 0)	/* PCLK from system PLL */
#define OV1063X_SC_CMMN_CORE_CTRL_PCLK_SEC	(1U << 0)	/* PCLK from secondary PLL */
#define OV1063X_SC_CMMN_CORE_CTRL1		OV1063X_REG_8BIT(0x3025)
#define OV1063X_SC_CMMN_PWDN_CTRL2		OV1063X_REG_8BIT(0x302d)
#define OV1063X_SC_CMMN_PWDN_CTRL2_RST_DIG1	BIT(3)
#define OV1063X_SC_CMMN_PWDN_CTRL2_RST_DIG2	BIT(2)
#define OV1063X_SC_CMMN_PWDN_CTRL2_RST_ISP	BIT(1)
#define OV1063X_SC_CMMN_PWDN_CTRL2_SEQUENCE	BIT(0)
#define OV1063X_SC_CMMN_SCLK2X_SEL		OV1063X_REG_8BIT(0x3033)
#define OV1063X_SC_CMMN_SCLK2X_SEL_DIV2		(1U << 2)
#define OV1063X_SC_CMMN_SCLK2X_SEL_DIV4		(2U << 2)
#define OV1063X_SC_SOC_CLKRST7			OV1063X_REG_8BIT(0x3042)
#define OV1063X_SC_SOC_CLKRST7_SCLK		GENMASK(7, 4)
#define OV1063X_SC_SOC_CLKRST7_RST		GENMASK(3, 0)

#define OV1063X_AEC_PK_MANUAL			OV1063X_REG_8BIT(0x3503)
#define OV1063X_AEC_PK_MANUAL_GAIN_DELAY	BIT(5)
#define OV1063X_AEC_PK_MANUAL_DELAY		BIT(4)

#define OV1063X_ANA_ADC1			OV1063X_REG_8BIT(0x3600)
#define OV1063X_ANA_ADC2			OV1063X_REG_8BIT(0x3601)
#define OV1063X_ANA_ADC3			OV1063X_REG_8BIT(0x3602)
#define OV1063X_ANA_ADC4			OV1063X_REG_8BIT(0x3603)
#define OV1063X_ANA_ANALOG1			OV1063X_REG_8BIT(0x3610)
#define OV1063X_ANA_ANALOG2			OV1063X_REG_8BIT(0x3611)
#define OV1063X_ANA_ANALOG3			OV1063X_REG_8BIT(0x3612)
#define OV1063X_ANA_ARRAY1			OV1063X_REG_8BIT(0x3621)
#define OV1063X_ANA_ARRAY1_FULL			(0 << 3)
#define OV1063X_ANA_ARRAY1_CROP_768		(1 << 3)
#define OV1063X_ANA_ARRAY1_CROP_656		(2 << 3)
#define OV1063X_ANA_ARRAY1_DELAY(n)		((n) << 0)
#define OV1063X_ANA_PWC1			OV1063X_REG_8BIT(0x3630)
#define OV1063X_ANA_PWC2			OV1063X_REG_8BIT(0x3631)
#define OV1063X_ANA_PWC3			OV1063X_REG_8BIT(0x3632)
#define OV1063X_ANA_PWC4			OV1063X_REG_8BIT(0x3633)

#define OV1063X_SENSOR_RSTGOLOW			OV1063X_REG_8BIT(0x3702)
#define OV1063X_SENSOR_HLDWIDTH			OV1063X_REG_8BIT(0x3703)
#define OV1063X_SENSOR_TXWIDTH			OV1063X_REG_8BIT(0x3704)
#define OV1063X_SENSOR_REG9			OV1063X_REG_8BIT(0x3709)
#define OV1063X_SENSOR_REGD			OV1063X_REG_8BIT(0x370d)
#define OV1063X_SENSOR_RSTYZ_GOLOW		OV1063X_REG_16BIT(0x3712)
#define OV1063X_SENSOR_EQ_GOLOW			OV1063X_REG_8BIT(0x3714)
#define OV1063X_SENSOR_REG15			OV1063X_REG_8BIT(0x3715)
#define OV1063X_SENSOR_BITSW_GO			OV1063X_REG_16BIT(0x371c)

#define OV1063X_TIMING_X_START_ADDR		OV1063X_REG_16BIT(0x3800)
#define OV1063X_TIMING_Y_START_ADDR		OV1063X_REG_16BIT(0x3802)
#define OV1063X_TIMING_X_END_ADDR		OV1063X_REG_16BIT(0x3804)
#define OV1063X_TIMING_Y_END_ADDR		OV1063X_REG_16BIT(0x3806)
#define OV1063X_TIMING_X_OUTPUT_SIZE		OV1063X_REG_16BIT(0x3808)
#define OV1063X_TIMING_Y_OUTPUT_SIZE		OV1063X_REG_16BIT(0x380a)
#define OV1063X_TIMING_HTS			OV1063X_REG_16BIT(0x380c)
#define OV1063X_TIMING_VTS			OV1063X_REG_16BIT(0x380e)
#define	OV1063X_TIMING_CTRL15			OV1063X_REG_8BIT(0x3815)
#define	OV1063X_TIMING_CTRL15_BLACK_LINE_HREF	BIT(7)
#define	OV1063X_TIMING_CTRL15_RIP_SOF		BIT(5)
#define	OV1063X_TIMING_CTRL15_BLACK_LINES(n)	((n) << 0)
#define	OV1063X_TIMING_CTRL1C			OV1063X_REG_8BIT(0x381c)
#define	OV1063X_TIMING_CTRL1C_VFLIP_DIG		BIT(7)
#define	OV1063X_TIMING_CTRL1C_VFLIP_ARRAY	BIT(6)
#define	OV1063X_TIMING_CTRL1C_VSUB4		BIT(1)
#define	OV1063X_TIMING_CTRL1C_VSUB2		BIT(0)
#define	OV1063X_TIMING_CTRL1D			OV1063X_REG_8BIT(0x381d)
#define	OV1063X_TIMING_CTRL1D_VFLIP_BLACK_LINE	BIT(7)
#define	OV1063X_TIMING_CTRL1D_WDR		BIT(6)
#define	OV1063X_TIMING_CTRL1D_HFLIP_DIG		BIT(1)
#define	OV1063X_TIMING_CTRL1D_HFLIP_ARRAY	BIT(0)
#define	OV1063X_VSTART_OFFSET			OV1063X_REG_16BIT(0x381e)

#define	OV1063X_START_LINE			OV1063X_REG_8BIT(0x4001)
#define	OV1063X_LINE_NUM			OV1063X_REG_8BIT(0x4004)	/* Black lines */
#define	OV1063X_BLC_AVG_CTRL1			OV1063X_REG_8BIT(0x4050)
#define	OV1063X_BLC_AVG_CTRL2			OV1063X_REG_8BIT(0x4051)

#define OV1063X_FORMAT_CTRL00			OV1063X_REG_8BIT(0x4300)
#define OV1063X_FORMAT_YUYV			0x38
#define OV1063X_FORMAT_YYYU			0x39
#define OV1063X_FORMAT_UYVY			0x3a
#define OV1063X_FORMAT_VYUY			0x3b
#define	OV1063X_FORMAT_YMAX			OV1063X_REG_16BIT(0x4302)
#define	OV1063X_FORMAT_YMIN			OV1063X_REG_16BIT(0x4304)
#define	OV1063X_FORMAT_UMAX			OV1063X_REG_16BIT(0x4306)
#define	OV1063X_FORMAT_UMIN			OV1063X_REG_16BIT(0x4308)

#define OV1063X_VFIFO_LLEN_FIRS1_SEL		OV1063X_REG_8BIT(0x4605)
#define OV1063X_VFIFO_LLEN_FIRS1_SEL_8B_YUV	BIT(3)
#define OV1063X_VFIFO_LINE_LENGTH_MAN		OV1063X_REG_16BIT(0x4606)
#define OV1063X_VFIFO_HSYNC_START_POSITION	OV1063X_REG_16BIT(0x460a)

#define OV1063X_DVP_MOD_SEL			OV1063X_REG_8BIT(0x4700)
#define OV1063X_DVP_MOD_SEL_CCIR_V		BIT(3)
#define OV1063X_DVP_MOD_SEL_CCIR_F		BIT(2)
#define OV1063X_DVP_MOD_SEL_CCIR_656		BIT(1)
#define OV1063X_DVP_MOD_SEL_HSYNC		BIT(0)

#define	OV1063X_ISP_RW00			OV1063X_REG_8BIT(0x5000)
#define	OV1063X_ISP_RW00_COLOR_MATRIX_EN	BIT(7)
#define	OV1063X_ISP_RW00_COLOR_INTERP_EN	BIT(6)
#define	OV1063X_ISP_RW00_DENOISE_EN		BIT(5)
#define	OV1063X_ISP_RW00_WHITE_DPC_EN		BIT(4)	/* White defect pixel correction enable */
#define	OV1063X_ISP_RW00_BLACK_DPC_EN		BIT(3)	/* Black defect pixel connection enable */
#define	OV1063X_ISP_RW00_AWB_STATS_EN		BIT(2)
#define	OV1063X_ISP_RW00_AWB_GAIN_EN		BIT(1)
#define	OV1063X_ISP_RW00_LSC_EN			BIT(0)
#define	OV1063X_ISP_RW05			OV1063X_REG_8BIT(0x5005)
#define	OV1063X_ISP_RW05_VERT_SUB_EN		BIT(7)	/* Enable vertical subsampling */
#define	OV1063X_ISP_RW05_LSC_CENTER_AUTO	BIT(6)	/* Set LSC center automatically based on image window */
#define	OV1063X_ISP_RW05_SUB_OUT_ROW_2ND	BIT(5)	/* Output 2nd (1) or 1st (0) row when skipping */
#define	OV1063X_ISP_RW05_SUB_OUT_COL_2ND	BIT(4)	/* Output 2nd (1) or 1st (0) column when skipping */
#define	OV1063X_ISP_RW05_SUB_AVG		BIT(3)	/* Average (1) or sum (0) when binning */
#define	OV1063X_ISP_RW05_SUB_G_DROP		BIT(2)	/* Skip (1) or bin (0) Green / Y */
#define	OV1063X_ISP_RW05_SUB_RB_DROP		BIT(1)	/* Skip (1) or bin (0) Red Blue / UV */
#define	OV1063X_ISP_RW05_SUB_ENABLE		BIT(0)	/* Enable sub-sampling */
#define	OV1063X_ISP_CTRL3D			OV1063X_REG_8BIT(0x503d)
#define	OV1063X_ISP_CTRL3D_TEST_PATTERN_EN	BIT(7)
#define	OV1063X_ISP_CTRL3D_COLOR_BAR(n)		((n) << 4)
#define	OV1063X_ISP_CTRL3D_ROLLING_BAR_EN	BIT(2)

#define	OV1063X_GAIN_AWB_CTRL32			OV1063X_REG_8BIT(0x5120)
#define	OV1063X_GAIN_AWB_CTRL32_MANUAL_EN	BIT(0)

#define	OV1063X_AEC_CTRLD0			OV1063X_REG_8BIT(0x56d0)
#define	OV1063X_AEC_CTRLD0_R_MAN_EN(n)		((n) << 0)

#define	OV1063X_HORIZ_COLORCORRECT		OV1063X_REG_8BIT(0x6900)
#define OV1063X_HORIZ_COLORCORRECT_ON		BIT(0)

#define OV1063X_MAX_EXP_LONG			OV1063X_REG_16BIT(0xc488)
#define OV1063X_MAX_EXP_SHORT			OV1063X_REG_16BIT(0xc48a)
#define OV1063X_SIMPLE_MIN_NUM			OV1063X_REG_16BIT(0xc4cc)
#define OV1063X_CT_MIN_NUM			OV1063X_REG_16BIT(0xc4ce)

#define OV1063X_VTS_ADDR			OV1063X_REG_16BIT(0xc518)
#define OV1063X_HTS_ADDR			OV1063X_REG_16BIT(0xc51a)

#include "ov1063x_regs.h"

/* IDs */
#define OV10633_VERSION_REG			0xa630
#define OV10635_VERSION_REG			0xa635
#define OV1063X_VERSION(pid, ver)		(((pid) << 8) | ((ver) & 0xff))

enum ov1063x_model {
	SENSOR_OV10633,
	SENSOR_OV10635,
};

#define OV1063X_SENSOR_WIDTH			1312
#define OV1063X_SENSOR_HEIGHT			814

#define OV1063X_MAX_WIDTH			1280
#define OV1063X_MAX_HEIGHT			800

struct ov1063x_priv {
	struct device			*dev;

	struct regmap			*regmap;
	struct clk			*clk;
	struct gpio_desc		*reset_gpio;
	struct gpio_desc		*pwdn_gpio;

	int				model;
	unsigned long			clk_rate;

	struct v4l2_subdev		subdev;
	struct media_pad		pad;

	struct v4l2_ctrl_handler	hdl;
	struct v4l2_ctrl		*colorbar;

	/*
	 * The streaming and format fields are protected by the control handler
	 * lock.
	 */
	bool				streaming;
	struct v4l2_mbus_framefmt	format;

	int				fps_numerator;
	int				fps_denominator;
};

static const struct v4l2_area ov1063x_framesizes[] = {
	{
		.width		= 1280,
		.height		= 800,
	}, {
		.width		= 1280,
		.height		= 720,
	}, {
		.width		= 752,
		.height		= 480,
	}, {
		.width		= 640,
		.height		= 480,
	}, {
		.width		= 600,
		.height		= 400,
	}, {
		.width		= 352,
		.height		= 288,
	}, {
		.width		= 320,
		.height		= 240,
	},
};

/*
 * supported color format list
 */
static const u32 ov1063x_mbus_formats[] = {
	MEDIA_BUS_FMT_YUYV8_2X8,
	MEDIA_BUS_FMT_UYVY8_2X8,
	MEDIA_BUS_FMT_VYUY8_2X8,
	MEDIA_BUS_FMT_YVYU8_2X8,
	MEDIA_BUS_FMT_YUYV10_2X10,
};

static inline struct ov1063x_priv *to_ov1063x(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov1063x_priv, subdev);
}

/* -----------------------------------------------------------------------------
 * Read/Write Helpers
 */

static int ov1063x_read(struct ov1063x_priv *priv, u32 reg, u32 *val)
{
	unsigned int len = (reg >> OV1063X_REG_SIZE_SHIFT) & 3;
	u16 addr = reg & OV1063X_REG_ADDR_MASK;
	unsigned int i;
	int ret;

	*val = 0;

	for (i = 0; i < len; ++i) {
		u32 byte;

		ret = regmap_read(priv->regmap, addr, &byte);
		if (ret)
			return ret;

		*val = (*val << 8) | byte;
		addr++;
	}

	return 0;
}

static int ov1063x_write(struct ov1063x_priv *priv, u32 reg, u32 val, int *err)
{
	unsigned int len = (reg >> OV1063X_REG_SIZE_SHIFT) & 7;
	u16 addr = reg & OV1063X_REG_ADDR_MASK;
	unsigned int shift = (len - 1) * 8;
	unsigned int i;
	int ret;

	if (err && *err)
		return *err;

	for (i = 0; i < len; ++i) {
		ret = regmap_write(priv->regmap, addr, (val >> shift) & 0xff);
		if (ret) {
			if (err)
				*err = ret;
			return ret;
		}

		shift -= 8;
		addr++;
	}

	return 0;
}

static int ov1063x_write_array(struct ov1063x_priv *priv,
			       const struct ov1063x_reg *regs,
			       unsigned int nr_regs)
{
	struct i2c_client *client = to_i2c_client(priv->dev);
	unsigned int i;
	int ret;
	u8 val;

	for (i = 0; i < nr_regs; i++) {
		if (regs[i].reg == OV1063X_SC_CMMN_SCCB_ID)
			val = OV1063X_SC_CMMN_SCCB_ID_ADDR(client->addr)
			    | OV1063X_SC_CMMN_SCCB_ID_SEL;
		else
			val = regs[i].val;

		ret = ov1063x_write(priv, regs[i].reg, val, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static int ov1063x_update(struct ov1063x_priv *priv, u32 reg, u32 mask, u32 val,
			  int *err)
{
	unsigned int len = (reg >> OV1063X_REG_SIZE_SHIFT) & 7;
	u16 addr = reg & OV1063X_REG_ADDR_MASK;
	unsigned int shift = (len - 1) * 8;
	unsigned int i;
	int ret;

	if (err && *err)
		return *err;

	for (i = 0; i < len; ++i) {
		ret = regmap_update_bits(priv->regmap, addr,
					 (mask >> shift) & 0xff,
					 (val >> shift) & 0xff);
		if (ret) {
			if (err)
				*err = ret;
			return ret;
		}

		shift -= 8;
		addr++;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Hardware Configuration
 */

/*
 * Get the best pixel clock (pclk) that meets minimum hts/vts requirements.
 * clk_rate => pre-divider => clk1 => multiplier => clk2 => post-divider
 * => pclk
 * We try all valid combinations of settings for the 3 blocks to get the pixel
 * clock, and from that calculate the actual hts/vts to use. The vts is
 * extended so as to achieve the required frame rate. The function also returns
 * the PLL register contents needed to set the pixel clock.
 */
static int ov1063x_get_pclk(int clk_rate, int *htsmin, int *vtsmin,
			    int fps_numerator, int fps_denominator,
			    u8 *r3003, u8 *r3004)
{
	int pre_divs[] = { 2, 3, 4, 6, 8, 10, 12, 14 };
	int pclk;
	int best_pclk = INT_MAX;
	int best_hts = 0;
	int i, j, k;
	int best_i = 0, best_j = 0, best_k = 0;
	int clk1, clk2;
	int hts;

	/* Pre-div, reg 0x3004, bits 6:4 */
	for (i = 0; i < ARRAY_SIZE(pre_divs); i++) {
		clk1 = (clk_rate / pre_divs[i]) * 2;

		if (clk1 < 3000000 || clk1 > 27000000)
			continue;

		/* Mult = reg 0x3003, bits 5:0 */
		for (j = 1; j < 32; j++) {
			clk2 = (clk1 * j);

			if (clk2 < 200000000 || clk2 > 500000000)
				continue;

			/* Post-div, reg 0x3004, bits 2:0 */
			for (k = 0; k < 8; k++) {
				pclk = clk2 / (2 * (k + 1));

				if (pclk > 96000000)
					continue;

				hts = *htsmin + 200 + pclk / 300000;

				/* 2 clock cycles for every YUV422 pixel */
				if (pclk < (((hts * *vtsmin) / fps_denominator)
					* fps_numerator * 2))
					continue;

				if (pclk < best_pclk) {
					best_pclk = pclk;
					best_hts = hts;
					best_i = i;
					best_j = j;
					best_k = k;
				}
			}
		}
	}

	/* register contents */
	*r3003 = (u8)best_j;
	*r3004 = ((u8)best_i << 4) | (u8)best_k;

	/* Did we get a valid PCLK? */
	if (best_pclk == INT_MAX)
		return -1;

	*htsmin = best_hts;

	/* Adjust vts to get as close to the desired frame rate as we can */
	*vtsmin = best_pclk / ((best_hts / fps_denominator) *
		  fps_numerator * 2);

	return best_pclk;
}

static int ov1063x_isp_reset(struct ov1063x_priv *priv, bool reset)
{
	unsigned int i;
	int ret = 0;

	if (!reset) {
		/*
		 * Enable ISP blocks. Why OV1063X_SC_SOC_CLKRST7 needs to be
		 * written 26 times is unknown.
		 */
		for (i = 0; i < 26; ++i)
			ov1063x_write(priv, OV1063X_SC_SOC_CLKRST7,
				      OV1063X_SC_SOC_CLKRST7_SCLK, &ret);

		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST1,
			      OV1063X_SC_CMMN_CLKRST1_SCLK, &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST2,
			      OV1063X_SC_CMMN_CLKRST2_PCLK_DVP |
			      OV1063X_SC_CMMN_CLKRST2_SCLK, &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST0,
			      OV1063X_SC_CMMN_CLKRST0_SCLK, &ret);
	} else {
		/* Reset the ISP. */
		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST1,
			      OV1063X_SC_CMMN_CLKRST1_SCLK |
			      OV1063X_SC_CMMN_CLKRST1_RST, &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST2,
			      OV1063X_SC_CMMN_CLKRST2_PCLK_DVP |
			      OV1063X_SC_CMMN_CLKRST2_SCLK |
			      OV1063X_SC_CMMN_CLKRST2_RST_DVP |
			      OV1063X_SC_CMMN_CLKRST2_RST, &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST0,
			      OV1063X_SC_CMMN_CLKRST0_SCLK |
			      OV1063X_SC_CMMN_CLKRST0_RST, &ret);
	}

	return ret;
}

/* Setup registers according to resolution and color encoding */
static int ov1063x_set_params(struct ov1063x_priv *priv)
{
	int pclk;
	int hts, vts;
	u8 r3003, r3004, r4300;
	int tmp;
	u32 height_pre_subsample;
	u32 width_pre_subsample;
	u8 horiz_crop_mode;
	int nr_isp_pixels;
	int vert_sub_sample = 0;
	int horiz_sub_sample = 0;
	int sensor_width;
	u32 width;
	u32 height;
	int ret = 0;

	width = priv->format.width;
	height = priv->format.height;

	/* Vertical sub-sampling? */
	height_pre_subsample = height;
	if (height <= 400) {
		vert_sub_sample = 1;
		height_pre_subsample <<= 1;
	}

	/* Horizontal sub-sampling? */
	width_pre_subsample = width;
	if (width <= 640) {
		horiz_sub_sample = 1;
		width_pre_subsample <<= 1;
	}

	/* Horizontal cropping */
	if (width_pre_subsample > 768) {
		sensor_width = OV1063X_SENSOR_WIDTH;
		horiz_crop_mode = 0x60 | OV1063X_ANA_ARRAY1_FULL
				| OV1063X_ANA_ARRAY1_DELAY(3);
	} else if (width_pre_subsample > 656) {
		sensor_width = 768;
		horiz_crop_mode = 0x60 | OV1063X_ANA_ARRAY1_CROP_768
				| OV1063X_ANA_ARRAY1_DELAY(3);
	} else {
		sensor_width = 656;
		horiz_crop_mode = 0x60 | OV1063X_ANA_ARRAY1_CROP_656
				| OV1063X_ANA_ARRAY1_DELAY(3);
	}

	/* minimum values for hts and vts */
	hts = sensor_width;
	vts = height_pre_subsample + 50;
	dev_dbg(priv->dev, "fps=(%d/%d), hts=%d, vts=%d\n",
		priv->fps_numerator, priv->fps_denominator, hts, vts);

	/* Get the best PCLK & adjust hts,vts accordingly */
	pclk = ov1063x_get_pclk(priv->clk_rate, &hts, &vts,
				priv->fps_numerator, priv->fps_denominator,
				&r3003, &r3004);
	if (pclk < 0)
		return -EINVAL;
	dev_dbg(priv->dev, "pclk=%d, hts=%d, vts=%d\n", pclk, hts, vts);
	dev_dbg(priv->dev, "r3003=0x%X r3004=0x%X\n", r3003, r3004);

	/* Reset the ISP. */
	ret = ov1063x_isp_reset(priv, true);

	/* Set PLL */
	ov1063x_write(priv, OV1063X_SC_CMMN_PLL_CTRL0, r3003, &ret);
	ov1063x_write(priv, OV1063X_SC_CMMN_PLL_CTRL1, r3004, &ret);

	/* Set HSYNC */
	ov1063x_write(priv, OV1063X_DVP_MOD_SEL, 0, &ret);

	switch (priv->format.code) {
	case MEDIA_BUS_FMT_UYVY8_2X8:
		r4300 = OV1063X_FORMAT_UYVY;
		break;
	case MEDIA_BUS_FMT_VYUY8_2X8:
		r4300 = OV1063X_FORMAT_VYUY;
		break;
	case MEDIA_BUS_FMT_YUYV8_2X8:
		r4300 = OV1063X_FORMAT_YUYV;
		break;
	case MEDIA_BUS_FMT_YVYU8_2X8:
		r4300 = OV1063X_FORMAT_YYYU;
		break;
	default:
		r4300 = OV1063X_FORMAT_UYVY;
		break;
	}

	/* Set format to UYVY */
	ov1063x_write(priv, OV1063X_FORMAT_CTRL00, r4300, &ret);

	dev_dbg(priv->dev, "r4300=0x%X\n", r4300);

	/* Set output to 8-bit yuv */
	ov1063x_write(priv, OV1063X_VFIFO_LLEN_FIRS1_SEL,
		      OV1063X_VFIFO_LLEN_FIRS1_SEL_8B_YUV, &ret);

	/* Horizontal cropping */
	ov1063x_write(priv, OV1063X_ANA_ARRAY1, horiz_crop_mode, &ret);

	ov1063x_write(priv, OV1063X_SENSOR_RSTGOLOW, (pclk + 1500000) / 3000000, &ret);
	ov1063x_write(priv, OV1063X_SENSOR_HLDWIDTH, (pclk + 666666) / 1333333, &ret);
	ov1063x_write(priv, OV1063X_SENSOR_TXWIDTH, (pclk + 961500) / 1923000, &ret);

	/* Vertical cropping */
	tmp = ((OV1063X_SENSOR_HEIGHT - height_pre_subsample) / 2) & ~0x1;
	ov1063x_write(priv, OV1063X_TIMING_Y_START_ADDR, tmp, &ret);
	tmp = tmp + height_pre_subsample + 3;
	ov1063x_write(priv, OV1063X_TIMING_Y_END_ADDR, tmp, &ret);

	dev_dbg(priv->dev, "width x height = %x x %x\n", width, height);
	/* Output size */
	ov1063x_write(priv, OV1063X_TIMING_X_OUTPUT_SIZE, width, &ret);
	ov1063x_write(priv, OV1063X_TIMING_Y_OUTPUT_SIZE, height, &ret);

	dev_dbg(priv->dev, "hts x vts = %x x %x\n", hts, vts);

	ov1063x_write(priv, OV1063X_TIMING_HTS, hts, &ret);
	ov1063x_write(priv, OV1063X_TIMING_VTS, vts, &ret);

	if (ret < 0)
		return ret;

	if (vert_sub_sample) {
		ret = ov1063x_write_array(priv, ov1063x_regs_vert_sub2,
					  ARRAY_SIZE(ov1063x_regs_vert_sub2));
	} else {
		ret = ov1063x_write_array(priv, ov1063x_regs_vert_no_sub,
					  ARRAY_SIZE(ov1063x_regs_vert_no_sub));
	}
	if (ret)
		return ret;

	ov1063x_write(priv, OV1063X_VFIFO_LINE_LENGTH_MAN, 2 * hts, &ret);
	ov1063x_write(priv, OV1063X_VFIFO_HSYNC_START_POSITION,
			2 * (hts - width_pre_subsample), &ret);

	tmp = (vts - 8) * 16;
	ov1063x_write(priv, OV1063X_MAX_EXP_LONG, tmp, &ret);
	ov1063x_write(priv, OV1063X_MAX_EXP_SHORT, tmp, &ret);

	nr_isp_pixels = sensor_width * (height + 4);
	ov1063x_write(priv, OV1063X_SIMPLE_MIN_NUM, nr_isp_pixels / 256, &ret);
	ov1063x_write(priv, OV1063X_CT_MIN_NUM, nr_isp_pixels / 256, &ret);
	ov1063x_write(priv, OV1063X_REG_16BIT(0xc512), nr_isp_pixels / 16,
		      &ret);

	/* Horizontal sub-sampling */
	if (horiz_sub_sample) {
		ov1063x_write(priv, OV1063X_ISP_RW05, OV1063X_ISP_RW05_SUB_AVG |
			      OV1063X_ISP_RW05_SUB_ENABLE, &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_PCLK_DIV_CTRL, 2, &ret);
	} else {
		ov1063x_write(priv, OV1063X_ISP_RW05, OV1063X_ISP_RW05_SUB_AVG,
			      &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_PCLK_DIV_CTRL, 1, &ret);
	}

	ov1063x_write(priv, OV1063X_VTS_ADDR, vts, &ret);
	ov1063x_write(priv, OV1063X_HTS_ADDR, hts, &ret);

	if (ret)
		return ret;

	return ov1063x_isp_reset(priv, false);
}

/* -----------------------------------------------------------------------------
 * V5L2 Control Operations
 */

static int ov1063x_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov1063x_priv *priv = container_of(ctrl->handler,
					struct ov1063x_priv, hdl);
	const struct ov1063x_reg *regs;
	int n_regs, ret = 0;

	if (!priv->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_VFLIP: {
		const u32 vflip = OV1063X_TIMING_CTRL1C_VFLIP_DIG
				| OV1063X_TIMING_CTRL1C_VFLIP_ARRAY;

		return ov1063x_update(priv, OV1063X_TIMING_CTRL1C, vflip,
				      ctrl->val ? vflip : 0, NULL);
	}

	case V4L2_CID_HFLIP: {
		const u32 hflip = OV1063X_TIMING_CTRL1D_HFLIP_DIG
				| OV1063X_TIMING_CTRL1D_HFLIP_ARRAY;

		ov1063x_update(priv, OV1063X_HORIZ_COLORCORRECT,
			       OV1063X_HORIZ_COLORCORRECT_ON,
			       ctrl->val ? OV1063X_HORIZ_COLORCORRECT_ON : 0,
			       &ret);
		ov1063x_update(priv, OV1063X_TIMING_CTRL1D, hflip,
			       ctrl->val ? hflip : 0, &ret);
		return ret;
	}

	case V4L2_CID_TEST_PATTERN:
		if (ctrl->val) {
			n_regs = ARRAY_SIZE(ov1063x_regs_colorbar_enable);
			regs = ov1063x_regs_colorbar_enable;
		} else {
			n_regs = ARRAY_SIZE(ov1063x_regs_colorbar_disable);
			regs = ov1063x_regs_colorbar_disable;
		}
		return ov1063x_write_array(priv, regs, n_regs);
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops ov1063x_ctrl_ops = {
	.s_ctrl = ov1063x_s_ctrl,
};

static const char * const ov1063x_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bars",
};

/* -----------------------------------------------------------------------------
 * V4L2 Subdev Operations
 */

static int ov1063x_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov1063x_priv *priv = to_ov1063x(sd);
	int ret = 0;

	if (!enable) {
		ov1063x_write(priv, OV1063X_STREAM_MODE, 0, &ret);
		ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST2,
			      OV1063X_SC_CMMN_CLKRST2_SCLK, &ret);

		pm_runtime_mark_last_busy(priv->dev);
		pm_runtime_put_autosuspend(priv->dev);

		mutex_lock(priv->hdl.lock);
		priv->streaming = false;
		mutex_unlock(priv->hdl.lock);

		return ret;
	}

	mutex_lock(priv->hdl.lock);

	/* Streaming needs to be true for ov1063x_s_ctrl() to proceed. */
	priv->streaming = true;

	ret = pm_runtime_get_sync(priv->dev);
	if (ret < 0)
		goto done;

	ret = ov1063x_set_params(priv);
	if (ret < 0)
		goto done;

	ret = __v4l2_ctrl_handler_setup(&priv->hdl);
	if (ret < 0)
		goto done;

	ret = 0;
	ov1063x_write(priv, OV1063X_STREAM_MODE, OV1063X_STREAM_MODE_ON, &ret);
	ov1063x_write(priv, OV1063X_SC_CMMN_CLKRST2,
		      OV1063X_SC_CMMN_CLKRST2_PCLK_DVP |
		      OV1063X_SC_CMMN_CLKRST2_SCLK, &ret);

done:
	if (ret < 0) {
		/*
		 * In case of error, turn the power off synchronously as the
		 * device likely has no other chance to recover.
		 */
		pm_runtime_put_sync(priv->dev);
		priv->streaming = false;
	}

	mutex_unlock(priv->hdl.lock);

	return ret;
}

static struct v4l2_mbus_framefmt *
__ov1063x_get_pad_format(struct ov1063x_priv *priv,
			 struct v4l2_subdev_pad_config *cfg,
			 unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&priv->subdev, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &priv->format;
	default:
		return NULL;
	}
}

static int ov1063x_init_cfg(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg)
{
	u32 which = cfg ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	struct ov1063x_priv *priv = to_ov1063x(sd);
	struct v4l2_mbus_framefmt *format;

	format = __ov1063x_get_pad_format(priv, cfg, 0, which);
	format->code = ov1063x_mbus_formats[0];
	format->width = ov1063x_framesizes[0].width;
	format->height = ov1063x_framesizes[0].height;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int ov1063x_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(ov1063x_mbus_formats))
		return -EINVAL;

	code->code = ov1063x_mbus_formats[code->index];

	return 0;
}

static int ov1063x_enum_frame_sizes(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ov1063x_mbus_formats); ++i) {
		if (ov1063x_mbus_formats[i] == fse->code)
			break;
	}

	if (i == ARRAY_SIZE(ov1063x_mbus_formats))
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(ov1063x_framesizes))
		return -EINVAL;

	fse->min_width  = ov1063x_framesizes[fse->index].width;
	fse->max_width  = fse->min_width;
	fse->max_height = ov1063x_framesizes[fse->index].height;
	fse->min_height = fse->max_height;

	return 0;
}

static int ov1063x_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct ov1063x_priv *priv = to_ov1063x(sd);

	fmt->format = *__ov1063x_get_pad_format(priv, cfg, fmt->pad,
						fmt->which);

	return 0;
}

static int ov1063x_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct ov1063x_priv *priv = to_ov1063x(sd);
	struct v4l2_mbus_framefmt *format;
	const struct v4l2_area *fsize;
	unsigned int i;
	u32 code;
	int ret = 0;

	/*
	 * Validate the media bus code, defaulting to the first one if the
	 * requested code isn't supported.
	 */
	for (i = 0; i < ARRAY_SIZE(ov1063x_mbus_formats); ++i) {
		if (ov1063x_mbus_formats[i] == fmt->format.code) {
			code = fmt->format.code;
			break;
		}
	}

	if (i == ARRAY_SIZE(ov1063x_mbus_formats))
		code = ov1063x_mbus_formats[0];

	/* Find the nearest supported frame size. */
	fsize = v4l2_find_nearest_size(ov1063x_framesizes,
				       ARRAY_SIZE(ov1063x_framesizes),
				       width, height, fmt->format.width,
				       fmt->format.height);

	/* Update the stored format and return it. */
	format = __ov1063x_get_pad_format(priv, cfg, fmt->pad, fmt->which);

	mutex_lock(priv->hdl.lock);

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && priv->streaming) {
		ret = -EBUSY;
		goto done;
	}

	format->code = code;
	format->width = fsize->width;
	format->height = fsize->height;

	fmt->format = *format;

done:
	mutex_unlock(priv->hdl.lock);

	return ret;
}

static const struct v4l2_subdev_core_ops ov1063x_subdev_core_ops = {
	.log_status		= v4l2_ctrl_subdev_log_status,
	.subscribe_event	= v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event	= v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ov1063x_subdev_video_ops = {
	.s_stream	= ov1063x_s_stream,
};

static const struct v4l2_subdev_pad_ops ov1063x_subdev_pad_ops = {
	.init_cfg		= ov1063x_init_cfg,
	.enum_mbus_code		= ov1063x_enum_mbus_code,
	.enum_frame_size	= ov1063x_enum_frame_sizes,
	.get_fmt		= ov1063x_get_fmt,
	.set_fmt		= ov1063x_set_fmt,
};

static struct v4l2_subdev_ops ov1063x_subdev_ops = {
	.core	= &ov1063x_subdev_core_ops,
	.video	= &ov1063x_subdev_video_ops,
	.pad	= &ov1063x_subdev_pad_ops,
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int ov1063x_power_on_init(struct ov1063x_priv *priv)
{
	int ret;

	ret = ov1063x_write_array(priv, ov1063x_regs_default,
				  ARRAY_SIZE(ov1063x_regs_default));
	if (ret < 0)
		return ret;

	usleep_range(500, 510);
	return 0;
}

static int ov1063x_power_on(struct ov1063x_priv *priv)
{
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret < 0)
		return ret;

	if (priv->pwdn_gpio) {
		gpiod_set_value_cansleep(priv->pwdn_gpio, 0);
		usleep_range(1000, 1200);
	}

	if (priv->reset_gpio) {
		gpiod_set_value_cansleep(priv->reset_gpio, 0);
		usleep_range(250000, 260000);
	}

	return 0;
}

static void ov1063x_power_off(struct ov1063x_priv *priv)
{
	gpiod_set_value_cansleep(priv->pwdn_gpio, 1);
	gpiod_set_value_cansleep(priv->reset_gpio, 1);

	clk_disable_unprepare(priv->clk);
}

static int ov1063x_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct ov1063x_priv *priv = to_ov1063x(subdev);
	int ret;

	ret = ov1063x_power_on(priv);
	if (ret < 0)
		return ret;

	ret = ov1063x_power_on_init(priv);
	if (ret < 0) {
		ov1063x_power_off(priv);
		return ret;
	}

	return 0;
}

static int ov1063x_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct ov1063x_priv *priv = to_ov1063x(subdev);

	ov1063x_power_off(priv);

	return 0;
}

static const struct dev_pm_ops ov1063x_pm_ops = {
	SET_RUNTIME_PM_OPS(ov1063x_runtime_suspend, ov1063x_runtime_resume, NULL)
};

/* -----------------------------------------------------------------------------
 * I2C Driver, Probe & Remove
 */

static int ov1063x_detect(struct ov1063x_priv *priv)
{
	const char *name;
	u32 pid, ver;
	int ret;

	/* Read and check the product ID. */
	ret = ov1063x_read(priv, OV1063X_PID, &pid);
	if (ret)
		return ret;

	ret = ov1063x_read(priv, OV1063X_VER, &ver);
	if (ret)
		return ret;

	pid = OV1063X_VERSION(pid, ver);

	switch (pid) {
	case OV10633_VERSION_REG:
		priv->model = SENSOR_OV10633;
		name = "OV10633";
		break;
	case OV10635_VERSION_REG:
		priv->model = SENSOR_OV10635;
		name = "OV10635";
		break;
	default:
		dev_err(priv->dev, "Unknown product ID %04x\n", pid);
		return -ENODEV;
	}

	dev_info(priv->dev, "%s detected\n", name);

	return 0;
}

static const struct regmap_config ov1063x_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

static int ov1063x_probe(struct i2c_client *client)
{
	struct ov1063x_priv *priv;
	struct v4l2_subdev *sd;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;

	/* Acquire resources: regmap, GPIOs and clock. The GPIOs are optional. */
	priv->regmap = devm_regmap_init_i2c(client, &ov1063x_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->pwdn_gpio = devm_gpiod_get_optional(priv->dev, "powerdown",
						  GPIOD_OUT_HIGH);
	if (IS_ERR(priv->pwdn_gpio))
		return PTR_ERR(priv->pwdn_gpio);

	priv->reset_gpio = devm_gpiod_get_optional(priv->dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return PTR_ERR(priv->reset_gpio);
	priv->clk = devm_clk_get(priv->dev, "xvclk");
	if (IS_ERR(priv->clk)) {
		ret = PTR_ERR(priv->clk);
		dev_err(priv->dev, "Failed to get xvclk clock: %d\n", ret);
		return ret;
	}

	priv->clk_rate = clk_get_rate(priv->clk);
	dev_dbg(priv->dev, "xvclk rate: %lu Hz\n", priv->clk_rate);

	if (priv->clk_rate < 6000000 || priv->clk_rate > 27000000)
		return -EINVAL;

	/*
	 * Enable power and detect the device.
	 *
	 * The driver supports runtime PM, but needs to work when runtime PM is
	 * disabled in the kernel. To that end, power it on manually here.
	 */
	ret = ov1063x_power_on(priv);
	if (ret < 0)
		return ret;

	ret = ov1063x_detect(priv);
	if (ret)
		goto err_power;

	/* Initialize the subdev and its controls. */
	sd = &priv->subdev;
	v4l2_i2c_subdev_init(sd, client, &ov1063x_subdev_ops);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;

	v4l2_ctrl_handler_init(&priv->hdl, 3);
	v4l2_ctrl_new_std(&priv->hdl, &ov1063x_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&priv->hdl, &ov1063x_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	priv->colorbar = v4l2_ctrl_new_std_menu_items(
		&priv->hdl, &ov1063x_ctrl_ops, V4L2_CID_TEST_PATTERN,
		ARRAY_SIZE(ov1063x_test_pattern_menu) - 1, 0, 0,
		ov1063x_test_pattern_menu);

	if (priv->hdl.error) {
		ret = priv->hdl.error;
		goto err_power;
	}

	sd->ctrl_handler = &priv->hdl;

	/* Default framerate */
	priv->fps_numerator = 30;
	priv->fps_denominator = 1;
	ov1063x_init_cfg(&priv->subdev, NULL);

	/* Initialize the media entity. */
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &priv->pad);
	if (ret < 0)
		goto err_ctrls;

	/*
	 * Enable runtime PM. As the device has been powered manually, mark it
	 * as active, and increase the usage count without resuming the device.
	 */
	pm_runtime_set_active(priv->dev);
	pm_runtime_get_noresume(priv->dev);
	pm_runtime_enable(priv->dev);

	/*
	 * Enable autosuspend as it can help avoiding costly power transitions
	 * when reconfiguring the sensor.
	 */
	pm_runtime_set_autosuspend_delay(priv->dev, 1000);
	pm_runtime_use_autosuspend(priv->dev);

	/*
	 * At this point the device is powered on and active from a runtime PM
	 * point of view, but hasn't gone through the full initialization
	 * performed by the runtime resume operation. Suspend it synchronously
	 * to turn the power off, ensuring proper initialization will take
	 * place before the first usage.
	 */
	pm_runtime_put_sync(priv->dev);

	/*
	 * In case runtime PM is disabled in the kernel, the device remains
	 * active and needs to be fully initialized at this point.
	 */
	if (!pm_runtime_status_suspended(priv->dev)) {
		ret = ov1063x_power_on_init(priv);
		if (ret < 0)
			goto err_pm;
	}

	/* Finally, register the subdev. */
	ret = v4l2_async_register_subdev(sd);
	if (ret < 0)
		goto err_pm;

	dev_info(priv->dev, "%s sensor driver registered !!\n", sd->name);

	return 0;

err_pm:
	pm_runtime_disable(priv->dev);
	media_entity_cleanup(&priv->subdev.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&priv->hdl);
err_power:
	if (!pm_runtime_status_suspended(priv->dev))
		ov1063x_power_off(priv);
	return ret;
}

static int ov1063x_remove(struct i2c_client *client)
{
	struct ov1063x_priv *priv = i2c_get_clientdata(client);

	v4l2_ctrl_handler_free(&priv->hdl);
	v4l2_async_unregister_subdev(&priv->subdev);
	media_entity_cleanup(&priv->subdev.entity);

	/*
	 * Disable runtime PM. In case runtime PM is disabled in the kernel,
	 * make sure to turn power off manually.
	 */
	pm_runtime_disable(priv->dev);
	if (!pm_runtime_status_suspended(priv->dev))
		ov1063x_power_off(priv);
	pm_runtime_set_suspended(priv->dev);

	return 0;
}

static const struct i2c_device_id ov1063x_id[] = {
	{ "ov10635", 0 },
	{ "ov10633", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov1063x_id);

static const struct of_device_id ov1063x_dt_id[] = {
	{ .compatible = "ovti,ov10635" },
	{ .compatible = "ovti,ov10633" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov1063x_dt_id);

static struct i2c_driver ov1063x_i2c_driver = {
	.driver = {
		.name = "ov1063x",
		.of_match_table = of_match_ptr(ov1063x_dt_id),
		.pm = &ov1063x_pm_ops,
	},
	.probe_new = ov1063x_probe,
	.remove = ov1063x_remove,
	.id_table = ov1063x_id,
};

module_i2c_driver(ov1063x_i2c_driver);

MODULE_DESCRIPTION("Camera Sensor Driver for OmniVision OV10633/OV10635");
MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_LICENSE("GPL v2");
