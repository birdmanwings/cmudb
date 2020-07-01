# 前言

B+树的删除功能的实现，也很复杂，来看一下吧，然后对前面的代码要做一些修正。

# 正文

首先我们来看下b_plus_tree.cpp文件里面的remove函数，从这里是remove的入口：

```c++
/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immdiately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  if (IsEmpty()) {
    return;
  }
  B_PLUS_TREE_LEAF_PAGE_TYPE *tar = FindLeafPage(key);
  int curSize = tar->RemoveAndDeleteRecord(key, comparator_); // get the size after the deletion
  if (curSize
      < tar->GetMinSize()) {  // if the current size is smaller than min size, the page needs to be coalesce or redistribute
    CoalesceOrRedistribute(tar, transaction);
  }
  buffer_pool_manager_->UnpinPage(tar->GetPageId(), true);
  assert(Check());
}
```

先是为空就直接返回，然后找到存在这个key的leaf page，然后调用RemoveAndDeleteRecord函数，返回的是删除后的叶子大小，然后如果是小于最小size会调用CoalesceOrRedistribute函数来进行合并或者重新分配，最后记住unpin一下tar page。

然后我们看下RemoveAndDeleteRecord函数，在leaf page文件中实现

```c++
/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * First look through leaf page to see whether delete key exist or not. If
 * exist, perform deletion, otherwise return immdiately.
 * NOTE: store key&value pair continuously after deletion
 * @return   page size after deletion
 */
INDEX_TEMPLATE_ARGUMENTS
int B_PLUS_TREE_LEAF_PAGE_TYPE::RemoveAndDeleteRecord(
    const KeyType &key, const KeyComparator &comparator) {
  int firstKeyIndex = KeyIndex(key, comparator);  // find the first key index larger or equal
  if (firstKeyIndex >= GetSize()
      || comparator(key, KeyAt(firstKeyIndex)) != 0) {  // if not found the key, or the key != firstKeyIndex, return
    return GetSize();
  }
  // delete the key/value in the tar position using memmove function
  int tar = firstKeyIndex;
  memmove(array + tar, array + tar + 1, static_cast<size_t>((GetSize() - tar - 1) * sizeof(MappingType)));
  IncreaseSize(-1);
  return GetSize();
}
```

看下删除，首先找到第一个大于等于key值的index，如果firstKeyIndex大于size或者比较找到的firstKeyindex不等于key值，说明没找到相应的key直接返回size大小就行了。找到的话，就利用memmove函数来把后一位开始的element进行移位，注意这里是指针哦，然后大小减一，返回size。

看下CoalesceOrRedistribute是怎么做的最复杂的部分

```c++
/*
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
bool BPLUSTREE_TYPE::CoalesceOrRedistribute(N *node, Transaction *transaction) {
  // if this is a root node
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }
  N *siblingNode;
  bool isSuffix = FindSibling(node, siblingNode); // true means sibling node is back, false means front
  BPlusTreePage *parent = FetchPage(node->GetParentPageId());
  B_PLUS_TREE_INTERNAL_PAGE *parentPage = static_cast<B_PLUS_TREE_INTERNAL_PAGE *>(parent);
  // if the sum size < max size, coalesce two node
  if (node->GetSize() + siblingNode->GetSize() <= node->GetMaxSize()) {
    if (isSuffix) {
      swap(node, siblingNode);
    }
    int removeIndex = parentPage->ValueIndex(node->GetPageId());  // get the page id will be removed
    Coalesce(siblingNode, node, parentPage, removeIndex, transaction);
    buffer_pool_manager_->UnpinPage(parentPage->GetPageId(), true);
    return true;
  }
  // redistribute the node, borrow the element from sibling node
  int nodeInParentIndex = parentPage->ValueIndex(node->GetPageId());
  Redistribute(siblingNode, node, nodeInParentIndex);
  buffer_pool_manager_->UnpinPage(parentPage->GetPageId(), false);
  return false;
}
```

如果需要合并或者重新分配的是根节点那么需要调整root结点，来看下AdjustRoot函数。我想先来说下根结点的存在方式，根节点是internal page，所以对于第一个index是invalid的，key是空的，value是个左指针指向page或者下面的internal page，然后如果只有大小为1的话那么value指向的是一个page也就是一个leaf page，这种情况的话，我们需要用这个value指向的page来替换我们原来的root page，还有一种情况是直接空了index 0的地方value也是空了的话，那么可以直接删除B+树了，然后我们细看下整个代码。

