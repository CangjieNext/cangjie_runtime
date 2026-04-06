package test_dangling

import std.fs.{File, OpenMode, FSException}
import std.runtime.*
import std.posix.getpid

main() {
    println("=== 测试 getNulFileHandle() 悬空句柄问题 ===")
    println("")
    
    let pid = getpid()
    println("当前进程 PID: ${pid}")
    println("")
    
    let handle1 = getTestNulFileHandle()
    if (handle1.isNone()) {
        println("获取 fd 失败")
        return
    }
    let fd1 = handle1.getOrThrow()
    println("获取到的 fd = ${fd1}")
    println("")
    println("请在另一个终端运行以下命令验证 fd 状态:")
    println("  ls -la /proc/${pid}/fd/${fd1}")
    println("  readlink /proc/${pid}/fd/${fd1}")
    println("")
    
    println("等待 10 秒后触发 GC...")
    sleep(Duration.second * 10)
    
    println("触发 GC...")
    for (_ in 0..20) {
        gc(heavy: true)
    }
    println("GC 完成")
    println("")
    println("请再次运行以下命令验证 fd 是否仍然有效:")
    println("  ls -la /proc/${pid}/fd/${fd1}")
    println("  readlink /proc/${pid}/fd/${fd1}")
    println("")
    
    println("等待 30 秒后退出...")
    sleep(Duration.second * 30)
}

func getTestNulFileHandle(): Option<IntNative> {
    return try {
        File("/dev/null", OpenMode.ReadWrite).fileDescriptor.fileHandle
    } catch (_: FSException) {
        None
    }
}
