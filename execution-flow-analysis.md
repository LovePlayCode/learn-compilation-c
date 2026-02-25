# 项目完整运行流程分析

## 示例程序

```lox
var a = 1;
{
    var a = 2;
    var b = 3;
    print a;
    print b;
}
```

预期输出：

```
2
3
```

---

## 整体架构

```mermaid
graph TB
    Source["源代码 (test.txt)"] --> Scanner["Scanner 词法分析"]
    Scanner --> |Token 流| Compiler["Compiler 编译器"]
    Compiler --> |字节码| Chunk["Chunk 字节码块"]
    Chunk --> VM["VM 虚拟机"]
    VM --> Output["输出结果"]

    subgraph "编译期"
        Scanner
        Compiler
    end

    subgraph "运行时"
        VM
    end

    subgraph "数据结构"
        Chunk
        ConstPool["常量池 ValueArray"]
        Stack["值栈 Value[]"]
        Globals["全局变量 Table"]
        Strings["字符串驻留 Table"]
    end

    Chunk --> ConstPool
    VM --> Stack
    VM --> Globals
    VM --> Strings
```

---

## 阶段一：入口与初始化（main.c）

```mermaid
graph TD
    main["main(argc=2, argv='test.txt')"]
    main --> initVM["initVM()"]
    initVM --> resetStack["resetStack(): stackTop = stack"]
    initVM --> initGlobals["initTable(&vm.globals)"]
    initVM --> initStrings["initTable(&vm.strings)"]
    main --> runFile["runFile('test.txt')"]
    runFile --> readFile["readFile(): 读取整个文件到内存"]
    readFile --> interpret["interpret(source)"]
    interpret --> compile["compile(): 编译阶段"]
    interpret --> run["run(): 执行阶段"]
    runFile --> free["free(source)"]
    main --> freeVM["freeVM(): 释放所有资源"]
```

`main.c` 根据命令行参数决定执行模式：
- `argc == 1`：REPL 逐行模式
- `argc == 2`：脚本模式，`readFile()` 一次性读入整个文件，交给 `interpret()` 编译执行

---

## 阶段二：词法分析（scanner.c）

Scanner 将源代码拆分为 Token 流：

```
源代码: var a = 1; { var a = 2; var b = 3; print a; print b; }

Token 流:
 ┌──────────────────┬──────────────────┬──────────────┐
 │ Token 类型        │ 词素 (lexeme)    │ 行号          │
 ├──────────────────┼──────────────────┼──────────────┤
 │ TOKEN_VAR        │ "var"            │ 1            │
 │ TOKEN_IDENTIFIER │ "a"              │ 1            │
 │ TOKEN_EQUAL      │ "="              │ 1            │
 │ TOKEN_NUMBER     │ "1"              │ 1            │
 │ TOKEN_SEMICOLON  │ ";"              │ 1            │
 │ TOKEN_LEFT_BRACE │ "{"              │ 2            │
 │ TOKEN_VAR        │ "var"            │ 3            │
 │ TOKEN_IDENTIFIER │ "a"              │ 3            │
 │ TOKEN_EQUAL      │ "="              │ 3            │
 │ TOKEN_NUMBER     │ "2"              │ 3            │
 │ TOKEN_SEMICOLON  │ ";"              │ 3            │
 │ TOKEN_VAR        │ "var"            │ 4            │
 │ TOKEN_IDENTIFIER │ "b"              │ 4            │
 │ TOKEN_EQUAL      │ "="              │ 4            │
 │ TOKEN_NUMBER     │ "3"              │ 4            │
 │ TOKEN_SEMICOLON  │ ";"              │ 4            │
 │ TOKEN_PRINT      │ "print"          │ 5            │
 │ TOKEN_IDENTIFIER │ "a"              │ 5            │
 │ TOKEN_SEMICOLON  │ ";"              │ 5            │
 │ TOKEN_PRINT      │ "print"          │ 6            │
 │ TOKEN_IDENTIFIER │ "b"              │ 6            │
 │ TOKEN_SEMICOLON  │ ";"              │ 6            │
 │ TOKEN_RIGHT_BRACE│ "}"              │ 7            │
 │ TOKEN_EOF        │                  │ 7            │
 └──────────────────┴──────────────────┴──────────────┘
```

