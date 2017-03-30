/*
** 2008 Nov 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This module contains code that implements a parser for fts3 query strings
** (the right-hand argument to the MATCH operator). Because the supported 
** syntax is relatively simple, the whole tokenizer/parser system is
** hand-coded. 
*/
#include "fts3Int.h"
#if !defined(SQLITE4_CORE) || defined(SQLITE4_ENABLE_FTS3)

/*
** By default, this module parses the legacy syntax that has been 
** traditionally used by fts3. Or, if SQLITE4_ENABLE_FTS3_PARENTHESIS
** is defined, then it uses the new syntax. The differences between
** the new and the old syntaxes are:
**
**  a) The new syntax supports parenthesis. The old does not.
**
**  b) The new syntax supports the AND and NOT operators. The old does not.
**
**  c) The old syntax supports the "-" token qualifier. This is not 
**     supported by the new syntax (it is replaced by the NOT operator).
**
**  d) When using the old syntax, the OR operator has a greater precedence
**     than an implicit AND. When using the new, both implicity and explicit
**     AND operators have a higher precedence than OR.
**
** If compiled with SQLITE4_TEST defined, then this module exports the
** symbol "int sqlite4_fts3_enable_parentheses". Setting this variable
** to zero causes the module to use the old syntax. If it is set to 
** non-zero the new syntax is activated. This is so both syntaxes can
** be tested using a single build of testfixture.
**
** The following describes the syntax supported by the fts3 MATCH
** operator in a similar format to that used by the lemon parser
** generator. This module does not use actually lemon, it uses a
** custom parser.
**
**   query ::= andexpr (OR andexpr)*.
**
**   andexpr ::= notexpr (AND? notexpr)*.
**
**   notexpr ::= nearexpr (NOT nearexpr|-TOKEN)*.
**   notexpr ::= LP query RP.
**
**   nearexpr ::= phrase (NEAR distance_opt nearexpr)*.
**
**   distance_opt ::= .
**   distance_opt ::= / INTEGER.
**
**   phrase ::= TOKEN.
**   phrase ::= COLUMN:TOKEN.
**   phrase ::= "TOKEN TOKEN TOKEN...".
*/

#ifdef SQLITE4_TEST
int sqlite4_fts3_enable_parentheses = 0;
#else
# ifdef SQLITE4_ENABLE_FTS3_PARENTHESIS 
#  define sqlite4_fts3_enable_parentheses 1
# else
#  define sqlite4_fts3_enable_parentheses 0
# endif
#endif

/*
** Default span for NEAR operators.
*/
#define SQLITE4_FTS3_DEFAULT_NEAR_PARAM 10

#include <string.h>
#include <assert.h>

/*
** isNot:
**   This variable is used by function getNextNode(). When getNextNode() is
**   called, it sets ParseContext.isNot to true if the 'next node' is a 
**   FTSQUERY_PHRASE with a unary "-" attached to it. i.e. "mysql" in the
**   FTS3 query "sqlite -mysql". Otherwise, ParseContext.isNot is set to
**   zero.
*/
typedef struct ParseContext ParseContext;
struct ParseContext {
  sqlite4_tokenizer *pTokenizer;      /* Tokenizer module */
  const char **azCol;                 /* Array of column names for fts3 table */
  int bFts4;                          /* True to allow FTS4-only syntax */
  int nCol;                           /* Number of entries in azCol[] */
  int iDefaultCol;                    /* Default column to query */
  int isNot;                          /* True if getNextNode() sees a unary - */
  sqlite4_context *pCtx;              /* Write error message here */
  int nNest;                          /* Number of nested brackets */
};

/*
** This function is equivalent to the standard isspace() function. 
**
** The standard isspace() can be awkward to use safely, because although it
** is defined to accept an argument of type int, its behaviour when passed
** an integer that falls outside of the range of the unsigned char type
** is undefined (and sometimes, "undefined" means segfault). This wrapper
** is defined to accept an argument of type char, and always returns 0 for
** any values that fall outside of the range of the unsigned char type (i.e.
** negative values).
*/
static int fts3isspace(char c){
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\v' || c=='\f';
}

/*
** Allocate nByte bytes of memory using sqlite4_malloc(). If successful,
** zero the memory before returning a pointer to it. If unsuccessful, 
** return NULL.
*/
static void *fts3MallocZero(int nByte){
  void *pRet = sqlite4_malloc(nByte);
  if( pRet ) memset(pRet, 0, nByte);
  return pRet;
}


