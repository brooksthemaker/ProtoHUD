// SPDX-License-Identifier: GPL-2.0
/*
 * AKM AK7323 optical image stabilisation controller — ProtoHUD skeleton
 *
 * The AK7323 is a closed-loop two-axis OIS driver: it reads the module's
 * Hall sensors, runs the servo, and moves the lens barrel to cancel shake.
 * The stabilisation loop itself runs inside the chip (~kHz rate); the host
 * only configures it over I2C and switches modes. That's why this is a
 * "control" driver and not a real-time one.
 *
 * STATUS: SKELETON. AKM's register map is under NDA and is not public. Every
 * register below is a named placeholder (TODO(regs)). The plumbing — power,
 * runtime PM, V4L2 controls, Hall readback for debugging — is complete so the
 * real values can be dropped in once known (datasheet from Waveshare/AKM, or
 * an I2C capture of the Waveshare USB variant enabling OIS).
 *
 * OPEN QUESTION: gyro source. Closed-loop OIS needs angular-rate data. Phone
 * modules either have a gyro inside the module wired to the AK7323's SPI
 * master, or the phone's gyro is wired to it over SPI on the flex. The
 * Waveshare 24-pin connector has no obvious room for SPI, which suggests the
 * gyro is internal. Until that's confirmed, "centering" (servo-on, lens held
 * at optical centre) is the only mode guaranteed useful — it still stops the
 * barrel rattling on a moving helmet, which matters for image quality.
 *
 * Controls (on /dev/v4l-subdevN):
 *   V4L2_CID_IMAGE_STABILIZATION  0/1 — stabilise on/off
 *   AK7323_CID_OIS_MODE           menu: Off / Centering / Still / Movie
 *   AK7323_CID_HALL_X / _Y        read-only lens position (debug)
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

/* ── TODO(regs): every address/value here is a placeholder ──────────────── */
#define AK7323_REG_ID		CCI_REG8(0x00)	/* chip/product ID */
#define AK7323_REG_MODE		CCI_REG8(0x02)	/* standby / active */
#define AK7323_MODE_STANDBY	0x40
#define AK7323_MODE_ACTIVE	0x00
#define AK7323_REG_SERVO	CCI_REG8(0x03)	/* servo + OIS enable bits */
#define AK7323_SERVO_ON		BIT(0)
#define AK7323_OIS_ON		BIT(1)
#define AK7323_REG_OIS_PROFILE	CCI_REG8(0x04)	/* still / movie tuning */
#define AK7323_REG_HALL_X	CCI_REG16(0x10)	/* signed Hall readback */
#define AK7323_REG_HALL_Y	CCI_REG16(0x12)

#define AK7323_PU_DELAY_US	5000	/* TODO(verify): boot/OTP load time */

#define AK7323_CID_BASE		(V4L2_CID_USER_BASE | 0x1d00)
#define AK7323_CID_OIS_MODE	(AK7323_CID_BASE + 0)
#define AK7323_CID_HALL_X	(AK7323_CID_BASE + 1)
#define AK7323_CID_HALL_Y	(AK7323_CID_BASE + 2)

enum ak7323_ois_mode {
	AK7323_OIS_OFF,
	AK7323_OIS_CENTERING,
	AK7323_OIS_STILL,
	AK7323_OIS_MOVIE,
};

static const char * const ak7323_mode_menu[] = {
	"Off",
	"Centering",
	"Still",
	"Movie",
};

struct ak7323 {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *stabilize;
	struct v4l2_ctrl *mode;
	struct regmap *regmap;
	struct regulator *vdd;
};

static inline struct ak7323 *to_ak7323(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ak7323, sd);
}

/* Map (enable, mode) onto the chip. */
static int ak7323_apply(struct ak7323 *ois)
{
	enum ak7323_ois_mode mode = ois->mode->val;
	u64 servo = 0;
	int ret = 0;

	if (!ois->stabilize->val && mode > AK7323_OIS_CENTERING)
		mode = AK7323_OIS_CENTERING;

	switch (mode) {
	case AK7323_OIS_OFF:
		break;
	case AK7323_OIS_CENTERING:
		servo = AK7323_SERVO_ON;
		break;
	case AK7323_OIS_STILL:
	case AK7323_OIS_MOVIE:
		servo = AK7323_SERVO_ON | AK7323_OIS_ON;
		/* TODO(regs): profile values for still vs movie tuning. */
		cci_write(ois->regmap, AK7323_REG_OIS_PROFILE,
			  mode == AK7323_OIS_MOVIE, &ret);
		break;
	}

	cci_write(ois->regmap, AK7323_REG_SERVO, servo, &ret);
	return ret;
}

static int ak7323_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ak7323 *ois = container_of(ctrl->handler, struct ak7323, ctrls);
	int ret;

	if (pm_runtime_get_if_in_use(ois->sd.dev) == 0)
		return 0;

	ret = ak7323_apply(ois);
	pm_runtime_put(ois->sd.dev);
	return ret;
}

static int ak7323_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ak7323 *ois = container_of(ctrl->handler, struct ak7323, ctrls);
	u64 val = 0;
	int ret;

	if (pm_runtime_get_if_in_use(ois->sd.dev) == 0) {
		ctrl->val = 0;
		return 0;
	}

	ret = cci_read(ois->regmap, ctrl->id == AK7323_CID_HALL_X ?
		       AK7323_REG_HALL_X : AK7323_REG_HALL_Y, &val, NULL);
	ctrl->val = (s16)val;
	pm_runtime_put(ois->sd.dev);
	return ret;
}

