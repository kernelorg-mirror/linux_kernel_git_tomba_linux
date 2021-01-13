// SPDX-License-Identifier: GPL-2.0
/**
 * Driver for the Texas Instruments DS90UB913-Q1 video serializer
 *
 * Copyright (c) 2019 Luca Ceresoli <luca@lucaceresoli.net>
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <dt-bindings/media/ds90ub953.h>

#define DS90_NUM_GPIOS			4  /* Physical GPIO pins */


#define DS90_REG_DEVICE_ID		0x00

#define DS90_REG_RESET_CTL		0x01
//#define DS90_REG_RESET_CTL_RESTART_AUTOLOAD	BIT(2)
#define DS90_REG_RESET_CTL_DIGITAL_RESET_1	BIT(1)
#define DS90_REG_RESET_CTL_DIGITAL_RESET_0	BIT(0)

#define DS90_REG_GENERAL_CFG		0x03
#define DS90_REG_MODE_SEL		0x05

#define DS90_REG_SCL_HIGH_TIME		0x11
#define DS90_REG_SCL_LOW_TIME		0x12

#if 0

#define DS90_REG_BC_MODE_SELECT		0x04
#define DS90_REG_PLLCLK_CTRL		0x05
#define DS90_REG_CLKOUT_CTRL0		0x06
#define DS90_REG_CLKOUT_CTRL1		0x07
#define DS90_REG_BCC_WATCHDOG		0x08
#define DS90_REG_I2C_CONTROL1		0x09
#define DS90_REG_I2C_CONTROL2		0x0A
#define DS90_REG_SCL_HIGH_TIME		0x0B
#define DS90_REG_SCL_LOW_TIME		0x0C

#define DS90_REG_LOCAL_GPIO_DATA	0x0D
#define DS90_REG_LOCAL_GPIO_DATA_RMTEN(n)	BIT((n) + 4)
#define DS90_REG_LOCAL_GPIO_DATA_OUT_SRC(n)	BIT((n) + 4)

#define DS90_REG_GPIO_INPUT_CTRL	0x0E
#define DS90_REG_GPIO_INPUT_CTRL_INPUT_EN(n)	BIT((n))
#define DS90_REG_GPIO_INPUT_CTRL_OUT_EN(n)	BIT((n) + 4)

#define DS90_REG_DVP_CFG		0x10
#define DS90_REG_DVP_DT			0x11
#define DS90_REG_FORCE_BIST_ERR		0x13
#define DS90_REG_REMOTE_BIST_CTRL	0x14
#define DS90_REG_SENSOR_VGAIN		0x15
#define DS90_REG_SENSOR_CTRL0		0x17
#define DS90_REG_SENSOR_CTRL1		0x18
#define DS90_REG_SENSOR_V0_THRESH	0x19
#define DS90_REG_SENSOR_V1_THRESH	0x1A
#define DS90_REG_SENSOR_T_THRESH	0x1B
#define DS90_REG_SENSOR_T_THRESH	0x1B
#define DS90_REG_ALARM_CSI_EN		0x1C
#define DS90_REG_ALARM_SENSE_EN		0x1D
#define DS90_REG_ALARM_BC_EN		0x1E

#define DS90_REG_CSI_POL_SEL		0x20
#define DS90_REG_CSI_POL_SEL_POLARITY_CLK0	BIT(4)

#define DS90_REG_CSI_LP_POLARITY	0x21
#define DS90_REG_CSI_LP_POLARITY_POL_LP_CLK0	BIT(4)

#define DS90_REG_CSI_EN_HSRX		0x22
#define DS90_REG_CSI_EN_LPRX		0x23
#define DS90_REG_CSI_EN_RXTERM		0x24
#define DS90_REG_CSI_PKT_HDR_TINIT_CTRL 0x31
#define DS90_REG_BCC_CONFIG		0x32
#define DS90_REG_DATAPATH_CTL1		0x33
#define DS90_REG_REMOTE_PAR_CAP1	0x35
#define DS90_REG_DES_ID			0x37
#define DS90_REG_SLAVE_ID_0		0x39
#define DS90_REG_SLAVE_ID_1		0x3A
#define DS90_REG_SLAVE_ID_2		0x3B
#define DS90_REG_SLAVE_ID_3		0x3C
#define DS90_REG_SLAVE_ID_4		0x3D
#define DS90_REG_SLAVE_ID_5		0x3E
#define DS90_REG_SLAVE_ID_6		0x3F
#define DS90_REG_SLAVE_ID_7		0x40
#define DS90_REG_SLAVE_ID_ALIAS_0	0x41
#define DS90_REG_SLAVE_ID_ALIAS_0	0x41
#define DS90_REG_SLAVE_ID_ALIAS_1	0x42
#define DS90_REG_SLAVE_ID_ALIAS_2	0x43
#define DS90_REG_SLAVE_ID_ALIAS_3	0x44
#define DS90_REG_SLAVE_ID_ALIAS_4	0x45
#define DS90_REG_SLAVE_ID_ALIAS_5	0x46
#define DS90_REG_SLAVE_ID_ALIAS_6	0x47
#define DS90_REG_SLAVE_ID_ALIAS_7	0x48
#define DS90_REG_BC_CTRL		0x49
#define DS90_REG_REV_MASK_ID		0x50

