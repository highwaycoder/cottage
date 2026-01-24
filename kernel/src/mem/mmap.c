// first-party headers
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mem/mmap.h>
#include <mem/align.h>
#include <lock/lock.h>
#include <panic.h>
#include <errors/errno.h>

// standard headers
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Maps a page in a global range and all its local ranges.
// Caller must hold locks for all affected pagemaps.
// Analyzer can't track locks through pointer indirection.
NO_THREAD_SAFETY_ANALYSIS
bool mmap_map_page_in_range(mmap_range_global_t* global_range, uint64_t virt, uint64_t phys, uint64_t prot)
{
    uint64_t pt_flags = PTE_FLAG_PRESENT | PTE_FLAG_USER;
    if ((prot & MMAP_PROT_WRITE) != 0)
    {
        pt_flags |= PTE_FLAG_WRITABLE;
    }

	// so good, we map it twice? maybe bug...
    if(!map_page(&global_range->shadow_pagemap, virt, phys, pt_flags)) return false;
    if(!map_page(&global_range->shadow_pagemap, virt, phys, pt_flags)) return false;

    for(uint64_t i = 0; i < global_range->num_locals; i++)
    {
        mmap_range_local_t* local = global_range->locals[i];
        if (virt < local->base || virt >= local->base + local->length)
            continue;
        
        if(!map_page(local->pagemap, virt, phys, pt_flags)) return false;
    }
    return true;
}

// todo: we need some much better naming going on in this function,
// it's all over the place
// Acquires pagemap->lock internally. Analyzer can't track lock through pointer.
NO_THREAD_SAFETY_ANALYSIS
bool mmap_map_range(pagemap_t* pagemap, uint64_t virt, uint64_t phys, uint64_t size, uint64_t prot, uint64_t _flags)
{
    uint64_t flags = _flags | MMAP_MAP_ANON; 

    uint64_t virt_addr = align_down(virt, PAGE_SIZE);
    uint64_t length = align_up(size + (virt_addr - virt), PAGE_SIZE);
    mmap_range_local_t* range_local = malloc(sizeof (mmap_range_local_t));
    *range_local = (mmap_range_local_t) {
        .pagemap = pagemap,
        .base = virt_addr,
        .length = length,
        .prot = prot,
        .flags = flags,
        .global = NULL,
    };

    mmap_range_global_t* range_global = malloc(sizeof(mmap_range_global_t));
    
    *range_global = (mmap_range_global_t) {
        .locals = malloc(sizeof(mmap_range_local_t*)),
        .num_locals = 1,
        .base = virt_addr,
        .length = length,
        .resource = NULL,
        .shadow_pagemap = (pagemap_t) {
            .top_level = 0
        }
    };

    range_local->global = range_global;
    range_global->locals[0] = range_local;

    range_global->shadow_pagemap.top_level = pmm_alloc(1);
    range_global->shadow_pagemap.lock = (lock_t)LOCK_INITIALIZER("shadow_pagemap->lock");

    // Hold the lock for the entire operation - both updating mmap_ranges
    // and performing the actual page mappings. This prevents races in
    // get_next_level() when two threads map addresses sharing page table levels.
    lock_acquire(&pagemap->lock);

    pagemap->mmap_ranges = realloc(pagemap->mmap_ranges, sizeof(void*) * pagemap->mmap_range_count+1);
    pagemap->mmap_ranges[pagemap->mmap_range_count] = range_local;
    pagemap->mmap_range_count++;

    for(uint64_t i = 0; i < length; i+= PAGE_SIZE)
    {
        mmap_map_page_in_range(range_global, virt_addr + i, phys + i, prot);
    }

    lock_release(&pagemap->lock);
    return true;
}

// Caller must hold pagemap->lock.
// Analyzer can't track lock through pointer.
NO_THREAD_SAFETY_ANALYSIS
bool addr2range(pagemap_t* pagemap, uint64_t addr, mmap_range_local_t** range_out, uint64_t* memory_page_out, uint64_t* file_page_out)
{
    for(uint64_t i = 0; i < pagemap->mmap_range_count; i++)
    {
        *range_out = pagemap->mmap_ranges[i];
        if (addr >= (*range_out)->base && addr < (*range_out)->base + (*range_out)->length)
        {
            *memory_page_out = addr / PAGE_SIZE;
            *file_page_out = (uint64_t)((*range_out)->offset / PAGE_SIZE + (memory_page_out - (*range_out)->base / PAGE_SIZE));
            return true;
        }
    }
    return false;
}

