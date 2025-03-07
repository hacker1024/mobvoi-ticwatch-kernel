/*
 * Copyright (C) 2016 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <linux/gpio.h>
#include <linux/module.h>
#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

#include "main.h"
#include "bl.h"
#include "comms.h"

#define SPI_TIMEOUT		65535
#define SPI_MIN_DMA		48

struct nanohub_spi_data {
	struct nanohub_data data;
	struct spi_device *device;
	struct semaphore spi_sem;
	int cs;
	uint16_t rx_length;
	uint16_t rx_offset;
#if defined(CONFIG_FB)
	struct notifier_block fb_notif;
#endif
};

static uint8_t bl_checksum(const uint8_t *bytes, int length)
{
	int i;
	uint8_t csum;

	if (length == 1) {
		csum = ~bytes[0];
	} else if (length > 1) {
		for (csum = 0, i = 0; i < length; i++)
			csum ^= bytes[i];
	} else {
		csum = 0xFF;
	}

	return csum;
}

static uint8_t spi_bl_write_data(const void *data, uint8_t *tx, int length)
{
	const struct nanohub_spi_data *spi_data = data;
	const struct nanohub_bl *bl = &spi_data->data.bl;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = length + 1,
		.tx_buf = bl->tx_buffer,
		.rx_buf = bl->rx_buffer,
		.cs_change = 1,
	};

	tx[length] = bl_checksum(tx, length);
	memcpy(bl->tx_buffer, tx, length + 1);

	spi_message_init_with_transfers(&msg, &xfer, 1);

	if (spi_sync_locked(spi_data->device, &msg) == 0)
		return bl->rx_buffer[length];
	else
		return CMD_NACK;
}

static uint8_t spi_bl_write_cmd(const void *data, uint8_t cmd)
{
	const struct nanohub_spi_data *spi_data = data;
	const struct nanohub_bl *bl = &spi_data->data.bl;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = 3,
		.tx_buf = bl->tx_buffer,
		.rx_buf = bl->rx_buffer,
		.cs_change = 1,
	};
	bl->tx_buffer[0] = CMD_SOF;
	bl->tx_buffer[1] = cmd;
	bl->tx_buffer[2] = ~cmd;

	spi_message_init_with_transfers(&msg, &xfer, 1);

	if (spi_sync_locked(spi_data->device, &msg) == 0)
		return CMD_ACK;
	else
		return CMD_NACK;
}

static uint8_t spi_bl_read_data(const void *data, uint8_t *rx, int length)
{
	const struct nanohub_spi_data *spi_data = data;
	const struct nanohub_bl *bl = &spi_data->data.bl;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = length + 1,
		.tx_buf = bl->tx_buffer,
		.rx_buf = bl->rx_buffer,
		.cs_change = 1,
	};
	memset(&bl->tx_buffer[0], 0x00, length + 1);

	spi_message_init_with_transfers(&msg, &xfer, 1);

	if (spi_sync_locked(spi_data->device, &msg) == 0) {
		memcpy(rx, &bl->rx_buffer[1], length);
		return CMD_ACK;
	} else {
		return CMD_NACK;
	}
}

static uint8_t spi_bl_read_ack(const void *data)
{
	const struct nanohub_spi_data *spi_data = data;
	const struct nanohub_bl *bl = &spi_data->data.bl;
	int32_t timeout = SPI_TIMEOUT;
	uint8_t ret;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = 1,
		.tx_buf = bl->tx_buffer,
		.rx_buf = bl->rx_buffer,
		.cs_change = 1,
	};
	bl->tx_buffer[0] = 0x00;

	spi_message_init_with_transfers(&msg, &xfer, 1);

	if (spi_sync_locked(spi_data->device, &msg) == 0) {
		do {
			spi_sync_locked(spi_data->device, &msg);
			timeout--;
			if (bl->rx_buffer[0] != CMD_ACK
			    && bl->rx_buffer[0] != CMD_NACK
			    && timeout % 256 == 0)
				schedule();
		} while (bl->rx_buffer[0] != CMD_ACK
			 && bl->rx_buffer[0] != CMD_NACK && timeout > 0);

		if (bl->rx_buffer[0] != CMD_ACK && bl->rx_buffer[0] != CMD_NACK
		    && timeout == 0)
			ret = CMD_NACK;
		else
			ret = bl->rx_buffer[0];

		bl->tx_buffer[0] = CMD_ACK;
		spi_sync_locked(spi_data->device, &msg);
		return ret;
	} else {
		return CMD_NACK;
	}
}

static int spi_bl_open(const void *data)
{
	const struct nanohub_spi_data *spi_data = data;
	int ret;

	spi_bus_lock(spi_data->device->master);
	spi_data->device->max_speed_hz = 8000000;
	spi_data->device->mode = SPI_MODE_0;
	spi_data->device->bits_per_word = 8;
	ret = spi_setup(spi_data->device);
	if (!ret)
		gpio_set_value(spi_data->cs, 0);

	return ret;
}

static void spi_bl_close(const void *data)
{
	const struct nanohub_spi_data *spi_data = data;

	gpio_set_value(spi_data->cs, 1);
	spi_bus_unlock(spi_data->device->master);
}

static uint8_t spi_bl_sync(const void *data)
{
	const struct nanohub_spi_data *spi_data = data;
	const struct nanohub_bl *bl = &spi_data->data.bl;
	int32_t timeout = SPI_TIMEOUT;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = 1,
		.tx_buf = bl->tx_buffer,
		.rx_buf = bl->rx_buffer,
		.cs_change = 1,
	};
	bl->tx_buffer[0] = CMD_SOF;

	spi_message_init_with_transfers(&msg, &xfer, 1);

	do {
		if (spi_sync_locked(spi_data->device, &msg) != 0)
			return CMD_NACK;
		timeout--;
		if (bl->rx_buffer[0] != CMD_SOF_ACK && timeout % 256 == 0)
			schedule();
	} while (bl->rx_buffer[0] != CMD_SOF_ACK && timeout > 0);

	if (bl->rx_buffer[0] == CMD_SOF_ACK)
		return bl->read_ack(data);
	else
		return CMD_NACK;
}

void nanohub_spi_bl_init(struct nanohub_spi_data *spi_data)
{
	struct nanohub_bl *bl = &spi_data->data.bl;

	bl->open = spi_bl_open;
	bl->sync = spi_bl_sync;
	bl->write_data = spi_bl_write_data;
	bl->write_cmd = spi_bl_write_cmd;
	bl->read_data = spi_bl_read_data;
	bl->read_ack = spi_bl_read_ack;
	bl->close = spi_bl_close;
}

int nanohub_spi_write(void *data, uint8_t *tx, int length, int timeout)
{
	struct nanohub_spi_data *spi_data = data;
	const struct nanohub_comms *comms = &spi_data->data.comms;
	int max_len = sizeof(struct nanohub_packet) + MAX_UINT8 +
		      sizeof(struct nanohub_packet_crc);
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = max_len + timeout,
		.tx_buf = comms->tx_buffer,
		.rx_buf = comms->rx_buffer,
		.cs_change = 1,
	};
	spi_data->rx_offset = max_len;
	spi_data->rx_length = max_len + timeout;
	memcpy(comms->tx_buffer, tx, length);
	memset(comms->tx_buffer + length, 0xFF, max_len + timeout - length);

	spi_message_init_with_transfers(&msg, &xfer, 1);

	if (spi_sync_locked(spi_data->device, &msg) == 0)
		return length;
	else
		return ERROR_NACK;
}

int nanohub_spi_read(void *data, uint8_t *rx, int max_length, int timeout)
{
	struct nanohub_spi_data *spi_data = data;
	struct nanohub_comms *comms = &spi_data->data.comms;
	const int min_size = sizeof(struct nanohub_packet) +
	    sizeof(struct nanohub_packet_crc);
	int i, ret;
	int offset = 0;
	struct nanohub_packet *packet = NULL;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = timeout,
		.tx_buf = comms->tx_buffer,
		.rx_buf = comms->rx_buffer,
		.cs_change = 1,
	};

	if (max_length < min_size)
		return ERROR_NACK;

	/* consume leftover bytes, if any */
	if (spi_data->rx_offset < spi_data->rx_length) {
		for (i = spi_data->rx_offset; i < spi_data->rx_length; i++) {
			if (comms->rx_buffer[i] != 0xFF) {
				offset = spi_data->rx_length - i;

				if (offset <
				    offsetof(struct nanohub_packet,
					     len) + sizeof(packet->len)) {
					memcpy(rx, &comms->rx_buffer[i],
					       offset);
					xfer.len =
					    min_size + MAX_UINT8 - offset;
					break;
				} else {
					packet =
					    (struct nanohub_packet *)&comms->
					    rx_buffer[i];
					if (offset < min_size + packet->len) {
						memcpy(rx, packet, offset);
						xfer.len =
						    min_size + packet->len -
						    offset;
						break;
					} else {
						memcpy(rx, packet,
						       min_size + packet->len);
						spi_data->rx_offset = i +
						     min_size + packet->len;
						return min_size + packet->len;
					}
				}
			}
		}
	}

	if (xfer.len != 1 && xfer.len < SPI_MIN_DMA)
		xfer.len = SPI_MIN_DMA;
	memset(comms->tx_buffer, 0xFF, xfer.len);

	spi_message_init_with_transfers(&msg, &xfer, 1);

	ret = spi_sync_locked(spi_data->device, &msg);
	if (ret == 0) {
		if (offset > 0) {
			packet = (struct nanohub_packet *)rx;
			if (offset + xfer.len > max_length)
				memcpy(&rx[offset], comms->rx_buffer,
					max_length - offset);
			else
				memcpy(&rx[offset], comms->rx_buffer, xfer.len);
			spi_data->rx_length = xfer.len;
			spi_data->rx_offset = min_size + packet->len - offset;
		} else {
			for (i = 0; i < xfer.len; i++) {
				if (comms->rx_buffer[i] != 0xFF) {
					spi_data->rx_length = xfer.len;

					if (xfer.len - i < min_size) {
						spi_data->rx_offset = i;
						break;
					} else {
						packet =
						    (struct nanohub_packet *)
						    &comms->rx_buffer[i];
						if (xfer.len - i <
						    min_size + packet->len) {
							packet = NULL;
							spi_data->rx_offset = i;
						} else {
							memcpy(rx, packet,
							       min_size +
							       packet->len);
							spi_data->rx_offset =
							    i + min_size +
							    packet->len;
						}
					}
					break;
				}
			}
		}
	}

	if (ret < 0)
		return ret;
	else if (!packet)
		return 0;
	else
		return min_size + packet->len;
}

