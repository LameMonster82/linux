// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_dsc.h>

#include <video/mipi_display.h>

struct panel_cmd {
	size_t len;
	const char *data;
};

#define _INIT_CMD(...) { \
	.len = sizeof((char[]){__VA_ARGS__}), \
	.data = (char[]){__VA_ARGS__} }

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static const char * const regulator_names[] = {
	"vddio",
};

static unsigned long const regulator_enable_loads[] = {
	1700000,
};

static unsigned long const regulator_disable_loads[] = {
	100,
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	const char *panel_name;

	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	const struct panel_cmd *on_cmds_1;
	const struct panel_cmd *on_cmds_2;
};

struct panel_info {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct panel_desc *desc;

	struct backlight_device *backlight;
	u32 brightness;
	u32 max_brightness;

	u32 init_delay_us;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *suspend;

	bool prepared;
	bool enabled;
	bool first_enable;
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	pr_err("sw49410: to_panel_info called");
	return container_of(panel, struct panel_info, base);
}


/*
 * Need to reset gpios and regulators once in the beginning,
 */
static int panel_reset_at_beginning(struct panel_info * pinfo)
{
	int ret = 0, i;

	pr_err("sw49410: panel_reset_at_beginning");

	DRM_DEV_ERROR(pinfo->base.dev,
					"panel_reset_at_beginning\n");
	/* enable supplies */
	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	pr_err("sw49410: enable regulators");

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	/* Disable supplies */
	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(pinfo->base.dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	pr_err("sw49410: regulator bulk disable");

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	/*
	 * Reset sequence of LG sw49410_rev1 panel requires the panel to be
	 * out of reset for 6ms, followed by being held in reset
	 * for 1ms and then out again
	 */
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(30000, 10000);
	gpiod_set_value(pinfo->reset_gpio, 0);
	usleep_range(30000, 2000);
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(30000, 10000);

	return 0;
}

static int send_mipi_cmds(struct drm_panel *panel, const struct panel_cmd *cmds)
{
	struct panel_info *pinfo = to_panel_info(panel);
	unsigned int i = 0;
	int err;

	pr_err("sw49410: sending a mipi command");

	if (!cmds)
		return -EFAULT;

	for (i = 0; cmds[i].len != 0; i++) {
		const struct panel_cmd *cmd = &cmds[i];

		if (cmd->len == 2)
			err = mipi_dsi_dcs_write(pinfo->link,
						    cmd->data[1], NULL, 0);
		else
			err = mipi_dsi_dcs_write(pinfo->link,
						    cmd->data[1], cmd->data + 2,
						    cmd->len - 2);

		if (err < 0)
			return err;

		usleep_range((cmd->data[0]) * 1000,
			    (1 + cmd->data[0]) * 1000);
	}

	return 0;
}

static int panel_set_pinctrl_state(struct panel_info *panel, bool enable)
{
	int rc = 0;
	struct pinctrl_state *state;

	pr_err("sw49410: setting a pinctrl state");

	if (enable)
		state = panel->active;
	else
		state = panel->suspend;

	rc = pinctrl_select_state(panel->pinctrl, state);
	if (rc)
		pr_err("[%s] failed to set pin state, rc=%d\n", panel->desc->panel_name,
			rc);
	return rc;
}

static int lg_panel_disable(struct drm_panel *panel)
{
/* HACK: return 0 for now */

#if 0
	struct panel_info *pinfo = to_panel_info(panel);

	backlight_disable(pinfo->backlight);

	pinfo->enabled = false;
#endif
	return 0;
}

static int lg_panel_power_off(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i, ret = 0;
	gpiod_set_value(pinfo->reset_gpio, 0);

	pr_err("sw49410: panel power off");

        ret = panel_set_pinctrl_state(pinfo, false);
        if (ret) {
                pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
        }

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(panel->dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret) {
		DRM_DEV_ERROR(panel->dev,
			"regulator_bulk_disable failed %d\n", ret);
	}
	return ret;
}

static int lg_panel_unprepare(struct drm_panel *panel)
{
/* HACK : Currently, after a suspend, the resume doesn't enable screen, so
 *        don't disable the panel until we figure out why that is.
 */
return 0;

#if 0
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	if (!pinfo->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"set_display_off cmd failed ret = %d\n",
			ret);
	}

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = mipi_dsi_dcs_enter_sleep_mode(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"enter_sleep cmd failed ret = %d\n", ret);
	}
	/* 0x64 = 100ms delay */
	msleep(100);

	ret = lg_panel_power_off(panel);
	if (ret < 0)
		DRM_DEV_ERROR(panel->dev, "power_off failed ret = %d\n", ret);

	pinfo->prepared = false;

	return ret;
