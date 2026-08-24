/*
 * Copyright (c) 2024 Cody Wong
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lib/bio.h>
#include <lib/cmdline.h>
#include <lib/fs.h>
#include <lib/unittest.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Unit tests for lib/fs/9p, run against a live virtio-9p host share (e.g.
// scripts/do-qemuarm -f <dir>). The suite skips when no 9p device is present,
// so it is safe in every ut all run; set test.v9fs.required=1 on the kernel
// command line to turn a missing device into a hard failure.
//
// The driver has no remove/rmdir yet, so the suite cannot clean up after
// itself: it leaves lk_v9fs_test_file and lk_v9fs_test_dir/ in the share, and
// is written to pass again when they already exist.

#define V9FS_MOUNT_POINT "/v9p"
#define V9FS_NAME        "9p"
#define V9P_BDEV_NAME    "v9p0"

#define TEST_FILE V9FS_MOUNT_POINT "/lk_v9fs_test_file"
#define TEST_DIR  V9FS_MOUNT_POINT "/lk_v9fs_test_dir"

// Distinctive marker so a host-side harness can grep a boot log and tell a
// real run from a skipped one.
#define V9FS_SKIP_MARKER "V9FS-TEST-SKIPPED(no 9p device)"

// Returns true if the 9p block device exists. Result is cached after the
// first call.
static bool have_device(void) {
    static bool checked = false;
    static bool present = false;

    if (!checked) {
        checked = true;
        bdev_t *bio = bio_open(V9P_BDEV_NAME);
        if (bio) {
            bio_close(bio);
            present = true;
        }
    }
    return present;
}

static bool device_required(void) {
    static bool checked = false;
    static bool required = false;

    if (!checked) {
        checked = true;
        bool val = false;
        if (cmdline_get_bool("test.v9fs.required", &val) == NO_ERROR) {
            required = val;
        }
    }
    return required;
}

#define SKIP_TEST_IF_NO_DEVICE()                                                  \
    do {                                                                          \
        if (!have_device()) {                                                     \
            if (device_required()) {                                              \
                UNITTEST_FAIL_TRACEF("test.v9fs.required is set but no 9p "       \
                                     "device is present\n");                      \
                return false;                                                     \
            }                                                                     \
            unittest_printf(" " V9FS_SKIP_MARKER " ");                            \
            return true;                                                          \
        }                                                                         \
    } while (0)

static bool mount_v9fs(void) {
    return fs_mount(V9FS_MOUNT_POINT, V9FS_NAME, V9P_BDEV_NAME,
                    FS_MOUNT_OPTION_NONE) == NO_ERROR;
}

// The share's content is not under our control, so file data is verified with
// a position dependent pattern we write ourselves.
static uint8_t pattern_byte(uint32_t seed, uint64_t offset) {
    uint32_t x = seed ^ (uint32_t)(offset * 2654435761u);
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (uint8_t)x;
}

// A previous run may have left the file behind, and the driver has no remove,
// so treat create and open-existing the same.
static status_t create_or_open(const char *path, filehandle **h) {
    status_t err = fs_create_file(path, h, 0);
    if (err < 0) {
        err = fs_open_file(path, h);
    }
    return err;
}

static bool test_v9fs_mount(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    // a second cycle proves unmount really released the attach fid
    ASSERT_TRUE(mount_v9fs(), "second mount");
    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "second unmount");

    END_TEST;
}

static bool test_v9fs_file_io(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");

    // creating at the top of the mount exercises the root-level create path
    // (which used to send "/name", slash included, to the server)
    filehandle *h;
    ASSERT_EQ(NO_ERROR, create_or_open(TEST_FILE, &h), "create");

    enum { kLen = 4096 };
    uint8_t *buf = malloc(kLen);
    ASSERT_NONNULL(buf, "buffer");

    for (size_t i = 0; i < kLen; i++) {
        buf[i] = pattern_byte(0x9999, i);
    }
    EXPECT_EQ((ssize_t)kLen, fs_write_file(h, buf, 0, kLen), "write");

    // stat goes to the server; a failure here used to leave the file mutex held
    struct file_stat st;
    EXPECT_EQ(NO_ERROR, fs_stat_file(h, &st), "stat");
    EXPECT_LE((uint64_t)kLen, st.size, "stat size");
    EXPECT_FALSE(st.is_dir, "not a dir");

    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close");

    // read it back through a fresh handle so nothing is served from the
    // handle's page buffer
    ASSERT_EQ(NO_ERROR, fs_open_file(TEST_FILE, &h), "reopen");
    memset(buf, 0, kLen);
    EXPECT_EQ((ssize_t)kLen, fs_read_file(h, buf, 0, kLen), "read");
    bool match = true;
    for (size_t i = 0; i < kLen; i++) {
        if (buf[i] != pattern_byte(0x9999, i)) {
            unittest_printf("\n        content mismatch at offset %zu ", i);
            match = false;
            break;
        }
    }
    EXPECT_TRUE(match, "content");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "close reopened");

    free(buf);

    EXPECT_EQ(ERR_NOT_FOUND, fs_open_file(V9FS_MOUNT_POINT "/lk_v9fs_no_such_file", &h),
              "missing file");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

static bool test_v9fs_dirs(void) {
    BEGIN_TEST;
    SKIP_TEST_IF_NO_DEVICE();

    ASSERT_TRUE(mount_v9fs(), "mount");

    // root-level mkdir exercises the same leading-slash path as create.
    // the server refuses a mkdir of an existing directory, which the driver
    // currently surfaces as ERR_NOT_ALLOWED; what matters is the directory
    // exists and works afterwards.
    status_t err = fs_make_dir(TEST_DIR);
    EXPECT_TRUE(err == NO_ERROR || err == ERR_NOT_ALLOWED, "mkdir");

    filehandle *h;
    ASSERT_EQ(NO_ERROR, create_or_open(TEST_DIR "/nested", &h), "nested create");
    EXPECT_EQ(NO_ERROR, fs_close_file(h), "nested close");

    // enumerate the directory and expect to find the file
    dirhandle *dh;
    ASSERT_EQ(NO_ERROR, fs_open_dir(TEST_DIR, &dh), "opendir");
    bool seen = false;
    struct dirent ent;
    status_t rerr;
    while ((rerr = fs_read_dir(dh, &ent)) == NO_ERROR) {
        if (!strcmp(ent.name, "nested")) {
            seen = true;
        }
    }
    // 9p reports end-of-directory as ERR_OUT_OF_RANGE today; the fs rework
    // will unify EOF on ERR_NOT_FOUND, so accept both
    EXPECT_TRUE(rerr == ERR_OUT_OF_RANGE || rerr == ERR_NOT_FOUND, "readdir eof");
    EXPECT_TRUE(seen, "nested file enumerated");
    EXPECT_EQ(NO_ERROR, fs_close_dir(dh), "closedir");

    // a file is not openable as a directory
    EXPECT_GT(0, fs_open_dir(TEST_DIR "/nested", &dh), "file as dir");

    EXPECT_EQ(NO_ERROR, fs_unmount(V9FS_MOUNT_POINT), "unmount");

    END_TEST;
}

BEGIN_TEST_CASE(v9fs_tests)
RUN_TEST(test_v9fs_mount)
RUN_TEST(test_v9fs_file_io)
RUN_TEST(test_v9fs_dirs)
END_TEST_CASE(v9fs_tests)
