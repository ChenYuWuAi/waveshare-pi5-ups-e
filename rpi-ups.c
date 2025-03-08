/*
 * 电池管理MCU驱动 - 树外内核模块
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/reboot.h>

#define DRIVER_NAME "rpi_ups"

#define DESIGN_FULL_ENERGY_UWH 72000000 /* 72Wh */
#define DESIGN_FULL_ENERGY_MAH 4800     /* 4800 mAh */

#define DATA_TIMEOUT_MS 5000

struct rpi_ups_data
{
    struct i2c_client *client;
    /* 电池设备 */
    struct power_supply *battery;
    struct power_supply_desc battery_desc;
    struct power_supply_config battery_cfg;
    /* USB Charger 设备 */
    struct power_supply *usb_charger;
    struct power_supply_desc usb_charger_desc;
    struct power_supply_config usb_charger_cfg;
    /* 缓存数据 */
    u8 battery_buf[12];                /* 寄存器 0x20 读取的 12 字节数据 */
    u8 charger_buf[6];                 /* 寄存器 0x10 读取的 6 字节数据 */
    u8 charger_status;                 /* 寄存器 0x02 读取的 1 字节状态 */
    unsigned long last_update_jiffies; /* 最近一次数据更新时刻 */
    spinlock_t lock;                   /* 保护缓存数据 */
    struct task_struct *update_thread; /* 后台更新线程 */
};

static enum power_supply_property rpi_ups_battery_props[] = {
    POWER_SUPPLY_PROP_STATUS,             /* 电池充放电状态 */
    POWER_SUPPLY_PROP_PRESENT,            /* 电池是否存在 */
    POWER_SUPPLY_PROP_VOLTAGE_NOW,        /* 当前电池电压（单位：微伏） */
    POWER_SUPPLY_PROP_CURRENT_NOW,        /* 当前电池电流（单位：微安） */
    POWER_SUPPLY_PROP_POWER_NOW,          /* 当前电池功率（单位：微瓦） */
    POWER_SUPPLY_PROP_CAPACITY,           /* 百分比 */
    POWER_SUPPLY_PROP_ENERGY_NOW,         /* 当前能量 */
    POWER_SUPPLY_PROP_ENERGY_FULL,        /* 满电能量 */
    POWER_SUPPLY_PROP_MODEL_NAME,         /* 电池型号 */
    POWER_SUPPLY_PROP_MANUFACTURER,       /* 制造商 */
    POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,  /* 放电剩余时间（分钟） */
    POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,   /* 充电剩余时间（分钟） */
    POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN, /* 最低电量警告 */
};

static enum power_supply_property rpi_ups_usb_charger_props[] = {
    POWER_SUPPLY_PROP_STATUS,      /* 充电状态 */
    POWER_SUPPLY_PROP_VOLTAGE_NOW, /* 当前充电电压（单位：微伏） */
    POWER_SUPPLY_PROP_CURRENT_NOW, /* 当前充电电流（单位：微安） */
    POWER_SUPPLY_PROP_POWER_NOW,   /* 当前充电功率（单位：微瓦） */
};

/*
 * 更新线程：定时读取寄存器数据，并判断低电量关机条件
 */
