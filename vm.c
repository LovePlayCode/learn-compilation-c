#include "common.h"
#include "vm.h"
#include <stdio.h>
#include "debug.h"
#include "compiler.h"

VM vm;

static void resetStack() {
    vm.stackTop = vm.stack;
}

void initVM() {
    resetStack();

}

Value pop() {
    vm.stackTop--;
    return *vm.stackTop;
}

void push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

void freeVM() {
}

/**
 * 任何指令的第一个字节都是操作码。给定一个操作码，我们需要找到实现该指令语义的正确的 C 代码，这个过程被称为解码或指令分派。
 ***/
static InterpretResult run() {
#define READ_BYTE() (*vm.ip++)
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
#define BINARY_OP(op) \
do { \
double b = pop(); \
double a = pop(); \
push(a op b); \
} while (false)

    for (;;) {
        

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
        for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        disassembleInstruction(vm.chunk,
                               (int)(vm.ip - vm.chunk->code));
#endif
        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_ADD:      BINARY_OP(+); break;
            case OP_SUBTRACT: BINARY_OP(-); break;
            case OP_MULTIPLY: BINARY_OP(*); break;
            case OP_DIVIDE:   BINARY_OP(/); break;
            case OP_NEGATE:   push(-pop()); break;
            case OP_RETURN: {
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

InterpretResult interpret(const char* source) {
    compile(source);
    return INTERPRET_OK;
}