# 前言

# 正文

先把上次剩下东西搞完，有个valgrind测试内存泄漏的东西没弄，这里安装弄一下。首先brew自己带的valgrind没法直接安装在mac 10.15.4 catalina上，在stackoverflow上找到了[解决方案](https://stackoverflow.com/questions/58360093/how-to-install-valgrind-on-macos-catalina-10-15-with-homebrew)，但是brew安装的时候用curl需要挂下代理，这里稍微在iterm2[里面配下代理](https://zhuanlan.zhihu.com/p/109131168)，然后proxy代理过后curl cip.cc看下有没有成功挂上代理，但是我傻逼了弄半天还是没发curl到raw.github，然后wget了下，发现直接域名解析就挂了，才想起来上次配公司内网的时候改了下dns，然后我把dns改回8.8.8.8就好了，最后

`brew install --HEAD https://raw.githubusercontent.com/LouisBrunner/valgrind-macos/master/valgrind.rb`

就成功安装上最新的valgrind,然后在clion里面按照cmu给的配置配一下valgrind就可以测试有没有内存泄漏了。

然后测了下有几个possible leak，上网搜了下感觉可能原因是指针移动操作导致valgrind无法分析处是否释放内存了，然后我找半天没分析好，，，等之后再接着看看来分析吧。

然后再分析修复下之前的逻辑。

## 去除多余的unpin

首先需要分析下在并发的情况下，我们需要关注的点，第一点就是lab里面给的常见错误：You have to release the latch on that page BEFORE you unpin the same page from the buffer pool。意思就是当某个page被unpin到0的时候他可能会被buffer pool回收掉，但是如果这个时候这个page还有锁的话就会出现问题，它被刷回buffer pool了，但是还被某个thread持有latch，就会导致这个thread错误，所以结论就是要求我们释放锁的时候pin一定不为0。

对此我们需要调整下之前的代码，来保证这种情况一定不会发生，首先我们需要移除多余的unpin操作，因为之前是利用在Unpin函数中判断<=0来直接返回，让其可以幂等操作，这里我们存在多线程的情况下就可能存在先unpin到0了再free lock的操作了，为了保证不会出现这种情况，我们首先修改下buffer_pool_manager里面的代码，先是Unpin函数

```c++
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  ...

  if (p->GetPinCount() <= 0) {
    cout << "DeletePage Error:" << p->page_id_ << endl;
    assert(false);
    return false;
  }

  ...
}
```

这里添加一个assert来确保不会发生，

然后在DeletePage中

```c++
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  ...
  if (p == nullptr) {
    disk_manager_->DeallocatePage(page_id);
  } else {
    if (p->GetPinCount() > 0) {   // if there's still thread hold this page, return false
      cout << "DeletePage Error in Delete func:" << p->page_id_ << endl;
      assert(false);
      return false;
    }
    ...
  }
  return true;
}
```

也加上一个assert来保证这里，单线程情况下必定是不会走`p->GetPinCount()>0的这个逻辑的`，但是多线程的情况下是允许出现的，然后我们接下来需要调整下之前Remove的代码，因为利用了幂等。

首先是b_plus_tree中的Remove函数，增加一个removeSucc，来判断是否最终的合并或者重分成功，如果成功了交给内层来unpin，只有在失败的时候才由最外层来unpin掉

```c++
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  if (IsEmpty()) {
    return;
  }
  B_PLUS_TREE_LEAF_PAGE_TYPE *tar = FindLeafPage(key);
  int curSize = tar->RemoveAndDeleteRecord(key, comparator_); // get the size after the deletion
  bool removeSucc = false;  //
  if (curSize
      < tar->GetMinSize()) {  // if the current size is smaller than min size, the page needs to be coalesce or redistribute
    removeSucc = CoalesceOrRedistribute(tar, transaction);
  }
  if (!removeSucc) {
    buffer_pool_manager_->UnpinPage(tar->GetPageId(), true);
  }

  assert(Check());
}
```

然后看下CoalesceOrRedistribute函数中，先跟进下AdjustRoot

```c++
if (old_root_node->GetSize()
    == 1) {  // case 1, if there is only one element left, get the page id from the element as the root page
	...
  buffer_pool_manager_->UnpinPage(root_page_id_, true);
  buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), false);
  buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
  return true;
}
```

在unpin后面加个return true返回给最外层来表示成功了。

然后接下来在合并Coalesce的时候我们也加个removeSucc来防治重复unpin

```c++
bool removeSucc = Coalesce(siblingNode, node, parentPage, removeIndex, transaction);  // judge if it success
if (!removeSucc) {
  buffer_pool_manager_->UnpinPage(parentPage->GetPageId(), true);
}
```

然后在Redistribute的时候我们把一个多余的unpin注释掉，这个node交给最外层来进行unpin

```c++
//buffer_pool_manager_->UnpinPage(node->GetPageId(), true); unpin this node in Remove function
buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
```

然后就可以跑通。

## 分析一下并行算法

### lock and latch

首先看一下lock和latch的定义

Locks:

- Protects the indexs logical contents from other transactions. 
- Held for (mostly) the entire duration of the transaction.
- The DBMS needs to be able to rollback changes.

Latches:

- Protects the critical sections of the indexs internal data structure from other threads. 
- Held for operation duration.
- The DBMS does not need to be able to rollback changes.
- Two Modes:
  - READ: Multiple threads are allowed to read the same item at the same time. A thread can acquire the read latch if another thread has it in read mode.
  - WRITE: Only one thread is allowed to access the item. A thread cannot acquire a write latch if another thread holds the latch in any mode.

然后关注下读写锁，说明了一个变量可以被多个线程拿读锁，但是不能被加写锁，或者拿了写锁后不能被再被读写锁拿了。

|       | Read | Write |
| ----- | ---- | ----- |
| Read  | Yes  | No    |
| Write | No   | No    |

### crabbing protool算法

然后是latch crabbing protocol算法，基本的思想是这样的

1. Get latch for parent.

2. Get latch for child.

3. Release latch for parent if it is deemed safe. A safe node is one that will not split or merge when

   updated (not full on insertion or more than half full on deletion).

这里关注下结点安全的定义，就是说这个结点更新后不会分类或者被合并。

然后这里的crabbing算法其实有两种的，basic和improve，这里我们用的是basic的算法，我们看下描述。

Basic Latch Crabbing Protocol:

- Search: Start at root and go down, repeatedly acquire latch on child and then unlatch parent.

- Insert/Delete: Start at root and go down, obtaining X latches as needed. Once child is latched, check

  if it is safe. If the child is safe, release latches on all its ancestors.

意思是read的时候，先lock parent page然后在lock children page，然后就可以释放parent page了；当insert或者delete的时候，同样是拿parent page的write latch,然后一路往下拿children page的write latch，只有当child是安全的，才释放它祖先的所有锁

然后我们的实现的代码类似：

```c++
/*
 * Helper methods for concurrent index
 */
bool BPlusTreePage::IsSafe(OpType op) {
  int size = GetSize();
  if (op == OpType::INSERT) {
    return size < GetMaxSize();
  }
  int minSize = GetMinSize() + 1;
  if (op == OpType::DELETE) {
    return (IsLeafPage()) ? size >= minSize : size > minSize;  // because internal page's first key is invalid
  }
  assert(false);
}
```

注意在删除的时候是有不同的,internal page的条件更严格点,因为internal page的第一个key是invalid的

### transaction是什么

然后我们有个transaction的东西，这个是做什么的呢，我们看下lab的描述

For this task, you have to use the passed in pointer parameter called `transaction` (`src/include/concurrency/transaction.h`). It provides methods to store the page on which you have acquired latch while traversing through B+ tree and also methods to store the page which you have deleted during `Remove` operation. Our suggestion is to look closely at the `FindLeafPage` method within B+ tree, you may wanna modify your previous implementation (note that you may need to change to **return value** for this method) and then add the logic of latch crabbing within this particular method.

重点是这句话：

It provides methods to store the page on which you have acquired latch while traversing through B+ tree and also methods to store the page which you have deleted during `Remove` operation. 

什么意思呢，就是transacition这里面维护了两个set，一个是被锁lock住的page，方便我最后处理去统一unlock，然后还有一个delete set来存储我们将要删除的page，具体实现类似

```c++
/*
 * 1.unlock the page in read operation
 * 2.delete the page in the delete set
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::FreePageInTransaction(bool exclusive, Transaction *transaction, page_id_t cur) {
  TryUnlockRootPageId(exclusive);
  if (transaction == nullptr) {
    assert(!exclusive && cur >= 0);  // make sure it's READ
    Unlock(false, cur);
    buffer_pool_manager_->UnpinPage(cur, false);
    return;
  }
  // delete the page stored in the transaction delete set (we delete the page together in here)
  for (Page *page : *transaction->GetPageSet()) {
    int curPid = page->GetPageId();
    Unlock(exclusive, page);
    buffer_pool_manager_->UnpinPage(curPid, exclusive);
    if (transaction->GetDeletedPageSet()->find(curPid) != transaction->GetDeletedPageSet()->end()) {
      buffer_pool_manager_->DeletePage(curPid);
      transaction->GetDeletedPageSet()->erase(curPid);
    }
  }
  assert(transaction->GetDeletedPageSet()->empty());
  transaction->GetPageSet()->clear();  // clear the set
}
```

### 保护root page id

然后还有我么需要加锁维护root page id, lab的描述的如下

- One of the corner case is that when insert and delete, the member variable **root_page_id** (`src/include/index/b_plus_tree.h`) will also be updated. It is your responsibility to protect from concurrent update of this shared variable(hint: add an abstract layer in B+ tree index, you can use std::mutex to protect this variable)

什么意思呢，就是在极端的情况下，我们的root page id可能会在insert或者delete的时候被更新掉（**本质上是因为我们这个时候只有root page id，没有fetch到Page这个结构，也就没发先直接给其加锁，所以需要单独来声明一个读写锁来保护，而对于之后的page,因为我们已经提前lock住了它的parent page就不会产生parent page被另一个线程改变的情况了**），那么就会增删时候发生找不到root page id的情况，所以我们需要加一个读写锁来保护，那么我们什么时候来加锁呢，只要涉及到root page是否会有改变，比如插入，删除，更新都需要加锁，那么我们看下哪里会发生这些情况，第一种情况是insert的时候，如果是空树会new root page所以需要加一下，然后对于其他的查找，删除，插入都是从FindLeafPage开始的，也就是说我们可以统一在FindLeafPage处进行加锁，然后当root page安全，也就是从transaction的set中被释放掉后，就可以说明其安全了。所以这里我们声明一个读写锁，还有一个线程局部存储的变量rootLockedCnt来统计某个单独的线程加锁的数量，这样做的目的是，防止其他线程unlock了root page的锁，因为我们的lock,unlock是判断rootLockedCnt>0说明root page被锁的，并且Try unlock是在FreePageInTransaction里面执行的，这个函数会执行多次所以要防止unlock了别的线程的root page的锁。

实现的代码类似：

```c++
inline void LockRootPageId(bool exclusive) {
  if (exclusive) {
    mutex_.WLock();
  } else {
    mutex_.RLock();
  }
  rootLockedCnt++;
}

inline void TryUnlockRootPageId(bool exclusive) {
  if (rootLockedCnt > 0) {
    if (exclusive) {
      mutex_.WUnlock();
    } else {
      mutex_.RUnlock();
    }
    rootLockedCnt--;
  }
}
```

声明了两个变量

```c++
RWMutex mutex_;
static thread_local int rootLockedCnt;
```

### 保护sibling node

还有一个要注意的点是在remove的合并coalesce的时候，我们需要保证sibling page也不会被改变，所以在取它的时候加写锁，然后送入transaction set中最后统一释放

### 保证iterator的线程安全

- Make sure the index iterator always perfroms thread-safe scans. A correct implementation would requires the Leaf Page to throw an `std::exception` when it cannot acquire a latch on its sibling to avoid potential dead-locks. However, for the project purpose, you can just use a regular Read Latch. For the test, we will not test your iterator on situations that can cause potential dead-locks.

我们看下怎么更改

现在先更改.h的添加一个函数

```c++
void UnlockAndUnPin() {
  bufferPoolManager_->FetchPage(leaf_->GetPageId())->RUnlatch();
  bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
  bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
}
```

用来unlock read latch，然后更改++

```c++
IndexIterator &operator++() {
  index_++;
  if (index_ >= leaf_->GetSize()) {
    page_id_t next = leaf_->GetNextPageId();
    UnlockAndUnPin();  // release read latch and then unpin the page
    if (next == INVALID_PAGE_ID) {
      leaf_ = nullptr;
    } else {
      //bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
      Page *page = bufferPoolManager_->FetchPage(next);
      page->RLatch();  // remember to get the read latch
      leaf_ = reinterpret_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(page->GetData());
      index_ = 0;
    }
  }
  return *this;
}
```

然后在.cpp中的析构函数，

```c++
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::~IndexIterator() {
  if (leaf_ != nullptr) {
    UnlockAndUnPin();  // remember to release the read latch first
  }
}
```

## 总结下整个思路

首先我们声明一个Lock和Unlock操作，统一加解锁，再声明一个LockRootPageId和TryUnlockRootPageId作为特殊情况对待root page的加解锁。然后将所有的unpin page和delete page操作全部统一推迟，利用transaction进行管理（除了临时fetch的parent page），Insert的开头LockRootPageId，然后改写FindLeafPage同样LockRootPageId，然后换成调用CrabingProtocalFetchPage来带锁拿page。所有的lock操作都在CrabingProtocalFetchPage，所有的unlock,delete,unpin操作都在FreePageInTransaction中。还有记得调整下iterator。

```c++
/*
 * use basic crabing protocol
 */
INDEX_TEMPLATE_ARGUMENTS
BPlusTreePage *BPLUSTREE_TYPE::CrabingProtocalFetchPage(page_id_t page_id,
                                                        OpType op,
                                                        page_id_t previous,
                                                        Transaction *transaction) {
  bool exclusive = (op != OpType::READ);
  auto page = buffer_pool_manager_->FetchPage(page_id);
  Lock(exclusive, page);
  auto treePage = reinterpret_cast<BPlusTreePage *>(page->GetData());
  /*
   * in here, we use basic crabbing protocol
   * Search: Start at root and go down, repeatedly acquire latch on child and then unlatch parent.
   * Insert/Delete: Start at root and go down, obtaining X latches as needed. Once child is latched, check
   * if it is safe. If the child is safe, release latches on all its ancestors.
   */
  if (previous > 0 && (!exclusive || treePage->IsSafe(op))) {
    FreePageInTransaction(exclusive, transaction, previous);
  }
  if (transaction != nullptr) {
    transaction->AddIntoPageSet(page);
  }
  return treePage;
}

/*
 * 1.unlock the page in read operation
 * 2.delete the page in the delete set
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::FreePageInTransaction(bool exclusive, Transaction *transaction, page_id_t cur) {
  TryUnlockRootPageId(exclusive);
  if (transaction == nullptr) {
    assert(!exclusive && cur >= 0);  // make sure it's READ
    Unlock(false, cur);
    buffer_pool_manager_->UnpinPage(cur, false);
    return;
  }
  // delete the page stored in the transaction delete set (we delete the page together in here)
  for (Page *page : *transaction->GetPageSet()) {
    int curPid = page->GetPageId();
    Unlock(exclusive, page);
    buffer_pool_manager_->UnpinPage(curPid, exclusive);
    if (transaction->GetDeletedPageSet()->find(curPid) != transaction->GetDeletedPageSet()->end()) {
      buffer_pool_manager_->DeletePage(curPid);
      transaction->GetDeletedPageSet()->erase(curPid);
    }
  }
  assert(transaction->GetDeletedPageSet()->empty());
  transaction->GetPageSet()->clear();  // clear the set
}
```

最后看下哪里会调用CrabingProtocalFetchPage，一个是在FindLeafPage中，一个是在FindSibling中，然后FreePageInTransaction是在read,insert,remove都有，还有在CrabingProtocalFetchPage中。

最后记得把几个assert去掉，因为允许并行，所以可能page不会全部unpin掉，最后跑完所有的并行测试，然后一个循环测试脚本。

```sh
#!/usr/bin/env bash
for ((i=0;i<1000;i++));
do
  if !(./cmake-build-debug/test/b_plus_tree_concurrent_test &> ./res/$i); then
    exit
  else
    echo $i;
  fi
done
```

成功跑通测试

<img src="http://image.bdwms.com/Fn-xEcKwT9AJtvuWNL5ohloOyMk-" style="zoom:50%;" />

# 总结

最后的总结分析，还有测试样例放在后买呢进行分析，终于糊完了。。。