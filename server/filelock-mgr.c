#include "common.h"
#include "log.h"
#include "seafile-error.h"
#include "seafile-session.h"
#include "filelock-mgr.h"

#include <pthread.h>

#define DEFAULT_EXPIRE_SECONDS (12 * 3600)
#define CLEAN_INTERVAL_SECONDS 900

typedef struct {
    GList *locks;
} LockList;

static gboolean
collect_lock (SeafDBRow *row, void *data)
{
    LockList *list = data;
    GObject *lock = g_object_new (SEAFILE_TYPE_FILE_LOCK,
                                  "repo-id", seaf_db_row_get_column_text (row, 0),
                                  "path", seaf_db_row_get_column_text (row, 1),
                                  "user", seaf_db_row_get_column_text (row, 2),
                                  "lock-time", seaf_db_row_get_column_int64 (row, 3),
                                  "expire", seaf_db_row_get_column_int64 (row, 4),
                                  NULL);
    list->locks = g_list_prepend (list->locks, lock);
    return TRUE;
}

static gboolean
collect_owner (SeafDBRow *row, void *data)
{
    char **owner = data;
    *owner = g_strdup (seaf_db_row_get_column_text (row, 0));
    return FALSE;
}

static void
resolve_path (SeafFilelockManager *mgr, const char *repo_id, const char *path,
              char **real_repo_id, char **real_path)
{
    SeafRepo *repo = seaf_repo_manager_get_repo (mgr->seaf->repo_mgr, repo_id);

    if (repo && repo->virtual_info) {
        *real_repo_id = g_strdup (repo->virtual_info->origin_repo_id);
        *real_path = g_build_filename (repo->virtual_info->path, path, NULL);
    } else {
        *real_repo_id = g_strdup (repo_id);
        *real_path = g_strdup (path);
    }
    if (repo)
        seaf_repo_unref (repo);
}

static int
touch_timestamp (SeafFilelockManager *mgr, const char *repo_id)
{
    const char *sql;

    if (seaf_db_type (mgr->seaf->db) == SEAF_DB_TYPE_MYSQL)
        sql = "INSERT INTO FileLockTimestamp (repo_id, update_time) VALUES (?, ?) "
              "ON DUPLICATE KEY UPDATE update_time=VALUES(update_time)";
    else if (seaf_db_type (mgr->seaf->db) == SEAF_DB_TYPE_PGSQL)
        sql = "INSERT INTO FileLockTimestamp (repo_id, update_time) VALUES (?, ?) "
              "ON CONFLICT (repo_id) DO UPDATE SET update_time=EXCLUDED.update_time";
    else
        sql = "INSERT OR REPLACE INTO FileLockTimestamp (repo_id, update_time) VALUES (?, ?)";

    return seaf_db_statement_query (mgr->seaf->db, sql, 2,
                                    "string", repo_id, "int64", (gint64)time (NULL));
}

static void
notify_lock_changed (SeafFilelockManager *mgr, const char *repo_id,
                     const char *path, const char *change, const char *user)
{
    json_t *event, *content;
    char *msg;

    if (!mgr->seaf->notif_mgr)
        return;
    event = json_object ();
    content = json_object ();
    json_object_set_new (event, "type", json_string ("file-lock-changed"));
    json_object_set_new (content, "repo_id", json_string (repo_id));
    json_object_set_new (content, "path", json_string (path));
    json_object_set_new (content, "change_event", json_string (change));
    json_object_set_new (content, "lock_user", json_string (user ? user : ""));
    json_object_set_new (event, "content", content);
    msg = json_dumps (event, JSON_COMPACT);
    seaf_notif_manager_send_event (mgr->seaf->notif_mgr, msg);
    g_free (msg);
    json_decref (event);
}

