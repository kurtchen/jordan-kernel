/*
 * Copyright (C) 2007-2009 Motorola, Inc.
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

#include <asm/div64.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/cpcap.h>
#include <linux/spi/cpcap-regbits.h>
#include <linux/spi/spi.h>
#include <linux/time.h>
#include <linux/miscdevice.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#define CPCAP_BATT_IRQ_BATTDET 0x01
#define CPCAP_BATT_IRQ_OV      0x02
#define CPCAP_BATT_IRQ_CC_CAL  0x04
#define CPCAP_BATT_IRQ_ADCDONE 0x08
#define CPCAP_BATT_IRQ_MACRO   0x10

//#define USE_OWN_CALCULATE_METHOD

#ifdef USE_OWN_CALCULATE_METHOD
static u32 battery_old_cap = -1;
#define USE_OWN_CHARGING_METHOD
#define BATTERY_DEBUG
#include "cpcap_charge_table.h"
#endif
#ifndef USE_OWN_CHARGING_METHOD

static int cpcap_batt_ioctl(struct inode *inode,
			    struct file *file,
			    unsigned int cmd,
			    unsigned long arg);
static unsigned int cpcap_batt_poll(struct file *file, poll_table *wait);
static int cpcap_batt_open(struct inode *inode, struct file *file);
static ssize_t cpcap_batt_read(struct file *file, char *buf, size_t count,
			       loff_t *ppos);
#endif
static int cpcap_batt_probe(struct platform_device *pdev);
static int cpcap_batt_remove(struct platform_device *pdev);
static int cpcap_batt_resume(struct platform_device *pdev);
struct cpcap_batt_ps {
	struct power_supply batt;
	struct power_supply ac;
	struct power_supply usb;
	struct cpcap_device *cpcap;
	struct cpcap_batt_data batt_state;
	struct cpcap_batt_ac_data ac_state;
	struct cpcap_batt_usb_data usb_state;
	struct cpcap_adc_request req;
	struct mutex lock;
	char irq_status;
	char data_pending;
	wait_queue_head_t wait;
	char async_req_pending;
	unsigned long last_run_time;
	bool no_update;
#ifdef USE_OWN_CALCULATE_METHOD
	unsigned int battery_stats_counter[4]; /* Battery stats for past seconds */
#endif
};
#ifndef USE_OWN_CHARGING_METHOD
static const struct file_operations batt_fops = {
	.owner = THIS_MODULE,
	.open = cpcap_batt_open,
	.ioctl = cpcap_batt_ioctl,
	.read = cpcap_batt_read,
	.poll = cpcap_batt_poll,
};

static struct miscdevice batt_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "cpcap_batt",
	.fops	= &batt_fops,
};
#endif
static enum power_supply_property cpcap_batt_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_COUNTER
};

static enum power_supply_property cpcap_batt_ac_props[] =
{
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_MODEL_NAME
};

static enum power_supply_property cpcap_batt_usb_props[] =
{
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_MODEL_NAME
};

static struct platform_driver cpcap_batt_driver = {
	.probe		= cpcap_batt_probe,
	.remove		= cpcap_batt_remove,
	.resume		= cpcap_batt_resume,
	.driver		= {
		.name	= "cpcap_battery",
		.owner	= THIS_MODULE,
	},
};

static struct cpcap_batt_ps *cpcap_batt_sply;
#ifdef USE_OWN_CALCULATE_METHOD
static struct task_struct * batt_task;
static int cpcap_batt_status(struct cpcap_batt_ps *sply);
static int cpcap_batt_counter(struct cpcap_batt_ps *sply);
static int cpcap_batt_value(struct cpcap_batt_ps *sply, int value);
#endif

