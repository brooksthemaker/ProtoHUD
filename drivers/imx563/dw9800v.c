// SPDX-License-Identifier: GPL-2.0
/*
 * Dongwoon DW9800V voice-coil (autofocus) driver — ProtoHUD
 *
 * 10-bit I2C VCM driver used for focus on the Waveshare IMX563 module.
 * Exposes V4L2_CID_FOCUS_ABSOLUTE on a lens subdev; the sensor DT node links
 * to it with `lens-focus = <&vcm>` so libcamera's AF algorithm drives it.
 *
 * Register map follows the DW9807/DW9800 family (the same layout used by the
 * mainline dw9807-vcm driver). TODO(verify) the SAC mode / resonance values
 * against the DW9800V datasheet and the module's actuator.
 *
 * Before using this, try the in-tree driver: the DW9800V is often
 * register-compatible with "dongwoon,dw9807-vcm". This driver exists so the
 * ringing-compensation (SAC) settings can be set from DT.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define DW9800V_REG_INFO	CCI_REG8(0x00)
#define DW9800V_REG_CONTROL	CCI_REG8(0x02)
#define DW9800V_CTL_PD		BIT(0)		/* power down */
#define DW9800V_CTL_RING	BIT(1)		/* SAC/ring control enable */
#define DW9800V_REG_POSITION	CCI_REG16(0x03)	/* 10-bit, MSB first */
#define DW9800V_REG_STATUS	CCI_REG8(0x05)
#define DW9800V_REG_MODE	CCI_REG8(0x06)	/* SAC mode select */
#define DW9800V_REG_RESONANCE	CCI_REG8(0x07)	/* SAC timing / prescale */

#define DW9800V_MAX_FOCUS	1023
#define DW9800V_FOCUS_STEP	1
#define DW9800V_DEFAULT_FOCUS	300	/* roughly hyperfocal; TODO(tune) */

/* Datasheet timing: wait after power-up before the first I2C write. */
#define DW9800V_PU_DELAY_US	1000
/* Move slowly on power-off to avoid the lens slamming ("tick" noise). */
#define DW9800V_PARK_STEP	50
#define DW9800V_PARK_DELAY_US	2000

struct dw9800v {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct regmap *regmap;
	struct regulator *vdd;
	u32 sac_mode;
	u32 sac_timing;
};

static inline struct dw9800v *to_dw9800v(struct v4l2_subdev *sd)
{
	return container_of(sd, struct dw9800v, sd);
}

static int dw9800v_set_position(struct dw9800v *vcm, u16 pos)
{
	return cci_write(vcm->regmap, DW9800V_REG_POSITION, pos, NULL);
}

static int dw9800v_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dw9800v *vcm = container_of(ctrl->handler, struct dw9800v,
					   ctrls);
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	/* Unpowered: the value is cached and applied on power-up. */
	if (pm_runtime_get_if_in_use(vcm->sd.dev) == 0)
		return 0;

	ret = dw9800v_set_position(vcm, ctrl->val);
	pm_runtime_put(vcm->sd.dev);
	return ret;
}

static const struct v4l2_ctrl_ops dw9800v_ctrl_ops = {
	.s_ctrl = dw9800v_set_ctrl,
};

/* Opening the subdev node powers the actuator (same as other VCM drivers). */
static int dw9800v_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int dw9800v_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops dw9800v_internal_ops = {
	.open = dw9800v_open,
	.close = dw9800v_close,
};

static const struct v4l2_subdev_ops dw9800v_ops = { };

static int dw9800v_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct dw9800v *vcm = to_dw9800v(sd);
	int ret = 0;

	ret = regulator_enable(vcm->vdd);
	if (ret)
		return ret;
	usleep_range(DW9800V_PU_DELAY_US, DW9800V_PU_DELAY_US + 100);

	/* Wake, enable SAC, program ringing compensation. */
	cci_write(vcm->regmap, DW9800V_REG_CONTROL, 0, &ret);
	cci_write(vcm->regmap, DW9800V_REG_CONTROL, DW9800V_CTL_RING, &ret);
	cci_write(vcm->regmap, DW9800V_REG_MODE, vcm->sac_mode, &ret);
	cci_write(vcm->regmap, DW9800V_REG_RESONANCE, vcm->sac_timing, &ret);
	if (ret) {
		dev_err(dev, "init failed (%d)\n", ret);
		regulator_disable(vcm->vdd);
		return ret;
	}

	/* Restore the last requested position. */
	return dw9800v_set_position(vcm, vcm->focus->val);
}

