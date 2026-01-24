#include <fs/fs.h>
#include <fs/tmpfs.h>
#include <stat/stat.h>
#include <string.h>
#include <panic.h>
#include <mem/align.h>
#include <mem/pmm.h>
#include <mem/mmap.h>

#include <stdlib.h>
#include <stdatomic.h>


filesystem_t* tmpfs_create()
{
    tmpfs_t* ret = malloc(sizeof (tmpfs_t));
    ret->filesystem.instantiate = tmpfs_init;
    ret->filesystem.populate = tmpfs_populate;
    ret->filesystem.mount = tmpfs_mount;
    ret->filesystem.create_node = tmpfs_create_node;
    ret->filesystem.symlink = tmpfs_symlink;
    ret->filesystem.link = tmpfs_link;
    ret->filesystem.close = tmpfs_close;
    return (filesystem_t*)ret;
}

filesystem_t* tmpfs_init(filesystem_t* self)
{
    return self;
}

void tmpfs_close(__attribute__((unused)) filesystem_t* self)
{
    // todo: implement a thread-safe `close` function for tmpfs?
    panic("TMPFS should never be closed!");
}

void tmpfs_populate(__attribute__((unused)) filesystem_t* self, __attribute__((unused)) vfs_node_t* node)
{
    // body left deliberately empty (populate is a no-op for tmpfs)
}

// todo: why is `source` unused?  VINIX doesn't use it, but surely we need to mount the source node into the node tree
// in this function?  Unless I've misunderstood something, this may be a bug we have accidentally ported from VINIX
// which wouldn't surprise me, as mounting a sub-filesystem into a tmpfs is an unusual usage pattern so I would
// expect this code to be less thoroughly tested
// if it is a bug, feel free to submit a PR to fix it!
vfs_node_t* tmpfs_mount(filesystem_t* self, vfs_node_t* parent, const char* name, __attribute__((unused)) vfs_node_t* source)
{
    ((tmpfs_t*)self)->dev_id = resource_create_dev_id(); 
    return self->create_node(self, parent, name, 0644 | STAT_IFDIR);
}

vfs_node_t* tmpfs_create_node(filesystem_t* _self, vfs_node_t* parent, const char* name, int mode)
{
    tmpfs_t* self = (tmpfs_t*)_self;
    vfs_node_t* new_node = vfs_create_node(_self, parent, name, stat_is_dir(mode));

    tmpfs_resource_t* new_resource = malloc(sizeof(tmpfs_resource_t));

    new_resource->storage = 0;
    atomic_store(&new_resource->resource.refcount, 1);

    if(stat_is_reg(mode))
    {
        new_resource->capacity = 4096;
        new_resource->storage = malloc(new_resource->capacity);
        new_resource->resource.can_mmap = true;
    }

    new_resource->resource.stat.size = 0;
    new_resource->resource.stat.blocks = 0;
    new_resource->resource.stat.block_size = 512;
    new_resource->resource.stat.device = self->dev_id;
    new_resource->resource.stat.inode = self->inode_counter++;
    new_resource->resource.stat.mode = mode;
    new_resource->resource.stat.nlink = 1;

    new_resource->resource.stat.accessed_time = realtime_clock;
    new_resource->resource.stat.created_time = realtime_clock;
    new_resource->resource.stat.modified_time = realtime_clock;

    new_resource->resource.grow  = tmpfs_resource_grow;
    new_resource->resource.read  = tmpfs_resource_read;
    new_resource->resource.write = tmpfs_resource_write;
    new_resource->resource.ioctl = tmpfs_resource_ioctl;
    new_resource->resource.unref = tmpfs_resource_unref;
    new_resource->resource.link  = tmpfs_resource_link;
    new_resource->resource.unlink= tmpfs_resource_unlink;
    new_resource->resource.mmap  = tmpfs_resource_mmap;

    new_node->resource = (resource_t*)new_resource;

    return new_node;
}

vfs_node_t* tmpfs_symlink(filesystem_t* _self, vfs_node_t* parent, const char* dest, const char* target)
{
    tmpfs_t* self = (tmpfs_t*)_self;
    vfs_node_t* new_node = vfs_create_node(_self, parent, target, false);
    tmpfs_resource_t* new_resource = malloc(sizeof(tmpfs_resource_t));

    new_resource->storage = 0;
    atomic_store(&new_resource->resource.refcount, 1);

    new_resource->resource.stat.size = strlen(target);
    new_resource->resource.stat.blocks = 0;
    new_resource->resource.stat.block_size = 512;
    new_resource->resource.stat.device = self->dev_id;
    new_resource->resource.stat.inode = self->inode_counter++;
    new_resource->resource.stat.mode = STAT_IFLNK | 0777;
    new_resource->resource.stat.nlink = 1;

    new_resource->resource.stat.accessed_time = realtime_clock;
    new_resource->resource.stat.created_time = realtime_clock;
    new_resource->resource.stat.modified_time = realtime_clock;

    new_resource->resource.grow  = tmpfs_resource_grow;
    new_resource->resource.read  = tmpfs_resource_read;
    new_resource->resource.write = tmpfs_resource_write;
    new_resource->resource.ioctl = tmpfs_resource_ioctl;
    new_resource->resource.unref = tmpfs_resource_unref;
    new_resource->resource.link  = tmpfs_resource_link;
    new_resource->resource.unlink= tmpfs_resource_unlink;
    new_resource->resource.mmap  = tmpfs_resource_mmap;

    new_node->resource = (resource_t*) new_resource;

    new_node->symlink_target = dest;

    return new_node;
}

