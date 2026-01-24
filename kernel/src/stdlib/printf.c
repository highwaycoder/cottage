// kernel headers
#include <klog/klog.h>
#include <math/minmax.h>
#include <term/term.h>

// c stdlib headers
#include <lock/lock.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// bunch of printf state machine variables
// todo: refactor into enums
#define PRINTF_STATE_NORMAL 0
#define PRINTF_STATE_LENGTH 1
#define PRINTF_STATE_LENGTH_SHORT 2
#define PRINTF_STATE_LENGTH_LONG 3
#define PRINTF_STATE_SPEC 4

#define PRINTF_LENGTH_DEFAULT 0
#define PRINTF_LENGTH_SHORT_SHORT 1
#define PRINTF_LENGTH_SHORT 2
#define PRINTF_LENGTH_LONG 3
#define PRINTF_LENGTH_LONG_LONG 4

static lock_t printf_lock = LOCK_INITIALIZER("printf_lock");

// forward declaration of "local" functions
int vsnprintf_unsigned(char **str, size_t n, unsigned long long number,
                       int radix);
int vsnprintf_signed(char **str, size_t n, long long number, int radix);
void snputc(char **str, size_t n, char c);

void snputc(char **str, size_t n, char c)
{
    if (n > 0)
    {
        **str = c;
        (*str)++;
    }
}

int snprintf(char *s, size_t max, const char *fmt, ...)
{
    va_list arg;
    int done;

    va_start(arg, fmt);
    done = vsnprintf(s, max, fmt, arg);
    va_end(arg);

    return done;
}

#define MAX_PRINTF 4096

int vsnprintf(char *str, size_t n, const char *fmt, va_list args)
{
    lock_acquire(&printf_lock);
    int state = PRINTF_STATE_NORMAL;
    int length = PRINTF_LENGTH_DEFAULT;
    int radix = 10;
    bool sign = false;
    bool number = false;
    const size_t orig_size = n; // so we can return how many bytes we wrote

    while (*fmt)
    {
        switch (state)
        {
        case PRINTF_STATE_NORMAL:
            switch (*fmt)
            {
            case '%':
                state = PRINTF_STATE_LENGTH;
                break;
            default:
                snputc(&str, n--, *fmt);
                break;
            }
            break;

        case PRINTF_STATE_LENGTH:
            switch (*fmt)
            {
            case 'h':
                length = PRINTF_LENGTH_SHORT;
                state = PRINTF_STATE_LENGTH_SHORT;
                break;
            case 'l':
                length = PRINTF_LENGTH_LONG;
                state = PRINTF_STATE_LENGTH_LONG;
                break;
            default:
                goto PRINTF_STATE_SPEC_;
            }
            break;

        case PRINTF_STATE_LENGTH_SHORT:
            if (*fmt == 'h')
            {
                length = PRINTF_LENGTH_SHORT_SHORT;
                state = PRINTF_STATE_SPEC;
            }
            else
                goto PRINTF_STATE_SPEC_;
            break;

        case PRINTF_STATE_LENGTH_LONG:
            if (*fmt == 'l')
            {
                length = PRINTF_LENGTH_LONG_LONG;
                state = PRINTF_STATE_SPEC;
            }
            else
                goto PRINTF_STATE_SPEC_;
            break;

        case PRINTF_STATE_SPEC:
        PRINTF_STATE_SPEC_:
            switch (*fmt)
            {
            case 'c':
                snputc(&str, n--, (char)va_arg(args, int));
                break;

            case 's':
            {
                const char *s = va_arg(args, const char *);
                for (size_t i = 0; i < strlen(s); i++)
                {
                    snputc(&str, n--, s[i]);
                }

                break;
            }

            case '%':
                snputc(&str, n--, '%');
                break;

            case 'd':
            case 'i':
                radix = 10;
                sign = true;
                number = true;
                break;

            case 'u':
                radix = 10;
                sign = false;
                number = true;
                break;

            case 'X':
            case 'x':
            case 'p':
                radix = 16;
                sign = false;
                number = true;
                break;

            case 'o':
                radix = 8;
                sign = false;
                number = true;
                break;

            // ignore invalid spec
            default:
                break;
            }

            if (number)
            {
                if (sign)
                {
                    switch (length)
                    {
                    case PRINTF_LENGTH_SHORT_SHORT:
                    case PRINTF_LENGTH_SHORT:
                    case PRINTF_LENGTH_DEFAULT:
                        n -= vsnprintf_signed(&str, n, va_arg(args, int), radix);
                        break;

                    case PRINTF_LENGTH_LONG:
                        n -= vsnprintf_signed(&str, n, va_arg(args, long), radix);
                        break;

                    case PRINTF_LENGTH_LONG_LONG:
                        n -= vsnprintf_signed(&str, n, va_arg(args, long long), radix);
                        break;
                    }
                }
                else
                {
                    switch (length)
                    {
                    case PRINTF_LENGTH_SHORT_SHORT:
                    case PRINTF_LENGTH_SHORT:
                    case PRINTF_LENGTH_DEFAULT:
                        n -= vsnprintf_unsigned(&str, n, va_arg(args, unsigned int), radix);
                        break;

                    case PRINTF_LENGTH_LONG:
                        n -=
                            vsnprintf_unsigned(&str, n, va_arg(args, unsigned long), radix);
                        break;

                    case PRINTF_LENGTH_LONG_LONG:
                        n -= vsnprintf_unsigned(&str, n, va_arg(args, unsigned long long),
                                                radix);
                        break;
                    }
                }
            }

            // reset state
            state = PRINTF_STATE_NORMAL;
            length = PRINTF_LENGTH_DEFAULT;
            radix = 10;
            sign = false;
            number = false;
            break;
        default:
            klog("stdio", "Unrecognised printf state");
            break;
        }

        fmt++;
    }

    // add a nul byte at the end as per the spec
    // but exclude it from the count of bytes written
    snputc(&str, n, '\0');
    lock_release(&printf_lock);
    // n is 0 if number of bytes written == orig_size
    // n is <0 if number of bytes written > orig_size
    // n is >0 if number of bytes written < orig_size
    // so num bytes written == orig_size + (-n) (right?)
    return orig_size + (-n);
}

const char g_HexChars[] = "0123456789abcdef";

int vsnprintf_unsigned(char **str, size_t n, unsigned long long number,
                       int radix)
{
    char buffer[32];
    int pos = 0;
    int ret = 0;

    // handle 0 as a special case (otherwise it won't get handled)
    if (number == 0)
    {
        snputc(str, 1, '0');
        return 1;
    }

    // convert number to ASCII
    do
    {
        unsigned long long rem = number % radix;
        number /= radix;
        buffer[pos++] = g_HexChars[rem];
    } while (number > 0);

    // print number in reverse order
    while (n > 0 && pos-- >= 0)
    {
        snputc(str, n, (buffer[pos]));
        // every time we iterate through this loop, we have written a byte
        ret++;
    }

    return ret;
}

int vsnprintf_signed(char **str, size_t n, long long number, int radix)
{
    if (number < 0)
    {
        snputc(str, n, '-');
        return vsnprintf_unsigned(str, n, -number, radix) + 1;
    }
    else
        return vsnprintf_unsigned(str, n, number, radix);
}
