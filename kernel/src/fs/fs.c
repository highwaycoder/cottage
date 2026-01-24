#include <fs/fs.h>
#include <fs/tmpfs.h>
#include <fs/devtmpfs.h>
#include <lock/lock.h>
#include <klog/klog.h>
#include <debug/debug.h>
#include <errors/errno.h>
#include <errno.h>

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

// Global VFS lock - protects the VFS tree structure
// Must be held when:
// - Modifying any vfs_node_t's children array or children_count
// - Traversing the VFS tree (path2node, node_get_child, reduce_node)
// - Modifying mountpoints or redirects
lock_t vfs_lock;
vfs_node_t* vfs_root;
filesystem_t** filesystems;
// Atomic because it's read in fs_mount() while potentially being
// modified during filesystem registration
_Atomic size_t num_filesystems;

vfs_node_t* vfs_create_node(filesystem_t* filesystem, vfs_node_t* parent, const char* name, bool dir)
{
    vfs_node_t* node = malloc(sizeof(vfs_node_t));

    // copy the name into the name field, so we know this node "owns" its own memory
    node->name = malloc(strlen(name)+1);
    memcpy(node->name, name, strlen(name));

    node->parent = parent;
    node->mountpoint = NULL;
    node->redir = NULL;
    node->children = NULL;
    node->children_count = -1;
    node->resource = malloc(sizeof(resource_t));
    node->filesystem = filesystem;

    if(dir)
    {
        // pre-allocate the first child -- adding children should always
        // populate the current node, and pre-allocate the next node 
        node->children = malloc(sizeof(vfs_node_t));
        node->children_count = 0;
    }


    return node;
}

// INTERNAL: Caller must hold vfs_lock
// Adds a child node to a parent's children array.
// WARNING: This function calls realloc() which may move the children array,
// invalidating any pointers into it. Callers must not hold such pointers
// across calls to this function.
void vfs_add_child(vfs_node_t* parent, vfs_node_t* new_child)
{
    // the last node is already pre-allocated for us, so just copy the new child in
    parent->children[parent->children_count] = *new_child;
    parent->children_count++;
    // pre-allocate the next child
    parent->children = realloc(parent->children, sizeof(vfs_node_t*) * (parent->children_count + 1));
}

void fs_init()
{
   // Initialize the global VFS lock
   vfs_lock = (lock_t)LOCK_INITIALIZER("vfs_lock");

   vfs_root = vfs_create_node(NULL, NULL, "", false);

   filesystems = malloc(sizeof(filesystem_t*) * KERNEL_FILESYSTEM_COUNT);
   filesystems[FS_TMPFS] = tmpfs_create();

   filesystems[FS_DEVTMPFS] = devtmpfs_create();

   atomic_store(&num_filesystems, 2);
   // todo: finish implementing ext2 support
   //filesystems[FS_EXT2] = ext2_init();
}

// INTERNAL: Caller must hold vfs_lock
// Follows redirects and mountpoints to get the "real" node.
vfs_node_t* reduce_node(vfs_node_t* node, bool follow_symlinks)
{
    if (node->redir != 0)
    {
        return reduce_node(node->redir, follow_symlinks);
    }
    if (node->mountpoint != 0)
    {
        return reduce_node(node->mountpoint, follow_symlinks);
    }
    if (follow_symlinks && node->symlink_target != NULL && strlen(node->symlink_target) != 0)
    {
        vfs_node_t* parent_node, *next_node;
        char* basename;
        path2node(node->parent, node->symlink_target, &parent_node, &next_node, &basename);
        // we don't use the basename
        free(basename);
        if (next_node == NULL)
        {
            return NULL;
        }
        return reduce_node(next_node, follow_symlinks);
    }
    
    return node;
}

// INTERNAL: Caller must hold vfs_lock
// Expects child_name to be zero-terminated string containing only the
// specific child we are looking for.  Do not pass in a subpath, as we will
// not split it! (this behaviour may change in future versions)
//
// WARNING: Returns a pointer into node->children array. This pointer becomes
// invalid if vfs_add_child() is called on the same parent (due to realloc).
// Caller must hold vfs_lock for the lifetime of the returned pointer.
vfs_node_t* node_get_child(vfs_node_t* node, const char* child_name)
{
    klog("fs", "node_get_child node=%x (node->name=%s) child_name=%s children=%d",
        node,
        node->name,
        child_name,
        node->children_count
    );
    // very simple implementation so we just do a linear search,
    // in future versions we may implement some kind of hash table for node
    // children, to speed things up when we're looking through large directories
    for(int i = 0; i < node->children_count; i++)
    {
        // skip entries where the names are different lengths (stops us matching prefixes by accident)
        if(strlen(node->children[i].name) != strlen(child_name)) {
            klog("fs", "skipping names with different lengths: needle=%s mismatch=%s", child_name, node->children[i].name);
            continue;
        }
        if(strncmp(node->children[i].name, child_name, strlen(child_name)) == 0)
        {
            klog("fs", "found %s, returning %x", child_name, &node->children[i]);
            return &node->children[i];
        }
        else
        {
            klog("fs", "node names did not match node=%s needle=%s", node->children[i].name, child_name);
        }
    }
    return NULL;
}

