// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/lcd.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include "../include/ingenicfb.h"

#define LG_CMD(r)	(0x10000 | (r))
#define LG_DAT(v)	(0x20000 | (v))
#define ST_CMD(c)	(0x40000 | (c))
#define ST_DAT(v)	(0x40000 | 0x100 | (v))
#define SLEEP(us)	(0x80000000 | (us))

static const u32 lg_init[] = {
	LG_CMD(0xb100), LG_DAT(0x06), LG_CMD(0xb101), LG_DAT(0x01), LG_CMD(0xb102), LG_DAT(0x54),
	LG_CMD(0xb104), LG_DAT(0x07), LG_CMD(0xb105), LG_DAT(0x06), LG_CMD(0xb106), LG_DAT(0x06),
	LG_CMD(0xb107), LG_DAT(0x4a), LG_CMD(0xb108), LG_DAT(0x21), LG_CMD(0xb109), LG_DAT(0x08),
	LG_CMD(0xf101), LG_DAT(0xc0), LG_CMD(0xf103), LG_DAT(0x01), SLEEP(5000),
	LG_CMD(0xb400), LG_DAT(0x01), LG_CMD(0xb600), LG_DAT(0x10), LG_CMD(0x3a00), LG_DAT(0x77),
	LG_CMD(0x3b00), LG_DAT(0x03), LG_CMD(0x5e04), LG_DAT(0x02), LG_CMD(0x3600), LG_DAT(0x08),
	SLEEP(5000),
	LG_CMD(0xe000), LG_DAT(0x00), LG_CMD(0xe001), LG_DAT(0x2f), LG_CMD(0xe002), LG_DAT(0x3f),
	LG_CMD(0xe003), LG_DAT(0x1f), LG_CMD(0xe004), LG_DAT(0x21), LG_CMD(0xe005), LG_DAT(0x2c),
	LG_CMD(0xe006), LG_DAT(0x2c), LG_CMD(0xe007), LG_DAT(0x61), LG_CMD(0xe008), LG_DAT(0x1c),
	LG_CMD(0xe009), LG_DAT(0x18), LG_CMD(0xe00a), LG_DAT(0xbc), LG_CMD(0xe00b), LG_DAT(0x27),
	LG_CMD(0xe00c), LG_DAT(0x59), LG_CMD(0xe00d), LG_DAT(0x6a), LG_CMD(0xe00e), LG_DAT(0xbb),
	LG_CMD(0xe00f), LG_DAT(0xbe), LG_CMD(0xe010), LG_DAT(0x3e), LG_CMD(0xe011), LG_DAT(0x48),
	LG_CMD(0xe100), LG_DAT(0x00), LG_CMD(0xe101), LG_DAT(0x32), LG_CMD(0xe102), LG_DAT(0x45),
	LG_CMD(0xe103), LG_DAT(0x26), LG_CMD(0xe104), LG_DAT(0x21), LG_CMD(0xe105), LG_DAT(0x2c),
	LG_CMD(0xe106), LG_DAT(0x5c), LG_CMD(0xe107), LG_DAT(0x63), LG_CMD(0xe108), LG_DAT(0x1c),
	LG_CMD(0xe109), LG_DAT(0x19), LG_CMD(0xe10a), LG_DAT(0xd3), LG_CMD(0xe10b), LG_DAT(0x28),
	LG_CMD(0xe10c), LG_DAT(0x59), LG_CMD(0xe10d), LG_DAT(0x6b), LG_CMD(0xe10e), LG_DAT(0xdb),
	LG_CMD(0xe10f), LG_DAT(0xde), LG_CMD(0xe110), LG_DAT(0x5f), LG_CMD(0xe111), LG_DAT(0x6b),
	LG_CMD(0xe200), LG_DAT(0x7f), LG_CMD(0xe201), LG_DAT(0x7d), LG_CMD(0xe202), LG_DAT(0x86),
	LG_CMD(0xe203), LG_DAT(0x89), LG_CMD(0xe204), LG_DAT(0x10), LG_CMD(0xe205), LG_DAT(0x1e),
	LG_CMD(0xe206), LG_DAT(0x41), LG_CMD(0xe207), LG_DAT(0x77), LG_CMD(0xe208), LG_DAT(0x0d),
	LG_CMD(0xe209), LG_DAT(0x1d), LG_CMD(0xe20a), LG_DAT(0xc8), LG_CMD(0xe20b), LG_DAT(0x30),
	LG_CMD(0xe20c), LG_DAT(0x5f), LG_CMD(0xe20d), LG_DAT(0x71), LG_CMD(0xe20e), LG_DAT(0xc3),
	LG_CMD(0xe20f), LG_DAT(0xbe), LG_CMD(0xe210), LG_DAT(0x3e), LG_CMD(0xe211), LG_DAT(0x48),
	LG_CMD(0xe300), LG_DAT(0x7f), LG_CMD(0xe301), LG_DAT(0x7f), LG_CMD(0xe302), LG_DAT(0x86),
	LG_CMD(0xe303), LG_DAT(0x93), LG_CMD(0xe304), LG_DAT(0x10), LG_CMD(0xe305), LG_DAT(0x1e),
	LG_CMD(0xe306), LG_DAT(0x41), LG_CMD(0xe307), LG_DAT(0x89), LG_CMD(0xe308), LG_DAT(0x0d),
	LG_CMD(0xe309), LG_DAT(0x1d), LG_CMD(0xe30a), LG_DAT(0xe0), LG_CMD(0xe30b), LG_DAT(0x30),
	LG_CMD(0xe30c), LG_DAT(0x5f), LG_CMD(0xe30d), LG_DAT(0x70), LG_CMD(0xe30e), LG_DAT(0xe4),
	LG_CMD(0xe30f), LG_DAT(0xdf), LG_CMD(0xe310), LG_DAT(0x5f), LG_CMD(0xe311), LG_DAT(0x6b),
	LG_CMD(0xe400), LG_DAT(0x00), LG_CMD(0xe401), LG_DAT(0x43), LG_CMD(0xe402), LG_DAT(0x6f),
	LG_CMD(0xe403), LG_DAT(0x60), LG_CMD(0xe404), LG_DAT(0x21), LG_CMD(0xe405), LG_DAT(0x2c),
	LG_CMD(0xe406), LG_DAT(0x5a), LG_CMD(0xe407), LG_DAT(0x6d), LG_CMD(0xe408), LG_DAT(0x1c),
	LG_CMD(0xe409), LG_DAT(0x18), LG_CMD(0xe40a), LG_DAT(0xc6), LG_CMD(0xe40b), LG_DAT(0x27),
	LG_CMD(0xe40c), LG_DAT(0x59), LG_CMD(0xe40d), LG_DAT(0x6a), LG_CMD(0xe40e), LG_DAT(0xbb),
	LG_CMD(0xe40f), LG_DAT(0xbe), LG_CMD(0xe410), LG_DAT(0x3e), LG_CMD(0xe411), LG_DAT(0x48),
	LG_CMD(0xe500), LG_DAT(0x00), LG_CMD(0xe501), LG_DAT(0x46), LG_CMD(0xe502), LG_DAT(0x75),
	LG_CMD(0xe503), LG_DAT(0x67), LG_CMD(0xe504), LG_DAT(0x21), LG_CMD(0xe505), LG_DAT(0x2c),
	LG_CMD(0xe506), LG_DAT(0x5a), LG_CMD(0xe507), LG_DAT(0x7e), LG_CMD(0xe508), LG_DAT(0x1c),
	LG_CMD(0xe509), LG_DAT(0x19), LG_CMD(0xe50a), LG_DAT(0xdd), LG_CMD(0xe50b), LG_DAT(0x28),
	LG_CMD(0xe50c), LG_DAT(0x59), LG_CMD(0xe50d), LG_DAT(0x6b), LG_CMD(0xe50e), LG_DAT(0xdb),
	LG_CMD(0xe50f), LG_DAT(0xde), LG_CMD(0xe510), LG_DAT(0x5f), LG_CMD(0xe511), LG_DAT(0x6b),
	SLEEP(5000),
	LG_CMD(0xc200), LG_DAT(0x70), LG_CMD(0xc202), LG_DAT(0x70), LG_CMD(0xc203), LG_DAT(0x44),
	SLEEP(20000),
	LG_CMD(0xc200), LG_DAT(0x51), LG_CMD(0xc201), LG_DAT(0x06), LG_CMD(0xc202), LG_DAT(0x51),
	LG_CMD(0xc203), LG_DAT(0x66), SLEEP(20000),
	LG_CMD(0xc100), LG_DAT(0x45), SLEEP(5000),
	LG_CMD(0xc700), LG_DAT(0x46), LG_CMD(0xc000), LG_DAT(0xa3), LG_CMD(0xc002), LG_DAT(0x8a),
	SLEEP(5000),
	LG_CMD(0x1100), SLEEP(130000),
	LG_CMD(0x2900), LG_CMD(0x2c00),
};

