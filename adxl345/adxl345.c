/*
 * Author : Thomas Stäheli
*/
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
#define ADXL345_THRESH_TAP      0x1D
#define ADXL345_DUR             0x21
#define ADXL345_LATENT          0x22
#define ADXL345_WINDOW          0x23
#define ADXL345_TAP_AXES        0x2A
#define ADXL345_ACT_TAP_STATUS  0x2B
#define ADXL345_INT_ENABLE      0x2E
#define ADXL345_INT_MAP         0x2F
#define ADXL345_INT_SOURCE      0x30

// Configuration
#define ADXL345_RANGE_4G       0x01
#define ADXL345_MEASURE_MODE   0x08
#define ADXL345_SLEEP_MODE     0x00

// Bits pour INT_ENABLE/INT_SOURCE
#define ADXL345_INT_SINGLE_TAP  0x40
#define ADXL345_INT_DOUBLE_TAP  0x20

// Recommandation fabricant dans la déclaration des axes
#define ADXL345_SUPRESS_BIT     (1 << 3)

// Bits pour TAP_AXES
#define ADXL345_TAP_AXIS_X      (1 << 2)
#define ADXL345_TAP_AXIS_Y      (1 << 1)
#define ADXL345_TAP_AXIS_Z      (1 << 0)

// Prototypes
// Chemin pour les sysfs : /sys/bus/i2c/devices/0-0053/...
static ssize_t tap_axis_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t tap_axis_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t tap_mode_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t tap_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t tap_wait_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t tap_count_show(struct device *dev, struct device_attribute *attr, char *buf);

// Attributs sysfs
static DEVICE_ATTR_RW(tap_axis);
static DEVICE_ATTR_RW(tap_mode);
static DEVICE_ATTR_RO(tap_wait);
static DEVICE_ATTR_RO(tap_count);

// Permet de regrouper toutes les fichiers sysfs qui seront crées
static struct attribute *adxl345_attrs[] = {
    &dev_attr_tap_axis.attr,
    &dev_attr_tap_mode.attr,
    &dev_attr_tap_wait.attr,
    &dev_attr_tap_count.attr,
    NULL,
};

// Groupe contenant toutes les fonctions sysfs
static const struct attribute_group adxl345_attr_group = {
    .attrs = adxl345_attrs,
};

struct adxl345_data {
    struct i2c_client *client;
    struct miscdevice miscdev;
    struct mutex lock;
    int irq;
    // Variables pour sysfs
    char tap_axis;            // 'x', 'y', 'z'
    char tap_mode;            // 'o'=off, 's'=single, 'd'=double, 'b'=both
    wait_queue_head_t wait_queue;
    atomic_t tap_count;
    atomic_t tap_event;       // 0=none, 1=single, 2=double
    atomic_t wait_busy;       // 0=free, 1=busy
};

static ssize_t tap_axis_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct adxl345_data *priv = dev_get_drvdata(dev);
    char axis_char;
    
    mutex_lock(&priv->lock);
    axis_char = priv->tap_axis;
    mutex_unlock(&priv->lock);
    
    return sprintf(buf, "%c\n", axis_char);
}

static ssize_t tap_axis_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct adxl345_data *priv = dev_get_drvdata(dev);
    struct i2c_client *client = priv->client;
    u8 new_axis = 0;
    u8 config;
    int ret;

    if (count < 1)
        return -EINVAL;
    
    switch (buf[0]) {
        case 'x': case 'X': 
            new_axis = ADXL345_TAP_AXIS_X; break;
        case 'y': case 'Y': 
            new_axis = ADXL345_TAP_AXIS_Y; break;
        case 'z': case 'Z': 
            new_axis = ADXL345_TAP_AXIS_Z; break;
        default: return -EINVAL;
    }
    
    mutex_lock(&priv->lock);
    
    // Mettre à jour la configuration matérielle
    config = ADXL345_SUPRESS_BIT | new_axis;
    ret = i2c_smbus_write_byte_data(client, ADXL345_TAP_AXES, config);
    
    if (ret < 0) {
        mutex_unlock(&priv->lock);
        dev_err(dev, "Erreur configuration tap_axis\n");
        return ret;
    }
    
    priv->tap_axis = buf[0];
    mutex_unlock(&priv->lock);
    
    return count;
}