// INTERNAL: Caller must hold vfs_lock
// Resolves a path string to VFS nodes.
// todo: support passing in NULL as the last three parameters if we aren't interested in the return value
//
// WARNING: Returns pointers into the VFS tree. These pointers become invalid
// if the tree structure changes. Caller must hold vfs_lock for the lifetime
// of the returned pointers.
void path2node(vfs_node_t* parent, const char* path, vfs_node_t** parent_out, vfs_node_t** node_out, char** basename_out)
{
    if(strlen(path) == 0)
    {
        set_errno(ENOENT);
        *parent_out = NULL;
        *node_out = NULL;
        *basename_out = NULL;
        return;
    }

    uint64_t index = 0;
    vfs_node_t* current_node = reduce_node(parent, false);

    if (path[index] == '/')
    {
        // it's an absolute path, so start from vfs_root instead
        current_node = reduce_node(vfs_root, false);
        while(path[index] == '/') // eat up all extraneous leading `/` characters
        {
            if(index == strlen(path) - 1)
            {
                // if we only have `/` characters, we're essentially at the root
                *parent_out = current_node;
                *node_out = current_node;
                *basename_out = NULL;
                return;
            }
            index++;
        }
    }

    while(1)
    {
        // warning! this pointer sometimes gets returned, do not free() in those cases
        // - we expect the pointer to be free'd when it is copied into a structure
        char* path_elem = malloc(strlen(path));
        size_t i = 0;
        // get the next component from the path string
        while(index < strlen(path) && path[index] != '/')
        {
            path_elem[i++] = path[index++];
        }

        // eat up all the extraneous `/` separators
        while(index < strlen(path) && path[index] == '/') 
        {
            index++;
        }

        bool last = index == strlen(path);
        // handle paths with trailing / -- this is still the last element,
        // but we expect it to be a directory.
        // (we should maybe error if it's not a directory?)
        if (!last && index == strlen(path) - 1 && path[index] == '/')
        {
            last = true;
        }

        current_node = reduce_node(current_node, false);

        vfs_node_t* new_node = node_get_child(current_node, path_elem);
        if(new_node == NULL)
        {
            set_errno(ENOENT);
            if(last)
            {
                *parent_out = current_node;
                *node_out = NULL;
                *basename_out = path_elem;
                return;
            }
            *parent_out = NULL;
            *node_out = NULL;
            *basename_out = NULL;
            return;
        }

        new_node = reduce_node(new_node, false);

        if(last)
        {
            *parent_out = current_node;
            *node_out = new_node;
            *basename_out = path_elem;
            return;
        }

        current_node = new_node;

        if(stat_is_lnk(current_node->resource->stat.mode))
        {
            vfs_node_t* parent, *current;
            char* basename;
            path2node(current_node->parent, current_node->symlink_target, &parent, &current, &basename);
            if(current == NULL)
            {
                free(path_elem);
                *parent_out = NULL;
                *node_out = NULL;
                *basename_out = NULL;
                return;
            }
            free(path_elem);
            continue;
        }

        if(!stat_is_dir(current_node->resource->stat.mode))
        {
            set_errno(ENOTDIR);
            *parent_out = NULL;
            *node_out = NULL;
            *basename_out = NULL;
            return;
        }

        free(path_elem);
    }

    set_errno(ENOENT);
    *parent_out = NULL;
    *node_out = NULL;
    *basename_out = NULL;
    return;
}

vfs_node_t* fs_symlink(vfs_node_t* parent, const char* dest, const char* target)
{
    lock_acquire(&vfs_lock);

    vfs_node_t *parent_of_tgt;
    vfs_node_t *target_node;
    char* basename;
    path2node(parent, target, &parent_of_tgt, &target_node, &basename);

    if (target_node != NULL || parent_of_tgt != NULL)
    {
        set_errno(EEXIST);
        // if we allocated a basename in the path2node function, we will need to free it here
        free(basename);
        lock_release(&vfs_lock);
        return NULL;
    }

    target_node = parent_of_tgt->filesystem->symlink(parent_of_tgt->filesystem, parent_of_tgt, dest, basename);
    free(basename);

    vfs_add_child(parent_of_tgt, target_node);

    lock_release(&vfs_lock);
    return target_node;
}

