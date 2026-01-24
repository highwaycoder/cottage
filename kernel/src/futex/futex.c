#include <lock/lock.h>
#include <futex/futex.h>
#include <klog/klog.h>

lock_t futex_lock = LOCK_INITIALIZER("futex_lock");

void futex_init()
{
    klog("futex", "Futexes are not yet properly implemented");
}
