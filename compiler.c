#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "compiler.h"
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

typedef void (*ParseFn)();

typedef struct
{
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

// 前向声明
static void expression();
static void parsePrecedence(Precedence precedence);
static ParseRule *getRule(TokenType type);
static void errorAt(Token *token, const char *message);
static void advance();
static void endCompiler();
static void emitReturn();
static void emitConstant(Value value);
static void number();
static void grouping();
static void unary();
static void binary();

Parser parser;
Chunk *compilingChunk;

static Chunk *currentChunk()
{
    return compilingChunk;
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

static void emitReturn()
{
    emitByte(OP_RETURN);
#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError)
    {
        disassembleChunk(currentChunk(), "code");
    }
#endif
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

static void endCompiler()
{
    emitReturn();
}

// ==================== 解析规则表 ====================

static void number()
{
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void grouping()
{
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

/**
 * 首先计算操作数，并将其值留在堆栈中。
   然后弹出该值，对其取负，并将结果压入栈中。
   按照源代码中的顺序对程序进行解析，并按照执行顺序对其重新排序。
 */
static void unary()
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

static void binary()
{
    TokenType operatorType = parser.previous.type;
    // 表驱动语法，通过 getRule() 查表 + precedence + 1，Pratt 解析器在解析右操作数时，精准地"只吃该吃的表达式"，从而让一个 binary() 函数正确处理所有不同优先级的二元运算符。
    ParseRule *rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operatorType)
    {
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

static void literal()
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
    [TOKEN_LEFT_PAREN] = {grouping, NULL, PREC_NONE},
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
    [TOKEN_BANG_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_GREATER] = {NULL, NULL, PREC_NONE},
    [TOKEN_GREATER_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_LESS] = {NULL, NULL, PREC_NONE},
    [TOKEN_LESS_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_IDENTIFIER] = {NULL, NULL, PREC_NONE},
    [TOKEN_STRING] = {NULL, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, NULL, PREC_NONE},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {NULL, NULL, PREC_NONE},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, NULL, PREC_NONE},
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

    prefixRule();

    while (precedence <= getRule(parser.current.type)->precedence)
    {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule();
    }
}

static void expression()
{
    // 我们只需要解析最低优先级，它也包含了所有更高优先级的表达式
    parsePrecedence(PREC_ASSIGNMENT);
}

// ==================== 编译入口 ====================

bool compile(const char *source, Chunk *chunk)
{
    initScanner(source);
    compilingChunk = chunk;
    parser.hadError = false;
    parser.panicMode = false;
    advance();
    expression();
    consume(TOKEN_EOF, "Expect end of expression.");
    endCompiler();
    return !parser.hadError;
}
