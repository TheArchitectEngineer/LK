/*
 * Copyright (c) 2009-2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lib/fs.h>

#include <assert.h>
#include <kernel/mutex.h>
#include <lib/bio.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

// The mount namespace is a tree of nodes, one per path component the layer
// knows about. A node either has a filesystem mounted at it or is scaffolding:
// an intermediate directory that exists only because a mount lives somewhere
// beneath it (mounting at /mnt/foo creates "mnt" and "foo"). Path resolution
// walks the tree by component; reaching a mounted node hands the rest of the
// path to that filesystem. Listing a scaffold node enumerates its children,
// which is what makes "ls /" work with no filesystem mounted at "/".
struct fs_node {
    struct list_node node;      // in parent->children
    struct fs_node *parent;     // NULL only for the root
    struct list_node children;  // of struct fs_node
    int ref;                    // children + mounted fs + open dirhandles
    struct fs_mount *mounted;   // filesystem mounted at this node, if any
    char name[];                // single path component, "" for the root
};

struct fs_mount {
    struct list_node node;      // in the mounts list

    char *path;                 // normalized mount point path, for display
    struct fs_node *mountpoint; // node this filesystem is mounted at
    bdev_t *dev;
    fscookie *cookie;
    int ref;
    const struct fs_impl *fs;
    const struct fs_api *api;
};

struct filehandle {
    filecookie *cookie;
    struct fs_mount *mount;
};

struct dirhandle {
    dircookie *cookie;          // fs cookie, or a scaffold_dircookie if mount == NULL
    struct fs_mount *mount;
};

// protects the node tree, all refcounts, the mount list and the open scaffold
// dir cookies
static mutex_t fs_lock = MUTEX_INITIAL_VALUE(fs_lock);
static struct list_node mounts = LIST_INITIAL_VALUE(mounts);

static struct fs_node fs_root = {
    .node = LIST_INITIAL_CLEARED_VALUE,
    .parent = NULL,
    .children = LIST_INITIAL_VALUE(fs_root.children),
    .ref = 1, // never freed
    .mounted = NULL,
};

// an open directory handle on a scaffold node; the cursor is a live pointer
// into the child list, fixed up when the node it points at is pruned
struct scaffold_dircookie {
    struct list_node node;      // in active_scaffold_cookies
    struct fs_node *dir;        // holds a ref
    struct fs_node *next_child; // next child to report, NULL = exhausted
};
static struct list_node active_scaffold_cookies = LIST_INITIAL_VALUE(active_scaffold_cookies);

// defined by the linker, wrapping all structs in the "fs_impl" section
extern const struct fs_impl __start_fs_impl[] __WEAK;
extern const struct fs_impl __stop_fs_impl[] __WEAK;

static const struct fs_impl *find_fs(const char *name) {
    for (const struct fs_impl *fs = __start_fs_impl; fs != __stop_fs_impl; fs++) {
        if (!strcmp(name, fs->name)) {
            return fs;
        }
    }
    return NULL;
}

void fs_dump_list(void) {
    for (const struct fs_impl *fs = __start_fs_impl; fs != __stop_fs_impl; fs++) {
        puts(fs->name);
    }
}

void fs_dump_mounts(void) {
    printf("%-16s%s\n", "Filesystem", "Path");
    mutex_acquire(&fs_lock);
    struct fs_mount *mount;
    list_for_every_entry(&mounts, mount, struct fs_mount, node) {
        printf("%-16s%s\n", mount->fs->name, mount->path);
    }
    mutex_release(&fs_lock);
}

// ---------------------------------------------------------------------------
// node tree primitives, all called with fs_lock held
// ---------------------------------------------------------------------------

static struct fs_node *node_find_child(struct fs_node *parent, const char *name, size_t len) {
    struct fs_node *child;
    list_for_every_entry(&parent->children, child, struct fs_node, node) {
        if (strlen(child->name) == len && memcmp(child->name, name, len) == 0) {
            return child;
        }
    }
    return NULL;
}

// Creates a child with one reference, held by the caller; also accounts the
// child's reference on the parent.
static struct fs_node *node_add_child(struct fs_node *parent, const char *name, size_t len) {
    struct fs_node *child = malloc(sizeof(struct fs_node) + len + 1);
    if (!child) {
        return NULL;
    }

    memcpy(child->name, name, len);
    child->name[len] = 0;
    child->parent = parent;
    list_initialize(&child->children);
    child->ref = 1;
    child->mounted = NULL;

    list_add_tail(&parent->children, &child->node);
    parent->ref++;

    return child;
}

// Called before a child node is unlinked: any open scaffold dir cursor
// pointing at it is advanced to the next sibling.
static void scaffold_child_removed(struct fs_node *child) {
    struct fs_node *next = list_next_type(&child->parent->children, &child->node,
                                          struct fs_node, node);
    struct scaffold_dircookie *dc;
    list_for_every_entry(&active_scaffold_cookies, dc, struct scaffold_dircookie, node) {
        if (dc->next_child == child) {
            dc->next_child = next;
        }
    }
}

// Drop a reference; a node whose count hits zero is pruned, which recursively
// drops the reference it held on its parent.
static void node_release(struct fs_node *node) {
    while (node && --node->ref == 0) {
        struct fs_node *parent = node->parent;

        DEBUG_ASSERT(parent); // the root holds a permanent self reference
        DEBUG_ASSERT(list_is_empty(&node->children));
        DEBUG_ASSERT(!node->mounted);

        scaffold_child_removed(node);
        list_delete(&node->node);
        free(node);

        node = parent;
    }
}

// Resolution outcome for a normalized path: it either enters a mounted
// filesystem (with the unconsumed remainder of the path), lands on a scaffold
// node, or names nothing the layer knows about.
enum resolve_result {
    FS_RESOLVE_NONE,
    FS_RESOLVE_MOUNT,
    FS_RESOLVE_NODE,
};

// Walk a normalized path through the tree. On FS_RESOLVE_MOUNT a reference on
// the mount is returned and *remainder points into the caller's path buffer
// (or at a literal "/" when the mount point was named exactly). On
// FS_RESOLVE_NODE a reference on the node is returned.
static enum resolve_result fs_resolve(const char *path, struct fs_mount **out_mount,
                                      const char **remainder, struct fs_node **out_node) {
    // the normalized form of "/" is the empty string, which names the root
    // node; everything else must be absolute
    if (path[0] != '/' && path[0] != 0) {
        return FS_RESOLVE_NONE;
    }

    mutex_acquire(&fs_lock);

    struct fs_node *cur = &fs_root;
    const char *pos = path; // always at a '/' or the terminator

    for (;;) {
        if (cur->mounted) {
            struct fs_mount *mount = cur->mounted;
            mount->ref++;
            mutex_release(&fs_lock);
            *out_mount = mount;
            *remainder = (pos[0] == 0) ? "/" : pos;
            return FS_RESOLVE_MOUNT;
        }

        if (pos[0] == 0) {
            cur->ref++;
            mutex_release(&fs_lock);
            *out_node = cur;
            return FS_RESOLVE_NODE;
        }

        // normalized paths have no empty components
        const char *comp = pos + 1;
        const char *end = strchr(comp, '/');
        size_t len = end ? (size_t)(end - comp) : strlen(comp);

        struct fs_node *child = node_find_child(cur, comp, len);
        if (!child) {
            mutex_release(&fs_lock);
            return FS_RESOLVE_NONE;
        }

        cur = child;
        pos = comp + len;
    }
}

static void node_put(struct fs_node *node) {
    mutex_acquire(&fs_lock);
    node_release(node);
    mutex_release(&fs_lock);
}

// find the mount a path leads into and the remainder of the path for the
// filesystem to resolve; takes a reference on the mount
static struct fs_mount *find_mount(const char *path, const char **trimmed_path) {
    struct fs_mount *mount;
    struct fs_node *node;
    const char *remainder;

    switch (fs_resolve(path, &mount, &remainder, &node)) {
        case FS_RESOLVE_MOUNT:
            if (trimmed_path) {
                *trimmed_path = remainder;
            }
            return mount;
        case FS_RESOLVE_NODE:
            node_put(node);
            return NULL;
        default:
            return NULL;
    }
}

// decrement the ref to the mount structure, which may
// cause an unmount operation
static void put_mount(struct fs_mount *mount) {
    mutex_acquire(&fs_lock);
    if ((--mount->ref) == 0) {
        LTRACEF("last ref, unmounting fs at '%s'\n", mount->path);

        struct fs_node *mountpoint = mount->mountpoint;
        mountpoint->mounted = NULL;

        list_delete(&mount->node);
        mount->api->unmount(mount->cookie);
        free(mount->path);
        if (mount->dev) {
            bio_close(mount->dev);
        }
        free(mount);

        // prunes the scaffolding up to the nearest still-referenced node
        node_release(mountpoint);
    }
    mutex_release(&fs_lock);
}

static status_t mount(const char *path, const char *device, const struct fs_impl *fs, enum fs_mount_options options) {
    struct fs_mount *mount;
    const struct fs_api *api = fs->api;
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    if (temppath[0] != '/') {
        return ERR_BAD_PATH;
    }

    /* see if there's already something at (or above) this path, abort if there is */
    mount = find_mount(temppath, NULL);
    if (mount) {
        put_mount(mount);
        return ERR_ALREADY_MOUNTED;
    }

    /* open a bio device if the string is nonnull */
    bdev_t *dev = NULL;
    if (device && device[0] != '\0') {
        dev = bio_open(device);
        if (!dev) {
            return ERR_NOT_FOUND;
        }
    }

    /* call into the fs implementation */
    fscookie *cookie;
    status_t err = api->mount(dev, &cookie, options);
    if (err < 0) {
        if (dev) {
            bio_close(dev);
        }
        return err;
    }

    /* create the mount structure */
    mount = malloc(sizeof(struct fs_mount));
    if (!mount) {
        err = ERR_NO_MEMORY;
        goto err_unmount;
    }
    mount->path = strdup(temppath);
    if (!mount->path) {
        free(mount);
        err = ERR_NO_MEMORY;
        goto err_unmount;
    }
    mount->dev = dev;
    mount->cookie = cookie;
    mount->ref = 1;
    mount->fs = fs;
    mount->api = api;

    /* walk to the mount point, creating scaffold nodes for any missing
     * components, and attach the mount there */
    mutex_acquire(&fs_lock);

    struct fs_node *cur = &fs_root;
    cur->ref++;
    const char *pos = temppath;
    while (pos[0] != 0) {
        if (cur->mounted) {
            // raced with another mount that now covers this path
            node_release(cur);
            mutex_release(&fs_lock);
            free(mount->path);
            free(mount);
            err = ERR_ALREADY_MOUNTED;
            goto err_unmount;
        }

        const char *comp = pos + 1;
        const char *end = strchr(comp, '/');
        size_t len = end ? (size_t)(end - comp) : strlen(comp);

        struct fs_node *child = node_find_child(cur, comp, len);
        if (child) {
            child->ref++;
        } else {
            child = node_add_child(cur, comp, len);
        }
        node_release(cur);
        if (!child) {
            mutex_release(&fs_lock);
            free(mount->path);
            free(mount);
            err = ERR_NO_MEMORY;
            goto err_unmount;
        }

        cur = child;
        pos = comp + len;
    }

    if (cur->mounted) {
        node_release(cur);
        mutex_release(&fs_lock);
        free(mount->path);
        free(mount);
        err = ERR_ALREADY_MOUNTED;
        goto err_unmount;
    }

    /* the walk's reference on the mount point becomes the mount's */
    mount->mountpoint = cur;
    cur->mounted = mount;

    list_add_head(&mounts, &mount->node);

    mutex_release(&fs_lock);

    return 0;