static int nanohub_spi_open(void *data)
{
	struct nanohub_spi_data *spi_data = data;
	int ret;

	down(&spi_data->spi_sem);
	spi_bus_lock(spi_data->device->master);
	spi_data->device->max_speed_hz = 10000000;
	spi_data->device->mode = SPI_MODE_0;
	spi_data->device->bits_per_word = 8;
	ret = spi_setup(spi_data->device);
	if (!ret) {
		udelay(40);
		gpio_set_value(spi_data->cs, 0);
		udelay(30);
	}
	return ret;
}

static void nanohub_spi_close(void *data)
{
	struct nanohub_spi_data *spi_data = data;

	gpio_set_value(spi_data->cs, 1);
	spi_bus_unlock(spi_data->device->master);
	up(&spi_data->spi_sem);
	udelay(60);
}

void nanohub_spi_comms_init(struct nanohub_spi_data *spi_data)
{
	struct nanohub_comms *comms = &spi_data->data.comms;
	int max_len = sizeof(struct nanohub_packet) + MAX_UINT8 +
		      sizeof(struct nanohub_packet_crc);

	comms->seq = 1;
	comms->timeout_write = 544;
	comms->timeout_ack = 272;
	comms->timeout_reply = 512;
	comms->open = nanohub_spi_open;
	comms->close = nanohub_spi_close;
	comms->write = nanohub_spi_write;
	comms->read = nanohub_spi_read;

	max_len += comms->timeout_write;
	max_len = max(max_len, comms->timeout_ack);
	max_len = max(max_len, comms->timeout_reply);
	comms->tx_buffer = kmalloc(max_len, GFP_KERNEL | GFP_DMA);
	comms->rx_buffer = kmalloc(max_len, GFP_KERNEL | GFP_DMA);

	spi_data->rx_length = 0;
	spi_data->rx_offset = 0;

	sema_init(&spi_data->spi_sem, 1);
}

