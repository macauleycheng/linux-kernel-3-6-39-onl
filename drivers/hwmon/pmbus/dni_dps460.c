/*
 * Hardware monitoring driver for Delta DPS460
 *
 * Copyright (C) 2014 Cumulus Networks, LLC
 * Author: Puneet Shenoy <puneet@cumulusnetworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/i2c/pmbus.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include "pmbus.h"

enum chips { dni_dps460 };

/* Data provided by DELL Inc */
#define FAN_RPM_MIN 7200
#define FAN_RPM_MAX 18000
#define FAN_VALUE_MIN 0x28
#define FAN_VALUE_MAX 0x64

/* Needed to access the mutex. Copied from pmbus_core.c */
#define PB_STATUS_BASE		0
#define PB_STATUS_VOUT_BASE	(PB_STATUS_BASE + PMBUS_PAGES)
#define PB_STATUS_IOUT_BASE	(PB_STATUS_VOUT_BASE + PMBUS_PAGES)
#define PB_STATUS_FAN_BASE	(PB_STATUS_IOUT_BASE + PMBUS_PAGES)
#define PB_STATUS_FAN34_BASE	(PB_STATUS_FAN_BASE + PMBUS_PAGES)
#define PB_STATUS_TEMP_BASE	(PB_STATUS_FAN34_BASE + PMBUS_PAGES)
#define PB_STATUS_INPUT_BASE	(PB_STATUS_TEMP_BASE + PMBUS_PAGES)
#define PB_STATUS_VMON_BASE	(PB_STATUS_INPUT_BASE + 1)
#define PB_NUM_STATUS_REG	(PB_STATUS_VMON_BASE + 1)
struct pmbus_data {
	struct device *dev;
	struct device *hwmon_dev;

	u32 flags;		/* from platform data */

	int exponent[PMBUS_PAGES];
				/* linear mode: exponent for output voltages */

	const struct pmbus_driver_info *info;

	int max_attributes;
	int num_attributes;
	struct attribute_group group;
	const struct attribute_group *groups[2];

	struct pmbus_sensor *sensors;

	struct mutex update_lock;
	bool valid;
	unsigned long last_updated;	/* in jiffies */

	/*
	 * A single status register covers multiple attributes,
	 * so we keep them all together.
	 */
	u8 status[PB_NUM_STATUS_REG];
	u8 status_register;

	u8 currpage;
};

/*
 * We are only concerned with the first fan. The get_target and set_target are
 * are written accordingly.
 */
static ssize_t get_target(struct device *dev, struct device_attribute *devattr,
			  char *buf) {

	struct i2c_client *client = to_i2c_client(dev);
	struct pmbus_data *data = i2c_get_clientdata(client);
	int val;
	u32 rpm;

        /*
	 * The FAN_COMMAND_n takes a value which is not the RPM.
	 * The value and RPM have a liner relation.
	 * rpm  = (FAN_RPM_MIN/FAN_VALUE_MIN) * val
	 * The slope is (FAN_RPM_MIN/FAN_VALUE_MIN) = 180
	 */
	mutex_lock(&data->update_lock);
	val = pmbus_read_word_data(client, 0, PMBUS_FAN_COMMAND_1);
	pmbus_clear_faults(client);
	mutex_unlock(&data->update_lock);
	if (val < 0) {
		return val;
	}
	rpm = val * (FAN_RPM_MIN/FAN_VALUE_MIN);
	return sprintf(buf, "%d\n", rpm);
}

static ssize_t set_target(struct device *dev, struct device_attribute *devattr,
			  const char *buf, size_t count) {

	struct i2c_client *client = to_i2c_client(dev);
	struct pmbus_data *data = i2c_get_clientdata(client);
	int err;
	unsigned long val;
	unsigned long rpm;

	err = kstrtol(buf, 10, &rpm);
	if (err)
		return err;

	rpm = clamp_val(rpm, FAN_RPM_MIN, FAN_RPM_MAX);

	mutex_lock(&data->update_lock);

	val = FAN_VALUE_MIN * rpm;
	val /= FAN_RPM_MIN;
	pmbus_write_word_data(client, 0, PMBUS_FAN_COMMAND_1, (u16)val);
	pmbus_clear_faults(client);

	mutex_unlock(&data->update_lock);

	return count;
}

static ssize_t show_pec(struct device *dev, struct device_attribute *dummy,
			char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	return sprintf(buf, "%d\n", !!(client->flags & I2C_CLIENT_PEC));
}

static ssize_t set_pec(struct device *dev, struct device_attribute *dummy,
		       const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	long val;
	int err;

	err = strict_strtol(buf, 10, &val);
	if (err < 0)
		return err;

	if (val != 0)
		client->flags |= I2C_CLIENT_PEC;
	else
		client->flags &= ~I2C_CLIENT_PEC;

	return count;
}

static SENSOR_DEVICE_ATTR(pec, S_IWUSR | S_IRUGO, show_pec, set_pec, 0);
static SENSOR_DEVICE_ATTR(fan1_target, S_IWUSR | S_IRUGO, get_target,
			  set_target, 0);

static struct attribute *dni_dps460_attrs[] = {
	&sensor_dev_attr_fan1_target.dev_attr.attr,
	&sensor_dev_attr_pec.dev_attr.attr,
	NULL
};
static struct attribute_group dni_dps460_attr_grp = {
	.attrs = dni_dps460_attrs,
};

static int dni_dps460_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct pmbus_driver_info *info;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA |
				     I2C_FUNC_SMBUS_PEC))
		return -ENODEV;

	/* Needs PEC(PACKET ERROR CODE). Writes wont work without this. */
	client->flags = I2C_CLIENT_PEC;

	info = kzalloc(sizeof(struct pmbus_driver_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	/* Use only 1 page with 1 Fan, 2 Temps. */
	info->pages = 1;
	info->func[0] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12 |
		PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP;

	ret = pmbus_do_probe(client, id, info);
	if (ret < 0)
		goto out;

	ret = sysfs_create_group(&client->dev.kobj, &dni_dps460_attr_grp);
	if (ret)
		goto out;
	return 0;
out:
	kfree(info);
	return ret;
}

static int dni_dps460_remove(struct i2c_client *client)
{
	struct pmbus_data *data = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &dni_dps460_attr_grp);
	if (data->info)
		kfree(data->info);
	pmbus_do_remove(client);
	return 0;
}

static const struct i2c_device_id dni_dps460_id[] = {
	{"dni_dps460", dni_dps460},
	{}
};
MODULE_DEVICE_TABLE(i2c, dni_dps460_id);

static struct i2c_driver dni_dps460_driver = {
	.driver = {
		   .name = "dni_dps460",
		   },
	.probe = dni_dps460_probe,
	.remove = dni_dps460_remove,
	.id_table = dni_dps460_id,
};

module_i2c_driver(dni_dps460_driver);

MODULE_AUTHOR("Puneet Shenoy");
MODULE_DESCRIPTION("PMBus driver for Delta DPS460");
MODULE_LICENSE("GPL");
