/*
** 2003 September 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used for creating, destroying, and populating
** a VDBE (or an "sqlite4_stmt" as it is known to the outside world.)  Prior
** to version 2.8.7, all this code was combined into the vdbe.c source file.
** But that file was getting too big so this subroutines were split out.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"


/*
** Create a new virtual database engine.
*/
Vdbe *sqlite4VdbeCreate(sqlite4 *db){
  Vdbe *p;
  p = sqlite4DbMallocZero(db, sizeof(Vdbe) );
  if( p==0 ) return 0;
  p->db = db;
  if( db->pVdbe ){
    db->pVdbe->pPrev = p;
  }
  p->pNext = db->pVdbe;
  p->pPrev = 0;
  db->pVdbe = p;
  p->magic = VDBE_MAGIC_INIT;
  return p;
}

/*
** Remember the SQL string for a prepared statement.
*/
void sqlite4VdbeSetSql(Vdbe *p, const char *z, int n){
  if( p==0 ) return;
  assert( p->zSql==0 );
  p->zSql = sqlite4DbStrNDup(p->db, z, n);
}

/*
** Return the SQL associated with a prepared statement
*/
const char *sqlite4_stmt_sql(sqlite4_stmt *pStmt){
  Vdbe *p = (Vdbe *)pStmt;
  return p ? p->zSql : 0;
}

/*
** Swap all content between two VDBE structures.
*/
void sqlite4VdbeSwap(Vdbe *pA, Vdbe *pB){
  Vdbe tmp, *pTmp;
  char *zTmp;
  tmp = *pA;
  *pA = *pB;
  *pB = tmp;
  pTmp = pA->pNext;
  pA->pNext = pB->pNext;
  pB->pNext = pTmp;
  pTmp = pA->pPrev;
  pA->pPrev = pB->pPrev;
  pB->pPrev = pTmp;
  zTmp = pA->zSql;
  pA->zSql = pB->zSql;
  pB->zSql = zTmp;
}

#ifdef SQLITE4_DEBUG
/*
** Turn tracing on or off
*/
void sqlite4VdbeTrace(Vdbe *p, FILE *trace){
  p->trace = trace;
}
#endif

/*
** Resize the Vdbe.aOp array so that it is at least one op larger than 
** it was.
**
** If an out-of-memory error occurs while resizing the array, return
** SQLITE4_NOMEM. In this case Vdbe.aOp and Vdbe.nOpAlloc remain 
** unchanged (this is so that any opcodes already allocated can be 
** correctly deallocated along with the rest of the Vdbe).
*/
static int growOpArray(Vdbe *p){
  VdbeOp *pNew;
  int nNew = (p->nOpAlloc ? p->nOpAlloc*2 : (int)(1024/sizeof(Op)));
  pNew = sqlite4DbRealloc(p->db, p->aOp, nNew*sizeof(Op));
  if( pNew ){
    p->nOpAlloc = sqlite4DbMallocSize(p->db, pNew)/sizeof(Op);
    p->aOp = pNew;
  }
  return (pNew ? SQLITE4_OK : SQLITE4_NOMEM);
}

/*
** Add a new instruction to the list of instructions current in the
** VDBE.  Return the address of the new instruction.
**
** Parameters:
**
**    p               Pointer to the VDBE
**
**    op              The opcode for this instruction
**
**    p1, p2, p3      Operands
**
** Use the sqlite4VdbeResolveLabel() function to fix an address and
** the sqlite4VdbeChangeP4() function to change the value of the P4
** operand.
*/
int sqlite4VdbeAddOp3(Vdbe *p, int op, int p1, int p2, int p3){
  int i;
  VdbeOp *pOp;

  i = p->nOp;
  assert( p->magic==VDBE_MAGIC_INIT );
  assert( op>0 && op<0xff );
  if( p->nOpAlloc<=i ){
    if( growOpArray(p) ){
      return 1;
    }
  }
  p->nOp++;
  pOp = &p->aOp[i];
  pOp->opcode = (u8)op;
  pOp->p5 = 0;
  pOp->p1 = p1;
  pOp->p2 = p2;
  pOp->p3 = p3;
  pOp->p4.p = 0;
  pOp->p4type = P4_NOTUSED;
#ifdef SQLITE4_DEBUG
  pOp->zComment = 0;
  if( p->db->flags & SQLITE4_VdbeAddopTrace ){
    sqlite4VdbePrintOp(0, i, &p->aOp[i]);
  }
#endif
#ifdef VDBE_PROFILE
  pOp->cycles = 0;
  pOp->cnt = 0;
#endif
  return i;
}
int sqlite4VdbeAddOp0(Vdbe *p, int op){
  return sqlite4VdbeAddOp3(p, op, 0, 0, 0);
}
int sqlite4VdbeAddOp1(Vdbe *p, int op, int p1){
  return sqlite4VdbeAddOp3(p, op, p1, 0, 0);
}
int sqlite4VdbeAddOp2(Vdbe *p, int op, int p1, int p2){
  return sqlite4VdbeAddOp3(p, op, p1, p2, 0);
}


/*
** Add an opcode that includes the p4 value as a pointer.
*/
int sqlite4VdbeAddOp4(
  Vdbe *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  const char *zP4,    /* The P4 operand */
  int p4type          /* P4 operand type */
){
  int addr = sqlite4VdbeAddOp3(p, op, p1, p2, p3);
  sqlite4VdbeChangeP4(p, addr, zP4, p4type);
  return addr;
}

/*
** Add an OP_ParseSchema opcode.  This routine is broken out from
** sqlite4VdbeAddOp4() since it needs to also needs to mark all btrees
** as having been used.
**
** The zWhere string must have been obtained from sqlite4_malloc().
** This routine will take ownership of the allocated memory.
*/
void sqlite4VdbeAddParseSchemaOp(Vdbe *p, int iDb, char *zWhere){
  int j;
  int addr = sqlite4VdbeAddOp3(p, OP_ParseSchema, iDb, 0, 0);
  sqlite4VdbeChangeP4(p, addr, zWhere, P4_DYNAMIC);
  for(j=0; j<p->db->nDb; j++) sqlite4VdbeUsesStorage(p, j);
}

/*
** Add an opcode that includes the p4 value as an integer.
*/
int sqlite4VdbeAddOp4Int(
  Vdbe *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  int p4              /* The P4 operand as an integer */
){
  int addr = sqlite4VdbeAddOp3(p, op, p1, p2, p3);
  sqlite4VdbeChangeP4(p, addr, SQLITE4_INT_TO_PTR(p4), P4_INT32);
  return addr;
}

/*
** Create a new symbolic label for an instruction that has yet to be
** coded.  The symbolic label is really just a negative number.  The
** label can be used as the P2 value of an operation.  Later, when
** the label is resolved to a specific address, the VDBE will scan
** through its operation list and change all values of P2 which match
** the label into the resolved address.
**
** The VDBE knows that a P2 value is a label because labels are
** always negative and P2 values are suppose to be non-negative.
** Hence, a negative P2 value is a label that has yet to be resolved.
**
** Zero is returned if a malloc() fails.
*/
int sqlite4VdbeMakeLabel(Vdbe *p){
  int i;
  i = p->nLabel++;
  assert( p->magic==VDBE_MAGIC_INIT );
  if( i>=p->nLabelAlloc ){
    int n = p->nLabelAlloc*2 + 5;
    p->aLabel = sqlite4DbReallocOrFree(p->db, p->aLabel,
                                       n*sizeof(p->aLabel[0]));
    p->nLabelAlloc = sqlite4DbMallocSize(p->db, p->aLabel)/sizeof(p->aLabel[0]);
  }
  if( p->aLabel ){
    p->aLabel[i] = -1;
  }
  return -1-i;
}

/*
** Resolve label "x" to be the address of the next instruction to
** be inserted.  The parameter "x" must have been obtained from
** a prior call to sqlite4VdbeMakeLabel().
*/
void sqlite4VdbeResolveLabel(Vdbe *p, int x){
  int j = -1-x;
  assert( p->magic==VDBE_MAGIC_INIT );
  assert( j>=0 && j<p->nLabel );
  if( p->aLabel ){
    p->aLabel[j] = p->nOp;
  }
}

/*
** Mark the VDBE as one that can only be run one time.
*/
void sqlite4VdbeRunOnlyOnce(Vdbe *p){
  p->runOnlyOnce = 1;
}

#ifdef SQLITE4_DEBUG /* sqlite4AssertMayAbort() logic */

/*
** The following type and function are used to iterate through all opcodes
** in a Vdbe main program and each of the sub-programs (triggers) it may 
** invoke directly or indirectly. It should be used as follows:
**
**   Op *pOp;
**   VdbeOpIter sIter;
**
**   memset(&sIter, 0, sizeof(sIter));
**   sIter.v = v;                            // v is of type Vdbe* 
**   while( (pOp = opIterNext(&sIter)) ){
**     // Do something with pOp
**   }
**   sqlite4DbFree(v->db, sIter.apSub);
** 
*/
typedef struct VdbeOpIter VdbeOpIter;
struct VdbeOpIter {
  Vdbe *v;                   /* Vdbe to iterate through the opcodes of */
  SubProgram **apSub;        /* Array of subprograms */
  int nSub;                  /* Number of entries in apSub */
  int iAddr;                 /* Address of next instruction to return */
  int iSub;                  /* 0 = main program, 1 = first sub-program etc. */
};
static Op *opIterNext(VdbeOpIter *p){
  Vdbe *v = p->v;
  Op *pRet = 0;
  Op *aOp;
  int nOp;

  if( p->iSub<=p->nSub ){

    if( p->iSub==0 ){
      aOp = v->aOp;
      nOp = v->nOp;
    }else{
      aOp = p->apSub[p->iSub-1]->aOp;
      nOp = p->apSub[p->iSub-1]->nOp;
    }
    assert( p->iAddr<nOp );

    pRet = &aOp[p->iAddr];
    p->iAddr++;
    if( p->iAddr==nOp ){
      p->iSub++;
      p->iAddr = 0;
    }
  
    if( pRet->p4type==P4_SUBPROGRAM ){
      int nByte = (p->nSub+1)*sizeof(SubProgram*);
      int j;
      for(j=0; j<p->nSub; j++){
        if( p->apSub[j]==pRet->p4.pProgram ) break;
      }
      if( j==p->nSub ){
        p->apSub = sqlite4DbReallocOrFree(v->db, p->apSub, nByte);
        if( !p->apSub ){
          pRet = 0;
        }else{
          p->apSub[p->nSub++] = pRet->p4.pProgram;
        }
      }
    }
  }

  return pRet;
}