// Caller must hold pagemap->lock.
// Analyzer can't track lock through pointer.
NO_THREAD_SAFETY_ANALYSIS
bool munmap(pagemap_t* pagemap, uint64_t addr, uint64_t length)
{
    if (length == 0)
    {
        set_errno(EINVAL);
        return false;
    }

    uint64_t actual_length = align_up(length, PAGE_SIZE);

    for(uint64_t i = addr; i < (addr + actual_length); i++)
    {
        mmap_range_local_t* local_range;
        uint64_t memory_page;
        uint64_t file_page;
        // if the address is not in any of the mmap'd ranges in this pagemap,
        // skip the rest of this loop
        if(!addr2range(pagemap, i, &local_range, &memory_page, &file_page)) continue;
        mmap_range_global_t* global_range = local_range->global;

        uint64_t snip_begin = i;
        do
        {
            i += PAGE_SIZE;
        } while (i < local_range->base + local_range->length &&
                i < addr + length);
        uint64_t snip_end = i;        

        uint64_t snip_size = snip_end - snip_begin;
        if (snip_begin > local_range->base && snip_end < local_range->base + local_range->length)
        {
            mmap_range_local_t* postsplit_range = malloc(sizeof (mmap_range_local_t));
            *postsplit_range = (mmap_range_local_t){
                .pagemap = local_range->pagemap,
                .base = snip_end,
                .length = (local_range->base + local_range->length) - snip_end,
                .offset = local_range->offset + (int64_t)(snip_end - local_range->base),
                .prot = local_range->prot,
                .flags = local_range->flags,
                .global = local_range->global
            };
            pagemap->mmap_ranges = realloc(pagemap->mmap_ranges, sizeof(void*) * pagemap->mmap_range_count + 1);
            pagemap->mmap_ranges[pagemap->mmap_range_count] = postsplit_range;
            pagemap->mmap_range_count++;
            local_range->length -= postsplit_range->length;
        }

        for (uint64_t j = snip_begin; j < snip_end; j += PAGE_SIZE)
        {
            unmap_page(pagemap, j);
        }

        if(snip_size == local_range->length)
        {
            pagemap->mmap_range_count--;
            pagemap->mmap_ranges = realloc(pagemap->mmap_ranges, sizeof(void*)*pagemap->mmap_range_count);
        }

        if(snip_size == local_range->length && global_range->num_locals == 1)
        {
            if ((local_range->flags & MMAP_MAP_ANON) != 0)
            {
                for (size_t j = 0; j < global_range->base + global_range->length; j+= PAGE_SIZE)
                {
                    uint64_t phys = 0;
                    if(!virt2phys(pagemap, j, &phys)) continue;
                    if(!unmap_page(&global_range->shadow_pagemap, j))
                    {
                        set_errno(EINVAL);
                        return false;
                    }
                    pmm_free((void*)phys, 1);
                }
            }
            free(local_range);
        }
        else
        {
            if (snip_begin == local_range->base)
            {
                local_range->offset += (int64_t)snip_size;
                local_range->base = snip_end;
            }
            local_range->length -= snip_size;
        }
    }
    return true;
}

// Acquires old_pagemap->lock internally.
// Analyzer can't track lock through pointer.
NO_THREAD_SAFETY_ANALYSIS
pagemap_t* mmap_fork_pagemap(pagemap_t* old_pagemap)
{
    pagemap_t* pagemap = malloc(sizeof(pagemap_t));
    *pagemap = new_pagemap();

    lock_acquire(&old_pagemap->lock);

    for(size_t i = 0; i < old_pagemap->mmap_range_count; i++)
    {
        mmap_range_local_t* local_range = old_pagemap->mmap_ranges[i];
        mmap_range_global_t* global_range = local_range->global;

        mmap_range_local_t* new_local_range = malloc(sizeof(mmap_range_local_t));

        *new_local_range = (mmap_range_local_t){
            .pagemap = NULL,
            .global = NULL,
        };

        new_local_range[0] = local_range[0];
        if(global_range->resource != NULL)
        {
            global_range->resource->refcount++;
        }

        if((local_range->flags & MMAP_MAP_SHARED) != 0)
        {
            global_range->locals = realloc(global_range->locals, sizeof(mmap_range_local_t*) * global_range->num_locals + 1);
            global_range->locals[global_range->num_locals] = new_local_range;
            global_range->num_locals++;
            for (size_t i = 0; i < local_range->base + local_range->length; i+= PAGE_SIZE)
            {
                uint64_t* old_pte = virt2pte(old_pagemap, i, false);
                if(old_pte == NULL) continue;
                uint64_t* new_pte = virt2pte(pagemap, i, true);
                if(new_pte == NULL) {
                    lock_release(&old_pagemap->lock);
                    return NULL;
                }
                new_pte[0] = old_pte[0];
            }
        } else {
            mmap_range_global_t* new_global_range = malloc(sizeof (mmap_range_global_t));
            *new_global_range = (mmap_range_global_t) {
                .resource = global_range->resource,
                .base = global_range->base,
                .length = global_range->length,
                .offset = global_range->offset,
                .shadow_pagemap = (pagemap_t){
                    .top_level = pmm_alloc(1)
                },
                .locals = NULL,
                .num_locals = 0,
            };
            if ((local_range->flags & MMAP_MAP_ANON) != 0)
            {
                for(uint64_t i = local_range->base; i < local_range->base + local_range->length; i+= PAGE_SIZE)
                {
                    uint64_t* old_pte = virt2pte(old_pagemap, i, false);
                    if((old_pte[0] & 1) == 0) continue;
                    uint64_t* new_pte = virt2pte(pagemap, i, true);
                    if(new_pte == NULL) 
                    {
                        lock_release(&old_pagemap->lock);
                        return NULL;
                    }
                    uint64_t* new_spte = virt2pte(&new_global_range->shadow_pagemap, i, true);
                    if(new_spte == NULL) 
                    {
                        lock_release(&old_pagemap->lock);
                        return NULL;
                    }
                    void* page = pmm_alloc(1);
                    memcpy(page + HIGHER_HALF, (void*) (old_pte[0] & (~(uint64_t)0xfff)) + HIGHER_HALF, PAGE_SIZE);
                    new_pte[0] = (old_pte[0] & (uint64_t)0xfff) | (uint64_t)page;
                    new_spte[0] = new_pte[0];
                }
            }
            else
            {
                panic("Non-anonymous pagemap fork!  This should never happen");
            }
        }

        pagemap->mmap_ranges = realloc(pagemap->mmap_ranges, sizeof(mmap_range_local_t*) * pagemap->mmap_range_count + 1);
        pagemap->mmap_ranges[pagemap->mmap_range_count] = new_local_range;
        pagemap->mmap_range_count++;
    }

    lock_release(&old_pagemap->lock);
    return pagemap;
}