/*
** Extract the next token from buffer z (length n) using the tokenizer
** and other information (column names etc.) in pParse. Create an Fts3Expr
** structure of type FTSQUERY_PHRASE containing a phrase consisting of this
** single token and set *ppExpr to point to it. If the end of the buffer is
** reached before a token is found, set *ppExpr to zero. It is the
** responsibility of the caller to eventually deallocate the allocated 
** Fts3Expr structure (if any) by passing it to sqlite4_free().
**
** Return SQLITE4_OK if successful, or SQLITE4_NOMEM if a memory allocation
** fails.
*/
static int getNextToken(
  ParseContext *pParse,                   /* fts3 query parse context */
  int iCol,                               /* Value for Fts3Phrase.iColumn */
  const char *z, int n,                   /* Input string */
  Fts3Expr **ppExpr,                      /* OUT: expression */
  int *pnConsumed                         /* OUT: Number of bytes consumed */
){
  sqlite4_tokenizer *pTokenizer = pParse->pTokenizer;
  sqlite4_tokenizer_module const *pModule = pTokenizer->pModule;
  int rc;
  sqlite4_tokenizer_cursor *pCursor;
  Fts3Expr *pRet = 0;
  int nConsumed = 0;

  rc = pModule->xOpen(pTokenizer, z, n, &pCursor);
  if( rc==SQLITE4_OK ){
    const char *zToken;
    int nToken, iStart, iEnd, iPosition;
    int nByte;                               /* total space to allocate */

    pCursor->pTokenizer = pTokenizer;
    rc = pModule->xNext(pCursor, &zToken, &nToken, &iStart, &iEnd, &iPosition);

    if( rc==SQLITE4_OK ){
      nByte = sizeof(Fts3Expr) + sizeof(Fts3Phrase) + nToken;
      pRet = (Fts3Expr *)fts3MallocZero(nByte);
      if( !pRet ){
        rc = SQLITE4_NOMEM;
      }else{
        pRet->eType = FTSQUERY_PHRASE;
        pRet->pPhrase = (Fts3Phrase *)&pRet[1];
        pRet->pPhrase->nToken = 1;
        pRet->pPhrase->iColumn = iCol;
        pRet->pPhrase->aToken[0].n = nToken;
        pRet->pPhrase->aToken[0].z = (char *)&pRet->pPhrase[1];
        memcpy(pRet->pPhrase->aToken[0].z, zToken, nToken);

        if( iEnd<n && z[iEnd]=='*' ){
          pRet->pPhrase->aToken[0].isPrefix = 1;
          iEnd++;
        }

        while( 1 ){
          if( !sqlite4_fts3_enable_parentheses 
           && iStart>0 && z[iStart-1]=='-' 
          ){
            pParse->isNot = 1;
            iStart--;
          }else if( pParse->bFts4 && iStart>0 && z[iStart-1]=='^' ){
            pRet->pPhrase->aToken[0].bFirst = 1;
            iStart--;
          }else{
            break;
          }
        }

      }
      nConsumed = iEnd;
    }

    pModule->xClose(pCursor);
  }
  
  *pnConsumed = nConsumed;
  *ppExpr = pRet;
  return rc;
}


/*
** Enlarge a memory allocation.  If an out-of-memory allocation occurs,
** then free the old allocation.
*/
static void *fts3ReallocOrFree(void *pOrig, int nNew){
  void *pRet = sqlite4_realloc(pOrig, nNew);
  if( !pRet ){
    sqlite4_free(pOrig);
  }
  return pRet;
}

