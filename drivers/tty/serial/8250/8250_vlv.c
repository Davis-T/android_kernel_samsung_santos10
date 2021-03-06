/*
 * 8250_vlv.c: driver for High Speed UART device of Intel ValleyView2
 *
 * Refer 8250.c and some other drivers in drivers/serial/
 *
 * (C) Copyright 2013 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/debugfs.h>
#include <asm/intel-mid.h>
#include <linux/pm_runtime.h>

MODULE_AUTHOR("Yang Bin <bin.yang@intel.com>");
MODULE_DESCRIPTION("Intel ValleyView HSU Runtime PM friendly Driver");
MODULE_LICENSE("GPL");

enum {
	baylake_0 = 0,
	baylake_1,
};

struct vlv_hsu_port {
	char			*name;
	int			use_dma;
	int			irq;
	int			*dev;
	unsigned char __iomem	*membase;
	int			idle_delay;
	int			wake_gpio;
	int			last_lcr;
	int			line;
	struct dentry		*debugfs;
};

struct vlv_hsu_config {
	char *name;
	int use_dma;
	int uartclk;
	int wake_gpio;
	int idle_delay;
	void(*setup)(struct vlv_hsu_port *);
};

static void baylake_setup(struct vlv_hsu_port *vp)
{
	writel(0, (vp->membase + 0x804));
	writel(3, (vp->membase + 0x804));
	writel(0x80020003, (vp->membase + 0x800));
}

static struct vlv_hsu_config port_configs[] = {
	[baylake_0] = {
		.name = "bt_gps_hsu",
		.uartclk = 50000000,
		.idle_delay = 100,
		.setup = baylake_setup,
	},
	[baylake_1] = {
		.name = "nouse_hsu",
		.uartclk = 50000000,
		.idle_delay = 100,
		.setup = baylake_setup,
	},
};

static void vlv_hsu_serial_out(struct uart_port *p, int offset, int value)
{
	struct vlv_hsu_port *vp = p->private_data;

	if (offset == UART_LCR)
		vp->last_lcr = value;

	offset <<= p->regshift;
	writeb(value, p->membase + offset);
}

static unsigned int vlv_hsu_serial_in(struct uart_port *p, int offset)
{
	offset <<= p->regshift;

	return readb(p->membase + offset);
}

static void vlv_hsu_serial_out32(struct uart_port *p, int offset, int value)
{
	struct vlv_hsu_port *vp = p->private_data;

	if (offset == UART_LCR)
		vp->last_lcr = value;

	offset <<= p->regshift;
	writel(value, p->membase + offset);
}

static unsigned int vlv_hsu_serial_in32(struct uart_port *p, int offset)
{
	offset <<= p->regshift;

	return readl(p->membase + offset);
}

/* Offset for the DesignWare's UART Status Register. */
#define UART_USR	0x1f

static int vlv_hsu_handle_irq(struct uart_port *p)
{
	struct vlv_hsu_port *vp = p->private_data;
	unsigned int iir = p->serial_in(p, UART_IIR);

	if (serial8250_handle_irq(p, iir)) {
		return 1;
	} else if ((iir & UART_IIR_BUSY) == UART_IIR_BUSY) {
		/* Clear the USR and write the LCR again. */
		(void)p->serial_in(p, UART_USR);
		p->serial_out(p, vp->last_lcr, UART_LCR);

		return 1;
	}

	return 0;
}

static int vlv_hsu_do_suspend(struct pci_dev *pdev)
{
	struct vlv_hsu_port *vp = pci_get_drvdata(pdev);

	serial8250_suspend_port(vp->line);
	return 0;
}

static int vlv_hsu_do_resume(struct pci_dev *pdev)
{
	struct vlv_hsu_port *vp = pci_get_drvdata(pdev);

	serial8250_resume_port(vp->line);
	return 0;
}

static int vlv_hsu_suspend(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);

	return vlv_hsu_do_suspend(pdev);
}

static int vlv_hsu_resume(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);

	return vlv_hsu_do_resume(pdev);
}

static int vlv_hsu_runtime_idle(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct vlv_hsu_port *vp = pci_get_drvdata(pdev);

	pm_schedule_suspend(dev, vp->idle_delay);
	return -EBUSY;
}

static int vlv_hsu_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);

	return vlv_hsu_do_suspend(pdev);
}

static int vlv_hsu_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);

	return vlv_hsu_do_resume(pdev);
}

static const struct dev_pm_ops vlv_hsu_pm_ops = {

	SET_SYSTEM_SLEEP_PM_OPS(vlv_hsu_suspend,
				vlv_hsu_resume)
	SET_RUNTIME_PM_OPS(vlv_hsu_runtime_suspend,
				vlv_hsu_runtime_resume,
				vlv_hsu_runtime_idle)
};

