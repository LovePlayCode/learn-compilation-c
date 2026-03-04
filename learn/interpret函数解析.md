# `interpret()` 函数深度解析

## 源码

```c
InterpretResult interpret(const char *source)
{
    ObjFunction *function = compile(source);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    CallFrame *frame = &vm.frames[vm.frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;
    frame->slots = vm.stack;
    return run();
}
```

## 函数定位

`interpret()` 是整个 clox 虚拟机的**总入口函数**，它是**编译阶段**和**执行阶段**之间的桥梁。用户传入一段源代码字符串，该函数负责将其编译为字节码，再交给 VM 执行。

## 整体执行流程

```mermaid
flowchart TD
    A["interpret(source)"] --> B["compile(source)"]
    B --> C{编译是否成功?}
    C -->|"function == NULL"| D["返回 INTERPRET_COMPILE_ERROR"]
    C -->|编译成功| E["push(OBJ_VAL(function))"]
    E --> F["创建 CallFrame"]
    F --> G["设置 frame->function"]
    G --> H["设置 frame->ip 指向字节码起始"]
    H --> I["设置 frame->slots 指向栈底"]
    I --> J["return run()"]
    J --> K{运行结果}
    K -->|成功| L["INTERPRET_OK"]
    K -->|运行时错误| M["INTERPRET_RUNTIME_ERROR"]

    style A fill:#4a9eff,color:#fff
    style D fill:#ff6b6b,color:#fff
    style L fill:#51cf66,color:#fff
    style M fill:#ff6b6b,color:#fff
```

## 逐行详解

### 第 1 步：编译源码

```c
ObjFunction *function = compile(source);
```

调用编译器，将源代码字符串编译为一个 `ObjFunction` 对象。这个对象包含：

| 字段 | 类型 | 含义 |
|------|------|------|
| `obj` | `Obj` | 对象头，包含类型标识和 GC 链表指针 |
| `arity` | `int` | 函数参数个数（顶层脚本为 0） |
| `chunk` | `Chunk` | 编译产生的字节码块（含指令数组、常量池、行号信息） |
| `name` | `ObjString*` | 函数名（顶层脚本为 NULL） |

`compile()` 内部的流程：

```mermaid
flowchart LR
    A["源代码字符串"] -->|initScanner| B["Scanner\n词法分析器"]
    B -->|"逐个产出 Token"| C["Parser\nPratt 解析器"]
    C -->|"生成字节码"| D["ObjFunction\n(含 Chunk)"]

    style A fill:#ffd43b,color:#000
    style D fill:#4a9eff,color:#fff
```

### 第 2 步：编译错误检查

```c
if (function == NULL)
    return INTERPRET_COMPILE_ERROR;
```

如果编译过程中发生语法错误（`parser.hadError == true`），`compile()` 返回 NULL，此时直接返回编译错误，不进入执行阶段。

### 第 3 步：将函数对象压入栈

```c
push(OBJ_VAL(function));
```

这一行做了两件事：

1. **`OBJ_VAL(function)`** — 将 C 指针包装成 `Value`：
   ```c
   // 展开后等价于：
   (Value){VAL_OBJ, {.obj = (Obj *)function}}
   ```

2. **`push(...)`** — 将这个 Value 压入 VM 的值栈：
   ```c
   void push(Value value) {
       *vm.stackTop = value;  // 写入栈顶位置
       vm.stackTop++;         // 栈顶指针上移
   }
   ```

**为什么要压栈？** 这是为了让 GC（垃圾回收器）能找到这个函数对象。如果函数对象只存在于 C 局部变量中，GC 在回收时无法追踪到它，可能会误回收。压入栈后，GC 扫描栈就能发现它。

### 第 4 步：创建调用帧 (CallFrame)

```c
CallFrame *frame = &vm.frames[vm.frameCount++];
frame->function = function;
frame->ip = function->chunk.code;
frame->slots = vm.stack;
```

这是函数调用机制的核心。逐字段解释：

| 赋值 | 含义 |
|------|------|
| `vm.frames[vm.frameCount++]` | 从调用帧数组中取下一个空闲帧，并递增帧计数 |
| `frame->function = function` | 记录当前帧执行的是哪个函数（用于访问其字节码和常量池） |
| `frame->ip = function->chunk.code` | 指令指针指向字节码数组的起始位置（即第一条指令） |
| `frame->slots = vm.stack` | 局部变量槽指向值栈的栈底（顶层脚本占据整个栈） |

### 第 5 步：执行字节码

```c
return run();
```

进入 VM 的取指-解码-执行（fetch-decode-execute）主循环，逐条执行字节码指令直到遇到 `OP_RETURN`。

## 关键数据结构的内存布局