/*
** Buffer zInput, length nInput, contains the contents of a quoted string
** that appeared as part of an fts3 query expression. Neither quote character
** is included in the buffer. This function attempts to tokenize the entire
** input buffer and create an Fts3Expr structure of type FTSQUERY_PHRASE 
** containing the results.
**
** If successful, SQLITE4_OK is returned and *ppExpr set to point at the
** allocated Fts3Expr structure. Otherwise, either SQLITE4_NOMEM (out of memory
** error) or SQLITE4_ERROR (tokenization error) is returned and *ppExpr set
** to 0.
*/
static int getNextString(
  ParseContext *pParse,                   /* fts3 query parse context */
  const char *zInput, int nInput,         /* Input string */
  Fts3Expr **ppExpr                       /* OUT: expression */
){
  sqlite4_tokenizer *pTokenizer = pParse->pTokenizer;
  sqlite4_tokenizer_module const *pModule = pTokenizer->pModule;
  int rc;
  Fts3Expr *p = 0;
  sqlite4_tokenizer_cursor *pCursor = 0;
  char *zTemp = 0;
  int nTemp = 0;

  const int nSpace = sizeof(Fts3Expr) + sizeof(Fts3Phrase);
  int nToken = 0;

  /* The final Fts3Expr data structure, including the Fts3Phrase,
  ** Fts3PhraseToken structures token buffers are all stored as a single 
  ** allocation so that the expression can be freed with a single call to
  ** sqlite4_free(). Setting this up requires a two pass approach.
  **
  ** The first pass, in the block below, uses a tokenizer cursor to iterate
  ** through the tokens in the expression. This pass uses fts3ReallocOrFree()
  ** to assemble data in two dynamic buffers:
  **
  **   Buffer p: Points to the Fts3Expr structure, followed by the Fts3Phrase
  **             structure, followed by the array of Fts3PhraseToken 
  **             structures. This pass only populates the Fts3PhraseToken array.
  **
  **   Buffer zTemp: Contains copies of all tokens.
  **
  ** The second pass, in the block that begins "if( rc==SQLITE4_DONE )" below,
  ** appends buffer zTemp to buffer p, and fills in the Fts3Expr and Fts3Phrase
  ** structures.
  */
  rc = pModule->xOpen(pTokenizer, zInput, nInput, &pCursor);
  if( rc==SQLITE4_OK ){
    int ii;
    pCursor->pTokenizer = pTokenizer;
    for(ii=0; rc==SQLITE4_OK; ii++){
      const char *zByte;
      int nByte, iBegin, iEnd, iPos;
      rc = pModule->xNext(pCursor, &zByte, &nByte, &iBegin, &iEnd, &iPos);
      if( rc==SQLITE4_OK ){
        Fts3PhraseToken *pToken;

        p = fts3ReallocOrFree(p, nSpace + ii*sizeof(Fts3PhraseToken));
        if( !p ) goto no_mem;

        zTemp = fts3ReallocOrFree(zTemp, nTemp + nByte);
        if( !zTemp ) goto no_mem;

        assert( nToken==ii );
        pToken = &((Fts3Phrase *)(&p[1]))->aToken[ii];
        memset(pToken, 0, sizeof(Fts3PhraseToken));

        memcpy(&zTemp[nTemp], zByte, nByte);
        nTemp += nByte;

        pToken->n = nByte;
        pToken->isPrefix = (iEnd<nInput && zInput[iEnd]=='*');
        pToken->bFirst = (iBegin>0 && zInput[iBegin-1]=='^');
        nToken = ii+1;
      }
    }

    pModule->xClose(pCursor);
    pCursor = 0;
  }

  if( rc==SQLITE4_DONE ){
    int jj;
    char *zBuf = 0;

    p = fts3ReallocOrFree(p, nSpace + nToken*sizeof(Fts3PhraseToken) + nTemp);
    if( !p ) goto no_mem;
    memset(p, 0, (char *)&(((Fts3Phrase *)&p[1])->aToken[0])-(char *)p);
    p->eType = FTSQUERY_PHRASE;
    p->pPhrase = (Fts3Phrase *)&p[1];
    p->pPhrase->iColumn = pParse->iDefaultCol;
    p->pPhrase->nToken = nToken;

    zBuf = (char *)&p->pPhrase->aToken[nToken];
    if( zTemp ){
      memcpy(zBuf, zTemp, nTemp);
      sqlite4_free(zTemp);
    }else{
      assert( nTemp==0 );
    }

    for(jj=0; jj<p->pPhrase->nToken; jj++){
      p->pPhrase->aToken[jj].z = zBuf;
      zBuf += p->pPhrase->aToken[jj].n;
    }
    rc = SQLITE4_OK;
  }

  *ppExpr = p;
  return rc;
no_mem:

  if( pCursor ){
    pModule->xClose(pCursor);
  }
  sqlite4_free(zTemp);
  sqlite4_free(p);
  *ppExpr = 0;
  return SQLITE4_NOMEM;
}

/*
** Function getNextNode(), which is called by fts3ExprParse(), may itself
** call fts3ExprParse(). So this forward declaration is required.
*/
static int fts3ExprParse(ParseContext *, const char *, int, Fts3Expr **, int *);

