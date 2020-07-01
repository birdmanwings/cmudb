/**
 * LRU implementation
 */
#include "buffer/lru_replacer.h"
#include "page/page.h"

namespace cmudb {

/*
 * Init a doubly linked list to store the nodes
 */
template<typename T>
LRUReplacer<T>::LRUReplacer() {
  head = make_shared<Node>();
  tail = make_shared<Node>();
  head->next = tail;
  tail->prev = head;
}

template<typename T>
LRUReplacer<T>::~LRUReplacer() {
  // 这里需要回收，否则会导致内存泄漏，因为虽然使用了智能指针，但是我们使用的双向链表会导致循环引用的问题导致一直不能释放内存
  // 可以参考一下这篇文章，http://senlinzhan.github.io/2015/04/24/%E6%B7%B1%E5%85%A5shared-ptr/
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

/*
 * Insert value into LRU
 */
template<typename T>
void LRUReplacer<T>::Insert(const T &value) {
  lock_guard<mutex> lock(latch);
  shared_ptr<Node> cur;
  if (map.find(value) != map.end()) {  // if the value has been in the list, refresh this node to the first
    cur = map[value];
    shared_ptr<Node> prev = cur->prev;
    shared_ptr<Node> succ = cur->next;
    prev->next = succ;
    succ->prev = prev;
  } else {  // else insert a new node to the first
    cur = make_shared<Node>(value);
  }
  shared_ptr<Node> fir = head->next;
  cur->next = fir;
  fir->prev = cur;
  cur->prev = head;
  head->next = cur;
  map[value] = cur;  // refresh the map
  return;
}

/* If LRU is non-empty, pop the head member from LRU to argument "value", and
 * return true. If LRU is empty, return false
 */
template<typename T>
bool LRUReplacer<T>::Victim(T &value) {
  lock_guard<mutex> lock(latch);
  if (map.empty()) {
    return false;
  }
  shared_ptr<Node> last = tail->prev;
  tail->prev = last->prev;
  last->prev->next = tail;
  value = last->val;
  map.erase(last->val);
  return true;
}

/*
 * Remove value from LRU. If removal is successful, return true, otherwise
 * return false
 */
template<typename T>
bool LRUReplacer<T>::Erase(const T &value) {
  lock_guard<mutex> lock(latch);
  if (map.find(value) != map.end()) {
    shared_ptr<Node> cur = map[value];
    cur->prev->next = cur->next;
    cur->next->prev = cur->prev;
  }
  return map.erase(value);
}

template<typename T>
size_t LRUReplacer<T>::Size() {
  lock_guard<mutex> lock(latch);
//  return map.size();
  return 1;
}

template
class LRUReplacer<Page *>;
// test only
template
class LRUReplacer<int>;

} // namespace cmudb
