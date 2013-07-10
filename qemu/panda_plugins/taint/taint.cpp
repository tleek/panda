/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */
/*
 * PANDA taint analysis plugin
 * Ryan Whelan
 */

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

extern "C" {

#include "panda_plugin.h"
#include "panda_memlog.h"
#include "panda_stats.h"

#ifndef CONFIG_SOFTMMU
#include "syscall_defs.h"
#endif

}

#include "llvm/PassManager.h"
#include "llvm/PassRegistry.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"

#include "tcg-llvm.h"

#include "llvm_taint_lib.h"
#include "panda_dynval_inst.h"
#include "taint_processor.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
int before_block_exec(CPUState *env, TranslationBlock *tb);
int after_block_exec(CPUState *env, TranslationBlock *tb,
    TranslationBlock *next_tb);
int cb_cpu_restore_state(CPUState *env, TranslationBlock *tb);
int guest_hypercall_callback(CPUState *env);

#ifndef CONFIG_SOFTMMU
int user_after_syscall(void *cpu_env, bitmask_transtbl *fcntl_flags_tbl,
                       int num, abi_long arg1, abi_long arg2, abi_long arg3,
                       abi_long arg4, abi_long arg5, abi_long arg6, abi_long
                       arg7, abi_long arg8, void *p, abi_long ret);

#endif
int phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf);
int phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
        target_ulong size, void *buf);

Shad *shadow; // Global shadow memory

}

// Our pass manager to derive taint ops
llvm::FunctionPassManager *taintfpm;

// Taint and instrumentation function passes
llvm::PandaTaintFunctionPass *PTFP;
llvm::PandaInstrFunctionPass *PIFP;

// Global count of taint labels
int count = 0;

// Apply taint to a buffer of memory
void add_taint(Shad *shad, TaintOpBuffer *tbuf, uint64_t addr, int length){
    struct addr_struct a = {};
    a.typ = MADDR;
    a.val.ma = addr;
    struct taint_op_struct op = {};
    op.typ = LABELOP;
    for (int i = 0; i < length; i++){
        a.off = i;
        op.val.label.a = a;
        op.val.label.l = i + count; // byte label
        //op.val.label.l = 1; // binary label
        tob_op_write(tbuf, op);
    }
    tob_process(tbuf, shad, NULL);
    count += length;
}

/*
 * These memory callbacks are only for whole-system mode.  User-mode memory
 * accesses are captured by IR instrumentation.
 */
int phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    log_dynval(dynval_buffer, ADDRENTRY, STORE, addr);
    return 0;
}

int phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
        target_ulong size, void *buf){
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    log_dynval(dynval_buffer, ADDRENTRY, LOAD, addr);
    return 0;
}

namespace llvm {

static void llvm_init(){
    ExecutionEngine *ee = tcg_llvm_ctx->getExecutionEngine();
    FunctionPassManager *fpm = tcg_llvm_ctx->getFunctionPassManager();
    Module *mod = tcg_llvm_ctx->getModule();
    LLVMContext &ctx = mod->getContext();

    // Link logging function in with JIT
    Function *logFunc;
    std::vector<Type*> argTypes;
    // DynValBuffer*
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(uintptr_t)));
    // DynValEntryType
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(DynValEntryType)));
    // LogOp
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(LogOp)));
    // Dynamic value
    argTypes.push_back(IntegerType::get(ctx, 8*sizeof(uintptr_t)));
    logFunc = Function::Create(
            FunctionType::get(Type::getVoidTy(ctx), argTypes, false),
            Function::ExternalLinkage, "log_dynval", mod);
    logFunc->addFnAttr(Attribute::AlwaysInline);
    ee->addGlobalMapping(logFunc, (void*) &log_dynval);

    // Create instrumentation pass and add to function pass manager
    llvm::FunctionPass *instfp = createPandaInstrFunctionPass(mod);
    fpm->add(instfp);
    PIFP = static_cast<PandaInstrFunctionPass*>(instfp);
}

} // namespace llvm

// Derive taint ops
int before_block_exec(CPUState *env, TranslationBlock *tb){
    //printf("%s\n", tcg_llvm_get_func_name(tb));

    taintfpm->run(*(tb->llvm_function));
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    clear_dynval_buffer(dynval_buffer);
    return 0;
}

// Execute taint ops
int after_block_exec(CPUState *env, TranslationBlock *tb,
        TranslationBlock *next_tb){
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    rewind_dynval_buffer(dynval_buffer);

    //printf("%s\n", tb->llvm_function->getName().str().c_str());
    //PTFP->debugTaintOps();
    //printf("\n\n");
    execute_taint_ops(PTFP->ttb, shadow, dynval_buffer);

    // Make sure there's nothing left in the buffer
    assert(dynval_buffer->ptr - dynval_buffer->start == dynval_buffer->cur_size);
    return 0;
}

