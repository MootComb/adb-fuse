#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>

#define ADB_CHECK_INTERVAL  3
#define MOUNT_BASE_DIR      "/run/media"
#define ADB_SUBDIR          "adb"
#define CMD_TIMEOUT_SEC     5
#define SHELL_READY_TIMEOUT 3
#define INIT_DELAY_SEC      3
#define ENABLE_DEBUG        1

#define LOG_INFO(fmt, ...) \
    do { \
        time_t now = time(NULL); \
        char time_str[64]; \
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now)); \
        fprintf(stdout, "[INFO] [%s] " fmt "\n", time_str, ##__VA_ARGS__); \
        fflush(stdout); \
    } while(0)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (ENABLE_DEBUG) { \
            time_t now = time(NULL); \
            char time_str[64]; \
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now)); \
            fprintf(stdout, "[DEBUG] [%s] " fmt "\n", time_str, ##__VA_ARGS__); \
            fflush(stdout); \
        } \
    } while(0)

#define LOG_ERROR(fmt, ...) \
    do { \
        time_t now = time(NULL); \
        char time_str[64]; \
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now)); \
        fprintf(stderr, "[ERROR] [%s] " fmt "\n", time_str, ##__VA_ARGS__); \
        fflush(stderr); \
    } while(0)

#define LOG_WARN(fmt, ...) \
    do { \
        time_t now = time(NULL); \
        char time_str[64]; \
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now)); \
        fprintf(stderr, "[WARN] [%s] " fmt "\n", time_str, ##__VA_ARGS__); \
        fflush(stderr); \
    } while(0)

typedef struct {
    FILE *input;
    FILE *output;
    int output_fd;
    pid_t shell_pid;
    pthread_mutex_t lock;
    char *serial;
    atomic_int connected;
    time_t mount_time;
} adb_shell_t;

typedef struct adb_device {
    char *serial;
    char *device_name;
    char *mount_point;
    pid_t fuse_pid;
    adb_shell_t *shell;
    pthread_t monitor_thread;
    atomic_int active;
    struct adb_device *next;
} adb_device_t;

typedef struct {
    char name[256];
    mode_t mode;
    off_t size;
    time_t cache_time;
} file_cache_t;

static adb_device_t *device_list = NULL;
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t main_monitor_thread;
static atomic_int keep_running = 1;

static file_cache_t file_cache[512];
static int cache_count = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void cache_add(const char *name, mode_t mode, off_t size) {
    pthread_mutex_lock(&cache_mutex);
    
    for (int i = 0; i < cache_count; i++) {
        if (strcmp(file_cache[i].name, name) == 0) {
            file_cache[i].mode = mode;
            file_cache[i].size = size;
            file_cache[i].cache_time = time(NULL);
            pthread_mutex_unlock(&cache_mutex);
            return;
        }
    }
    
    if (cache_count < 512) {
        strncpy(file_cache[cache_count].name, name, 255);
        file_cache[cache_count].name[255] = '\0';
        file_cache[cache_count].mode = mode;
        file_cache[cache_count].size = size;
        file_cache[cache_count].cache_time = time(NULL);
        cache_count++;
    }
    
    pthread_mutex_unlock(&cache_mutex);
}

