#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <spi.h>
#include <sysctl.h>
#include <uart.h>
#include <uarths.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "klink_v1.h"
#include "klink_v1_link.h"
#include "kupdate_v2.h"
#include "k210_flash.h"
#include "kboot_meta_v2.h"
#include "sha256_stream.h"

#define PIN_CS 0
#define PIN_CLK 1
#define PIN_MISO 2
#define PIN_MOSI 3
#define PIN_DBG_RX 4
#define PIN_DBG_TX 5
#define PIN_ESP_TX 6
#define PIN_ESP_RX 7
#define PIN_ESP_EN 8
#define PIN_ESP_BOOT 15
#define GPIO_ESP_EN 0
#define GPIO_ESP_BOOT 3
#define LOG_BAUD 115200u
#define ESP_MARKER "STA_READY ssid=Fermiums_2.4 ip="
#define WIRE_PREFIX 2u
#define WIRE_BYTES (WIRE_PREFIX+KLINK_V1_CELL_BYTES)
#define READY_MS 100u
#define ACQUIRE_MS 15000u
#define APP_MAGIC 0x4b323130u
#define APP_MAGIC_INV 0xb4cdcedfu

typedef struct __attribute__((packed)) app_header {uint32_t magic,magic_inv,load_addr,entry_addr,image_size,image_crc32,flags,reserved;} app_header_t;
typedef enum { U_IDLE, U_RECEIVING, U_FAILED } update_state_t;

static volatile gpio_t *const G=(volatile gpio_t *)GPIO_BASE_ADDR;
static handle_t s_spi,s_dev;
static klink_v1_endpoint_t s_link;
static uint8_t s_tx[WIRE_BYTES] __attribute__((aligned(64)));
static uint8_t s_rx[WIRE_BYTES] __attribute__((aligned(64)));
static uint8_t s_page[4096] __attribute__((aligned(64)));
static uint8_t s_verify[4096] __attribute__((aligned(64)));
static update_state_t s_state;
static kupdate_v2_open_t s_open;
static uint32_t s_received,s_buffered,s_programmed,s_slot_offset;
static uint8_t s_target_slot;
static sha256_stream_t s_stream_hash;

