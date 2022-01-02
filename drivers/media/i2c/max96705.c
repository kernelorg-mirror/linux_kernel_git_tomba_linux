// SPDX-License-Identifier: GPL-2.0+
/*
 * This file exports functions to control the Maxim MAX96705 GMSL serializer
 * chip. This is not a self-contained driver, as MAX96705 is usually embedded in
 * camera modules with at least one image sensor and optional additional
 * components, such as uController units or ISPs/DSPs.
 *
 * Drivers for the camera modules (i.e. mars module) are expected to use
 * functions exported from this library driver to maximize code re-use.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include "max96705.h"

static int max96705_read(struct max96705_device *dev, u8 reg)
{
	int ret;
	int retry = 5;

	while (retry--) {
		ret = i2c_smbus_read_byte_data(dev->client, reg);
		if (ret < 0) {
			dev_err(&dev->client->dev,
				"%s: register 0x%02x read failed (%d)\n",
				__func__, reg, ret);
		} else {
			break;
		}
		usleep_range(5000, 10000);
	}

	if (ret < 0) {
		dev_err(&dev->client->dev,
			"%s: register 0x%02x read failed (%d) - all retries failed\n",
			__func__, reg, ret);
		return ret;
	}

	dev_dbg(&dev->client->dev, "%s(0x%02x) -> 0x%02x\n", __func__, reg, ret);

	return ret;
}

static int max96705_write(struct max96705_device *dev, u8 reg, u8 val)
{
	int ret;
	int retry = 5;

	dev_dbg(&dev->client->dev, "%s(0x%02x, 0x%02x)\n", __func__, reg, val);

	while (retry--) {
		ret = i2c_smbus_write_byte_data(dev->client, reg, val);
		if (ret < 0) {
			dev_err(&dev->client->dev,
				"%s: register 0x%02x write failed (%d)\n",
				__func__, reg, ret);
		} else {
			break;
		}
		usleep_range(5000, 10000);
	}

	if (ret < 0)
		dev_err(&dev->client->dev,
			"%s: register 0x%02x write failed (%d) - all retries failed\n",
			__func__, reg, ret);

	return ret;
}

/*
 * max96705_pclk_detect() - Detect valid pixel clock from image sensor
 *
 * Wait up to 10ms for a valid pixel clock.
 *
 * Returns 0 for success, < 0 for pixel clock not properly detected
 */
static int max96705_pclk_detect(struct max96705_device *dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max96705_read(dev, MAX96705_INPUT_STATUS);
		if (ret < 0)
			return ret;

		if (ret & MAX96705_PCLKDET)
			return 0;

		usleep_range(50, 100);
	}

	dev_err(&dev->client->dev, "Unable to detect valid pixel clock\n");

	return -EIO;
}