#define DS90_REG_DEVICE_STS		0x51
#define DS90_REG_DEVICE_STS_CFG_INIT_DONE	BIT(6)

#define DS90_REG_GENERAL_STATUS		0x52
#define DS90_REG_GPIO_PIN_STS		0x53
#define DS90_REG_BIST_ERR_CNT		0x54
#define DS90_REG_CRC_ERR_CNT1		0x55
#define DS90_REG_CRC_ERR_CNT2		0x56
#define DS90_REG_SENSOR_STATUS		0x57
#define DS90_REG_SENSOR_V0		0x58
#define DS90_REG_SENSOR_V1		0x59
#define DS90_REG_SENSOR_T		0x5A
#define DS90_REG_SENSOR_T		0x5A
#define DS90_REG_CSI_ERR_CNT		0x5C
#define DS90_REG_CSI_ERR_STATUS		0x5D
#define DS90_REG_CSI_ERR_DLANE01	0x5E
#define DS90_REG_CSI_ERR_DLANE23	0x5F
#define DS90_REG_CSI_ERR_CLK_LANE	0x60
#define DS90_REG_CSI_PKT_HDR_VC_ID	0x61
#define DS90_REG_PKT_HDR_WC_LSB		0x62
#define DS90_REG_PKT_HDR_WC_MSB		0x63
#define DS90_REG_CSI_ECC		0x64
#define DS90_REG_IND_ACC_CTL		0xB0
#define DS90_REG_IND_ACC_ADDR		0xB1
#define DS90_REG_IND_ACC_DATA		0xB2
#define DS90_REG_FPD3_RX_ID0		0xF0
#define DS90_REG_FPD3_RX_ID1		0xF1
#define DS90_REG_FPD3_RX_ID2		0xF2
#define DS90_REG_FPD3_RX_ID3		0xF3
#define DS90_REG_FPD3_RX_ID4		0xF4
#define DS90_REG_FPD3_RX_ID5		0xF5
#endif

struct ds90_data {
	struct i2c_client      *client;

	u32 gpio_func[DS90_NUM_GPIOS];
};

/* -----------------------------------------------------------------------------
 * Basic device access
 */
static s32 ds90_read(const struct ds90_data *ds90, u8 reg)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(ds90->client, reg);
	if (ret < 0)
		dev_err(&ds90->client->dev, "Cannot read register 0x%02x: %d!\n",
			reg, ret);

	return ret;
}

static s32 ds90_write(const struct ds90_data *ds90, u8 reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(ds90->client, reg, val);
	if (ret < 0)
		dev_err(&ds90->client->dev, "Cannot write register 0x%02x: %d!\n",
			reg, ret);

	return ret;
}

/* -----------------------------------------------------------------------------
 * GPIOs
 */
#if 0
static int ds90_configure_gpios(struct ds90_data *ds90)
{
	struct device *dev = &ds90->client->dev;
	u8 gpio_input_ctrl = 0;
	u8 local_gpio_data = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(ds90->gpio_func); i++) {

		if (ds90->gpio_func[i] >= DS90_GPIO_N_FUNCS) {
			dev_err(dev,
				"Unknown gpio-functions value %u, GPIO%d will be unused",
				ds90->gpio_func[i], i);
			continue;
		}

		switch (ds90->gpio_func[i]) {
		case DS90_GPIO_FUNC_INPUT:
			gpio_input_ctrl |= DS90_REG_GPIO_INPUT_CTRL_INPUT_EN(i);
			break;
		case DS90_GPIO_FUNC_OUTPUT_REMOTE:
			gpio_input_ctrl |= DS90_REG_GPIO_INPUT_CTRL_OUT_EN(i);
			local_gpio_data |= DS90_REG_LOCAL_GPIO_DATA_RMTEN(i);
			break;
		}
	}

	ds90_write(ds90, DS90_REG_LOCAL_GPIO_DATA, local_gpio_data);
	ds90_write(ds90, DS90_REG_GPIO_INPUT_CTRL, gpio_input_ctrl);
	/* TODO setting DATAPATH_CTL1 is needed for inputs? */

	return 0;
}
#endif

/* -----------------------------------------------------------------------------
 * Core
 */

/*
 * Reset via registers (useful from remote).
 * Note: the procedure is undocumented, but this one seems to work.
 */
