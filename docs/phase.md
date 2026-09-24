# btreelite 实现计划

从 SQLite 3.54.0 (`sqlite-src-3530400`) 提取存储子系统（btree + pager + WAL + pcache + VFS），
重铸为纯 KV 形式的 B-tree 存储引擎。本文档为完整实现计划。

约定：公共 API 前缀 `btreelite_`；内部符号保留 `sqlite3*` 原名（便于与上游对照/rebase）；
文件名统一使用 `btreelite` 前缀。

---

## 0. 核心设计判断

SQLite 的 btree.c 本身就是双模 KV 引擎：

- **Table btree（INTKEY）**：键是 64 位整数（rowid），值是任意字节串。
- **Index btree（BLOBKEY）**：键是任意字节串，无值（key 即内容）。

目标 KV 语义——key 为任意字节串、按 memcmp 二进制序排列、value 为任意字节串——
从两个变体中各取一半：取 index btree 的键语义 + table btree 的载荷语义。

**原封不动原则的精确含义**：算法不动（balance、allocateBtreePage、freelist、WAL、
hot-journal 恢复全部保留），文件格式骨架不动（页类型、cell 布局、overflow 链、
freelist trunk 结构保留），动的是入口接口和 SQL 耦合点的缝线。

---

## 1. 目标 API（公共接口，`include/btreelite.h`）

```c
typedef struct btreelite_db btreelite_db;     /* opaque, 一个数据库文件 */
typedef struct btreelite_cur btreelite_cur;   /* opaque cursor */

/* 打开/关闭 */
int btreelite_open(const char *path, btreelite_db **ppDb);  /* NULL → 内存树 */
void btreelite_close(btreelite_db *p);

/* 多树：一个文件内多个 btree，root page 标识 */
int btreelite_create_tree(btreelite_db *p, unsigned *piRoot);
int btreelite_cursor_open(btreelite_db *p, unsigned iRoot, int wrFlag,
                          btreelite_cur **ppCur);
void btreelite_cursor_close(btreelite_cur *c);

/* 事务 */
int btreelite_begin(btreelite_db *p, int wrflag);   /* 0=读 1=写 2=独占 */
int btreelite_commit(btreelite_db *p);
int btreelite_rollback(btreelite_db *p);
int btreelite_savepoint(btreelite_db *p, int op, int idx);

/* CRUD —— 读写事务边界内调用 */
int btreelite_get(btreelite_cur *c, const void *k, int nK);
int btreelite_put(btreelite_cur *c, const void *k, int nK,
                  const void *v, int nV);
int btreelite_del(btreelite_cur *c, const void *k);

/* 游标遍历 */
int btreelite_first(btreelite_cur *c, int *pRes);   /* *pRes: 0=有, 1=空 */
int btreelite_next(btreelite_cur *c);
int btreelite_prev(btreelite_cur *c);
int btreelite_last(btreelite_cur *c, int *pRes);
int btreelite_seek(btreelite_cur *c, const void *k, int nK); /* 定位到 >=k */

/* 读取当前项 */
int btreelite_key(btreelite_cur *c, void *buf, int nMax);
const void *btreelite_value_fetch(btreelite_cur *c, int *pAmt); /* zero-copy 本地段 */
int btreelite_value_read(btreelite_cur *c, u32 offset, u32 amt, void *pBuf);
int btreelite_value_write_partial(...);  /* 原 incrblob 路径 */

/* WAL / 维护 */
int btreelite_checkpoint(btreelite_db *p, int eMode, int *pnLog, int *pnCkpt);
int btreelite_integrity_check(btreelite_db *p, unsigned iRoot, int mxErr,
                              int *pnErr, char **pzOut);
```

---

## 2. 关键技术决策：单一 cell 格式 = "带数据的 index btree"

SQLite 两种 cell 类型：

```
index cell:      [child ptr?] varint(nKey) KeyBytes            ← 无 value
table cell:      varint(nData) varint(intKey) DataBytes        ← int 键 + value
```

两者都不能直接表达"字节串 key + 字节串 value"。方案对比：

