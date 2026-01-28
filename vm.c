#include "common.h"
#include "vm.h"
#include <stdio.h>

VM vm;

void initVM() {
}

void freeVM() {
}

/**
 * 任何指令的第一个字节都是操作码。给定一个操作码，我们需要找到实现该指令语义的正确的 C 代码，这个过程被称为解码或指令分派。
 ***/
static InterpretResult run() {
#define READ_BYTE() (*vm.ip++)
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])

    for (;;) {
        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                printValue(constant);
                printf("\n");
                break;
            }
            case OP_RETURN: {
                return INTERPRET_OK;
            }
        }
    }

#undef READ_BYTE
#undef READ_CONSTANT
}

InterpretResult interpret(Chunk* chunk) {
    vm.chunk = chunk;
    vm.ip = vm.chunk->code;
    return run();
}