static int cache_get(const char *name, mode_t *mode, off_t *size) {
    pthread_mutex_lock(&cache_mutex);
    
    time_t now = time(NULL);
    
    for (int i = 0; i < cache_count; i++) {
        if (strcmp(file_cache[i].name, name) == 0) {
            if (now - file_cache[i].cache_time <= 5) {
                *mode = file_cache[i].mode;
                *size = file_cache[i].size;
                pthread_mutex_unlock(&cache_mutex);
                return 1;
            }
        }
    }
    
    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

static adb_shell_t *shell_create(const char *serial) {
    adb_shell_t *shell = calloc(1, sizeof(adb_shell_t));
    if (!shell) return NULL;
    
    shell->serial = strdup(serial);
    if (!shell->serial) {
        free(shell);
        return NULL;
    }
    
    pthread_mutex_init(&shell->lock, NULL);
    shell->connected = 0;
    shell->input = NULL;
    shell->output = NULL;
    shell->output_fd = -1;
    shell->shell_pid = -1;
    shell->mount_time = time(NULL);
    
    return shell;
}

static int shell_connect(adb_shell_t *shell) {
    if (!shell) return -1;
    
    pthread_mutex_lock(&shell->lock);
    
    if (shell->connected) {
        pthread_mutex_unlock(&shell->lock);
        return 0;
    }
    
    int to_shell[2] = {-1, -1};
    int from_shell[2] = {-1, -1};
    
    if (pipe(to_shell) < 0 || pipe(from_shell) < 0) {
        if (to_shell[0] >= 0) { close(to_shell[0]); close(to_shell[1]); }
        if (from_shell[0] >= 0) { close(from_shell[0]); close(from_shell[1]); }
        pthread_mutex_unlock(&shell->lock);
        return -1;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        close(to_shell[0]); close(to_shell[1]);
        close(from_shell[0]); close(from_shell[1]);
        pthread_mutex_unlock(&shell->lock);
        return -1;
    }
    
    if (pid == 0) {
        close(to_shell[1]);
        close(from_shell[0]);
        
        if (dup2(to_shell[0], STDIN_FILENO) < 0 ||
            dup2(from_shell[1], STDOUT_FILENO) < 0 ||
            dup2(from_shell[1], STDERR_FILENO) < 0) {
            _exit(1);
        }
        
        close(to_shell[0]);
        close(from_shell[1]);
        
        int max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 256;
        for (int i = 3; i < max_fd; i++) {
            close(i);
        }
        
        execlp("adb", "adb", "-s", shell->serial, "shell", NULL);
        _exit(1);
    }
    
    close(to_shell[0]);
    close(from_shell[1]);
    
    shell->input = fdopen(to_shell[1], "w");
    shell->output = fdopen(from_shell[0], "r");
    shell->output_fd = from_shell[0];
    shell->shell_pid = pid;
    
    if (!shell->input || !shell->output) {
        if (shell->input) fclose(shell->input);
        else close(to_shell[1]);
        if (shell->output) fclose(shell->output);
        else close(from_shell[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_unlock(&shell->lock);
        return -1;
    }
    
    setvbuf(shell->input, NULL, _IONBF, 0);
    setvbuf(shell->output, NULL, _IONBF, 0);
    
    fprintf(shell->input, "echo READY_%d\n", (int)time(NULL));
    fflush(shell->input);
    
    char buf[256];
    fd_set fds;
    struct timeval tv;
    
    FD_ZERO(&fds);
    FD_SET(shell->output_fd, &fds);
    tv.tv_sec = SHELL_READY_TIMEOUT;
    tv.tv_usec = 0;
    
    int ret = select(shell->output_fd + 1, &fds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(shell->output_fd, &fds)) {
        if (fgets(buf, sizeof(buf), shell->output)) {
            buf[strcspn(buf, "\r\n")] = 0;
            if (strstr(buf, "READY_")) {
                shell->connected = 1;
                shell->mount_time = time(NULL);
                LOG_INFO("Shell connected to %s", shell->serial);
            }
        }
    }
    
    if (!shell->connected) {
        fclose(shell->input);
        fclose(shell->output);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        shell->input = NULL;
        shell->output = NULL;
        shell->output_fd = -1;
        shell->shell_pid = -1;
    }
    
    pthread_mutex_unlock(&shell->lock);
    return shell->connected ? 0 : -1;
}

static void shell_disconnect(adb_shell_t *shell) {
    if (!shell) return;
    
    pthread_mutex_lock(&shell->lock);
    
    if (shell->connected) {
        if (shell->input) {
            fprintf(shell->input, "exit\n");
            fflush(shell->input);
        }
        
        if (shell->input) fclose(shell->input);
        if (shell->output) fclose(shell->output);
        
        shell->input = NULL;
        shell->output = NULL;
        shell->output_fd = -1;
        
        if (shell->shell_pid > 0) {
            int status;
            int wait_count = 0;
            while (wait_count < 10) {
                pid_t result = waitpid(shell->shell_pid, &status, WNOHANG);
                if (result == shell->shell_pid) break;
                if (result == -1) break;
                usleep(100000);
                wait_count++;
            }
            
            if (wait_count >= 10 && shell->shell_pid > 0) {
                kill(shell->shell_pid, SIGTERM);
                usleep(100000);
                if (waitpid(shell->shell_pid, &status, WNOHANG) == 0) {
                    kill(shell->shell_pid, SIGKILL);
                    waitpid(shell->shell_pid, &status, 0);
                }
            }
        }
        
        shell->shell_pid = -1;
        shell->connected = 0;
        LOG_INFO("Shell disconnected from %s", shell->serial);
    }
    
    pthread_mutex_unlock(&shell->lock);
    pthread_mutex_destroy(&shell->lock);
    
    free(shell->serial);
    free(shell);
}

static char *shell_exec(adb_shell_t *shell, const char *cmd) {
    if (!shell || !shell->connected || !shell->input || !shell->output) {
        return NULL;
    }
    
    pthread_mutex_lock(&shell->lock);
    
    int status;
    if (shell->shell_pid > 0 && waitpid(shell->shell_pid, &status, WNOHANG) == shell->shell_pid) {
        shell->connected = 0;
        pthread_mutex_unlock(&shell->lock);
        return NULL;
    }
    
    static atomic_int cmd_counter = 0;
    int cmd_id = atomic_fetch_add(&cmd_counter, 1);
    
    char end_marker[64];
    snprintf(end_marker, sizeof(end_marker), "__END_%d_%ld__", cmd_id, (long)time(NULL));
    
    fprintf(shell->input, "%s; echo '%s'\n", cmd, end_marker);
    fflush(shell->input);
    
    size_t buf_size = 65536;
    size_t total = 0;
    char *result = malloc(buf_size);
    if (!result) {
        pthread_mutex_unlock(&shell->lock);
        return NULL;
    }
    result[0] = '\0';
    
    char read_buf[8192];
    fd_set fds;
    struct timeval tv;
    time_t start_time = time(NULL);
    int marker_found = 0;
    
    while (time(NULL) - start_time <= CMD_TIMEOUT_SEC) {
        FD_ZERO(&fds);
        FD_SET(shell->output_fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(shell->output_fd + 1, &fds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            free(result);
            pthread_mutex_unlock(&shell->lock);
            return NULL;
        }
        
        if (ret == 0) continue;
        
        ssize_t n = read(shell->output_fd, read_buf, sizeof(read_buf) - 1);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n == 0) {
                shell->connected = 0;
                free(result);
                pthread_mutex_unlock(&shell->lock);
                return NULL;
            }
            break;
        }
        
        read_buf[n] = '\0';
        
        char *marker_pos = strstr(read_buf, end_marker);
        if (marker_pos) {
            size_t len = marker_pos - read_buf;
            if (total + len >= buf_size) {
                buf_size = total + len + 1;
                char *new_result = realloc(result, buf_size);
                if (!new_result) {
                    free(result);
                    pthread_mutex_unlock(&shell->lock);
                    return NULL;
                }
                result = new_result;
            }
            memcpy(result + total, read_buf, len);
            total += len;
            result[total] = '\0';
            marker_found = 1;
            break;
        }
        
        if (total + n >= buf_size) {
            buf_size = (total + n) * 2;
            char *new_result = realloc(result, buf_size);
            if (!new_result) {
                free(result);
                pthread_mutex_unlock(&shell->lock);
                return NULL;
            }
            result = new_result;
        }
        memcpy(result + total, read_buf, n);
        total += n;
        result[total] = '\0';
    }
    
    if (!marker_found) {
        free(result);
        pthread_mutex_unlock(&shell->lock);
        return NULL;
    }
    
    while (total > 0 && (result[total - 1] == '\n' || result[total - 1] == '\r')) {
        result[total - 1] = '\0';
        total--;
    }
    
    pthread_mutex_unlock(&shell->lock);
    return result;
}

static char *get_device_name(adb_shell_t *shell) {
    if (!shell || !shell->connected) return NULL;
    
    const char *props[] = {
        "ro.product.marketname",
        "ro.product.model",
        "ro.product.vendor.model",
        "ro.product.name",
        "ro.product.device"
    };
    
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "getprop %s 2>/dev/null", props[i]);
        
        char *raw_name = shell_exec(shell, cmd);
        if (!raw_name) continue;
        
        char *name = raw_name;
        while (*name == ' ' || *name == '\t' || *name == '\r' || *name == '\n') {
            name++;
        }
        
        char *end = name + strlen(name) - 1;
        while (end > name && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        
        if (strlen(name) > 0) {
            for (char *p = name; *p; p++) {
                if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || 
                    *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
                    *p = '_';
                }
            }
            
            char *result = strdup(name);
            free(raw_name);
            return result;
        }
        free(raw_name);
    }
    
    return strdup(shell->serial);
}

