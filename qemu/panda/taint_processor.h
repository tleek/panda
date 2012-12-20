
/*

  API for Taint processor

*/

#ifndef __TAINT_PROCESSOR_H__
#define __TAINT_PROCESSOR_H__

#include <stdint.h>
#include "shad_dir_32.h"
#include "shad_dir_64.h"

#define EXCEPTIONSTRING "3735928559"  // 0xDEADBEEF read from dynamic log
#define OPNAMELENGTH 15
#define FUNCNAMELENGTH 50
#define FUNCTIONFRAMES 2 // handle 2 frames for now, but increase it soon
#define MAXREGSIZE 16 // Maximum LLVM register size is 16 bytes

//#define TAINTDEBUG // print out all debugging info for taint ops

/*
 * Tainted pointer mode: for now, we will just add additional taint
 * operations if tainted pointer mode is on.  These operations will also
 * perform compute taint from the LLVM register that holds the pointer, to
 * the destination of the load.  It will also preserve any taint in the
 * register that is a result of the load.  Later on, we may want to have
 * multiple tainted pointer modes: a smarter tainted pointer mode might
 * check to see if the data is tainted, and if so, only propagate that
 * taint, even if the pointer is also tainted.  Without tainted data, it
 * would propagate the taint of the pointer, if it exists.  This should be
 * done behind the scenes in the taint processor.
 */
#define TAINTED_POINTER

//#define TAINTSTATS

/* these need to be the same size because when we have an unknown dynamic value
 * that we need to fill in later, we need to fix up the taint op in the buffer
 */
typedef uint64_t HAddr;    // hard drive
typedef uint64_t MAddr;    // physical ram
typedef uint64_t IAddr;    // io buffers (net & hd)
typedef uint64_t LAddr;    // local values
typedef uint64_t GReg;     // guest general purpose register
typedef uint64_t GSpec;    // guest special address, like floating point regs
typedef uint64_t Unk;      // unknown type that needs to be fixed up
typedef uint64_t Const;    // constant that we currently treat as untainted
typedef uint64_t Ret;      // LLVM return value, also temp register

typedef enum {HADDR, MADDR, IADDR, LADDR, GREG, GSPEC, UNK, CONST, RET} AddrType;

typedef enum {
    IRRELEVANT=-1,  // memory access to CPU state we don't care about
    EXCEPTION=1,    // indicates that there was a memory exception
    READLOG,        // indicates that we need to read from dynamic log
    FUNCARG         // indicates that we need to copy to the current frame + 1
} AddrFlag;

typedef enum {
    INSNREADLOG=1 // indicates that we need to read from dynamic log
} InsnFlag;

typedef struct addr_struct {
  AddrType typ;
  union {
    HAddr ha;
    MAddr ma;
    IAddr ia;
    LAddr la;
    GReg gr;
    GSpec gs;
    Unk ua;
    Const con;
    Ret ret;
  } val;
  uint16_t off;   // offset within local registers and guest registers
  AddrFlag flag;  // indication that we might need to look up address from log
} Addr;


typedef uint32_t Label;


typedef struct shad_struct {
  uint64_t hd_size;
  uint32_t mem_size;
  uint64_t io_size;
  uint32_t num_vals;
  uint32_t guest_regs;
  SdDir64 *hd;
#ifdef TARGET_X86_64
  SdDir64 *ram;
#else
  SdDir32 *ram;
#endif
  SdDir64 *io;
  LabelSet **llv;  // LLVM registers, with multiple frames
  LabelSet **ret;  // LLVM return value, also temp register
  LabelSet **grv;  // guest general purpose registers
  LabelSet **gsv;  // guest special values, like FP, and parts of CPUState
  uint8_t *ram_bitmap;
  uint32_t current_frame; // keeps track of current function frame
} Shad;

// returns a shadow memory to be used by taint processor
Shad *tp_init(uint64_t hd_size, uint32_t mem_size, uint64_t io_size, uint32_t max_vals);