err_unmount:
    api->unmount(cookie);
    if (dev) {
        bio_close(dev);
    }
    return err;
}

status_t fs_format_device(const char *fsname, const char *device, const void *args) {
    const struct fs_impl *fs = find_fs(fsname);
    if (!fs) {
        return ERR_NOT_FOUND;
    }

    if (fs->api->format == NULL) {
        return ERR_NOT_SUPPORTED;
    }

    bdev_t *dev = NULL;
    if (device && device[0] != '\0') {
        dev = bio_open(device);
        if (!dev) {
            return ERR_NOT_FOUND;
        }
    }

    return fs->api->format(dev, args);
}

status_t fs_mount(const char *path, const char *fsname, const char *device, enum fs_mount_options options) {
    const struct fs_impl *fs = find_fs(fsname);
    if (!fs) {
        return ERR_NOT_FOUND;
    }

    return mount(path, device, fs, options);
}

status_t fs_unmount(const char *path) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    const char *remainder;
    struct fs_mount *mount = find_mount(temppath, &remainder);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    // only the mount point itself may be unmounted, not a path inside it
    if (remainder[0] != '/' || remainder[1] != 0) {
        put_mount(mount);
        return ERR_NOT_FOUND;
    }

    // return the ref that find_mount added and one extra
    put_mount(mount);
    put_mount(mount);

    return 0;
}

