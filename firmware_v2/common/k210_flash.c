#include "k210_flash.h"
#include <platform.h>
#include <spi.h>
#include <string.h>
#include <sysctl.h>

#define CS 1u
#define TIMEOUT 100000u
#define WIP_TIMEOUT 8000000u
#define SR_BUSY 1u
#define SR_TFNF 2u
#define SR_TFE 4u
#define SR_RFNE 8u
#define TMOD_TX 1u
#define TMOD_EEPROM 3u
static volatile spi_t *const S=(volatile spi_t *)SPI3_BASE_ADDR;

static int wait_mask(uint32_t mask,uint32_t value){for(uint32_t i=0;i<TIMEOUT;i++)if((S->sr&mask)==value)return 0;return -1;}
static void flush_rx(void){for(unsigned i=0;i<128u && (S->sr&SR_RFNE);i++)(void)S->dr[0];}
static void end_transfer(void){S->ser=0;(void)wait_mask(SR_BUSY,0);S->dmacr=0;S->ssienr=0;flush_rx();}
static void prepare(uint32_t tmod)
{
    sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_SPI3,1u);sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI3,2u);sysctl_clock_enable(SYSCTL_CLOCK_SPI3);
    (void)wait_mask(SR_BUSY,0);S->ser=0;S->ssienr=0;S->baudr=4u;S->imr=0;S->dmacr=0;S->rx_sample_delay=0;S->endian=0;S->spi_ctrlr0=0;S->ctrlr1=0;S->ctrlr0=7u|(tmod<<10);(void)S->icr;flush_rx();
}
static int tx(const uint8_t *p,uint32_t n)
{
    prepare(TMOD_TX);S->ssienr=1;S->ser=CS;
    for(uint32_t i=0;i<n;i++){if(wait_mask(SR_TFNF,SR_TFNF)){end_transfer();return -1;}S->dr[0]=p[i];}
    if(wait_mask(SR_TFE,SR_TFE)||wait_mask(SR_BUSY,0)){end_transfer();return -2;}end_transfer();return 0;
}
static int read_cmd(const uint8_t *cmd,uint32_t cn,uint8_t *out,uint32_t n)
{
    if(!n)return 0;prepare(TMOD_EEPROM);S->ctrlr1=n-1u;S->ssienr=1;
    for(uint32_t i=0;i<cn;i++){if(wait_mask(SR_TFNF,SR_TFNF)){end_transfer();return -1;}S->dr[0]=cmd[i];}S->ser=CS;
    uint32_t got=0;while(got<n){uint32_t before=got;for(uint32_t i=0;i<TIMEOUT && got<n;i++)while((S->sr&SR_RFNE)&&got<n)out[got++]=(uint8_t)S->dr[0];if(got==before){end_transfer();return -2;}}
    end_transfer();return 0;
}
static int sr1(uint8_t *v){uint8_t c=5;return read_cmd(&c,1,v,1);}
static int wren(void){uint8_t c=6,v=0;if(tx(&c,1)||sr1(&v))return -1;return (v&2u)?0:-2;}
static int done(void){for(uint32_t i=0;i<WIP_TIMEOUT;i++){uint8_t v;if(sr1(&v))return -1;if(v==0xffu)return -2;if(!(v&1u))return 0;}return -3;}
int k210_flash_erase_4k(uint32_t o){if(o&0xfffu)return -1;uint8_t c[4]={0x20,(uint8_t)(o>>16),(uint8_t)(o>>8),(uint8_t)o};if(wren())return -2;if(tx(c,4))return -3;return done();}
int k210_flash_program(uint32_t o,const void *data,uint32_t size)
{
    const uint8_t *p=(const uint8_t *)data;uint8_t c[260];
    while(size){uint32_t n=256u-(o&255u);if(n>size)n=size;c[0]=2;c[1]=(uint8_t)(o>>16);c[2]=(uint8_t)(o>>8);c[3]=(uint8_t)o;memcpy(c+4,p,n);if(wren())return -1;if(tx(c,n+4u))return -2;if(done())return -3;o+=n;p+=n;size-=n;}return 0;
}
int k210_flash_read(uint32_t o,void *data,uint32_t size){uint8_t c[4]={3,(uint8_t)(o>>16),(uint8_t)(o>>8),(uint8_t)o};return read_cmd(c,4,(uint8_t *)data,size);}
