// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

extern "C" {

#include "config.h"
#include "qemu-common.h"
#include "monitor.h"
#include "cpu.h"
#include "disas.h"

#include "panda_plugin.h"

}

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <map>
#include <list>
#include <algorithm>

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
int mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf);
int mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf);

}

struct prog_point {
    target_ulong caller;
    target_ulong pc;
    target_ulong cr3;
    bool operator <(const prog_point &p) const {
        return (this->pc < p.pc) || \
               (this->pc == p.pc && this->caller < p.caller) || \
               (this->pc == p.pc && this->caller == p.caller && this->cr3 < p.cr3);
    }
};

std::map<prog_point,FILE *> text_tracker;
//FILE *text_memlog;

int mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    prog_point p = {};
#ifdef TARGET_I386
    panda_virtual_memory_rw(env, env->regs[R_EBP]+4, (uint8_t *)&p.caller, 4, 0);
    if((env->hflags & HF_CPL_MASK) != 0) // Lump all kernel-mode CR3s together
        p.cr3 = env->cr[3];
#endif
    p.pc = pc;
    if (text_tracker[p] == NULL) {
        char path[256];
        sprintf(path, "/scratch/tapdump/" TARGET_FMT_lx "." TARGET_FMT_lx "." TARGET_FMT_lx,
            p.cr3, p.pc, p.caller);
        text_tracker[p] = fopen(path, "wb");
        if(text_tracker[p] == NULL) {
            perror("fopen");
        }
    }

    fwrite((unsigned char *)buf, size, 1, text_tracker[p]);
    return 1;
}

bool init_plugin(void *self) {
    panda_cb pcb;

    printf("Initializing plugin memdump\n");

    // Need this to get EIP with our callbacks
    panda_enable_precise_pc();
    // Enable memory logging
    panda_enable_memcb();

    pcb.virt_mem_write = mem_write_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_WRITE, pcb);

    return true;
}

void uninit_plugin(void *self) {
    printf("Closing files...\n");
    size_t i = 0;
    size_t mapsize = text_tracker.size();
    for(std::map<prog_point,FILE *>::iterator it = text_tracker.begin(); it != text_tracker.end(); it++) {
        i++;
        fclose(it->second);
        if ((i & 0xfff) == 0) printf("%ld / %ld files closed\n", i, mapsize);
    }
}
