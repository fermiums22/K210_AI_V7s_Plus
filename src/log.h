#ifndef APP_LOG_H
#define APP_LOG_H

void log_init(void);
void log_puts(const char *s);
void log_printf(const char *fmt, ...);

#define LOG(s) log_puts(s)
#define LOGF(...) log_printf(__VA_ARGS__)

#endif