| 方案 | 做法 | 结论 |
|---|---|---|
| A. Key 包含 value | 复用 index btree，把 value 拼进 key | 改 key 即全树重写；update-value 退化为 delete+insert，破坏 overwrite 快路径。**弃** |
| B. Value 存单独树 | key→(root) 二级树 | 每次读 2 次树遍历，复杂度爆炸。**弃** |
| C. 改 cell 格式加 value 字段 | index cell 的 key 后追加 value 段 | **采用** |

### 最终 KV cell 布局

```
leaf cell:     varint(nKey) | key bytes | varint(nValue) | value bytes | [4B ovfl ptr]
interior cell: 4B child | varint(nKey) | key bytes     ← 与 SQLite index interior 相同
overflow:      4B next | data（沿用）
```

关键设计点：

- **payload 连续存储 `[key][value]`**，`nPayload = nKeyBytes + nValueBytes`。
  overflow 分配、`accessPayload(offset, amt)`、`clearCellOverflow`、`fillInCell`
  全部按现有 index cell 逻辑原封不动——它们不感知 key/value 边界。
- **本地/溢出阈值沿用 index btree 公式**：
  `maxLocal = (usable-12)*64/255 - 23`，`minLocal = (usable-12)*32/255 - 23`。
- **CellInfo 增加字段**：`u32 nKeyBytes`（键字节长度）、`u32 nValueBytes`。
  解析时 nKey varint 已读出，零额外成本。
- **比较器**：`xRecordCompare`（多字段 collation）降为纯 memcmp +
  长度决胜。`btreeMoveto` 统一为单一 memcmp 序。
- **divider（interior cell）只有 key，无 value** —— 与 index interior cell
  完全同构。这是 balance 算法原封不动的根本保证。

### 对 balance 算法的冲击评估

`balance_nonroot` 里 divider 处理有三条路径：
- `leafData`（LEAFDATA 树）：divider 即 key —— KV 不走此路径
- `indexLeaf`：divider 是完整 cell —— **KV 走这条，与现有 PTF_ZERODATA 树同构**
- intkey：divider 无内容 —— KV 不走

结论：**balance 算法原封不动**。KV 树在 balance 看来就是一个
"index btree，只是叶子 cell 尾部多挂了 value"。

`MemPage.xParseCell/xCellSize` 函数指针机制保留，新增 KV 变体：
`btreeParseCellPtrKV`、`cellSizePtrKV`（解析尾部 nValue + value 本地长度），
与现有 4 个变体（noPayload/TableLeaf/Idx/IdxLeaf）同构。

---

## 3. 文件格式决策

- **魔串**：`"btreelite format 1\0"`（16 字节含 `\0`），写入 btreeInt.h 的
  `SQLITE_FILE_HEADER`。与 SQLite 文件天然不兼容（sqlite3 CLI 不会误开）。
- **整库单一模式**：库级标记 `BtShared.btsFlags |= BTS_KV`，打开时校验魔串一致。
  `decodeFlags` 按 BTS_KV 统一走 KV 分支，只接受 0x02（interior）/0x0A（leaf）
  两种页类型，其余一律 CORRUPT。不引入新的 PTF_* flag 位（避免破坏位语义校验）。
- **100 字节文件头逐字节保留 SQLite 语义**：
  - offset 16-17: page size；18/19: read/write version（1=journal, 2=WAL）
  - offset 20: reserved bytes；21-23: max/min embedded payload fraction (64/32/32)
  - offset 24: change counter；28: page count；32/36: freelist 首页/计数
  - offset 92: version-valid-for；96: version number
  - offset 40+4i 的 15 个 meta 值：保留 GetMeta/UpdateMeta 机制。
    meta[4]（原 largest_root_page）→ 弃用。
- **root page 目录**：不做内置目录树。`btreelite_create_tree` 调
  `allocateBtreePage`（BTALLOC_ANY 模式）返回新 root page 号，调用方自行持久化。
  保持 KV 层最薄。将来可选加 `btreelite_tree_root_dir`（固定 root 的目录树）。

---

## 4. SQL 耦合点清单（逐条列出，抽取时拆除）

### btree.c