/*
** Check if the program stored in the VM associated with pParse may
** throw an ABORT exception (causing the statement, but not entire transaction
** to be rolled back). This condition is true if the main program or any
** sub-programs contains any of the following:
**
**   *  OP_Halt with P1=SQLITE4_CONSTRAINT and P2=OE_Abort.
**   *  OP_HaltIfNull with P1=SQLITE4_CONSTRAINT and P2=OE_Abort.
**   *  OP_VUpdate
**   *  OP_VRename
**   *  OP_FkCounter with P2==0 (immediate foreign key constraint)
**
** Then check that the value of Parse.mayAbort is true if an
** ABORT may be thrown, or false otherwise. Return true if it does
** match, or false otherwise. This function is intended to be used as
** part of an assert statement in the compiler. Similar to:
**
**   assert( sqlite4VdbeAssertMayAbort(pParse->pVdbe, pParse->mayAbort) );
*/
int sqlite4VdbeAssertMayAbort(Vdbe *v, int mayAbort){
  int hasAbort = 0;
  Op *pOp;
  VdbeOpIter sIter;
  memset(&sIter, 0, sizeof(sIter));
  sIter.v = v;

  while( (pOp = opIterNext(&sIter))!=0 ){
    int opcode = pOp->opcode;
    if( opcode==OP_VUpdate || opcode==OP_VRename 
#ifndef SQLITE4_OMIT_FOREIGN_KEY
     || (opcode==OP_FkCounter && pOp->p1==0 && pOp->p2==1) 
#endif
     || ((opcode==OP_Halt || opcode==OP_HaltIfNull) 
      && (pOp->p1==SQLITE4_CONSTRAINT && pOp->p2==OE_Abort))
    ){
      hasAbort = 1;
      break;
    }
  }
  sqlite4DbFree(v->db, sIter.apSub);

  /* Return true if hasAbort==mayAbort. Or if a malloc failure occured.
  ** If malloc failed, then the while() loop above may not have iterated
  ** through all opcodes and hasAbort may be set incorrectly. Return
  ** true for this case to prevent the assert() in the callers frame
  ** from failing.  */
  return ( v->db->mallocFailed || hasAbort==mayAbort );
}
#endif /* SQLITE4_DEBUG - the sqlite4AssertMayAbort() function */

/*
** Loop through the program looking for P2 values that are negative
** on jump instructions.  Each such value is a label.  Resolve the
** label by setting the P2 value to its correct non-zero value.
**
** This routine is called once after all opcodes have been inserted.
**
** Variable *pMaxFuncArgs is set to the maximum value of any P2 argument 
** to an OP_Function, OP_AggStep or OP_VFilter opcode. This is used by 
** sqlite4VdbeMakeReady() to size the Vdbe.apArg[] array.
**
** The Op.opflags field is set on all opcodes.
*/
static void resolveP2Values(Vdbe *p, int *pMaxFuncArgs){
  int i;
  int nMaxArgs = *pMaxFuncArgs;
  Op *pOp;
  int *aLabel = p->aLabel;
  p->readOnly = 1;
  for(pOp=p->aOp, i=p->nOp-1; i>=0; i--, pOp++){
    u8 opcode = pOp->opcode;

    pOp->opflags = sqlite4OpcodeProperty[opcode];
    if( opcode==OP_Function || opcode==OP_AggStep ){
      if( pOp->p5>nMaxArgs ) nMaxArgs = pOp->p5;
    }else if( (opcode==OP_Transaction && pOp->p2!=0) ){
      p->readOnly = 0;
#ifndef SQLITE4_OMIT_VIRTUALTABLE
    }else if( opcode==OP_VUpdate ){
      if( pOp->p2>nMaxArgs ) nMaxArgs = pOp->p2;
    }else if( opcode==OP_VFilter ){
      int n;
      assert( p->nOp - i >= 3 );
      assert( pOp[-1].opcode==OP_Integer );
      n = pOp[-1].p1;
      if( n>nMaxArgs ) nMaxArgs = n;
#endif
    }else if( opcode==OP_Next || opcode==OP_SorterNext ){
      pOp->p4.xAdvance = sqlite4VdbeNext;
      pOp->p4type = P4_ADVANCE;
    }else if( opcode==OP_Prev ){
      pOp->p4.xAdvance = sqlite4VdbePrevious;
      pOp->p4type = P4_ADVANCE;
    }

    if( (pOp->opflags & OPFLG_JUMP)!=0 && pOp->p2<0 ){
      assert( -1-pOp->p2<p->nLabel );
      pOp->p2 = aLabel[-1-pOp->p2];
    }
  }
  sqlite4DbFree(p->db, p->aLabel);
  p->aLabel = 0;

  *pMaxFuncArgs = nMaxArgs;
}

/*
** Return the address of the next instruction to be inserted.
*/
int sqlite4VdbeCurrentAddr(Vdbe *p){
  assert( p->magic==VDBE_MAGIC_INIT );
  return p->nOp;
}

/*
** This function returns a pointer to the array of opcodes associated with
** the Vdbe passed as the first argument. It is the callers responsibility
** to arrange for the returned array to be eventually freed using the 
** vdbeFreeOpArray() function.
**
** Before returning, *pnOp is set to the number of entries in the returned
** array. Also, *pnMaxArg is set to the larger of its current value and 
** the number of entries in the Vdbe.apArg[] array required to execute the 
** returned program.
*/
VdbeOp *sqlite4VdbeTakeOpArray(Vdbe *p, int *pnOp, int *pnMaxArg){
  VdbeOp *aOp = p->aOp;
  assert( aOp && !p->db->mallocFailed );

  resolveP2Values(p, pnMaxArg);
  *pnOp = p->nOp;
  p->aOp = 0;
  return aOp;
}

/*
** Add a whole list of operations to the operation stack.  Return the
** address of the first operation added.
*/
int sqlite4VdbeAddOpList(Vdbe *p, int nOp, VdbeOpList const *aOp){
  int addr;
  assert( p->magic==VDBE_MAGIC_INIT );
  if( p->nOp + nOp > p->nOpAlloc && growOpArray(p) ){
    return 0;
  }
  addr = p->nOp;
  if( ALWAYS(nOp>0) ){
    int i;
    VdbeOpList const *pIn = aOp;
    for(i=0; i<nOp; i++, pIn++){
      int p2 = pIn->p2;
      VdbeOp *pOut = &p->aOp[i+addr];
      pOut->opcode = pIn->opcode;
      pOut->p1 = pIn->p1;
      if( p2<0 && (sqlite4OpcodeProperty[pOut->opcode] & OPFLG_JUMP)!=0 ){
        pOut->p2 = addr + ADDR(p2);
      }else{
        pOut->p2 = p2;
      }
      pOut->p3 = pIn->p3;
      pOut->p4type = P4_NOTUSED;
      pOut->p4.p = 0;
      pOut->p5 = 0;
#ifdef SQLITE4_DEBUG
      pOut->zComment = 0;
      if( p->db->flags & SQLITE4_VdbeAddopTrace ){
        sqlite4VdbePrintOp(0, i+addr, &p->aOp[i+addr]);
      }
#endif
    }
    p->nOp += nOp;
  }
  return addr;
}

/*
** Change the value of the P1 operand for a specific instruction.
** This routine is useful when a large program is loaded from a
** static array using sqlite4VdbeAddOpList but we want to make a
** few minor changes to the program.
*/
void sqlite4VdbeChangeP1(Vdbe *p, u32 addr, int val){
  assert( p!=0 );
  if( ((u32)p->nOp)>addr ){
    p->aOp[addr].p1 = val;
  }
}

/*
** Change the value of the P2 operand for a specific instruction.
** This routine is useful for setting a jump destination.
*/
void sqlite4VdbeChangeP2(Vdbe *p, u32 addr, int val){
  assert( p!=0 );
  if( ((u32)p->nOp)>addr ){
    p->aOp[addr].p2 = val;
  }
}

/*
** Change the value of the P3 operand for a specific instruction.
*/
void sqlite4VdbeChangeP3(Vdbe *p, u32 addr, int val){
  assert( p!=0 );
  if( ((u32)p->nOp)>addr ){
    p->aOp[addr].p3 = val;
  }
}

/*
** Change the value of the P5 operand for the most recently
** added operation.
*/
void sqlite4VdbeChangeP5(Vdbe *p, u8 val){
  assert( p!=0 );
  if( p->aOp ){
    assert( p->nOp>0 );
    p->aOp[p->nOp-1].p5 = val;
  }
}

/*
** Change the P2 operand of instruction addr so that it points to
** the address of the next instruction to be coded.
*/
void sqlite4VdbeJumpHere(Vdbe *p, int addr){
  assert( addr>=0 || p->db->mallocFailed );
  if( addr>=0 ) sqlite4VdbeChangeP2(p, addr, p->nOp);
}


/*
** If the input FuncDef structure is ephemeral, then free it.  If
** the FuncDef is not ephermal, then do nothing.
*/
static void freeEphemeralFunction(sqlite4 *db, FuncDef *pDef){
  if( ALWAYS(pDef) && (pDef->flags & SQLITE4_FUNC_EPHEM)!=0 ){
    if( pDef->xDestroy ){
      pDef->xDestroy(pDef->pUserData);
    }
    sqlite4DbFree(db, pDef);
  }
}

static void vdbeFreeOpArray(sqlite4 *, Op *, int);

/*
** Delete a P4 value if necessary.
*/
static void freeP4(sqlite4 *db, int p4type, void *p4){
  if( p4 ){
    assert( db );
    switch( p4type ){
      case P4_NUM:
      case P4_DYNAMIC:
      case P4_KEYINFO:
      case P4_INTARRAY:
      case P4_KEYINFO_HANDOFF: {
        sqlite4DbFree(db, p4);
        break;
      }
      case P4_VDBEFUNC: {
        VdbeFunc *pVdbeFunc = (VdbeFunc *)p4;
        freeEphemeralFunction(db, pVdbeFunc->pFunc);
        if( db->pnBytesFreed==0 ) sqlite4VdbeDeleteAuxData(pVdbeFunc, 0);
        sqlite4DbFree(db, pVdbeFunc);
        break;
      }
      case P4_FUNCDEF: {
        freeEphemeralFunction(db, (FuncDef*)p4);
        break;
      }
      case P4_MEM: {
        if( db->pnBytesFreed==0 ){
          sqlite4ValueFree((sqlite4_value*)p4);
        }else{
          Mem *p = (Mem*)p4;
          sqlite4DbFree(db, p->zMalloc);
          sqlite4DbFree(db, p);
        }
        break;
      }
      case P4_VTAB : {
        if( db->pnBytesFreed==0 ) sqlite4VtabUnlock((VTable *)p4);
        break;
      }
      case P4_FTS5INFO : {
        sqlite4Fts5FreeInfo(db, (Fts5Info *)p4);
        break;
      }
    }
  }
}

