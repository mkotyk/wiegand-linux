/* wiegand-gpio.c
 *
 * Wiegand driver using GPIO an interrupts.
 *
 * Modified Nov 2020 mkotyk for RPI GPIO
 */

/* Standard headers for LKMs */
#include <linux/module.h>    /* Needed by all modules */
#include <linux/kernel.h>    /* Needed for KERN_INFO */
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

//#define DEBUG_PATTERN       1

#define MAX_WIEGAND_BYTES   6
#define MAX_PATTERNS        2
#define PATTERN_RATE_MS     25
#define WIEGAND_TIMEOUT_MS  50

#define W0                  19
#define W1                  26
#define LED                 27
#define BEEP                22

const char *LED_TOKEN = "LED:";
const char *BEEP_TOKEN = "BEEP:";
const char *PAD_TOKEN = "PAD:";
const char *TAG_TOKEN = "TAG:";

struct pattern {
    unsigned long bitmask;
    unsigned int position;
    unsigned int gpio;
};

static struct wiegand {
    int read_count;
    char buffer[MAX_WIEGAND_BYTES];
    int current_bit;

    int last_read_size;
    unsigned int last_keypad;
    unsigned int last_facility_code;
    unsigned int last_card_number;

    struct pattern patterns[MAX_PATTERNS];
} wiegand;

static struct timer_list wiegand_timer;
static struct timer_list pattern_timer;

static ssize_t wiegand_read_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    switch (wiegand.last_read_size) {
        case 4:
            return sprintf(
                    buf, "%s%1X\n",
                    PAD_TOKEN,
                    wiegand.last_keypad
            );
        case 26:
        case 34:
            return sprintf(
                    buf, "%s%.3d:%.5d\n",
                    TAG_TOKEN,
                    wiegand.last_facility_code,
                    wiegand.last_card_number
            );
        default:
            return 0;
    }
}

static ssize_t wiegand_control_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    int index;
    const char *tag;
    char line[30];

    *buf = '\0';
    for (index = 0; index < MAX_PATTERNS; index++) {
        if (wiegand.patterns[index].bitmask != 0) {
            switch (wiegand.patterns[index].gpio) {
                case LED:
                    tag = LED_TOKEN;
                    break;
                case BEEP:
                    tag = BEEP_TOKEN;
                    break;
                default:
                    tag = "UNKNOWN:";
                    break;
            }
            snprintf(line, sizeof(line), "%s%08lX:%d\n",
                     tag,
                     wiegand.patterns[index].bitmask,
                     wiegand.patterns[index].position);
            strcat(buf, line);
        }
    }
    return strlen(buf);
}

static void wiegand_start_pattern(unsigned long bitmask, unsigned int gpio) {
    int index, active_timer = 0;

    for (index = 0; index < MAX_PATTERNS; index++) {
        if (wiegand.patterns[index].bitmask != 0 &&
            wiegand.patterns[index].gpio == gpio) {
            // Already have a pattern for this gpio in progress
            break;
        }

        if (wiegand.patterns[index].bitmask == 0) {
            wiegand.patterns[index].gpio = gpio;
            wiegand.patterns[index].position = 0;
            wiegand.patterns[index].bitmask = bitmask;
            active_timer = 1;
#ifdef DEBUG_PATTERN
            printk(KERN_DEBUG "wiegand_start_pattern %d:%08lX:%d",
                   wiegand.patterns[index].gpio,
                   wiegand.patterns[index].bitmask,
                   wiegand.patterns[index].position
            );
#endif
            break;
        }
    }

    if (active_timer) {
        mod_timer(&pattern_timer, jiffies + msecs_to_jiffies(PATTERN_RATE_MS));
    }
}

static void wiegand_clear_pattern(struct pattern *pattern) {
    pattern->position = 0;
    pattern->bitmask = 0;
    pattern->gpio = 0;
}

