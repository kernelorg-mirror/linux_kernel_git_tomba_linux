// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV10633/OV10635 Camera Driver
 *
 * Based on the original driver written by Phil Edworthy.
 * Copyright (C) 2013 Phil Edworthy
 * Copyright (C) 2013 Renesas Electronics
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
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
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/v4l2-mediabus.h>
#include <linux/videodev2.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "ov1063x_regs.h"

/* Register definitions */
#define	OV1063X_VFLIP			0x381c
#define	 OV1063X_VFLIP_ON		GENMASK(7, 6)
#define	 OV1063X_VFLIP_SUBSAMPLE	BIT(0)
#define	OV1063X_HMIRROR			0x381d
#define	 OV1063X_HMIRROR_ON		GENMASK(1, 0)
#define	OV1063X_HORIZ_COLORCORRECT	0x6900
#define	 OV1063X_HORIZ_COLORCORRECT_ON	BIT(0)
#define OV1063X_PID			0x300a
#define OV1063X_VER			0x300b

#define OV1063X_FORMAT_CTRL00		0x4300
#define   OV1063X_FORMAT_YUYV		0x38
#define   OV1063X_FORMAT_YYYU		0x39
#define   OV1063X_FORMAT_UYVY		0x3A
#define   OV1063X_FORMAT_VYUY		0x3B

/* IDs */
#define OV10633_VERSION_REG		0xa630
#define OV10635_VERSION_REG		0xa635
#define OV1063X_VERSION(pid, ver)	(((pid) << 8) | ((ver) & 0xff))

enum ov1063x_model {
	SENSOR_OV10633,
	SENSOR_OV10635,
};

#define OV1063X_SENSOR_WIDTH		1312
#define OV1063X_SENSOR_HEIGHT		814

#define OV1063X_MAX_WIDTH		1280
#define OV1063X_MAX_HEIGHT		800

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

	/* Protects the struct fields below */
	struct mutex			lock;

	int				fps_numerator;
	int				fps_denominator;
	struct v4l2_mbus_framefmt	format;
	int				width;
	int				height;
	bool				power;
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

static int ov1063x_write8(struct ov1063x_priv *priv, u16 reg, u8 val, int *err)
{
	int ret;

	if (err && *err)
		return *err;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret && err)
		*err = ret;

	return ret;
}

static int ov1063x_write16(struct ov1063x_priv *priv, u16 reg, u16 val, int *err)
{
	int ret = err ? *err : 0;

	if (ret)
		return ret;

	ov1063x_write8(priv, reg, val >> 8, &ret);
	ov1063x_write8(priv, reg + 1, val & 0xff, &ret);

	if (ret && err)
		*err = ret;

	return ret;
}

