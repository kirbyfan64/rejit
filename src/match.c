/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define REJIT_INSTR
#include "rejit.h"

#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include "dynasm/dasm_proto.h"
#include "utf/utf.h"

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#define MAXSZ 100

static void compile_one(dasm_State**, rejit_instruction*, int*, int*, int,
                        rejit_flags);

#define GROW dasm_growpc(Dst, ++*pcl)

static unsigned long genmagic(char* s, char* min, size_t* len) {
    unsigned long res=0;
    char* b = s;
    Rune r;
    // Get minimum.
    *min = *s;
    for (b=s; *s; ++s) if (*s < *min) *min = *s;
    *len = s-b;
    for (s=b; *s; ++s) {
        int diff, rl = chartorune(&r, s);
        if (rl > 1) {
            int i;
            b[*len+(s-b)+1] = 'W';
            for (i=1; i<rl; ++i) {
                b[*len+(s-b+i)+1] = 'U';
            }
            s += rl-1;
            continue;
        }
        diff = *s-*min;
        if (diff > sizeof(res)*8-1) b[*len+(s-b)+1] = 0;
        else res |= 1lu<<diff;
    }
    return res;
}

static void unskip(rejit_instruction* instr) {
    rejit_instruction* i;
    if (instr->kind > RJ_ISKIP) instr->kind -= RJ_ISKIP;
    if (instr->kind == RJ_IGROUP)
        for (i = (rejit_instruction*)instr++->value; instr != i; ++instr)
            unskip(instr);
    else {
        if (instr->kind > RJ_IVARG) unskip((rejit_instruction*)instr->value);
        if (instr->kind > RJ_IARG) unskip(instr+1);
    }
}

static void skip(rejit_instruction* instr) {
    if (instr->kind < RJ_ISKIP) instr->kind += RJ_ISKIP;
}

#include "codegen.c"

static void* link_and_encode(dasm_State** d, size_t* sz) {
    void* buf;
    #ifdef DEBUG
    FILE* f;
    #endif
    dasm_link(d, sz);
    buf = mmap(0, *sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
               0);
    dasm_encode(d, buf);
    #ifdef DEBUG
    f = fopen("/tmp/.rejit.dis", "w");
    if (f) {
        fwrite(buf, 1, *sz, f);
        fclose(f);
    }
    #endif
    mprotect(buf, *sz, PROT_READ | PROT_EXEC);
    return buf;
}

static rejit_func compile(dasm_State** d, size_t* sz, rejit_instruction* instrs,
                          rejit_flags flags) {
    int i;
    void* labels[lbl__MAX];
    dasm_setupglobal(d, labels, lbl__MAX);
    dasm_setup(d, actions);

    int errpc=0, pcl=1;
    dasm_growpc(d, 1);

    compile_prolog(d);
    for (i=0; instrs[i].kind; ++i)
        compile_one(d, &instrs[i], &errpc, &pcl, 0, flags);
    compile_epilog(d);

    return link_and_encode(d, sz);
}

rejit_matcher rejit_compile_instrs(rejit_instruction* instrs, int groups,
                                   rejit_flags flags) {
    rejit_func func;
    rejit_matcher res;
    size_t sz;
    dasm_State* d;
    dasm_init(&d, DASM_MAXSECTION);
    func = compile(&d, &sz, instrs, flags);
    dasm_free(&d);
    res = malloc(sizeof(struct rejit_matcher_type));
    if (!res) return NULL;
    res->func = func;
    res->sz = sz;
    res->groups = groups;
    return res;
}

int rejit_match(rejit_matcher m, const char* str, rejit_group* groups) {
    return m->func(str, groups);
}

int rejit_search(rejit_matcher m, const char* str, const char** tgt,
                 rejit_group* groups) {
    int res = -1;
    for (;res == -1 && *str; ++str) {
        if (m->groups) memset(groups, 0, sizeof(rejit_group)*m->groups);
        res = rejit_match(m, str, groups);
    }
    if (tgt != NULL && res != -1) *tgt = str+1;
    return res;
}

void rejit_free_matcher(rejit_matcher m) { munmap(m->func, m->sz); free(m); }