static const u32 st_init[] = {
	ST_CMD(0xff), ST_DAT(0x77), ST_DAT(0x01), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x13),
	ST_CMD(0xef), ST_DAT(0x08), ST_CMD(0xff), ST_DAT(0x77), ST_DAT(0x01), ST_DAT(0x00),
	ST_DAT(0x00), ST_DAT(0x10), ST_CMD(0xc0), ST_DAT(0x63), ST_DAT(0x00), ST_CMD(0xc1),
	ST_DAT(0x09), ST_DAT(0x0c), ST_CMD(0xc2), ST_DAT(0x07), ST_DAT(0x08), ST_CMD(0xcc),
	ST_DAT(0x30), ST_CMD(0xb0), ST_DAT(0x00), ST_DAT(0x0d), ST_DAT(0x14), ST_DAT(0x0d),
	ST_DAT(0x11), ST_DAT(0x07), ST_DAT(0x04), ST_DAT(0x08), ST_DAT(0x08), ST_DAT(0x20),
	ST_DAT(0x05), ST_DAT(0x14), ST_DAT(0x12), ST_DAT(0x25), ST_DAT(0x2d), ST_DAT(0x1c),
	ST_CMD(0xb1), ST_DAT(0x00), ST_DAT(0x0c), ST_DAT(0x14), ST_DAT(0x0d), ST_DAT(0x11),
	ST_DAT(0x06), ST_DAT(0x03), ST_DAT(0x08), ST_DAT(0x08), ST_DAT(0x1f), ST_DAT(0x05),
	ST_DAT(0x14), ST_DAT(0x12), ST_DAT(0x25), ST_DAT(0x2e), ST_DAT(0x1c), ST_CMD(0xff),
	ST_DAT(0x77), ST_DAT(0x01), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x11), ST_CMD(0xb0),
	ST_DAT(0x58), ST_CMD(0xb1), ST_DAT(0x4a), ST_CMD(0xb2), ST_DAT(0x87), ST_CMD(0xb3),
	ST_DAT(0x80), ST_CMD(0xb5), ST_DAT(0x4c), ST_CMD(0xb7), ST_DAT(0x8a), ST_CMD(0xb8),
	ST_DAT(0x21), ST_CMD(0xc0), ST_DAT(0x03), ST_CMD(0xc1), ST_DAT(0x78), ST_CMD(0xc2),
	ST_DAT(0x78), ST_CMD(0xd0), ST_DAT(0x88), ST_CMD(0xe0), ST_DAT(0x00), ST_DAT(0x00),
	ST_DAT(0x02), ST_CMD(0xe1), ST_DAT(0x01), ST_DAT(0xa0), ST_DAT(0x03), ST_DAT(0xa0),
	ST_DAT(0x02), ST_DAT(0xa0), ST_DAT(0x04), ST_DAT(0xa0), ST_DAT(0x00), ST_DAT(0x44),
	ST_DAT(0x44), ST_CMD(0xe2), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00),
	ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00),
	ST_DAT(0x00), ST_DAT(0x00), ST_CMD(0xe3), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x33),
	ST_DAT(0x33), ST_CMD(0xe4), ST_DAT(0x44), ST_DAT(0x44), ST_CMD(0xe5), ST_DAT(0x01),
	ST_DAT(0x26), ST_DAT(0xa0), ST_DAT(0xa0), ST_DAT(0x03), ST_DAT(0x28), ST_DAT(0xa0),
	ST_DAT(0xa0), ST_DAT(0x05), ST_DAT(0x2a), ST_DAT(0xa0), ST_DAT(0xa0), ST_DAT(0x07),
	ST_DAT(0x2c), ST_DAT(0xa0), ST_DAT(0xa0), ST_CMD(0xe6), ST_DAT(0x00), ST_DAT(0x00),
	ST_DAT(0x33), ST_DAT(0x33), ST_CMD(0xe7), ST_DAT(0x44), ST_DAT(0x44), ST_CMD(0xe8),
	ST_DAT(0x02), ST_DAT(0x26), ST_DAT(0xa0), ST_DAT(0xa0), ST_DAT(0x04), ST_DAT(0x28),
	ST_DAT(0xa0), ST_DAT(0xa0), ST_DAT(0x06), ST_DAT(0x2a), ST_DAT(0xa0), ST_DAT(0xa0),
	ST_DAT(0x08), ST_DAT(0x2c), ST_DAT(0xa0), ST_DAT(0xa0), ST_CMD(0xeb), ST_DAT(0x00),
	ST_DAT(0x00), ST_DAT(0xe4), ST_DAT(0xe4), ST_DAT(0x44), ST_DAT(0x00), ST_DAT(0x40),
	ST_CMD(0xed), ST_DAT(0xff), ST_DAT(0xf7), ST_DAT(0x65), ST_DAT(0x4f), ST_DAT(0x0b),
	ST_DAT(0xa1), ST_DAT(0xcf), ST_DAT(0xff), ST_DAT(0xff), ST_DAT(0xfc), ST_DAT(0x1a),
	ST_DAT(0xb0), ST_DAT(0xf4), ST_DAT(0x56), ST_DAT(0x7f), ST_DAT(0xff), ST_CMD(0xef),
	ST_DAT(0x08), ST_DAT(0x08), ST_DAT(0x08), ST_DAT(0x45), ST_DAT(0x3f), ST_DAT(0x54),
	ST_CMD(0xff), ST_DAT(0x77), ST_DAT(0x01), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x13),
	ST_CMD(0xe8), ST_DAT(0x00), ST_DAT(0x0e), ST_CMD(0x3a), ST_DAT(0x66), ST_CMD(0x36),
	ST_DAT(0x08), ST_CMD(0x11), SLEEP(120000),
	ST_CMD(0xe8), ST_DAT(0x00), ST_DAT(0x0c), SLEEP(20000),
	ST_CMD(0xe8), ST_DAT(0x00), ST_DAT(0x00), ST_CMD(0xe6), ST_DAT(0x16), ST_DAT(0x7c),
	ST_CMD(0xff), ST_DAT(0x77), ST_DAT(0x01), ST_DAT(0x00), ST_DAT(0x00), ST_DAT(0x00),
	ST_CMD(0x29),
};