status_t fs_open_file(const char *path, filehandle **handle) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    LTRACEF("path %s temppath %s\n", path, temppath);

    const char *newpath;
    struct fs_mount *mount = find_mount(temppath, &newpath);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    LTRACEF("path %s temppath %s newpath %s\n", path, temppath, newpath);

    filecookie *cookie;
    status_t err = mount->api->open(mount->cookie, newpath, &cookie);
    if (err < 0) {
        put_mount(mount);
        return err;
    }

    filehandle *f = malloc(sizeof(*f));
    if (!f) {
        mount->api->close(cookie);
        put_mount(mount);
        return ERR_NO_MEMORY;
    }
    f->cookie = cookie;
    f->mount = mount;
    *handle = f;

    return 0;
}

status_t fs_file_ioctl(filehandle *handle, int request, void *argp) {
    LTRACEF("filehandle %p, request %d, argp, %p\n", handle, request, argp);

    if (unlikely(!handle->mount ||
                 !handle->mount->api || !handle->mount->api->file_ioctl)) {
        return ERR_INVALID_ARGS;
    }

    return handle->mount->api->file_ioctl(handle->cookie, request, argp);
}

status_t fs_create_file(const char *path, filehandle **handle, uint64_t len) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    const char *newpath;
    struct fs_mount *mount = find_mount(temppath, &newpath);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    if (!mount->api->create) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    filecookie *cookie;
    status_t err = mount->api->create(mount->cookie, newpath, &cookie, len);
    if (err < 0) {
        put_mount(mount);
        return err;
    }

    filehandle *f = malloc(sizeof(*f));
    if (!f) {
        mount->api->close(cookie);
        put_mount(mount);
        return ERR_NO_MEMORY;
    }
    f->cookie = cookie;
    f->mount = mount;
    *handle = f;

    return 0;
}