#if defined(CONFIG_FB)
/*******************************************************************************
*  Name: fb_notifier_callback
*  Brief:
*  Input:
*  Output:
*  Return:
*******************************************************************************/
static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct nanohub_spi_data *spi_data =
		container_of(self, struct nanohub_spi_data, fb_notif);
	int ret = 0;

	if (LCD_MUTEX_OFF == atomic_read(&(spi_data->data.lcd_mutex)))
		return 0;

	blank = evdata->data;

	if (event == FB_EVENT_BLANK && *blank == FB_BLANK_POWERDOWN)
		ret = __nanohub_send_AP_cmd(&spi_data->data, GPIO_CMD_AMBIENT);

	if (event == FB_EARLY_EVENT_BLANK &&
		(*blank == FB_BLANK_UNBLANK
		|| *blank == FB_BLANK_NORMAL
		|| *blank == FB_BLANK_VSYNC_SUSPEND))
		ret = __nanohub_send_AP_cmd(&spi_data->data, GPIO_CMD_NORMAL);

	return ret;
}
#endif

static int nanohub_spi_probe(struct spi_device *spi)
{
	struct nanohub_spi_data *spi_data;
	struct iio_dev *iio_dev;
	int error;

	iio_dev = iio_device_alloc(sizeof(struct nanohub_spi_data));

	iio_dev = nanohub_probe(&spi->dev, iio_dev);

	if (IS_ERR(iio_dev))
		return PTR_ERR(iio_dev);

	spi_data = iio_priv(iio_dev);

	spi_set_drvdata(spi, iio_dev);

	if (gpio_is_valid(spi_data->data.pdata->spi_cs_gpio)) {
		error =
		    gpio_request(spi_data->data.pdata->spi_cs_gpio,
				 "nanohub_spi_cs");
		if (error) {
			pr_err("nanohub: spi_cs_gpio request failed\n");
		} else {
			spi_data->cs = spi_data->data.pdata->spi_cs_gpio;
			gpio_direction_output(spi_data->cs, 1);
		}
	} else {
		pr_err("nanohub: spi_cs_gpio is not valid\n");
	}

	spi_data->device = spi;
	nanohub_spi_comms_init(spi_data);

	spi_data->data.bl.cmd_erase = CMD_ERASE;
	spi_data->data.bl.cmd_read_memory = CMD_READ_MEMORY;
	spi_data->data.bl.cmd_write_memory = CMD_WRITE_MEMORY;
	spi_data->data.bl.cmd_get_version = CMD_GET_VERSION;
	spi_data->data.bl.cmd_get_id = CMD_GET_ID;
	spi_data->data.bl.cmd_readout_protect = CMD_READOUT_PROTECT;
	spi_data->data.bl.cmd_readout_unprotect = CMD_READOUT_UNPROTECT;
	spi_data->data.bl.cmd_update_finished = CMD_UPDATE_FINISHED;
	nanohub_spi_bl_init(spi_data);

	nanohub_start(&spi_data->data);
#if defined(CONFIG_FB)
	spi_data->fb_notif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&spi_data->fb_notif))
		pr_err("nanohub: Unable to register fb_notifier:");
