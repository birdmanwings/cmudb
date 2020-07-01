/**
 * lock_manager.cpp
 */

#include "concurrency/lock_manager.h"
using namespace std;

namespace cmudb {

bool LockManager::LockShared(Transaction *txn, const RID &rid) {
  return lockTemplate(txn, rid, LockMode::SHARED);
}

bool LockManager::LockExclusive(Transaction *txn, const RID &rid) {
  return lockTemplate(txn, rid, LockMode::EXCLUSIVE);
}

bool LockManager::LockUpgrade(Transaction *txn, const RID &rid) {
  return lockTemplate(txn, rid, LockMode::UPGRADING);
}

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
  if (!canGrant && txList.locks_.back().tid_ < txn->GetTransactionId()) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }
  // try to insert the txn into txlist
  txList.insert(txn, rid, mode, canGrant, &txListLatch);
  return true;
}

} // namespace cmudb
