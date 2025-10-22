/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  QEMU TCG monitor
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "qapi/qapi-commands-machine.h"
#include "monitor/monitor.h"
#include "system/tcg.h"
#include "tcg/tcg.h"
#include "internal-common.h"
#include "exec/log.h"
#include "exec/target_page.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/cutils.h"
#include <errno.h>

#define MMU_LOG_FILE_PATH "/Users/jueun/BoB/vm/mmu.log"

typedef struct MMULogFilterRequest {
    bool specified;
    uint64_t start;
    uint64_t end;
} MMULogFilterRequest;

static bool mmu_parse_virtual_range(const MMULogVirtualRange *virtual_range,
                                    const MMULogVirtualPageRange *virtual_page_range,
                                    MMULogFilterRequest *req,
                                    Error **errp)
{
    ERRP_GUARD();

    if (virtual_range && virtual_page_range) {
        error_setg(errp,
                   "virtual-range and virtual-page-range cannot be used together");
        return false;
    }

    if (!virtual_range && !virtual_page_range) {
        req->specified = false;
        req->start = 0;
        req->end = UINT64_MAX;
        return true;
    }

    if (virtual_page_range) {
        uint64_t page_start, page_end;
        uint64_t lo, hi;
        size_t page_size = qemu_target_page_size();

        if (page_size == 0) {
            error_setg(errp, "Target page size is not available");
            return false;
        }

        if (qemu_strtou64(virtual_page_range->start, NULL, 0, &page_start) < 0) {
            error_setg(errp, "Invalid virtual page range start '%s'",
                       virtual_page_range->start);
            return false;
        }
        if (qemu_strtou64(virtual_page_range->end, NULL, 0, &page_end) < 0) {
            error_setg(errp, "Invalid virtual page range end '%s'",
                       virtual_page_range->end);
            return false;
        }

        if (page_start <= page_end) {
            lo = page_start;
            hi = page_end;
        } else {
            lo = page_end;
            hi = page_start;
        }

        if (lo > UINT64_MAX / page_size) {
            error_setg(errp, "Virtual page range start '%s' is too large",
                       virtual_page_range->start);
            return false;
        }
        if (hi > UINT64_MAX / page_size) {
            error_setg(errp, "Virtual page range end '%s' is too large",
                       virtual_page_range->end);
            return false;
        }

        uint64_t start = lo * page_size;
        uint64_t end_base = hi * page_size;
        uint64_t end;

        if (end_base > UINT64_MAX - (page_size - 1)) {
            error_setg(errp, "Virtual page range end '%s' exceeds address space",
                       virtual_page_range->end);
            return false;
        }
        end = end_base + (page_size - 1);

        req->start = start;
        req->end = end;
        req->specified = true;
        return true;
    }

    uint64_t start, end;

    if (qemu_strtou64(virtual_range->start, NULL, 0, &start) < 0) {
        error_setg(errp, "Invalid virtual range start '%s'",
                   virtual_range->start);
        return false;
    }
    if (qemu_strtou64(virtual_range->end, NULL, 0, &end) < 0) {
        error_setg(errp, "Invalid virtual range end '%s'",
                   virtual_range->end);
        return false;
    }

    if (start <= end) {
        req->start = start;
        req->end = end;
    } else {
        req->start = end;
        req->end = start;
    }
    req->specified = true;
    return true;
}

static void mmu_apply_filter_request(const MMULogFilterRequest *req)
{
    if (req->specified) {
        mmu_log_filter_enable(req->start, req->end);
    } else {
        mmu_log_filter_disable();
    }
}

static void mmu_log_enabled_message(const char *message,
                                    const MMULogFilterRequest *req)
{
    if (req->specified) {
        qemu_log("%s (virtual range 0x%016" PRIx64 " - 0x%016" PRIx64 ").\n",
                 message, req->start, req->end);
    } else {
        qemu_log("%s.\n", message);
    }
}

