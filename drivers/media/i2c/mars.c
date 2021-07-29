// SPDX-License-Identifier: GPL-2.0+
/*
 * MARS GMSL Camera Driver
 */

#define DEBUG
/*
 * The camera is made of an ON Semiconductor AR0231 sensor connected to a Maxim
 * MAX96705 GMSL serializer.
 */

#include "mars.h"

#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "max96705.h"

/*
 * As the drivers supports a single MEDIA_BUS_FMT_SGRBG8_1X8 format we
 * can harcode the pixel rate.
 */
#define AR0231_PIXEL_RATE		(75000000)

#define MARS_MODE_REG_TABLE_SIZE	15

/* There's no standard V4L2_CID_GREEN_BALANCE defined in the
 * linux kernel. Let's borrow V4L2_CID_CHROMA_GAIN on green
 * balance adjustment
 */
#define V4L2_CID_GREEN_BALANCE		V4L2_CID_CHROMA_GAIN

struct mars_device {
	struct device			*dev;
	struct max96705_device		serializer;
	struct i2c_client		*sensor;
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct v4l2_ctrl_handler	ctrls;
	u32				addrs[2];
	struct regmap			*sensor_regmap;
	struct v4l2_mbus_framefmt	fmt;
	/* Protects fmt structure */
	struct mutex			mutex;
};

static const struct ar0231_reg mode_1280x720[] = {
	{ 0x301A, 0x10D8 }, // RESET_REGISTER
	{ 0x3004, 0x0140 }, // X_ADDR_START = 320
	{ 0x3008, 0x063F }, // X_ADDR_END = 1599 ... 1599-320 = 1279
	{ 0x3002, 0x00F0 }, // Y_ADDR_START = 240
	{ 0x3006, 0x03BF }, // Y_ADDR_END = 959 ... 959-240 =719
	{ 0x3032, 0x0000 }, // SCALING_MODE
	{ 0x3400, 0x0010 }, // RESERVED_MFR_3400
	{ 0x3402, 0x0F10 }, // X_OUTPUT_CONTROL
	{ 0x3402, 0x0A10 }, // X_OUTPUT_CONTROL
	{ 0x3404, 0x0880 }, // Y_OUTPUT_CONTROL
	{ 0x3404, 0x05B0 }, // Y_OUTPUT_CONTROL
	{ 0x300C, 0x05DC },  // LINE_LENGTH_PCK_ = 1500 (1280+220 or 1280+17%)
	{ 0x300A, 0x0335 },  // FRAME_LENGTH_LINES_ = 821
	{ 0x3042, 0x0000 },  // EXTRA_DELAY = 0
						// TOTAL CYCLES = 1500*821 + 0
						// = 1,315,000
	{ 0x301A, 0x19DC }, // RESET_REGISTER
};

static const struct ar0231_reg mode_1920x1080[] = {
	{ 0x301A, 0x10D8 }, // RESET_REGISTER
	{ 0x3004, 0x0000 }, // X_ADDR_START = 0
	{ 0x3008, 0x077F }, // X_ADDR_END = 1919
	{ 0x3002, 0x003C }, // Y_ADDR_START = 60
	{ 0x3006, 0x0473 }, // Y_ADDR_END = 1139 ... 1139-60 // =1079
	{ 0x3032, 0x0000 }, // SCALING_MODE
	{ 0x3400, 0x0010 }, // RESERVED_MFR_3400
	{ 0x3402, 0x0F10 }, // X_OUTPUT_CONTROL
	{ 0x3402, 0x0F10 }, // X_OUTPUT_CONTROL
	{ 0x3404, 0x0880 }, // Y_OUTPUT_CONTROL
	{ 0x3404, 0x0880 }, // Y_OUTPUT_CONTROL
	{ 0x300C, 0x080E },  // LINE_LENGTH_PCK_ = 2062
	{ 0x300A, 0x0484 },  // FRAME_LENGTH_LINES_ = 1156
	{ 0x3042, 0x0000 },  // EXTRA_DELAY = 0
						// TOTAL CYCLES = 2062*1156 + 0
						// = 2,383,672
	{ 0x301A, 0x19DC }, // RESET_REGISTER
};

