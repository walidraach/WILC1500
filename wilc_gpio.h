#ifndef WILC_GPIO
#define WILC_GPIO

#define SDIO_GPIO_NODE "sdio"
struct wilc_gpio
{
    int gpio_irq;
	int gpio_reset;
	int gpio_chip_en;
};

extern struct wilc_gpio wilc_gpio;

#endif
