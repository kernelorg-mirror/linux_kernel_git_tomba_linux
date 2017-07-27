// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx VPSS Color Space Converter
 *
 * Copyright (C) 2017 Xilinx, Inc.
 *
 * Contacts: Rohit Athavale <rohit.athavale@xilinx.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#define XV_CSC_PAD_SINK			0
#define XV_CSC_PAD_SOURCE		1
#define XV_CSC_NUM_PADS			2

/* Register map */
#define XV_CSC_AP_CTRL			0x000
#define XV_CSC_AP_CTRL_START		BIT(0)
#define XV_CSC_AP_CTRL_AUTO_RESTART	BIT(7)
#define XV_CSC_INVIDEOFORMAT		0x010
#define XV_CSC_OUTVIDEOFORMAT		0x018
#define XV_CSC_WIDTH			0x020
#define XV_CSC_HEIGHT			0x028
#define XV_CSC_K11			0x050
#define XV_CSC_K12			0x058
#define XV_CSC_K13			0x060
#define XV_CSC_K21			0x068
#define XV_CSC_K22			0x070
#define XV_CSC_K23			0x078
#define XV_CSC_K31			0x080
#define XV_CSC_K32			0x088
#define XV_CSC_K33			0x090
#define XV_CSC_ROFFSET			0x098
#define XV_CSC_GOFFSET			0x0a0
#define XV_CSC_BOFFSET			0x0a8
#define XV_CSC_CLAMPMIN			0x0b0
#define XV_CSC_CLIPMAX			0x0b8

/* The IP clamps the output to [0, 2^color_depth - 1]. */
#define XV_CSC_CLAMP_MIN		0

/*
 * Coefficients are Q4.12 fixed point values, i.e. 1.0 is represented as
 * XV_CSC_SCALE_FACTOR. XV_CSC_DIVISOR is the scale of the decimal constants
 * in the coefficient tables below.
 */
#define XV_CSC_SCALE_FACTOR		4096
#define XV_CSC_DIVISOR			10000

/*
 * Coefficient matrices are 3x3 matrices with an extra column holding the
 * per-row offset that is added after the multiplication.
 */
#define XV_CSC_K_DIM			3
#define XV_CSC_K_OFFSET_COL		XV_CSC_K_DIM
#define XV_CSC_K_MAT_COLS		(XV_CSC_K_DIM + 1)

#define XV_CSC_MIN_WIDTH		64
#define XV_CSC_MAX_WIDTH		8192
#define XV_CSC_MIN_HEIGHT		64
#define XV_CSC_MAX_HEIGHT		4320
#define XV_CSC_DEFAULT_WIDTH		1280
#define XV_CSC_DEFAULT_HEIGHT		720

/*
 * The colour controls are exposed to userspace as 0..100 sliders and are
 * mapped to the IP-specific ranges by xcsc_s_ctrl(). The defaults below
 * correspond to the default slider position and to a unity coefficient
 * matrix.
 */
#define XCSC_CTRL_DEFAULT		50
#define XCSC_BRIGHTNESS_DEFAULT		120
#define XCSC_CONTRAST_DEFAULT		0
#define XCSC_GAIN_DEFAULT		120

/* Values programmed into XV_CSC_IN/OUTVIDEOFORMAT. */
enum xcsc_color_fmt {
	XCSC_COLOR_FMT_RGB = 0,
	XCSC_COLOR_FMT_YCRCB_444 = 1,
	XCSC_COLOR_FMT_YCRCB_422 = 2,
};

/* Rows of the coefficient matrix, and indices into the gain arrays. */
enum xcsc_gain_idx {
	XCSC_GAIN_RED = 0,
	XCSC_GAIN_GREEN = 1,
	XCSC_GAIN_BLUE = 2,
	XCSC_GAIN_NUM,
};

/**
 * struct xcsc_format_info - Media bus format description
 * @code: Media bus format code
 * @cft: IP specific colour format for this media bus format
 */
struct xcsc_format_info {
	u32 code;
	enum xcsc_color_fmt cft;
};

static const struct xcsc_format_info xcsc_formats[] = {
	{ MEDIA_BUS_FMT_RBG888_1X24, XCSC_COLOR_FMT_RGB },
	{ MEDIA_BUS_FMT_VUY8_1X24, XCSC_COLOR_FMT_YCRCB_444 },
	{ MEDIA_BUS_FMT_UYVY8_1X16, XCSC_COLOR_FMT_YCRCB_422 },
	{ MEDIA_BUS_FMT_UYVY10_1X20, XCSC_COLOR_FMT_YCRCB_422 },
};