static ssize_t tap_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct adxl345_data *priv = dev_get_drvdata(dev);
    const char *mode_str;
    
    mutex_lock(&priv->lock);
    switch (priv->tap_mode) {
        case 'o': mode_str = "off\n"; break;
        case 's': mode_str = "single\n"; break;
        case 'd': mode_str = "double\n"; break;
        case 'b': mode_str = "both\n"; break;
        default: mode_str = "unknown\n";
    }
    mutex_unlock(&priv->lock);
    
    return sprintf(buf, mode_str);
}

static ssize_t tap_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct adxl345_data *priv = dev_get_drvdata(dev);
    struct i2c_client *client = priv->client;
    char new_mode;
    u8 int_enable = 0;
    int ret;

    if (strncmp(buf, "off", 3) == 0) new_mode = 'o';
    else if (strncmp(buf, "single", 6) == 0) new_mode = 's';
    else if (strncmp(buf, "double", 6) == 0) new_mode = 'd';
    else if (strncmp(buf, "both", 4) == 0) new_mode = 'b';
    else return -EINVAL;
    
    mutex_lock(&priv->lock);
    
    // Configurer les interruptions matérielles selon la demande de l'utilisateur
    switch (new_mode) {
    case 's': 
        int_enable = ADXL345_INT_SINGLE_TAP; 
        break;
    case 'd': 
        int_enable = ADXL345_INT_DOUBLE_TAP; 
        break;
    case 'b': 
        int_enable = ADXL345_INT_SINGLE_TAP | ADXL345_INT_DOUBLE_TAP; 
        break;
    case 'o': 
    default: 
        int_enable = 0;
    }
    // Modification du registre d'interruption
    ret = i2c_smbus_write_byte_data(client, ADXL345_INT_ENABLE, int_enable);
    
    if (ret < 0) {
        mutex_unlock(&priv->lock);
        dev_err(dev, "Erreur configuration tap_mode\n");
        return ret;
    }
    
    priv->tap_mode = new_mode;
    mutex_unlock(&priv->lock);
    
    return count;
}

static ssize_t tap_wait_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct adxl345_data *priv = dev_get_drvdata(dev);
    DEFINE_WAIT(wait);
    int event;
    
    // Vérifier si déjà en attente
    // Compare la valeur actuelle de wait_busy avec 0
    // Si égale, remplace par 1 et retourne 0
    // Si différente, ne change rien et retourne la valeur actuelle
    if (atomic_cmpxchg(&priv->wait_busy, 0, 1) != 0)
        return sprintf(buf, "busy\n");
    
    // Attendre un événement
    prepare_to_wait(&priv->wait_queue, &wait, TASK_INTERRUPTIBLE);
    
    while (!(event = atomic_read(&priv->tap_event))) {
        // Permet d'interrompre le processus
        if (signal_pending(current)) {
            atomic_set(&priv->wait_busy, 0);
            finish_wait(&priv->wait_queue, &wait);
            return -ERESTARTSYS;
        }
        // Passe à une autre tâche mais se met en mode INTERRUPTIBLE
        schedule();
    }
    
    finish_wait(&priv->wait_queue, &wait);
    
    // Réinitialiser pour le prochain événement
    atomic_set(&priv->tap_event, 0);
    atomic_set(&priv->wait_busy, 0);
    
    return sprintf(buf, "%s\n", (event == 1) ? "single" : "double");
}

static ssize_t tap_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct adxl345_data *priv = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", atomic_read(&priv->tap_count));
}