/*
** Free the space allocated for aOp and any p4 values allocated for the
** opcodes contained within. If aOp is not NULL it is assumed to contain 
** nOp entries. 
*/
static void vdbeFreeOpArray(sqlite4 *db, Op *aOp, int nOp){
  if( aOp ){
    Op *pOp;
    for(pOp=aOp; pOp<&aOp[nOp]; pOp++){
      freeP4(db, pOp->p4type, pOp->p4.p);
#ifdef SQLITE4_DEBUG
      sqlite4DbFree(db, pOp->zComment);
#endif     
    }
  }
  sqlite4DbFree(db, aOp);
}

/*
** Link the SubProgram object passed as the second argument into the linked
** list at Vdbe.pSubProgram. This list is used to delete all sub-program
** objects when the VM is no longer required.
*/
void sqlite4VdbeLinkSubProgram(Vdbe *pVdbe, SubProgram *p){
  p->pNext = pVdbe->pProgram;
  pVdbe->pProgram = p;
}

/*
** Change the opcode at addr into OP_Noop
*/
void sqlite4VdbeChangeToNoop(Vdbe *p, int addr){
  if( p->aOp ){
    VdbeOp *pOp = &p->aOp[addr];
    sqlite4 *db = p->db;
    freeP4(db, pOp->p4type, pOp->p4.p);
    memset(pOp, 0, sizeof(pOp[0]));
    pOp->opcode = OP_Noop;
  }
}

/*
** Change the value of the P4 operand for a specific instruction.
** This routine is useful when a large program is loaded from a
** static array using sqlite4VdbeAddOpList but we want to make a
** few minor changes to the program.
**
** If n>=0 then the P4 operand is dynamic, meaning that a copy of
** the string is made into memory obtained from sqlite4_malloc().
** A value of n==0 means copy bytes of zP4 up to and including the
** first null byte.  If n>0 then copy n+1 bytes of zP4.
**
** If n==P4_KEYINFO it means that zP4 is a pointer to a KeyInfo structure.
** A copy is made of the KeyInfo structure into memory obtained from
** sqlite4_malloc, to be freed when the Vdbe is finalized.
** n==P4_KEYINFO_HANDOFF indicates that zP4 points to a KeyInfo structure
** stored in memory that the caller has obtained from sqlite4_malloc. The 
** caller should not free the allocation, it will be freed when the Vdbe is
** finalized.
** 
** Other values of n (P4_STATIC, P4_COLLSEQ etc.) indicate that zP4 points
** to a string or structure that is guaranteed to exist for the lifetime of
** the Vdbe. In these cases we can just copy the pointer.
**
** If addr<0 then change P4 on the most recently inserted instruction.
*/
void sqlite4VdbeChangeP4(Vdbe *p, int addr, const char *zP4, int n){
  Op *pOp;
  sqlite4 *db;
  assert( p!=0 );
  db = p->db;
  assert( p->magic==VDBE_MAGIC_INIT );
  if( p->aOp==0 || db->mallocFailed ){
    if ( n!=P4_KEYINFO && n!=P4_VTAB ) {
      freeP4(db, n, (void*)*(char**)&zP4);
    }
    return;
  }
  assert( p->nOp>0 );
  assert( addr<p->nOp );
  if( addr<0 ){
    addr = p->nOp - 1;
  }
  pOp = &p->aOp[addr];
  freeP4(db, pOp->p4type, pOp->p4.p);
  pOp->p4.p = 0;
  if( n==P4_INT32 ){
    /* Note: this cast is safe, because the origin data point was an int
    ** that was cast to a (const char *). */
    pOp->p4.i = SQLITE4_PTR_TO_INT(zP4);
    pOp->p4type = P4_INT32;
  }else if( zP4==0 ){
    pOp->p4.p = 0;
    pOp->p4type = P4_NOTUSED;
  }else if( n==P4_KEYINFO ){
    KeyInfo *pKeyInfo;
    int nField, nByte;

    nField = ((KeyInfo*)zP4)->nField;
    nByte = sizeof(*pKeyInfo) + (nField-1)*sizeof(pKeyInfo->aColl[0]) + nField;
    pKeyInfo = sqlite4DbMallocRaw(0, nByte);
    pOp->p4.pKeyInfo = pKeyInfo;
    if( pKeyInfo ){
      u8 *aSortOrder;
      memcpy((char*)pKeyInfo, zP4, nByte - nField);
      aSortOrder = pKeyInfo->aSortOrder;
      if( aSortOrder ){
        pKeyInfo->aSortOrder = (unsigned char*)&pKeyInfo->aColl[nField];
        memcpy(pKeyInfo->aSortOrder, aSortOrder, nField);
      }
      pOp->p4type = P4_KEYINFO;
    }else{
      p->db->mallocFailed = 1;
      pOp->p4type = P4_NOTUSED;
    }
  }else if( n==P4_KEYINFO_HANDOFF ){
    pOp->p4.p = (void*)zP4;
    pOp->p4type = P4_KEYINFO;
  }else if( n==P4_VTAB ){
    pOp->p4.p = (void*)zP4;
    pOp->p4type = P4_VTAB;
    sqlite4VtabLock((VTable *)zP4);
    assert( ((VTable *)zP4)->db==p->db );
  }else if( n<0 ){
    pOp->p4.p = (void*)zP4;
    pOp->p4type = (signed char)n;
  }else{
    if( n==0 ) n = sqlite4Strlen30(zP4);
    pOp->p4.z = sqlite4DbStrNDup(p->db, zP4, n);
    pOp->p4type = P4_DYNAMIC;
  }
}

#ifndef NDEBUG
/*
** Change the comment on the the most recently coded instruction.  Or
** insert a No-op and add the comment to that new instruction.  This
** makes the code easier to read during debugging.  None of this happens
** in a production build.
*/
static void vdbeVComment(Vdbe *p, const char *zFormat, va_list ap){
  assert( p->nOp>0 || p->aOp==0 );
  assert( p->aOp==0 || p->aOp[p->nOp-1].zComment==0 || p->db->mallocFailed );
  if( p->nOp ){
    assert( p->aOp );
    sqlite4DbFree(p->db, p->aOp[p->nOp-1].zComment);
    p->aOp[p->nOp-1].zComment = sqlite4VMPrintf(p->db, zFormat, ap);
  }
}
void sqlite4VdbeComment(Vdbe *p, const char *zFormat, ...){
  va_list ap;
  if( p ){
    va_start(ap, zFormat);
    vdbeVComment(p, zFormat, ap);
    va_end(ap);
  }
}
void sqlite4VdbeNoopComment(Vdbe *p, const char *zFormat, ...){
  va_list ap;
  if( p ){
    sqlite4VdbeAddOp0(p, OP_Noop);
    va_start(ap, zFormat);
    vdbeVComment(p, zFormat, ap);
    va_end(ap);
  }
}
#endif  /* NDEBUG */

/*
** Return the opcode for a given address.  If the address is -1, then
** return the most recently inserted opcode.
**
** If a memory allocation error has occurred prior to the calling of this
** routine, then a pointer to a dummy VdbeOp will be returned.  That opcode
** is readable but not writable, though it is cast to a writable value.
** The return of a dummy opcode allows the call to continue functioning
** after a OOM fault without having to check to see if the return from 
** this routine is a valid pointer.  But because the dummy.opcode is 0,
** dummy will never be written to.  This is verified by code inspection and
** by running with Valgrind.
**
** About the #ifdef SQLITE4_OMIT_TRACE:  Normally, this routine is never called
** unless p->nOp>0.  This is because in the absense of SQLITE4_OMIT_TRACE,
** an OP_Trace instruction is always inserted by sqlite4VdbeGet() as soon as
** a new VDBE is created.  So we are free to set addr to p->nOp-1 without
** having to double-check to make sure that the result is non-negative. But
** if SQLITE4_OMIT_TRACE is defined, the OP_Trace is omitted and we do need to
** check the value of p->nOp-1 before continuing.
*/
VdbeOp *sqlite4VdbeGetOp(Vdbe *p, int addr){
  /* C89 specifies that the constant "dummy" will be initialized to all
  ** zeros, which is correct.  MSVC generates a warning, nevertheless. */
  static VdbeOp dummy;  /* Ignore the MSVC warning about no initializer */
  assert( p->magic==VDBE_MAGIC_INIT );
  if( addr<0 ){
#ifdef SQLITE4_OMIT_TRACE
    if( p->nOp==0 ) return (VdbeOp*)&dummy;
#endif
    addr = p->nOp - 1;
  }
  assert( (addr>=0 && addr<p->nOp) || p->db->mallocFailed );
  if( p->db->mallocFailed ){
    return (VdbeOp*)&dummy;
  }else{
    return &p->aOp[addr];
  }
}

#if !defined(SQLITE4_OMIT_EXPLAIN) || !defined(NDEBUG) \
     || defined(VDBE_PROFILE) || defined(SQLITE4_DEBUG)