void cpcap_batt_irq_hdlr(enum cpcap_irqs irq, void *data)
{
	struct cpcap_batt_ps *sply = data;

	mutex_lock(&sply->lock);
	sply->data_pending = 1;

	switch (irq) {
	case CPCAP_IRQ_BATTDETB:
		sply->irq_status |= CPCAP_BATT_IRQ_BATTDET;
		cpcap_irq_unmask(sply->cpcap, irq);
		break;

	case CPCAP_IRQ_VBUSOV:
		sply->irq_status |=  CPCAP_BATT_IRQ_OV;
		cpcap_irq_unmask(sply->cpcap, irq);
		break;

	case CPCAP_IRQ_CC_CAL:
		sply->irq_status |= CPCAP_BATT_IRQ_CC_CAL;
		cpcap_irq_unmask(sply->cpcap, irq);
		break;

	case CPCAP_IRQ_UC_PRIMACRO_7:
	case CPCAP_IRQ_UC_PRIMACRO_8:
	case CPCAP_IRQ_UC_PRIMACRO_9:
	case CPCAP_IRQ_UC_PRIMACRO_10:
	case CPCAP_IRQ_UC_PRIMACRO_11:
		sply->irq_status |= CPCAP_BATT_IRQ_MACRO;
		break;
	default:
		break;
	}

	mutex_unlock(&sply->lock);

	wake_up_interruptible(&sply->wait);
}

void cpcap_batt_adc_hdlr(struct cpcap_device *cpcap, void *data)
{
	struct cpcap_batt_ps *sply = data;
	mutex_lock(&sply->lock);

	sply->async_req_pending = 0;

	sply->data_pending = 1;

	sply->irq_status |= CPCAP_BATT_IRQ_ADCDONE;

	mutex_unlock(&sply->lock);

	wake_up_interruptible(&sply->wait);
}
#ifndef USE_OWN_CHARGING_METHOD
static int cpcap_batt_open(struct inode *inode, struct file *file)
{
	file->private_data = cpcap_batt_sply;
	return 0;
}

static unsigned int cpcap_batt_poll(struct file *file, poll_table *wait)
{
	struct cpcap_batt_ps *sply = file->private_data;
	unsigned int ret = 0;

	poll_wait(file, &sply->wait, wait);

	if (sply->data_pending)
		ret = (POLLIN | POLLRDNORM);

	return ret;
}

static ssize_t cpcap_batt_read(struct file *file,
			       char *buf, size_t count, loff_t *ppos)
{
	struct cpcap_batt_ps *sply = file->private_data;
	int ret = -EFBIG;
	unsigned long long temp;

	if (count >= sizeof(char)) {
		mutex_lock(&sply->lock);
		if (!copy_to_user((void *)buf, (void *)&sply->irq_status,
				  sizeof(sply->irq_status)))
			ret = sizeof(sply->irq_status);
		else
			ret = -EFAULT;
		sply->data_pending = 0;
		temp = sched_clock();
		do_div(temp, NSEC_PER_SEC);
		sply->last_run_time = (unsigned long)temp;

		sply->irq_status = 0;
		mutex_unlock(&sply->lock);
	}

	return ret;
}

