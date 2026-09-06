// SPDX-License-Identifier: GPL-2.0-only
/*
 * Battery driver for the PiSugar 3 family of Raspberry Pi UPS boards
 *
 * The board is a small microcontroller on I2C (address 0x57 by default)
 * fronting a charger, a boost converter and a single-cell lithium polymer
 * pack. Over I2C it reports the cell voltage, a percentage of its own,
 * whether external power is present, whether charging is permitted, and
 * its temperature. It does not report current on any firmware seen so far
 * (the current registers read zero on v1.3.4), so this is a voltage gauge:
 * capacity comes from a voltage-to-capacity table supplied through a
 * "monitored-battery" simple-battery node, and only falls back to the
 * board's own percentage when no table is given. The board's number is a
 * generous lookup, not a measurement, and reads well above a table measured
 * on the actual cell.
 *
 * The driver reads, and writes exactly once: at system power-off, when the
 * node carries "system-power-controller", it asks the board to drop the 5V
 * output a few seconds later. A halted Pi still draws tens of milliamps and
 * would flatten the cell; the cut is what makes a shutdown a shutdown. The
 * same write sets the bit that turns the output back on when external power
 * returns - on a device with no power button that bit is the only way it
 * ever comes back, and it is not recoverable in software once cleared, so
 * nothing here ever clears it.
 *
 * Copyright (C) 2026 Luca Martinetti <luca@luca.io>
 */

#include <linux/bits.h>
#include <linux/ctype.h>
#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>

#define PISUGAR3_REG_VERSION		0x00	/* 3 on a PiSugar 3 */
#define PISUGAR3_REG_MODE		0x01	/* 0x0f: application firmware */
#define PISUGAR3_REG_CTRL1		0x02
#define PISUGAR3_REG_TEMP		0x04	/* degrees Celsius + 40 */
#define PISUGAR3_REG_POWER_OFF_DELAY	0x09	/* seconds, once the output bit is cleared */
#define PISUGAR3_REG_VOLTAGE		0x22	/* millivolts, big-endian u16 */
#define PISUGAR3_REG_CAPACITY		0x2a	/* the board's own percentage */
#define PISUGAR3_REG_FIRMWARE		0xe2	/* NUL-terminated ASCII, "v1.3.4" */

#define PISUGAR3_VERSION_3		0x03
#define PISUGAR3_MODE_APPLICATION	0x0f

#define PISUGAR3_CTRL1_EXTERNAL_POWER	BIT(7)
#define PISUGAR3_CTRL1_CHARGE_ENABLE	BIT(6)
#define PISUGAR3_CTRL1_OUTPUT_ENABLE	BIT(5)
#define PISUGAR3_CTRL1_RESTART_ON_POWER	BIT(4)

#define PISUGAR3_TEMP_OFFSET		40
#define PISUGAR3_FIRMWARE_LEN		16
#define PISUGAR3_POLL_MS		10000

/*
 * Seconds between the power-off handler arming the cut and the rail going.
 * The handler runs at the end of kernel_power_off(), after userspace is gone
 * and before machine_power_off(), so the halt is milliseconds away; the
 * margin is for the board's countdown, which its datasheet calls inaccurate
 * and which has been seen run long, never short.
 */
#define PISUGAR3_POWER_OFF_DELAY_S	5

struct pisugar3_reading {
	int voltage_uv;
	int temp;		/* tenths of a degree Celsius */
	int capacity;		/* percent */
	int status;		/* POWER_SUPPLY_STATUS_* */
	bool online;		/* external power present */
};

struct pisugar3 {
	struct i2c_client *client;
	struct power_supply *battery;
	struct power_supply *mains;
	struct power_supply_battery_info *info;
	struct delayed_work work;
	struct mutex lock;	/* protects @last */
	struct pisugar3_reading last;
};