int cb_cpu_restore_state(CPUState *env, TranslationBlock *tb){
    printf("EXCEPTION - logging\n");
    DynValBuffer *dynval_buffer = PIFP->PIV->getDynvalBuffer();
    log_exception(dynval_buffer);

    // Then execute taint ops up until the exception occurs.  Execution of taint
    // ops will stop at the point of the exception.
    rewind_dynval_buffer(dynval_buffer);
    execute_taint_ops(PTFP->ttb, shadow, dynval_buffer);

    // Make sure there's nothing left in the buffer
    assert(dynval_buffer->ptr - dynval_buffer->start == dynval_buffer->cur_size);
    return 0;
}

int guest_hypercall_callback(CPUState *env) {
#ifdef TARGET_I386
  if(env->regs[R_EAX] == 0xdeadbeef) {
    target_ulong buf_start = env->regs[R_ECX];
    target_ulong buf_len = env->regs[R_EDX];

    if(env->regs[R_EBX] == 0) { //Taint label
      TaintOpBuffer *tempBuf = tob_new(5*1048576 /* 1MB */);
      add_taint(shadow, tempBuf, (uint64_t)buf_start, (int)buf_len);
      tob_delete(tempBuf);
    }
    else if(env->regs[R_EBX] == 1) { //Query taint on label
      bufplot(shadow, (uint64_t)buf_start, (int)buf_len);
    }
  }
#endif
    return 1;
}

#ifndef CONFIG_SOFTMMU

// Globals to keep track of file descriptors
int infd = -1;
int outfd = -1;

/*
 * Kind of a hacky way to see if the file being opened is something we're
 * interested in.  For now, we are working under the assumption that a program
 * will open/read one file of interest, and open/write the other file of
 * interest.  So we assume that files that are opened from /etc and /lib aren't
 * of interest. /proc and openssl.cnf also aren't interesting, from looking at
 * openssl.
 */
static int user_open(bitmask_transtbl *fcntl_flags_tbl, abi_long ret, void *p,
              abi_long flagarg){
    const char *file = path((const char*)p);
    unsigned int flags = target_to_host_bitmask(flagarg, fcntl_flags_tbl);
    if (ret > 0){
        if((strncmp(file, "/etc", 4) != 0)
                && (strncmp(file, "/lib", 4) != 0)
                && (strncmp(file, "/proc", 5) != 0)
                //&& (strncmp(file, "/dev", 4) != 0)
                && (strncmp(file, "/usr", 4) != 0)
                && (strstr(file, "openssl.cnf") == 0)
                && (strstr(file, "xpdfrc") == 0)){
            printf("open %s for ", file);
            if ((flags & (O_RDONLY | O_WRONLY)) == O_RDONLY){
                printf("read\n");
                infd = ret;
            }
            if (flags & O_WRONLY){
                printf("write\n");
                outfd = ret;
            }
        }
    }
    return 0;
}

static int user_creat(abi_long ret, void *p){
    const char *file = path((const char*)p);
    if (ret > 0){
        printf("open %s for write\n", file);
        outfd = ret;
    }
    return 0;
}

static int user_read(abi_long ret, abi_long fd, void *p){
    if (ret > 0 && fd == infd){
        TaintOpBuffer *tempBuf = tob_new(5*1048576 /* 1MB */);
        add_taint(shadow, tempBuf, (uint64_t)p /*pointer*/, ret /*length*/);
        tob_delete(tempBuf);
    }
    return 0;
}

static int user_write(abi_long ret, abi_long fd, void *p){
    if (ret > 0 && fd == outfd){
        bufplot(shadow, (uint64_t)p /*pointer*/, ret /*length*/);
    }
    return 0;
}

int user_after_syscall(void *cpu_env, bitmask_transtbl *fcntl_flags_tbl,
                       int num, abi_long arg1, abi_long arg2, abi_long arg3,
                       abi_long arg4, abi_long arg5, abi_long arg6,
                       abi_long arg7, abi_long arg8, void *p, abi_long ret){
    switch (num){
        case TARGET_NR_read:
            user_read(ret, arg1, p);
            break;
        case TARGET_NR_write:
            user_write(ret, arg1, p);
            break;
        case TARGET_NR_open:
            user_open(fcntl_flags_tbl, ret, p, arg2);
            break;
        case TARGET_NR_openat:
            user_open(fcntl_flags_tbl, ret, p, arg3);
            break;
        case TARGET_NR_creat:
            user_creat(ret, p);
            break;
        default:
            break;
    }
    return 0;
}

