/*
** 2026 September 24
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
** Phase-0 shim for vdbeInt.h.  status.c includes vdbeInt.h solely to
** service SQLITE_DBSTATUS_STMT_USED, a prepared-statement accounting
** feature of the SQL layer.  btreelite has no VDBE; this empty header
** lets status.c compile.  The SQLITE_DBSTATUS_STMT_USED case is deleted
** in Phase 2 together with this file.
*/
#ifndef VDBEINT_H
#define VDBEINT_H
#endif /* VDBEINT_H */