#ifndef LOAD_SO_H
#define LOAD_SO_H

#include <stdlib.h>

// 初始化函数，返回0表示成功，-1表示失败
int init_stack_unwinder(const char* dl_path);

// 关闭和清理资源
void close_stack_unwinder();

// 业务函数，不再需要 dl_path 参数
const char* get_stack(char* map_buffer, void* opt, void* regs_buf, void* stack_buf);
const char* get_stackv2(int pid, void* opt, void* regs_buf, void* stack_buf);

// 内存释放函数
void free_stack_str(char* str);

#endif // LOAD_SO_H