```c++
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::AdjustRoot(BPlusTreePage *old_root_node) {
  if (old_root_node->IsLeafPage()) {  // case 2, the root page size < GetMinSize() = 1, so it points to null, delete the tree
    assert(old_root_node->GetSize() == 0);
    assert(old_root_node->GetParentPageId() == INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), false);  // unpin and delete the page
    buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId();
    return true;
  }
  if (old_root_node->GetSize()
      == 1) {  // case 1, if there is only one element left, get the page id from the element as the root page
    B_PLUS_TREE_INTERNAL_PAGE *root = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(old_root_node);
    const page_id_t newRootId = root->RemoveAndReturnOnlyChild(); // remove the key/value and return the value(page id)
    root_page_id_ = newRootId;
    UpdateRootPageId();
    Page *page = buffer_pool_manager_->FetchPage(root_page_id_);
    assert(page != nullptr);
    B_PLUS_TREE_INTERNAL_PAGE *newRoot = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(page->GetData());
    newRoot->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(root_page_id_, true);
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), false);
    buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
  }
  return false;
}
```

对于第二种情况，如果这个root page同时也是leaf page，因为能触发这里的话，说明root page size < GetMinSize() = 1，说明这个树空了，直接先unpin这个root page，然后delete这个root page，然后记得更新下root_page_id_因为要更新header信息，然后看下第一种情况，只有index 1的地方有个value指向leaf page，然后我们要拿这个page替换root page,

先是RemoveAndReturnOnlyChild函数，拿到0处的value值，然后size-1，然后return value拿到新的root node的page id

```c++
/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 */
INDEX_TEMPLATE_ARGUMENTS
ValueType B_PLUS_TREE_INTERNAL_PAGE_TYPE::RemoveAndReturnOnlyChild() {
  ValueType ret = ValueAt(0);
  IncreaseSize(-1);
  assert(GetSize() == 0);
  return ret;
}
```

然后更新root_page_id_，然后拿到新的page页面进行一次强制转换为internal page，然后更新new root id的信息，然后unpin原来的和新的root page，然后删除老的page。

返回之后我们继续看因为不是根结点的话，需要寻找他的兄弟结点，我们优先找它的前兄弟结点，只有它没有前兄弟的时候才找后兄弟，我们自己添加一个FindSibling函数

```c++
/*
 * Find the sibling node for coalesce or redistribute, first use pre node otherwise use post node
 * true mean pre, flase mean post
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
bool BPLUSTREE_TYPE::FindSibling(N *node, N *&sibling) {
  auto page = FetchPage(node->GetParentPageId());  // we need get the parent page first for searching sibling node
  B_PLUS_TREE_INTERNAL_PAGE *parent = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(page);
  int index = parent->ValueIndex(node->GetPageId());
  int siblingIndex = index - 1;
  if (index == 0) {
    siblingIndex = index + 1;
  }
  sibling = reinterpret_cast<N *>(FetchPage(parent->ValueAt(siblingIndex)));
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), false);
  return index == 0;
}
```

因为我们需要寻找兄弟结点，首先需要寻找父节点（因为可能是internal page要找兄弟），找到parent node后先找到当前node的index然后找到sibling node index,记得unpin parent page，然后又分为两种情况，如果sibling node和node合并大小小于max时候合并，否则重新分配他们，然后分两部分看下。

先看合并部分，

```c++
// if the sum size < max size, coalesce two node
if (node->GetSize() + siblingNode->GetSize() <= node->GetMaxSize()) {
  if (isSuffix) {
    swap(node, siblingNode);
  }
  int removeIndex = parentPage->ValueIndex(node->GetPageId());  // get the page id will be removed
  Coalesce(siblingNode, node, parentPage, removeIndex, transaction);
  buffer_pool_manager_->UnpinPage(parentPage->GetPageId(), true);
  return true;
}
```

我们要保证siblingNode里面的值是前面的值，所以先交换，然后我们拿到需要被remove的page id（我们需要将node的所有element移到钱买呢的sibling page node）,然后我们看下Coalsece函数是怎么具体的合并，最后记得unpin parent page

