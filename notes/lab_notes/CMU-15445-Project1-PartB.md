# 前言

实现一个LRU算法，感觉比较简单，开始吧。

# 正文

LRU算法的原理这里不再赘述，网上太多了，直接来看我们的代码。

首先看下结构体，我们先声明一个Node节点

```c++
struct Node {
    Node() {};
    Node(T val) : val(val) {};
    T val;
    shared_ptr<Node> prev;
    shared_ptr<Node> next;
  };
```

我们这里用双向链表来组织。

成员变量

```c++
shared_ptr<Node> head;
shared_ptr<Node> tail;
unordered_map<T, shared_ptr<Node>> map;
mutable mutex latch;
```

用unordered_map来存储k和对应的节点，latch要声明为mutable的才能让const函数来改变

然后就是我们cpp里面的实现，看下insert，如果这个value存在就刷新他到前面去，否则就插入一个新的节点到head后面。

```c++
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
```

victim用来删除最老的那个节点

```c++
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
```

Erase删除某个节点

```c++
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
```

最后pass过test

# 总结

lru还是很简单的，就是用一个双向链表来组织。