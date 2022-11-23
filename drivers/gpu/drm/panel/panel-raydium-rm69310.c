// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct rm69310 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	bool prepared;
	bool enabled;
	struct dentry *debugfs;
};

static const struct drm_display_mode default_mode = {
	.clock = 3060,

	.hdisplay = 120,
	.hsync_start = 120 + 80,
	.hsync_end = 120 + 80 + 32,
	.htotal = 120 + 80 + 32 + 40,

	.vdisplay = 240,
	.vsync_start = 240 + 1,
	.vsync_end = 240 + 1 + 8,
	.vtotal = 240 + 1 + 8 + 26,

	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	//.flags = 0,
	.width_mm = 68,
	.height_mm = 122,
};

static inline struct rm69310 *panel_to_rm69310(struct drm_panel *panel)
{
	return container_of(panel, struct rm69310, panel);
}

static int rm69310_unprepare(struct drm_panel *panel)
{
	struct rm69310 *ctx = panel_to_rm69310(panel);
	//(struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	//int ret;

	if (!ctx->prepared)
		return 0;
/*
	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret)
		dev_warn(panel->dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret)
		dev_warn(panel->dev, "failed to enter sleep mode: %d\n", ret);

	msleep(120);
*/
	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	ctx->prepared = false;

	return 0;
}

static int rm69310_disable(struct drm_panel *panel)
{
	struct rm69310 *ctx = panel_to_rm69310(panel);

	if (!ctx->enabled)
		return 0;

	ctx->enabled = false;

	rm69310_unprepare(panel);

	return 0;
}

static void early_prepare(struct drm_panel *panel)
{
	struct rm69310 *ctx = panel_to_rm69310(panel);

	if (ctx->reset_gpio) {
		printk("PANEL RESET\n");
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(50);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(50);
	}
}

