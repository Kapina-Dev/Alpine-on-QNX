#ifndef LINUXEMU_H
#define LINUXEMU_H

#define _QNX_SOURCE 1
#include <sys/elf.h>
#include <signal.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <ucontext.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_LOAD_SEGMENTS 32
#define MAX_PATCHES 4096
#define GUEST_MIN_ADDRESS 0x00010000u
#define GUEST_MAX_ADDRESS 0x70000000u
#define GUEST_STACK_SIZE (1024u * 1024u)
#define MAIN_ET_DYN_BIAS 0x20000000u
#define INTERPRETER_BIAS 0x60000000u

struct guest_image {
    uintptr_t entry;
    uintptr_t start_entry;
    uintptr_t interpreter_base;
    uintptr_t program_headers;
    uint32_t program_header_size;
    uint32_t program_header_count;
    uintptr_t initial_brk;
};

struct elf_object {
    uintptr_t entry;
    uintptr_t load_bias;
    uintptr_t program_headers;
    uint32_t program_header_size;
    uint32_t program_header_count;
    uintptr_t data_end;
};

uintptr_t align_down(uintptr_t value, uintptr_t alignment);
uintptr_t align_up(uintptr_t value, uintptr_t alignment);
int read_exact_at(int fd, void *buffer, size_t length, off_t offset);

int guest_memory_initialize(long page_size);
int guest_memory_map_load_segment(int fd, const Elf32_Phdr *header,
    uintptr_t load_bias, off_t file_size);
int guest_memory_finalize(void);
int guest_memory_is_executable(uintptr_t address, size_t length);
size_t guest_memory_segment_count(void);
void guest_memory_rollback(size_t segment_count);
int guest_memory_range_owned(uintptr_t address, size_t length);
int guest_memory_runtime_owned(uintptr_t address, size_t length);
int guest_memory_runtime_map(uintptr_t address, size_t length, int executable);
int guest_memory_runtime_protect(uintptr_t address, size_t length,
    int executable);
void guest_memory_runtime_unmap(uintptr_t address, size_t length);
void guest_memory_after_fork(void);
int guest_brk_initialize(uintptr_t initial_break);
uintptr_t guest_brk_set(uintptr_t requested);

int arm_patch_range(uintptr_t start, size_t length);
int arm_patch_elf_mapping(int fd, off_t offset, uintptr_t address,
    size_t length);
void arm_patch_forget_range(uintptr_t address, size_t length);
const uint32_t *arm_patch_original(uintptr_t address);
size_t arm_patch_count(void);
void arm_patch_rollback(size_t patch_count);

int load_guest_image(const char *path, struct guest_image *image);

int guest_path_initialize(const char *root);
int guest_path_resolve(const char *path, int allow_missing_leaf,
    char *host_path, size_t host_path_size);
int guest_path_resolve_nofollow(const char *path, char *host_path,
    size_t host_path_size);
int guest_path_resolve_at(const char *host_directory, const char *path,
    int allow_missing_leaf, int nofollow, char *host_path,
    size_t host_path_size);
int guest_path_getcwd(char *buffer, size_t size);
int guest_path_readlink(const char *path, char *buffer, size_t size);
int guest_path_chdir(const char *path);
int guest_path_fchdir(const char *host_path);
const char *guest_path_root(void);
const char *guest_path_cwd(void);

int guest_process_initialize(const char *emulator);
int guest_process_retry_load(char *const arguments[]);
int guest_process_exec(const char *host_executable, const char *guest_executable,
    char *const guest_argv[], char *const guest_envp[]);
int32_t guest_signal_action(int linux_signal, const void *guest_action,
    void *guest_old_action, size_t signal_set_size);
int32_t guest_signal_mask(int how, const void *guest_set, void *guest_old_set,
    size_t signal_set_size);
int32_t guest_signal_suspend(const void *guest_set, size_t signal_set_size);
int32_t guest_signal_send(pid_t process, int linux_signal);
int32_t guest_signal_send_thread(int32_t process, int32_t thread,
    int linux_signal);
int guest_signal_host_mask(const void *guest_set, size_t signal_set_size,
    sigset_t *host_set);
int guest_signal_deliver(ucontext_t *context, int linux_signal,
    const siginfo_t *host_info);
int guest_signal_deliver_pending(ucontext_t *context, uint32_t syscall_number,
    uint32_t original_r0);
int guest_signal_return(ucontext_t *context, int realtime);
int32_t guest_signal_altstack(ucontext_t *context, const void *guest_stack,
    void *guest_old_stack);
int guest_signal_from_host(int host_signal);
extern void linuxemu_signal_return_trampoline(void);
extern void linuxemu_rt_signal_return_trampoline(void);

int32_t linux_time_syscall(uint32_t number, uint32_t arguments[6]);
int32_t linux_poll_syscall(uint32_t number, uint32_t arguments[6]);
int32_t linux_ioctl(int fd, uint32_t request, void *guest_argument);
int32_t linux_socket_syscall(uint32_t number, uint32_t arguments[6]);
int32_t linux_futex(uint32_t *address, uint32_t operation, uint32_t value,
    const void *guest_timeout, uint32_t *second_address, uint32_t bitset);
int32_t linux_futex_wake(uint32_t *address, int count, uint32_t bitset);
void linux_futex_after_fork(void);

int guest_thread_initialize(void);
int32_t guest_thread_clone(ucontext_t *context);
int32_t guest_thread_tid(void);
int32_t guest_thread_set_tid_address(uint32_t *address);
void guest_thread_after_fork(void);
void guest_thread_exit(int status) __attribute__((noreturn));
void guest_thread_signal_pending(int linux_signal, const siginfo_t *information);
int guest_thread_take_pending(siginfo_t *information, int *has_information);
int32_t guest_thread_kill(int32_t tid, int host_signal);
void guest_thread_signal_mask_get(uint32_t words[2]);
void guest_thread_signal_mask_set(const uint32_t words[2]);
void guest_thread_signal_suspend_restore(const uint32_t words[2]);
void guest_thread_signal_delivery_mask(uint32_t words[2]);
int guest_thread_signal_wait_begin(void);
void guest_thread_signal_wait_arm(void);
void guest_thread_signal_wait_interrupted(const uint32_t old_words[2]);
void guest_thread_signal_wait_complete(const uint32_t old_words[2]);
void guest_thread_signal_wait_end(void);
void guest_thread_syscall_enter(void);
void guest_thread_syscall_leave(void);
int guest_thread_syscall_active(void);
void guest_thread_futex_wait_begin(sem_t *semaphore);
int guest_thread_futex_wait_end(void);
void guest_thread_altstack_get(uint32_t *pointer, uint32_t *size);
void guest_thread_altstack_set(uint32_t pointer, uint32_t size);

int linux_errno_number(int host_errno);
int linux_open_flags(uint32_t linux_flags, int *host_flags);
uint32_t linux_status_flags(int host_flags);
void linux_stat64_store(void *guest_buffer, const struct stat *host_status);
void linux_rusage_store(void *guest_buffer, const struct rusage *host_usage);

void runtime_initialize(long page_size, int trace_enabled);
long runtime_page_size(void);
int runtime_trace_enabled(void);
uint32_t runtime_guest_tls(void);
void runtime_set_guest_tls(uint32_t value);
uintptr_t create_guest_stack(int guest_argc, char **guest_argv,
    const struct guest_image *image);
void linuxemu_enter_guest(uintptr_t entry, uintptr_t stack_pointer);

void linux_syscall_dispatch(ucontext_t *context);
int install_guest_traps(void);

#endif