static int cpcap_batt_ioctl(struct inode *inode,
			    struct file *file,
			    unsigned int cmd,
			    unsigned long arg)
{
	int ret = 0;
	int i;
	struct cpcap_batt_ps *sply = file->private_data;
	struct cpcap_adc_request *req_async = &sply->req;
	struct cpcap_adc_request req;
	struct cpcap_adc_us_request req_us;
	struct spi_device *spi = sply->cpcap->spi;
	struct cpcap_platform_data *data = spi->controller_data;

	switch (cmd) {
	case CPCAP_IOCTL_BATT_DISPLAY_UPDATE:
#ifndef USE_OWN_CALCULATE_METHOD
		if (sply->no_update)
#endif
			return 0;
		if (copy_from_user((void *)&sply->batt_state,
				   (void *)arg, sizeof(struct cpcap_batt_data)))
			return -EFAULT;
		power_supply_changed(&sply->batt);
		if (data->batt_changed)
			data->batt_changed(&sply->batt, &sply->batt_state);
		break;

	case CPCAP_IOCTL_BATT_ATOD_ASYNC:
		mutex_lock(&sply->lock);
		if (!sply->async_req_pending) {
			if (copy_from_user((void *)&req_us, (void *)arg,
					   sizeof(struct cpcap_adc_us_request)
					   )) {
				mutex_unlock(&sply->lock);
				return -EFAULT;
			}

			req_async->format = req_us.format;
			req_async->timing = req_us.timing;
			req_async->type = req_us.type;
			req_async->callback = cpcap_batt_adc_hdlr;
			req_async->callback_param = sply;

			ret = cpcap_adc_async_read(sply->cpcap, req_async);
			if (!ret)
				sply->async_req_pending = 1;
		} else {
			ret = -EAGAIN;
		}
		mutex_unlock(&sply->lock);

		break;

	case CPCAP_IOCTL_BATT_ATOD_SYNC:
		if (copy_from_user((void *)&req_us, (void *)arg,
				   sizeof(struct cpcap_adc_us_request)))
			return -EFAULT;

		req.format = req_us.format;
		req.timing = req_us.timing;
		req.type = req_us.type;

		ret = cpcap_adc_sync_read(sply->cpcap, &req);
		if (ret)
			return ret;
		req_us.status = req.status;

		for (i = 0; i < CPCAP_ADC_BANK0_NUM; i++)
			req_us.result[i] = req.result[i];

		if (copy_to_user((void *)arg, (void *)&req_us,
				 sizeof(struct cpcap_adc_us_request)))
			return -EFAULT;
		break;

	case CPCAP_IOCTL_BATT_ATOD_READ:
		req_us.format = req_async->format;
		req_us.timing = req_async->timing;
		req_us.type = req_async->type;
		req_us.status = req_async->status;
		for (i = 0; i < CPCAP_ADC_BANK0_NUM; i++)
			req_us.result[i] = req_async->result[i];

		if (copy_to_user((void *)arg, (void *)&req_us,
				 sizeof(struct cpcap_adc_us_request)))
			return -EFAULT;
		break;

	default:
		return -ENOTTY;
		break;
	}

	return ret;
}
#endif
static char *cpcap_batt_ac_models[] = {
	"none", "charger"
};

static int cpcap_batt_ac_get_property(struct power_supply *psy,
				      enum power_supply_property psp,
				      union power_supply_propval *val)
{
	int ret = 0;
	struct cpcap_batt_ps *sply = container_of(psy, struct cpcap_batt_ps,
						 ac);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = sply->ac_state.online;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = cpcap_batt_ac_models[sply->ac_state.online];
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static char *cpcap_batt_usb_models[] = {
	"none", "usb", "factory"
};

static int cpcap_batt_usb_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	int ret = 0;
	struct cpcap_batt_ps *sply = container_of(psy, struct cpcap_batt_ps,
						 usb);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = sply->usb_state.online;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = sply->usb_state.current_now;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = cpcap_batt_usb_models[sply->usb_state.model];
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int cpcap_batt_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	int ret = 0;
	struct cpcap_batt_ps *sply = container_of(psy, struct cpcap_batt_ps,
						  batt);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
#ifdef USE_OWN_CALCULATE_METHOD
		val->intval = cpcap_batt_status(sply);
#else
		val->intval = sply->batt_state.status;
#endif
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = sply->batt_state.health;
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = sply->batt_state.present;
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
#ifdef USE_OWN_CALCULATE_METHOD
		val->intval = cpcap_batt_counter(sply);
#else
		val->intval = sply->batt_state.capacity;
#endif
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
#ifdef USE_OWN_CALCULATE_METHOD
		val->intval = cpcap_batt_value(sply, CPCAP_ADC_BATTP)*1000;
#else
		val->intval = sply->batt_state.batt_volt;
#endif
		break;