#endif
}

static int lg_panel_power_on(struct panel_info *pinfo)
{
	int ret, i;
	pr_err("sw49410: panelpower on");

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	pr_err("sw49410: power on bulk enable");

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	pr_err("sw49410: called a pinctrl state");

	ret = panel_set_pinctrl_state(pinfo, true);
	if (ret) {
		pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
	}

	/*
	 * Reset sequence of LG sw49410_rev1 panel requires the panel to be
	 * out of reset for 9ms, followed by being held in reset
	 * for 1ms and then out again
	 * For now dont set this sequence as it causes panel to not come
	 * back
	 */
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(30000, 10000);
	gpiod_set_value(pinfo->reset_gpio, 0);
	usleep_range(30000, 2000);
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(30000, 10000);

	return 0;
}

static int lg_panel_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct mipi_dsi_device *dsi = pinfo->link;
	struct device *dev = &dsi->dev;
	int err, ret;

	pr_err("sw49410: preparing the panel");

	if (unlikely(pinfo->first_enable)) {
		pinfo->first_enable = false;
		err = panel_reset_at_beginning(pinfo);
		if (err < 0) {
		pr_err("sw49410_rev1 panel_reset_at_beginning failed: %d\n", err);
			return err;
		}
	}

	if (pinfo->prepared)
		return 0;

	usleep_range(pinfo->init_delay_us, pinfo->init_delay_us);

	pr_err("sw49410: powering on the panel");

	err = lg_panel_power_on(pinfo);
	if (err < 0)
		goto poweroff;

	pr_err("sw49410: sending first batch of commands");

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK); /* TE On */
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x0c2f); /* Page Address Set */
	if (ret < 0) {
		dev_err(dev, "Failed to set page address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_display_brightness(dsi, 0x00ff); /* BLU Control 1 */
	if (ret < 0) {
		dev_err(dev, "Failed to set display brightness: %d\n", ret);
		return ret;
	}

	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c); /* BLU Control 2 */
	dsi_dcs_write_seq(dsi, MIPI_DCS_SET_CABC_MIN_BRIGHTNESS, 0x00); /* BLU Control 3 */
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_POWER_SAVE, 0x81); /* BLU Control 4 */
	dsi_dcs_write_seq(dsi, 0xb0, 0xac); /*Manufacturer protection*/
	dsi_dcs_write_seq(dsi, 0xb3,
			  0x04, 0x04, 0x28, 0x08, 0x5a, 0x12, 0x23, 0x02); /*Source Control*/
	dsi_dcs_write_seq(dsi, 0xb4,
			  0x11, 0x04, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
			  0x01, 0x01, 0x01, 0x01, 0xd0, 0xe4, 0xe4, 0xe4, 0x93,
			  0x4e, 0x39, 0x0a, 0x10, 0x18, 0x25, 0x24, 0x00, 0x00,
			  0x00, 0x00, 0x00, 0x00); /*Gate & Mux Control*/
	dsi_dcs_write_seq(dsi, 0xb5,
			  0x2e, 0x0f, 0x10, 0xc0, 0x00, 0x10, 0xc0, 0x00); /*Sync Setup*/
	dsi_dcs_write_seq(dsi, 0xb6, 0x03, 0x05, 0x0b, 0xb3, 0x30); /*Panel Setting*/
	dsi_dcs_write_seq(dsi, 0xb8,
			  0x57, 0x02, 0x90, 0x40, 0x5d, 0xd0, 0x05, 0x00, 0x00,
			  0x18, 0x22, 0x04, 0x01, 0x02, 0x90, 0x40, 0x4c, 0xc0,
			  0x04, 0x00, 0x00, 0x18, 0x22, 0x04, 0x01, 0x08, 0x00,
			  0x3a, 0x86, 0x83, 0x00); /*Touch Timing Control*/
	dsi_dcs_write_seq(dsi, 0xb9, 0x64, 0x64, 0x2a, 0x3f, 0xee); /*Touch Source Setting*/
	dsi_dcs_write_seq(dsi, 0xba,
			  0x3d, 0x1f, 0x01, 0xff, 0x01, 0x3c, 0x1f, 0x01, 0xff,
			  0x01, 0x00); /*DSC Configuration*/
	dsi_dcs_write_seq(dsi, 0xbc, 0x00, 0x00, 0x00, 0x90); /*Low Rate Refresh Setting*/
	dsi_dcs_write_seq(dsi, 0xbd, 0x00, 0x00); /*Black Frame Setting*/
	dsi_dcs_write_seq(dsi, 0xbf, 0x4f, 0x02); /* U2 Corner Down */
	dsi_dcs_write_seq(dsi, 0xc0,
			  0x00, 0x04, 0x18, 0x07, 0x11, 0x11, 0x3c, 0x00, 0x0a,
			  0x0a); /*Internal Oscillator Setting*/
	dsi_dcs_write_seq(dsi, 0xc1, 0x01, 0x00, 0xf0, 0xc2, 0xcf, 0x0c); /*Power Control1*/
	dsi_dcs_write_seq(dsi, 0xc2,
			  0xcc, 0x44, 0x44, 0x20, 0x22, 0x26, 0x21, 0x00); /*Power Control2*/
	dsi_dcs_write_seq(dsi, 0xc3,
			  0x92, 0x11, 0x09, 0x09, 0x11, 0xcc, 0x02, 0x02, 0xa4,
			  0xa4, 0x02, 0xa2, 0x38, 0x28, 0x14, 0x40, 0x38, 0xc0); /*Power Control3*/
	dsi_dcs_write_seq(dsi, 0xc4, 0x26, 0x00); /*Vcom Setting*/
	dsi_dcs_write_seq(dsi, 0xc9, 0x05, 0x5d, 0x03, 0x04, 0x00); /*Power Sequence Option Configuration*/
	dsi_dcs_write_seq(dsi, 0xca, 0x9b, 0x10); /*Abrupt Power Off Control*/
	dsi_dcs_write_seq(dsi, 0xcb, 0xf3, 0x90, 0x3d, 0x30, 0xcc); /*LFD Control*/
	dsi_dcs_write_seq(dsi, 0xcc, 0x00, 0x40, 0x50, 0x90, 0x41); /*Tail TFT Setting*/
	dsi_dcs_write_seq(dsi, 0xce, 0x00, 0x00); /*U2 Option*/
	dsi_dcs_write_seq(dsi, 0xd0,
			  0x12, 0x05, 0x20, 0x1b, 0x2c, 0x28, 0x3f, 0x3d, 0x4f,
			  0x4f, 0x66, 0x66, 0x6e, 0x6e, 0x76, 0x76, 0x80, 0x80,
			  0x88, 0x88, 0x95, 0x95, 0x3f, 0x3f, 0xa2, 0xa2, 0x94,
			  0x94, 0x8b, 0x8b, 0x81, 0x81, 0x75, 0x75, 0x66, 0x66,
			  0x47, 0x47, 0x2d, 0x2d, 0x00, 0x01, 0x12, 0x05, 0x20,
			  0x1b, 0x2c, 0x28, 0x3f, 0x3d, 0x4f, 0x4f, 0x66, 0x66,
			  0x6e, 0x6e, 0x76, 0x76, 0x80, 0x80, 0x88, 0x88, 0x95,
			  0x95, 0x3f, 0x3f, 0xa2, 0xa2, 0x94, 0x94, 0x8b, 0x8b,
			  0x81, 0x81, 0x75, 0x75, 0x66, 0x66, 0x47, 0x47, 0x2d,
			  0x2d, 0x00, 0x01, 0x12, 0x05, 0x20, 0x1b, 0x2c, 0x28,
			  0x3f, 0x3d, 0x4f, 0x4f, 0x66, 0x66, 0x6e, 0x6e, 0x76,
			  0x76, 0x80, 0x80, 0x88, 0x88, 0x95, 0x95, 0x3f, 0x3f,
			  0xa2, 0xa2, 0x94, 0x94, 0x8b, 0x8b, 0x81, 0x81, 0x75,
			  0x75, 0x66, 0x66, 0x47, 0x47, 0x2d, 0x2d, 0x00, 0x01,
			  0x12, 0x05, 0x20, 0x1b, 0x2c, 0x28, 0x3f, 0x3d, 0x4f,
			  0x4f, 0x66, 0x66, 0x6e, 0x6e, 0x76, 0x76, 0x80, 0x80,
			  0x88, 0x88, 0x94, 0x94, 0x3f, 0x3f, 0xa4, 0xa4, 0x95,
			  0x95, 0x8b, 0x8b, 0x81, 0x81, 0x75, 0x75, 0x66, 0x66,
			  0x47, 0x47, 0x2d, 0x2d, 0x00, 0x01); /*Gamma 1*/
	dsi_dcs_write_seq(dsi, 0xd1,
			  0x12, 0x05, 0x20, 0x1b, 0x2e, 0x29, 0x41, 0x3f, 0x52,
			  0x52, 0x6a, 0x6a, 0x72, 0x72, 0x7a, 0x7a, 0x84, 0x84,
			  0x8c, 0x8c, 0x9a, 0x9a, 0x3f, 0x3f, 0x9b, 0x9b, 0x8d,
			  0x8d, 0x84, 0x84, 0x7a, 0x7a, 0x6e, 0x6e, 0x5f, 0x5f,
			  0x41, 0x41, 0x2a, 0x2a, 0x00, 0x01, 0x12, 0x05, 0x20,
			  0x1b, 0x2e, 0x29, 0x41, 0x3f, 0x52, 0x52, 0x6a, 0x6a,
			  0x72, 0x72, 0x7a, 0x7a, 0x84, 0x84, 0x8c, 0x8c, 0x9a,
			  0x9a, 0x3f, 0x3f, 0x9b, 0x9b, 0x8d, 0x8d, 0x84, 0x84,
			  0x7a, 0x7a, 0x6e, 0x6e, 0x5f, 0x5f, 0x41, 0x41, 0x2a,
			  0x2a, 0x00, 0x01, 0x12, 0x05, 0x20, 0x1b, 0x2e, 0x29,
			  0x41, 0x3f, 0x52, 0x52, 0x6a, 0x6a, 0x72, 0x72, 0x7a,
			  0x7a, 0x84, 0x84, 0x8c, 0x8c, 0x9a, 0x9a, 0x3f, 0x3f,
			  0x9b, 0x9b, 0x8d, 0x8d, 0x84, 0x84, 0x7a, 0x7a, 0x6e,
			  0x6e, 0x5f, 0x5f, 0x41, 0x41, 0x2a, 0x2a, 0x00, 0x01,
			  0x12, 0x05, 0x20, 0x1b, 0x2e, 0x29, 0x41, 0x3f, 0x52,
			  0x52, 0x6a, 0x6a, 0x72, 0x72, 0x7a, 0x7a, 0x84, 0x84,
			  0x8c, 0x8c, 0x9a, 0x9a, 0x3f, 0x3f, 0x9b, 0x9b, 0x8d,
			  0x8d, 0x84, 0x84, 0x7a, 0x7a, 0x6e, 0x6e, 0x5f, 0x5f,
			  0x41, 0x41, 0x2a, 0x2a, 0x00, 0x01); /*Gamma 2*/
	dsi_dcs_write_seq(dsi, 0xd2,
			  0x12, 0x05, 0x20, 0x1b, 0x2f, 0x2a, 0x43, 0x41, 0x55,
			  0x55, 0x6e, 0x6e, 0x76, 0x76, 0x7e, 0x7e, 0x88, 0x88,
			  0x90, 0x90, 0x9f, 0x9f, 0x3f, 0x3f, 0x95, 0x95, 0x86,
			  0x86, 0x7d, 0x7d, 0x74, 0x74, 0x68, 0x68, 0x59, 0x59,
			  0x3c, 0x3c, 0x26, 0x26, 0x00, 0x01, 0x12, 0x05, 0x20,
			  0x1b, 0x2f, 0x2a, 0x43, 0x41, 0x55, 0x55, 0x6e, 0x6e,
			  0x76, 0x76, 0x7e, 0x7e, 0x88, 0x88, 0x90, 0x90, 0x9f,
			  0x9f, 0x3f, 0x3f, 0x95, 0x95, 0x86, 0x86, 0x7d, 0x7d,
			  0x74, 0x74, 0x68, 0x68, 0x59, 0x59, 0x3c, 0x3c, 0x26,
			  0x26, 0x00, 0x01, 0x12, 0x05, 0x20, 0x1b, 0x2f, 0x2a,
			  0x43, 0x41, 0x55, 0x55, 0x6e, 0x6e, 0x76, 0x76, 0x7e,
			  0x7e, 0x88, 0x88, 0x90, 0x90, 0x9f, 0x9f, 0x3f, 0x3f,
			  0x95, 0x95, 0x86, 0x86, 0x7d, 0x7d, 0x74, 0x74, 0x68,
			  0x68, 0x59, 0x59, 0x3c, 0x3c, 0x26, 0x26, 0x00, 0x01,
			  0x12, 0x05, 0x20, 0x1b, 0x2f, 0x2a, 0x43, 0x41, 0x55,
			  0x55, 0x6e, 0x6e, 0x76, 0x76, 0x7e, 0x7e, 0x88, 0x88,
			  0x90, 0x90, 0x9f, 0x9f, 0x3f, 0x3f, 0x95, 0x95, 0x86,
			  0x86, 0x7d, 0x7d, 0x74, 0x74, 0x68, 0x68, 0x59, 0x59,
			  0x3c, 0x3c, 0x26, 0x26, 0x00, 0x01); /*Gamma 3*/
	dsi_dcs_write_seq(dsi, 0xd3, 0x12, 0x01, 0x00, 0x00); /* MPLUS Control */
	dsi_dcs_write_seq(dsi, 0xd4,
			  0xdc, 0x5f, 0x9c, 0xbe, 0x39, 0x39, 0x39, 0x47, 0x48,
			  0x48, 0x48, 0x3a, 0x00, 0x03, 0x6d, 0x80, 0x00, 0x00,
			  0x8c, 0x66, 0x00, 0x00, 0x8c, 0x66, 0x00, 0x00, 0x8c,
			  0x66, 0x00, 0x0a, 0x48, 0x80, 0x00, 0x0a, 0x48, 0x80,
			  0x00, 0x0a, 0x48, 0x80, 0x00, 0x0a, 0x48, 0x80, 0x20,
			  0x0a, 0x14, 0x0a, 0x18, 0x00, 0x1c, 0xcc, 0x23, 0x9e,
			  0x23, 0x9e, 0x01, 0x01, 0x01, 0x01, 0x04, 0x04, 0x04,
			  0x04, 0x01, 0x00, 0x02, 0x80, 0x00, 0x10, 0x00, 0x10,
			  0x00, 0x10, 0x13, 0x9e, 0x13, 0x9e, 0x13, 0x9e, 0x13,
			  0x9e, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
			  0x23, 0x9e, 0xff, 0xff, 0x13, 0x33, 0x18, 0x00, 0x16,
			  0x66, 0x10, 0x00, 0xff, 0x01, 0x00, 0x02, 0x00, 0x03,
			  0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00,
			  0x08, 0x00, 0x09, 0x00, 0x0a, 0x00, 0x0b, 0x00, 0x0c,
			  0x00, 0x0d, 0x00, 0x0e, 0x00, 0x0f, 0x00, 0x1b, 0x25,
			  0xdc, 0x18, 0x00, 0x20, 0x00, 0x1c, 0xe1, 0x00, 0xff,
			  0xe0, 0xc8, 0xc8, 0x41, 0x8f); /*Mplus Setting*/
	dsi_dcs_write_seq(dsi, 0xad,
			  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x06, 0x06, 0x06,
			  0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x20, 0x40,
			  0x60, 0x90, 0xc0, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff,
			  0xff, 0xff, 0xff, 0xff, 0xff); /*Notch Up Gradation*/
	dsi_dcs_write_seq(dsi, 0xae,
			  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x06, 0x06, 0x06,
			  0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x20, 0x40,
			  0x60, 0x90, 0xc0, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff,
			  0xff, 0xff, 0xff, 0xff, 0xff); /*Notch Down Gradation*/
	dsi_dcs_write_seq(dsi, 0xe5,
			  0x0b, 0x0a, 0x0c, 0x00, 0x02, 0x04, 0x06, 0x08, 0x0f,
			  0x1b, 0x02, 0x1a, 0x1a, 0x0b, 0x0a, 0x0c, 0x01, 0x03,
			  0x05, 0x07, 0x09, 0x10, 0x1b, 0x03, 0x1a, 0x1a); /*GIP Setting*/
	dsi_dcs_write_seq(dsi, 0xe6,
			  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x11,
			  0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18); /*Mux Setting*/
	dsi_dcs_write_seq(dsi, 0xed, 0x21, 0x49, 0x00, 0x00, 0x00, 0x00); /*Test1*/
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_POWER_SAVE, 0x81); /*BLU Control*/
	dsi_dcs_write_seq(dsi, 0xf3, 0x00, 0x01, 0x00, 0x0d, 0x00); /*Sharpness 1*/
	dsi_dcs_write_seq(dsi, 0xf4,
			  0x00, 0x00, 0x40, 0x83, 0xc5, 0x00, 0x01, 0x00, 0x00,
			  0x00, 0x00, 0x00, 0x00); /*Sharpness 2*/
	dsi_dcs_write_seq(dsi, 0xfb,
			  0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0, 0x13, 0x18,
			  0x18, 0x18, 0x16, 0x0d, 0x0d, 0x00, 0xc7, 0xcf, 0xd8,
			  0xe1, 0xea, 0xf3, 0xf9, 0xff); /*Sharpness 3*/
	dsi_dcs_write_seq(dsi, 0xf5, 0x00); /*Gamma Correction 1*/
	dsi_dcs_write_seq(dsi, 0xf6,
			  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00,
			  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); /*Gamma Correction 2*/
	dsi_dcs_write_seq(dsi, 0xf7,
			  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00,
			  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); /*Gamma Correction 3*/
	dsi_dcs_write_seq(dsi, 0xf8,
			  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00,
			  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); /*Gamma Correction 4*/
	dsi_dcs_write_seq(dsi, 0xfc,
			  0x13, 0x70, 0xd0, 0x26, 0x30, 0x7c, 0x02, 0xff, 0x12,
			  0x22, 0x22, 0x10, 0x00); /*BLU PWM Control*/
	dsi_dcs_write_seq(dsi, MIPI_DCS_ENTER_NORMAL_MODE);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(256);

	/* Set DCS_COMPRESSION_MODE */
	err = mipi_dsi_dcs_write(pinfo->link, MIPI_DSI_COMPRESSION_MODE, (u8[]){ 0x11 }, 0);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to set compression mode: %d\n", err);
		goto poweroff;
	}

	dsi_dcs_write_seq(dsi, 0xbd, 0x01, 0x05); /*Black Frame Setting 1*/

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}
	usleep_range(5000, 6000);

	/* 0x32 = 50ms delay */
	msleep(120);

	pinfo->prepared = true;

	return 0;