static const s32 rgb_unity_matrix[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS] = {
	{ XV_CSC_SCALE_FACTOR, 0, 0, 0 },
	{ 0, XV_CSC_SCALE_FACTOR, 0, 0 },
	{ 0, 0, XV_CSC_SCALE_FACTOR, 0 },
};

/*
 * BT.709 limited range YCbCr to full range RGB. The columns are Y, Cb and Cr,
 * the rows R, G and B. The offset column is computed at runtime as it depends
 * on the colour depth.
 */
static const s32 ycrcb_to_rgb_unity[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS] = {
	{
		11644 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0,
		17927 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0
	}, {
		11644 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		-2132 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		-5329 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0
	}, {
		11644 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		21124 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0,
		0
	},
};

/*
 * Full range RGB to BT.709 limited range YCbCr, the inverse of the matrix
 * above.
 */
static const s32 rgb_to_ycrcb_unity[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS] = {
	{
		1826 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		6142 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		620 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0
	}, {
		-1006 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		-3386 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		4392 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0
	}, {
		4392 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		-3989 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		-403 * XV_CSC_SCALE_FACTOR / XV_CSC_DIVISOR,
		0
	},
};

/**
 * struct xcsc_dev - Xilinx VPSS CSC device structure
 * @subdev: V4L2 subdevice
 * @notifier: Async notifier for the upstream subdev
 * @dev: (OF) device
 * @iomem: device I/O register space remapped to kernel virtual memory
 * @clk: video core clock
 * @rst_gpio: PS GPIO used to assert/de-assert the IP reset line
 * @pads: media pads
 * @ctrl_handler: V4L2 control handler
 * @color_depth: bits per component the IP is configured for
 * @max_width: maximum width supported by the IP
 * @max_height: maximum height supported by the IP
 * @cft_in: IP specific input colour format
 * @cft_out: IP specific output colour format
 * @brightness: requested brightness
 * @contrast: requested contrast
 * @gain: requested red, green and blue gains
 * @brightness_active: brightness applied to @shadow_coeff
 * @contrast_active: contrast applied to @shadow_coeff
 * @gain_active: gains applied to @shadow_coeff
 * @k_hw: coefficients written to the IP
 * @shadow_coeff: RGB domain coefficients tracking the colour controls
 *
 * The colour controls operate in the RGB domain on @shadow_coeff, which
 * xcsc_correct_coeff() then converts to the IP input and output colour
 * formats to produce @k_hw. Only the difference between the requested and the
 * currently applied control value is applied, hence the *_active fields.
 */
struct xcsc_dev {
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct device *dev;
	void __iomem *iomem;
	struct clk *clk;
	struct gpio_desc *rst_gpio;
	struct media_pad pads[XV_CSC_NUM_PADS];
	struct v4l2_ctrl_handler ctrl_handler;

	unsigned int color_depth;
	u32 max_width;
	u32 max_height;

	enum xcsc_color_fmt cft_in;
	enum xcsc_color_fmt cft_out;

	s32 brightness;
	s32 contrast;
	s32 gain[XCSC_GAIN_NUM];
	s32 brightness_active;
	s32 contrast_active;
	s32 gain_active[XCSC_GAIN_NUM];

	s32 k_hw[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS];
	s32 shadow_coeff[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS];
};

static inline struct xcsc_dev *to_csc(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct xcsc_dev, subdev);
}

static const struct xcsc_format_info *xcsc_get_format_info(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xcsc_formats); i++) {
		if (xcsc_formats[i].code == code)
			return &xcsc_formats[i];
	}

	return NULL;
}

/* -----------------------------------------------------------------------------
 * Register Access
 */

static void xcsc_write(struct xcsc_dev *xcsc, u32 reg, u32 data)
{
	iowrite32(data, xcsc->iomem + reg);
}

static void xcsc_write_coeff(struct xcsc_dev *xcsc)
{
	xcsc_write(xcsc, XV_CSC_K11, xcsc->k_hw[0][0]);
	xcsc_write(xcsc, XV_CSC_K12, xcsc->k_hw[0][1]);
	xcsc_write(xcsc, XV_CSC_K13, xcsc->k_hw[0][2]);
	xcsc_write(xcsc, XV_CSC_K21, xcsc->k_hw[1][0]);
	xcsc_write(xcsc, XV_CSC_K22, xcsc->k_hw[1][1]);
	xcsc_write(xcsc, XV_CSC_K23, xcsc->k_hw[1][2]);
	xcsc_write(xcsc, XV_CSC_K31, xcsc->k_hw[2][0]);
	xcsc_write(xcsc, XV_CSC_K32, xcsc->k_hw[2][1]);
	xcsc_write(xcsc, XV_CSC_K33, xcsc->k_hw[2][2]);
	xcsc_write(xcsc, XV_CSC_ROFFSET, xcsc->k_hw[0][XV_CSC_K_OFFSET_COL]);
	xcsc_write(xcsc, XV_CSC_GOFFSET, xcsc->k_hw[1][XV_CSC_K_OFFSET_COL]);
	xcsc_write(xcsc, XV_CSC_BOFFSET, xcsc->k_hw[2][XV_CSC_K_OFFSET_COL]);
}

