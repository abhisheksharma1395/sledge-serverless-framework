#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef LOG_TO_FILE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#endif

#include "debuglog.h"
#include "module.h"
#include "panic.h"
#include "runtime.h"
#include "sandbox.h"
#include "software_interrupt.h"
#include "worker_thread.h"

/* Conditionally used by debuglog when NDEBUG is not set */
int32_t debuglog_file_descriptor = -1;

uint32_t       runtime_processor_speed_MHz      = 0;
uint64_t       runtime_relative_deadline_us_max = 0; /* a value higher than this will cause overflow on a uint64_t */
uint32_t       runtime_total_online_processors  = 0;
uint32_t       runtime_worker_threads_count     = 0;
const uint32_t runtime_first_worker_processor   = 1;
/* TODO: the worker never actually records state here */
int runtime_worker_threads_argument[WORKER_THREAD_CORE_COUNT] = { 0 }; /* The worker sets its argument to -1 on error */
pthread_t runtime_worker_threads[WORKER_THREAD_CORE_COUNT];


FILE *runtime_sandbox_perf_log = NULL;

enum RUNTIME_SCHEDULER runtime_scheduler = RUNTIME_SCHEDULER_FIFO;
int                    runtime_worker_core_count;

/**
 * Returns instructions on use of CLI if used incorrectly
 * @param cmd - The command the user entered
 */
static void
runtime_usage(char *cmd)
{
	printf("%s <modules_file>\n", cmd);
}

/**
 * Sets the process data segment (RLIMIT_DATA) and # file descriptors
 * (RLIMIT_NOFILE) soft limit to its hard limit (see man getrlimit)
 */
void
runtime_set_resource_limits_to_max()
{
	struct rlimit resource_limit;
	if (getrlimit(RLIMIT_DATA, &resource_limit) < 0) {
		perror("getrlimit RLIMIT_DATA");
		exit(-1);
	}
	resource_limit.rlim_cur = resource_limit.rlim_max;
	if (setrlimit(RLIMIT_DATA, &resource_limit) < 0) {
		perror("setrlimit RLIMIT_DATA");
		exit(-1);
	}
	if (getrlimit(RLIMIT_NOFILE, &resource_limit) < 0) {
		perror("getrlimit RLIMIT_NOFILE");
		exit(-1);
	}
	resource_limit.rlim_cur = resource_limit.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &resource_limit) < 0) {
		perror("setrlimit RLIMIT_NOFILE");
		exit(-1);
	}
}

/**
 * Check the number of cores and the compiler flags and allocate available cores
 */
void
runtime_allocate_available_cores()
{
	/* Find the number of processors currently online */
	runtime_total_online_processors = sysconf(_SC_NPROCESSORS_ONLN);
	printf("\tCore Count: %u\n", runtime_total_online_processors);
	uint32_t max_possible_workers = runtime_total_online_processors - 1;

	if (runtime_total_online_processors < 2) panic("Runtime requires at least two cores!");

	/* Number of Workers */
	char *worker_count_raw = getenv("SLEDGE_NWORKERS");
	if (worker_count_raw != NULL) {
		int worker_count = atoi(worker_count_raw);
		if (worker_count <= 0 || worker_count > max_possible_workers) {
			panic("Invalid Worker Count. Was %d. Must be {1..%d}\n", worker_count, max_possible_workers);
		}
		runtime_worker_threads_count = worker_count;
	} else {
		runtime_worker_threads_count = max_possible_workers;
	}

	printf("\tListener core ID: %u\n", LISTENER_THREAD_CORE_ID);
	printf("\tFirst Worker core ID: %u\n", runtime_first_worker_processor);
	printf("\tWorker core count: %u\n", runtime_worker_threads_count);
}

/**
 * Returns the cpu MHz entry for CPU0 in /proc/cpuinfo, rounded to the nearest MHz
 * We are assuming all cores are the same clock speed, which is not true of many systems
 * We are also assuming this value is static
 * @return proceccor speed in MHz
 */
static inline uint32_t
runtime_get_processor_speed_MHz(void)
{
	uint32_t return_value;

	FILE *cmd = popen("grep '^cpu MHz' /proc/cpuinfo | head -n 1 | awk '{print $4}'", "r");
	if (unlikely(cmd == NULL)) goto err;

	char   buff[16];
	size_t n = fread(buff, 1, sizeof(buff) - 1, cmd);
	if (unlikely(n <= 0)) goto err;
	buff[n] = '\0';

	float processor_speed_MHz;
	n = sscanf(buff, "%f", &processor_speed_MHz);
	if (unlikely(n != 1)) goto err;
	if (unlikely(processor_speed_MHz < 0)) goto err;

	return_value = (uint32_t)nearbyintf(processor_speed_MHz);

done:
	pclose(cmd);
	return return_value;
err:
	return_value = 0;
	goto done;
}

