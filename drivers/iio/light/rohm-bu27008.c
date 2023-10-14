// SPDX-License-Identifier: GPL-2.0-only
/*
 * BU27008 ROHM Colour Sensor
 *
 * Copyright (c) 2023, ROHM Semiconductor.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/iio-gts-helper.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define BU27008_REG_SYSTEM_CONTROL	0x40
#define BU27008_MASK_SW_RESET		BIT(7)
#define BU27008_MASK_PART_ID		GENMASK(5, 0)
#define BU27008_ID			0x1a
#define BU27008_REG_MODE_CONTROL1	0x41
#define BU27008_MASK_MEAS_MODE		GENMASK(2, 0)
#define BU27008_MASK_CHAN_SEL		GENMASK(3, 2)

#define BU27008_REG_MODE_CONTROL2	0x42
#define BU27008_MASK_RGBC_GAIN		GENMASK(7, 3)
#define BU27008_MASK_IR_GAIN_LO		GENMASK(2, 0)
#define BU27008_SHIFT_IR_GAIN		3

#define BU27008_REG_MODE_CONTROL3	0x43
#define BU27008_MASK_VALID		BIT(7)
#define BU27008_MASK_INT_EN		BIT(1)
#define BU27008_INT_EN			BU27008_MASK_INT_EN
#define BU27008_INT_DIS			0
#define BU27008_MASK_MEAS_EN		BIT(0)
#define BU27008_MEAS_EN			BIT(0)
#define BU27008_MEAS_DIS		0

#define BU27008_REG_DATA0_LO		0x50
#define BU27008_REG_DATA1_LO		0x52
#define BU27008_REG_DATA2_LO		0x54
#define BU27008_REG_DATA3_LO		0x56
#define BU27008_REG_DATA3_HI		0x57
#define BU27008_REG_MANUFACTURER_ID	0x92
#define BU27008_REG_MAX BU27008_REG_MANUFACTURER_ID

/**
 * enum bu27008_chan_type - BU27008 channel types
 * @BU27008_RED:	Red channel. Always via data0.
 * @BU27008_GREEN:	Green channel. Always via data1.
 * @BU27008_BLUE:	Blue channel. Via data2 (when used).
 * @BU27008_CLEAR:	Clear channel. Via data2 or data3 (when used).
 * @BU27008_IR:		IR channel. Via data3 (when used).
 * @BU27008_NUM_CHANS:	Number of channel types.
 */
enum bu27008_chan_type {
	BU27008_RED,
	BU27008_GREEN,
	BU27008_BLUE,
	BU27008_CLEAR,
	BU27008_IR,
	BU27008_NUM_CHANS
};

/**
 * enum bu27008_chan - BU27008 physical data channel
 * @BU27008_DATA0:		Always red.
 * @BU27008_DATA1:		Always green.
 * @BU27008_DATA2:		Blue or clear.
 * @BU27008_DATA3:		IR or clear.
 * @BU27008_NUM_HW_CHANS:	Number of physical channels
 */
enum bu27008_chan {
	BU27008_DATA0,
	BU27008_DATA1,
	BU27008_DATA2,
	BU27008_DATA3,
	BU27008_NUM_HW_CHANS
};

/* We can always measure red and green at same time */
#define ALWAYS_SCANNABLE (BIT(BU27008_RED) | BIT(BU27008_GREEN))

/* We use these data channel configs. Ensure scan_masks below follow them too */
#define BU27008_BLUE2_CLEAR3		0x0 /* buffer is R, G, B, C */
#define BU27008_CLEAR2_IR3		0x1 /* buffer is R, G, C, IR */
#define BU27008_BLUE2_IR3		0x2 /* buffer is R, G, B, IR */

static const unsigned long bu27008_scan_masks[] = {
	/* buffer is R, G, B, C */
	ALWAYS_SCANNABLE | BIT(BU27008_BLUE) | BIT(BU27008_CLEAR),
	/* buffer is R, G, C, IR */
	ALWAYS_SCANNABLE | BIT(BU27008_CLEAR) | BIT(BU27008_IR),
	/* buffer is R, G, B, IR */
	ALWAYS_SCANNABLE | BIT(BU27008_BLUE) | BIT(BU27008_IR),
	0
};

