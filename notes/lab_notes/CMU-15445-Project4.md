# 前言

Project4中我们需要完成一个基于ARIES的简单的日志系统，来保证我们数据库的原子性和隔离型。

# 正文

## 基本简述

我们的日志管理系统是基于ARIES，现在大部分的关系型数据库很多都是基于这个的变种，具体的介绍我们可以来看下这两篇文章

- [图解数据库Aries事务Recovery算法](https://my.oschina.net/fileoptions/blog/2988622#comments)
- [关系数据库是如何工作的(8)](https://www.devbean.net/2016/05/how-database-works-8/)

把第一个的例子看懂了，基本就懂了，然后我感觉可以再把《mysql技术内幕》那本书看下应该就可以基本完成关系型数据库的理解了。

我们的但是我们这里只实现的简化版本，因为没有实现check point机制所以也不需要分析阶段，然后我们不考虑undo时候再次发生崩溃，所以不需要CLR记录来保证我们不会重复undo。

## Log Manager

对于这个部分我们首先需要实现一个日志系统来记录每次的操作，但是ARIES是的no-force和steal的，所以我们不是每次写一个log就放入disk中，需要有个刷新机制来触发刷新。

先完成日志系统的大概架构，log_manager.cpp有三个函数去实现，先是RunFlushThread

```c++
/*
 * set ENABLE_LOGGING = true
 * Start a separate thread to execute flush to disk operation periodically
 * The flush can be triggered when the log buffer is full or buffer pool
 * manager wants to force flush (it only happens when the flushed page has a
 * larger LSN than persistent LSN)
 */
void LogManager::RunFlushThread() {
  if (ENABLE_LOGGING) {
    return;
  }
  ENABLE_LOGGING = true;
  // separate a background thread to flush the log into the disk
  flush_thread_ = new thread([&] {
    while (ENABLE_LOGGING) {
      unique_lock<mutex> latch(latch_);
      // timeout or the log buffer is full
      cv_.wait_for(latch, LOG_TIMEOUT, [&] { return needFlush_.load(); });
      assert(flushBufferSize_ == 0);
      if (logBufferOffset_ > 0) {
        // swap the log buffer and flush buffer, and disk manager write the log into disk
        swap(log_buffer_, flush_buffer_);
        swap(logBufferOffset_, flushBufferSize_);
        disk_manager_->WriteLog(flush_buffer_, flushBufferSize_);
        flushBufferSize_ = 0;
        SetPersistentLSN(lastLsn_);
      }
      needFlush_ = false;
      appendCv_.notify_all();
    }
  });
}
```

大致的意思就是我们维护一个后台线程用来负责将日志刷新入disk中，我们来大概看一下它的逻辑，开启一个flush_thread_，然后再匿名函数中写一个循环如果ENABLE_LOGGING开启的情况下的话，加锁后我们用一个条件变量来做刷新时机的维护，一个是每LOG_TIMEOUT时间到的话就触发刷新，或者当needFlush为true时刷新，此时我们需要交换logger buffer和flush buffer中的东西，然后开始调用WriteLog方法落盘，然后刷新设置lastLsn（该事务的最后一条log），直到我们落盘完成才用条件便利那个appendCv继续添加日志，

然后是StopFlushThread，是用来通知flush thread的

```c++
/*
 * Stop and join the flush thread, set ENABLE_LOGGING = false
 */
void LogManager::StopFlushThread() {
  if (!ENABLE_LOGGING) {
    return;
  }
  ENABLE_LOGGING = false;
  Flush(true);
  flush_thread_->join();
  assert(logBufferOffset_ == 0 && flushBufferSize_ == 0);
  delete flush_thread_;
}
```

然后我们看下AppendLogRecord，思路是当要写的东西还能往BUFFER里放的情况下，根据不同的操作类型（详情可以看下log_record.h中不同操作的log长什么样子）写入不同的LOG。如果放不下了，就要唤醒后台线程，然后换一个空的BUFFER，随后放LOG。

```c++
/*
 * append a log record into log buffer
 * you MUST set the log record's lsn within this method
 * @return: lsn that is assigned to this log record
 *
 *
 * example below
 * // First, serialize the must have fields(20 bytes in total)
 * log_record.lsn_ = next_lsn_++;
 * memcpy(log_buffer_ + offset_, &log_record, 20);
 * int pos = offset_ + 20;
 *
 * if (log_record.log_record_type_ == LogRecordType::INSERT) {
 *    memcpy(log_buffer_ + pos, &log_record.insert_rid_, sizeof(RID));
 *    pos += sizeof(RID);
 *    // we have provided serialize function for tuple class
 *    log_record.insert_tuple_.SerializeTo(log_buffer_ + pos);
 *  }
 *
 */
lsn_t LogManager::AppendLogRecord(LogRecord &log_record) {
  unique_lock<mutex> latch(latch_);
  // if the log buffer will be full, wake up the flush thread, and wait this thread until the log buffer is enough to record
  if (logBufferOffset_ + log_record.GetSize() >= LOG_BUFFER_SIZE) {
    needFlush_ = true;
    cv_.notify_one();
    appendCv_.wait(latch, [&] { return logBufferOffset_ + log_record.GetSize() < LOG_BUFFER_SIZE; });
  }
  log_record.lsn_ = next_lsn_++; // update the lsn
  // copy the log header first
  memcpy(log_buffer_ + logBufferOffset_, &log_record, LogRecord::HEADER_SIZE);
  int pos = logBufferOffset_ + LogRecord::HEADER_SIZE;

  // according to the log type
  if (log_record.log_record_type_ == LogRecordType::INSERT) {
    // tuple_rid
    memcpy(log_buffer_ + pos, &log_record.insert_rid_, sizeof(RID));
    pos += sizeof(RID);
    // tuple size and data
    log_record.insert_tuple_.SerializeTo(log_buffer_ + pos);
  } else if (log_record.log_record_type_ == LogRecordType::MARKDELETE ||
      log_record.log_record_type_ == LogRecordType::APPLYDELETE ||
      log_record.log_record_type_ == LogRecordType::ROLLBACKDELETE) {
    memcpy(log_buffer_ + pos, &log_record.delete_rid_, sizeof(RID));
    pos += sizeof(RID);
    log_record.delete_tuple_.SerializeTo(log_buffer_ + pos);
  } else if (log_record.log_record_type_ == LogRecordType::UPDATE) {
    // tuple rid
    memcpy(log_buffer_ + pos, &log_record.update_rid_, sizeof(RID));
    pos += sizeof(RID);
    // old tuple size and data
    log_record.old_tuple_.SerializeTo(log_buffer_ + pos);
    pos += log_record.old_tuple_.GetLength() + sizeof(int32_t);
    // new tuple size and data
    log_record.new_tuple_.SerializeTo(log_buffer_ + pos);
  } else if (log_record.log_record_type_ == LogRecordType::NEWPAGE) {
    memcpy(log_buffer_ + pos, &log_record.prev_page_id_, sizeof(page_id_t));
    pos += sizeof(page_id_t);
    memcpy(log_buffer_ + pos, &log_record.page_id_, sizeof(page_id_t));
  }
  logBufferOffset_ += log_record.GetSize();
  return lastLsn_ = log_record.lsn_;
}
```

然后实现group commit机制，可以在多个txn并行情况下不用每一次commit都刷，等到time out的时候才刷，但是如果有dirty page的情况下就需要刷回去了，所以当force为true的时候就是需要强制刷新回去了。

```c++
void LogManager::Flush(bool force) {
  unique_lock<mutex> latch(latch_);
  if (force) {
    needFlush_ = true;
    cv_.notify_one();
    if (ENABLE_LOGGING) {
      // when needFlush == false, wake up the appendCv and continue to append the log
      appendCv_.wait(latch, [&] {
        return !needFlush_.load();
      });
    }
  } else {
    appendCv_.wait(latch);
  }
}
```

然后我们需要看下哪些情况下需要强制刷新，首先是buffer_pool_manager中，FetchPage和NewPage两个函数

```c++
if (p->is_dirty_) {  // if the page is dirty
  if (ENABLE_LOGGING && log_manager_->GetPersistentLSN() < p->GetLSN()) {
    log_manager_->Flush(true);
  }
  disk_manager_->WritePage(p->GetPageId(), p->data_);
}
```

两处需要判断刷新回去的Page是否是脏的，如果是脏的话，要强制刷新日志到disk中。再看下一处需要刷新的地方。

transaction_manager.cpp中我们在Begin,Commit,Abort时候插入log并设置prep lsn，先看begin的时候

```c++
LogRecord log{txn->GetTransactionId(), txn->GetPrevLSN(), LogRecordType::BEGIN};
txn->SetPrevLSN(log_manager_->AppendLogRecord(log));
```

然后是commit和abort的时候

```c++
LogRecord log{txn->GetTransactionId(), txn->GetPrevLSN(), LogRecordType::COMMIT};
txn->SetPrevLSN(log_manager_->AppendLogRecord(log));
log_manager_->Flush(false);
```

```c++
LogRecord log{txn->GetTransactionId(), txn->GetPrevLSN(), LogRecordType::ABORT};
txn->SetPrevLSN(log_manager_->AppendLogRecord(log));
log_manager_->Flush(false);
```

注意在这两处需要等刷新log完成才能释放txn。

下一处是对txn中的操作进行加日志

在init(newpage)，insert，（mark，apply，rollback）delete，update，的要加上相应的log操作

## System Recovery

这部分我么只要实现简单的redo和undo，因为我们没有实现check point所以没有analyse，因为没有undo阶段的crash所以不要CLR

在log_recovery.cpp中，我们首先判断有没有超过我们的log buffer的大小，然后先拿到header的信息来判断是哪种类型的log，在调用DeserializeFrom拿到具体的数据操作

```c++
/*
 * deserialize a log record from log buffer
 * @return: true means deserialize succeed, otherwise can't deserialize cause
 * incomplete log record
 */
bool LogRecovery::DeserializeLogRecord(const char *data,
                                       LogRecord &log_record) {
  // left size should be larger than header size
  if (data + LogRecord::HEADER_SIZE > log_buffer_ + LOG_BUFFER_SIZE) {
    return false;
  }
  // first copy the header to log_record to judge the log type
  memcpy(&log_record, data, LogRecord::HEADER_SIZE);
  // make sure the size of log_record that has deserialized < log buffer size
  if (log_record.size_ <= 0 || data + log_record.size_ > log_buffer_ + LOG_BUFFER_SIZE) {
    return false;
  }
  // jump to the data position
  data += LogRecord::HEADER_SIZE;
  switch (log_record.log_record_type_) {
    case LogRecordType::INSERT:log_record.insert_rid_ = *reinterpret_cast<const RID *>(data);
      log_record.insert_tuple_.DeserializeFrom(data + sizeof(RID));
    case LogRecordType::MARKDELETE:
    case LogRecordType::APPLYDELETE:
    case LogRecordType::ROLLBACKDELETE:log_record.delete_rid_ = *reinterpret_cast<const RID *>(data);
      log_record.delete_tuple_.DeserializeFrom(data + sizeof(RID));
      break;
    case LogRecordType::UPDATE:log_record.update_rid_ = *reinterpret_cast<const RID *>(data);
      log_record.old_tuple_.DeserializeFrom(data + sizeof(RID));
      log_record.new_tuple_.DeserializeFrom(data + sizeof(RID) + 4 + log_record.old_tuple_.GetLength());
      break;
    case LogRecordType::BEGIN:
    case LogRecordType::COMMIT:
    case LogRecordType::ABORT:break;
    case LogRecordType::NEWPAGE:log_record.prev_page_id_ = *reinterpret_cast<const page_id_t *>(data);
      log_record.page_id_ = *reinterpret_cast<const page_id_t *>(data + sizeof(page_id_t));
      break;
    default:assert(false);
  }
  return true;
}
```

然后看下Redo，比较复杂，稍微说下就是我们每次反序列化的话因为log buffer大小的限制可能有一段是不完整的x，我们需要调整下位置，让下一次ReadLog的时候来从剩下的那部分末尾x开始读，直到读到x加上后读的到达LOG_BUFFER_SIZE的大小，然后我们继续DeserializeLogRecord，一直往复，然后读到最后我们怎么知道没了么，其实在ReadLog函数中根据log大小判断是否有空余位置来补零，然后在上面的DeserializeLogRecord函数中log_record.size_ <= 0就停止了。

说完了读取log和反序列化log的要点后，根据读到的log type来做相应的处理，注意只有log lsn > page lsn时说明log的操作没有映射到disk上需要redo，然后还有在new page的时候更改新加了个page_id变量在log中方便new page

```c++
/*
 *redo phase on TABLE PAGE level(table/table_page.h)
 *read log file from the beginning to end (you must prefetch log records into
 *log buffer to reduce unnecessary I/O operations), remember to compare page's
 *LSN with log_record's sequence number, and also build active_txn_ table &
 *lsn_mapping_ table
 */
void LogRecovery::Redo() {
  //lock_guard<mutex> lock(mu_); no thread safe
  // ENABLE_LOGGING must be false when recovery
  assert(ENABLE_LOGGING == false);
  // always replay history from start without checkpoint
  offset_ = 0;
  int bufferOffset = 0;
  // read the log to the log_buffer_ + bufferOffset position
  while (disk_manager_->ReadLog(log_buffer_ + bufferOffset,
                                LOG_BUFFER_SIZE - bufferOffset,
                                offset_)) {// false means log eof
    int bufferStart = offset_;
    offset_ += LOG_BUFFER_SIZE - bufferOffset;
    bufferOffset = 0;
    LogRecord log;
    //
    while (DeserializeLogRecord(log_buffer_ + bufferOffset, log)) {
      lsn_mapping_[log.GetLSN()] = bufferStart + bufferOffset;
      active_txn_[log.txn_id_] = log.lsn_;
      bufferOffset += log.size_;
      if (log.log_record_type_ == LogRecordType::BEGIN) continue;
      if (log.log_record_type_ == LogRecordType::COMMIT ||
          log.log_record_type_ == LogRecordType::ABORT) {
        assert(active_txn_.erase(log.GetTxnId()) > 0);
        continue;
      }
      if (log.log_record_type_ == LogRecordType::NEWPAGE) {
        auto page = static_cast<TablePage *>(buffer_pool_manager_->FetchPage(log.page_id_));
        assert(page != nullptr);
        // if log's lsn > page's lsn, it means the log has been writen to disk, but the action hasn't done
        // so we need redo this action.
        bool needRedo = log.lsn_ > page->GetLSN();
        if (needRedo) {
          page->Init(log.page_id_, PAGE_SIZE, log.prev_page_id_, nullptr, nullptr);
          page->SetLSN(log.lsn_);
          if (log.prev_page_id_ != INVALID_PAGE_ID) {
            auto prevPage = static_cast<TablePage *>(
                buffer_pool_manager_->FetchPage(log.prev_page_id_));
            assert(prevPage != nullptr);
            bool needChange = prevPage->GetNextPageId() == log.page_id_;
            prevPage->SetNextPageId(log.page_id_);
            buffer_pool_manager_->UnpinPage(prevPage->GetPageId(), needChange);
          }
        }
        buffer_pool_manager_->UnpinPage(page->GetPageId(), needRedo);

        continue;
      }

      RID rid = log.log_record_type_ == LogRecordType::INSERT ? log.insert_rid_ :
                log.log_record_type_ == LogRecordType::UPDATE ? log.update_rid_ :
                log.delete_rid_;
      auto page = static_cast<TablePage *>(
          buffer_pool_manager_->FetchPage(rid.GetPageId()));
      assert(page != nullptr);
      bool needRedo = log.lsn_ > page->GetLSN();
      if (needRedo) {
        if (log.log_record_type_ == LogRecordType::INSERT) {
          page->InsertTuple(log.insert_tuple_, rid, nullptr, nullptr, nullptr);
        } else if (log.log_record_type_ == LogRecordType::UPDATE) {
          page->UpdateTuple(log.new_tuple_, log.old_tuple_, rid, nullptr, nullptr, nullptr);
        } else if (log.log_record_type_ == LogRecordType::MARKDELETE) {
          page->MarkDelete(rid, nullptr, nullptr, nullptr);
        } else if (log.log_record_type_ == LogRecordType::APPLYDELETE) {
          page->ApplyDelete(rid, nullptr, nullptr);
        } else if (log.log_record_type_ == LogRecordType::ROLLBACKDELETE) {
          page->RollbackDelete(rid, nullptr, nullptr);
        } else {
          assert(false);//invalid area
        }
        page->SetLSN(log.lsn_);  // remember to update the page's lsn
      }
      buffer_pool_manager_->UnpinPage(page->GetPageId(), needRedo);
    }
    memmove(log_buffer_, log_buffer_ + bufferOffset, LOG_BUFFER_SIZE - bufferOffset);
    bufferOffset = LOG_BUFFER_SIZE - bufferOffset;//rest partial log
  }
}
```

最后来看下undo操作，平常undo是根据ATT的大小从大到小来恢复并且有CLR但是我们这里不考虑crash，所以就可以直接遍历active_txn，来执行undo的操作就行了

```c++
/*
 *undo phase on TABLE PAGE level(table/table_page.h)
 *iterate through active txn map and undo each operation
 */
void LogRecovery::Undo() {
  //lock_guard<mutex> lock(mu_); no thread safe
  // ENABLE_LOGGING must be false when recovery
  assert(ENABLE_LOGGING == false);
  // because in this project, we don's need to worry about crash during recovery process,
  // so we don't need to undo from the largest lsn, we can just loop to undo from active_txn.
  for (auto &txn : active_txn_) {
    lsn_t lsn = txn.second;
    while (lsn != INVALID_LSN) {
      LogRecord log;
      disk_manager_->ReadLog(log_buffer_, PAGE_SIZE, lsn_mapping_[lsn]);
      assert(DeserializeLogRecord(log_buffer_, log));
      assert(log.lsn_ == lsn);
      lsn = log.prev_lsn_;  // continue for next lsn
      if (log.log_record_type_ == LogRecordType::BEGIN) {
        assert(log.prev_lsn_ == INVALID_LSN);
        continue;
      }
      if (log.log_record_type_ == LogRecordType::COMMIT ||
          log.log_record_type_ == LogRecordType::ABORT)
        assert(false);  // ATT shouldn't have committed or aborted txn
      if (log.log_record_type_ == LogRecordType::NEWPAGE) {
        if (!buffer_pool_manager_->DeletePage(log.page_id_))
          disk_manager_->DeallocatePage(log.page_id_);
        if (log.prev_page_id_ != INVALID_PAGE_ID) {
          auto prevPage = static_cast<TablePage *>(
              buffer_pool_manager_->FetchPage(log.prev_page_id_));
          assert(prevPage != nullptr);
          assert(prevPage->GetNextPageId() == log.page_id_);
          // delete the new page, so we need to set the pre's next page to invalid
          prevPage->SetNextPageId(INVALID_PAGE_ID);
          buffer_pool_manager_->UnpinPage(prevPage->GetPageId(), true);
        }
        continue;
      }
      RID rid = log.log_record_type_ == LogRecordType::INSERT ? log.insert_rid_ :
                log.log_record_type_ == LogRecordType::UPDATE ? log.update_rid_ :
                log.delete_rid_;
      auto page = static_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
      assert(page != nullptr);
      assert(page->GetLSN() >= log.lsn_);
      if (log.log_record_type_ == LogRecordType::INSERT) {
        page->ApplyDelete(log.insert_rid_, nullptr, nullptr);
      } else if (log.log_record_type_ == LogRecordType::UPDATE) {
        Tuple tuple;
        page->UpdateTuple(log.old_tuple_, tuple, log.update_rid_, nullptr, nullptr, nullptr);
        assert(tuple.GetLength() == log.new_tuple_.GetLength() &&
            memcmp(tuple.GetData(), log.new_tuple_.GetData(), tuple.GetLength()) == 0);
      } else if (log.log_record_type_ == LogRecordType::MARKDELETE) {
        page->RollbackDelete(log.delete_rid_, nullptr, nullptr);
      } else if (log.log_record_type_ == LogRecordType::APPLYDELETE) {
        page->InsertTuple(log.delete_tuple_, log.delete_rid_, nullptr, nullptr, nullptr);
      } else if (log.log_record_type_ == LogRecordType::ROLLBACKDELETE) {
        page->MarkDelete(log.delete_rid_, nullptr, nullptr, nullptr);
      } else
        assert(false);
      buffer_pool_manager_->UnpinPage(page->GetPageId(), true);
    }
  }
  active_txn_.clear();
  lsn_mapping_.clear();
}
```

最后需要更改下buffer_used这个静态变量到成员变量中，然后改下UpdateTuple中的GetTupleSize(i) != 0，因为有可能tuple为负了。最后跑通测试

# 总结

基本完成了所有的project，做的比较匆忙，回头分析下遗漏的部分和test，加油。