/*
** Compute a string that describes the P4 parameter for an opcode.
** Use zTemp for any required temporary buffer space.
*/
static char *displayP4(Op *pOp, char *zTemp, int nTemp){
  char *zP4 = zTemp;
  assert( nTemp>=30 );
  switch( pOp->p4type ){
    case P4_KEYINFO_STATIC:
    case P4_KEYINFO: {
      int i, j;
      KeyInfo *pKeyInfo = pOp->p4.pKeyInfo;
      i = sqlite4_snprintf(zTemp, nTemp,
                 "keyinfo(%d,%d,%d", pKeyInfo->nField, pKeyInfo->nPK,
                 pKeyInfo->nData);
      for(j=0; j<pKeyInfo->nField; j++){
        CollSeq *pColl = pKeyInfo->aColl[j];
        if( pColl ){
          int n = sqlite4Strlen30(pColl->zName);
          if( i+n>nTemp-6 ){
            memcpy(&zTemp[i],",...",4);
            break;
          }
          zTemp[i++] = ',';
          if( pKeyInfo->aSortOrder && pKeyInfo->aSortOrder[j] ){
            zTemp[i++] = '-';
          }
          memcpy(&zTemp[i], pColl->zName,n+1);
          i += n;
        }else if( i+4<nTemp-6 ){
          memcpy(&zTemp[i],",nil",4);
          i += 4;
        }
      }
      zTemp[i++] = ')';
      zTemp[i] = 0;
      assert( i<nTemp );
      break;
    }
    case P4_COLLSEQ: {
      CollSeq *pColl = pOp->p4.pColl;
      sqlite4_snprintf(zTemp, nTemp, "collseq(%.20s)", pColl->zName);
      break;
    }
    case P4_FUNCDEF: {
      FuncDef *pDef = pOp->p4.pFunc;
      sqlite4_snprintf(zTemp, nTemp, "%s(%d)", pDef->zName, pDef->nArg);
      break;
    }
    case P4_INT32: {
      sqlite4_snprintf(zTemp, nTemp, "%d", pOp->p4.i);
      break;
    }
    case P4_NUM: {
      sqlite4_num_to_text(*pOp->p4.pNum, zTemp, 0); 
      break;
    }
    case P4_MEM: {
      Mem *pMem = pOp->p4.pMem;
      if( pMem->flags & MEM_Str ){
        zP4 = pMem->z;
      }else if( pMem->flags & (MEM_Int|MEM_Real) ){
        char aOut[30];
        sqlite4_num_to_text(pMem->u.num, aOut, (pMem->flags & MEM_Real));
        sqlite4_snprintf(zTemp, nTemp, "%s", aOut);
      }else if( pMem->flags & MEM_Null ){
        sqlite4_snprintf(zTemp, nTemp, "NULL");
      }else{
        assert( pMem->flags & MEM_Blob );
        zP4 = "(blob)";
      }
      break;
    }
#ifndef SQLITE4_OMIT_VIRTUALTABLE
    case P4_VTAB: {
      sqlite4_vtab *pVtab = pOp->p4.pVtab->pVtab;
      sqlite4_snprintf(zTemp, nTemp, "vtab:%p:%p", pVtab, pVtab->pModule);
      break;
    }
#endif
    case P4_INTARRAY: {
      int i, j, n, cSep = '(';
      i = sqlite4_snprintf(zTemp, nTemp, "intarray");
      n = pOp->opcode==OP_Permutation ? pOp->p1 : 0;
      for(j=0; j<n; j++){
        i += sqlite4_snprintf(zTemp+i, nTemp-i,"%c%d",cSep, pOp->p4.ai[j]);
        cSep = ',';
      }
      sqlite4_snprintf(zTemp+i, nTemp-i, ")");
      break;
    }
    case P4_SUBPROGRAM: {
      sqlite4_snprintf(zTemp, nTemp, "program");
      break;
    }
    case P4_ADVANCE: {
      zTemp[0] = 0;
      break;
    }
    default: {
      if( pOp->opcode==OP_Blob ){
        int n = pOp->p1*2+2;
        if( n>nTemp-2 ) n = nTemp-2;
        zTemp[0] = 'x';
        zTemp[1] = '\'';
        sqlite4BlobToHex((n-2)/2,pOp->p4.z,zTemp+2);
        zTemp[n] = '\'';
        zTemp[n+1] = 0;
      }else{
        zP4 = pOp->p4.z;
        if( zP4==0 ){
          zP4 = zTemp;
          zTemp[0] = 0;
        }
      }
    }
  }
  assert( zP4!=0 );
  return zP4;
}
#endif

/*
** Declare to the Vdbe that the database at db->aDb[i] is used.
*/
void sqlite4VdbeUsesStorage(Vdbe *p, int i){
  assert( i>=0 && i<p->db->nDb && i<(int)sizeof(yDbMask)*8 );
}


#if defined(VDBE_PROFILE) || defined(SQLITE4_DEBUG)
/*
** Print a single opcode.  This routine is used for debugging only.
*/
void sqlite4VdbePrintOp(FILE *pOut, int pc, Op *pOp){
  char *zP4;
  char zPtr[150];
  static const char *zFormat1 = "%4d %-13s %4d %4d %4d %-4s %.2X %s\n";
  if( pOut==0 ) pOut = stdout;
  zP4 = displayP4(pOp, zPtr, sizeof(zPtr));
  fprintf(pOut, zFormat1, pc, 
      sqlite4OpcodeName(pOp->opcode), pOp->p1, pOp->p2, pOp->p3, zP4, pOp->p5,
#ifdef SQLITE4_DEBUG
      pOp->zComment ? pOp->zComment : ""
#else
      ""
#endif
  );
  fflush(pOut);
}
#endif

/*
** Release an array of N Mem elements
*/
static void releaseMemArray(Mem *p, int N){
  if( p && N ){
    Mem *pEnd;
    sqlite4 *db = p->db;
    u8 malloc_failed = db->mallocFailed;
    if( db->pnBytesFreed ){
      for(pEnd=&p[N]; p<pEnd; p++){
        sqlite4DbFree(db, p->zMalloc);
      }
      return;
    }
    for(pEnd=&p[N]; p<pEnd; p++){
      assert( (&p[1])==pEnd || p[0].db==p[1].db );

      /* This block is really an inlined version of sqlite4VdbeMemRelease()
      ** that takes advantage of the fact that the memory cell value is 
      ** being set to NULL after releasing any dynamic resources.
      **
      ** The justification for duplicating code is that according to 
      ** callgrind, this causes a certain test case to hit the CPU 4.7 
      ** percent less (x86 linux, gcc version 4.1.2, -O6) than if 
      ** sqlite4MemRelease() were called from here. With -O2, this jumps
      ** to 6.6 percent. The test case is inserting 1000 rows into a table 
      ** with no indexes using a single prepared INSERT statement, bind() 
      ** and reset(). Inserts are grouped into a transaction.
      */
      if( p->flags&(MEM_Agg|MEM_Dyn|MEM_Frame|MEM_RowSet) ){
        sqlite4VdbeMemRelease(p);
      }else if( p->zMalloc ){
        sqlite4DbFree(db, p->zMalloc);
        p->zMalloc = 0;
      }

      p->flags = MEM_Invalid;
    }
    db->mallocFailed = malloc_failed;
  }
}

/*
** Delete a VdbeFrame object and its contents. VdbeFrame objects are
** allocated by the OP_Program opcode in sqlite4VdbeExec().
*/
void sqlite4VdbeFrameDelete(VdbeFrame *p){
  int i;
  Mem *aMem = VdbeFrameMem(p);
  VdbeCursor **apCsr = (VdbeCursor **)&aMem[p->nChildMem];
  for(i=0; i<p->nChildCsr; i++){
    sqlite4VdbeFreeCursor(apCsr[i]);
  }
  releaseMemArray(aMem, p->nChildMem);
  sqlite4DbFree(p->v->db, p);
}

