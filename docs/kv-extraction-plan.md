# litebtree：从 SQLite btree 提取 KV 引擎 — 实现计划

目标快照：`sqlite-snapshot-202607312245`（SQLite 3.54.0，public domain）
输出：一个纯 KV 存储引擎（项目名 litebtree，库前缀 litebtree_），复用原版
btree/pager 机制，剥离 SQL 层。

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
   - 方案：定义极小 `lb_db` 结构（含 mutex、flags、busyHandler 字段），btree 源码里 `sqlite3` → `lb_db` 类型别名
5. **pager 无 SQL 概念**，纯块层。依赖链：pager.c → pcache.c/pcache1.c/memjournal.c/bitvec.c/os_*.c/malloc/mutex/util/printf/random。WAL 保留（用户要求）→ wal.c 一并带走。
6. cell payload 对 btree **不透明**（`BtreePayload` 传指针+长度），页布局不含 record 格式。

## 2. 目标架构

```
litebtree/
  include/litebtree.h        # 公共 KV API（唯一对外头文件）
  src/
    lbConfig.h               # 编译开关（唯一配置头）
    lbInt.h                  # 内部类型/宏/结构（sqliteInt.h 的最小替代）
    lbDb.h / lb_db.c         # lb_db shim：极小连接对象
    lb_init.c                 # 最小 sqlite3_initialize（全局 mutex/内存/vfs 注册）
    lb_api.c lb_ops.c        # facade：KV API → btree 调用翻译（全新代码）
    lb_record.c              # record 编解码（全新代码）
    lb_compare.c             # memcmp comparator（替代 KeyInfo/RecordCompare）
    lb_catalog.c（可选）      # 命名树目录
    lb_amalgam.c（可选）      # 仿 sqlite3.c 拼接，方便单文件分发
    btree.c btmutex.c btreeInt.h   # 原样抽取（仅做 §4 手术）
    pager.c wal.c wal.h pcache.c pcache1.c memjournal.c bitvec.c rowset.c
    os.h os.c os_common.h os_unix.c os_kv.c memdb.c
    malloc.c mem0.c mem1.c mem2.c mutex.c mutex_unix.c mutex_noop.c
    util.c printf.c random.c threads.c utf.c hash.c status.c global.c fault.c
  scripts/
    extract.sh manifest.tsv  # 行号驱动的代码抽取（见 §7.0）
  test/
    pager_test.c btree_test.c lb_test.c stress.c
  Makefile / CMakeLists.txt
```

预计规模：约 3.2 万–3.6 万行 C（对比 sqlite3.c 27 万行）。

## 3. 公共 API（lb_api.c 实现，C 接口）

- 编译产物：`liblitebtree.a` / `liblitebtree.so`（macOS `.dylib`），无第三方依赖
- 公共符号前缀：`litebtree_`（API 函数）、`LITEBTREE_`（宏/错误码）
- 内部符号前缀：`lb_`（源文件与内部结构，不对外）
- 公共头文件：`include/litebtree.h`（唯一对外头文件）

