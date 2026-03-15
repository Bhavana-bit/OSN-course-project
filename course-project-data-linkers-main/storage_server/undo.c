#include "server.h"
#include "logging.h"

/* ----------------------------------------------------------
   copy_file_path()
---------------------------------------------------------- */
int copy_file_path(const char *src, const char *dst)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "Copying file: %s → %s", src, dst);
    log_event("SS", "UNDO", msg);

    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0)
    {
        snprintf(msg, sizeof(msg), "Failed to open source file: %s", src);
        log_event("SS", "ERROR", msg);
        return 0;
    }

    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0)
    {
        snprintf(msg, sizeof(msg), "Failed to open destination: %s", dst);
        log_event("SS", "ERROR", msg);

        close(in_fd);
        return 0;
    }

    char buf[4096];
    ssize_t n;

    while ((n = read(in_fd, buf, sizeof(buf))) > 0)
    {
        if (write(out_fd, buf, n) != n)
        {
            log_event("SS", "ERROR", "Write failed during copy");
            close(in_fd);
            close(out_fd);
            return 0;
        }
    }

    close(in_fd);
    close(out_fd);

    log_event("SS", "INFO", "File copy successful");
    return 1;
}

/* ----------------------------------------------------------
   save_undo()
   - Saves current <file> + <file.meta> into undo snapshots
---------------------------------------------------------- */
int save_undo(const char *filename)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "save_undo(): %s", filename);
    log_event("SS", "UNDO", msg);

    char path[512], path_undo[512];
    char meta[512], meta_undo[512];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, filename);
    snprintf(path_undo, sizeof(path_undo), "%s/%s.undo", DATA_PATH, filename);

    snprintf(meta, sizeof(meta), "%s/%s.meta", DATA_PATH, filename);
    snprintf(meta_undo, sizeof(meta_undo), "%s/%s.undo.meta", DATA_PATH, filename);

    if (stat(path, &st) != 0)
        return 0;

    /* overwrite previous undo snapshot */
    if (!copy_file_path(path, path_undo))
        return 0;

    if (stat(meta, &st) == 0)
        copy_file_path(meta, meta_undo);

    log_event("SS", "INFO", "Undo snapshot saved successfully");
    return 1;
}

/* ----------------------------------------------------------
   perform_undo()
   - Restores from <file>.undo and <file>.undo.meta
   - DOES NOT delete undo snapshot (project requirement)
---------------------------------------------------------- */
int perform_undo(const char *filename)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "perform_undo(): %s", filename);
    log_event("SS", "UNDO", msg);

    char path[512], path_undo[512];
    char meta[512], meta_undo[512];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", DATA_PATH, filename);
    snprintf(path_undo, sizeof(path_undo), "%s/%s.undo", DATA_PATH, filename);

    snprintf(meta, sizeof(meta), "%s/%s.meta", DATA_PATH, filename);
    snprintf(meta_undo, sizeof(meta_undo), "%s/%s.undo.meta", DATA_PATH, filename);

    /* ensure undo exists */
    if (stat(path_undo, &st) != 0)
    {
        log_event("SS", "WARN", "perform_undo(): no undo snapshot");
        return 0;
    }

    /* restore main file */
    if (!copy_file_path(path_undo, path))
        return 0;

    /* restore meta */
    if (stat(meta_undo, &st) == 0)
        copy_file_path(meta_undo, meta);

    log_event("SS", "INFO", "Undo restore complete");
    return 1;
}