static char *escape_path(const char *path) {
    static char escaped[4096];
    const char *src = path;
    char *dst = escaped;
    size_t remaining = sizeof(escaped) - 1;
    
    while (*src && remaining > 1) {
        if (*src == '\'' || *src == '\\' || *src == '$' || 
            *src == '`' || *src == '"' || *src == ' ' || *src == '&' ||
            *src == ';' || *src == '|' || *src == '(' || *src == ')' ||
            *src == '[' || *src == ']' || *src == '{' || *src == '}' ||
            *src == '*' || *src == '?' || *src == '!' || *src == '~' ||
            *src == '<' || *src == '>' || *src == '#' || *src == '%' ||
            *src == '\n' || *src == '\r' || *src == '\t') {
            *dst++ = '\\';
            remaining--;
            if (remaining == 0) break;
        }
        *dst++ = *src++;
        remaining--;
    }
    *dst = '\0';
    return escaped;
}

static int is_initialized(adb_shell_t *shell) {
    if (!shell) return 0;
    return (time(NULL) - shell->mount_time) >= INIT_DELAY_SEC;
}

static int adb_readlink(const char *path, char *buf, size_t size) {
    adb_shell_t *shell = (adb_shell_t *)fuse_get_context()->private_data;
    
    if (!shell || !shell->connected) {
        return -EIO;
    }
    
    if (!is_initialized(shell)) {
        return -ENOENT;
    }
    
    char *escaped = escape_path(path);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "readlink '%s' 2>/dev/null", escaped);
    
    char *result = shell_exec(shell, cmd);
    if (!result || strlen(result) == 0) {
        free(result);
        return -ENOENT;
    }
    
    strncpy(buf, result, size - 1);
    buf[size - 1] = '\0';
    
    free(result);
    return 0;
}