/*
** The output variable *ppExpr is populated with an allocated Fts3Expr 
** structure, or set to 0 if the end of the input buffer is reached.
**
** Returns an SQLite error code. SQLITE4_OK if everything works, SQLITE4_NOMEM
** if a malloc failure occurs, or SQLITE4_ERROR if a parse error is encountered.
** If SQLITE4_ERROR is returned, pContext is populated with an error message.
*/
static int getNextNode(
  ParseContext *pParse,                   /* fts3 query parse context */
  const char *z, int n,                   /* Input string */
  Fts3Expr **ppExpr,                      /* OUT: expression */
  int *pnConsumed                         /* OUT: Number of bytes consumed */
){
  static const struct Fts3Keyword {
    char *z;                              /* Keyword text */
    unsigned char n;                      /* Length of the keyword */
    unsigned char parenOnly;              /* Only valid in paren mode */
    unsigned char eType;                  /* Keyword code */
  } aKeyword[] = {
    { "OR" ,  2, 0, FTSQUERY_OR   },
    { "AND",  3, 1, FTSQUERY_AND  },
    { "NOT",  3, 1, FTSQUERY_NOT  },
    { "NEAR", 4, 0, FTSQUERY_NEAR }
  };
  int ii;
  int iCol;
  int iColLen;
  int rc;
  Fts3Expr *pRet = 0;

  const char *zInput = z;
  int nInput = n;

  pParse->isNot = 0;

  /* Skip over any whitespace before checking for a keyword, an open or
  ** close bracket, or a quoted string. 
  */
  while( nInput>0 && fts3isspace(*zInput) ){
    nInput--;
    zInput++;
  }
  if( nInput==0 ){
    return SQLITE4_DONE;
  }

  /* See if we are dealing with a keyword. */
  for(ii=0; ii<(int)(sizeof(aKeyword)/sizeof(struct Fts3Keyword)); ii++){
    const struct Fts3Keyword *pKey = &aKeyword[ii];

    if( (pKey->parenOnly & ~sqlite4_fts3_enable_parentheses)!=0 ){
      continue;
    }

    if( nInput>=pKey->n && 0==memcmp(zInput, pKey->z, pKey->n) ){
      int nNear = SQLITE4_FTS3_DEFAULT_NEAR_PARAM;
      int nKey = pKey->n;
      char cNext;

      /* If this is a "NEAR" keyword, check for an explicit nearness. */
      if( pKey->eType==FTSQUERY_NEAR ){
        assert( nKey==4 );
        if( zInput[4]=='/' && zInput[5]>='0' && zInput[5]<='9' ){
          nNear = 0;
          for(nKey=5; zInput[nKey]>='0' && zInput[nKey]<='9'; nKey++){
            nNear = nNear * 10 + (zInput[nKey] - '0');
          }
        }
      }

      /* At this point this is probably a keyword. But for that to be true,
      ** the next byte must contain either whitespace, an open or close
      ** parenthesis, a quote character, or EOF. 
      */
      cNext = zInput[nKey];
      if( fts3isspace(cNext) 
       || cNext=='"' || cNext=='(' || cNext==')' || cNext==0
      ){
        pRet = (Fts3Expr *)fts3MallocZero(sizeof(Fts3Expr));
        if( !pRet ){
          return SQLITE4_NOMEM;
        }
        pRet->eType = pKey->eType;
        pRet->nNear = nNear;
        *ppExpr = pRet;
        *pnConsumed = (int)((zInput - z) + nKey);
        return SQLITE4_OK;
      }

      /* Turns out that wasn't a keyword after all. This happens if the
      ** user has supplied a token such as "ORacle". Continue.
      */
    }
  }

  /* Check for an open bracket. */
  if( sqlite4_fts3_enable_parentheses ){
    if( *zInput=='(' ){
      int nConsumed;
      pParse->nNest++;
      rc = fts3ExprParse(pParse, &zInput[1], nInput-1, ppExpr, &nConsumed);
      if( rc==SQLITE4_OK && !*ppExpr ){
        rc = SQLITE4_DONE;
      }
      *pnConsumed = (int)((zInput - z) + 1 + nConsumed);
      return rc;
    }
  
    /* Check for a close bracket. */
    if( *zInput==')' ){
      pParse->nNest--;
      *pnConsumed = (int)((zInput - z) + 1);
      return SQLITE4_DONE;
    }
  }

  /* See if we are dealing with a quoted phrase. If this is the case, then
  ** search for the closing quote and pass the whole string to getNextString()
  ** for processing. This is easy to do, as fts3 has no syntax for escaping
  ** a quote character embedded in a string.
  */
  if( *zInput=='"' ){
    for(ii=1; ii<nInput && zInput[ii]!='"'; ii++);
    *pnConsumed = (int)((zInput - z) + ii + 1);
    if( ii==nInput ){
      return SQLITE4_ERROR;
    }
    return getNextString(pParse, &zInput[1], ii-1, ppExpr);
  }


  /* If control flows to this point, this must be a regular token, or 
  ** the end of the input. Read a regular token using the sqlite4_tokenizer
  ** interface. Before doing so, figure out if there is an explicit
  ** column specifier for the token. 
  **
  ** TODO: Strangely, it is not possible to associate a column specifier
  ** with a quoted phrase, only with a single token. Not sure if this was
  ** an implementation artifact or an intentional decision when fts3 was
  ** first implemented. Whichever it was, this module duplicates the 
  ** limitation.
  */
  iCol = pParse->iDefaultCol;
  iColLen = 0;
  for(ii=0; ii<pParse->nCol; ii++){
    const char *zStr = pParse->azCol[ii];
    int nStr = (int)strlen(zStr);
    if( nInput>nStr && zInput[nStr]==':' 
     && sqlite4_strnicmp(zStr, zInput, nStr)==0 
    ){
      iCol = ii;
      iColLen = (int)((zInput - z) + nStr + 1);
      break;
    }
  }
  rc = getNextToken(pParse, iCol, &z[iColLen], n-iColLen, ppExpr, pnConsumed);
  *pnConsumed += iColLen;
  return rc;
}

