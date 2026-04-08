#ifndef BLINKY_H
#define BLINKY_H

#include <zephyr/drivers/gpio.h>

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
extern const struct gpio_dt_spec led0;
extern const struct gpio_dt_spec led1;
int blinky(void);
int led_init(void);


#endif /* BLINKY_H */
