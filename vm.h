//
// Created by nathenieli on 2026/1/28.
//
#ifndef clox_vm_h
#define clox_vm_h

#include "chunk.h"
#include "value.h"

#define STACK_MAX 256

typedef struct {
    Chunk* chunk;
    // 当虚拟机运行字节码时，它会记录它在哪里
    // IP 总是指向下一条指令，而不是当前正在处理的指令
    uint8_t* ip;
    // 栈
    Value stack[STACK_MAX];
    // 栈顶指针
    // 指针指向数组中栈顶元素的下一个元素位置
    Value* stackTop;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
  } InterpretResult;

void initVM();
void freeVM();

InterpretResult interpret(const char* source);

void push(Value value);
Value pop();

#endif