/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/regulator/driver.h>
#include <linux/regmap.h>
#include <linux/log2.h>
#include <linux/regulator/onsemi-ncp6335d.h>
#include <linux/platform_device.h>

/* registers */
#define REG_NCP6335D_PID		0x03
#define REG_NCP6335D_FID		0x05
#define REG_NCP6335D_PROGVSEL1		0x10
#define REG_NCP6335D_PROGVSEL0		0x11
#define REG_NCP6335D_PGOOD		0x12
#define REG_NCP6335D_TIMING		0x13
#define REG_NCP6335D_COMMAND		0x14
#define REG_NCP6335D_LIMCONF		0x16

/* constraints */
#define NCP6335D_MIN_VOLTAGE_UV		600000
#define NCP6335D_STEP_VOLTAGE_UV	6250
#define NCP6335D_MIN_SLEW_NS		333
#define NCP6335D_MAX_SLEW_NS		2666
#define NCP6335D_DEF_VTG_UV		1100000

/* bits */
#define NCP6335D_ENABLE			BIT(7)
#define NCP6335D_DVS_PWM_MODE		BIT(5)
#define NCP6335D_PWM_MODE1		BIT(6)
#define NCP6335D_PWM_MODE0		BIT(7)
#define NCP6335D_PGOOD_DISCHG		BIT(4)
#define NCP6335D_SLEEP_MODE		BIT(4)

#define NCP6335D_VOUT_SEL_MASK		0x7F
#define NCP6335D_SLEW_MASK		0x18
#define NCP6335D_SLEW_SHIFT		0x3
#define NCP6335D_TSD_MASK		0x01
#define NCP6335D_TSD_VAL		0x00

#define NCP6335D_DELAY_MASK 0xE0
#define NCP6335D_DELAY_SHIFT 0x5

struct ncp6335d_info {
	struct regulator_dev *regulator;
	struct regulator_init_data *init_data;
	struct regmap *regmap;
	struct device *dev;
	unsigned int vsel_reg;
	unsigned int vsel_ctrl_val;
	unsigned int mode_bit;
	int curr_voltage;
	int slew_rate;
};

static struct ncp6335d_info *ncp6335d;

static void dump_registers(struct ncp6335d_info *dd,
			unsigned int reg, const char *func)
{
#ifdef DEBUG
	unsigned int val = 0;

	regmap_read(dd->regmap, reg, &val);
	dev_dbg(dd->dev, "%s: NCP6335D: Reg = %x, Val = %x\n", func, reg, val);
#endif
}

int ncp6335d_restart_config()
{
	int rc, set_val;

	if (!ncp6335d) {
		pr_err("%s: OnSemi NCP6335D driver not intialized\n", __func__);
		return -ENODEV;
	}

	set_val = DIV_ROUND_UP(NCP6335D_DEF_VTG_UV - NCP6335D_MIN_VOLTAGE_UV,
						NCP6335D_STEP_VOLTAGE_UV);
	rc = regmap_update_bits(ncp6335d->regmap, ncp6335d->vsel_reg,
		NCP6335D_VOUT_SEL_MASK, (set_val & NCP6335D_VOUT_SEL_MASK));
	if (rc)
		dev_err(ncp6335d->dev, "Unable to set volatge rc(%d)", rc);
	else
		udelay(20);

	return rc;
}
EXPORT_SYMBOL(ncp6335d_restart_config);

static void ncp633d_slew_delay(struct ncp6335d_info *dd,
					int prev_uV, int new_uV)
{
	u8 val;
	int delay;

	val = abs(prev_uV - new_uV) / NCP6335D_STEP_VOLTAGE_UV;
	delay =  ((val * dd->slew_rate) / 1000) + 1;

	dev_dbg(dd->dev, "Slew Delay = %d\n", delay);

	udelay(delay);
}

static int ncp6335d_enable(struct regulator_dev *rdev)
{
	int rc;
	unsigned int temp = 0;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	temp = dd->vsel_ctrl_val & ~NCP6335D_ENABLE;
	temp |= NCP6335D_ENABLE;

	rc = regmap_write(dd->regmap, dd->vsel_reg, temp);
	if (rc) {
		dev_err(dd->dev, "Unable to enable regualtor rc(%d)", rc);
		return rc;
	}

	dd->vsel_ctrl_val = temp;
	dump_registers(dd, dd->vsel_reg, __func__);

	return 0;
}

