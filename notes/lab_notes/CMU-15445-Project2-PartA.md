# 前言

Project2的第一部分，主要是实现B+树的主体结构部分，还有搜索和插入两个功能。

# 正文

## Part1 B+TREE PAGES

### page头文件

实现是实现B+树的叶结构，这里我们设计一个父类BPlusTreePage，然后两个子类BPlusTreeLeafPage和BPlusTreeInternalPage，一个表示叶子page，一个表示中间page。

先看下父类的结构

```c++
// Abstract class.
class BPlusTreePage {
public:
  bool IsLeafPage() const;
  bool IsRootPage() const;
  void SetPageType(IndexPageType page_type);

  int GetSize() const;
  void SetSize(int size);
  void IncreaseSize(int amount);

  int GetMaxSize() const;
  void SetMaxSize(int max_size);
  int GetMinSize() const;

  page_id_t GetParentPageId() const;
  void SetParentPageId(page_id_t parent_page_id);

  page_id_t GetPageId() const;
  void SetPageId(page_id_t page_id);

  void SetLSN(lsn_t lsn = INVALID_LSN);

private:
  // member variable, attributes that both internal and leaf page share
  IndexPageType page_type_;
  lsn_t lsn_;
  int size_;
  int max_size_;
  page_id_t parent_page_id_;
  page_id_t page_id_;
};
```

其中的成员变量看下，作为头部信息

| Variable Name   | Size | Description                             |
| --------------- | ---- | --------------------------------------- |
| page_type_      | 4    | Page Type (internal or leaf)            |
| lsn_            | 4    | Log sequence number (Used in Project 4) |
| size_           | 4    | Number of Key & Value pairs in page     |
| max_size_       | 4    | Max number of Key & Value pairs in page |
| parent_page_id_ | 4    | Parent Page Id                          |
| page_id_        | 4    | Self Page Id                            |

然后看下leaf page，看下多了的两个成员变量

```c++
page_id_t next_page_id_;
MappingType array[0];
```

对于叶子结点的组织方式除了上层节点的指针，还有一个链表结构（`next_page_id_`）将叶子结点组织起来，然后对于每一个叶子结点其存储数组的结构是key/value，key是索引（唯一的），value是关联到slot id的，然后我们来看下具体的叶子结点，假如其`max_size_`为4，实际上它是可以存储5个值但是最后一位会留给溢出的元素，来方便之后的处理，类似0 1 2 3 4，这五个索引，前四个都是有效的，下标为4的索引只有插入的值溢出时才会把其放在末尾。

然后是internal page，他只有

```c++
MappingType array[0];
```

然后提前定义了个宏给后面用

```c++
#define B_PLUS_TREE_INTERNAL_PAGE BPlusTreeInternalPage <KeyType, page_id_t, KeyComparator>
```

然后我们来看下internal page的结构是什么样的，同样是`max_size_`为4，实际容量为5，保留一位溢出位，但是n个有效位需要n+1个指针来指向子结点，所以我们需要将第一位也保留，作为左指针，即类似0 1 2 3 4，从索引1到3才是有效的，索引0对应的值是左指针，1对应的是右边指针，只有三位是有效的索引位，所以其实真正的maxsize是3。

### b_plus_tree_page.cpp

然后我们来具体实现一下里面的一些基础函数。先从b_plus_tree_page.cpp，里面唯一一个需要注意的函数是

```c++
/*
 * Helper method to get min page size
 * Generally, min page size == max page size / 2
 */
int BPlusTreePage::GetMinSize() const {
  if (IsRootPage() && IsLeafPage()) {
    return 1;
  }
  return IsRootPage() ? 2 : (GetMaxSize() + 1) / 2;
}
```

对于只有一个结点的树，如果该结点的size小于1即为0的时候需要调整header page，就需要回收这个root page，然后他是根结点并且不是leaf page的话，那么应该返回2，因为只有一个leaf page的话需要将leaf page调整为root page（这里的逻辑在后面的部分会实现，具体看代码部分），即一个左指针一个右指针，否则就返回`(GetMaxSize() + 1) / 2`，小于min的结点需要merge。