static irqreturn_t adxl345_irq_thread(int irq, void *dev_id)
{
    struct adxl345_data *priv = dev_id;
    struct i2c_client *client = priv->client;
    u8 int_source, tap_status;
    int event_type = 0;
    char axes[4] = {0};  // Stockage des axes détectés
    int idx = 0;
    int ret;

    // Lire le registre INT_SOURCE
    ret = i2c_smbus_read_byte_data(client, ADXL345_INT_SOURCE);
    if (ret < 0) {
        dev_err(&client->dev, "Erreur lecture INT_SOURCE\n");
        return IRQ_NONE;
    }
    int_source = ret;

    // Lire le registre ACT_TAP_STATUS
    ret = i2c_smbus_read_byte_data(client, ADXL345_ACT_TAP_STATUS);
    if (ret < 0) {
        dev_err(&client->dev, "Erreur lecture ACT_TAP_STATUS\n");
        return IRQ_NONE;
    }
    tap_status = ret;

    // Identifier les axes concernés
    if (tap_status & ADXL345_TAP_AXIS_X) axes[idx++] = 'X';
    if (tap_status & ADXL345_TAP_AXIS_Y) axes[idx++] = 'Y';
    if (tap_status & ADXL345_TAP_AXIS_Z) axes[idx++] = 'Z';
    if (idx == 0) axes[idx++] = '?';

    // Identifier le type d'événement
    if (int_source & ADXL345_INT_SINGLE_TAP) {
        event_type = 1; 
    }
    if (int_source & ADXL345_INT_DOUBLE_TAP) {
        event_type = 2;
    }
    
    if (!event_type) {
        dev_dbg(&client->dev, "Interruption non gérée: 0x%02X\n", int_source);
        return IRQ_NONE;
    }

    atomic_inc(&priv->tap_count);
    atomic_set(&priv->tap_event, event_type);
    wake_up_interruptible(&priv->wait_queue);

    dev_info(&client->dev, "Detection: %s on axis %c\n", 
            (event_type == 1) ? "SINGLE TAP" : "DOUBLE TAP", priv->tap_axis);
    return IRQ_HANDLED;
}