`scanToken()` 通过首字符判断 Token 类型：
- `isAlpha(c)` → `identifier()` → 再通过 `identifierType()` 区分关键字（`var`/`print`）和标识符（`a`/`b`）
- `isDigit(c)` → `number()`
- `=`, `;`, `{`, `}` 等 → 直接返回对应 Token

---

## 阶段三：编译（compiler.c）

### 编译器状态

```
Compiler:
  locals[256]:  局部变量数组
  localCount:   当前局部变量数量
  scopeDepth:   当前作用域深度（0 = 全局）
```

### 编译流程总览

```mermaid
graph TD
    compile["compile(source, chunk)"]
    compile --> initScanner["initScanner(source)"]
    compile --> initCompiler["initCompiler(): localCount=0, scopeDepth=0"]
    compile --> advance["advance(): 加载第一个 Token"]
    compile --> loop{"match(TOKEN_EOF)?"}
    loop -->|否| declaration["declaration()"]
    declaration --> loop
    loop -->|是| endCompiler["endCompiler(): 发射 OP_RETURN"]
```

### 逐语句编译

#### 语句 1：`var a = 1;`（全局作用域，scopeDepth=0）

```mermaid
graph TD
    D1["declaration()"] --> matchVar{"match(TOKEN_VAR)?"}
    matchVar -->|是| varDecl["varDeclaration()"]
    varDecl --> parseVar["parseVariable()"]
    parseVar --> consume["consume(TOKEN_IDENTIFIER): 消费 'a'"]
    consume --> declareVar["declareVariable()"]
    declareVar --> checkDepth{"scopeDepth == 0?"}
    checkDepth -->|是 全局| skipLocal["跳过，不添加到 locals"]
    skipLocal --> idConst["identifierConstant(): 'a' → 常量池[0]"]
    idConst --> returnIdx["返回 global = 0"]

    varDecl --> matchEq{"match(TOKEN_EQUAL)?"}
    matchEq -->|是| expr["expression(): 编译 '1'"]
    expr --> emitConst["emitConstant(1.0): 1.0 → 常量池[1]"]
    emitConst --> emitOp1["发射 OP_CONSTANT 1"]

    varDecl --> consumeSemi["consume(TOKEN_SEMICOLON)"]
    consumeSemi --> defVar["defineVariable(0)"]
    defVar --> checkDepth2{"scopeDepth > 0?"}
    checkDepth2 -->|否 全局| emitDef["发射 OP_DEFINE_GLOBAL 0"]
```

生成字节码：`OP_CONSTANT 1`, `OP_DEFINE_GLOBAL 0`

#### 语句 2：`{` 进入块作用域

```mermaid
graph TD
    D2["declaration() → statement()"]
    D2 --> matchBrace{"match(TOKEN_LEFT_BRACE)?"}
    matchBrace -->|是| beginScope["beginScope(): scopeDepth = 1"]
    beginScope --> block["block(): 循环解析块内语句"]
    block --> endScope["endScope(): scopeDepth = 0"]
```

#### 语句 2a：`var a = 2;`（块作用域，scopeDepth=1）

```mermaid
graph TD
    VD2["varDeclaration()"]
    VD2 --> pv["parseVariable()"]
    pv --> cons["consume(TOKEN_IDENTIFIER): 消费 'a'"]
    cons --> dv["declareVariable()"]
    dv --> check{"scopeDepth == 0?"}
    check -->|否 局部| addL["addLocal('a'): locals[0] = {name:'a', depth:1}"]
    addL --> retZero["scopeDepth > 0 → return 0"]

    VD2 --> expr["expression(): 编译 '2'"]
    expr --> emit["发射 OP_CONSTANT 2"]

    VD2 --> defV["defineVariable(0)"]
    defV --> mark["scopeDepth > 0 → markInitialized()"]
    mark --> noEmit["不发射 OP_DEFINE_GLOBAL！\n值留在栈上就是局部变量"]
```

**关键**：局部变量**不需要**发射 `OP_DEFINE_GLOBAL`。`OP_CONSTANT 2` 把 `2.0` 压入栈顶，这个栈位置本身就代表了局部变量 `a`。

#### 语句 2b：`var b = 3;`（块作用域，scopeDepth=1）

同理：`addLocal('b')` → `locals[1] = {name:'b', depth:1}`，发射 `OP_CONSTANT 3`，值留在栈上。

#### 语句 2c：`print a;`