### b_plus_tree_internal_page.cpp

然后看b_plus_tree_internal_page.cpp来实现internal page中的基础函数。先是Init函数

```c++
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Init(page_id_t page_id,
                                          page_id_t parent_id) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetMaxSize((PAGE_SIZE - sizeof(BPlusTreeInternalPage)) / sizeof(MappingType) - 1);
}
```

主要看下SetMaxSize这里是怎么实现的，PAGE_SIZE是512byte是一个页面的大小，首先需要减去header头信息即sizeof(BPlusTreeInternalPage),(因为internal page也是从page类继承来的，基类page里面包含了header的信息)，然后除以一个key/value对的大小，得到是整个的大小，然后-1是为了预留一个分裂时用的溢出位置。

然后是一些小的查找函数

```c++
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
INDEX_TEMPLATE_ARGUMENTS
KeyType B_PLUS_TREE_INTERNAL_PAGE_TYPE::KeyAt(int index) const {
  // replace with your own code
  assert(index >= 0 && index < GetSize());
  return array[index].first;  // get the key according to index
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetKeyAt(int index, const KeyType &key) {
  assert(index >= 0 && index < GetSize());
  array[index].first = key;
}

/*
 * Helper method to find and return array index(or offset), so that its value
 * equals to input "value"
 */
INDEX_TEMPLATE_ARGUMENTS
int B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueIndex(const ValueType &value) const {
  for (int i = 0; i < GetSize(); i++) {
    if (value != ValueAt(i)) continue;
    return i;
  }
  return -1;
}

/*
 * Helper method to get the value associated with input "index"(a.k.a array
 * offset)
 */
INDEX_TEMPLATE_ARGUMENTS
ValueType B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueAt(int index) const {
  assert(index >= 0 && index < GetSize());
  return array[index].second;
}
```

然后是一个Lookup函数用来在page内部寻找对应的子结点指针，因为key是有序的所以可以用二分来找，注意返回的是`array[l-1].second`，某个index（没有0这个index）对应的value是右侧范围的值这里注意下，如果不懂可以画图看下

```c++
/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 */
INDEX_TEMPLATE_ARGUMENTS
ValueType
B_PLUS_TREE_INTERNAL_PAGE_TYPE::Lookup(const KeyType &key,
                                       const KeyComparator &comparator) const {
  assert(GetSize() > 1);
  // because keys are ordered, we use binary search
  int l = 1, r = GetSize() - 1, mid;
  while (l <= r) {
    mid = (r - l) / 2 + l;
    if (comparator(array[mid].first, key) <= 0) {
      l = mid + 1;
    } else {
      r = mid - 1;
    }
  }
  return array[l - 1].second;
}
```

### b_plus_tree_leaf_page.cpp

然后来看下leaf page的基础函数

同样是Init，SetMaxSize同样需要保留一位溢出位，这里sizeof的问题之后再研究，（update：基本弄懂了sizeof，这里继承的基类里面的header为20，然后leaf page中又添加了一个int和一个数组，size都是4，所以总共是28）

```c++
/**
 * Init method after creating a new leaf page
 * Including set page type, set current size to zero, set page id/parent id, set
 * next page id and set max size
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Init(page_id_t page_id, page_id_t parent_id) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  // TODO
  // 这里没懂为什么是28，这里先保留下，之后再研究
  assert(sizeof(BPlusTreeLeafPage) == 28);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetNextPageId(INVALID_PAGE_ID);
  SetMaxSize((PAGE_SIZE - sizeof(BPlusTreeLeafPage)) / sizeof(MappingType) - 1);
}
```

几个辅助函数

