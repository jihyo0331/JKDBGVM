// contrib/plugins/opcode.c
// Stream {cpu, pc, len, bytes, asm} as JSONL to a UNIX socket.
// Disassembly via Capstone (x86-64 only) and is MT-safe with a mutex.

#include "qemu-plugin.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <capstone/capstone.h>
#include <glib.h>

QEMU_PLUGIN_EXPORT int  qemu_plugin_version = QEMU_PLUGIN_VERSION;
/* silence -Wmissing-prototypes on some builds */
QEMU_PLUGIN_EXPORT void qemu_plugin_exit(qemu_plugin_id_t id);

/* ---- runtime options ---- */
static const char *opt_sock_path = "/tmp/opcode.sock";
static int        opt_sample     = 1;          /* send 1 per N insns (1=all) */
static int        opt_cpu        = -1;         /* -1 = all vCPUs */
static uint64_t   opt_start      = 0;
static uint64_t   opt_end        = UINT64_MAX;
static size_t     opt_maxlen     = 15;         /* x86 max 15 */
/* output selection */
static int        opt_emit_bytes = 1;          /* bytes=0/1 (default on) */
static int        opt_emit_dis   = 0;          /* dis=0/1   (default off) */

/* ---- state ---- */
static int sock_fd = -1;
static uint64_t seq_global = 0;
/* Capstone handle for x86-64 */
static csh cs64 = 0;
/* Global mutex to serialize Capstone calls across vCPUs */
static pthread_mutex_t cs_mtx = PTHREAD_MUTEX_INITIALIZER;

/* tiny JSON escape: quotes/backslash; control bytes → space */
static void json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 2 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= outsz) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if (c >= 0x20) {
            out[o++] = c;
        } else {
            out[o++] = ' ';
        }
    }
    if (o < outsz) out[o] = '\0';
}

static inline void send_json_line(const char *s, size_t n) {
    if (sock_fd >= 0) (void)write(sock_fd, s, n);
}

static void on_exec(unsigned int cpu_index, void *udata) {
    struct qemu_plugin_insn *insn = (struct qemu_plugin_insn *)udata;

    /* sampling */
    if (opt_sample > 1) {
        uint64_t s = __atomic_add_fetch(&seq_global, 1, __ATOMIC_RELAXED);
        if (s % (uint64_t)opt_sample) return;
    }
    /* vCPU filter */
    if (opt_cpu >= 0 && (int)cpu_index != opt_cpu) return;

    uint64_t pc = qemu_plugin_insn_vaddr(insn);
    if (!(opt_start <= pc && pc < opt_end)) return;

    /* We'll fetch from memory at RIP and decode one insn. */
    size_t len = opt_maxlen;
    if (len == 0) return;                 /* guard */

    /* Read bytes at RIP directly from guest memory */
    g_autoptr(GByteArray) buf = g_byte_array_new();
    if (!qemu_plugin_read_memory_vaddr(pc, buf, len)) {
        return;                           /* unable to read */
    }
    if (buf->len == 0) return;

    /* bytes → hex (optional) */
    char bytes_hex[2 * 15 + 1]; bytes_hex[0] = '\0';
    size_t emit_n = 0;
    if (opt_emit_bytes) {
        size_t off = 0;
        size_t max_emit = buf->len;
        if (max_emit > opt_maxlen) max_emit = opt_maxlen;
        if (max_emit > 15) max_emit = 15;
        for (size_t i = 0; i < max_emit && off + 2 < sizeof(bytes_hex); i++) {
            off += snprintf(bytes_hex + off, sizeof(bytes_hex) - off, "%02x", buf->data[i]);
        }
        bytes_hex[off] = '\0';
        emit_n = max_emit;
    }

    /* disassemble via Capstone (x86-64) */
    char asm_buf[512] = {0};
    if (opt_emit_dis) {
        cs_insn *ci = NULL;
        size_t dn = 0;

        pthread_mutex_lock(&cs_mtx); /* Capstone isn't multi-thread-safe per handle */
        if (!dn && cs64) dn = cs_disasm(cs64, buf->data, buf->len, pc, 1, &ci);
        if (dn == 1 && ci) {
            char tmp[480];
            if (ci->op_str[0]) snprintf(tmp, sizeof(tmp), "%s %s", ci->mnemonic, ci->op_str);
            else               snprintf(tmp, sizeof(tmp), "%s", ci->mnemonic);
            json_escape(tmp, asm_buf, sizeof(asm_buf));
            /* Use decoded size for len and trim emitted bytes accordingly */
            if (ci->size > 0 && ci->size <= buf->len) {
                len = ci->size;
                if (emit_n && emit_n > len) {
                    emit_n = len;
                }
            }
            cs_free(ci, dn);
        }
        pthread_mutex_unlock(&cs_mtx);
    }

    /* If we didn't disassemble, set len to what we emitted/read */
    if (!opt_emit_dis) {
        if (emit_n) {
            len = emit_n;
        } else if (buf->len < len) {
            len = buf->len;
        }
    }

    /* emit JSON (only enabled fields) */
    char line[1024];
    int n = 0;
    if (opt_emit_bytes && opt_emit_dis) {
        n = snprintf(line, sizeof(line),
            "{\"cpu\":%u,\"pc\":\"0x%016" PRIx64 "\",\"len\":%zu,"
            "\"bytes\":\"%s\",\"asm\":\"%s\"}\n",
            cpu_index, pc, len, bytes_hex, asm_buf);
    } else if (opt_emit_bytes) {
        n = snprintf(line, sizeof(line),
            "{\"cpu\":%u,\"pc\":\"0x%016" PRIx64 "\",\"len\":%zu,"
            "\"bytes\":\"%s\"}\n",
            cpu_index, pc, len, bytes_hex);
    } else if (opt_emit_dis) {
        n = snprintf(line, sizeof(line),
            "{\"cpu\":%u,\"pc\":\"0x%016" PRIx64 "\",\"len\":%zu,"
            "\"asm\":\"%s\"}\n",
            cpu_index, pc, len, asm_buf);
    } else {
        /* nothing to emit */
        return;
    }
    if (n > 0 && (size_t)n < sizeof(line)) send_json_line(line, (size_t)n);
}