vfs_node_t*tmpfs_link(filesystem_t* self, vfs_node_t* parent, const char* dest, vfs_node_t* target)
{
    vfs_node_t* new_node = vfs_create_node(self, parent, dest, false);

    atomic_fetch_add(&target->resource->refcount, 1);

    new_node->resource = target->resource;
    new_node->children = target->children;

    return new_node;
}

bool tmpfs_resource_grow(resource_t* _self, __attribute__((unused)) void* handle, uint64_t size)
{
    bool rv = true;
    lock_acquire(&_self->lock);

    tmpfs_resource_t* self = (tmpfs_resource_t*)_self;
    size_t new_capacity = self->capacity;
    while(size > new_capacity)
    {
        new_capacity *= 2;
    }

    uint8_t* new_storage = realloc(self->storage, new_capacity);

    if(new_storage == NULL)
    {
        rv = false;
    }
    else
    {
        self->storage = new_storage;
        self->capacity = new_capacity;

        _self->stat.size = size;
        _self->stat.blocks = div_roundup(size, _self->stat.block_size);
    }

    
    lock_release(&_self->lock);
    return rv;
}

int64_t tmpfs_resource_read(resource_t* _self, __attribute__((unused)) void* handle, void* buf, uint64_t loc, uint64_t count)
{
    lock_acquire(&_self->lock);
    tmpfs_resource_t* self = (tmpfs_resource_t*)_self;

    int64_t actual_count = count;
    if( loc + count > _self->stat.size)
    {
        actual_count = count - ((loc + count) - _self->stat.size);
    }

    memcpy(buf, &self->storage[loc], actual_count);

    lock_release(&_self->lock);

    return actual_count;
}

// this whole function is sus
int64_t tmpfs_resource_write(resource_t* _self, __attribute__((unused)) void* handle, void* buf, uint64_t loc, uint64_t count)
{
    lock_acquire(&_self->lock);
    tmpfs_resource_t* self = (tmpfs_resource_t*)_self;

    if ( loc + count > self->capacity)
    {
        size_t new_capacity = self->capacity;

        while (loc + count > new_capacity)
        {
            new_capacity *= 2;
        }

        uint8_t* new_storage = realloc(self->storage, new_capacity);

        if (new_storage == NULL)
        {
            // maybe this can be refactored to get rid of the goto?
            // todo: error handling?
            goto defer;
        }
        self->storage = new_storage;
        self->capacity = new_capacity;
    }

    memcpy(self->storage + loc, buf, count);

    if (loc + count > _self->stat.size)
    {
        _self->stat.size = loc + count;
        _self->stat.blocks = div_roundup(_self->stat.size, _self->stat.block_size);
    }

defer:
    lock_release(&_self->lock);

    return count;
}

int tmpfs_resource_ioctl(__attribute__((unused)) resource_t* _self, void* handle, uint64_t request, void* argp)
{
    return resource_default_ioctl(handle, request, argp);
}

bool tmpfs_resource_unref(resource_t* _self, __attribute__((unused)) void* handle)
{
    tmpfs_resource_t* self = (tmpfs_resource_t*)_self;
    atomic_fetch_sub(&_self->refcount, 1);
    if (_self->refcount == 0 && stat_is_reg(_self->stat.mode))
    {
        free(self->storage);
        free(self);
    }
    return true;
}

bool tmpfs_resource_link(resource_t* _self,  __attribute__((unused)) void* handle)
{
    atomic_fetch_add(&_self->stat.nlink, 1);
    return true;
}

bool tmpfs_resource_unlink(resource_t* _self,  __attribute__((unused)) void* handle)
{
    atomic_fetch_sub(&_self->stat.nlink, 1);
    return true;
}

void* tmpfs_resource_mmap(resource_t* _self, uint64_t page, int flags)
{
    void* rv = NULL;
    lock_acquire(&_self->lock);
    tmpfs_resource_t* self = (tmpfs_resource_t*)_self;

    if((flags & MMAP_MAP_SHARED) != 0)
    {
        rv = (void*)((uint64_t)&(self->storage[page * PAGE_SIZE]) - HIGHER_HALF);
    }
    else
    {
        void* copy_page = pmm_alloc(1);
        memcpy(copy_page + HIGHER_HALF, &self->storage[page * PAGE_SIZE], PAGE_SIZE);
        rv = copy_page;
    }

    lock_release(&_self->lock);
    return rv;
}