static void pattern_timer_handler(struct timer_list *timer) {
    int index, active_timer = 0;
    for (index = 0; index < MAX_PATTERNS; index++) {
        if (wiegand.patterns[index].bitmask != 0) {
            int state = 0;
            active_timer = 1;
            if (wiegand.patterns[index].position < sizeof(wiegand.patterns[index].bitmask) * 8) {
                state = wiegand.patterns[index].bitmask & (1UL << wiegand.patterns[index].position++);
            } else {
                wiegand.patterns[index].bitmask = 0;
            }
#ifdef DEBUG_PATTERN
            printk(KERN_DEBUG "pattern_timer_handler %d:%08lX:%d = %d",
                   wiegand.patterns[index].gpio,
                   wiegand.patterns[index].bitmask,
                   wiegand.patterns[index].position,
                   state
            );
#endif
            gpio_set_value(wiegand.patterns[index].gpio, state);
        }
    }
    if (active_timer) {
#ifdef DEBUG_PATTERN
        printk(KERN_DEBUG "pattern_timer_handler - reactivate %d ms from now", PATTERN_RATE_MS);
#endif
        mod_timer(&pattern_timer, jiffies + msecs_to_jiffies(PATTERN_RATE_MS));
    }
}

static ssize_t wiegand_control_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
    long pattern = 0;
    if (count > strlen(LED_TOKEN) && strncmp(buf, LED_TOKEN, strlen(LED_TOKEN)) == 0) {
        if (kstrtoul(buf + strlen(LED_TOKEN), 16, &pattern) == 0) {
            wiegand_start_pattern(pattern, LED);
        }
    } else if (count > strlen(BEEP_TOKEN) && strncmp(buf, BEEP_TOKEN, strlen(BEEP_TOKEN)) == 0) {
        if (kstrtoul(buf + strlen(BEEP_TOKEN), 16, &pattern) == 0) {
            wiegand_start_pattern(pattern, BEEP);
        }
    }
#ifdef DEBUG_PATTERN
    else {
        printk(KERN_DEBUG "command not handled [%s] %ld", buf, count);
    }
#endif

    return count;
}

static struct kobj_attribute wiegand_read_attribute = __ATTR(read, 0660, wiegand_read_show, NULL);
static struct kobj_attribute wiegand_control_attribute = __ATTR(control, 0660, wiegand_control_show, wiegand_control_store);

static struct attribute *attrs[] = {
        &wiegand_read_attribute.attr,
        &wiegand_control_attribute.attr,
        NULL,   /* need to NULL terminate the list of attributes */
};

static struct attribute_group attr_group = {
        .attrs = attrs,
};

static struct kobject *wiegand_kernel_object;

irqreturn_t wiegand_data_isr(int irq, void *dev_id);

static void wiegand_clear(struct wiegand *w) {
    w->current_bit = 0;
    memset(w->buffer, 0, MAX_WIEGAND_BYTES);
}

static void wiegand_init(struct wiegand *w) {
    int index;

    w->last_facility_code = 0;
    w->last_card_number = 0;
    w->last_read_size = 0;
    w->last_keypad = 0;
    w->read_count = 0;
    for (index = 0; index < MAX_PATTERNS; index++) {
        wiegand_clear_pattern(&w->patterns[index]);
    }
    wiegand_clear(w);
}


static void wiegand_timer_handler(struct timer_list *timer) {
    // TODO: reimplement parity check if we can find an algorithm that actually works for all cards
    // according to https://www.hidglobal.com/sites/default/files/hid-understanding_card_data_formats-wp-en.pdf
    // the parity odd/even is up to the program that formats the cards in the first place.
    // Other formats put 2 or more parity bits at the start and none at the end.

    //ok all good set facility code and card code
    wiegand.last_facility_code = wiegand.buffer[0] << 1 |
                                 wiegand.buffer[1] >> 7;

    wiegand.last_card_number = (wiegand.buffer[1] & 0x7F) << 9 |
                               (wiegand.buffer[2] << 1) |
                               (wiegand.buffer[3] & 1);
    wiegand.read_count++;
    wiegand.last_keypad = (wiegand.buffer[0] >> 4) & 0xF;
    wiegand.last_read_size = wiegand.current_bit;

    wiegand_clear(&wiegand);
}

