# 前言

Project3中我们需要实现的是基于二阶段锁协议的Lock manager，然后为了解决死锁的问题采用的是死锁预防的方法，采用wait-die模式。

# 正文

首先我们需要了解下二阶段锁协议，具体的数学证明我们暂且放下不看，介绍看[这篇文章](https://www.cnblogs.com/gatsby123/p/10800089.html)。二阶段锁协议就是为了解决我们什么时候加锁，什么时候解锁，但是容易产生级连崩溃，所以我们又有了严格二阶段锁协议，也就是只有当事务提交或者放弃的时候才能放弃锁，但是二阶段锁协议不能够解决死锁的问题，所以我们需要有死锁预防或者死锁检测来解决，这里我们使用的是wait-die模式的死锁预防，也就是高优先级的（老的）事务想要获取被低优先级事物占有的锁时，采用等待的策略，反之则直接放弃本次事务。

然后对于本次Project的话我们需要实现的是一个锁管理器即Lock Manager，我们看下他的大概结构，

![](http://image.bdwms.com/Fij1Lbs60WCH9-W8C_bnuE7VZiJv)

首先是一个一个rid为key来对应一个list，这个list维护了将在这个rid上操作的事务txn以及其状态组成的txItem，当一个请求到来时，如果请求的数据项当前没有任何事务访问，那么创建一个空队列，将当前请求直接放入其中，授权通过。如果不是第一个请求，那么将当前事务加入队列，只有当前请求之前的请求和当前请求兼容（这个兼容的定义我们放在后面说），才授权，否则等待。

然后我们会在什么地方调用呢？page/table_page.cpp中的TablePage类用于插入，删除，更新，查找表记录。在执行插入，删除，查找前都会获取相应的锁，确保多个事务同时操作相同数据项是安全的。

## lock_manager.h

我们先看下lock_manager.h中，先给一些定义和基本操作：

先是TxItem

```c++
struct TxItem {
  TxItem(txn_id_t tid, LockMode mode, bool granted) : tid_(tid), mode_(mode), granted_(granted) {}

  void Wait() {
    // we use condition_varialbe in each txn, so we just use notify_one to wake up this txn alone
    unique_lock<mutex> ul(mutex_);
    cv_.wait(ul, [this] {
      return this->granted_;
    });
  }

  void Grant() {
    lock_guard<mutex> lg(mutex_);
    granted_ = true;
    cv_.notify_one();
  }

  mutex mutex_;
  condition_variable cv_;
  txn_id_t tid_;
  LockMode mode_;
  bool granted_;
};
```

里面包含了一些关于这个事务txn的基本信息，tid是事务的编号，注意这里我们的事务编号是递增的，也就是说编号越小说明事务越老，也就是优先级越高

```c++
Transaction *TransactionManager::Begin() {
  Transaction *txn = new Transaction(next_txn_id_++);

  if (ENABLE_LOGGING) {
    // TODO: write log and update transaction's prev_lsn here
  }

  return txn;
}
```

然后mode_是三种模式，共享锁shared，互斥锁exclusive，升级共享锁为互斥锁upgrading，是否被授予锁granted，最后当事务因为不满足获取锁的条件而挂起等待的时候，我们需要在条件满足的时候去通知他，所以我们这里采用条件变量condition_variable，然后cv是需要一个锁mutex。

说完了成员变量，我们来看下两个操作,wait就是，只有当当前txn被granted后才可能被唤醒，

```c++
void Wait() {
    // we use condition_varialbe in each txn, so we just use notify_one to wake up this txn alone
    unique_lock<mutex> ul(mutex_);
    cv_.wait(ul, [this] {
      return this->granted_;
    });
  }
```

然后因为我们一个事务只能被wait一次所以直接notify_one就足够了（note the behavior of trying to lock locked rids by same txn is undefined）

```c++
void Grant() {
  lock_guard<mutex> lg(mutex_);
  granted_ = true;
  cv_.notify_one();
}
```

我们再看下TxList的定义和操作

```c++
struct TxList {
  mutex mutex_;
  list<TxItem> locks_;
  bool hasUpgrading_;

  bool checkCanGrant(LockMode mode) {
    // when will the txn will be granted ?
    // 1. the txList is empty
    // 2. the exclusive will be granted only when it is at the first position
    // 3. the shared will be granted when all the txn that is before it is granted shared
    //    because if there is a exclusive or upgrade before it, the shared can't be granted
    // 4. the upgrading will be granted when it is the only granted upgrading, that means
    //    it should be at the first position and it's shared originally,
    //    otherwise, it can't be upgraded and it will be sent into exclusive set in insert function
    if (locks_.empty()) {
      return true;
    }
    const auto &last = locks_.back();
    if (mode == LockMode::SHARED) {
      return last.granted_ && last.mode_ == LockMode::SHARED;
    }
    return false;
  }

  void insert(Transaction *txn, const RID &rid, LockMode mode, bool granted, unique_lock<mutex> *lock) {
    bool upgradingMode = (mode == LockMode::UPGRADING);
    if (upgradingMode && granted) {  // if it's upgrading and granted, it can be upgraded to EXCLUSIVE
      mode = LockMode::EXCLUSIVE;
    }
    locks_.emplace_back(txn->GetTransactionId(), mode, granted);
    auto &last = locks_.back();
    if (!granted) {
      hasUpgrading_ |= upgradingMode;  // idempotent to record if there is a txn that need upgrading is waiting
      lock->unlock();
      last.Wait();  // wait on the conditional variable
    }
    if (mode == LockMode::SHARED) {
      txn->GetSharedLockSet()->insert(rid);
    } else {
      txn->GetExclusiveLockSet()->insert(rid);  // Exclusive and upgrade will be inserted into this set
    }
  }
};
```

这里我们维护了一个TxItem的list，一个list级别的锁，一个hasUpgrading_来记录是否已经有待升级的txn，重点是其中的两个操作我们来仔细分析一下两个函数

checkCanGrant用来判断当前的txn是否可以被granted，这里是最重要的逻辑和复杂的，我们一个一个分析，

1. 当前的list是空的话，无论是s还是e锁都可以直接被granted
2. e锁想要被granted只有当它是第一个的时候
3. 当s锁被granted后，之后可以继续申请s锁并且被granted，但是e锁必然是无法被granted的，所以会被挂起，从这之后的r锁也无法g了，因为前面有个e锁在等待不可能插队么，否则e锁一直不被granted
4. s锁想要升级为e锁的话，前提条件也是必须为第一个才能被granted（因为是被升级为e锁）也就是满许的是locks.empty这个条件，否则的话也要挂起，并且将hasUpgrading标记为true，并将txn转到exclusive_lock_set_中

基于以上四点我们基本就可以理清逻辑了

```c++
bool checkCanGrant(LockMode mode) {
  // when will the txn will be granted ?
  // 1. the txList is empty
  // 2. the exclusive will be granted only when it is at the first position
  // 3. the shared will be granted when all the txn that is before it is granted shared
  //    because if there is a exclusive or upgrade before it, the shared can't be granted
  // 4. the upgrading will be granted when it is the only granted upgrading, that means
  //    it should be at the first position and it's shared originally,
  //    otherwise, it can't be upgraded and it will be sent into exclusive set in insert function
  if (locks_.empty()) {
    return true;
  }
  const auto &last = locks_.back();
  if (mode == LockMode::SHARED) {
    return last.granted_ && last.mode_ == LockMode::SHARED;
  }
  return false;
}
```

我们来看下inset的逻辑，如果是upgrading并且granted的话就可以直接转换为exclusive，然后加入TxList中，然后如果没有被grant锁那么我们就要把他挂起，然后我们将其rid加入相应的set

```c++
void insert(Transaction *txn, const RID &rid, LockMode mode, bool granted, unique_lock<mutex> *lock) {
  bool upgradingMode = (mode == LockMode::UPGRADING);
  if (upgradingMode && granted) {  // if it's upgrading and granted, it can be upgraded to EXCLUSIVE
    mode = LockMode::EXCLUSIVE;
  }
  locks_.emplace_back(txn->GetTransactionId(), mode, granted);
  auto &last = locks_.back();
  if (!granted) {
    hasUpgrading_ |= upgradingMode;  // idempotent to record if there is a txn that need upgrading is waiting
    lock->unlock();
    last.Wait();  // wait on the conditional variable
  }
  if (mode == LockMode::SHARED) {
    txn->GetSharedLockSet()->insert(rid);
  } else {
    txn->GetExclusiveLockSet()->insert(rid);  // Exclusive and upgrade will be inserted into this set
  }
}
```

## lock_manager.cpp

然后看一下cpp文件中，LockShared,Exclusive,Upgrading都统一到lockTemplate中，我们看下lockTemplate

```c++
bool LockManager::lockTemplate(Transaction *txn, const RID &rid, LockMode mode) {
  // if txn is in growing and it tries to lock, abort this schedule and return false
  if (txn->GetState() != TransactionState::GROWING) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }

  // get the txn list according to rid, remember to use the latch
  unique_lock<mutex> tableLatch(mutex_);
  TxList &txList = lockTable_[rid];
  unique_lock<mutex> txListLatch(txList.mutex_);
  tableLatch.unlock();

  // if the mode is upgrading, we need delete the origin read latch and then we can update the latch to write
  if (mode == LockMode::UPGRADING) {
    if (txList.hasUpgrading_) {
      txn->SetState(TransactionState::ABORTED);
      return false;
    }
    // get the tx item that tid equal to the txn
    auto it = find_if(txList.locks_.begin(),
                      txList.locks_.end(),
                      [txn](const TxItem &item) { return item.tid_ == txn->GetTransactionId(); });
    // if not found or it exclusive/upgrading or it isn't granted
    // (if it isn't granted that means there is at least a eclusive latch is before it)
    if (it == txList.locks_.end() || it->mode_ != LockMode::SHARED || !it->granted_) {
      txn->SetState(TransactionState::ABORTED);
      return false;
    }
    txList.locks_.erase(it);
    assert(txn->GetSharedLockSet()->erase(rid) == 1);
  }

  // judge if it can be granted
  bool canGrant = txList.checkCanGrant(mode);
  
  // try to insert the txn into txlist
  txList.insert(txn, rid, mode, canGrant, &txListLatch);
  return true;
}
```

首先我们同样是需要判断只有在growing的时候才能上锁，否则就abort，然后我们先用表锁来锁住整表，来拿到rid对应的TxList，然后上行锁后才能放掉表锁，非Upgrading的情况的话直接判断下是否可以granted也就是我们上面所说的，然后将这个txn插入TxList中，同样类似上文提及的，然后看下Upgrading的情况，我们首先判断下是否已经存在等待的Upgrading的txn，有的话直接abort，否则我们找到对应txn，`it == txList.locks_.end() || it->mode_ != LockMode::SHARED || !it->granted_`这个条件的意思是如果没找懂，或者这个要升级的txn原来不是shared，或者它没有granted（这个说明前面有e锁的事务在它前面肯定拿不到granted），如果成功的话，就将这个txn先从txList抹除，再从shared_lock_set中抹除，然后接下来的步骤和前面的一样

然后我们看下Unlock操作

```c++
bool LockManager::Unlock(Transaction *txn, const RID &rid) {
  // in the strict 2pl, we can only unlock when the txn is in committed or abort
  if (strict_2PL_) {
    if (txn->GetState() != TransactionState::COMMITTED && txn->GetState() != TransactionState::ABORTED) {
      txn->SetState(TransactionState::ABORTED);
      return false;
    }
  } else if (txn->GetState()
      == TransactionState::GROWING) { // if txn is growing, so we need ajust the state to the shrinking
    txn->SetState(TransactionState::SHRINKING);
  }

  // get the txList according to the rid
  unique_lock<mutex> tableLatch(mutex_);
  TxList &txList = lockTable_[rid];
  unique_lock<mutex> txListLatch(txList.mutex_);

  // find the item in the txList and get the lock set to erase and if the txList is empty, we can delete this txList
  // remember to hold the table latch, because we may to delete the txList
  auto it = find_if(txList.locks_.begin(), txList.locks_.end(), [txn](const TxItem &item) {
    return item.tid_ == txn->GetTransactionId();
  });
  assert(it != txList.locks_.end());
  auto lockSet = it->mode_ == LockMode::SHARED ? txn->GetSharedLockSet() : txn->GetExclusiveLockSet();
  assert(lockSet->erase(rid) == 1);
  txList.locks_.erase(it);
  if (txList.locks_.empty()) {
    lockTable_.erase(rid);
    return true;
  }
  tableLatch.unlock();

  // check whether it can grant other
  for (auto &tx : txList.locks_) {
    if (tx.granted_) {
      break;
    }
    tx.Grant();
    if (tx.mode_ == LockMode::SHARED) {
      // loop execute to grant the blocked shared
      continue;
    }
    if (tx.mode_ == LockMode::UPGRADING) {
      // update "hasUpgrading" to false and then turn this txn to exclusive
      txList.hasUpgrading_ = false;
      tx.mode_ = LockMode::EXCLUSIVE;
    }
    break;
  }

  return true;
}
```

同样是先检测是否是严格二阶段锁，然后用表锁拿到TxList再上行锁，注意这里先不放掉表锁，因为后面可能会删除这个TxList，找到这个TxItem然后删除它，判断TxList是否为空，为空就可以删除这个list，至此可以释放掉表锁，接下来我们需要开始便利Txlist来寻找是否可以。

之后我们需要实现下wait-die的死锁预测，比较简单直接根据txn_id判断优先级，id越小则越老，权重越大

```c++
bool canGrant = txList.checkCanGrant(mode);
if (!canGrant && txList.locks_.back().tid_ < txn->GetTransactionId()) {
  txn->SetState(TransactionState::ABORTED);
  return false;
}
```

这个什么意思呢，就是当前txn无法拿到锁时，只有此txn的优先级更高即id更小时，才等待，否则直接放弃。（因为wait-die机制只允许时间戳小的等待时间戳大的事务，也就是说在wait-for graph中任意一条边Ti->Tj，Ti的时间戳都小于Tj，显然不可能出现环。所以不会出现环，也就不可能出现死锁。）

然后跑了1000次测试通过

# 总结

之后的一些test和具体调用分析会放在后面进行，今天先到这里吧。