SeafFilelockManager *
seaf_filelock_manager_new (SeafileSession *seaf)
{
    SeafFilelockManager *mgr = g_new0 (SeafFilelockManager, 1);
    int hours;

    mgr->seaf = seaf;
    hours = g_key_file_get_integer (seaf->config, "file_lock",
                                    "default_expire_hours", NULL);
    mgr->default_expire = hours > 0 ? (gint64)hours * 3600
                                    : DEFAULT_EXPIRE_SECONDS;
    return mgr;
}

int
seaf_filelock_manager_init (SeafFilelockManager *mgr)
{
    SeafDB *db = mgr->seaf->db;
    int type = seaf_db_type (db);

    if (!mgr->seaf->create_tables && type != SEAF_DB_TYPE_PGSQL)
        return 0;

    if (type == SEAF_DB_TYPE_MYSQL) {
        if (seaf_db_query (
                db,
                "CREATE TABLE IF NOT EXISTS FileLocks ("
                "id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT, "
                "repo_id CHAR(40) NOT NULL, path TEXT NOT NULL, "
                "user_name VARCHAR(255) NOT NULL, lock_time BIGINT, expire BIGINT, "
                "KEY(repo_id)) ENGINE=INNODB") < 0 ||
            seaf_db_query (
                db,
                "CREATE TABLE IF NOT EXISTS FileLockTimestamp ("
                "id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT, repo_id CHAR(40), "
                "update_time BIGINT NOT NULL, UNIQUE INDEX(repo_id))") < 0)
            return -1;
    } else {
        if (seaf_db_query (
                db,
                "CREATE TABLE IF NOT EXISTS FileLocks ("
                "repo_id CHAR(40) NOT NULL, path TEXT NOT NULL, "
                "user_name VARCHAR(255) NOT NULL, lock_time BIGINT, expire BIGINT)") < 0 ||
            seaf_db_query (
                db, "CREATE INDEX IF NOT EXISTS FileLocksIndex "
                    "ON FileLocks (repo_id)") < 0 ||
            seaf_db_query (
                db,
                "CREATE TABLE IF NOT EXISTS FileLockTimestamp ("
                "repo_id CHAR(40) PRIMARY KEY, update_time BIGINT NOT NULL)") < 0)
            return -1;
    }
    return 0;
}

GList *
seaf_filelock_manager_get_locked_files (SeafFilelockManager *mgr,
                                        const char *repo_id, GError **error)
{
    LockList list = { 0 };
    char *real_repo_id, *real_path;
    SeafRepo *repo;

    resolve_path (mgr, repo_id, "/", &real_repo_id, &real_path);
    if (seaf_db_statement_foreach_row (
            mgr->seaf->db,
            "SELECT repo_id, path, user_name, lock_time, expire FROM FileLocks "
            "WHERE repo_id=? AND (expire<0 OR "
            "(lock_time>? AND (expire=0 OR expire>?)))",
            collect_lock, &list, 3, "string", real_repo_id,
            "int64", (gint64)time (NULL) - mgr->default_expire,
            "int64", (gint64)time (NULL)) < 0) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Failed to get locked files info from db.");
        g_list_free_full (list.locks, g_object_unref);
        list.locks = NULL;
    }
    g_free (real_repo_id);
    g_free (real_path);

    /* Virtual repos store locks against the origin repo. Return only locks
     * visible below the virtual root and translate them back to virtual paths. */
    repo = seaf_repo_manager_get_repo (mgr->seaf->repo_mgr, repo_id);
    if (repo && repo->virtual_info) {
        size_t root_len = strlen (repo->virtual_info->path);
        GList *ptr = list.locks;
        while (ptr) {
            GList *next = ptr->next;
            GObject *lock = ptr->data;
            char *path;
            g_object_get (lock, "path", &path, NULL);
            if (!g_str_has_prefix (path, repo->virtual_info->path) ||
                (root_len > 1 && path[root_len] != '/' && path[root_len] != '\0')) {
                list.locks = g_list_delete_link (list.locks, ptr);
                g_object_unref (lock);
            } else {
                const char *relative = path + strlen (repo->virtual_info->path);
                g_object_set (lock, "repo-id", repo_id,
                              "path", *relative ? relative : "/", NULL);
            }
            g_free (path);
            ptr = next;
        }
    }
    if (repo)
        seaf_repo_unref (repo);
    return g_list_reverse (list.locks);
}