/*
** The argument is an Fts3Expr structure for a binary operator (any type
** except an FTSQUERY_PHRASE). Return an integer value representing the
** precedence of the operator. Lower values have a higher precedence (i.e.
** group more tightly). For example, in the C language, the == operator
** groups more tightly than ||, and would therefore have a higher precedence.
**
** When using the new fts3 query syntax (when SQLITE4_ENABLE_FTS3_PARENTHESIS
** is defined), the order of the operators in precedence from highest to
** lowest is:
**
**   NEAR
**   NOT
**   AND (including implicit ANDs)
**   OR
**
** Note that when using the old query syntax, the OR operator has a higher
** precedence than the AND operator.
*/
static int opPrecedence(Fts3Expr *p){
  assert( p->eType!=FTSQUERY_PHRASE );
  if( sqlite4_fts3_enable_parentheses ){
    return p->eType;
  }else if( p->eType==FTSQUERY_NEAR ){
    return 1;
  }else if( p->eType==FTSQUERY_OR ){
    return 2;
  }
  assert( p->eType==FTSQUERY_AND );
  return 3;
}

/*
** Argument ppHead contains a pointer to the current head of a query 
** expression tree being parsed. pPrev is the expression node most recently
** inserted into the tree. This function adds pNew, which is always a binary
** operator node, into the expression tree based on the relative precedence
** of pNew and the existing nodes of the tree. This may result in the head
** of the tree changing, in which case *ppHead is set to the new root node.
*/
static void insertBinaryOperator(
  Fts3Expr **ppHead,       /* Pointer to the root node of a tree */
  Fts3Expr *pPrev,         /* Node most recently inserted into the tree */
  Fts3Expr *pNew           /* New binary node to insert into expression tree */
){
  Fts3Expr *pSplit = pPrev;
  while( pSplit->pParent && opPrecedence(pSplit->pParent)<=opPrecedence(pNew) ){
    pSplit = pSplit->pParent;
  }

  if( pSplit->pParent ){
    assert( pSplit->pParent->pRight==pSplit );
    pSplit->pParent->pRight = pNew;
    pNew->pParent = pSplit->pParent;
  }else{
    *ppHead = pNew;
  }
  pNew->pLeft = pSplit;
  pSplit->pParent = pNew;
}

