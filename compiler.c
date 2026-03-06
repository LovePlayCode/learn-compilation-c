#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "compiler.h"
#include <string.h>
#include "scanner.h"
#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct
{
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

typedef enum
{
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_OR,         // or
    PREC_AND,        // and
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_TERM,       // + -
    PREC_FACTOR,     // * /
    PREC_UNARY,      // ! -
    PREC_CALL,       // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct
{
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct
{
    Token name;
    int depth;
} Local;

typedef enum
{
    TYPE_FUNCTION,
    TYPE_SCRIPT
} FunctionType;

typedef struct Compiler
{
    struct Compiler *enclosing;
    ObjFunction *function;
    FunctionType type;
    Local locals[UINT8_COUNT];
    // 记录了作用域中有多少局部变量(有多少数组槽在使用，还会跟踪"作用域深度")
    int localCount;
    int scopeDepth;
} Compiler;

// 前向声明
static void expression();
static void statement();
static void declaration();
static void parsePrecedence(Precedence precedence);
static ParseRule *getRule(TokenType type);
static void errorAt(Token *token, const char *message);
static void advance();
static void consume(TokenType type, const char *message);
static ObjFunction *endCompiler();
static void emitReturn();
static void emitConstant(Value value);
static uint8_t makeConstant(Value value);
static uint8_t identifierConstant(Token *name);
static uint8_t parseVariable(const char *errorMessage);
static void varDeclaration();
static void defineVariable(uint8_t global);
static void synchronize();
static void expressionStatement();
static void number(bool canAssign);
static void string(bool canAssign);
static void grouping(bool canAssign);
static void unary(bool canAssign);
static void binary(bool canAssign);
static void and_(bool canAssign);
static uint8_t argumentList();
static void forStatement();
static void ifStatement();

Parser parser;
Compiler *current = NULL;
Chunk *compilingChunk;

static Chunk *currentChunk()
{
    return &current->function->chunk;
}

// ==================== 错误处理 ====================

static void errorAt(Token *token, const char *message)
{
    if (parser.panicMode)
        return;

    parser.panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF)
    {
        fprintf(stderr, " at end");
    }
    else if (token->type == TOKEN_ERROR)
    {
        // Nothing.
    }
    else
    {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char *message)
{
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char *message)
{
    errorAt(&parser.current, message);
}

// ==================== 词法辅助 ====================

/**
 * advance() 函数的作用是"前进"到下一个 token。
 * 它维护两个状态：
 * - parser.previous: 上一个已消耗的 token
 * - parser.current:  当前要处理的 token
 *
 * 工作流程：
 * 1. parser.previous = parser.current  (保存旧的当前token)
 * 2. parser.current = scanToken()      (从扫描器读取新token)
 *
 * 这使得在任何规则函数中，parser.previous 总是指向刚被"消耗"的 token。
 * 例如在 number() 中，parser.previous 会指向数字token。
 *
 * 初始化的重要性：
 * 在 compile() 中，第一个 advance() 的目的是初始化 parser.current，
 * 因为在进入解析前，parser.current 处于未初始化状态。
 * 只有调用了这第一个 advance()，parser.current 才会有第一个真正的 token，
 * 然后 parsePrecedence() 内的 advance() 才能正确推进，
 * 同时把第一个 token 保存到 parser.previous。
 */
static void advance()
{
    parser.previous = parser.current;

    // 语法防火墙，不允许错误的 token 传递到 Parser
    // 解析器的其它部分只能看到有效的标记
    for (;;)
    {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR)
            break;

        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char *message)
{
    if (parser.current.type == type)
    {
        advance();
        return;
    }

    errorAtCurrent(message);
}

static bool check(TokenType type)
{
    return parser.current.type == type;
}

static bool match(TokenType type)
{
    if (!check(type))
        return false;
    advance();
    return true;
}

// ==================== 字节码生成 ====================

static void emitByte(uint8_t byte)
{
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2)
{
    emitByte(byte1);
    emitByte(byte2);
}

static void emitLoop(int loopStart)
{
    emitByte(OP_LOOP);

    // +2是考虑到了OP_LOOP指令自身操作数的大小，这个操作数我们也需要跳过。
    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX)
        error("Loop body too large.");

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

// 发射一条跳转指令，并预留两个字节的占位符用于后续回填跳转偏移量（backpatching）。
// 使用两个字节（16位）表示偏移量，最大可跳转 65535 字节，在紧凑性和跳转范围间取得平衡。
static int emitJump(uint8_t instruction)
{
    // 写入跳转指令操作码（如 OP_JUMP、OP_JUMP_IF_FALSE）
    emitByte(instruction);
    // 写入两个字节的占位符 0xff，此时还不知道跳转距离，后续由 patchJump 回填。
    // code 数组类型为 uint8_t，单个元素只能存 1 字节，所以 16 位偏移量需要拆成两个字节存储。
    emitByte(0xff);
    emitByte(0xff);
    // 返回占位符第一个字节在 code 数组中的索引，供 patchJump 定位并回填。
    return currentChunk()->count - 2;
}

static void emitReturn()
{
    emitByte(OP_NIL);

    emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value)
{
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX)
    {
        error("Too many constants in one chunk.");
        return 0;
    }

    return (uint8_t)constant;
}

static void emitConstant(Value value)
{
    emitBytes(OP_CONSTANT, makeConstant(value));
}

// 回填 emitJump 预留的跳转偏移量占位符。
// 在编译完跳转目标代码后调用，将真正的跳转距离写回 code 数组中之前预留的位置。
static void patchJump(int offset)
{
    // 计算跳转距离：从占位符之后到当前位置的字节数。
    // -2 是因为 VM 读完两个占位符字节后 IP 已指向其后方，偏移量应从那里算起。
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX)
    {
        error("Too much code to jump over.");
    }

    // 将 16 位跳转距离以大端序写回 code 数组，覆盖之前的 0xff 占位符。
    // 例如 jump = 0x0A1B:
    //   (jump >> 8) & 0xff → 取高 8 位 → 0x0A → 存入 code[offset]
    //   jump & 0xff        → 取低 8 位 → 0x1B → 存入 code[offset+1]
    // VM 读取时通过 (code[offset] << 8) | code[offset+1] 拼回完整的 16 位值。
    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler *compiler, FunctionType type)
{
    compiler->enclosing = current;

    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->function = newFunction();
    current = compiler;
    if (type != TYPE_SCRIPT)
    {
        current->function->name = copyString(parser.previous.start, parser.previous.length);
    }
    Local *local = &current->locals[current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
}

static ObjFunction *endCompiler()
{
    emitReturn();
    ObjFunction *function = current->function;
#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError)
    {
        // 替换部分开始
        disassembleChunk(currentChunk(), function->name != NULL
                                             ? function->name->chars
                                             : "<script>");
        // 替换部分结束
    }
#endif
    // 当编译器完成时，将之前的编译器恢复为新的当前编译器，从而将自己从栈中弹出
    current = current->enclosing;

    return function;
}

static void beginScope()
{
    current->scopeDepth++;
}

static void endScope()
{
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth >
               current->scopeDepth)
    {
        emitByte(OP_POP);
        current->localCount--;
    }
}

static uint8_t identifierConstant(Token *name)
{
    return makeConstant(OBJ_VAL(copyString(name->start,
                                           name->length)));
}

static void addLocal(Token name)
{
    if (current->localCount == UINT8_COUNT)
    {
        error("Too many local variables in function.");
        return;
    }
    Local *local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
    local->depth = current->scopeDepth;
}

static bool identifiersEqual(Token *a, Token *b)
{
    if (a->length != b->length)
        return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name)
{
    for (int i = compiler->localCount - 1; i >= 0; i--)
    {
        Local *local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name))
        {
            if (local->depth == -1)
            {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }

    return -1;
}

static void declareVariable()
{
    if (current->scopeDepth == 0)
        return;

    Token *name = &parser.previous;

    for (int i = current->localCount - 1; i >= 0; i--)
    {
        Local *local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth)
        {
            break;
        }

        if (identifiersEqual(name, &local->name))
        {
            error("Already a variable with this name in this scope.");
        }
    }

    addLocal(*name);
}