static int ncp6335d_disable(struct regulator_dev *rdev)
{
	int rc;
	unsigned int temp = 0;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	temp = dd->vsel_ctrl_val & ~NCP6335D_ENABLE;

	rc = regmap_write(dd->regmap, dd->vsel_reg, temp);
	if (rc) {
		dev_err(dd->dev, "Unable to disable regualtor rc(%d)", rc);
		return rc;
	}

	dd->vsel_ctrl_val = temp;
	dump_registers(dd, dd->vsel_reg, __func__);

	return rc;
}

static int ncp6335d_get_voltage(struct regulator_dev *rdev)
{
	unsigned int val;
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	rc = regmap_read(dd->regmap, dd->vsel_reg, &val);
	if (rc) {
		dev_err(dd->dev, "Unable to get volatge rc(%d)", rc);
		return rc;
	}
	dd->curr_voltage = ((val & NCP6335D_VOUT_SEL_MASK) *
			NCP6335D_STEP_VOLTAGE_UV) + NCP6335D_MIN_VOLTAGE_UV;

	dump_registers(dd, dd->vsel_reg, __func__);

	return dd->curr_voltage;
}

static int ncp6335d_set_voltage(struct regulator_dev *rdev,
			int min_uV, int max_uV, unsigned *selector)
{
	unsigned int temp = 0;
	int rc, set_val, new_uV;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	set_val = DIV_ROUND_UP(min_uV - NCP6335D_MIN_VOLTAGE_UV,
					NCP6335D_STEP_VOLTAGE_UV);
	new_uV = (set_val * NCP6335D_STEP_VOLTAGE_UV) +
					NCP6335D_MIN_VOLTAGE_UV;
	if (new_uV > (max_uV + NCP6335D_STEP_VOLTAGE_UV)) {
		dev_err(dd->dev, "Unable to set volatge (%d %d)\n",
							min_uV, max_uV);
		return -EINVAL;
	}

	temp = dd->vsel_ctrl_val & ~NCP6335D_VOUT_SEL_MASK;
	temp |= (set_val & NCP6335D_VOUT_SEL_MASK);

	rc = regmap_write(dd->regmap, dd->vsel_reg, temp);
	if (rc) {
		dev_err(dd->dev, "Unable to set volatge (%d %d)\n",
							min_uV, max_uV);
	} else {
		ncp633d_slew_delay(dd, dd->curr_voltage, new_uV);
		dd->curr_voltage = new_uV;
		dd->vsel_ctrl_val = temp;
	}

	dump_registers(dd, dd->vsel_reg, __func__);

	return rc;
}

static int ncp6335d_set_mode(struct regulator_dev *rdev,
					unsigned int mode)
{
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	/* only FAST and NORMAL mode types are supported */
	if (mode != REGULATOR_MODE_FAST && mode != REGULATOR_MODE_NORMAL) {
		dev_err(dd->dev, "Mode %d not supported\n", mode);
		return -EINVAL;
	}

	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND, dd->mode_bit,
			(mode == REGULATOR_MODE_FAST) ? dd->mode_bit : 0);
	if (rc) {
		dev_err(dd->dev, "Unable to set operating mode rc(%d)", rc);
		return rc;
	}

	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND,
					NCP6335D_DVS_PWM_MODE,
					(mode == REGULATOR_MODE_FAST) ?
					NCP6335D_DVS_PWM_MODE : 0);
	if (rc)
		dev_err(dd->dev, "Unable to set DVS trans. mode rc(%d)", rc);

	dump_registers(dd, REG_NCP6335D_COMMAND, __func__);

	return rc;
}

static unsigned int ncp6335d_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	rc = regmap_read(dd->regmap, REG_NCP6335D_COMMAND, &val);
	if (rc) {
		dev_err(dd->dev, "Unable to get regulator mode rc(%d)\n", rc);
		return rc;
	}

	dump_registers(dd, REG_NCP6335D_COMMAND, __func__);

	if (val & dd->mode_bit)
		return REGULATOR_MODE_FAST;

	return REGULATOR_MODE_NORMAL;
}

static struct regulator_ops ncp6335d_ops = {
	.set_voltage = ncp6335d_set_voltage,
	.get_voltage = ncp6335d_get_voltage,
	.enable = ncp6335d_enable,
	.disable = ncp6335d_disable,
	.set_mode = ncp6335d_set_mode,
	.get_mode = ncp6335d_get_mode,
};

static struct regulator_desc rdesc = {
	.name = "ncp6335d",
	.owner = THIS_MODULE,
	.n_voltages = 128,
	.ops = &ncp6335d_ops,
};

