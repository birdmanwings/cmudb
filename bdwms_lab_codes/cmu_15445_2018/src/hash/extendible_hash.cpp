#include <list>

#include "hash/extendible_hash.h"
#include "page/page.h"

using namespace std;

namespace cmudb {

/*
 * constructor
 * array_size: fixed array size for each bucket
 */
template<typename K, typename V>
ExtendibleHash<K, V>::ExtendibleHash(size_t size):globalDepth(0), bucketSize(size), bucketNum(1) {
  directories.push_back(make_shared<Bucket>(0));
}

template<typename K, typename V>
ExtendibleHash<K, V>::ExtendibleHash() {
  ExtendibleHash(64);
}

/*
 * helper function to calculate the hashing address of input key
 */
template<typename K, typename V>
size_t ExtendibleHash<K, V>::HashKey(const K &key) const {
  return hash<K>{}(key);
}

/*
 * helper function to return global depth of hash table
 * NOTE: you must implement this function in order to pass test
 */
template<typename K, typename V>
int ExtendibleHash<K, V>::GetGlobalDepth() const {
  lock_guard<mutex> lock(latch);
  return globalDepth;
}

/*
 * helper function to return local depth of one specific bucket
 * NOTE: you must implement this function in order to pass test
 */
template<typename K, typename V>
int ExtendibleHash<K, V>::GetLocalDepth(int bucket_id) const {
  if (directories[bucket_id]) {
    lock_guard<mutex> lck(directories[bucket_id]->latch);
    if (directories[bucket_id]->kmap.size() == 0) {
      return -1;
    }
    return directories[bucket_id]->localDepth;
  }
  return -1;
}

/*
 * helper function to return current number of bucket in hash table
 */
template<typename K, typename V>
int ExtendibleHash<K, V>::GetNumBuckets() const {
  lock_guard<mutex> lock(latch);
  return bucketNum;
}

/*
 * lookup function to find value associate with input key
 */
template<typename K, typename V>
bool ExtendibleHash<K, V>::Find(const K &key, V &value) {
  int idx = getIdx(key);
  lock_guard<mutex> lck(directories[idx]->latch);
  if (directories[idx]->kmap.find(key) != directories[idx]->kmap.end()) {
    value = directories[idx]->kmap[key];
    return true;
  }
  return false;
}

/*
 *  helper function to get the index
 */
template<typename K, typename V>
int ExtendibleHash<K, V>::getIdx(const K &key) const {
  lock_guard<mutex> lock(latch);
  return HashKey(key) & ((1 << globalDepth) - 1);  // return globalDepth length LSBs of HashKey(key)
}

/*
 * delete <key,value> entry in hash table
 * Shrink & Combination is not required for this project
 */
template<typename K, typename V>
bool ExtendibleHash<K, V>::Remove(const K &key) {
  int idx = getIdx(key);
  lock_guard<mutex> lck(directories[idx]->latch);
  shared_ptr<Bucket> cur = directories[idx];  // use smart pointer to simplify memory management
  if (cur->kmap.find(key) == cur->kmap.end()) {
    return false;
  }
  cur->kmap.erase(key);
  return true;
}

/*
 * insert <key,value> entry in hash table
 * Split & Redistribute bucket when there is overflow and if necessary increase
 * global depth
 */
template<typename K, typename V>
void ExtendibleHash<K, V>::Insert(const K &key, const V &value) {
  int idx = getIdx(key);
  shared_ptr<Bucket> cur = directories[idx];  // get the specific bucket according to the key
  while (true) {  // maybe it isn't enough to complete the insert the data in only one round
    lock_guard<mutex> lck(cur->latch);
    if (cur->kmap.find(key) != cur->kmap.end() || cur->kmap.size() < bucketSize) {
      cur->kmap[key] = value;
      break;
    }
    // from here, deal with the problem about the spliting
    int mask = (1
        << (cur->localDepth));  // mask means higher one bit to judge the entry is in old or new bucket.
    cur->localDepth++;

    {  // pay attention to this scope, it should be locked when different threads modify the directory
      lock_guard<mutex> lock(latch);  // lock the dictionary
      if (cur->localDepth > globalDepth) {
        size_t length = directories.size();
        for (size_t i = 0; i < length; i++) {
          directories.push_back(directories[i]);
        }
        globalDepth++;
      }
      bucketNum++;
      auto newBuc = make_shared<Bucket>(cur->localDepth);  // create a new bucket with the new localDepth

      typename map<K, V>::iterator iter;
      for (iter = cur->kmap.begin(); iter != cur->kmap.end();) {  // rehash each entry with a new local depth
        if (HashKey(iter->first) &
            mask) {  // if the higher bit is 1, allocate this entry to new bucket, and delete it from old bucket
          newBuc->kmap[iter->first] = iter->second;
          iter = cur->kmap.erase(iter);  // erase return the next iter
        } else {  // else keep it in old bucket
          iter++;
        }
      }

      for (size_t i = 0;
           i < directories.size(); i++) {  // assign each directory point to the correct the bucket
        if (directories[i] == cur && (i & mask)) {
          directories[i] = newBuc;
        }
      }
    }
    idx = getIdx(key);
    cur = directories[idx];  // get the current bucket, maybe it's still full so we need use while loop, until we find the empty position for the key.
  }
}

template
class ExtendibleHash<page_id_t, Page *>;

template
class ExtendibleHash<Page *, std::list<Page *>::iterator>;

// test purpose
template
class ExtendibleHash<int, std::string>;

template
class ExtendibleHash<int, std::list<int>::iterator>;

template
class ExtendibleHash<int, int>;
} // namespace cmudb