```c++
/**
 * Helper methods to set/get next page id
 */
INDEX_TEMPLATE_ARGUMENTS
page_id_t B_PLUS_TREE_LEAF_PAGE_TYPE::GetNextPageId() const {
  return next_page_id_;
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::SetNextPageId(page_id_t next_page_id) {
  next_page_id_ = next_page_id;
}
```

```c++
/*
 * Helper method to find and return the key associated with input "index"(a.k.a
 * array offset)
 */
INDEX_TEMPLATE_ARGUMENTS
KeyType B_PLUS_TREE_LEAF_PAGE_TYPE::KeyAt(int index) const {
  // replace with your own code
  assert(index >= 0 && index < GetSize());
  return array[index].first;
}

/*
 * Helper method to find and return the key & value pair associated with input
 * "index"(a.k.a array offset)
 */
INDEX_TEMPLATE_ARGUMENTS
const MappingType &B_PLUS_TREE_LEAF_PAGE_TYPE::GetItem(int index) {
  // replace with your own code
  assert(index >= 0 && index < GetSize());
  return array[index];
}
```

然后是一个KeyIndex函数根据key来找到第一个大于等于key的array[index].first，同样是利用二分来做，但是这里要注意下返回值，因为是>=所以应该返回的r+1，不懂的可以画图试试就知道了

```c++
/**
 * Helper method to find the first index i so that array[i].first >= key
 * NOTE: This method is only used when generating index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
int B_PLUS_TREE_LEAF_PAGE_TYPE::KeyIndex(
    const KeyType &key, const KeyComparator &comparator) const {
  assert(GetSize() >= 0);
  // use binary search
  int l = 0, r = GetSize() - 1, mid;
  while (l <= r) {
    mid = (r - l) / 2 + l;
    if (comparator(array[mid].first, key) < 0) {
      l = mid + 1;
    } else {
      r = mid - 1;
    }
  }
  return r + 1;  // pay attention to it's the first index >= key
}
```

然后是一个Lookup函数，这里我们直接调用上面的KeyIndex函数来寻找第一个大于等于的，然后比较key是否一样

```c++
/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * For the given key, check to see whether it exists in the leaf page. If it
 * does, then store its corresponding value in input "value" and return true.
 * If the key does not exist, then return false
 */
INDEX_TEMPLATE_ARGUMENTS
bool B_PLUS_TREE_LEAF_PAGE_TYPE::Lookup(const KeyType &key, ValueType &value,
                                        const KeyComparator &comparator) const {
  int idx = KeyIndex(key, comparator);  // find the key in the leaf page.
  if (idx < GetSize() && comparator(array[idx].first, key) == 0) {
    value = array[idx].second;
    return true;
  }
  return false;
}
```

## Part2 B+TREE DATA STRUCTURE (INSERTION & POINT SEARCH)

### Find

#### b_plus_tree_page.cpp

要实现搜索和插入的功能，那么我们先实现搜索的功能，先在b_plus_tree.h头文件中添加一个FetchPage函数用来从buffer pool中获取page

`BPlusTreePage *FetchPage(page_id_t page_id);`

然后同样是先完善一下基础的函数

```c++
/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }
```

然后看GetValue函数，FindLeafPage函数找到理论上key应该存在的leaf page，如果找到了，再用Lookup函数看是否能取出value到result中，最后记得要unpin一下这个leaf page.

```c++
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::GetValue(const KeyType &key,
                              std::vector<ValueType> &result,
                              Transaction *transaction) {
  B_PLUS_TREE_LEAF_PAGE_TYPE *targetPage = FindLeafPage(key);  // find the page containing the key
  if (targetPage == nullptr) {
    return false;
  }
  result.resize(1);
  auto isFind = targetPage->Lookup(key, result[0], comparator_);  // put the value in the result

  buffer_pool_manager_->UnpinPage(targetPage->GetPageId(), false);  // unpin this page

  return isFind;
}
```

