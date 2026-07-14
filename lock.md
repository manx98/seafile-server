# 文件锁实现说明

## 当前结论

当前仓库的 CE 代码没有真正实现文件锁。`python/seaserv/api.py` 里的 `check_file_lock()` 明确写了 CE 不支持文件锁，并且始终返回 `0`，也就是“没有被锁”。

```python
def check_file_lock(self, repo_id, path, user):
    """
    Always return 0 since CE doesn't support file locking.
    """
    return 0
```

所以在当前项目里，上传、更新、删除等文件写操作不会因为文件锁被服务端拦截。

## 代码里保留的文件锁结构

虽然没有完整实现，仓库里保留了几处文件锁相关接口和数据结构。

### 数据库表

`scripts/sql/mysql/seafile.sql` 和 `scripts/sql/sqlite/seafile.sql` 都创建了文件锁表：

```sql
CREATE TABLE IF NOT EXISTS FileLocks (
  repo_id CHAR(40) NOT NULL,
  path TEXT NOT NULL,
  user_name VARCHAR(255) NOT NULL,
  lock_time BIGINT,
  expire BIGINT
);
```

MySQL 版本带自增 `id` 和 `KEY(repo_id)`，SQLite 版本只有 `repo_id` 索引。

字段含义按命名可读为：

- `repo_id`：资料库 ID。
- `path`：被锁文件路径。
- `user_name`：加锁用户。
- `lock_time`：加锁时间。
- `expire`：过期时间。

另有 `FileLockTimestamp`：

```sql
CREATE TABLE IF NOT EXISTS FileLockTimestamp (
  repo_id CHAR(40) PRIMARY KEY,
  update_time BIGINT NOT NULL
);
```

它用于记录某个 repo 的文件锁更新时间，方便客户端或上层服务判断锁状态是否变化。

### RPC 接口声明

`include/seafile-rpc.h` 声明了两个客户端侧 RPC 接口：

```c
int seafile_mark_file_locked (const char *repo_id, const char *path, GError **error);
int seafile_mark_file_unlocked (const char *repo_id, const char *path, GError **error);
```

但当前仓库没有搜到对应实现或服务端注册逻辑，说明这里只是保留接口声明。

### 目录项字段

`lib/dirent.vala` 的目录项对象有文件锁展示字段：

```vala
public bool is_locked { set; get; }
public string lock_owner { set; get; }
public int64 lock_time { set; get; }
```

这些字段用于把文件锁状态带到目录列表结果里，但当前 CE 服务端没有填充这几个字段的完整逻辑。

### 通知事件

`notification-server/event.go` 支持转发 `file-lock-changed` 事件：

```go
type FileLockEvent struct {
    RepoID      string `json:"repo_id"`
    Path        string `json:"path"`
    ChangeEvent string `json:"change_event"`
    LockUser    string `json:"lock_user"`
}
```

通知服务只负责按 `repo_id` 找订阅客户端并转发消息，不负责写锁、解锁或判断锁冲突。

## 实际请求链路

当前实际链路很短：

1. 上层调用 `seaserv.api.check_file_lock(repo_id, path, user)`。
2. CE 直接返回 `0`。
3. 文件操作继续执行。

也就是说，现在的文件锁结果永远是“未锁定”。

## 如果要补齐文件锁

最小实现点应该放在服务端共享层，不要在每个上传/更新入口重复加判断。

需要补的内容：

1. 实现锁表读写：加锁、解锁、按 `repo_id + path` 查询锁记录，并处理 `expire`。
2. 实现 `check_file_lock(repo_id, path, user)`：同一用户可写，其他用户遇到未过期锁应拒绝。
3. 在文件写操作的公共入口调用锁检查。
4. 写锁状态变化时更新 `FileLockTimestamp`。
5. 发出 `file-lock-changed` 通知，让已订阅客户端刷新状态。
6. 目录列表填充 `is_locked`、`lock_owner`、`lock_time`。

当前代码已经有表结构、展示字段和通知事件，缺的是核心读写和服务端拦截逻辑。
