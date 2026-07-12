#include <fpioa.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sysctl.h>
#include <uarths.h>
#include "k210_flash.h"
#include "sha256_stream.h"

#define TEST_BYTES (1024u * 1024u)
#define FLASH_BASE 0x00b00000u
#define CHUNK 4096u
#define LOG_BAUD 115200u

static uint8_t s_ram[TEST_BYTES] __attribute__((aligned(64)));
static uint8_t s_readback[CHUNK] __attribute__((aligned(64)));
static uint8_t s_expected[32];
static uint8_t s_actual[32];
static char s_expected_hex[65];
static char s_actual_hex[65];

static uint64_t now(void) { uint64_t v; __asm__ volatile("rdcycle %0":"=r"(v)); return v; }
static uint32_t ms_since(uint64_t start) { uint64_t ms=(now()-start)/390000u; return (uint32_t)(ms?ms:1u); }
static uint32_t kib_s(uint32_t bytes,uint32_t ms) { return (uint32_t)(((uint64_t)bytes*1000u)/ms/1024u); }

static void emit_uint(uint64_t value,unsigned base,unsigned width,int zero_pad)
{
    static const char digits[]="0123456789abcdef";char b[32];unsigned n=0;
    do{b[n++]=digits[value%base];value/=base;}while(value);
    while(n<width)b[n++]=zero_pad?'0':' ';
    while(n)uarths_write_byte((uint8_t)b[--n]);
}
static void log_line(const char *format,...)
{
    va_list args;va_start(args,format);
    while(*format){if(*format!='%'){uarths_write_byte((uint8_t)*format++);continue;}format++;int zero=0;unsigned width=0;if(*format=='0'){zero=1;format++;}while(*format>='0'&&*format<='9')width=width*10u+(unsigned)(*format++-'0');int wide=*format=='l';if(wide)format++;char spec=*format?*format++:0;
      if(spec=='s'){const char *s=va_arg(args,const char *);uarths_puts(s?s:"(null)");}
      else if(spec=='c')uarths_write_byte((uint8_t)va_arg(args,int));
      else if(spec=='u')emit_uint(wide?va_arg(args,unsigned long):va_arg(args,unsigned int),10,width,zero);
      else if(spec=='x')emit_uint(wide?va_arg(args,unsigned long):va_arg(args,unsigned int),16,width,zero);
      else if(spec=='d'){int v=va_arg(args,int);if(v<0){uarths_write_byte('-');emit_uint((uint64_t)(-(int64_t)v),10,width,zero);}else emit_uint((unsigned)v,10,width,zero);}
      else uarths_write_byte((uint8_t)spec);
    }va_end(args);uarths_puts("\r\n");
}
static void digest_hex(const uint8_t d[32],char out[65])
{
    static const char h[]="0123456789abcdef";for(unsigned i=0;i<32;i++){out[2*i]=h[d[i]>>4];out[2*i+1]=h[d[i]&15u];}out[64]=0;
}
static uint8_t pattern(uint32_t i)
{
    uint32_t x=i+0x9e3779b9u;x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return (uint8_t)x;
}