static int rpi_ups_update_thread(void *arg)
{
    struct rpi_ups_data *data = arg;
    int ret;
    unsigned long flags;
    u8 buf12[12];
    u8 buf6[6];
    u8 status;
    int low_counter = 0; /* 低电量计数器 */
    int capacity, raw_current, voltage;

    while (!kthread_should_stop())
    {
        /* 读取电池数据：寄存器 0x20，12 字节 */
        ret = i2c_smbus_read_i2c_block_data(data->client, 0x20, 12, buf12);
        spin_lock_irqsave(&data->lock, flags);
        if (ret >= 0)
            memcpy(data->battery_buf, buf12, 12);
        /* 更新数据时间戳 */
        data->last_update_jiffies = jiffies;
        spin_unlock_irqrestore(&data->lock, flags);

        /* 读取 USB Charger 状态：寄存器 0x02，1 字节 */
        ret = i2c_smbus_read_i2c_block_data(data->client, 0x02, 1, &status);
        spin_lock_irqsave(&data->lock, flags);
        if (ret >= 0)
            data->charger_status = status;
        spin_unlock_irqrestore(&data->lock, flags);

        /* 读取 USB Charger 数据：寄存器 0x10，6 字节 */
        ret = i2c_smbus_read_i2c_block_data(data->client, 0x10, 6, buf6);
        spin_lock_irqsave(&data->lock, flags);
        if (ret >= 0)
            memcpy(data->charger_buf, buf6, 6);
        spin_unlock_irqrestore(&data->lock, flags);

        /* 低电量关机判断：从缓存中获取容量、充电电流及电压 */
        spin_lock_irqsave(&data->lock, flags);
        capacity = data->battery_buf[4] | (data->battery_buf[5] << 8);
        raw_current = (int16_t)(data->battery_buf[2] | (data->battery_buf[3] << 8));
        voltage = data->battery_buf[0] | (data->battery_buf[1] << 8);
        spin_unlock_irqrestore(&data->lock, flags);

        /*
         * 判断逻辑：如果电池容量低于 5%，且当前不在充电状态（raw_current < 50 表示低充电电流，
         * 这里假设负数表示放电状态），则累计低电量计数
         */
        if ((capacity <= 5) && (raw_current < 50))
        {
            low_counter++;
            if (low_counter >= 30)
            { /* 累计 30 次，即约 30 秒内持续低电量 */
                pr_info("RPI-UPS: Low battery detected (capacity=%d%%, current=%d), initiating system shutdown now.\n",
                        capacity, raw_current);
                /* 通过 I2C 写寄存器：向地址 0x2d 的设备写入 0x55 到寄存器 0x01 */
                if (data->client->addr == 0x2d)
                {
                    ret = i2c_smbus_write_byte_data(data->client, 0x01, 0x55);
                    if (ret < 0)
                        pr_err("RPI-UPS: Failed to write shutdown register: %d\n", ret);
                    else
                        pr_info("RPI-UPS: Shutdown register written successfully.\n");
                }
                else
                {
                    pr_err("RPI-UPS: I2C device 0x2d not detected, shutdown register write skipped.\n");
                }
                /* 触发系统关机 */
                kernel_power_off();
                break;
            }
            else
            {
                pr_info("RPI-UPS: Low battery, system will shutdown in %d seconds if not recovered.\n",
                        60 - 2 * low_counter);
            }
        }
        else
        {
            low_counter = 0;
        }
        msleep(1000); /* 每 1 秒执行一次循环 */
    }
    return 0;
}

/*
 * 电池设备 get_property：从缓存数据中返回信息，若数据超时则将 PRESENT 返回为 0
 */
static int rpi_ups_battery_get_property(struct power_supply *psy,
                                        enum power_supply_property psp,
                                        union power_supply_propval *val)
{
    struct rpi_ups_data *data = power_supply_get_drvdata(psy);
    unsigned long flags;
    unsigned long now = jiffies;
    u8 *buf;

    spin_lock_irqsave(&data->lock, flags);
    buf = data->battery_buf;
    if (time_after(now, data->last_update_jiffies + msecs_to_jiffies(DATA_TIMEOUT_MS)))
    {
        if (psp == POWER_SUPPLY_PROP_PRESENT)
        {
            val->intval = 0;
            spin_unlock_irqrestore(&data->lock, flags);
            return 0;
        }
    }
    switch (psp)
    {
    case POWER_SUPPLY_PROP_STATUS:
    {
        int16_t raw_current = (int16_t)(buf[2] | (buf[3] << 8));
        val->intval = raw_current < 0 ? POWER_SUPPLY_STATUS_DISCHARGING : POWER_SUPPLY_STATUS_CHARGING;
        break;
    }
    case POWER_SUPPLY_PROP_PRESENT:
        val->intval = 1;
        break;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        val->intval = (buf[0] | (buf[1] << 8)) * 1000;
        break;
    case POWER_SUPPLY_PROP_CURRENT_NOW:
    {
        int16_t raw_current = (int16_t)(buf[2] | (buf[3] << 8));
        val->intval = raw_current * 1000;
        break;
    }
    case POWER_SUPPLY_PROP_CAPACITY:
        val->intval = (buf[4] | (buf[5] << 8));
        break;
    case POWER_SUPPLY_PROP_ENERGY_NOW:
    {
        int energy = (buf[6] | (buf[7] << 8)) * 14800;
        val->intval = energy;
        break;
    }
    case POWER_SUPPLY_PROP_ENERGY_FULL:
        val->intval = DESIGN_FULL_ENERGY_UWH;
        break;
    case POWER_SUPPLY_PROP_MODEL_NAME:
        val->strval = "RPI-UPS";
        break;
    case POWER_SUPPLY_PROP_MANUFACTURER:
        val->strval = "Waveshare";
        break;
    case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
    case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
    {
        int16_t raw_current = (int16_t)(buf[2] | (buf[3] << 8));
        if (raw_current < 0)
        {
            if (psp == POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW)
                val->intval = (buf[8] | (buf[9] << 8));
            else
                val->intval = 0;
        }
        else
        {
            if (psp == POWER_SUPPLY_PROP_TIME_TO_FULL_NOW)
                val->intval = (buf[10] | (buf[11] << 8));
            else
                val->intval = 0;
        }
        break;
    }
    case POWER_SUPPLY_PROP_POWER_NOW:
    {
        int16_t raw_current = (int16_t)(buf[2] | (buf[3] << 8));
        val->intval = (buf[0] | (buf[1] << 8)) * raw_current;
        break;
    }
    case POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN:
        val->intval = 5; /* 5% */
        break;
    default:
        spin_unlock_irqrestore(&data->lock, flags);
        return -EINVAL;
    }
    spin_unlock_irqrestore(&data->lock, flags);
    return 0;
}

