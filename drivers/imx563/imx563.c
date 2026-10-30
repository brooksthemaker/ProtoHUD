// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX563 V4L2 sensor driver — ProtoHUD bring-up framework
 *
 * Target: Waveshare "IMX563 12MP OIS Camera Module" (4-lane MIPI CSI-2,
 * 24-pin B2B, DW9800V VCM, AKM AK7323 OIS) on a Raspberry Pi 5 / CM5
 * (rp1-cfe receiver + PiSP ISP via libcamera).
 *
 * STATUS: FRAMEWORK. Power sequencing, chip-ID probe, V4L2 control plumbing,
 * format/crop negotiation and runtime PM are complete. The register values
 * that make the sensor actually stream (PLL config, vendor "magic" init
 * block, per-mode timing) are NOT public and are marked TODO(regs). See
 * docs/imx563-ois-bringup.md for how to obtain them (Waveshare datasheet,
 * I2C capture from the USB variant, vendor Android kernel trees) and
 * tools/i2c_log_to_regs.py to convert a capture into the tables below.
 *
 * Register layout follows the SMIA++/MIPI-CCS-style map that every Sony
 * mobile sensor of this generation uses (IMX477/519/586/708...). The
 * addresses below are those standard ones; verify against the datasheet.
 *
 * Structure is modelled on the Raspberry Pi kernel's imx477/imx708 drivers.
 * Written against the v6.12 media API (rpi-6.12.y); builds out-of-tree.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

/* ── Standard Sony/CCS registers ─────────────────────────────────────────── */
#define IMX563_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX563_CHIP_ID			0x0563	/* TODO(verify): read on HW */

#define IMX563_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX563_MODE_STANDBY		0x00
#define IMX563_MODE_STREAMING		0x01

#define IMX563_REG_ORIENTATION		CCI_REG8(0x0101)	/* b0 H, b1 V */
#define IMX563_REG_SW_RESET		CCI_REG8(0x0103)
#define IMX563_REG_GROUP_HOLD		CCI_REG8(0x0104)

#define IMX563_REG_CSI_FORMAT		CCI_REG16(0x0112)	/* 0x0a0a=RAW10 */
#define IMX563_REG_CSI_LANE_MODE	CCI_REG8(0x0114)	/* lanes - 1 */
#define IMX563_REG_EXCK_FREQ		CCI_REG16(0x0136)	/* MHz, 8.8 fixed */

#define IMX563_REG_FRAME_LENGTH		CCI_REG16(0x0340)
#define IMX563_FRAME_LENGTH_MAX		0xffff
#define IMX563_REG_LINE_LENGTH		CCI_REG16(0x0342)

#define IMX563_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX563_EXPOSURE_MIN		8	/* TODO(verify) */
#define IMX563_EXPOSURE_STEP		1
#define IMX563_EXPOSURE_OFFSET		22	/* frame_length - max coarse; TODO */

#define IMX563_REG_ANALOG_GAIN		CCI_REG16(0x0204)
/* Sony code: gain = 1024 / (1024 - code). 978 ~= 22.3x. TODO(verify max) */
#define IMX563_ANA_GAIN_MIN		0
#define IMX563_ANA_GAIN_MAX		978
#define IMX563_ANA_GAIN_STEP		1
#define IMX563_ANA_GAIN_DEFAULT		0

#define IMX563_REG_DIGITAL_GAIN		CCI_REG16(0x020e)	/* 8.8, 0x100=1x */
#define IMX563_DGTL_GAIN_MIN		0x0100
#define IMX563_DGTL_GAIN_MAX		0x0fff
#define IMX563_DGTL_GAIN_DEFAULT	0x0100
#define IMX563_DGTL_GAIN_STEP		1

#define IMX563_REG_TEST_PATTERN		CCI_REG16(0x0600)
#define IMX563_REG_TP_R			CCI_REG16(0x0602)
#define IMX563_REG_TP_GR		CCI_REG16(0x0604)
#define IMX563_REG_TP_B			CCI_REG16(0x0606)
#define IMX563_REG_TP_GB		CCI_REG16(0x0608)
#define IMX563_TP_COLOUR_MAX		0x03ff