/* -----------------------------------------------------------------------------
 * Coefficient Computation
 */

/*
 * Multiply two coefficient matrices, computing @kout = @k2 x @k1. The 3x3
 * parts are multiplied as Q4.12 fixed point values, the offset column of @k1
 * is transformed by @k2 and the offset column of @k2 added to the result.
 */
static void
xcsc_matrix_multiply(const s32 k1[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS],
		     const s32 k2[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS],
		     s32 kout[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS])
{
	s32 result[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS];
	unsigned int i, j, k;

	for (i = 0; i < XV_CSC_K_DIM; i++) {
		for (j = 0; j < XV_CSC_K_MAT_COLS; j++) {
			s32 sum = 0;

			for (k = 0; k < XV_CSC_K_DIM; k++)
				sum += k2[i][k] * k1[k][j];

			result[i][j] = sum / XV_CSC_SCALE_FACTOR;
		}

		result[i][XV_CSC_K_OFFSET_COL] += k2[i][XV_CSC_K_OFFSET_COL];
	}

	memcpy(kout, result, sizeof(result));
}

/*
 * See http://graficaobscura.com/matrix/index.html for how the coefficients in
 * the tables above are derived. The VPSS CSC IP implements the same matrix
 * style algorithm.
 */
static void xcsc_ycrcb_to_rgb(struct xcsc_dev *xcsc,
			      s32 temp[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS])
{
	s32 bpc_scale = BIT(xcsc->color_depth - 8);

	memcpy(temp, ycrcb_to_rgb_unity, sizeof(ycrcb_to_rgb_unity));

	temp[0][XV_CSC_K_OFFSET_COL] = -248 * bpc_scale;
	temp[1][XV_CSC_K_OFFSET_COL] = 77 * bpc_scale;
	temp[2][XV_CSC_K_OFFSET_COL] = -289 * bpc_scale;
}

static void xcsc_rgb_to_ycrcb(struct xcsc_dev *xcsc,
			      s32 temp[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS])
{
	s32 bpc_scale = BIT(xcsc->color_depth - 8);

	memcpy(temp, rgb_to_ycrcb_unity, sizeof(rgb_to_ycrcb_unity));

	temp[0][XV_CSC_K_OFFSET_COL] = 16 * bpc_scale;
	temp[1][XV_CSC_K_OFFSET_COL] = 128 * bpc_scale;
	temp[2][XV_CSC_K_OFFSET_COL] = 128 * bpc_scale;
}

/*
 * Convert the RGB domain coefficient matrix @rgb to the IP input and output
 * colour formats, storing the result in xcsc->k_hw.
 */
static void xcsc_correct_coeff(struct xcsc_dev *xcsc,
			       const s32 rgb[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS])
{
	s32 csc_change[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS];
	s32 csc_extra[XV_CSC_K_DIM][XV_CSC_K_MAT_COLS];
	bool rgb_in = xcsc->cft_in == XCSC_COLOR_FMT_RGB;
	bool rgb_out = xcsc->cft_out == XCSC_COLOR_FMT_RGB;

	if (rgb_in && rgb_out) {
		memcpy(xcsc->k_hw, rgb, sizeof(xcsc->k_hw));
	} else if (rgb_in) {
		xcsc_rgb_to_ycrcb(xcsc, csc_change);
		xcsc_matrix_multiply(rgb, csc_change, xcsc->k_hw);
	} else if (rgb_out) {
		xcsc_ycrcb_to_rgb(xcsc, csc_change);
		xcsc_matrix_multiply(csc_change, rgb, xcsc->k_hw);
	} else {
		xcsc_ycrcb_to_rgb(xcsc, csc_change);
		xcsc_matrix_multiply(csc_change, rgb, csc_extra);
		xcsc_rgb_to_ycrcb(xcsc, csc_change);
		xcsc_matrix_multiply(csc_extra, csc_change, xcsc->k_hw);
	}
}

static void xcsc_set_unity_matrix(struct xcsc_dev *xcsc)
{
	memcpy(xcsc->k_hw, rgb_unity_matrix, sizeof(xcsc->k_hw));
	memcpy(xcsc->shadow_coeff, rgb_unity_matrix,
	       sizeof(xcsc->shadow_coeff));
}

