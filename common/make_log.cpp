#include "make_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <string>

namespace {

const size_t LOG_PATH_LEN = 1024;
const size_t LOG_MSG_LEN = 4096;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    int get() const { return fd_; }

    int close_now()
    {
        int fd = fd_;
        fd_ = -1;
        return close(fd);
    }

private:
    int fd_;
};

class MutexGuard {
public:
    explicit MutexGuard(pthread_mutex_t *mutex) : mutex_(mutex)
    {
        pthread_mutex_lock(mutex_);
    }

    ~MutexGuard()
    {
        pthread_mutex_unlock(mutex_);
    }

    MutexGuard(const MutexGuard &) = delete;
    MutexGuard &operator=(const MutexGuard &) = delete;

private:
    pthread_mutex_t *mutex_;
};

bool get_local_time(struct tm *now)
{
    time_t t = time(NULL);
    if (localtime_r(&t, now) == NULL) {
        fprintf(stderr, "localtime_r failed\n");
        return false;
    }

    return true;
}

int mkdir_if_missing(const std::string &path)
{
    if (mkdir(path.c_str(), 0755) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }

    fprintf(stderr, "create %s failed: %s\n", path.c_str(), strerror(errno));
    return -1;
}

std::string format_string(const char *fmt, ...)
{
    std::array<char, LOG_PATH_LEN> buf;
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf.data(), buf.size(), fmt, ap);
    va_end(ap);

    if (ret < 0 || static_cast<size_t>(ret) >= buf.size()) {
        return std::string();
    }

    return std::string(buf.data());
}

bool build_log_path(const char *module_name, const char *proc_name, std::string *path)
{
    struct tm now;
    std::string module_dir;
    std::string year_dir;
    std::string month_dir;

    if (module_name == NULL || proc_name == NULL || path == NULL) {
        return false;
    }

    if (!get_local_time(&now)) {
        return false;
    }

    if (mkdir_if_missing("./logs") != 0) {
        return false;
    }

    module_dir = format_string("./logs/%s", module_name);
    if (module_dir.empty() || mkdir_if_missing(module_dir) != 0) {
        return false;
    }

    year_dir = format_string("%s/%04d", module_dir.c_str(), now.tm_year + 1900);
    if (year_dir.empty() || mkdir_if_missing(year_dir) != 0) {
        return false;
    }

    month_dir = format_string("%s/%02d", year_dir.c_str(), now.tm_mon + 1);
    if (month_dir.empty() || mkdir_if_missing(month_dir) != 0) {
        return false;
    }

    *path = format_string("%s/%s-%02d.log", month_dir.c_str(), proc_name, now.tm_mday);
    return !path->empty();
}

int write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t ret = write(fd, buf + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (ret == 0) {
            errno = EIO;
            return -1;
        }

        written += static_cast<size_t>(ret);
    }

    return 0;
}

} // namespace

pthread_mutex_t ca_log_lock = PTHREAD_MUTEX_INITIALIZER;

int make_path(char *path, const char *module_name, const char *proc_name)
{
    std::string log_path;

    if (path == NULL || !build_log_path(module_name, proc_name, &log_path)) {
        return -1;
    }

    if (log_path.size() >= LOG_PATH_LEN) {
        fprintf(stderr, "log path too long\n");
        return -1;
    }

    snprintf(path, LOG_PATH_LEN, "%s", log_path.c_str());
    return 0;
}

int out_put_file(const char *path, const char *buf)
{
    int raw_fd;

    if (path == NULL || buf == NULL) {
        return -1;
    }

    raw_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    FileDescriptor fd(raw_fd);
    if (fd.get() < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (write_all(fd.get(), buf, strlen(buf)) != 0) {
        fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (fd.close_now() != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

int dumpmsg_to_file(const char *module_name, const char *proc_name, const char *filename,
                    int line, const char *funcname, const char *fmt, ...)
{
    std::array<char, LOG_MSG_LEN> mesg;
    std::array<char, LOG_MSG_LEN> buf;
    std::string filepath;
    struct tm now;
    va_list ap;
    int ret;

    (void)funcname;

    if (module_name == NULL || proc_name == NULL || filename == NULL || fmt == NULL) {
        return -1;
    }

    if (!get_local_time(&now)) {
        return -1;
    }

    va_start(ap, fmt);
    vsnprintf(mesg.data(), mesg.size(), fmt, ap);
    va_end(ap);

    ret = snprintf(buf.data(), buf.size(),
                   "[%04d-%02d-%02d %02d:%02d:%02d]--[%s:%d]--%s",
                   now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                   now.tm_hour, now.tm_min, now.tm_sec, filename, line, mesg.data());
    if (ret < 0) {
        fprintf(stderr, "format log message failed\n");
        return -1;
    }

    if (!build_log_path(module_name, proc_name, &filepath)) {
        return -1;
    }

    {
        MutexGuard lock(&ca_log_lock);
        ret = out_put_file(filepath.c_str(), buf.data());
    }

    return ret;
}
