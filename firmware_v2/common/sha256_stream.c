#include "sha256_stream.h"
#include <string.h>

static const uint32_t k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void transform(sha256_stream_t *c, const uint8_t b[64])
{
    uint32_t w[64], a,bv,d,e,f,g,h,cc;
    for (unsigned i=0;i<16;i++) w[i]=be32(b+4*i);
    for (unsigned i=16;i<64;i++) { uint32_t x=w[i-15],y=w[i-2]; w[i]=(ror(y,17)^ror(y,19)^(y>>10))+w[i-7]+(ror(x,7)^ror(x,18)^(x>>3))+w[i-16]; }
    a=c->state[0]; bv=c->state[1]; cc=c->state[2]; d=c->state[3]; e=c->state[4]; f=c->state[5]; g=c->state[6]; h=c->state[7];
    for (unsigned i=0;i<64;i++) { uint32_t t1=h+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^((~e)&g))+k[i]+w[i]; uint32_t t2=(ror(a,2)^ror(a,13)^ror(a,22))+((a&bv)^(a&cc)^(bv&cc)); h=g;g=f;f=e;e=d+t1;d=cc;cc=bv;bv=a;a=t1+t2; }
    c->state[0]+=a;c->state[1]+=bv;c->state[2]+=cc;c->state[3]+=d;c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;
}
void sha256_stream_init(sha256_stream_t *c)
{
    static const uint32_t init[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->state,init,sizeof(init)); c->total_bytes=0;c->block_bytes=0;
}
void sha256_stream_update(sha256_stream_t *c, const void *data, size_t size)
{
    const uint8_t *p=(const uint8_t *)data; c->total_bytes+=size;
    if(c->block_bytes){size_t n=64u-c->block_bytes;if(n>size)n=size;memcpy(c->block+c->block_bytes,p,n);c->block_bytes+=(uint32_t)n;p+=n;size-=n;if(c->block_bytes==64u){transform(c,c->block);c->block_bytes=0;}}
    while(size>=64u){transform(c,p);p+=64u;size-=64u;}
    if(size){memcpy(c->block,p,size);c->block_bytes=(uint32_t)size;}
}
void sha256_stream_final(sha256_stream_t *c, uint8_t digest[32])
{
    uint64_t bits=c->total_bytes*8u; uint8_t pad[128]={0x80}; size_t n=c->block_bytes<56u?56u-c->block_bytes:120u-c->block_bytes;
    sha256_stream_update(c,pad,n); uint8_t len[8]; for(unsigned i=0;i<8;i++)len[7-i]=(uint8_t)(bits>>(i*8)); sha256_stream_update(c,len,8);
    for(unsigned i=0;i<8;i++){digest[4*i]=(uint8_t)(c->state[i]>>24);digest[4*i+1]=(uint8_t)(c->state[i]>>16);digest[4*i+2]=(uint8_t)(c->state[i]>>8);digest[4*i+3]=(uint8_t)c->state[i];}
}
