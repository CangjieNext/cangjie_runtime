import std.fs.*
import std.encoding.json.*
import std.time.*
import std.io.*

// 待办项结构体
struct Todo {
    var id: Int64
    var content: String       // 待办内容
    var priority: Int32       // 优先级：数字越大优先级越高 1低 2中 3高
    var deadline: String      // 截止日期，yyyy-MM-dd
    var finished: Bool        // 是否完成
}

// 全局json文件名
const TODO_FILE: String = "todo.json"

// 读取本地json，加载所有待办
func loadTodos(): Array<Todo> {
    if (!exists(TODO_FILE)) {
        return Array<Todo>()
    }
    let str = readText(TODO_FILE)
    if (str.trim() == "") {
        return Array<Todo>()
    }
    return fromJson<Array<Todo>>(str)
}

// 保存待办数组写入json文件
func saveTodos(todos: Array<Todo>): Unit {
    let jsonStr = toJson(todos)
    writeText(TODO_FILE, jsonStr)
}

// 排序规则：先按优先级降序，优先级相同则按截止日期升序
func sortTodoList(todos: Array<Todo>): Array<Todo> {
    return todos.sort({ a,b =>
        if (a.priority != b.priority) {
            b.priority.compare(a.priority)
        } else {
            a.deadline.compare(b.deadline)
        }
    })
}

// 添加待办 add 内容 优先级 截止日期
func addTodo(args: Array<String>): Unit {
    if (args.size < 4) {
        println("用法：add <内容> <优先级(1/2/3)> <截止日期yyyy-MM-dd>")
        return
    }
    let todos = loadTodos()
    let newId = if todos.isEmpty() {1} else { todos.map(it=>it.id).max() + 1 }
    let todo = Todo{
        id: newId,
        content: args[1],
        priority: args[2].parseInt32(),
        deadline: args[3],
        finished: false
    }
    todos.append(todo)
    saveTodos(todos)
    println("✅ 添加成功，ID=${newId}")
}

// 删除待办 delete id
func deleteTodo(args: Array<String>): Unit {
    if (args.size < 2) {
        println("用法：delete <id>")
        return
    }
    let targetId = args[1].parseInt64()
    let todos = loadTodos()
    let newList = todos.filter({it => it.id != targetId})
    if (newList.size == todos.size) {
        println("❌ 找不到该ID")
        return
    }
    saveTodos(newList)
    println("🗑️ 删除成功")
}

// 标记完成 done id
func doneTodo(args: Array<String>): Unit {
    if (args.size < 2) {
        println("用法：done <id>")
        return
    }
    let targetId = args[1].parseInt64()
    var todos = loadTodos()
    for (i in 0..todos.size) {
        if (todos[i].id == targetId) {
            todos[i].finished = true
            saveTodos(todos)
            println("✔️ 标记完成")
            return
        }
    }
    println("❌ 找不到该ID")
}

// 展示列表 list
func listTodo(): Unit {
    var todos = loadTodos()
    todos = sortTodoList(todos)
    println("=====================================")
    println("ID\t状态\t优先级\t截止日期\t内容")
    println("=====================================")
    for (item in todos) {
        let status = if(item.finished){"✅已完成"}else{"⏳待办"}
        let priText = match(item.priority) {
            1 => "低"
            2 => "中"
            3 => "高"
            _ => "未知"
        }
        println("${item.id}\t${status}\t${priText}\t${item.deadline}\t${item.content}")
    }
}

// 主入口，解析命令行参数
main(args: Array<String>): Int64 {
    if (args.size < 1) {
        println("支持命令：add / delete / done / list")
        return 0
    }
    let cmd = args[0]
    match(cmd) {
        "add" => addTodo(args)
        "delete" => deleteTodo(args)
        "done" => doneTodo(args)
        "list" => listTodo()
        _ => println("未知命令！可用：add delete done list")
    }
    return 0
}
