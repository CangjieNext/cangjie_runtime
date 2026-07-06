/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
package std.fs

/**
 * 文件/目录变化事件类型。
 */
public enum WatchEventKind {
    | create           // 新建
    | modify           // 内容修改
    | delete           // 删除
    | renameFrom       // 重命名/移动：源名
    | renameTo         // 重命名/移动：目的名
    | attributeChange  // 元数据（权限/时间戳）变化
    | watchDisappeared // 监听根自身被删除/移走
    | overflow         // 内核事件队列溢出，有事件丢失
}

/**
 * 一次文件变化事件。path 为相对监听根的路径。
 */
public struct WatchEvent {
    public let kind: WatchEventKind
    public let path: String
    public let isDirectory: Bool

    public init(kind: WatchEventKind, path: String, isDirectory: Bool) {
        this.kind = kind
        this.path = path
        this.isDirectory = isDirectory
    }
}