struct r1_panel {
	struct device *dev;
	struct lcd_device *lcd;
	struct gpio_desc *rst, *cs, *sck, *mosi;
	struct regulator *vcc, *vccio;
	u32 lcd_id;
	int power;
};

static void r1_spi_bits(struct r1_panel *p, u32 word, int nbits)
{
	int i;

	gpiod_set_value(p->sck, 0);
	gpiod_set_value(p->cs, 0);
	udelay(1);
	for (i = nbits - 1; i >= 0; i--) {
		gpiod_set_value(p->mosi, (word >> i) & 1);
		udelay(1);
		gpiod_set_value(p->sck, 1);
		udelay(1);
		gpiod_set_value(p->sck, 0);
	}
	udelay(1);
	gpiod_set_value(p->cs, 1);
	gpiod_set_value(p->mosi, 0);
	udelay(1);
}

static void r1_lg_cmd(struct r1_panel *p, u16 reg)
{
	r1_spi_bits(p, 0x2000 | (reg >> 8), 16);
	r1_spi_bits(p, reg & 0xff, 16);
}

static void r1_run(struct r1_panel *p, const u32 *ops, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		u32 op = ops[i];

		if (op & 0x80000000) {
			usleep_range(op & 0x7fffffff, (op & 0x7fffffff) + 100);
			continue;
		}
		switch (op & 0x70000) {
		case 0x10000:
			r1_lg_cmd(p, op & 0xffff);
			break;
		case 0x20000:
			r1_spi_bits(p, 0x4000 | (op & 0xff), 16);
			break;
		case 0x40000:
			r1_spi_bits(p, op & 0x1ff, 9);
			break;
		}
	}
}