```c++
/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
bool BPLUSTREE_TYPE::Coalesce(
    N *&neighbor_node, N *&node,
    BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *&parent,
    int index, Transaction *transaction) {  // we think neighbor_node is before the node
  assert(node->GetSize() + neighbor_node->GetSize() <= node->GetMaxSize());
  node->MoveAllTo(neighbor_node, index, buffer_pool_manager_); // move the elements in node to neighbor_node
  page_id_t pId = node->GetPageId();
  buffer_pool_manager_->UnpinPage(pId, true);
  buffer_pool_manager_->DeletePage(pId);
  buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
  parent->Remove(index);
  // pay attention to the <=, because it's a internal page, effective key/value will decrease 1 because the first index is invalid
  if (parent->GetSize()
      <= parent->GetMinSize()) { // if the parent size is less than min size, it will coalesce or redistribute recursively
    return CoalesceOrRedistribute(parent, transaction);
  }
  return false;
}
```

调用MoveAllTo函数来迁移element，这个具体在internal page和leaf page中实现的等会再说，记得unpin自己这个page，然后delete，还要unpin neighbor_node，最后从parent中移除自己的index，简单看下Remove就是简单的array移动下

```c++
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array offset)
 * NOTE: store key&value pair continuously after deletion
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Remove(int index) {
  assert(index >= 0 && index < GetSize());
  for (int i = index + 1; i < GetSize(); i++) {
    array[i - 1] = array[i];
  }
  IncreaseSize(-1);
}
```

记得有可能因为parent node减小后也会触发，**注意这里是<=，为什么呢，因为我们internal page的第一个index是invalid的，只有value没有key，所以是小于等于**。所以需要递归的调用。

之后我们回来看下重新分配

```c++
// redistribute the node, borrow the element from sibling node
int nodeInParentIndex = parentPage->ValueIndex(node->GetPageId());
Redistribute(siblingNode, node, nodeInParentIndex);
buffer_pool_manager_->UnpinPage(parentPage->GetPageId(), false);
```

我们先拿下在parent中当前node的index值，然后调用Redistribute

```c++
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
void BPLUSTREE_TYPE::Redistribute(N *neighbor_node, N *node, int index) {
  // index is the node index in the parent page
  if (index == 0) {  // if the node is first, get the first element in back node
    neighbor_node->MoveFirstToEndOf(node, buffer_pool_manager_);
  } else {  // else get the last element from the front node
    neighbor_node->MoveLastToFrontOf(node, index, buffer_pool_manager_);
  }
  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
}
```

然后看下其中的逻辑，如果node是第一个，那么就把后面结点的第一个element移到node的末尾，否则话就把前面的node的最后一个element移到node的first位置，这两个函数也是leaf page和internal page中具体实现的，最后记得unpin这两个node。

之后我们来看下之前的几个函数在internal page和leaf page中的具体实现

先看MoveAllTo函数在leaf page

```c++
/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all of key & value pairs from this page to "recipient" page, then
 * update next page id
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveAllTo(BPlusTreeLeafPage *recipient,
                                           int, BufferPoolManager *) {
  assert(recipient != nullptr);
  int startIdx = recipient->GetSize();
  for (int i = 0; i < GetSize(); i++) {
    recipient->array[startIdx + i].first = array[i].first;
    recipient->array[startIdx + i].second = array[i].second;
  }
  recipient->SetNextPageId(GetNextPageId());  // adjust the next point
  recipient->IncreaseSize(GetSize()); // adjust the page size
  SetSize(0);
}
```

把node的element都移到recipient中，然后更新recipient page的下一个page,再更新recipient size，自己的size设置为0

然后是internal page中的MoveAllTo，这个比较麻烦

```c++
/*
 * Remove all of key & value pairs from this page to "recipient" page, then
 * update relavent key & value pair in its parent page.
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveAllTo(
    BPlusTreeInternalPage *recipient, int index_in_parent,
    BufferPoolManager *buffer_pool_manager) {
  int startIdx = recipient->GetSize();
  page_id_t recipientPageId = recipient->GetPageId();
  Page *parentPage = buffer_pool_manager->FetchPage(GetParentPageId());
  assert(parentPage != nullptr);
  BPlusTreeInternalPage *parent = reinterpret_cast<BPlusTreeInternalPage *>(parentPage->GetData());

  // set the key in parent at the node's invalid key position, and then move all node elements to recipient page
  SetKeyAt(0, parent->KeyAt(index_in_parent));
  buffer_pool_manager->UnpinPage(parent->GetPageId(), false);
  for (int i = 0; i < GetSize(); i++) {
    recipient->array[startIdx + i].first = array[i].first;
    recipient->array[startIdx + i].second = array[i].second;
    // update the children's parent page
    auto childRawPage = buffer_pool_manager->FetchPage(array[i].second);
    BPlusTreePage *childTreePage = reinterpret_cast<BPlusTreePage *>(childRawPage->GetData());
    childTreePage->SetParentPageId(recipientPageId);
    buffer_pool_manager->UnpinPage(array[i].second, true);
  }

  recipient->SetSize(startIdx + GetSize());
  assert(recipient->GetSize() <= GetMaxSize());
  SetSize(0);
  buffer_pool_manager->UnpinPage(GetPageId(), true);
  buffer_pool_manager->UnpinPage(recipient->GetPageId(), true);
}
```