#endif // CONFIG_SOFTMMU

bool init_plugin(void *self) {
    printf("Initializing taint plugin\n");
    panda_cb pcb;
    panda_enable_memcb();
    panda_disable_tb_chaining();
    pcb.before_block_exec = before_block_exec;
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);
    pcb.after_block_exec = after_block_exec;
    panda_register_callback(self, PANDA_CB_AFTER_BLOCK_EXEC, pcb);
    pcb.phys_mem_read = phys_mem_read_callback;
    panda_register_callback(self, PANDA_CB_PHYS_MEM_READ, pcb);
    pcb.phys_mem_write = phys_mem_write_callback;
    panda_register_callback(self, PANDA_CB_PHYS_MEM_WRITE, pcb);
    pcb.cb_cpu_restore_state = cb_cpu_restore_state;
    panda_register_callback(self, PANDA_CB_CPU_RESTORE_STATE, pcb);
    pcb.guest_hypercall = guest_hypercall_callback;
    panda_register_callback(self, PANDA_CB_GUEST_HYPERCALL, pcb);

#ifndef CONFIG_SOFTMMU
    pcb.user_after_syscall = user_after_syscall;
    panda_register_callback(self, PANDA_CB_USER_AFTER_SYSCALL, pcb);
#endif

    if (!execute_llvm){
        panda_enable_llvm();
    }
    llvm::llvm_init();
    panda_enable_llvm_helpers();

    /*
     * Run instrumentation pass over all helper functions that are now in the
     * module, and verify module.
     */
    llvm::Module *mod = tcg_llvm_ctx->getModule();
    for (llvm::Module::iterator i = mod->begin(); i != mod->end(); i++){
        if (i->isDeclaration()){
            continue;
        }
        PIFP->runOnFunction(*i);
    }
    std::string err;
    if(verifyModule(*mod, llvm::AbortProcessAction, &err)){
        printf("%s\n", err.c_str());
        exit(1);
    }

    /*
     * Taint processor initialization
     */

    //uint32_t ram_size = 536870912; // 500MB each
#ifdef TARGET_X86_64
    // this is only for the fast bitmap which we currently aren't using for
    // 64-bit, it only supports 32-bit
    uint64_t ram_size = 0;
#else
    uint32_t ram_size = 0xffffffff; //guest address space -- QEMU user mode
#endif
    uint64_t hd_size =  536870912;
    uint64_t io_size = 536870912;
    uint16_t num_vals = 2000; // LLVM virtual registers
    shadow = tp_init(hd_size, ram_size, io_size, num_vals);
    if (shadow == NULL){
        printf("Error initializing shadow memory...\n");
        exit(1);
    }

    taintfpm = new llvm::FunctionPassManager(tcg_llvm_ctx->getModule());

    // Add the taint analysis pass to our taint pass manager
    llvm::FunctionPass *taintfp =
        llvm::createPandaTaintFunctionPass(5*1048576/* global taint op buffer
        size, 5MB */, NULL /* existing taint cache */);
    PTFP = static_cast<llvm::PandaTaintFunctionPass*>(taintfp);
    taintfpm->add(taintfp);
    taintfpm->doInitialization();

    // Populate taint cache with helper function taint ops
    for (llvm::Module::iterator i = mod->begin(); i != mod->end(); i++){
        if (i->isDeclaration()){
            continue;
        }
        PTFP->runOnFunction(*i);
    }

    return true;
}

void uninit_plugin(void *self) {
    /*
     * XXX: Here, we unload our pass from the PassRegistry.  This seems to work
     * fine, until we reload this plugin again into QEMU and we get an LLVM
     * assertion saying the pass is already registered.  This seems like a bug
     * with LLVM.  Switching between TCG and LLVM works fine when passes aren't
     * added to LLVM.
     */
    llvm::PassRegistry *pr = llvm::PassRegistry::getPassRegistry();
    const llvm::PassInfo *pi =
        //pr->getPassInfo(&llvm::PandaInstrFunctionPass::ID);
        pr->getPassInfo(llvm::StringRef("PandaInstr"));
    if (!pi){
        printf("Unable to find 'PandaInstr' pass in pass registry\n");
    }
    else {
        pr->unregisterPass(*pi);
    }

    delete taintfpm; // Delete function pass manager and pass

    tp_free(shadow);

    panda_disable_llvm();
    panda_disable_memcb();
    panda_enable_tb_chaining();
}