static void tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    int n = qemu_plugin_tb_n_insns(tb);
    for (int i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_insn_exec_cb(
            insn, on_exec, QEMU_PLUGIN_CB_NO_REGS, (void *)insn);
    }
}

static int connect_unix_stream(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa; memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    return fd;
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    /* parse args */
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "sock=", 5))        opt_sock_path   = argv[i] + 5;
        else if (!strncmp(argv[i], "sample=", 7))  { int v = atoi(argv[i] + 7); opt_sample = (v >= 1) ? v : 1; }
        else if (!strncmp(argv[i], "cpu=", 4))     opt_cpu         = atoi(argv[i] + 4);
        else if (!strncmp(argv[i], "start=0x", 8)) opt_start       = strtoull(argv[i] + 8, NULL, 16);
        else if (!strncmp(argv[i], "end=0x", 6))   opt_end         = strtoull(argv[i] + 6, NULL, 16);
        else if (!strncmp(argv[i], "maxlen=", 7))  { int v = atoi(argv[i] + 7); if (v < 1) v = 1; if (v > 15) v = 15; opt_maxlen = (size_t)v; }
        else if (!strncmp(argv[i], "bytes=", 6))   opt_emit_bytes  = atoi(argv[i] + 6) != 0;
        else if (!strncmp(argv[i], "dis=", 4))     opt_emit_dis    = atoi(argv[i] + 4) != 0;
    }

    /* Capstone: open handle for x86-64 only */
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &cs64) != CS_ERR_OK) cs64 = 0;
    if (cs64) { cs_option(cs64, CS_OPT_DETAIL, CS_OPT_OFF); cs_option(cs64, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL); }

    /* connect to consumer */
    sock_fd = connect_unix_stream(opt_sock_path);

    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb);
    return 0;
}

QEMU_PLUGIN_EXPORT
void qemu_plugin_exit(qemu_plugin_id_t id) {
    if (sock_fd >= 0) close(sock_fd);
    if (cs64) { cs_close(&cs64); cs64 = 0; }
}