然后跟进完成一下FindLeafPage函数，FindLeafPage的逻辑是先判断不为空树，然后利用FetchPage从buffer pool中根据root_page_id_来获取到根page，然后开始遍历直到到达leaf page，这里又个leftMost是在实现iterator时候会用到，这里先不详细说，这里我们FetchPage获取到的指针式BPlusTreePage格式的，我们需要用static_cast来进行一个良性转换为`B_PLUS_TREE_INTERNAL_PAGE`（因为是子类所以是良性的），然后用Lookup函数来找到相应key存在的指向子page的指针，这样循环直到leaf page，最后返回指向这个leaf page的指针。（这里的查找最底层都是利用二分的）

```c++
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 */
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

看下我们自己定义的FetchPage函数，从buffer pool中根据page_id来拿到对应的page

```c++
/*
 * Fetch the page from the buffer pool manager using its unique page_id, then reinterpret cast to either
 * a leaf or an internal page
 */
INDEX_TEMPLATE_ARGUMENTS
BPlusTreePage *BPLUSTREE_TYPE::FetchPage(page_id_t page_id) {
  auto page = buffer_pool_manager_->FetchPage(page_id);
  return reinterpret_cast<BPlusTreePage *>(page->GetData());
}
```

### Insert

#### b_plus_tree_page.cpp

Insert这一块比较复杂，因为涉及到分裂的情况，先看Insert()函数

```c++
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value,
                            Transaction *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