int
seaf_filelock_manager_lock_file (SeafFilelockManager *mgr,
                                 const char *repo_id, const char *path,
                                 const char *user, gint64 expire, GError **error)
{
    SeafDBTrans *trans;
    char *real_repo_id, *real_path, *owner = NULL;
    const char *sql;
    int ret = -1;

    resolve_path (mgr, repo_id, path, &real_repo_id, &real_path);
    trans = seaf_db_begin_transaction (mgr->seaf->db);
    if (!trans)
        goto out;

    if (seaf_db_trans_query (
            trans,
            "DELETE FROM FileLocks WHERE repo_id=? AND path=? AND expire>=0 "
            "AND (lock_time<=? OR (expire>0 AND expire<=?))",
            4, "string", real_repo_id, "string", real_path,
            "int64", (gint64)time (NULL) - mgr->default_expire,
            "int64", (gint64)time (NULL)) < 0)
        goto rollback;

    sql = seaf_db_type (mgr->seaf->db) == SEAF_DB_TYPE_SQLITE
        ? "SELECT user_name FROM FileLocks WHERE repo_id=? AND path=?"
        : "SELECT user_name FROM FileLocks WHERE repo_id=? AND path=? FOR UPDATE";
    if (seaf_db_trans_foreach_selected_row (trans, sql, collect_owner, &owner, 2,
                                            "string", real_repo_id,
                                            "string", real_path) < 0)
        goto rollback;

    if (owner) {
        if (g_strcmp0 (owner, user) != 0) {
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                         "File is locked by others.");
            goto rollback;
        }
        ret = seaf_db_commit (trans);
        goto close;
    }

    if (seaf_db_trans_query (
            trans,
            "INSERT INTO FileLocks (repo_id, path, user_name, lock_time, expire) VALUES (?, ?, ?, ?, ?)",
            5, "string", real_repo_id, "string", real_path, "string", user,
            "int64", (gint64)time (NULL), "int64", expire) < 0 ||
        seaf_db_commit (trans) < 0) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "DB Error.");
        goto rollback;
    }
    ret = 0;
    touch_timestamp (mgr, real_repo_id);
    notify_lock_changed (mgr, real_repo_id, real_path, "locked", user);
    goto close;

rollback:
    seaf_db_rollback (trans);
close:
    seaf_db_trans_close (trans);
out:
    g_free (owner);
    g_free (real_repo_id);
    g_free (real_path);
    return ret;
}

int
seaf_filelock_manager_unlock_file (SeafFilelockManager *mgr,
                                   const char *repo_id, const char *path,
                                   GError **error)
{
    char *real_repo_id, *real_path;
    int ret;

    resolve_path (mgr, repo_id, path, &real_repo_id, &real_path);
    ret = seaf_db_statement_query (mgr->seaf->db,
                                   "DELETE FROM FileLocks WHERE repo_id=? AND path=?",
                                   2, "string", real_repo_id, "string", real_path);
    if (ret < 0)
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Failed to delete lock file info from db.");
    else
        touch_timestamp (mgr, real_repo_id);
    if (ret >= 0)
        notify_lock_changed (mgr, real_repo_id, real_path, "unlocked", NULL);
    g_free (real_repo_id);
    g_free (real_path);
    return ret;
}

int
seaf_filelock_manager_check_file_lock (SeafFilelockManager *mgr,
                                       const char *repo_id, const char *path,
                                       const char *user)
{
    char *real_repo_id, *real_path, *owner;
    int ret = 0;

    resolve_path (mgr, repo_id, path, &real_repo_id, &real_path);
    owner = seaf_db_statement_get_string (
        mgr->seaf->db,
        "SELECT user_name FROM FileLocks WHERE repo_id=? AND path=?",
        2, "string", real_repo_id, "string", real_path);
    if (owner)
        ret = g_strcmp0 (owner, user) == 0 ? 2 : 1;
    g_free (owner);
    g_free (real_repo_id);
    g_free (real_path);
    return ret;
}