#ifndef SQLITE4_OMIT_EXPLAIN
/*
** Give a listing of the program in the virtual machine.
**
** The interface is the same as sqlite4VdbeExec().  But instead of
** running the code, it invokes the callback once for each instruction.
** This feature is used to implement "EXPLAIN".
**
** When p->explain==1, each instruction is listed.  When
** p->explain==2, only OP_Explain instructions are listed and these
** are shown in a different format.  p->explain==2 is used to implement
** EXPLAIN QUERY PLAN.
**
** When p->explain==1, first the main program is listed, then each of
** the trigger subprograms are listed one by one.
*/
int sqlite4VdbeList(
  Vdbe *p                   /* The VDBE */
){
  int nRow;                            /* Stop when row count reaches this */
  int nSub = 0;                        /* Number of sub-vdbes seen so far */
  SubProgram **apSub = 0;              /* Array of sub-vdbes */
  Mem *pSub = 0;                       /* Memory cell hold array of subprogs */
  sqlite4 *db = p->db;                 /* The database connection */
  int i;                               /* Loop counter */
  int rc = SQLITE4_OK;                  /* Return code */
  Mem *pMem = &p->aMem[1];             /* First Mem of result set */

  assert( p->explain );
  assert( p->magic==VDBE_MAGIC_RUN );
  assert( p->rc==SQLITE4_OK || p->rc==SQLITE4_BUSY || p->rc==SQLITE4_NOMEM );

  /* Even though this opcode does not use dynamic strings for
  ** the result, result columns may become dynamic if the user calls
  ** sqlite4_column_text16(), causing a translation to UTF-16 encoding.
  */
  releaseMemArray(pMem, 8);
  p->pResultSet = 0;

  if( p->rc==SQLITE4_NOMEM ){
    /* This happens if a malloc() inside a call to sqlite4_column_text() or
    ** sqlite4_column_text16() failed.  */
    db->mallocFailed = 1;
    return SQLITE4_ERROR;
  }

  /* When the number of output rows reaches nRow, that means the
  ** listing has finished and sqlite4_step() should return SQLITE4_DONE.
  ** nRow is the sum of the number of rows in the main program, plus
  ** the sum of the number of rows in all trigger subprograms encountered
  ** so far.  The nRow value will increase as new trigger subprograms are
  ** encountered, but p->pc will eventually catch up to nRow.
  */
  nRow = p->nOp;
  if( p->explain==1 ){
    /* The first 8 memory cells are used for the result set.  So we will
    ** commandeer the 9th cell to use as storage for an array of pointers
    ** to trigger subprograms.  The VDBE is guaranteed to have at least 9
    ** cells.  */
    assert( p->nMem>9 );
    pSub = &p->aMem[9];
    if( pSub->flags&MEM_Blob ){
      /* On the first call to sqlite4_step(), pSub will hold a NULL.  It is
      ** initialized to a BLOB by the P4_SUBPROGRAM processing logic below */
      nSub = pSub->n/sizeof(Vdbe*);
      apSub = (SubProgram **)pSub->z;
    }
    for(i=0; i<nSub; i++){
      nRow += apSub[i]->nOp;
    }
  }

  do{
    i = p->pc++;
  }while( i<nRow && p->explain==2 && p->aOp[i].opcode!=OP_Explain );
  if( i>=nRow ){
    p->rc = SQLITE4_OK;
    rc = SQLITE4_DONE;
  }else if( db->u1.isInterrupted ){
    p->rc = SQLITE4_INTERRUPT;
    rc = SQLITE4_ERROR;
    sqlite4SetString(&p->zErrMsg, db, "%s", sqlite4ErrStr(p->rc));
  }else{
    char *z;
    Op *pOp;
    if( i<p->nOp ){
      /* The output line number is small enough that we are still in the
      ** main program. */
      pOp = &p->aOp[i];
    }else{
      /* We are currently listing subprograms.  Figure out which one and
      ** pick up the appropriate opcode. */
      int j;
      i -= p->nOp;
      for(j=0; i>=apSub[j]->nOp; j++){
        i -= apSub[j]->nOp;
      }
      pOp = &apSub[j]->aOp[i];
    }
    if( p->explain==1 ){
      pMem->flags = MEM_Int;
      pMem->type = SQLITE4_INTEGER;
      pMem->u.num = sqlite4_num_from_int64(i);             /* Program counter */
      pMem++;
  
      pMem->flags = MEM_Static|MEM_Str|MEM_Term;
      pMem->z = (char*)sqlite4OpcodeName(pOp->opcode);     /* Opcode */
      assert( pMem->z!=0 );
      pMem->n = sqlite4Strlen30(pMem->z);
      pMem->type = SQLITE4_TEXT;
      pMem->enc = SQLITE4_UTF8;
      pMem++;

      /* When an OP_Program opcode is encounter (the only opcode that has
      ** a P4_SUBPROGRAM argument), expand the size of the array of subprograms
      ** kept in p->aMem[9].z to hold the new program - assuming this subprogram
      ** has not already been seen.
      */
      if( pOp->p4type==P4_SUBPROGRAM ){
        int nByte = (nSub+1)*sizeof(SubProgram*);
        int j;
        for(j=0; j<nSub; j++){
          if( apSub[j]==pOp->p4.pProgram ) break;
        }
        if( j==nSub && SQLITE4_OK==sqlite4VdbeMemGrow(pSub, nByte, 1) ){
          apSub = (SubProgram **)pSub->z;
          apSub[nSub++] = pOp->p4.pProgram;
          pSub->flags |= MEM_Blob;
          pSub->n = nSub*sizeof(SubProgram*);
        }
      }
    }

    pMem->flags = MEM_Int;
    pMem->u.num = sqlite4_num_from_int64(pOp->p1);         /* P1 */
    pMem->type = SQLITE4_INTEGER;
    pMem++;

    pMem->flags = MEM_Int;
    pMem->u.num = sqlite4_num_from_int64(pOp->p2);         /* P2 */
    pMem->type = SQLITE4_INTEGER;
    pMem++;

    pMem->flags = MEM_Int;
    pMem->u.num = sqlite4_num_from_int64(pOp->p3);         /* P3 */
    pMem->type = SQLITE4_INTEGER;
    pMem++;

    if( sqlite4VdbeMemGrow(pMem, 150, 0) ){                /* P4 */
      assert( p->db->mallocFailed );
      return SQLITE4_ERROR;
    }
    pMem->flags = MEM_Dyn|MEM_Str|MEM_Term;
    z = displayP4(pOp, pMem->z, 150);
    if( z!=pMem->z ){
      sqlite4VdbeMemSetStr(pMem, z, -1, SQLITE4_UTF8, 0, 0);
    }else{
      assert( pMem->z!=0 );
      pMem->n = sqlite4Strlen30(pMem->z);
      pMem->enc = SQLITE4_UTF8;
    }
    pMem->type = SQLITE4_TEXT;
    pMem++;

    if( p->explain==1 ){
      if( sqlite4VdbeMemGrow(pMem, 4, 0) ){
        assert( p->db->mallocFailed );
        return SQLITE4_ERROR;
      }
      pMem->flags = MEM_Dyn|MEM_Str|MEM_Term;
      pMem->n = 2;
      sqlite4_snprintf(pMem->z, 3, "%.2x", pOp->p5);   /* P5 */
      pMem->type = SQLITE4_TEXT;
      pMem->enc = SQLITE4_UTF8;
      pMem++;
  
#ifdef SQLITE4_DEBUG
      if( pOp->zComment ){
        pMem->flags = MEM_Str|MEM_Term;
        pMem->z = pOp->zComment;
        pMem->n = sqlite4Strlen30(pMem->z);
        pMem->enc = SQLITE4_UTF8;
        pMem->type = SQLITE4_TEXT;
      }else
#endif
      {
        pMem->flags = MEM_Null;                       /* Comment */
        pMem->type = SQLITE4_NULL;
      }
    }

    p->nResColumn = 8 - 4*(p->explain-1);
    p->pResultSet = &p->aMem[1];
    p->rc = SQLITE4_OK;
    rc = SQLITE4_ROW;
  }
  return rc;
}
#endif /* SQLITE4_OMIT_EXPLAIN */

#ifdef SQLITE4_DEBUG
/*
** Print the SQL that was used to generate a VDBE program.
*/
void sqlite4VdbePrintSql(Vdbe *p){
  int nOp = p->nOp;
  VdbeOp *pOp;
  if( nOp<1 ) return;
  pOp = &p->aOp[0];
  if( pOp->opcode==OP_Trace && pOp->p4.z!=0 ){
    const char *z = pOp->p4.z;
    while( sqlite4Isspace(*z) ) z++;
    printf("SQL: [%s]\n", z);
  }
}
#endif

#if !defined(SQLITE4_OMIT_TRACE) && defined(SQLITE4_ENABLE_IOTRACE)
/*
** Print an IOTRACE message showing SQL content.
*/
void sqlite4VdbeIOTraceSql(Vdbe *p){
  int nOp = p->nOp;
  VdbeOp *pOp;
  if( sqlite4IoTrace==0 ) return;
  if( nOp<1 ) return;
  pOp = &p->aOp[0];
  if( pOp->opcode==OP_Trace && pOp->p4.z!=0 ){
    int i, j;
    char z[1000];
    sqlite4_snprintf(z, sizeof(z), "%s", pOp->p4.z);
    for(i=0; sqlite4Isspace(z[i]); i++){}
    for(j=0; z[i]; i++){
      if( sqlite4Isspace(z[i]) ){
        if( z[i-1]!=' ' ){
          z[j++] = ' ';
        }
      }else{
        z[j++] = z[i];
      }
    }
    z[j] = 0;
    sqlite4IoTrace("SQL %s\n", z);
  }
}
#endif /* !SQLITE4_OMIT_TRACE && SQLITE4_ENABLE_IOTRACE */

/*
** Allocate space from a fixed size buffer and return a pointer to
** that space.  If insufficient space is available, return NULL.
**
** The pBuf parameter is the initial value of a pointer which will
** receive the new memory.  pBuf is normally NULL.  If pBuf is not
** NULL, it means that memory space has already been allocated and that
** this routine should not allocate any new memory.  When pBuf is not
** NULL simply return pBuf.  Only allocate new memory space when pBuf
** is NULL.
**
** nByte is the number of bytes of space needed.
**
** *ppFrom points to available space and pEnd points to the end of the
** available space.  When space is allocated, *ppFrom is advanced past
** the end of the allocated space.
**
** *pnByte is a counter of the number of bytes of space that have failed
** to allocate.  If there is insufficient space in *ppFrom to satisfy the
** request, then increment *pnByte by the amount of the request.
*/
static void *allocSpace(
  void *pBuf,          /* Where return pointer will be stored */
  int nByte,           /* Number of bytes to allocate */
  u8 **ppFrom,         /* IN/OUT: Allocate from *ppFrom */
  u8 *pEnd,            /* Pointer to 1 byte past the end of *ppFrom buffer */
  int *pnByte          /* If allocation cannot be made, increment *pnByte */
){
  assert( EIGHT_BYTE_ALIGNMENT(*ppFrom) );
  if( pBuf ) return pBuf;
  nByte = ROUND8(nByte);
  if( &(*ppFrom)[nByte] <= pEnd ){
    pBuf = (void*)*ppFrom;
    *ppFrom += nByte;
  }else{
    *pnByte += nByte;
  }
  return pBuf;
}

/*
** Rewind the VDBE back to the beginning in preparation for
** running it.
*/
void sqlite4VdbeRewind(Vdbe *p){
#if defined(SQLITE4_DEBUG) || defined(VDBE_PROFILE)
  int i;
#endif
  assert( p!=0 );
  assert( p->magic==VDBE_MAGIC_INIT );

  /* There should be at least one opcode.
  */
  assert( p->nOp>0 );

  /* Set the magic to VDBE_MAGIC_RUN sooner rather than later. */
  p->magic = VDBE_MAGIC_RUN;

#ifdef SQLITE4_DEBUG
  for(i=1; i<p->nMem; i++){
    assert( p->aMem[i].db==p->db );
  }
#endif
  p->pc = -1;
  p->rc = SQLITE4_OK;
  p->errorAction = OE_Abort;
  p->magic = VDBE_MAGIC_RUN;
  p->nChange = 0;
  p->cacheCtr = 1;
  p->minWriteFileFormat = 255;
  p->stmtTransMask = 0;
  p->nFkConstraint = 0;
#ifdef VDBE_PROFILE
  for(i=0; i<p->nOp; i++){
    p->aOp[i].cnt = 0;
    p->aOp[i].cycles = 0;
  }
#endif
}