/**
 * Controls the behavior of the debuglog macro defined in types.h
 * If LOG_TO_FILE is defined, close stdin, stdout, stderr, and debuglog writes to a logfile named awesome.log.
 * Otherwise, it writes to STDOUT
 */
void
runtime_process_debug_log_behavior()
{
#ifdef LOG_TO_FILE
	debuglog_file_descriptor = open(RUNTIME_LOG_FILE, O_CREAT | O_TRUNC | O_WRONLY, S_IRWXU | S_IRWXG);
	if (debuglog_file_descriptor < 0) {
		perror("Error opening logfile\n");
		exit(-1);
	}
	dup2(debuglog_file_descriptor, STDOUT_FILENO);
	dup2(debuglog_file_descriptor, STDERR_FILENO);
#else
	debuglog_file_descriptor = STDOUT_FILENO;
#endif /* LOG_TO_FILE */
}

/**
 * Starts all worker threads and sleeps forever on pthread_join, which should never return
 */
void
runtime_start_runtime_worker_threads()
{
	printf("Starting %d worker thread(s)\n", runtime_worker_threads_count);
	for (int i = 0; i < runtime_worker_threads_count; i++) {
		int ret = pthread_create(&runtime_worker_threads[i], NULL, worker_thread_main,
		                         (void *)&runtime_worker_threads_argument[i]);
		if (ret) {
			errno = ret;
			perror("pthread_create");
			exit(-1);
		}

		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(runtime_first_worker_processor + i, &cs);
		ret = pthread_setaffinity_np(runtime_worker_threads[i], sizeof(cs), &cs);
		assert(ret == 0);
	}
	debuglog("Sandboxing environment ready!\n");
}

void
runtime_cleanup()
{
	if (runtime_sandbox_perf_log != NULL) fflush(runtime_sandbox_perf_log);

	exit(EXIT_SUCCESS);
}

void
runtime_configure()
{
	signal(SIGTERM, runtime_cleanup);
	/* Scheduler Policy */
	char *scheduler_policy = getenv("SLEDGE_SCHEDULER");
	if (scheduler_policy == NULL) scheduler_policy = "FIFO";

	if (strcmp(scheduler_policy, "EDF") == 0) {
		runtime_scheduler = RUNTIME_SCHEDULER_EDF;
	} else if (strcmp(scheduler_policy, "FIFO") == 0) {
		runtime_scheduler = RUNTIME_SCHEDULER_FIFO;
	} else {
		panic("Invalid scheduler policy: %s. Must be {EDF|FIFO}\n", scheduler_policy);
	}
	printf("\tScheduler Policy: %s\n", print_runtime_scheduler(runtime_scheduler));

	/* Runtime Perf Log */
	char *runtime_sandbox_perf_log_path = getenv("SLEDGE_SANDBOX_PERF_LOG");
	if (runtime_sandbox_perf_log_path != NULL) {
		printf("\tSandbox Performance Log: %s\n", runtime_sandbox_perf_log_path);
		runtime_sandbox_perf_log = fopen(runtime_sandbox_perf_log_path, "w");
		if (runtime_sandbox_perf_log == NULL) { perror("sandbox perf log"); }
		fprintf(runtime_sandbox_perf_log, "id,function,state,deadline,actual,queued,initializing,runnable,"
		                                  "running,blocked,returned,memory\n");
	} else {
		printf("\tSandbox Performance Log: Disabled\n");
	}
}

