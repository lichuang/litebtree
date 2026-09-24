# btreelite：从 SQLite btree 提取 KV 引擎 — 实现计划

目标快照：`sqlite-snapshot-202607312245`（SQLite 3.54.0，public domain）
输出：一个纯 KV 存储引擎 `btreelite`，复用原版 btree/pager 机制，剥离 SQL 层。

## 0. 用户已确认的决策

| 决策点 | 选择 |
|---|---|
| key 类型 | **字节串**（memcmp 二进制序），底层用 BLOBKEY 索引树 |
| 树组织 | **多树**（一个文件多棵 btree，root page 句柄） |
| 文件格式 | **新 magic**（独立格式，避免被 sqlite3 CLI 误开） |
| pager | **保持与 SQLite 一致，不裁 WAL**（用户明确要求） |
| 并发 | 进程内多线程 + 跨进程文件锁（继承 sqlite 原生能力） |

## 1. 已确认的代码事实（第一手读码结论）

1. **btree.c 天然是 KV 引擎**。两种树：
   - `BTREE_INTKEY` 表树：i64 key + payload（行存表）
   - `BTREE_BLOBKEY` 索引树：key=blob，key 即内容，无独立 data（btree.h 17290-17301）
   - KV 语义选择：**用 BLOBKEY 索引树**。`sqlite3BtreeInsert`（81851）对 index 树接受 `pX.pKey/nKey` 且 `pData=0`——原生就是 put(key) 语义。
2. **无覆盖写 API 的缺失**：btree.c 的 `btreeOverwriteCell` 优化路径存在（81947/81999），index 树 insert 时 loc==0（同 key）即覆盖。**原生即 upsert**，无需额外补齐。
3. **SQL 耦合只有一处**：`btreeMoveto`（73280）把 packed record 交给 `sqlite3VdbeRecordUnpack`（vdbemem.c，依赖 KeyInfo/CollSeq/Mem/sqlite3*）→ `sqlite3BtreeIndexMoveto`（78478）用 `xRecordCompare` 比较。
   - `sqlite3BtreeIndexMoveto` 内部引用 UnpackedRecord 的字段仅：`default_rc`、`eqSeen`、`errCode`、`nField`（+`pKeyInfo` 经 pCur）。结构面极小。
   - `saveCursorKey`（73134）index 分支只是存原始 key 字节 + 17 字节 pad，不碰 record 格式。
4. **db 句柄引用可 shim**（btree/btmutex 内 71348-84045 行区间统计）：
   - `db->mutex` ~21 处、`db->flags` ~8、`db->aDb/nDb` 仅共享缓存路径、`xProgress/xAutovacPages/nVdbeRead/busyHandler/szMmap/pVfs` 零星
   - 共享缓存整体 `#ifdef SQLITE_OMIT_SHARED_CACHE` 剔除后 `aDb/nDb` 全部消失
   - 方案：定义极小 `kv_db` 结构（含 mutex、flags、busyHandler 字段），btree 源码里 `sqlite3` → `kv_db` 类型别名
5. **pager 无 SQL 概念**，纯块层。依赖链：pager.c → pcache.c/pcache1.c/memjournal.c/bitvec.c/os_*.c/malloc/mutex/util/printf/random。WAL 保留（用户要求）→ wal.c 一并带走。
6. cell payload 对 btree **不透明**（`BtreePayload` 传指针+长度），页布局不含 record 格式。

## 2. 目标架构

```
btreelite/
  include/btreelite.h        # 公共 KV API（唯一对外头文件）
  src/
    kv_api.c                 # facade：KV API → btree 调用翻译（全新代码）
    kv_db.c                  # kv_db shim：极小连接对象 + mutex/flags/busyhandler
    kv_compare.c             # memcmp comparator（替代 KeyInfo/RecordCompare）
    btree.c btmutex.c        # 原样抽取（仅做第 4 节列出的小改）
    pager.c wal.c pcache.c pcache1.c memjournal.c bitvec.c
    os_unix.c os.c os_common.h os_setup.h
    malloc.c mutex.c mutex_unix.c mutex_noop.c util.c printf.c random.c
    hash.c status.c threads.c utf.c (仅 btree/pager 触及的符号)
    kv_amalgam.c（可选）      # 仿 sqlite3.c 拼接，方便单文件分发
  Makefile / CMakeLists.txt
```

预计规模：约 3.2 万–3.6 万行 C（对比 sqlite3.c 27 万行）。

## 3. 公共 API（kv_api.c 实现，C 接口）

