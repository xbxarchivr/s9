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
#define SEL_WRITEJOB    5
#define JOB_SLOT        1
#define MAGIC           0xc0ffee42u
#define RUNS            100
#define ATTR_NC         0x4c4c4c4cu     /* what Linux uses for a non-coherent GPU */
#define ATTR_IMPL       0x88888888u     /* implementation-defined cacheable */
#define M_NEWJOB        1u      /* a job descriptor page never used before */
#define M_NEWTARGET     2u      /* a target page never used before */
#define M_RESET         4u      /* soft reset the GPU before each job */
#define M_UNCACHED      8u      /* access the memory through the uncached mapping */
#define M_KWRITE        16u     /* the kernel writes the job through its own mapping */

static io_connect_t conn;
static volatile uint8_t *mem, *mem_uc;
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
configure(uint32_t attr, bool reset)
{
	uint64_t in[2] = { attr, reset };
	return IOConnectCallScalarMethod(conn, SEL_CONFIGURE, in, 2, NULL, NULL);
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

/*
 * Repeat jobs fail on the G72: status 0x58 or an old value in the target, and
 * neither a wait before submitting nor a whole-GPU cache flush cures it
 * (boot-ios48, 49). The first job after a reset has never failed. Each mode
 * changes one thing: GPU memory attribute, reset, which mapping writes the job.
 */
static int
run_mode(const char *name, uint32_t job, uint32_t target, unsigned mode)
{
	static uint32_t value = MAGIC;
	volatile uint8_t *m = (mode & M_UNCACHED) ? mem_uc : mem;
	uint64_t r[6];
	int rejected = 0, wrong = 0, late = 0, shown = 0;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < RUNS; i++) {
		uint32_t want = ++value;
		if ((mode & M_NEWJOB) && (job = gpu_alloc(4096)) == 0) {
			return 1;
		}
		if ((mode & M_NEWTARGET) && (target = gpu_alloc(4096)) == 0) {
			return 1;
		}
		volatile uint32_t *j = (volatile uint32_t *)(m + (job - base));
		volatile uint32_t *t = (volatile uint32_t *)(m + (target - base));
		if (mode & M_RESET) {
			kern_return_t kr = configure(0, true);
			if (kr != KERN_SUCCESS) {
				printf("  run %u: reset %#x\n", i, kr);
				return 1;
			}
		}
		if (mode & M_KWRITE) {
			uint64_t in[3] = { job, target, want };
			(void)IOConnectCallScalarMethod(conn, SEL_WRITEJOB, in, 3, NULL, NULL);
		} else {
			job_write(j, target, want);
		}
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
			printf("  run %u: kr %#x want %#x got %#x (want-1: %d) exception %#x\n",
			    i, kr, want, got, got == want - 1, ex);
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
	mach_vm_address_t addr = 0, addr_uc = 0;
	mach_vm_size_t size = 0, size_uc = 0;
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
	kr = IOConnectMapMemory64(conn, 1, mach_task_self(), &addr_uc, &size_uc, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		printf("FAIL: uncached map %#x\n", kr);
		return 1;
	}
	mem_uc = (volatile uint8_t *)(uintptr_t)addr_uc;
	printf("mapped at %#llx, uncached at %#llx\n", (unsigned long long)addr, (unsigned long long)addr_uc);

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

	int bad = 0;
	bad += run_mode("0x4c same pages", job, target, 0);
	bad += run_mode("0x4c reset each job", job, target, M_RESET);
	bad += run_mode("0x4c uncached mapping", job, target, M_UNCACHED);
	bad += run_mode("0x4c kernel writes job", job, target, M_KWRITE);
	bad += run_mode("0x4c same pages again", job, target, 0);
	if ((kr = configure(ATTR_IMPL, false)) != KERN_SUCCESS) {
		printf("FAIL: configure %#x\n", kr);
		return 1;
	}
	bad += run_mode("0x88 same pages", job, target, 0);
	bad += run_mode("0x88 new pages", job, target, M_NEWJOB | M_NEWTARGET);
	bad += run_mode("0x88 uncached mapping", job, target, M_UNCACHED);
	bad += run_mode("0x88 reset each job", job, target, M_RESET);
	(void)configure(ATTR_NC, false);

	IOConnectCallScalarMethod(conn, SEL_FREE, NULL, 0, NULL, NULL);
	IOServiceClose(conn);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad != 0;
}
