#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"
#define ALLOCATE_OBJ(type, objectType) \
    (type *)allocateObject(sizeof(type), objectType)

static Obj *allocateObject(size_t size, ObjType type)
{
    Obj *object = (Obj *)reallocate(NULL, 0, size);
    object->type = type;
    return object;
}
/**
 * 在堆上创建字符串对象的副本
 * @param chars 源字符串指针（不会被修改）
 * @param length 字符串长度（不包括 '\0'）
 * @return 新创建的字符串对象
 *
 * 例如: copyString("hello", 5)
 * 1. 分配 6 字节堆内存 (5 + 1 用于存储 '\0')
 * 2. 复制 5 个字符到堆内存
 * 3. 添加字符串终止符 '\0'
 * 4. 创建 ObjString 对象包装这个堆字符串
 */
ObjString *copyString(const char *chars, int length)
{
    // 在堆上分配内存，+1 是为了存储字符串终止符 '\0'
    char *heapChars = ALLOCATE(char, length + 1);
    // 复制源字符串的内容到堆内存
    memcpy(heapChars, chars, length);
    // 添加 C 字符串终止符
    heapChars[length] = '\0';
    // 创建并返回 ObjString 对象
    return allocateString(heapChars, length);
}

void printObject(Value value)
{
    switch (OBJ_TYPE(value))
    {
    case OBJ_STRING:
        printf("%s", AS_CSTRING(value));
        break;
    }
}

static ObjString *allocateString(char *chars, int length)
{
    ObjString *string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->length = length;
    string->chars = chars;
    return string;
}

ObjString *takeString(char *chars, int length)
{
    return allocateString(chars, length);
}