poweroff:
	gpiod_set_value(pinfo->reset_gpio, 1);
	return err;
}

static int lg_panel_enable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	pr_err("sw49410: panel enabling");

	if (pinfo->enabled)
		return 0;


	pr_err("sw49410: backlight enable");
	ret = backlight_enable(pinfo->backlight);
	if (ret) {
		DRM_DEV_ERROR(panel->dev,
				"Failed to enable backlight %d\n", ret);
		return ret;
	}

	if (panel->dsc) {
		/* this panel uses DSC so send the pps */
		drm_dsc_pps_payload_pack(&pps, panel->dsc);
		print_hex_dump(KERN_DEBUG, "DSC params:", DUMP_PREFIX_NONE,
                               16, 1, &pps, sizeof(pps), false);

		//ret = mipi_dsi_picture_parameter_set(pinfo->link, &pps);
		//if (ret < 0) {
		//	DRM_DEV_ERROR(panel->dev,
		//		      "failed to set pps: %d\n", ret);
		//	return ret;
		//}
	} else {
		pr_err("sw49410: panel not dsc????");
	}

	pinfo->enabled = true;

	return 0;
}

static int lg_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;

	pr_err("sw49410: panel get mode");
	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode) {
		DRM_DEV_ERROR(panel->dev, "failed to add mode %ux%u\n",
				m->hdisplay, m->vdisplay);
		return -ENOMEM;
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static int lg_panel_backlight_update_status(struct backlight_device *bl)
{
	struct panel_info *pinfo = bl_get_data(bl);
	int ret = 0;

	pr_err("sw49410: backlight update status");

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & BL_CORE_FBBLANK) {
		pinfo->brightness = 0;
	}
	else
		pinfo->brightness = bl->props.brightness;

	ret = mipi_dsi_dcs_set_display_brightness(pinfo->link,
					pinfo->brightness);
	return ret;
}

