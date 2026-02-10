#include <stdio.h>
#include <string.h>
#include "object.h"
#include "memory.h"
#include "value.h"

void initValueArray(ValueArray *array)
{
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void printValue(Value value)
{
    switch (value.type)
    {
    case VAL_BOOL:
        printf(AS_BOOL(value) ? "true" : "false");
        break;
    case VAL_NIL:
        printf("nil");
        break;
    case VAL_NUMBER:
        printf("%g", AS_NUMBER(value));
        break;
    case VAL_OBJ:
        printObject(value);
        break;
    }
}

/**
 * 为什么不能简单的使用 memcmp() 来比较两个 Value 结构体？
 *
 * 原因 1: 联合体(Union)的值问题
 *   Value 使用联合体存储不同类型的值，memcmp() 会比较整个结构体的字节序列。
 *   这会导致错误的比较结果，因为联合体中未使用的字段可能包含垃圾数据。
 *   例如：BOOL_VAL(true) 和 NIL_VAL 的字节可能完全不同，但它们在语义上可能相等。
 *
 * 原因 2: 未初始化的内存污染
 *   当只初始化联合体的一个字段（如 as.boolean）时，其他字段（如 as.number）
 *   的字节内容是不确定的，包含垃圾数据。memcmp() 会把这些垃圾字节当做比较对象，
 *   导致结果不确定。
 *
 * 原因 3: 类型信息才是关键
 *   Value 的比较必须遵循语义层面的逻辑：
 *   - 先检查 type 字段是否相同（类型必须匹配）
 *   - 再根据类型，只比较对应的有效字段
 *   memcmp() 是低层次的字节比较，忽略了类型的语义意义。
 *
 * 正确做法：类型检查 + switch 语句根据类型比较对应字段。
 */
bool valuesEqual(Value a, Value b)
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
    case VAL_BOOL:
        return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NIL:
        return true;
    case VAL_NUMBER:
        return AS_NUMBER(a) == AS_NUMBER(b);
    case VAL_OBJ:
        return AS_OBJ(a) == AS_OBJ(b);
    default:
        return false; // Unreachable.
    }
}

void writeValueArray(ValueArray *array, Value value)
{
    if (array->capacity < array->count + 1)
    {
        int oldCapacity = array->capacity;
        array->capacity = GROW_CAPACITY(oldCapacity);
        array->values = GROW_ARRAY(Value, array->values,
                                   oldCapacity, array->capacity);
    }

    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray *array)
{
    FREE_ARRAY(Value, array->values, array->capacity);
    initValueArray(array);
}