status_t fs_truncate_file(filehandle *handle, uint64_t len) {
    LTRACEF("filehandle %p, length %llu\n", handle, len);

    if (!handle->mount->api->truncate) {
        return ERR_NOT_SUPPORTED;
    }

    return handle->mount->api->truncate(handle->cookie, len);
}

status_t fs_remove_file(const char *path) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    const char *newpath;
    struct fs_mount *mount = find_mount(temppath, &newpath);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    if (!mount->api->remove) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    status_t err = mount->api->remove(mount->cookie, newpath);

    put_mount(mount);

    return err;
}

status_t fs_remove_dir(const char *path) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    const char *newpath;
    struct fs_mount *mount = find_mount(temppath, &newpath);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    if (!mount->api->rmdir) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    status_t err = mount->api->rmdir(mount->cookie, newpath);

    put_mount(mount);

    return err;
}

ssize_t fs_read_file(filehandle *handle, void *buf, off_t offset, size_t len) {
    return handle->mount->api->read(handle->cookie, buf, offset, len);
}

ssize_t fs_write_file(filehandle *handle, const void *buf, off_t offset, size_t len) {
    if (!handle->mount->api->write) {
        return ERR_NOT_SUPPORTED;
    }

    return handle->mount->api->write(handle->cookie, buf, offset, len);
}

status_t fs_close_file(filehandle *handle) {
    status_t err = handle->mount->api->close(handle->cookie);
    if (err < 0) {
        return err;
    }

    put_mount(handle->mount);
    free(handle);
    return 0;
}

status_t fs_stat_file(filehandle *handle, struct file_stat *stat) {
    // zero the struct so fields a filesystem does not fill in (e.g. capacity)
    // read as zero instead of stack garbage
    memset(stat, 0, sizeof(*stat));
    return handle->mount->api->stat(handle->cookie, stat);
}

