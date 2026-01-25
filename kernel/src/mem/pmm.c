#include <mem/pmm.h>
#include <mem/align.h> 

#include <klog/klog.h>
#include <panic.h>
#include <limine.h>
#include <stdbool.h>
#include <string.h>
#include <mem/slaballoc.h>
#include <lock/lock.h>

void bitmap_setbit(size_t index);
void bitmap_resetbit(size_t index);
bool bitmap_testbit(size_t index);

// this is called pmm_avl_page_count in the VINIX source,
// however I don't know what "AVL" stands for so I am omitting it.
static uint64_t pmm_page_count;
static void* pmm_bitmap;
static size_t free_pages;

// PMM lock - protects all PMM state during allocation and freeing.
// Must be held when accessing:
// - pmm_bitmap (via bitmap_testbit/setbit/resetbit)
// - last_used_index
// - free_pages
// Acquired by pmm_alloc() and pmm_free() for the duration of their operations.
static lock_t pmm_lock;

void pmm_init(struct limine_memmap_response* memmap)
{
    pmm_lock = (lock_t)LOCK_INITIALIZER("pmm_lock");

    uint64_t first_free_page = UINT64_MAX;
    uint64_t highest_address = 0;
    struct limine_memmap_entry** entries = memmap->entries;
    klog("pmm", "Found these memory regions:");
    
    for(size_t i = 0; i < memmap->entry_count; i++)
    {
        klog("pmm", "\tbase=%x length=%d type=%x", entries[i]->base, entries[i]->length, entries[i]->type);
        // skip non-usable, non-reclaimable regions
        if(entries[i]->type != LIMINE_MEMMAP_USABLE
            && entries[i]->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
        ) continue;

        uint64_t top = entries[i]->base + entries[i]->length;
        if(top > highest_address) {
            highest_address = top;
        }
    }

    pmm_page_count = highest_address / PAGE_SIZE;
    size_t bitmap_size = align_up(pmm_page_count / 8, PAGE_SIZE);

    klog("pmm", "Bitmap size: %llu bytes (%llu bits)", bitmap_size, bitmap_size * 8);

    for(size_t i = 0; i < memmap->entry_count; i++)
    {
        if (entries[i]->type != LIMINE_MEMMAP_USABLE) continue;
        if (entries[i]->length >= bitmap_size) 
        {
            pmm_bitmap = (void*)(entries[i]->base + HIGHER_HALF);

            // initialize all parts of the bitmap as not available
            memset(pmm_bitmap, 0xFF, bitmap_size);

            // hide the bitmap from the free region populator
            entries[i]->length -= bitmap_size;
            entries[i]->base += bitmap_size;
            
            // only allocate the bitmap once, when you first find a region big enough
            break;
        }
    }

    // populate the bitmap with free entries
    for(size_t i = 0; i < memmap->entry_count; i++) {
        // skip unusable regions
        if(entries[i]->type != LIMINE_MEMMAP_USABLE) continue;
        
        if(first_free_page == UINT64_MAX) {
            first_free_page = entries[i]->base;
        }

        for(size_t j = 0; j < entries[i]->length; j += PAGE_SIZE) {
            free_pages++;
            bitmap_resetbit((entries[i]->base + j) / PAGE_SIZE);
        }
    }

    klog("pmm", "Free pages: %llu", free_pages);
    klog("pmm", "First free page is at: %x", first_free_page);

    
    slaballoc_init();
}

static size_t last_used_index = 0;

// uses a basic next-fit allocation model to find contiguous regions
// of physical memory
void* inner_alloc(size_t count, size_t limit)
{
    size_t p = 0;
    while(last_used_index < limit) {
        if(!bitmap_testbit(last_used_index))
        {
            last_used_index++;
            p++;
            if (p == count) {
                size_t page = last_used_index - count;
                for(size_t i = page; i < page + count; i++)
                {
                    bitmap_setbit(i);
                }
                return (void*) (page * PAGE_SIZE);
            }
        } 
        else 
        {
            last_used_index++;
            p = 0;
        }
    }
    return NULL;
}

void* pmm_alloc(size_t count)
{
    lock_acquire(&pmm_lock);
    size_t last = last_used_index;

    void* ret = inner_alloc(count, pmm_page_count);
    if (ret == NULL) 
    {
        last_used_index = 0;
        ret = inner_alloc(count, last);
        if (ret == NULL)
        {
            klog("pmm", "tried to allocate %d pages, free_pages=%d", count, free_pages);
            panic("Kernel OOM");
        }
    }

    free_pages -= count;

    // zero out the freshly allocated memory before passing it back to the caller
    void* ptr = ret + HIGHER_HALF;
    memset(ptr, 0, count * PAGE_SIZE);

    lock_release(&pmm_lock);

    return ret;
}

void pmm_free(void* ptr, size_t count)
{
    lock_acquire(&pmm_lock);
    size_t page = (uint64_t)ptr / PAGE_SIZE;
    for(size_t i = page; i < page + count; i++)
    {
        bitmap_resetbit(i);
    }
    free_pages += count;
    lock_release(&pmm_lock);
}

// this is a little bit magic, ported from VINIX, and tests a single bit
// in a bitmap.  I'm not exactly sure how it works, so don't ask me to debug.
bool bitmap_testbit(size_t index)
{
    size_t bits_type = sizeof(uint64_t) * 8;
    size_t test_index = index % bits_type;
    size_t test_sample = ((uint64_t*)pmm_bitmap)[index / bits_type];
    return ((test_sample >> test_index) & (uint64_t)(1)) != 0;
}

// this is a little bit magic, ported from VINIX, and sets a single bit
// in a bitmap.  I'm not exactly sure how it works, so don't ask me to debug.
void bitmap_setbit(size_t index)
{
    size_t bits_type = sizeof(uint64_t) * 8;
    size_t test_index = index % bits_type;
    ((uint64_t*)pmm_bitmap)[index / bits_type] |= ((uint64_t)(1) << test_index);
}

// this is a little bit magic, ported from VINIX, and resets a single bit
// in a bitmap.  I'm not exactly sure how it works, so don't ask me to debug.
void bitmap_resetbit(size_t index)
{
    size_t bits_type = sizeof(uint64_t) * 8;
    size_t test_index = index % bits_type;
    ((uint64_t*)pmm_bitmap)[index / bits_type] &= ~((uint64_t)(1) << test_index);
}


