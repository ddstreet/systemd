/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-hwdb.h"

#include "alloc-util.h"
#include "errno-util.h"
#include "errno.h"
#include "hwdb-internal.h"
#include "nulstr-util.h"
#include "tests.h"

static int test_failed_enumerate(void) {
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        const char *key, *value;
        int r;

        log_info("/* %s */", __func__);

        r = sd_hwdb_new(&hwdb);
        if (r == -ENOENT || ERRNO_IS_PRIVILEGE(r))
                return r;
        assert_se(r == 0);

        assert_se(sd_hwdb_seek(hwdb, "no-such-modalias-should-exist") == 0);

        assert_se(sd_hwdb_enumerate(hwdb, &key, &value) == 0);
        assert_se(sd_hwdb_enumerate(hwdb, &key, NULL) == -EINVAL);
        assert_se(sd_hwdb_enumerate(hwdb, NULL, &value) == -EINVAL);

        return 0;
}

#define DELL_MODALIAS \
        "evdev:atkbd:dmi:bvnXXX:bvrYYY:bdZZZ:svnDellXXX:pnYYY"

static void test_basic_enumerate(void) {
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        const char *key, *value;
        size_t len1 = 0, len2 = 0;
        int r;

        log_info("/* %s */", __func__);

        assert_se(sd_hwdb_new(&hwdb) == 0);

        assert_se(sd_hwdb_seek(hwdb, DELL_MODALIAS) == 0);

        for (;;) {
                r = sd_hwdb_enumerate(hwdb, &key, &value);
                assert(IN_SET(r, 0, 1));
                if (r == 0)
                        break;
                assert(key);
                assert(value);
                log_debug("A: \"%s\" → \"%s\"", key, value);
                len1 += strlen(key) + strlen(value);
        }

        SD_HWDB_FOREACH_PROPERTY(hwdb, DELL_MODALIAS, key, value) {
                log_debug("B: \"%s\" → \"%s\"", key, value);
                len2 += strlen(key) + strlen(value);
        }

        assert_se(len1 == len2);
}

static void test_sd_hwdb_new_from_path(void) {
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        const char *hwdb_bin_path = NULL;
        int r;

        log_info("/* %s */", __func__);

        assert_se(sd_hwdb_new_from_path(NULL, &hwdb) == -EINVAL);
        assert_se(sd_hwdb_new_from_path("", &hwdb) == -EINVAL);
        assert_se(sd_hwdb_new_from_path("/path/that/should/not/exist", &hwdb) < 0);

        NULSTR_FOREACH(hwdb_bin_path, hwdb_bin_paths) {
                r = sd_hwdb_new_from_path(hwdb_bin_path, &hwdb);
                if (r >= 0)
                        break;
        }

        assert_se(r >= 0);
}

int main(int argc, char *argv[]) {
        int r;

        test_setup_logging(LOG_DEBUG);

        r = test_failed_enumerate();
        if (r < 0)
                return log_tests_skipped_errno(r, "cannot open hwdb");

        test_basic_enumerate();
        test_sd_hwdb_new_from_path();

        return 0;
}
