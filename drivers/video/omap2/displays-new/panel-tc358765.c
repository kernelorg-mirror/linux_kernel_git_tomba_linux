/*
 * Toshiba TC358765 DSI-to-LVDS chip driver
 *
 * Copyright (C) Texas Instruments
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com> (3.0)
 * Author: Sergii Kibrik <sergiikibrik@ti.com> (3.4)
 * Author: Ruslan Bilovol <ruslan.bilovol@ti.com> (3.8+)
 *
 * Based on original version from Jerry Alexander <x0135174@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DEBUG

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>

#include <video/omapdss.h>

#include "panel-tc358765.h"

#define A_RO 0x1
#define A_WO 0x2
#define A_RW (A_RO|A_WO)

#define FLD_MASK(start, end)	(((1 << ((start) - (end) + 1)) - 1) << (end))
#define FLD_VAL(val, start, end) (((val) << (end)) & FLD_MASK(start, end))
#define FLD_GET(val, start, end) (((val) & FLD_MASK(start, end)) >> (end))
#define FLD_MOD(orig, val, start, end) \
	(((orig) & ~FLD_MASK(start, end)) | FLD_VAL(val, start, end))

static const struct omap_video_timings tc358765_timings = {
	.pixel_clock = 65000,
	.x_res       = 1024,
	.y_res       = 768,
	.hfp         = 150,
	.hsw         = 40,
	.hbp         = 150,
	.vfp         = 15,
	.vsw         = 8,
	.vbp         = 15,
};

/**
 * struct tc358765_board_data - represent DSI-to-LVDS bridge configuration
 * @lp_time: Timing Generation Counter
 * @clrsipo: CLRSIPO counter (one value for all lanes)
 * @lv_is: charge pump control pin
 * @lv_nd: Feed Back Divider Ratio
 * @pclkdiv: PCLK Divide Option
 * @pclksel: PCLK Selection: HSRCK/HbyteHSClkx2/ByteHsClk
 * @lvdlink: is single or dual link
 * @msf: enable/disable Magic Square
 * @evtmode: event/pulse mode of video timing information transmission
 * @pin_config: DSI pin configuration
*/
static struct tc358765_board_data {
	int	num_lanes;
	u16	lp_time;
	u8	clrsipo;
	u8	lv_is;
	u8	lv_nd;
	u8	pclkdiv;
	u8	pclksel;
	bool	lvdlink;
	bool	msf;
	bool	evtmode;
} test_board_data = {
	.num_lanes	= 5,
	.lp_time        = 0x4,
	.clrsipo        = 0x3,
	.lv_is          = 0x1,
	.lv_nd          = 0x6,
};

/* device private data structure */
struct panel_drv_data {
	struct omap_dss_device dssdev;
	struct omap_dss_device *in;

	struct mutex lock;
	struct mutex xfer_lock;

	int channel0;
	int channel1;

	int reset_gpio;

	struct i2c_client *client;

	const struct tc358765_board_data *board_data;

	struct omap_video_timings videomode;

	struct regmap *regmap;
};

#define to_panel_data(p) container_of(p, struct panel_drv_data, dssdev)

static struct omap_dss_dsi_config tc358765_dsi_config = {
	.mode = OMAP_DSS_DSI_VIDEO_MODE,
	.pixel_format = OMAP_DSS_DSI_FMT_RGB888,
	.timings = &tc358765_timings,
	.hs_clk_min = 100000000,
	.hs_clk_max = 250000000,
	.lp_clk_min = 7000000,
	.lp_clk_max = 10000000,
	.ddr_clk_always_on = true,
	.trans_mode = OMAP_DSS_DSI_BURST_MODE,
};

static int tc358765_read_register(struct omap_dss_device *dssdev,
					u16 reg, u32 *val)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	int r;

	mutex_lock(&ddata->xfer_lock);

	r = regmap_read(ddata->regmap, reg, val);

	mutex_unlock(&ddata->xfer_lock);

	return 0;
}

static int tc358765_write_register(struct omap_dss_device *dssdev, u16 reg,
		u32 val)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	int r;

	mutex_lock(&ddata->xfer_lock);

	r = regmap_write(ddata->regmap, reg, val);

	mutex_unlock(&ddata->xfer_lock);

	return 0;
}