static int lg_panel_backlight_get_brightness(struct backlight_device *bl)
{
	struct panel_info *pinfo = bl_get_data(bl);
	int ret = 0;
	u16 brightness = 0;

	pr_err("sw49410: backlight get brightness");

	ret = mipi_dsi_dcs_get_display_brightness(pinfo->link, &brightness);
	if (ret < 0)
		return ret;

	return brightness & 0xff;
}

const struct backlight_ops lg_panel_backlight_ops = {
	.update_status = lg_panel_backlight_update_status,
	.get_brightness = lg_panel_backlight_get_brightness,
};

static int lg_panel_backlight_init(struct panel_info *pinfo)
{
	struct backlight_properties props = {};
	struct backlight_device	*bl;
	struct device *dev = &pinfo->link->dev;

	pr_err("sw49410: backlight init");

	props.type = BACKLIGHT_RAW;

	/* Set the max_brightness to 255 to begin with */
	props.max_brightness = pinfo->max_brightness = 255;
	props.brightness = pinfo->max_brightness;
	pinfo->brightness = pinfo->max_brightness;

	pr_err("sw49410: register a new backlight");
	bl = devm_backlight_device_register(dev, "lg-sw49410_rev1", dev, pinfo,
					     &lg_panel_backlight_ops, &props);
	if (IS_ERR(bl)) {
		DRM_ERROR("failed to register backlight device\n");
		return PTR_ERR(bl);
	}
	pinfo->backlight = bl;

	return 0;
}