/* ── Sensor geometry (pixel array) — TODO(verify) against datasheet ─────── */
/*
 * Waveshare's docs say 4032x3024 effective; the product page says 4046x3344.
 * The driver uses 4032x3024 active until the datasheet settles it.
 */
#define IMX563_NATIVE_WIDTH		4064U
#define IMX563_NATIVE_HEIGHT		3056U
#define IMX563_PIXEL_ARRAY_LEFT		16U
#define IMX563_PIXEL_ARRAY_TOP		16U
#define IMX563_PIXEL_ARRAY_WIDTH	4032U
#define IMX563_PIXEL_ARRAY_HEIGHT	3024U

/* ── Clocks / link ───────────────────────────────────────────────────────── */
#define IMX563_XCLK_FREQ		24000000	/* module needs a 24 MHz INCK */
#define IMX563_NUM_LANES		4
/*
 * TODO(regs): both values come from the PLL setup in imx563_common_regs.
 * 600 MHz DDR = 1.2 Gbps/lane = 4.8 Gbps total, comfortably inside the
 * Pi 5 receiver and enough for full-res RAW10 at ~30 fps.
 */
#define IMX563_DEFAULT_LINK_FREQ	600000000ULL
#define IMX563_PIXEL_RATE		(IMX563_DEFAULT_LINK_FREQ * 2 * \
					 IMX563_NUM_LANES / 10)

/* Power-on timings. TODO(verify) — typical Sony values. */
#define IMX563_XCLR_MIN_DELAY_US	6200
#define IMX563_XCLR_DELAY_RANGE_US	1000

static const char * const imx563_supply_name[] = {
	"vana",		/* 2.8 V analog  (AVDD)  */
	"vdig",		/* ~1.1 V core   (DVDD)  */
	"vddl",		/* 1.8 V I/O     (OVDD)  */
};
#define IMX563_NUM_SUPPLIES ARRAY_SIZE(imx563_supply_name)

static const s64 imx563_link_freq_menu[] = {
	IMX563_DEFAULT_LINK_FREQ,
};

/* Bayer order as a function of [vflip][hflip]. TODO(verify) native = RGGB */
static const u32 imx563_mbus_codes[2][2] = {
	{ MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10 },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10 },
};

static const char * const imx563_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Colour Bars",
	"Grey Colour Bars",
	"PN9",
};

static const int imx563_test_pattern_val[] = {
	0x0000, 0x0001, 0x0002, 0x0003, 0x0004,
};

