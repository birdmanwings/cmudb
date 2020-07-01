# 前言

Project1最后一个部分是利用我们前面的extendible hash table和lru来完成我们的buffer pool。

# 正文

我们利用extendible hash table来做根据page id来找对应的page，通过diskmanager我们可以找到page对应在disk上的物理页，然后我们用一个std::list结构free_list来维护空闲的page，当free_page没有剩余的空闲快时，我们需要调用前面写的lrureplacer中挑选出将要被替换的page（实验里声明的victim page，这里用添加了一个GetVictimPage函数专门来获取），然后每个page如果被一个线程使用的话pin+1，然后还有个dirty位来判断page是否需要被更新回disk。

正式开始，先看看.h文件中定义的

```c++
namespace cmudb {
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager *disk_manager,
                    LogManager *log_manager = nullptr);

  ~BufferPoolManager();

  Page *FetchPage(page_id_t page_id);

  bool UnpinPage(page_id_t page_id, bool is_dirty);

  bool FlushPage(page_id_t page_id);

  Page *NewPage(page_id_t &page_id);

  bool DeletePage(page_id_t page_id);

 private:
  size_t pool_size_; // number of pages in buffer pool
  Page *pages_;      // array of pages
  DiskManager *disk_manager_;
  LogManager *log_manager_;
  HashTable<page_id_t, Page *> *page_table_; // to keep track of pages
  Replacer<Page *> *replacer_;   // to find an unpinned page for replacement
  std::list<Page *> *free_list_; // to find a free page for replacement
  std::mutex latch_;             // to protect shared data structure
  Page *GetVictimPage();         // to get a page that will be replaced
};
} // namespace cmudb
```

`disk_manager_`是与磁盘交互读写用的，`log_manager_`在这里还没有用到，`page_table_`就是我们前面写的extendible hash table，`free_list_`用来存储空闲page，`GetVictimPage()`是唯一新添加的方便使用的。

然后来看下cpp文件

```c++
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
```

FetchPage函数在缓存池中根据page_id找page，找到的话pin+1,从replacer中删除掉，没有的话就找victim page，然后判断这个victim page是否是脏的，是否需要刷新回disk中，然后就是在page_table中删除这个victim page并插入新的然后从disk中根据page_id读入数据到p->data中，最后更新下p的一些数据。

看下GetVictimPage()函数

```c++
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
  assert(p->GetPinCount() == 0);  // the replaced page must be free from all threads
  return p;
}
} // namespace cmudb
```

先从free_list中找是否有空闲页，没有就从replacer中找，

然后UnpinPage,FlushPage,DeletePage比较简单不细说了

```c++
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
  p->is_dirty_ = is_dirty;
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
  return false;
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
      return false;
    }
    replacer_->Erase(p);
    page_table_->Remove(page_id);
    p->is_dirty_ = false;
    p->ResetMemory();
    free_list_->push_back(p);
  }
  return true;
}
```

最后看下NewPage

```c++
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
```

根据注释当我们需要新建个page时调用，首先需要个victim page从free list或者replacer中，然后从disk中allocate中获取到新的page_id然后判断victim page是否是脏的，最后就是清理page_table后插入然后初始化page的meta data。

# 总结

Buffer Pool Manager比lru难点，比extendible hash table简单，最难的还是extendible hash table需要仔细阅读我给的那个文章的内容，好的继续学习数据库，刚把爹。