int main(void)
{
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK,0);sysctl_pll_set_freq(SYSCTL_PLL0,780000000u);sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK,SYSCTL_SOURCE_PLL0);
    fpioa_set_function(4,FUNC_UARTHS_RX);fpioa_set_function(5,FUNC_UARTHS_TX);uarths_init();uint32_t div=sysctl_clock_get_freq(SYSCTL_CLOCK_CPU)/LOG_BAUD;if(div)--div;((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div=(uint16_t)div;
    log_line("SELFTEST:START bytes=%lu flash=0x%08lx sd=absent",(unsigned long)TEST_BYTES,(unsigned long)FLASH_BASE);

    uint64_t t=now();for(uint32_t i=0;i<TEST_BYTES;i++)s_ram[i]=pattern(i);uint32_t ram_write_ms=ms_since(t);
    sha256_stream_t sha;sha256_stream_init(&sha);t=now();for(uint32_t i=0;i<TEST_BYTES;i++){if(s_ram[i]!=pattern(i)){log_line("SELFTEST:FAIL target=RAM offset=%lu",(unsigned long)i);for(;;)__asm__ volatile("wfi");}}sha256_stream_update(&sha,s_ram,sizeof(s_ram));sha256_stream_final(&sha,s_expected);uint32_t ram_read_ms=ms_since(t);digest_hex(s_expected,s_expected_hex);
    log_line("SELFTEST:PASS target=RAM op=write bytes=%lu ms=%lu KiB/s=%lu",(unsigned long)TEST_BYTES,(unsigned long)ram_write_ms,(unsigned long)kib_s(TEST_BYTES,ram_write_ms));
    log_line("SELFTEST:PASS target=RAM op=read_verify bytes=%lu ms=%lu KiB/s=%lu sha256=%s",(unsigned long)TEST_BYTES,(unsigned long)ram_read_ms,(unsigned long)kib_s(TEST_BYTES,ram_read_ms),s_expected_hex);

    t=now();for(uint32_t o=0;o<TEST_BYTES;o+=CHUNK){int rc=k210_flash_erase_4k(FLASH_BASE+o);if(rc){log_line("SELFTEST:FAIL target=SPI3 op=erase offset=%lu rc=%d",(unsigned long)o,rc);for(;;)__asm__ volatile("wfi");}}uint32_t erase_ms=ms_since(t);
    t=now();for(uint32_t o=0;o<TEST_BYTES;o+=CHUNK){int rc=k210_flash_program(FLASH_BASE+o,s_ram+o,CHUNK);if(rc){log_line("SELFTEST:FAIL target=SPI3 op=write offset=%lu rc=%d",(unsigned long)o,rc);for(;;)__asm__ volatile("wfi");}}uint32_t write_ms=ms_since(t);
    sha256_stream_init(&sha);t=now();for(uint32_t o=0;o<TEST_BYTES;o+=CHUNK){int rc=k210_flash_read(FLASH_BASE+o,s_readback,CHUNK);if(rc){log_line("SELFTEST:FAIL target=SPI3 op=read offset=%lu rc=%d",(unsigned long)o,rc);for(;;)__asm__ volatile("wfi");}if(memcmp(s_readback,s_ram+o,CHUNK)){uint32_t i=0;while(i<CHUNK&&s_readback[i]==s_ram[o+i])i++;log_line("SELFTEST:FAIL target=SPI3 op=compare offset=%lu",(unsigned long)(o+i));for(;;)__asm__ volatile("wfi");}sha256_stream_update(&sha,s_readback,CHUNK);}sha256_stream_final(&sha,s_actual);uint32_t read_ms=ms_since(t);digest_hex(s_actual,s_actual_hex);
    if(memcmp(s_actual,s_expected,32)){log_line("SELFTEST:FAIL target=SPI3 op=sha expected=%s actual=%s",s_expected_hex,s_actual_hex);for(;;)__asm__ volatile("wfi");}
    log_line("SELFTEST:PASS target=SPI3 op=erase bytes=%lu ms=%lu KiB/s=%lu",(unsigned long)TEST_BYTES,(unsigned long)erase_ms,(unsigned long)kib_s(TEST_BYTES,erase_ms));
    log_line("SELFTEST:PASS target=SPI3 op=write bytes=%lu ms=%lu KiB/s=%lu",(unsigned long)TEST_BYTES,(unsigned long)write_ms,(unsigned long)kib_s(TEST_BYTES,write_ms));
    log_line("SELFTEST:PASS target=SPI3 op=read_verify bytes=%lu ms=%lu KiB/s=%lu sha256=%s",(unsigned long)TEST_BYTES,(unsigned long)read_ms,(unsigned long)kib_s(TEST_BYTES,read_ms),s_actual_hex);
    log_line("SELFTEST:SUMMARY RAM_WRITE_MS=%lu RAM_READ_MS=%lu SPI3_ERASE_MS=%lu SPI3_WRITE_MS=%lu SPI3_READ_MS=%lu SHA256=%s",
      (unsigned long)ram_write_ms,(unsigned long)ram_read_ms,(unsigned long)erase_ms,
      (unsigned long)write_ms,(unsigned long)read_ms,s_actual_hex);
    log_line("SELFTEST:ALL_PASS");for(;;)__asm__ volatile("wfi");
}
