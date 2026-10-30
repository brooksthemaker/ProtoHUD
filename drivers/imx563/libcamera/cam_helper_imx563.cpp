/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cam_helper_imx563.cpp - Raspberry Pi IPA camera helper for the Sony IMX563
 *
 * Drop into libcamera at src/ipa/rpi/cam_helper/ and add it to that
 * directory's meson.build (see README.md next to this file).
 *
 * TODO(verify): gain law, frameIntegrationDiff and delays must match the
 * kernel driver (IMX563_ANA_GAIN_*, IMX563_EXPOSURE_OFFSET) and the
 * datasheet. The values below are the IMX477/IMX708 family defaults.
 */

#include <cmath>

#include "cam_helper.h"

using namespace RPiController;

class CamHelperImx563 : public CamHelper
{
public:
	CamHelperImx563();
	uint32_t gainCode(double gain) const override;
	double gain(uint32_t gainCode) const override;
	void getDelays(int &exposureDelay, int &gainDelay,
		       int &vblankDelay, int &hblankDelay) const override;

private:
	/*
	 * Smallest difference between frame length and integration time, in
	 * lines. Must equal IMX563_EXPOSURE_OFFSET in the kernel driver.
	 */
	static constexpr int frameIntegrationDiff = 22;
};

/* No embedded-data parser yet: the kernel driver has no metadata pad. */
CamHelperImx563::CamHelperImx563()
	: CamHelper({}, frameIntegrationDiff)
{
}

/* Sony analogue gain law: gain = 1024 / (1024 - code). */
uint32_t CamHelperImx563::gainCode(double gain) const
{
	return static_cast<uint32_t>(1024 - 1024 / gain);
}

double CamHelperImx563::gain(uint32_t gainCode) const
{
	return 1024.0 / (1024 - gainCode);
}

void CamHelperImx563::getDelays(int &exposureDelay, int &gainDelay,
				int &vblankDelay, int &hblankDelay) const
{
	exposureDelay = 2;
	gainDelay = 2;
	vblankDelay = 3;
	hblankDelay = 3;
}

static CamHelper *create()
{
	return new CamHelperImx563();
}

static RegisterCamHelper reg("imx563", &create);