```c
/* 错误码直接沿用 SQLITE_OK/CORRUPT/NOMEM/... （数值不变，语义不变） */

typedef struct litebtree_store litebtree_store;    /* = Btree 句柄 */
typedef struct litebtree_cursor litebtree_cursor;  /* = BtCursor（用户按 litebtree_cursor_size() 分配） */

/* --- 开关库 --- */
int litebtree_open(const char *zFilename, litebtree_store **ppStore, unsigned flags);
  /* flags: LITEBTREE_OPEN_READONLY / LITEBTREE_OPEN_MEMORY / LITEBTREE_OPEN_NOMUTEX / LITEBTREE_OPEN_CREATE */
int litebtree_close(litebtree_store*);

/* --- 事务 --- */
int litebtree_begin(litebtree_store*);               /* deferred；可加 litebtree_begin_immediate/exclusive */
int litebtree_commit(litebtree_store*);
int litebtree_rollback(litebtree_store*);
/* savepoint 继承 sqlite3BtreeSavepoint，先不暴露，留内部 */

/* --- 树管理（多树） --- */
int litebtree_tree_create(litebtree_store*, unsigned *pRoot);   /* 返回 root page 号 */
int litebtree_tree_drop(litebtree_store*, unsigned root);
/* 16 个 meta 槽（user_version 等）暴露 2-3 个常用的 */

/* --- KV 操作（cursor-based，贴近 btree 原生） --- */
int litebtree_cursor_open(litebtree_store*, unsigned root, litebtree_cursor**);   /* write cursor 默认 */
int litebtree_cursor_close(litebtree_cursor*);

int litebtree_put(litebtree_cursor*, const void *k, int nk, const void *v, int nv);
int litebtree_get(litebtree_cursor*, const void *k, int nk, void *buf, int cap, int *pnOut);
  /* 语义：不存在 → LITEBTREE_NOTFOUND；值>cap → LITEBTREE_TOOBIG（返回实际大小） */
int litebtree_del(litebtree_cursor*, const void *k, int nk);
int litebtree_first(litebtree_cursor*), litebtree_last(litebtree_cursor*);
int litebtree_next(litebtree_cursor*), litebtree_prev(litebtree_cursor*);
int litebtree_seek(litebtree_cursor*, const void *k, int nk);        /* 定位到 >=k 的首条 */
  /* 内部走 IndexMoveto；pRes>0 时即"第一个大于 k 的项"（LMDB set_range 语义） */

int litebtree_key(litebtree_cursor*, const void **pk, int *pn);      /* 零拷贝读 key */
int litebtree_value(litebtree_cursor*, const void **pv, int *pn);    /* 零拷贝读 value（页内直读，溢出页时走 accessPayload） */

/* --- 维护 --- */
int litebtree_integrity_check(litebtree_store*, char **pzMsg);       /* 继承 IntegrityCk */
int litebtree_checkpoint(litebtree_store*, int eMode);               /* WAL checkpoint */
```

**KeyInfo shim（lb_compare.c）**：构造一个 `KeyInfo` 形状兼容的静态结构：
- `nKeyField=1, nAllField=1, aSortFlags={0}(ASC), aColl[0]=指向 {xCompare=binaryCompare}` 的伪 CollSeq
- `db` 字段指向 lb_db shim
比较逻辑：`btreeMoveto`/IndexMoveto 拿到的是 record 格式 key，因此 **facade 在 put/seek 前把原始字节 key 编码成单字段 BLOB record**：`[hdr_len=2][serial_type≥13 奇数=blob 长度 varint][raw bytes]`，长度 = 2 + varint_len(nk) + nk。value 同样编码进同一 record 尾部（index 树 key 即全部内容，因此 KV 条目 = record{key,value} 两字段）。
> 修正（重要）：BTREE_BLOBKEY 树"key 即内容"没有独立 value。KV 的 value 要放进 record：record = [field0=key blob][field1=value blob]。比较时 xRecordCompare 只比较 field0（nKeyField=1），field1 不参与排序。这正是 SQLite index 的 (key-column, rowid) 排法——field1 用 nKeyField 边界自然排除。

## 4. 对抽取代码的手术清单（btree.c 预计 ≤120 行改动）

| 位置 | 改动 |
|---|---|
| 头部 include | sqliteInt.h → 新 `lbInt.h`（最小类型集：u8..u64/Pgno/i64/sqlite3*-alias/宏） |
| `sqlite3` 类型 | `#define sqlite3 lb_db`（或 typedef），lb_db 只含 mutex/flags/busyHandler/mallocFailed/pVfs |
| mutex | `sqlite3MutexAlloc` → 保留，mutex_unix.c 原样带走；`SQLITE_MUTEX_STATIC_*` 静态互斥初始化搬进 litebtree_open（原 main.c 的 sqlite3_initialize 职责） |
| 内存 | malloc.c/mem0.c 原样带走，`sqlite3_initialize()` 的最小版本进 litebtree_open |
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
| include | 同上换 lbInt.h |
| `sqlite3Os*` | os.h/os.c 原样保留，无需改 |
| `sqlite3Wal` 接口 | wal.c/wal.h 原样保留（用户要求保留 WAL） |
| busy handler 回调 | pager→os 层的 busy 机制保留（sqlite3OsFileControl/lock 重试） |
| super-journal | 保留（多文件未来兼容；单文件时是 no-op 开销） |
| **[审查新增] `sqlite3PagerCheckpoint` 里的 `sqlite3_exec(db,"PRAGMA table_list",...)`**（66391 行附近） | **必须改**：那是空-WAL 库首次 checkpoint 的冷路径 hack，直接调 SQL。改为直接调用 sqlite3PagerOpenWal+BeginTrans 等价物或删掉该分支（pWal==0 且 journalmode==WAL 时先开一次事务） |
| **[审查新增] wal.c 的 `db->setlkTimeout`**（68741） | lb_db 里补 `setlkTimeout` 字段（blocking locks 开关，POSIX VFS 的 SQLITE_FCNTL_LOCK_TIMEOUT）。不补则 walEnableBlocking 永远走 no-op，功能退化但可用；为完整性补上 |
| **[审查新增] wal.c checkpoint 里的 `db->u1.isInterrupted` / `db->mallocFailed`**（69008） | lb_db 补 `u1.isInterrupted` 位 + mallocFailed；litebtree_interrupt() API 可选暴露 |
| **[审查新增] `sqlite3BtreeIntegrityCheck`/`sqlite3BtreeCount` 签名里的 `sqlite3_value*/Mem*`**（83595, 83698 `sqlite3MemSetArrayInt64`） | facade 不暴露带 aCnt 的重载；facade 用 IntegrityCk 时传 null/封装。sqlite3_value 依赖仅此一处，lbInt.h 里给 8 字节占位类型 |
| **[审查新增] StrAccum（printf.c 的 sqlite3_str_*/sqlite3StrAccum*）** | integrity_check 错误消息需要，printf.c 本来就在带走清单里，确认覆盖 |