static int setup(struct drm_panel *panel)
{
	struct rm69310 *ctx = panel_to_rm69310(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;
	u8 buf[10];
	int retries;
	const int num_retries = 10;

	printk("SENDING SETUP SEQ\n");

	for (retries = num_retries; retries > 0; retries--) {
		buf[0] = 0x0;
		ret = mipi_dsi_dcs_write(dsi, 0xfe, buf, 1);
		if (!ret)
			break;
	}

	if (retries == 0) {
		dev_err(&dsi->dev, "All init retries failed\n");
		return -EIO;
	}

	for (retries = num_retries; retries > 0; retries--) {
		ret = mipi_dsi_dcs_set_column_address(dsi, 4, 124-1);
		if (!ret)
			break;
	}

	if (retries == 0) {
		dev_err(&dsi->dev, "All init retries failed\n");
		return -EIO;
	}

	for (retries = num_retries; retries > 0; retries--) {
		ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
		if (!ret)
			break;
	}

	if (retries == 0) {
		dev_err(&dsi->dev, "All init retries failed\n");
		return -EIO;
	}

	for (retries = num_retries; retries > 0; retries--) {
		buf[0] = 0xc4;
		buf[1] = 0x80;
		ret = mipi_dsi_generic_write(dsi, buf, 2);
		if (!ret)
			break;
	}

	if (retries == 0) {
		dev_err(&dsi->dev, "All init retries failed\n");
		return -EIO;
	}

	for (retries = num_retries; retries > 0; retries--) {
		ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
		if (!ret)
			break;
	}

	if (retries == 0) {
		dev_err(&dsi->dev, "All init retries failed\n");
		return -EIO;
	}

	msleep(120);

	for (retries = num_retries; retries > 0; retries--) {
		ret = mipi_dsi_dcs_set_display_on(dsi);
		if (!ret)
			break;
	}

	if (retries == 0) {
		dev_err(&dsi->dev, "All init retries failed\n");
		return -EIO;
	}

	printk("SENDING SETUP SEQ DONE\n");

	return 0;
}

static int rm69310_prepare(struct drm_panel *panel)
{
	struct rm69310 *ctx = panel_to_rm69310(panel);

	if (ctx->prepared)
		return 0;

	early_prepare(panel);

	ctx->prepared = true;

	return 0;
}

static int rm69310_enable(struct drm_panel *panel)
{
	struct rm69310 *ctx = panel_to_rm69310(panel);

	if (ctx->enabled)
		return 0;

	setup(panel);

	ctx->enabled = true;

	return 0;
}

static int rm69310_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs rm69310_drm_funcs = {
	.disable = rm69310_disable,
	.unprepare = rm69310_unprepare,
	.prepare = rm69310_prepare,
	.enable = rm69310_enable,
	.get_modes = rm69310_get_modes,
};

static u16 s_debugfs_addr;
static u16 s_debugfs_data;

static ssize_t rm69310_debugfs_write_reg_access(struct file *file, const char __user *udata,
		size_t count, loff_t *ppos)
{
	struct mipi_dsi_device *dsi = file->private_data;
	u8 buf[1] = { s_debugfs_data };
	int ret;
	char data[1];
	unsigned long err;

	printk("rm69310_debugfs_write_reg_access %u, %lld\n", count, *ppos);

	if (count == 0)
		return 0;

	err = copy_from_user(data, udata, 1);
	if (err)
		return -EFAULT;

	if (data[0] == 'W') {
		printk("debugwrite %#x = %#x\n", s_debugfs_addr, s_debugfs_data);

		ret = mipi_dsi_dcs_write(dsi, s_debugfs_addr, buf, 1);

		printk("debugwrite %d\n", ret);
		if (ret)
			return -EIO;

	} else if (data[0] == 'R') {
		printk("debugread %#x\n", s_debugfs_addr);

		buf[0] = 0;

		ret = mipi_dsi_dcs_read(dsi, s_debugfs_addr, buf, 1);

		printk("debugread %d: %#x\n", ret, buf[0]);

		s_debugfs_data = buf[0];
		if (ret)
			return -EIO;

	} else {
		return -EINVAL;
	}

	printk("OK\n");

	return count;
}

static const struct file_operations rm69310_debugfs_write_fops = {
	.open = simple_open,
	.write = rm69310_debugfs_write_reg_access,
	.llseek = generic_file_llseek,
};

static int rm69310_debugfs_init(struct mipi_dsi_device *dsi)
{
	struct rm69310 *ctx = mipi_dsi_get_drvdata(dsi);
	struct dentry *dir;

	dir = debugfs_create_dir("rm69310", NULL);

	ctx->debugfs = dir;

	debugfs_create_x16("addr", S_IRUSR | S_IWUSR, dir, &s_debugfs_addr);
	debugfs_create_x16("data", S_IRUSR | S_IWUSR, dir, &s_debugfs_data);

	debugfs_create_file("cmd", S_IWUSR, dir,
				dsi, &rm69310_debugfs_write_fops);

	return 0;
}

static void rm69310_debugfs_uninit(struct mipi_dsi_device *dsi)
{
	struct rm69310 *ctx = mipi_dsi_get_drvdata(dsi);

	debugfs_remove_recursive(ctx->debugfs);
}

static int rm69310_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rm69310 *ctx;
	int ret;

	printk("rm69310_probe\n");

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "cannot get reset GPIO: %d\n", ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 1;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE/* |
			  MIPI_DSI_MODE_LPM*/;

	drm_panel_init(&ctx->panel, dev, &rm69310_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->panel.prepare_upstream_first = true;

#if 0
	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;
#endif

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	//early_prepare(&ctx->panel);

	rm69310_debugfs_init(dsi);

	printk("rm69310_probe done\n");

	return 0;
}

static int rm69310_remove(struct mipi_dsi_device *dsi)
{
	struct rm69310 *ctx = mipi_dsi_get_drvdata(dsi);

	printk("rm69310_remove\n");

	rm69310_debugfs_uninit(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	printk("rm69310_remove done\n");

	return 0;
}

static const struct of_device_id raydium_rm69310_of_match[] = {
	{ .compatible = "raydium,rm69310" },
	{}
};
MODULE_DEVICE_TABLE(of, raydium_rm69310_of_match);

static struct mipi_dsi_driver raydium_rm69310_driver = {
	.probe = rm69310_probe,
	.remove = rm69310_remove,
	.driver = {
		.name = "panel-raydium-rm69310",
		.of_match_table = raydium_rm69310_of_match,
	},
};
module_mipi_dsi_driver(raydium_rm69310_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>");
MODULE_DESCRIPTION("DRM Driver for Raydium RM69310 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
