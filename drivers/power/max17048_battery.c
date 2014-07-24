/*
 *  max17048_battery.c
 *  fuel-gauge systems for lithium-ion (Li+) batteries
 *
 * Copyright (c) 2012-2013, NVIDIA CORPORATION.  All rights reserved.
 *  Chandler Zhang <chazhang@nvidia.com>
 *  Syed Rafiuddin <srafiuddin@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <asm/unaligned.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/max17048_battery.h>
#include <linux/jiffies.h>
#include <linux/thermal.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/platform_data/ina230.h>
#include <linux/platform_data/tegra_edp.h>
#include <generated/mach-types.h>

#define MAX17048_VCELL		0x02
#define MAX17048_SOC		0x04
#define MAX17048_VER		0x08
#define MAX17048_HIBRT		0x0A
#define MAX17048_CONFIG		0x0C
#define MAX17048_OCV		0x0E
#define MAX17048_VALRT		0x14
#define MAX17048_VRESET		0x18
#define MAX17048_STATUS		0x1A
#define MAX17048_UNLOCK		0x3E
#define MAX17048_TABLE		0x40
#define MAX17048_RCOMPSEG1	0x80
#define MAX17048_RCOMPSEG2	0x90
#define MAX17048_CMD		0xFF
#define MAX17048_UNLOCK_VALUE	0x4a57
#define MAX17048_RESET_VALUE	0x5400
#define MAX17048_DELAY      (10*HZ)
#define MAX17048_BATTERY_FULL	100
#define MAX17048_BATTERY_LOW	15
#define MAX17048_BATTERY_HOT	(60*1000)
#define MAX17048_BATTERY_COLD	(-10*1000)
#define MAX17048_VERSION_NO_11	0x11
#define MAX17048_VERSION_NO_12	0x12

/* MAX17048 ALERT interrupts */
#define MAX17048_STATUS_RI		0x0100 /* reset */
#define MAX17048_STATUS_VH		0x0200 /* voltage high */
#define MAX17048_STATUS_VL		0x0400 /* voltage low */
#define MAX17048_STATUS_VR		0x0800 /* voltage reset */
#define MAX17048_STATUS_HD		0x1000 /* SOC low  */
#define MAX17048_STATUS_SC		0x2000 /* 1% SOC change */
#define MAX17048_STATUS_ENVR		0x4000 /* enable voltage reset alert */

#define MAX17048_CONFIG_ALRT		0x0020 /* CONFIG.ALRT bit*/

/* #define DEBUG_PRINTK_SOC_VCELL */

struct max17048_chip {
	struct i2c_client		*client;
	struct delayed_work		work;
	struct power_supply		battery;
	struct max17048_platform_data *pdata;

	/* battery voltage */
	int vcell;
	/* battery capacity */
	int soc;
	/* State Of Charge */
	int status;
	/* battery health */
	int health;
	/* battery capacity */
	int capacity_level;
	/* battery temperature */
	long temperature;
	/* current threshold */
	int current_threshold;

	int internal_soc;
	int lasttime_soc;
	int lasttime_status;
	long lasttime_temperature;
	int lasttime_current_threshold;
	int shutdown_complete;
	struct mutex mutex;
};
struct max17048_chip *max17048_data;

static int max17048_write_word(struct i2c_client *client, int reg, u16 value)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&chip->mutex);
	if (chip && chip->shutdown_complete) {
		mutex_unlock(&chip->mutex);
		return -ENODEV;
	}


	ret = i2c_smbus_write_word_data(client, reg, swab16(value));

	if (ret < 0)
		dev_err(&client->dev, "%s(): Failed in writing register"
					"0x%02x err %d\n", __func__, reg, ret);

	mutex_unlock(&chip->mutex);
	return ret;
}


static int max17048_write_block(const struct i2c_client *client,
		uint8_t command, uint8_t length, const uint8_t *values)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&chip->mutex);
	if (chip && chip->shutdown_complete) {
		mutex_unlock(&chip->mutex);
		return -ENODEV;
	}

	ret = i2c_smbus_write_i2c_block_data(client, command, length, values);
	if (ret < 0)
		dev_err(&client->dev, "%s(): Failed in writing block data to"
				"0x%02x err %d\n", __func__, command, ret);
	mutex_unlock(&chip->mutex);
	return ret;
}


static int max17048_read_word(struct i2c_client *client, int reg)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&chip->mutex);
	if (chip && chip->shutdown_complete) {
		mutex_unlock(&chip->mutex);
		return -ENODEV;
	}

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0) {
		dev_err(&client->dev, "%s(): Failed in reading register"
					"0x%02x err %d\n", __func__, reg, ret);

		mutex_unlock(&chip->mutex);
		return ret;
	} else {
		ret = (int)swab16((uint16_t)(ret & 0x0000ffff));

		mutex_unlock(&chip->mutex);
		return ret;

	}
}

