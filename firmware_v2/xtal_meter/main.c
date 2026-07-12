#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <stdint.h>
#include <sysctl.h>
#include <uarths.h>

#define ESP_EN_GPIO 0
#define ESP_PULSE_GPIO 3
#define K210_CYCLES_PER_SECOND 390000000ull
static volatile gpio_t *const G=(volatile gpio_t *)GPIO_BASE_ADDR;

static uint64_t cycle(void){uint64_t v;__asm__ volatile("rdcycle %0":"=r"(v));return v;}
static void put_uint(uint64_t v){char b[24];unsigned n=0;do{b[n++]=(char)('0'+v%10u);v/=10u;}while(v);while(n)uarths_write_byte((uint8_t)b[--n]);}
static void line_value(const char *name,uint64_t value,const char *unit){uarths_puts(name);put_uint(value);uarths_puts(unit);uarths_puts("\r\n");}
static void gpio_out(int n,int v){G->direction.u32[0]|=1u<<n;if(v)G->data_output.u32[0]|=1u<<n;else G->data_output.u32[0]&=~(1u<<n);}
static void wait_cycles(uint64_t n){uint64_t end=cycle()+n;while((int64_t)(end-cycle())>0)__asm__ volatile("nop");}

int main(void)
{
 sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK,0);sysctl_pll_set_freq(SYSCTL_PLL0,780000000u);sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK,SYSCTL_SOURCE_PLL0);
 fpioa_set_function(4,FUNC_UARTHS_RX);fpioa_set_function(5,FUNC_UARTHS_TX);uarths_init();uint32_t d=sysctl_clock_get_freq(SYSCTL_CLOCK_CPU)/115200u;if(d)--d;((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div=(uint16_t)d;
 sysctl_clock_enable(SYSCTL_CLOCK_GPIO);fpioa_set_function(8,FUNC_GPIO0);fpioa_set_function(15,FUNC_GPIO3);
 uarths_puts("XTALMETER:START esp_build_xtal=26 k210_hz=390000000\r\n");
 gpio_out(ESP_PULSE_GPIO,1);gpio_out(ESP_EN_GPIO,0);wait_cycles(K210_CYCLES_PER_SECOND/10u);gpio_out(ESP_EN_GPIO,1);wait_cycles(K210_CYCLES_PER_SECOND/5u);G->direction.u32[0]&=~(1u<<ESP_PULSE_GPIO);
 uint32_t previous=(G->data_input.u32[0]>>ESP_PULSE_GPIO)&1u;uint64_t last=0,sum=0;unsigned periods=0;uint64_t deadline=cycle()+K210_CYCLES_PER_SECOND*20u;
 while(periods<10u&&(int64_t)(deadline-cycle())>0){uint32_t now=(G->data_input.u32[0]>>ESP_PULSE_GPIO)&1u;if(previous&& !now){uint64_t at=cycle();if(last){uint64_t delta=at-last;sum+=delta;periods++;line_value("XTALMETER:PERIOD_CYCLES=",delta,"");line_value("XTALMETER:PERIOD_US=",delta/390u,"");}last=at;}previous=now;}
 if(periods<4u){uarths_puts("XTALMETER:FAIL insufficient_edges\r\n");for(;;)__asm__ volatile("wfi");}
 uint64_t average=sum/periods;line_value("XTALMETER:AVERAGE_CYCLES=",average,"");line_value("XTALMETER:AVERAGE_US=",average/390u,"");
 if(average>370000000ull&&average<410000000ull)uarths_puts("XTALMETER:RESULT XTAL=26MHz MATCH\r\n");
 else if(average>240000000ull&&average<270000000ull)uarths_puts("XTALMETER:RESULT XTAL=40MHz period_ratio=26/40\r\n");
 else if(average>580000000ull&&average<620000000ull)uarths_puts("XTALMETER:RESULT XTAL=40MHz period_ratio=40/26\r\n");
 else uarths_puts("XTALMETER:RESULT UNKNOWN\r\n");
 for(;;)__asm__ volatile("wfi");
}
