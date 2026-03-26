/*
 * HEADER ONLY LOG
 */

#ifndef __MLOG_H_
#define __MLOG_H_

#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <syslog.h>

#ifndef __FILENAME__
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#endif

/* void openlog(const char *ident, int option, int facility);
 * openlog() 的使用是可选的，如果没有调用，它将在第一次调用 syslog() 时自动调用，在这种情况下，ident 将默认为 NULL。
 *
 * ident: 指向的字符串被添加到每条日志信息的前面，一般指定为程序名。如果 ident 为 NULL，则使用程序名称。
 * option: LOG_PID 在每条日志信息中添加进程 ID。
 *         LOG_CONS 如果无法将消息发送到系统日志守护进程，则将消息打印到系统控制台。
 * facility: 指定日志的类型，常用的有 LOG_USER（用户级消息）和 LOG_DAEMON（系统守护进程消息）。
 *
 * 使用 prctl 获取进程名，并传入 openlog，主要是因为 musl 中并没有默认使用进程名作为 ident。
 */
#define MLOG_OPEN()                                       \
    do {                                                  \
        char proc_name[32] = { 0 };                       \
        prctl(PR_GET_NAME, proc_name, 0, 0, 0);           \
        openlog(proc_name, LOG_PID | LOG_CONS, LOG_USER); \
    } while (0)
#define MLOG_CLOSE() closelog()

// - `LOG_EMERG`: system is unusable
// - `LOG_ALERT`: action must be taken immediately
// - `LOG_CRIT`: critical conditions
// - `LOG_ERR`: error conditions
// - `LOG_WARNING`: warning conditions
// - `LOG_NOTICE`: normal, but significant, condition
// - `LOG_INFO`: informational message
// - `LOG_DEBUG`: debug-level message
#define MLOG_EMERG(fmt, ...) syslog(LOG_EMERG, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_ALERT(fmt, ...) syslog(LOG_ALERT, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_CRIT(fmt, ...) syslog(LOG_CRIT, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_ERR(fmt, ...) syslog(LOG_ERR, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_WARN(fmt, ...) syslog(LOG_WARNING, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_NOTICE(fmt, ...) syslog(LOG_NOTICE, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_INFO(fmt, ...) syslog(LOG_INFO, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)
#define MLOG_DBG(fmt, ...) syslog(LOG_DEBUG, "UI-[%s:%d %s] " fmt, __FILENAME__, __LINE__, __func__, ##__VA_ARGS__)

#endif /* __MLOG_H_ */