/*
** Prepare a virtual machine for execution for the first time after
** creating the virtual machine.  This involves things such
** as allocating stack space and initializing the program counter.
** After the VDBE has be prepped, it can be executed by one or more
** calls to sqlite4VdbeExec().  
**
** This function may be called exact once on a each virtual machine.
** After this routine is called the VM has been "packaged" and is ready
** to run.  After this routine is called, futher calls to 
** sqlite4VdbeAddOp() functions are prohibited.  This routine disconnects
** the Vdbe from the Parse object that helped generate it so that the
** the Vdbe becomes an independent entity and the Parse object can be
** destroyed.
**
** Use the sqlite4VdbeRewind() procedure to restore a virtual machine back
** to its initial state after it has been run.
*/
void sqlite4VdbeMakeReady(
  Vdbe *p,                       /* The VDBE */
  Parse *pParse                  /* Parsing context */
){
  sqlite4 *db;                   /* The database connection */
  int nVar;                      /* Number of parameters */
  int nMem;                      /* Number of VM memory registers */
  int nCursor;                   /* Number of cursors required */
  int nArg;                      /* Number of arguments in subprograms */
  int nOnce;                     /* Number of OP_Once instructions */
  int n;                         /* Loop counter */
  u8 *zCsr;                      /* Memory available for allocation */
  u8 *zEnd;                      /* First byte past allocated memory */
  int nByte;                     /* How much extra memory is needed */

  assert( p!=0 );
  assert( p->nOp>0 );
  assert( pParse!=0 );
  assert( p->magic==VDBE_MAGIC_INIT );
  db = p->db;
  assert( db->mallocFailed==0 );
  nVar = pParse->nVar;
  nMem = pParse->nMem;
  nCursor = pParse->nTab;
  nArg = pParse->nMaxArg;
  nOnce = pParse->nOnce;
  if( nOnce==0 ) nOnce = 1; /* Ensure at least one byte in p->aOnceFlag[] */
  
  /* For each cursor required, also allocate a memory cell. Memory
  ** cells (nMem+1-nCursor)..nMem, inclusive, will never be used by
  ** the vdbe program. Instead they are used to allocate space for
  ** VdbeCursor/BtCursor structures. The blob of memory associated with 
  ** cursor 0 is stored in memory cell nMem. Memory cell (nMem-1)
  ** stores the blob of memory associated with cursor 1, etc.
  **
  ** See also: allocateCursor().
  */
  nMem += nCursor;

  /* Allocate space for memory registers, SQL variables, VDBE cursors and 
  ** an array to marshal SQL function arguments in.
  */
  zCsr = (u8*)&p->aOp[p->nOp];       /* Memory avaliable for allocation */
  zEnd = (u8*)&p->aOp[p->nOpAlloc];  /* First byte past end of zCsr[] */

  resolveP2Values(p, &nArg);
  p->needSavepoint = (u8)(pParse->isMultiWrite && pParse->mayAbort);
  if( pParse->explain && nMem<10 ){
    nMem = 10;
  }
  memset(zCsr, 0, zEnd-zCsr);
  zCsr += (zCsr - (u8*)0)&7;
  assert( EIGHT_BYTE_ALIGNMENT(zCsr) );
  p->expired = 0;

  /* Memory for registers, parameters, cursor, etc, is allocated in two
  ** passes.  On the first pass, we try to reuse unused space at the 
  ** end of the opcode array.  If we are unable to satisfy all memory
  ** requirements by reusing the opcode array tail, then the second
  ** pass will fill in the rest using a fresh allocation.  
  **
  ** This two-pass approach that reuses as much memory as possible from
  ** the leftover space at the end of the opcode array can significantly
  ** reduce the amount of memory held by a prepared statement.
  */
  do {
    nByte = 0;
    p->aMem = allocSpace(p->aMem, nMem*sizeof(Mem), &zCsr, zEnd, &nByte);
    p->aVar = allocSpace(p->aVar, nVar*sizeof(Mem), &zCsr, zEnd, &nByte);
    p->apArg = allocSpace(p->apArg, nArg*sizeof(Mem*), &zCsr, zEnd, &nByte);
    p->azVar = allocSpace(p->azVar, nVar*sizeof(char*), &zCsr, zEnd, &nByte);
    p->apCsr = allocSpace(p->apCsr, nCursor*sizeof(VdbeCursor*),
                          &zCsr, zEnd, &nByte);
    p->aOnceFlag = allocSpace(p->aOnceFlag, nOnce, &zCsr, zEnd, &nByte);
    if( nByte ){
      p->pFree = sqlite4DbMallocZero(db, nByte);
    }
    zCsr = p->pFree;
    zEnd = &zCsr[nByte];
  }while( nByte && !db->mallocFailed );

  p->nCursor = (u16)nCursor;
  p->nOnceFlag = nOnce;
  if( p->aVar ){
    p->nVar = (ynVar)nVar;
    for(n=0; n<nVar; n++){
      p->aVar[n].flags = MEM_Null;
      p->aVar[n].db = db;
    }
  }
  if( p->azVar ){
    p->nzVar = pParse->nzVar;
    memcpy(p->azVar, pParse->azVar, p->nzVar*sizeof(p->azVar[0]));
    memset(pParse->azVar, 0, pParse->nzVar*sizeof(pParse->azVar[0]));
  }
  if( p->aMem ){
    p->aMem--;                      /* aMem[] goes from 1..nMem */
    p->nMem = nMem;                 /*       not from 0..nMem-1 */
    for(n=1; n<=nMem; n++){
      p->aMem[n].flags = MEM_Invalid;
      p->aMem[n].db = db;
    }
  }
  p->explain = pParse->explain;
  sqlite4VdbeRewind(p);
}

/*
** Copy the values stored in the VdbeFrame structure to its Vdbe. This
** is used, for example, when a trigger sub-program is halted to restore
** control to the main program.
*/
int sqlite4VdbeFrameRestore(VdbeFrame *pFrame){
  Vdbe *v = pFrame->v;
  v->aOnceFlag = pFrame->aOnceFlag;
  v->nOnceFlag = pFrame->nOnceFlag;
  v->aOp = pFrame->aOp;
  v->nOp = pFrame->nOp;
  v->aMem = pFrame->aMem;
  v->nMem = pFrame->nMem;
  v->apCsr = pFrame->apCsr;
  v->nCursor = pFrame->nCursor;
  v->nChange = pFrame->nChange;
  return pFrame->pc;
}

/*
** Close all cursors.
**
** Also release any dynamic memory held by the VM in the Vdbe.aMem memory 
** cell array. This is necessary as the memory cell array may contain
** pointers to VdbeFrame objects, which may in turn contain pointers to
** open cursors.
*/
static void closeAllCursors(Vdbe *p){
  if( p->pFrame ){
    VdbeFrame *pFrame;
    for(pFrame=p->pFrame; pFrame->pParent; pFrame=pFrame->pParent);
    sqlite4VdbeFrameRestore(pFrame);
  }
  p->pFrame = 0;
  p->nFrame = 0;

  if( p->apCsr ){
    int i;
    for(i=0; i<p->nCursor; i++){
      VdbeCursor *pC = p->apCsr[i];
      if( pC ){
        sqlite4VdbeFreeCursor(pC);
        p->apCsr[i] = 0;
      }
    }
  }
  if( p->aMem ){
    releaseMemArray(&p->aMem[1], p->nMem);
  }
  while( p->pDelFrame ){
    VdbeFrame *pDel = p->pDelFrame;
    p->pDelFrame = pDel->pParent;
    sqlite4VdbeFrameDelete(pDel);
  }
}

/*
** Clean up the VM after execution.
**
** This routine will automatically close any cursors, lists, and/or
** sorters that were left open.
*/
static void Cleanup(Vdbe *p){
  sqlite4 *db = p->db;

#ifdef SQLITE4_DEBUG
  /* Execute assert() statements to ensure that the Vdbe.apCsr[] and 
  ** Vdbe.aMem[] arrays have already been cleaned up.  */
  int i;
  if( p->apCsr ) for(i=0; i<p->nCursor; i++) assert( p->apCsr[i]==0 );
  if( p->aMem ){
    for(i=1; i<=p->nMem; i++) assert( p->aMem[i].flags==MEM_Invalid );
  }
#endif

  sqlite4DbFree(db, p->zErrMsg);
  p->zErrMsg = 0;
  p->pResultSet = 0;
}

/*
** Set the number of result columns that will be returned by this SQL
** statement. This is now set at compile time, rather than during
** execution of the vdbe program so that sqlite4_column_count() can
** be called on an SQL statement before sqlite4_step().
*/
void sqlite4VdbeSetNumCols(Vdbe *p, int nResColumn){
  Mem *pColName;
  int n;
  sqlite4 *db = p->db;

  releaseMemArray(p->aColName, p->nResColumn*COLNAME_N);
  sqlite4DbFree(db, p->aColName);
  n = nResColumn*COLNAME_N;
  p->nResColumn = (u16)nResColumn;
  p->aColName = pColName = (Mem*)sqlite4DbMallocZero(db, sizeof(Mem)*n );
  if( p->aColName==0 ) return;
  while( n-- > 0 ){
    pColName->flags = MEM_Null;
    pColName->db = p->db;
    pColName++;
  }
}

/*
** Set the name of the idx'th column to be returned by the SQL statement.
** zName must be a pointer to a nul terminated string.
**
** This call must be made after a call to sqlite4VdbeSetNumCols().
**
** The final parameter, xDel, must be one of SQLITE4_DYNAMIC, SQLITE4_STATIC
** or SQLITE4_TRANSIENT. If it is SQLITE4_DYNAMIC, then the buffer pointed
** to by zName will be freed by sqlite4DbFree() when the vdbe is destroyed.
*/
int sqlite4VdbeSetColName(
  Vdbe *p,                         /* Vdbe being configured */
  int idx,                         /* Index of column zName applies to */
  int var,                         /* One of the COLNAME_* constants */
  const char *zName,               /* Pointer to buffer containing name */
  void (*xDel)(void*,void*)        /* Memory management strategy for zName */
){
  int rc;
  Mem *pColName;
  assert( idx<p->nResColumn );
  assert( var<COLNAME_N );
  assert( xDel==SQLITE4_STATIC || xDel==SQLITE4_TRANSIENT
             || xDel==SQLITE4_DYNAMIC );
  if( p->db->mallocFailed ){
    assert( !zName || xDel!=SQLITE4_DYNAMIC );
    return SQLITE4_NOMEM;
  }
  assert( p->aColName!=0 );
  pColName = &(p->aColName[idx+var*p->nResColumn]);
  rc = sqlite4VdbeMemSetStr(pColName, zName, -1, SQLITE4_UTF8, xDel, 0);
  assert( rc!=0 || !zName || (pColName->flags&MEM_Term)!=0 );
  return rc;
}

/*
** Free all Savepoint structures that correspond to transaction levels
** larger than iLevel. Passing iLevel==1 deletes all Savepoint structures.
** iLevel==2 deletes all except for the outermost. And so on.
*/
static void freeSavepoints(sqlite4 *db, int iLevel){
  if( iLevel<1 ) iLevel = 1;
  while( (iLevel-1)<db->nSavepoint ){
    Savepoint *pDel = db->pSavepoint;
    db->pSavepoint = pDel->pNext;
    db->nSavepoint--;
    sqlite4DbFree(db, pDel);
  }
}

#ifdef SQLITE4_DEBUG
static int countSavepoints(sqlite4 *db){
  int nRet = 0;
  Savepoint *p;
  for(p=db->pSavepoint; p; p=p->pNext) nRet++;
  return nRet;
}
#endif

