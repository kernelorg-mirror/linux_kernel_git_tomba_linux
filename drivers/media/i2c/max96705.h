/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __MEDIA_I2C_MAX96705_H__
#define __MEDIA_I2C_MAX96705_H__

#include <linux/i2c.h>

#define MAX96705_DEFAULT_ADDR		0x40

#define MAX96705_SERADDR		0x00
#define MAX96705_DESADDR		0x01

#define MAX96705_MAIN_CONTROL		0x04
#define MAX96705_SEREN			BIT(7)
#define MAX96705_CLINKEN		BIT(6)
#define MAX96705_PRBSEN			BIT(5)
#define MAX96705_SLEEP			BIT(4)
#define MAX96705_INTTYPE_I2C		(0 << 2)
#define MAX96705_INTTYPE_UART		(1 << 2)
#define MAX96705_INTTYPE_NONE		(2 << 2)
#define MAX96705_REVCCEN		BIT(1)
#define MAX96705_FWDCCEN		BIT(0)

#define MAX96705_CMLLVL_PREEMP		0x06
#define MAX96705_CMLLVL(n)		(((n) / 50) << 4) /* in mV */
#define MAX96705_PREEMP_1_2DB_DEEMP	(1 << 0)
#define MAX96705_PREEMP_2_5DB_DEEMP	(2 << 0)
#define MAX96705_PREEMP_4_1DB_DEEMP	(3 << 0)
#define MAX96705_PREEMP_6_0DB_DEEMP	(4 << 0)
#define MAX96705_PREEMP_1_1DB_PREEMP	(8 << 0)
#define MAX96705_PREEMP_2_2DB_PREEMP	(9 << 0)
#define MAX96705_PREEMP_3_3DB_PREEMP	(10 << 0)
#define MAX96705_PREEMP_4_4DB_PREEMP	(11 << 0)
#define MAX96705_PREEMP_6_0DB_PREEMP	(12 << 0)
#define MAX96705_PREEMP_8_0DB_PREEMP	(13 << 0)
#define MAX96705_PREEMP_10_5DB_PREEMP	(14 << 0)
#define MAX96705_PREEMP_14_0DB_PREEMP	(15 << 0)

#define MAX96705_CONFIG			0x07
#define MAX96705_DBL			BIT(7)
#define MAX96705_HIBW			BIT(6)
#define MAX96705_BWS			BIT(5)
#define MAX96705_ES			BIT(4)
#define MAX96705_HVEN			BIT(2)
#define MAX96705_EDC_1BIT_PARITY	(0 << 0)
#define MAX96705_EDC_6BIT_CRC		(1 << 0)

#define MAX96705_RSVD_8			0x08

#define MAX96705_I2C_SOURCE_A		0x09
#define MAX96705_I2C_DEST_A		0x0a

#define MAX96705_I2C_CONFIG		0x0d
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
#define MAX96705_I2CMSTBT_84KBPS	(2 << 2)
#define MAX96705_I2CMSTBT_28KBPS	(1 << 2)
#define MAX96705_I2CMSTBT_8KBPS		(0 << 2)
#define MAX96705_I2CSLVTO_NONE		(3 << 0)
#define MAX96705_I2CSLVTO_1024US	(2 << 0)
#define MAX96705_I2CSLVTO_256US		(1 << 0)
#define MAX96705_I2CSLVTO_64US		(0 << 0)

#define MAX96705_GPIO_EN		0x0e
#define MAX96705_GPIO_OUT		0x0f
#define MAX96705_GPIO5OUT		BIT(5)
#define MAX96705_GPIO4OUT		BIT(4)
#define MAX96705_GPIO3OUT		BIT(3)
#define MAX96705_GPIO2OUT		BIT(2)
#define MAX96705_GPIO1OUT		BIT(1)
#define MAX96705_GPO			BIT(0)

#define MAX96705_INPUT_STATUS		0x15
#define MAX96705_PCLKDET		BIT(0)

#define MAX96705_ID			0x1e
#define MAX96705_ID_VALUE		0x41

#define MAX96705_SYNC_GEN_CONFIG	0x43
#define MAX96705_GEN_VS                 BIT(5)
#define MAX96705_GEN_HS                 BIT(4)
#define MAX96705_GEN_DE                 BIT(3)
#define MAX96705_VS_TRIG_FALL           (0 << 2)
#define MAX96705_VS_TRIG_RISE           (1 << 2)
#define MAX96705_VTG_MODE_VS_TRACKED    (0 << 0)
#define MAX96705_VTG_MODE_VS_FRAME      (1 << 0)
#define MAX96705_VTG_MODE_VS_GEN        (2 << 0)

#define MAX96705_VS_DLY_2		0x44
#define MAX96705_VS_DLY_1		0x45
#define MAX96705_VS_DLY_0		0x46
#define MAX96705_VS_H_2			0x47
#define MAX96705_VS_H_1			0x48
#define MAX96705_VS_H_0			0x49

#define MAX96705_DBL_ALIGN_TO		0x67
#define MAX96705_AUTO_CLINK		BIT(5)
#define MAX96705_DBL_ALIGN_TO_EXT_HI_LO	(0 << 0)
#define MAX96705_DBL_ALIGN_TO_FORCE	(2 << 0)
#define MAX96705_DBL_ALIGN_TO_HS	(4 << 0)
#define MAX96705_DBL_ALIGN_TO_DE	(5 << 0)
#define MAX96705_DBL_ALIGN_TO_NONE	(7 << 0)

#define MAX96705_RSVD_97		0x97
#define MAX96705_REV_OSMPL		BIT(7)
#define MAX96705_EN_REV_CFG		BIT(6)
#define MAX96705_REV_HICUT2		BIT(5)
#define MAX96705_REV_HICAP		BIT(4)
#define MAX96705_REV_HIRES		BIT(3)
#define MAX96705_REV_PRES(n)		((n) << 0)


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

#endif /* __MEDIA_I2C_MAX96705_H__ */
