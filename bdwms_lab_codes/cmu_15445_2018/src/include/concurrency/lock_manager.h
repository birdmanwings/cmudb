/**
 * lock_manager.h
 *
 * Tuple level lock manager, use wait-die to prevent deadlocks
 */

#pragma once

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/rid.h"
#include "concurrency/transaction.h"
using namespace std;
namespace cmudb {

enum class LockMode { SHARED = 0, EXCLUSIVE, UPGRADING };

class LockManager {
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

 public:
  LockManager(bool strict_2PL) : strict_2PL_(strict_2PL) {};

  /*** below are APIs need to implement ***/
  // lock:
  // return false if transaction is aborted
  // it should be blocked on waiting and should return true when granted
  // note the behavior of trying to lock locked rids by same txn is undefined
  // it is transaction's job to keep track of its current locks
  bool LockShared(Transaction *txn, const RID &rid);
  bool LockExclusive(Transaction *txn, const RID &rid);
  bool LockUpgrade(Transaction *txn, const RID &rid);

  // unlock:
  // release the lock hold by the txn
  bool Unlock(Transaction *txn, const RID &rid);
  /*** END OF APIs ***/

 private:
  bool lockTemplate(Transaction *txn, const RID &rid, LockMode mode);

  bool strict_2PL_;
  mutex mutex_;
  unordered_map<RID, TxList> lockTable_;
};

} // namespace cmudb