1. `Btree.db` / `pBt->db`（sqlite3*）：~60 处引用。`btreeInvokeBusyHandler`
   调 `sqlite3InvokeBusyHandler(&pBt->db->busyHandler)` → 改为自有 busy 回调
   （`sqlite3PagerSetBusyHandler` 已是通用接口）。
2. `BtShared.pSchema` / `xFreeSchema` / `sqlite3BtreeSchema()` /
   `sqlite3BtreeSchemaLocked()`：删除。btree_open 成功路径
   `sqlite3BtreeSchema(p,0,0)==0` 判断删掉。
3. shared-cache 全套：`querySharedCacheTableLock`（btree.c:329-381）、
   `setSharedCacheTableLock`（401-455）、`clearAllSharedCacheTableLocks`（467-508）、
   `downgradeAllSharedCacheTableLocks`（513-527）、
   `hasSharedCacheTableLock` / `hasReadConflicts`（debug-only）、
   `sqlite3BtreeLockTable`、`BtLock` 结构、`BtShared.pLock/pWriter/BTS_PENDING`。
   全删。btmutex.c 整个文件删除，`sqlite3BtreeEnter/Leave` 变 no-op 宏。
4. autovacuum：`ptrmapPut/ptrmapGet/ptrmapPutOvflPtr`、
   `incrVacuumStep/finalDbSize/relocatePage/modifyPagePointer/setChildPtrmaps`、
   `sqlite3BtreeSetAutoVacuum/GetAutoVacuum/IncrVacuum`、`autoVacuumCommit`、
   `btreeDropTable` 的 root-page 重定位分支、`btreeCreateTable` 的 meta[4]
   更新分支。全部删除（编译为 `#define SQLITE_OMIT_AUTOVACUUM`），约 -700 行。
5. **intkey 专属路径：删除**。包括 `btreeParseCellPtr`、`cellSizePtrTableLeaf`、
   `sqlite3BtreeTableMoveto`、`BtreePayload` intkey 语义、
   `sqlite3BtreeIntegerKey`、`sqlite3BtreeTransferRow`、`BTREE_APPEND`/`BTREE_PREFORMAT`
   分支、`sqlite3BtreeCount`、`sqlite3BtreeTransferRow`。理由：
   KV 语义下所有键都是字节串，intkey 路径无调用者；保留会让 decodeFlags
   同时支持两种页类型并在热路径增加分支。约 -800 行。
   将来需要 intkey 语义可在 KV 层加 "固定 8 字节大端 key" wrapper。
6. `sqlite3BtreeIntegrityCheck`：签名带 sqlite3*（中断/进度回调）→ 改为
   `btreelite_integrity_check(btreelite_db*, ...)`，checkProgress 只查自有中断标志。
   checkTreePage 需 KV 变体（校验 KV cell 尾部 value 段与 overflow 链一致性）。
7. `sqlite3BtreeCursor` 的 `KeyInfo*`：删除。`pCur->pKeyInfo` 字段删除，
   `pCur->curIntKey` 保留（KV 树恒 0）。`xRecordCompare` 机制删，
   比较统一为 memcmp。
8. `saveCursorKey` 的 index 分支（malloc 拷贝 key）：保留，正是 KV 树用的。
9. `sqlite3BtreeGetMeta/UpdateMeta`：保留（free-page-count、user_version 等头部管理）。
10. `SQLITE_HAS_CODEC` 钩子：btree/pager 本身无 codec 直接引用，无需处理。
11. `sqlite3ConnectionBlocked`：shared-cache 专用，删。
12. `sqlite3TempInMemory(db)`：删。`newDatabase()` 的 autovacuum 写入分支随
    SQLITE_OMIT_AUTOVACUUM 删除。
13. `btreeCursor` 断言 `hasSharedCacheTableLock`：删。
14. `invalidateIncrblobCursors` + `sqlite3BtreePutData/IncrblobCursor/PayloadChecked`：
    **保留**。改名为 `btreelite_value_write_partial` —— KV 大 value
    原子增量写的天然接口。
