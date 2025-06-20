/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "analyze.h"
#include "analyze-digests.h"
#include "format-table.h"
#include "log.h"
#include "openssl-util.h"
#include "strv.h"

int verb_digests(int argc, char *argv[], void *userdata) {
#if HAVE_OPENSSL

        _cleanup_(table_unrefp) Table *table = NULL;
        _cleanup_(md_vector_freep) struct md_vector *mds = NULL;
        int r;

        table = table_new("name", "aliases", "asn.1", "mdname");
        if (!table)
                return log_oom();

        (void) table_set_align_percent(table, table_get_cell(table, 0, 2), 100);

        r = openssl_supported_digests(&mds);
        if (r < 0)
                return log_error_errno(r, "Failed to get OpenSSL supported digests: %m");

        for (size_t i=0; i<mds->size; i++) {
                _cleanup_strv_free_ char **aliases = NULL;
                _cleanup_free_ char *name = NULL, *asn1 = NULL;

                r = openssl_digest_names(mds->mds[i], /* ret_names= */ NULL, &name, &aliases, &asn1);
                if (r < 0)
                        return log_error_errno(r, "Failed to get OpenSSL digest names: %m");

                _cleanup_free_ char *aliasstr = strv_join(aliases, ",");
                if (!aliasstr)
                        return log_oom();

                r = table_add_many(table,
                                   TABLE_STRING, name,
                                   TABLE_STRING, aliasstr,
                                   TABLE_STRING, asn1,
                                   TABLE_STRING, EVP_MD_name(mds->mds[i]));
                if (r < 0)
                        return table_log_add_error(r);
        }

        (void) table_set_sort(table, (size_t) 0);

        r = table_print_with_pager(table, arg_json_format_flags, arg_pager_flags, arg_legend);
        if (r < 0)
                return log_error_errno(r, "Failed to output table: %m");

        return EXIT_SUCCESS;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Openssl support not available.");
#endif
}