/*
 * Available scales with gain 1x - 1024x, timings 55, 100, 200, 400 mS
 * Time impacts to gain: 1x, 2x, 4x, 8x.
 *
 * => Max total gain is HWGAIN * gain by integration time (8 * 1024) = 8192
 *
 * Max amplification is (HWGAIN * MAX integration-time multiplier) 1024 * 8
 * = 8192. With NANO scale we get rid of accuracy loss when we start with the
 * scale 16.0 for HWGAIN1, INT-TIME 55 mS. This way the nano scale for MAX
 * total gain 8192 will be 1953125
 */
#define BU27008_SCALE_1X 16

/* See the data sheet for the "Gain Setting" table */
#define BU27008_GSEL_1X		0x00
#define BU27008_GSEL_4X		0x08
#define BU27008_GSEL_8X		0x09
#define BU27008_GSEL_16X	0x0a
#define BU27008_GSEL_32X	0x0b
#define BU27008_GSEL_64X	0x0c
#define BU27008_GSEL_256X	0x18
#define BU27008_GSEL_512X	0x19
#define BU27008_GSEL_1024X	0x1a

static const struct iio_gain_sel_pair bu27008_gains[] = {
	GAIN_SCALE_GAIN(1, BU27008_GSEL_1X),
	GAIN_SCALE_GAIN(4, BU27008_GSEL_4X),
	GAIN_SCALE_GAIN(8, BU27008_GSEL_8X),
	GAIN_SCALE_GAIN(16, BU27008_GSEL_16X),
	GAIN_SCALE_GAIN(32, BU27008_GSEL_32X),
	GAIN_SCALE_GAIN(64, BU27008_GSEL_64X),
	GAIN_SCALE_GAIN(256, BU27008_GSEL_256X),
	GAIN_SCALE_GAIN(512, BU27008_GSEL_512X),
	GAIN_SCALE_GAIN(1024, BU27008_GSEL_1024X),
};

static const struct iio_gain_sel_pair bu27008_gains_ir[] = {
	GAIN_SCALE_GAIN(2, BU27008_GSEL_1X),
	GAIN_SCALE_GAIN(4, BU27008_GSEL_4X),
	GAIN_SCALE_GAIN(8, BU27008_GSEL_8X),
	GAIN_SCALE_GAIN(16, BU27008_GSEL_16X),
	GAIN_SCALE_GAIN(32, BU27008_GSEL_32X),
	GAIN_SCALE_GAIN(64, BU27008_GSEL_64X),
	GAIN_SCALE_GAIN(256, BU27008_GSEL_256X),
	GAIN_SCALE_GAIN(512, BU27008_GSEL_512X),
	GAIN_SCALE_GAIN(1024, BU27008_GSEL_1024X),
};

#define BU27008_MEAS_MODE_100MS		0x00
#define BU27008_MEAS_MODE_55MS		0x01
#define BU27008_MEAS_MODE_200MS		0x02
#define BU27008_MEAS_MODE_400MS		0x04
#define BU27008_MEAS_TIME_MAX_MS	400

static const struct iio_itime_sel_mul bu27008_itimes[] = {
	GAIN_SCALE_ITIME_US(400000, BU27008_MEAS_MODE_400MS, 8),
	GAIN_SCALE_ITIME_US(200000, BU27008_MEAS_MODE_200MS, 4),
	GAIN_SCALE_ITIME_US(100000, BU27008_MEAS_MODE_100MS, 2),
	GAIN_SCALE_ITIME_US(55000, BU27008_MEAS_MODE_55MS, 1),
};

/*
 * All the RGBC channels share the same gain.
 * IR gain can be fine-tuned from the gain set for the RGBC by 2 bit, but this
 * would yield quite complex gain setting. Especially since not all bit
 * compinations are supported. And in any case setting GAIN for RGBC will
 * always also change the IR-gain.
 *
 * On top of this, the selector '0' which corresponds to hw-gain 1X on RGBC,
 * corresponds to gain 2X on IR. Rest of the selctors correspond to same gains
 * though. This, however, makes it not possible to use shared gain for all
 * RGBC and IR settings even though they are all changed at the one go.
 */