/*
** Rollback to transaction level iLevel. iLevel is as defined by the kv-store
** layer. For example, if the user has done:
**
**   BEGIN;
**     write 1;
**       SAVEPOINT one;
**         write 2;
**           SAVEPOINT two;
**             write 3;
**
** then:
**
**   iLevel==1     Roll back and close top-level transaction.
**   iLevel==2     Roll back top-level transaction. Transaction remains open.
**   iLevel==3     Roll back to just after "write 1". Savepoint "one" remains
**                 open. Savepoint "two" is closed.
**   iLevel==4     Roll back to just after "write 2". Both savepoints remain
**                 open (but "write 3" has been backed out).
*/
int sqlite4VdbeRollback(sqlite4 *db, int iLevel){
  int i;
  assert( sqlite4_mutex_held(db->mutex) );
  assert( db->nSavepoint==countSavepoints(db) );

  /* Invoke the xRollback() hook on all backends. */
  sqlite4BeginBenignMalloc(db->pEnv);
  for(i=0; i<db->nDb; i++){
    KVStore *pKV = db->aDb[i].pKV;
    if( pKV && pKV->iTransLevel>=iLevel ){
      sqlite4KVStoreRollback(pKV, iLevel);
    }
  }
  sqlite4EndBenignMalloc(db->pEnv);

  /* If the InternChanges flag is set, expire prepared statements and
  ** reload the schema. If this is not a rollback of the top-level 
  ** transaction, do not clear the SQLITE4_InternChanges flag.  */ 
  if( db->flags&SQLITE4_InternChanges ){
    sqlite4ExpirePreparedStatements(db);
    sqlite4ResetInternalSchema(db, -1);
    if( iLevel>2 ) db->flags |= SQLITE4_InternChanges;
  }

  /* Free elements from the db->pSavepoint list. Restore the deferred FK
  ** constraint counter to the value consistent with the restored database
  ** state.  */
  freeSavepoints(db, iLevel);
  db->nDeferredCons = (db->pSavepoint ? db->pSavepoint->nDeferredCons : 0);
  assert( db->nSavepoint==countSavepoints(db) );

  return SQLITE4_OK;
}

/*
** Commit to transaction level iLevel.
*/
int sqlite4VdbeCommit(sqlite4 *db, int iLevel){
  int rc = SQLITE4_OK;
  int i;
  assert( sqlite4_mutex_held(db->mutex) );
  assert( db->nSavepoint==countSavepoints(db) );
  assert( iLevel>1 || db->nDeferredCons==0 );

  /* Invoke the xCommit() hook on all backends. */
  for(i=0; rc==SQLITE4_OK && i<db->nDb; i++){
    KVStore *pKV = db->aDb[i].pKV;
    if( pKV && pKV->iTransLevel>iLevel ){
      rc = sqlite4KVStoreCommit(pKV, iLevel);
    }
  }

  if( rc!=SQLITE4_OK ){
    sqlite4VdbeRollback(db, 1);
  }else{
    freeSavepoints(db, iLevel);
  }
  assert( db->nSavepoint==countSavepoints(db) );
  return rc;
}

static int vdbeCloseStatement(Vdbe *p, int bRollback){
  int i;
  int rc = SQLITE4_OK;
  sqlite4 *db = p->db;

  for(i=0; rc==SQLITE4_OK && i<db->nDb; i++){
    if( p->stmtTransMask & ((yDbMask)1)<<i ){
      KVStore *pKV = db->aDb[i].pKV;
      assert( pKV->iTransLevel>2 );
      if( bRollback ){
        rc = sqlite4KVStoreRollback(pKV, pKV->iTransLevel);
      }
      if( rc==SQLITE4_OK ){
        rc = sqlite4KVStoreCommit(pKV, pKV->iTransLevel-1);
      }
    }
  }

  if( bRollback ){
    p->db->nDeferredCons = p->nStmtDefCons;
  }

  if( rc ){
    sqlite4VdbeRollback(db, 1);
  }
  return rc;
}

/* 
** This routine checks that the sqlite4.activeVdbeCnt count variable
** matches the number of vdbe's in the list sqlite4.pVdbe that are
** currently active. An assertion fails if the two counts do not match.
** This is an internal self-check only - it is not an essential processing
** step.
**
** This is a no-op if NDEBUG is defined.
*/
#ifndef NDEBUG
static void checkActiveVdbeCnt(sqlite4 *db){
  Vdbe *p;
  int cnt = 0;
  int nWrite = 0;
  p = db->pVdbe;
  while( p ){
    if( p->magic==VDBE_MAGIC_RUN && p->pc>=0 ){
      cnt++;
      if( p->readOnly==0 ) nWrite++;
    }
    p = p->pNext;
  }
  assert( cnt==db->activeVdbeCnt );
  assert( nWrite==db->writeVdbeCnt );
}
#else
#define checkActiveVdbeCnt(x)
#endif

/*
** If the Vdbe passed as the first argument opened a statement-transaction,
** close it now. Argument eOp must be either SAVEPOINT_ROLLBACK or
** SAVEPOINT_RELEASE. If it is SAVEPOINT_ROLLBACK, then the statement
** transaction is rolled back. If eOp is SAVEPOINT_RELEASE, then the 
** statement transaction is commtted.
**
** If an IO error occurs, an SQLITE4_IOERR_XXX error code is returned. 
** Otherwise SQLITE4_OK.
*/
int sqlite4VdbeCloseStatement(Vdbe *p, int eOp){
  return SQLITE4_OK;
}



/*
** This function is called when a transaction opened by the database 
** handle associated with the VM passed as an argument is about to be 
** committed. If there are outstanding deferred foreign key constraint
** violations, return SQLITE4_ERROR. Otherwise, SQLITE4_OK.
**
** If there are outstanding FK violations and this function returns 
** SQLITE4_ERROR, set the result of the VM to SQLITE4_CONSTRAINT and write
** an error message to it. Then return SQLITE4_ERROR.
*/
#ifndef SQLITE4_OMIT_FOREIGN_KEY
int sqlite4VdbeCheckFk(Vdbe *p, int deferred){
  sqlite4 *db = p->db;
  if( (deferred && db->nDeferredCons>0) || (!deferred && p->nFkConstraint>0) ){
    p->rc = SQLITE4_CONSTRAINT;
    p->errorAction = OE_Abort;
    sqlite4SetString(&p->zErrMsg, db, "foreign key constraint failed");
    return SQLITE4_ERROR;
  }
  return SQLITE4_OK;
}
#endif

/*
** This routine is called the when a VDBE tries to halt.  If the VDBE
** has made changes and is in autocommit mode, then commit those
** changes.  If a rollback is needed, then do the rollback.
**
** This routine is the only way to move the state of a VM from
** SQLITE4_MAGIC_RUN to SQLITE4_MAGIC_HALT.  It is harmless to
** call this on a VM that is in the SQLITE4_MAGIC_HALT state.
**
** Return an error code.  If the commit could not complete because of
** lock contention, return SQLITE4_BUSY.  If SQLITE4_BUSY is returned, it
** means the close did not happen and needs to be repeated.
*/
int sqlite4VdbeHalt(Vdbe *p){
  sqlite4 *db = p->db;

  if( db->mallocFailed ) p->rc = SQLITE4_NOMEM;
  if( p->aOnceFlag ) memset(p->aOnceFlag, 0, p->nOnceFlag);
  closeAllCursors(p);
  if( p->magic!=VDBE_MAGIC_RUN ){
    return SQLITE4_OK;
  }
  checkActiveVdbeCnt(db);

  if( p->pc>=0 ){
    /* Figure out if a transaction or statement transaction needs to be
    ** committed or rolled back.
    **
    **    0 - Do commit, either statement or transaction.
    **    1 - Do rollback, either statement or transaction.
    **    2 - Do transaction rollback.
    */
    int eAction = (p->rc!=SQLITE4_OK);

    if( p->rc==SQLITE4_CONSTRAINT ){
      if( p->errorAction==OE_Rollback ){
        eAction = 2;
      }else if( p->errorAction==OE_Fail ){
        eAction = 0;
      }
    }
    if( eAction==0 && sqlite4VdbeCheckFk(p, 0) ) eAction = 1;

    if( eAction==2 || (
       db->writeVdbeCnt==(p->readOnly==0) && db->pSavepoint==0
    )){

      if( eAction==0 && sqlite4VdbeCheckFk(p, 1) ) eAction = 1;
      if( eAction==0 ){
        int rc = sqlite4VdbeCommit(db, 1);
        if( rc!=SQLITE4_OK ){
          p->rc = rc;
          eAction = 1;
        }else{
          assert( db->nDeferredCons<=0 );
          sqlite4CommitInternalChanges(db);
        }
      }

      if( eAction ){
        sqlite4VdbeRollback(db, 1);
      }

      db->nDeferredCons = 0;
    }else if( p->stmtTransMask ){
      /* Auto-commit mode is turned off and no "OR ROLLBACK" constraint was
      ** encountered. So either commit (if eAction==0) or rollback (if 
      ** eAction==1) any statement transactions opened by this VM.  */
      assert( eAction==0 || eAction==1 );
      vdbeCloseStatement(p, eAction);
    }

    if( p->changeCntOn ){
      sqlite4VdbeSetChanges(db, (eAction ? 0 : p->nChange));
    }
    p->nChange = 0;

    /* We have successfully halted and closed the VM.  Record this fact. */
    db->activeVdbeCnt--;
    if( !p->readOnly ){
      db->writeVdbeCnt--;
    }
    assert( db->activeVdbeCnt>=db->writeVdbeCnt );

    if( db->pSavepoint==0 && db->activeVdbeCnt==0 ){
      sqlite4VdbeRollback(db, 0);
    }
  }

  p->magic = VDBE_MAGIC_HALT;
  checkActiveVdbeCnt(db);
  if( p->db->mallocFailed ){
    p->rc = SQLITE4_NOMEM;
  }
  return (p->rc==SQLITE4_BUSY ? SQLITE4_BUSY : SQLITE4_OK);
}


/*
** Each VDBE holds the result of the most recent sqlite4_step() call
** in p->rc.  This routine sets that result back to SQLITE4_OK.
*/
void sqlite4VdbeResetStepResult(Vdbe *p){
  p->rc = SQLITE4_OK;
}

/*
** Copy the error code and error message belonging to the VDBE passed
** as the first argument to its database handle (so that they will be 
** returned by calls to sqlite4_errcode() and sqlite4_errmsg()).
**
** This function does not clear the VDBE error code or message, just
** copies them to the database handle.
*/
int sqlite4VdbeTransferError(Vdbe *p){
  sqlite4 *db = p->db;
  int rc = p->rc;
  if( p->zErrMsg ){
    u8 mallocFailed = db->mallocFailed;
    sqlite4BeginBenignMalloc(db->pEnv);
    sqlite4ValueSetStr(db->pErr, -1, p->zErrMsg, SQLITE4_UTF8,
                       SQLITE4_TRANSIENT, 0);
    sqlite4EndBenignMalloc(db->pEnv);
    db->mallocFailed = mallocFailed;
    db->errCode = rc;
  }else{
    sqlite4Error(db, rc, 0);
  }
  return rc;
}