static int __devinit ncp6335d_init(struct ncp6335d_info *dd,
			const struct ncp6335d_platform_data *pdata)
{
	int rc;
	unsigned int val;

	switch (pdata->default_vsel) {
	case NCP6335D_VSEL0:
		dd->vsel_reg = REG_NCP6335D_PROGVSEL0;
		dd->mode_bit = NCP6335D_PWM_MODE0;
	break;
	case NCP6335D_VSEL1:
		dd->vsel_reg = REG_NCP6335D_PROGVSEL1;
		dd->mode_bit = NCP6335D_PWM_MODE1;
	break;
	default:
		dev_err(dd->dev, "Invalid VSEL ID %d\n", pdata->default_vsel);
		return -EINVAL;
	}

	/* get the current programmed voltage */
	rc = regmap_read(dd->regmap, dd->vsel_reg, &val);
	if (rc) {
		dev_err(dd->dev, "Unable to get volatge rc(%d)", rc);
		return rc;
	}
	dd->vsel_ctrl_val = val;
	dd->curr_voltage = ((val & NCP6335D_VOUT_SEL_MASK) *
			NCP6335D_STEP_VOLTAGE_UV) + NCP6335D_MIN_VOLTAGE_UV;

	/* set discharge */
	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_PGOOD,
					NCP6335D_PGOOD_DISCHG,
					(pdata->discharge_enable ?
					NCP6335D_PGOOD_DISCHG : 0));
	if (rc) {
		dev_err(dd->dev, "Unable to set Active Discharge rc(%d)\n", rc);
		return -EINVAL;
	}

	/* set slew rate */
	if (pdata->slew_rate_ns < NCP6335D_MIN_SLEW_NS ||
			pdata->slew_rate_ns > NCP6335D_MAX_SLEW_NS) {
		dev_err(dd->dev, "Invalid slew rate %d\n", pdata->slew_rate_ns);
		return -EINVAL;
	}
	val = DIV_ROUND_UP(pdata->slew_rate_ns, NCP6335D_MIN_SLEW_NS);
	dd->slew_rate = val * NCP6335D_MIN_SLEW_NS;
	if (val)
		val = ilog2(val);
	else
		dd->slew_rate = NCP6335D_MIN_SLEW_NS;
       #if 1
		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_TIMING,
			NCP6335D_DELAY_MASK, val << NCP6335D_DELAY_SHIFT);
		if (rc)
			dev_err(dd->dev, "Unable to set slew rate rc(%d)\n", rc);
	#endif
	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_TIMING,
			NCP6335D_SLEW_MASK, val << NCP6335D_SLEW_SHIFT);
	if (rc)
		dev_err(dd->dev, "Unable to set slew rate rc(%d)\n", rc);

	if (pdata->rearm_disable) {

		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_LIMCONF,
				NCP6335D_TSD_MASK, NCP6335D_TSD_VAL);
		if (rc)
			dev_err(dd->dev, "Unable to reset REARM bit rc(%d)\n",
					rc);
	}

	/* Set Sleep mode bit */
	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND,
				NCP6335D_SLEEP_MODE, pdata->sleep_enable ?
						NCP6335D_SLEEP_MODE : 0);
	if (rc)
		dev_err(dd->dev, "Unable to set sleep mode (%d)\n", rc);

	dump_registers(dd, REG_NCP6335D_PROGVSEL0, __func__);
	dump_registers(dd, REG_NCP6335D_PGOOD, __func__);
	dump_registers(dd, REG_NCP6335D_TIMING, __func__);
	dump_registers(dd, REG_NCP6335D_COMMAND, __func__);
	dump_registers(dd, REG_NCP6335D_LIMCONF, __func__);

	return rc;
}