static int adb_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    adb_shell_t *shell = (adb_shell_t *)fuse_get_context()->private_data;
    
    if (!shell || !shell->connected) {
        return -EIO;
    }
    
    memset(stbuf, 0, sizeof(struct stat));
    
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    
    if (!is_initialized(shell)) {
        if (strcmp(path, "/hello.txt") == 0) {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = 16;
            return 0;
        }
        return -ENOENT;
    }
    
    const char *name = strrchr(path, '/');
    if (!name) name = path;
    else name++;
    
    mode_t cached_mode;
    off_t cached_size;
    
    if (cache_get(name, &cached_mode, &cached_size)) {
        stbuf->st_mode = cached_mode;
        stbuf->st_nlink = S_ISDIR(cached_mode) ? 2 : 1;
        stbuf->st_size = cached_size;
        stbuf->st_uid = 0;
        stbuf->st_gid = 0;
        stbuf->st_atime = 0;
        stbuf->st_mtime = 0;
        return 0;
    }
    
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size = 4096;
    stbuf->st_uid = 0;
    stbuf->st_gid = 0;
    
    return 0;
}

static int adb_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    adb_shell_t *shell = (adb_shell_t *)fuse_get_context()->private_data;
    
    if (!shell || !shell->connected) {
        return -EIO;
    }
    
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    if (!is_initialized(shell)) {
        if (strcmp(path, "/") == 0) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_mode = S_IFREG | 0644;
            st.st_nlink = 1;
            st.st_size = 16;
            filler(buf, "hello.txt", &st, 0, 0);
        }
        return 0;
    }
    
    char *escaped = escape_path(path);
    char cmd[1024];
    
    snprintf(cmd, sizeof(cmd),
             "cd '%s' 2>/dev/null && for f in $(ls -1A 2>/dev/null); do "
             "if [ -L \"$f\" ]; then "
             "echo \"link|$f|0|777\"; "
             "elif [ -d \"$f\" ]; then "
             "echo \"dir|$f|4096|755\"; "
             "else "
             "size=$(stat -c %%s \"$f\" 2>/dev/null || echo 0); "
             "echo \"file|$f|$size|644\"; "
             "fi; "
             "done", escaped);
    
    char *result = shell_exec(shell, cmd);
    if (!result) {
        return -ENOENT;
    }
    
    char *saveptr = NULL;
    char *line = strtok_r(result, "\n", &saveptr);
    
    while (line) {
        char name[256] = {0};
        long long size = 0;
        int perms = 0;
        
        struct stat st;
        memset(&st, 0, sizeof(st));
        
        if (strncmp(line, "link|", 5) == 0) {
            if (sscanf(line, "link|%255[^|]|%lld|%o", name, &size, &perms) == 3) {
                st.st_mode = S_IFLNK | perms;
                st.st_nlink = 1;
                st.st_size = 0;
                cache_add(name, st.st_mode, st.st_size);
                filler(buf, name, &st, 0, 0);
            }
        } else if (strncmp(line, "dir|", 4) == 0) {
            if (sscanf(line, "dir|%255[^|]|%lld|%o", name, &size, &perms) == 3) {
                st.st_mode = S_IFDIR | perms;
                st.st_nlink = 2;
                st.st_size = 4096;
                cache_add(name, st.st_mode, st.st_size);
                filler(buf, name, &st, 0, 0);
            }
        } else if (strncmp(line, "file|", 5) == 0) {
            if (sscanf(line, "file|%255[^|]|%lld|%o", name, &size, &perms) == 3) {
                st.st_mode = S_IFREG | perms;
                st.st_nlink = 1;
                st.st_size = size;
                cache_add(name, st.st_mode, st.st_size);
                filler(buf, name, &st, 0, 0);
            }
        }
        
        line = strtok_r(NULL, "\n", &saveptr);
    }
    
    free(result);
    return 0;
}

