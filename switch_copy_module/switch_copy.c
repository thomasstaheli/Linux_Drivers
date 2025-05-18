#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
// Using misc framework
#include <linux/miscdevice.h>

#define NB_IRQ_TO_HANDLE		  			3
// KEYS
#define KEY0				  				0x01
#define KEY1				  				0x02
#define KEY2				  				0x04
// Peripherals offsets
#define LEDS_OFFSET			 			    0x00
#define KEYS_OFFSET			  				0x50
#define SWITCH_OFFSET			 		  	0x40
// Interrupts OFFSET
#define KEY_OFFSET_INTERRUPTMASK_REGISTER 	0x58
#define KEY_OFFSET_EDGECAPTURE_REGISTER	  	0x5C

// Private structure
struct priv {
	struct device *dev;
	struct miscdevice miscdev; // Framework
	void __iomem *base_addr; // base addr of Peripherals
	void __iomem *switch_addr;
	void __iomem *led_addr;
	// Addr pointing to important register to detect interrupt
	void __iomem *key_interrupt_mask;
	void __iomem *key_edge_capture;
	int irq_key;
};

static irqreturn_t irq_handler(int irq, void *data)
{
	struct priv *priv = (struct priv *)data;

	// Reading keys value
	uint8_t keys_value = ioread8(priv->key_edge_capture);

	// KEY0 pressed
	if (keys_value & 0x01) {
		// Copy switch value on leds
		uint16_t switch_value = ioread16(priv->switch_addr);
		iowrite16(switch_value, priv->led_addr);
	}
	// KEY1 pressed
	if (keys_value & 0x02) {
		// Shift right leds
		uint16_t led_value = ioread16(priv->led_addr);
		iowrite16(led_value >> 1, priv->led_addr);
	}
	// KEY2 pressed
	if (keys_value & 0x04) {
		// Shift left leds
		uint16_t led_value = ioread16(priv->led_addr);
		iowrite16(led_value << 1, priv->led_addr);
	}

	iowrite32(0xF, priv->key_edge_capture);

	return IRQ_HANDLED;
}

static int switch_copy_probe(struct platform_device *pdev)
{
	struct priv *priv;
	int err;
	struct resource *res;

	// Allocate memory for our private struct
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		return -ENOMEM;
	}

	// Store a pointer to our private struct in the platform device
	platform_set_drvdata(pdev, priv);
	// And link our private struct to the struct device
	priv->dev = &pdev->dev;

	// Retrieves the physical address of peripherals in the DT
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(priv->dev, "failed to get switch memory resource\n");
		return -ENXIO;
	}

	// Map the physical address to a virtual address
	priv->base_addr = devm_ioremap_resource(priv->dev, res);
	if (IS_ERR(priv->base_addr)) {
		return PTR_ERR(priv->base_addr);
	}

	// Register all periphericals addr
	priv->led_addr = priv->base_addr + LEDS_OFFSET;
	priv->switch_addr = priv->base_addr + SWITCH_OFFSET;
	priv->key_interrupt_mask =
		priv->base_addr + KEY_OFFSET_INTERRUPTMASK_REGISTER;
	priv->key_edge_capture =
		priv->base_addr + KEY_OFFSET_EDGECAPTURE_REGISTER;

	// Retrieve the IRQ number from the DT
	priv->irq_key = platform_get_irq(pdev, 0);
	if (priv->irq_key < 0) {
		dev_err(priv->dev, "failed to get KEYS IRQ\n");
		return priv->irq_key;
	}
	// Register the ISR function associated with the interrupt
	err = devm_request_irq(priv->dev, priv->irq_key, irq_handler,
			       IRQF_TRIGGER_RISING, "irq_key", priv);
	if (err) {
		dev_err(priv->dev, "failed to request IRQ for KEYS\n");
		return err;
	}

	// Setuping edge capture and key interrupt mask register to detect interruption
	iowrite32(0xF, priv->key_interrupt_mask);
	iowrite32(0xF, priv->key_edge_capture);

	// Register in the misc framework
	priv->miscdev.name = "drv_switch_copy";
	priv->miscdev.parent = &pdev->dev;
	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	err = misc_register(&priv->miscdev);
	if (err) {
		dev_err(&pdev->dev, "failed to register misc device (%d)\n",
			err);
		return err;
	}

	dev_info(&pdev->dev, "ready");

	return 0;

}

static int switch_copy_remove(struct platform_device *pdev)
{

	struct priv *priv = platform_get_drvdata(pdev);

	// Unregister the misc device
	misc_deregister(&priv->miscdev);

	// Turn off the LED
	iowrite32(0, priv->led_addr);

	dev_info(priv->dev, "removed");

	return 0;
}

static const struct of_device_id switch_copy_driver_id[] = {
	{ .compatible = "drv2025" },
	{ /* END */ },
};

MODULE_DEVICE_TABLE(of, switch_copy_driver_id);

static struct platform_driver switch_copy_driver = {
	.driver = {
		.name = "drv-lab4",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(switch_copy_driver_id),
	},
	.probe = switch_copy_probe,
	.remove = switch_copy_remove,
};

module_platform_driver(switch_copy_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("REDS");
MODULE_DESCRIPTION("Introduction to the interrupt and platform drivers");
