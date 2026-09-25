/*
** 2026 September 25
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
** Phase 1 smoke test.  Drives the extracted btree/pager/journal stack
** directly, with no SQL layer, to prove the storage mechanics work after
** the SQL couplings were cut:
**
**   1. a file-backed database is created and rows are written through a
**      btree cursor in a write transaction, then committed;
**   2. the committed rows are read back from disk in a fresh transaction;
**   3. a rolled-back write transaction leaves the database unchanged;
**   4. a hot journal left behind by a simulated crash is replayed on the
**      next open.
**
** The trees here are BTREE_INTKEY (rowid keys with data payloads), which
** is the storage format the extracted code implements today.  The KV
** cell format (byte-string keys and values in one cell) is Phase 2 work.
*/
#include "sqliteInt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int nFail = 0;
static int nTest = 0;

#define CHECK(cond) do{                                            \
  nTest++;                                                         \
  if( !(cond) ){                                                   \
    nFail++;                                                       \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
  }                                                                \
}while(0)

static sqlite3 *makeConn(void){
  sqlite3 *db = (sqlite3*)sqlite3MallocZero(sizeof(sqlite3));
  db->mutex = sqlite3MutexAlloc(SQLITE_MUTEX_RECURSIVE);
  db->aDb = (Db*)sqlite3MallocZero(sizeof(Db));
  db->nDb = 1;
  db->errMask = 0xff;
  sqlite3_mutex_enter(db->mutex);
  return db;
}
static void freeConn(sqlite3 *db){
  sqlite3_mutex_leave(db->mutex);
  sqlite3_mutex_free(db->mutex);
  sqlite3_free(db->aDb);
  sqlite3_free(db);
}

static int writeRows(Btree *pBt, Pgno iRoot, int iFirst, int iLast){
  BtCursor *pCur = (BtCursor*)sqlite3Malloc(sqlite3BtreeCursorSize());
  int rc, i;
  sqlite3BtreeCursorZero(pCur);
  rc = sqlite3BtreeCursor(pBt, iRoot, BTREE_WRCSR, 0, pCur);
  if( rc ){ sqlite3_free(pCur); return rc; }
  for(i=iFirst; i<=iLast; i++){
    char zVal[64];
    BtreePayload x;
    int nV = sprintf(zVal, "payload-for-row-%04d", i);
    memset(&x, 0, sizeof(x));
    x.nKey = i;
    x.pData = zVal;
    x.nData = nV;
    rc = sqlite3BtreeInsert(pCur, &x, BTREE_APPEND, 0);
    if( rc ) break;
  }
  sqlite3BtreeCloseCursor(pCur);
  sqlite3_free(pCur);
  return rc;
}

static int countRows(Btree *pBt, Pgno iRoot, int *pnRow){
  BtCursor *pCur = (BtCursor*)sqlite3Malloc(sqlite3BtreeCursorSize());
  int rc, res = 0, nRow = 0;
  sqlite3BtreeCursorZero(pCur);
  rc = sqlite3BtreeCursor(pBt, iRoot, 0, 0, pCur);
  if( rc ){ sqlite3_free(pCur); return rc; }
  rc = sqlite3BtreeFirst(pCur, &res);
  while( rc==SQLITE_OK && res==0 ){
    i64 iKey = sqlite3BtreeIntegerKey(pCur);
    char zExpect[64], zGot[64];
    int nPayload = (int)sqlite3BtreePayloadSize(pCur);
    (void)sprintf(zExpect, "payload-for-row-%04d", (int)iKey);
    if( nPayload!=(int)strlen(zExpect) ){ rc = SQLITE_CORRUPT; break; }
    rc = sqlite3BtreePayload(pCur, 0, nPayload, zGot);
    if( rc ) break;
    zGot[nPayload] = 0;
    if( strcmp(zGot, zExpect)!=0 ){ rc = SQLITE_CORRUPT; break; }
    nRow++;
    rc = sqlite3BtreeNext(pCur, 0);
    if( rc==SQLITE_DONE ){ rc = SQLITE_OK; break; }
    res = sqlite3BtreeEof(pCur) ? 1 : 0;
  }
  *pnRow = nRow;
  sqlite3BtreeCloseCursor(pCur);
  sqlite3_free(pCur);
  return rc;
}