## 5. 文件格式（新 magic）

- 改 `SQLITE_FILE_HEADER` 为 `"litebtree  fmt"`（15 字符+`\0`，btreeInt.h 71615）
- 100 字节 file header 布局**原样保留**（page size/freelist/meta 槽/版本号全部不动）
- meta 槽使用约定（写入文档）：槽 0=free page count（引擎自用），槽 4=largest root page（autovacuum 用，引擎自用），槽 6=user_version → **KV 元数据区**（树数、根页表位置），槽 8=application_id
- **树目录实现**：facade 维护一棵专用 BLOBKEY 树（root page 存于 file header 槽 6/或 header 72-91 unused 20 字节存 root page 号）。`litebtree_tree_create/drop` 在目录树中登记 `tree_name → root_page`；`litebtree_open` 时读目录校验
  - 简化替代（若 facade 想再薄）：不搞命名目录，直接暴露 root page 号当"树 ID"，元数据由上层自管。**默认取简化替代**，命名目录树作为可选模块 `lb_catalog.c`
- 与 sqlite3 CLI 不兼容：magic 不同，CLI 会报 "file is not a database"（预期行为）

## 6. 事务语义（直接继承，无需新写）

- `litebtree_begin/commit/rollback` → `sqlite3BtreeBeginTrans/Commit/Rollback`
- 崩溃恢复：rollback journal + WAL 机制原样生效（hot journal 自动回滚、WAL 帧校验）
- savepoint：`sqlite3BtreeBeginStmt/Savepoint` 已在 btree 内，facade 暂不暴露，API 留桩
- 多进程并发：文件锁（os_unix.c 原样）、busy handler、`PENDING_BYTE` 防饿死机制全部继承

## 7. 实施步骤（阶段化，每阶段可编译可验证）

### 7.0 抽取机制（先建立，供所有阶段复用）

amalgamation 的文件边界是显式标注的（`/************** Begin file xxx.c ***/`
与 `End of xxx`），所以抽取是纯机械操作。先写一个清单驱动的抽取脚本，避免手工
sed 出错、且可重放（换 SQLite 版本时只需更新行号清单）：

```
scripts/
  extract.sh        # 按 manifest.tsv 逐行: <dest_file> <begin_line> <end_line>
  manifest.tsv      # 每行一个抽取项，行号对应当前快照，注释列写用途
```

操作约定：
- 每个抽取文件**头部加一行** `/* extracted from sqlite3.c @ 3.54.0, do not hand-merge */`
- 抽取后立刻 `clang-format`（原版风格是 3 空格缩进，配置照抄 sqlite 源）
- **不理解的代码不删**，只用宏开关关闭；每处删改都在 git 独立 commit，可回溯
- 编译统一开关（`src/lbConfig.h`，唯一配置头）：

```c
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_DEPRECATED   1
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#define SQLITE_OMIT_GET_TABLE    1
#define SQLITE_OMIT_TRACE        1
#define SQLITE_OMIT_AUTORESET    1
#define SQLITE_THREADSAFE 1          /* 多线程要求 */
#define SQLITE_MAX_EXPR_DEPTH 0
#define SQLITE_ENABLE_EXPLAIN_COMMENTS 0
/* 注意：不定义 SQLITE_OMIT_WAL、不定义 SQLITE_OMIT_AUTOVACUUM */
```

