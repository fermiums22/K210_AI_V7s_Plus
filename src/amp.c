#include "amp.h"
#include "pinout.h"
#include <fpioa.h>
#include <gpio.h>
#include <sysctl.h>
#include <platform.h>

static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

static void gpio_out_set(int gpio_n, int val)
{
    REG_GPIO->direction.u32[0] |= (1u << gpio_n);
    if (val)
        REG_GPIO->data_output.u32[0] |=  (1u << gpio_n);
    else
        REG_GPIO->data_output.u32[0] &= ~(1u << gpio_n);
}

void amp_init(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);   /* MUST enable before GPIO writes */
    fpioa_set_function(PIN_AMP_MUTE, FUNC_GPIO1);
    fpioa_set_function(PIN_AMP_SHDN, FUNC_GPIO2);
    gpio_out_set(GPIO_AMP_MUTE, 0);           /* LOW = muted    */
    gpio_out_set(GPIO_AMP_SHDN, 0);           /* LOW = shutdown  */
}

void amp_set(bool on)
{
    gpio_out_set(GPIO_AMP_SHDN, on ? 1 : 0);
    gpio_out_set(GPIO_AMP_MUTE, on ? 1 : 0);
}
