/*
 * s9mali: first userland client of the kernel's SweetMali driver (patch 0144).
 * Maps the GPU memory, builds the same Write value job as the kernel self-test
 * from userland, then runs repeat jobs under different conditions.
 *
 * The iOS SDK ships IOKit without headers, so the few declarations needed are
 * below. Link with -framework IOKit.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef mach_port_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *input,
    uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);

#define kIOMapAnywhere  1
#define SEL_INFO        0
#define SEL_ALLOC       1
#define SEL_SUBMIT      2
#define SEL_FREE        3
#define SEL_CONFIGURE   4
#define JOB_SLOT        1
#define MAGIC           0xc0ffee42u
#define RUNS            1000
#define ATTR_NC         0x4c4c4c4cu
#define ATTR_IMPL       0x88888888u
#define M_NEWPAGES      1u      /* job and target pages never used before */

static io_connect_t conn;
static volatile uint8_t *mem;
static uint64_t base;

static uint32_t
gpu_alloc(uint32_t size)
{
	uint64_t in = size, out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_ALLOC, &in, 1, &out, &n);
	if (kr != KERN_SUCCESS) {
		printf("alloc %u: %#x\n", size, kr);
		return 0;
	}
	return (uint32_t)out;
}

static kern_return_t
configure(uint32_t attr, bool flush)
{
	uint64_t in[3] = { attr, 0, flush };
	return IOConnectCallScalarMethod(conn, SEL_CONFIGURE, in, 3, NULL, NULL);
}

/* one Write value job: 64-bit descriptor, type 2, index 1, Immediate 32 */
static void
job_write(volatile uint32_t *j, uint32_t target, uint32_t value)
{
	for (int i = 0; i < 16; i++) {
		j[i] = 0;
	}
	j[4] = 1u | 2u << 1 | 1u << 16;
	j[8] = target;
	j[10] = 6;
	j[12] = value;
}

static kern_return_t
submit(uint32_t job, uint64_t *r)
{
	uint64_t in[2] = { job, JOB_SLOT };
	uint32_t n = 6;
	/* an error reply carries no outputs: the kernel log has them */
	memset(r, 0, 6 * sizeof(*r));
	return IOConnectCallScalarMethod(conn, SEL_SUBMIT, in, 2, r, &n);
}

static int
run_mode(const char *name, uint32_t job, uint32_t target, unsigned mode)
{
	static uint32_t value = MAGIC;
	uint64_t r[6];
	int rejected = 0, wrong = 0, late = 0, shown = 0;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < RUNS; i++) {
		uint32_t want = ++value;
		if ((mode & M_NEWPAGES) && ((job = gpu_alloc(4096)) == 0 || (target = gpu_alloc(4096)) == 0)) {
			return 1;
		}
		volatile uint32_t *j = (volatile uint32_t *)(mem + (job - base));
		volatile uint32_t *t = (volatile uint32_t *)(mem + (target - base));
		job_write(j, target, want);
		kern_return_t kr = submit(job, r);
		uint32_t got = t[0], ex = j[0];
		if (kr != KERN_SUCCESS) {
			rejected++;
		} else if (got != want) {
			wrong++;
			usleep(1000);
			if (t[0] == want) {
				late++;
			}
		}
		if ((kr != KERN_SUCCESS || got != want) && shown < 2) {
			shown++;
			printf("  run %u: kr %#x want %#x got %#x exception %#x\n", i, kr, want, got, ex);
		}
	}
	uint64_t ns = (mach_absolute_time() - t0) * tb.numer / tb.denom;
	printf("%s: %d runs, %d rejected, %d wrong value (%d right 1 ms later), %llu us per run\n",
	    name, RUNS, rejected, wrong, late, ns / 1000 / RUNS);
	return rejected + wrong != 0;
}

int
main(void)
{
	uint64_t info[4], r[6];
	uint32_t n = 4;
	mach_vm_address_t addr = 0;
	mach_vm_size_t size = 0;
	setvbuf(stdout, NULL, _IOLBF, 0);

	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetMali"));
	if (svc == MACH_PORT_NULL) {
		printf("FAIL: no SweetMali service (boot-arg sweetmali?)\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		printf("FAIL: IOServiceOpen %#x\n", kr);
		return 1;
	}
	kr = IOConnectCallScalarMethod(conn, SEL_INFO, NULL, 0, info, &n);
	if (kr != KERN_SUCCESS) {
		printf("FAIL: info %#x\n", kr);
		return 1;
	}
	base = info[2];
	printf("GPU_ID %#llx shaders %#llx memory VA %#llx size %#llx\n", info[0], info[1], info[2], info[3]);
	kr = IOConnectMapMemory64(conn, 0, mach_task_self(), &addr, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		printf("FAIL: map %#x\n", kr);
		return 1;
	}
	mem = (volatile uint8_t *)(uintptr_t)addr;
	printf("mapped at %#llx size %#llx\n", (unsigned long long)addr, (unsigned long long)size);

	uint32_t job = gpu_alloc(4096), target = gpu_alloc(4096);
	if (job == 0 || target == 0) {
		printf("FAIL: alloc\n");
		return 1;
	}
	volatile uint32_t *j = (volatile uint32_t *)(mem + (job - base));
	volatile uint32_t *t = (volatile uint32_t *)(mem + (target - base));

	job_write(j, target, MAGIC);
	kr = submit(job, r);
	printf("first job: kr %#x target %#x (want %#x) exception %#x; raw %#llx status %#llx mmu %#llx fault %#llx addr %#llx gpu %#llx\n",
	    kr, t[0], MAGIC, j[0], r[0], r[1], r[2], r[3], r[4], r[5]);
	if (kr != KERN_SUCCESS || t[0] != MAGIC) {
		printf("FAIL: first job\n");
		return 1;
	}

	/*
	 * 0x88 passed 400 of 400 on boot-ios50 but only ever ran after 0x4c jobs.
	 * Here it is the default from reset and runs first; the 0x4c control shows
	 * the failure is still there and that 0x88 cures it afterwards as well. The
	 * no-flush modes ask whether the job's own start and end flushes are enough.
	 */
	int bad = 0;
	bad += run_mode("0x88 same pages, flushes", job, target, 0);
	bad += run_mode("0x88 new pages, flushes", job, target, M_NEWPAGES);
	(void)configure(0, false);
	bad += run_mode("0x88 same pages, no flushes", job, target, 0);
	bad += run_mode("0x88 new pages, no flushes", job, target, M_NEWPAGES);
	(void)configure(ATTR_NC, true);
	int control = run_mode("0x4c same pages, control", job, target, 0);
	(void)configure(ATTR_IMPL, true);
	bad += run_mode("0x88 same pages, after 0x4c", job, target, 0);

	IOConnectCallScalarMethod(conn, SEL_FREE, NULL, 0, NULL, NULL);
	IOServiceClose(conn);
	printf("0x4c control %s\n", control ? "fails as expected" : "PASSED: 0x4c may not be the cause");
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad != 0;
}
