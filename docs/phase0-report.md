# Phase 0 报告：骨架与编译验证

日期：2026-09-25
状态：**完成** —— 18/18 编译单元零错误编译，`libbtreelite.a`（macOS libtool 格式）
链接成功，最小链接测试程序（校验 `sqlite3PendingByte` / `sqlite3UpperToLower`）
运行通过。

## 1. 交付物

| 交付物 | 位置 | 说明 |
|---|---|---|
| 公共 API 头 | `include/btreelite.h` | btreelite_* 前缀，opaque db/cursor |
| 内部头 | `src/btreeliteInt.h` | sqliteInt.h 的存储子集（~1100 行） |
| 兼容垫片 | `src/sqliteInt.h` | Phase 4 删除；上游 .c 无缝编译的关键 |
| VDBE 空壳 | `src/vdbeInt.h` | 仅为 status.c 的已删 case 服务，Phase 2 删 |
| Makefile | `src/Makefile` | `make` 构建库；`make scan`/`census` 产出符号清单 |
| 库 | `libbtreelite.a` | 18 个 TU，~186KB |

## 2. 编译结果

```
malloc.o mem0.o global.o mem1.o status.o random.o
mutex.o mutex_unix.o bitvec.o util.o memjournal.o
os.o os_unix.o pcache.o pcache1.o pager.o wal.o btree.o   → 0 errors
/usr/bin/libtool -static -o libbtreelite.a *.o            → 链接成功
```

工具链注记：macOS 上必须用 `/usr/bin/libtool -static` 打包；homebrew 的
GNU binutils `ar` 写 GNU 符号表，系统 ld 报 `archive member '/' not a
mach-o file`。

## 3. Phase 0 期间做的源码修改（提前执行的 Phase 1/2 工作项）

原则是"不改 .c"，但两处深度 SQL 耦合文件按计划 §5 提前裁剪：

| 文件 | 修改 | 原因 | 上游行数 → 现行数 |
|---|---|---|---|
| util.c | 裁剪为纯数据助手（varint/4byte/比较/整数溢出运算/FaultSim） | 计划 §5 既定 | 2255 → ~530 |
| global.c | 保留 UpperToLower/CtypeMap/PendingByte/Config 单例；删 SQL 函数表、aLTb/aEQb/aGTb（OP_Ne 表）、sqlite3BuiltinFunctions | 同上 | 401 → ~290 |
| status.c | 删 SQLITE_DBSTATUS_SCHEMA_USED/STMT_USED/TEMPBUF_SPILL/DEFERRED_FKS 四个 case + vdbeInt.h include | 纯 SQL 统计 | 446 → ~330 |
| pcache1.c | `sqlite3_config(SQLITE_CONFIG_PCACHE2)` → `sqlite3Config.pcache2 = defaultMethods`（1 行） | 无 config 分发层 | — |
| pager.c | 删 `sqlite3_exec(db,"PRAGMA table_list")` hack（zero-page WAL 边缘用例，btreelite_checkpoint 显式开 WAL，路径不成立） | 计划 §4 pager.2 | — |

## 4. 外部符号清单（33 个，Phase 1-4 需实现）

`libbtreelite.a` 的全部未解析符号按去向分类：

**A. Phase 1/2 随功能删除而消失（桩，暂不实现）**
- `sqlite3BackupRestart/Update` — backup.c 不移植，Phase 1 变 no-op 宏
- `sqlite3TempInMemory/IsMemdb/WritableSchema` — SQL 层判定，Phase 2 删调用点
- `sqlite3InvokeBusyHandler` — Phase 1 API 层实现
- `sqlite3VdbeAllocUnpackedRecord/RecordUnpack/RecordCompare/FindCompare` —
  Phase 2 KV 改造核心点：memcmp 比较器替代
- `sqlite3Error/ErrorMsg` — malloc.c 的 OOM 传播路径用，Phase 2 简化

**B. util.c/printf.c 需补的函数体（Phase 1）**
- `sqlite3CorruptError/MisuseError/CantopenError/NomemError/IoerrnomemError/
  CorruptPgnoError/ReportError` — 上游 util.c 末段（裁剪时未带走），
  Phase 1 随 StrAccum/printf 一起移植
- `sqlite3_log`（printf.c）、`sqlite3BeginBenignMalloc/EndBenignMalloc`（malloc.c 内未编入部分）
- `sqlite3FaultSim`（测试钩子，util.c 已有主体，链接缺 = SQLITE_UNTESTABLE 裁掉了，Phase 1 保留定义）

**C. 基础实现（Phase 1 一次性补齐）**
- `sqlite3DefaultMutex/NoopMutex/MemoryBarrier/MemSetDefault/RealToI64/
  HexToInt/PutVarint32/uri_boolean/initialize/os_init`

（完整清单见 `src/scan.out/undefined_symbols.txt`，`make scan` 重新生成。）

## 5. SQL 耦合点记录（垫片吸收的、尚未拆的）

以下引用当前由垫片 `sqliteInt.h` 吸收，Phase 2 拆线时按此清单处理：

1. **struct sqlite3 残留字段**：`nVdbeActive/nVdbeRead/nVdbeWrite/nVdbeExec/
   pParse/errMask/nSavepoint/nStatement/pnBytesFreed/mallocFailed/lookaside/errCode
   (via sqlite3Error)/aDb/nDb/busyHandler/u1.isInterrupted/szMmap`。
   Phase 2 按"btreelite_env 只保留 mutex + busyHandler + interrupt 标志"收缩。
2. **malloc.c 的 Parse 依赖**：`sqlite3OomFault` 引用 `db->pParse→rc/nErr/
   pOuterParse`；Phase 2 删该分支（Parse 桩同步删除）。
3. **btree.c 的 schema 锁断言**：`hasSharedCacheTableLock` 等 debug 断言
   由垫片宏（OMIT_SHARED_CACHE 路径）吸收，Phase 2 连同 btree.c 内部
   shared-cache 块删除。
4. **pager.c 的 `sqlite3PagerBackupPtr`**：已声明未定义（backup.c 不移植）。

## 6. 与计划的偏差

- `global.c` 原计划遗漏，实际必须带走（全局 Config 单例 + ctype 表）。
- `btree.h`/`pcache.h` 未在 §5 清单中列出（btree.h 列了），已补拷。
- status.c/util.c/global.c 的裁剪从 Phase 2 提前到 Phase 0 —— 原因：
  "零修改编译"对 SQL 专用代码不可行，强行打桩反而制造无意义兼容层。

## 7. Phase 1 入口

下一阶段（Phase 1：pager 层脱 SQL）工作项，按优先级：
1. 移植 printf-lite（StrAccum 机制 + sqlite3_log + VXPrintf 的 %s/%d 子集），
   满足 integrity_check 的错误消息路径。
2. 实现 B 类符号（错误报告入口、FaultSim、BenignMalloc 已有但未导出检查）。
3. 删 super-journal（writeSuperJournal/readSuperJournal/pager_delsuper），
   `sqlite3PagerCommitPhaseOne(zSuper)` 恒 NULL。
4. pager.h/wal.h 签名中 `sqlite3*db` → `btreelite_db*`（中断标志）。
5. 目标：`make` 全绿 + 用 pager 层直接跑"建文件-写页-回滚-恢复"冒烟测试。