//
// Created by nathenieli on 2026/1/27.
//
#include <stdlib.h>
#include "vm.h"

#include "memory.h"

/**
 * 扩容场景（原地扩展成功）：
┌─────────┐              ┌─────────────────┐
│ 原数据   │     →       │ 原数据  │ 新空间 │
└─────────┘              └─────────────────┘

扩容场景（需要移动）：
原位置          新位置
┌─────────┐    ┌─────────────────┐
│ 原数据   │ →  │ 原数据  │ 新空间 │ ← 返回新地址
└─────────┘    └─────────────────┘
    ↓
  被释放
四种使用场景
pointer	oldSize	newSize	行为
NULL	0	> 0	分配新内存
有效指针	> 0	> oldSize	扩容
有效指针	> 0	< oldSize	缩容
有效指针	> 0	0	释放内存
 */
void *reallocate(void *pointer, size_t oldSize, size_t newSize)
{
    if (newSize == 0)
    {
        free(pointer);
        return NULL;
    }

    void *result = realloc(pointer, newSize);
    if (result == NULL)
        exit(1);

    return result;
}

static void freeObject(Obj *object)
{
    switch (object->type)
    {
    case OBJ_FUNCTION:
    {
        ObjFunction *function = (ObjFunction *)object;
        freeChunk(&function->chunk);
        FREE(ObjFunction, object);
        break;
    }
    case OBJ_NATIVE:
    {
        FREE(ObjNative, object);
        break;
    }
    case OBJ_STRING:
    {
        ObjString *string = (ObjString *)object;
        FREE_ARRAY(char, string->chars, string->length + 1);
        FREE(ObjString, object);
        break;
    }
    }
}

void freeObjects()
{
    Obj *object = vm.objects;
    while (object != NULL)
    {
        Obj *next = object->next;
        freeObject(object);
        object = next;
    }
}