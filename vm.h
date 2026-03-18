//
// Created by nathenieli on 2026/1/28.
//
#ifndef clox_vm_h
#define clox_vm_h
#include "object.h"
#include "chunk.h"
#include "value.h"
#include "table.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)
/**
 * 一个CallFrame 代表一个正在进行的函数调用。slots字段指向虚拟机的值栈中该函数可以使用的第一个槽
 */
typedef struct
{
  ObjClosure *closure;
  uint8_t *ip;
  Value *slots;
} CallFrame;

typedef struct
{
  Chunk *chunk;
  // 当虚拟机运行字节码时，它会记录它在哪里
  // IP 总是指向下一条指令，而不是当前正在处理的指令
  uint8_t *ip;
  CallFrame frames[FRAMES_MAX];
  int frameCount;
  // 栈
  Value stack[STACK_MAX];
  // 栈顶指针
  // 指针指向数组中栈顶元素的下一个元素位置
  Value *stackTop;
  Table globals;
  Table strings;
  // 指向上值的链表头指针
  ObjUpvalue *openUpvalues;
  // 虚拟机已分配的托管内存实时字节总数
  size_t bytesAllocated;
  // 触发下一次回收的阈值
  size_t nextGC;
  Obj *objects;
  int grayCount;
  int grayCapacity;
  Obj **grayStack;
} VM;

extern VM vm;

typedef enum
{
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR
} InterpretResult;

void initVM();
void freeVM();

InterpretResult interpret(const char *source);

void push(Value value);
Value pop();

#endif