void
log_compiletime_config()
{
	printf("Static Compiler Flags:\n");
#ifdef ADMISSIONS_CONTROL
	printf("\tAdmissions Control: Enabled\n");
#else
	printf("\tAdmissions Control: Disabled\n");
#endif

#ifdef NDEBUG
	printf("\tAssertions and Debug Logs: Disabled\n");
#else
	printf("\tAssertions and Debug Logs: Enabled\n");
#endif

#ifdef LOG_TO_FILE
	printf("\tLogging to: %s\n", RUNTIME_LOG_FILE);
#else
	printf("\tLogging to: STDOUT and STDERR\n");
#endif

#if defined(aarch64)
	printf("\tArchitecture: %s\n", "aarch64");
#elif defined(x86_64)
	printf("\tArchitecture: %s\n", "x86_64");
#endif

	// TODO: Static assertion of define
	int ncores = NCORES;
	printf("\tTotal Cores: %d\n", ncores);
	int page_size = PAGE_SIZE;
	printf("\tPage Size: %d\n", page_size);

#ifdef LOG_HTTP_PARSER
	printf("\tLog HTTP Parser: Enabled\n");
#else
	printf("\tLog HTTP Parser: Disabled\n");
#endif

#ifdef LOG_STATE_CHANGES
	printf("\tLog State Changes: Enabled\n");
#else
	printf("\tLog State Changes: Disabled\n");
#endif

#ifdef LOG_LOCK_OVERHEAD
	printf("\tLog Lock Overhead: Enabled\n");
#else
	printf("\tLog Lock Overhead: Disabled\n");
#endif

#ifdef LOG_LISTENER_LOCK_OVERHEAD
	printf("\tLog Listener Lock Overhead: Enabled\n");
#else
	printf("\tLog Listener Lock Overhead: Disabled\n");
#endif

#ifdef LOG_CONTEXT_SWITCHES
	printf("\tLog Context Switches: Enabled\n");
#else
	printf("\tLog Context Switches: Disabled\n");
#endif

#ifdef LOG_ADMISSIONS_CONTROL
	printf("\tLog Admissions Control: Enabled\n");
#else
	printf("\tLog Admissions Control: Disabled\n");
#endif

#ifdef LOG_REQUEST_ALLOCATION
	printf("\tLog Request Allocation: Enabled\n");
#else
	printf("\tLog Request Allocation: Disabled\n");
#endif

#ifdef LOG_PREEMPTION
	printf("\tLog Preemption: Enabled\n");
#else
	printf("\tLog Preemption: Disabled\n");
#endif

#ifdef LOG_MODULE_LOADING
	printf("\tLog Module Loading: Enabled\n");
#else
	printf("\tLog Module Loading: Disabled\n");
#endif

#ifdef LOG_TOTAL_REQS_RESPS
	printf("\tLog Total Reqs/Resps: Enabled\n");
#else
	printf("\tLog Total Reqs/Resps: Disabled\n");
#endif

#ifdef LOG_SANDBOX_COUNT
	printf("\tLog Sandbox Count: Enabled\n");
#else
	printf("\tLog Sandbox Count: Disabled\n");
#endif

#ifdef LOG_LOCAL_RUNQUEUE
	printf("\tLog Local Runqueue: Enabled\n");
#else
	printf("\tLog Local Runqueue: Disabled\n");
#endif
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		runtime_usage(argv[0]);
		exit(-1);
	}

	printf("Starting the Sledge runtime\n");

	log_compiletime_config();
	runtime_process_debug_log_behavior();

	printf("Runtime Environment:\n");

	memset(runtime_worker_threads, 0, sizeof(pthread_t) * WORKER_THREAD_CORE_COUNT);

	runtime_processor_speed_MHz = runtime_get_processor_speed_MHz();
	if (unlikely(runtime_processor_speed_MHz == 0)) panic("Failed to detect processor speed\n");

	runtime_relative_deadline_us_max = UINT64_MAX / runtime_processor_speed_MHz;
	// TODO: Just here for troubleshooting. Delete me later
	printf("Runtime Relative Deadline Max: %lu\n", runtime_relative_deadline_us_max);
	printf("UINT32_MAX: %u\n", UINT32_MAX);
	software_interrupt_interval_duration_in_cycles = (uint64_t)SOFTWARE_INTERRUPT_INTERVAL_DURATION_IN_USEC
	                                                 * runtime_processor_speed_MHz;
	printf("\tProcessor Speed: %u MHz\n", runtime_processor_speed_MHz);

	runtime_set_resource_limits_to_max();
	runtime_allocate_available_cores();
	runtime_configure();
	runtime_initialize();
#ifdef LOG_MODULE_LOADING
	debuglog("Parsing modules file [%s]\n", argv[1]);
#endif
	if (module_new_from_json(argv[1])) panic("failed to parse modules file[%s]\n", argv[1]);

	runtime_start_runtime_worker_threads();
	listener_thread_initialize();

	for (int i = 0; i < runtime_worker_threads_count; i++) {
		int ret = pthread_join(runtime_worker_threads[i], NULL);
		if (ret) {
			errno = ret;
			perror("pthread_join");
			exit(-1);
		}
	}

	exit(-1);
}
