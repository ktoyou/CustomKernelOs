#ifndef PAGING_H
#define PAGING_H
#include "types.h"
#include "pmm.h"

/* Флаги записей каталога и таблиц страниц (x86, 32 бит) */
#define PG_PRESENT  0x001
#define PG_WRITE    0x002
#define PG_USER     0x004           /* доступно из ring 3 */
#define PG_HUGE     0x080           /* в PDE: страница 4 МБ (нужен CR4.PSE) */
#define PG_COW      0x200           /* логически записываемая, сейчас только чтение: копия при записи */

/* Карта виртуальной памяти */
#define IDENTITY_MAX 0x40000000u    /* 0..1 ГБ: вся RAM 1:1, только для ядра */
#define USER_BASE    0x40000000u    /* 1..3 ГБ: память программы (своя у каждого процесса) */
#define USER_TOP     0xC0000000u
#define SHM_BASE     0xA0000000u    /* сюда отображается общая память (ipc.c) */
#define SHM_END      0xB0000000u
#define INTERP_BASE  0x80000000u    /* динамический загрузчик (PT_INTERP) */
#define MMAP_BASE    SHM_END        /* mmap: анонимные страницы и MAP_PRIVATE */
#define USER_STACK_PAGES 1024u /* 4 МБ: git; remote-http после фикса time/datetime_get */
#define USER_STACK_BOTTOM (USER_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_GUARD (USER_STACK_BOTTOM - PAGE_SIZE)
#define MMAP_END USER_STACK_GUARD
#define THREAD_STACK_MIN (3u * PAGE_SIZE)
_Static_assert(MMAP_BASE < MMAP_END && MMAP_END + PAGE_SIZE == USER_STACK_BOTTOM, "mmap/stack guard layout");
/* 0xD0000000..: куча ядра (heap.h) — общая для всех каталогов */

void paging_init(void);
void paging_init_ap_cache(void); /* same PAT layout before an AP uses mappings */
u32 paging_video_flags(void);    /* WC when PAT is available, otherwise UC */

/* операции над каталогом ядра */
int  vmm_map(u32 virt, u32 phys, u32 flags);   /* 0 = ок, <0 = ошибка */
u32  vmm_unmap(u32 virt);                      /* -> физ. адрес, 0 если не было */
bool vmm_translate(u32 virt, u32 *phys);       /* в ТЕКУЩЕМ адресном пространстве */

/* адресные пространства процессов: каталог задаётся его физ. адресом */
u32  vmm_kernel_dir(void);
u32  vmm_create_dir(void);                     /* новый каталог с ядром внутри */
int  vmm_map_in(u32 dir, u32 virt, u32 phys, u32 flags);
bool vmm_lookup(u32 dir, u32 virt, u32 *phys, u32 *flags);
u32  vmm_unmap_in(u32 dir, u32 virt);         /* -> физ. адрес; кадр не освобождается */
u32  vmm_destroy_dir(u32 dir);                 /* освободить всё пользовательское -> страниц */
u32  vmm_clone_user(u32 src, u32 *pages);      /* общее отображение памяти (fork); запись копирует страницу */
bool vmm_cow_fault(u32 virt);                  /* page fault на записи: сделать процессу свою копию */
void vmm_switch(u32 dir);                      /* загрузить CR3 */
u32  vmm_current_dir(void);

u32  paging_identity_limit(void);
u32  paging_table_count(void);

#endif
