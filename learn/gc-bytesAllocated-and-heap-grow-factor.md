# bytesAllocated 与 GC_HEAP_GROW_FACTOR 详解

## 一、概述

在 clox 虚拟机的垃圾回收（GC）机制中，`bytesAllocated` 和 `GC_HEAP_GROW_FACTOR` 是两个核心变量，它们协同工作，决定了 **何时触发 GC** 以及 **GC 后下一次触发的阈值**。

---

## 二、变量定义

```c
// vm.h - VM 结构体中
typedef struct {
    // ...
    size_t bytesAllocated;  // 虚拟机已分配的托管内存实时字节总数
    size_t nextGC;          // 触发下一次回收的阈值
    // ...
} VM;
```

```c
// memory.c
#define GC_HEAP_GROW_FACTOR 2
```

| 变量 | 类型 | 作用 |
|------|------|------|
| `bytesAllocated` | `size_t` | 实时跟踪 VM 当前托管内存的总字节数（油表） |
| `nextGC` | `size_t` | 触发下一次 GC 的内存阈值（油箱容量线） |
| `GC_HEAP_GROW_FACTOR` | 宏常量 `2` | 每次 GC 后，阈值 = 存活内存 × 此因子 |

---

## 三、整体协作流程

```mermaid
flowchart TD
    A["reallocate() 被调用"] --> B["更新 bytesAllocated\nbytesAllocated += newSize - oldSize"]
    B --> C{"是内存增长操作?\nnewSize > oldSize"}
    C -- 否 --> D["直接执行 realloc / free"]
    C -- 是 --> E{"bytesAllocated > nextGC ?"}
    E -- 否 --> F["直接执行 realloc"]
    E -- 是 --> G["collectGarbage()"]
    G --> H["markRoots()\n标记根对象"]
    H --> I["traceReferences()\n追踪所有引用"]
    I --> J["tableRemoveWhite()\n清理字符串驻留表"]
    J --> K["sweep()\n清扫未标记对象"]
    K --> L["nextGC = bytesAllocated × GC_HEAP_GROW_FACTOR\n设定新阈值"]
    L --> F
```

---

## 四、bytesAllocated 详解

### 4.1 记账原理

`reallocate()` 是虚拟机中 **所有内存操作的唯一入口**，其第一行就更新计数：

```c
vm.bytesAllocated += newSize - oldSize;
```

这一行巧妙地覆盖了所有四种内存操作场景：

```mermaid
flowchart LR
    subgraph 四种场景
        A["分配新内存\noldSize=0, newSize>0\n差值: +正数"]
        B["扩容\noldSize>0, newSize>oldSize\n差值: +正数"]
        C["缩容\noldSize>0, newSize<oldSize\n差值: -负数"]
        D["释放内存\noldSize>0, newSize=0\n差值: -oldSize"]
    end
    A --> E["bytesAllocated\n始终 = 当前托管内存总量"]
    B --> E
    C --> E
    D --> E
```

### 4.2 触发 GC 的判断

```c
if (newSize > oldSize) {              // 只在内存增长时检查
    if (vm.bytesAllocated > vm.nextGC) {  // 超过阈值?
        collectGarbage();                  // 触发 GC
    }
}
```

> 只有 **内存增长** 时才检查，缩容和释放不会触发 GC（因为内存在减少，没必要回收）。

---

## 五、GC_HEAP_GROW_FACTOR 详解

### 5.1 作用

每次 GC 结束后，用存活内存量乘以此因子来设定下一次 GC 的阈值：

```c
vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
```

### 5.2 自适应调度示意