static void ds90_soft_reset(struct ds90_data *ds90)
{
	int retries = 10;
	s32 ret;

	while (retries-- > 0) {
		ret = ds90_write(ds90, DS90_REG_RESET_CTL,
				 DS90_REG_RESET_CTL_DIGITAL_RESET_0);
		if (ret >= 0)
			break;
		usleep_range(1000, 3000);
	}

	retries = 10;
	while (retries-- > 0) {
		ret = ds90_read(ds90, DS90_REG_RESET_CTL);
		if (ret >= 0 && (ret & DS90_REG_RESET_CTL_DIGITAL_RESET_0) == 0) {
			printk("ub913 reset done\n");
			break;
		}
		usleep_range(1000, 3000);
	}
}

static int ds90_parse_dt(struct ds90_data *ds90)
{
	struct device_node *np = ds90->client->dev.of_node;
	struct device *dev = &ds90->client->dev;
	int err;

	if (!np) {
		dev_err(dev, "OF: no device tree node!\n");
		return -ENOENT;
	}

	/* optional, if absent all GPIO pins are unused */
	err = of_property_read_u32_array(np, "gpio-functions", ds90->gpio_func,
					ARRAY_SIZE(ds90->gpio_func));
	if (err && err != -EINVAL)
		dev_err(dev, "DT: invalid gpio-functions property (%d)", err);
#if 0
	if (of_property_read_bool(np, "ti,ds90ub953-q1-clk-inv-pol-quirk")) {
		ds90_write(ds90,
			   DS90_REG_CSI_POL_SEL,
			   DS90_REG_CSI_POL_SEL_POLARITY_CLK0);
		ds90_write(ds90,
			   DS90_REG_CSI_LP_POLARITY,
			   DS90_REG_CSI_LP_POLARITY_POL_LP_CLK0);
	}
#endif
	return 0;
}

static int ds90_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ds90_data *ds90;
	s32 rev_mask;
	int err;

	dev_dbg(dev, "probing, addr 0x%02x\n", client->addr);

	ds90 = devm_kzalloc(dev, sizeof(*ds90), GFP_KERNEL);
	if (!ds90)
		return -ENOMEM;

	ds90->client = client;
	i2c_set_clientdata(client, ds90);

	ds90_soft_reset(ds90);

	//rev_mask = ds90_read(ds90, DS90_REG_REV_MASK_ID);
	rev_mask = 0x1234; // no rev id?
	if (rev_mask < 0) {
		err = rev_mask;
		dev_err(dev,
			"Cannot read first register (%d), abort\n", err);
		goto err_reg_read;
	}

	err = ds90_parse_dt(ds90);
	if (err)
		goto err_parse_dt;

	/* I2C fast mode 400 kHz */
	/* TODO compute values from REFCLK */
	//ds90_write(ds90, DS90_REG_SCL_HIGH_TIME, 0x13);
	//ds90_write(ds90, DS90_REG_SCL_LOW_TIME,  0x26);

#if 0
	/*
	 * TODO compute these hard-coded values
	 *
	 * SET CLK_OUT:
	 * MODE    = 0 = CSI-2 sync (strap, reg 0x03) -> refclk from deser
	 * REFCLK  = 23..26 MHz (REFCLK pin @ remote deserializer)
	 * FC      = fwd channel data rate = 160 x REFCLK
	 * CLK_OUT = FC * M / (HS_CLK_DIV * N)
	 *         = FC * 1 / (4 * 20) = 2 * REFCLK
	 */
	ds90_write(ds90, DS90_REG_CLKOUT_CTRL0, 0x41); /* div by 4, M=1 */
	ds90_write(ds90, DS90_REG_CLKOUT_CTRL1,   20); /* N=20 */
#endif

	//err = ds90_configure_gpios(ds90);
	//if (err)
	//	goto err_configure_gpios;

	dev_info(dev, "Successfully probed (rev/mask %02x)\n", rev_mask);

	return 0;

//err_configure_gpios:
err_parse_dt:
err_reg_read:
	return err;
}

static int ds90_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "Removing\n");
	return 0;
}

static const struct i2c_device_id ds90_id[] = {
	{ "ds90ub913a-q1", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ds90_id);

#ifdef CONFIG_OF
static const struct of_device_id ds90_dt_ids[] = {
	{ .compatible = "ti,ds90ub913a-q1", },
	{ }
};
MODULE_DEVICE_TABLE(of, ds90_dt_ids);
#endif

static struct i2c_driver ds90ub913_driver = {
	.probe_new	= ds90_probe,
	.remove		= ds90_remove,
	.id_table	= ds90_id,
	.driver = {
		.name	= "ds90ub913a",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ds90_dt_ids),
	},
};

module_i2c_driver(ds90ub913_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Texas Instruments DS90UB913-Q1 CSI-2 serializer driver");
MODULE_AUTHOR("Luca Ceresoli <luca@lucaceresoli.net>");
