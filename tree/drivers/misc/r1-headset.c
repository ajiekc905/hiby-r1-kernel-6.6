// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
#include <linux/device.h>
#include <linux/iio/consumer.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

extern void axp2101_mic_bias(int on);

#define HS_POLL_MS	200
#define HS_MIN_MV	2800
#define HS_MAX_MV	3300
#define EP_POLL_MS	10
#define EP_SETTLE	10

struct ep_key {
	int hold_ticks;
	int repeat_ticks;
	int reset_ticks;
	int min;
	int max;
	int last;
	int code;
	int presses;
	int releases;
	int hold;
	int repeat;
	int idle;
	void (*process)(struct ep_key *k, struct input_dev *in, int pressed);
};

struct r1_headset {
	struct iio_channel *jack;
	struct iio_channel *remote;
	struct class *sw_class;
	struct device *sw_dev;
	int sample;
	int state;
	int diff;
	struct delayed_work poll;
	struct platform_device *ep_pdev;
	struct input_dev *input;
	struct delayed_work ep_work;
	int ep_on;
	int settle;
	int active;
	int bias_on;
	struct ep_key keys[3];
};

static void ep_event(struct input_dev *in, int code, int value)
{
	input_event(in, EV_KEY, code, value);
	input_event(in, EV_SYN, SYN_REPORT, 0);
}

static void ep_key_clear(struct ep_key *k)
{
	k->last = 0;
	k->code = 0;
	k->presses = 0;
	k->releases = 0;
	k->hold = 0;
	k->idle = 0;
}

static void ep_volume_process(struct ep_key *k, struct input_dev *in, int pressed)
{
	if (pressed) {
		if (++k->presses == 1)
			ep_event(in, k->code, 1);
	} else if (k->presses) {
		ep_event(in, k->code, 0);
		k->presses = 0;
	}
}

static void ep_pp_process(struct ep_key *k, struct input_dev *in, int pressed)
{
	int code;

	if (k->last != pressed) {
		if (pressed)
			k->presses++;
		else
			k->releases++;
		k->idle = 0;
	}
	if (pressed && k->last == pressed) {
		k->hold++;
		if (k->hold == k->hold_ticks)
			k->repeat = k->repeat_ticks;
		else if (k->hold > k->hold_ticks)
			k->repeat++;
		if (k->repeat >= k->repeat_ticks) {
			k->code = k->presses == 1 ? KEY_FASTFORWARD :
				  k->presses == 2 ? KEY_REWIND : 0;
			if (k->code)
				ep_event(in, k->code, 1);
			k->repeat = 0;
		}
	} else if (!pressed && k->hold >= k->hold_ticks) {
		ep_event(in, k->code, 0);
		ep_key_clear(k);
	}
	if (k->presses > 0 && k->presses == k->releases &&
	    ++k->idle > k->reset_ticks) {
		code = k->presses == 1 ? KEY_PLAYPAUSE :
		       k->presses == 2 ? KEY_NEXTSONG :
		       k->presses == 3 ? KEY_PREVIOUSSONG : 0;
		if (code) {
			ep_event(in, code, 1);
			ep_event(in, code, 0);
		}
		k->code = 0;
		k->presses = 0;
		k->releases = 0;
		k->hold = 0;
		k->idle = 0;
	}
	k->last = pressed;
}

static void ep_release_keys(struct r1_headset *hs)
{
	int i;

	for (i = 0; i < 3; i++)
		if (hs->keys[i].presses)
			hs->keys[i].process(&hs->keys[i], hs->input, 0);
}

static void ep_bias(struct r1_headset *hs, int on)
{
	if (hs->bias_on != on) {
		axp2101_mic_bias(on);
		hs->bias_on = on;
	}
}

static int hs_read_mv(struct iio_channel *chan, int *mv)
{
	return iio_read_channel_processed(chan, mv);
}

