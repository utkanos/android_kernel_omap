/*
 * Copyright (C) 2009 Motorola, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/led-lm3530.h>
#include <linux/types.h>

struct lm3530_data {
	struct led_classdev led_dev;
	struct i2c_client *client;
	struct work_struct wq;
	struct workqueue_struct *working_queue;
	struct lm3530_platform_data *als_pdata;
	uint8_t mode;
	uint8_t last_requested_brightness;
	uint8_t last_gen_config;
	uint8_t zone;
};

int lm3530_read_reg(struct lm3530_data *als_data, unsigned reg,
		    unsigned int num_bytes, uint8_t *value)
{
	int error = 0;
	int i = 0;
	uint8_t dest_buffer;

	if (num_bytes <= 0) {
		pr_err("%s: invalid number of bytes to read: %d\n",
		       __func__, num_bytes);
		return -EINVAL;
	}

	if (!value) {
		pr_err("%s: invalid value pointer\n", __func__);
		return -EINVAL;
	}
	do {
		dest_buffer = reg;
		error = i2c_master_send(als_data->client, &dest_buffer, 1);
		if (error == 1) {
			error = i2c_master_recv(als_data->client,
						&dest_buffer, num_bytes);

		}
		if (error != num_bytes) {
			pr_err("%s: read[%i] failed: %d\n", __func__, i, error);
			msleep_interruptible(LD_LM3530_I2C_RETRY_DELAY);
		}
	} while ((error != num_bytes) && ((++i) < LD_LM3530_MAX_RW_RETRIES));
	if (error == num_bytes) {
		error = 0;
		*value = dest_buffer;
	}

	return error;
}

int lm3530_write_reg(struct lm3530_data *als_data, unsigned reg, uint8_t value)
{
	uint8_t buf[LD_LM3530_ALLOWED_W_BYTES] = { reg, value };
	int bytes;
	int i = 0;

	do {
		bytes = i2c_master_send(als_data->client, buf,
				      LD_LM3530_ALLOWED_W_BYTES);

		if (bytes != LD_LM3530_ALLOWED_W_BYTES) {
			pr_err("%s: write %d failed: %d\n", __func__, i, bytes);
			msleep_interruptible(LD_LM3530_I2C_RETRY_DELAY);
		}
	} while ((bytes != (LD_LM3530_ALLOWED_W_BYTES))
		 && ((++i) < LD_LM3530_MAX_RW_RETRIES));

	if (bytes < LD_LM3530_ALLOWED_W_BYTES) {
		pr_err("%s: i2c_master_send error\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static void ld_lm3530_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness value)
{
	int brightness = 0;
	int error = 0;
	struct lm3530_data *als_data =
	    container_of(led_cdev, struct lm3530_data, led_dev);

	als_data->last_requested_brightness = value;
	if (value == LED_OFF) {
		brightness = als_data->last_gen_config &
		    LD_LM3530_LAST_BRIGHTNESS_MASK;

	} else {
		switch (als_data->mode) {
		case AUTOMATIC:
			if ((value > LD_LM3530_OFF)
			    && (value <= LD_LM3530_155))
				brightness = als_data->als_pdata->zone_data_2;
			else if ((value >= LD_LM3530_156)
				 && value <= LD_LM3530_201)
				brightness = als_data->als_pdata->zone_data_3;
			else if ((value >= LD_LM3530_202)
				 && value <= LD_LM3530_FULL)
				brightness = als_data->als_pdata->zone_data_4;
			break;
		case MANUAL:
			error = lm3530_write_reg(als_data,
						 LM3530_BRIGHTNESS_CTRL_REG,
						 value / 2);
			if (error)
				pr_err ("%s:Failed to set brightness:%d\n",
				     __func__, error);
			brightness = als_data->als_pdata->zone_data_4;
			break;
		}
	}

	als_data->last_gen_config = brightness;
	error = lm3530_write_reg(als_data, LM3530_GEN_CONFIG, brightness);
	if (error)
		pr_err("%s:writing failed while setting brightness:%d\n",
		       __func__, error);

}
EXPORT_SYMBOL(ld_lm3530_brightness_set);

static ssize_t ld_lm3530_als_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = container_of(dev->parent, struct i2c_client,
						 dev);
	struct lm3530_data *als_data = i2c_get_clientdata(client);
	sprintf(buf, "%u\n", als_data->mode);
	return strlen(buf) + 1;
}

static ssize_t ld_lm3530_als_store(struct device *dev, struct device_attribute
				   *attr, const char *buf, size_t size)
{
	struct i2c_client *client = container_of(dev->parent, struct i2c_client,
						 dev);
	struct lm3530_data *als_data = i2c_get_clientdata(client);
	/* TODO: This should disable als if the user writes 1 = MANAUL */
	return als_data->mode;
}

