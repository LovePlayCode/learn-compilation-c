//
// Created by nathenieli on 2026/1/27.
//
#include <stdlib.h>

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

 */
void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
    if (newSize == 0) {
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, newSize);
    if (result == NULL) exit(1);

    return result;
}