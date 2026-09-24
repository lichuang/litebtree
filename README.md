# litebtree

一个从 SQLite 存储引擎（btree + pager + WAL）中提取出来的、独立于 SQL 的
KV（键值对）存储引擎。

- 源自：SQLite 3.54.0 快照（`sqlite-snapshot-202607312245`，public domain）
- 语义：`put / get / delete / scan / seek`，字节串 key，二进制序（memcmp）排序
- 组织：一个数据库文件可包含多棵 btree（以 root page 号为树 ID）
- 事务：ACID，完整继承 SQLite 的 rollback journal + WAL 崩溃恢复
- 并发：进程内多线程 + 跨进程文件锁（继承 SQLite 原生能力）

设计细节见 [docs/kv-extraction-plan.md](docs/kv-extraction-plan.md)。

## 项目状态

🚧 设计/规划阶段，尚未开始实现。实施计划：

| 阶段 | 内容 |
|---|---|
| Phase 1 | 骨架与 shim（kvInt.h、内存/mutex/printf 子系统、kv_db） |
| Phase 2 | pager 层接入（os_unix、pcache、memjournal、bitvec、WAL） |
| Phase 3 | btree 层接入（btree.c/btmutex.c 手术清单见计划第 4 节） |
| Phase 4 | KV facade（kv_api.c、record 编解码、伪 KeyInfo comparator） |
| Phase 5 | 加固（integrity check、checkpoint、多线程/多进程/崩溃测试） |

## 构建与使用

待实现后补充（预期：`cmake` 或纯 `Makefile`，零第三方依赖，纯 C99）。

## API 预览

```c
kv_store   *db;
kv_cursor  *cur;

kv_open("test.kv", &db, KV_OPEN_CREATE);
kv_tree_create(db, &root);          /* 多树：返回 root page 号 */

kv_cursor_open(db, root, &cur);
kv_put(cur, "key", 3, "value", 5);
kv_get(cur, "key", 3, buf, sizeof(buf), &n);
kv_del(cur, "key", 3);

kv_begin(db); kv_commit(db);        /* ACID 事务 */

kv_cursor_close(cur);
kv_close(db);
```

## License

衍生自 SQLite（public domain）。本项目同样以 public domain 发布。

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```