/*
 * USB Charger get_property：直接返回缓存的数据
 */
static int rpi_ups_usb_charger_get_property(struct power_supply *psy,
                                            enum power_supply_property psp,
                                            union power_supply_propval *val)
{
    struct rpi_ups_data *data = power_supply_get_drvdata(psy);
    unsigned long flags;
    u8 status;
    u8 *buf;

    spin_lock_irqsave(&data->lock, flags);
    status = data->charger_status;
    buf = data->charger_buf;
    spin_unlock_irqrestore(&data->lock, flags);

    switch (psp)
    {
    case POWER_SUPPLY_PROP_STATUS:
        if (status & 0x40)
            val->intval = POWER_SUPPLY_STATUS_CHARGING;
        else if (status & 0x80)
            val->intval = POWER_SUPPLY_STATUS_CHARGING;
        else if (status & 0x20)
            val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
        else
            val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
        break;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        val->intval = (buf[0] | (buf[1] << 8));
        break;
    case POWER_SUPPLY_PROP_CURRENT_NOW:
        val->intval = (buf[2] | (buf[3] << 8));
        break;
    case POWER_SUPPLY_PROP_POWER_NOW:
        val->intval = (buf[4] | (buf[5] << 8));
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int rpi_ups_probe(struct i2c_client *client)
{
    struct rpi_ups_data *data;
    int ret;

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);
    spin_lock_init(&data->lock);
    data->last_update_jiffies = jiffies;

    /* 注册电池设备 */
    data->battery_desc.name = "rpi-ups-battery";
    data->battery_desc.type = POWER_SUPPLY_TYPE_UPS;
    data->battery_desc.properties = rpi_ups_battery_props;
    data->battery_desc.num_properties = ARRAY_SIZE(rpi_ups_battery_props);
    data->battery_desc.get_property = rpi_ups_battery_get_property;
    data->battery_cfg.drv_data = data;

    data->battery = power_supply_register(&client->dev,
                                          &data->battery_desc,
                                          &data->battery_cfg);
    if (IS_ERR(data->battery))
    {
        ret = PTR_ERR(data->battery);
        dev_err(&client->dev, "Failed to register battery: %d\n", ret);
        return ret;
    }

    /* 注册 USB Charger 设备 */
    data->usb_charger_desc.name = "rpi-ups-usb-charger";
    data->usb_charger_desc.type = POWER_SUPPLY_TYPE_USB;
    data->usb_charger_desc.properties = rpi_ups_usb_charger_props;
    data->usb_charger_desc.num_properties = ARRAY_SIZE(rpi_ups_usb_charger_props);
    data->usb_charger_desc.get_property = rpi_ups_usb_charger_get_property;
    data->usb_charger_cfg.drv_data = data;

    data->usb_charger = power_supply_register(&client->dev,
                                              &data->usb_charger_desc,
                                              &data->usb_charger_cfg);
    if (IS_ERR(data->usb_charger))
    {
        ret = PTR_ERR(data->usb_charger);
        dev_err(&client->dev, "Failed to register USB charger: %d\n", ret);
        power_supply_unregister(data->battery);
        return ret;
    }

    /* 启动更新线程 */
    data->update_thread = kthread_run(rpi_ups_update_thread, data, "rpi_ups_update");
    if (IS_ERR(data->update_thread))
    {
        ret = PTR_ERR(data->update_thread);
        dev_err(&client->dev, "Failed to start update thread: %d\n", ret);
        power_supply_unregister(data->usb_charger);
        power_supply_unregister(data->battery);
        return ret;
    }

    dev_info(&client->dev, "Raspberrypi UPS driver probed\n");
    return 0;
}

static void rpi_ups_remove(struct i2c_client *client)
{
    struct rpi_ups_data *data = i2c_get_clientdata(client);

    if (data->update_thread)
        kthread_stop(data->update_thread);
    power_supply_unregister(data->usb_charger);
    power_supply_unregister(data->battery);
    dev_info(&client->dev, "Raspberrypi UPS driver removed\n");
}

static const struct i2c_device_id rpi_ups_id[] = {
    {DRIVER_NAME, 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, rpi_ups_id);

static const struct of_device_id rpi_ups_of_match[] = {
    {
        .compatible = "rpi,ups",
    },
    {}};
MODULE_DEVICE_TABLE(of, rpi_ups_of_match);

static struct i2c_driver rpi_ups_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = rpi_ups_of_match,
    },
    .probe = rpi_ups_probe,
    .remove = rpi_ups_remove,
    .id_table = rpi_ups_id,
};

module_i2c_driver(rpi_ups_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cheng Hao");
MODULE_DESCRIPTION("Waveshare RaspberryPi UPS Driver");