static int tc358765_connect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	struct device *dev = &ddata->client->dev;
	int r;

	if (omapdss_device_is_connected(dssdev))
		return 0;

	r = in->ops.dsi->connect(in, dssdev);
	if (r) {
		dev_err(dev, "Failed to connect to video source\n");
		return r;
	}

	/* channel0 used for video packets */
	r = in->ops.dsi->request_vc(ddata->in, &ddata->channel0);
	if (r) {
		dev_err(dev, "failed to get virtual channel\n");
		goto err_req_vc0;
	}

	r = in->ops.dsi->set_vc_id(ddata->in, ddata->channel0, 0);
	if (r) {
		dev_err(dev, "failed to set VC_ID\n");
		goto err_vc_id0;
	}

	/* channel1 used for registers access in LP mode */
	r = in->ops.dsi->request_vc(ddata->in, &ddata->channel1);
	if (r) {
		dev_err(dev, "failed to get virtual channel\n");
		goto err_req_vc1;
	}

	r = in->ops.dsi->set_vc_id(ddata->in, ddata->channel1, 0);
	if (r) {
		dev_err(dev, "failed to set VC_ID\n");
		goto err_vc_id1;
	}

	return 0;

err_vc_id1:
	in->ops.dsi->release_vc(ddata->in, ddata->channel1);
err_req_vc1:
err_vc_id0:
	in->ops.dsi->release_vc(ddata->in, ddata->channel0);
err_req_vc0:
	in->ops.dsi->disconnect(in, dssdev);
	return r;
}

static void tc358765_disconnect(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	if (!omapdss_device_is_connected(dssdev))
		return;

	in->ops.dsi->release_vc(in, ddata->channel0);
	in->ops.dsi->release_vc(in, ddata->channel1);
	in->ops.dsi->disconnect(in, dssdev);
}




static void tc358765_hw_reset(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	if (!gpio_is_valid(ddata->reset_gpio))
		return;

	gpio_set_value(ddata->reset_gpio, 1);
	usleep_range(200, 1000);
	/* reset the panel */
	gpio_set_value(ddata->reset_gpio, 0);
	/* assert reset */
	usleep_range(200, 1000);
	gpio_set_value(ddata->reset_gpio, 1);
	/* wait after releasing reset */
	msleep(200);

	return;
}

static int tc358765_init_ppi(struct omap_dss_device *dssdev)
{
	u32 go_cnt, sure_cnt, val = 0;
	u8 lanes = 0;
	int ret = 0;
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	const struct tc358765_board_data *board_data = ddata->board_data;
	int num_lanes = board_data->num_lanes;

	/*
	 * This register setting is required only if host wishes to
	 * perform DSI read transactions
	 */
	go_cnt = (board_data->lp_time * 5 - 3) / 4;
	sure_cnt = DIV_ROUND_UP(board_data->lp_time * 3, 2);
	val = FLD_MOD(val, go_cnt, 26, 16);
	val = FLD_MOD(val, sure_cnt, 10, 0);
	ret |= tc358765_write_register(dssdev, PPI_TX_RX_TA, val);

	/* SYSLPTX Timing Generation Counter */
	ret |= tc358765_write_register(dssdev, PPI_LPTXTIMECNT,
					board_data->lp_time);

	/* D*S_CLRSIPOCOUNT = [(THS-SETTLE + THS-ZERO) /
					HS_byte_clock_period ] */

	if (num_lanes >= 1)
		lanes |= (1 << 0);

	if (num_lanes >= 2) {
		lanes |= (1 << 1);
		ret |= tc358765_write_register(dssdev, PPI_D0S_CLRSIPOCOUNT,
							board_data->clrsipo);
	}
	if (num_lanes >= 3) {
		lanes |= (1 << 2);
		ret |= tc358765_write_register(dssdev, PPI_D1S_CLRSIPOCOUNT,
							board_data->clrsipo);
	}
	if (num_lanes >= 4) {
		lanes |= (1 << 3);
		ret |= tc358765_write_register(dssdev, PPI_D2S_CLRSIPOCOUNT,
							board_data->clrsipo);
	}
	if (num_lanes >= 5) {
		lanes |= (1 << 4);
		ret |= tc358765_write_register(dssdev, PPI_D3S_CLRSIPOCOUNT,
							board_data->clrsipo);
	}

	ret |= tc358765_write_register(dssdev, PPI_LANEENABLE, lanes);
	ret |= tc358765_write_register(dssdev, DSI_LANEENABLE, lanes);

	return ret;
}

