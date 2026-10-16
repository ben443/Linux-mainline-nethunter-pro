// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung TSP protocol driver for the FTS5CU56A touchscreen.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#define FTS_READ_DEVICE_ID		0x22
#define FTS_ID0				0x48
#define FTS_ID1				0x36

#define FTS_READ_ONE_EVENT		0x60
#define FTS_READ_ALL_EVENT		0x61
#define FTS_CMD_CLEAR_ALL_EVENT		0x62
#define FTS_CMD_SET_TOUCHTYPE		0x30
#define FTS_TOUCHTYPE_DEFAULT		0x0061

#define FTS_EVENT_SIZE			16
#define FTS_FIFO_MAX			31
#define FTS_FINGER_MAX			10
#define FTS_MAX_COORD			4095

#define FTS_EV_COORDINATE		0
#define FTS_EV_STATUS			1
#define FTS_ACT_PRESS			1
#define FTS_ACT_MOVE			2
#define FTS_ACT_RELEASE			3
#define FTS_STYPE_INFO			2
#define FTS_INFO_READY			0x00

struct fts5cu56a {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator_bulk_data supplies[2];
	struct touchscreen_properties prop;
};

static int fts5cu56a_write(struct fts5cu56a *ts, const u8 *buf, int len)
{
	int ret = i2c_master_send(ts->client, buf, len);

	if (ret < 0)
		return ret;

	return ret == len ? 0 : -EIO;
}

static int fts5cu56a_read(struct fts5cu56a *ts, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[] = {
		{ .addr = ts->client->addr, .len = 1, .buf = &reg },
		{ .addr = ts->client->addr, .flags = I2C_M_RD,
		  .len = len, .buf = buf },
	};
	int ret = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int fts5cu56a_wait_ready(struct fts5cu56a *ts)
{
	u8 event[FTS_EVENT_SIZE];
	int ret, retry;

	for (retry = 0; retry < 50; retry++) {
		ret = fts5cu56a_read(ts, FTS_READ_ONE_EVENT, event,
				     sizeof(event));
		if (ret)
			return ret;

		if ((event[0] & 0x3) == FTS_EV_STATUS &&
		    ((event[0] >> 2) & 0xf) == FTS_STYPE_INFO &&
		    event[1] == FTS_INFO_READY)
			return 0;

		msleep(20);
	}

	return -ETIMEDOUT;
}

static int fts5cu56a_reset(struct fts5cu56a *ts)
{
	static const u8 command[] = { 0xfa, 0x20, 0x00, 0x00, 0x24, 0x81 };
	int ret;

	ret = fts5cu56a_write(ts, command, sizeof(command));
	if (ret)
		return ret;

	msleep(10);

	return fts5cu56a_wait_ready(ts);
}

static void fts5cu56a_report_event(struct fts5cu56a *ts, const u8 *event)
{
	u8 id = event[0] & 0x3;
	u8 slot, action;
	unsigned int x, y;

	if (id != FTS_EV_COORDINATE)
		return;

	slot = (event[0] >> 2) & 0xf;
	action = event[0] >> 6;
	if (slot >= FTS_FINGER_MAX)
		return;

	x = (event[1] << 4) | (event[3] >> 4);
	y = (event[2] << 4) | (event[3] & 0xf);

	input_mt_slot(ts->input, slot);

	switch (action) {
	case FTS_ACT_PRESS:
	case FTS_ACT_MOVE:
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, event[4]);
		input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, event[5]);
		input_report_abs(ts->input, ABS_MT_PRESSURE,
				 event[6] & 0x3f);
		break;
	case FTS_ACT_RELEASE:
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		break;
	}
}

