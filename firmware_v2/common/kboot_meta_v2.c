#include "kboot_meta_v2.h"
#include "k210_flash.h"
#include "kupdate_v2.h"
#include <stddef.h>
#include <string.h>

typedef struct journal_scan {
    int valid;
    kboot_meta_v2_t newest;
    uint32_t free_offset;
} journal_scan_t;

static int record_valid(const kboot_meta_v2_t *m)
{
    if(m->magic!=KBOOT_META_MAGIC||m->version!=KBOOT_META_VERSION||
       m->record_bytes!=sizeof(*m)||m->commit!=KBOOT_META_COMMIT)return 0;
    if(m->active_slot>1u||m->confirmed_slot>1u||
       (m->pending_slot!=KBOOT_SLOT_NONE&&m->pending_slot>1u))return 0;
    return m->crc32==kupdate_v2_crc32(m,offsetof(kboot_meta_v2_t,crc32));
}
static int empty_record(const kboot_meta_v2_t *m)
{
    const uint32_t *p=(const uint32_t *)m;for(unsigned i=0;i<sizeof(*m)/4u;i++)if(p[i]!=0xffffffffu)return 0;return 1;
}
static int scan(uint32_t base,journal_scan_t *s)
{
    memset(s,0,sizeof(*s));s->free_offset=0xffffffffu;
    for(uint32_t o=0;o<KBOOT_META_JOURNAL_BYTES;o+=sizeof(kboot_meta_v2_t)){
        kboot_meta_v2_t m;if(k210_flash_read(base+o,&m,sizeof(m)))return -1;
        if(empty_record(&m)){if(s->free_offset==0xffffffffu)s->free_offset=o;continue;}
        if(record_valid(&m)&&(!s->valid||(int32_t)(m.generation-s->newest.generation)>0)){s->valid=1;s->newest=m;}
    }return 0;
}
void kboot_meta_v2_default(kboot_meta_v2_t *m)
{
    memset(m,0,sizeof(*m));m->magic=KBOOT_META_MAGIC;m->version=KBOOT_META_VERSION;m->record_bytes=sizeof(*m);m->active_slot=0;m->confirmed_slot=0;m->pending_slot=KBOOT_SLOT_NONE;
}
int kboot_meta_v2_load(kboot_meta_v2_t *m)
{
    journal_scan_t a,b;if(!m)return -1;if(scan(KBOOT_META_JOURNAL0,&a)||scan(KBOOT_META_JOURNAL1,&b))return -2;
    if(!a.valid&&!b.valid){kboot_meta_v2_default(m);return 1;}
    *m=(!b.valid||(a.valid&&(int32_t)(a.newest.generation-b.newest.generation)>0))?a.newest:b.newest;return 0;
}
int kboot_meta_v2_append(kboot_meta_v2_t *m)
{
    journal_scan_t a,b;uint32_t base,off;if(!m)return -1;if(scan(KBOOT_META_JOURNAL0,&a)||scan(KBOOT_META_JOURNAL1,&b))return -2;
    journal_scan_t *cur=(!b.valid||(a.valid&&(int32_t)(a.newest.generation-b.newest.generation)>0))?&a:&b;
    base=(cur==&a)?KBOOT_META_JOURNAL0:KBOOT_META_JOURNAL1;off=cur->free_offset;
    if(off==0xffffffffu){base=(base==KBOOT_META_JOURNAL0)?KBOOT_META_JOURNAL1:KBOOT_META_JOURNAL0;for(uint32_t e=0;e<KBOOT_META_JOURNAL_BYTES;e+=0x1000u)if(k210_flash_erase_4k(base+e))return -3;off=0;}
    m->magic=KBOOT_META_MAGIC;m->version=KBOOT_META_VERSION;m->record_bytes=sizeof(*m);m->generation=(a.valid||b.valid)?((a.valid&&(!b.valid||(int32_t)(a.newest.generation-b.newest.generation)>0)?a.newest.generation:b.newest.generation)+1u):1u;
    m->crc32=kupdate_v2_crc32(m,offsetof(kboot_meta_v2_t,crc32));m->commit=0xffffffffu;
    if(k210_flash_program(base+off,m,offsetof(kboot_meta_v2_t,commit)))return -4;
    uint32_t commit=KBOOT_META_COMMIT;if(k210_flash_program(base+off+offsetof(kboot_meta_v2_t,commit),&commit,sizeof(commit)))return -5;m->commit=commit;return 0;
}

int kboot_meta_v2_confirm_running(uint8_t slot)
{
    kboot_meta_v2_t m;
    if (slot > 1u) return -1;
    int rc = kboot_meta_v2_load(&m);
    if (rc != 0) return rc > 0 ? -2 : rc;
    if (m.active_slot != slot) return -3;
    if (m.pending_slot == KBOOT_SLOT_NONE && m.confirmed_slot == slot && m.boot_attempts == 0u)
        return 1;
    if (m.pending_slot != slot || m.boot_attempts != 1u) return -4;
    m.confirmed_slot = slot;
    m.pending_slot = KBOOT_SLOT_NONE;
    m.boot_attempts = 0u;
    return kboot_meta_v2_append(&m);
}