```mermaid
graph TB
    subgraph "VM 全局状态"
        direction TB
        VM["vm (全局变量)"]
    end

    subgraph "调用帧数组 vm.frames[]"
        direction LR
        F0["frames[0]\n─────────\nfunction: →\nip: →\nslots: →"]
        F1["frames[1]\n(空闲)"]
        F2["...\nframes[63]"]
    end

    subgraph "ObjFunction"
        direction TB
        FN["function\n─────────\nobj.type: OBJ_FUNCTION\narity: 0\nname: NULL\nchunk: →"]
    end

    subgraph "Chunk (字节码块)"
        direction TB
        CH["chunk\n─────────\ncode: [OP_CONSTANT, 0, OP_PRINT, OP_RETURN, ...]\nconstants: ValueArray\nlines: [1, 1, 1, 1, ...]"]
    end

    subgraph "值栈 vm.stack[]"
        direction LR
        S0["stack[0]\nOBJ_VAL(function)"]
        S1["stack[1]\n(空闲)"]
        S2["...\nstack[16383]"]
    end

    VM --> F0
    VM --> S0
    F0 -->|"function"| FN
    F0 -->|"ip"| CH
    F0 -->|"slots"| S0
    FN --> CH

    style VM fill:#4a9eff,color:#fff
    style F0 fill:#ffd43b,color:#000
    style FN fill:#51cf66,color:#000
    style CH fill:#ff922b,color:#fff
```

## 完整生命周期时序图

```mermaid
sequenceDiagram
    participant User as 用户代码
    participant I as interpret()
    participant C as compile()
    participant Scanner as Scanner
    participant Parser as Parser
    participant VM_Stack as VM 值栈
    participant Frame as CallFrame
    participant R as run()

    User->>I: interpret("print 1+2;")
    I->>C: compile(source)
    C->>Scanner: initScanner(source)
    Scanner-->>C: 就绪

    loop 解析每条声明
        C->>Parser: declaration()
        Parser->>Scanner: advance() 获取 Token
        Scanner-->>Parser: Token
        Parser-->>C: 生成字节码写入 Chunk
    end

    C-->>I: 返回 ObjFunction*

    Note over I: 编译成功，function != NULL

    I->>VM_Stack: push(OBJ_VAL(function))
    Note over VM_Stack: stack[0] = function

    I->>Frame: 创建 CallFrame
    Note over Frame: ip → chunk.code[0]<br/>slots → stack[0]

    I->>R: run()

    loop 取指-解码-执行循环
        R->>R: instruction = READ_BYTE()
        R->>R: switch(instruction) 分派执行
        R->>VM_Stack: push / pop 操作
    end

    R-->>I: INTERPRET_OK
    I-->>User: 返回结果
```

## 具体示例：`print 1 + 2;`

假设输入源代码为 `print 1 + 2;`，以下是 `interpret()` 启动后的完整执行过程：

### 编译阶段产出

```
常量池: [0: 1.0,  1: 2.0]
字节码: OP_CONSTANT 0 | OP_CONSTANT 1 | OP_ADD | OP_PRINT | OP_RETURN
```

### 执行阶段栈变化

```mermaid
flowchart TD
    subgraph Step1["1. OP_CONSTANT 0"]
        S1["栈: [ 1.0 ]"]
    end
    subgraph Step2["2. OP_CONSTANT 1"]
        S2["栈: [ 1.0 ][ 2.0 ]"]
    end
    subgraph Step3["3. OP_ADD"]
        S3["pop 2.0, pop 1.0\npush 3.0\n栈: [ 3.0 ]"]
    end
    subgraph Step4["4. OP_PRINT"]
        S4["pop 3.0 → 输出 '3'\n栈: (空)"]
    end
    subgraph Step5["5. OP_RETURN"]
        S5["返回 INTERPRET_OK"]
    end

    Step1 --> Step2 --> Step3 --> Step4 --> Step5

    style Step1 fill:#e8f4fd,color:#000
    style Step2 fill:#e8f4fd,color:#000
    style Step3 fill:#fff3e0,color:#000
    style Step4 fill:#e8f5e9,color:#000
    style Step5 fill:#fce4ec,color:#000
```

## 设计要点总结

| 设计决策 | 原因 |
|----------|------|
| 顶层代码包装为 `ObjFunction` | 统一处理：顶层脚本和函数调用使用相同的 CallFrame 机制 |
| 函数对象压栈 | GC 安全：确保编译产物不会被垃圾回收器误回收 |
| `frame->slots = vm.stack` | 顶层脚本的局部变量从栈底开始，嵌套函数调用时 slots 指向各自的栈窗口 |
| `frameCount++` 后置递增 | 先取当前空闲帧的索引，再递增计数，一步完成分配 |
| 返回 `run()` 的结果 | 将执行结果（OK / RUNTIME_ERROR）直接传递给调用者 |