/* Return value in uV */
static int max17048_get_ocv(struct max17048_chip *chip)
{
	int r;
	int reg;
	int ocv;

	r = max17048_write_word(chip->client, MAX17048_UNLOCK,
			MAX17048_UNLOCK_VALUE);
	if (r)
		return r;

	reg = max17048_read_word(chip->client, MAX17048_OCV);
	ocv = (reg >> 4) * 1250;

	r = max17048_write_word(chip->client, MAX17048_UNLOCK, 0);
	WARN_ON(r);

	return ocv;
}

static int max17048_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct max17048_chip *chip = container_of(psy,
				struct max17048_chip, battery);

	switch (psp) {
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chip->status;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* unit is uV */
		val->intval = chip->vcell * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = chip->soc;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chip->health;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = chip->capacity_level;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		/* unit is uV */
		val->intval = max17048_get_ocv(chip);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		/* show 1 places of decimals, 681 means 68.1C */
		val->intval = chip->temperature / 100;
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		/* show 1 places of decimals, 681 means 68.1C */
		val->intval = chip->temperature / 100;
		break;
	default:
	return -EINVAL;
	}
	return 0;
}

static void max17048_get_vcell(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int vcell;

	vcell = max17048_read_word(client, MAX17048_VCELL);
	if (vcell < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, vcell);
	else
		chip->vcell = (uint16_t)(((vcell >> 4) * 125) / 100);

#ifdef DEBUG_PRINTK_SOC_VCELL
	dev_info(&client->dev, "%s(): VCELL %dmV\n", __func__, chip->vcell);
#endif
}

static void max17048_get_soc(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	struct max17048_battery_model *mdata = chip->pdata->model_data;
	int soc;

	soc = max17048_read_word(client, MAX17048_SOC);
	if (soc < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, soc);
	else {
		if (mdata->bits == 18)
			chip->internal_soc = (uint16_t)soc >> 8;
		else
			chip->internal_soc = (uint16_t)soc >> 9;
	}

#ifdef DEBUG_PRINTK_SOC_VCELL
	dev_info(&client->dev, "%s(): SOC %d%%\n",
			__func__, chip->internal_soc);
#endif

	chip->soc = chip->internal_soc;

	if (chip->internal_soc >= MAX17048_BATTERY_FULL) {
		if (chip->status == POWER_SUPPLY_STATUS_CHARGING)
			chip->status = POWER_SUPPLY_STATUS_FULL;
		chip->soc = MAX17048_BATTERY_FULL;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		chip->health = POWER_SUPPLY_HEALTH_GOOD;
	} else if (chip->soc < MAX17048_BATTERY_LOW) {
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_DEAD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	} else {
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_GOOD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	}
}

static void max17048_set_current_threshold(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	s32 ret;
	int min_cpu;
	int i;

	/* current threshold by SOC */
	/* current_threshold arrays should be sorted in ascending order */
	if (chip->pdata->set_current_threshold &&
		chip->pdata->current_threshold_num &&
		chip->pdata->current_normal) {

		min_cpu = 2;
		chip->current_threshold = chip->pdata->current_normal;

		for (i = 0; i < chip->pdata->current_threshold_num; i++) {
			if ((chip->internal_soc <=
				chip->pdata->current_threshold_soc[i]) &&
				chip->pdata->current_threshold[i]) {
				chip->current_threshold =
					chip->pdata->current_threshold[i];
				/* prevent current monitor power down */
				min_cpu = 1;
				break;
			}
		}

		if (chip->current_threshold !=
			chip->lasttime_current_threshold) {
			ret = chip->pdata->
				set_current_threshold(
					chip->current_threshold, min_cpu);
			if (ret < 0)
				dev_err(&client->dev,
					"%s: set current threshold err\n",
					__func__);
			else {
				dev_info(&client->dev,
					"%s(): set current threshold %d mA\n",
					__func__, chip->current_threshold);
				chip->lasttime_current_threshold =
						chip->current_threshold;
			}
		}
	}
}

static void max17048_sysedp_throttle(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int i;
	unsigned int power = ULONG_MAX;

	/* edp throttle by SOC */
	/* sysedp_throttle_power array should be sorted in ascending order */
	if (chip->pdata->sysedp_throttle) {
		for (i = 0; i < chip->pdata->sysedp_throttle_num; i++) {
			if ((chip->internal_soc <=
				chip->pdata->sysedp_throttle_soc[i]) &&
				chip->pdata->sysedp_throttle_power[i]) {
				power = chip->pdata->sysedp_throttle_power[i];
				break;
			}
		}
		chip->pdata->sysedp_throttle(power);
	}
}

static uint16_t max17048_get_version(struct i2c_client *client)
{
	return max17048_read_word(client, MAX17048_VER);
}

static int max17048_thz_match(struct thermal_zone_device *thz, void *data)
{
	return strcmp((char *)data, thz->type) == 0;
}

static int max17048_thz_get_temp(void *data, long *temp)
{
	struct thermal_zone_device *thz;

	thz = thermal_zone_device_find(data, max17048_thz_match);

	if (!thz || thz->ops->get_temp(thz, temp))
		*temp = 20000;

	return 0;
}