// label -- associate label l with address a
void tp_label(Shad *shad, Addr a, Label l); 

// untaint -- discard label set associated with a
void tp_delete(Shad *shad, Addr a); 

// copy -- b gets whatever label set is currently associated with a
void tp_copy(Shad *shad, Addr a, Addr b);

// compute -- c gets union of label sets currently associated with a and b
void tp_compute(Shad *shad, Addr a, Addr b, Addr c);

// query -- returns TRUE (1) iff a is tainted
uint8_t tp_query(Shad *shad, Addr a);

uint8_t addrs_equal(Addr a, Addr b);

uint8_t get_ram_bit(Shad *shad, uint32_t addr);

typedef struct taint_op_buffer_struct {
  char *start;        // beginning of ops
  uint32_t max_size;  // max size
  uint32_t size;      // current size of this buffer in bytes
  char *ptr;          // current location in buf for write / read
} TaintOpBuffer;


/*** taint translation block stuff ***/
/* There are a few different notions of 'blocks'.  A guest basic block is
 * translated into a QEMU translation block.  There may be multiple basic blocks
 * within a translation block since QEMU and LLVM have control flow
 * instructions.  So a taint translation block corresponds to a QEMU translation
 * block, or LLVM function, which may consist of multiple basic blocks.  Every
 * TaintTB has an entry basic block.  Additional basic blocks start at
 * TaintTB.tbbs[0].  LLVM functions from source code also fall into the category
 * of TaintTB.
 */

typedef struct taint_bb_struct {
    int label;           // corresponding LLVM BB label
    TaintOpBuffer *ops;  // taint ops for this taint BB
} TaintBB;

typedef struct taint_tb_struct {
    char *name;      // corresponding name of LLVM function
    int numBBs;      // number of taint BBs
    TaintBB *entry;  // entry taint BB
    TaintBB **tbbs;  // array of other taint BBs
} TaintTB;

TaintTB *taint_tb_new(const char *name, int numBBs);
void taint_tb_cleanup(TaintTB *ttb);

typedef enum {
    LABELOP,
    DELETEOP,
    COPYOP,
    COMPUTEOP, 
    INSNSTARTOP,
    CALLOP,
    RETOP
} TaintOpType;

typedef struct taint_op_struct {
  TaintOpType typ;
  union {
    struct {Addr a; Label l;} label;
    struct {Addr a;} deletel;
    struct {Addr a, b;} copy;
    struct {Addr a, b, c;} compute;
    struct {
        char name[15];
        int num_ops;
        InsnFlag flag;
        // true and false labels when used with branch
        // true and false values when used with select
        int branch_labels[2];
    } insn_start;
    struct {char name[50]; TaintTB *ttb;} call;
    struct {int null; /* data currently not used */} ret;
  } val;
} TaintOp;

#include "panda_memlog.h"

TaintOpBuffer *tob_new(uint32_t size);
void tob_rewind(TaintOpBuffer *buf);
//uint8_t tob_empty(TaintOpBuffer *buf);

// write op to buffer
void tob_op_write(TaintOpBuffer *buf, TaintOp op);

// read op from buffer
TaintOp tob_op_read(TaintOpBuffer *buf);

// execute a function or taint translation block of taint ops
void execute_taint_ops(TaintTB *ttb, Shad *shad, DynValBuffer *dynval_buf);

// process ops in taint op buffer (called by execute)
void tob_process(TaintOpBuffer *buf, Shad *shad, DynValBuffer *dynval_buf);

void tob_op_print(Shad *shad, TaintOp op);

uint8_t tob_end(TaintOpBuffer *buf);

float tob_full_frac(TaintOpBuffer *buf);

void tob_clear(TaintOpBuffer *buf);

// stuff for control flow in trace
enum {RETURN, BRANCH, EXCEPT};

void print_addr(Shad *shad, Addr a);

void process_insn_start_op(TaintOp op, TaintOpBuffer *buf,
    DynValBuffer *dynval_buf);

#endif
