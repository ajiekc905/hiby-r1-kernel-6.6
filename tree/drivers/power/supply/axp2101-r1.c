// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define REG_STATUS0	0x00
#define REG_CONFIG	0x10
#define REG_IIN		0x16
#define REG_LOWWARN	0x1a
#define REG_IRQ_EN0	0x40
#define REG_IRQ_STAT0	0x48
#define REG_TS_CFG	0x50
#define REG_IPRE	0x61
#define REG_ICC		0x62
#define REG_ITERM	0x63
#define REG_CV		0x64
#define REG_BATDET	0x68
#define REG_CHGLED	0x69
#define REG_COIN	0x6a
#define REG_DCDC_MODE	0x81
#define REG_LDO_EN0	0x90
#define REG_ALDO2_V	0x93
#define REG_BLDO1_V	0x96
#define REG_BLDO2_V	0x97

struct axp2101 {
	struct device *dev;
	struct regmap *map;
	struct power_supply *ac;
	struct power_supply *usb;
	struct gpio_desc *irq_gpio;
	int irq;
	int icc_ma;
	int iin_ma;
};

static struct axp2101 *axp_power_off_dev;

static int axp_rd(struct axp2101 *a, unsigned int reg, unsigned int *v)
{
	return regmap_read(a->map, reg, v);
}

static bool axp_vbus(struct axp2101 *a)
{
	unsigned int v;

	if (axp_rd(a, REG_STATUS0, &v))
		return false;
	return !!(v & 0x20);
}

static int axp_ac_get(struct power_supply *psy, enum power_supply_property psp,
		      union power_supply_propval *val)
{
	struct axp2101 *a = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = axp_vbus(a);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = 4600000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = 1200000;
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property axp_ac_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
};

static char *axp_supplied_to[] = {
	"cw2015-battery",
	NULL
};

static const struct power_supply_desc axp_ac_desc = {
	.name = "ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = axp_ac_props,
	.num_properties = ARRAY_SIZE(axp_ac_props),
	.get_property = axp_ac_get,
};

static int axp_usb_get(struct power_supply *psy, enum power_supply_property psp,
		       union power_supply_propval *val)
{
	struct axp2101 *a = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = axp_vbus(a);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = 4400000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = a->icc_ma * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = a->iin_ma * 1000;
		return 0;
	default:
		return -EINVAL;
	}
}

static int axp_set_icc(struct axp2101 *a, int ma);
static int axp_set_iin(struct axp2101 *a, int ma);

static int axp_usb_set(struct power_supply *psy, enum power_supply_property psp,
		       const union power_supply_propval *val)
{
	struct axp2101 *a = power_supply_get_drvdata(psy);
	int ma = val->intval / 1000;
	int ret;

	if (ma < 0)
		ma = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = axp_set_icc(a, ma);
		if (!ret)
			a->icc_ma = ma > 2000 ? 2000 : ma;
		return ret;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		if (!ma)
			ma = 2000;
		ret = axp_set_iin(a, ma);
		if (!ret)
			a->iin_ma = ma;
		return ret;
	default:
		return -EINVAL;
	}
}


static enum power_supply_property axp_usb_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
};

static int axp_usb_writeable(struct power_supply *psy,
			     enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX ||
	       psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX;
}

static const struct power_supply_desc axp_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = axp_usb_props,
	.num_properties = ARRAY_SIZE(axp_usb_props),
	.get_property = axp_usb_get,
	.set_property = axp_usb_set,
	.property_is_writeable = axp_usb_writeable,
};

static irqreturn_t axp_irq(int irq, void *data)
{
	struct axp2101 *a = data;
	u8 stat[3];
	int i, ret;

	ret = regmap_bulk_read(a->map, REG_IRQ_STAT0, stat, sizeof(stat));
	if (ret)
		return IRQ_HANDLED;
	for (i = 0; i < 3; i++)
		if (stat[i])
			regmap_write(a->map, REG_IRQ_STAT0 + i, stat[i]);
	dev_dbg(a->dev, "irq stat %02x %02x %02x\n", stat[0], stat[1], stat[2]);
	power_supply_changed(a->ac);
	power_supply_changed(a->usb);
	return IRQ_HANDLED;
}

static void axp_power_off(void)
{
	regmap_update_bits(axp_power_off_dev->map, REG_CONFIG, 1, 1);
	msleep(500);
}

static const struct regmap_config axp_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static int axp_set_icc(struct axp2101 *a, int ma)
{
	unsigned int v;

	if (ma > 2000)
		ma = 2000;
	if (ma <= 200)
		v = ma / 25;
	else
		v = (ma - 200) / 100 + 8;
	return regmap_update_bits(a->map, REG_ICC, 0x1f, v);
}

static int axp_set_iin(struct axp2101 *a, int ma)
{
	static const unsigned int tbl[] = { 100, 500, 900, 1000, 1500, 2000 };
	unsigned int i, v;

	for (i = 0; i < ARRAY_SIZE(tbl); i++)
		if ((unsigned int)ma < tbl[i])
			break;
	v = i ? i - 1 : 0;
	if (ma > 2000)
		v = 7;
	return regmap_update_bits(a->map, REG_IIN, 7, v);
}

