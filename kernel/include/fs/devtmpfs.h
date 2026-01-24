#pragma once

#include <stddef.h>
#include <fs/fs.h>

typedef struct {
    filesystem_t filesystem;
} devtmpfs_t;

typedef struct {
    resource_t resource;
    uint8_t* storage;
    size_t capacity;
} devtmpfs_resource_t;


filesystem_t* devtmpfs_create();

filesystem_t* devtmpfs_init(filesystem_t* self);
void devtmpfs_populate(filesystem_t* self, vfs_node_t* node);
vfs_node_t* devtmpfs_mount(filesystem_t* self, vfs_node_t* parent, const char* name, vfs_node_t* source);
vfs_node_t* devtmpfs_create_node(filesystem_t* self, vfs_node_t* parent, const char* name, int mode);
vfs_node_t* devtmpfs_symlink(filesystem_t* self, vfs_node_t* parent, const char* dest, const char* target);
vfs_node_t* devtmpfs_link(filesystem_t* self, vfs_node_t* parent, const char* dest, vfs_node_t* target);
void devtmpfs_close(filesystem_t* self);

void devtmpfs_add_device(resource_t* device, const char* name);