```mermaid
sequenceDiagram
    participant P as 程序运行
    participant M as bytesAllocated
    participant GC as GC

    Note over M: 初始 nextGC = 1MB

    P->>M: 不断分配内存...
    M->>M: bytesAllocated 增长到 1MB
    M->>GC: bytesAllocated > nextGC, 触发 GC!

    Note over GC: 标记 + 清扫
    GC->>M: sweep 释放垃圾后 bytesAllocated = 600KB
    GC->>M: nextGC = 600KB × 2 = 1.2MB

    P->>M: 继续分配内存...
    M->>M: bytesAllocated 增长到 1.2MB
    M->>GC: bytesAllocated > nextGC, 触发 GC!

    Note over GC: 标记 + 清扫
    GC->>M: sweep 释放垃圾后 bytesAllocated = 800KB
    GC->>M: nextGC = 800KB × 2 = 1.6MB

    P->>M: 继续分配内存...
```

### 5.3 因子大小的权衡

```mermaid
quadrantChart
    title GC_HEAP_GROW_FACTOR 权衡
    x-axis "GC频率低" --> "GC频率高"
    y-axis "内存占用高" --> "内存占用低"
    "因子=4 (宽松)": [0.2, 0.2]
    "因子=2 (平衡)": [0.5, 0.5]
    "因子=1.5 (紧凑)": [0.75, 0.75]
```

| 因子值 | GC 频率 | 内存占用 | 适用场景 |
|--------|---------|----------|----------|
| **1.5**（小） | 高，GC 频繁 | 低，内存紧凑 | 内存受限环境 |
| **2**（当前值） | 适中 | 适中 | 通用平衡选择 |
| **4**（大） | 低，GC 很少 | 高，允许大量积累 | CPU 敏感、内存充裕 |

> 选择 **2** 的原因：与动态数组倍增策略一致，保证 GC 的摊还成本为 O(1)。

---

## 六、sweep 如何让 bytesAllocated 减小

sweep 并不直接修改 `bytesAllocated`，而是通过调用链间接完成：

```mermaid
flowchart TD
    A["sweep() 遍历对象链表"] --> B{"object->isMarked ?"}
    B -- "true (存活)" --> C["清除标记\nisMarked = false\n保留对象，继续遍历"]
    B -- "false (垃圾)" --> D["从链表中摘除"]
    D --> E["freeObject(unreached)"]
    E --> F["FREE(type, pointer)\n宏展开为 ↓"]
    F --> G["reallocate(ptr, sizeof(type), 0)\nnewSize = 0"]
    G --> H["bytesAllocated += 0 - oldSize\n自动减小!"]
    H --> I["free(pointer)\n真正释放内存"]
    C --> B
```

### 具体数字示例

```mermaid
flowchart TD
    subgraph GC前["GC 前: bytesAllocated = 1MB"]
        A1["对象A 200KB ✓ 存活"]
        A2["对象B 150KB ✓ 存活"]
        A3["对象C 250KB ✓ 存活"]
        A4["对象D 300KB ✗ 垃圾"]
        A5["对象E 100KB ✗ 垃圾"]
    end

    subgraph Sweep["sweep 逐个释放"]
        S1["释放对象D: 1MB - 300KB = 700KB"]
        S2["释放对象E: 700KB - 100KB = 600KB"]
        S1 --> S2
    end

    subgraph GC后["GC 后: bytesAllocated = 600KB"]
        B1["对象A 200KB"]
        B2["对象B 150KB"]
        B3["对象C 250KB"]
    end

    GC前 --> Sweep --> GC后

    GC后 --> Final["nextGC = 600KB × 2 = 1.2MB"]
```

---

## 七、总结

```mermaid
mindmap
  root((GC 内存管理))
    bytesAllocated
      实时记录托管内存总量
      每次 reallocate 自动更新
      分配时增加 释放时减少
      超过 nextGC 时触发 GC
    GC_HEAP_GROW_FACTOR
      值为 2 倍增因子
      GC 后设定新阈值
      nextGC = 存活内存 × 因子
      平衡 CPU 与内存开销
    协同工作
      bytesAllocated 是油表
      nextGC 是油箱容量线
      GROW_FACTOR 决定油箱大小
      自适应调度 摊还 O(1)
```

> **一句话总结**：`bytesAllocated` 是"油表"（告诉你用了多少内存），`GC_HEAP_GROW_FACTOR` 决定"油箱容量"（用到多满才去清理），两者配合实现了自适应的垃圾回收调度。
