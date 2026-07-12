#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../firmware_v2/common/sha256_stream.h"

#define N (1024u * 1024u)
static uint8_t data[N];
static uint8_t pattern(uint32_t i)
{
    uint32_t x=i+0x9e3779b9u;x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return (uint8_t)x;
}
static void digest(size_t chunk,uint8_t out[32])
{
    sha256_stream_t s;sha256_stream_init(&s);
    for(size_t o=0;o<N;){size_t n=N-o;if(n>chunk)n=chunk;sha256_stream_update(&s,data+o,n);o+=n;}
    sha256_stream_final(&s,out);
}
int main(void)
{
    for(uint32_t i=0;i<N;i++)data[i]=pattern(i);
    const size_t chunks[]={N,4096,48,1};uint8_t expected[32],actual[32];digest(chunks[0],expected);
    for(unsigned i=1;i<sizeof(chunks)/sizeof(chunks[0]);i++){digest(chunks[i],actual);if(memcmp(expected,actual,32)){printf("SHA256_STREAM_CHUNK_FAIL chunk=%zu\n",chunks[i]);return 1;}}
    printf("SHA256_STREAM_CHUNK_PASS bytes=1048576 chunks=1048576,4096,48,1 sha256=");
    for(unsigned i=0;i<32;i++)printf("%02x",expected[i]);
    printf(" first=");for(unsigned i=0;i<16;i++)printf("%02x",data[i]);puts("");return 0;
}