#define BU27008_CHAN(color, data, separate_avail)				\
{										\
	.type = IIO_INTENSITY,							\
	.modified = 1,								\
	.channel2 = IIO_MOD_LIGHT_##color,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |				\
			      BIT(IIO_CHAN_INFO_SCALE),				\
	.info_mask_separate_available = (separate_avail),			\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_INT_TIME),			\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_INT_TIME),	\
	.address = BU27008_REG_##data##_LO,					\
	.scan_index = BU27008_##color,						\
	.scan_type = {								\
		.sign = 's',							\
		.realbits = 16,							\
		.storagebits = 16,						\
		.endianness = IIO_LE,						\
	},									\
}

/* For raw reads we always configure DATA3 for CLEAR */
static const struct iio_chan_spec bu27008_channels[] = {
	BU27008_CHAN(RED, DATA0, BIT(IIO_CHAN_INFO_SCALE)),
	BU27008_CHAN(GREEN, DATA1, BIT(IIO_CHAN_INFO_SCALE)),
	BU27008_CHAN(BLUE, DATA2, BIT(IIO_CHAN_INFO_SCALE)),
	BU27008_CHAN(CLEAR, DATA2, BIT(IIO_CHAN_INFO_SCALE)),
	/*
	 * We don't allow setting scale for IR (because of shared gain bits).
	 * Hence we don't advertise available ones either.
	 */
	BU27008_CHAN(IR, DATA3, 0),
	IIO_CHAN_SOFT_TIMESTAMP(BU27008_NUM_CHANS),
};

struct bu27008_data {
	struct regmap *regmap;
	struct iio_trigger *trig;
	struct device *dev;
	struct iio_gts gts;
	struct iio_gts gts_ir;
	int irq;

	/*
	 * Prevent changing gain/time config when scale is read/written.
	 * Similarly, protect the integration_time read/change sequence.
	 * Prevent changing gain/time when data is read.
	 */
	struct mutex mutex;
};

static const struct regmap_range bu27008_volatile_ranges[] = {
	{
		.range_min = BU27008_REG_SYSTEM_CONTROL,	/* SWRESET */
		.range_max = BU27008_REG_SYSTEM_CONTROL,
	}, {
		.range_min = BU27008_REG_MODE_CONTROL3,		/* VALID */
		.range_max = BU27008_REG_MODE_CONTROL3,
	}, {
		.range_min = BU27008_REG_DATA0_LO,		/* DATA */
		.range_max = BU27008_REG_DATA3_HI,
	},
};

static const struct regmap_access_table bu27008_volatile_regs = {
	.yes_ranges = &bu27008_volatile_ranges[0],
	.n_yes_ranges = ARRAY_SIZE(bu27008_volatile_ranges),
};

static const struct regmap_range bu27008_read_only_ranges[] = {
	{
		.range_min = BU27008_REG_DATA0_LO,
		.range_max = BU27008_REG_DATA3_HI,
	}, {
		.range_min = BU27008_REG_MANUFACTURER_ID,
		.range_max = BU27008_REG_MANUFACTURER_ID,
	},
};

static const struct regmap_access_table bu27008_ro_regs = {
	.no_ranges = &bu27008_read_only_ranges[0],
	.n_no_ranges = ARRAY_SIZE(bu27008_read_only_ranges),
};

static const struct regmap_config bu27008_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = BU27008_REG_MAX,
	.cache_type = REGCACHE_RBTREE,
	.volatile_table = &bu27008_volatile_regs,
	.wr_table = &bu27008_ro_regs,
	/*
	 * All register writes are serialized by the mutex which protects the
	 * scale setting/getting. This is needed because scale is combined by
	 * gain and integration time settings and we need to ensure those are
	 * not read / written when scale is being computed.
	 *
	 * As a result of this serializing, we don't need regmap locking. Note,
	 * this is not true if we add any configurations which are not
	 * serialized by the mutex and which may need for example a protected
	 * read-modify-write cycle (eg. regmap_update_bits()). Please, revise
	 * this when adding features to the driver.
	 */
	.disable_locking = true,
};

#define BU27008_MAX_VALID_RESULT_WAIT_US	50000
#define BU27008_VALID_RESULT_WAIT_QUANTA_US	1000