static void xcsc_reset_controls(struct xcsc_dev *xcsc)
{
	unsigned int i;

	xcsc->brightness_active = XCSC_BRIGHTNESS_DEFAULT;
	xcsc->contrast_active = XCSC_CONTRAST_DEFAULT;

	for (i = 0; i < XCSC_GAIN_NUM; i++)
		xcsc->gain_active[i] = XCSC_GAIN_DEFAULT;
}

/*
 * The brightness control scales the whole 3x3 matrix, the gain controls scale
 * a single row each. Only the ratio between the requested and the currently
 * applied value is applied, as the previous value is already baked into the
 * shadow coefficients.
 */
static void xcsc_set_brightness(struct xcsc_dev *xcsc)
{
	unsigned int i, j;

	if (xcsc->brightness == xcsc->brightness_active)
		return;

	dev_dbg(xcsc->dev, "brightness %d -> %d\n",
		xcsc->brightness_active, xcsc->brightness);

	for (i = 0; i < XV_CSC_K_DIM; i++) {
		for (j = 0; j < XV_CSC_K_DIM; j++)
			xcsc->shadow_coeff[i][j] = xcsc->shadow_coeff[i][j] *
						   xcsc->brightness /
						   xcsc->brightness_active;
	}

	xcsc->brightness_active = xcsc->brightness;

	xcsc_correct_coeff(xcsc, xcsc->shadow_coeff);
	xcsc_write_coeff(xcsc);
}

static void xcsc_set_contrast(struct xcsc_dev *xcsc)
{
	s32 scale = BIT(xcsc->color_depth - 8);
	s32 contrast;
	unsigned int i;

	contrast = xcsc->contrast - xcsc->contrast_active;
	if (!contrast)
		return;

	dev_dbg(xcsc->dev, "contrast %d -> %d (scale %d)\n",
		xcsc->contrast_active, xcsc->contrast, scale);

	for (i = 0; i < XV_CSC_K_DIM; i++)
		xcsc->shadow_coeff[i][XV_CSC_K_OFFSET_COL] += contrast * scale;

	xcsc->contrast_active = xcsc->contrast;

	xcsc_correct_coeff(xcsc, xcsc->shadow_coeff);
	xcsc_write_coeff(xcsc);
}

static void xcsc_set_gain(struct xcsc_dev *xcsc, unsigned int idx)
{
	unsigned int j;

	if (xcsc->gain[idx] == xcsc->gain_active[idx])
		return;

	dev_dbg(xcsc->dev, "gain %u: %d -> %d\n", idx,
		xcsc->gain_active[idx], xcsc->gain[idx]);

	for (j = 0; j < XV_CSC_K_DIM; j++)
		xcsc->shadow_coeff[idx][j] = xcsc->shadow_coeff[idx][j] *
					     xcsc->gain[idx] /
					     xcsc->gain_active[idx];

	xcsc->gain_active[idx] = xcsc->gain[idx];

	xcsc_correct_coeff(xcsc, xcsc->shadow_coeff);
	xcsc_write_coeff(xcsc);
}

/* -----------------------------------------------------------------------------
 * Hardware Configuration
 */

static void xcsc_write_config(struct xcsc_dev *xcsc)
{
	xcsc_write(xcsc, XV_CSC_INVIDEOFORMAT, xcsc->cft_in);
	xcsc_write(xcsc, XV_CSC_OUTVIDEOFORMAT, xcsc->cft_out);

	xcsc_write_coeff(xcsc);

	xcsc_write(xcsc, XV_CSC_CLIPMAX, BIT(xcsc->color_depth) - 1);
	xcsc_write(xcsc, XV_CSC_CLAMPMIN, XV_CSC_CLAMP_MIN);
}

/* Bring the IP to the state the driver assumes at probe time. */
static void xcsc_init_hw(struct xcsc_dev *xcsc)
{
	xcsc->cft_in = XCSC_COLOR_FMT_RGB;
	xcsc->cft_out = XCSC_COLOR_FMT_RGB;

	xcsc_set_unity_matrix(xcsc);
	xcsc_write_config(xcsc);
}

/*
 * Program the input and output colour formats and the corresponding
 * coefficient matrix. The colour controls are applied on top of the result by
 * the callers.
 */
static void xcsc_update_formats(struct xcsc_dev *xcsc,
				struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format;
	bool rgb_in, rgb_out;

	format = v4l2_subdev_state_get_format(state, XV_CSC_PAD_SINK);
	xcsc->cft_in = xcsc_get_format_info(format->code)->cft;

	format = v4l2_subdev_state_get_format(state, XV_CSC_PAD_SOURCE);
	xcsc->cft_out = xcsc_get_format_info(format->code)->cft;

	rgb_in = xcsc->cft_in == XCSC_COLOR_FMT_RGB;
	rgb_out = xcsc->cft_out == XCSC_COLOR_FMT_RGB;

	dev_dbg(xcsc->dev, "colour format in %u out %u\n",
		xcsc->cft_in, xcsc->cft_out);

	/*
	 * RGB to RGB and YCrCb to YCrCb conversions are pass-through. Note
	 * that the unity matrix also resets the shadow coefficients, while
	 * the conversion matrices are applied to the hardware coefficients
	 * only.
	 */
	if (rgb_in == rgb_out)
		xcsc_set_unity_matrix(xcsc);
	else if (rgb_out)
		xcsc_ycrcb_to_rgb(xcsc, xcsc->k_hw);
	else
		xcsc_rgb_to_ycrcb(xcsc, xcsc->k_hw);

	xcsc_write_config(xcsc);
}