static TickType_t ticks(uint32_t ms){TickType_t t=pdMS_TO_TICKS(ms);return t?t:1;}
static uint64_t cycle_now(void){uint64_t v;__asm__ volatile("rdcycle %0":"=r"(v));return v;}
static void idle_bus_pause(void){uint64_t end=cycle_now()+780000u;while((int64_t)(end-cycle_now())>0)__asm__ volatile("nop");}
static void log_line(const char *fmt,...){char b[256];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);b[sizeof(b)-1]=0;uarths_puts(b);uarths_puts("\r\n");}
static void halt_with_logs(const char *reason){for(;;){log_line("UPDATE:HALTED %s",reason);vTaskDelay(ticks(1000));}}
static void clock_init(void){sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK,0);sysctl_pll_set_freq(SYSCTL_PLL0,780000000u);sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK,SYSCTL_SOURCE_PLL0);sysctl_pll_set_freq(SYSCTL_PLL1,160000000u);sysctl_pll_set_freq(SYSCTL_PLL2,45158400u);}
static void log_init(void){fpioa_set_function(PIN_DBG_RX,FUNC_UARTHS_RX);fpioa_set_function(PIN_DBG_TX,FUNC_UARTHS_TX);uarths_init();uint32_t d=sysctl_clock_get_freq(SYSCTL_CLOCK_CPU)/LOG_BAUD;if(d)d--;if(d>0xffff)d=0xffff;((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div=(uint16_t)d;}
static void gpio_out(int n,int v){G->direction.u32[0]|=1u<<n;if(v)G->data_output.u32[0]|=1u<<n;else G->data_output.u32[0]&=~(1u<<n);}
static bool boot_esp(void)
{
 sysctl_clock_enable(SYSCTL_CLOCK_GPIO);fpioa_set_function(PIN_ESP_TX,FUNC_UART2_RX);fpioa_set_function(PIN_ESP_RX,FUNC_UART2_TX);fpioa_set_function(PIN_ESP_EN,FUNC_GPIO0);fpioa_set_function(PIN_ESP_BOOT,FUNC_GPIO3);gpio_out(GPIO_ESP_BOOT,1);gpio_out(GPIO_ESP_EN,0);
 handle_t u=io_open("/dev/uart2");if(!u)return false;uart_config(u,115200,8,UART_STOP_1,UART_PARITY_NONE);vTaskDelay(ticks(20));gpio_out(GPIO_ESP_EN,1);vTaskDelay(ticks(20));G->direction.u32[0]&=~(1u<<GPIO_ESP_BOOT);
 size_t m=0;bool found=false;uint64_t end=cycle_now()+5850000000ull;while((int64_t)(end-cycle_now())>0){uint8_t c;if(io_read(u,&c,1)<=0){__asm__ volatile("nop");continue;}uarths_write_byte(c);if(found){if(c=='\n'){io_close(u);return true;}continue;}if(c==(uint8_t)ESP_MARKER[m]){if(!ESP_MARKER[++m])found=true;}else m=c==(uint8_t)ESP_MARKER[0]?1:0;}io_close(u);return false;
}
static bool spi_init_link(void){fpioa_set_function(PIN_CS,FUNC_SPI1_SS0);fpioa_set_function(PIN_CLK,FUNC_SPI1_SCLK);fpioa_set_function(PIN_MOSI,FUNC_SPI1_D0);fpioa_set_function(PIN_MISO,FUNC_SPI1_D1);s_spi=io_open("/dev/spi1");if(!s_spi)return false;s_dev=spi_get_device(s_spi,SPI_MODE_0,SPI_FF_STANDARD,1,8);if(!s_dev)return false;spi_dev_set_clock_rate(s_dev,20000000.0);return true;}
static int transfer(klink_v1_cell_t *tx,klink_v1_cell_t *rx)
{
 TickType_t end=xTaskGetTickCount()+ticks(READY_MS);while(!(G->data_input.u32[0]&(1u<<GPIO_ESP_BOOT))){if((int32_t)(end-xTaskGetTickCount())<=0)return -100;taskYIELD();}
 uint8_t p[2]={3,0};memset(s_rx,0,KLINK_V1_CELL_BYTES);int n=spi_dev_transfer_sequential(s_dev,p,2,s_rx,KLINK_V1_CELL_BYTES);if(n!=(int)KLINK_V1_CELL_BYTES)return -101;memcpy(rx,s_rx,sizeof(*rx));
 end=xTaskGetTickCount()+ticks(READY_MS);while(G->data_input.u32[0]&(1u<<GPIO_ESP_BOOT)){if((int32_t)(end-xTaskGetTickCount())<=0)return -102;taskYIELD();}
 s_tx[0]=2;s_tx[1]=0;memcpy(s_tx+2,tx,sizeof(*tx));n=io_write(s_dev,s_tx,sizeof(s_tx));return n==(int)sizeof(s_tx)?0:-103;
}
static int exchange(klink_v1_event_t *event,klink_v1_cell_t *rx)
{
 klink_v1_cell_t tx;klink_v1_build_tx(&s_link,&tx);int rc=transfer(&tx,rx);if(rc)return rc;(void)klink_v1_process_rx(&s_link,rx,event);if(event->flags&KLINK_EVENT_FAULT)return -200-(int)event->fault;if(KLINK_TYPE(rx->channel_type)==KLINK_T_IDLE)idle_bus_pause();return 0;
}
static bool acquire(void)
{
 bool cap=false;TickType_t end=xTaskGetTickCount()+ticks(ACQUIRE_MS);while((int32_t)(end-xTaskGetTickCount())>0){klink_v1_cell_t rx;klink_v1_event_t ev;if(exchange(&ev,&rx))continue;if(ev.flags&KLINK_EVENT_RX){if(ev.rx_channel==KLINK_CH_CONTROL&&ev.rx_type==KLINK_T_CAPABILITIES)cap=true;if(rx.flags&KLINK_F_RELIABLE)klink_v1_release_credit(&s_link,ev.rx_channel,1);}if(cap&&s_link.peer_credit[KLINK_CH_BULK])return true;}return false;
}
static int send_status(uint8_t state,uint8_t error,uint32_t detail)
{
 kupdate_v2_status_t st;memset(&st,0,sizeof(st));st.state=state;st.error=error;st.target_slot=s_target_slot;st.offset=s_received;st.image_size=s_open.image_size;st.detail=detail;kupdate_v2_status_finalize(&st);
 for(;;){if(klink_v1_queue(&s_link,KLINK_CH_BULK,KLINK_T_STATUS,KLINK_F_RELIABLE,&st,sizeof(st)))return 0;klink_v1_cell_t rx;klink_v1_event_t ev;int rc=exchange(&ev,&rx);if(rc)return rc;if(ev.flags&KLINK_EVENT_RX){if(rx.flags&KLINK_F_RELIABLE)klink_v1_release_credit(&s_link,ev.rx_channel,1);}}
}
static void fail(uint8_t error,uint32_t detail){log_line("UPDATE:FAIL error=%u detail=%lu offset=%lu",error,(unsigned long)detail,(unsigned long)s_received);s_state=U_FAILED;(void)send_status(KUPDATE_V2_STATE_FAILED,error,detail);}
static int flush_buffer(void)
{
 if(!s_buffered)return 0;int rc=k210_flash_program(s_slot_offset+s_programmed,s_page,s_buffered);if(rc)return rc;s_programmed+=s_buffered;s_buffered=0;return 0;
}
static void begin_update(const klink_v1_cell_t *cell)
{
 if(s_state!=U_IDLE){fail(KUPDATE_V2_ERR_STATE,1);return;}if(cell->payload_length!=sizeof(s_open)){fail(KUPDATE_V2_ERR_PROTOCOL,cell->payload_length);return;}memcpy(&s_open,cell->payload,sizeof(s_open));if(!kupdate_v2_open_valid(&s_open,KBOOT_SLOT_BYTES)){fail(KUPDATE_V2_ERR_SIZE,s_open.image_size);return;}
 kboot_meta_v2_t meta;int mrc=kboot_meta_v2_load(&meta);if(mrc<0){fail(KUPDATE_V2_ERR_METADATA,(uint32_t)-mrc);return;}if(mrc==0&&meta.pending_slot!=KBOOT_SLOT_NONE){fail(KUPDATE_V2_ERR_STATE,0x50454e44u);return;}s_target_slot=(uint8_t)(meta.confirmed_slot^1u);s_slot_offset=s_target_slot?KBOOT_SLOT_B_OFFSET:KBOOT_SLOT_A_OFFSET;
 uint32_t erase=(s_open.image_size+0xfffu)&~0xfffu;log_line("UPDATE:ERASE slot=%c offset=0x%08lx bytes=%lu",s_target_slot?'B':'A',(unsigned long)s_slot_offset,(unsigned long)erase);
 for(uint32_t o=0;o<erase;o+=0x1000u){int rc=k210_flash_erase_4k(s_slot_offset+o);if(rc){fail(KUPDATE_V2_ERR_FLASH_ERASE,(uint32_t)-rc);return;}}
 s_received=s_buffered=s_programmed=0;sha256_stream_init(&s_stream_hash);s_state=U_RECEIVING;(void)send_status(KUPDATE_V2_STATE_READY,KUPDATE_V2_OK,0);log_line("UPDATE:READY slot=%c size=%lu",s_target_slot?'B':'A',(unsigned long)s_open.image_size);
}
static void accept_data(const klink_v1_cell_t *cell)
{
 if(s_state!=U_RECEIVING){fail(KUPDATE_V2_ERR_STATE,2);return;}if(cell->payload_length<4u||cell->payload_length>48u){fail(KUPDATE_V2_ERR_PROTOCOL,cell->payload_length);return;}kupdate_v2_data_t d;memset(&d,0,sizeof(d));memcpy(&d,cell->payload,cell->payload_length);uint32_t n=cell->payload_length-4u;if(d.offset!=s_received||n>s_open.image_size-s_received){fail(KUPDATE_V2_ERR_OFFSET,d.offset);return;}
 sha256_stream_update(&s_stream_hash,d.bytes,n);uint32_t at=0;while(at<n){uint32_t take=sizeof(s_page)-s_buffered;if(take>n-at)take=n-at;memcpy(s_page+s_buffered,d.bytes+at,take);s_buffered+=take;at+=take;if(s_buffered==sizeof(s_page)){int rc=flush_buffer();if(rc){fail(KUPDATE_V2_ERR_FLASH_WRITE,(uint32_t)-rc);return;}}}s_received+=n;
}
static void close_update(const klink_v1_cell_t *cell)
{
 if(s_state!=U_RECEIVING||cell->payload_length!=4u){fail(KUPDATE_V2_ERR_STATE,3);return;}uint32_t declared;memcpy(&declared,cell->payload,4);if(declared!=s_received||s_received!=s_open.image_size){fail(KUPDATE_V2_ERR_SIZE,declared);return;}int rc=flush_buffer();if(rc){fail(KUPDATE_V2_ERR_FLASH_WRITE,(uint32_t)-rc);return;}
 uint8_t digest[32];sha256_stream_final(&s_stream_hash,digest);if(memcmp(digest,s_open.image_sha256,32)){fail(KUPDATE_V2_ERR_HASH,1);return;}log_line("UPDATE:STREAM_HASH_OK bytes=%lu",(unsigned long)s_received);
 sha256_stream_t verify;sha256_stream_init(&verify);for(uint32_t o=0;o<s_open.image_size;){uint32_t n=s_open.image_size-o;if(n>sizeof(s_verify))n=sizeof(s_verify);rc=k210_flash_read(s_slot_offset+o,s_verify,n);if(rc){fail(KUPDATE_V2_ERR_FLASH_READ,(uint32_t)-rc);return;}sha256_stream_update(&verify,s_verify,n);o+=n;}sha256_stream_final(&verify,digest);if(memcmp(digest,s_open.image_sha256,32)){fail(KUPDATE_V2_ERR_HASH,2);return;}
 app_header_t hdr;rc=k210_flash_read(s_slot_offset,&hdr,sizeof(hdr));if(rc||hdr.magic!=APP_MAGIC||hdr.magic_inv!=APP_MAGIC_INV||hdr.load_addr!=0x80100000u||hdr.image_size!=s_open.image_size||hdr.entry_addr<0x80100000u||hdr.entry_addr>=0x80600000u){fail(KUPDATE_V2_ERR_PROTOCOL,0x484452u);return;}log_line("UPDATE:READBACK_VERIFY_OK slot=%c",s_target_slot?'B':'A');
 kboot_meta_v2_t meta;rc=kboot_meta_v2_load(&meta);if(rc<0){fail(KUPDATE_V2_ERR_METADATA,(uint32_t)-rc);return;}meta.pending_slot=s_target_slot;meta.boot_attempts=0;meta.image_size[s_target_slot]=s_open.image_size;memcpy(meta.image_sha256[s_target_slot],s_open.image_sha256,32);rc=kboot_meta_v2_append(&meta);if(rc){fail(KUPDATE_V2_ERR_METADATA,(uint32_t)-rc);return;}s_state=U_IDLE;(void)send_status(KUPDATE_V2_STATE_COMMITTED,KUPDATE_V2_OK,meta.generation);log_line("UPDATE:COMMITTED pending=%c generation=%lu",s_target_slot?'B':'A',(unsigned long)meta.generation);
}
static void handle_bulk(const klink_v1_cell_t *cell,const klink_v1_event_t *ev)
{
 if(ev->rx_type==KLINK_T_OPEN)begin_update(cell);else if(ev->rx_type==KLINK_T_DATA)accept_data(cell);else if(ev->rx_type==KLINK_T_CLOSE)close_update(cell);else if(ev->rx_type==KLINK_T_ABORT){s_state=U_IDLE;s_received=s_buffered=s_programmed=0;log_line("UPDATE:ABORT");}else fail(KUPDATE_V2_ERR_PROTOCOL,ev->rx_type);
}
int main(void)
{
 clock_init();log_init();log_line("UPDATE:BOOT wifi->ESP->KLINK->inactive-slot build=2");if(!boot_esp())halt_with_logs("ESP marker missing");if(!spi_init_link())halt_with_logs("SPI init");klink_v1_endpoint_init(&s_link,8);s_target_slot=KBOOT_SLOT_NONE;if(!acquire())halt_with_logs("link acquire");log_line("UPDATE:LINK_READY clock=19.5MHz");
 for(;;){klink_v1_cell_t rx;klink_v1_event_t ev;int rc=exchange(&ev,&rx);if(rc){log_line("UPDATE:STOP link rc=%d fault=%u channel=%u expected=%u observed=%u",rc,s_link.fault,s_link.fault_channel,s_link.fault_expected,s_link.fault_observed);halt_with_logs("link exchange");}if(ev.flags&KLINK_EVENT_RX){if(ev.rx_channel==KLINK_CH_BULK)handle_bulk(&rx,&ev);if(rx.flags&KLINK_F_RELIABLE)klink_v1_release_credit(&s_link,ev.rx_channel,1);}}
}