static DEVICE_ATTR(als, 0644, ld_lm3530_als_show, ld_lm3530_als_store);

irqreturn_t ld_lm3530_irq_handler(int irq, void *dev)
{
	struct lm3530_data *als_data = dev;

	disable_irq(als_data->client->irq);
	queue_work(als_data->working_queue, &als_data->wq);

	return IRQ_HANDLED;
}

void ld_lm3530_work_queue(struct work_struct *work)
{
	int ret;
	struct lm3530_data *als_data =
	    container_of(work, struct lm3530_data, wq);

	ret = lm3530_read_reg(als_data,
			      LM3530_ALS_ZONE_REG, LD_LM3530_ALLOWED_R_BYTES,
			      &als_data->zone);
	if (ret != 0) {
		pr_err("%s:Unable to read ALS Zone read back: %d\n",
		       __func__, ret);

		enable_irq(als_data->client->irq);

		return;
	}

	als_data->zone = als_data->zone & LM3530_ALS_READ_MASK;

	/* TODO: This is where we would send the data to the other drivers
	   for other control of the LEDs */

	enable_irq(als_data->client->irq);
}

int ld_lm3530_init_registers(struct lm3530_data *als_data)
{
	if (lm3530_write_reg(als_data, LM3530_ALS_CONFIG,
				 als_data->als_pdata->als_config) ||
	    lm3530_write_reg(als_data, LM3530_BRIGHTNESS_RAMP_RATE,
				 als_data->als_pdata->brightness_ramp) ||
	    lm3530_write_reg(als_data, LM3530_ALS_RESISTOR_SELECT,
				 als_data->als_pdata->als_resistor_sel) ||
	    lm3530_write_reg(als_data, LM3530_ALS_ZB0_REG,
				 als_data->als_pdata->zone_boundary_0) ||
	    lm3530_write_reg(als_data, LM3530_ALS_ZB1_REG,
				 als_data->als_pdata->zone_boundary_1) ||
	    lm3530_write_reg(als_data, LM3530_ALS_ZB2_REG,
				 als_data->als_pdata->zone_boundary_2) ||
	    lm3530_write_reg(als_data, LM3530_ALS_ZB3_REG,
				 als_data->als_pdata->zone_boundary_3) ||
	    lm3530_write_reg(als_data, LM3530_ALS_Z0T_REG,
				 als_data->als_pdata->zone_target_0) ||
	    lm3530_write_reg(als_data, LM3530_ALS_Z1T_REG,
				 als_data->als_pdata->zone_target_1) ||
	    lm3530_write_reg(als_data, LM3530_ALS_Z2T_REG,
				 als_data->als_pdata->zone_target_2) ||
	    lm3530_write_reg(als_data, LM3530_ALS_Z3T_REG,
				 als_data->als_pdata->zone_target_3) ||
	    lm3530_write_reg(als_data, LM3530_ALS_Z4T_REG,
			     als_data->als_pdata->zone_target_4)) {
		    pr_err("%s:Register initialization failed\n", __func__);
		    return -EINVAL;
	}
	return 0;
}

