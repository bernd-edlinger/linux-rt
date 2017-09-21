/*
 * This is a GPIO driver for the internal FPGA Manager I/O ports
 * connecting the HPS to the FPGA logic on certain Altera parts.
 *
 * Copyright (c) 2015 Softing Industrial Automation GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/gpio/driver.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

struct fpgamgr_port_property {
	struct device_node		*node;
	const char			*name;
	unsigned int			idx;
};

struct fpgamgr_platform_data {
	struct fpgamgr_port_property	*properties;
	unsigned int			nports;
};

struct fpgamgr_gpio_port {
	struct gpio_chip		bgc;
	struct fpgamgr_gpio		*gpio;
	unsigned int			idx;
};

struct fpgamgr_gpio {
	struct	device			*dev;
	void __iomem			*regs;
	struct fpgamgr_gpio_port	*ports;
	unsigned int			nr_ports;
};

static int fpgamgr_gpio_add_port(struct fpgamgr_gpio *gpio,
				 struct fpgamgr_port_property *pp,
				 unsigned int offs)
{
	struct fpgamgr_gpio_port *port;
	void __iomem *dat;
	int err;

	port = &gpio->ports[offs];
	port->gpio = gpio;
	port->idx = pp->idx;

	dat = gpio->regs + (pp->idx * 4);

	err = bgpio_init(&port->bgc, gpio->dev, 4, dat, NULL, NULL,
			 NULL, NULL, pp->idx ? BGPIOF_NO_OUTPUT : 0);
	if (err) {
		dev_err(gpio->dev, "failed to init gpio chip for %s\n",
			pp->name);
		return err;
	}

	port->bgc.of_node = pp->node;

	err = devm_gpiochip_add_data(gpio->dev, &port->bgc, NULL);
	if (err)
		dev_err(gpio->dev, "failed to register gpiochip for %s\n",
			pp->name);

	return err;
}

static struct fpgamgr_platform_data *
fpgamgr_gpio_get_pdata_of(struct device *dev)
{
	struct device_node *np = dev->of_node, *port_np;
	struct fpgamgr_platform_data *pdata;
	struct fpgamgr_port_property *pp;
	int nports;
	int i;

	if (!np)
		return ERR_PTR(-ENODEV);

	nports = of_get_child_count(np);
	if (nports == 0)
		return ERR_PTR(-ENODEV);

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->properties = kcalloc(nports, sizeof(*pp), GFP_KERNEL);
	if (!pdata->properties) {
		kfree(pdata);
		return ERR_PTR(-ENOMEM);
	}

	pdata->nports = nports;

	i = 0;
	for_each_child_of_node(np, port_np) {
		pp = &pdata->properties[i++];
		pp->node = port_np;

		if (of_property_read_u32(port_np, "reg", &pp->idx) ||
		    pp->idx > 1) {
			dev_err(dev, "missing/invalid port index for %s\n",
				port_np->full_name);
			kfree(pdata->properties);
			kfree(pdata);
			return ERR_PTR(-EINVAL);
		}

		pp->name = port_np->full_name;
	}

	return pdata;
}

static inline void fpgamgr_free_pdata_of(struct fpgamgr_platform_data *pdata)
{
	if (!pdata)
		return;

	kfree(pdata->properties);
	kfree(pdata);
}

static int fpgamgr_gpio_probe(struct platform_device *pdev)
{
	unsigned int i;
	struct resource *res;
	struct fpgamgr_gpio *fgpio;
	int err;
	struct device *dev = &pdev->dev;
	struct fpgamgr_platform_data *pdata = dev_get_platdata(dev);
	bool is_pdata_alloc = !pdata;

	if (is_pdata_alloc) {
		pdata = fpgamgr_gpio_get_pdata_of(dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}

	if (!pdata->nports) {
		err = -ENODEV;
		goto out_err;
	}

	fgpio = devm_kzalloc(dev, sizeof(*fgpio), GFP_KERNEL);
	if (!fgpio) {
		err = -ENOMEM;
		goto out_err;
	}
	fgpio->dev = dev;
	fgpio->nr_ports = pdata->nports;

	fgpio->ports = devm_kcalloc(dev, fgpio->nr_ports,
				    sizeof(*fgpio->ports), GFP_KERNEL);
	if (!fgpio->ports) {
		err = -ENOMEM;
		goto out_err;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	fgpio->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(fgpio->regs)) {
		err = PTR_ERR(fgpio->regs);
		goto out_err;
	}

	for (i = 0; i < fgpio->nr_ports; i++) {
		err = fpgamgr_gpio_add_port(fgpio, &pdata->properties[i], i);
		if (err)
			goto out_unregister;
	}
	platform_set_drvdata(pdev, fgpio);

	goto out_err;

out_unregister:
	while (i > 0)
		devm_gpiochip_remove(dev, &fgpio->ports[--i].bgc);

out_err:
	if (is_pdata_alloc)
		fpgamgr_free_pdata_of(pdata);

	return err;
}

static const struct of_device_id fpgamgr_of_match[] = {
	{ .compatible = "altr,fpgamgr-gpio" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, fpgamgr_of_match);

static struct platform_driver fpgamgr_gpio_driver = {
	.driver		= {
		.name	= "gpio-altera-fpgamgr",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(fpgamgr_of_match),
	},
	.probe		= fpgamgr_gpio_probe,
};

module_platform_driver(fpgamgr_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bernd Edlinger");
MODULE_DESCRIPTION("Altera fpgamgr GPIO driver");