bool fs_mount(vfs_node_t* parent, const char* source, const char* target, hpr_fsid_t fs_identifier)
{
    // Check filesystem identifier before acquiring lock (read-only check)
    if(fs_identifier > atomic_load(&num_filesystems) - 1)
    {
        klog("fs", "Mount failed: invalid filesystem identifier %d", fs_identifier);
        return false;
    }

    lock_acquire(&vfs_lock);

    vfs_node_t* source_node = NULL;
    if (strlen(source) != 0)
    {
        vfs_node_t* __parent;
        char* basename;
        path2node(parent, source, &__parent, &source_node, &basename);
        free(basename);
        if(source_node == NULL || stat_is_dir(source_node->resource->stat.mode))
        {
            klog("fs", "Mount failed: invalid source, or source is directory");
            lock_release(&vfs_lock);
            return false;
        }
    }

    vfs_node_t* target_node;
    vfs_node_t* parent_of_tgt_node;
    char* basename;
    path2node(parent, target, &parent_of_tgt_node, &target_node, &basename);

    // path2node returns NULL basename for root path "/", use empty string instead
    if (basename == NULL)
    {
        basename = malloc(1);
        basename[0] = '\0';
    }

    bool mounting_root = target_node == vfs_root;

    if (target_node == NULL || (!mounting_root && !stat_is_dir(target_node->resource->stat.mode)))
    {
        klog("fs", "Mount failed: target is not directory");
        free(basename);
        lock_release(&vfs_lock);
        return false;
    }


    filesystem_t* f_sys = filesystems[fs_identifier]->instantiate(filesystems[fs_identifier]);

    vfs_node_t* mount_node = f_sys->mount(f_sys, parent_of_tgt_node, basename, source_node);
    free(basename);

    target_node->mountpoint = mount_node;

    dir_create_dotentries(mount_node, parent_of_tgt_node);

    lock_release(&vfs_lock);

    if(strlen(source) > 0)
    {
        klog("vfs", "Mounted %s to %s with filesystem %s", source, target, fs_name(fs_identifier));
    }
    else
    {
        klog("vfs", "Mounted %s to %s", fs_name(fs_identifier), target);
    }
    return true;
}

vfs_node_t* internal_create(vfs_node_t* parent, const char* name, int mode)
{
    vfs_node_t* parent_of_tgt_node;
    vfs_node_t* target_node;
    char* basename;
    path2node(parent, name, &parent_of_tgt_node, &target_node, &basename);

    klog_debug("fs", "fs_create parent->name=%s name=%s mode=%o", parent->name, name, mode);

    if(target_node != NULL)
    {
        set_errno(EEXIST);
        free(basename);
        return NULL;
    }

    if(parent_of_tgt_node == NULL)
    {
        set_errno(ENOENT);
        free(basename);
        return NULL;
    }

    target_node = parent_of_tgt_node->filesystem->create_node(parent_of_tgt_node->filesystem, parent_of_tgt_node, basename, mode);
    vfs_add_child(parent_of_tgt_node, target_node);
    if(stat_is_dir(target_node->resource->stat.mode))
    {
        dir_create_dotentries(target_node, parent_of_tgt_node);
    }

    free(basename);
    return target_node;
}

vfs_node_t* fs_create(vfs_node_t* parent, const char* name, int mode)
{
    lock_acquire(&vfs_lock);
    vfs_node_t* ret = internal_create(parent, name, mode);
    lock_release(&vfs_lock);
    return ret;
}

// INTERNAL: Caller must hold vfs_lock
// Create . and .. aliases to "current node" and "parent node", respectively
void dir_create_dotentries(vfs_node_t* node, vfs_node_t* parent)
{
    vfs_node_t* dot = vfs_create_node(node->filesystem, node, ".", false);
    vfs_node_t* dotdot = vfs_create_node(node->filesystem, node, "..", false);
    dot->redir = node;
    dotdot->redir = parent;
    vfs_add_child(node, dot);
    vfs_add_child(node, dotdot);
}

vfs_node_t* fs_get_node(vfs_node_t* parent, const char* path, bool follow_symlinks)
{
    lock_acquire(&vfs_lock);

    klog_debug("fs", "calling path2node with args: parent=%x path=%s, parent->name=%s", parent, path, parent->name);
    vfs_node_t* current, *next_parent;
    char* basename;
    path2node(parent, path, &next_parent, &current, &basename);
    klog_debug("fs", "path2node returned current=%x parent=%x basename=%s", current, parent, basename);
    free(basename); // not used

    if(current == NULL)
    {
        lock_release(&vfs_lock);
        return NULL;
    }

    vfs_node_t* result = current;
    if (follow_symlinks)
    {
        result = reduce_node(current, true);
    }

    lock_release(&vfs_lock);
    return result;
}
