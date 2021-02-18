/* SPDX-License-Identifier: GPL-2.0+ */
/*
 */

#include <linux/i2c.h>

#define MAX96705_DEFAULT_ADDR	0x40

/* Register 0x04 */
#define MAX96705_SEREN			BIT(7)
#define MAX96705_CLINKEN			BIT(6)
#define MAX96705_PRBSEN			BIT(5)
#define MAX96705_SLEEP			BIT(4)
#define MAX96705_INTTYPE_I2C		(0 << 2)
#define MAX96705_INTTYPE_UART		(1 << 2)
#define MAX96705_INTTYPE_NONE		(2 << 2)
#define MAX96705_REVCCEN			BIT(1)
#define MAX96705_FWDCCEN			BIT(0)
/* Register 0x07 */
#define MAX96705_DBL			BIT(7)
#define MAX96705_HIBW			BIT(6)
#define MAX96705_BWS			BIT(5)
#define MAX96705_ES			BIT(4)
#define MAX96705_HVEN			BIT(2)
#define MAX96705_EDC_1BIT_PARITY		(0 << 0)
#define MAX96705_EDC_6BIT_CRC		(1 << 0)
/* Register 0x0d */
#define MAX96705_I2CLOCACK		BIT(7)
#define MAX96705_I2CSLVSH_1046NS_469NS	(3 << 5)
#define MAX96705_I2CSLVSH_938NS_352NS	(2 << 5)
#define MAX96705_I2CSLVSH_469NS_234NS	(1 << 5)
#define MAX96705_I2CSLVSH_352NS_117NS	(0 << 5)
#define MAX96705_I2CMSTBT_837KBPS	(7 << 2)
#define MAX96705_I2CMSTBT_533KBPS	(6 << 2)
#define MAX96705_I2CMSTBT_339KBPS	(5 << 2)
#define MAX96705_I2CMSTBT_173KBPS	(4 << 2)
#define MAX96705_I2CMSTBT_105KBPS	(3 << 2)
#define MAX96705_I2CMSTBT_84KBPS		(2 << 2)
#define MAX96705_I2CMSTBT_28KBPS		(1 << 2)
#define MAX96705_I2CMSTBT_8KBPS		(0 << 2)
#define MAX96705_I2CSLVTO_NONE		(3 << 0)
#define MAX96705_I2CSLVTO_1024US		(2 << 0)
#define MAX96705_I2CSLVTO_256US		(1 << 0)
#define MAX96705_I2CSLVTO_64US		(0 << 0)
/* Register 0x0f */
#define MAX96705_GPIO5OUT		BIT(5)
#define MAX96705_GPIO4OUT		BIT(4)
#define MAX96705_GPIO3OUT		BIT(3)
#define MAX96705_GPIO2OUT		BIT(2)
#define MAX96705_GPIO1OUT		BIT(1)
#define MAX96705_GPO			BIT(0)
/* Register 0x15 */
#define MAX96705_PCLKDET			BIT(0)
/* Register 0x1e */
#define MAX96705_ID         0x41
/* Register 0x43 */
#define MAX96705_GEN_VS                 BIT(5)
#define MAX96705_GEN_HS                 BIT(4)
#define MAX96705_GEN_DE                 BIT(3)
#define MAX96705_VS_TRIG_FALL           (0 << 2)
#define MAX96705_VS_TRIG_RISE           (1 << 2)
#define MAX96705_VTG_MODE_VS_TRACKED    (0 << 0)
#define MAX96705_VTG_MODE_VS_FRAME      (1 << 0)
#define MAX96705_VTG_MODE_VS_GEN        (2 << 0)

/**
 * struct max96705_device - max96705 device
 * @client: The i2c client for the max96705 instance
 */
struct max96705_device {
	struct i2c_client *client;
};