---

### Phase 1: 基础子系统（目标：`liblitebtree.a` 可编译链接）

**抽取清单**（行号来自 sqlite3.c @ 3.54.0，写进 manifest.tsv）：

| 源文件 | sqlite3.c 行区间 | 目标 |
|---|---|---|
| sqliteLimit.h | 14774-15072 | src/lbLimit.h |
| hash.h | 15490-15589 | 并入 lbInt.h |
| os.h | 16585-16907 | src/os.h |
| pager.h | 16910-17175 | src/pager.h |
| btree.h | 17178-17615 | 并入 lbInt.h |
| pcache.h | 18304-18496 | src/pcache.h |
| vdbeInt.h（仅 typedef/宏片段） | 24466-25221 | 只摘 Mem/sqlite3_value 布局到 lbInt.h |
| os_common.h | 23112-23213 | src/os_common.h |
| global.c | 24034-24447 | src/global.c |
| status.c | 24448-25653 | src/status.c |
| os.c | 27538-27987 | src/os.c |
| fault.c | 27988-28077 | src/fault.c |
| mem0.c | 28078-28139 | src/mem0.c |
| mem1.c | 28140-28433 | src/mem1.c |
| mem2.c | 28434-28964 | src/mem2.c（SQLITE_MEMDEBUG 才用，可先不编） |
| mutex.c | 30243-30628 | src/mutex.c |
| mutex_noop.c | 30629-30846 | src/mutex_noop.c |
| mutex_unix.c | 30847-31262 | src/mutex_unix.c |
| malloc.c | 31694-32594 | src/malloc.c |
| printf.c | 32595-34326 | src/printf.c |
| random.c | 35658-35817 | src/random.c |
| threads.c | 35818-36095 | src/threads.c |
| utf.c | 36096-36695 | src/utf.c（record 序列化用） |
| util.c | 36696-38960 | src/util.c |
| hash.c | 38961-39236 | src/hash.c |

**工作项**：
1. 写 `scripts/manifest.tsv`（上表）+ `scripts/extract.sh`，执行抽取
2. 写 `src/lbInt.h`：基础 typedef（u8/u16/u32/u64/i64/Pgno/DbPage/...）、
   SQLITE_OK 等错误码（从 sqlite3.h 321-14756 区间摘）、get2byte/put2byte/
   get4byte/put4byte/getVarint 宏、MIN/MAX/ROUND8 等工具宏
3. 写 `src/lbConfig.h`（上节开关）+ `src/lbDb.h`：lb_db 结构第一版
   （字段：mutex, flags, busyHandler, busyTimeout, pVfs, szMmap, mallocFailed,
   setlkTimeout, u1.isInterrupted, nSavepoint, xAutovacPages/pArg——按
   btree/pager/wal 实际引用清单填充，§4 已列）
4. `typedef struct lb_db sqlite3;` 放 lbDb.h，被所有抽取文件包含
   （抽取文件原 `#include "sqliteInt.h"` 一律替换为 `#include "lbInt.h"`）
5. 写 `src/lb_init.c`：最小 `sqlite3_initialize()`——从 main.c（187170-192528）
   抄全局初始化骨架：sqlite3MutexInit、sqlite3MallocInit、sqlite3PcacheInit、
   status 初始化；其余 feature 分支全删
6. Makefile：`-Isrc -Iinclude`，`-DSQLITE_CORE`，先编 `liblitebtree.a`

**验收**：
- `make base` 编译零错误零警告（`-Wall -Wextra`）
- 链接一个 smoke test：初始化 → 分配/释放内存 → 生成随机数 → 退出
- `nm liblitebtree.a | grep -E ' T ' | wc -l` 与 manifest 符号预期对照

---

### Phase 2: pager + 存储层（目标：可在 KV 文件上读写页）

**抽取清单**：

| 源文件 | 行区间 | 备注 |
|---|---|---|
| os_setup.h / os_dep 部分 | 16613-16706 等 | os.h 的组成段，并入 src/os.h |
| os_kv.c | 39448-40547 | 保留（内存库 fallback 有用） |
| os_unix.c | 40548-49132 | 唯一真 VFS |
| memdb.c | 54480-55419 | 内存库路径 |
| bitvec.c | 55420-55917 | |
| pcache.c | 55918-56856 | |
| pcache1.c | 56857-58139 | |
| rowset.c | 58140-58644 | savepoint 用 |
| pager.h | 16910-17175 | 并入 lbInt.h 或独立 |
| pager.c | 58645-66699 | |
| wal.h | 58669-58831 | |
| wal.c | 66700-71347 | |