static int ld_lm3530_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct lm3530_platform_data *pdata = client->dev.platform_data;
	struct lm3530_data *als_data;
	int error = 0;

	if (pdata == NULL) {
		pr_err("%s: platform data required\n", __func__);
		return -ENODEV;
	} else if (!client->irq) {
		pr_err("%s: polling mode currently not supported\n", __func__);
		return -ENODEV;
	}
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s:I2C_FUNC_I2C not supported\n", __func__);
		return -ENODEV;
	}

	als_data = kzalloc(sizeof(struct lm3530_data), GFP_KERNEL);
	if (als_data == NULL) {
		error = -ENOMEM;
		goto err_alloc_data_failed;
	}

	als_data->client = client;
	als_data->als_pdata = pdata;
	als_data->mode = AUTOMATIC;
	als_data->zone = LM3530_ALS_ZONE0;

	als_data->led_dev.name = LD_LM3530_LED_DEV;
	als_data->led_dev.brightness_set = ld_lm3530_brightness_set;

	als_data->working_queue = create_singlethread_workqueue("als_wq");
	INIT_WORK(&als_data->wq, ld_lm3530_work_queue);

	error = request_irq(als_data->client->irq, ld_lm3530_irq_handler,
			    IRQF_TRIGGER_FALLING, LD_LM3530_NAME, als_data);
	if (error != 0) {
		pr_err("%s: irq request failed: %d\n", __func__, error);
		error = -ENODEV;
		goto err_req_irq_failed;
	}

	i2c_set_clientdata(client, als_data);

	error = ld_lm3530_init_registers(als_data);
	if (error < 0) {
		pr_err("%s: Register Initialization failed: %d\n",
		       __func__, error);
		error = -ENODEV;
		goto err_reg_init_failed;
	}

	error = lm3530_write_reg(als_data, LM3530_GEN_CONFIG,
				 pdata->gen_config);
	if (error) {
		pr_err("%s:Initialize Gen Config Reg failed %d\n",
		       __func__, error);
		error = -ENODEV;
		goto err_reg_init_failed;
	}

	error = led_classdev_register((struct device *)
				      &client->dev,
				      &als_data->led_dev);
	if (error < 0) {
		pr_err("%s: Register led class failed: %d\n", __func__, error);
		error = -ENODEV;
		goto err_class_reg_failed;
	}

	error = device_create_file(als_data->led_dev.dev, &dev_attr_als);
	if (error < 0) {
		pr_err("%s:File device creation failed: %d\n", __func__, error);
		error = -ENODEV;
		goto err_create_file_failed;
	}

	disable_irq(als_data->client->irq);
	queue_work(als_data->working_queue, &als_data->wq);

	return 0;
err_create_file_failed:
	led_classdev_unregister(&als_data->led_dev);
err_class_reg_failed:
err_reg_init_failed:
	free_irq(als_data->client->irq, als_data);
err_req_irq_failed:
	destroy_workqueue(als_data->working_queue);
	kfree(als_data);
err_alloc_data_failed:
	return error;
}

static int ld_lm3530_remove(struct i2c_client *client)
{
	struct lm3530_data *als_data = i2c_get_clientdata(client);
	device_remove_file(als_data->led_dev.dev, &dev_attr_als);
	led_classdev_unregister(&als_data->led_dev);
	free_irq(als_data->client->irq, als_data);
	if (als_data->working_queue)
		destroy_workqueue(als_data->working_queue);
	kfree(als_data);
	return 0;
}

static const struct i2c_device_id lm3530_id[] = {
	{LD_LM3530_NAME, 0},
	{}
};

static struct i2c_driver ld_lm3530_i2c_driver = {
	.probe = ld_lm3530_probe,
	.remove = ld_lm3530_remove,
	.id_table = lm3530_id,
	.driver = {
		   .name = LD_LM3530_NAME,
		   .owner = THIS_MODULE,
	},
};

static int __init ld_lm3530_init(void)
{
	return i2c_add_driver(&ld_lm3530_i2c_driver);
}

static void __exit ld_lm3530_exit(void)
{
	i2c_del_driver(&ld_lm3530_i2c_driver);

}

module_init(ld_lm3530_init);
module_exit(ld_lm3530_exit);

MODULE_DESCRIPTION("Lighting driver for LM3530");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");