static int bu27008_chan_read_data(struct bu27008_data *data, int reg, int *val)
{
	int ret, valid;
	__le16 tmp;

	ret = regmap_read_poll_timeout(data->regmap, BU27008_REG_MODE_CONTROL3,
				       valid, (valid & BU27008_MASK_VALID),
				       BU27008_VALID_RESULT_WAIT_QUANTA_US,
				       BU27008_MAX_VALID_RESULT_WAIT_US);
	if (ret)
		return ret;

	ret = regmap_bulk_read(data->regmap, reg, &tmp, sizeof(tmp));
	if (ret)
		dev_err(data->dev, "Reading channel data failed\n");

	*val = le16_to_cpu(tmp);

	return ret;
}

static int bu27008_get_gain(struct bu27008_data *data, struct iio_gts *gts, int *gain)
{
	int ret, sel;

	ret = regmap_read(data->regmap, BU27008_REG_MODE_CONTROL2, &sel);
	if (ret)
		return ret;

	sel = FIELD_GET(BU27008_MASK_RGBC_GAIN, sel);

	ret = iio_gts_find_gain_by_sel(gts, sel);
	if (ret < 0) {
		dev_err(data->dev, "unknown gain value 0x%x\n", sel);
		return ret;
	}

	*gain = ret;

	return 0;
}

static int bu27008_write_gain_sel(struct bu27008_data *data, int sel)
{
	int regval;

	regval = FIELD_PREP(BU27008_MASK_RGBC_GAIN, sel);

	/*
	 * We do always set also the LOW bits of IR-gain because othervice we
	 * would risk resulting an invalid GAIN register value.
	 *
	 * We could allow setting separate gains for RGBC and IR when the
	 * values were such that HW could support both gain settings.
	 * Eg, when the shared bits were same for both gain values.
	 *
	 * This, however, has a negligible benefit compared to the increased
	 * software complexity when we would need to go through the gains
	 * for both channels separately when the integration time changes.
	 * This would end up with nasty logic for computing gain values for
	 * both channels - and rejecting them if shared bits changed.
	 *
	 * We should then build the logic by guessing what a user prefers.
	 * RGBC or IR gains correctly set while other jumps to odd value?
	 * Maybe look-up a value where both gains are somehow optimized
	 * <what this somehow is, is ATM unknown to us>. Or maybe user would
	 * expect us to reject changes when optimal gains can't be set to both
	 * channels w/given integration time. At best that would result
	 * solution that works well for a very specific subset of
	 * configurations but causes unexpected corner-cases.
	 *
	 * So, we keep it simple. Always set same selector to IR and RGBC.
	 * We disallow setting IR (as I expect that most of the users are
	 * interested in RGBC). This way we can show the user that the scales
	 * for RGBC and IR channels are different (1X Vs 2X with sel 0) while
	 * still keeping the operation deterministic.
	 */
	regval |= FIELD_PREP(BU27008_MASK_IR_GAIN_LO, sel);

	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL2,
				  BU27008_MASK_RGBC_GAIN, regval);
}

static int bu27008_set_gain(struct bu27008_data *data, int gain)
{
	int ret;

	ret = iio_gts_find_sel_by_gain(&data->gts, gain);
	if (ret < 0)
		return ret;

	return bu27008_write_gain_sel(data, ret);
}

static int bu27008_get_int_time_sel(struct bu27008_data *data, int *sel)
{
	int ret, val;

	ret = regmap_read(data->regmap, BU27008_REG_MODE_CONTROL1, &val);
	*sel = FIELD_GET(BU27008_MASK_MEAS_MODE, val);

	return ret;
}

static int bu27008_set_int_time_sel(struct bu27008_data *data, int sel)
{
	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL1,
				  BU27008_MASK_MEAS_MODE, sel);
}

static int bu27008_get_int_time_us(struct bu27008_data *data)
{
	int ret, sel;

	ret = bu27008_get_int_time_sel(data, &sel);
	if (ret)
		return ret;

	return iio_gts_find_int_time_by_sel(&data->gts, sel);
}

static int _bu27008_get_scale(struct bu27008_data *data, bool ir, int *val,
			      int *val2)
{
	struct iio_gts *gts;
	int gain, ret;

	if (ir)
		gts = &data->gts_ir;
	else
		gts = &data->gts;

	ret = bu27008_get_gain(data, gts, &gain);
	if (ret)
		return ret;

	ret = bu27008_get_int_time_us(data);
	if (ret < 0)
		return ret;

	return iio_gts_get_scale(gts, gain, ret, val, val2);
}