static void xcsc_stop(struct xcsc_dev *xcsc)
{
	/*
	 * Clear ap_start and auto-restart. The core finishes the frame in
	 * flight and then stops. This must be done unconditionally: the reset
	 * GPIO below is optional, as some designs reset the IP through a
	 * reset line shared with the rest of the pipeline.
	 */
	xcsc_write(xcsc, XV_CSC_AP_CTRL, 0);

	/* Reset the IP through the PS GPIO. */
	gpiod_set_value_cansleep(xcsc->rst_gpio, 1);
	gpiod_set_value_cansleep(xcsc->rst_gpio, 0);

	/* The reset drops the coefficients, forget what has been applied. */
	xcsc_reset_controls(xcsc);
	memcpy(xcsc->shadow_coeff, rgb_unity_matrix,
	       sizeof(xcsc->shadow_coeff));
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int xcsc_set_remote_streams(struct xcsc_dev *xcsc, bool enable)
{
	struct media_pad *remote;
	struct v4l2_subdev *subdev;

	remote = media_pad_remote_pad_first(&xcsc->pads[XV_CSC_PAD_SINK]);

	/*
	 * The upstream entity may be a video node (an MM2S DMA), whose
	 * streaming is controlled by the DMA engine. Nothing to propagate.
	 */
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return 0;

	subdev = media_entity_to_v4l2_subdev(remote->entity);

	if (enable)
		return v4l2_subdev_enable_streams(subdev, remote->index,
						  BIT_ULL(0));

	return v4l2_subdev_disable_streams(subdev, remote->index, BIT_ULL(0));
}

static int xcsc_enable_streams(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state,
			       u32 pad, u64 streams_mask)
{
	struct xcsc_dev *xcsc = to_csc(subdev);
	const struct v4l2_mbus_framefmt *format;
	unsigned int i;
	int ret;

	/* Program the in/out video formats and the coefficient matrix. */
	xcsc_update_formats(xcsc, state);

	/* Apply the colour controls on top of the coefficient matrix. */
	xcsc_set_brightness(xcsc);
	xcsc_set_contrast(xcsc);
	for (i = 0; i < XCSC_GAIN_NUM; i++)
		xcsc_set_gain(xcsc, i);

	format = v4l2_subdev_state_get_format(state, XV_CSC_PAD_SINK);
	xcsc_write(xcsc, XV_CSC_WIDTH, format->width);
	xcsc_write(xcsc, XV_CSC_HEIGHT, format->height);

	xcsc_write(xcsc, XV_CSC_AP_CTRL,
		   XV_CSC_AP_CTRL_START | XV_CSC_AP_CTRL_AUTO_RESTART);

	/* Start the upstream subdev. */
	ret = xcsc_set_remote_streams(xcsc, true);
	if (ret < 0) {
		xcsc_stop(xcsc);
		return ret;
	}

	return 0;
}

static int xcsc_disable_streams(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state,
				u32 pad, u64 streams_mask)
{
	struct xcsc_dev *xcsc = to_csc(subdev);

	/* Stop the upstream subdev. */
	xcsc_set_remote_streams(xcsc, false);

	xcsc_stop(xcsc);

	return 0;
}

/*
 * The IP always converts between full range RGB and BT.709 limited range
 * YCbCr. The colorspace and transfer function are not modified by the
 * conversion and are inherited from the sink pad.
 */
static void xcsc_set_colorimetry(struct v4l2_mbus_framefmt *format)
{
	bool rgb = xcsc_get_format_info(format->code)->cft ==
		   XCSC_COLOR_FMT_RGB;

	if (format->colorspace == V4L2_COLORSPACE_DEFAULT)
		format->colorspace = V4L2_COLORSPACE_REC709;

	if (format->xfer_func == V4L2_XFER_FUNC_DEFAULT)
		format->xfer_func =
			V4L2_MAP_XFER_FUNC_DEFAULT(format->colorspace);

	format->ycbcr_enc = rgb ? V4L2_YCBCR_ENC_DEFAULT : V4L2_YCBCR_ENC_709;
	format->quantization = rgb ? V4L2_QUANTIZATION_FULL_RANGE
				   : V4L2_QUANTIZATION_LIM_RANGE;
}

static int xcsc_set_format(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct xcsc_dev *xcsc = to_csc(subdev);
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(state, fmt->pad);

	/* The media bus code selects the conversion performed by the IP. */
	if (!xcsc_get_format_info(fmt->format.code))
		fmt->format.code = MEDIA_BUS_FMT_RBG888_1X24;

	format->code = fmt->format.code;
	format->field = V4L2_FIELD_NONE;

	if (fmt->pad == XV_CSC_PAD_SINK) {
		struct v4l2_mbus_framefmt *source;

		format->width = clamp_t(unsigned int, fmt->format.width,
					XV_CSC_MIN_WIDTH, xcsc->max_width);
		format->height = clamp_t(unsigned int, fmt->format.height,
					 XV_CSC_MIN_HEIGHT, xcsc->max_height);
		format->colorspace = fmt->format.colorspace;
		format->xfer_func = fmt->format.xfer_func;

		xcsc_set_colorimetry(format);

		/*
		 * The IP doesn't scale, propagate the frame size and the
		 * colorimetry to the source pad.
		 */
		source = v4l2_subdev_state_get_format(state,
						      XV_CSC_PAD_SOURCE);
		source->width = format->width;
		source->height = format->height;
		source->field = format->field;
		source->colorspace = format->colorspace;
		source->xfer_func = format->xfer_func;

		xcsc_set_colorimetry(source);
	} else {
		const struct v4l2_mbus_framefmt *sink;

		/* Only the media bus code is configurable on the source pad. */
		sink = v4l2_subdev_state_get_format(state, XV_CSC_PAD_SINK);

		format->width = sink->width;
		format->height = sink->height;
		format->colorspace = sink->colorspace;
		format->xfer_func = sink->xfer_func;

		xcsc_set_colorimetry(format);
	}

	fmt->format = *format;

	return 0;
}

static int xcsc_enum_mbus_code(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(xcsc_formats))
		return -EINVAL;

	code->code = xcsc_formats[code->index].code;

	return 0;
}

