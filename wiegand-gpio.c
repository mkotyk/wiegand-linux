/* wiegand-gpio.c 
 * 
 * Wiegand driver using GPIO an interrupts. 
 *
 * Modified Nov 2020 mkotyk for RPI GPIO
 */

/* Standard headers for LKMs */
#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/timer.h>

#include <linux/tty.h>      /* console_print() interface */
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#include <asm/irq.h>
#include <linux/gpio.h>

#define MAX_WIEGAND_BYTES 6

#define W0		17
#define W1		18
#define LED		19

static struct wiegand
{
  int startParity;
  int readNum;
  char buffer[MAX_WIEGAND_BYTES];
  int currentBit;
  
  unsigned int lastFacilityCode;
  unsigned int lastCardNumber;
} 
wiegand; 

static struct timer_list timer;

static ssize_t wiegandShow(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
  return sprintf(
    buf, "%.5d:%.3d:%.5d\n", 
    wiegand.readNum,
    wiegand.lastFacilityCode,
    wiegand.lastCardNumber
  );
}

static struct kobj_attribute wiegand_attribute = __ATTR(read, 0660, wiegandShow, NULL);

static struct attribute *attrs[] = 
{
  &wiegand_attribute.attr,
  NULL,   /* need to NULL terminate the list of attributes */
};

static struct attribute_group attr_group = 
{
  .attrs = attrs,
};

static struct kobject *wiegandKObj;

irqreturn_t wiegand_data_isr(int irq, void *dev_id);

void wiegand_clear(struct wiegand *w)
{
  w->currentBit = 0;
  w->startParity = 0;
  memset(w->buffer, 0, MAX_WIEGAND_BYTES);
}

void wiegand_init(struct wiegand *w)
{
  w->lastFacilityCode = 0;
  w->lastCardNumber = 0; 
  w->readNum = 0;
  wiegand_clear(w);
}

//returns true if even parity
bool checkParity(char *buffer, int numBytes, int parityCheck)
{
  int byte = 0;
  int bit = 0;
  int mask;
  int parity = parityCheck;

  for (byte = 0; byte < numBytes; byte++)
  {
    mask = 0x80;
    for (bit = 0; bit < 8; bit++)
    {
      if (mask & buffer[byte])
      { 
        parity++;
      } 
      mask >>= 1;
    }
  }  
  return (parity % 2) == 1;
}

static void wiegand_timer(struct timer_list* timer) 
{
  char *lcn;
  struct wiegand *w = &wiegand;
  int numBytes = w->currentBit / 16;
  int endParity = w->buffer[w->currentBit / 8] & (0x80 >> w->currentBit % 8);

  printk(KERN_DEBUG "wiegand read complete");
 
  //check the start parity
  if (checkParity(w->buffer, numBytes, w->startParity))
  {
    printk(KERN_DEBUG "start parity check failed");
    return; 
  }
    
  //check the end parity
  if (!checkParity(&w->buffer[numBytes], numBytes, endParity))
  {
    printk(KERN_DEBUG "end parity check failed");
    return; 
  }

  //ok all good set facility code and card code
  w->lastFacilityCode = (unsigned int)w->buffer[0];

  //note relies on 32 bit architecture
  w->lastCardNumber = 0;
  lcn = (char *)&w->lastCardNumber;
  lcn[0] = w->buffer[2];
  lcn[1] = w->buffer[1];
  w->readNum++;
  
  printk(KERN_DEBUG 
    "new read available: %d:%d", 
    w->lastFacilityCode, 
    w->lastCardNumber);
  
  //turn off the green led
  gpio_set_value(LED, 0);

  //reset for next reading 
  wiegand_clear(w);  
}

int init_module()
{
  int retval;

  printk(KERN_INFO "wiegand intialising");
  
  wiegand_init(&wiegand);

  if (gpio_request(W0, "W0")) 
  {
    printk(KERN_DEBUG "Could not request GPIO pin %i.", W0);
    return -EIO;
  }

  if (gpio_request(W1, "W1"))
  {
    printk(KERN_DEBUG "Could not request GPIO pin %i.", W1);
    return -EIO;
  }

  if (gpio_request(LED, "LED"))
  {
    printk(KERN_DEBUG "Could not request GPIO pin %i.", LED);
    return -EIO;
  }

  if (gpio_direction_input(W0))
  {
    printk(KERN_DEBUG "Could not set GPIO pin %i as input.", W0);
    return -EIO;
  }

  if (gpio_direction_input(W1))
  {
    printk(KERN_DEBUG "Could not set GPIO pin %i as input.", W1);
    return -EIO;
  }

  if (gpio_direction_output(LED, 0))
  {
    printk(KERN_DEBUG "Could not set GPIO pin %i as output.", LED);
    return -EIO;
  }


  
  /** Request IRQ for pin */
  if(request_irq(gpio_to_irq(W0), wiegand_data_isr, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "wiegand_data", &wiegand))
  {
    printk(KERN_DEBUG"Can't register IRQ %d", W0);
    return -EIO;
  }

  if(request_irq(gpio_to_irq(W1), wiegand_data_isr, IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "wiegand_data", &wiegand))
  {
    printk(KERN_DEBUG"Can't register IRQ %d", W1);
    return -EIO;
  }
  
  //setup the sysfs
  wiegandKObj = kobject_create_and_add("wiegand", kernel_kobj);
  
  if (!wiegandKObj)
  { 
    printk(KERN_DEBUG "wiegand failed to create sysfs");
    return -ENOMEM;
  } 

  retval = sysfs_create_group(wiegandKObj, &attr_group);
  if (retval)
  {
    kobject_put(wiegandKObj);
  }
  
  //setup the timer
  timer_setup(&timer, wiegand_timer, 0);
  
  //turn off leds 
  gpio_set_value(LED, 0);

  printk(KERN_INFO "wiegand ready");
  return retval;  
}

irqreturn_t wiegand_data_isr(int irq, void *dev_id)
{
  struct wiegand *w = (struct wiegand *)dev_id;  

  int data0 = gpio_get_value(W0);
  int data1 = gpio_get_value(W1);
  int value = ((data0 == 1) && (data1 == 0)) ? 0x80 : 0;

  if ((data0 == 1) && (data1 == 1))
  { //rising edge, ignore
    return IRQ_HANDLED;
  }

  //stop the end of transfer timer
  del_timer(&timer);

  //this is the start parity bit 
  if (w->currentBit == 0)
  {
    gpio_set_value(LED, 1);    
    w->startParity = value;
  }
  else
  {
    value ? printk("1 ") : printk("0 "); 
    w->buffer[(w->currentBit-1) / 8] |= (value >> ((w->currentBit-1) % 8)); 
  }

  w->currentBit++;

  //if we don't get another interrupt for 50ms we
  //assume the data is complete.   
  mod_timer(&timer, jiffies + msecs_to_jiffies(50));

  return IRQ_HANDLED;
}

void cleanup_module()
{
  kobject_put(wiegandKObj);
  del_timer(&timer);

  free_irq(gpio_to_irq(W0), &wiegand);
  free_irq(gpio_to_irq(W1), &wiegand);

  gpio_free(W0);
  gpio_free(W1);
  gpio_free(LED);
  printk(KERN_INFO "wiegand removed");
}

MODULE_DESCRIPTION("Wiegand GPIO driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("VerveWorks Pty. Ltd.");