/*
** Parse the fts3 query expression found in buffer z, length n. This function
** returns either when the end of the buffer is reached or an unmatched 
** closing bracket - ')' - is encountered.
**
** If successful, SQLITE4_OK is returned, *ppExpr is set to point to the
** parsed form of the expression and *pnConsumed is set to the number of
** bytes read from buffer z. Otherwise, *ppExpr is set to 0 and SQLITE4_NOMEM
** (out of memory error) or SQLITE4_ERROR (parse error) is returned.
*/
static int fts3ExprParse(
  ParseContext *pParse,                   /* fts3 query parse context */
  const char *z, int n,                   /* Text of MATCH query */
  Fts3Expr **ppExpr,                      /* OUT: Parsed query structure */
  int *pnConsumed                         /* OUT: Number of bytes consumed */
){
  Fts3Expr *pRet = 0;
  Fts3Expr *pPrev = 0;
  Fts3Expr *pNotBranch = 0;               /* Only used in legacy parse mode */
  int nIn = n;
  const char *zIn = z;
  int rc = SQLITE4_OK;
  int isRequirePhrase = 1;

  while( rc==SQLITE4_OK ){
    Fts3Expr *p = 0;
    int nByte = 0;
    rc = getNextNode(pParse, zIn, nIn, &p, &nByte);
    if( rc==SQLITE4_OK ){
      int isPhrase;

      if( !sqlite4_fts3_enable_parentheses 
       && p->eType==FTSQUERY_PHRASE && pParse->isNot 
      ){
        /* Create an implicit NOT operator. */
        Fts3Expr *pNot = fts3MallocZero(sizeof(Fts3Expr));
        if( !pNot ){
          sqlite4Fts3ExprFree(p);
          rc = SQLITE4_NOMEM;
          goto exprparse_out;
        }
        pNot->eType = FTSQUERY_NOT;
        pNot->pRight = p;
        if( pNotBranch ){
          pNot->pLeft = pNotBranch;
        }
        pNotBranch = pNot;
        p = pPrev;
      }else{
        int eType = p->eType;
        isPhrase = (eType==FTSQUERY_PHRASE || p->pLeft);

        /* The isRequirePhrase variable is set to true if a phrase or
        ** an expression contained in parenthesis is required. If a
        ** binary operator (AND, OR, NOT or NEAR) is encounted when
        ** isRequirePhrase is set, this is a syntax error.
        */
        if( !isPhrase && isRequirePhrase ){
          sqlite4Fts3ExprFree(p);
          rc = SQLITE4_ERROR;
          goto exprparse_out;
        }
  
        if( isPhrase && !isRequirePhrase ){
          /* Insert an implicit AND operator. */
          Fts3Expr *pAnd;
          assert( pRet && pPrev );
          pAnd = fts3MallocZero(sizeof(Fts3Expr));
          if( !pAnd ){
            sqlite4Fts3ExprFree(p);
            rc = SQLITE4_NOMEM;
            goto exprparse_out;
          }
          pAnd->eType = FTSQUERY_AND;
          insertBinaryOperator(&pRet, pPrev, pAnd);
          pPrev = pAnd;
        }

        /* This test catches attempts to make either operand of a NEAR
        ** operator something other than a phrase. For example, either of
        ** the following:
        **
        **    (bracketed expression) NEAR phrase
        **    phrase NEAR (bracketed expression)
        **
        ** Return an error in either case.
        */
        if( pPrev && (
            (eType==FTSQUERY_NEAR && !isPhrase && pPrev->eType!=FTSQUERY_PHRASE)
         || (eType!=FTSQUERY_PHRASE && isPhrase && pPrev->eType==FTSQUERY_NEAR)
        )){
          sqlite4Fts3ExprFree(p);
          rc = SQLITE4_ERROR;
          goto exprparse_out;
        }
  
        if( isPhrase ){
          if( pRet ){
            assert( pPrev && pPrev->pLeft && pPrev->pRight==0 );
            pPrev->pRight = p;
            p->pParent = pPrev;
          }else{
            pRet = p;
          }
        }else{
          insertBinaryOperator(&pRet, pPrev, p);
        }
        isRequirePhrase = !isPhrase;
      }
      assert( nByte>0 );
    }
    assert( rc!=SQLITE4_OK || (nByte>0 && nByte<=nIn) );
    nIn -= nByte;
    zIn += nByte;
    pPrev = p;
  }

  if( rc==SQLITE4_DONE && pRet && isRequirePhrase ){
    rc = SQLITE4_ERROR;
  }

  if( rc==SQLITE4_DONE ){
    rc = SQLITE4_OK;
    if( !sqlite4_fts3_enable_parentheses && pNotBranch ){
      if( !pRet ){
        rc = SQLITE4_ERROR;
      }else{
        Fts3Expr *pIter = pNotBranch;
        while( pIter->pLeft ){
          pIter = pIter->pLeft;
        }
        pIter->pLeft = pRet;
        pRet = pNotBranch;
      }
    }
  }
  *pnConsumed = n - nIn;

exprparse_out:
  if( rc!=SQLITE4_OK ){
    sqlite4Fts3ExprFree(pRet);
    sqlite4Fts3ExprFree(pNotBranch);
    pRet = 0;
  }
  *ppExpr = pRet;
  return rc;
}