```mermaid
graph TD
    PS["printStatement()"]
    PS --> expr["expression() → variable() → namedVariable('a')"]
    expr --> resolve["resolveLocal(current, 'a')"]
    resolve --> search["从 locals 数组末尾向前搜索"]
    search --> found["locals[0].name == 'a' → 找到！返回 slot 0"]
    found --> emitGet["发射 OP_GET_LOCAL 0"]
    PS --> emitPrint["发射 OP_PRINT"]
```

`resolveLocal` 先在局部变量中查找，找到了 `locals[0]`（局部 `a=2`），**遮蔽**了全局的 `a=1`。

#### 语句 2d：`print b;`

同理：`resolveLocal` 找到 `locals[1]`（`b=3`），发射 `OP_GET_LOCAL 1`, `OP_PRINT`。

#### 语句 2e：`}` 离开块作用域

```mermaid
graph TD
    ES["endScope()"]
    ES --> dec["scopeDepth-- → 0"]
    dec --> check1{"locals[1].depth(1) > scopeDepth(0)?"}
    check1 -->|是| pop1["发射 OP_POP，localCount-- → 1"]
    pop1 --> check2{"locals[0].depth(1) > scopeDepth(0)?"}
    check2 -->|是| pop2["发射 OP_POP，localCount-- → 0"]
    pop2 --> done["清理完成，栈恢复到块之前的状态"]
```

#### 编译结束

`endCompiler()` 发射 `OP_RETURN`。

---

### 编译结果

#### 常量池

| 索引 | 值 | 说明 |
|------|------|------|
| 0 | `"a"` | 全局变量名 |
| 1 | `1.0` | 全局 a 的初始值 |
| 2 | `2.0` | 局部 a 的初始值 |
| 3 | `3.0` | 局部 b 的初始值 |

#### 字节码

| 偏移 | 指令 | 操作数 | 说明 |
|------|------|--------|------|
| 0 | `OP_CONSTANT` | 1 | 压入 1.0 |
| 2 | `OP_DEFINE_GLOBAL` | 0 | 弹出栈顶，定义全局变量 `"a"` = 1.0 |
| 4 | `OP_CONSTANT` | 2 | 压入 2.0（局部变量 a，占据 stack[0]） |
| 6 | `OP_CONSTANT` | 3 | 压入 3.0（局部变量 b，占据 stack[1]） |
| 8 | `OP_GET_LOCAL` | 0 | 读取 stack[0] = 2.0，压入栈顶 |
| 10 | `OP_PRINT` | — | 弹出并打印 2.0 |
| 11 | `OP_GET_LOCAL` | 1 | 读取 stack[1] = 3.0，压入栈顶 |
| 13 | `OP_PRINT` | — | 弹出并打印 3.0 |
| 14 | `OP_POP` | — | 弹出局部变量 b |
| 15 | `OP_POP` | — | 弹出局部变量 a |
| 16 | `OP_RETURN` | — | 程序结束 |

---

## 阶段四：VM 执行（vm.c）

### 执行流程

```mermaid
sequenceDiagram
    participant IP as 指令指针
    participant Stack as 值栈
    participant Globals as 全局变量表

    Note over IP,Globals: OP_CONSTANT 1
    IP->>Stack: push(1.0)
    Note right of Stack: [1.0]

    Note over IP,Globals: OP_DEFINE_GLOBAL 0
    IP->>Globals: globals["a"] = pop() = 1.0
    Note right of Stack: []
    Note right of Globals: {"a": 1.0}

    Note over IP,Globals: OP_CONSTANT 2 (局部变量 a)
    IP->>Stack: push(2.0)
    Note right of Stack: [2.0]

    Note over IP,Globals: OP_CONSTANT 3 (局部变量 b)
    IP->>Stack: push(3.0)
    Note right of Stack: [2.0, 3.0]

    Note over IP,Globals: OP_GET_LOCAL 0
    IP->>Stack: push(stack[0]) = push(2.0)
    Note right of Stack: [2.0, 3.0, 2.0]

    Note over IP,Globals: OP_PRINT
    IP->>Stack: pop() → 打印 "2"
    Note right of Stack: [2.0, 3.0]

    Note over IP,Globals: OP_GET_LOCAL 1
    IP->>Stack: push(stack[1]) = push(3.0)
    Note right of Stack: [2.0, 3.0, 3.0]

    Note over IP,Globals: OP_PRINT
    IP->>Stack: pop() → 打印 "3"
    Note right of Stack: [2.0, 3.0]

    Note over IP,Globals: OP_POP (清理局部变量 b)
    IP->>Stack: pop()
    Note right of Stack: [2.0]

    Note over IP,Globals: OP_POP (清理局部变量 a)
    IP->>Stack: pop()
    Note right of Stack: []

    Note over IP,Globals: OP_RETURN
    IP->>IP: return INTERPRET_OK
```