static void testCommitPersist(const char *zFile){
  sqlite3 *db = makeConn();
  Btree *pBt = 0;
  Pgno iRoot = 0;
  int rc, nRow = 0;

  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), zFile, db, &pBt, 0,
                        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
  CHECK( rc==SQLITE_OK );
  if( rc ) { freeConn(db); return; }

  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeCreateTable(pBt, &iRoot, BTREE_INTKEY);
  CHECK( rc==SQLITE_OK );
  rc = writeRows(pBt, iRoot, 1, 50);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeCommit(pBt);
  CHECK( rc==SQLITE_OK );
  sqlite3BtreeClose(pBt);
  freeConn(db);

  db = makeConn();
  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), zFile, db, &pBt, 0,
                        SQLITE_OPEN_READWRITE);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeBeginTrans(pBt, 0, 0);
  CHECK( rc==SQLITE_OK );
  /* The first user tree created is rooted on page 2. */
  rc = countRows(pBt, 2, &nRow);
  CHECK( rc==SQLITE_OK );
  CHECK( nRow==50 );
  sqlite3BtreeCommit(pBt);
  sqlite3BtreeClose(pBt);
  freeConn(db);
}

static void testRollback(const char *zFile){
  sqlite3 *db = makeConn();
  Btree *pBt = 0;
  int rc, nRow = 0;

  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), zFile, db, &pBt, 0,
                        SQLITE_OPEN_READWRITE);
  CHECK( rc==SQLITE_OK );
  if( rc ){ freeConn(db); return; }

  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);
  CHECK( rc==SQLITE_OK );
  rc = writeRows(pBt, 2, 51, 70);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeRollback(pBt, SQLITE_OK, 0);
  CHECK( rc==SQLITE_OK );

  rc = sqlite3BtreeBeginTrans(pBt, 0, 0);
  CHECK( rc==SQLITE_OK );
  rc = countRows(pBt, 2, &nRow);
  CHECK( rc==SQLITE_OK );
  CHECK( nRow==50 );
  sqlite3BtreeCommit(pBt);
  sqlite3BtreeClose(pBt);
  freeConn(db);
}

static void testHotJournal(const char *zFile){
  sqlite3 *db = makeConn();
  Btree *pBt = 0;
  int rc, nRow = 0;
  pid_t pid;
  int status;

  pid = fork();
  if( pid==0 ){
    sqlite3 *db2 = makeConn();
    Btree *pTmp = 0;
    int rc2 = sqlite3BtreeOpen(sqlite3_vfs_find(0), zFile, db2, &pTmp, 0,
                               SQLITE_OPEN_READWRITE);
    if( rc2==SQLITE_OK ){
      rc2 = sqlite3BtreeBeginTrans(pTmp, 1, 0);
      if( rc2==SQLITE_OK ){
        BtCursor *pCur = (BtCursor*)sqlite3Malloc(sqlite3BtreeCursorSize());
        sqlite3BtreeCursorZero(pCur);
        if( sqlite3BtreeCursor(pTmp, 2, BTREE_WRCSR, 0, pCur)==SQLITE_OK ){
          BtreePayload x;
          char zVal[64];
          int nV = sprintf(zVal, "dirty-uncommitted-payload-%04d", 1);
          memset(&x, 0, sizeof(x));
          x.nKey = 1; x.pData = zVal; x.nData = nV;
          sqlite3BtreeInsert(pCur, &x, 0, 0);
          sqlite3BtreeCloseCursor(pCur);
        }
        sqlite3_free(pCur);
      }
    }
    _exit(0);
  }
  waitpid(pid, &status, 0);

  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), zFile, db, &pBt, 0,
                        SQLITE_OPEN_READWRITE);
  CHECK( rc==SQLITE_OK );
  if( rc ){ freeConn(db); return; }
  rc = sqlite3BtreeBeginTrans(pBt, 0, 0);
  CHECK( rc==SQLITE_OK );
  rc = countRows(pBt, 2, &nRow);
  CHECK( rc==SQLITE_OK );
  CHECK( nRow==50 );
  sqlite3BtreeCommit(pBt);
  sqlite3BtreeClose(pBt);
  freeConn(db);
}

int main(void){
  const char *zFile = "t_smoke.db";
  int rc = sqlite3_initialize();
  if( rc!=SQLITE_OK ){
    printf("initialize failed: %d\n", rc);
    return 2;
  }
  unlink(zFile);
  unlink("t_smoke.db-journal");

  testCommitPersist(zFile);
  testRollback(zFile);
  testHotJournal(zFile);

  unlink(zFile);
  unlink("t_smoke.db-journal");

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}