struct imx563_mode {
	u32 width;
	u32 height;
	struct v4l2_rect crop;		/* analog crop on the pixel array */
	u32 line_length_pix;		/* HTS in pixels (reg 0x0342) */
	u32 frame_length_default;	/* VTS default (reg 0x0340) */
	u32 frame_length_min;		/* VTS for max fps */
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

/*
 * TODO(regs): vendor init block. Written once after power-up, before any
 * mode. This is the NDA part — on IMX477 it is ~150 writes in 0x3xxx-0xExxx.
 * Paste the output of tools/i2c_log_to_regs.py here.
 */
static const struct cci_reg_sequence imx563_common_regs[] = {
	{ IMX563_REG_EXCK_FREQ,		0x1800 },	/* 24.00 MHz */
	{ IMX563_REG_CSI_LANE_MODE,	IMX563_NUM_LANES - 1 },
	{ IMX563_REG_CSI_FORMAT,	0x0a0a },	/* RAW10 in, RAW10 out */
	/* PLL — TODO(regs): fill from capture/datasheet.
	 * { CCI_REG8(0x0301), .. },  IVTPXCK_DIV
	 * { CCI_REG8(0x0303), .. },  IVTSYCK_DIV
	 * { CCI_REG8(0x0305), .. },  IVT_PREPLLCK_DIV
	 * { CCI_REG16(0x0306), .. }, IVT_PLL_MPY
	 * { CCI_REG8(0x0309), .. },  IOPPXCK_DIV
	 * { CCI_REG8(0x030b), .. },  IOPSYCK_DIV
	 * { CCI_REG8(0x030d), .. },  IOP_PREPLLCK_DIV
	 * { CCI_REG16(0x030e), .. }, IOP_PLL_MPY
	 */
};

/* Full resolution 4032x3024 — TODO(regs) timing values are estimates. */
static const struct cci_reg_sequence imx563_mode_4032x3024_regs[] = {
	{ CCI_REG16(0x0344), 0 },		/* x_addr_start */
	{ CCI_REG16(0x0346), 0 },		/* y_addr_start */
	{ CCI_REG16(0x0348), 4031 },		/* x_addr_end */
	{ CCI_REG16(0x034a), 3023 },		/* y_addr_end */
	{ CCI_REG16(0x034c), 4032 },		/* x_output_size */
	{ CCI_REG16(0x034e), 3024 },		/* y_output_size */
	{ CCI_REG8(0x0381), 1 },		/* x_even_inc */
	{ CCI_REG8(0x0383), 1 },		/* x_odd_inc */
	{ CCI_REG8(0x0385), 1 },		/* y_even_inc */
	{ CCI_REG8(0x0387), 1 },		/* y_odd_inc */
	{ CCI_REG8(0x0900), 0 },		/* binning off */
	{ CCI_REG8(0x0901), 0x11 },
	/* TODO(regs): mode-specific vendor registers */
};

/* 2x2 binned 2016x1512 — the likely ProtoHUD working mode. */
static const struct cci_reg_sequence imx563_mode_2016x1512_regs[] = {
	{ CCI_REG16(0x0344), 0 },
	{ CCI_REG16(0x0346), 0 },
	{ CCI_REG16(0x0348), 4031 },
	{ CCI_REG16(0x034a), 3023 },
	{ CCI_REG16(0x034c), 2016 },
	{ CCI_REG16(0x034e), 1512 },
	{ CCI_REG8(0x0381), 1 },
	{ CCI_REG8(0x0383), 1 },
	{ CCI_REG8(0x0385), 1 },
	{ CCI_REG8(0x0387), 1 },
	{ CCI_REG8(0x0900), 1 },		/* binning on */
	{ CCI_REG8(0x0901), 0x22 },		/* 2x2 */
	/* TODO(regs): mode-specific vendor registers */
};

static const struct imx563_mode imx563_modes[] = {
	{
		.width = 4032,
		.height = 3024,
		.crop = {
			.left = IMX563_PIXEL_ARRAY_LEFT,
			.top = IMX563_PIXEL_ARRAY_TOP,
			.width = 4032,
			.height = 3024,
		},
		.line_length_pix = 4800,	/* TODO(regs) */
		.frame_length_default = 3200,	/* TODO(regs) ~30 fps */
		.frame_length_min = 3200,
		.regs = imx563_mode_4032x3024_regs,
		.num_regs = ARRAY_SIZE(imx563_mode_4032x3024_regs),
	},
	{
		.width = 2016,
		.height = 1512,
		.crop = {
			.left = IMX563_PIXEL_ARRAY_LEFT,
			.top = IMX563_PIXEL_ARRAY_TOP,
			.width = 4032,
			.height = 3024,
		},
		.line_length_pix = 4800,	/* TODO(regs) */
		.frame_length_default = 1600,	/* TODO(regs) ~60 fps */
		.frame_length_min = 1600,
		.regs = imx563_mode_2016x1512_regs,
		.num_regs = ARRAY_SIZE(imx563_mode_2016x1512_regs),
	},
};

struct imx563 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct device *dev;
	struct regmap *regmap;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;	/* XCLR, active low in DT */
	struct regulator_bulk_data supplies[IMX563_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	const struct imx563_mode *mode;
};

static inline struct imx563 *to_imx563(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx563, sd);
}