**工作项**：
1. 抽取上述文件，include 换 lbInt.h；编译修错（预期主要错误：
   `sqlite3_initialize` 引用、`sqlite3_str_*` 在 os 层的错误路径、
   `sqlite3_vfs_find`/`sqlite3_vfs_register`——都在 lb_init.c 补实现，
   从 main.c 摘 vfsList 管理 ~40 行）
2. `sqlite3PagerCheckpoint` 的 `sqlite3_exec("PRAGMA table_list")` 分支
   （66385-66393）改写：`pWal==0 && journalMode==WAL` 时改为直接调
   `sqlite3PagerOpenWal` 等价序列（§4 审查项）
3. wal.c：`db->setlkTimeout`/`u1.isInterrupted`/`mallocFailed` 字段补进
   lb_db（§4 审查项）；`sqlite3WalDb` 由 kv 层传入 lb_db
4. 写 `src/test/pager_test.c`（纯 C，无框架，assert 风格）

**验收**：
- `make pager && ./test/pager_test` 全绿，覆盖：
  a) 新建文件 → PagerOpen → Get page1 → Put → CommitPhaseOne/Two → reopen 读回
  b) 写事务中途模拟崩溃：fork 子进程写一半 kill -9，父进程 reopen 后
     校验数据回到事务前状态（rollback journal 恢复路径）
  c) journal_mode=WAL 打开 → 写 → checkpoint → 写 → 再校验
- `ls -la` 确认 -wal/-shm/-journal 文件行为与 sqlite 一致

---

### Phase 3: btree 层（目标：btree 全 API 可用，仍无 KV facade）

**抽取清单**：

| 源文件 | 行区间 | 备注 |
|---|---|---|
| btreeInt.h | 71367-72115 | 含 MemPage/BtShared/BtCursor/CellInfo 全部结构 |
| btmutex.c | 71348-72409 | |
| btree.c | 72410-84045 | 手术清单见 §4 |
| backup.c | 84046-84844 | 可选；在线备份 API 顺带继承 |

**btree.c 手术清单**（§4 展开，逐项独立 commit）：
1. include 替换 + lbConfig/lbDb 引入（1 commit）
2. `sqlite3BtreeIntegrityCheck`/`sqlite3BtreeCount` 的 `sqlite3_value*`→
   `void*` 占位（仅 83595/83698 两处引用 `sqlite3MemSetArrayInt64`，
   facade 不用此功能，直接改签名为内部计数缓冲）
3. `sqlite3PagerCheckpoint` 传入的 `p->db` 已是 lb_db（Phase 2 已处理）
4. SQLITE_DEBUG 分支里 `sqlite3ReportError`/`corruptPageError` 引用
   `sqlite3_mprintf`——printf.c 已在 Phase 1，确认链接
5. `db->xProgress`（83041-83045）：lb_db 留空函数指针，永不触发，保留代码
6. 编译错误驱动补 lbInt.h（预期集中在这几类：Schema 前置声明、
   DbPage 完整定义、sqlite3Pcache 符号——pcache.h 已抽取）

**工作项**：
1. 抽取 + 逐项手术（每项独立 commit，可 revert）
2. `src/test/btree_test.c`：直接调 sqlite3Btree* 测试

**验收**：
- `make btree && ./test/btree_test` 全绿：
  a) Open 内存库 → BeginTrans → CreateTable(INTKEY) → Insert 10w 条 →
     First/Next 遍历序正确 → Delete → IntegrityCheck 通过
  b) 同上 BLOBKEY 路径（用临时 KeyInfo+record 比较器——此阶段允许用
     简陋 record 格式，Phase 4 会替换为正式版）
  c) 溢出页：插入 >maxLocal 的 payload（默认 4096 页约 >1012B 触发），
     读回完整
  d) 多树：create 5 棵树独立读写互不干扰
  e) 事务回滚：insert 后 rollback，reopen 确认数据消失

---

### Phase 4: KV facade（目标：正式对外 API）

**全新代码**（不抽取，参考语义写新实现）：

