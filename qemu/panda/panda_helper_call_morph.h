#ifndef PANDA_HELPER_CALL_MORPH_H
#define PANDA_HELPER_CALL_MORPH_H

#ifdef __cplusplus

#include "llvm/LLVMContext.h"
#include "llvm/Pass.h"
#include "llvm/Support/InstVisitor.h"

namespace llvm {

class PandaCallMorphFunctionPass;

/* 
 * PandaHelperCallVisitor class
 * Changes all LLVM call instructions to call LLVM versions of helper functions.
 */
class PandaHelperCallVisitor: public InstVisitor<PandaHelperCallVisitor> {
    PandaCallMorphFunctionPass *PCMFP;
public:
    PandaHelperCallVisitor(PandaCallMorphFunctionPass *pass) :
        PCMFP(pass) {}

    ~PandaHelperCallVisitor(){}

    void visitCallInst(CallInst &I);
};

/*
 * PandaCallMorphFunctionPass
 * A function pass that changes calls of helper functions to the LLVM version
 * for the functions in our helper function bitcode.
 */
class PandaCallMorphFunctionPass : public FunctionPass {
    PandaHelperCallVisitor *PHCV;
public:
    static char ID;
    bool functionChanged; // Return value for runOnFunction()

    PandaCallMorphFunctionPass() :
        FunctionPass(ID),
        PHCV(new PandaHelperCallVisitor(this)),
        functionChanged(false) {}

    ~PandaCallMorphFunctionPass(){
        delete PHCV;
    }

    bool runOnFunction(Function &F);

    void getAnalysisUsage(AnalysisUsage &AU) const {
        // We modify in a non-trivial way, so we do nothing here
    }
};

} // End LLVM namespace

#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the process of including the execution of QEMU helper functions in the
 * LLVM JIT.
 */
void init_llvm_helpers(void);

/*
 * Stop running QEMU helper functions in the JIT.
 */
void uninit_llvm_helpers(void);

#ifdef __cplusplus
}
#endif

#endif