```c
/* 错误码直接沿用 SQLITE_OK/CORRUPT/NOMEM/... （数值不变，语义不变） */

typedef struct kv_store kv_store;    /* = Btree 句柄 */
typedef struct kv_cursor kv_cursor;  /* = BtCursor（用户按 btreelite_cursor_size() 分配） */

/* --- 开关库 --- */
int kv_open(const char *zFilename, kv_store **ppStore, unsigned flags);
  /* flags: KV_OPEN_READONLY / KV_OPEN_MEMORY / KV_OPEN_NOMUTEX / KV_OPEN_CREATE */
int kv_close(kv_store*);

/* --- 事务 --- */
int kv_begin(kv_store*);               /* deferred；可加 kv_begin_immediate/exclusive */
int kv_commit(kv_store*);
int kv_rollback(kv_store*);
/* savepoint 继承 sqlite3BtreeSavepoint，先不暴露，留内部 */

/* --- 树管理（多树） --- */
int kv_tree_create(kv_store*, unsigned *pRoot);   /* 返回 root page 号 */
int kv_tree_drop(kv_store*, unsigned root);
/* 16 个 meta 槽（user_version 等）暴露 2-3 个常用的 */

/* --- KV 操作（cursor-based，贴近 btree 原生） --- */
int kv_cursor_open(kv_store*, unsigned root, kv_cursor**);   /* write cursor 默认 */
int kv_cursor_close(kv_cursor*);

int kv_put(kv_cursor*, const void *k, int nk, const void *v, int nv);
int kv_get(kv_cursor*, const void *k, int nk, void *buf, int cap, int *pnOut);
  /* 语义：不存在 → KV_NOTFOUND；值>cap → KV_TOOBIG（返回实际大小） */
int kv_del(kv_cursor*, const void *k, int nk);
int kv_first(kv_cursor*), kv_last(kv_cursor*);
int kv_next(kv_cursor*), kv_prev(kv_cursor*);
int kv_seek(kv_cursor*, const void *k, int nk);        /* 定位到 >=k 的首条 */
  /* 内部走 IndexMoveto；pRes>0 时即"第一个大于 k 的项"（LMDB set_range 语义） */

int kv_key(kv_cursor*, const void **pk, int *pn);      /* 零拷贝读 key */
int kv_value(kv_cursor*, const void **pv, int *pn);    /* 零拷贝读 value（页内直读，溢出页时走 accessPayload） */

/* --- 维护 --- */
int kv_integrity_check(kv_store*, char **pzMsg);       /* 继承 IntegrityCk */
int kv_checkpoint(kv_store*, int eMode);               /* WAL checkpoint */
```

**KeyInfo shim（kv_compare.c）**：构造一个 `KeyInfo` 形状兼容的静态结构：
- `nKeyField=1, nAllField=1, aSortFlags={0}(ASC), aColl[0]=指向 {xCompare=binaryCompare}` 的伪 CollSeq
- `db` 字段指向 kv_db shim
比较逻辑：`btreeMoveto`/IndexMoveto 拿到的是 record 格式 key，因此 **facade 在 put/seek 前把原始字节 key 编码成单字段 BLOB record**：`[hdr_len=2][serial_type≥13 奇数=blob 长度 varint][raw bytes]`，长度 = 2 + varint_len(nk) + nk。value 同样编码进同一 record 尾部（index 树 key 即全部内容，因此 KV 条目 = record{key,value} 两字段）。
> 修正（重要）：BTREE_BLOBKEY 树"key 即内容"没有独立 value。KV 的 value 要放进 record：record = [field0=key blob][field1=value blob]。比较时 xRecordCompare 只比较 field0（nKeyField=1），field1 不参与排序。这正是 SQLite index 的 (key-column, rowid) 排法——field1 用 nKeyField 边界自然排除。

## 4. 对抽取代码的手术清单（btree.c 预计 ≤120 行改动）

| 位置 | 改动 |
|---|---|
| 头部 include | sqliteInt.h → 新 `kvInt.h`（最小类型集：u8..u64/Pgno/i64/sqlite3*-alias/宏） |
| `sqlite3` 类型 | `#define sqlite3 kv_db`（或 typedef），kv_db 只含 mutex/flags/busyHandler/mallocFailed/pVfs |
| mutex | `sqlite3MutexAlloc` → 保留，mutex_unix.c 原样带走；`SQLITE_MUTEX_STATIC_*` 静态互斥初始化搬进 kv_open（原 main.c 的 sqlite3_initialize 职责） |
| 内存 | malloc.c/mem0.c 原样带走，`sqlite3_initialize()` 的最小版本进 kv_open |
| random | random.c 带走（rollback journal magic 需要 `sqlite3_randomness`） |
| pager 调用 | 零改动（btree 不改，全部 sqlite3Pager* 原样保留） |
| `SQLITE_OMIT_SHARED_CACHE` | 定义之，剔除共享缓存代码路径 |
| `SQLITE_OMIT_AUTOVACUUM` | **不定义**——多树场景 root page 会移动（B,L 空间），保留 ptrmap 机制 |
| SQLITE_DEBUG/测试钩子 | 全部保留（DEBUG 宏不开即可） |
| btreeInt.h 的 BtLock/BtShared.pLock | 随 OMIT_SHARED_CACHE 消失 |
| `sqlite3BtreeSchema`/pSchema | 保留原函数（分配 0 字节），facade 不用 |

