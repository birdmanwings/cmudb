# 前言

B+树终于糊完了，支持basic crabbing protocol来做并发控制，感觉做的真的浑身酥爽，这里再总结下，把前面遗留的问题和测试用例大概说一下。

# 正文

## 一些遗留问题

都在前面的blog里面进行了更新，这里放在这里统一一下

### GetMinSize

```c++
int BPlusTreePage::GetMinSize() const {
  if (IsRootPage() && IsLeafPage()) {
    return 1;
  }
  return IsRootPage() ? 2 : (GetMaxSize() + 1) / 2;
}
```

对于只有一个结点的树，如果该结点的size小于1即为0的时候需要调整header page，就需要回收这个root page，然后他是根结点并且不是leaf page的话，那么应该返回2，因为只有一个leaf page的话需要将leaf page调整为root page（这里的逻辑在后面的部分会实现，具体看代码部分），即一个左指针一个右指针，否则就返回`(GetMaxSize() + 1) / 2`，小于min的结点需要merge。

### sizeof()

```c++
SetMaxSize((PAGE_SIZE - sizeof(BPlusTreeInternalPage)) / sizeof(MappingType) - 1);
```

对于C++中的sizeof

1.类的大小为类的非静态成员数据的类型大小之和，也就是说静态成员数据不作考虑。

2.普通成员函数与sizeof无关。

3.虚函数由于要维护在虚函数表，所以要占据一个指针大小，也就是4字节。

4.类的总大小也遵守类似class字节对齐的，调整规则。

internal page和leaf page都是继承自基类page，page中维护了header的信息，然后internal page和leaf page中也各自有几个变量

这里还有个讲内存对齐比较清楚的文章，贴一下https://blog.csdn.net/hairetz/article/details/4084088

### leftMost

在FindLeafPage中有一个leftMost变量，这个是干什么的呢

```c++
INDEX_TEMPLATE_ARGUMENTS
B_PLUS_TREE_LEAF_PAGE_TYPE *BPLUSTREE_TYPE::FindLeafPage(const KeyType &key,
                                                         bool leftMost) {
  if (IsEmpty()) {
    return nullptr;
  }
  auto pointer = FetchPage(root_page_id_);
  page_id_t next;
  for (page_id_t cur = root_page_id_; !pointer->IsLeafPage(); cur = next, pointer = FetchPage(cur)) {
    B_PLUS_TREE_INTERNAL_PAGE *internalPage = static_cast<B_PLUS_TREE_INTERNAL_PAGE *>(pointer); // benign conversion
    if (leftMost) {
      next = internalPage->ValueAt(0);
    } else {
      next = internalPage->Lookup(key, comparator_);
    }
    buffer_pool_manager_->UnpinPage(cur, false);
  }
  return static_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(pointer);
}
```

leftMost是说明leaf page从最开始的地方，因为这是个迭代器类似，利用到的函数

```c++
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::Begin() {
  KeyType useless;
  auto start_leaf = FindLeafPage(useless, true);
  return INDEXITERATOR_TYPE(start_leaf, 0, buffer_pool_manager_);
}
```

### 如何做Check

根据B+树的几个特性：

- Minsize <= Size <= MaxSize
- Leaf page的元素有序且唯一，internal page左子树最大元素小于等于internal的相应的元素，右子树最小的元素大于等于internal的相应元素 
- 每条路径深度相等，也就是B+树平衡的
- 所有操作完成后所有的Page都应该pin为0（这个是单线程的情况下，后面并行的情况下不一定要求这样）
- 在并发的情况下，一定要保证先unlock再unpin，否则unpin到0但是还持有锁的话，可能会被flush回buffer pool然后被另外一个线程fetch到，但是这个时候还有锁，就会产生错误

Check函数以及一些其他的assert就是类似这样，这里不展开了，直接看代码

## b_plus_tree_page_test.cpp

首先是page test的用例，有两个分别测试internal page和leaf page。方式都差不多，都是先调用提供的sqlite接口和我们lab1中完成的buffer pool的接口

然后分别测试两个page中维护的几个方法，比较简单不再赘述。

## b_plus_tree_insert_test.cpp

测试b+树的insert，先是test1，测试了Insert和GetValue，还有iterator的++，test2类似1换了个顺序。

test3是大规模的测试插入1000个page，test4是反过来的插入1000个page.

最后一个test5是随机插入，注意下random_shuffle已经被抛弃了，这里我换用了

```c++
unsigned seed = chrono::system_clock::now().time_since_epoch().count();
shuffle(keys.begin(), keys.end(), default_random_engine(seed));
```

因为那个random_shuffle是播种子好像是要用个全局变量不利于并发的被抛弃了，这里用系统时间作为种子进行播种

## b_plus_tree_delete_test.cpp

前三个test就是前面insert后添加了remove，然后几个测试例子不同，test4是放大规模的，test5是随机的跟前面类似不做重复了

## b_plus_tree.cpp

ScaleTest大量测试Insert,GetValue,Remove和iterator，RandomTest随机测试b+树的三个功能和最后检查iterator

## b_plus_tree_concurrent_test.cpp

首先是几个辅助函数InsertHelper,InsertAndGetHelper,IterateHelper,InsertHelperSplit,DeleteHelper,DeleteAndGetHelper,DeleteHelperSplit

```c++
template <typename... Args>
void LaunchParallelTest(uint64_t num_threads, Args &&... args) {
  std::vector<std::thread> thread_group;

  // Launch a group of threads
  for (uint64_t thread_itr = 0; thread_itr < num_threads; ++thread_itr) {
    thread_group.push_back(std::thread(args..., thread_itr));
  }

  // Join the threads with the main thread
  for (uint64_t thread_itr = 0; thread_itr < num_threads; ++thread_itr) {
    thread_group[thread_itr].join();
  }
}
```

这里只看LaunchParallelTest使用变长参数的右值引用传参数，具体调用类似

```c++
LaunchParallelTest(4, InsertHelper, std::ref(tree), keys);
```

可以看到我们传递std:ref(tree)，（std::ref只是尝试模拟引用传递，并不能真正变成引用，在非模板情况下，std::ref根本没法实现引用传递，只有模板自动推导类型时，ref能用包装类型reference_wrapper来代替原本会被识别的值类型，而reference_wrapper能隐式转换为被引用的值的引用类型，但是并不能被用作`&`类型。thread的方法传递引用的时候，**我们希望使用的是参数的引用，而不是浅拷贝，所以必须用ref来进行引用传递**。）

然后test1是个简单的并行插入，多个线程共用一个keys来插入，理论上存在重复插入就返回，最后检查是否插入正确

test2是调用分裂插入InsertHelperSplit，将对应的key交给对应的thread来插入，最后测试

test3是InsertAndGetHelper插入后并查找，来测试并发下的插入与查找会不会出现错误读。

然后test4是DeleteTest1就是并发删除

test5是DeleteAndGetTest，测试Remove和GetValue

test6是DeleteTest2，DeleteHelperSplit分开线程删除

test7是DeleteTest3，是并发删除部分key

test8是DeleteTest4，是并发分开删除部分key

test9是DeleteTest5，是随机打乱key，并发分开删除部分key

test10是MixTest，是并发插入，并发删除

test11是MixTest2，是并发分开插入，并发分开删除

test12是MixTest3，是大量随机，多种并发插入删除混合在一起。

## b_plus_tree_print_test.cpp

可对b+树操作的一个test，就不具体分析了

# 总结

终于把B+树弄完了，学习到了超级多东西，继续加油研究数据库吧。