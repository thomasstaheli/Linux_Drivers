#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/device.h>

#define DRV_NAME "adxl345"

// Registres ADXL345
#define ADXL345_DEVID          0x00
#define ADXL345_DEVID_VAL      0xE5
#define ADXL345_DATA_FORMAT    0x31
#define ADXL345_POWER_CTL      0x2D
#define ADXL345_DATAX0         0x32

// Configuration
#define ADXL345_RANGE_4G       0x01
#define ADXL345_MEASURE_MODE   0x08
#define ADXL345_SLEEP_MODE     0x00

// Specifications
// ±4 g, 10-bit resolution (datasheet)
#define ADXL345_4G_RES_10_BITS  8 // 7.8 typical dans la datasheet

struct adxl345_data {
    struct i2c_client *client;
    struct miscdevice miscdev;
    struct mutex lock;
};

static ssize_t adxl345_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct adxl345_data *priv = container_of(file->private_data, struct adxl345_data, miscdev);

    loff_t pos = *ppos;
    // Variable to stock DATAX0 to DATAZ1
    u8 data_regs[6];
    int data_x, data_y, data_z;
    unsigned int abs_x, abs_y, abs_z;
    int ret;
    unsigned int len;
    char output[50];

	/// Check parameters
	if (buf == NULL || count == 0) {
		// Invalid parameters
		return 0;
	}

    mutex_lock(&priv->lock);
    // Lecture des 6 registres qui contiennent les axes X Y et Z
    ret = i2c_smbus_read_i2c_block_data(priv->client, ADXL345_DATAX0, sizeof(data_regs), data_regs);
    mutex_unlock(&priv->lock);

    if(!ret) {
        pr_info("Error while reading data block\n");
        return 0;
    }

    // Cast en int obligatoire, sinon on perd les valeurs
    data_x = (((int)data_regs[1] << 8) | data_regs[0]);
    data_y = (((int)data_regs[3] << 8) | data_regs[2]);
    data_z = (((int)data_regs[5] << 8) | data_regs[4]);
    // Dans la datasheet, 
    data_x *= ADXL345_4G_RES_10_BITS;
    data_y *= ADXL345_4G_RES_10_BITS;
    data_z *= ADXL345_4G_RES_10_BITS;

	// Update the file position
    len = snprintf(output, sizeof(output),
                  "X = %c%u.%03u; Y = %c%u.%03u; Z = %c%u.%03u\n",
                  data_x < 0 ? ' ' : '+', data_x / 1000, data_x % 1000,
                  data_y < 0 ? ' ' : '+', data_y / 1000, data_y % 1000,
                  data_z < 0 ? ' ' : '+', data_z / 1000, data_z % 1000);

    if (len <= 0)
        return -EIO;

    if (*ppos >= len)
        return 0;

    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(buf, output + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct file_operations adxl345_fops = {
    .owner = THIS_MODULE,
    .read = adxl345_read,
};

static int adxl345_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct adxl345_data *priv;
    u8 devid;
    int ret;

    // Vérification DEVID
    devid = i2c_smbus_read_byte_data(client, ADXL345_DEVID);
    if (devid != ADXL345_DEVID_VAL) {
        pr_err("ID invalide: 0x%02x (attendu: 0x%02x)\n", 
                devid, ADXL345_DEVID_VAL);
        return -ENODEV;
    }

    // Allocation structure driver
    priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->client = client;
    mutex_init(&priv->lock);
    i2c_set_clientdata(client, priv);

    // Configuration DATA_FORMAT (+ ou - 4g)
    ret = i2c_smbus_write_byte_data(client, ADXL345_DATA_FORMAT, ADXL345_RANGE_4G);
    if (ret < 0) {
        pr_err("Erreur configuration DATA_FORMAT\n");
        return ret;
    }

    // Activation mode mesure
    ret = i2c_smbus_write_byte_data(client, ADXL345_POWER_CTL, ADXL345_MEASURE_MODE);
    if (ret < 0) {
        dev_err(&client->dev, "Erreur activation mode mesure\n");
        return ret;
    }

    // Configuration miscdevice
    priv->miscdev.minor = MISC_DYNAMIC_MINOR;
    priv->miscdev.name = DRV_NAME;
    priv->miscdev.fops = &adxl345_fops;
    priv->miscdev.parent = &client->dev;

    ret = misc_register(&priv->miscdev);
    if (ret) {
        pr_err("Erreur enregistrement miscdevice\n");
        // Mise en veille en cas d'erreur
        i2c_smbus_write_byte_data(client, ADXL345_POWER_CTL, ADXL345_SLEEP_MODE);
        return ret;
    }

    pr_info("Driver ADXL345 init\n");
    return 0;    
}

static void adxl345_remove(struct i2c_client *client)
{
    struct adxl345_data *priv = i2c_get_clientdata(client);

    // Mise en veille du capteur
    i2c_smbus_write_byte_data(client, ADXL345_POWER_CTL, ADXL345_SLEEP_MODE);
    
    // Nettoyage des ressources
    misc_deregister(&priv->miscdev);
    mutex_destroy(&priv->lock);
    pr_info("Driver ADXL345 removed\n");
}

static const struct i2c_device_id adxl345_id[] = {
    { "drv2025", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, adxl345_id);

static const struct of_device_id adxl345_of_match[] = {
    { .compatible = "adi,adxl345" },
    { }
};
MODULE_DEVICE_TABLE(of, adxl345_of_match);

static struct i2c_driver adxl345_driver = {
    .driver = {
        .name = DRV_NAME,
        .of_match_table = adxl345_of_match,
    },
    .probe = adxl345_probe,
    .remove = adxl345_remove,
    .id_table = adxl345_id,
};
module_i2c_driver(adxl345_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomas Stäheli");
MODULE_DESCRIPTION("Driver ADXL345 pour mesures d'accélération");