static int xcsc_enum_frame_size(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_frame_size_enum *fse)
{
	struct xcsc_dev *xcsc = to_csc(subdev);

	if (fse->index || !xcsc_get_format_info(fse->code))
		return -EINVAL;

	if (fse->pad == XV_CSC_PAD_SINK) {
		fse->min_width = XV_CSC_MIN_WIDTH;
		fse->max_width = xcsc->max_width;
		fse->min_height = XV_CSC_MIN_HEIGHT;
		fse->max_height = xcsc->max_height;
	} else {
		const struct v4l2_mbus_framefmt *sink;

		/*
		 * The size on the source pad is fixed and always identical to
		 * the size on the sink pad.
		 */
		sink = v4l2_subdev_state_get_format(state, XV_CSC_PAD_SINK);

		fse->min_width = sink->width;
		fse->max_width = sink->width;
		fse->min_height = sink->height;
		fse->max_height = sink->height;
	}

	return 0;
}

static int xcsc_init_state(struct v4l2_subdev *subdev,
			   struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *format;
	unsigned int pad;

	for (pad = 0; pad < XV_CSC_NUM_PADS; pad++) {
		format = v4l2_subdev_state_get_format(state, pad);

		format->code = MEDIA_BUS_FMT_RBG888_1X24;
		format->width = XV_CSC_DEFAULT_WIDTH;
		format->height = XV_CSC_DEFAULT_HEIGHT;
		format->field = V4L2_FIELD_NONE;
		format->colorspace = V4L2_COLORSPACE_REC709;
		format->xfer_func = V4L2_XFER_FUNC_DEFAULT;

		xcsc_set_colorimetry(format);
	}

	return 0;
}

static const struct v4l2_subdev_video_ops xcsc_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops xcsc_pad_ops = {
	.enum_mbus_code = xcsc_enum_mbus_code,
	.enum_frame_size = xcsc_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = xcsc_set_format,
	.enable_streams = xcsc_enable_streams,
	.disable_streams = xcsc_disable_streams,
};

static const struct v4l2_subdev_ops xcsc_ops = {
	.video = &xcsc_video_ops,
	.pad = &xcsc_pad_ops,
};

static const struct v4l2_subdev_internal_ops xcsc_internal_ops = {
	.init_state = xcsc_init_state,
};

static const struct media_entity_operations xcsc_media_ops = {
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
	.link_validate = v4l2_subdev_link_validate,
};

/* -----------------------------------------------------------------------------
 * V4L2 Controls
 */