static void r1_rail(struct r1_panel *p, struct regulator *r, int on)
{
	if (!r)
		return;
	if (on) {
		if (regulator_enable(r))
			dev_warn(p->dev, "rail enable failed\n");
		usleep_range(5000, 5100);
	} else {
		regulator_disable(r);
	}
}

static void r1_panel_on(struct r1_panel *p)
{
	gpiod_set_value(p->cs, 1);
	r1_rail(p, p->vccio, 1);
	r1_rail(p, p->vcc, 1);
	gpiod_set_value(p->rst, 0);
	usleep_range(5000, 5100);
	gpiod_set_value(p->rst, 1);
	msleep(50);
	gpiod_set_value(p->rst, 0);
	msleep(30);
	if (p->lcd_id)
		r1_run(p, st_init, ARRAY_SIZE(st_init));
	else
		r1_run(p, lg_init, ARRAY_SIZE(lg_init));
}

static void r1_panel_off(struct r1_panel *p)
{
	if (!p->lcd_id) {
		r1_lg_cmd(p, 0x2800);
		usleep_range(5000, 5100);
		r1_lg_cmd(p, 0x1000);
		usleep_range(5000, 5100);
	}
	gpiod_set_value(p->rst, 1);
	r1_rail(p, p->vccio, 0);
	r1_rail(p, p->vcc, 0);
	gpiod_set_value(p->mosi, 0);
	gpiod_set_value(p->sck, 0);
	gpiod_set_value(p->cs, 0);
}

