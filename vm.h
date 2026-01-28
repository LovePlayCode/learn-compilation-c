//
// Created by nathenieli on 2026/1/28.
//
#ifndef clox_vm_h
#define clox_vm_h

#include "chunk.h"

typedef struct {
    Chunk* chunk;
    // 当虚拟机运行字节码时，它会记录它在哪里
    // IP 总是指向下一条指令，而不是当前正在处理的指令
    uint8_t* ip;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
  } InterpretResult;

void initVM();
void freeVM();

InterpretResult interpret(Chunk* chunk);

#endif