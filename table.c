#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table *table)
{
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void freeTable(Table *table)
{
    FREE_ARRAY(Entry, table->entries, table->capacity);
    initTable(table);
}

// 哈希表查找槽位的核心函数，使用开放寻址 + 线性探测解决哈希冲突
// 同时服务于查找（tableGet）和插入（tableSet）：
//   - tableGet 用它查找已有 key
//   - tableSet 用它找到 key 应该放置的位置
static Entry *findEntry(Entry *entries, int capacity,
                        ObjString *key)
{
    uint32_t index = key->hash % capacity; // 计算理想槽位
    Entry *tombstone = NULL;

    // 线性探测：从起始位置逐个检查槽位
    for (;;)
    {
        Entry *entry = &entries[index];
        if (entry->key == NULL)
        {
            if (IS_NIL(entry->value))
            {
                // 空槽：key 不存在。若之前遇到过 tombstone 则复用其位置，否则返回当前空槽
                return tombstone != NULL ? tombstone : entry;
            }
            else
            {
                // Tombstone（已删除条目，key=NULL 但 value=true）
                // 不能直接返回：此时还不确定目标 key 是否在后面，
                // 直接返回会导致查找误判"key 不存在"，或插入时产生重复 key。
                // 只记录第一个 tombstone，继续探测直到找到 key 或真正的空槽。
                if (tombstone == NULL)
                    tombstone = entry;
            }
        }
        else if (entry->key == key)
        {
            // 找到目标 key（指针比较，因为字符串做了驻留/intern 处理）
            return entry;
        }

        index = (index + 1) % capacity; // 步进到下一个槽位
    }
}

// 哈希表扩容/重新散列函数
// 容量变化后，元素位置由 hash % capacity 决定，因此不能简单复制，
// 必须对每个元素重新计算槽位（rehash）
static void adjustCapacity(Table *table, int capacity)
{
    // 1. 按新容量分配新的 Entry 数组
    Entry *entries = ALLOCATE(Entry, capacity);
    // 2. 初始化所有槽位为空
    for (int i = 0; i < capacity; i++)
    {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }

    // 哈希表的“自愈机制”
    table->count = 0;

    // 3. 遍历旧数组，将有效条目重新插入新数组
    for (int i = 0; i < table->capacity; i++)
    {
        Entry *entry = &table->entries[i];
        if (entry->key == NULL)
            continue; // 跳过空槽

        // 在新数组中找到正确的槽位，复制 key/value
        Entry *dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }
    // 4. 释放旧数组，将 table 指向新数组
    FREE_ARRAY(Entry, table->entries, table->capacity);
    table->entries = entries;
    table->capacity = capacity;
}

bool tableSet(Table *table, ObjString *key, Value value)
{
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD)
    {
        int capacity = GROW_CAPACITY(table->capacity);
        adjustCapacity(table, capacity);
    }
    Entry *entry = findEntry(table->entries, table->capacity, key);
    bool isNewKey = entry->key == NULL;
    // 墓碑不计数，同时满足这两个条件才增加计数
    if (isNewKey && IS_NIL(entry->value))
        table->count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

void tableAddAll(Table *from, Table *to)
{
    for (int i = 0; i < from->capacity; i++)
    {
        Entry *entry = &from->entries[i];
        if (entry->key != NULL)
        {
            tableSet(to, entry->key, entry->value);
        }
    }
}

bool tableGet(Table *table, ObjString *key, Value *value)
{
    if (table->count == 0)
        return false;

    Entry *entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL)
        return false;

    *value = entry->value;
    return true;
}

bool tableDelete(Table *table, ObjString *key)
{
    if (table->count == 0)
        return false;

    // Find the entry.
    Entry *entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL)
        return false;

    // Place a tombstone in the entry.
    entry->key = NULL;
    entry->value = BOOL_VAL(true);
    return true;
}

ObjString *tableFindString(Table *table, const char *chars,
                           int length, uint32_t hash)

{
    if (table->count == 0)
        return NULL;

    uint32_t index = hash % table->capacity;
    for (;;)
    {
        Entry *entry = &table->entries[index];
        if (entry->key == NULL)
        {
            // Stop if we find an empty non-tombstone entry.
            if (IS_NIL(entry->value))
                return NULL;
        }
        else if (entry->key->length == length &&
                 entry->key->hash == hash &&
                 memcmp(entry->key->chars, chars, length) == 0)
        {
            // We found it.
            return entry->key;
        }

        index = (index + 1) % table->capacity;
    }
}

void markTable(Table *table)
{
    for (int i = 0; i < table->capacity; i++)
    {
        Entry *entry = &table->entries[i];
        markObject((Obj *)entry->key);
        markValue(entry->value);
    }
}