int init_module() {
    int retval;

    printk(KERN_INFO "wiegand intialising");

    wiegand_init(&wiegand);

    if (gpio_request(W0, "W0")) {
        printk(KERN_DEBUG "Could not request GPIO pin %i.", W0);
        return -EIO;
    }

    if (gpio_request(W1, "W1")) {
        printk(KERN_DEBUG "Could not request GPIO pin %i.", W1);
        return -EIO;
    }

    if (gpio_request(LED, "LED")) {
        printk(KERN_DEBUG "Could not request GPIO pin %i.", LED);
        return -EIO;
    }

    if (gpio_request(BEEP, "BEEP")) {
        printk(KERN_DEBUG "Could not request GPIO pin %i.", BEEP);
        return -EIO;
    }

    if (gpio_direction_input(W0)) {
        printk(KERN_DEBUG "Could not set GPIO pin %i as input.", W0);
        return -EIO;
    }

    if (gpio_direction_input(W1)) {
        printk(KERN_DEBUG "Could not set GPIO pin %i as input.", W1);
        return -EIO;
    }

    if (gpio_direction_output(LED, 0)) {
        printk(KERN_DEBUG "Could not set GPIO pin %i as output for LED.", LED);
        return -EIO;
    }

    if (gpio_direction_output(BEEP, 0)) {
        printk(KERN_DEBUG "Could not set GPIO pin %i as output for BEEP.", BEEP);
        return -EIO;
    }

    /** Request IRQ for pin */
    if (request_irq(gpio_to_irq(W0), wiegand_data_isr, IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "wiegand_data", &wiegand)) {
        printk(KERN_DEBUG"Can't register IRQ %d", W0);
        return -EIO;
    }

    if (request_irq(gpio_to_irq(W1), wiegand_data_isr, IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "wiegand_data", &wiegand)) {
        printk(KERN_DEBUG"Can't register IRQ %d", W1);
        return -EIO;
    }

    //setup the sysfs
    wiegand_kernel_object = kobject_create_and_add("wiegand", kernel_kobj);

    if (!wiegand_kernel_object) {
        printk(KERN_DEBUG "wiegand failed to create sysfs");
        return -ENOMEM;
    }

    retval = sysfs_create_group(wiegand_kernel_object, &attr_group);
    if (retval) {
        kobject_put(wiegand_kernel_object);
    }

    //setup the timer
    timer_setup(&wiegand_timer, wiegand_timer_handler, 0);
    timer_setup(&pattern_timer, pattern_timer_handler, 0);

    //turn off led & beeper
    gpio_set_value(LED, 0);
    gpio_set_value(BEEP, 0);

    printk(KERN_INFO "wiegand ready");
    return retval;
}

irqreturn_t wiegand_data_isr(int irq, void *dev_id) {
    struct wiegand *w = (struct wiegand *) dev_id;

    int data0 = gpio_get_value(W0);
    int data1 = gpio_get_value(W1);
    int value = ((data0 == 1) && (data1 == 0)) ? 0x80 : 0;

    if ((data0 == 1) && (data1 == 1)) { //rising edge, ignore
        return IRQ_HANDLED;
    }

    //stop the end of transfer timer
    del_timer(&wiegand_timer);
    if (w->current_bit < MAX_WIEGAND_BYTES * 8) {
        w->buffer[w->current_bit / 8] |= value >> (w->current_bit % 8);
    }
    w->current_bit++;

    //if we don't get another interrupt for 50ms we
    //assume the data is complete.
    mod_timer(&wiegand_timer, jiffies + msecs_to_jiffies(WIEGAND_TIMEOUT_MS));

    return IRQ_HANDLED;
}

void cleanup_module() {
    kobject_put(wiegand_kernel_object);
    del_timer(&wiegand_timer);
    del_timer(&pattern_timer);

    free_irq(gpio_to_irq(W0), &wiegand);
    free_irq(gpio_to_irq(W1), &wiegand);

    gpio_set_value(LED, 0);
    gpio_set_value(BEEP, 0);

    gpio_free(W0);
    gpio_free(W1);
    gpio_free(LED);
    gpio_free(BEEP);
    printk(KERN_INFO "wiegand removed");
}

MODULE_DESCRIPTION("Wiegand GPIO driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("VerveWorks Pty. Ltd., Mark Kotyk");