static int xcsc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct xcsc_dev *xcsc = container_of(ctrl->handler, struct xcsc_dev,
					     ctrl_handler);

	/*
	 * The controls are applied to the hardware when the stream is
	 * started, changing them while streaming has no effect.
	 */
	switch (ctrl->id) {
	case V4L2_CID_XILINX_CSC_BRIGHTNESS:
		xcsc->brightness = 2 * ctrl->val + 20;
		break;
	case V4L2_CID_XILINX_CSC_CONTRAST:
		xcsc->contrast = 4 * ctrl->val - 200;
		break;
	case V4L2_CID_XILINX_CSC_RED_GAIN:
		xcsc->gain[XCSC_GAIN_RED] = 2 * ctrl->val + 20;
		break;
	case V4L2_CID_XILINX_CSC_GREEN_GAIN:
		xcsc->gain[XCSC_GAIN_GREEN] = 2 * ctrl->val + 20;
		break;
	case V4L2_CID_XILINX_CSC_BLUE_GAIN:
		xcsc->gain[XCSC_GAIN_BLUE] = 2 * ctrl->val + 20;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops xcsc_ctrl_ops = {
	.s_ctrl = xcsc_s_ctrl,
};

static const struct {
	u32 id;
	const char *name;
} xcsc_ctrl_info[] = {
	{ V4L2_CID_XILINX_CSC_BRIGHTNESS, "CSC Brightness" },
	{ V4L2_CID_XILINX_CSC_CONTRAST, "CSC Contrast" },
	{ V4L2_CID_XILINX_CSC_RED_GAIN, "CSC Red Gain" },
	{ V4L2_CID_XILINX_CSC_BLUE_GAIN, "CSC Blue Gain" },
	{ V4L2_CID_XILINX_CSC_GREEN_GAIN, "CSC Green Gain" },
};

static int xcsc_init_controls(struct xcsc_dev *xcsc)
{
	unsigned int i;
	int ret;

	v4l2_ctrl_handler_init(&xcsc->ctrl_handler,
			       ARRAY_SIZE(xcsc_ctrl_info));

	for (i = 0; i < ARRAY_SIZE(xcsc_ctrl_info); i++) {
		const struct v4l2_ctrl_config cfg = {
			.ops = &xcsc_ctrl_ops,
			.id = xcsc_ctrl_info[i].id,
			.name = xcsc_ctrl_info[i].name,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.min = 0,
			.max = 100,
			.step = 1,
			.def = XCSC_CTRL_DEFAULT,
			.flags = V4L2_CTRL_FLAG_SLIDER,
		};

		v4l2_ctrl_new_custom(&xcsc->ctrl_handler, &cfg, NULL);
	}

	if (xcsc->ctrl_handler.error) {
		ret = xcsc->ctrl_handler.error;
		dev_err(xcsc->dev, "failed to add controls: %d\n", ret);
		return ret;
	}

	xcsc->subdev.ctrl_handler = &xcsc->ctrl_handler;

	ret = v4l2_ctrl_handler_setup(&xcsc->ctrl_handler);
	if (ret < 0) {
		dev_err(xcsc->dev, "failed to set controls: %d\n", ret);
		return ret;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xcsc_parse_of(struct xcsc_dev *xcsc)
{
	struct device *dev = xcsc->dev;
	struct device_node *node = dev->of_node;
	u32 video_width;
	int ret;

	ret = of_property_read_u32(node, "xlnx,max-height", &xcsc->max_height);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to read xlnx,max-height\n");

	if (xcsc->max_height > XV_CSC_MAX_HEIGHT ||
	    xcsc->max_height < XV_CSC_MIN_HEIGHT)
		return dev_err_probe(dev, -EINVAL,
				     "invalid xlnx,max-height %u\n",
				     xcsc->max_height);

	ret = of_property_read_u32(node, "xlnx,max-width", &xcsc->max_width);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to read xlnx,max-width\n");

	if (xcsc->max_width > XV_CSC_MAX_WIDTH ||
	    xcsc->max_width < XV_CSC_MIN_WIDTH)
		return dev_err_probe(dev, -EINVAL,
				     "invalid xlnx,max-width %u\n",
				     xcsc->max_width);

	ret = of_property_read_u32(node, "xlnx,video-width", &video_width);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to read xlnx,video-width\n");

	if (video_width != 8 && video_width != 10)
		return dev_err_probe(dev, -EINVAL,
				     "unsupported colour depth %u\n",
				     video_width);

	xcsc->color_depth = video_width;

	return 0;
}

static int xcsc_notify_bound(struct v4l2_async_notifier *notifier,
			     struct v4l2_subdev *sd,
			     struct v4l2_async_connection *asc)
{
	struct xcsc_dev *xcsc =
		container_of(notifier, struct xcsc_dev, notifier);

	return v4l2_create_fwnode_links_to_pad(sd,
					       &xcsc->pads[XV_CSC_PAD_SINK],
					       MEDIA_LNK_FL_ENABLED |
					       MEDIA_LNK_FL_IMMUTABLE);
}

static const struct v4l2_async_notifier_operations xcsc_notify_ops = {
	.bound = xcsc_notify_bound,
};

static int xcsc_register_notifier(struct xcsc_dev *xcsc)
{
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(xcsc->dev),
					     XV_CSC_PAD_SINK, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return 0;

	v4l2_async_subdev_nf_init(&xcsc->notifier, &xcsc->subdev);
	xcsc->notifier.ops = &xcsc_notify_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&xcsc->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		v4l2_async_nf_cleanup(&xcsc->notifier);
		return PTR_ERR(asc);
	}

	ret = v4l2_async_nf_register(&xcsc->notifier);
	if (ret)
		v4l2_async_nf_cleanup(&xcsc->notifier);

	return ret;
}

static int xcsc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_subdev *subdev;
	struct xcsc_dev *xcsc;
	int ret;

	xcsc = devm_kzalloc(dev, sizeof(*xcsc), GFP_KERNEL);
	if (!xcsc)
		return -ENOMEM;

	xcsc->dev = dev;

	ret = xcsc_parse_of(xcsc);
	if (ret < 0)
		return ret;

	xcsc->rst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xcsc->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(xcsc->rst_gpio),
				     "failed to get reset GPIO\n");

	/* Release the reset. */
	gpiod_set_value_cansleep(xcsc->rst_gpio, 0);

	xcsc->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xcsc->iomem))
		return PTR_ERR(xcsc->iomem);

	xcsc->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(xcsc->clk))
		return dev_err_probe(dev, PTR_ERR(xcsc->clk),
				     "failed to get core clock\n");

	/* Initialize the core. */
	xcsc_reset_controls(xcsc);
	xcsc_init_hw(xcsc);

	/* Initialize the V4L2 subdev and the media entity. */
	subdev = &xcsc->subdev;
	v4l2_subdev_init(subdev, &xcsc_ops);
	subdev->dev = dev;
	subdev->internal_ops = &xcsc_internal_ops;
	strscpy(subdev->name, dev_name(dev));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &xcsc_media_ops;
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV;
	v4l2_set_subdevdata(subdev, xcsc);

	xcsc->pads[XV_CSC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	xcsc->pads[XV_CSC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&subdev->entity, ARRAY_SIZE(xcsc->pads),
				     xcsc->pads);
	if (ret < 0)
		return ret;

	ret = xcsc_init_controls(xcsc);
	if (ret < 0)
		goto error_ctrl_free;

	/*
	 * Share the control handler lock with the subdev state, the streaming
	 * operations read the control values while holding the state lock.
	 */
	subdev->state_lock = subdev->ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(subdev);
	if (ret < 0)
		goto error_ctrl_free;

	platform_set_drvdata(pdev, xcsc);

	ret = xcsc_register_notifier(xcsc);
	if (ret < 0)
		goto error_subdev_cleanup;

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev: %d\n", ret);
		goto error_notifier_cleanup;
	}

	return 0;

