// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/usb/role.h>

struct tcs1421 {
	struct gpio_desc *cfg0;
	struct gpio_desc *cfg1;
	struct gpio_desc *vbus;
	struct usb_role_switch *role_sw;
	struct device_node *role_np;
	char cfg[20];
};

extern void ingenic_usb_phy_id_host(int host);

static void tcs_set(struct tcs1421 *t, const char *name, int save)
{
	int cfg0, cfg1, host;

	if (!strcmp(name, "Sink")) {
		cfg0 = 0; cfg1 = 0; host = 0;
	} else if (!strcmp(name, "Source")) {
		cfg0 = 1; cfg1 = 0; host = 1;
	} else if (!strcmp(name, "StrongDRP")) {
		cfg0 = 0; cfg1 = 1; host = 1;
	} else {
		name = "NormalDRP";
		cfg0 = 1; cfg1 = 1; host = 1;
	}
	if (save)
		strscpy(t->cfg, name, sizeof(t->cfg));
	if (!t->role_sw && t->role_np)
		t->role_sw = usb_role_switch_find_by_fwnode(of_fwnode_handle(t->role_np));
	if (host) {
		ingenic_usb_phy_id_host(1);
		gpiod_set_value(t->cfg0, cfg0);
		gpiod_set_value(t->cfg1, cfg1);
		if (t->vbus)
			gpiod_set_value(t->vbus, 1);
		if (t->role_sw)
			usb_role_switch_set_role(t->role_sw, USB_ROLE_HOST);
	} else {
		if (t->role_sw)
			usb_role_switch_set_role(t->role_sw, USB_ROLE_DEVICE);
		if (t->vbus)
			gpiod_set_value(t->vbus, 0);
		ingenic_usb_phy_id_host(0);
		gpiod_set_value(t->cfg0, cfg0);
		gpiod_set_value(t->cfg1, cfg1);
	}
}

static ssize_t tcs1421_cfg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct tcs1421 *t = dev_get_drvdata(dev);

	return sprintf(buf, "%s", t->cfg);
}

static ssize_t tcs1421_cfg_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct tcs1421 *t = dev_get_drvdata(dev);
	char name[28];

	if (sscanf(buf, "%27s", name) != 1)
		name[0] = '\0';
	tcs_set(t, name, 1);
	return count;
}

static DEVICE_ATTR(tcs1421_cfg, 0600, tcs1421_cfg_show, tcs1421_cfg_store);

static int tcs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tcs1421 *t;
	int ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->cfg0 = devm_gpiod_get(dev, "cfg0", GPIOD_OUT_LOW);
	if (IS_ERR(t->cfg0))
		return PTR_ERR(t->cfg0);
	t->cfg1 = devm_gpiod_get(dev, "cfg1", GPIOD_OUT_LOW);
	if (IS_ERR(t->cfg1))
		return PTR_ERR(t->cfg1);
	t->vbus = devm_gpiod_get_optional(dev, "vbus", GPIOD_OUT_LOW);
	if (IS_ERR(t->vbus))
		return PTR_ERR(t->vbus);
	t->role_np = of_parse_phandle(dev->of_node, "usb-role", 0);
	platform_set_drvdata(pdev, t);
	ret = device_create_file(dev, &dev_attr_tcs1421_cfg);
	if (ret)
		return ret;
	tcs_set(t, "Sink", 1);
	dev_info(dev, "TCS1421 straps on cfg0/cfg1, role target %s\n",
		 t->role_np ? t->role_np->name : "none");
	return 0;
}

static int tcs_remove(struct platform_device *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_tcs1421_cfg);
	return 0;
}

static const struct of_device_id tcs_of_match[] = {
	{ .compatible = "hiby,tcs1421" },
	{ }
};
MODULE_DEVICE_TABLE(of, tcs_of_match);

static struct platform_driver tcs_driver = {
	.probe = tcs_probe,
	.remove = tcs_remove,
	.driver = {
		.name = "tcs1421",
		.of_match_table = tcs_of_match,
	},
};
module_platform_driver(tcs_driver);

MODULE_DESCRIPTION("HiBy R1 TCS1421 USB-C role straps");
MODULE_AUTHOR("ajiekc905");
MODULE_LICENSE("GPL");