status_t fs_make_dir(const char *path) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    const char *newpath;
    struct fs_mount *mount = find_mount(temppath, &newpath);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    if (!mount->api->mkdir) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    status_t err = mount->api->mkdir(mount->cookie, newpath);

    put_mount(mount);

    return err;
}

// ---------------------------------------------------------------------------
// scaffold directory handles: listing a node of the mount namespace itself
// ---------------------------------------------------------------------------

// takes over the caller's reference on the node
static status_t scaffold_opendir(struct fs_node *node, struct scaffold_dircookie **out) {
    struct scaffold_dircookie *dc = malloc(sizeof(*dc));
    if (!dc) {
        return ERR_NO_MEMORY;
    }

    dc->dir = node;

    mutex_acquire(&fs_lock);
    dc->next_child = list_peek_head_type(&node->children, struct fs_node, node);
    list_add_tail(&active_scaffold_cookies, &dc->node);
    mutex_release(&fs_lock);

    *out = dc;
    return NO_ERROR;
}

static status_t scaffold_readdir(struct scaffold_dircookie *dc, struct dirent *ent) {
    mutex_acquire(&fs_lock);

    struct fs_node *child = dc->next_child;
    if (!child) {
        mutex_release(&fs_lock);
        return ERR_NOT_FOUND;
    }

    strlcpy(ent->name, child->name, sizeof(ent->name));
    dc->next_child = list_next_type(&dc->dir->children, &child->node, struct fs_node, node);

    mutex_release(&fs_lock);
    return NO_ERROR;
}

static void scaffold_closedir(struct scaffold_dircookie *dc) {
    mutex_acquire(&fs_lock);
    list_delete(&dc->node);
    node_release(dc->dir);
    mutex_release(&fs_lock);
    free(dc);
}

status_t fs_open_dir(const char *path, dirhandle **handle) {
    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, path, sizeof(temppath));
    fs_normalize_path(temppath);

    LTRACEF("path %s temppath %s\n", path, temppath);

    struct fs_mount *mount;
    struct fs_node *node;
    const char *newpath;

    switch (fs_resolve(temppath, &mount, &newpath, &node)) {
        case FS_RESOLVE_MOUNT:
            break;
        case FS_RESOLVE_NODE: {
            // a directory of the mount namespace itself
            struct scaffold_dircookie *dc;
            status_t err = scaffold_opendir(node, &dc);
            if (err < 0) {
                node_put(node);
                return err;
            }

            dirhandle *d = malloc(sizeof(*d));
            if (!d) {
                scaffold_closedir(dc);
                return ERR_NO_MEMORY;
            }
            d->cookie = (dircookie *)dc;
            d->mount = NULL; // sentinel: scaffold handle
            *handle = d;
            return NO_ERROR;
        }
        default:
            return ERR_NOT_FOUND;
    }

    LTRACEF("path %s temppath %s newpath %s\n", path, temppath, newpath);

    if (!mount->api->opendir) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    dircookie *cookie;
    status_t err = mount->api->opendir(mount->cookie, newpath, &cookie);
    if (err < 0) {
        put_mount(mount);
        return err;
    }

    dirhandle *d = malloc(sizeof(*d));
    if (!d) {
        mount->api->closedir(cookie);
        put_mount(mount);
        return ERR_NO_MEMORY;
    }
    d->cookie = cookie;
    d->mount = mount;
    *handle = d;

    return 0;
}

status_t fs_read_dir(dirhandle *handle, struct dirent *ent) {
    if (!handle->mount) {
        return scaffold_readdir((struct scaffold_dircookie *)handle->cookie, ent);
    }

    if (!handle->mount->api->readdir) {
        return ERR_NOT_SUPPORTED;
    }

    return handle->mount->api->readdir(handle->cookie, ent);
}

status_t fs_close_dir(dirhandle *handle) {
    if (!handle->mount) {
        scaffold_closedir((struct scaffold_dircookie *)handle->cookie);
        free(handle);
        return NO_ERROR;
    }

    if (!handle->mount->api->closedir) {
        return ERR_NOT_SUPPORTED;
    }

    status_t err = handle->mount->api->closedir(handle->cookie);
    if (err < 0) {
        return err;
    }

    put_mount(handle->mount);
    free(handle);
    return 0;
}