static const struct drm_panel_funcs panel_funcs = {
	.disable = lg_panel_disable,
	.unprepare = lg_panel_unprepare,
	.prepare = lg_panel_prepare,
	.enable = lg_panel_enable,
	.get_modes = lg_panel_get_modes,
};

static const struct panel_cmd lg_sw49410_rev1_on_cmds_1[] = {
	_INIT_CMD(0x00, 0x26, 0x02),	// MIPI_DCS_SET_GAMMA_CURVE, 0x02
	_INIT_CMD(0x00, 0x35, 0x00),	// MIPI_DCS_SET_TEAR_ON
	_INIT_CMD(0x00, 0x53, 0x0C, 0x30),
	_INIT_CMD(0x00, 0x55, 0x00, 0x70, 0xDF, 0x00, 0x70, 0xDF),
	_INIT_CMD(0x00, 0xF7, 0x01, 0x49, 0x0C),

	{},
};

static const struct panel_cmd lg_sw49410_rev1_on_cmds_2[] = {
	_INIT_CMD(0x00, 0xB0, 0xAC),
	_INIT_CMD(0x00, 0xCD,
			0x00, 0x00, 0x00, 0x19, 0x19, 0x19, 0x19, 0x19,
			0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
			0x16, 0x16),
	_INIT_CMD(0x00, 0xCB, 0x80, 0x5C, 0x07, 0x03, 0x28),
	_INIT_CMD(0x00, 0xC0, 0x02, 0x02, 0x0F),
	_INIT_CMD(0x00, 0xE5, 0x00, 0x3A, 0x00, 0x3A, 0x00, 0x0E, 0x10),
	_INIT_CMD(0x00, 0xB5,
			0x75, 0x60, 0x2D, 0x5D, 0x80, 0x00, 0x0A, 0x0B,
			0x00, 0x05, 0x0B, 0x00, 0x80, 0x0D, 0x0E, 0x40,
			0x00, 0x0C, 0x00, 0x16, 0x00, 0xB8, 0x00, 0x80,
			0x0D, 0x0E, 0x40, 0x00, 0x0C, 0x00, 0x16, 0x00,
			0xB8, 0x00, 0x81, 0x00, 0x03, 0x03, 0x03, 0x01,
			0x01),
	_INIT_CMD(0x00, 0x55, 0x04, 0x61, 0xDB, 0x04, 0x70, 0xDB),
	_INIT_CMD(0x00, 0xB0, 0xCA),

	{},
};

