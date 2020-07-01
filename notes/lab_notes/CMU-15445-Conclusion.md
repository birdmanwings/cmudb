# 前言

终于大概结束了CMU数据库的学习，感觉收获颇丰（尤其是感觉学习了一波c++，c++可太难了orz），这里我们做个大致的总结和对前面遗漏的点的补充。

# 正文

## 一些遗留问题

### LRU中的析构函数

```c++
template<typename T>
LRUReplacer<T>::~LRUReplacer() {
  // 这里需要回收，否则会导致内存泄漏，因为虽然使用了智能指针，但是我们使用的双向链表会导致循环引用的问题导致一直不能释放内存
  // 可以参考一下这篇文章，http://senlinzhan.github.io/2015/04/24/%E6%B7%B1%E5%85%A5shared-ptr/
  while (head) {
    shared_ptr<Node> tmp = head->next;
    head->next = nullptr;
    head = tmp;
  }

  while (tail) {
    shared_ptr<Node> tmp = tail->prev;
    tail->prev = nullptr;
    tail = tmp;
  }
}
```

我们虽然使用了智能指针来维护我们的结点但是并不能自动释放，因为我们采用的是双向链表的结构，所以回有一个环导致循环引用存在，所以必须打断链表的连接才能自动回收，具体的看这篇[文章](http://senlinzhan.github.io/2015/04/24/%E6%B7%B1%E5%85%A5shared-ptr/)，

### Slotted page是什么

动态长度的page，黑皮书上说动态tuple那张说的很清楚，header维护了page信息，freespace指针和tuple的偏移量和大小，后面是存放tuple，中间是free space，倒着存储数据，插入更新删除的时候要调整tuple位置和header中关于tuple的信息，这里有个mark delete是将tuple的size变负来标记是要删除的（方便undo），apply delete才是确切删除

```c++
/**
 * table_page.h
 *
 * Slotted page format:
 *  ---------------------------------------
 * | HEADER | ... FREE SPACES ... | TUPLES |
 *  ---------------------------------------
 *                                 ^
 *                         free space pointer
 *
 *  Header format (size in byte):
 *  --------------------------------------------------------------------------
 * | PageId (4)| LSN (4)| PrevPageId (4)| NextPageId (4)| FreeSpacePointer(4) |
 *  --------------------------------------------------------------------------
 *  --------------------------------------------------------------
 * | TupleCount (4) | Tuple_1 offset (4) | Tuple_1 size (4) | ... |
 *  --------------------------------------------------------------
 *
 */
```

## 整体架构

我们四个Project下来，完成了一个为sqlite提供的面向磁盘的存储管理器，向上层次的sql语句的处理转化我们没有涉及，向下封装了disk磁盘的操作，我们只要关注缓冲区管理，索引管理，事务并发锁管理，以及数据库日志管理四个东西。

### 缓冲池管理

我们先来看下缓冲池的架构，缓冲池主要负责将物理Page从硬盘到内存中，我们首先实现了一个extendible hash table可拓展hash进行从page id到Page指针的映射关系，然后实现了个基于LRU的replacer用来选取要替换的Page。

我们看下buffer pool维护了什么，一个基于extendible hash的page table来负责page id到具体page的对应，一个free list来提供替换，一个lru replacer来选择替换的page，我们每次都是先从free list中先拿没有的话才从replacer选择要驱逐的page，然后要将dirty的page写回时，开启日志的情况下要刷新日志回disk中

<img src="http://image.bdwms.com/Fs5I75DpPkfPAAiWYCSkdyKFvhOl" style="zoom:67%;" />

整个的架构图类似上面，page table就是extendible hash table维护的page id到buffer pool中具体的page，然后buffer pool中蓝色的就是已经有对应的物理页的，空白的就是free page，然后我们假设page1,page3没有线程pin住它那么这些page应该被插入到LRUreplacer中，整个的架构就类似这样。

### B+树索引管理

我们实现了基于buffer pool manager的支持并发的B+树，其中B+树的具体分析我们不在这里赘述了，前面已经有总结了，这里大概看下他的简单架构，是分为leaf page和internal page其中分别维护了各自page上需要实现的方法，然后同一个b_plus_tree.cpp文件，其中维护了find, insert, delete三个主要操作，然后我们使用crabbing latch算法来保证我么B+树的并发支持，然后b+树利用buffer pool来获取管理page

### 锁管理器

为了保证事务中操作的正确交叉，我们实现了一个基于strict 2PL的lock manager，支持的级别是tuple级别的，支持共享锁，互斥锁的管理以及共享锁升级为互斥锁的upgrade操作，我们的lock manager只支持到**REPEATABLE READ**级别，这也是mysql的默认级别，能够给操作的行都上锁，但是没有表锁，所以也就不能避免幻读的现象，也就是说不支持**SERIALIZABLE**级别的隔离，那么我们会在哪些地方进行锁操作呢？主要就是在table_page.cpp中对tuple做操作的时候会调用lock manager，以及在table_heap.cpp中的对tuple的操作。

### 日志管理器

最后我们实现了一个简单的基于ARIES的lock manager，wal的每次日志先落盘，然后group commit设置一个time out或者log buffer满了，或者强制flush才将log刷到disk上提高性能，然后我们又实现了redo和undo但是我们没有实现checkpoint机制，所以就没有分析阶段了所以redo是从头开始读log就行恢复，然后我们假定undo不会失败所以就没有CLR记录我们undo到哪里了，然后我们也就没必要倒着来undo了，直接把ATT记录中log循环undo掉就行了，最后我们需要给那些加log的地方添上log的操作，一个是bufferpool的地方fetch和new page的时候会将dirty page刷回disk中所以要求强制flush log让其先落盘，然后对弈事务的begin,abort, commit阶段都需要龙记录，以及对txn的各种操作也要加log进行操作。

## 一次完整的数据库查询的过程

这里我只提个大概，来大概总览下我们已经学习的和没有涉及的，我们看个总览图

![](http://image.bdwms.com/FljvV_xIZBWxBzFLlz3nIk35ACwn)

核心组件

- **进程管理器**：很多数据库都需要管理**进程池或线程池**。而且，为了改进那么几纳秒，一些现代数据库还会使用它们自己的线程，而不是操作系统提供的线程。
- **网络管理器**：网络 I/O 是个大问题，尤其对于分布式数据库。这也就是为什么有些数据库会有它们自己的网络管理器。
- **文件系统管理器**：**磁盘 I/O 是数据库的最大瓶颈**。因此，有一个能够完美地处理操作系统文件系统，甚至取代操作系统文件系统的文件系统管理器就变得非常重要。
- **内存管理器**：为了避免大量磁盘 I/O，大容量内存必不可少。但是，如果你有很大数量的内存，你就需要一个高效的内存管理器。尤其是当你在同一时间有多个查询时。
- **安全管理器**：管理用户的认证和授权。
- **客户端管理器**：管理客户端连接。
- …

工具

- **备份管理器**：保存和恢复数据库。
- **恢复管理器**：在数据库崩溃之后将其重启到一个**一致性状态**。
- **监控管理器**：记录数据库动作日志，提供工具监控数据库。
- **管理管理器**：保存元数据（比如表的名字和结构等），提供工具管理数据库、模式和表空间等。
- …

查询管理器

- **查询处理器**：检查查询语句属否合法。
- **查询重写器**：为查询语句预优化。
- **查询优化器**：优化查询语句。
- **查询执行器**：编译并执行查询语句。

数据管理器

- **事务管理器**：处理事务。
- **缓存管理器**：在使用数据之前或将数据写入磁盘之前，将数据放到内存中。
- **数据访问管理器**：访问磁盘上的数据。

我们没有涉及到的是查询管理这部分，

- 首先，查询语句会被**解析（parse）**，检查是不是合法。
- 然后，查询语句会被**重写（rewrite）**，移除没用的操作，增加一些预先的优化。
- 然后，查询语句会被**优化（optimize）**，改进性能，转换成一个执行和数据访问计划。
- 然后，这个计划会被**编译（compile）**，
- 最后，这个计划会**被执行（execute）**。

之后的就到了我们的事务管理部分，然后继续是贴近到存储管理即缓冲池，索引那些，最后的磁盘交互我们也没有做具体分析学习了。总的来说本次的数据库学习让我们基本了解了关系型数据库需要知道的大部分知识，收获颇丰。

# 总结

整个CMU数据库的学习历经了将近三个月的时间也告一段落了，收获很多，之后可能会写一篇完整执行数据库查询的文章，今天就到这里吧。

加油，努力！