static int bu27008_get_scale(struct bu27008_data *data, bool ir, int *val,
			     int *val2)
{
	int ret;

	mutex_lock(&data->mutex);
	ret = _bu27008_get_scale(data, ir, val, val2);
	mutex_unlock(&data->mutex);

	return ret;
}

static int bu27008_set_int_time(struct bu27008_data *data, int time)
{
	int ret;

	ret = iio_gts_find_sel_by_int_time(&data->gts, time);
	if (ret < 0)
		return ret;

	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL1,
				  BU27008_MASK_MEAS_MODE, ret);
}

/* Try to change the time so that the scale is maintained */
static int bu27008_try_set_int_time(struct bu27008_data *data, int int_time_new)
{
	int ret, old_time_sel, new_time_sel,  old_gain, new_gain;

	mutex_lock(&data->mutex);

	ret = bu27008_get_int_time_sel(data, &old_time_sel);
	if (ret < 0)
		goto unlock_out;

	if (!iio_gts_valid_time(&data->gts, int_time_new)) {
		dev_dbg(data->dev, "Unsupported integration time %u\n",
			int_time_new);

		ret = -EINVAL;
		goto unlock_out;
	}

	/* If we already use requested time, then we're done */
	new_time_sel = iio_gts_find_sel_by_int_time(&data->gts, int_time_new);
	if (new_time_sel == old_time_sel)
		goto unlock_out;

	ret = bu27008_get_gain(data, &data->gts, &old_gain);
	if (ret)
		goto unlock_out;

	ret = iio_gts_find_new_gain_sel_by_old_gain_time(&data->gts, old_gain,
				old_time_sel, new_time_sel, &new_gain);
	if (ret) {
		int scale1, scale2;
		bool ok;

		_bu27008_get_scale(data, false, &scale1, &scale2);
		dev_dbg(data->dev,
			"Can't support time %u with current scale %u %u\n",
			int_time_new, scale1, scale2);

		if (new_gain < 0)
			goto unlock_out;

		/*
		 * If caller requests for integration time change and we
		 * can't support the scale - then the caller should be
		 * prepared to 'pick up the pieces and deal with the
		 * fact that the scale changed'.
		 */
		ret = iio_find_closest_gain_low(&data->gts, new_gain, &ok);
		if (!ok)
			dev_dbg(data->dev, "optimal gain out of range\n");

		if (ret < 0) {
			dev_dbg(data->dev,
				 "Total gain increase. Risk of saturation");
			ret = iio_gts_get_min_gain(&data->gts);
			if (ret < 0)
				goto unlock_out;
		}
		new_gain = ret;
		dev_dbg(data->dev, "scale changed, new gain %u\n", new_gain);
	}

	ret = bu27008_set_gain(data, new_gain);
	if (ret)
		goto unlock_out;

	ret = bu27008_set_int_time(data, int_time_new);

unlock_out:
	mutex_unlock(&data->mutex);

	return ret;
}

static int bu27008_meas_set(struct bu27008_data *data, int state)
{
	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL3,
				  BU27008_MASK_MEAS_EN, state);
}

static int bu27008_chan_cfg(struct bu27008_data *data,
			    struct iio_chan_spec const *chan)
{
	int chan_sel;

	if (chan->scan_index == BU27008_BLUE)
		chan_sel = BU27008_BLUE2_CLEAR3;
	else
		chan_sel = BU27008_CLEAR2_IR3;

	chan_sel = FIELD_PREP(BU27008_MASK_CHAN_SEL, chan_sel);

	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL3,
				  BU27008_MASK_CHAN_SEL, chan_sel);
}

static int bu27008_read_one(struct bu27008_data *data, struct iio_dev *idev,
			    struct iio_chan_spec const *chan, int *val, int *val2)
{
	int ret, int_time;

	ret = bu27008_chan_cfg(data, chan);
	if (ret)
		return ret;

	ret = bu27008_meas_set(data, BU27008_MEAS_EN);
	if (ret)
		return ret;

	ret = bu27008_get_int_time_us(data);
	if (ret < 0)
		int_time = BU27008_MEAS_TIME_MAX_MS;
	else
		int_time = ret / USEC_PER_MSEC;

