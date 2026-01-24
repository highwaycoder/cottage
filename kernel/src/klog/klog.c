/**
 * This is just a very simple ring buffer based logging system,
 * really only meant for internal kernel use.
 */

#include <klog/klog.h>
#include <math/si.h>
#include <lock/lock.h>
#include <mem/malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <term/term.h>

// 128K ring buffer - tune as needed
#define KLOG_BUFSIZE KiB(128)

static char klog_buf[KLOG_BUFSIZE];
static size_t log_end = 0;

// couple of kernel booleans to help us track which features have been
// bootstrapped so far
extern bool have_term;
extern bool have_malloc;

lock_t klog_lock = {
    .is_locked = false
};

void klog_putc(char c);

// Syscall handler for SYS_KLOG
// arg0: pointer to message string (const char*)
// arg1-5: unused (reserved for future use)
// Note: This is a simple logging syscall - it just prints the string as-is
// For format strings, the formatting should be done in userspace
uint64_t syscall_klog(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                      uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char* msg = (const char*)arg0;

    // Basic validation - make sure pointer looks reasonable
    // TODO: proper userspace pointer validation
    if (msg == NULL) {
        return (uint64_t)-1;
    }

    klog("user", "%s", msg);
    return 0;
}

void vklog(const char* module, const char* fmt, va_list args)
{
    size_t log_end_start = log_end;
    klog_putc('[');
    while (*module)
    {
        klog_buf[log_end++] = *module;
        if (log_end == KLOG_BUFSIZE)
            log_end = 0;
        module++;
    }
    klog_putc(']');
    klog_putc(' ');

    int max_chars = KLOG_BUFSIZE - log_end - 1;
    int chars_written = vsnprintf(klog_buf + log_end, max_chars, fmt, args);

    if (chars_written > max_chars)
    {
        // output will simply get truncated if we don't have malloc yet,
        // this allows us to use klog calls earlier in the boot process
        // while not trying to call an uninitialized malloc.
        if (have_malloc)
        {
            char *line_buf = malloc(chars_written);
            vsnprintf(line_buf, chars_written, fmt, args);
            memcpy(klog_buf, line_buf + max_chars, max_chars - chars_written);
            log_end = max_chars - chars_written;
            free(line_buf);
        }
        else
        {
            // we wrote up to the end of the buffer, and then stopped because
            // we don't have malloc yet (truncating the log entry)
            log_end = 0;
        }
    }
    else
    {
        log_end += chars_written;
    }

    // users shouldn't need to put their own newlines in
    klog_putc('\n');

    // if we don't have a terminal, hopefully by the time we do have one
    // we can start printing to it
    if (have_term)
    {
        // write the buffer out to tty0 as we go
        // deal with the nasty case of a split log that's
        // half at the start of the buffer and half at the end
        if (log_end_start > log_end)
        {
            // write the bits from the end of the buffer
            term_write(klog_buf + log_end_start, KLOG_BUFSIZE - log_end_start);
            // write the bytes from the start of the buffer
            term_write(klog_buf, log_end);
        }
        else
        {
            //      term_printf("Printing");
            term_write(klog_buf + log_end_start, (log_end - log_end_start));
        }
    }
}

void klog(const char *module, const char *fmt, ...)
{
    lock_acquire(&klog_lock);
    va_list args;
    va_start(args, fmt);
    vklog(module, fmt, args);
    va_end(args);
    lock_release(&klog_lock);
}

#ifdef COTTAGE_DEBUG
void klog_debug(const char* module, const char* fmt, ...)
{
    lock_acquire(&klog_lock);
    va_list args;
    va_start(args, fmt);
    vklog(module, fmt, args);
    va_end(args);
    lock_release(&klog_lock);
}
#endif

void klog_putc(char c)
{
    klog_buf[log_end++] = c;
    if (log_end == KLOG_BUFSIZE)
        log_end = 0;
}