static struct regmap_config ncp6335d_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int __devinit ncp6335d_regulator_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	int rc;
	unsigned int val = 0, val1 = 0;
	struct ncp6335d_info *dd;
	const struct ncp6335d_platform_data *pdata;

	pdata = client->dev.platform_data;
	if (!pdata) {
		dev_err(&client->dev, "Platform data not specified\n");
		return -EINVAL;
	}

	dd = devm_kzalloc(&client->dev, sizeof(*dd), GFP_KERNEL);
	if (!dd) {
		dev_err(&client->dev, "Unable to allocate memory\n");
		return -ENOMEM;
	}

	dd->regmap = devm_regmap_init_i2c(client, &ncp6335d_regmap_config);
	if (IS_ERR(dd->regmap)) {
		dev_err(&client->dev, "Error allocating regmap\n");
		return PTR_ERR(dd->regmap);
	}

	rc = regmap_read(dd->regmap, REG_NCP6335D_PID, &val);
	if (rc) {
		dev_err(&client->dev,
			"Unable to identify NCP6335D (PID), rc(%d)\n", rc);
		return rc;
	}

	rc = regmap_read(dd->regmap, REG_NCP6335D_FID, &val1);
	if (rc) {
		dev_err(&client->dev,
			"Unable to identify NCP6335D (FID), rc(%d)\n", rc);
		return rc;
	}
	dev_info(&client->dev,
		"Detected Regulator NCP6335D PID = %d, FID = %d\n", val, val1);

	dd->init_data = pdata->init_data;
	dd->dev = &client->dev;
	i2c_set_clientdata(client, dd);

	rc = ncp6335d_init(dd, pdata);
	if (rc) {
		dev_err(&client->dev, "Unable to intialize the regulator\n");
		return -EINVAL;
	}

	dd->regulator = regulator_register(&rdesc, &client->dev,
					dd->init_data, dd, NULL);
	if (IS_ERR(dd->regulator)) {
		dev_err(&client->dev, "Unable to register regulator rc(%ld)",
						PTR_ERR(dd->regulator));
		return PTR_ERR(dd->regulator);
	}

	ncp6335d = dd;

	return 0;
}

static int __devexit ncp6335d_regulator_remove(struct i2c_client *client)
{
	struct ncp6335d_info *dd = i2c_get_clientdata(client);

	regulator_unregister(dd->regulator);

	return 0;
}

static const struct i2c_device_id ncp6335d_id[] = {
	{"ncp6335d", -1},
	{ },
};

static struct i2c_driver ncp6335d_regulator_driver = {
	.driver = {
		.name = "ncp6335d-regulator",
	},
	.probe = ncp6335d_regulator_probe,
	.remove = __devexit_p(ncp6335d_regulator_remove),
	.id_table = ncp6335d_id,
};

static inline int ncp6335d_read(struct i2c_client* client, u8 reg)
{
	return i2c_smbus_read_byte_data(client,	reg);
}


bool vdd_cx_buck_detected = false;
EXPORT_SYMBOL(vdd_cx_buck_detected);
static int __devinit ncp6335d_pdev_probe(struct platform_device* pdev)
{
	int rc = 0;
	unsigned int val = 0;
	struct ncp6335d_i2c_platform_data* pdata = NULL;
	struct i2c_adapter *adapter = NULL;
	struct i2c_client *client = NULL;

	if(vdd_cx_buck_detected)
		return -EINVAL;

	pdata = (struct ncp6335d_i2c_platform_data*)pdev->dev.platform_data;
	if(pdata == NULL)
	{
		dev_err(&pdev->dev, "can't find the platform data\n");
		return -EINVAL;
	}

	adapter = i2c_get_adapter(pdata->i2c_bus_id);
	if (IS_ERR(adapter)) {
		dev_err(&pdev->dev, "can't get i2c adapter for mp8845c\n");
		rc = PTR_ERR(adapter);
		goto probe_failed;		
	}

	client = i2c_new_device(adapter, pdata->i2c_info);
	if (client ==NULL) {
		dev_err(&pdev->dev, "can't create i2c device for mp8845c\n");
		rc = -ENODEV;
		goto probe_failed;
	}
	
	val = ncp6335d_read(client, REG_NCP6335D_PID);
	if(val != 0x10)	//default PID = 0x10
	{
		dev_err(&client->dev, "Detected Regulator NCP6335D = %d\n", val);
		goto unreg_i2c_device;
	}

	vdd_cx_buck_detected = true;
		
	rc = i2c_add_driver(&ncp6335d_regulator_driver);
	if(rc)
	{
		dev_err(&client->dev, "add mp8845c i2c driver failed\n");
		goto unreg_i2c_device;
	}
	return rc;
	
unreg_i2c_device:	
	i2c_unregister_device(client);
probe_failed:
	return rc;

}
static int __devexit ncp6335d_pdev_remove(struct platform_device* pdev)
{
	i2c_del_driver(&ncp6335d_regulator_driver);
	return 0;
}

static struct platform_driver ncp6335d_pdev_driver = {
	.driver = {
		.name = "ncp6335d_ldo",
		.owner = THIS_MODULE,
	},
	.probe = ncp6335d_pdev_probe,
	.remove = __devexit_p(ncp6335d_pdev_remove),
};

static int __init ncp6335d_regulator_init(void)
{
	return platform_driver_register(&ncp6335d_pdev_driver);
}
fs_initcall(ncp6335d_regulator_init);

static void __exit ncp6335d_regulator_exit(void)
{
	platform_driver_unregister(&ncp6335d_pdev_driver);
}
module_exit(ncp6335d_regulator_exit);