int
seaf_filelock_manager_refresh_file_lock (SeafFilelockManager *mgr,
                                         const char *repo_id, const char *path,
                                         gint64 expire, GError **error)
{
    gboolean db_err;
    char *real_repo_id, *real_path;
    int ret;

    resolve_path (mgr, repo_id, path, &real_repo_id, &real_path);
    if (!seaf_db_statement_exists (
            mgr->seaf->db,
            "SELECT 1 FROM FileLocks WHERE repo_id=? AND path=?", &db_err,
            2, "string", real_repo_id, "string", real_path)) {
        ret = db_err ? -1 : -2;
        if (!db_err)
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                         "File is not locked.");
        goto out;
    }
    ret = seaf_db_statement_query (
        mgr->seaf->db,
        "UPDATE FileLocks SET lock_time=?, expire=? WHERE repo_id=? AND path=?",
        4, "int64", (gint64)time (NULL), "int64", expire,
        "string", real_repo_id, "string", real_path);
    if (ret >= 0)
        ret = touch_timestamp (mgr, real_repo_id);
out:
    g_free (real_repo_id);
    g_free (real_path);
    return ret;
}

GObject *
seaf_filelock_manager_get_lock_info (SeafFilelockManager *mgr,
                                     const char *repo_id, const char *path)
{
    LockList list = { 0 };
    GObject *lock = NULL;
    char *real_repo_id, *real_path;

    resolve_path (mgr, repo_id, path, &real_repo_id, &real_path);
    if (seaf_db_statement_foreach_row (
            mgr->seaf->db,
            "SELECT repo_id, path, user_name, lock_time, expire FROM FileLocks "
            "WHERE repo_id=? AND path=? AND (expire<0 OR "
            "(lock_time>? AND (expire=0 OR expire>?)))",
            collect_lock, &list, 4, "string", real_repo_id,
            "string", real_path,
            "int64", (gint64)time (NULL) - mgr->default_expire,
            "int64", (gint64)time (NULL)) >= 0 && list.locks)
        lock = list.locks->data;
    g_list_free (list.locks);
    g_free (real_repo_id);
    g_free (real_path);
    return lock;
}

char *
seaf_filelock_manager_get_lock_owner (SeafFilelockManager *mgr,
                                      const char *repo_id, const char *path)
{
    GObject *lock = seaf_filelock_manager_get_lock_info (mgr, repo_id, path);
    char *owner = NULL;

    if (lock) {
        g_object_get (lock, "user", &owner, NULL);
        g_object_unref (lock);
    }
    return owner;
}

static gboolean
path_is_same_or_child (const char *path, const char *parent)
{
    size_t len = strlen (parent);

    return strcmp (path, parent) == 0 ||
           (g_str_has_prefix (path, parent) &&
            (len == 1 || path[len] == '/'));
}

int
seaf_filelock_manager_delete_path (SeafFilelockManager *mgr,
                                   const char *repo_id, const char *path)
{
    GList *locks, *ptr;
    int ret = 0;

    locks = seaf_filelock_manager_get_locked_files (mgr, repo_id, NULL);
    for (ptr = locks; ptr; ptr = ptr->next) {
        GObject *lock = ptr->data;
        char *lock_path = NULL;
        g_object_get (lock, "path", &lock_path, NULL);
        if (path_is_same_or_child (lock_path, path) &&
            seaf_filelock_manager_unlock_file (mgr, repo_id, lock_path, NULL) < 0)
            ret = -1;
        g_free (lock_path);
    }
    g_list_free_full (locks, g_object_unref);
    return ret;
}

