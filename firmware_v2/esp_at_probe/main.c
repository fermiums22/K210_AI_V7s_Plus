#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <sysctl.h>
#include <uart.h>
#include <uarths.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define GPIO_ESP_EN 0
#define GPIO_ESP_BOOT 3
static volatile gpio_t *const G=(volatile gpio_t *)GPIO_BASE_ADDR;

static TickType_t ticks(uint32_t ms){TickType_t t=pdMS_TO_TICKS(ms);return t?t:1;}
static void log_line(const char *fmt,...){char b[256];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);b[sizeof(b)-1]=0;uarths_puts(b);uarths_puts("\r\n");}
static void gpio_out(int n,int v){G->direction.u32[0]|=1u<<n;if(v)G->data_output.u32[0]|=1u<<n;else G->data_output.u32[0]&=~(1u<<n);}
static void reset_esp(void){gpio_out(GPIO_ESP_BOOT,1);gpio_out(GPIO_ESP_EN,0);vTaskDelay(ticks(100));gpio_out(GPIO_ESP_EN,1);}

static bool command(handle_t uart,const char *cmd,const char *token,uint32_t timeout)
{
 uint8_t trash[64];while(io_read(uart,trash,sizeof(trash))>0){}
 log_line("ATPROBE:TX %s",cmd);if(io_write(uart,(const uint8_t *)cmd,strlen(cmd))!=(int)strlen(cmd)||io_write(uart,(const uint8_t *)"\r\n",2)!=2)return false;
 size_t m=0,e=0;const char *err="ERROR";TickType_t end=xTaskGetTickCount()+ticks(timeout);
 while((int32_t)(end-xTaskGetTickCount())>0){uint8_t c;if(io_read(uart,&c,1)<=0){taskYIELD();continue;}uarths_write_byte(c);
  if(c==(uint8_t)token[m]){if(!token[++m]){log_line("\r\nATPROBE:PASS cmd=%s token=%s",cmd,token);return true;}}else m=c==(uint8_t)token[0]?1:0;
  if(c==(uint8_t)err[e]){if(!err[++e]){log_line("\r\nATPROBE:ERROR cmd=%s",cmd);return false;}}else e=c==(uint8_t)err[0]?1:0;}
 log_line("\r\nATPROBE:TIMEOUT cmd=%s token=%s",cmd,token);return false;
}

int main(void)
{
 sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK,0);sysctl_pll_set_freq(SYSCTL_PLL0,780000000u);sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK,SYSCTL_SOURCE_PLL0);
 fpioa_set_function(4,FUNC_UARTHS_RX);fpioa_set_function(5,FUNC_UARTHS_TX);uarths_init();uint32_t d=sysctl_clock_get_freq(SYSCTL_CLOCK_CPU)/115200u;if(d)--d;((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div=(uint16_t)d;
 sysctl_clock_enable(SYSCTL_CLOCK_GPIO);fpioa_set_function(6,FUNC_UART2_RX);fpioa_set_function(7,FUNC_UART2_TX);fpioa_set_function(8,FUNC_GPIO0);fpioa_set_function(15,FUNC_GPIO3);
 handle_t uart=io_open("/dev/uart2");if(!uart){log_line("ATPROBE:HALT uart-open");for(;;)vTaskDelay(ticks(1000));}
 uart_config(uart,115200u,8,UART_STOP_1,UART_PARITY_NONE);reset_esp();log_line("ATPROBE:BOOT baud=115200");TickType_t end=xTaskGetTickCount()+ticks(3000);uint32_t n=0;
 while((int32_t)(end-xTaskGetTickCount())>0){uint8_t c;if(io_read(uart,&c,1)>0){uarths_write_byte(c);n++;}else taskYIELD();}log_line("\r\nATPROBE:BOOT_DONE bytes=%lu",(unsigned long)n);
 const uint32_t bauds[]={177230,177000,176000,180000,172800,153600,128000,115200,74880,75000,9600,19200,38400,57600,76800,230400,460800,921600};uint32_t found=0;for(unsigned i=0;i<sizeof(bauds)/sizeof(bauds[0]);i++){uart_config(uart,bauds[i],8,UART_STOP_1,UART_PARITY_NONE);log_line("ATPROBE:BAUD %lu",(unsigned long)bauds[i]);if(command(uart,"AT","OK",2000)){found=bauds[i];break;}}
 if(!found){log_line("ATPROBE:HALT no-at-baud");for(;;)vTaskDelay(ticks(1000));}
 bool ok=command(uart,"AT+GMR","OK",3000)&&command(uart,"AT+CWMODE=1","OK",3000)&&command(uart,"AT+CWLAP","ELECTRONICS",15000)&&command(uart,"AT+CWJAP=\"ELECTRONICS\",\"bdc123print\"","OK",25000)&&command(uart,"AT+CIFSR","STAIP",5000);
 if(ok)log_line("ATPROBE:ALL_PASS baud=%lu ssid=ELECTRONICS",(unsigned long)found);else log_line("ATPROBE:HALT wifi-test-failed");for(;;)vTaskDelay(ticks(1000));
}
