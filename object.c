#include <stdio.h>
#include <string.h>
#include "table.h"

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"
#define ALLOCATE_OBJ(type, objectType) \
    (type *)allocateObject(sizeof(type), objectType)

static ObjString *allocateString(char *chars, int length, uint32_t hash);
static uint32_t hashString(const char *key, int length);

static Obj *allocateObject(size_t size, ObjType type)
{
    Obj *object = (Obj *)reallocate(NULL, 0, size);
    object->type = type;
    object->next = vm.objects;
    vm.objects = object;
    return object;
}

ObjClosure *newClosure(ObjFunction *function)
{
    ObjUpvalue **upvalues = ALLOCATE(ObjUpvalue *,
                                     function->upvalueCount);
    for (int i = 0; i < function->upvalueCount; i++)
    {
        upvalues[i] = NULL;
    }
    ObjClosure *closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalueCount = function->upvalueCount;
    return closure;
}

ObjFunction *newFunction()
{
    ObjFunction *function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
    function->arity = 0;
    function->upvalueCount = 0;
    function->name = NULL;
    initChunk(&function->chunk);
    return function;
}

ObjNative *newNative(NativeFn function)
{
    ObjNative *native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = function;
    return native;
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
    uint32_t hash = hashString(chars, length);
    ObjString *interned = tableFindString(&vm.strings, chars, length,
                                          hash);
    if (interned != NULL)
        return interned;
    // 在堆上分配内存，+1 是为了存储字符串终止符 '\0'
    char *heapChars = ALLOCATE(char, length + 1);
    // 复制源字符串的内容到堆内存
    memcpy(heapChars, chars, length);
    // 添加 C 字符串终止符
    heapChars[length] = '\0';
    // 创建并返回 ObjString 对象
    return allocateString(heapChars, length, hash);
}

ObjUpvalue *newUpvalue(Value *slot)
{
    ObjUpvalue *upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
    upvalue->location = slot;
    return upvalue;
}

static void printFunction(ObjFunction *function)
{
    if (function->name == NULL)
    {
        printf("<script>");
        return;
    }

    printf("<fn %s>", function->name->chars);
}

void printObject(Value value)
{
    switch (OBJ_TYPE(value))
    {
    case OBJ_CLOSURE:
        printFunction(AS_CLOSURE(value)->function);
        break;
    case OBJ_FUNCTION:
        printFunction(AS_FUNCTION(value));
        break;
    case OBJ_NATIVE:
        printf("<native fn>");
        break;
    case OBJ_STRING:
        printf("%s", AS_CSTRING(value));
        break;
    case OBJ_UPVALUE:
        printf("upvalue");
        break;
    }
}

static ObjString *allocateString(char *chars, int length,
                                 uint32_t hash)
{
    ObjString *string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;
    tableSet(&vm.strings, string, NIL_VAL);
    return string;
}

// FNV-1a 哈希函数：将字符串转换为 32 位哈希值
// 用于哈希表中的字符串键散列，特点是实现简单、速度快、分布均匀
// FNV-1a 先异或后乘（区别于 FNV-1 的先乘后异或），雪崩效应更好
static uint32_t hashString(const char *key, int length)
{
    uint32_t hash = 2166136261u; // FNV offset basis (0x811c9dc5)
    for (int i = 0; i < length; i++)
    {
        hash ^= (uint8_t)key[i]; // 将当前字节与哈希值异或
        hash *= 16777619;        // 乘以 FNV 素数 (0x01000193)
    }
    return hash;
}

ObjString *takeString(char *chars, int length)
{
    uint32_t hash = hashString(chars, length);
    ObjString *interned = tableFindString(&vm.strings, chars, length,
                                          hash);
    if (interned != NULL)
    {
        FREE_ARRAY(char, chars, length + 1);
        return interned;
    }
    return allocateString(chars, length, hash);
}