/*
 * A QEMU plugin for counting the number of dynamic instructions.
 *
 * Copyright (C) 2023-2024 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>

/* QEMU */
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* for short code */
#define _QPS_U64   qemu_plugin_scoreboard_u64
#define _GS_PRINTF g_string_append_printf

/* Options */
static bool do_inline;
static bool verbose;
static bool tabsplit;

struct qemu_plugin_scoreboard *insn_count;
struct qemu_plugin_scoreboard *last_pc;

typedef struct
{
    char *match_string;
    struct qemu_plugin_scoreboard *hits;
    struct qemu_plugin_scoreboard *last_hit;
    struct qemu_plugin_scoreboard *total_delta;
} Match;
static GArray *matches;

typedef struct
{
    Match *match;
    uint64_t vaddr;
    uint64_t hits;
    char *disas;
} Instruction;

static void
vcpu_insn_exec_before(unsigned int cpu_index, void *udata)
{
    uint64_t this_pc = GPOINTER_TO_UINT(udata);
    if (this_pc == qemu_plugin_u64_get(_QPS_U64(last_pc), cpu_index)) {
        g_autofree gchar *out = g_strdup_printf(
            "detected repeat execution @ 0x%" PRIx64 "\n", this_pc);
        qemu_plugin_outs(out);
    }

    qemu_plugin_u64_add(_QPS_U64(insn_count), cpu_index, 1);
    qemu_plugin_u64_set(_QPS_U64(last_pc), cpu_index, this_pc);
}

static void
vcpu_insn_matched_exec_before(unsigned int cpu_index, void *udata)
{
    Instruction *insn = (Instruction *)udata;
    Match *match = insn->match;
    uint64_t icount, delta;

    insn->hits++;
    icount = qemu_plugin_u64_get(_QPS_U64(insn_count), cpu_index);
    delta = icount - qemu_plugin_u64_get(_QPS_U64(match->last_hit), cpu_index);
    qemu_plugin_u64_add(_QPS_U64(match->hits), cpu_index, 1);
    qemu_plugin_u64_add(_QPS_U64(match->total_delta), cpu_index, delta);
    qemu_plugin_u64_set(_QPS_U64(match->last_hit), cpu_index, icount);

    if (verbose) {
        uint64_t hits, total;

        hits = qemu_plugin_u64_get(_QPS_U64(match->hits), cpu_index);
        total = qemu_plugin_u64_get(_QPS_U64(match->total_delta), cpu_index);
        g_autoptr(GString) out = g_string_new("");
        _GS_PRINTF(out, "0x%" PRIx64 ", '%s', %" PRId64 " hits", insn->vaddr,
                   insn->disas, insn->hits);
        _GS_PRINTF(out,
                   ", %" PRId64 " match hits, "
                   "Δ+%" PRId64 " since last match,"
                   " %" PRId64 " avg insns/match\n",
                   hits, delta, total / hits);
        qemu_plugin_outs(out->str);
    }
}

/*
 * On translation of a new TB
 *
 * QEMU converts code by translation block (TB). By hooking here we can then
 * hook a callback on each instruction execution.
 */
static void
vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        if (do_inline) {
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, _QPS_U64(insn_count), 1);
        } else {
            uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
            qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec_before,
                                                   QEMU_PLUGIN_CB_NO_REGS,
                                                   GUINT_TO_POINTER(vaddr));
        }

        /*
         * If we are tracking specific instructions, we need additional
         * information about the instruction, such as its disassembly
         * and match string.
         */
        if (matches) {
            char *insn_disas = qemu_plugin_insn_disas(insn);

            for (int j = 0; j < matches->len; j++) {
                Match *m = &g_array_index(matches, Match, j);
                if (g_str_has_prefix(insn_disas, m->match_string)) {
                    /* TODO collect these records and release them */
                    Instruction *rec = g_new0(Instruction, 1);
                    rec->disas = g_strdup(insn_disas);
                    rec->vaddr = qemu_plugin_insn_vaddr(insn);
                    rec->match = m;

                    qemu_plugin_register_vcpu_insn_exec_cb(
                        insn, vcpu_insn_matched_exec_before,
                        QEMU_PLUGIN_CB_NO_REGS, rec);
                }
            }
            g_free(insn_disas);
        }
    }
}

/* Add an instruction match. */
static void
parse_match(char *match)
{
    /*
     * Terminate match string with a space. Avoid partial matching, such as
     * 'bl' in 'ble'.
     * Add option 'tabsplit=on' before any 'match=...' if the disassembler
     * uses a tab as the delimiter between opcode and operands.
     */
    gchar *match_str = g_strdup_printf("%s%c", match, tabsplit ? '\t' : ' ');

    Match new_match;
    new_match.match_string = match_str;
    new_match.hits = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    new_match.last_hit = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    new_match.total_delta = qemu_plugin_scoreboard_new(sizeof(uint64_t));

    if (!matches) {
        matches = g_array_new(false, true, sizeof(Match));
    }
    g_array_append_val(matches, new_match);
}

static void
plugin_init(void)
{
    insn_count = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    last_pc = qemu_plugin_scoreboard_new(sizeof(uint64_t));
}

static void
matches_free(void)
{
    for (int i = 0; i < matches->len; i++) {
        Match *match = &g_array_index(matches, Match, i);

        g_free(match->match_string);
        qemu_plugin_scoreboard_free(match->hits);
        qemu_plugin_scoreboard_free(match->last_hit);
        qemu_plugin_scoreboard_free(match->total_delta);
    }
    g_array_free(matches, TRUE);
    matches = NULL;
}

static void
plugin_free(void)
{
    qemu_plugin_scoreboard_free(insn_count);
    insn_count = NULL;
    qemu_plugin_scoreboard_free(last_pc);
    last_pc = NULL;

    if (matches) {
        matches_free();
    }
}

static void
plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) out = g_string_new(NULL);

    _GS_PRINTF(out, "Total insns: %" PRIu64 "\n",
               qemu_plugin_u64_sum(_QPS_U64(insn_count)));

    if (matches) {
        for (int j = 0; j < matches->len; j++) {
            Match *match = &g_array_index(matches, Match, j);
            uint64_t hits = qemu_plugin_u64_sum(_QPS_U64(match->hits));
            uint64_t avg_delta;

            if (hits == 0)
                continue;

            avg_delta = qemu_plugin_u64_sum(_QPS_U64(match->total_delta));
            avg_delta /= hits;
            _GS_PRINTF(out,
                       "Insn '%s': "
                       "%" PRId64 " match hits, "
                       "%" PRId64 " avg insns/match\n",
                       match->match_string, hits, avg_delta);
        }
    }
    qemu_plugin_outs(out->str);

    plugin_free();
}

QEMU_PLUGIN_EXPORT int
qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc,
                    char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_autofree char **tokens = g_strsplit(opt, "=", 2);

        if (g_strcmp0(tokens[0], "inline") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_inline)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "verbose") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &verbose)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "tabsplit") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &tabsplit)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "match") == 0) {
            parse_match(tokens[1]);
        } else {
            fprintf(stderr, "option parsing failed: %s\n", opt);
            return -1;
        }
    }

    plugin_init();

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