static irqreturn_t fts5cu56a_irq_handler(int irq, void *data)
{
	struct fts5cu56a *ts = data;
	u8 events[FTS_FIFO_MAX * FTS_EVENT_SIZE];
	unsigned int left, i;
	int ret;

	ret = fts5cu56a_read(ts, FTS_READ_ONE_EVENT, events, FTS_EVENT_SIZE);
	if (ret)
		return IRQ_HANDLED;

	left = min_t(unsigned int, events[7] & 0x1f, FTS_FIFO_MAX - 1);
	if (left) {
		ret = fts5cu56a_read(ts, FTS_READ_ALL_EVENT,
				     events + FTS_EVENT_SIZE,
				     left * FTS_EVENT_SIZE);
		if (ret)
			left = 0;
	}

	for (i = 0; i <= left; i++)
		fts5cu56a_report_event(ts, events + i * FTS_EVENT_SIZE);

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);

	return IRQ_HANDLED;
}

static int fts5cu56a_hw_init(struct fts5cu56a *ts)
{
	static const u8 clear_events = FTS_CMD_CLEAR_ALL_EVENT;
	static const u8 touchtype[] = {
		FTS_CMD_SET_TOUCHTYPE,
		FTS_TOUCHTYPE_DEFAULT & 0xff,
		FTS_TOUCHTYPE_DEFAULT >> 8,
	};
	static const u8 int_on[] = { 0xa4, 0x01, 0x01 };
	static const u8 scan_on[] = { 0xa0, 0x00, 0x01 };
	u8 id[5];
	int ret;

	ret = fts5cu56a_read(ts, FTS_READ_DEVICE_ID, id, sizeof(id));
	if (ret)
		return dev_err_probe(&ts->client->dev, ret,
				     "failed to read controller ID\n");

	if (id[2] != FTS_ID0 || id[3] != FTS_ID1)
		return dev_err_probe(&ts->client->dev, -ENODEV,
				     "unexpected controller ID %*ph\n",
				     (int)sizeof(id), id);

	ret = fts5cu56a_reset(ts);
	if (ret)
		dev_warn(&ts->client->dev,
			 "controller reset not confirmed (%d)\n", ret);

	ret = fts5cu56a_write(ts, &clear_events, sizeof(clear_events));
	if (ret)
		return ret;
	ret = fts5cu56a_write(ts, touchtype, sizeof(touchtype));
	if (ret)
		return ret;
	ret = fts5cu56a_write(ts, int_on, sizeof(int_on));
	if (ret)
		return ret;

	msleep(10);

	ret = fts5cu56a_write(ts, scan_on, sizeof(scan_on));
	if (ret)
		return ret;

	msleep(50);

	return 0;
}

static void fts5cu56a_power_off(void *data)
{
	struct fts5cu56a *ts = data;

	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static int fts5cu56a_probe(struct i2c_client *client)
{
	struct fts5cu56a *ts;
	int ret;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->supplies[0].supply = "avdd";
	ts->supplies[1].supply = "vdd";
	ret = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(ts->supplies),
				      ts->supplies);
	if (ret)
		return ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&client->dev, fts5cu56a_power_off, ts);
	if (ret)
		return ret;

	/* The controller needs time to boot its Samsung TSP firmware. */
	msleep(300);

	ret = fts5cu56a_hw_init(ts);
	if (ret)
		return ret;

	ts->input = devm_input_allocate_device(&client->dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "FTS5CU56A Touchscreen";
	ts->input->id.bustype = BUS_I2C;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, FTS_MAX_COORD,
			     0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, FTS_MAX_COORD,
			     0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 63, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->prop);

	ret = input_mt_init_slots(ts->input, FTS_FINGER_MAX,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	return devm_request_threaded_irq(&client->dev, client->irq, NULL,
					fts5cu56a_irq_handler, IRQF_ONESHOT,
					"fts5cu56a", ts);
}

static const struct of_device_id fts5cu56a_of_match[] = {
	{ .compatible = "samsung,fts5cu56a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fts5cu56a_of_match);

static struct i2c_driver fts5cu56a_driver = {
	.probe = fts5cu56a_probe,
	.driver = {
		.name = "fts5cu56a",
		.of_match_table = fts5cu56a_of_match,
	},
};
module_i2c_driver(fts5cu56a_driver);

MODULE_DESCRIPTION("Samsung FTS5CU56A touchscreen driver");
MODULE_LICENSE("GPL");