static int adb_open(const char *path, struct fuse_file_info *fi) {
    adb_shell_t *shell = (adb_shell_t *)fuse_get_context()->private_data;
    
    if (!shell || !shell->connected) {
        return -EIO;
    }
    
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
    }
    
    if (!is_initialized(shell)) {
        if (strcmp(path, "/hello.txt") == 0) {
            return 0;
        }
        return -ENOENT;
    }
    
    const char *name = strrchr(path, '/');
    if (!name) name = path;
    else name++;
    
    mode_t mode;
    off_t size;
    
    if (cache_get(name, &mode, &size)) {
        if (S_ISDIR(mode) || S_ISLNK(mode)) {
            return -EACCES;
        }
        return 0;
    }
    
    char *escaped = escape_path(path);
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "test -f '%s' && echo OK || echo FAIL", escaped);
    
    char *result = shell_exec(shell, cmd);
    if (!result || !strstr(result, "OK")) {
        free(result);
        return -ENOENT;
    }
    free(result);
    
    return 0;
}

static int adb_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void)fi;
    adb_shell_t *shell = (adb_shell_t *)fuse_get_context()->private_data;
    
    if (!shell || !shell->connected) {
        return -EIO;
    }
    
    if (!is_initialized(shell)) {
        if (strcmp(path, "/hello.txt") == 0) {
            const char *hello_msg = "Hello from FUSE!";
            size_t msg_len = strlen(hello_msg);
            
            if ((size_t)offset >= msg_len) {
                return 0;
            }
            
            size_t copy_size = msg_len - offset;
            if (copy_size > size) copy_size = size;
            
            memcpy(buf, hello_msg + offset, copy_size);
            return (int)copy_size;
        }
        return -ENOENT;
    }
    
    const char *name = strrchr(path, '/');
    if (!name) name = path;
    else name++;
    
    mode_t mode;
    off_t sz;
    
    if (cache_get(name, &mode, &sz)) {
        if (S_ISDIR(mode) || S_ISLNK(mode)) {
            return -EACCES;
        }
    }
    
    char *escaped = escape_path(path);
    char cmd[16384];
    
    snprintf(cmd, sizeof(cmd), 
             "tail -c +%ld '%s' 2>/dev/null | head -c %zu",
             (long)(offset + 1), escaped, size);
    
    char *result = shell_exec(shell, cmd);
    if (!result) {
        return -EIO;
    }
    
    size_t result_len = strlen(result);
    if (result_len == 0) {
        free(result);
        return 0;
    }
    
    size_t copy_size = result_len < size ? result_len : size;
    memcpy(buf, result, copy_size);
    
    free(result);
    return (int)copy_size;
}