static int tc358765_init_video_timings(struct omap_dss_device *dssdev)
{
	u32 val;
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	const struct tc358765_board_data *board_data = ddata->board_data;
	int ret;
	bool evtmode;
	struct device *dev = &ddata->client->dev;

	ret = tc358765_read_register(dssdev, VPCTRL, &val);
	if (ret < 0) {
		dev_warn(dev,
			"couldn't access VPCTRL, going on with reset value\n");
		val = 0;
	}

	if (dssdev->ctrl.pixel_size == 18) {
		/* Magic Square FRC available for RGB666 only */
		val = FLD_MOD(val, board_data->msf, 0, 0);
		val = FLD_MOD(val, 0, 8, 8);
	} else {
		val = FLD_MOD(val, 1, 8, 8);
	}

	evtmode = tc358765_dsi_config.trans_mode != OMAP_DSS_DSI_PULSE_MODE;

        /* VTGEN */
        val = FLD_MOD(val, 0, 4, 4);
        /* EVTMODE */
        val = FLD_MOD(val, evtmode, 5, 5);
        /* VSDELAY */
        val = FLD_MOD(val, 5, 31, 20);

	ret = tc358765_write_register(dssdev, VPCTRL, val);
	return ret;
}

static int tc358765_write_init_config(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	const struct tc358765_board_data *board_data = ddata->board_data;
	u32 val;
	int r;
	struct device *dev = &ddata->client->dev;

	r = tc358765_read_register(dssdev, IDREG, &val);
	printk("tc358765 ID: %x\n", val);

	/* HACK: dummy read: if we read via DSI, first reads always fail */
	tc358765_read_register(dssdev, DSI_INTSTATUS, &val);
	/* clear the intial DSI error */
	tc358765_write_register(dssdev, DSI_INTCLR, val);

	r = tc358765_init_ppi(dssdev);
	if (r) {
		dev_err(dev, "failed to initialize PPI layer\n");
		return r;
	}

	r = tc358765_write_register(dssdev, PPI_STARTPPI, 0x1);
	if (r) {
		dev_err(dev, "failed to start PPI-TX\n");
		return r;
	}

	r = tc358765_write_register(dssdev, DSI_STARTDSI, 0x1);
	if (r) {
		dev_err(dev, "failed to start DSI-RX\n");
		return r;
	}

	/* reset LVDS-PHY */
	tc358765_write_register(dssdev, LVPHY0, (1 << 22));
	usleep_range(2000, 3000);

	r = tc358765_read_register(dssdev, LVPHY0, &val);
	if (r < 0) {
		dev_warn(dev, "couldn't access LVPHY0, going on with reset value\n");
		val = 0;
	}
	val = FLD_MOD(val, 0, LV_RST_E, LV_RST_B);
	val = FLD_MOD(val, board_data->lv_is, LV_IS_E, LV_IS_B);
	val = FLD_MOD(val, board_data->lv_nd, LV_ND_E, LV_ND_B);
	r = tc358765_write_register(dssdev, LVPHY0, val);

	if (r) {
		dev_err(dev, "failed to initialize LVDS-PHY\n");
		return r;
	}

	r = tc358765_init_video_timings(dssdev);

	if (r) {
		dev_err(dev, "failed to initialize video path layer\n");
		return r;
	}

	r = tc358765_read_register(dssdev, LVCFG, &val);
	if (r < 0) {
		dev_warn(dev,
			"couldn't access LVCFG, going on with reset value\n");
		val = 0;
	}

	val = FLD_MOD(val, board_data->pclkdiv, 9, 8);
	val = FLD_MOD(val, board_data->pclksel, 11, 10);
	val = FLD_MOD(val, board_data->lvdlink, 1, 1);
	/* enable LVDS transmitter */
	val = FLD_MOD(val, 1, 0, 0);
	r = tc358765_write_register(dssdev, LVCFG, val);
	if (r) {
		dev_err(dev, "failed to start LVDS transmitter\n");
		return r;
	}

	/* Issue a soft reset to LCD Controller for a clean start */
	r = tc358765_write_register(dssdev, SYSRST, (1 << 2));
	/* commit video configuration */
	r |= tc358765_write_register(dssdev, VFUEN, 0x1);
	if (r)
		dev_err(dev, "failed to latch video timings\n");

	r = tc358765_read_register(dssdev, DSI_INTSTATUS, &val);
	if (r)
		dev_err(dev, "failed to read error status\n");
	if (val)
		printk("tc358765_write_init_config: DSI_INTSTATUS %x\n", val);

	return r;
}