| 文件 | 内容 | 预计行数 |
|---|---|---|
| include/litebtree.h | 公共 API（§3）+ 编译产物说明：`liblitebtree.a/.so`（macOS: `liblitebtree.dylib`） | ~200 |
| src/lb_api.c | litebtree_open/close/begin/commit/rollback/tree_create/tree_drop/cursor 生命周期 | ~400 |
| src/lb_ops.c | put/get/del/seek/first/last/next/prev 的 btree 映射 | ~500 |
| src/lb_record.c | record 编解码：encode(kv entry)→record bytes；decode(record)→key/value | ~200 |
| src/lb_compare.c | 伪 KeyInfo + 伪 CollSeq（binary compare）+ BINARY collseq 函数 | ~150 |
| src/lb_db.c | lb_db 生命周期、busy timeout 设置、mutex 初始化胶水 | ~150 |

**record 编码格式**（lb_record.c 实现，含单测）：
```
kv entry (key K, value V) → btree index cell payload:
  [hdr varint: header 总长(含自身)]
  [varint serial_type(K)] [varint serial_type(V)]
  [K raw bytes] [V raw bytes]
serial_type(blob n) = 12 + 2n + 1（奇数）
KeyInfo: nKeyField=1, nAllField=2 → 比较只到 field0(K)，V 不参与排序
```
关键映射（lb_ops.c）：
- `litebtree_put` → `encode(k,v)` → `IndexMoveto(loc)` → `Insert(BtreePayload{pKey=record,nKey=len},0,loc)`
  （loc!=0 直接插，loc==0 时 insert 内部覆盖——upsert 一次调用完成）
- `litebtree_get` → `IndexMoveto(pRes)` → pRes==0 时 `PayloadFetch/Payload` 读 record → decode
- `litebtree_del` → `IndexMoveto` → pRes==0 → `BtreeDelete(BTREE_AUXDELETE)`
- `litebtree_seek` → `IndexMoveto`（default_rc=+1，UnpackedRecord 语义：找首个 ≥k）
  → pRes>0 时 `BtreeNext` 校正到下一条
- `litebtree_first/last` → `BtreeFirst/Last`；`litebtree_next/prev` → `BtreeNext/Previous`
- cursor 生命周期：`BtCursor` 按 `sqlite3BtreeCursorSize()` 分配（lb_api
  内部 malloc，对外句柄不透明——不做用户分配，简化生命周期管理）

**工作项**：
1. lb_record.c + 单测（编解码 round-trip、varint 边界 127/128/16383/16384、
   空 key、空 value、64KB key）
2. lb_compare.c + 单测：伪 KeyInfo 静态构造断言（offsetof 校验）、
   比较器对两条真实 record 的排序与 memcmp(key) 序一致
3. lb_api.c/lb_ops.c + 单测

**验收**（`./test/lb_test` 全绿）：
- put/get/del roundtrip；同 key 覆盖（upsert）；get 不存在返回 LITEBTREE_NOTFOUND
- 顺序遍历 == 按 memcmp(key) 升序（随机 1w 条 key 验证）
- seek 边界：seek 精确命中/未命中/超过末尾（应返回 LITEBTREE_NOTFOUND 且 cursor 置 EOF）
- 空 key ""、空 value ""、key 64KB（溢出页）、value 10MB（多级溢出链）
- 空库 first/last/next 返回 EOF；单条库 next/prev 边界
- 事务：begin 后 put → 第二连接不可见 → commit 后可见（WAL 与 journal 双模式各跑一遍）
- 二次打开：树 root page 稳定，数据完整

---

### Phase 5: 加固与发布

**工作项**：
1. `litebtree_integrity_check`：facade 包装 sqlite3BtreeIntegrityCheck（去掉
   aCnt 参数路径），对根页数组做全树校验；`src/test` 里人为破坏页数据
   （mmap 改字节）验证能报 CORRUPT
2. `litebtree_checkpoint(PASSIVE/FULL/RESTART/TRUNCATE)` + 单测：写满 WAL →
   checkpoint → wal 文件截断
3. 并发压测（`src/test/stress.c`）：
   - 多线程：8 线程 × 各自 cursor，4 写 4 读，断言无脏读、计数一致
   - 多进程：两个进程同时打开，writer 独占时 reader 用 busy timeout 等待，
     验证文件锁语义（继承 sqlite 行为：writer 阻塞 reader 仅在提交瞬间）
   - crash：脚本随机 kill -9 写进程（100 轮），每次 reopen 后
     integrity_check 通过 + 最后成功 commit 的数据存在