#define POWER_IS_ON(pwr)	((pwr) <= FB_BLANK_NORMAL)

static int r1_set_power(struct lcd_device *lcd, int power)
{
	struct r1_panel *p = lcd_get_data(lcd);

	if (POWER_IS_ON(power) && !POWER_IS_ON(p->power))
		r1_panel_on(p);
	else if (!POWER_IS_ON(power) && POWER_IS_ON(p->power))
		r1_panel_off(p);
	p->power = power;
	return 0;
}

static int r1_get_power(struct lcd_device *lcd)
{
	struct r1_panel *p = lcd_get_data(lcd);

	return p->power;
}

static struct lcd_ops r1_lcd_ops = {
	.set_power = r1_set_power,
	.get_power = r1_get_power,
};

static struct fb_videomode r1_modes[] = {
	{
		.name = "hiby-r1",
		.refresh = 62,
		.xres = 480,
		.yres = 800,
		.left_margin = 24,
		.right_margin = 24,
		.upper_margin = 8,
		.lower_margin = 14,
		.hsync_len = 24,
		.vsync_len = 5,
		.vmode = FB_VMODE_NONINTERLACED,
	},
};

static struct tft_config r1_tft = {
	.color_even = TFT_LCD_COLOR_EVEN_BGR,
	.color_odd = TFT_LCD_COLOR_ODD_BGR,
	.mode = TFT_LCD_MODE_PARALLEL_888,
};

static struct lcd_panel_ops r1_panel_ops;

static struct lcd_panel r1_lcd_panel = {
	.name = "hiby-r1",
	.num_modes = ARRAY_SIZE(r1_modes),
	.modes = r1_modes,
	.bpp = 24,
	.lcd_type = LCD_TYPE_TFT,
	.tft_config = &r1_tft,
	.ops = &r1_panel_ops,
};

static struct regulator *r1_get_rail(struct device *dev, const char *id)
{
	struct regulator *r = devm_regulator_get_optional(dev, id);

	return IS_ERR(r) ? NULL : r;
}

static int r1_panel_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct r1_panel *p;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->rst = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	p->cs = devm_gpiod_get(dev, "spi-cs", GPIOD_OUT_HIGH);
	p->sck = devm_gpiod_get(dev, "spi-sck", GPIOD_OUT_LOW);
	p->mosi = devm_gpiod_get(dev, "spi-mosi", GPIOD_OUT_LOW);
	if (IS_ERR(p->rst) || IS_ERR(p->cs) || IS_ERR(p->sck) || IS_ERR(p->mosi))
		return dev_err_probe(dev, -EINVAL, "panel gpios missing\n");
	p->vcc = r1_get_rail(dev, "vcc");
	p->vccio = r1_get_rail(dev, "vccio");
	of_property_read_u32(dev->of_node, "hiby,lcd-id", &p->lcd_id);
	dev_info(dev, "%s controller, rails %s\n", p->lcd_id ? "ST7701S" : "LG",
		 p->vcc || p->vccio ? "managed" : "left as found");
	platform_set_drvdata(pdev, p);
	p->lcd = devm_lcd_device_register(dev, "panel_lcd", dev, p, &r1_lcd_ops);
	if (IS_ERR(p->lcd))
		return PTR_ERR(p->lcd);
	p->power = FB_BLANK_POWERDOWN;
	r1_set_power(p->lcd, FB_BLANK_UNBLANK);
	ret = ingenicfb_register_panel(&r1_lcd_panel);
	if (ret < 0) {
		r1_set_power(p->lcd, FB_BLANK_POWERDOWN);
		return dev_err_probe(dev, ret, "ingenicfb_register_panel\n");
	}
	return 0;
}

static const struct of_device_id r1_panel_of_match[] = {
	{ .compatible = "hiby,r1-panel" },
	{ }
};

static struct platform_driver r1_panel_driver = {
	.probe = r1_panel_probe,
	.driver = {
		.name = "hiby-r1-panel",
		.of_match_table = r1_panel_of_match,
	},
};
module_platform_driver(r1_panel_driver);

MODULE_DESCRIPTION("HiBy R1 480x800 TFT panel (LG / ST7701S controller)");
MODULE_AUTHOR("ajiekc905");
MODULE_LICENSE("GPL");