error_notifier_cleanup:
	v4l2_async_nf_unregister(&xcsc->notifier);
	v4l2_async_nf_cleanup(&xcsc->notifier);
error_subdev_cleanup:
	v4l2_subdev_cleanup(subdev);
error_ctrl_free:
	v4l2_ctrl_handler_free(&xcsc->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
	return ret;
}

static void xcsc_remove(struct platform_device *pdev)
{
	struct xcsc_dev *xcsc = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &xcsc->subdev;

	v4l2_async_nf_unregister(&xcsc->notifier);
	v4l2_async_nf_cleanup(&xcsc->notifier);
	v4l2_async_unregister_subdev(subdev);
	v4l2_subdev_cleanup(subdev);
	v4l2_ctrl_handler_free(&xcsc->ctrl_handler);
	media_entity_cleanup(&subdev->entity);
}

static const struct of_device_id xcsc_of_id_table[] = {
	{ .compatible = "xlnx,v-vpss-csc" },
	{ }
};
MODULE_DEVICE_TABLE(of, xcsc_of_id_table);

static struct platform_driver xcsc_driver = {
	.driver = {
		.name		= "xilinx-vpss-csc",
		.of_match_table	= xcsc_of_id_table,
	},
	.probe			= xcsc_probe,
	.remove			= xcsc_remove,
};

module_platform_driver(xcsc_driver);

MODULE_AUTHOR("Rohit Athavale <rohit.athavale@xilinx.com>");
MODULE_DESCRIPTION("Xilinx VPSS CSC Driver");
MODULE_LICENSE("GPL");