15. `sqlite3BtreeTripAllCursors`：保留（rollback 需要 trip 所有 cursor）。
16. `sqlite3BtreeCursorHint/HasHint`：删（hint 系统仅 COMDB2 用）。
17. `newDatabase()`：保留，魔串换 btreelite 自己的。
18. `sqlite3BtreeInsert` 重构为 `btreelite_put`：overwrite 快路径
    （`btreeOverwriteCell`）条件改为 `nKeyBytes 相同 && nValueBytes 相同`；
    其余 dropCell+insertCell 路径不变。`BtreePayload` 保留结构，
    `pKey/nKey` + `pData/nData` → payload = key+value 拼接。

### pager.c

1. `pPager->pBackup` → no-op（backup.c 不带走）。`sqlite3PagerBackupPtr` 删除，
   `sqlite3BackupRestart/Update` 变 no-op 宏。
2. `sqlite3PagerCheckpoint` 里 `sqlite3_exec(db, "PRAGMA table_list")` hack：删。
3. `sqlite3PagerCheckpoint(db)` / `sqlite3WalCheckpoint(db)` 的 sqlite3* 参数：
   改为 `btreelite_db*`（仅用于中断标志位，替代 `db->u1.isInterrupted`）。
   中断机制：提供 `btreelite_set_interrupt(btreelite_db*)` 入口。
4. busy handler 通用化（`sqlite3PagerSetBusyHandler` 接口保留，回调由 btreelite 层提供）。
5. `sqlite3PagerReadFileheader`（btree 读 100 字节头）保留。
6. `Pager.xReiniter` = `pageReinit`（btree 提供）保留。nExtra = sizeof(MemPage)
   机制保留 —— MemPage 作为页 extra 空间随 PgHdr 分配。
7. `pPager->subjInMemory`（上层传 `sqlite3TempInMemory`）→ 固定常量。
8. temp-db 分支保留 —— 内存树（BTREE_MEMORY）本质就是 temp-file pager，
   WAL/锁全 no-op，服务 `btreelite_open(NULL)`。
9. super-journal（多文件事务）相关：`writeSuperJournal/readSuperJournal/
   pager_delsuper/setSuper` 删除，`sqlite3PagerCommitPhaseOne(zSuper)` 恒 NULL。
10. `SQLITE_TEMP_STORE` / temp_store_directory 逻辑删除。

### wal.c

1. `sqlite3*db` 参数（interrupt 检查 `db->u1.isInterrupted`）→ `btreelite_db*`。
2. `walBusyLock` 的 xBusy 回调：保留接口，由 btreelite 层传入。
3. SEH（Windows）代码：保留（`#ifdef SQLITE_USE_SEH`，Windows 非目标，不编）。
4. `walEnableBlocking` 的 `pWal->db->setlkTimeout` → 删
   （`SQLITE_ENABLE_SETLK_TIMEOUT` 不启用）。
5. snapshot API（`sqlite3_snapshot_*`）：**保留** —— 纯 WAL 机制，零 SQL 依赖，
   为 btreelite 提供 MVCC 读快照能力。

### pcache / pcache1 / os_unix.c

基本无 SQL 耦合。`sqlite3GlobalConfig.pcache2` 间接层改为编译期直接绑定
pcache1 函数表，`sqlite3PcacheSetDefault` 删。os_unix.c 删 proxy-lock、
dot-file lock 等不需要的 VFS 变体（可选精简）。`SQLITE_FCNTL_BUSYHANDLER`
file-control 保留。

---

## 5. 模块抽取清单