pager.c / wal.c 侧手术（预计 ≤120 行；审查发现的隐藏依赖）：
| 位置 | 改动 |
|---|---|
| include | 同上换 kvInt.h |
| `sqlite3Os*` | os.h/os.c 原样保留，无需改 |
| `sqlite3Wal` 接口 | wal.c/wal.h 原样保留（用户要求保留 WAL） |
| busy handler 回调 | pager→os 层的 busy 机制保留（sqlite3OsFileControl/lock 重试） |
| super-journal | 保留（多文件未来兼容；单文件时是 no-op 开销） |
| **[审查新增] `sqlite3PagerCheckpoint` 里的 `sqlite3_exec(db,"PRAGMA table_list",...)`**（66391 行附近） | **必须改**：那是空-WAL 库首次 checkpoint 的冷路径 hack，直接调 SQL。改为直接调用 sqlite3PagerOpenWal+BeginTrans 等价物或删掉该分支（pWal==0 且 journalmode==WAL 时先开一次事务） |
| **[审查新增] wal.c 的 `db->setlkTimeout`**（68741） | kv_db 里补 `setlkTimeout` 字段（blocking locks 开关，POSIX VFS 的 SQLITE_FCNTL_LOCK_TIMEOUT）。不补则 walEnableBlocking 永远走 no-op，功能退化但可用；为完整性补上 |
| **[审查新增] wal.c checkpoint 里的 `db->u1.isInterrupted` / `db->mallocFailed`**（69008） | kv_db 补 `u1.isInterrupted` 位 + mallocFailed；kv_interrupt() API 可选暴露 |
| **[审查新增] `sqlite3BtreeIntegrityCheck`/`sqlite3BtreeCount` 签名里的 `sqlite3_value*/Mem*`**（83595, 83698 `sqlite3MemSetArrayInt64`） | facade 不暴露带 aCnt 的重载；facade 用 IntegrityCk 时传 null/封装。sqlite3_value 依赖仅此一处，kvInt.h 里给 8 字节占位类型 |
| **[审查新增] StrAccum（printf.c 的 sqlite3_str_*/sqlite3StrAccum*）** | integrity_check 错误消息需要，printf.c 本来就在带走清单里，确认覆盖 |

## 5. 文件格式（新 magic）

- 改 `SQLITE_FILE_HEADER` 为 `"btreelite  fmt"`（15 字符+`\0`，btreeInt.h 71615）
- 100 字节 file header 布局**原样保留**（page size/freelist/meta 槽/版本号全部不动）
- meta 槽使用约定（写入文档）：槽 0=free page count（引擎自用），槽 4=largest root page（autovacuum 用，引擎自用），槽 6=user_version → **KV 元数据区**（树数、根页表位置），槽 8=application_id
- **树目录实现**：facade 维护一棵专用 BLOBKEY 树（root page 存于 file header 槽 6/或 header 72-91 unused 20 字节存 root page 号）。`kv_tree_create/drop` 在目录树中登记 `tree_name → root_page`；`kv_open` 时读目录校验
  - 简化替代（若 facade 想再薄）：不搞命名目录，直接暴露 root page 号当"树 ID"，元数据由上层自管。**默认取简化替代**，命名目录树作为可选模块 `kv_catalog.c`
- 与 sqlite3 CLI 不兼容：magic 不同，CLI 会报 "file is not a database"（预期行为）

## 6. 事务语义（直接继承，无需新写）

- `kv_begin/commit/rollback` → `sqlite3BtreeBeginTrans/Commit/Rollback`
- 崩溃恢复：rollback journal + WAL 机制原样生效（hot journal 自动回滚、WAL 帧校验）
- savepoint：`sqlite3BtreeBeginStmt/Savepoint` 已在 btree 内，facade 暂不暴露，API 留桩
- 多进程并发：文件锁（os_unix.c 原样）、busy handler、`PENDING_BYTE` 防饿死机制全部继承

## 7. 实施步骤（阶段化，每阶段可编译可验证）

### Phase 1: 骨架与 shim（可编译的空 KV 库）
1. 建目录骨架 + `kvInt.h`（最小类型/宏集，从 sqliteInt.h 摘取 btree/pager 实际用到的定义）
2. 抽取 malloc/mem0/mutex/mutex_unix/mutex_noop/util/printf/random/status/threads/utf/hash
3. 写 kv_db shim、最小 `sqlite3_initialize`（全局 mutex + 内存子系统初始化）
4. 验证：独立编译通过，`lsp_diagnostics` 干净

