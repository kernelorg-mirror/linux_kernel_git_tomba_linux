// SPDX-License-Identifier: GPL-2.0
/**
 * Driver for the Texas Instruments DS90UB913-Q1 video serializer
 *
 * Based on a driver from Luca Ceresoli <luca@lucaceresoli.net>
 *
 * Copyright (c) 2019 Luca Ceresoli <luca@lucaceresoli.net>
 * Copyright (c) 2021 Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <dt-bindings/media/ds90ub953.h>

#define UB913_NUM_GPIOS			4

#define UB913_REG_RESET_CTL		0x01
#define UB913_REG_RESET_CTL_DIGITAL_RESET_1	BIT(1)
#define UB913_REG_RESET_CTL_DIGITAL_RESET_0	BIT(0)

#define UB913_REG_GENERAL_CFG		0x03
#define UB913_REG_MODE_SEL		0x05

#define UB913_REG_GPIO_CFG(n)		(0x0d + (n))
#define UB913_REG_GPIO_CFG_ENABLE(n)	BIT(0 + (n) * 4)
#define UB913_REG_GPIO_CFG_DIR_INPUT(n)	BIT(1 + (n) * 4)
#define UB913_REG_GPIO_CFG_REMOTE_EN(n)	BIT(2 + (n) * 4)
#define UB913_REG_GPIO_CFG_OUT_VAL(n)	BIT(3 + (n) * 4)

struct ub913_data {
	struct i2c_client *client;

	u32 gpio_func[UB913_NUM_GPIOS];
};

static s32 ub913_read(const struct ub913_data *priv, u8 reg)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(priv->client, reg);
	if (ret < 0)
		dev_err(&priv->client->dev, "Cannot read register 0x%02x: %d!\n",
			reg, ret);

	return ret;
}

static s32 ub913_write(const struct ub913_data *priv, u8 reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(priv->client, reg, val);
	if (ret < 0)
		dev_err(&priv->client->dev, "Cannot write register 0x%02x: %d!\n",
			reg, ret);

	return ret;
}

static void ub913_configure_gpios(struct ub913_data *priv)
{
	#if 0
	struct device *dev = &priv->client->dev;
	u8 gpio_reg_val[2] = { 0 };
	int i;

	for (i = 0; i < ARRAY_SIZE(priv->gpio_func); i++) {
		unsigned int reg_idx;
		unsigned int field_idx;

		reg_idx = i / 2;
		field_idx = i % 2;

		switch (priv->gpio_func[i]) {
		case DS90_GPIO_FUNC_UNUSED:
			break;
		case DS90_GPIO_FUNC_OUTPUT:
			gpio_reg_val[reg_idx] |=
				UB913_REG_GPIO_CFG_ENABLE(field_idx) | UB913_REG_GPIO_CFG_OUT_VAL(field_idx);
			break;
		case DS90_GPIO_FUNC_INPUT:
			gpio_reg_val[reg_idx] |=
				UB913_REG_GPIO_CFG_ENABLE(field_idx) |
				UB913_REG_GPIO_CFG_DIR_INPUT(field_idx);
			break;
		case DS90_GPIO_FUNC_OUTPUT_REMOTE:
			gpio_reg_val[reg_idx] |=
				UB913_REG_GPIO_CFG_ENABLE(field_idx) |
				UB913_REG_GPIO_CFG_REMOTE_EN(field_idx);
			break;
		default:
			dev_err(dev,
				"Unknown gpio-functions value %u, GPIO%d will be unused",
				priv->gpio_func[i], i);
			break;
		}
	}

	ub913_write(priv, UB913_REG_GPIO_CFG(0), gpio_reg_val[0]);
	ub913_write(priv, UB913_REG_GPIO_CFG(1), gpio_reg_val[1]);
#endif
}


/*
 * Reset via registers (useful from remote).
 * Note: the procedure is undocumented, but this one seems to work.
 */
static void ub913_soft_reset(struct ub913_data *priv)
{
	int retries = 10;
	s32 ret;

	while (retries-- > 0) {
		ret = ub913_write(priv, UB913_REG_RESET_CTL,
				 UB913_REG_RESET_CTL_DIGITAL_RESET_0);
		if (ret >= 0)
			break;
		usleep_range(1000, 3000);
	}

	retries = 10;
	while (retries-- > 0) {
		ret = ub913_read(priv, UB913_REG_RESET_CTL);
		if (ret >= 0 && (ret & UB913_REG_RESET_CTL_DIGITAL_RESET_0) == 0) {
			printk("ub913 reset done\n");
			break;
		}
		usleep_range(1000, 3000);
	}
}

static int ub913_parse_dt(struct ub913_data *priv)
{
	struct device_node *np = priv->client->dev.of_node;
	struct device *dev = &priv->client->dev;
	int err;

	if (!np) {
		dev_err(dev, "OF: no device tree node!\n");
		return -ENOENT;
	}

	/* optional, if absent all GPIO pins are unused */
	err = of_property_read_u32_array(np, "gpio-functions", priv->gpio_func,
					ARRAY_SIZE(priv->gpio_func));
	if (err && err != -EINVAL)
		dev_err(dev, "DT: invalid gpio-functions property (%d)", err);

	return 0;
}

static int ub913_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ub913_data *priv;
	int err;

	dev_dbg(dev, "probing, addr 0x%02x\n", client->addr);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	i2c_set_clientdata(client, priv);

	ub913_soft_reset(priv);

	err = ub913_parse_dt(priv);
	if (err)
		goto err_parse_dt;

	ub913_configure_gpios(priv);

	dev_info(dev, "Successfully probed\n");

	return 0;

err_parse_dt:
	return err;
}

static int ub913_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "Removing\n");
	return 0;
}

static const struct i2c_device_id ub913_id[] = {
	{ "ds90ub913a-q1", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ub913_id);

#ifdef CONFIG_OF
static const struct of_device_id ub913_dt_ids[] = {
	{ .compatible = "ti,ds90ub913a-q1", },
	{ }
};
MODULE_DEVICE_TABLE(of, ub913_dt_ids);
#endif

static struct i2c_driver ds90ub913_driver = {
	.probe_new	= ub913_probe,
	.remove		= ub913_remove,
	.id_table	= ub913_id,
	.driver = {
		.name	= "ds90ub913a",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ub913_dt_ids),
	},
};

module_i2c_driver(ds90ub913_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Texas Instruments DS90UB913-Q1 CSI-2 serializer driver");
MODULE_AUTHOR("Luca Ceresoli <luca@lucaceresoli.net>");
