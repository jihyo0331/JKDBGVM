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
#include "qapi/qapi-commands-misc.h"
#include <errno.h>

#define MMU_LOG_FILE_PATH "../vm/mmu.log"

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

void qmp_sfmmu(Error **errp)
{
    mmu_fast_log_enabled = true;
    qemu_log("Fast path MMU write logging enabled.\n");
}

void qmp_qfmmu(Error **errp)
{
    mmu_fast_log_enabled = false;
    qemu_log("Fast path MMU write logging disabled.\n");
    if (!mmu_slow_log_enabled) {
        mmu_close_log_file();
    }
}

void qmp_ssmmu(Error **errp)
{
    mmu_slow_log_enabled = true;
    qemu_log("Slow path MMU write logging enabled.\n");
}

void qmp_qsmmu(Error **errp)
{
    mmu_slow_log_enabled = false;
    qemu_log("Slow path MMU write logging disabled.\n");
    if (!mmu_fast_log_enabled) {
        mmu_close_log_file();
    }
}

void qmp_wmmu(Error **errp)
{
    if (!mmu_open_log_file(errp)) {
        return;
    }

    mmu_fast_log_enabled = true;
    mmu_slow_log_enabled = true;
    qemu_log("MMU write logging enabled (file and console).\n");
}

void qmp_smmu(Error **errp)
{
    mmu_fast_log_enabled = true;
    mmu_slow_log_enabled = true;
    qemu_log("MMU write logging enabled.\n");
}

void qmp_qmmu(Error **errp)
{
    mmu_fast_log_enabled = false;
    mmu_slow_log_enabled = false;
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