```

当是一个空树时，新建一个树StartNewTree，否则调用InsertIntoLeaf插入到leaf page中，我们先看StartNewTree

```c++
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::StartNewTree(const KeyType &key, const ValueType &value) {
  page_id_t newRootPageId;  // as a new root page id
  Page *rootPage = buffer_pool_manager_->NewPage(newRootPageId);  // new a page from buffer pool
  assert(rootPage != nullptr);

  // convert the struct Page into the struct B_PLUS_TREE_LEAF_PAGE_TYPE
  B_PLUS_TREE_LEAF_PAGE_TYPE *root = reinterpret_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(rootPage->GetData());

  root->Init(newRootPageId, INVALID_PAGE_ID); // init the root
  root_page_id_ = newRootPageId;
  UpdateRootPageId(true);  // insert a new root page id into header page
  root->Insert(key, value, comparator_); // insert key/value into leaf page

  buffer_pool_manager_->UnpinPage(newRootPageId, true);  // unpin this page and mark it dirty
}
```

先从buffer pool中new一个Page出来，存储id在newRootPageId中，然后我们要将Page * struct转换为B_PLUS_TREE_LEAF_PAGE_TYPE *类型，需要使用reinterpret_cast进行不安全的转换，然后需要初始化一些信息，Init()，更新root_page_id，UpdateRootPageId更新root page id到header page中，然后将key/value插入（在b_plus_tree_leaf_page.cp中实现）到root这个page中，最后unpin这个page。

然后我们看下InsertIntoLeaf这个函数

```c++
/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immdiately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::InsertIntoLeaf(const KeyType &key, const ValueType &value,
                                    Transaction *transaction) {
  B_PLUS_TREE_LEAF_PAGE_TYPE *leafPage = FindLeafPage(key);
  ValueType v;
  bool isExist = leafPage->Lookup(key, v, comparator_);
  // if it's duplicate key
  if (isExist) {
    buffer_pool_manager_->UnpinPage(leafPage->GetPageId(), false);
    return false;
  }
  leafPage->Insert(key, value, comparator_);
  // if it's overflow, then split
  if (leafPage->GetSize() > leafPage->GetMaxSize()) {
    B_PLUS_TREE_LEAF_PAGE_TYPE *newLeafPage = Split(leafPage);
    // insert the new leaf page into parent page
    InsertIntoParent(leafPage, newLeafPage->KeyAt(0), newLeafPage, transaction);
  }
  buffer_pool_manager_->UnpinPage(leafPage->GetPageId(), true);
  return false;
}
```

首先我们应该FindLeafPage中找到相应key应该在的leaf page，然后判断是否已经存在key了，如果存在直接返回false，并且unpin这个leaf page，否则就Insert到这个leaf page中，然后我们需要判断这个page是否溢出了，如果溢出了，我们需要调用Split函数来分裂出一个新的page，并将值分配好，然后调用InsertIntoParent函数将new leaf page插入到parent page中的相应的位置，最后同样记得unpin原来的leaf page（new leaf page我们在别的地方unpin）。

然后我们逐一看下其中的函数，先看Split函数，他用来分裂出新的page，他的逻辑是从buffer pool中new一个Page然后转换后初始化Init，然后调用MoveHalfTo函数将old node中一半的element转移，最后返回new node,（MoveHalfTo放在后面说）

```c++
/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
N *BPLUSTREE_TYPE::Split(N *node) {
  // get a new page from buffer pool
  page_id_t newPageId;
  Page *newPage = buffer_pool_manager_->NewPage(newPageId);
  assert(newPage != nullptr);

  // convert struct
  N *newNode = reinterpret_cast<N *>(newPage->GetData());

  // init the new node(leaf or internal page)
  newNode->Init(newPageId, node->GetParentPageId());
  // move half key/value into new node
  node->MoveHalfTo(newNode, buffer_pool_manager_);
  return newNode;
}
```

然后看下InsertIntoParent，这个函数是将new node记录到parent node相应的位置

```c++
/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(BPlusTreePage *old_node,
                                      const KeyType &key,
                                      BPlusTreePage *new_node,
                                      Transaction *transaction) {
  // if the old node is root node, we need new a page from buffer pool as a new root
  if (old_node->IsRootPage()) {
    Page *newPage = buffer_pool_manager_->NewPage(root_page_id_);
    assert(newPage != nullptr);
    assert(newPage->GetPinCount() == 1);
    B_PLUS_TREE_INTERNAL_PAGE *newRoot = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(newPage->GetData());
    newRoot->Init(root_page_id_);
    newRoot->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(root_page_id_);
    new_node->SetParentPageId(root_page_id_);
    UpdateRootPageId();  // update the root page id
    // remember to unpin the new root page and the new_node page
    buffer_pool_manager_->UnpinPage(new_node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(newRoot->GetPageId(), true);
    return;
  }
  page_id_t parentId = old_node->GetParentPageId();  // get the parent id
  auto *page = FetchPage(parentId);  // fetch the Page from buffer pool according to page_id
  assert(page != nullptr);
  B_PLUS_TREE_INTERNAL_PAGE *parent = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(page);
  new_node->SetParentPageId(parentId);
  buffer_pool_manager_->UnpinPage(new_node->GetPageId(), true);
  // for parent node insert the new page after the old page
  parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
  // recursive if parent is overflow
  if (parent->GetSize() > parent->GetMaxSize()) {
    B_PLUS_TREE_INTERNAL_PAGE *newLeafPage = Split(parent);
    InsertIntoParent(parent, newLeafPage->KeyAt(0), newLeafPage, transaction);
  }
  buffer_pool_manager_->UnpinPage(parentId, true);
}
```

这个函数比较复杂，需要考虑几种情况，首先是当old node是根结点，此时没有parent node，那么我们就需要从buffer pool中new一个新的page，经过初始化后，调用PopulateNewRoot函数来计算new root中的key/value（在internal page cpp中实现），将old node和new node的parent node id设置为新的root，然后更新UpdateRootPageId，最后unpin一下新的root node和new node。另一个情况是存在parent node这种情况，我们FetchPage拿到这个parent node的指针转换后，将new node的parent node id设置为它，然后unpin new node，对于这个parent node需要调用InsertNodeAfter，将new node插到old node后面，然后判断parent node是否有溢出，如果溢出继续分裂parent node，这种情况是递归得，最后记得unpin这个parent node。

之后我们再看下b_plus_tree_internal_page.cpp和b_plus_tree_leaf_page.cpp中要实现的Insert的部分。

#### b_plus_tree_internal_page.cpp

首先看下b_plus_tree_internal_page.cpp的PopulateNewRoot，这个主要更新root结点的左右两个指针

```c++
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::PopulateNewRoot(
    const ValueType &old_value, const KeyType &new_key,
    const ValueType &new_value) {
  array[0].second = old_value;  // 0 value is left pointer that point to the old node
  array[1].first = new_key;     // 1 key is the new root key
  array[1].second = new_value;  // 1 value is right pointer that point to the new node
  SetSize(2);
}
```

来看下InsertNodeAfter这个函数，ValueIndex来获取old node的key的位置，+1位new node的位置，IncreaseSize这个node的大小，然后顺位后移idx后面的index，然后将new node对应的key/value放入，这里没有判断是否溢出，是在InsertIntoParent中进行检查的溢出。

```c++
/*
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
INDEX_TEMPLATE_ARGUMENTS
int B_PLUS_TREE_INTERNAL_PAGE_TYPE::InsertNodeAfter(
    const ValueType &old_value, const KeyType &new_key,
    const ValueType &new_value) {
  int idx = ValueIndex(old_value) + 1; // get the index position for the new node
  assert(idx > 0);
  IncreaseSize(1);
  int curSize = GetSize();
  for (int i = curSize - 1; i > idx; i--) {
    array[i].first = array[i - 1].first;
    array[i].second = array[i - 1].second;
  }
  array[idx].first = new_key;
  array[idx].second = new_value;
  return curSize;
}
```

然后是MoveHalfTo函数，用来切半，注意要更改childrens' parent id，其他的具体看注释

```c++
/*
 * Remove half of key & value pairs from this page to "recipient" page
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveHalfTo(
    BPlusTreeInternalPage *recipient,
    BufferPoolManager *buffer_pool_manager) {
  /*
   * internal page is different from leaf page
   * example: maxsize is 4, and this internal page can contains 5 elements,
   * but the first element is the left pointer, the last element is prepared for the element that will lead to split
   * x 1 2 3 4, after spliting x 1 in the old internal node, 2 3 4 is in the new internal node,
   * but 2 will be moved to the parent node in function InsertIntoParent(), so in the new node's first element is just a left pointer
   */
  assert(recipient != nullptr);
  int total = GetMaxSize() + 1;
  assert(GetSize() == total);
  int copyIdx = total / 2;
  page_id_t recipientPageId = recipient->GetPageId();
  for (int i = copyIdx; i < total; i++) {
    recipient->array[i - copyIdx].first = array[i].first;
    recipient->array[i - copyIdx].second = array[i].second;
    // pay attention to that we need adjust the childrens' parent_id
    auto childRawPage = buffer_pool_manager->FetchPage(array[i].second);
    BPlusTreePage *childTreePage = reinterpret_cast<BPlusTreePage *>(childRawPage->GetData());
    childTreePage->SetParentPageId(recipientPageId);
    buffer_pool_manager->UnpinPage(array[i].second, true);
  }
  SetSize(copyIdx);
  recipient->SetSize(total - copyIdx);
}
```

#### b_plus_tree_leaf_page.cpp

看下insert函数，找到第一个大于等于的idx，然后后移，然后插入

```c++
/*
 * Insert key & value pair into leaf page ordered by key
 * @return  page size after insertion
 */
