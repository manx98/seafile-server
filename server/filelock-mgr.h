#ifndef FILELOCK_MGR_H
#define FILELOCK_MGR_H

struct _SeafileSession;

typedef struct _SeafFilelockManager {
    struct _SeafileSession *seaf;
    gint64 default_expire;
} SeafFilelockManager;

SeafFilelockManager *seaf_filelock_manager_new (struct _SeafileSession *seaf);
int seaf_filelock_manager_init (SeafFilelockManager *mgr);
int seaf_filelock_manager_start (SeafFilelockManager *mgr);
GList *seaf_filelock_manager_get_locked_files (SeafFilelockManager *mgr,
                                               const char *repo_id,
                                               GError **error);
int seaf_filelock_manager_lock_file (SeafFilelockManager *mgr,
                                     const char *repo_id, const char *path,
                                     const char *user, gint64 expire,
                                     GError **error);
int seaf_filelock_manager_unlock_file (SeafFilelockManager *mgr,
                                       const char *repo_id, const char *path,
                                       GError **error);
int seaf_filelock_manager_check_file_lock (SeafFilelockManager *mgr,
                                           const char *repo_id, const char *path,
                                           const char *user);
int seaf_filelock_manager_refresh_file_lock (SeafFilelockManager *mgr,
                                             const char *repo_id, const char *path,
                                             gint64 expire, GError **error);
GObject *seaf_filelock_manager_get_lock_info (SeafFilelockManager *mgr,
                                              const char *repo_id, const char *path);
char *seaf_filelock_manager_get_lock_owner (SeafFilelockManager *mgr,
                                            const char *repo_id, const char *path);
int seaf_filelock_manager_delete_path (SeafFilelockManager *mgr,
                                       const char *repo_id, const char *path);
int seaf_filelock_manager_move_path (SeafFilelockManager *mgr,
                                     const char *repo_id,
                                     const char *old_path,
                                     const char *new_path);
int seaf_filelock_manager_delete_repo_locks (SeafFilelockManager *mgr,
                                             const char *repo_id);

#endif