4. 元数据 API：`litebtree_user_version(get/set)`、`litebtree_tree_count`（meta 槽封装）
5. （可选）`lb_catalog.c`：命名树目录（内置专用树，name→root 映射，
   事务性 create/drop）；`lb_amalgam.c` 生成脚本（仿 sqlite 的 concat 顺序）
6. 构建完善：`make test` / `make valgrind`（memdebug 构建）/ 安装规则；
   CMakeLists.txt 双轨支持

**验收**：
- 三个压测脚本全绿且无 ASAN/UBSAN 报告（`-fsanitize=address,undefined` 跑全部单测）
- valgrind/memcheck 零泄漏零未初始化读（macOS 用 ASAN 替代）
- README 更新真实 API 与构建说明

**每阶段收尾硬性检查**：
- `lsp_diagnostics` 对全部改动文件干净
- git 历史按工作项粒度提交（手术清单每项独立 commit）
- manifest.tsv 行号与实际抽取核对（防止错位）

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| sqliteInt.h 依赖比预想深（宏/typedef 长尾） | lbInt.h 按"编译错误驱动"补齐；预计 300-600 行；不搬 schema/parse/vdbe 任何东西 |
| record 编码层性能损耗 | 每次操作一次 memcpy + varint；可加 fast path：key≤可用空间时直接指针拼接（BtreePayload 支持分片 nZero 不可用于 index 树，故仅做单 buffer 拼装） |
| KeyInfo.aColl 柔性数组对齐陷阱 | 伪 KeyInfo 用静态分配 + 正确 offsetof 计算，DEBUG 下用 assert 验证 |
| `db->flags` 位语义（如 SQLITE_ReadUncommit/BTS_*） | lb_db 里原位复制所需 flag 位定义 |
| WAL 与新 magic 共存 | wal.c 不读 file header magic（只做帧校验），无冲突；已确认 SQLITE_FILE_HEADER 只在 btreeInt.h 使用 |
| busy timeout 默认 0 | facade 默认设 5s（sqlite3BtreeSetPagerFlags/busyhandler 路径） |
| pager 对 `SQLITE_MAX_PAGE_SIZE` 等宏的长尾依赖 | sqliteLimit.h 原样并入 lbInt.h |
| **[审查新增] sqlite3_initialize() 的真实职责比"全局 mutex+内存"多** | main.c 的 sqlite3_initialize 还做：静态 mutex 分配、memstatus 开关、pcache/mutex 全局初始化、malloc 子系统自检。最小版按 malloc.c/mutex.c/pcache1.c 的全局初始化需求逐项搬，编译错误驱动 |
| **[审查新增] vdbesort.c 等"看起来无关"的模块其实不被引用** | 抽取范围以 btree/pager/wal 的符号闭包为准，用 ctags/nm 驱动，不做主观清单 |

## 8.5 审查结论（Momus 缺席，人工执行）

逐条对源码验证计划假设后的发现：

1. ✅ **record 编码方案成立**：UnpackedRecord 字段接触面（default_rc/eqSeen/errCode/nField）与计划一致
2. ✅ **upsert 语义成立**：index 树 insert loc==0 时走 btreeOverwriteCell（81947/81999）
3. ✅ **saveCursorKey 不依赖 record 解析**：只存原始字节（73134-73167）
4. ⚠️ **修正 4 处隐藏 SQL 耦合**（已并入第 4 节手术清单）：sqlite3PagerCheckpoint 的 sqlite3_exec hack、wal.c 的 setlkTimeout/isInterrupted/mallocFailed、IntegrityCheck 的 sqlite3_value 参数、StrAccum 依赖
5. ⚠️ **btree.c 手术量上修**：≤120 行 → 约 150-200 行（主要是 lb_db shim 的字段补齐），但仍是 shim 级
6. ✅ **WAL 与新 magic 无冲突**：SQLITE_FILE_HEADER 仅 btreeInt.h:71616 一处定义 + 72442 引用，wal.c 不读 magic
7. 结论：**计划可执行**，无阻断性缺陷。

## 9. 明确不做（out of scope）

- SQL/解析/vdbe/schema/trigger/视图/事务语义变更
- INTKEY 表树对外暴露（内部保留代码，facade 不开 API）
- 加密、压缩、自定义 collation
- 修改 btree/pager 算法逻辑（只做类型/接口 shim 级手术）