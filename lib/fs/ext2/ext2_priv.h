/*
 * Copyright (c) 2007-2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include "ext2_fs.h"
#include <lib/bcache.h>
#include <lib/bio.h>
#include <lib/fs.h>

typedef uint32_t blocknum_t;
typedef uint32_t inodenum_t;
typedef uint32_t groupnum_t;

typedef struct {
    bdev_t *dev;
    bcache_t cache;

    struct ext2_super_block sb;
    int s_group_count;
    struct ext2_group_desc *gd;
} ext2_t;

/* One filesystem object, hanging off the layer's vnode as its private data.
 * The inode is kept by value: the layer deduplicates vnodes by inode number,
 * so there is exactly one of these per object in use, and the filesystem is
 * read-only, so the copy can never go stale. */
typedef struct {
    ext2_t *ext2;
    inodenum_t inum;
    struct ext2_inode inode;
} ext2_vnode_t;

/* internal routines */
int ext2_load_inode(ext2_t *ext2, inodenum_t num, struct ext2_inode *inode);
status_t ext2_create_vnode(ext2_t *ext2, inodenum_t inum, struct fs_vnode **out);

/* io */
int ext2_read_block(ext2_t *ext2, void *buf, blocknum_t bnum);
int ext2_get_block(ext2_t *ext2, void **ptr, blocknum_t bnum);
int ext2_put_block(ext2_t *ext2, blocknum_t bnum);
blocknum_t ext2_file_block_to_fs_block(ext2_t *ext2, struct ext2_inode *inode, uint fileblock);

off_t ext2_file_len(ext2_t *ext2, struct ext2_inode *inode);
ssize_t ext2_read_inode(ext2_t *ext2, struct ext2_inode *inode, void *buf, off_t offset, size_t len);

/* fs api */
status_t ext2_mount(bdev_t *dev, enum fs_mount_options options, fscookie **cookie,
                    struct fs_vnode **root);
status_t ext2_unmount(fscookie *cookie);
status_t ext2_lookup(struct fs_vnode *dir, const char *name, struct fs_vnode **out);
void ext2_release(struct fs_vnode *vn);
ssize_t ext2_read(struct fs_vnode *vn, void *buf, off_t offset, size_t len);
ssize_t ext2_readlink(struct fs_vnode *vn, char *buf, size_t len);
status_t ext2_stat(struct fs_vnode *vn, struct file_stat *stat);
status_t ext2_opendir(struct fs_vnode *vn, dircookie **cookie);
status_t ext2_readdir(dircookie *cookie, struct dirent *ent);
status_t ext2_closedir(dircookie *cookie);

/* mode stuff */
#define S_IFMT   0170000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFBLK  0060000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISCHR(mode)  (((mode) & S_IFMT) == S_IFCHR)
#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#define S_ISBLK(mode)  (((mode) & S_IFMT) == S_IFBLK)
#define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#define S_ISLNK(mode)  (((mode) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)