static void hs_poll(struct work_struct *work)
{
	struct r1_headset *hs = container_of(to_delayed_work(work),
					     struct r1_headset, poll);
	int mv, s;

	if (hs_read_mv(hs->jack, &mv) == 0) {
		s = mv >= HS_MIN_MV && mv < HS_MAX_MV;
		hs->sample = s;
		if (s != hs->state) {
			if (++hs->diff > 2) {
				char *envp[3];
				char state[16];

				hs->state = s;
				snprintf(state, sizeof(state), "SWITCH_STATE=%d", s);
				envp[0] = "SWITCH_NAME=headset";
				envp[1] = state;
				envp[2] = NULL;
				kobject_uevent_env(&hs->sw_dev->kobj, KOBJ_CHANGE, envp);
			}
		} else {
			hs->diff = 0;
		}
	}
	schedule_delayed_work(&hs->poll, msecs_to_jiffies(HS_POLL_MS));
}

static void ep_work_fn(struct work_struct *work)
{
	struct r1_headset *hs = container_of(to_delayed_work(work),
					     struct r1_headset, ep_work);
	int v, i;

	if (!hs->sample) {
		ep_bias(hs, 0);
		for (i = 0; i < 3; i++) {
			if (hs->keys[i].presses)
				hs->keys[i].process(&hs->keys[i], hs->input, 0);
			ep_key_clear(&hs->keys[i]);
		}
		hs->keys[1].code = KEY_VOLUMEUP;
		hs->keys[2].code = KEY_VOLUMEDOWN;
		hs->settle = EP_SETTLE;
		hs->active = 0;
		goto out;
	}
	ep_bias(hs, 1);
	if (hs_read_mv(hs->remote, &v))
		goto out;
	if (hs->settle) {
		if (v >= hs->keys[0].min && v < hs->keys[0].max)
			hs->settle--;
		else {
			hs->settle = 0;
			hs->active = 1;
		}
		goto out;
	}
	if (!hs->active)
		goto out;
	for (i = 0; i < 3; i++) {
		if (v >= hs->keys[i].min && v < hs->keys[i].max) {
			hs->keys[i].process(&hs->keys[i], hs->input, 1);
			goto out;
		}
	}
	ep_release_keys(hs);
out:
	if (hs->ep_on)
		schedule_delayed_work(&hs->ep_work, msecs_to_jiffies(EP_POLL_MS));
}

static ssize_t headset_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct r1_headset *hs = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", hs->state);
}

static ssize_t headset_name_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "headset\n");
}

static DEVICE_ATTR(state, 0444, headset_state_show, NULL);
static DEVICE_ATTR(name, 0444, headset_name_show, NULL);

static ssize_t ep_sw_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct r1_headset *hs = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", hs->ep_on);
}

static ssize_t ep_sw_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct r1_headset *hs = dev_get_drvdata(dev);

	if (!strncmp(buf, "on", 2)) {
		hs->ep_on = 1;
		schedule_delayed_work(&hs->ep_work, msecs_to_jiffies(50));
		return count;
	}
	hs->ep_on = 0;
	cancel_delayed_work_sync(&hs->ep_work);
	ep_bias(hs, 0);
	ep_release_keys(hs);
	return count;
}

static DEVICE_ATTR(earpods_adc_sw, 0644, ep_sw_show, ep_sw_store);

static struct attribute *ep_attrs[] = {
	&dev_attr_earpods_adc_sw.attr,
	NULL,
};

static const struct attribute_group ep_group = {
	.name = "earpods_adc",
	.attrs = ep_attrs,
};

static int ep_ticks(int ms)
{
	return (EP_POLL_MS / 2 + ms) / EP_POLL_MS;
}