static int adb_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    (void)fi;
    return 0;
}

static const struct fuse_operations adb_ops = {
    .getattr = adb_getattr,
    .readdir = adb_readdir,
    .open    = adb_open,
    .read    = adb_read,
    .release = adb_release,
    .readlink = adb_readlink,
};

static int mount_device_fuse(adb_shell_t *shell, const char *mount_point, pid_t *out_pid) {
    pid_t pid = fork();
    if (pid == -1) {
        return -1;
    }

    if (pid == 0) {
        struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
        
        fuse_opt_add_arg(&args, "adb_fuse");
        fuse_opt_add_arg(&args, "-oattr_timeout=1");
        fuse_opt_add_arg(&args, "-oentry_timeout=1");
        fuse_opt_add_arg(&args, "-onegative_timeout=0");
        fuse_opt_add_arg(&args, "-odefault_permissions");

        struct fuse *fuse = fuse_new(&args, &adb_ops, sizeof(adb_ops), (void *)shell);
        if (!fuse) {
            fuse_opt_free_args(&args);
            _exit(1);
        }
        fuse_opt_free_args(&args);

        if (fuse_mount(fuse, mount_point) != 0) {
            fuse_destroy(fuse);
            _exit(1);
        }

        int ret = fuse_loop(fuse);
        
        fuse_unmount(fuse);
        fuse_destroy(fuse);
        
        _exit(ret);
    }

    if (out_pid) *out_pid = pid;
    return 0;
}