/*
** Parameters z and n contain a pointer to and length of a buffer containing
** an fts3 query expression, respectively. This function attempts to parse the
** query expression and create a tree of Fts3Expr structures representing the
** parsed expression. If successful, *ppExpr is set to point to the head
** of the parsed expression tree and SQLITE4_OK is returned. If an error
** occurs, either SQLITE4_NOMEM (out-of-memory error) or SQLITE4_ERROR (parse
** error) is returned and *ppExpr is set to 0.
**
** If parameter n is a negative number, then z is assumed to point to a
** nul-terminated string and the length is determined using strlen().
**
** The first parameter, pTokenizer, is passed the fts3 tokenizer module to
** use to normalize query tokens while parsing the expression. The azCol[]
** array, which is assumed to contain nCol entries, should contain the names
** of each column in the target fts3 table, in order from left to right. 
** Column names must be nul-terminated strings.
**
** The iDefaultCol parameter should be passed the index of the table column
** that appears on the left-hand-side of the MATCH operator (the default
** column to match against for tokens for which a column name is not explicitly
** specified as part of the query string), or -1 if tokens may by default
** match any table column.
*/
int sqlite4Fts3ExprParse(
  sqlite4_tokenizer *pTokenizer,      /* Tokenizer module */
  char **azCol,                       /* Array of column names for fts3 table */
  int bFts4,                          /* True to allow FTS4-only syntax */
  int nCol,                           /* Number of entries in azCol[] */
  int iDefaultCol,                    /* Default column to query */
  const char *z, int n,               /* Text of MATCH query */
  Fts3Expr **ppExpr                   /* OUT: Parsed query structure */
){
  int nParsed;
  int rc;
  ParseContext sParse;
  sParse.pTokenizer = pTokenizer;
  sParse.azCol = (const char **)azCol;
  sParse.nCol = nCol;
  sParse.iDefaultCol = iDefaultCol;
  sParse.nNest = 0;
  sParse.bFts4 = bFts4;
  if( z==0 ){
    *ppExpr = 0;
    return SQLITE4_OK;
  }
  if( n<0 ){
    n = (int)strlen(z);
  }
  rc = fts3ExprParse(&sParse, z, n, ppExpr, &nParsed);

  /* Check for mismatched parenthesis */
  if( rc==SQLITE4_OK && sParse.nNest ){
    rc = SQLITE4_ERROR;
    sqlite4Fts3ExprFree(*ppExpr);
    *ppExpr = 0;
  }

  return rc;
}

/*
** Free a parsed fts3 query expression allocated by sqlite4Fts3ExprParse().
*/
void sqlite4Fts3ExprFree(Fts3Expr *p){
  if( p ){
    assert( p->eType==FTSQUERY_PHRASE || p->pPhrase==0 );
    sqlite4Fts3ExprFree(p->pLeft);
    sqlite4Fts3ExprFree(p->pRight);
    sqlite4Fts3EvalPhraseCleanup(p->pPhrase);
    sqlite4_free(p->aMI);
    sqlite4_free(p);
  }
}

/****************************************************************************
*****************************************************************************
** Everything after this point is just test code.
*/

#ifdef SQLITE4_TEST

#include <stdio.h>

/*
** Function to query the hash-table of tokenizers (see README.tokenizers).
*/
static int queryTestTokenizer(
  sqlite4 *db, 
  const char *zName,  
  const sqlite4_tokenizer_module **pp
){
  int rc;
  sqlite4_stmt *pStmt;
  const char zSql[] = "SELECT fts3_tokenizer(?)";

  *pp = 0;
  rc = sqlite4_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE4_OK ){
    return rc;
  }

  sqlite4_bind_text(pStmt, 1, zName, -1, SQLITE4_STATIC);
  if( SQLITE4_ROW==sqlite4_step(pStmt) ){
    if( sqlite4_column_type(pStmt, 0)==SQLITE4_BLOB ){
      memcpy((void *)pp, sqlite4_column_blob(pStmt, 0), sizeof(*pp));
    }
  }

  return sqlite4_finalize(pStmt);
}

/*
** Return a pointer to a buffer containing a text representation of the
** expression passed as the first argument. The buffer is obtained from
** sqlite4_malloc(). It is the responsibility of the caller to use 
** sqlite4_free() to release the memory. If an OOM condition is encountered,
** NULL is returned.
**
** If the second argument is not NULL, then its contents are prepended to 
** the returned expression text and then freed using sqlite4_free().
*/
static char *exprToString(Fts3Expr *pExpr, char *zBuf){
  switch( pExpr->eType ){
    case FTSQUERY_PHRASE: {
      Fts3Phrase *pPhrase = pExpr->pPhrase;
      int i;
      zBuf = sqlite4_mprintf(
          "%zPHRASE %d 0", zBuf, pPhrase->iColumn);
      for(i=0; zBuf && i<pPhrase->nToken; i++){
        zBuf = sqlite4_mprintf("%z %.*s%s", zBuf, 
            pPhrase->aToken[i].n, pPhrase->aToken[i].z,
            (pPhrase->aToken[i].isPrefix?"+":"")
        );
      }
      return zBuf;
    }

    case FTSQUERY_NEAR:
      zBuf = sqlite4_mprintf("%zNEAR/%d ", zBuf, pExpr->nNear);
      break;
    case FTSQUERY_NOT:
      zBuf = sqlite4_mprintf("%zNOT ", zBuf);
      break;
    case FTSQUERY_AND:
      zBuf = sqlite4_mprintf("%zAND ", zBuf);
      break;
    case FTSQUERY_OR:
      zBuf = sqlite4_mprintf("%zOR ", zBuf);
      break;
  }

  if( zBuf ) zBuf = sqlite4_mprintf("%z{", zBuf);
  if( zBuf ) zBuf = exprToString(pExpr->pLeft, zBuf);
  if( zBuf ) zBuf = sqlite4_mprintf("%z} {", zBuf);

  if( zBuf ) zBuf = exprToString(pExpr->pRight, zBuf);
  if( zBuf ) zBuf = sqlite4_mprintf("%z}", zBuf);

  return zBuf;
}