static void max17048_update_rcomp(struct max17048_chip *chip, long temp)
{
	struct i2c_client *client = chip->client;
	struct max17048_battery_model *mdata = chip->pdata->model_data;
	int new_rcomp;
	int ret, val;
	s64 curr_temp;
	s64 hot_temp, cold_temp;

	curr_temp = temp;
	hot_temp = mdata->t_co_hot;
	cold_temp = mdata->t_co_cold;

	hot_temp = div64_s64(
			(curr_temp - (s64)20000LL) * hot_temp,
			(s64)1000000LL);
	cold_temp = div64_s64(
			(curr_temp - (s64)20000LL) * cold_temp,
			(s64)1000000LL);

	if (temp > 20000)
		new_rcomp = mdata->rcomp + (int)hot_temp;
	else if (temp < 20000)
		new_rcomp = mdata->rcomp + (int)cold_temp;
	else
		new_rcomp = mdata->rcomp;

	if (new_rcomp > 0xFF)
		new_rcomp = 0xFF;
	else if (new_rcomp < 0)
		new_rcomp = 0;

	dev_info(&client->dev, "%s: new_rcomp %d\n", __func__, new_rcomp);

	val = max17048_read_word(client, MAX17048_CONFIG);
	if (val < 0) {
		dev_err(&client->dev,
				"%s(): Failed in reading register" \
				"MAX17048_CONFIG err %d\n",
					__func__, val);
	} else {
		/* clear upper byte */
		val &= 0xFF;
		/* Apply new Rcomp value */
		val |= (new_rcomp << 8);
		ret = max17048_write_word(client, MAX17048_CONFIG, val);
		if (ret < 0)
			dev_err(&client->dev,
				"failed set RCOMP\n");
	}
}

static void max17048_work(struct work_struct *work)
{
	struct max17048_chip *chip;
	long temp;

	chip = container_of(work, struct max17048_chip, work.work);

	if (machine_is_tegratab() || machine_is_tegranote7c()) {
		/* Use Tskin as Battery Temp */
		max17048_thz_get_temp("therm_est", &temp);

		chip->temperature = temp;
	}

	if (abs(chip->temperature - chip->lasttime_temperature) >= 1500) {
		dev_info(&chip->client->dev, "%s(): Temp %ldC\n",
				__func__, chip->temperature / 1000);
		chip->lasttime_temperature = chip->temperature;
		max17048_update_rcomp(chip, chip->temperature);
		power_supply_changed(&chip->battery);
	}

	max17048_get_vcell(chip->client);
	max17048_get_soc(chip->client);
	max17048_set_current_threshold(chip->client);
	max17048_sysedp_throttle(chip->client);

	if (chip->temperature > MAX17048_BATTERY_HOT) {
		chip->health = POWER_SUPPLY_HEALTH_OVERHEAT;
		dev_info(&chip->client->dev, "%s: BATTERY HOT, Temp %ldC\n",
				__func__, chip->temperature / 1000);
		power_supply_changed(&chip->battery);
	} else if (chip->temperature < MAX17048_BATTERY_COLD) {
		dev_info(&chip->client->dev, "%s: BATTERY COLD, Temp %ldC\n",
				__func__, chip->temperature / 1000);
		chip->health = POWER_SUPPLY_HEALTH_COLD;
		power_supply_changed(&chip->battery);
	}

	if (chip->soc != chip->lasttime_soc ||
		chip->status != chip->lasttime_status) {
		chip->lasttime_soc = chip->soc;
		power_supply_changed(&chip->battery);
	}

	schedule_delayed_work(&chip->work, MAX17048_DELAY);
}

void max17048_battery_status(int status,
				int chrg_type)
{
	if (!max17048_data)
		return;

	if (status == progress) {
		max17048_data->status = POWER_SUPPLY_STATUS_CHARGING;
	} else {
		max17048_data->status = POWER_SUPPLY_STATUS_DISCHARGING;
	}
	power_supply_changed(&max17048_data->battery);

	max17048_data->lasttime_status = max17048_data->status;
}
EXPORT_SYMBOL_GPL(max17048_battery_status);

int max17048_check_vcell(void)
{
	if (!max17048_data)
		return -1;

	return max17048_data->vcell;
}
EXPORT_SYMBOL_GPL(max17048_check_vcell);

int max17048_check_soc(void)
{
	if (!max17048_data)
		return -1;

	return max17048_data->internal_soc;
}
EXPORT_SYMBOL_GPL(max17048_check_soc);

static enum power_supply_property max17048_battery_props[] = {
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
};

