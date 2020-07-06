#pragma once

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <printf.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>


#define EXPORT __attribute__((visibility("default")))
#define IMPORT __attribute__((visibility("default")))

#define INLINE __attribute__((always_inline))
#define WEAK   __attribute__((weak))

#ifndef PAGE_SIZE
#define PAGE_SIZE (1 << 12)
#endif

#define PAGE_ALIGNED __attribute__((aligned(PAGE_SIZE)))

/* For this family of macros, do NOT pass zero as the pow2 */
#define round_to_pow2(x, pow2)    (((unsigned long)(x)) & (~((pow2)-1)))
#define round_up_to_pow2(x, pow2) (round_to_pow2(((unsigned long)x) + (pow2)-1, (pow2)))

#define round_to_page(x)    round_to_pow2(x, PAGE_SIZE)
#define round_up_to_page(x) round_up_to_pow2(x, PAGE_SIZE)

/* Type alias's so I don't have to write uint32_t a million times */
typedef signed char   i8;
typedef unsigned char u8;
typedef int16_t       i16;
typedef uint16_t      u16;
typedef int32_t       i32;
typedef uint32_t      u32;
typedef int64_t       i64;
typedef uint64_t      u64;

/* FIXME: per-module configuration? */
#define WASM_PAGE_SIZE   (1024 * 64) /* 64KB */
#define WASM_START_PAGES (1 << 8)    /* 16MB */
#define WASM_MAX_PAGES   (1 << 15)   /* 4GB */
#define WASM_STACK_SIZE  (1 << 19)   /* 512KB */
#define SBOX_MAX_MEM     (1L << 32)  /* 4GB */

/* These are per module symbols and I'd need to dlsym for each module. instead just use global constants, see above
macros. The code generator compiles in the starting number of wasm pages, and the maximum number of pages If we try
and allocate more than max_pages, we should fault */

// TODO: Should this be deleted?
// extern u32 starting_pages;
// extern u32 max_pages;

/* The code generator also compiles in stubs that populate the linear memory and function table */
void populate_memory(void);
void populate_table(void);

/* memory also provides the table access functions */
#define INDIRECT_TABLE_SIZE (1 << 10)

struct indirect_table_entry {
	u32   type_id;
	void *func_pointer;
};

/* Cache of Frequently Accessed Members used to avoid pointer chasing */
struct sandbox_context_cache {
	struct indirect_table_entry *module_indirect_table;
	void *                       linear_memory_start;
	u32                          linear_memory_size;
};

extern __thread struct sandbox_context_cache local_sandbox_context_cache;

/* TODO: LOG_TO_FILE logic is untested */
extern i32 runtime_log_file_descriptor;

/* functions in the module to lookup and call per sandbox. */
typedef i32 (*mod_main_fn_t)(i32 a, i32 b);
typedef void (*mod_glb_fn_t)(void);
typedef void (*mod_mem_fn_t)(void);
typedef void (*mod_tbl_fn_t)(void);
typedef void (*mod_libc_fn_t)(i32, i32);

/**
 * debuglog is a macro that behaves based on the macros DEBUG and LOG_TO_FILE
 * If DEBUG is not set, debuglog does nothing
 * If DEBUG is set and LOG_TO_FILE is set, debuglog prints to the logfile defined in runtime_log_file_descriptor
 * If DEBUG is set adn LOG_TO_FILE is not set, debuglog prints to STDOUT
 */
#ifdef DEBUG
#ifdef LOG_TO_FILE
#define debuglog(fmt, ...)                                                                                  \
	dprintf(runtime_log_file_descriptor, "(%d,%lu) %s: " fmt, sched_getcpu(), pthread_self(), __func__, \
	        ##__VA_ARGS__)
#else /* !LOG_TO_FILE */
#define debuglog(fmt, ...) printf("(%d,%lu) %s: " fmt, sched_getcpu(), pthread_self(), __func__, ##__VA_ARGS__)
#endif /* LOG_TO_FILE */
#else  /* !DEBUG */
#define debuglog(fmt, ...)
#endif /* DEBUG */

#define HTTP_MAX_HEADER_COUNT            16
#define HTTP_MAX_HEADER_LENGTH           32
#define HTTP_MAX_HEADER_VALUE_LENGTH     64
#define HTTP_RESPONSE_200_OK             "HTTP/1.1 200 OK\r\n"
#define HTTP_RESPONSE_CONTENT_LENGTH     "Content-length:             \r\n\r\n" /* content body follows this */
#define HTTP_RESPONSE_CONTENT_TYPE       "Content-type:                                 \r\n"
#define HTTP_RESPONSE_CONTENT_TYPE_PLAIN "text/plain"

#define JSON_MAX_ELEMENT_COUNT 16   /* Max number of elements defined in JSON */
#define JSON_MAX_ELEMENT_SIZE  1024 /* Max size of a single module in JSON */

#define LISTENER_THREAD_CORE_ID          0 /* Dedicated Listener Core */
#define LISTENER_THREAD_MAX_EPOLL_EVENTS 1024

#define MODULE_DEFAULT_REQUEST_RESPONSE_SIZE (PAGE_SIZE)
#define MODULE_INITIALIZE_GLOBALS            "populate_globals"  /* From Silverfish */
#define MODULE_INITIALIZE_MEMORY             "populate_memory"   /* From Silverfish */
#define MODULE_INITIALIZE_TABLE              "populate_table"    /* From Silverfish */
#define MODULE_INITIALIZE_LIBC               "wasmf___init_libc" /* From Silverfish */
#define MODULE_MAIN                          "wasmf_main"        /* From Silverfish */
#define MODULE_MAX_ARGUMENT_COUNT            16                  /* Max number of arguments */
#define MODULE_MAX_ARGUMENT_SIZE             64                  /* Max size of a single argument */
#define MODULE_MAX_MODULE_COUNT              128                 /* Max number of modules */
#define MODULE_MAX_NAME_LENGTH               32                  /* Max module name length */
#define MODULE_MAX_PATH_LENGTH               256                 /* Max length of path string */
#define MODULE_MAX_PENDING_CLIENT_REQUESTS   1000

#define RUNTIME_LOG_FILE                  "awesome.log"
#define RUNTIME_READ_WRITE_VECTOR_LENGTH  16
#define RUNTIME_MAX_SANDBOX_REQUEST_COUNT (1 << 19) /* random! */

#define SANDBOX_FILE_DESCRIPTOR_PREOPEN_MAGIC (707707707) /* upside down LOLLOLLOL 🤣😂🤣*/
#define SANDBOX_MAX_IO_HANDLE_COUNT           32
#define SANDBOX_PULL_BATCH_SIZE               1 /* Max # standboxes pulled onto the local runqueue in a single batch */

#define SOFTWARE_INTERRUPT_TIME_TO_START_IN_USEC     (10 * 1000) /* start timers 10 ms from now. */
#define SOFTWARE_INTERRUPT_INTERVAL_DURATION_IN_USEC (1000 * 5)  /* and execute every 5ms */


/* If multicore, use all but the dedicated listener core
If there are fewer cores than this, main dynamically overrides this and uses all available */
#define WORKER_THREAD_CORE_COUNT (NCORES > 1 ? NCORES - 1 : NCORES)