static u32 imx563_get_format_code(struct imx563 *imx563)
{
	/* Flip controls are NULL until the handler is built. */
	int h = imx563->hflip ? imx563->hflip->val : 0;
	int v = imx563->vflip ? imx563->vflip->val : 0;

	return imx563_mbus_codes[v][h];
}

static void imx563_update_fmt(struct imx563 *imx563,
			      const struct imx563_mode *mode,
			      struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = imx563_get_format_code(imx563);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

/* Recompute blanking ranges for the active mode. */
static int imx563_set_framing_limits(struct imx563 *imx563)
{
	const struct imx563_mode *mode = imx563->mode;
	u32 hblank = mode->line_length_pix - mode->width;
	u32 vblank_def = mode->frame_length_default - mode->height;
	u32 vblank_min = mode->frame_length_min - mode->height;
	int ret;

	ret = __v4l2_ctrl_modify_range(imx563->vblank, vblank_min,
				       IMX563_FRAME_LENGTH_MAX - mode->height,
				       1, vblank_def);
	if (ret)
		return ret;
	ret = __v4l2_ctrl_s_ctrl(imx563->vblank, vblank_def);
	if (ret)
		return ret;

	/* HBLANK is fixed per mode until line_length tuning is understood. */
	return __v4l2_ctrl_modify_range(imx563->hblank, hblank, hblank, 1,
					hblank);
}

static int imx563_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx563 *imx563 = container_of(ctrl->handler, struct imx563,
					     ctrls);
	const struct imx563_mode *mode = imx563->mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Keep exposure inside the new frame length. */
		int exp_max = mode->height + ctrl->val - IMX563_EXPOSURE_OFFSET;

		__v4l2_ctrl_modify_range(imx563->exposure,
					 imx563->exposure->minimum, exp_max,
					 imx563->exposure->step,
					 min(imx563->exposure->default_value,
					     (s64)exp_max));
	}

	/* Only touch the hardware while powered. */
	if (pm_runtime_get_if_in_use(imx563->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(imx563->regmap, IMX563_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(imx563->regmap, IMX563_REG_ANALOG_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(imx563->regmap, IMX563_REG_DIGITAL_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(imx563->regmap, IMX563_REG_FRAME_LENGTH,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_HBLANK:
		cci_write(imx563->regmap, IMX563_REG_LINE_LENGTH,
			  mode->width + ctrl->val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(imx563->regmap, IMX563_REG_ORIENTATION,
			  imx563->hflip->val | (imx563->vflip->val << 1), &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(imx563->regmap, IMX563_REG_TEST_PATTERN,
			  imx563_test_pattern_val[ctrl->val], &ret);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
		cci_write(imx563->regmap, IMX563_REG_TP_R, ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN_GREENR:
		cci_write(imx563->regmap, IMX563_REG_TP_GR, ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN_BLUE:
		cci_write(imx563->regmap, IMX563_REG_TP_B, ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN_GREENB:
		cci_write(imx563->regmap, IMX563_REG_TP_GB, ctrl->val, &ret);
		break;
	default:
		dev_warn(imx563->dev, "unhandled ctrl 0x%x\n", ctrl->id);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx563->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx563_ctrl_ops = {
	.s_ctrl = imx563_set_ctrl,
};

static int imx563_init_controls(struct imx563 *imx563)
{
	struct v4l2_ctrl_handler *hdl = &imx563->ctrls;
	const struct imx563_mode *mode = imx563->mode;
	struct v4l2_fwnode_device_properties props;
	unsigned int i;
	int ret;

	v4l2_ctrl_handler_init(hdl, 16);

	imx563->pixel_rate = v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       IMX563_PIXEL_RATE,
					       IMX563_PIXEL_RATE, 1,
					       IMX563_PIXEL_RATE);

	imx563->link_freq = v4l2_ctrl_new_int_menu(hdl, &imx563_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(imx563_link_freq_menu) - 1,
						   0, imx563_link_freq_menu);
	if (imx563->link_freq)
		imx563->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Real ranges are set by imx563_set_framing_limits(). */
	imx563->vblank = v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
					   V4L2_CID_VBLANK, 0, 0xffff, 1, 0);
	imx563->hblank = v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
					   V4L2_CID_HBLANK, 0, 0xffff, 1, 0);

	imx563->exposure = v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX563_EXPOSURE_MIN,
					     mode->frame_length_default -
					     IMX563_EXPOSURE_OFFSET,
					     IMX563_EXPOSURE_STEP,
					     mode->frame_length_default -
					     IMX563_EXPOSURE_OFFSET);

	v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX563_ANA_GAIN_MIN, IMX563_ANA_GAIN_MAX,
			  IMX563_ANA_GAIN_STEP, IMX563_ANA_GAIN_DEFAULT);
	v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX563_DGTL_GAIN_MIN, IMX563_DGTL_GAIN_MAX,
			  IMX563_DGTL_GAIN_STEP, IMX563_DGTL_GAIN_DEFAULT);

	imx563->hflip = v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx563->hflip)
		imx563->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	imx563->vflip = v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx563->vflip)
		imx563->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	v4l2_ctrl_new_std_menu_items(hdl, &imx563_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx563_test_pattern_menu) - 1,
				     0, 0, imx563_test_pattern_menu);
	for (i = 0; i < 4; i++)
		v4l2_ctrl_new_std(hdl, &imx563_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_RED + i,
				  0, IMX563_TP_COLOUR_MAX, 1,
				  IMX563_TP_COLOUR_MAX);

	if (hdl->error) {
		ret = hdl->error;
		dev_err(imx563->dev, "control init failed (%d)\n", ret);
		goto err;
	}

	ret = v4l2_fwnode_device_parse(imx563->dev, &props);
	if (ret)
		goto err;
	ret = v4l2_ctrl_new_fwnode_properties(hdl, &imx563_ctrl_ops, &props);
	if (ret)
		goto err;

	imx563->sd.ctrl_handler = hdl;

	v4l2_ctrl_lock(imx563->vblank);
	imx563_set_framing_limits(imx563);
	v4l2_ctrl_unlock(imx563->vblank);

	return 0;

err:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

/* ── Streaming ───────────────────────────────────────────────────────────── */

static int imx563_start_streaming(struct imx563 *imx563)
{
	const struct imx563_mode *mode = imx563->mode;
	int ret;

	ret = pm_runtime_resume_and_get(imx563->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(imx563->regmap, imx563_common_regs,
				  ARRAY_SIZE(imx563_common_regs), NULL);
	if (ret) {
		dev_err(imx563->dev, "common regs failed (%d)\n", ret);
		goto err_rpm;
	}

	ret = cci_multi_reg_write(imx563->regmap, mode->regs, mode->num_regs,
				  NULL);
	if (ret) {
		dev_err(imx563->dev, "mode regs failed (%d)\n", ret);
		goto err_rpm;
	}

	/* Push current exposure/gain/flip/blanking to the sensor. */
	ret = __v4l2_ctrl_handler_setup(imx563->sd.ctrl_handler);
	if (ret)
		goto err_rpm;

	ret = cci_write(imx563->regmap, IMX563_REG_MODE_SELECT,
			IMX563_MODE_STREAMING, NULL);
	if (ret)
		goto err_rpm;

	/* Flip changes the Bayer order; lock it while streaming. */
	__v4l2_ctrl_grab(imx563->hflip, true);
	__v4l2_ctrl_grab(imx563->vflip, true);

	return 0;

err_rpm:
	pm_runtime_put(imx563->dev);
	return ret;
}

static void imx563_stop_streaming(struct imx563 *imx563)
{
	int ret;

	ret = cci_write(imx563->regmap, IMX563_REG_MODE_SELECT,
			IMX563_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx563->dev, "failed to stop stream (%d)\n", ret);

	__v4l2_ctrl_grab(imx563->hflip, false);
	__v4l2_ctrl_grab(imx563->vflip, false);

	pm_runtime_put(imx563->dev);
}

static int imx563_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx563 *imx563 = to_imx563(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (enable)
		ret = imx563_start_streaming(imx563);
	else
		imx563_stop_streaming(imx563);
	v4l2_subdev_unlock_state(state);

	return ret;
}

/* ── Pad ops ─────────────────────────────────────────────────────────────── */

static int imx563_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = imx563_get_format_code(to_imx563(sd));
	return 0;
}

static int imx563_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx563 *imx563 = to_imx563(sd);

	if (fse->index >= ARRAY_SIZE(imx563_modes))
		return -EINVAL;
	if (fse->code != imx563_get_format_code(imx563))
		return -EINVAL;

	fse->min_width = imx563_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = imx563_modes[fse->index].height;
	fse->max_height = fse->min_height;
	return 0;
}

static int imx563_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx563 *imx563 = to_imx563(sd);
	const struct imx563_mode *mode;
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	mode = v4l2_find_nearest_size(imx563_modes, ARRAY_SIZE(imx563_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);
	imx563_update_fmt(imx563, mode, &fmt->format);

	format = v4l2_subdev_state_get_format(state, 0);
	*format = fmt->format;
	crop = v4l2_subdev_state_get_crop(state, 0);
	*crop = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		imx563->mode = mode;
		return imx563_set_framing_limits(imx563);
	}

	return 0;
}

static int imx563_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX563_NATIVE_WIDTH;
		sel->r.height = IMX563_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = IMX563_PIXEL_ARRAY_LEFT;
		sel->r.top = IMX563_PIXEL_ARRAY_TOP;
		sel->r.width = IMX563_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX563_PIXEL_ARRAY_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int imx563_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = imx563_modes[1].width,
			.height = imx563_modes[1].height,
		},
	};

	return imx563_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_core_ops imx563_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx563_video_ops = {
	.s_stream = imx563_s_stream,
};