/**
 * max96705_set_serial_link() - Enable/disable serial link
 * @dev: The max96705 device
 * @enable: Serial link enable/disable flag
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_set_serial_link(struct max96705_device *dev, bool enable);

/**
 * max96705_configure_i2c() - Configure I2C bus parameters
 * @dev: The max96705 device
 * @i2c_config: The I2C bus configuration bit mask
 *
 * Configure MAX96705 I2C interface. The bus configuration provided in the
 * @i2c_config parameter shall be assembled using bit values defined by the
 * MAX96705_I2C* macros.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_configure_i2c(struct max96705_device *dev, u8 i2c_config);

/**
 * max96705_set_high_threshold() - Enable or disable reverse channel high
 *				  threshold
 * @dev: The max96705 device
 * @enable: High threshold enable/disable flag
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_set_high_threshold(struct max96705_device *dev, bool enable);

/**
 * max96705_configure_gmsl_link() - Configure the GMSL link
 * @dev: The max96705 device
 *
 * FIXME: the GMSL link configuration is currently hardcoded and performed
 * by programming registers 0x04, 0x07 and 0x02.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_configure_gmsl_link(struct max96705_device *dev);

/**
 * max96705_set_gpios() - Set gpio lines to physical high value
 * @dev: The max96705 device
 * @gpio_mask: The mask of gpio lines to set to high value
 *
 * The @gpio_mask parameter shall be assembled using the MAX96705_GP[IO|O]*
 * bit values.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_set_gpios(struct max96705_device *dev, u8 gpio_mask);

/**
 * max96705_clear_gpios() - Set gpio lines to physical low value
 * @dev: The max96705 device
 * @gpio_mask: The mask of gpio lines to set to low value
 *
 * The @gpio_mask parameter shall be assembled using the MAX96705_GP[IO|O]*
 * bit values.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_clear_gpios(struct max96705_device *dev, u8 gpio_mask);

/**
 * max96705_enable_gpios() - Enable gpio lines
 * @dev: The max96705 device
 * @gpio_mask: The mask of gpio lines to enable
 *
 * The @gpio_mask parameter shall be assembled using the MAX96705_GPIO*
 * bit values. GPO line is always enabled by default.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_enable_gpios(struct max96705_device *dev, u8 gpio_mask);

/**
 * max96705_disable_gpios() - Disable gpio lines
 * @dev: The max96705 device
 * @gpio_mask: The mask of gpio lines to disable
 *
 * The @gpio_mask parameter shall be assembled using the MAX96705_GPIO*
 * bit values. GPO line is always enabled by default and cannot be disabled.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_disable_gpios(struct max96705_device *dev, u8 gpio_mask);

/**
 * max96705_verify_id() - Read and verify MAX96705 id
 * @dev: The max96705 device
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_verify_id(struct max96705_device *dev);

/**
 * max96705_set_address() - Program a new I2C address
 * @dev: The max96705 device
 * @addr: The new I2C address in 7-bit format
 *
 * This function only takes care of programming the new I2C address @addr to
 * in the MAX96705 chip registers, it is responsiblity of the caller to set
 * the i2c address client to the @addr value to be able to communicate with
 * the MAX96705 chip using the I2C framework APIs after this function returns.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_set_address(struct max96705_device *dev, u8 addr);

/**
 * max96705_set_deserializer_address() - Program the remote deserializer address
 * @dev: The max96705 device
 * @addr: The deserializer I2C address in 7-bit format
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_set_deserializer_address(struct max96705_device *dev, u8 addr);

/**
 * max96705_set_translation() - Program I2C address translation
 * @dev: The max96705 device
 * @source: The I2C source address
 * @dest: The I2C destination address
 *
 * Program address translation from @source to @dest. This is required to
 * communicate with local devices that do not support address reprogramming.
 *
 * TODO: The device supports translation of two address, this function currently
 * supports a single one.
 *
 * Return 0 on success or a negative error code on failure
 */
int max96705_set_translation(struct max96705_device *dev, u8 source, u8 dest);