static bool mmu_open_log_file(Error **errp)
{
    if (mmu_log_to_file && mmu_log_file) {
        return true;
    }

    mmu_log_file = fopen(MMU_LOG_FILE_PATH, "a");
    if (!mmu_log_file) {
        error_setg_errno(errp, errno,
                         "Could not open MMU log file '%s'", MMU_LOG_FILE_PATH);
        return false;
    }
    mmu_log_to_file = true;
    return true;
}

static void mmu_close_log_file(void)
{
    if (mmu_log_file) {
        fclose(mmu_log_file);
        mmu_log_file = NULL;
    }
    mmu_log_to_file = false;
}

void qmp_sfmmu(MMULogVirtualRange *virtual_range,
               MMULogVirtualPageRange *virtual_page_range,
               Error **errp)
{
    MMULogFilterRequest filter = { 0 };

    if (!mmu_parse_virtual_range(virtual_range, virtual_page_range,
                                 &filter, errp)) {
        return;
    }

    mmu_apply_filter_request(&filter);
    mmu_fast_log_enabled = true;
    mmu_log_enabled_message("Fast path MMU write logging enabled", &filter);
}

void qmp_qfmmu(Error **errp)
{
    mmu_fast_log_enabled = false;
    qemu_log("Fast path MMU write logging disabled.\n");
    if (!mmu_slow_log_enabled) {
        mmu_close_log_file();
    }
}

void qmp_ssmmu(MMULogVirtualRange *virtual_range,
               MMULogVirtualPageRange *virtual_page_range,
               Error **errp)
{
    MMULogFilterRequest filter = { 0 };

    if (!mmu_parse_virtual_range(virtual_range, virtual_page_range,
                                 &filter, errp)) {
        return;
    }

    mmu_apply_filter_request(&filter);
    mmu_slow_log_enabled = true;
    mmu_log_enabled_message("Slow path MMU write logging enabled", &filter);
}

void qmp_qsmmu(Error **errp)
{
    mmu_slow_log_enabled = false;
    qemu_log("Slow path MMU write logging disabled.\n");
    if (!mmu_fast_log_enabled) {
        mmu_close_log_file();
    }
}

void qmp_wmmu(MMULogVirtualRange *virtual_range,
              MMULogVirtualPageRange *virtual_page_range,
              Error **errp)
{
    MMULogFilterRequest filter = { 0 };

    if (!mmu_parse_virtual_range(virtual_range, virtual_page_range,
                                 &filter, errp)) {
        return;
    }

    if (!mmu_open_log_file(errp)) {
        return;
    }

    mmu_apply_filter_request(&filter);
    mmu_fast_log_enabled = true;
    mmu_slow_log_enabled = true;
    mmu_log_enabled_message(
        "MMU write logging enabled (file and console)", &filter);
}

void qmp_smmu(MMULogVirtualRange *virtual_range,
              MMULogVirtualPageRange *virtual_page_range,
              Error **errp)
{
    MMULogFilterRequest filter = { 0 };

    if (!mmu_parse_virtual_range(virtual_range, virtual_page_range,
                                 &filter, errp)) {
        return;
    }

    mmu_apply_filter_request(&filter);
    mmu_fast_log_enabled = true;
    mmu_slow_log_enabled = true;
    mmu_log_enabled_message("MMU write logging enabled", &filter);
}

void qmp_qmmu(Error **errp)
{
    mmu_fast_log_enabled = false;
    mmu_slow_log_enabled = false;
    mmu_log_filter_disable();
    mmu_close_log_file();
    qemu_log("MMU write logging disabled.\n");
}

HumanReadableText *qmp_x_query_jit(Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");

    if (!tcg_enabled()) {
        error_setg(errp, "JIT information is only available with accel=tcg");
        return NULL;
    }

    tcg_dump_stats(buf);

    return human_readable_text_from_str(buf);
}

static void hmp_tcg_register(void)
{
    monitor_register_hmp_info_hrt("jit", qmp_x_query_jit);
}

type_init(hmp_tcg_register);