static const struct v4l2_subdev_pad_ops imx563_pad_ops = {
	.enum_mbus_code = imx563_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx563_set_pad_format,
	.get_selection = imx563_get_selection,
	.enum_frame_size = imx563_enum_frame_size,
};

static const struct v4l2_subdev_ops imx563_subdev_ops = {
	.core = &imx563_core_ops,
	.video = &imx563_video_ops,
	.pad = &imx563_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx563_internal_ops = {
	.init_state = imx563_init_state,
};

/* ── Power ───────────────────────────────────────────────────────────────── */

static int imx563_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx563 *imx563 = to_imx563(sd);
	int ret;

	/* Sony order: OVDD -> AVDD -> DVDD (bulk enables in array order). */
	ret = regulator_bulk_enable(IMX563_NUM_SUPPLIES, imx563->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators\n");
		return ret;
	}

	ret = clk_prepare_enable(imx563->xclk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		goto err_reg;
	}

	gpiod_set_value_cansleep(imx563->reset_gpio, 0);	/* release XCLR */
	usleep_range(IMX563_XCLR_MIN_DELAY_US,
		     IMX563_XCLR_MIN_DELAY_US + IMX563_XCLR_DELAY_RANGE_US);

	return 0;

err_reg:
	regulator_bulk_disable(IMX563_NUM_SUPPLIES, imx563->supplies);
	return ret;
}

