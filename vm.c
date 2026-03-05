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
    vm.frameCount = 0;
}

void initVM()
{
    resetStack();
    vm.objects = NULL;
    initTable(&vm.globals);
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
    CallFrame *frame = &vm.frames[vm.frameCount - 1];
    size_t instruction = frame->ip - frame->function->chunk.code - 1;
    int line = frame->function->chunk.lines[instruction];
    fprintf(stderr, "[line %d] in script\n", line);
    resetStack();
}

static Value peek(int distance)
{
    return vm.stackTop[-1 - distance];
}

static bool call(ObjFunction *function, int argCount)
{
    CallFrame *frame = &vm.frames[vm.frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;
    frame->slots = vm.stackTop - argCount - 1;
    return true;
}

static bool callValue(Value callee, int argCount)
{
    if (IS_OBJ(callee))
    {
        switch (OBJ_TYPE(callee))
        {
        case OBJ_FUNCTION:
            return call(AS_FUNCTION(callee), argCount);
        default:
            break; // Non-callable object type.
        }
    }
    runtimeError("Can only call functions and classes.");
    return false;
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
    freeTable(&vm.globals);

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
    CallFrame *frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)

#define READ_SHORT() \
    (frame->ip += 2, \
     (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

#define READ_CONSTANT() \
    (frame->function->chunk.constants.values[READ_BYTE()])
// 从字节码中读取一个字符串：先通过 READ_CONSTANT() 从常量池取出 Value，
// 再用 AS_STRING() 将其转换为 ObjString* 指针。
// 用于需要字符串操作数的指令（如 OP_DEFINE_GLOBAL），一步完成"读索引→查常量→转字符串"。
//
// 例: 编译 var name = "hello"; 后：
//   常量池: [ 0: "name",  1: "hello" ]
//   字节码: ... OP_DEFINE_GLOBAL 0 ...
//
// VM 执行 OP_DEFINE_GLOBAL 时调用 READ_STRING()，展开过程：
//   1. READ_BYTE():      *vm.ip++ → 读出字节 0，ip 前进一位
//   2. READ_CONSTANT():  vm.chunk->constants.values[0] → 取出包装了 "name" 的 Value
//   3. AS_STRING():      (ObjString*)AS_OBJ(value) → 转换为 ObjString* 指针
//   最终得到 ObjString { length=4, chars="name", hash=... }
//   然后 OP_DEFINE_GLOBAL 以此为键，将栈顶值 "hello" 存入 vm.globals 哈希表
#define READ_STRING() AS_STRING(READ_CONSTANT())
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
        disassembleInstruction(&frame->function->chunk,
                               (int)(frame->ip - frame->function->chunk.code));
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
        // 表达式语句（如 1+2;）求值后结果会留在栈顶，
        // 但该结果不会被后续指令使用，必须弹出以保持栈平衡
        case OP_POP:
            pop();
            break;
        case OP_GET_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            push(frame->slots[slot]);
            break;
        }
        case OP_SET_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = peek(0);
            break;
        }
        case OP_GET_GLOBAL:
        {
            ObjString *name = READ_STRING();
            Value value;
            if (!tableGet(&vm.globals, name, &value))
            {
                runtimeError("Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            push(value);
            break;
        }
        case OP_DEFINE_GLOBAL:
        {
            ObjString *name = READ_STRING();
            tableSet(&vm.globals, name, peek(0));
            pop();
            break;
        }
        // 全局变量赋值（如 name = "world";），区别于 OP_DEFINE_GLOBAL（创建新变量）。
        // 用 peek(0) 而非 pop() 读取新值，因为赋值是表达式，其结果值需保留在栈上。
        // tableSet 返回 true 表示键不存在（新插入），说明变量未被 var 定义过，
        // 此时需要 tableDelete 清除刚插入的非法条目，然后报运行时错误。
        case OP_SET_GLOBAL:
        {
            ObjString *name = READ_STRING();
            if (tableSet(&vm.globals, name, peek(0)))
            {
                tableDelete(&vm.globals, name);
                runtimeError("Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }

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
        // OP_ADD 是零操作数（zero-operand）的栈式指令，
        // 不需要从字节码流中读取额外字节，因此不需要移动 ip。
        // 操作数全部来自栈（pop两次），结果也压回栈（push一次）。
        // 此时 ip 已被 switch(instruction = READ_BYTE()) 移过操作码，
        // 正好指向下一条指令，无需再动。
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
        case OP_PRINT:
        {
            printValue(pop());
            printf("\n");
            break;
        }
        case OP_JUMP:
        {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE:
        {
            uint16_t offset = READ_SHORT();
            if (isFalsey(peek(0)))
                frame->ip += offset;

            break;
        }
        case OP_LOOP:
        {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }
        case OP_CALL:
        {
            int argCount = READ_BYTE();
            if (!callValue(peek(argCount), argCount))
            {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_RETURN:
        {
            return INTERPRET_OK;
        }
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char *source)
{
    ObjFunction *function = compile(source);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    call(function, 0);
    return run();
}