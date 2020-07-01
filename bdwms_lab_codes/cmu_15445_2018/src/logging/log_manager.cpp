/**
 * log_manager.cpp
 */

#include <include/common/logger.h>
#include "logging/log_manager.h"

namespace cmudb {
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

} // namespace cmudb