static const struct v4l2_ctrl_ops ak7323_ctrl_ops = {
	.s_ctrl = ak7323_s_ctrl,
	.g_volatile_ctrl = ak7323_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config ak7323_mode_cfg = {
	.ops = &ak7323_ctrl_ops,
	.id = AK7323_CID_OIS_MODE,
	.name = "OIS Mode",
	.type = V4L2_CTRL_TYPE_MENU,
	.max = ARRAY_SIZE(ak7323_mode_menu) - 1,
	.def = AK7323_OIS_CENTERING,
	.qmenu = ak7323_mode_menu,
};

#define AK7323_HALL_CFG(_id, _name)					\
	{								\
		.ops = &ak7323_ctrl_ops,				\
		.id = _id,						\
		.name = _name,						\
		.type = V4L2_CTRL_TYPE_INTEGER,				\
		.min = S16_MIN,						\
		.max = S16_MAX,						\
		.step = 1,						\
		.flags = V4L2_CTRL_FLAG_READ_ONLY |			\
			 V4L2_CTRL_FLAG_VOLATILE,			\
	}

static const struct v4l2_ctrl_config ak7323_hall_cfg[] = {
	AK7323_HALL_CFG(AK7323_CID_HALL_X, "OIS Hall X"),
	AK7323_HALL_CFG(AK7323_CID_HALL_Y, "OIS Hall Y"),
};

static int ak7323_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int ak7323_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops ak7323_internal_ops = {
	.open = ak7323_open,
	.close = ak7323_close,
};

static const struct v4l2_subdev_ops ak7323_ops = { };

static int ak7323_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ak7323 *ois = to_ak7323(sd);
	int ret;

	ret = regulator_enable(ois->vdd);
	if (ret)
		return ret;
	usleep_range(AK7323_PU_DELAY_US, AK7323_PU_DELAY_US + 500);

	ret = cci_write(ois->regmap, AK7323_REG_MODE, AK7323_MODE_ACTIVE,
			NULL);
	if (!ret)
		ret = ak7323_apply(ois);
	if (ret) {
		dev_err(dev, "init failed (%d)\n", ret);
		regulator_disable(ois->vdd);
	}

	return ret;
}

static int ak7323_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ak7323 *ois = to_ak7323(sd);

	cci_write(ois->regmap, AK7323_REG_SERVO, 0, NULL);
	cci_write(ois->regmap, AK7323_REG_MODE, AK7323_MODE_STANDBY, NULL);
	return regulator_disable(ois->vdd);
}

static int ak7323_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ak7323 *ois;
	u64 id;
	int ret;

	ois = devm_kzalloc(dev, sizeof(*ois), GFP_KERNEL);
	if (!ois)
		return -ENOMEM;

	ois->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(ois->regmap))
		return PTR_ERR(ois->regmap);

	ois->vdd = devm_regulator_get(dev, "VDD");
	if (IS_ERR(ois->vdd))
		return dev_err_probe(dev, PTR_ERR(ois->vdd), "VDD\n");

	v4l2_i2c_subdev_init(&ois->sd, client, &ak7323_ops);
	ois->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ois->sd.internal_ops = &ak7323_internal_ops;
	ois->sd.entity.function = MEDIA_ENT_F_LENS;

	v4l2_ctrl_handler_init(&ois->ctrls, 4);
	ois->stabilize = v4l2_ctrl_new_std(&ois->ctrls, &ak7323_ctrl_ops,
					   V4L2_CID_IMAGE_STABILIZATION,
					   0, 1, 1, 1);
	ois->mode = v4l2_ctrl_new_custom(&ois->ctrls, &ak7323_mode_cfg, NULL);
	v4l2_ctrl_new_custom(&ois->ctrls, &ak7323_hall_cfg[0], NULL);
	v4l2_ctrl_new_custom(&ois->ctrls, &ak7323_hall_cfg[1], NULL);
	if (ois->ctrls.error) {
		ret = ois->ctrls.error;
		goto err_ctrls;
	}
	ois->sd.ctrl_handler = &ois->ctrls;

	/* Probe the chip once so a wrong address fails loudly. */
	ret = regulator_enable(ois->vdd);
	if (ret)
		goto err_ctrls;
	usleep_range(AK7323_PU_DELAY_US, AK7323_PU_DELAY_US + 500);
	ret = cci_read(ois->regmap, AK7323_REG_ID, &id, NULL);
	regulator_disable(ois->vdd);
	if (ret) {
		dev_err_probe(dev, ret, "no response from OIS controller\n");
		goto err_ctrls;
	}
	dev_info(dev, "AK7323 id reg = 0x%02llx (skeleton driver, TODO(regs))\n",
		 id);

	ret = media_entity_pads_init(&ois->sd.entity, 0, NULL);
	if (ret)
		goto err_ctrls;

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev(&ois->sd);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	media_entity_cleanup(&ois->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&ois->ctrls);
	return ret;
}

static void ak7323_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ak7323 *ois = to_ak7323(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		ak7323_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	v4l2_ctrl_handler_free(&ois->ctrls);
	media_entity_cleanup(&sd->entity);
}

static DEFINE_RUNTIME_DEV_PM_OPS(ak7323_pm_ops, ak7323_power_off,
				 ak7323_power_on, NULL);

static const struct of_device_id ak7323_of_match[] = {
	{ .compatible = "asahi-kasei,ak7323" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak7323_of_match);

static struct i2c_driver ak7323_i2c_driver = {
	.driver = {
		.name = "ak7323",
		.of_match_table = ak7323_of_match,
		.pm = pm_ptr(&ak7323_pm_ops),
	},
	.probe = ak7323_probe,
	.remove = ak7323_remove,
};
module_i2c_driver(ak7323_i2c_driver);

MODULE_DESCRIPTION("AKM AK7323 OIS controller skeleton (ProtoHUD)");
MODULE_LICENSE("GPL");
