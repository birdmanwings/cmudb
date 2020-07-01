#include "buffer/buffer_pool_manager.h"

namespace cmudb {

/*
 * BufferPoolManager Constructor
 * When log_manager is nullptr, logging is disabled (for test purpose)
 * WARNING: Do Not Edit This Function
 */
BufferPoolManager::BufferPoolManager(size_t pool_size,
                                     DiskManager *disk_manager,
                                     LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager),
      log_manager_(log_manager) {
  // a consecutive memory space for buffer pool
  pages_ = new Page[pool_size_];
  page_table_ = new ExtendibleHash<page_id_t, Page *>(BUCKET_SIZE);
  replacer_ = new LRUReplacer<Page *>;
  free_list_ = new std::list<Page *>;

  // put all the pages into free list
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_->push_back(&pages_[i]);
  }
}

/*
 * BufferPoolManager Deconstructor
 * WARNING: Do Not Edit This Function
 */
BufferPoolManager::~BufferPoolManager() {
  delete[] pages_;
  delete page_table_;
  delete replacer_;
  delete free_list_;
}

/**
 * 1. search hash table.
 *  1.1 if exist, pin the page and return immediately
 *  1.2 if no exist, find a replacement entry from either free list or lru
 *      replacer. (NOTE: always find from free list first)
 * 2. If the entry chosen for replacement is dirty, write it back to disk.
 * 3. Delete the entry for the old page from the hash table and insert an
 * entry for the new page.
 * 4. Update page metadata, read page content from disk file and return page
 * pointer
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  lock_guard<mutex> lock(latch_);
  Page *p = nullptr;
  if (page_table_->Find(page_id, p)) {  // if find the page in the page table
    p->pin_count_++;
    replacer_->Erase(p);
    return p;
  }
  p = GetVictimPage();  // find a replacement entry, in other words find a page that will be replaced
  if (p == nullptr) {
    return p;
  }
  if (p->is_dirty_) {  // if the page is dirty
    if (ENABLE_LOGGING && log_manager_->GetPersistentLSN() < p->GetLSN()) {
      log_manager_->Flush(true);
    }
    disk_manager_->WritePage(p->GetPageId(), p->data_);
  }
  page_table_->Remove(p->GetPageId());
  page_table_->Insert(page_id, p);  // prepare point p
  disk_manager_->ReadPage(page_id, p->data_); // read the content from disk to p.data_ according to page_id
  p->pin_count_ = 1;
  p->is_dirty_ = false;
  p->page_id_ = page_id;
  return p;
}

/*
 * Implementation of unpin page
 * if pin_count>0, decrement it and if it becomes zero, put it back to
 * replacer if pin_count<=0 before this call, return false. is_dirty: set the
 * dirty flag of this page
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  lock_guard<mutex> lock(latch_);
  Page *p = nullptr;
  page_table_->Find(page_id, p);
  if (p == nullptr) {
    return false;
  }
  p->is_dirty_ |= is_dirty;  // pay attion to use | , false can't cover the true flag.

  if (p->GetPinCount() <= 0) {
    cout << "DeletePage Error:" << p->page_id_ << endl;
    assert(false);
    return false;
  }

  if (--p->pin_count_ == 0) {
    replacer_->Insert(p);
  }
  return true;
}

/*
 * Used to flush a particular page of the buffer pool to disk. Should call the
 * write_page method of the disk manager
 * if page is not found in page table, return false
 * NOTE: make sure page_id != INVALID_PAGE_ID
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  lock_guard<mutex> lock(latch_);
  Page *p = nullptr;
  page_table_->Find(page_id, p);
  if (p == nullptr || p->page_id_ == INVALID_PAGE_ID) {
    return false;
  }
  if (p->is_dirty_) {
    disk_manager_->WritePage(page_id, p->GetData());
    p->is_dirty_ = false;
  }
  return true;
}

/**
 * User should call this method for deleting a page. This routine will call
 * disk manager to deallocate the page. First, if page is found within page
 * table, buffer pool manager should be reponsible for removing this entry out
 * of page table, reseting page metadata and adding back to free list. Second,
 * call disk manager's DeallocatePage() method to delete from disk file. If
 * the page is found within page table, but pin_count != 0, return false
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  lock_guard<mutex> lock(latch_);
  Page *p = nullptr;
  page_table_->Find(page_id, p);
  if (p == nullptr) {
    disk_manager_->DeallocatePage(page_id);
  } else {
    if (p->GetPinCount() > 0) {   // if there's still thread hold this page, return false
//      cout << "DeletePage Error in Delete func:" << p->page_id_ << endl;
//      assert(false);
      return false;
    }
    replacer_->Erase(p);
    page_table_->Remove(page_id);
    p->is_dirty_ = false;
    p->ResetMemory();
    p->page_id_ = INVALID_PAGE_ID;
    free_list_->push_back(p);
  }
  return true;
}

/**
 * User should call this method if needs to create a new page. This routine
 * will call disk manager to allocate a page.
 * Buffer pool manager should be responsible to choose a victim page either
 * from free list or lru replacer(NOTE: always choose from free list first),
 * update new page's metadata, zero out memory and add corresponding entry
 * into page table. return nullptr if all the pages in pool are pinned
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  lock_guard<mutex> lock(latch_);
  Page *p = nullptr;
  p = GetVictimPage();  // get a victim page for allocated page from disk
  if (p == nullptr) {
    return p;
  }
  page_id = disk_manager_->AllocatePage();
  if (p->is_dirty_) {
    if (ENABLE_LOGGING && log_manager_->GetPersistentLSN() < p->GetLSN()) {
      log_manager_->Flush(true);
    }
    disk_manager_->WritePage(p->GetPageId(), p->data_);
  }
  page_table_->Remove(p->GetPageId());
  page_table_->Insert(page_id, p);

  // init the page meta-date
  p->page_id_ = page_id;
  p->ResetMemory();
  p->is_dirty_ = false;
  p->pin_count_ = 1;
  return p;
}

/*
 * return the page that will be replaced from free list,
 * otherwise use lru replacer select an unpinned Page that was least recently used as the "victim" page
 */
Page *BufferPoolManager::GetVictimPage() {
  Page *p = nullptr;
  if (free_list_->empty()) {  // if there is no free page to be replaced, need to get from replacer
    if (replacer_->Size() == 0) { // if there is no page to be replaced into the disk, return nullptr
      return nullptr;
    }
    replacer_->Victim(p);  // return a page that need to be replaced
  } else {
    p = free_list_->front();
    free_list_->pop_front();
    assert(p->GetPageId() == INVALID_PAGE_ID);
  }
  if (p != nullptr) {
    assert(p->GetPinCount() == 0);  // the replaced page must be free from all threads
  }
  return p;
}

//DEBUG
bool BufferPoolManager::CheckAllUnpined() {
  bool res = true;
  for (size_t i = 1; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      std::cout << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }

  }
  return res;
}

} // namespace cmudb