static uint8_t parseVariable(const char *errorMessage)
{
    consume(TOKEN_IDENTIFIER, errorMessage);
    declareVariable();
    if (current->scopeDepth > 0)
        return 0;
    return identifierConstant(&parser.previous);
}

static void markInitialized()
{
    // 因为全局函数声明时也会调用，所以需要兼容这种情况。
    if (current->scopeDepth == 0)
        return;

    current->locals[current->localCount - 1].depth =
        current->scopeDepth;
}

// ==================== 解析规则表 ====================

static void number(bool canAssign)
{

    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

// 编译 or 运算符，实现短路求值（short-circuit evaluation）
// 进入时左操作数已编译完毕，其值在栈顶
static void or_(bool canAssign)
{
    // 如果左操作数为 false，跳到右操作数处继续求值
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    // 如果左操作数为 true（没跳走），直接跳到末尾，
    // 保留左操作数的值作为整个 or 表达式的结果——这就是短路
    int endJump = emitJump(OP_JUMP);

    // false 路径跳到这里，左操作数为 false 已无用，弹掉它
    patchJump(elseJump);
    emitByte(OP_POP);

    // 编译右操作数，其结果压栈，作为整个 or 表达式的值
    parsePrecedence(PREC_OR);
    // true 路径（短路）的跳转目标回填到这里，两条路径汇合
    patchJump(endJump);
}

static void string(bool canAssign)
{
    // 去掉字符串两端的引号
    // parser.previous.start + 1: 跳过开头的 " 引号
    // parser.previous.length - 2: 减去首尾两个引号的长度
    // 例如: "hello" -> hello (从索引1开始，复制5个字符)
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1,
                                    parser.previous.length - 2)));
}