/*
** Clean up a VDBE after execution but do not delete the VDBE just yet.
** Write any error messages into *pzErrMsg.  Return the result code.
**
** After this routine is run, the VDBE should be ready to be executed
** again.
**
** To look at it another way, this routine resets the state of the
** virtual machine from VDBE_MAGIC_RUN or VDBE_MAGIC_HALT back to
** VDBE_MAGIC_INIT.
*/
int sqlite4VdbeReset(Vdbe *p){
  sqlite4 *db;
  db = p->db;

  /* If the VM did not run to completion or if it encountered an
  ** error, then it might not have been halted properly.  So halt
  ** it now.
  */
  sqlite4VdbeHalt(p);

  /* If the VDBE has be run even partially, then transfer the error code
  ** and error message from the VDBE into the main database structure.  But
  ** if the VDBE has just been set to run but has not actually executed any
  ** instructions yet, leave the main database error information unchanged.
  */
  if( p->pc>=0 ){
    sqlite4VdbeTransferError(p);
    sqlite4DbFree(db, p->zErrMsg);
    p->zErrMsg = 0;
    if( p->runOnlyOnce ) p->expired = 1;
  }else if( p->rc && p->expired ){
    /* The expired flag was set on the VDBE before the first call
    ** to sqlite4_step(). For consistency (since sqlite4_step() was
    ** called), set the database error in this case as well.
    */
    sqlite4Error(db, p->rc, 0);
    sqlite4ValueSetStr(db->pErr, -1, p->zErrMsg, SQLITE4_UTF8,
                       SQLITE4_TRANSIENT, 0);
    sqlite4DbFree(db, p->zErrMsg);
    p->zErrMsg = 0;
  }

  /* Reclaim all memory used by the VDBE
  */
  Cleanup(p);

  /* Save profiling information from this VDBE run.
  */
#ifdef VDBE_PROFILE
  {
    FILE *out = fopen("vdbe_profile.out", "a");
    if( out ){
      int i;
      fprintf(out, "---- ");
      for(i=0; i<p->nOp; i++){
        fprintf(out, "%02x", p->aOp[i].opcode);
      }
      fprintf(out, "\n");
      for(i=0; i<p->nOp; i++){
        fprintf(out, "%6d %10lld %8lld ",
           p->aOp[i].cnt,
           p->aOp[i].cycles,
           p->aOp[i].cnt>0 ? p->aOp[i].cycles/p->aOp[i].cnt : 0
        );
        sqlite4VdbePrintOp(out, i, &p->aOp[i]);
      }
      fclose(out);
    }
  }
#endif
  p->magic = VDBE_MAGIC_INIT;
  return p->rc;
}
 
/*
** Clean up and delete a VDBE after execution.  Return an integer which is
** the result code.  Write any error message text into *pzErrMsg.
*/
int sqlite4VdbeFinalize(Vdbe *p){
  int rc = SQLITE4_OK;
  if( p->magic==VDBE_MAGIC_RUN || p->magic==VDBE_MAGIC_HALT ){
    rc = sqlite4VdbeReset(p);
  }
  sqlite4VdbeDelete(p);
  return rc;
}

/*
** Call the destructor for each auxdata entry in pVdbeFunc for which
** the corresponding bit in mask is clear.  Auxdata entries beyond 31
** are always destroyed.  To destroy all auxdata entries, call this
** routine with mask==0.
*/
void sqlite4VdbeDeleteAuxData(VdbeFunc *pVdbeFunc, int mask){
  int i;
  for(i=0; i<pVdbeFunc->nAux; i++){
    struct AuxData *pAux = &pVdbeFunc->apAux[i];
    if( (i>31 || !(mask&(((u32)1)<<i))) && pAux->pAux ){
      if( pAux->xDelete ){
        pAux->xDelete(pAux->pDeleteArg, pAux->pAux);
      }
      pAux->pAux = 0;
    }
  }
}

/*
** Free all memory associated with the Vdbe passed as the second argument.
** The difference between this function and sqlite4VdbeDelete() is that
** VdbeDelete() also unlinks the Vdbe from the list of VMs associated with
** the database connection.
*/
void sqlite4VdbeDeleteObject(sqlite4 *db, Vdbe *p){
  SubProgram *pSub, *pNext;
  int i;
  assert( p->db==0 || p->db==db );
  releaseMemArray(p->aVar, p->nVar);
  releaseMemArray(p->aColName, p->nResColumn*COLNAME_N);
  for(pSub=p->pProgram; pSub; pSub=pNext){
    pNext = pSub->pNext;
    vdbeFreeOpArray(db, pSub->aOp, pSub->nOp);
    sqlite4DbFree(db, pSub);
  }
  for(i=p->nzVar-1; i>=0; i--) sqlite4DbFree(db, p->azVar[i]);
  vdbeFreeOpArray(db, p->aOp, p->nOp);
  sqlite4DbFree(db, p->aLabel);
  sqlite4DbFree(db, p->aColName);
  sqlite4DbFree(db, p->zSql);
  sqlite4DbFree(db, p->pFree);
#if defined(SQLITE4_ENABLE_TREE_EXPLAIN)
  sqlite4DbFree(db, p->zExplain);
  sqlite4DbFree(db, p->pExplain);
#endif
  sqlite4DbFree(db, p);
}

/*
** Delete an entire VDBE.
*/
void sqlite4VdbeDelete(Vdbe *p){
  sqlite4 *db;

  if( NEVER(p==0) ) return;
  db = p->db;
  if( p->pPrev ){
    p->pPrev->pNext = p->pNext;
  }else{
    assert( db->pVdbe==p );
    db->pVdbe = p->pNext;
  }
  if( p->pNext ){
    p->pNext->pPrev = p->pPrev;
  }
  p->magic = VDBE_MAGIC_DEAD;
  p->db = 0;
  sqlite4VdbeDeleteObject(db, p);
}

/*
** If we are on an architecture with mixed-endian floating 
** points (ex: ARM7) then swap the lower 4 bytes with the 
** upper 4 bytes.  Return the result.
**
** For most architectures, this is a no-op.
**
** (later):  It is reported to me that the mixed-endian problem
** on ARM7 is an issue with GCC, not with the ARM7 chip.  It seems
** that early versions of GCC stored the two words of a 64-bit
** float in the wrong order.  And that error has been propagated
** ever since.  The blame is not necessarily with GCC, though.
** GCC might have just copying the problem from a prior compiler.
** I am also told that newer versions of GCC that follow a different
** ABI get the byte order right.
**
** Developers using SQLite on an ARM7 should compile and run their
** application using -DSQLITE4_DEBUG=1 at least once.  With DEBUG
** enabled, some asserts below will ensure that the byte order of
** floating point values is correct.
**
** (2007-08-30)  Frank van Vugt has studied this problem closely
** and has send his findings to the SQLite developers.  Frank
** writes that some Linux kernels offer floating point hardware
** emulation that uses only 32-bit mantissas instead of a full 
** 48-bits as required by the IEEE standard.  (This is the
** CONFIG_FPE_FASTFPE option.)  On such systems, floating point
** byte swapping becomes very complicated.  To avoid problems,
** the necessary byte swapping is carried out using a 64-bit integer
** rather than a 64-bit float.  Frank assures us that the code here
** works for him.  We, the developers, have no way to independently
** verify this, but Frank seems to know what he is talking about
** so we trust him.
*/
#ifdef SQLITE4_MIXED_ENDIAN_64BIT_FLOAT
static u64 floatSwap(u64 in){
  union {
    u64 r;
    u32 i[2];
  } u;
  u32 t;

  u.r = in;
  t = u.i[0];
  u.i[0] = u.i[1];
  u.i[1] = t;
  return u.r;
}
# define swapMixedEndianFloat(X)  X = floatSwap(X)
#else
# define swapMixedEndianFloat(X)
#endif


/*
** This routine sets the value to be returned by subsequent calls to
** sqlite4_changes() on the database handle 'db'. 
*/
void sqlite4VdbeSetChanges(sqlite4 *db, int nChange){
  assert( sqlite4_mutex_held(db->mutex) );
  db->nChange = nChange;
  db->nTotalChange += nChange;
}

/*
** Set a flag in the vdbe to update the change counter when it is finalised
** or reset.
*/
void sqlite4VdbeCountChanges(Vdbe *v){
  v->changeCntOn = 1;
}

/*
** Mark every prepared statement associated with a database connection
** as expired.
**
** An expired statement means that recompilation of the statement is
** recommend.  Statements expire when things happen that make their
** programs obsolete.  Removing user-defined functions or collating
** sequences, or changing an authorization function are the types of
** things that make prepared statements obsolete.
*/
void sqlite4ExpirePreparedStatements(sqlite4 *db){
  Vdbe *p;
  for(p = db->pVdbe; p; p=p->pNext){
    p->expired = 1;
  }
}

/*
** Return the database associated with the Vdbe.
*/
sqlite4 *sqlite4VdbeDb(Vdbe *v){
  return v->db;
}

/*
** Return a pointer to an sqlite4_value structure containing the value bound
** parameter iVar of VM v. Except, if the value is an SQL NULL, return 
** 0 instead. Unless it is NULL, apply affinity aff (one of the SQLITE4_AFF_*
** constants) to the value before returning it.
**
** The returned value must be freed by the caller using sqlite4ValueFree().
*/
sqlite4_value *sqlite4VdbeGetValue(Vdbe *v, int iVar, u8 aff){
  assert( iVar>0 );
  if( v ){
    Mem *pMem = &v->aVar[iVar-1];
    if( 0==(pMem->flags & MEM_Null) ){
      sqlite4_value *pRet = sqlite4ValueNew(v->db);
      if( pRet ){
        sqlite4VdbeMemCopy((Mem *)pRet, pMem);
        sqlite4ValueApplyAffinity(pRet, aff, SQLITE4_UTF8);
        sqlite4VdbeMemStoreType((Mem *)pRet);
      }
      return pRet;
    }
  }
  return 0;
}

/*
** Configure SQL variable iVar so that binding a new value to it signals
** to sqlite4_reoptimize() that re-preparing the statement may result
** in a better query plan.
*/
void sqlite4VdbeSetVarmask(Vdbe *v, int iVar){
  assert( iVar>0 );
  if( iVar>32 ){
    v->expmask = 0xffffffff;
  }else{
    v->expmask |= ((u32)1 << (iVar-1));
  }
}