static int pisugar3_read(struct pisugar3 *ps, struct pisugar3_reading *r)
{
	struct i2c_client *client = ps->client;
	bool charging;
	int ret;

	ret = i2c_smbus_read_byte_data(client, PISUGAR3_REG_CTRL1);
	if (ret < 0)
		return ret;
	r->online = ret & PISUGAR3_CTRL1_EXTERNAL_POWER;
	charging = r->online && (ret & PISUGAR3_CTRL1_CHARGE_ENABLE);

	/*
	 * One 16-bit transfer: the board auto-increments, so the high and
	 * low bytes come from the same conversion.
	 */
	ret = i2c_smbus_read_word_swapped(client, PISUGAR3_REG_VOLTAGE);
	if (ret < 0)
		return ret;
	r->voltage_uv = ret * 1000;

	ret = i2c_smbus_read_byte_data(client, PISUGAR3_REG_TEMP);
	if (ret < 0)
		return ret;
	r->temp = (ret - PISUGAR3_TEMP_OFFSET) * 10;

	ret = -ENODATA;
	if (ps->info)
		ret = power_supply_batinfo_ocv2cap(ps->info, r->voltage_uv,
						   r->temp / 10);
	if (ret < 0) {
		ret = i2c_smbus_read_byte_data(client, PISUGAR3_REG_CAPACITY);
		if (ret < 0)
			return ret;
		ret = min(ret, 100);
	}
	r->capacity = ret;

	/*
	 * The board has no "charge finished" signal: the charge-enable bit
	 * stays set for as long as the cable is in. Full is therefore the
	 * top of the table and nothing else.
	 */
	if (!r->online)
		r->status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (!charging)
		r->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	else if (r->capacity >= 100)
		r->status = POWER_SUPPLY_STATUS_FULL;
	else
		r->status = POWER_SUPPLY_STATUS_CHARGING;

	return 0;
}

static int pisugar3_update(struct pisugar3 *ps)
{
	struct pisugar3_reading r;
	bool battery_changed, mains_changed;
	int ret;

	ret = pisugar3_read(ps, &r);
	if (ret) {
		dev_err_ratelimited(&ps->client->dev, "read failed: %d\n", ret);
		return ret;
	}

	mutex_lock(&ps->lock);
	mains_changed = r.online != ps->last.online;
	battery_changed = mains_changed || r.status != ps->last.status ||
			  r.capacity != ps->last.capacity;
	ps->last = r;
	mutex_unlock(&ps->lock);

	if (battery_changed)
		power_supply_changed(ps->battery);
	if (mains_changed)
		power_supply_changed(ps->mains);

	return 0;
}

static void pisugar3_work(struct work_struct *work)
{
	struct pisugar3 *ps = container_of(work, struct pisugar3, work.work);

	pisugar3_update(ps);
	schedule_delayed_work(&ps->work, msecs_to_jiffies(PISUGAR3_POLL_MS));
}

static void pisugar3_external_power_changed(struct power_supply *psy)
{
	struct pisugar3 *ps = power_supply_get_drvdata(psy);

	mod_delayed_work(system_wq, &ps->work, 0);
}

static int pisugar3_battery_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct pisugar3 *ps = power_supply_get_drvdata(psy);
	struct pisugar3_reading r;

	mutex_lock(&ps->lock);
	r = ps->last;
	mutex_unlock(&ps->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = r.status;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = r.voltage_uv;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = r.capacity;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = r.temp;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		if (!ps->info || !power_supply_battery_info_has_prop(ps->info, psp))
			return -ENODATA;
		return power_supply_battery_info_get_prop(ps->info, psp, val);
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "PiSugar";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "PiSugar 3";
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property pisugar3_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc pisugar3_battery_desc = {
	.name			= "pisugar3-battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= pisugar3_battery_properties,
	.num_properties		= ARRAY_SIZE(pisugar3_battery_properties),
	.get_property		= pisugar3_battery_get_property,
	.external_power_changed	= pisugar3_external_power_changed,
};

static int pisugar3_mains_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct pisugar3 *ps = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	mutex_lock(&ps->lock);
	val->intval = ps->last.online;
	mutex_unlock(&ps->lock);

	return 0;
}