int
seaf_filelock_manager_move_path (SeafFilelockManager *mgr,
                                 const char *repo_id,
                                 const char *old_path,
                                 const char *new_path)
{
    GList *locks, *ptr;
    int ret = 0;

    locks = seaf_filelock_manager_get_locked_files (mgr, repo_id, NULL);
    for (ptr = locks; ptr; ptr = ptr->next) {
        GObject *lock = ptr->data;
        char *path = NULL, *owner = NULL;
        char *old_repo_id, *old_real_path, *new_repo_id, *new_real_path;
        char *moved_path;

        g_object_get (lock, "path", &path, "user", &owner, NULL);
        if (!path_is_same_or_child (path, old_path)) {
            g_free (path);
            g_free (owner);
            continue;
        }
        moved_path = g_strconcat (new_path, path + strlen (old_path), NULL);
        resolve_path (mgr, repo_id, path, &old_repo_id, &old_real_path);
        resolve_path (mgr, repo_id, moved_path, &new_repo_id, &new_real_path);
        if (seaf_db_statement_query (
                mgr->seaf->db,
                "UPDATE FileLocks SET repo_id=?, path=? WHERE repo_id=? AND path=?",
                4, "string", new_repo_id, "string", new_real_path,
                "string", old_repo_id, "string", old_real_path) < 0) {
            ret = -1;
        } else {
            touch_timestamp (mgr, old_repo_id);
            notify_lock_changed (mgr, old_repo_id, old_real_path, "unlocked", NULL);
            notify_lock_changed (mgr, new_repo_id, new_real_path, "locked", owner);
        }
        g_free (old_repo_id);
        g_free (old_real_path);
        g_free (new_repo_id);
        g_free (new_real_path);
        g_free (moved_path);
        g_free (path);
        g_free (owner);
    }
    g_list_free_full (locks, g_object_unref);
    return ret;
}

int
seaf_filelock_manager_delete_repo_locks (SeafFilelockManager *mgr,
                                         const char *repo_id)
{
    int ret = seaf_db_statement_query (
        mgr->seaf->db, "DELETE FROM FileLocks WHERE repo_id=?",
        1, "string", repo_id);
    if (ret >= 0)
        touch_timestamp (mgr, repo_id);
    return ret;
}

static void
clean_expired_locks (SeafFilelockManager *mgr)
{
    LockList list = { 0 };
    GList *ptr;
    gint64 now = time (NULL);

    if (seaf_db_statement_foreach_row (
            mgr->seaf->db,
            "SELECT repo_id, path, user_name, lock_time, expire FROM FileLocks "
            "WHERE expire>=0 AND (lock_time<=? OR (expire>0 AND expire<=?))",
            collect_lock, &list, 2,
            "int64", now - mgr->default_expire, "int64", now) < 0) {
        seaf_warning ("Failed to get expired file locks.\n");
        return;
    }

    for (ptr = list.locks; ptr; ptr = ptr->next) {
        char *repo_id = NULL, *path = NULL;
        g_object_get (ptr->data, "repo-id", &repo_id, "path", &path, NULL);
        seaf_filelock_manager_unlock_file (mgr, repo_id, path, NULL);
        g_free (repo_id);
        g_free (path);
    }
    g_list_free_full (list.locks, g_object_unref);
}

static void *
cleanup_worker (void *data)
{
    SeafFilelockManager *mgr = data;

    while (1) {
        clean_expired_locks (mgr);
        sleep (CLEAN_INTERVAL_SECONDS);
    }
    return NULL;
}

int
seaf_filelock_manager_start (SeafFilelockManager *mgr)
{
    pthread_t tid;
    char *primary_url;
    int rc;

    primary_url = g_key_file_get_string (mgr->seaf->config,
                                         "backup", "primary_url", NULL);
    if (primary_url) {
        g_free (primary_url);
        return 0;
    }

    rc = pthread_create (&tid, NULL, cleanup_worker, mgr);
    if (rc != 0) {
        seaf_warning ("Failed to create file lock cleanup worker: %s.\n",
                      strerror (rc));
        return -1;
    }
    pthread_detach (tid);
    return 0;
}