static void namedVariable(Token name, bool canAssign)
{
    uint8_t getOp, setOp;
    int arg = resolveLocal(current, &name);
    if (arg != -1)
    {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    }
    else
    {
        arg = identifierConstant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }
    if (canAssign && match(TOKEN_EQUAL))
    {
        expression();
        emitBytes(setOp, (uint8_t)arg);
    }
    else
    {
        emitBytes(getOp, (uint8_t)arg);
    }
}

static void variable(bool canAssign)
{
    namedVariable(parser.previous, canAssign);
}

static void grouping(bool canAssign)
{
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

/**
 * 首先计算操作数，并将其值留在堆栈中。
   然后弹出该值，对其取负，并将结果压入栈中。
   按照源代码中的顺序对程序进行解析，并按照执行顺序对其重新排序。
 */
static void unary(bool canAssign)
{
    TokenType operatorType = parser.previous.type;

    // Compile the operand.
    // 我们使用一元运算符本身的PREC_UNARY优先级来允许嵌套的一元表达式，如!!doubleNegative
    parsePrecedence(PREC_UNARY);

    // Emit the operator instruction.
    switch (operatorType)
    {
    case TOKEN_BANG:
        emitByte(OP_NOT);
        break;

    case TOKEN_MINUS:
        emitByte(OP_NEGATE);
        break;
    default:
        return; // Unreachable.
    }
}

static void binary(bool canAssign)
{

    TokenType operatorType = parser.previous.type;
    // 表驱动语法，通过 getRule() 查表 + precedence + 1，Pratt 解析器在解析右操作数时，精准地"只吃该吃的表达式"，从而让一个 binary() 函数正确处理所有不同优先级的二元运算符。
    ParseRule *rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operatorType)
    {
    // !=
    case TOKEN_BANG_EQUAL:
        emitBytes(OP_EQUAL, OP_NOT);
        break;
    // ==
    case TOKEN_EQUAL_EQUAL:
        emitByte(OP_EQUAL);
        break;
    case TOKEN_GREATER:
        emitByte(OP_GREATER);
        break;
    case TOKEN_GREATER_EQUAL:
        emitBytes(OP_LESS, OP_NOT);
        break;
    case TOKEN_LESS:
        emitByte(OP_LESS);
        break;
    case TOKEN_LESS_EQUAL:
        emitBytes(OP_GREATER, OP_NOT);
        break;
    case TOKEN_PLUS:
        emitByte(OP_ADD);
        break;
    case TOKEN_MINUS:
        emitByte(OP_SUBTRACT);
        break;
    case TOKEN_STAR:
        emitByte(OP_MULTIPLY);
        break;
    case TOKEN_SLASH:
        emitByte(OP_DIVIDE);
        break;
    default:
        return; // Unreachable.
    }
}

static void call(bool canAssign)
{
    uint8_t argCount = argumentList();
    emitBytes(OP_CALL, argCount);
}