static int max17048_write_rcomp_seg(struct i2c_client *client,
						uint16_t rcomp_seg)
{
	uint8_t rs1, rs2;
	int ret;
	uint8_t rcomp_seg_table[16];

	rs1 = (rcomp_seg >> 8) & 0xff;
	rs2 = rcomp_seg & 0xff;

	rcomp_seg_table[0] = rcomp_seg_table[2] = rcomp_seg_table[4] =
		rcomp_seg_table[6] = rcomp_seg_table[8] = rcomp_seg_table[10] =
			rcomp_seg_table[12] = rcomp_seg_table[14] = rs1;

	rcomp_seg_table[1] = rcomp_seg_table[3] = rcomp_seg_table[5] =
		rcomp_seg_table[7] = rcomp_seg_table[9] = rcomp_seg_table[11] =
			rcomp_seg_table[13] = rcomp_seg_table[15] = rs2;

	ret = max17048_write_block(client, MAX17048_RCOMPSEG1,
				16, (uint8_t *)rcomp_seg_table);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}

	ret = max17048_write_block(client, MAX17048_RCOMPSEG2,
				16, (uint8_t *)rcomp_seg_table);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

static int max17048_load_model_data(struct max17048_chip *chip)
{
	struct i2c_client *client = chip->client;
	struct max17048_battery_model *mdata = chip->pdata->model_data;
	uint16_t soc_tst, ocv;
	int i, ret = 0;

	/* read OCV */
	ret = max17048_read_word(client, MAX17048_OCV);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}
	ocv = (uint16_t)ret;
	if (ocv == 0xffff) {
		dev_err(&client->dev, "%s: Failed in unlocking"
					"max17048 err: %d\n", __func__, ocv);
		return -1;
	}

	/* write custom model data */
	for (i = 0; i < 4; i += 1) {
		if (max17048_write_block(client,
			(MAX17048_TABLE+i*16), 16,
				&mdata->data_tbl[i*0x10]) < 0) {
			dev_err(&client->dev, "%s: error writing model data:\n",
								__func__);
			return -1;
		}
	}

	/* Write OCV Test value */
	ret = max17048_write_word(client, MAX17048_OCV, mdata->ocvtest);
	if (ret < 0)
		return ret;

	ret = max17048_write_rcomp_seg(client, mdata->rcomp_seg);
	if (ret < 0)
		return ret;

	/* Disable hibernate */
	ret = max17048_write_word(client, MAX17048_HIBRT, 0x0000);
	if (ret < 0)
		return ret;

	/* Lock model access */
	ret = max17048_write_word(client, MAX17048_UNLOCK, 0x0000);
	if (ret < 0)
		return ret;

	/* Delay between 150ms to 600ms */
	mdelay(200);

	/* Read SOC Register and compare to expected result */
	ret = max17048_read_word(client, MAX17048_SOC);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}
	soc_tst = (uint16_t)ret;
	if (!((soc_tst >> 8) >= mdata->soccheck_A &&
				(soc_tst >> 8) <=  mdata->soccheck_B)) {
		dev_err(&client->dev, "%s: soc comparison failed %d\n",
					__func__, ret);
		return ret;
	} else {
		dev_info(&client->dev, "MAX17048 Custom data"
						" loading successfull\n");
	}

	/* unlock model access */
	ret = max17048_write_word(client, MAX17048_UNLOCK,
					MAX17048_UNLOCK_VALUE);
	if (ret < 0)
		return ret;

	/* Restore OCV */
	ret = max17048_write_word(client, MAX17048_OCV, ocv);
	if (ret < 0)
		return ret;

	return ret;
}

static int max17048_initialize(struct max17048_chip *chip)
{
	uint8_t ret;
	uint8_t config = 0;
	struct i2c_client *client = chip->client;
	struct max17048_battery_model *mdata = chip->pdata->model_data;

	/* unlock model access */
	ret = max17048_write_word(client, MAX17048_UNLOCK,
			MAX17048_UNLOCK_VALUE);
	if (ret < 0)
		return ret;

	/* load model data */
	ret = max17048_load_model_data(chip);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}

	if (mdata->bits == 19)
		config = 32 - (mdata->alert_threshold * 2);
	else if (mdata->bits == 18)
		config = 32 - mdata->alert_threshold;
	else
		dev_info(&client->dev, "Alert bit not set!");
	config = mdata->one_percent_alerts | config;

	ret = max17048_write_word(client, MAX17048_CONFIG,
			((mdata->rcomp << 8) | config));
	if (ret < 0)
		return ret;

	/* Voltage Alert configuration */
	ret = max17048_write_word(client, MAX17048_VALRT, mdata->valert);
	if (ret < 0)
		return ret;

	ret = max17048_write_word(client, MAX17048_VRESET, mdata->vreset);
	if (ret < 0)
		return ret;

	/* Lock model access */
	ret = max17048_write_word(client, MAX17048_UNLOCK, 0x0000);
	if (ret < 0)
		return ret;

	/* Add delay */
	mdelay(200);
	return 0;
}