| 源文件 | 处置 | 说明 |
|---|---|---|
| btree.c (11589) | 移入 + 改造 | 按第 4 节清单删 SQL 耦合；预计降至 ~7000 行 |
| btreeInt.h | 移入 | 删 BtLock/READ_LOCK/WRITE_LOCK、Schema*、autovacuum 宏 |
| btmutex.c | **删除** | shared cache 连同 mutex 逻辑一并删；Enter/Leave 变 no-op |
| pager.c (7880) | 移入 + 改造 | 删 super-journal、backup hooks、temp_store |
| pager.h | 移入 + 改造 | 同上 |
| wal.c (4645) | 移入 + 微改 | `sqlite3*` 参数 → btreelite_db* |
| wal.h | 移入 + 微改 | 同上 |
| pcache.c / pcache.h / pcache1.c | 移入 | `sqlite3GlobalConfig` 间接层 → 直接函数调用 |
| os.c / os.h / os_unix.c / os_common.h / os_setup.h | 移入 | 删 proxy/dotfile 锁变体（可选） |
| mutex.c / mutex.h / mutex_unix.c | 移入 | btree mutex + pcache1 LRU mutex |
| malloc.c / mem1.c / mem0.c | 移入 | sqlite3_malloc 家族 + BenignMalloc |
| util.c | 移入 | 只保留 Get4byte/Put4byte/varint 编解码/StrAccum 等 |
| bitvec.c | 移入 | pInJournal/savepoint bitvec |
| random.c | 移入 | WAL salt 用 |
| status.c | 移入（简化） | malloc 统计 |
| memjournal.c | 移入 | 内存 journal + spill |
| rowset.c | **删除** | 仅 SQL 层用 |
| hash.c | **删除** | 仅 schema 用 |
| threads.c | **删除** | 仅 VDBE sorter 用 |
| vdbe*.c / where*.c / select.c 等全部 SQL 层 | 不抽取 | — |

---

## 6. 事务/并发语义（原样继承）

- **事务**：`btreelite_begin/commit/rollback` 直接映射
  `sqlite3BtreeBeginTrans/Commit/Rollback`。`CommitPhaseOne/Two` 原样保留
  （PhaseOne 的 zSuperJrnl 恒 NULL）。
- **savepoint**：`btreelite_savepoint` → `sqlite3BtreeSavepoint` + pager
  savepoint，原样。
- **WAL**：保留全部能力：journal_mode 切换、
  `btreelite_checkpoint`（PASSIVE/FULL/RESTART/TRUNCATE）、
  auto-checkpoint 由 commit 后 btreelite 层计数触发
  （`sqlite3PagerWalCallback` 机制保留）。
- **锁**：POSIX advisory 锁原样。跨进程并发、crash recovery
  （hot-journal 回放 + WAL recovery）免费继承。
- **多树**：同一文件内多棵树共享一个 BtShared/Pager/事务。
  多树写并发由单写事务序列化（SQLite 语义，接受）。

---

## 7. 文件布局

```
btreelite/
  include/
    btreelite.h          /* 公共 API（薄封装声明） */
  src/
    btreelite.c          /* API 实现，薄封装层，~600 行 */
    btreeliteInt.h       /* 内部类型（原 sqliteInt.h 的存储子集） */
    btree.c / btreeInt.h
    pager.c / pager.h
    wal.c / wal.h
    pcache.c / pcache.h / pcache1.c
    os.c / os.h / os_unix.c / os_common.h / os_setup.h
    mutex.c / mutex.h / mutex_unix.c
    malloc.c / mem1.c / mem0.c
    util.c / bitvec.c / random.c / status.c / memjournal.c
  test/
    t_basic.c            /* CRUD / 遍历 / seek */
    t_overflow.c         /* 大 value、overflow 链 */
    t_rebalance.c        /* 分裂 / 合并 / freelist 复用 */
    t_txn.c              /* 事务 / savepoint */
    t_wal.c              /* WAL / checkpoint / crash 恢复 */
    t_concurrent.c       /* 多进程锁 */
    t_integrity.c
  Makefile
  README.md
```

---

## 8. 分阶段落地计划

**Phase 0：骨架与编译验证（1 天）**
- 建 `src/`：拷贝选定文件，新建 `include/btreelite.h`（公共 API）。
- 新建 `src/btreeliteInt.h`：从 sqliteInt.h 抽出 btree/pager/wal 需要的类型
  （u8/u16/u32/i64/Pgno/Bitvec/BusyHandler），`sqlite3Config` → `btreeliteConfig`。
- 定义 `SQLITE_OMIT_*` 宏族（AUTOVACUUM、SHARED_CACHE、TRIGGER、VACUUM、
  INCRBLOB 保留、WAL 保留、INTEGRITY_CHECK 保留）。
- 目标：先让"未改任何 .c"的文件集合编译通过（允许大量未解决符号），
  摸清 SQL 耦合面，产出符号清单驱动后续阶段。