#endif
	return 0;
}

static int nanohub_spi_remove(struct spi_device *spi)
{
	struct nanohub_spi_data *spi_data;
	struct iio_dev *iio_dev;

	iio_dev = spi_get_drvdata(spi);
	spi_data = iio_priv(iio_dev);

	if (gpio_is_valid(spi_data->cs)) {
		gpio_direction_output(spi_data->cs, 1);
		gpio_free(spi_data->cs);
	}

#if defined(CONFIG_FB)
		if (fb_unregister_client(&spi_data->fb_notif))
			pr_err("Error occurred while unregistering fb_notifier.");
#endif

	return nanohub_remove(iio_dev);
}

static void nanohub_spi_shutdown(struct spi_device *spi)
{
	struct nanohub_spi_data *spi_data;
	struct iio_dev *iio_dev;

	iio_dev = spi_get_drvdata(spi);
	spi_data = iio_priv(iio_dev);

	nanohub_shutdown(iio_dev);
}


static int nanohub_spi_suspend(struct device *dev)
{
	struct iio_dev *iio_dev = spi_get_drvdata(to_spi_device(dev));
	struct nanohub_spi_data *spi_data = iio_priv(iio_dev);
	int ret;

	ret = nanohub_suspend(iio_dev);

	if (!ret) {
		ret = down_interruptible(&spi_data->spi_sem);
		if (ret)
			up(&spi_data->spi_sem);
	}

	return ret;
}

static int nanohub_spi_resume(struct device *dev)
{
	struct iio_dev *iio_dev = spi_get_drvdata(to_spi_device(dev));
	struct nanohub_spi_data *spi_data = iio_priv(iio_dev);

	up(&spi_data->spi_sem);

	return nanohub_resume(iio_dev);
}

static struct spi_device_id nanohub_spi_id[] = {
	{NANOHUB_NAME, 0},
	{},
};

static const struct dev_pm_ops nanohub_spi_pm_ops = {
	.suspend = nanohub_spi_suspend,
	.resume = nanohub_spi_resume,
};
#ifdef CONFIG_OF
static struct of_device_id nanohub_match_table[] = {
	{ .compatible = "nanohub,nanohub_spi",},
	{ },
};
#else
#define nanohub_match_table NULL
#endif

static struct spi_driver nanohub_spi_driver = {
	.driver = {
		   .name = NANOHUB_NAME,
		   .owner = THIS_MODULE,
		   .pm = &nanohub_spi_pm_ops,
		   .of_match_table = nanohub_match_table,
		   },
	.probe = nanohub_spi_probe,
	.remove = nanohub_spi_remove,
	.shutdown = nanohub_spi_shutdown,
	.id_table = nanohub_spi_id,
};

int __init nanohub_spi_init(void)
{
	return spi_register_driver(&nanohub_spi_driver);
}

void nanohub_spi_cleanup(void)
{
	spi_unregister_driver(&nanohub_spi_driver);
}

MODULE_DEVICE_TABLE(spi, nanohub_spi_id);
