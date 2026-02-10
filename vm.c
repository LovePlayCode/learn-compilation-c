#include "common.h"
#include "vm.h"
#include <stdio.h>
#include "debug.h"
#include "compiler.h"
#include <stdarg.h>
#include <string.h>
#include "object.h"
#include "memory.h"
VM vm;

static void resetStack()
{
    vm.stackTop = vm.stack;
}

void initVM()
{
    resetStack();
    vm.objects = NULL;
    initTable(&vm.strings);
}

Value pop()
{
    vm.stackTop--;
    return *vm.stackTop;
}

static void runtimeError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    size_t instruction = vm.ip - vm.chunk->code - 1;
    int line = vm.chunk->lines[instruction];
    fprintf(stderr, "[line %d] in script\n", line);
    resetStack();
}

static Value peek(int distance)
{
    return vm.stackTop[-1 - distance];
}

void push(Value value)
{
    *vm.stackTop = value;
    vm.stackTop++;
}

/**
 * 虚拟机清理函数 - 释放所有动态分配的资源
 *
 * 执行流程：
 * 1. VM 通过 vm.objects 维护一个对象链表（单向链表）
 *    vm.objects → [字符串A] → [字符串B] → [字符串C] → NULL
 *
 * 2. freeObjects() 遍历链表释放每个对象：
 *    - 从链表头开始遍历
 *    - 保存 next 指针（避免访问已释放的内存）
 *    - 调用 freeObject() 释放当前对象（包括字符数组和对象结构体本身）
 *    - 移动到下一个节点
 *
 * 调用时机：程序退出前，在 main() 函数中调用
 *
 * 设计目的：
 * - 防止内存泄漏：确保所有堆对象被正确释放
 * - 集中管理：通过链表追踪所有创建的对象
 * - 为垃圾回收做准备：这是实现 GC 的基础架构
 */
void freeVM()
{
    freeTable(&vm.strings);

    // 释放虚拟机管理的所有堆对象（遍历 vm.objects 链表）
    freeObjects();
}
static bool isFalsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate()
{
    ObjString *b = AS_STRING(pop());
    ObjString *a = AS_STRING(pop());

    int length = a->length + b->length;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString *result = takeString(chars, length);
    push(OBJ_VAL(result));
}

/**
 * 任何指令的第一个字节都是操作码。给定一个操作码，我们需要找到实现该指令语义的正确的 C 代码，这个过程被称为解码或指令分派。
 ***/
static InterpretResult run()
{
#define READ_BYTE() (*vm.ip++)
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
#define BINARY_OP(valueType, op)                        \
    do                                                  \
    {                                                   \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) \
        {                                               \
            runtimeError("Operands must be numbers.");  \
            return INTERPRET_RUNTIME_ERROR;             \
        }                                               \
        double b = AS_NUMBER(pop());                    \
        double a = AS_NUMBER(pop());                    \
        push(valueType(a op b));                        \
    } while (false)

    for (;;)
    {

/**
 * chunk->code (数组起始地址)
    ↓
┌─────────────┬─────────────┬─────────────┐
│ OP_CONSTANT │   索引 0    │ OP_RETURN   │
└─────────────┴─────────────┴─────────────┘
                    ↑
                  vm.ip (当前指令指针)

offset = vm.ip - vm.chunk->code = 2
vm.chunk->code = 字节码数组的起始地址
vm.ip = 当前执行到的指令地址
两者相减 = 当前指令在数组中的索引
 */
#ifdef DEBUG_TRACE_EXECUTION
        printf("          ");
        for (Value *slot = vm.stack; slot < vm.stackTop; slot++)
        {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        disassembleInstruction(vm.chunk,
                               (int)(vm.ip - vm.chunk->code));
#endif
        uint8_t instruction;
        switch (instruction = READ_BYTE())
        {
        case OP_CONSTANT:
        {
            Value constant = READ_CONSTANT();
            push(constant);
            break;
        }
        case OP_NIL:
            push(NIL_VAL);
            break;
        case OP_TRUE:
            push(BOOL_VAL(true));
            break;
        case OP_FALSE:
            push(BOOL_VAL(false));
            break;
        case OP_EQUAL:
        {
            Value b = pop();
            Value a = pop();
            push(BOOL_VAL(valuesEqual(a, b)));
            break;
        }
        case OP_GREATER:
            BINARY_OP(BOOL_VAL, >);
            break;
        case OP_LESS:
            BINARY_OP(BOOL_VAL, <);
            break;
        case OP_ADD:
        {
            if (IS_STRING(peek(0)) && IS_STRING(peek(1)))
            {
                concatenate();
            }
            else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1)))
            {
                double b = AS_NUMBER(pop());
                double a = AS_NUMBER(pop());
                push(NUMBER_VAL(a + b));
            }
            else
            {
                runtimeError(
                    "Operands must be two numbers or two strings.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_SUBTRACT:
            BINARY_OP(NUMBER_VAL, -);
            break;
        case OP_MULTIPLY:
            BINARY_OP(NUMBER_VAL, *);
            break;
        case OP_DIVIDE:
            BINARY_OP(NUMBER_VAL, /);
            break;
        case OP_NOT:
            push(BOOL_VAL(isFalsey(pop())));
            break;
        case OP_NEGATE:
            if (!IS_NUMBER(peek(0)))
            {
                runtimeError("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(-AS_NUMBER(pop())));
            break;

        case OP_RETURN:
        {
            printValue(pop());
            printf("\n");
            return INTERPRET_OK;
        }
        }
    }

#undef READ_BYTE
#undef READ_CONSTANT
#undef BINARY_OP
}

InterpretResult interpret(const char *source)
{
    Chunk chunk;
    initChunk(&chunk);

    if (!compile(source, &chunk))
    {
        freeChunk(&chunk);
        return INTERPRET_COMPILE_ERROR;
    }

    vm.chunk = &chunk;
    vm.ip = vm.chunk->code;

    InterpretResult result = run();

    freeChunk(&chunk);
    return result;
}