INDEX_TEMPLATE_ARGUMENTS
int B_PLUS_TREE_LEAF_PAGE_TYPE::Insert(const KeyType &key,
                                       const ValueType &value,
                                       const KeyComparator &comparator) {
  int idx = KeyIndex(key, comparator);  // find the first lager key
  assert(idx >= 0);
  IncreaseSize(1);
  int curSize = GetSize();
  for (int i = curSize - 1; i > idx; i--) {  // adjust the position that lager
    array[i].first = array[i - 1].first;
    array[i].second = array[i - 1].second;
  }
  array[idx].first = key;  // insert the key/value
  array[idx].second = value;
  return curSize;
}
```

同样leaf也需要实现MoveHalfTo函数

```c++
/*
 * Remove half of key & value pairs from this page to "recipient" page
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveHalfTo(
    BPlusTreeLeafPage *recipient,
    __attribute__((unused)) BufferPoolManager *buffer_pool_manager) {
  /*
   * leaf page is different from internal page,
   * exmaple: maxsize is 4 , the leaf page can contains 5 elements,
   * because the last element is prepared for the element that will lead to split
   * 0 1 2 3 4, maxsize : 4 ,total : 5, copyIdx = 5/2= 2, so 2 3 4 will move to the new page
   * then we need to adjust some information
   */
  assert(recipient != nullptr);
  int total = GetMaxSize() + 1;
  assert(GetSize() == total);
  int copyIdx = total / 2;
  for (int i = copyIdx; i < total; i++) {
    recipient->array[i - copyIdx].first = array[i].first;
    recipient->array[i - copyIdx].second = array[i].second;
  }
  recipient->SetNextPageId(GetNextPageId());
  SetNextPageId(recipient->GetPageId());
  recipient->SetSize(total - copyIdx);
  SetSize(copyIdx);
}
```

### Iterator

对于我们需要实现Itreator

#### index_iterator.h

一个构造函数，然后isEnd()，然后重载*，表示取key/value。重载++，首先`index_`增加，然后如果溢出，判断是否有下一个leaf page，如果没有返回nullptr，如果存在那么unpin这个page,FecthPage下一个next page，然后`index_`重制为0

```c++
INDEX_TEMPLATE_ARGUMENTS
class IndexIterator {
 public:
  // you may define your own constructor based on your member variables
  IndexIterator(B_PLUS_TREE_LEAF_PAGE_TYPE *leaf, int index, BufferPoolManager *bufferPoolManager);
  ~IndexIterator();