static const enum power_supply_property pisugar3_mains_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc pisugar3_mains_desc = {
	.name		= "pisugar3-mains",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.properties	= pisugar3_mains_properties,
	.num_properties	= ARRAY_SIZE(pisugar3_mains_properties),
	.get_property	= pisugar3_mains_get_property,
};

static char *pisugar3_mains_supplied_to[] = {
	"pisugar3-battery",
};

/*
 * At power-off: arm the delayed output cut, with the restart-on-power bit set
 * in the same write. SYS_OFF_MODE_POWER_OFF_PREPARE rather than POWER_OFF,
 * because the I2C transfer needs interrupts and may sleep, and the cut is
 * delayed anyway - the board takes the rail away after the machine has
 * halted, not before.
 */
static int pisugar3_power_off_prepare(struct sys_off_data *data)
{
	struct pisugar3 *ps = data->cb_data;
	struct i2c_client *client = ps->client;
	int ret;

	cancel_delayed_work_sync(&ps->work);

	/* The delay first: the countdown starts when the output bit clears. */
	ret = i2c_smbus_write_byte_data(client, PISUGAR3_REG_POWER_OFF_DELAY,
					PISUGAR3_POWER_OFF_DELAY_S);
	if (ret)
		goto err;
	ret = i2c_smbus_read_byte_data(client, PISUGAR3_REG_CTRL1);
	if (ret < 0)
		goto err;
	ret = i2c_smbus_write_byte_data(client, PISUGAR3_REG_CTRL1,
					(ret | PISUGAR3_CTRL1_RESTART_ON_POWER) &
					~PISUGAR3_CTRL1_OUTPUT_ENABLE);
	if (ret)
		goto err;

	dev_info(&client->dev,
		 "output off in %d seconds, back on when external power returns\n",
		 PISUGAR3_POWER_OFF_DELAY_S);
	return NOTIFY_DONE;

err:
	dev_err(&client->dev, "cannot arm the output cut: %d\n", ret);
	return NOTIFY_DONE;
}

static void pisugar3_firmware(struct pisugar3 *ps, char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len - 1; i++) {
		int ret = i2c_smbus_read_byte_data(ps->client,
						   PISUGAR3_REG_FIRMWARE + i);

		if (ret <= 0 || !isprint(ret))
			break;
		buf[i] = ret;
	}
	buf[i] = '\0';
}

static int pisugar3_identify(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int ret;

	ret = i2c_smbus_read_byte_data(client, PISUGAR3_REG_VERSION);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no answer at 0x%02x\n",
				     client->addr);
	if (ret != PISUGAR3_VERSION_3)
		return dev_err_probe(dev, -ENODEV,
				     "version register reads 0x%02x, not a PiSugar 3\n",
				     ret);

	ret = i2c_smbus_read_byte_data(client, PISUGAR3_REG_MODE);
	if (ret < 0)
		return ret;
	if (ret != PISUGAR3_MODE_APPLICATION)
		return dev_err_probe(dev, -ENODEV,
				     "board is not running its application firmware (mode 0x%02x)\n",
				     ret);

	return 0;
}

static void pisugar3_put_battery_info(void *data)
{
	struct pisugar3 *ps = data;

	power_supply_put_battery_info(ps->battery, ps->info);
}

static void pisugar3_stop(void *data)
{
	struct pisugar3 *ps = data;

	cancel_delayed_work_sync(&ps->work);
}

