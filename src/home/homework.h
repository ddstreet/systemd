/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <linux/fs.h>
#include <sys/vfs.h>

#include "sd-id128.h"

#include "loop-util.h"
#include "strv.h"
#include "user-record.h"
#include "user-record-util.h"

typedef struct HomeSetup {
        char *dm_name;
        char *dm_node;

        LoopDevice *loop;
        struct crypt_device *crypt_device;
        int root_fd;
        int image_fd;
        sd_id128_t found_partition_uuid;
        sd_id128_t found_luks_uuid;
        sd_id128_t found_fs_uuid;

        uint8_t fscrypt_key_descriptor[FS_KEY_DESCRIPTOR_SIZE];

        void *volume_key;
        size_t volume_key_size;

        bool undo_dm:1;
        bool undo_mount:1;            /* Whether to unmount /run/systemd/user-home-mount */
        bool do_offline_fitrim:1;
        bool do_offline_fallocate:1;
        bool do_mark_clean:1;
        bool do_drop_caches:1;

        uint64_t partition_offset;
        uint64_t partition_size;

        char *mount_suffix;           /* The directory to use as home dir is this path below /run/systemd/user-home-mount */

        char *temporary_image_path;
} HomeSetup;

typedef struct PasswordCache {
        /* Decoding passwords from security tokens is expensive and typically requires user interaction,
         * hence cache any we already figured out. */
        char **pkcs11_passwords;
        char **fido2_passwords;
} PasswordCache;

void password_cache_free(PasswordCache *cache);

static inline bool password_cache_contains(const PasswordCache *cache, const char *p) {
        if (!cache)
                return false;

        return strv_contains(cache->pkcs11_passwords, p) || strv_contains(cache->fido2_passwords, p);
}

#define HOME_SETUP_INIT                                 \
        {                                               \
                .root_fd = -1,                          \
                .image_fd = -1,                         \
                .partition_offset = UINT64_MAX,         \
                .partition_size = UINT64_MAX,           \
        }

/* Various flags for the operation of setting up a home directory */
typedef enum HomeSetupFlags {
        HOME_SETUP_ALREADY_ACTIVATED = 1 << 0, /* Open an already activated home, rather than activate it afresh */

        /* CIFS backend: */
        HOME_SETUP_CIFS_MKDIR        = 1 << 1, /* Create CIFS subdir when missing */
} HomeSetupFlags;

int home_setup_done(HomeSetup *setup);

int home_setup_undo_mount(HomeSetup *setup, int level);
int home_setup_undo_dm(HomeSetup *setup, int level);

int home_setup(UserRecord *h, HomeSetupFlags flags, HomeSetup *setup, PasswordCache *cache, UserRecord **ret_header_home);

int home_refresh(UserRecord *h, HomeSetup *setup, UserRecord *header_home, PasswordCache *cache, struct statfs *ret_statfs, UserRecord **ret_new_home);

int home_maybe_shift_uid(UserRecord *h, HomeSetup *setup);
int home_populate(UserRecord *h, int dir_fd);

int home_load_embedded_identity(UserRecord *h, int root_fd, UserRecord *header_home, UserReconcileMode mode, PasswordCache *cache, UserRecord **ret_embedded_home, UserRecord **ret_new_home);
int home_store_embedded_identity(UserRecord *h, int root_fd, uid_t uid, UserRecord *old_home);
int home_extend_embedded_identity(UserRecord *h, UserRecord *used, HomeSetup *setup);

int user_record_authenticate(UserRecord *h, UserRecord *secret, PasswordCache *cache, bool strict_verify);

int home_sync_and_statfs(int root_fd, struct statfs *ret);

#define HOME_RUNTIME_WORK_DIR "/run/systemd/user-home-mount"
