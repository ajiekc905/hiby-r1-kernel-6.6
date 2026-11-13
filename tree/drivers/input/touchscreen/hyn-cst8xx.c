// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#define CST_REG_TOUCH		0x00
#define CST_REG_FW_VER		0xa6
#define CST_REG_VENDOR		0xa8
#define CST_REG_CHIP_ID		0xaa
#define CST_POINT_LEN		6
#define CST_BUF_LEN		(3 + CST_POINT_LEN)

struct cst_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset;
	struct regulator *vdd;
	u32 max_x, max_y;
};

static int cst_read(struct cst_ts *ts, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = ts->client->addr, .len = 1, .buf = &reg },
		{ .addr = ts->client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(ts->client->adapter, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static void cst_hw_reset(struct cst_ts *ts)
{
	if (!ts->reset)
		return;
	gpiod_set_value_cansleep(ts->reset, 1);
	msleep(20);
	gpiod_set_value_cansleep(ts->reset, 0);
	msleep(200);
}

static irqreturn_t cst_irq(int irq, void *data)
{
	struct cst_ts *ts = data;
	u8 buf[CST_BUF_LEN];
	const u8 *p = buf + 3;
	int count, x, y, event;
	bool down;

	if (cst_read(ts, CST_REG_TOUCH, buf, sizeof(buf)))
		return IRQ_HANDLED;
	count = buf[2] & 0x0f;
	event = p[0] >> 6;
	x = (p[0] & 0x0f) << 8 | p[1];
	y = (p[2] & 0x0f) << 8 | p[3];
	down = count > 0 && !(event & ~2) && p[2] < 0x20;
	input_mt_slot(ts->input, 0);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);
	if (down) {
		input_report_abs(ts->input, ABS_MT_POSITION_X, min_t(int, x, ts->max_x));
		input_report_abs(ts->input, ABS_MT_POSITION_Y, min_t(int, y, ts->max_y));
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static int cst_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *irq_gpio;
	struct cst_ts *ts;
	u8 id = 0xff, fw = 0, vendor = 0xff;
	int irq, ret, i;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	ts->max_x = 479;
	ts->max_y = 799;
	of_property_read_u32(dev->of_node, "touchscreen-max-x", &ts->max_x);
	of_property_read_u32(dev->of_node, "touchscreen-max-y", &ts->max_y);
	ts->vdd = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(ts->vdd)) {
		ts->vdd = NULL;
	} else {
		ret = regulator_enable(ts->vdd);
		if (ret)
			return ret;
	}
	ts->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset))
		return PTR_ERR(ts->reset);
	irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(irq_gpio))
		return dev_err_probe(dev, PTR_ERR(irq_gpio), "irq gpio\n");
	irq = gpiod_to_irq(irq_gpio);
	if (irq < 0)
		return irq;
	cst_hw_reset(ts);
	for (i = 0; i < 5; i++) {
		if (!cst_read(ts, CST_REG_CHIP_ID, &id, 1) &&
		    !cst_read(ts, CST_REG_FW_VER, &fw, 1) &&
		    !cst_read(ts, CST_REG_VENDOR, &vendor, 1))
			break;
		msleep(100);
	}
	if (i == 5)
		return dev_err_probe(dev, -ENODEV, "no answer at 0x%02x\n", client->addr);
	dev_info(dev, "chip 0x%02x fw 0x%02x vendor 0x%02x\n", id, fw, vendor);

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "hyn_ts";
	ts->input->id.bustype = BUS_I2C;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);
	ret = input_mt_init_slots(ts->input, 1, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;
	ret = input_register_device(ts->input);
	if (ret)
		return ret;
	return devm_request_threaded_irq(dev, irq, NULL, cst_irq,
					 IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "hyn_ts", ts);
}

static const struct of_device_id cst_of_match[] = {
	{ .compatible = "hynitron,cst8xx" },
	{ }
};
MODULE_DEVICE_TABLE(of, cst_of_match);

static struct i2c_driver cst_driver = {
	.probe = cst_probe,
	.driver = {
		.name = "hyn-cst8xx",
		.of_match_table = cst_of_match,
	},
};
module_i2c_driver(cst_driver);

MODULE_DESCRIPTION("Hynitron CST8xx single-touch controller (HiBy R1)");
MODULE_AUTHOR("ajiekc905");
MODULE_LICENSE("GPL");
