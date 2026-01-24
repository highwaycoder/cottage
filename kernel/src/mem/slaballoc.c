#include <mem/slaballoc.h>
#include <lock/lock.h>
#include <stdint.h>
#include <mem/pmm.h>
#include <mem/align.h>
#include <string.h>

slab_t slabs[SLAB_COUNT];
static lock_t slab_lock = LOCK_INITIALIZER("slab_lock");

void slaballoc_init()
{
    init_slab(&slabs[0], 8);
    init_slab(&slabs[1], 16);
    init_slab(&slabs[2], 24);
    init_slab(&slabs[3], 32);
    init_slab(&slabs[4], 48);
    init_slab(&slabs[5], 64);
    init_slab(&slabs[6], 128);
    init_slab(&slabs[7], 256);
    init_slab(&slabs[8], 512);
    init_slab(&slabs[9], 1024);
}

void init_slab(slab_t* slab, uint64_t ent_size)
{
    slab->ent_size = ent_size;
    slab->first_free = (uint64_t) pmm_alloc(1);
    slab->first_free += HIGHER_HALF;

    uint64_t avl_size = PAGE_SIZE - align_up(sizeof(slabheader_t), ent_size);
    slabheader_t* slabptr = (slabheader_t*)slab->first_free;
    slabptr->slab = slab;

    slab->first_free += align_up(sizeof(slabheader_t), ent_size);

    uint64_t* arr = (uint64_t*) slab->first_free;
    uint64_t max = avl_size / ent_size - 1;
    uint64_t fact = ent_size / 8;

    for(uint64_t i = 0; i < max; i++)
    {
        arr[i * fact] = (uint64_t) &arr[(i+1) * fact];
    }

    arr[max * fact] = (uint64_t) 0;
}

slab_t* find_slab(uint64_t size)
{
    for(uint64_t i = 0; i < SLAB_COUNT; i++)
    {
        if(slabs[i].ent_size >= size) {
            return &slabs[i];
        }
    }

    return NULL;
}

void* slab_alloc(slab_t* slab)
{
    lock_acquire(&slab_lock);
    if(slab->first_free == 0)
    {
        init_slab(slab, slab->ent_size);
    }

    uint64_t* old_free = (uint64_t*) slab->first_free;
    slab->first_free = *old_free;
    memset((void*)old_free, 0, slab->ent_size);
    lock_release(&slab_lock); 
    return old_free;
}

void slab_free(slab_t* slab, void* ptr)
{
    lock_acquire(&slab_lock);
    if(ptr == NULL) {
        lock_release(&slab_lock);
        return;
    }

    uint64_t* new_head = (uint64_t*) ptr;
    new_head[0] = slab->first_free;

    slab->first_free = (uint64_t) new_head;
    lock_release(&slab_lock);
}