static const struct mars_mode {
	u32 width;
	u32 height;
	const struct ar0231_reg *reg_table;
} mars_modes[] = {
	{
		.width = 1920,
		.height = 1080,
		.reg_table = mode_1920x1080,
	},
	{
		.width = 1280,
		.height = 720,
		.reg_table = mode_1280x720,
	},
};

static inline struct mars_device *sd_to_mars(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mars_device, sd);
}

static inline struct mars_device *i2c_to_mars(struct i2c_client *client)
{
	return sd_to_mars(i2c_get_clientdata(client));
}

static const struct regmap_config sensor_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.cache_type = REGCACHE_NONE,
};

static int sensor_read(struct mars_device *dev, unsigned int reg,
		       unsigned int *val)
{
	int ret;

	ret = regmap_read(dev->sensor_regmap, reg, val);
	return ret;
}

static int sensor_write(struct mars_device *dev, u16 reg, u16 val)
{
	return regmap_write(dev->sensor_regmap, reg, val);
}

static int sensor_set_regs(struct mars_device *dev,
			   const struct ar0231_reg *regs,
			   unsigned int nr_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < nr_regs; i++) {
		ret = sensor_write(dev, regs[i].reg, regs[i].val);
		if (ret) {
			dev_err(dev->dev,
				"%s: register %u (0x%04x) write failed (%d)\n",
				__func__, i, regs[i].reg, ret);
			return ret;
		}
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * Register Configuration
 */

#define AR0231_COARSE_INTEGRATION_TIME_		0x3012
#define AR0231_BLUE_GAIN			0x3058
#define AR0231_GREEN1_GAIN			0x3056
#define AR0231_GREEN2_GAIN			0x305C
#define AR0231_RED_GAIN				0x305A
#define AR0231_ANALOG_GAIN			0x3366
#define AR0231_DIGITAL_GAIN			0x3308
#define AR0231_READ_MODE			0x3040
#define AR0231_READ_MODE_HORIZ_MIRROR		BIT(14)
#define AR0231_READ_MODE_VERT_FLIP		BIT(15)
#define AR0231_TEST_PATTERN_MODE_		0x3070

static const struct ar0231_reg ar0231_test_pattern_none[] = {
	{ 0x3022, 0x0001 }, // GROUPED_PARAMETER_HOLD_
	{ 0x3070, 0x0000 }, // Test Pattern = normal
	{ 0x3072, 0x0000 }, // Red    = 0x0000
	{ 0x3074, 0x0000 }, // Green1 = 0x0000
	{ 0x3076, 0x0000 }, // Blue   = 0x0000
	{ 0x3078, 0x0000 }, // Green2 = 0x0000
	{ 0x307A, 0x0000 }, // ?
	{ 0x3022, 0x0000 }, // GROUPED_PARAMETER_HOLD_
};

static const struct ar0231_reg ar0231_test_pattern_solid_red[] = {
	{ 0x3022, 0x0001 }, // GROUPED_PARAMETER_HOLD_
	{ 0x3070, 0x0001 }, // Test Pattern = solid color
	{ 0x3072, 0x0FFF }, // Red    = 0x0FFF
	{ 0x3074, 0x0000 }, // Green1 = 0x0000
	{ 0x3076, 0x0000 }, // Blue   = 0x0000
	{ 0x3078, 0x0000 }, // Green2 = 0x0000
	{ 0x307A, 0x0000 }, // ?
	{ 0x3022, 0x0000 }, // GROUPED_PARAMETER_HOLD_
};

static const struct ar0231_reg ar0231_test_pattern_solid_green[] = {
	{ 0x3022, 0x0001 }, // GROUPED_PARAMETER_HOLD_
	{ 0x3070, 0x0001 }, // Test Pattern = solid color
	{ 0x3072, 0x0000 }, // Red    = 0x0000
	{ 0x3074, 0x0FFF }, // Green1 = 0x0FFF
	{ 0x3076, 0x0000 }, // Blue   = 0x0000
	{ 0x3078, 0x0FFF }, // Green2 = 0x0FFF
	{ 0x307A, 0x0000 }, // ?
	{ 0x3022, 0x0000 }, // GROUPED_PARAMETER_HOLD_
};

static const struct ar0231_reg ar0231_test_pattern_solid_blue[] = {
	{ 0x3022, 0x0001 }, // GROUPED_PARAMETER_HOLD_
	{ 0x3070, 0x0001 }, // Test Pattern = solid color
	{ 0x3072, 0x0000 }, // Red    = 0x0000
	{ 0x3074, 0x0000 }, // Green1 = 0x0000
	{ 0x3076, 0x0FFF }, // Blue   = 0x0FFF
	{ 0x3078, 0x0000 }, // Green2 = 0x0000
	{ 0x307A, 0x0000 }, // ?
	{ 0x3022, 0x0000 }, // GROUPED_PARAMETER_HOLD_
};

static const struct ar0231_reg ar0231_test_pattern_cbars_full[] = {
	{ 0x3022, 0x0001 }, // GROUPED_PARAMETER_HOLD_
	{ 0x3070, 0x0002 }, // Test Pattern = solid color bars
	{ 0x3072, 0x0000 }, // Red    = 0x0000
	{ 0x3074, 0x0000 }, // Green1 = 0x0000
	{ 0x3076, 0x0000 }, // Blue   = 0x0000
	{ 0x3078, 0x0000 }, // Green2 = 0x0000
	{ 0x307A, 0x0000 }, // ?
	{ 0x3022, 0x0000 }, // GROUPED_PARAMETER_HOLD_
};
static const struct ar0231_reg ar0231_test_pattern_cbars_f2g[] = {
	{ 0x3022, 0x0001 }, // GROUPED_PARAMETER_HOLD_
	{ 0x3070, 0x0003 }, // Test Pattern = fade to grey color bars
	{ 0x3072, 0x0000 }, // Red    = 0x0000
	{ 0x3074, 0x0000 }, // Green1 = 0x0000
	{ 0x3076, 0x0000 }, // Blue   = 0x0000
	{ 0x3078, 0x0000 }, // Green2 = 0x0000
	{ 0x307A, 0x0000 }, // ?
	{ 0x3022, 0x0000 }, // GROUPED_PARAMETER_HOLD_
};

static int ar0231_s_ctrl(struct v4l2_ctrl *ctrl)
{
	unsigned int val;
	int ret = 0;

	struct mars_device *dev =
		container_of(ctrl->handler, struct mars_device, ctrls);

	dev_dbg(dev->dev, "s_ctrl: %s, value: %d.\n",
		ctrl->name, ctrl->val);

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		dev_dbg(dev->dev, "set V4L2_CID_EXPOSURE\n");
		ret = sensor_write(dev, AR0231_COARSE_INTEGRATION_TIME_,
				   ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		val = ((ctrl->val & 0x0f) << 12) | ((ctrl->val & 0x0f) << 8) |
			((ctrl->val & 0x0f) << 4) | (ctrl->val & 0x0f);
		dev_dbg(dev->dev, "set V4L2_CID_ANALOGUE_GAIN 0x%04X\n", val);
		ret = sensor_write(dev, AR0231_ANALOG_GAIN, val);
		break;
	case V4L2_CID_GAIN:
		dev_dbg(dev->dev, "set V4L2_CID_GAIN\n");
		ret = sensor_write(dev, AR0231_DIGITAL_GAIN, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		dev_dbg(dev->dev, "set V4L2_CID_HFLIP\n");
		ret = sensor_read(dev, AR0231_READ_MODE, &val);

		dev_dbg(dev->dev, "old val 0x%04X\n", val);

		if (ret < 0)
			break;
		if (ctrl->val)
			val |= AR0231_READ_MODE_HORIZ_MIRROR;
		else
			val &= ~AR0231_READ_MODE_HORIZ_MIRROR;

		dev_dbg(dev->dev, "new val 0x%04X\n", val);
		ret = sensor_write(dev, AR0231_READ_MODE, val);
		break;
	case V4L2_CID_VFLIP:
		dev_dbg(dev->dev, "set V4L2_CID_VFLIP\n");
		ret = sensor_read(dev, AR0231_READ_MODE, &val);

		dev_dbg(dev->dev, "old val 0x%04X\n", val);

		if (ret < 0)
			break;
		if (ctrl->val)
			val |= AR0231_READ_MODE_VERT_FLIP;
		else
			val &= ~AR0231_READ_MODE_VERT_FLIP;

		dev_dbg(dev->dev, "new val 0x%04X\n", val);
		ret = sensor_write(dev, AR0231_READ_MODE, val);
		break;
	case V4L2_CID_RED_BALANCE:
		dev_dbg(dev->dev, "set V4L2_CID_RED_BALANCE\n");
		ret = sensor_write(dev, AR0231_RED_GAIN, ctrl->val);
		break;
	case V4L2_CID_BLUE_BALANCE:
		dev_dbg(dev->dev, "set V4L2_CID_BLUE_BALANCE\n");
		ret = sensor_write(dev, AR0231_BLUE_GAIN, ctrl->val);
		break;
	case V4L2_CID_GREEN_BALANCE:
		dev_dbg(dev->dev, "set V4L2_CID_GREEN_BALANCE\n");
		ret = sensor_write(dev, AR0231_GREEN1_GAIN, ctrl->val);
		if (ret < 0)
			break;

		ret = sensor_write(dev, AR0231_GREEN2_GAIN, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		switch (ctrl->val) {
		case 0:
			ret = sensor_set_regs(dev,
				ar0231_test_pattern_none,
				ARRAY_SIZE(ar0231_test_pattern_none));
			break;
		case 1:
			ret = sensor_set_regs(dev,
				ar0231_test_pattern_solid_red,
				ARRAY_SIZE(ar0231_test_pattern_solid_red));
			break;
		case 2:
			ret = sensor_set_regs(dev,
				ar0231_test_pattern_solid_green,
				ARRAY_SIZE(ar0231_test_pattern_solid_green));
			break;
		case 3:
			ret = sensor_set_regs(dev,
				ar0231_test_pattern_solid_blue,
				ARRAY_SIZE(ar0231_test_pattern_solid_blue));
			break;
		case 4:
			ret = sensor_set_regs(dev,
				ar0231_test_pattern_cbars_full,
				ARRAY_SIZE(ar0231_test_pattern_cbars_full));
			break;
		case 5:
			ret = sensor_set_regs(dev,
				ar0231_test_pattern_cbars_f2g,
				ARRAY_SIZE(ar0231_test_pattern_cbars_f2g));
			break;
		default:
			ret = -EINVAL;
			break;
		}
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static int mars_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mars_device *dev = sd_to_mars(sd);

	return max96705_set_serial_link(&dev->serializer, enable);
}

static int mars_enum_mbus_code(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG8_1X8;

	return 0;
}

static struct v4l2_mbus_framefmt *
mars_get_pad_format(struct mars_device *dev, struct v4l2_subdev_state *state,
		    unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&dev->sd, state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &dev->fmt;
	default:
		return NULL;
	}
}

static int mars_get_fmt(struct v4l2_subdev *sd,
			struct v4l2_subdev_state *state,
			struct v4l2_subdev_format *format)
{
	struct mars_device *dev = sd_to_mars(sd);

	if (format->pad)
		return -EINVAL;

	mutex_lock(&dev->mutex);
	format->format = *mars_get_pad_format(dev, state, format->pad,
					      format->which);
	mutex_unlock(&dev->mutex);

	return 0;
}

static int mars_set_fmt(struct v4l2_subdev *sd,
			struct v4l2_subdev_state *state,
			struct v4l2_subdev_format *format)
{
	struct mars_device *dev = sd_to_mars(sd);
	struct v4l2_mbus_framefmt *cfg_fmt;

	const struct mars_mode *mode;

	if (format->pad)
		return -EINVAL;

	cfg_fmt = mars_get_pad_format(dev, state, format->pad, format->which);
	if (!cfg_fmt)
		return -EINVAL;

	mode = v4l2_find_nearest_size(mars_modes,
				      ARRAY_SIZE(mars_modes), width, height,
				      format->format.width,
				      format->format.height);

	sensor_set_regs(dev, mode->reg_table, MARS_MODE_REG_TABLE_SIZE);

	mutex_lock(&dev->mutex);
	*cfg_fmt = format->format;
	cfg_fmt->width = mode->width;
	cfg_fmt->height = mode->height;
	mutex_unlock(&dev->mutex);

	return 0;
}

static const struct v4l2_subdev_video_ops mars_video_ops = {
	.s_stream	= mars_s_stream,
};

static const struct v4l2_subdev_pad_ops mars_subdev_pad_ops = {
	.enum_mbus_code	= mars_enum_mbus_code,
	.get_fmt	= mars_get_fmt,
	.set_fmt	= mars_set_fmt,
};

static const struct v4l2_subdev_ops mars_subdev_ops = {
	.video		= &mars_video_ops,
	.pad		= &mars_subdev_pad_ops,
};

static const struct v4l2_ctrl_ops ar0231_ctrl_ops = {
	.s_ctrl		= ar0231_s_ctrl,
};

static const char * const test_pattern_menu[] = {
	"Disabled",
	"Solid Red",
	"Solid Green",
	"Solid Blue",
	"Color Bars (full)",
	"Color Bars (f2grey)",
};

const s64 test_pattern_menu_index[] = {0, 1, 2, 3, 4, 5};

static struct v4l2_ctrl_config ar0231_sd_ctrls[] = {
	{
		.id = V4L2_CID_PIXEL_RATE,
		.min    = AR0231_PIXEL_RATE,
		.max    = AR0231_PIXEL_RATE,
		.step   = 1,
		.def    = AR0231_PIXEL_RATE,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_EXPOSURE,
		.name   = "AR0231 Exposure",
		.type   = V4L2_CTRL_TYPE_INTEGER,
		.min    = 0x10,
		.max    = 0x53b,
		.step   = 1,
		.def    = 0x0335,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_ANALOGUE_GAIN,
		.name   = "AR0231 Analog Gain",
		.type   = V4L2_CTRL_TYPE_INTEGER,
		.min    = 0,
		.max    = 0xe,
		.step   = 1,
		.def    = 7,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_GAIN,
		.name   = "AR0231 Digital Gain",
		.type   = V4L2_CTRL_TYPE_INTEGER,
		.min    = 0,
		.max    = 0x7ff,
		.step   = 1,
		.def    = 0x200,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_RED_BALANCE,
		.name   = "AR0231 Red Balance",
		.type   = V4L2_CTRL_TYPE_INTEGER,
		.min    = 0,
		.max    = 0x7ff,
		.step   = 1,
		.def    = 0x80,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_BLUE_BALANCE,
		.name   = "AR0231 Blue Balance",
		.type   = V4L2_CTRL_TYPE_INTEGER,
		.min    = 0,
		.max    = 0x7ff,
		.step   = 1,
		.def    = 0x26b,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_GREEN_BALANCE,
		.name   = "AR0231 Green Balance",
		.type   = V4L2_CTRL_TYPE_INTEGER,
		.min    = 0,
		.max    = 0x7ff,
		.step   = 1,
		.def    = 0x91,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_HFLIP,
		.name   = "AR0231 Horizontal Flip",
		.type   = V4L2_CTRL_TYPE_BOOLEAN,
		.min    = 0,
		.max    = 1,
		.step   = 1,
		.def    = 0,
	}, {
		.ops    = &ar0231_ctrl_ops,
		.id = V4L2_CID_VFLIP,
		.name   = "AR0231 Vertical Flip",
		.type   = V4L2_CTRL_TYPE_BOOLEAN,
		.min    = 0,
		.max    = 1,
		.step   = 1,
		.def    = 0,
	}, {
		.ops = &ar0231_ctrl_ops,
		.id = V4L2_CID_TEST_PATTERN,
		.name = "AR0231 Test Pattern",
		.type = V4L2_CTRL_TYPE_MENU,
		.min = 0,
		.max = ARRAY_SIZE(test_pattern_menu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu = test_pattern_menu,
	},
};

static void mars_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width		= mars_modes[0].width;
	fmt->height		= mars_modes[0].height;
	fmt->code		= MEDIA_BUS_FMT_SGRBG8_1X8;
	fmt->colorspace		= V4L2_COLORSPACE_SRGB;
	fmt->field		= V4L2_FIELD_NONE;
	fmt->ycbcr_enc		= V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization	= V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func		= V4L2_XFER_FUNC_DEFAULT;
}

static int mars_initialize(struct mars_device *dev)
{
	int ret;
	unsigned int chip_version = 0;

	/* wait at least 700ms */
	usleep_range(700000, 1000000);

	/* Verify communication with the MAX96705: ping to wakeup. */
	dev->serializer.client->addr = MAX96705_DEFAULT_ADDR;
	i2c_smbus_read_byte(dev->serializer.client);

	/* Serial link disabled during config as it needs a valid pixel clock.*/
	ret = max96705_set_serial_link(&dev->serializer, false);
	if (ret)
		return ret;

	ret = max96705_configure_gmsl_link(&dev->serializer);
	if (ret)
		return ret;

	ret = max96705_verify_id(&dev->serializer);
	if (ret < 0)
		return ret;

	ret = max96705_set_address(&dev->serializer, dev->addrs[0]);
	if (ret < 0)
		return ret;
	dev->serializer.client->addr = dev->addrs[0];

	ret = max96705_set_translation(&dev->serializer, dev->addrs[1], 0x10);
	if (ret < 0)
		return ret;

	dev->sensor_regmap = devm_regmap_init_i2c(dev->sensor,
						  &sensor_regmap_config);
	if (IS_ERR(dev->sensor_regmap)) {
		dev_err(dev->dev, "sensor_regmap init failed: %ld\n",
			PTR_ERR(dev->sensor_regmap));
		return -ENODEV;
	}

	ret = sensor_read(dev, 0x3000, &chip_version);
	if (ret < 0) {
		dev_err(dev->dev, "sensor_read failed (%d)\n",
			ret);
		return ret;
	}

	if (chip_version != 0x0354) {
		dev_err(dev->dev, "sensor ID mismatch (0x%04x)\n",
			chip_version);
		return -ENXIO;
	}

	/* RESET_REGISTER */
	ret = sensor_write(dev, 0x301A, 0x10D8);
	if (ret)
		return ret;

	/* wait at least 700ms */
	usleep_range(700000, 1000000);

	ret = sensor_set_regs(dev, ar0231_config_part1,
			 ARRAY_SIZE(ar0231_config_part1));
	if (ret)
		return ret;

	ret = sensor_set_regs(dev, ar0231_config_part1b,
			 ARRAY_SIZE(ar0231_config_part1b));
	if (ret)
		return ret;

	ret = sensor_set_regs(dev, ar0231_config_part2,
			 ARRAY_SIZE(ar0231_config_part2));
	if (ret)
		return ret;

	ret = sensor_set_regs(dev, ar0231_config_part6_exposure,
			 ARRAY_SIZE(ar0231_config_part6_exposure));
	if (ret)
		return ret;

	ret = sensor_set_regs(dev, ar0231_config_part7_gains,
			 ARRAY_SIZE(ar0231_config_part7_gains));
	if (ret)
		return ret;

	dev_info(dev->dev, "Identified MARS camera module\n");

	/*
	 * Set reverse channel high threshold to increase noise immunity.
	 *
	 * This should be compensated by increasing the reverse channel
	 * amplitude on the remote deserializer side.
	 */
	return max96705_set_high_threshold(&dev->serializer, true);
}

static int mars_probe(struct i2c_client *client)
{
	struct mars_device *dev;
	struct fwnode_handle *ep;
	int num_ctrls;
	int i;
	int ret;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->dev = &client->dev;

	mutex_init(&dev->mutex);


	dev->serializer.client = client;

	ret = of_property_read_u32_array(client->dev.of_node, "reg",
					 dev->addrs, 2);
	if (ret < 0) {
		dev_err(dev->dev, "Invalid DT reg property: %d\n", ret);
		return -EINVAL;
	}

	/* Create the dummy I2C client for the sensor. */
	dev->sensor = i2c_new_dummy_device(client->adapter,
					   dev->addrs[1]);
	if (IS_ERR(dev->sensor)) {
		ret = PTR_ERR(dev->sensor);
		goto error;
	}

	/* Initialize the hardware. */
	ret = mars_initialize(dev);
	if (ret < 0) {
		dev_err(dev->dev, "mars_initialize: %d\n", ret);
		return ret;
	}

	mars_init_format(&dev->fmt);

	/* Initialize and register the subdevice. */
	v4l2_i2c_subdev_init(&dev->sd, client, &mars_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	num_ctrls = ARRAY_SIZE(ar0231_sd_ctrls);
	dev_dbg(dev->dev, "# of ctrls = %d\n", num_ctrls);

	v4l2_ctrl_handler_init(&dev->ctrls, num_ctrls + 1);

	v4l2_ctrl_new_std(&dev->ctrls, NULL, V4L2_CID_PIXEL_RATE,
			  AR0231_PIXEL_RATE, AR0231_PIXEL_RATE, 1,
			  AR0231_PIXEL_RATE);

	for (i = 0; i < num_ctrls; i++) {
		struct v4l2_ctrl *ctrl;

		dev_dbg(dev->dev, "%d ctrl %s = 0x%x\n",
			i, ar0231_sd_ctrls[i].name, ar0231_sd_ctrls[i].id);

		ctrl = v4l2_ctrl_new_custom(&dev->ctrls,
						&ar0231_sd_ctrls[i], NULL);
		if (!ctrl) {
			dev_err(dev->dev, "Failed for %s ctrl\n",
				ar0231_sd_ctrls[i].name);
			goto error_free_ctrls;
		}
	}

	dev_dbg(dev->dev, "# v4l2 ctrls registered = %d\n", i - 1);

	dev->sd.ctrl_handler = &dev->ctrls;

	ret = dev->ctrls.error;
	if (ret)
		goto error_free_ctrls;

	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.flags |= MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret < 0)
		goto error_free_ctrls;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
	if (!ep) {
		dev_err(&client->dev,
			"Unable to get endpoint in node %pOF\n",
			client->dev.of_node);
		ret = -ENOENT;
		goto error_free_ctrls;
	}
	dev->sd.fwnode = ep;

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto error_put_node;

	return 0;

error_put_node:
	fwnode_handle_put(ep);
error_free_ctrls:
	v4l2_ctrl_handler_free(&dev->ctrls);
error:
	media_entity_cleanup(&dev->sd.entity);
	if (dev->sensor)
		i2c_unregister_device(dev->sensor);

	dev_err(&client->dev, "probe failed\n");

	return ret;
}

static void mars_remove(struct i2c_client *client)
{
	struct mars_device *dev = i2c_to_mars(client);

	fwnode_handle_put(dev->sd.fwnode);
	v4l2_async_unregister_subdev(&dev->sd);
	v4l2_ctrl_handler_free(&dev->ctrls);
	media_entity_cleanup(&dev->sd.entity);
	i2c_unregister_device(dev->sensor);

	mutex_destroy(&dev->mutex);
}

static void mars_shutdown(struct i2c_client *client)
{
	struct mars_device *dev = i2c_to_mars(client);

	/* make sure stream off during shutdown (reset/reboot) */
	mars_s_stream(&dev->sd, 0);
}

static const struct of_device_id mars_of_ids[] = {
	{ .compatible = "onnn,mars", },
	{ }
};
MODULE_DEVICE_TABLE(of, mars_of_ids);

static struct i2c_driver mars_i2c_driver = {
	.driver	= {
		.name	= "mars",
		.of_match_table = mars_of_ids,
	},
	.probe_new	= mars_probe,
	.remove		= mars_remove,
	.shutdown	= mars_shutdown,
};

module_i2c_driver(mars_i2c_driver);

MODULE_DESCRIPTION("GMSL Camera driver for Mars");
MODULE_AUTHOR("Thomas Nizan");
MODULE_LICENSE("GPL");