int max96705_set_serial_link(struct max96705_device *dev, bool enable)
{
	int ret;
	u8 val = MAX96705_REVCCEN | MAX96705_FWDCCEN;

	if (enable) {
		ret = max96705_pclk_detect(dev);
		if (ret)
			return ret;

		val |= MAX96705_SEREN;
	} else {
		val |= MAX96705_CLINKEN;
	}

	/*
	 * The serializer temporarily disables the reverse control channel for
	 * 350µs after starting/stopping the forward serial link, but the
	 * deserializer synchronization time isn't clearly documented.
	 *
	 * According to the serializer datasheet we should wait 3ms, while
	 * according to the deserializer datasheet we should wait 5ms.
	 *
	 * Short delays here appear to show bit-errors in the writes following.
	 * Therefore a conservative delay seems best here.
	 */
	ret = max96705_write(dev, MAX96705_MAIN_CONTROL, val);
	if (ret < 0)
		return ret;

	usleep_range(5000, 8000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_set_serial_link);

int max96705_configure_i2c(struct max96705_device *dev, u8 i2c_config)
{
	int ret;

	ret = max96705_write(dev, MAX96705_I2C_CONFIG, i2c_config);
	if (ret < 0)
		return ret;

	/* The delay required after an I2C bus configuration change is not
	 * characterized in the serializer manual. Sleep up to 5msec to
	 * stay safe.
	 */
	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_configure_i2c);

int max96705_set_high_threshold(struct max96705_device *dev, bool enable)
{

	/*
	 * WORK IN PROGRESS: Experiencing some issues writing/reading registers
	 * 0x08 and 0x97
	 * Reg 0x97 is sometimes initialized with value 0xff (instead of 0x1f)
	 *      when this happens, I can't read this register, but the driver
	 *      will keep working later
	 *
	 * Currently trying to get more information on this registers
	 */

	int ret;
	int retry = 5;
	int write_cmd = 0;

	max96705_read(dev, MAX96705_RSVD_97);

	ret = max96705_read(dev, MAX96705_RSVD_8);
	if (ret < 0)
		return ret;

	/*
	 * Enable or disable reverse channel high threshold to increase
	 * immunity to power supply noise.
	 */

	write_cmd = enable ? ret | BIT(0) : ret & ~BIT(0);
	while (retry--) {
		ret = max96705_write(dev, MAX96705_RSVD_8, write_cmd);

		usleep_range(200000, 250000);

		ret = max96705_read(dev, MAX96705_RSVD_8);

		if (write_cmd == ret)
			break;
	}

	if (retry < 0) {
		dev_info(&dev->client->dev,
			 "FAILED TO WRITE CORRECTLY 0x08: ret = 0x%02x\n", ret);
	}

	retry = 5;
	write_cmd = MAX96705_EN_REV_CFG | MAX96705_REV_HICAP
		  | MAX96705_REV_HIRES | MAX96705_REV_PRES(7);

	while (retry--) {
		ret = max96705_write(dev, MAX96705_RSVD_97, write_cmd);

		usleep_range(200000, 250000);

		ret = max96705_read(dev, MAX96705_RSVD_97);

		if (write_cmd == ret)
			break;
	}

	if (retry < 0) {
		dev_info(&dev->client->dev,
			 "FAILED TO WRITE CORRECTLY 0x97: ret = 0x%02x\n", ret);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_set_high_threshold);

int max96705_configure_gmsl_link(struct max96705_device *dev)
{
	unsigned int value;
	int ret;

	/*
	 * Configure the GMSL link:
	 *
	 * - Double input mode, High-Bandwidth, 24-bit mode
	 * - Enable HS/VS encoding
	 * - 1-bit parity error detection
	 *
	 * TODO: Make the GMSL link configuration parametric.
	 */
	ret = max96705_write(dev, MAX96705_CONFIG, MAX96705_DBL | MAX96705_HIBW |
			     MAX96705_HVEN | MAX96705_EDC_1BIT_PARITY);
	if (ret < 0)
		return ret;

	usleep_range(5000, 8000);

	ret = max96705_write(dev, MAX96705_CMLLVL_PREEMP, MAX96705_CMLLVL(500) |
			     MAX96705_PREEMP_6_0DB_PREEMP);
	if (ret < 0)
		return ret;

	usleep_range(5000, 8000);

	/*
	 * Enable vsync re-gen (VS internally generated),
	 * falling edge triggers one VS frame
	 */
	ret = max96705_write(dev, MAX96705_SYNC_GEN_CONFIG, MAX96705_GEN_VS |
			     MAX96705_VS_TRIG_FALL |
			     MAX96705_VTG_MODE_VS_FRAME);
	if (ret < 0)
		return ret;

	/* Set VSync Delay,  should be on the order of 4 lines or more */
	value = 2162 * 4;
	ret = max96705_write(dev, MAX96705_VS_DLY_2, (value >> 16) & 0xff);
	if (ret < 0)
		return ret;

	ret = max96705_write(dev, MAX96705_VS_DLY_1, (value >> 8) & 0xff);
	if (ret < 0)
		return ret;

	ret = max96705_write(dev, MAX96705_VS_DLY_0, (value >> 0) & 0xff);
	if (ret < 0)
		return ret;

	/* Set VSync High time, should be > 200 Pclks */
	value = 200;
	ret = max96705_write(dev, MAX96705_VS_H_2, (value >> 16) & 0xff);
	if (ret < 0)
		return ret;

	ret = max96705_write(dev, MAX96705_VS_H_1, (value >> 8) & 0xff);
	if (ret < 0)
		return ret;

	ret = max96705_write(dev, MAX96705_VS_H_0, (value >> 0) & 0xff);
	if (ret < 0)
		return ret;

	// align at HS rising edge
	ret = max96705_write(dev, MAX96705_DBL_ALIGN_TO,
			     0xc0 | MAX96705_DBL_ALIGN_TO_HS);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_configure_gmsl_link);

int max96705_set_gpios(struct max96705_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max96705_read(dev, MAX96705_GPIO_OUT);
	if (ret < 0)
		return 0;

	ret |= gpio_mask;
	ret = max96705_write(dev, MAX96705_GPIO_OUT, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to set gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_set_gpios);

int max96705_clear_gpios(struct max96705_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max96705_read(dev, MAX96705_GPIO_OUT);
	if (ret < 0)
		return 0;

	ret &= ~gpio_mask;
	ret = max96705_write(dev, MAX96705_GPIO_OUT, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to clear gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_clear_gpios);

int max96705_enable_gpios(struct max96705_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max96705_read(dev, MAX96705_GPIO_EN);
	if (ret < 0)
		return 0;

	/* BIT(0) reserved: GPO is always enabled. */
	ret |= (gpio_mask & ~BIT(0));
	ret = max96705_write(dev, MAX96705_GPIO_EN, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to enable gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_enable_gpios);

int max96705_disable_gpios(struct max96705_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max96705_read(dev, MAX96705_GPIO_EN);
	if (ret < 0)
		return 0;

	/* BIT(0) reserved: GPO cannot be disabled */
	ret &= ~(gpio_mask | BIT(0));
	ret = max96705_write(dev, MAX96705_GPIO_EN, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to disable gpio (%d)\n",
			ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_disable_gpios);

int max96705_verify_id(struct max96705_device *dev)
{
	int ret;

	ret = max96705_read(dev, MAX96705_ID);
	if (ret < 0)
		return ret;

	if (ret != MAX96705_ID_VALUE) {
		dev_err(&dev->client->dev, "MAX96705 ID mismatch (0x%02x)\n",
			ret);
		return -ENXIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_verify_id);

int max96705_set_address(struct max96705_device *dev, u8 addr)
{
	int ret;

	ret = max96705_write(dev, MAX96705_SERADDR, addr << 1);
	if (ret < 0) {
		dev_err(&dev->client->dev,
			"MAX96705 I2C address change failed (%d)\n", ret);
		return ret;
	}
	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_set_address);

int max96705_set_deserializer_address(struct max96705_device *dev, u8 addr)
{
	int ret;

	ret = max96705_write(dev, MAX96705_DESADDR, addr << 1);
	if (ret < 0) {
		dev_err(&dev->client->dev,
			"MAX96705 deserializer address set failed (%d)\n", ret);
		return ret;
	}
	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_set_deserializer_address);

int max96705_set_translation(struct max96705_device *dev, u8 source, u8 dest)
{
	int ret;

	ret = max96705_write(dev, MAX96705_I2C_SOURCE_A, source << 1);
	if (ret < 0) {
		dev_err(&dev->client->dev,
			"MAX96705 I2C translation setup failed (%d)\n", ret);
		return ret;
	}
	usleep_range(3500, 5000);

	ret = max96705_write(dev, MAX96705_I2C_DEST_A, dest << 1);
	if (ret < 0) {
		dev_err(&dev->client->dev,
			"MAX96705 I2C translation setup failed (%d)\n", ret);
		return ret;
	}
	usleep_range(3500, 5000);

	return 0;
}
EXPORT_SYMBOL_GPL(max96705_set_translation);

MODULE_DESCRIPTION("Maxim MAX96705 GMSL Serializer");
MODULE_AUTHOR("Thomas Nizan");
MODULE_LICENSE("GPL v2");