	case POWER_SUPPLY_PROP_TEMP:
#ifdef USE_OWN_CALCULATE_METHOD
		val->intval = (cpcap_batt_value(sply, CPCAP_ADC_AD3)-273)*10;
#else
		val->intval = sply->batt_state.batt_temp;
#endif
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = sply->batt_state.batt_full_capacity;
		break;

	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
#ifdef USE_OWN_CALCULATE_METHOD
		val->intval = cpcap_batt_counter(sply);
#else
		val->intval = sply->batt_state.batt_capacity_one;
#endif
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
#ifdef USE_OWN_CALCULATE_METHOD
static int cpcap_batt_status(struct cpcap_batt_ps *sply) {
	int amperage = 0;

	amperage = cpcap_batt_value(sply, CPCAP_ADC_CHG_ISENSE);
	if (sply->usb_state.online == 1 || sply->ac_state.online == 1) {
#ifdef USE_OWN_CHARGING_METHOD
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_CRM, 949, 949);
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCD0, 186, 186);
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCC2 , 16758, 16758);
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_UCTM, 0, CPCAP_BIT_UCTM);
#endif
		if (amperage < 100 && amperage > 50) {
			printk("Your charger not powerful, try reconnect or change charger, %d mA\n", amperage);
			return POWER_SUPPLY_STATUS_DISCHARGING;
		}
		if (cpcap_batt_counter(sply) > 95)
			return POWER_SUPPLY_STATUS_FULL;
		else
			return POWER_SUPPLY_STATUS_CHARGING;
        } else {
#ifdef USE_OWN_CHARGING_METHOD
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_CRM, 944, 944);
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCC2 , 310, 310);
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCD0, 0, 0);
		cpcap_regacc_write(sply->cpcap, CPCAP_REG_UCTM, 0, CPCAP_BIT_UCTM);
#endif
		return POWER_SUPPLY_STATUS_DISCHARGING;
	}
}

static int cpcap_batt_value(struct cpcap_batt_ps *sply, int value) {
	struct cpcap_adc_request req;

	req.format = CPCAP_ADC_FORMAT_CONVERTED;
	req.timing = CPCAP_ADC_TIMING_IMM;
	req.type = CPCAP_ADC_TYPE_BANK_0;
 
	cpcap_adc_sync_read(sply->cpcap, &req);

        return req.result[value];
}

static int cpcap_batt_counter(struct cpcap_batt_ps *sply) {
	int i, volt_batt;
	u32 cap = 0;
	volt_batt = cpcap_batt_value(sply, CPCAP_ADC_BATTP);
#ifdef BATTERY_DEBUG
	printk("%s: batt_vol=%d\n",__func__, volt_batt);
#endif
	for (i=0; i < ARRAY_SIZE(tbl); i++) {
		if (volt_batt <= 3500) {
			cap = 0;
			break;
		}
		if (volt_batt >= 4181) {
			cap = 100;
			break;
		}
		if (volt_batt >= tbl[i].volt_batt) {
			if (i == (ARRAY_SIZE(tbl)-1)) {
				cap = 99;
				break;
			}
			continue;
	
	}

                /* Prevent Percentage from going up again */
		if (battery_old_cap  == -1) {
			battery_old_cap = tbl[i].capacity;
		} else if (battery_old_cap < tbl[i].capacity && 
				(sply->usb_state.online == 1 || sply->ac_state.online == 1)) {
			cap = battery_old_cap;
#ifdef BATTERY_DEBUG
			printk("if2: tbl[i].capacity %d,  battery_old_cap %d\n", tbl[i].capacity, battery_old_cap);
#endif
		} else { 
 			battery_old_cap = tbl[i].capacity;
			cap = battery_old_cap;
		}

	break;
	}
#ifdef BATTERY_DEBUG
	printk("%s: capacity=%d\n",__func__,cap);
#endif
	return cap;
}