### 逐指令栈状态追踪

```
指令                      | 操作                           | 栈状态               | 全局变量表
========================= | ============================== | ==================== | ==========
OP_CONSTANT 1             | push(1.0)                      | [ 1.0 ]              | {}
OP_DEFINE_GLOBAL 0        | globals["a"] = pop()           | [ ]                  | {"a": 1.0}
OP_CONSTANT 2             | push(2.0)  ← 局部 a           | [ 2.0 ]              | {"a": 1.0}
OP_CONSTANT 3             | push(3.0)  ← 局部 b           | [ 2.0, 3.0 ]         | {"a": 1.0}
OP_GET_LOCAL 0            | push(stack[0]=2.0)             | [ 2.0, 3.0, 2.0 ]    | {"a": 1.0}
OP_PRINT                  | pop() → 输出 "2"              | [ 2.0, 3.0 ]         | {"a": 1.0}
OP_GET_LOCAL 1            | push(stack[1]=3.0)             | [ 2.0, 3.0, 3.0 ]    | {"a": 1.0}
OP_PRINT                  | pop() → 输出 "3"              | [ 2.0, 3.0 ]         | {"a": 1.0}
OP_POP                    | pop() → 丢弃 3.0              | [ 2.0 ]              | {"a": 1.0}
OP_POP                    | pop() → 丢弃 2.0              | [ ]                  | {"a": 1.0}
OP_RETURN                 | return INTERPRET_OK            | [ ]                  | {"a": 1.0}
```

---

## 核心机制总结

### 全局变量 vs 局部变量

```mermaid
graph LR
    subgraph "全局变量 (scopeDepth == 0)"
        G1["变量名存入常量池"]
        G2["值通过 OP_DEFINE_GLOBAL 存入 vm.globals 哈希表"]
        G3["通过 OP_GET_GLOBAL 按名字查表访问"]
        G1 --> G2 --> G3
    end

    subgraph "局部变量 (scopeDepth > 0)"
        L1["变量名记录在 Compiler.locals 数组（仅编译期使用）"]
        L2["值直接留在 VM 栈上，栈位置 = locals 数组索引"]
        L3["通过 OP_GET_LOCAL 按栈索引直接访问"]
        L1 --> L2 --> L3
    end
```

| 特性 | 全局变量 | 局部变量 |
|------|---------|---------|
| 存储位置 | `vm.globals` 哈希表 | VM 栈上 |
| 访问方式 | 按名字查哈希表 | 按栈索引直接读取 |
| 运行时开销 | 哈希 + 探测 | 一次数组下标 |
| 生命周期 | 整个程序 | `endScope` 时 `OP_POP` 清理 |
| 编译期记录 | 变量名存入常量池 | `locals[]` 数组（运行时不存在） |

### 变量遮蔽（Shadowing）

```
全局作用域 (scopeDepth=0):
  vm.globals["a"] = 1.0      ← 通过哈希表存储

块作用域 (scopeDepth=1):
  locals[0] = {name:"a", depth:1}  →  stack[0] = 2.0
  locals[1] = {name:"b", depth:1}  →  stack[1] = 3.0
```

当 `print a;` 执行时，`resolveLocal()` 从 `locals` 数组末尾向前搜索，先找到局部的 `a`（slot 0），返回 `OP_GET_LOCAL 0`。全局的 `a` 被**遮蔽**，根本不会去查 `vm.globals`。

### 块作用域生命周期

```mermaid
graph TD
    A["开始块 - beginScope: scopeDepth++"] --> B["var a = 2 - addLocal, OP_CONSTANT"]
    B --> C["var b = 3 - addLocal, OP_CONSTANT"]
    C --> D["print a - OP_GET_LOCAL 0, OP_PRINT"]
    D --> E["print b - OP_GET_LOCAL 1, OP_PRINT"]
    E --> F["结束块 - endScope"]
    F --> G["scopeDepth--"]
    G --> H["OP_POP: 弹出 b"]
    H --> I["OP_POP: 弹出 a"]
    I --> J["栈恢复到块之前的状态"]
```