static int imx563_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx563 *imx563 = to_imx563(sd);

	gpiod_set_value_cansleep(imx563->reset_gpio, 1);
	clk_disable_unprepare(imx563->xclk);
	regulator_bulk_disable(IMX563_NUM_SUPPLIES, imx563->supplies);

	return 0;
}

/* ── Probe ───────────────────────────────────────────────────────────────── */

static int imx563_identify(struct imx563 *imx563)
{
	u64 val;
	int ret;

	ret = cci_read(imx563->regmap, IMX563_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(imx563->dev, ret,
				     "no response reading chip id\n");

	if (val != IMX563_CHIP_ID) {
		/* Not fatal during bring-up: the expected ID is a guess. */
		dev_warn(imx563->dev,
			 "chip id 0x%04llx (expected 0x%04x) — update IMX563_CHIP_ID\n",
			 val, IMX563_CHIP_ID);
	}

	return 0;
}

static int imx563_check_hwcfg(struct imx563 *imx563)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long link_freq_bitmap;
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(imx563->dev),
						  NULL);
	if (!endpoint)
		return dev_err_probe(imx563->dev, -EINVAL, "no endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(imx563->dev, ret, "bad endpoint\n");

	if (ep.bus.mipi_csi2.num_data_lanes != IMX563_NUM_LANES) {
		ret = dev_err_probe(imx563->dev, -EINVAL,
				    "only %d data lanes supported\n",
				    IMX563_NUM_LANES);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(imx563->dev, ep.link_frequencies,
				       ep.nr_of_link_frequencies,
				       imx563_link_freq_menu,
				       ARRAY_SIZE(imx563_link_freq_menu),
				       &link_freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int imx563_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx563 *imx563;
	unsigned int i;
	int ret;

	imx563 = devm_kzalloc(dev, sizeof(*imx563), GFP_KERNEL);
	if (!imx563)
		return -ENOMEM;

	imx563->dev = dev;
	v4l2_i2c_subdev_init(&imx563->sd, client, &imx563_subdev_ops);
	imx563->sd.internal_ops = &imx563_internal_ops;

	imx563->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx563->regmap))
		return dev_err_probe(dev, PTR_ERR(imx563->regmap),
				     "regmap init failed\n");

	ret = imx563_check_hwcfg(imx563);
	if (ret)
		return ret;

	imx563->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(imx563->xclk))
		return dev_err_probe(dev, PTR_ERR(imx563->xclk),
				     "no xclk\n");
	if (clk_get_rate(imx563->xclk) != IMX563_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL, "xclk must be %u Hz\n",
				     IMX563_XCLK_FREQ);

	for (i = 0; i < IMX563_NUM_SUPPLIES; i++)
		imx563->supplies[i].supply = imx563_supply_name[i];
	ret = devm_regulator_bulk_get(dev, IMX563_NUM_SUPPLIES,
				      imx563->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "regulators\n");

	imx563->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx563->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx563->reset_gpio),
				     "reset gpio\n");

	ret = imx563_power_on(dev);
	if (ret)
		return ret;

	ret = imx563_identify(imx563);
	if (ret)
		goto err_power;

	dev_warn(dev, "IMX563 framework driver: mode register tables are placeholders (see TODO(regs))\n");

	imx563->mode = &imx563_modes[1];

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = imx563_init_controls(imx563);
	if (ret)
		goto err_pm;

	imx563->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	imx563->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx563->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx563->sd.entity, 1, &imx563->pad);
	if (ret)
		goto err_ctrls;

	imx563->sd.state_lock = imx563->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&imx563->sd);
	if (ret)
		goto err_entity;

	ret = v4l2_async_register_subdev_sensor(&imx563->sd);
	if (ret)
		goto err_subdev;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return 0;

err_subdev:
	v4l2_subdev_cleanup(&imx563->sd);
err_entity:
	media_entity_cleanup(&imx563->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&imx563->ctrls);
err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_put_noidle(dev);
err_power:
	imx563_power_off(dev);
	return ret;
}

static void imx563_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx563 *imx563 = to_imx563(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx563->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx563_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx563_pm_ops, imx563_power_off,
				 imx563_power_on, NULL);

static const struct of_device_id imx563_of_match[] = {
	{ .compatible = "sony,imx563" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx563_of_match);

static struct i2c_driver imx563_i2c_driver = {
	.driver = {
		.name = "imx563",
		.of_match_table = imx563_of_match,
		.pm = pm_ptr(&imx563_pm_ops),
	},
	.probe = imx563_probe,
	.remove = imx563_remove,
};
module_i2c_driver(imx563_i2c_driver);

MODULE_DESCRIPTION("Sony IMX563 sensor driver (ProtoHUD framework)");
MODULE_LICENSE("GPL");
