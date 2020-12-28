wiegand-linux
=============

Linux driver for reading wiegand data from GPIO, and controlling an optional LED and BEEPer.

Code is for the Raspberry Pi (any model)

### Build this module on your pi.  
```
sudo apt install build-essential
make
sudo insmod wiegand-gpio.ko
```

This module will create the following kernel attribute tree:

```
/sys/kernel
      + wiegand/
           + read 
           + control
```
	       
	       
### Reading
By reading the file `/sys/kernel/wiegand/read` you can get the latest input from the driver.  Examples:

```
cat /sys/kernel/wiegand/read
PAD:1	       
```
Keypad button 1 was pressed.

```
cat /sys/kernel/wiegand/read
TAG:071:24004
```
A 26 bit card was read.

### Controlling
By writing to `/sys/kernel/wiegand/control` you can toggle a pattern to either the LED or BEEP function.  Examples:

```
echo BEEP:1 > /sys/kernel/wiegand/control
```
Does a quick 25ms chirp.

```
echo LED:F5F5F5F5 > /sys/kernel/wiegand/control
```
Flashes the LED in the pattern of 11110101111101011111010111110101 at 25ms intervals

```
echo LED:FFFFFFFF > /sys/kernel/wiegand/control
```
Light the LED solid for 800ms.


## How to add the module to startup
 
1. Edit the `/etc/modules` file and add the name of the module (`wiegand-gpio`) on its own line.
1. Copy the module to a suitable folder in ```/lib/modules/`uname -r`/kernel/drivers``` 
1. Run `depmod`
1. Reboot and see if it's loaded


## How to set permissions to a non-root group
1. Create a user or just a group called `sentinel`
1. Copy the file `30-sentinel.rules` to `/etc/udev/rules.d/`
1. Reboot