	msleep(int_time);

	ret = bu27008_chan_read_data(data, chan->address, val);
	if (!ret)
		ret = IIO_VAL_INT;

	if (bu27008_meas_set(data, BU27008_MEAS_DIS))
		dev_warn(data->dev, "measurement disabling failed\n");

	return ret;
}

static int bu27008_read_raw(struct iio_dev *idev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bu27008_data *data = iio_priv(idev);
	int busy, ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		busy = iio_device_claim_direct_mode(idev);
		if (busy)
			return -EBUSY;

		mutex_lock(&data->mutex);
		ret = bu27008_read_one(data, idev, chan, val, val2);
		mutex_unlock(&data->mutex);

		iio_device_release_direct_mode(idev);

		return ret;

	case IIO_CHAN_INFO_SCALE:
		ret = bu27008_get_scale(data, chan->scan_index == BU27008_IR,
					val, val2);
		if (ret)
			return ret;

		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_INT_TIME:
		ret = bu27008_get_int_time_us(data);
		if (ret < 0)
			return ret;

		*val = 0;
		*val2 = ret;

		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

/* Called if the new scale could not be supported with existing int-time */
static int bu27008_try_find_new_time_gain(struct bu27008_data *data, int val,
					  int val2, int *gain_sel)
{
	int i, ret, new_time_sel;

	for (i = 0; i < data->gts.num_itime; i++) {
		new_time_sel = data->gts.itime_table[i].sel;
		ret = iio_gts_find_gain_sel_for_scale_using_time(&data->gts,
					new_time_sel, val, val2 * 1000, gain_sel);
		if (!ret)
			break;
	}
	if (i == data->gts.num_itime) {
		dev_err(data->dev, "Can't support scale %u %u\n", val, val2);

		return -EINVAL;
	}

	return bu27008_set_int_time_sel(data, new_time_sel);
}

static int bu27008_set_scale(struct bu27008_data *data,
			     struct iio_chan_spec const *chan,
			     int val, int val2)
{
	int ret, gain_sel, time_sel;

	if (chan->scan_index == BU27008_IR)
		return -EINVAL;

	mutex_lock(&data->mutex);

	ret = bu27008_get_int_time_sel(data, &time_sel);
	if (ret < 0)
		goto unlock_out;

	ret = iio_gts_find_gain_sel_for_scale_using_time(&data->gts, time_sel,
						val, val2 * 1000, &gain_sel);
	if (ret) {
		ret = bu27008_try_find_new_time_gain(data, val, val2, &gain_sel);
		if (ret)
			goto unlock_out;

	}
	ret = bu27008_write_gain_sel(data, gain_sel);

unlock_out:
	mutex_unlock(&data->mutex);

	return ret;
}

static int bu27008_write_raw(struct iio_dev *idev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct bu27008_data *data = iio_priv(idev);
	int ret;

	/*
	 * Do not allow changing scale when measurement is ongoing as doing so
	 * could make values in the buffer inconsistent.
	 */
	ret = iio_device_claim_direct_mode(idev);
	if (ret)
		return ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		ret = bu27008_set_scale(data, chan, val, val2);
		break;
	case IIO_CHAN_INFO_INT_TIME:
		if (val) {
			ret = -EINVAL;
			break;
		}
		ret = bu27008_try_set_int_time(data, val2);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	iio_device_release_direct_mode(idev);

	return ret;
}

static int bu27008_read_avail(struct iio_dev *idev,
			      struct iio_chan_spec const *chan, const int **vals,
			      int *type, int *length, long mask)
{
	struct bu27008_data *data = iio_priv(idev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		return iio_gts_avail_times(&data->gts, vals, type, length);
	case IIO_CHAN_INFO_SCALE:
		if (chan->channel2 == IIO_MOD_LIGHT_IR)
			return iio_gts_all_avail_scales(&data->gts_ir, vals,
							type, length);
		return iio_gts_all_avail_scales(&data->gts, vals, type, length);
	default:
		return -EINVAL;
	}
}

static int bu27008_update_scan_mode(struct iio_dev *idev,
				    const unsigned long *scan_mask)
{
	struct bu27008_data *data = iio_priv(idev);
	int chan_sel;

	/* Configure channel selection */
	if (test_bit(BU27008_BLUE, idev->active_scan_mask)) {
		if (test_bit(BU27008_CLEAR, idev->active_scan_mask))
			chan_sel = BU27008_BLUE2_CLEAR3;
		else
			chan_sel = BU27008_BLUE2_IR3;
	} else {
		chan_sel = BU27008_CLEAR2_IR3;
	}

	chan_sel = FIELD_PREP(BU27008_MASK_CHAN_SEL, chan_sel);

	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL3,
				 BU27008_MASK_CHAN_SEL, chan_sel);
}