/*
** This is the implementation of a scalar SQL function used to test the 
** expression parser. It should be called as follows:
**
**   fts3_exprtest(<tokenizer>, <expr>, <column 1>, ...);
**
** The first argument, <tokenizer>, is the name of the fts3 tokenizer used
** to parse the query expression (see README.tokenizers). The second argument
** is the query expression to parse. Each subsequent argument is the name
** of a column of the fts3 table that the query expression may refer to.
** For example:
**
**   SELECT fts3_exprtest('simple', 'Bill col2:Bloggs', 'col1', 'col2');
*/
static void fts3ExprTest(
  sqlite4_context *context,
  int argc,
  sqlite4_value **argv
){
  sqlite4_tokenizer_module const *pModule = 0;
  sqlite4_tokenizer *pTokenizer = 0;
  int rc;
  char **azCol = 0;
  const char *zExpr;
  int nExpr;
  int nCol;
  int ii;
  Fts3Expr *pExpr;
  char *zBuf = 0;
  sqlite4 *db = sqlite4_context_db_handle(context);

  if( argc<3 ){
    sqlite4_result_error(context, 
        "Usage: fts3_exprtest(tokenizer, expr, col1, ...", -1
    );
    return;
  }

  rc = queryTestTokenizer(db,
                          (const char *)sqlite4_value_text(argv[0]), &pModule);
  if( rc==SQLITE4_NOMEM ){
    sqlite4_result_error_nomem(context);
    goto exprtest_out;
  }else if( !pModule ){
    sqlite4_result_error(context, "No such tokenizer module", -1);
    goto exprtest_out;
  }

  rc = pModule->xCreate(0, 0, &pTokenizer);
  assert( rc==SQLITE4_NOMEM || rc==SQLITE4_OK );
  if( rc==SQLITE4_NOMEM ){
    sqlite4_result_error_nomem(context);
    goto exprtest_out;
  }
  pTokenizer->pModule = pModule;

  zExpr = (const char *)sqlite4_value_text(argv[1]);
  nExpr = sqlite4_value_bytes(argv[1]);
  nCol = argc-2;
  azCol = (char **)sqlite4_malloc(nCol*sizeof(char *));
  if( !azCol ){
    sqlite4_result_error_nomem(context);
    goto exprtest_out;
  }
  for(ii=0; ii<nCol; ii++){
    azCol[ii] = (char *)sqlite4_value_text(argv[ii+2]);
  }

  rc = sqlite4Fts3ExprParse(
      pTokenizer, azCol, 0, nCol, nCol, zExpr, nExpr, &pExpr
  );
  if( rc!=SQLITE4_OK && rc!=SQLITE4_NOMEM ){
    sqlite4_result_error(context, "Error parsing expression", -1);
  }else if( rc==SQLITE4_NOMEM || !(zBuf = exprToString(pExpr, 0)) ){
    sqlite4_result_error_nomem(context);
  }else{
    sqlite4_result_text(context, zBuf, -1, SQLITE4_TRANSIENT);
    sqlite4_free(zBuf);
  }

  sqlite4Fts3ExprFree(pExpr);

exprtest_out:
  if( pModule && pTokenizer ){
    rc = pModule->xDestroy(pTokenizer);
  }
  sqlite4_free(azCol);
}

/*
** Register the query expression parser test function fts3_exprtest() 
** with database connection db. 
*/
int sqlite4Fts3ExprInitTestInterface(sqlite4* db){
  return sqlite4_create_function(
      db, "fts3_exprtest", -1, SQLITE4_UTF8, 0, fts3ExprTest, 0, 0
  );
}

#endif
#endif /* !defined(SQLITE4_CORE) || defined(SQLITE4_ENABLE_FTS3) */