void delay_ms(__u32 t)
{
    __u32 timeout = t*HZ/1000;
    	
    set_current_state(TASK_INTERRUPTIBLE);
    schedule_timeout(timeout);
}

#endif
#ifdef USE_OWN_CHARGING_METHOD
static void cpcap_batt_phasing(void) {
	struct cpcap_batt_ps *sply = cpcap_batt_sply;
        struct cpcap_adc_phase phase;
	/*****Battery Phasing start ****/

//CPCAP_ADC_BATTI_ADC
	phase.offset_batti = 0;
	phase.slope_batti = 128;
//CPCAP_ADC_CHG_ISENSE
	phase.offset_chrgi = 0;
	phase.slope_chrgi = 128;
//CPCAP_ADC_BATTP
	phase.offset_battp = 0;
	phase.slope_battp = 128;
//CPCAP_ADC_BPLUS_AD4
	phase.offset_bp = 0;
	phase.slope_bp = 128;
//CPCAP_ADC_AD0_BATTDETB
	phase.offset_battt = 0;
	phase.slope_battt = 128;
//CPCAP_ADC_VBUS
	phase.offset_chrgv = 128;
	phase.slope_chrgv = 128;

	cpcap_adc_phase(sply->cpcap, &phase);
	/*****Battery Phasing end ****/

//For start Macros 7 we need phasing.
	cpcap_uc_start(sply->cpcap, CPCAP_MACRO_7);
	//cpcap_uc_stop(sply->cpcap, CPCAP_MACRO_8);
	cpcap_uc_start(sply->cpcap, CPCAP_MACRO_9);
	//cpcap_uc_stop(sply->cpcap, CPCAP_MACRO_10);
	cpcap_uc_start(sply->cpcap, CPCAP_MACRO_12);

	cpcap_regacc_write(sply->cpcap, CPCAP_REG_CCM, 1002, 1002);  // Always == 1002
	cpcap_regacc_write(sply->cpcap, CPCAP_REG_CRM, 944, 944); // Charger off == 944, Charger on = 949
	cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCC2, 310, 310); // Charger off == 310, Charger on = 16758
	cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCAL1, 504, 504); // Always == 504
	cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCAL2, 506, 506); // Always == 506
	cpcap_regacc_write(sply->cpcap, CPCAP_REG_ADCD0, 0, 0); // Charger off == 0, Charger on = 186/187
	cpcap_regacc_write(sply->cpcap, CPCAP_REG_UCTM, 0, CPCAP_BIT_UCTM);  /* UC Turbo Mode - Always Disabled in battd*/  
}
#endif
#ifdef BATTERY_DEBUG
static void cpcap_batt_dump(struct cpcap_batt_ps *sply) {
	int i;
	struct cpcap_adc_request req;
	struct cpcap_adc_us_request req_us;
	unsigned short value;

	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCC1, &value);
	printk("CPCAP_REG_CCC1 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CRM, &value);
	printk("CPCAP_REG_CRM %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCCC2, &value);
	printk("CPCAP_REG_CCCC2 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCM, &value);
	printk("CPCAP_REG_CCM %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCA1, &value);
	printk("CPCAP_REG_CCA1 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCA2, &value);
	printk("CPCAP_REG_CCA2 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCO, &value);
	printk("CPCAP_REG_CC0 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_CCI, &value);
	printk("CPCAP_REG_CCI %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_USBC1, &value);
	printk("CPCAP_REG_USBC1 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_USBC2, &value);
	printk("CPCAP_REG_USBC2 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_USBC2, &value);
	printk("CPCAP_REG_USBC3 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_UCTM, &value);
	printk("CPCAP_REG_UCTM %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCC1, &value);
	printk("CPCAP_REG_ADCC1 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCC2, &value);
	printk("CPCAP_REG_ADCC2 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD0, &value);
	printk("CPCAP_REG_ADCD0 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD1, &value);
	printk("CPCAP_REG_ADCD1 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD2, &value);
	printk("CPCAP_REG_ADCD2 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD3, &value);
	printk("CPCAP_REG_ADCD3 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD4, &value);
	printk("CPCAP_REG_ADCD4 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD5, &value);
	printk("CPCAP_REG_ADCD5 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD6, &value);
	printk("CPCAP_REG_ADCD6 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCD7, &value);
	printk("CPCAP_REG_ADCD7 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCAL1, &value);
	printk("CPCAP_REG_ADCAL1 %x  == %d\n",value,value);
	cpcap_regacc_read(sply->cpcap, CPCAP_REG_ADCAL2, &value);
	printk("CPCAP_REG_ADCAL2 %x  == %d\n",value,value);

	req.format = CPCAP_ADC_FORMAT_CONVERTED;
	req.timing = CPCAP_ADC_TIMING_IMM;
	req.type = CPCAP_ADC_TYPE_BANK_0;
	cpcap_adc_sync_read(sply->cpcap, &req);

	for (i = 0; i < CPCAP_ADC_BANK0_NUM; i++)
		req_us.result[i] = req.result[i];

	printk("Dump of CPCAP_ADC_BANK0_NUM:\n CPCAP_ADC_VBUS:%d\n CPCAP_ADC_AD3:%d\n \
CPCAP_ADC_BATTP:%d\n CPCAP_ADC_BPLUS_AD4:%d\n CPCAP_ADC_CHG_ISENSE:%d\n \
CPCAP_ADC_BATTI_ADC:%d\n CPCAP_ADC_USB_ID:%d\n CPCAP_ADC_AD0_BATTDETB:%d\n",
	req_us.result[CPCAP_ADC_VBUS],req_us.result[CPCAP_ADC_AD3],req_us.result[CPCAP_ADC_BATTP],
	req_us.result[CPCAP_ADC_BPLUS_AD4],req_us.result[CPCAP_ADC_CHG_ISENSE],
	req_us.result[CPCAP_ADC_BATTI_ADC],req_us.result[CPCAP_ADC_USB_ID], req_us.result[CPCAP_ADC_AD0_BATTDETB]);
}
#endif

#ifdef USE_OWN_CALCULATE_METHOD
static int cpcap_batt_update(void* arg) {
	struct cpcap_batt_ps *sply = cpcap_batt_sply;
	while(1) {
		power_supply_changed(&sply->batt);
#ifdef BATTERY_DEBUG
		cpcap_batt_dump(sply);
#endif
		delay_ms(20000);
	}
	return 0;
}
#endif
static int cpcap_batt_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct cpcap_batt_ps *sply;
	if (pdev->dev.platform_data == NULL) {
		dev_err(&pdev->dev, "no platform_data\n");
		ret = -EINVAL;
		goto prb_exit;
	}

	sply = kzalloc(sizeof(struct cpcap_batt_ps), GFP_KERNEL);
	if (sply == NULL) {
		ret = -ENOMEM;
		goto prb_exit;
	}

	sply->cpcap = pdev->dev.platform_data;
	mutex_init(&sply->lock);
	init_waitqueue_head(&sply->wait);
	sply->batt_state.status	= POWER_SUPPLY_STATUS_DISCHARGING; //Set discharging by default.  //POWER_SUPPLY_STATUS_UNKNOWN;
	sply->batt_state.health	= POWER_SUPPLY_HEALTH_GOOD;
	sply->batt_state.present = 1;
	sply->batt_state.capacity = 100;	/* Percentage */
	sply->batt_state.batt_volt = 4200000;	/* uV */
	sply->batt_state.batt_temp = 230;	/* tenths of degrees Celsius */
	sply->batt_state.batt_full_capacity = 0;
	sply->batt_state.batt_capacity_one = 100;

	sply->ac_state.online = 0;

	sply->usb_state.online = 0;
	sply->usb_state.current_now = 0;
	sply->usb_state.model = CPCAP_BATT_USB_MODEL_NONE;

	sply->batt.properties = cpcap_batt_props;
	sply->batt.num_properties = ARRAY_SIZE(cpcap_batt_props);
	sply->batt.get_property = cpcap_batt_get_property;
	sply->batt.name = "battery";
	sply->batt.type = POWER_SUPPLY_TYPE_BATTERY;

	sply->ac.properties = cpcap_batt_ac_props;
	sply->ac.num_properties = ARRAY_SIZE(cpcap_batt_ac_props);
	sply->ac.get_property = cpcap_batt_ac_get_property;
	sply->ac.name = "ac";
	sply->ac.type = POWER_SUPPLY_TYPE_MAINS;

	sply->usb.properties = cpcap_batt_usb_props;
	sply->usb.num_properties = ARRAY_SIZE(cpcap_batt_usb_props);
	sply->usb.get_property = cpcap_batt_usb_get_property;
	sply->usb.name = "usb";
	sply->usb.type = POWER_SUPPLY_TYPE_USB;

	sply->no_update = false;

	ret = power_supply_register(&pdev->dev, &sply->ac);
	if (ret)
		goto prb_exit;
	ret = power_supply_register(&pdev->dev, &sply->batt);
	if (ret)
		goto unregac_exit;
	ret = power_supply_register(&pdev->dev, &sply->usb);
	if (ret)
		goto unregbatt_exit;
	platform_set_drvdata(pdev, sply);
	sply->cpcap->battdata = sply;
	cpcap_batt_sply = sply;
#ifndef USE_OWN_CHARGING_METHOD
	ret = misc_register(&batt_dev);
	if (ret)
		goto unregmisc_exit;
#endif
	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_VBUSOV,
				 cpcap_batt_irq_hdlr, sply);
	if (ret)
		goto unregirq_exit;
	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_BATTDETB,
				 cpcap_batt_irq_hdlr, sply);
	if (ret)
		goto unregirq_exit;
	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_CC_CAL,
				 cpcap_batt_irq_hdlr, sply);
	if (ret)
		goto unregirq_exit;

	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_7,
				 cpcap_batt_irq_hdlr, sply);
	cpcap_irq_mask(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_7);

	if (ret)
		goto unregirq_exit;

	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_8,
				 cpcap_batt_irq_hdlr, sply);
	cpcap_irq_mask(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_8);

	if (ret)
		goto unregirq_exit;

	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_9,
				 cpcap_batt_irq_hdlr, sply);
	cpcap_irq_mask(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_9);

	if (ret)
		goto unregirq_exit;

	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_10,
				 cpcap_batt_irq_hdlr, sply);
	cpcap_irq_mask(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_10);

	if (ret)
		goto unregirq_exit;

	ret = cpcap_irq_register(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_11,
				 cpcap_batt_irq_hdlr, sply);
	cpcap_irq_mask(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_11);


	if (ret)
		goto unregirq_exit;

	goto prb_exit;

unregirq_exit:
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_VBUSOV);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_BATTDETB);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_CC_CAL);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_7);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_8);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_9);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_10);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_11);
#ifndef USE_OWN_CHARGING_METHOD
unregmisc_exit:
	misc_deregister(&batt_dev);
	power_supply_unregister(&sply->usb);
#endif
unregbatt_exit:
	power_supply_unregister(&sply->batt);
unregac_exit:
	power_supply_unregister(&sply->ac);

prb_exit:
#ifdef USE_OWN_CALCULATE_METHOD
#ifdef USE_OWN_CHARGING_METHOD
	cpcap_batt_phasing();
#endif
        batt_task = kthread_create(cpcap_batt_update, (void*)0, "cpcap_batt_update");
	wake_up_process(batt_task);
#endif
	return ret;
}

static int cpcap_batt_remove(struct platform_device *pdev)
{
	struct cpcap_batt_ps *sply = platform_get_drvdata(pdev);

	power_supply_unregister(&sply->batt);
	power_supply_unregister(&sply->ac);
	power_supply_unregister(&sply->usb);
#ifndef USE_OWN_CHARGING_METHOD
	misc_deregister(&batt_dev);
#endif
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_VBUSOV);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_BATTDETB);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_CC_CAL);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_7);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_8);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_9);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_10);
	cpcap_irq_free(sply->cpcap, CPCAP_IRQ_UC_PRIMACRO_11);
	sply->cpcap->battdata = NULL;
	kfree(sply);

	return 0;
}

