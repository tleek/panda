/*
 * This is the start of instrumentation code that we can run as we process
 * taint.  To start with, this implements the gathering of taint statistics for
 * the guest memory as we process taint.
 */

#include "stdint.h"
#include "stdio.h"

#include "bitvector_label_set.c"
#include "laredo_instrumentation.h"

#define INSTR_INTERVAL 10000

FILE *taintstats;
uint64_t instr_count = 0;

// Prints out tainted memory
void memplot(Shad *shad){
    FILE *memplotlog = fopen("memory.csv", "w");
    fprintf(memplotlog, "\"Address\",\"Label\",\"Type\"\n");
    unsigned int i;
    for (i = 0; i < 0xffffffff; i++){
#ifdef X86_64
        LabelSet *ls = shad_dir_find_64(shad->ram, i);
        if (ls){
            unsigned int j;
            for (j = 0; j < ls->set->current_size; j++){
                fprintf(memplotlog, "%d,%d,%d\n", i, ls->set->members[j],
                    ls->type);
            }
        }
#else
        if (get_ram_bit(shad, i)){
            LabelSet *ls = shad_dir_find_32(shad->ram, i);
            unsigned int j;
            for (j = 0; j < ls->set->current_size; j++){
                fprintf(memplotlog, "%d,%d,%d\n", i, ls->set->members[j],
                    ls->type);
            }
        }
#endif
    }
    fclose(memplotlog);
}

// Prints out taint of write() buffer
void bufplot(Shad *shad, uint64_t addr, int length){
    FILE *bufplotlog = fopen("writebuf.csv", "w");
    fprintf(bufplotlog, "\"Address\",\"Label\",\"Type\"\n");
    uint64_t i;
    for (i = addr; i < addr+length; i++){
#ifdef X86_64
        LabelSet *ls = shad_dir_find_64(shad->ram, i);
        if (ls){
            unsigned int j;
            for (j = 0; j < ls->set->current_size; j++){
                fprintf(bufplotlog, "%lu,%d,%d\n", i, ls->set->members[j],
                    ls->type);
            }
        }
#else
        if (get_ram_bit(shad, i)){
            LabelSet *ls = shad_dir_find_32(shad->ram, i);
            unsigned int j;
            for (j = 0; j < ls->set->current_size; j++){
                fprintf(bufplotlog, "%lu,%d,%d\n", i, ls->set->members[j],
                    ls->type);
            }
        }
#endif
    }
    fclose(bufplotlog);
}

/*
 * Dump the number of tainted bytes of guest memory to a file on an instruction
 * interval defined by INSTR_INTERVAL.
 */
void dump_taint_stats(Shad *shad){
    assert(shad != NULL);
    uint64_t tainted_addrs = 0;
    instr_count++;
    if (__builtin_expect(((instr_count % INSTR_INTERVAL) == 0), 0)){
        if (__builtin_expect((taintstats == NULL), 0)){
            taintstats = fopen("taintstats.csv", "w");
            fprintf(taintstats, "\"Instrs\",\"TaintedAddrs\"\n");
        }
#ifdef X86_64
        tainted_addrs = shad_dir_occ_64(shad->ram);
#else
        tainted_addrs = shad_dir_occ_32(shad->ram);
#endif
        fprintf(taintstats, "%lu,%lu\n", instr_count, tainted_addrs);
        fflush(taintstats);
    }
}

void cleanup_taint_stats(void){
    if (taintstats){
        fclose(taintstats);
    }
}

