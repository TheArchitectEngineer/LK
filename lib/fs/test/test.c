/*
 * Copyright (c) 2009-2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lib/fs.h>
#include <lk/err.h>

#include <lib/unittest.h>
#include <stdio.h>
#include <string.h>

// returns true if the input path passed through the path normalization
// routine matches the expected output.
static bool test_normalize(const char *in, const char *out) {
    char path[1024];

    strlcpy(path, in, sizeof(path));
    fs_normalize_path(path);
    return !strcmp(path, out);
}

static bool test_path_normalize(void) {
    BEGIN_TEST;

    EXPECT_TRUE(test_normalize("/", ""), "");
    EXPECT_TRUE(test_normalize("/test", "/test"), "");
    EXPECT_TRUE(test_normalize("/test/", "/test"), "");
    EXPECT_TRUE(test_normalize("test/", "test"), "");
    EXPECT_TRUE(test_normalize("test", "test"), "");
    EXPECT_TRUE(test_normalize("/test//", "/test"), "");
    EXPECT_TRUE(test_normalize("/test/foo", "/test/foo"), "");
    EXPECT_TRUE(test_normalize("/test/foo/", "/test/foo"), "");
    EXPECT_TRUE(test_normalize("/test/foo/bar", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test/foo/bar//", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//foo/bar//", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./foo/bar//", "/test/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./.foo/bar//", "/test/.foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./..foo/bar//", "/test/..foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test//./../foo/bar//", "/foo/bar"), "");
    EXPECT_TRUE(test_normalize("/test/../foo", "/foo"), "");
    EXPECT_TRUE(test_normalize("/test/bar/../foo", "/test/foo"), "");
    EXPECT_TRUE(test_normalize("../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("../foo/", "foo"), "");
    EXPECT_TRUE(test_normalize("/../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("/../foo/", "foo"), "");
    EXPECT_TRUE(test_normalize("/../../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("/bleh/../../foo", "foo"), "");
    EXPECT_TRUE(test_normalize("/bleh/bar/../../foo", "/foo"), "");
    EXPECT_TRUE(test_normalize("/bleh/bar/../../foo/..", ""), "");
    EXPECT_TRUE(test_normalize("/bleh/bar/../../foo/../meh", "/meh"), "");

    // edge cases: empty, standalone dots
    EXPECT_TRUE(test_normalize("", ""), "");
    EXPECT_TRUE(test_normalize(".", ""), "");
    EXPECT_TRUE(test_normalize("..", ""), "");
    EXPECT_TRUE(test_normalize("/.", ""), "");
    EXPECT_TRUE(test_normalize("/..", ""), "");

    // three dots is a regular filename
    EXPECT_TRUE(test_normalize("...", "..."), "");
    EXPECT_TRUE(test_normalize("/a/.../b", "/a/.../b"), "");

    // dot-prefixed filenames preserved
    EXPECT_TRUE(test_normalize(".hidden", ".hidden"), "");
    EXPECT_TRUE(test_normalize("/a/.hidden", "/a/.hidden"), "");
    EXPECT_TRUE(test_normalize("..hidden", "..hidden"), "");

    // dotdot at end of path
    EXPECT_TRUE(test_normalize("a/..", ""), "");
    EXPECT_TRUE(test_normalize("/a/..", ""), "");
    EXPECT_TRUE(test_normalize("/a/b/..", "/a"), "");

    // multiple dotdots chained
    EXPECT_TRUE(test_normalize("a/b/../../c", "c"), "");
    EXPECT_TRUE(test_normalize("/a/b/../../c", "/c"), "");
    EXPECT_TRUE(test_normalize("a/b/c/../../d", "a/d"), "");

    // dotdot past root clamped
    EXPECT_TRUE(test_normalize("a/../../b", "b"), "");
    EXPECT_TRUE(test_normalize("/a/b/../../../c", "c"), "");

    // mixed dots and dotdots
    EXPECT_TRUE(test_normalize("/a/./b/../c", "/a/c"), "");
    EXPECT_TRUE(test_normalize("a/./b/./c", "a/b/c"), "");
    EXPECT_TRUE(test_normalize("/a/b/./../c", "/a/c"), "");

    // consecutive separators in various positions
    EXPECT_TRUE(test_normalize("///", ""), "");
    EXPECT_TRUE(test_normalize("a///b", "a/b"), "");

    END_TEST;
}

#define TEST_MNT  "/test"
#define TEST_FILE TEST_MNT "/stdio_file_tests.txt"

static inline void test_stdio_fs_teardown(void *ptr) {
    fs_remove_file(TEST_FILE);
    fs_unmount(TEST_MNT);
}

static bool test_stdio_fs(void) {
    __attribute__((cleanup(test_stdio_fs_teardown))) BEGIN_TEST;

    // Setup
    const char *content = "Hello World\n";
    const size_t content_len = strlen(content);
    fs_mount(TEST_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE);

    // Tests
    FILE *stream = fopen(TEST_FILE, "w");
    ASSERT_NE(NULL, stream, "failed to open/create file " TEST_MNT);

    char buf[1024];
    // test stdout fprintf, fputs
    EXPECT_EQ(content_len, fprintf(stdout, "%s", content), "");
    EXPECT_EQ(content_len, fputs(content, stdout), "");

    // test fwrite
    EXPECT_EQ(content_len, fwrite(content, 1, content_len, stream), "");

    // test fread
    ASSERT_EQ(NO_ERROR, fseek(stream, 0, SEEK_SET), "fseek failed");
    EXPECT_EQ(content_len, fread(buf, 1, content_len, stream), "");
    EXPECT_BYTES_EQ((const uint8_t *)content, (const uint8_t *)buf, content_len,
                    "fread content mismatched");

    // testing fputs
    ASSERT_EQ(NO_ERROR, fseek(stream, 0, SEEK_SET), "fseek failed");
    EXPECT_EQ(content_len, fputs(content, stream), "");

    // testing fgets
    ASSERT_EQ(NO_ERROR, fseek(stream, 0, SEEK_SET), "fseek failed");
    ASSERT_NE(NULL, fgets(buf, content_len + 1, stream), "");
    EXPECT_BYTES_EQ((const uint8_t *)content, (const uint8_t *)buf, content_len,
                    "fgets content mismatched");

    END_TEST;
}

#define BUSY_MNT  "/fs_busy_test"
#define BUSY_FILE BUSY_MNT "/file"

static void test_remove_while_open_teardown(void *ptr) {
    fs_remove_file(BUSY_FILE);
    fs_unmount(BUSY_MNT);
}

static bool test_remove_while_open(void) {
    __attribute__((cleanup(test_remove_while_open_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(BUSY_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(BUSY_FILE, &h, 16), "create");

    // removing a file with an open handle must be refused, not freed underneath it
    EXPECT_EQ(ERR_BUSY, fs_remove_file(BUSY_FILE), "remove while open");

    // the handle must still be usable afterwards
    char buf[16];
    EXPECT_EQ(16, fs_read_file(h, buf, 0, sizeof(buf)), "read after failed remove");

    // a second open of the same file holds it busy on its own
    filehandle *h2;
    ASSERT_EQ(NO_ERROR, fs_open_file(BUSY_FILE, &h2), "second open");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close first handle");
    EXPECT_EQ(ERR_BUSY, fs_remove_file(BUSY_FILE), "remove with second handle open");
    EXPECT_EQ(NO_ERROR, fs_close_file(h2), "close second handle");

    // with every handle closed the remove goes through
    EXPECT_EQ(NO_ERROR, fs_remove_file(BUSY_FILE), "remove after close");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(BUSY_FILE, &h), "open after remove");

    END_TEST;
}

#define FLAT_MNT "/fs_flat_test"

static void test_flat_fs_semantics_teardown(void *ptr) {
    fs_unmount(FLAT_MNT);
}

// Pin down the contract a flat (no-directory) filesystem presents through the
// fs layer, using memfs as the reference: directory-shaped requests are
// ERR_NOT_SUPPORTED, missing names are ERR_NOT_FOUND, and the mount root is
// the one openable directory.
static bool test_flat_fs_semantics(void) {
    __attribute__((cleanup(test_flat_fs_semantics_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(FLAT_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    // no subdirectory support: mkdir and nested create are not supported
    filehandle *h;
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_make_dir(FLAT_MNT "/sub"), "mkdir on flat fs");
    EXPECT_EQ(ERR_NOT_SUPPORTED, fs_create_file(FLAT_MNT "/sub/file", &h, 0), "nested create");

    // a name that simply does not exist is ERR_NOT_FOUND, not ERR_NOT_SUPPORTED
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(FLAT_MNT "/nope", &h), "open missing file");
    dirhandle *dh;
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(FLAT_MNT "/nope", &dh), "open missing dir");

    // the root of the mount is openable as a directory
    ASSERT_EQ(NO_ERROR, fs_open_dir(FLAT_MNT, &dh), "open mount root");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close mount root");

    END_TEST;
}

#define DIR_MNT "/fs_readdir_test"

static void test_readdir_teardown(void *ptr) {
    fs_remove_file(DIR_MNT "/a");
    fs_remove_file(DIR_MNT "/b");
    fs_remove_file(DIR_MNT "/c");
    fs_unmount(DIR_MNT);
}

static bool test_readdir(void) {
    __attribute__((cleanup(test_readdir_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(DIR_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    const char *names[] = { DIR_MNT "/a", DIR_MNT "/b", DIR_MNT "/c" };
    for (size_t i = 0; i < countof(names); i++) {
        filehandle *h;
        ASSERT_EQ(NO_ERROR, fs_create_file(names[i], &h, 0), "create");
        ASSERT_EQ(NO_ERROR, fs_close_file(h), "close");
    }

    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir(DIR_MNT, &dh), "open dir");
    bool seen[countof(names)] = {};
    size_t entries = 0;
    struct dirent ent;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        entries++;
        if (!strcmp(ent.name, "a")) seen[0] = true;
        if (!strcmp(ent.name, "b")) seen[1] = true;
        if (!strcmp(ent.name, "c")) seen[2] = true;
    }
    EXPECT_EQ(3u, entries, "entry count");
    EXPECT_TRUE(seen[0] && seen[1] && seen[2], "every created file enumerated");

    // end of directory is ERR_NOT_FOUND, and stays that way on a re-read
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "eof");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "eof is stable");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close dir");

    END_TEST;
}

#define UMNT_MNT  "/fs_unmount_test"
#define UMNT_FILE UMNT_MNT "/file"

static void test_unmount_with_open_handle_teardown(void *ptr) {
    fs_remove_file(UMNT_FILE);
    fs_unmount(UMNT_MNT);
}

// Unmounting with a handle still open must never free the filesystem under
// the handle. Today the layer defers the teardown until the last close and
// returns NO_ERROR; a future version may refuse with ERR_BUSY instead. Either
// way the handle stays usable and the mount is gone once both the close and a
// (possibly second) unmount have happened.
static bool test_unmount_with_open_handle(void) {
    __attribute__((cleanup(test_unmount_with_open_handle_teardown))) BEGIN_TEST;

    ASSERT_EQ(NO_ERROR, fs_mount(UMNT_MNT, "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(UMNT_FILE, &h, 16), "create");

    status_t err = fs_unmount(UMNT_MNT);
    EXPECT_TRUE(err == NO_ERROR || err == ERR_BUSY, "unmount with open handle");

    // the handle must still work
    char buf[16];
    EXPECT_EQ(16, fs_read_file(h, buf, 0, sizeof(buf)), "read after unmount attempt");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    // after the last close the mount can be (or already has been) torn down
    err = fs_unmount(UMNT_MNT);
    EXPECT_TRUE(err == NO_ERROR || err == ERR_NOT_FOUND, "final unmount");

    // and the path is really gone
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(UMNT_FILE, &h), "open after unmount");

    END_TEST;
}

static void test_rootfs_teardown(void *ptr) {
    fs_unmount("/tmp");
    fs_unmount("/data");
}

static bool test_rootfs(void) {
    __attribute__((cleanup(test_rootfs_teardown))) BEGIN_TEST;

    // root listing must work even before we add any filesystems
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir("/", &dh), "open root with no mounts");
    struct dirent ent;
    // drain any pre-existing entries (other tests may have left mounts)
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
    }
    fs_close_dir(dh);

    // mount two filesystems; both must appear as directories in root listing
    ASSERT_EQ(NO_ERROR, fs_mount("/tmp", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount /tmp");
    ASSERT_EQ(NO_ERROR, fs_mount("/data", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount /data");

    ASSERT_EQ(NO_ERROR, fs_open_dir("/", &dh), "open root dir with mounts");
    bool found_tmp = false, found_data = false;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        if (strcmp(ent.name, "tmp") == 0) {
            found_tmp = true;
        }
        if (strcmp(ent.name, "data") == 0) {
            found_data = true;
        }
    }
    fs_close_dir(dh);
    EXPECT_TRUE(found_tmp, "tmp in root listing");
    EXPECT_TRUE(found_data, "data in root listing");

    END_TEST;
}

// Verifies that mount/unmount operations occurring during an open dir
// iteration do not crash or corrupt the iterator. Scaffold children are kept
// in creation order and the cursor is a live pointer into that list:
//   - a node removed while the cursor points at it is advanced past
//   - a mount added behind the cursor (at the tail) is seen later in the pass
static void test_rootfs_live_iter_teardown(void *ptr) {
    // best-effort; ignore errors for mounts that may already be gone
    fs_unmount("/live_a");
    fs_unmount("/live_b");
    fs_unmount("/live_c");
    fs_unmount("/live_d");
}

static bool test_rootfs_live_iter(void) {
    __attribute__((cleanup(test_rootfs_live_iter_teardown))) BEGIN_TEST;

    struct dirent ent;

    // Set up three mounts; scaffold children list in creation order a, b, c.
    ASSERT_EQ(NO_ERROR, fs_mount("/live_a", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_a");
    ASSERT_EQ(NO_ERROR, fs_mount("/live_b", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_b");
    ASSERT_EQ(NO_ERROR, fs_mount("/live_c", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_c");

    // Open the iterator; the cursor starts at the first child (live_a).
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir("/", &dh), "open root dir");

    // Read the first live_* entry (other tests may have left mounts behind,
    // which sort before ours in creation order); it must be live_a, after
    // which the cursor points at live_b.
    bool seen_a = false;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        if (strncmp(ent.name, "live_", 5) == 0) {
            seen_a = (strcmp(ent.name, "live_a") == 0);
            break;
        }
    }

    // Remove the mount whose node the cursor points at (live_b); the cursor
    // must be advanced off it before the node is pruned.
    EXPECT_EQ(NO_ERROR, fs_unmount("/live_b"), "unmount live_b mid-iter");

    // Add a new mount; its node lands at the tail, behind the cursor, so this
    // same pass will reach it.
    EXPECT_EQ(NO_ERROR, fs_mount("/live_d", "memfs", NULL, FS_MOUNT_OPTION_NONE), "mount live_d mid-iter");

    // Drain: live_b must not appear, live_c and live_d must.
    bool seen_b = false, seen_c = false, seen_d = false;
    while (fs_read_dir(dh, &ent) == NO_ERROR) {
        if (strcmp(ent.name, "live_b") == 0) {
            seen_b = true;
        }
        if (strcmp(ent.name, "live_c") == 0) {
            seen_c = true;
        }
        if (strcmp(ent.name, "live_d") == 0) {
            seen_d = true;
        }
    }
    fs_close_dir(dh);

    EXPECT_TRUE(seen_a, "live_a seen first");
    EXPECT_FALSE(seen_b, "live_b not seen after being unmounted mid-iter");
    EXPECT_TRUE(seen_c, "live_c seen after live_b removal");
    EXPECT_TRUE(seen_d, "live_d added mid-iter is reached");

    END_TEST;
}

#define NS_BASE "/ns_test"

static void test_mount_namespace_teardown(void *ptr) {
    fs_remove_file(NS_BASE "/deep/mnt/file");
    fs_unmount(NS_BASE "/deep/mnt");
    fs_unmount(NS_BASE "/deep/mnt2");
}

static bool test_mount_namespace(void) {
    __attribute__((cleanup(test_mount_namespace_teardown))) BEGIN_TEST;

    // mounting deep creates the intermediate scaffold directories
    ASSERT_EQ(NO_ERROR, fs_mount(NS_BASE "/deep/mnt", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "deep mount");

    // every intermediate level lists its single child
    dirhandle *dh;
    struct dirent ent;
    ASSERT_EQ(NO_ERROR, fs_open_dir(NS_BASE, &dh), "open scaffold");
    ASSERT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "read scaffold");
    EXPECT_EQ(0, strcmp(ent.name, "deep"), "child is deep");
    EXPECT_EQ(ERR_NOT_FOUND, fs_read_dir(dh, &ent), "one child only");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close scaffold");

    ASSERT_EQ(NO_ERROR, fs_open_dir(NS_BASE "/deep", &dh), "open inner scaffold");
    ASSERT_EQ(NO_ERROR, fs_read_dir(dh, &ent), "read inner scaffold");
    EXPECT_EQ(0, strcmp(ent.name, "mnt"), "child is mnt");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close inner scaffold");

    // a file created through the deep path lands in the filesystem
    filehandle *h;
    ASSERT_EQ(NO_ERROR, fs_create_file(NS_BASE "/deep/mnt/file", &h, 16), "create");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close file");

    // mounting on or below an existing mount is refused
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(NS_BASE "/deep/mnt", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount on mount");
    EXPECT_EQ(ERR_ALREADY_MOUNTED,
              fs_mount(NS_BASE "/deep/mnt/sub", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "mount below mount");

    // a second mount under the same scaffold shares it
    ASSERT_EQ(NO_ERROR, fs_mount(NS_BASE "/deep/mnt2", "memfs", NULL, FS_MOUNT_OPTION_NONE),
              "sibling mount");

    // unmounting by a path inside a mount is refused
    EXPECT_EQ(ERR_NOT_FOUND, fs_unmount(NS_BASE "/deep/mnt/file"), "unmount inner path");

    // unmounting prunes exactly the scaffolding nothing else needs
    ASSERT_EQ(NO_ERROR, fs_remove_file(NS_BASE "/deep/mnt/file"), "remove file");
    EXPECT_EQ(NO_ERROR, fs_unmount(NS_BASE "/deep/mnt"), "unmount first");
    dirhandle *dh2;
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(NS_BASE "/deep/mnt", &dh2), "mnt gone");
    ASSERT_EQ(NO_ERROR, fs_open_dir(NS_BASE "/deep", &dh), "scaffold still present");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "close remaining scaffold");
    EXPECT_EQ(NO_ERROR, fs_unmount(NS_BASE "/deep/mnt2"), "unmount second");
    EXPECT_EQ(ERR_NOT_FOUND, fs_open_dir(NS_BASE, &dh), "scaffold fully pruned");

    END_TEST;
}

BEGIN_TEST_CASE(fs_tests);
RUN_TEST(test_path_normalize);
RUN_TEST(test_stdio_fs);
RUN_TEST(test_remove_while_open);
RUN_TEST(test_flat_fs_semantics);
RUN_TEST(test_readdir);
RUN_TEST(test_unmount_with_open_handle);
RUN_TEST(test_rootfs);
RUN_TEST(test_rootfs_live_iter);
RUN_TEST(test_mount_namespace);
END_TEST_CASE(fs_tests);