static int tc358765_power_on(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct device *dev = &ddata->client->dev;
	struct omap_dss_device *in = ddata->in;
	int r;

	/* At power on the first vsync has not been received yet */

	dev_dbg(dev, "power_on\n");

	dev_dbg(dev, "dsi conf\n");
	r = in->ops.dsi->set_config(in, &tc358765_dsi_config);
	if (r) {
		dev_err(dev, "failed to configure DSI\n");
		goto err_disp_enable;
	}

	dev_dbg(dev, "dsi enable\n");
	r = in->ops.dsi->enable(in);
	if (r) {
		dev_err(dev, "failed to enable DSI\n");
		goto err_disp_enable;
	}

	/* reset tc358765 bridge */
	tc358765_hw_reset(dssdev);

	/*turn on HS clock to bring up bridge i2c slave */
	in->ops.dsi->enable_hs(in, ddata->channel0, true);

	/* configure D2L chip DSI-RX configuration registers */

	dev_dbg(dev, "write init\n");
	r = tc358765_write_init_config(dssdev);
	if (r)
		goto err_write_init;

	r = in->ops.dsi->enable_video_output(in, ddata->channel0);
	if (r)
		goto err_enable_vid;

	dev_dbg(dev, "power_on done\n");

	return r;

err_enable_vid:
err_write_init:
	in->ops.dsi->disable(in, false, false);
err_disp_enable:

	return r;
}

static void tc358765_power_off(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;

	in->ops.dsi->disable_video_output(in, ddata->channel0);

	in->ops.dsi->disable(in, false, false);
}

static int tc358765_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	int r;
	struct device *dev = &ddata->client->dev;

	dev_dbg(dev, "enable\n");

	if (dssdev->state != OMAP_DSS_DISPLAY_DISABLED)
		return -EINVAL;

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	mutex_lock(&ddata->lock);

	in->ops.dsi->bus_lock(in);

	r = tc358765_power_on(dssdev);

	in->ops.dsi->bus_unlock(in);

	if (r) {
		dev_dbg(dev, "enable failed\n");
		dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
	} else {
		dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
	}

	mutex_unlock(&ddata->lock);

	return r;
}

static void tc358765_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->in;
	struct device *dev = &ddata->client->dev;

	dev_dbg(dev, "disable\n");

	if (!omapdss_device_is_enabled(dssdev))
		return;

	mutex_lock(&ddata->lock);

	in->ops.dsi->bus_lock(in);

	tc358765_power_off(dssdev);

	in->ops.dsi->bus_unlock(in);
	mutex_unlock(&ddata->lock);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
}

static void tc358765_get_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*timings = ddata->videomode;
}

static void tc358765_set_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	//struct panel_drv_data *ddata = to_panel_data(dssdev);
	//struct omap_dss_device *in = ddata->in;

	//ddata->videomode = *timings;
	//dssdev->panel.timings = *timings;

	//in->ops.dsi->set_timings(in, timings);

}

static int tc358765_check_timings(struct omap_dss_device *dssdev,
		struct omap_video_timings *timings)
{
	if (unlikely(!timings)) {
		WARN(true, "%s: timings NULL pointer was passed\n", __func__);
		return -EINVAL;
	}

	if (tc358765_timings.x_res != timings->x_res ||
			tc358765_timings.y_res != timings->y_res ||
			tc358765_timings.pixel_clock != timings->pixel_clock ||
			tc358765_timings.hsw != timings->hsw ||
			tc358765_timings.hfp != timings->hfp ||
			tc358765_timings.hbp != timings->hbp ||
			tc358765_timings.vsw != timings->vsw ||
			tc358765_timings.vfp != timings->vfp ||
			tc358765_timings.vbp != timings->vbp)
		return -EINVAL;

	return 0;
}

static void tc358765_get_resolution(struct omap_dss_device *dssdev,
		u16 *xres, u16 *yres)
{
	*xres = tc358765_timings.x_res;
	*yres = tc358765_timings.y_res;
}