static int pisugar3_probe(struct i2c_client *client)
{
	struct power_supply_config cfg = {};
	struct device *dev = &client->dev;
	char firmware[PISUGAR3_FIRMWARE_LEN];
	struct pisugar3 *ps;
	int ret;

	ret = pisugar3_identify(client);
	if (ret)
		return ret;

	ps = devm_kzalloc(dev, sizeof(*ps), GFP_KERNEL);
	if (!ps)
		return -ENOMEM;

	ps->client = client;
	mutex_init(&ps->lock);
	INIT_DELAYED_WORK(&ps->work, pisugar3_work);
	i2c_set_clientdata(client, ps);

	cfg.drv_data = ps;
	cfg.fwnode = dev_fwnode(dev);
	ps->battery = devm_power_supply_register(dev, &pisugar3_battery_desc,
						 &cfg);
	if (IS_ERR(ps->battery))
		return dev_err_probe(dev, PTR_ERR(ps->battery),
				     "failed to register the battery\n");

	ret = power_supply_get_battery_info(ps->battery, &ps->info);
	if (ret == -ENOMEM)
		return ret;
	if (ret) {
		ps->info = NULL;
		dev_info(dev, "no monitored-battery: capacity is the board's own estimate\n");
	} else {
		ret = devm_add_action_or_reset(dev, pisugar3_put_battery_info,
					       ps);
		if (ret)
			return ret;
	}

	cfg.fwnode = NULL;
	cfg.supplied_to = pisugar3_mains_supplied_to;
	cfg.num_supplicants = ARRAY_SIZE(pisugar3_mains_supplied_to);
	ps->mains = devm_power_supply_register(dev, &pisugar3_mains_desc, &cfg);
	if (IS_ERR(ps->mains))
		return dev_err_probe(dev, PTR_ERR(ps->mains),
				     "failed to register the supply\n");

	ret = pisugar3_update(ps);
	if (ret)
		return ret;

	schedule_delayed_work(&ps->work, msecs_to_jiffies(PISUGAR3_POLL_MS));
	ret = devm_add_action_or_reset(dev, pisugar3_stop, ps);
	if (ret)
		return ret;

	if (device_property_present(dev, "system-power-controller")) {
		ret = devm_register_sys_off_handler(dev,
						    SYS_OFF_MODE_POWER_OFF_PREPARE,
						    SYS_OFF_PRIO_DEFAULT,
						    pisugar3_power_off_prepare,
						    ps);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register the power-off handler\n");
	}

	pisugar3_firmware(ps, firmware, sizeof(firmware));
	dev_info(dev, "PiSugar 3, firmware %s, %d.%03dV, %d%%%s%s\n", firmware,
		 ps->last.voltage_uv / 1000000,
		 (ps->last.voltage_uv / 1000) % 1000, ps->last.capacity,
		 ps->last.online ? ", external power" : "",
		 device_property_present(dev, "system-power-controller") ?
		 ", cuts the output at power-off" : "");

	return 0;
}

static void pisugar3_shutdown(struct i2c_client *client)
{
	struct pisugar3 *ps = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&ps->work);
}

static int pisugar3_suspend(struct device *dev)
{
	struct pisugar3 *ps = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&ps->work);

	return 0;
}

static int pisugar3_resume(struct device *dev)
{
	struct pisugar3 *ps = dev_get_drvdata(dev);

	schedule_delayed_work(&ps->work, 0);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pisugar3_pm_ops, pisugar3_suspend,
				pisugar3_resume);

static const struct of_device_id pisugar3_of_match[] = {
	{ .compatible = "pisugar,pisugar3" },
	{ }
};
MODULE_DEVICE_TABLE(of, pisugar3_of_match);

static const struct i2c_device_id pisugar3_id[] = {
	{ "pisugar3" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pisugar3_id);

static struct i2c_driver pisugar3_driver = {
	.driver = {
		.name		= "pisugar3-battery",
		.of_match_table	= pisugar3_of_match,
		.pm		= pm_sleep_ptr(&pisugar3_pm_ops),
	},
	.probe		= pisugar3_probe,
	.shutdown	= pisugar3_shutdown,
	.id_table	= pisugar3_id,
};
module_i2c_driver(pisugar3_driver);

MODULE_AUTHOR("Luca Martinetti <luca@luca.io>");
MODULE_DESCRIPTION("PiSugar 3 battery driver");
MODULE_LICENSE("GPL");