static void literal(bool canAssign)
{
    switch (parser.previous.type)
    {
    case TOKEN_FALSE:
        // 将布尔值和nil字面量编译为字节码
        emitByte(OP_FALSE);
        break;
    case TOKEN_NIL:
        emitByte(OP_NIL);
        break;
    case TOKEN_TRUE:
        emitByte(OP_TRUE);
        break;
    default:
        return; // Unreachable.
    }
}

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, NULL, PREC_NONE},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
    [TOKEN_BANG] = {unary, NULL, PREC_NONE},
    [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {NULL, NULL, PREC_NONE},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    [TOKEN_SUPER] = {NULL, NULL, PREC_NONE},
    [TOKEN_THIS] = {NULL, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},
    [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

static ParseRule *getRule(TokenType type)
{
    return &rules[type];
}

// ==================== Pratt 解析器核心 ====================

/**
 * 表达式：

-1 + 2 * 3


假设我们调用：

parsePrecedence(PREC_ASSIGNMENT);

步骤跟踪

解析前缀 -1

advance() → TOKEN_MINUS

prefixRule() → unary()

unary() 调用 parsePrecedence(PREC_UNARY) → 解析 1

发出 OP_CONSTANT 1 和 OP_NEGATE

循环处理中缀运算符 +

parser.current → TOKEN_PLUS

getRule()->precedence → PREC_TERM (加减)

PREC_ASSIGNMENT <= PREC_TERM → 是，进入循环

advance() 消耗 +

infixRule() → binary()

binary() 调用 parsePrecedence(PREC_TERM + 1) → 解析右操作数 2 * 3

解析 2 → 发出 OP_CONSTANT 2

解析 * → 乘法优先级比加法高 → 进入二元循环

解析 3 → 发出 OP_CONSTANT 3，再发出 OP_MULTIPLY

完成右操作数 → 发出 OP_ADD

下一个 token 是 EOF → 循环结束

整个表达式完成解析。

4. 为什么 Pratt Parser 高效优雅

不需要为每种运算符写不同函数

前缀用 prefixRule

中缀用 binary()（或者其它中缀函数）

优先级由表格驱动动态处理右操作数

天然处理结合性和优先级

parsePrecedence(当前运算符优先级+1) → 左结合

右结合可以传入同级优先级

灵活扩展

新增运算符只需更新 ParseRule 表格，函数指针即可自动接入解析流程。

5. 小结

parsePrecedence() 的核心思想：

先处理前缀 → 消耗第一个 token 并生成左操作数

循环处理中缀 → 根据优先级决定是否继续解析

递归调用 → 对右操作数和括号等结构递归解析

表格驱动 → ParseRule 表格将 token 与前缀/中缀函数和优先级关联起来

✅ 它将左结合和优先级控制自然整合，代码量少，但能解析复杂表达式。
 */
static void parsePrecedence(Precedence precedence)
{
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL)
    {
        error("Expect expression.");
        return;
    }

    // canAssign 由调用时传入的优先级决定：
    // - expression() 传入 PREC_ASSIGNMENT → canAssign = true（允许赋值）
    // - binary() 解析右操作数时传入更高优先级 → canAssign = false（禁止赋值）
    // 它在本函数中只被赋值一次，不会从 false 变为 true。
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence)
    {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }
    // 兜底检查：捕获非法赋值目标（如 a * b = 3;）。
    // 解析 a * b = 3; 时，最外层 canAssign = true，但 b 的 namedVariable
    // 收到的 canAssign = false（因为 binary() 以更高优先级解析右操作数），
    // 不会消费 =，= 号留在 token 流中。回到最外层后由此处兜住，报出明确错误，
    // 避免 = 号残留导致后续产生令人困惑的级联错误。
    if (canAssign && match(TOKEN_EQUAL))
    {
        error("Invalid assignment target.");
    }
}

static void expression()
{
    // 我们只需要解析最低优先级，它也包含了所有更高优先级的表达式
    parsePrecedence(PREC_ASSIGNMENT);
}

