package event

import (
    "errors"
    "os"
    "path"
    "sync"
    "unsafe"
)

/*
#cgo LDFLAGS: -ldl
#include <load_so.h>
#include <stdlib.h>
*/
import "C"

var (
    LibPath  string
    initOnce sync.Once
    initErr  error
)

// Init a C helper function, must be called before using ParseStack or ParseStackV2
func InitCLib() {
    initOnce.Do(func() {
        execPath, err := os.Executable()
        if err != nil {
            initErr = errors.New("failed to get executable path: " + err.Error())
            return
        }
        // 获取一次 后面用得到 免去重复获取
        execPath = path.Dir(execPath)
        LibPath = execPath + "/" + "preload_libs"

        cLibPath := C.CString(LibPath)
        defer C.free(unsafe.Pointer(cLibPath))

        ret := C.init_stack_unwinder(cLibPath)
        if ret != 0 {
            initErr = errors.New("failed to initialize stack unwinder C library")
        }

        // 可以在程序退出时注册清理函数，例如使用 atexit 或其他钩子
        // runtime.SetFinalizer or other cleanup mechanisms could call Close()
    })
}

// Close cleans up the C library resources.
func Close() {
    C.close_stack_unwinder()
}

// ParseStack correctly manages C memory.
func ParseStack(map_buffer string, opt *UnwindOption, ubuf *UnwindBuf) (string, error) {
    InitCLib()
    if initErr != nil {
        return "", initErr
    }

    cMapBuffer := C.CString(map_buffer)
    defer C.free(unsafe.Pointer(cMapBuffer))

    stack_str_ptr := C.get_stack(
        cMapBuffer,
        unsafe.Pointer(opt),
        unsafe.Pointer(&ubuf.Regs[0]),
        unsafe.Pointer(&ubuf.Data[0]),
    )

    if stack_str_ptr == nil {
        return "", errors.New("get_stack from C returned null")
    }
    defer C.free_stack_str(stack_str_ptr)

    return C.GoString(stack_str_ptr), nil
}

// ParseStackV2 correctly manages C memory.
func ParseStackV2(pid uint32, opt *UnwindOption, ubuf *UnwindBuf) (string, error) {
    InitCLib()
    if initErr != nil {
        return "", initErr
    }

    stack_str_ptr := C.get_stackv2(
        C.int(pid),
        unsafe.Pointer(opt),
        unsafe.Pointer(&ubuf.Regs[0]),
        unsafe.Pointer(&ubuf.Data[0]),
    )

    if stack_str_ptr == nil {
        return "", errors.New("get_stackv2 from C returned null")
    }
    defer C.free_stack_str(stack_str_ptr)

    return C.GoString(stack_str_ptr), nil
}
