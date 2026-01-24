#pragma clang diagnostic push
// ignore unused parameters, this file will be full of them due to implementing
// of an interface that expects us to do more than we actually want to do
#pragma clang diagnostic ignored "-Wunused-parameter"

// first-party headers
#include <random/random.h>
#include <fs/devtmpfs.h>
#include <cpu/cpu.h>
#include <klog/klog.h>
#include <mem/pmm.h>

// standard headers
#include <string.h>

bool ur_rdrand = false;
bool ur_rdseed = false;
dev_random_t devrandom;

// --- magic from wikipedia ---
// slightly modified to work with key/buffer rather than straight in->out
#define ROTL(a,b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d)(		\
	b ^= ROTL(a + d, 7),	\
	c ^= ROTL(b + a, 9),	\
	d ^= ROTL(c + b,13),	\
	a ^= ROTL(d + c,18))
#define ROUNDS 10
 
void salsa20_block(uint32_t out[16], uint32_t const key[16], uint32_t buf[16])
{
	int i;
	uint32_t x[16];

	for (i = 0; i < 16; ++i)
		x[i] = buf[i];

	// 10 loops × 2 rounds/loop = 20 rounds
	for (i = 0; i < ROUNDS; i += 2) {
		// Odd round
		QR(x[ 0], x[ 4], x[ 8], x[12]);	// column 1
		QR(x[ 5], x[ 9], x[13], x[ 1]);	// column 2
		QR(x[10], x[14], x[ 2], x[ 6]);	// column 3
		QR(x[15], x[ 3], x[ 7], x[11]);	// column 4
		// Even round
		QR(x[ 0], x[ 1], x[ 2], x[ 3]);	// row 1
		QR(x[ 5], x[ 6], x[ 7], x[ 4]);	// row 2
		QR(x[10], x[11], x[ 8], x[ 9]);	// row 3
		QR(x[15], x[12], x[13], x[14]);	// row 4
	}
	for (i = 0; i < 16; ++i)
		out[i] = x[i] + key[i];
    
    for(i = 0; i < 16; i++)
        buf[i] = x[i];
}
// --- end magic from wikipedia --- 

void random_init()
{
    // this is misleading - although we use the TSC for the initial seed,
    // we immediatelly reseed with RDSEED so it's fine.
    uint64_t seed = cpu_rdtsc();
    uint64_t bufseed = cpu_rdtsc();
    uint32_t a,b,c,d;
    bool success = cpu_id(1, 0, &a, &b, &c, &d);
    if(success && (c & (1 << 30)) != 0)
    {
        klog("urandom", "rdrand available");
        ur_rdrand = true;
    }
    success = cpu_id(7, 0, &a, &b, &c, &d);
    if (success && (b & (1 << 18)) != 0)
    {
        klog("urandom", "rdseed available");
        ur_rdseed = true;
    }

    devrandom = (dev_random_t){

        .resource = (resource_t){
            .stat.size = 0,
            .stat.blocks = 0,
            .stat.block_size = 4096,
            .stat.rdev = resource_create_dev_id(),
            .stat.mode = 0666 | STAT_IFCHR,
            .can_mmap = true,
        },

        .rng_lock = LOCK_INITIALIZER("devrandom->rng_lock"),

        .key[0] = seed,
        .key[2] = (seed >> 32),
        // here
        .buffer[0] = bufseed,
        .buffer[2] = (bufseed >> 32)
    };

    random_reseed(&devrandom);

    devtmpfs_add_device((resource_t*)&devrandom, "urandom");
}

void random_reseed(dev_random_t* self)
{
    // CAUTION!
    // if neither RDSEED nor RDRAND are available, reseeding is a no-op
    // this can cause security bugs, so be very careful what you run on systems
    // that don't support either
    if(ur_rdseed)
    {
        for (int i = 0; i < 16; i++)
        {
            self->key[i] ^= cpu_rdseed32();
        }
    } 
    else if (ur_rdrand)
    {
        for (int i = 0; i < 16; i++)
        {
            self->key[i] ^= cpu_rdrand32();
        }
    }
}

bool random_grow(resource_t* self, void* handle, uint64_t size)
{
	return true;
}

int64_t random_read(resource_t* _self, void* handle, void* buf, uint64_t loc, uint64_t count)
{
    if (count == 0) return 0;
    dev_random_t* self = (dev_random_t*) _self;

    lock_acquire(&self->rng_lock);


    uint32_t out[16];

    self->reseed_ctr += count;
    if (self->reseed_ctr >= 2048)
    {
        self->reseed_ctr = 0;
        random_reseed(self);
    }

    while(1)
    {
        if (count > 64)
        {
            salsa20_block(out, self->key, self->buffer);
            memcpy(buf, out, 64);
            buf += 64;
            count -= 64;
        } else
        {
            salsa20_block(out, self->key, self->buffer);
            memcpy(buf, out, count);
            break;
        }
    }

    lock_release(&self->rng_lock);

    return count;
}

int64_t random_write(resource_t* self, void* handle, void* buf, uint64_t loc, uint64_t count)
{
    return count;
}

int random_ioctl(resource_t* self, void* handle, uint64_t request, void* argp)
{
    return resource_default_ioctl(handle, request, argp);
}

bool random_unref(resource_t* self, void* handle)
{
    atomic_fetch_sub(&self->refcount, 1);
    return true;
}

bool random_link(resource_t* self, void* handle)
{
    atomic_fetch_add(&self->stat.nlink, 1);
    return true;
}

bool random_unlink(resource_t* self, void* handle)
{
    atomic_fetch_sub(&self->stat.nlink, 1);
    return true;
}

void* random_mmap(resource_t* self, uint64_t page, int flags)
{
    return pmm_alloc(1);
}

#pragma clang diagnostic pop
