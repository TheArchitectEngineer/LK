/*
 * Copyright (c) 2007 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "ext2_priv.h"
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

/* The walk recurses through symlinks, and the default thread stack is a
 * single page on some architectures, so no block or link sized buffer may
 * live in a walk stack frame. Everything the walk needs is gathered in one
 * scratch structure, allocated once per lookup -- never per component or per
 * symlink. The recursion limit bounds the per-level link target storage. */
#define EXT2_WALK_MAX_RECURSE 4
#define EXT2_WALK_MAX_LINK    512

struct ext2_walk_scratch {
    char path[EXT2_WALK_MAX_LINK];                          // mutable copy of the caller's path
    char link[EXT2_WALK_MAX_RECURSE][EXT2_WALK_MAX_LINK];   // one target buffer per nesting level
    uint8_t dirblock[];                                     // one block, for the dirent scan
};

/* read in the dir, look for the entry */
static int ext2_dir_lookup(ext2_t *ext2, struct ext2_walk_scratch *scratch,
                           struct ext2_inode *dir_inode, const char *name, inodenum_t *inum) {
    uint file_blocknum;
    int err;
    uint8_t *buf = scratch->dirblock;
    size_t namelen = strlen(name);

    if (!S_ISDIR(dir_inode->i_mode)) {
        return ERR_NOT_DIR;
    }

    file_blocknum = 0;
    for (;;) {
        /* read in the offset */
        err = ext2_read_inode(ext2, dir_inode, buf, (off_t)file_blocknum * EXT2_BLOCK_SIZE(ext2->sb), EXT2_BLOCK_SIZE(ext2->sb));
        if (err < 0) {
            return err;
        }
        if (err == 0) {
            /* the directory has been searched completely */
            return ERR_NOT_FOUND;
        }

        /* walk through the directory entries, looking for the one that matches */
        struct ext2_dir_entry_2 *ent;
        uint pos = 0;
        while (pos < EXT2_BLOCK_SIZE(ext2->sb)) {
            ent = (struct ext2_dir_entry_2 *)&buf[pos];

            LTRACEF("ent %d:%d: inode 0x%x, reclen %d, namelen %d\n",
                    file_blocknum, pos, LE32(ent->inode), LE16(ent->rec_len), ent->name_len /* , ent->name*/);

            /* sanity check the record length */
            if (LE16(ent->rec_len) == 0) {
                break;
            }

            if (ent->name_len == namelen && memcmp(name, ent->name, ent->name_len) == 0) {
                // match
                *inum = LE32(ent->inode);
                LTRACEF("match: inode %d\n", *inum);
                return 1;
            }

            pos += ROUNDUP(LE16(ent->rec_len), 4);
        }

        file_blocknum++;

        /* sanity check the directory. 4MB should be enough */
        if (file_blocknum > 1024) {
            return ERR_TOO_BIG;
        }
    }
}

/* note, trashes path */
static int ext2_walk(ext2_t *ext2, struct ext2_walk_scratch *scratch, char *path,
                     struct ext2_inode *start_inode, inodenum_t *inum, int recurse) {
    char *ptr;
    struct ext2_inode inode;
    struct ext2_inode dir_inode;
    int err;
    bool done;

    LTRACEF("path '%s', start_inode %p, inum %p, recurse %d\n", path, start_inode, inum, recurse);

    if (recurse > EXT2_WALK_MAX_RECURSE) {
        return ERR_RECURSE_TOO_DEEP;
    }

    /* chew up leading slashes */
    ptr = &path[0];
    while (*ptr == '/') {
        ptr++;
    }

    done = false;
    memcpy(&dir_inode, start_inode, sizeof(struct ext2_inode));
    while (!done) {
        /* process the first component */
        char *next_sep = strchr(ptr, '/');
        if (next_sep) {
            /* terminate the next component, giving us a substring */
            *next_sep = 0;
        } else {
            /* this is the last component */
            done = true;
        }

        LTRACEF("component '%s', done %d\n", ptr, done);

        /* do the lookup on this component */
        err = ext2_dir_lookup(ext2, scratch, &dir_inode, ptr, inum);
        if (err < 0) {
            return err;
        }

nextcomponent:
        LTRACEF("inum %u\n", *inum);

        /* load the next inode */
        err = ext2_load_inode(ext2, *inum, &inode);
        if (err < 0) {
            return err;
        }

        /* is it a symlink? */
        if (S_ISLNK(inode.i_mode)) {
            LTRACEF("hit symlink\n");

            /* this nesting level's link buffer in the walk scratch */
            char *link = scratch->link[recurse - 1];

            err = ext2_read_link(ext2, &inode, link, EXT2_WALK_MAX_LINK);
            if (err < 0) {
                return err;
            }

            LTRACEF("symlink read returns %d '%s'\n", err, link);

            /* recurse, parsing the link */
            if (link[0] == '/') {
                /* link starts with '/', so start over again at the rootfs */
                err = ext2_walk(ext2, scratch, link, &ext2->root_inode, inum, recurse + 1);
            } else {
                err = ext2_walk(ext2, scratch, link, &dir_inode, inum, recurse + 1);
            }

            LTRACEF("recursive walk returns %d\n", err);

            if (err < 0) {
                return err;
            }

            /* if we weren't done with our path parsing, start again with the result of this recurse */
            if (!done) {
                goto nextcomponent;
            }
        } else if (S_ISDIR(inode.i_mode)) {
            /* for the next cycle, point the dir inode at our new directory */
            memcpy(&dir_inode, &inode, sizeof(struct ext2_inode));
        } else {
            if (!done) {
                /* we aren't done and this walked over a nondir, abort */
                LTRACEF("not finished and component is nondir\n");
                return ERR_NOT_FOUND;
            }
        }

        if (!done) {
            /* move to the next separator */
            ptr = next_sep + 1;

            /* consume multiple separators */
            while (*ptr == '/') {
                ptr++;
            }
        }
    }

    return 0;
}

/* do a path parse, looking up each component */
int ext2_lookup(ext2_t *ext2, const char *_path, inodenum_t *inum) {
    LTRACEF("path '%s', inum %p\n", _path, inum);

    /* one allocation for everything the walk needs; see the comment on
     * struct ext2_walk_scratch */
    struct ext2_walk_scratch *scratch =
        malloc(sizeof(*scratch) + EXT2_BLOCK_SIZE(ext2->sb));
    if (!scratch) {
        return ERR_NO_MEMORY;
    }

    strlcpy(scratch->path, _path, sizeof(scratch->path));

    int err = ext2_walk(ext2, scratch, scratch->path, &ext2->root_inode, inum, 1);

    free(scratch);

    return err;
}