### Phase 2: pager 层接入
5. 抽取 os.h/os.c/os_unix.c/os_common.h/os_setup.h + bitvec/memjournal/pcache/pcache1
6. 抽取 pager.c + wal.c + wal.h，编译
7. 验证：单测 pager open/get/put/commit/rollback 循环 + 崩溃模拟（kill -9 后 reopen 校验）

### Phase 3: btree 层接入
8. 抽取 btmutex.c/btree.c/btreeInt.h（btree.h 内容并入 kvInt.h）
9. 应用手木清单（第 4 节），编译
10. 验证：btree 层直接单测——CreateTable/Insert/First/Next/Delete/IntegrityCheck，INTKEY 与 BLOBKEY 双路径

### Phase 4: KV facade
11. kv_compare.c：伪 KeyInfo/伪 CollSeq + 单字段 record 编解码器（put/get/seek 前后编码/解码）
12. kv_api.c：API 映射 + cursor 包装（注意 BTREE_SAVEPOSITION 与 cursor invalidation 语义透传）
13. 验证：KV 层单测——put/get/del/scan/seek/upsert/边界（空库、单条、超大 value→溢出页、key 长度上限）

### Phase 5: 加固
14. `kv_integrity_check` 暴露
15. WAL checkpoint API 暴露
16. 压测：多线程读写 + 多进程（文件锁验证）+ crash-recovery 随机 kill 测试
17. （可选）kv_catalog.c 命名树目录；kv_amalgam.c 单文件分发

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| sqliteInt.h 依赖比预想深（宏/typedef 长尾） | kvInt.h 按"编译错误驱动"补齐；预计 300-600 行；不搬 schema/parse/vdbe 任何东西 |
| record 编码层性能损耗 | 每次操作一次 memcpy + varint；可加 fast path：key≤可用空间时直接指针拼接（BtreePayload 支持分片 nZero 不可用于 index 树，故仅做单 buffer 拼装） |
| KeyInfo.aColl 柔性数组对齐陷阱 | 伪 KeyInfo 用静态分配 + 正确 offsetof 计算，DEBUG 下用 assert 验证 |
| `db->flags` 位语义（如 SQLITE_ReadUncommit/BTS_*） | kv_db 里原位复制所需 flag 位定义 |
| WAL 与新 magic 共存 | wal.c 不读 file header magic（只做帧校验），无冲突；已确认 SQLITE_FILE_HEADER 只在 btreeInt.h 使用 |
| busy timeout 默认 0 | facade 默认设 5s（sqlite3BtreeSetPagerFlags/busyhandler 路径） |
| pager 对 `SQLITE_MAX_PAGE_SIZE` 等宏的长尾依赖 | sqliteLimit.h 原样并入 kvInt.h |
| **[审查新增] sqlite3_initialize() 的真实职责比"全局 mutex+内存"多** | main.c 的 sqlite3_initialize 还做：静态 mutex 分配、memstatus 开关、pcache/mutex 全局初始化、malloc 子系统自检。最小版按 malloc.c/mutex.c/pcache1.c 的全局初始化需求逐项搬，编译错误驱动 |
| **[审查新增] vdbesort.c 等"看起来无关"的模块其实不被引用** | 抽取范围以 btree/pager/wal 的符号闭包为准，用 ctags/nm 驱动，不做主观清单 |

## 8.5 审查结论（Momus 缺席，人工执行）

逐条对源码验证计划假设后的发现：

1. ✅ **record 编码方案成立**：UnpackedRecord 字段接触面（default_rc/eqSeen/errCode/nField）与计划一致
2. ✅ **upsert 语义成立**：index 树 insert loc==0 时走 btreeOverwriteCell（81947/81999）
3. ✅ **saveCursorKey 不依赖 record 解析**：只存原始字节（73134-73167）
4. ⚠️ **修正 4 处隐藏 SQL 耦合**（已并入第 4 节手术清单）：sqlite3PagerCheckpoint 的 sqlite3_exec hack、wal.c 的 setlkTimeout/isInterrupted/mallocFailed、IntegrityCheck 的 sqlite3_value 参数、StrAccum 依赖
5. ⚠️ **btree.c 手术量上修**：≤120 行 → 约 150-200 行（主要是 kv_db shim 的字段补齐），但仍是 shim 级
6. ✅ **WAL 与新 magic 无冲突**：SQLITE_FILE_HEADER 仅 btreeInt.h:71616 一处定义 + 72442 引用，wal.c 不读 magic
7. 结论：**计划可执行**，无阻断性缺陷。

## 9. 明确不做（out of scope）

- SQL/解析/vdbe/schema/trigger/视图/事务语义变更
- INTKEY 表树对外暴露（内部保留代码，facade 不开 API）
- 加密、压缩、自定义 collation
- 修改 btree/pager 算法逻辑（只做类型/接口 shim 级手术）