int max17048_check_battery()
{
	uint16_t version;

	if (!max17048_data)
		return -ENODEV;

	version = max17048_get_version(max17048_data->client);
	if ((version != MAX17048_VERSION_NO_11) &&
		(version != MAX17048_VERSION_NO_12)) {
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max17048_check_battery);

static irqreturn_t max17048_irq(int id, void *dev)
{
	struct max17048_chip *chip = dev;
	struct i2c_client *client = chip->client;
	u16 val;
	u16 valrt;
	int ret;
	struct max17048_battery_model *mdata = chip->pdata->model_data;

	val = max17048_read_word(client, MAX17048_STATUS);
	if (val < 0) {
		dev_err(&client->dev,
				"%s(): Failed in reading register" \
				"MAX17048_STATUS err %d\n",
					__func__, val);
		goto clear_irq;
	}

	if (val & MAX17048_STATUS_RI)
		dev_info(&client->dev, "%s(): STATUS_RI\n", __func__);
	if (val & MAX17048_STATUS_VH)
		dev_info(&client->dev, "%s(): STATUS_VH\n", __func__);
	if (val & MAX17048_STATUS_VL) {
		dev_info(&client->dev, "%s(): STATUS_VL\n", __func__);
		/* Forced set SOC to 0 for power off */
		chip->soc = 0;
		chip->lasttime_soc = chip->soc;
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_DEAD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		power_supply_changed(&chip->battery);

		/* Clear VL for prevent continuous irq */
		valrt = mdata->valert & 0x00FF;
		ret = max17048_write_word(client, MAX17048_VALRT,
				valrt);
		if (ret < 0)
			dev_err(&client->dev, "failed write MAX17048_VALRT\n");
	}
	if (val & MAX17048_STATUS_VR)
		dev_info(&client->dev, "%s(): STATUS_VR\n", __func__);
	if (val & MAX17048_STATUS_HD) {
		max17048_get_vcell(client);
		max17048_get_soc(client);
		chip->lasttime_soc = chip->soc;
		dev_info(&client->dev,
				"%s(): STATUS_HD, VCELL %dmV, SOC %d%%\n",
				__func__, chip->vcell, chip->internal_soc);
		power_supply_changed(&chip->battery);
	}
	if (val & MAX17048_STATUS_SC) {
		max17048_get_vcell(client);
		max17048_get_soc(client);
		max17048_set_current_threshold(client);
		max17048_sysedp_throttle(client);

		chip->lasttime_soc = chip->soc;
		dev_info(&client->dev,
				"%s(): STATUS_SC, VCELL %dmV, SOC %d%%\n",
				__func__, chip->vcell, chip->internal_soc);
		power_supply_changed(&chip->battery);

		/* Set VL again when soc is above 1% */
		if (chip->internal_soc >= 1) {
			ret = max17048_write_word(client, MAX17048_VALRT,
					mdata->valert);
			if (ret < 0)
				dev_err(&client->dev,
					"failed write MAX17048_VALRT\n");
		}
	}
	if (val & MAX17048_STATUS_ENVR)
		dev_info(&client->dev, "%s(): STATUS_ENVR\n", __func__);

	ret = max17048_write_word(client, MAX17048_STATUS, 0x0000);
	if (ret < 0)
		dev_err(&client->dev, "failed clear STATUS\n");

clear_irq:
	val = max17048_read_word(client, MAX17048_CONFIG);
	if (val < 0) {
		dev_err(&client->dev,
				"%s(): Failed in reading register" \
				"MAX17048_CONFIG err %d\n",
					__func__, val);
		return IRQ_HANDLED;
	}
	val &= ~(MAX17048_CONFIG_ALRT);
	ret = max17048_write_word(client, MAX17048_CONFIG, val);
	if (ret < 0)
		dev_err(&client->dev, "failed clear CONFIG.ALRT\n");

	return IRQ_HANDLED;
}

#ifdef CONFIG_OF
static struct max17048_platform_data *max17048_parse_dt(struct device *dev)
{
	struct max17048_platform_data *pdata;
	struct max17048_battery_model *model_data;
	struct device_node *np = dev->of_node;
	u32 val, val_array[MAX17048_DATA_SIZE];
	u32 soc_array[MAX17048_MAX_SOC_STEP];
	const char *str;
	int i, ret;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	model_data = devm_kzalloc(dev, sizeof(*model_data), GFP_KERNEL);
	if (!model_data)
		return ERR_PTR(-ENOMEM);

	pdata->model_data = model_data;

	ret = of_property_read_u32(np, "bits", &val);
	if (ret < 0)
		return ERR_PTR(ret);

	if ((val == 18) || (val == 19))
		model_data->bits = val;

	ret = of_property_read_u32(np, "alert-threshold", &val);
	if (ret < 0)
		return ERR_PTR(ret);

	model_data->alert_threshold = val;

	ret = of_property_read_u32(np, "one-percent-alerts", &val);
	if (ret < 0)
		return ERR_PTR(ret);

	if (val)
		model_data->one_percent_alerts = 0x40;

	ret = of_property_read_u32(np, "valert-max", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->valert = (val / 20) & 0xFF; /* LSB is 20mV. */

	ret = of_property_read_u32(np, "valert-min", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->valert |= ((val / 20) & 0xFF) << 8; /* LSB is 20mV. */

	ret = of_property_read_u32(np, "vreset-threshold", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->vreset = ((val / 40) & 0xFE) << 8; /* LSB is 40mV. */

	ret = of_property_read_u32(np, "vreset-disable", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->vreset |= (val & 0x01) << 8;

	ret = of_property_read_u32(np, "hib-threshold", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->hibernate = (val & 0xFF) << 8;

	ret = of_property_read_u32(np, "hib-active-threshold", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->hibernate |= val & 0xFF;

	ret = of_property_read_u32(np, "rcomp", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->rcomp = val;

	ret = of_property_read_u32(np, "rcomp-seg", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->rcomp_seg = val;

	ret = of_property_read_u32(np, "soccheck-a", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->soccheck_A = val;

	ret = of_property_read_u32(np, "soccheck-b", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->soccheck_B = val;

	ret = of_property_read_u32(np, "ocvtest", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->ocvtest = val;

	ret = of_property_read_u32(np, "minus_t_co_hot", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->t_co_hot = -1 * val;

	ret = of_property_read_u32(np, "minus_t_co_cold", &val);
	if (ret < 0)
		return ERR_PTR(ret);
	model_data->t_co_cold = -1 * val;

	ret = of_property_read_u32_array(np, "data-tbl", val_array,
					 MAX17048_DATA_SIZE);
	if (ret < 0)
		return ERR_PTR(ret);

	for (i = 0; i < MAX17048_DATA_SIZE; i++)
		model_data->data_tbl[i] = val_array[i];

	ret = of_property_read_u32(np, "read_batt_id", &val);
	if (ret < 0)
		pdata->read_batt_id = 0;
	else
		pdata->read_batt_id = val;

	if ((!of_property_read_string(np, "set_current_threshold", &str)) &&
		(!strncmp(str, "ina230", strlen(str)))) {
		pdata->set_current_threshold = ina230_set_current_threshold;
	} else {
		pdata->set_current_threshold = NULL;
	}

	ret = of_property_read_u32(np, "current_normal", &val);
	if (ret < 0)
		pdata->current_normal = 0;
	else
		pdata->current_normal = val;

	ret = of_property_read_u32(np, "current_threshold_num", &val);
	if (ret < 0)
		pdata->current_threshold_num = 0;
	else
		pdata->current_threshold_num = val;

	if (pdata->current_threshold_num > MAX17048_MAX_SOC_STEP)
		pdata->current_threshold_num = MAX17048_MAX_SOC_STEP;

	if (pdata->set_current_threshold != NULL &&
		pdata->current_normal &&
		pdata->current_threshold_num) {
		ret = of_property_read_u32_array(np, "current_threshold_soc",
				soc_array, pdata->current_threshold_num);
		if (ret < 0)
			return ERR_PTR(ret);

		for (i = 0; i < pdata->current_threshold_num; i++)
			pdata->current_threshold_soc[i] = soc_array[i];

		ret = of_property_read_u32_array(np, "current_threshold",
				soc_array, pdata->current_threshold_num);
		if (ret < 0)
			return ERR_PTR(ret);

		for (i = 0; i < pdata->current_threshold_num; i++)
			pdata->current_threshold[i] = soc_array[i];
	}

	if ((!of_property_read_string(np, "sysedp_throttle", &str)) &&
		(!strncmp(str, "sysedp_lite", strlen(str)))) {
		pdata->sysedp_throttle = sysedp_lite_throttle;
	} else {
		pdata->sysedp_throttle = NULL;
	}

	ret = of_property_read_u32(np, "sysedp_throttle_num", &val);
	if (ret < 0)
		pdata->sysedp_throttle_num = 0;
	else
		pdata->sysedp_throttle_num = val;

	if (pdata->sysedp_throttle_num > MAX17048_MAX_SOC_STEP)
		pdata->sysedp_throttle_num = MAX17048_MAX_SOC_STEP;

	if (pdata->sysedp_throttle != NULL && pdata->sysedp_throttle_num) {
		ret = of_property_read_u32_array(np, "sysedp_throttle_soc",
					soc_array, pdata->sysedp_throttle_num);
		if (ret < 0)
			return ERR_PTR(ret);

		for (i = 0; i < pdata->sysedp_throttle_num; i++)
			pdata->sysedp_throttle_soc[i] = soc_array[i];

		ret = of_property_read_u32_array(np, "sysedp_throttle_power",
					soc_array, pdata->sysedp_throttle_num);
		if (ret < 0)
			return ERR_PTR(ret);

		for (i = 0; i < pdata->sysedp_throttle_num; i++)
			pdata->sysedp_throttle_power[i] = soc_array[i];
	}
	return pdata;
}
#else
static struct max17048_platform_data *max17048_parse_dt(struct device *dev)
{
	return NULL;
}
#endif /* CONFIG_OF */

static s32 show_battery_capacity(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct iio_channel *channel;
	int val, val2 = 0;
	int ret;
	struct i2c_client *client;
	int capacity = 0;

	if (!max17048_data)
		return 0;

	client = max17048_data->client;

	channel = iio_st_channel_get(dev_name(&client->dev),
				"batt_id");
	if (IS_ERR(channel)) {
		dev_err(&client->dev,
			"%s: Failed to get channel batt_id, %ld\n",
			__func__, PTR_ERR(channel));
		return 0;
	}

	ret = iio_st_read_channel_raw(channel, &val, &val2);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: Failed to read channel, %d\n",
			__func__, ret);
		return 0;
	}

	if (val > 3300) { /* over 200Kohm*/
		dev_info(&client->dev, "adc: %d, No battery\n", val);
		capacity = 0;
	} else if (val > 819) { /* over 50Kohm*/
		dev_info(&client->dev, "adc: %d, 3200mA Battery\n", val);
		capacity = 3200;
	} else {
		dev_info(&client->dev, "adc: %d, 4100mA Battery\n", val);
		capacity = 4100;
	}
	return sprintf(buf, "%d\n", capacity);
}

static s32 store_battery_capacity(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	return 0;
}

static struct device_attribute max17048_attrs[] = {
	__ATTR(battery_capacity, 0644,
		show_battery_capacity, store_battery_capacity),
};

static int __devinit max17048_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct max17048_chip *chip;
	int ret;
	uint16_t version;
	u16 val;
	int i;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;

	if (client->dev.of_node) {
		chip->pdata = max17048_parse_dt(&client->dev);
		if (IS_ERR(chip->pdata))
			return PTR_ERR(chip->pdata);
	} else {
		chip->pdata = client->dev.platform_data;
		if (!chip->pdata)
			return -ENODATA;
	}

	max17048_data = chip;
	mutex_init(&chip->mutex);
	chip->shutdown_complete = 0;
	i2c_set_clientdata(client, chip);

	version = max17048_get_version(client);
	dev_info(&client->dev, "MAX17048 Fuel-Gauge Ver 0x%x\n", version);

	ret = max17048_initialize(chip);
	if (ret < 0) {
		dev_err(&client->dev, "Error: Initializing fuel-gauge\n");
		goto error2;
	}

	chip->battery.name		= "battery";
	chip->battery.type		= POWER_SUPPLY_TYPE_BATTERY;
	chip->battery.get_property	= max17048_get_property;
	chip->battery.properties	= max17048_battery_props;
	chip->battery.num_properties	= ARRAY_SIZE(max17048_battery_props);
	chip->status			= POWER_SUPPLY_STATUS_DISCHARGING;
	chip->lasttime_status   = POWER_SUPPLY_STATUS_DISCHARGING;

	if (chip->pdata->current_normal) {
		chip->current_threshold = chip->pdata->current_normal;
		chip->lasttime_current_threshold = chip->pdata->current_normal;
	}

	ret = power_supply_register(&client->dev, &chip->battery);
	if (ret) {
		dev_err(&client->dev, "failed: power supply register\n");
		goto error2;
	}

	INIT_DELAYED_WORK_DEFERRABLE(&chip->work, max17048_work);
	schedule_delayed_work(&chip->work, 0);

	if (client->irq) {
		ret = request_threaded_irq(client->irq, NULL,
						max17048_irq,
						IRQF_TRIGGER_FALLING,
						chip->battery.name, chip);
		if (!ret) {
			ret = max17048_write_word(client, MAX17048_STATUS,
							0x0000);
			if (ret < 0)
				goto irq_clear_error;
			val = max17048_read_word(client, MAX17048_CONFIG);
			if (val < 0)
				goto irq_clear_error;
			val &= ~(MAX17048_CONFIG_ALRT);
			ret = max17048_write_word(client, MAX17048_CONFIG,
							val);
			if (ret < 0)
				goto irq_clear_error;
		} else {
			dev_err(&client->dev,
					"%s: request IRQ %d fail, err = %d\n",
					__func__, client->irq, ret);
			client->irq = 0;
			goto irq_reg_error;
		}
	}
	device_set_wakeup_capable(&client->dev, 1);

	if (chip->pdata->read_batt_id) {
		/* create sysfs node */
		for (i = 0; i < ARRAY_SIZE(max17048_attrs); i++) {
			ret = device_create_file(&client->dev,
							&max17048_attrs[i]);
			if (ret) {
				dev_err(&client->dev,
				"%s: device_create_file failed(%d)\n",
				__func__, ret);
				goto file_error;
			}
		}
	}

	return 0;
file_error:
	for (i = 0; i < ARRAY_SIZE(max17048_attrs); i++)
		device_remove_file(&client->dev, &max17048_attrs[i]);
irq_clear_error:
	free_irq(client->irq, chip);
irq_reg_error:
	cancel_delayed_work_sync(&chip->work);
	power_supply_unregister(&chip->battery);
error2:
	mutex_destroy(&chip->mutex);

	return ret;
}

static int __devexit max17048_remove(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int i;

	if (client->irq)
		free_irq(client->irq, chip);
	power_supply_unregister(&chip->battery);
	cancel_delayed_work_sync(&chip->work);
	for (i = 0; i < ARRAY_SIZE(max17048_attrs); i++)
		device_remove_file(&client->dev, &max17048_attrs[i]);
	mutex_destroy(&chip->mutex);

	return 0;
}

static void max17048_shutdown(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	struct max17048_battery_model *mdata = chip->pdata->model_data;
	int ret, val;

	/* reset RCOMP to default value */
	val = max17048_read_word(client, MAX17048_CONFIG);
	if (val < 0) {
		dev_err(&client->dev,
				"%s(): Failed in reading register" \
				"MAX17048_CONFIG err %d\n",
					__func__, val);
	} else {
		/* clear upper byte */
		val &= 0xFF;
		/* Apply defaut Rcomp value */
		val |= (mdata->rcomp << 8);
		ret = max17048_write_word(client, MAX17048_CONFIG, val);
		if (ret < 0)
			dev_err(&client->dev,
				"failed set RCOMP\n");
	}

	if (client->irq)
		disable_irq(client->irq);
	cancel_delayed_work_sync(&chip->work);
	mutex_lock(&chip->mutex);
	chip->shutdown_complete = 1;
	mutex_unlock(&chip->mutex);

}

#ifdef CONFIG_PM

static int max17048_suspend(struct i2c_client *client,
		pm_message_t state)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;
	struct max17048_battery_model *mdata = chip->pdata->model_data;
	u16 val;

	/* clear CONFIG.ALSC */
	if (mdata->one_percent_alerts) {
		val = max17048_read_word(client, MAX17048_CONFIG);
		if (val < 0) {
			dev_err(&client->dev,
					"%s(): Failed in reading register" \
					"MAX17048_CONFIG err %d\n",
						__func__, val);
		} else {
			val &= ~(mdata->one_percent_alerts);
			ret = max17048_write_word(client, MAX17048_CONFIG, val);
			if (ret < 0)
				dev_err(&client->dev,
					"failed clear CONFIG.ALSC\n");
		}
	}

	if (device_may_wakeup(&client->dev)) {
		enable_irq_wake(chip->client->irq);
	}
	cancel_delayed_work_sync(&chip->work);
	ret = max17048_write_word(client, MAX17048_HIBRT, 0xffff);
	if (ret < 0) {
		dev_err(&client->dev, "failed in entering hibernate mode\n");
		return ret;
	}

	return 0;
}

static int max17048_resume(struct i2c_client *client)
{
	struct max17048_chip *chip = i2c_get_clientdata(client);
	int ret;
	struct max17048_battery_model *mdata = chip->pdata->model_data;
	u16 val;

	ret = max17048_write_word(client, MAX17048_HIBRT, mdata->hibernate);
	if (ret < 0) {
		dev_err(&client->dev, "failed in exiting hibernate mode\n");
		return ret;
	}

	schedule_delayed_work(&chip->work, MAX17048_DELAY);
	if (device_may_wakeup(&client->dev)) {
		disable_irq_wake(client->irq);
	}

	/* set CONFIG.ALSC */
	if (mdata->one_percent_alerts) {
		val = max17048_read_word(client, MAX17048_CONFIG);
		if (val < 0) {
			dev_err(&client->dev,
					"%s(): Failed in reading register" \
					"MAX17048_CONFIG err %d\n",
						__func__, val);
		} else {
			val |= mdata->one_percent_alerts;
			ret = max17048_write_word(client, MAX17048_CONFIG, val);
			if (ret < 0)
				dev_err(&client->dev,
					"failed set CONFIG.ALSC\n");
		}
	}

	return 0;
}

#else

#define max17048_suspend NULL
#define max17048_resume NULL

#endif /* CONFIG_PM */

#ifdef CONFIG_OF
static const struct of_device_id max17048_dt_match[] = {
	{ .compatible = "maxim,max17048" },
	{ },
};
MODULE_DEVICE_TABLE(of, max17048_dt_match);
#endif

static const struct i2c_device_id max17048_id[] = {
	{ "max17048", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max17048_id);

static struct i2c_driver max17048_i2c_driver = {
	.driver	= {
		.name	= "max17048",
		.of_match_table = of_match_ptr(max17048_dt_match),
	},
	.probe		= max17048_probe,
	.remove		= __devexit_p(max17048_remove),
	.suspend	= max17048_suspend,
	.resume		= max17048_resume,
	.id_table	= max17048_id,
	.shutdown	= max17048_shutdown,
};

static int __init max17048_init(void)
{
	return i2c_add_driver(&max17048_i2c_driver);
}
subsys_initcall(max17048_init);

static void __exit max17048_exit(void)
{
	i2c_del_driver(&max17048_i2c_driver);
}
module_exit(max17048_exit);

MODULE_AUTHOR("Chandler Zhang <chazhang@nvidia.com>");
MODULE_DESCRIPTION("MAX17048 Fuel Gauge");
MODULE_LICENSE("GPL");