#ifdef CONFIG_DEBUG_FS
static int vlv_hsu_show(struct seq_file *s, void *data)
{
	struct vlv_hsu_port	*vp = data;

	seq_printf(s, "debugfs not implemented yet\n");
	return 0;
}

static int vlv_hsu_open(struct inode *inode, struct file *file)
{
	return single_open(file, vlv_hsu_show, inode->i_private);
}

static const struct file_operations vlv_hsu_operations = {
	.open		= vlv_hsu_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void vlv_hsu_debugfs_init(struct vlv_hsu_port *vp)
{
	vp->debugfs = debugfs_create_dir("hsu", NULL);
	debugfs_create_file(vp->name, S_IFREG | S_IRUGO,
				vp->debugfs, vp, &vlv_hsu_operations);
}

static void vlv_hsu_debugfs_exit(struct vlv_hsu_port *vp)
{
	if (vp->debugfs)
		debugfs_remove_recursive(vp->debugfs);
}
#else
static void vlv_hsu_debugfs_init(struct vlv_hsu_port *vp) { return; }
static void vlv_hsu_debugfs_exit(struct vlv_hsu_port *vp) { return; }
#endif	/* DEBUG_FS */

DEFINE_PCI_DEVICE_TABLE(hsu_port_pci_ids) = {
	{ PCI_VDEVICE(INTEL, 0x0f0a), baylake_0},
	{ PCI_VDEVICE(INTEL, 0x0f0C), baylake_0},
	{},
};

static int vlv_hsu_port_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	struct vlv_hsu_port *vp = NULL;
	struct uart_port port = {};
	struct vlv_hsu_config *cfg = &port_configs[id->driver_data];
	int ret;

	dev_info(&pdev->dev,
			"ValleyView HSU serial controller (ID: %04x:%04x)\n",
			pdev->vendor, pdev->device);

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	vp = devm_kzalloc(&pdev->dev, sizeof(*vp), GFP_KERNEL);
	if (!vp)
		goto err;

	spin_lock_init(&port.lock);
	memset(&port, 0, sizeof(port));
	port.private_data = vp;
	port.mapbase = pci_resource_start(pdev, 0);
	port.membase =
		ioremap_nocache(port.mapbase, pci_resource_len(pdev, 0));
	port.irq = pdev->irq;
	port.handle_irq = vlv_hsu_handle_irq;
	port.type = PORT_8250;
	port.flags = UPF_SHARE_IRQ | UPF_BOOT_AUTOCONF | UPF_IOREMAP |
		UPF_FIXED_PORT | UPF_FIXED_TYPE;
	port.dev = &pdev->dev;
	port.iotype = UPIO_MEM32;
	port.serial_in = vlv_hsu_serial_in32;
	port.serial_out = vlv_hsu_serial_out32;
	port.regshift = 2;
	port.uartclk = cfg->uartclk;

	vp->name = cfg->name;
	vp->use_dma = cfg->use_dma;
	vp->wake_gpio = cfg->wake_gpio;
	vp->idle_delay = cfg->idle_delay;
	vp->membase = port.membase;
	vp->irq = port.irq;
	vp->dev = port.dev;
	pci_set_drvdata(pdev, vp);
	if (cfg->setup)
		cfg->setup(vp);

	vp->line = serial8250_register_port(&port);
	if (vp->line < 0)
		goto err;

	vlv_hsu_debugfs_init(vp);
	pm_runtime_put_noidle(&pdev->dev);
	/* pm_runtime_allow(&pdev->dev); */
	return 0;

err:
	devm_kfree(&pdev->dev, vp);
	pci_disable_device(pdev);
	return ret;
}

static void vlv_hsu_port_remove(struct pci_dev *pdev)
{
	struct vlv_hsu_port *vp = pci_get_drvdata(pdev);

	vlv_hsu_debugfs_exit(vp);
	serial8250_unregister_port(vp->line);
	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);
	pci_disable_device(pdev);
	devm_kfree(&pdev->dev, vp);
}

static struct pci_driver hsu_port_pci_driver = {
	.name =		"HSU serial",
	.id_table =	hsu_port_pci_ids,
	.probe =	vlv_hsu_port_probe,
	.remove =	__devexit_p(vlv_hsu_port_remove),
	.driver = {
		.pm = &vlv_hsu_pm_ops,
	},
};

static int __init vlv_hsu_init(void)
{
	return pci_register_driver(&hsu_port_pci_driver);
}

static void __exit vlv_hsu_exit(void)
{
	pci_unregister_driver(&hsu_port_pci_driver);
}

module_init(vlv_hsu_init);
module_exit(vlv_hsu_exit);