static int unmount_device(adb_device_t *dev) {
    if (!dev || dev->fuse_pid <= 0) return 0;
    
    int retries = 3;
    while (retries-- > 0) {
        pid_t umount_pid = fork();
        if (umount_pid == 0) {
            execlp("fusermount3", "fusermount3", "-u", "-z", dev->mount_point, NULL);
            _exit(1);
        } else if (umount_pid > 0) {
            int status;
            waitpid(umount_pid, &status, 0);
        }
        
        struct stat st;
        if (stat(dev->mount_point, &st) != 0) {
            break;
        }
        
        usleep(100000);
    }
    
    if (kill(dev->fuse_pid, 0) == 0) {
        kill(dev->fuse_pid, SIGTERM);
        
        int status;
        int wait_count = 0;
        while (wait_count < 20) {
            pid_t result = waitpid(dev->fuse_pid, &status, WNOHANG);
            if (result == dev->fuse_pid) break;
            if (result == -1) break;
            usleep(100000);
            wait_count++;
        }
        
        if (wait_count >= 20) {
            kill(dev->fuse_pid, SIGKILL);
            waitpid(dev->fuse_pid, &status, 0);
        }
    }
    
    dev->fuse_pid = 0;
    return 0;
}

static adb_device_t *find_device_by_serial(const char *serial) {
    adb_device_t *dev = device_list;
    while (dev) {
        if (strcmp(dev->serial, serial) == 0) return dev;
        dev = dev->next;
    }
    return NULL;
}

static int add_device(const char *serial) {
    adb_device_t *dev = calloc(1, sizeof(adb_device_t));
    if (!dev) return -1;
    
    dev->serial = strdup(serial);
    if (!dev->serial) {
        free(dev);
        return -1;
    }
    
    dev->shell = shell_create(serial);
    if (!dev->shell || shell_connect(dev->shell) != 0) {
        if (dev->shell) shell_disconnect(dev->shell);
        free(dev->serial);
        free(dev);
        return -1;
    }
    
    dev->device_name = get_device_name(dev->shell);
    if (!dev->device_name) {
        dev->device_name = strdup(serial);
    }
    
    const char *user = getenv("USER");
    if (!user) user = "root";
    
    char folder_name[512];
    snprintf(folder_name, sizeof(folder_name), "%s (adb)", dev->device_name);
    
    char mount_point[4096];
    snprintf(mount_point, sizeof(mount_point), "%s/%s/%s/%s", 
             MOUNT_BASE_DIR, user, ADB_SUBDIR, folder_name);
    
    char mkdir_cmd[8192];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\" 2>/dev/null", mount_point);
    if (system(mkdir_cmd) != 0) {
        shell_disconnect(dev->shell);
        free(dev->serial);
        free(dev->device_name);
        free(dev);
        return -1;
    }
    
    struct stat st;
    if (stat(mount_point, &st) != 0 || !S_ISDIR(st.st_mode)) {
        shell_disconnect(dev->shell);
        free(dev->serial);
        free(dev->device_name);
        free(dev);
        return -1;
    }
    
    dev->mount_point = strdup(mount_point);
    if (!dev->mount_point) {
        rmdir(mount_point);
        shell_disconnect(dev->shell);
        free(dev->serial);
        free(dev->device_name);
        free(dev);
        return -1;
    }
    
    dev->active = 1;

    if (mount_device_fuse(dev->shell, dev->mount_point, &dev->fuse_pid) != 0) {
        rmdir(mount_point);
        shell_disconnect(dev->shell);
        free(dev->serial);
        free(dev->device_name);
        free(dev->mount_point);
        free(dev);
        return -1;
    }
    
    pthread_mutex_lock(&list_mutex);
    dev->next = device_list;
    device_list = dev;
    pthread_mutex_unlock(&list_mutex);
    
    LOG_INFO("Added device: %s (%s) at %s", dev->device_name, serial, dev->mount_point);
    
    return 0;
}