static int dw9800v_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct dw9800v *vcm = to_dw9800v(sd);
	int pos = vcm->focus->val;

	/* Walk the lens back to rest before cutting the coil current. */
	while (pos > 0) {
		pos = max(pos - DW9800V_PARK_STEP, 0);
		if (dw9800v_set_position(vcm, pos))
			break;
		usleep_range(DW9800V_PARK_DELAY_US,
			     DW9800V_PARK_DELAY_US + 100);
	}

	cci_write(vcm->regmap, DW9800V_REG_CONTROL, DW9800V_CTL_PD, NULL);
	return regulator_disable(vcm->vdd);
}

static int dw9800v_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct dw9800v *vcm;
	int ret;

	vcm = devm_kzalloc(dev, sizeof(*vcm), GFP_KERNEL);
	if (!vcm)
		return -ENOMEM;

	vcm->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(vcm->regmap))
		return PTR_ERR(vcm->regmap);

	vcm->vdd = devm_regulator_get(dev, "VDD");
	if (IS_ERR(vcm->vdd))
		return dev_err_probe(dev, PTR_ERR(vcm->vdd), "VDD\n");

	/* TODO(tune): defaults are the DW9807 reference values. */
	vcm->sac_mode = 0x02;		/* SAC3 */
	vcm->sac_timing = 0x60;
	of_property_read_u32(dev->of_node, "dongwoon,sac-mode",
			     &vcm->sac_mode);
	of_property_read_u32(dev->of_node, "dongwoon,sac-timing",
			     &vcm->sac_timing);

	v4l2_i2c_subdev_init(&vcm->sd, client, &dw9800v_ops);
	vcm->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	vcm->sd.internal_ops = &dw9800v_internal_ops;
	vcm->sd.entity.function = MEDIA_ENT_F_LENS;

	v4l2_ctrl_handler_init(&vcm->ctrls, 1);
	vcm->focus = v4l2_ctrl_new_std(&vcm->ctrls, &dw9800v_ctrl_ops,
				       V4L2_CID_FOCUS_ABSOLUTE, 0,
				       DW9800V_MAX_FOCUS, DW9800V_FOCUS_STEP,
				       DW9800V_DEFAULT_FOCUS);
	if (vcm->ctrls.error) {
		ret = vcm->ctrls.error;
		goto err_ctrls;
	}
	vcm->sd.ctrl_handler = &vcm->ctrls;

	ret = media_entity_pads_init(&vcm->sd.entity, 0, NULL);
	if (ret)
		goto err_ctrls;

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev(&vcm->sd);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	media_entity_cleanup(&vcm->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&vcm->ctrls);
	return ret;
}

static void dw9800v_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9800v *vcm = to_dw9800v(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		dw9800v_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	v4l2_ctrl_handler_free(&vcm->ctrls);
	media_entity_cleanup(&sd->entity);
}

static DEFINE_RUNTIME_DEV_PM_OPS(dw9800v_pm_ops, dw9800v_power_off,
				 dw9800v_power_on, NULL);

static const struct of_device_id dw9800v_of_match[] = {
	{ .compatible = "dongwoon,dw9800v" },
	{ }
};
MODULE_DEVICE_TABLE(of, dw9800v_of_match);

static struct i2c_driver dw9800v_i2c_driver = {
	.driver = {
		.name = "dw9800v",
		.of_match_table = dw9800v_of_match,
		.pm = pm_ptr(&dw9800v_pm_ops),
	},
	.probe = dw9800v_probe,
	.remove = dw9800v_remove,
};
module_i2c_driver(dw9800v_i2c_driver);

MODULE_DESCRIPTION("Dongwoon DW9800V VCM driver (ProtoHUD)");
MODULE_LICENSE("GPL");