static const struct drm_display_mode lg_panel_default_mode = {
	.clock = (1440 + 168 + 4 + 84) * (3120 + 2 + 18 + 18) * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 168,
	.hsync_end = 1440 + 168 + 4,
	.htotal = 1440 + 168 + 4 + 84,
	.vdisplay = 3120,
	.vsync_start = 3120 + 2,
	.vsync_end = 3120 + 2 + 18,
	.vtotal = 3120 + 2 + 18 + 18,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct panel_desc lg_panel_desc = {
	.display_mode = &lg_panel_default_mode,

	.width_mm = 65,
	.height_mm = 140,

	.mode_flags = MIPI_DSI_MODE_LPM,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = lg_sw49410_rev1_on_cmds_1,
	.on_cmds_2 = lg_sw49410_rev1_on_cmds_2,

	.panel_name = "panel-lg-sw49410-rev1",
};


static const struct of_device_id panel_of_match[] = {
	{ .compatible = "lg,sw49410-rev1",
	  .data = &lg_panel_desc
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, panel_of_match);

static int panel_pinctrl_init(struct panel_info *panel)
{
	struct device *dev = &panel->link->dev;
	int rc = 0;

	pr_err("sw49410: panel pinctrl init");

	panel->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(panel->pinctrl)) {
		rc = PTR_ERR(panel->pinctrl);
		pr_err("failed to get pinctrl, rc=%d\n", rc);
		goto error;
	}

	panel->active = pinctrl_lookup_state(panel->pinctrl,
							"panel_active");
	if (IS_ERR_OR_NULL(panel->active)) {
		rc = PTR_ERR(panel->active);
		pr_err("failed to get pinctrl active state, rc=%d\n", rc);
		goto error;
	}

	panel->suspend =
		pinctrl_lookup_state(panel->pinctrl, "panel_suspend");

	if (IS_ERR_OR_NULL(panel->suspend)) {
		rc = PTR_ERR(panel->suspend);
		pr_err("failed to get pinctrl suspend state, rc=%d\n", rc);
		goto error;
	}

error:
	return rc;
}

static int panel_add(struct panel_info *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int i, ret;

	pr_err("sw49410: adding a new panel");

	pinfo->init_delay_us = 5000;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++)
		pinfo->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return ret;

	pr_err("sw49410: getting reset gpio");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pinfo->reset_gpio)) {
		DRM_DEV_ERROR(dev, "cannot get reset gpio %ld\n",
			PTR_ERR(pinfo->reset_gpio));
		return PTR_ERR(pinfo->reset_gpio);
	}

	ret = panel_pinctrl_init(pinfo);
	if (ret < 0)
		return ret;

	ret = lg_panel_backlight_init(pinfo);
	if (ret < 0)
		return ret;

	drm_panel_init(&pinfo->base, dev, &panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&pinfo->base);
	return ret;
}