static const struct iio_info bu27008_info = {
	.read_raw = &bu27008_read_raw,
	.write_raw = &bu27008_write_raw,
	.read_avail = &bu27008_read_avail,
	.update_scan_mode = bu27008_update_scan_mode,
	.validate_trigger = iio_validate_own_trigger,
};

static int bu27008_chip_init(struct bu27008_data *data)
{
	int ret;

	ret = regmap_write_bits(data->regmap, BU27008_REG_SYSTEM_CONTROL,
				BU27008_MASK_SW_RESET, BU27008_MASK_SW_RESET);
	if (ret)
		return dev_err_probe(data->dev, ret, "Sensor reset failed\n");

	/*
	 * The data-sheet does not tell how long performing the IC reset takes.
	 * However, the data-sheet says the minimum time it takes the IC to be
	 * able to take inputs after power is applied, is 100 uS. I'd assume
	 * > 1 mS is enough.
	 */
	msleep(1);

	ret = regmap_reinit_cache(data->regmap, &bu27008_regmap);
	if (ret)
		dev_err(data->dev, "Failed to reinit reg cache\n");

	return ret;
}

static int bu27008_set_drdy_irq(struct bu27008_data *data, int state)
{
	return regmap_update_bits(data->regmap, BU27008_REG_MODE_CONTROL3,
				 BU27008_MASK_INT_EN, state);
}

static int bu27008_trigger_set_state(struct iio_trigger *trig,
				     bool state)
{
	struct bu27008_data *data = iio_trigger_get_drvdata(trig);
	int ret;

	if (state)
		ret = bu27008_set_drdy_irq(data, BU27008_INT_EN);
	else
		ret = bu27008_set_drdy_irq(data, BU27008_INT_DIS);
	if (ret)
		dev_err(data->dev, "Failed to set trigger state\n");

	return ret;
}

static void bu27008_trigger_reenable(struct iio_trigger *trig)
{
	struct bu27008_data *data = iio_trigger_get_drvdata(trig);

	enable_irq(data->irq);
}

static const struct iio_trigger_ops bu27008_trigger_ops = {
	.set_trigger_state = bu27008_trigger_set_state,
	.reenable = bu27008_trigger_reenable,
};

static irqreturn_t bu27008_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *idev = pf->indio_dev;
	struct bu27008_data *data = iio_priv(idev);
	struct {
		__le16 chan[BU27008_NUM_HW_CHANS];
		s64 ts __aligned(8);
	} raw;
	int ret, dummy;

	memset(&raw, 0, sizeof(raw));

	/*
	 * After some measurements, it seems reading the
	 * BU27008_REG_MODE_CONTROL3 debounces the IRQ line
	 */
	ret = regmap_read(data->regmap, BU27008_REG_MODE_CONTROL3, &dummy);
	if (ret < 0)
		goto err_read;

	ret = regmap_bulk_read(data->regmap, BU27008_REG_DATA0_LO, &raw.chan,
			       sizeof(raw.chan));
	if (ret < 0)
		goto err_read;

	iio_push_to_buffers_with_timestamp(idev, &raw, pf->timestamp);
err_read:
	iio_trigger_notify_done(idev->trig);

	return IRQ_HANDLED;
}

static int bu27008_buffer_preenable(struct iio_dev *idev)
{
	struct bu27008_data *data = iio_priv(idev);

	return bu27008_meas_set(data, BU27008_MEAS_EN);
}

static int bu27008_buffer_postdisable(struct iio_dev *idev)
{
	struct bu27008_data *data = iio_priv(idev);

	return bu27008_meas_set(data, BU27008_MEAS_DIS);
}

static const struct iio_buffer_setup_ops bu27008_buffer_ops = {
	.preenable = bu27008_buffer_preenable,
	.postdisable = bu27008_buffer_postdisable,
};