对于internal page的move all需要先拿到parent page然后将这个node在parent中的index所对应的key，需要下移到对应的结点的index 0处就是invalid处，值下放后再将node移动到recipient处，注意因为是internal page，所以需要更新node的children中的parent id，注意每一个都要unpin一下。

然后我们来看leaf page中的MoveFirstToEndOf和MoveLastToFrontOf两个函数，都比较类似，这里直接贴下代码，主要就是element迁移

```c++
/*
 * Remove the first key & value pair from this page to "recipient" page, then
 * update relavent key & value pair in its parent page.
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveFirstToEndOf(
    BPlusTreeLeafPage *recipient,
    BufferPoolManager *buffer_pool_manager) {
  MappingType pair = GetItem(0);  // get the first element
  IncreaseSize(-1);
  // move the element from index 1 to end
  memmove(array, array + 1, static_cast<size_t>(GetSize() * sizeof(MappingType)));
  recipient->CopyLastFrom(pair);  // copy pair to last recipient position
  Page *page = buffer_pool_manager->FetchPage(GetParentPageId());
  B_PLUS_TREE_INTERNAL_PAGE *parent = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(page->GetData());

  // update the parent pointer to neighbor_node
  parent->SetKeyAt(parent->ValueIndex(GetPageId()), array[0].first);
  buffer_pool_manager->UnpinPage(GetParentPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::CopyLastFrom(const MappingType &item) {
  assert(GetSize() + 1 <= GetMaxSize());
  array[GetSize()] = item;
  IncreaseSize(1);
}
/*
 * Remove the last key & value pair from this page to "recipient" page, then
 * update relavent key & value pair in its parent page.
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveLastToFrontOf(
    BPlusTreeLeafPage *recipient, int parentIndex,
    BufferPoolManager *buffer_pool_manager) {
  MappingType pair = GetItem(GetSize() - 1);
  IncreaseSize(-1);
  recipient->CopyFirstFrom(pair, parentIndex, buffer_pool_manager);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::CopyFirstFrom(
    const MappingType &item, int parentIndex,
    BufferPoolManager *buffer_pool_manager) {
  assert(GetSize() + 1 < GetMaxSize());
  memmove(array + 1, array, GetSize() * sizeof(MappingType));
  IncreaseSize(1);
  array[0] = item;

  Page *page = buffer_pool_manager->FetchPage(GetParentPageId());
  B_PLUS_TREE_INTERNAL_PAGE *parent = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE *>(page->GetData());
  parent->SetKeyAt(parentIndex, array[0].first);
  buffer_pool_manager->UnpinPage(GetParentPageId(), true);
}
```

然后是internal page中的两个move，但是会多了更新Childers's parent id，不贴代码了。

最后需要修几个前面的bug，一个是buffer_pool_manager中flush的最后应该是return true，还有DeletePage中需要记得更新p的page id为INVALID_PAGE_ID

还有在index_iterator在unpin leaf page时应该不管什么情况都要unpin

```c++
IndexIterator &operator++() {
  index_++;
  if (index_ >= leaf_->GetSize()) {
    page_id_t next = leaf_->GetNextPageId();
    bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
    if (next == INVALID_PAGE_ID) {
      leaf_ = nullptr;
    } else {
      //bufferPoolManager_->UnpinPage(leaf_->GetPageId(), false);
      Page *page = bufferPoolManager_->FetchPage(next);
      leaf_ = reinterpret_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(page->GetData());
      index_ = 0;
    }
```

最后一些check函数和test到写完再分析，太菜了写不动

# 总结

B+树写的难度我是不会写了，只能看了，先读懂实现吧，等最后一致性完成后再做总的分析。