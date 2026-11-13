// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define SADC_ENABLE	0x00
#define SADC_CONFIG	0x04
#define SADC_CTRL	0x08
#define SADC_STATUS	0x0c
#define SADC_AUX0	0x10
#define SADC_AUX1	0x14
#define SADC_CLKDIV	0x20
#define SADC_STABLE	0x24

#define SADC_POWERDOWN	0x8000
#define SADC_CHANNELS	4

struct x1600_sadc {
	void __iomem *base;
	struct clk *clk;
	struct mutex lock;
};

static int x1600_sadc_sample(struct x1600_sadc *adc, int ch)
{
	u8 st;
	u32 v;
	int ret;

	writeb(BIT(ch), adc->base + SADC_STATUS);
	writew(BIT(ch), adc->base + SADC_ENABLE);
	ret = readb_poll_timeout(adc->base + SADC_STATUS, st, st & BIT(ch), 20, 20000);
	writew(0, adc->base + SADC_ENABLE);
	writeb(BIT(ch), adc->base + SADC_STATUS);
	if (ret)
		return ret;
	v = readl(adc->base + (ch < 2 ? SADC_AUX0 : SADC_AUX1));
	return (ch & 1 ? v >> 16 : v) & 0xfff;
}

static int x1600_sadc_read_raw(struct iio_dev *iio, struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask)
{
	struct x1600_sadc *adc = iio_priv(iio);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&adc->lock);
		ret = x1600_sadc_sample(adc, chan->channel);
		mutex_unlock(&adc->lock);
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 3300;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;
	}
	return -EINVAL;
}

static const struct iio_info x1600_sadc_info = {
	.read_raw = x1600_sadc_read_raw,
};

#define SADC_CHAN(n) {							\
	.type = IIO_VOLTAGE,						\
	.indexed = 1,							\
	.channel = (n),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.datasheet_name = "AUX" #n,					\
}

static const struct iio_chan_spec x1600_sadc_channels[SADC_CHANNELS] = {
	SADC_CHAN(0), SADC_CHAN(1), SADC_CHAN(2), SADC_CHAN(3),
};

static int x1600_sadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *iio;
	struct x1600_sadc *adc;

	iio = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!iio)
		return -ENOMEM;
	adc = iio_priv(iio);
	mutex_init(&adc->lock);
	adc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adc->base))
		return PTR_ERR(adc->base);
	adc->clk = devm_clk_get_enabled(dev, "gate_sadc");
	if (IS_ERR(adc->clk))
		return dev_err_probe(dev, PTR_ERR(adc->clk), "gate_sadc\n");
	writew(SADC_POWERDOWN, adc->base + SADC_ENABLE);
	writel(1, adc->base + SADC_CONFIG);
	writeb(0xf0, adc->base + SADC_CTRL);
	writel(119 | (1 << 8) | (99 << 16), adc->base + SADC_CLKDIV);
	writel(0x00100001, adc->base + SADC_STABLE);
	writeb(0xff, adc->base + SADC_STATUS);
	writew(0, adc->base + SADC_ENABLE);
	iio->name = "x1600-sadc";
	iio->info = &x1600_sadc_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = x1600_sadc_channels;
	iio->num_channels = SADC_CHANNELS;
	return devm_iio_device_register(dev, iio);
}

static const struct of_device_id x1600_sadc_of_match[] = {
	{ .compatible = "ingenic,x1600-sadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, x1600_sadc_of_match);

static struct platform_driver x1600_sadc_driver = {
	.probe = x1600_sadc_probe,
	.driver = {
		.name = "x1600-sadc",
		.of_match_table = x1600_sadc_of_match,
	},
};
module_platform_driver(x1600_sadc_driver);

MODULE_DESCRIPTION("Ingenic X1600 SADC auxiliary channels");
MODULE_AUTHOR("ajiekc905");
MODULE_LICENSE("GPL");