static int hs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct r1_headset *hs;
	struct ep_key *k;
	int ret;

	hs = devm_kzalloc(dev, sizeof(*hs), GFP_KERNEL);
	if (!hs)
		return -ENOMEM;
	hs->jack = devm_iio_channel_get(dev, "headset");
	if (IS_ERR(hs->jack))
		return PTR_ERR(hs->jack);
	hs->remote = devm_iio_channel_get(dev, "remote");
	if (IS_ERR(hs->remote))
		return PTR_ERR(hs->remote);
	platform_set_drvdata(pdev, hs);

	hs->sw_class = class_create("switch");
	if (IS_ERR(hs->sw_class))
		return PTR_ERR(hs->sw_class);
	hs->sw_dev = device_create(hs->sw_class, dev, MKDEV(0, 0), hs, "headset");
	if (IS_ERR(hs->sw_dev)) {
		ret = PTR_ERR(hs->sw_dev);
		goto err_class;
	}
	ret = device_create_file(hs->sw_dev, &dev_attr_state);
	if (ret)
		goto err_swdev;
	device_create_file(hs->sw_dev, &dev_attr_name);

	hs->ep_pdev = platform_device_alloc("earpods_adc", -1);
	if (!hs->ep_pdev) {
		ret = -ENOMEM;
		goto err_swdev;
	}
	platform_set_drvdata(hs->ep_pdev, hs);
	ret = platform_device_add(hs->ep_pdev);
	if (ret)
		goto err_epalloc;
	ret = sysfs_create_group(&hs->ep_pdev->dev.kobj, &ep_group);
	if (ret)
		goto err_epdev;

	hs->input = devm_input_allocate_device(dev);
	if (!hs->input) {
		ret = -ENOMEM;
		goto err_group;
	}
	hs->input->name = "earpods_adc";
	hs->input->phys = "earpods-adc/input0";
	hs->input->id.bustype = BUS_HOST;
	hs->input->id.vendor = 1;
	hs->input->id.product = 2;
	hs->input->id.version = 0x100;
	k = &hs->keys[0];
	k->min = 0;
	k->max = 10;
	k->process = ep_pp_process;
	k->hold_ticks = ep_ticks(500);
	k->repeat_ticks = ep_ticks(200);
	k->reset_ticks = ep_ticks(500);
	hs->keys[1].min = 50;
	hs->keys[1].max = 250;
	hs->keys[1].code = KEY_VOLUMEUP;
	hs->keys[1].process = ep_volume_process;
	hs->keys[2].min = 350;
	hs->keys[2].max = 550;
	hs->keys[2].code = KEY_VOLUMEDOWN;
	hs->keys[2].process = ep_volume_process;
	input_set_capability(hs->input, EV_KEY, KEY_NEXTSONG);
	input_set_capability(hs->input, EV_KEY, KEY_PREVIOUSSONG);
	input_set_capability(hs->input, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(hs->input, EV_KEY, KEY_FASTFORWARD);
	input_set_capability(hs->input, EV_KEY, KEY_REWIND);
	input_set_capability(hs->input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(hs->input, EV_KEY, KEY_VOLUMEDOWN);
	ret = input_register_device(hs->input);
	if (ret)
		goto err_group;
	hs->settle = EP_SETTLE;
	INIT_DELAYED_WORK(&hs->poll, hs_poll);
	INIT_DELAYED_WORK(&hs->ep_work, ep_work_fn);
	schedule_delayed_work(&hs->poll, msecs_to_jiffies(HS_POLL_MS));
	dev_info(dev, "R1 headset switch + earpods remote\n");
	return 0;

err_group:
	sysfs_remove_group(&hs->ep_pdev->dev.kobj, &ep_group);
err_epdev:
	platform_device_del(hs->ep_pdev);
err_epalloc:
	platform_device_put(hs->ep_pdev);
err_swdev:
	device_destroy(hs->sw_class, MKDEV(0, 0));
err_class:
	class_destroy(hs->sw_class);
	return ret;
}

static int hs_remove(struct platform_device *pdev)
{
	struct r1_headset *hs = platform_get_drvdata(pdev);

	hs->ep_on = 0;
	cancel_delayed_work_sync(&hs->poll);
	cancel_delayed_work_sync(&hs->ep_work);
	ep_bias(hs, 0);
	input_unregister_device(hs->input);
	sysfs_remove_group(&hs->ep_pdev->dev.kobj, &ep_group);
	platform_device_del(hs->ep_pdev);
	platform_device_put(hs->ep_pdev);
	device_destroy(hs->sw_class, MKDEV(0, 0));
	class_destroy(hs->sw_class);
	return 0;
}

static const struct of_device_id hs_of_match[] = {
	{ .compatible = "hiby,r1-headset" },
	{ }
};
MODULE_DEVICE_TABLE(of, hs_of_match);

static struct platform_driver hs_driver = {
	.probe = hs_probe,
	.remove = hs_remove,
	.driver = {
		.name = "r1-headset",
		.of_match_table = hs_of_match,
	},
};
module_platform_driver(hs_driver);

MODULE_DESCRIPTION("HiBy R1 headset jack switch and inline remote");
MODULE_AUTHOR("ajiekc905");
MODULE_LICENSE("GPL");