static ssize_t adxl345_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct adxl345_data *priv = container_of(file->private_data, struct adxl345_data, miscdev);
    u8 data_regs[6];
    s16 raw_x, raw_y, raw_z; // Valeurs brutes signées
    int mg_x, mg_y, mg_z;    // Valeurs en millig (mg)
    unsigned int abs_x, abs_y, abs_z;
    char sign_x, sign_y, sign_z;
    int ret;
    unsigned int len;
    char output[50];

    if (buf == NULL || count == 0) {
        return 0;
    }

    mutex_lock(&priv->lock);
    ret = i2c_smbus_read_i2c_block_data(priv->client, ADXL345_DATAX0, sizeof(data_regs), data_regs);
    mutex_unlock(&priv->lock);

    if (ret != sizeof(data_regs)) {
        pr_err("Erreur lecture bloc: %d (attendu: %zu)\n", ret, sizeof(data_regs));
        return -EIO;
    }

    // Extraction des valeurs brutes (signées 16 bits)
    raw_x = (s16)((data_regs[1] << 8) | data_regs[0]);
    raw_y = (s16)((data_regs[3] << 8) | data_regs[2]);
    raw_z = (s16)((data_regs[5] << 8) | data_regs[4]);

    // Conversion en millig (mg) avec précision améliorée
    // 1 LSB = 7.8 mg (valeur réelle pour ±4g) -> 78/10 = 39/5
    mg_x = (raw_x * 78) / 10;
    mg_y = (raw_y * 78) / 10;
    mg_z = (raw_z * 78) / 10;

    // Gestion des signes et valeurs absolues
    sign_x = mg_x < 0 ? '-' : '+';
    sign_y = mg_y < 0 ? '-' : '+';
    sign_z = mg_z < 0 ? '-' : '+';
    abs_x = mg_x < 0 ? -mg_x : mg_x;
    abs_y = mg_y < 0 ? -mg_y : mg_y;
    abs_z = mg_z < 0 ? -mg_z : mg_z;

    // Formatage de sortie (X = -1.234 g)
    len = snprintf(output, sizeof(output),
                  "X = %c%u.%03u; Y = %c%u.%03u; Z = %c%u.%03u\n",
                  sign_x, abs_x / 1000, abs_x % 1000,
                  sign_y, abs_y / 1000, abs_y % 1000,
                  sign_z, abs_z / 1000, abs_z % 1000);

    // Gestion des erreurs de snprintf
    if (len >= sizeof(output)) {
        pr_warn("Troncation de la sortie (%u > %zu)\n", len, sizeof(output));
        len = sizeof(output) - 1;
    }

    // Gestion de la position de lecture
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

    // Stocker le numéro d'IRQ
    priv->irq = client->irq;
    
    // Configuration des paramètres de tap (valeurs typiques)
    ret = i2c_smbus_write_byte_data(client, ADXL345_THRESH_TAP, 0x20);   // Seuil à 2g (32 * 62.5mg)
    ret |= i2c_smbus_write_byte_data(client, ADXL345_DUR, 0x08);         // Durée à 5ms (8 * 625μs)
    ret |= i2c_smbus_write_byte_data(client, ADXL345_LATENT, 0x32);      // Latence à 50ms (50 * 1ms)
    ret |= i2c_smbus_write_byte_data(client, ADXL345_WINDOW, 0xFF);      // Fenêtre à 255ms (max)
    
    if (ret < 0) {
        dev_err(&client->dev, "Erreur configuration tap parameters\n");
        goto err_power_off;
    }

    // Activer la détection sur tous les axes + Supress bit (un sel axe considéré)
    ret = i2c_smbus_write_byte_data(client, ADXL345_TAP_AXES, ADXL345_SUPRESS_BIT | 
        ADXL345_TAP_AXIS_X | ADXL345_TAP_AXIS_Y | ADXL345_TAP_AXIS_Z);
    if (ret < 0) {
        dev_err(&client->dev, "Erreur configuration TAP_AXES\n");
        goto err_power_off;
    }

    // Configurer l'interruption (mapping et activation)
    ret = i2c_smbus_write_byte_data(client, ADXL345_INT_MAP, 0); // Toutes les INT sur INT1
    ret |= i2c_smbus_write_byte_data(client, ADXL345_INT_ENABLE, 
        ADXL345_INT_SINGLE_TAP | ADXL345_INT_DOUBLE_TAP);

    if (ret < 0) {
        dev_err(&client->dev, "Erreur configuration interruptions\n");
        goto err_power_off;
    }

    // Enregistrer l'IRQ threaded
    ret = devm_request_threaded_irq(&client->dev, priv->irq, NULL, adxl345_irq_thread, 
        IRQF_TRIGGER_RISING | IRQF_ONESHOT, DRV_NAME, priv);

    if (ret) {
        dev_err(&client->dev, "Erreur demande IRQ %d\n", priv->irq);
        goto err_power_off;
    }

    // Initialisation sysfs
    priv->tap_axis = 'z';  // Valeur par défaut
    priv->tap_mode = 'o';  // Mode off par défaut
    atomic_set(&priv->tap_count, 0);
    atomic_set(&priv->tap_event, 0);
    atomic_set(&priv->wait_busy, 0);
    init_waitqueue_head(&priv->wait_queue);
    
    // Enregistrement sysfs
    ret = sysfs_create_group(&client->dev.kobj, &adxl345_attr_group);
    if (ret) {
        dev_err(&client->dev, "Erreur création sysfs\n");
        goto err_power_off;
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

        goto err_misc_register;
    }

    pr_info("Driver ADXL345 init\n");
    return 0;    

err_misc_register:
    sysfs_remove_group(&client->dev.kobj, &adxl345_attr_group);
err_power_off:
    i2c_smbus_write_byte_data(client, ADXL345_POWER_CTL, ADXL345_SLEEP_MODE);
    return ret;
}

static void adxl345_remove(struct i2c_client *client)
{
    struct adxl345_data *priv = i2c_get_clientdata(client);

    // Désactiver les interruptions
    i2c_smbus_write_byte_data(client, ADXL345_INT_ENABLE, 0);
    // Mise en veille du capteur
    i2c_smbus_write_byte_data(client, ADXL345_POWER_CTL, ADXL345_SLEEP_MODE);

    // Nettoyage des ressources
    sysfs_remove_group(&client->dev.kobj, &adxl345_attr_group);
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