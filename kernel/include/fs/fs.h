#pragma once

#include <lock/lock.h>
#include <resource/resource.h>

// number of filesystems that are included in the kernel source tree
// increase this if you add a new filesystem, or else get bugs
#define KERNEL_FILESYSTEM_COUNT 3

// the indices into the filesystems[] array for each of the kernel-included
// filesystems.  Modular filesystems are placed after the baked in ones.
typedef enum {
    FS_TMPFS = 0,
    FS_DEVTMPFS = 1,
    FS_EXT2 = 2
} hpr_fsid_t;

typedef struct filesystem_s filesystem_t;
typedef struct vfs_node_s vfs_node_t;

typedef struct filesystem_s {
    filesystem_t* (*instantiate)(filesystem_t* self);
    void (*populate)(filesystem_t* self, vfs_node_t* node);
    vfs_node_t* (*mount)(filesystem_t* self, vfs_node_t* parent, const char* name, vfs_node_t* source);
    vfs_node_t* (*create_node)(filesystem_t* self, vfs_node_t* parent, const char* name, int mode);
    vfs_node_t* (*symlink)(filesystem_t* self, vfs_node_t* parent, const char* dest, const char* target);
    vfs_node_t* (*link)(filesystem_t* self, vfs_node_t* parent, const char* dest, vfs_node_t* target);
    void (*close)(filesystem_t* self);
} filesystem_t;

typedef struct vfs_node_s {
    vfs_node_t* mountpoint;
    vfs_node_t* redir;
    resource_t* resource;
    filesystem_t* filesystem;
    char* name;
    vfs_node_t* parent;
    vfs_node_t* children;

    // -1 means "not a directory", 0 is a directory with 0 children
    int children_count; 

    const char* symlink_target;
} vfs_node_t;

void fs_init();
vfs_node_t* vfs_create_node(filesystem_t* filesystem, vfs_node_t* parent, const char* name, bool dir);
void vfs_add_child(vfs_node_t* parent, vfs_node_t* new_child);
void dir_create_dotentries(vfs_node_t* node, vfs_node_t* parent);
void path2node(vfs_node_t* parent, const char* path, vfs_node_t** parent_out, vfs_node_t** node_out, char** basename_out);
vfs_node_t* node_get_child(vfs_node_t* node, const char* child_name);

bool fs_mount(vfs_node_t* parent, const char* source, const char* target, hpr_fsid_t fs_identifier);
vfs_node_t* fs_create(vfs_node_t* parent, const char* name, int mode);
vfs_node_t* fs_symlink(vfs_node_t* parent, const char* dest, const char* target);
vfs_node_t* fs_get_node(vfs_node_t* parent, const char* path, bool follow_symlinks);

static inline const char* fs_name(hpr_fsid_t fsid)
{
    switch(fsid)
    {
        case FS_TMPFS: return "tmpfs";
        case FS_DEVTMPFS: return "devtmpfs";
        case FS_EXT2: return "ext2";
    }
    return "unknown";
}

// todo: extern these?
// extern lock_t vfs_lock;
extern vfs_node_t* vfs_root;
extern filesystem_t** filesystems;