  bool isEnd() {
    return (leaf_ == nullptr) || (index_ >= leaf_->GetSize());
  }

  const MappingType &operator*() {
    return leaf_->GetItem(index_);
  }

  IndexIterator &operator++() {
    index_++;
    if (index_ >= leaf_->GetSize()) {
      page_id_t next = leaf_->GetNextPageId();
      if (next == INVALID_PAGE_ID) {
        leaf_ = nullptr;
      } else {
        bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
        Page *page = bufferPoolManager_->FetchPage(next);
        leaf_ = reinterpret_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(page->GetData());
        index_ = 0;
      }
    }
    return *this;
  }

 private:
  // add your own private member variables here
  int index_;
  B_PLUS_TREE_LEAF_PAGE_TYPE *leaf_;
  BufferPoolManager *bufferPoolManager_;
};
```

#### index_iterator.cpp

然后看下index_iterator.cpp文件中，主要是构造和析构函数

```c++
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::IndexIterator(B_PLUS_TREE_LEAF_PAGE_TYPE *leaf, int index, BufferPoolManager *bufferPoolManager)
    : index_(index), leaf_(leaf), bufferPoolManager_(bufferPoolManager) {}

INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE::~IndexIterator() {
  if (leaf_ != nullptr) {
    bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
  }
}
```

## Fix bug

前面又一些bug需要修

#### buffer_pool_manager.cpp

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
  p->is_dirty_ |= is_dirty;  // pay attion to use | , false can't cover the true flag.

  if (p->GetPinCount() <= 0) {
    return false;
  }

  if (--p->pin_count_ == 0) {
    replacer_->Insert(p);
  }
  return true;
}
```

原来是直接`p->is_dirty = is_dirty`是直接赋值，但是false不应该覆盖原来的true值，应该用或符号来刷新pin

#### lru_replacer.cpp

缺少析构函数原来，会导致内存泄漏

```c++
template<typename T>
LRUReplacer<T>::~LRUReplacer() {
  // 这里需要回收，否则会导致内存泄漏
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

最后跑通了两个test



# 总结

今天先把find和insertion部分搞定，写完后再分析下测试用例和整个架构