status_t fs_stat_fs(const char *mountpoint, struct fs_stat *stat) {
    LTRACEF("mountpoint %s stat %p\n", mountpoint, stat);

    char temppath[FS_MAX_PATH_LEN];

    strlcpy(temppath, mountpoint, sizeof(temppath));
    fs_normalize_path(temppath);

    struct fs_mount *mount = find_mount(temppath, NULL);
    if (!mount) {
        return ERR_NOT_FOUND;
    }

    if (!mount->api->fs_stat) {
        put_mount(mount);
        return ERR_NOT_SUPPORTED;
    }

    memset(stat, 0, sizeof(*stat));
    status_t result = mount->api->fs_stat(mount->cookie, stat);

    put_mount(mount);

    return result;
}

const char *trim_name(const char *_name) {
    const char *name = &_name[0];
    // chew up leading spaces
    while (*name == ' ') {
        name++;
    }

    // chew up leading slashes
    while (*name == '/') {
        name++;
    }

    return name;
}

void fs_normalize_path(char *path) {
    int outpos;
    int pos;
    char c;
    bool done;
    enum {
        INITIAL,
        FIELD_START,
        IN_FIELD,
        SEP,
        SEEN_SEP,
        DOT,
        SEEN_DOT,
        DOTDOT,
        SEEN_DOTDOT,
    } state;

    state = INITIAL;
    pos = 0;
    outpos = 0;
    done = false;

    /* remove duplicate path separators, flatten empty fields (only composed of .), backtrack fields with .., remove trailing slashes */
    while (!done) {
        c = path[pos];
        switch (state) {
            case INITIAL:
                if (c == '/') {
                    state = SEP;
                } else if (c == '.') {
                    state = DOT;
                } else {
                    state = FIELD_START;
                }
                break;
            case FIELD_START:
                if (c == '.') {
                    state = DOT;
                } else if (c == 0) {
                    done = true;
                } else {
                    state = IN_FIELD;
                }
                break;
            case IN_FIELD:
                if (c == '/') {
                    state = SEP;
                } else if (c == 0) {
                    done = true;
                } else {
                    path[outpos++] = c;
                    pos++;
                }
                break;
            case SEP:
                pos++;
                path[outpos++] = '/';
                state = SEEN_SEP;
                break;
            case SEEN_SEP:
                if (c == '/') {
                    // eat it
                    pos++;
                } else if (c == 0) {
                    done = true;
                } else {
                    state = FIELD_START;
                }
                break;
            case DOT:
                pos++; // consume the dot
                state = SEEN_DOT;
                break;
            case SEEN_DOT:
                if (c == '.') {
                    // dotdot now
                    state = DOTDOT;
                } else if (c == '/') {
                    // a field composed entirely of a .
                    // consume the / and move directly to the SEEN_SEP state
                    pos++;
                    state = SEEN_SEP;
                } else if (c == 0) {
                    done = true;
                } else {
                    // a field prefixed with a .
                    // emit a . and move directly into the IN_FIELD state
                    path[outpos++] = '.';
                    state = IN_FIELD;
                }
                break;
            case DOTDOT:
                pos++; // consume the dot
                state = SEEN_DOTDOT;
                break;
            case SEEN_DOTDOT:
                if (c == '/' || c == 0) {
                    // a field composed entirely of '..'
                    // search back and consume a field we've already emitted
                    if (outpos > 0) {
                        // we have already consumed at least one field
                        outpos--;

                        // walk backwards until we find the next field boundary
                        while (outpos > 0) {
                            if (path[outpos - 1] == '/') {
                                break;
                            }
                            outpos--;
                        }
                    }
                    if (c == 0) {
                        done = true;
                    } else {
                        pos++;
                        state = SEEN_SEP;
                    }
                } else {
                    // a field prefixed with ..
                    // emit the .. and move directly to the IN_FIELD state
                    path[outpos++] = '.';
                    path[outpos++] = '.';
                    state = IN_FIELD;
                }
                break;
        }
    }

    /* don't end with trailing slashes */
    if (outpos > 0 && path[outpos - 1] == '/') {
        outpos--;
    }

    path[outpos++] = 0;
}