static int cpcap_batt_resume(struct platform_device *pdev)
{
	struct cpcap_batt_ps *sply = platform_get_drvdata(pdev);
	unsigned long cur_time;
	unsigned long long temp;
	temp = sched_clock();
	do_div(temp, NSEC_PER_SEC);
	cur_time = ((unsigned long)temp);
	if ((cur_time - sply->last_run_time) < 0)
		sply->last_run_time = 0;

	if ((cur_time - sply->last_run_time) > 50) {
		mutex_lock(&sply->lock);
		sply->data_pending = 1;
		sply->irq_status |= CPCAP_BATT_IRQ_MACRO;

		mutex_unlock(&sply->lock);

		wake_up_interruptible(&sply->wait);
	}

	return 0;
}

void cpcap_batt_set_ac_prop(struct cpcap_device *cpcap, int online)
{
	struct cpcap_batt_ps *sply = cpcap->battdata;
	struct spi_device *spi = cpcap->spi;
	struct cpcap_platform_data *data = spi->controller_data;

	if (sply != NULL) {
		sply->ac_state.online = online;
		power_supply_changed(&sply->ac);
		if (data->ac_changed)
			data->ac_changed(&sply->ac, &sply->ac_state);
	}
}
EXPORT_SYMBOL(cpcap_batt_set_ac_prop);