static struct omap_dss_driver tc358765_ops = {
	.connect	= tc358765_connect,
	.disconnect	= tc358765_disconnect,

	.enable		= tc358765_enable,
	.disable	= tc358765_disable,

	.get_resolution	= tc358765_get_resolution,
	.get_recommended_bpp = omapdss_default_get_recommended_bpp,

	.get_timings	= tc358765_get_timings,
	.set_timings	= tc358765_set_timings,
	.check_timings	= tc358765_check_timings,
};

static int tc358765_probe_of(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct panel_drv_data *ddata = i2c_get_clientdata(client);
	struct omap_dss_device *in;
	int gpio;

	gpio = of_get_gpio(node, 0);
	if (!gpio_is_valid(gpio)) {
		dev_err(dev, "failed to parse reset gpio\n");
		return gpio;
	}
	ddata->reset_gpio = gpio;

	in = omapdss_of_find_source_for_first_ep(node);
	if (IS_ERR(in)) {
		dev_err(dev, "failed to find video source\n");
		return PTR_ERR(in);
	}

	ddata->in = in;

	return 0;
}

static struct regmap_config tc358765_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
};

static int tc358765_i2c_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;
	int r;
	struct device *dev = &client->dev;

	dev_dbg(dev, "probe\n");

	ddata = devm_kzalloc(&client->dev, sizeof(*ddata), GFP_KERNEL);
	if (ddata == NULL)
		return -ENOMEM;

	ddata->client = client;

	i2c_set_clientdata(client, ddata);

	ddata->regmap = devm_regmap_init_i2c(client, &tc358765_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		r = PTR_ERR(ddata->regmap);
		dev_err(dev, "Failed to init regmap: %d\n", r);
		return r;
	}

	if (dev->of_node) {
		r = tc358765_probe_of(client);
		if (r)
			return r;
	} else {
		return -ENODEV;
	}

	if (gpio_is_valid(ddata->reset_gpio)) {
		r = devm_gpio_request_one(dev, ddata->reset_gpio,
				GPIOF_OUT_INIT_LOW, "tc385765 rst");
		if (r) {
			dev_err(dev, "failed to request reset gpio\n");
			return r;
		}
	}

	mutex_init(&ddata->xfer_lock);
	mutex_init(&ddata->lock);

	ddata->videomode = tc358765_timings;

	ddata->board_data = &test_board_data;

	dssdev = &ddata->dssdev;
	dssdev->dev = dev;
	dssdev->driver = &tc358765_ops;
	dssdev->panel.timings = ddata->videomode;
	dssdev->type = OMAP_DISPLAY_TYPE_DSI;
	dssdev->owner = THIS_MODULE;

	dssdev->panel.dsi_pix_fmt = OMAP_DSS_DSI_FMT_RGB888;

	r = omapdss_register_display(dssdev);
	if (r) {
		dev_err(dev, "Failed to register panel\n");
		return r;
	}

	dev_dbg(&client->dev, "probe done\n");

	return 0;
}

/* driver remove function */
static int __exit tc358765_i2c_remove(struct i2c_client *client)
{
	struct panel_drv_data *ddata = i2c_get_clientdata(client);
	struct omap_dss_device *dssdev = &ddata->dssdev;
	struct device *dev = &client->dev;

	dev_dbg(dev, "remove\n");

	omapdss_unregister_display(dssdev);

	tc358765_disable(dssdev);
	tc358765_disconnect(dssdev);

	omap_dss_put_device(ddata->in);

	mutex_destroy(&ddata->lock);
	mutex_destroy(&ddata->xfer_lock);

	return 0;
}

static const struct i2c_device_id tc358765_i2c_idtable[] = {
	{"tc358765", 0},
	{},
};

static const struct of_device_id tc358765_of_match[] = {
	{ .compatible = "toshiba,tc358765", },
	{},
};

static struct i2c_driver tc358765_driver = {
	.probe = tc358765_i2c_probe,
	.remove = __exit_p(tc358765_i2c_remove),
	.id_table = tc358765_i2c_idtable,
	.driver = {
		   .name  = "tc358765",
		   .owner = THIS_MODULE,
		   .of_match_table = tc358765_of_match,
	},
};

module_i2c_driver(tc358765_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("TC358765 DSI-2-LVDS Driver");
MODULE_LICENSE("GPL");
