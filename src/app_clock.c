#include "app_clock.h"
#include <sysctl.h>

#define APP_PLL0_HZ 780000000UL
#define APP_PLL1_HZ 160000000UL
#define APP_PLL2_HZ 45158400UL

void app_clock_init(void)
{
    /*
     * Own the clock state after bootloader jump.  Do not rely on ROM/kflash
     * side effects.  PLL0=780 MHz and ACLK divider 0 gives CPU/ACLK=390 MHz.
     */
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK, 0);
    sysctl_pll_set_freq(SYSCTL_PLL0, APP_PLL0_HZ);
    sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK, SYSCTL_SOURCE_PLL0);

    sysctl_pll_set_freq(SYSCTL_PLL1, APP_PLL1_HZ);
    sysctl_pll_set_freq(SYSCTL_PLL2, APP_PLL2_HZ);
}