static int ov1063x_write_array(struct ov1063x_priv *priv,
			       const struct ov1063x_reg *regs,
			       unsigned int nr_regs)
{
	struct i2c_client *client = to_i2c_client(priv->dev);
	struct regmap *map = priv->regmap;
	unsigned int i;
	int ret;
	u8 val;

	for (i = 0; i < nr_regs; i++) {
		if (regs[i].reg == 0x300c) {
			val = ((client->addr * 2) | 0x1);

			ret = regmap_write(map, regs[i].reg, val);
			if (ret)
				return ret;
		} else {
			ret = regmap_write(map, regs[i].reg, regs[i].val);
			if (ret)
				return ret;
		}
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

/* Setup registers according to resolution and color encoding */
static int ov1063x_set_params(struct ov1063x_priv *priv, u32 width, u32 height)
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
	int n_regs;
	int ret;

	if (width > OV1063X_MAX_WIDTH || height > OV1063X_MAX_HEIGHT)
		return -EINVAL;

	priv->width = width;
	priv->height = height;

	/* Vertical sub-sampling? */
	height_pre_subsample = priv->height;
	if (priv->height <= 400) {
		vert_sub_sample = 1;
		height_pre_subsample <<= 1;
	}

	/* Horizontal sub-sampling? */
	width_pre_subsample = priv->width;
	if (priv->width <= 640) {
		horiz_sub_sample = 1;
		width_pre_subsample <<= 1;
	}

	/* Horizontal cropping */
	if (width_pre_subsample > 768) {
		sensor_width = OV1063X_SENSOR_WIDTH;
		horiz_crop_mode = 0x63;
	} else if (width_pre_subsample > 656) {
		sensor_width = 768;
		horiz_crop_mode = 0x6b;
	} else {
		sensor_width = 656;
		horiz_crop_mode = 0x73;
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

	/* Disable ISP & program all registers that we might modify */
	ret = ov1063x_write_array(priv, ov1063x_regs_change_mode,
				  ARRAY_SIZE(ov1063x_regs_change_mode));
	if (ret)
		return ret;

	/* Set PLL */
	ov1063x_write8(priv, 0x3003, r3003, &ret);
	ov1063x_write8(priv, 0x3004, r3004, &ret);

	/* Set HSYNC */
	ov1063x_write8(priv, 0x4700, 0x00, &ret);

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
	ov1063x_write8(priv, OV1063X_FORMAT_CTRL00, r4300, &ret);

	dev_dbg(priv->dev, "r4300=0x%X\n", r4300);

	/* Set output to 8-bit yuv */
	ov1063x_write8(priv, 0x4605, 0x08, &ret);

	/* Horizontal cropping */
	ov1063x_write8(priv, 0x3621, horiz_crop_mode, &ret);

	ov1063x_write8(priv, 0x3702, (pclk + 1500000) / 3000000, &ret);
	ov1063x_write8(priv, 0x3703, (pclk + 666666) / 1333333, &ret);
	ov1063x_write8(priv, 0x3704, (pclk + 961500) / 1923000, &ret);

	/* Vertical cropping */
	tmp = ((OV1063X_SENSOR_HEIGHT - height_pre_subsample) / 2) & ~0x1;
	ov1063x_write16(priv, 0x3802, tmp, &ret);
	tmp = tmp + height_pre_subsample + 3;
	ov1063x_write16(priv, 0x3806, tmp, &ret);

	dev_dbg(priv->dev, "width x height = %x x %x\n",
		priv->width, priv->height);
	/* Output size */
	ov1063x_write16(priv, 0x3808, priv->width, &ret);
	ov1063x_write16(priv, 0x380a, priv->height, &ret);

	dev_dbg(priv->dev, "hts x vts = %x x %x\n", hts, vts);

	ov1063x_write16(priv, 0x380c, hts, &ret);
	ov1063x_write16(priv, 0x380e, vts, &ret);

	if (ret < 0)
		return ret;

	if (vert_sub_sample) {
		ret = regmap_update_bits(priv->regmap, OV1063X_VFLIP,
					 OV1063X_VFLIP_SUBSAMPLE,
					 OV1063X_VFLIP_SUBSAMPLE);
		if (ret)
			return ret;
		n_regs = ARRAY_SIZE(ov1063x_regs_vert_sub_sample);
		ret = ov1063x_write_array(priv, ov1063x_regs_vert_sub_sample,
					  n_regs);
		if (ret)
			return ret;
	}

	ov1063x_write16(priv, 0x4606, 2 * hts, &ret);
	ov1063x_write16(priv, 0x460a, 2 * (hts - width_pre_subsample), &ret);

	tmp = (vts - 8) * 16;
	ov1063x_write16(priv, 0xc488, tmp, &ret);
	ov1063x_write16(priv, 0xc48a, tmp, &ret);

	nr_isp_pixels = sensor_width * (priv->height + 4);
	ov1063x_write16(priv, 0xc4cc, nr_isp_pixels / 256, &ret);
	ov1063x_write16(priv, 0xc4ce, nr_isp_pixels / 256, &ret);
	ov1063x_write16(priv, 0xc512, nr_isp_pixels / 16, &ret);

	/* Horizontal sub-sampling */
	if (horiz_sub_sample) {
		ov1063x_write8(priv, 0x5005, 0x9, &ret);
		ov1063x_write8(priv, 0x3007, 0x2, &ret);
	}

	ov1063x_write16(priv, 0xc518, vts, &ret);
	ov1063x_write16(priv, 0xc51a, hts, &ret);
	if (ret < 0)
		return ret;

	/* Enable ISP blocks */
	ret = ov1063x_write_array(priv, ov1063x_regs_enable,
				  ARRAY_SIZE(ov1063x_regs_enable));
	if (ret)
		return ret;

	return 0;
}

/* -----------------------------------------------------------------------------
 * V5L2 Control Operations
 */

static int ov1063x_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov1063x_priv *priv = container_of(ctrl->handler,
					struct ov1063x_priv, hdl);
	struct regmap *map = priv->regmap;
	const struct ov1063x_reg *regs;
	int n_regs, ret;

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		return regmap_update_bits(map, OV1063X_VFLIP,
					  OV1063X_VFLIP_ON,
					  ctrl->val ? OV1063X_VFLIP_ON : 0);
	case V4L2_CID_HFLIP:
		ret = regmap_update_bits(map, OV1063X_HORIZ_COLORCORRECT,
					 OV1063X_HORIZ_COLORCORRECT_ON,
					 ctrl->val ?
					 OV1063X_HORIZ_COLORCORRECT_ON : 0);
		if (ret)
			return ret;

		return regmap_update_bits(map, OV1063X_HMIRROR,
					  OV1063X_HMIRROR_ON,
					  ctrl->val ? OV1063X_HMIRROR_ON : 0);
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

	ov1063x_write8(priv, 0x0100, enable, &ret);
	ov1063x_write8(priv, 0x301c, enable ? 0xf0 : 0x70, &ret);

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

	mutex_lock(&priv->lock);

	format->code = code;
	format->width = fsize->width;
	format->height = fsize->height;

	fmt->format = *format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ret = ov1063x_set_params(priv, format->width, format->height);

	mutex_unlock(&priv->lock);

	return ret;
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

#if 0
static int ov1063x_enum_frame_sizes(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	int i = ARRAY_SIZE(ov1063x_mbus_formats);

	if (fse->index >= ARRAY_SIZE(ov1063x_framesizes))
		return -EINVAL;

	while (--i)
		if (ov1063x_mbus_formats[i] == fse->code)
			break;

	fse->code = ov1063x_mbus_formats[i];

	fse->min_width  = ov1063x_framesizes[fse->index].width;
	fse->max_width  = fse->min_width;
	fse->max_height = ov1063x_framesizes[fse->index].height;
	fse->min_height = fse->max_height;

	return 0;
}
#endif

static void ov1063x_set_power(struct ov1063x_priv *priv, bool on)
{
	dev_dbg(priv->dev, "%s: on: %d\n", __func__, on);

	if (priv->power == on)
		return;

	if (on) {
		if (priv->pwdn_gpio) {
			gpiod_set_value_cansleep(priv->pwdn_gpio, 0);
			usleep_range(1000, 1200);
		}
		if (priv->reset_gpio) {
			gpiod_set_value_cansleep(priv->reset_gpio, 0);
			usleep_range(250000, 260000);
		}
	} else {
		if (priv->pwdn_gpio)
			gpiod_set_value_cansleep(priv->pwdn_gpio, 1);
		if (priv->reset_gpio)
			gpiod_set_value_cansleep(priv->reset_gpio, 1);
	}

	priv->power = on;
}

static const struct v4l2_subdev_video_ops ov1063x_subdev_video_ops = {
	.s_stream	= ov1063x_s_stream,
};

static const struct v4l2_subdev_core_ops ov1063x_subdev_core_ops = {
	.log_status		= v4l2_ctrl_subdev_log_status,
	.subscribe_event	= v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event	= v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_pad_ops ov1063x_subdev_pad_ops = {
	.init_cfg		= ov1063x_init_cfg,
	.enum_mbus_code		= ov1063x_enum_mbus_code,
//	.enum_frame_size	= ov1063x_enum_frame_sizes,
	.get_fmt		= ov1063x_get_fmt,
	.set_fmt		= ov1063x_set_fmt,
};

static struct v4l2_subdev_ops ov1063x_subdev_ops = {
	.core	= &ov1063x_subdev_core_ops,
	.video	= &ov1063x_subdev_video_ops,
	.pad	= &ov1063x_subdev_pad_ops,
};

/* -----------------------------------------------------------------------------
 * I2C Driver, Probe & Remove
 */

static int ov1063x_detect(struct ov1063x_priv *priv)
{
	struct regmap *map = priv->regmap;
	const char *name;
	u32 pid, ver;
	int ret;

	ov1063x_set_power(priv, true);

	ret = ov1063x_write_array(priv, ov1063x_regs_default,
				  ARRAY_SIZE(ov1063x_regs_default));
	if (ret)
		return ret;

	usleep_range(500, 510);

	/* Read and check the product ID. */
	ret = regmap_read(map, OV1063X_PID, &pid);
	if (ret)
		return ret;

	ret = regmap_read(map, OV1063X_VER, &ver);
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
	mutex_init(&priv->lock);

	/* Acquire resources: regmap, GPIOs and clock. The GPIOs are optional. */
	priv->regmap = devm_regmap_init_i2c(client, &ov1063x_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		goto err_mutex;
	}

	priv->pwdn_gpio = devm_gpiod_get_optional(priv->dev, "powerdown",
						  GPIOD_OUT_HIGH);
	if (IS_ERR(priv->pwdn_gpio)) {
		ret = PTR_ERR(priv->pwdn_gpio);
		goto err_mutex;
	}

	priv->reset_gpio = devm_gpiod_get_optional(priv->dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio)) {
		ret = PTR_ERR(priv->reset_gpio);
		goto err_mutex;
	}

	priv->clk = devm_clk_get(priv->dev, "xvclk");
	if (IS_ERR(priv->clk)) {
		ret = PTR_ERR(priv->clk);
		dev_err(priv->dev, "Failed to get xvclk clock: %d\n", ret);
		goto err_mutex;
	}

	priv->clk_rate = clk_get_rate(priv->clk);
	dev_dbg(priv->dev, "xvclk rate: %lu Hz\n", priv->clk_rate);

	if (priv->clk_rate < 6000000 || priv->clk_rate > 27000000) {
		ret = -EINVAL;
		goto err_mutex;
	}

	/* Enable the clock and detect the device. */
	ret = clk_prepare_enable(priv->clk);
	if (ret < 0)
		goto err_mutex;

	ret = ov1063x_detect(priv);
	if (ret)
		goto err_clock;

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
		goto err_clock;
	}

	sd->ctrl_handler = &priv->hdl;
	ret = v4l2_ctrl_handler_setup(&priv->hdl);
	if (ret < 0)
		goto err_ctrls;

	/* Default framerate */
	priv->fps_numerator = 30;
	priv->fps_denominator = 1;
	ov1063x_init_cfg(&priv->subdev, NULL);
	priv->width = priv->format.width;
	priv->height = priv->format.height;

	/* Initialize the media entity. */
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &priv->pad);
	if (ret < 0)
		goto err_ctrls;

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0)
		goto err_media;

	dev_info(priv->dev, "%s sensor driver registered !!\n", sd->name);

	return 0;

err_media:
	media_entity_cleanup(&priv->subdev.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&priv->hdl);
err_clock:
	clk_disable_unprepare(priv->clk);
err_mutex:
	mutex_destroy(&priv->lock);
	return ret;
}

static int ov1063x_remove(struct i2c_client *client)
{
	struct ov1063x_priv *priv = i2c_get_clientdata(client);

	v4l2_ctrl_handler_free(&priv->hdl);
	v4l2_async_unregister_subdev(&priv->subdev);
	mutex_destroy(&priv->lock);
	media_entity_cleanup(&priv->subdev.entity);
	ov1063x_set_power(priv, false);
	clk_disable_unprepare(priv->clk);

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
		.name	= "ov1063x",
		.of_match_table = of_match_ptr(ov1063x_dt_id),
	},
	.probe_new = ov1063x_probe,
	.remove = ov1063x_remove,
	.id_table = ov1063x_id,
};

module_i2c_driver(ov1063x_i2c_driver);

MODULE_DESCRIPTION("Camera Sensor Driver for OmniVision OV10633/OV10635");
MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_LICENSE("GPL v2");