**Phase 1：pager 层脱 SQL（2-3 天）**
- 删 backup/super-journal/PRAGMA hack，`sqlite3*` 参数改 btreelite_db*。
- busy handler 通用化。
- 编译通过 + 单元测试：创建文件、写页、hot-journal 回放、savepoint。

**Phase 2：btree 层改造（3-5 天）**
- 按第 4 节清单删 SQL 耦合，KeyInfo/UnpackedRecord 依赖解除。
- 实现 KV cell 格式：新 parse/size 变体 + decodeFlags KV 分支 + memcmp 统一比较。
- `sqlite3BtreeInsert` 重构为 `btreelite_put`：保留 overwrite 快路径。
- 编译通过 + 针对性测试：树分裂、大 value overflow 链、
  删除至 underfull rebalance、freelist 复用。

**Phase 3：WAL 接缝（1-2 天）**
- `sqlite3*` 参数替换、busy 回调接 btreelite busy handler。
- 测试：WAL 模式写 + checkpoint、crash 恢复（kill -9 模拟）。

**Phase 4：API 层 + 测试（2-3 天）**
- `btreelite.h` API 实现（薄封装）。
- 测试矩阵：
  - 功能：CRUD、遍历、seek、多树、大 value（>page、>1MB）、多树事务、
    savepoint、integrity check
  - 恢复：hot-journal 回放、WAL crash recovery、checkpoint TRUNCATE
  - 并发：双进程锁互斥、WAL 多 reader 单 writer、busy 超时
  - 耐久：synchronous=FULL/NORMAL 混跑、mmap read 路径
- 基准：KV 负载下与原 SQLite (SQL) 路径做读写吞吐对比
  （预期 KV 略快：无记录编码/解码、无 SQL 层开销）。

**Phase 5：清理与文档（1 天）**
- 公共符号 btreelite_ 前缀（内部 sqlite3 前缀保留）。
- 删 test*.c 测试钩子附件（SQLITE_TEST 分支不编入默认 build）。
- 更新 README。

总计约 **10-14 个工作日**。Phase 0-1 产出可编译的 pager 底座；Phase 2 是主战场。

---

## 9. 风险清单

| 风险 | 缓解 |
|---|---|
| KV cell 格式对 balance 的隐性依赖（divider 处理的分支假设） | KV divider = index divider（无 value），leafData 路径不走；Phase 2 用树分裂/合并压力测试覆盖。风险：中 |
| decodeFlags 校验放宽导致的损坏文件误读 | KV 模式下只接受 0x02/0x0A 两种页类型，其余一律 CORRUPT；魔串隔离保证 SQLite 库不会误开 btreelite 文件（反之亦然） |
| btreeComputeFreeSpace 对 KV cell 的 nFree 校验 | 该函数只走 freeblock 链，不解析 cell —— 无影响；integrity check 在 Phase 4 之前实现，作为 KV 格式正确性第一道验证 |
| sqlite3BtreeInsert overwrite 快路径的长度判断 | 条件改为 nKeyBytes 相同 && nValueBytes 相同，否则走 dropCell+insertCell |
| PENDING_BYTE 页跳过 | 原样保留（锁字节区），btreelite 文件同样不能占用锁页 |
| 删除至 underfull → 树收缩（copyNodeContent 路径） | 历经百万级测试验证的代码，原样保留 |
| SQLITE_DIRECT_OVERFLOW_READ 优化路径读 KV payload | 该路径直接从文件读 overflow 页，与页类型无关，无影响 |
| balance_nonroot 的 secure_delete 路径复制 divider cell | BTS_FAST_SECURE 行为与 cell 类型无关，无影响 |

---

## 10. 设计判断摘要

- **cell 格式**：index cell + value 段；payload = [key][value] 连续存储。
- **balance 算法不动**：KV divider 与 index divider 完全同构。
- **intkey 路径删除**：调用者消失；将来可用 KV wrapper（8 字节大端 key）实现。
- **内部符号保留 sqlite3 原名**：与上游对照/rebase 能力优先，公共 API 层做唯一前缀。
- **btree.c 预计从 11589 行降到 ~7000 行**，整体代码量约为原 SQLite 存储栈的 55-60%。