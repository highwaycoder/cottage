// currently only support the flanterm backend
// future, maybe more? (unlikely though)

#include <term/term.h>
#include <lock/lock.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <mem/malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <macro.h>
#include <cpu/smp.h>
#include <serial/serial.h>

extern bool have_term;

static struct flanterm_context *ctx;

static lock_t term_lock = LOCK_INITIALIZER("term_lock");

void term_putc(const char c);

void term_init(uint32_t *framebuffer, size_t width, size_t height,
               size_t pitch)
{
    ctx = flanterm_fb_simple_init(framebuffer, width, height, pitch);
    ctx->autoflush = false; // disable autoflushing for SMP reasons
}

// Internal write function - caller must hold term_lock
static void term_write_unlocked(const char *string, size_t count)
{
    flanterm_write(ctx, string, count);
    print_serial(string);
    print_serial("\r"); // add a carriage return after each write on serial
    if(have_smp)
    {
        // Save and restore interrupt state rather than unconditionally enabling
        // This allows klog_unlocked to work safely from ISR context
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        ctx->double_buffer_flush(ctx);
        // Only restore IF if it was previously set
        if (flags & (1 << 9)) {
            asm volatile("sti" ::: "memory");
        }
    }
    else
    {
        ctx->double_buffer_flush(ctx);
    }
}

void term_write(const char *string, size_t count)
{
    have_term = false; // stops a PANIC loop
    // term writes must be locked since async writes will corrupt the framebuffer
    lock_acquire(&term_lock);
    term_write_unlocked(string, count);
    lock_release(&term_lock);
    have_term = true;
}

void term_putc(const char c) { term_write(&c, 1); }


void term_vprintf(const char* fmt, va_list args)
{
    lock_acquire(&term_lock);
    char buf[256] = {0};
    vsnprintf(buf, 256, fmt, args);
    size_t len = strlen(buf);
    term_write_unlocked(buf, len);
    // ensure newline is printed
    if(len > 0 && buf[len-1] != '\n')
        term_write_unlocked("\n", 1);
    lock_release(&term_lock);
}

void term_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    term_vprintf(fmt, args);
    va_end(args);
}