void cpcap_batt_set_usb_prop_online(struct cpcap_device *cpcap, int online,
				    enum cpcap_batt_usb_model model)
{
	struct cpcap_batt_ps *sply = cpcap->battdata;
	struct spi_device *spi = cpcap->spi;
	struct cpcap_platform_data *data = spi->controller_data;

	if (sply != NULL) {
		sply->usb_state.online = online;
		sply->usb_state.model = model;
		power_supply_changed(&sply->usb);
		if (data->usb_changed)
			data->usb_changed(&sply->usb, &sply->usb_state);
	}
}
EXPORT_SYMBOL(cpcap_batt_set_usb_prop_online);

void cpcap_batt_set_usb_prop_curr(struct cpcap_device *cpcap, unsigned int curr)
{
	struct cpcap_batt_ps *sply = cpcap->battdata;
	struct spi_device *spi = cpcap->spi;
	struct cpcap_platform_data *data = spi->controller_data;

	if (sply != NULL) {
		sply->usb_state.current_now = curr;
		power_supply_changed(&sply->usb);

		if (data->usb_changed)
			data->usb_changed(&sply->usb, &sply->usb_state);
	}
}
EXPORT_SYMBOL(cpcap_batt_set_usb_prop_curr);

static int __init cpcap_batt_init(void)
{
	return platform_driver_register(&cpcap_batt_driver);
}
subsys_initcall(cpcap_batt_init);

static void __exit cpcap_batt_exit(void)
{
	platform_driver_unregister(&cpcap_batt_driver);
}
module_exit(cpcap_batt_exit);

MODULE_ALIAS("platform:cpcap_batt");
MODULE_DESCRIPTION("CPCAP BATTERY driver");
MODULE_AUTHOR("Motorola, Quarx");
MODULE_LICENSE("GPL");