static void axp_charger_init(struct axp2101 *a)
{
	int limit = 4400, cv = 1;

	regmap_write(a->map, REG_TS_CFG, 0x0e);
	if (limit > 0x1003) {
		cv = 2;
		if (limit > 0x1067) {
			cv = 3;
			if (limit > 0x10fd) {
				cv = 4;
				if (limit > 0x112f) {
					cv = 5;
					if (limit > 0x11f7)
						cv = 0;
				}
			}
		}
	}
	regmap_update_bits(a->map, REG_CV, 7, cv);
	regmap_update_bits(a->map, REG_COIN, 7, 3);
	regmap_update_bits(a->map, REG_CHGLED, 0x40, 0);
	regmap_write(a->map, REG_LOWWARN, 0xa0);
	regmap_update_bits(a->map, REG_IPRE, 0xf, 4);
	regmap_update_bits(a->map, REG_ITERM, 0xf, 100 / 25);
	regmap_update_bits(a->map, REG_ITERM, 0x10, 0x10);
	regmap_update_bits(a->map, REG_DCDC_MODE, 0x10, 0x10);
	regmap_update_bits(a->map, REG_CHGLED, 1, 0);
	axp_set_iin(a, 500);
	axp_set_icc(a, 780);
	regmap_write(a->map, REG_BATDET, 1);
	a->icc_ma = 780;
	a->iin_ma = 500;
}

#define REG_ALDO4_V	0x95

void axp2101_mic_bias(int on)
{
	struct axp2101 *a = axp_power_off_dev;

	if (!a)
		return;
	regmap_update_bits(a->map, REG_ALDO4_V, 0x1f, 0x1c);
	regmap_update_bits(a->map, REG_LDO_EN0, 0x08, on ? 0x08 : 0);
}
EXPORT_SYMBOL(axp2101_mic_bias);

static int axp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = { };
	struct axp2101 *a;
	unsigned int v;
	int ret, i;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	a->map = devm_regmap_init_i2c(client, &axp_regmap_config);
	if (IS_ERR(a->map))
		return PTR_ERR(a->map);

	ret = axp_rd(a, 0x03, &v);
	if (ret)
		return -ENODEV;
	if ((v & 0xcf) != 0x47 && (v & 0xcf) != 0x4a) {
		dev_err(dev, "unknown chip id %02x\n", v);
		return -ENODEV;
	}
	dev_info(dev, "AXP2101 chip id %02x\n", v);

	regmap_update_bits(a->map, REG_CONFIG, 0x08, 0x08);
	regmap_update_bits(a->map, REG_CONFIG, 0x04, 0x04);
	regmap_update_bits(a->map, REG_CONFIG, 0x08, 0x08);
	regmap_update_bits(a->map, 0x22, 0x04, 0);
	regmap_write(a->map, 0x24, 0);
	regmap_update_bits(a->map, 0x25, 0x03, 0);
	regmap_update_bits(a->map, REG_ALDO2_V, 0x1f, 0x1c);
	regmap_update_bits(a->map, REG_BLDO1_V, 0x1f, 0x17);
	regmap_update_bits(a->map, REG_BLDO2_V, 0x1f, 0x17);
	regmap_update_bits(a->map, REG_LDO_EN0, 0x32, 0x32);
	axp_charger_init(a);

	cfg.drv_data = a;
	cfg.supplied_to = axp_supplied_to;
	cfg.num_supplicants = 1;
	a->ac = devm_power_supply_register(dev, &axp_ac_desc, &cfg);
	if (IS_ERR(a->ac))
		return PTR_ERR(a->ac);
	a->usb = devm_power_supply_register(dev, &axp_usb_desc, &cfg);
	if (IS_ERR(a->usb))
		return PTR_ERR(a->usb);

	a->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(a->irq_gpio))
		return PTR_ERR(a->irq_gpio);
	if (a->irq_gpio) {
		a->irq = gpiod_to_irq(a->irq_gpio);
		if (a->irq < 0)
			return a->irq;
		for (i = 0; i < 3; i++)
			regmap_write(a->map, REG_IRQ_STAT0 + i, 0xff);
		regmap_write(a->map, REG_IRQ_EN0, 0);
		regmap_write(a->map, REG_IRQ_EN0 + 1, 0xf0);
		regmap_write(a->map, REG_IRQ_EN0 + 2, 0);
		ret = devm_request_threaded_irq(dev, a->irq, NULL, axp_irq,
						IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
						"axp2101", a);
		if (ret)
			return ret;
		dev_info(dev, "IRQ %d on PB23\n", a->irq);
	}

	axp_power_off_dev = a;
	pm_power_off = axp_power_off;
	i2c_set_clientdata(client, a);
	return 0;
}

static void axp_remove(struct i2c_client *client)
{
	struct axp2101 *a = i2c_get_clientdata(client);

	if (pm_power_off == axp_power_off)
		pm_power_off = NULL;
	if (axp_power_off_dev == a)
		axp_power_off_dev = NULL;
}

static const struct of_device_id axp_of_match[] = {
	{ .compatible = "x-powers,axp2101" },
	{ }
};
MODULE_DEVICE_TABLE(of, axp_of_match);

static const struct i2c_device_id axp_id[] = {
	{ "axp2101", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, axp_id);

static struct i2c_driver axp_driver = {
	.driver = {
		.name = "axp2101-r1",
		.of_match_table = axp_of_match,
	},
	.probe = axp_probe,
	.remove = axp_remove,
	.id_table = axp_id,
};
static int __init axp_init(void)
{
	return i2c_add_driver(&axp_driver);
}
subsys_initcall(axp_init);

static void __exit axp_exit(void)
{
	i2c_del_driver(&axp_driver);
}
module_exit(axp_exit);

MODULE_DESCRIPTION("HiBy R1 AXP2101 PMIC charger and supplies");
MODULE_AUTHOR("ajiekc905");
MODULE_LICENSE("GPL");