static void panel_del(struct panel_info *pinfo)
{
	pr_err("sw49410: delete panel");
	if (pinfo->base.dev)
		drm_panel_remove(&pinfo->base);
}

static int panel_probe(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo;
	const struct panel_desc *desc;
	struct drm_dsc_config *dsc;
	int err;

	pr_err("sw49410: probing a new panel");

	pinfo = devm_kzalloc(&dsi->dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;

	pinfo->link = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);

	err = panel_add(pinfo);
	if (err < 0)
		return err;

	pr_err("sw49410: panel dsi attach");

	err = mipi_dsi_attach(dsi);
	if (err < 0) {
		pr_err("sw49410: link dis attach failed");
		return err;
	}

	/* The panel is DSC panel only, set the dsc params */
	dsc = devm_kzalloc(&dsi->dev, sizeof(*dsc), GFP_KERNEL);
	if (!dsc) {
		pr_err("sw49410: not dsc");
		return -ENOMEM;
	}

	dsc->dsc_version_major = 17;
	dsc->dsc_version_minor = 1;

	dsc->slice_height = 60;
	dsc->slice_width = 720;
	dsc->slice_count = 4; // TODO: fix this value
	dsc->bits_per_component = 10;
	dsc->bits_per_pixel = 10;
	dsc->block_pred_enable = true;

	pinfo->base.dsc = dsc;

	return err;
}

static int panel_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = lg_panel_unprepare(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to unprepare panel: %d\n",
				err);

	err = lg_panel_disable(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to disable panel: %d\n", err);

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to detach from DSI host: %d\n",
				err);

	panel_del(pinfo);

	return 0;
}

static void panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	lg_panel_disable(&pinfo->base);
	lg_panel_unprepare(&pinfo->base);
}

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-lg-sw49410-rev1",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
	.shutdown = panel_shutdown,
};
module_mipi_dsi_driver(panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_DESCRIPTION("LG SW49410-rev1 MIPI-DSI LED panel");
MODULE_LICENSE("GPL");