static irqreturn_t bu27008_data_rdy_poll(int irq, void *private)
{
	/*
	 * The BU27008 keeps IRQ asserted until we read the VALID bit from
	 * a register. We need to keep the IRQ disabled until then.
	 */
	disable_irq_nosync(irq);
	iio_trigger_poll(private);

	return IRQ_HANDLED;
}

static int bu27008_setup_trigger(struct bu27008_data *data, struct iio_dev *idev)
{
	struct iio_trigger *itrig;
	char *name;
	int ret;

	ret = devm_iio_triggered_buffer_setup(data->dev, idev,
					      &iio_pollfunc_store_time,
					      bu27008_trigger_handler,
					      &bu27008_buffer_ops);
	if (ret)
		return dev_err_probe(data->dev, ret,
			     "iio_triggered_buffer_setup_ext FAIL\n");

	itrig = devm_iio_trigger_alloc(data->dev, "%sdata-rdy-dev%d",
				       idev->name, iio_device_id(idev));
	if (!itrig)
		return -ENOMEM;

	data->trig = itrig;

	itrig->ops = &bu27008_trigger_ops;
	iio_trigger_set_drvdata(itrig, data);

	name = devm_kasprintf(data->dev, GFP_KERNEL, "%s-bu27008",
			      dev_name(data->dev));

	ret = devm_request_irq(data->dev, data->irq,
			       &bu27008_data_rdy_poll,
			       0, name, itrig);
	if (ret)
		return dev_err_probe(data->dev, ret, "Could not request IRQ\n");

	ret = devm_iio_trigger_register(data->dev, itrig);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Trigger registration failed\n");

	/* set default trigger */
	idev->trig = iio_trigger_get(itrig);

	return 0;
}

static int bu27008_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct bu27008_data *data;
	struct regmap *regmap;
	unsigned int part_id, reg;
	struct iio_dev *idev;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &bu27008_regmap);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to initialize Regmap\n");

	idev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!idev)
		return -ENOMEM;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulator\n");

	data = iio_priv(idev);

	ret = regmap_read(regmap, BU27008_REG_SYSTEM_CONTROL, &reg);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to access sensor\n");

	part_id = FIELD_GET(BU27008_MASK_PART_ID, reg);

	if (part_id != BU27008_ID)
		dev_warn(dev, "unknown device 0x%x\n", part_id);

	ret = devm_iio_init_iio_gts(dev, BU27008_SCALE_1X, 0, bu27008_gains,
				    ARRAY_SIZE(bu27008_gains), bu27008_itimes,
				    ARRAY_SIZE(bu27008_itimes), &data->gts);
	if (ret)
		return ret;

	ret = devm_iio_init_iio_gts(dev, BU27008_SCALE_1X, 0, bu27008_gains_ir,
				    ARRAY_SIZE(bu27008_gains_ir), bu27008_itimes,
				    ARRAY_SIZE(bu27008_itimes), &data->gts_ir);
	if (ret)
		return ret;

	mutex_init(&data->mutex);
	data->regmap = regmap;
	data->dev = dev;
	data->irq = i2c->irq;

	idev->channels = bu27008_channels;
	idev->num_channels = ARRAY_SIZE(bu27008_channels);
	idev->name = "bu27008";
	idev->info = &bu27008_info;
	idev->modes = INDIO_DIRECT_MODE;
	idev->available_scan_masks = bu27008_scan_masks;

	ret = bu27008_chip_init(data);
	if (ret)
		return ret;

	if (i2c->irq) {
		ret = bu27008_setup_trigger(data, idev);
		if (ret)
			return ret;
	} else {
		dev_info(dev, "No IRQ, buffered mode disabled\n");
	}

	ret = devm_iio_device_register(dev, idev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to register iio device\n");

	return 0;
}

static const struct of_device_id bu27008_of_match[] = {
	{ .compatible = "rohm,bu27008" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu27008_of_match);

static struct i2c_driver bu27008_i2c_driver = {
	.driver = {
		.name = "bu27008",
		.of_match_table = bu27008_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = bu27008_probe,
};
module_i2c_driver(bu27008_i2c_driver);

MODULE_DESCRIPTION("ROHM BU27008 colour sensor driver");
MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_GTS_HELPER);
