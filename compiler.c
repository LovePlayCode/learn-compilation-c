#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "compiler.h"
#include "scanner.h"

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

Parser parser;

Chunk *compilingChunk;

static Chunk *currentChunk()
{
    return compilingChunk;
}

static void errorAtCurrent(const char *message)
{
    errorAt(&parser.current, message);
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

static void emitByte(uint8_t byte)
{
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2)
{
    emitByte(byte1);
    emitByte(byte2);
}

static void error(const char *message)
{
    errorAt(&parser.previous, message);
}

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

static void expression()
{
    // 我们只需要解析最低优先级，它也包含了所有更高优先级的表达式
    parsePrecedence(PREC_ASSIGNMENT);
}

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

static void endCompiler()
{
    emitReturn();
}

static void grouping()
{
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void parsePrecedence(Precedence precedence)
{
    // What goes here?
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
    case TOKEN_MINUS:
        emitByte(OP_NEGATE);
        break;
    default:
        return; // Unreachable.
    }
}

static void number()
{
    double value = strtod(parser.previous.start, NULL);
    emitConstant(value);
}

static void emitReturn()
{
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

static void advance()
{
    parser.previous = parser.current;

    // 语法防火墙，不允许错误的 token 传递到 Parse
    // 解析器的其它部分只能看到有效的标记
    for (;;)
    {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR)
            break;

        errorAtCurrent(parser.current.start);
    }
}