static void remove_device(adb_device_t *dev) {
    if (!dev) return;
    
    unmount_device(dev);
    
    usleep(500000);
    
    if (dev->mount_point) {
        rmdir(dev->mount_point);
        
        char parent_dir[4096];
        strncpy(parent_dir, dev->mount_point, sizeof(parent_dir) - 1);
        parent_dir[sizeof(parent_dir) - 1] = '\0';
        
        char *last_slash = strrchr(parent_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            rmdir(parent_dir);
        }
    }
    
    if (dev->shell) {
        shell_disconnect(dev->shell);
        dev->shell = NULL;
    }
    
    pthread_mutex_lock(&list_mutex);
    if (device_list == dev) {
        device_list = dev->next;
    } else {
        adb_device_t *prev = device_list;
        while (prev && prev->next != dev) prev = prev->next;
        if (prev) prev->next = dev->next;
    }
    pthread_mutex_unlock(&list_mutex);
    
    free(dev->serial);
    free(dev->device_name);
    free(dev->mount_point);
    free(dev);
}

static void *main_monitor_loop(void *arg) {
    (void)arg;
    
    while (atomic_load(&keep_running)) {
        FILE *fp = popen("adb devices 2>/dev/null | tail -n +2", "r");
        if (!fp) {
            sleep(ADB_CHECK_INTERVAL);
            continue;
        }

        char *current_serials[128] = {0};
        int current_count = 0;
        
        char line[256];
        while (fgets(line, sizeof(line), fp) && current_count < 128) {
            line[strcspn(line, "\r\n")] = 0;
            
            if (strlen(line) == 0) continue;
            
            char *device_pos = strstr(line, "\tdevice");
            if (!device_pos) {
                device_pos = strstr(line, " device");
            }
            
            if (device_pos) {
                *device_pos = '\0';
                if (strlen(line) > 0) {
                    current_serials[current_count++] = strdup(line);
                }
            }
        }
        pclose(fp);

        for (int i = 0; i < current_count; i++) {
            pthread_mutex_lock(&list_mutex);
            adb_device_t *existing = find_device_by_serial(current_serials[i]);
            pthread_mutex_unlock(&list_mutex);
            
            if (!existing) {
                add_device(current_serials[i]);
            }
        }

        pthread_mutex_lock(&list_mutex);
        adb_device_t *dev = device_list;
        while (dev) {
            adb_device_t *next = dev->next;
            
            int found = 0;
            for (int i = 0; i < current_count; i++) {
                if (strcmp(dev->serial, current_serials[i]) == 0) {
                    found = 1;
                    break;
                }
            }
            
            if (!found) {
                pthread_mutex_unlock(&list_mutex);
                remove_device(dev);
                pthread_mutex_lock(&list_mutex);
            }
            
            dev = next;
        }
        pthread_mutex_unlock(&list_mutex);

        for (int i = 0; i < current_count; i++) {
            free(current_serials[i]);
        }

        sleep(ADB_CHECK_INTERVAL);
    }
    
    return NULL;
}

static void signal_handler(int sig) {
    (void)sig;
    atomic_store(&keep_running, 0);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    const char *user = getenv("USER");
    if (!user) user = "root";
    
    char base_dir[4096];
    snprintf(base_dir, sizeof(base_dir), "%s/%s/%s", MOUNT_BASE_DIR, user, ADB_SUBDIR);
    
    char mkdir_cmd[8192];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", base_dir);
    if (system(mkdir_cmd) != 0) {
        return 1;
    }
    
    LOG_INFO("ADB FUSE Monitor started");
    LOG_INFO("Mount base: %s", base_dir);
    LOG_INFO("Press Ctrl+C to exit.");

    if (pthread_create(&main_monitor_thread, NULL, main_monitor_loop, NULL) != 0) {
        return 1;
    }
    pthread_detach(main_monitor_thread);

    while (atomic_load(&keep_running)) {
        sleep(1);
    }

    pthread_mutex_lock(&list_mutex);
    adb_device_t *dev = device_list;
    while (dev) {
        adb_device_t *next = dev->next;
        pthread_mutex_unlock(&list_mutex);
        remove_device(dev);
        pthread_mutex_lock(&list_mutex);
        dev = next;
    }
    device_list = NULL;
    pthread_mutex_unlock(&list_mutex);

    rmdir(base_dir);
    
    LOG_INFO("Done.");
    return 0;
}