static void block()
{
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
    {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type)
{
    Compiler compiler;
    initCompiler(&compiler, type);
    beginScope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    if (!check(TOKEN_RIGHT_PAREN))
    {
        do
        {
            current->function->arity++;
            if (current->function->arity > 255)
            {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            uint8_t constant = parseVariable("Expect parameter name.");
            defineVariable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    ObjFunction *function = endCompiler();
    emitBytes(OP_CONSTANT, makeConstant(OBJ_VAL(function)));
}

static void funDeclaration()
{
    uint8_t global = parseVariable("Expect function name.");
    markInitialized();
    function(TYPE_FUNCTION);
    defineVariable(global);
}

static void printStatement()
{
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void returnStatement()
{
    if (current->type == TYPE_SCRIPT)
    {
        error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON))
    {
        emitReturn();
    }
    else
    {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitByte(OP_RETURN);
    }
}

static void whileStatement()
{
    // 代码的初始位置，作为循环的起点，供 emitLoop() 使用
    int loopStart = currentChunk()->count;
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();
    emitLoop(loopStart);
    patchJump(exitJump);
    emitByte(OP_POP);
}

static void declaration()
{
    if (match(TOKEN_FUN))
    {
        funDeclaration();
    }
    else if (match(TOKEN_VAR))
    {

        varDeclaration();
    }
    else
    {
        statement();
    }
    // 使用恐慌模式下的错误恢复来减少它所报告的级联编译错误。
    if (parser.panicMode)
        synchronize();
}

// 编译变量声明语句（var name = "hello"; 或 var name;）
//
// 以 var name = "hello"; 为例，token 流: VAR name = "hello" ;
// 进入本函数时 VAR 已被 declaration() 消费。
//
// 1. parseVariable() 消费 token `name`，将变量名 "name" 存入常量池，返回索引（假设为 0）
//    常量池: [ 0: "name" ]
//
// 2. match(TOKEN_EQUAL) 成功 → expression() 编译 "hello"，生成 OP_CONSTANT 1
//    常量池: [ 0: "name",  1: "hello" ]
//    字节码: OP_CONSTANT 1          ← 将 "hello" 压入栈顶
//    若无 = 号（如 var name;），则 emitByte(OP_NIL)，用 nil 作为默认初始值
//
// 3. consume(TOKEN_SEMICOLON) 消费分号
//
// 4. defineVariable(0) → emitBytes(OP_DEFINE_GLOBAL, 0)
//    字节码: OP_CONSTANT 1, OP_DEFINE_GLOBAL 0
//
// VM 执行时：
//   OP_CONSTANT 1       → 从常量池取 "hello" 压入栈     栈: ["hello"]
//   OP_DEFINE_GLOBAL 0  → READ_STRING() 取常量池[0]即 "name"
//                       → tableSet(&vm.globals, "name", pop())  栈: []
static void varDeclaration()
{
    uint8_t global = parseVariable("Expect variable name.");

    if (match(TOKEN_EQUAL))
    {
        expression();
    }
    else
    {
        emitByte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON,
            "Expect ';' after variable declaration.");

    defineVariable(global);
}

static void defineVariable(uint8_t global)
{
    if (current->scopeDepth > 0)
    {
        markInitialized();
        return;
    }
    emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList()
{
    uint8_t argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN))
    {
        do
        {
            expression();
            if (argCount == 255)
            {
                error("Can't have more than 255 arguments.");
            }
            argCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return argCount;
}

static void and_(bool canAssign)
{
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(endJump);
}

// 恐慌模式错误恢复：当编译器遇到语法错误进入 panicMode 后，
// 会跳过后续 token 直到找到一个"同步点"——即下一条语句的边界，
// 从而避免一个语法错误引发大量无意义的级联错误报告。
static void synchronize()
{
    // 退出恐慌模式，恢复正常的错误报告
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF)
    {
        // 同步点1：刚跳过一个分号，说明上一条语句已结束，
        // 下一个 token 是新语句的开头，可以恢复正常编译
        if (parser.previous.type == TOKEN_SEMICOLON)
            return;
        // 同步点2：当前 token 是语句起始关键字，
        // 说明已到达新语句的边界，可以从这里恢复编译
        switch (parser.current.type)
        {
        case TOKEN_CLASS:
        case TOKEN_FUN:
        case TOKEN_VAR:
        case TOKEN_FOR:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_PRINT:
        case TOKEN_RETURN:
            return;

        default:; // 不是同步点，继续跳过
        }

        // 丢弃当前 token，继续向前扫描寻找同步点
        advance();
    }
}

static void statement()
{
    if (match(TOKEN_PRINT))
    {
        printStatement();
    }
    else if (match(TOKEN_FOR))
    {
        forStatement();
        // 新增部分结束
    }
    else if (match(TOKEN_IF))
    {
        ifStatement();
    }
    else if (match(TOKEN_RETURN))
    {
        returnStatement();
    }
    else if (match(TOKEN_WHILE))
    {
        whileStatement();
    }
    else if (match(TOKEN_LEFT_BRACE))
    {
        beginScope();
        block();
        endScope();
    }
    else
    {
        expressionStatement();
    }
}

static void expressionStatement()
{
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void forStatement()
{
    // 有变量存在，将变量的作用域限制在 for 循环内部
    beginScope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    /**
     * 这是 for 循环初始化子句的编译，处理 for ( 后面的第一个部分。for 循环有三种写法：

  for (;          ...)   → 没有初始化
  for (var i = 0; ...)   → 变量声明
  for (i = 0;     ...)   → 表达式语句（通常是赋值）

  逐段对应：

  if (match(TOKEN_SEMICOLON))
  {
      // for (; ...) — 直接是分号，没有初始化，什么都不做
  }
  else if (match(TOKEN_VAR))
  {
      // for (var i = 0; ...) — 声明一个新变量
      varDeclaration();
  }
  else
  {
      // for (i = 0; ...) — 普通表达式语句（赋值等）
      // 用 expressionStatement 而不是 expression，
      // 因为它会消费分号并生成 OP_POP 丢弃表达式的值
      expressionStatement();
  }

  第三种用 expressionStatement() 而不是 expression() 有两个原因：
  1. 它会自动消费结尾的 ;
  2. 它会生成 OP_POP 丢弃表达式的结果值（初始化子句的值没人需要）
     */
    if (match(TOKEN_SEMICOLON))
    {
        // No initializer.
    }
    else if (match(TOKEN_VAR))
    {
        varDeclaration();
    }
    else
    {
        expressionStatement();
    }
    int loopStart = currentChunk()->count;
    int exitJump = -1;
    if (!match(TOKEN_SEMICOLON))
    {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        // Jump out of the loop if the condition is false.
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP); // Condition.
    }
    if (!match(TOKEN_RIGHT_PAREN))
    {
        int bodyJump = emitJump(OP_JUMP);
        int incrementStart = currentChunk()->count;
        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emitLoop(loopStart);
        loopStart = incrementStart;
        patchJump(bodyJump);
    }

    statement();
    emitLoop(loopStart);
    if (exitJump != -1)
    {
        patchJump(exitJump);
        emitByte(OP_POP); // Condition.
    }
    endScope();
}

// 编译 if 语句，使用 backpatching（回填）技术处理前向跳转
static void ifStatement()
{
    // 解析 if (condition) 语法结构
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression(); // 编译条件表达式，运行时结果会被压入栈顶
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    // 发射条件跳转指令：如果栈顶值为 false，则跳过 then 分支
    // 此时跳转距离未知，emitJump 先写入占位符(0xFF, 0xFF)，返回占位符的偏移量
    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    // 编译 then 分支的语句体，字节码紧跟在跳转指令之后
    statement();
    int elseJump = emitJump(OP_JUMP);
    // then 分支编译完毕，现在可以计算跳转距离了
    // 将实际偏移量回填到之前的占位符处，使 false 时跳转到此处继续执行
    patchJump(thenJump);
    emitByte(OP_POP);
    if (match(TOKEN_ELSE))
        statement();
    patchJump(elseJump);
}

// ==================== 编译入口 ====================

ObjFunction *compile(const char *source)
{
    initScanner(source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);
    parser.hadError = false;
    parser.panicMode = false;
    // 第一次 advance()：初始化 Parser 状态，加载第一个 token 到 parser.current
    // 这是必需的，否则 parser.current 处于未初始化状态
    advance();
    // 现在 parser.current 有了第一个真正的 token，可以开始解析表达式
    // parsePrecedence() 会调用第二个 advance()，将第一个 token 推到 parser.previous
    while (!match(TOKEN_EOF))
    {
        declaration();
    }
    ObjFunction *function = endCompiler();
    return parser.hadError ? NULL : function;
}
