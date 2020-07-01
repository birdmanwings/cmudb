# 前言

CMU15445 Project1关于缓存池的部分实现，因为18年代码没开源，使用的是17年的代码，首先是第一部分的EXTENDIBLE HASH TABLE的实现，https://github.com/birdmanwings/CMU-15445。

# 正文

part1是关于实现一个extendible hash table（可拓展哈希表？），具体的原理看这篇[文章](https://www.geeksforgeeks.org/extendible-hashing-dynamic-approach-to-dbms/)写的很详细并且通俗易懂。大致上就是利用dictory和bucket结构来简化拓展，采用动态拓展的方式让hash更加灵活。

优点：

- 数据恢复更简单
- 不存在数据丢失因为动态拓展的特性
- 随着哈希函数的动态改变，相关的旧值会被新的哈希函数重新计算

缺点：

- 如果记录分布的不均匀，可能多个dictory的id会映射到同一个bucket上，导致dictory过大（因为dictory是以指数增长的）
- 每一个bucket的大小是固定的
- 大量的空间会被浪费如果global depth和local depth相差过大的话
- 代码实现很复杂

然后就正式开始我们的project吧，这个project只用考虑扩张，不用考虑压缩的情况。首先是要考虑我们的结构是怎么组织的。

```c++
template<typename K, typename V>
    class ExtendibleHash : public HashTable<K, V> {
        struct Bucket {
            Bucket(int depth) : localDepth(depth) {};
            int localDepth;
            map<K, V> kmap;
            mutex latch;
        };

    public:
        // constructor
        ExtendibleHash(size_t size);

        ExtendibleHash();

        // helper function to generate hash addressing
        size_t HashKey(const K &key) const;

        // helper function to get global & local depth
        int GetGlobalDepth() const;

        int GetLocalDepth(int bucket_id) const;

        int GetNumBuckets() const;

        // lookup and modifier
        bool Find(const K &key, V &value) override;

        bool Remove(const K &key) override;

        void Insert(const K &key, const V &value) override;

        int getIdx(const K &key) const;

    private:
        // add your own member variables here
        int globalDepth;
        size_t bucketSize;
        int bucketNum;
        vector<shared_ptr<Bucket>> directories;
        mutable mutex latch;
    };
} // namespace cmudb
```

这里面我们看一下基本结构

![](http://image.bdwms.com/Basic-Structure-of-Extendible-Hashing.png)

首先我们声明一个Bucket的struct

```c++
struct Bucket {
            Bucket(int depth) : localDepth(depth) {};
            int localDepth;
            map<K, V> kmap;
            mutex latch;
        };
```

使用一个latch来对单独的bucket加锁。

然后是一些成员变量：

```c++
int globalDepth;
size_t bucketSize;
int bucketNum;
vector<shared_ptr<Bucket>> directories;
mutable mutex latch;
```

bucketSize是每个bucket的大小，bucketNum是bucket的数量，然后directories是一个用来指向bucket的vector，然后我们用shared_ptr智能指针来帮助我们简化内存的管理不用手动new，delete，然后directories也需要一个锁，然后这个锁应该是mutable的，因为是对外无感知的且需要在内部变化。

然后我们看一下extendible_hash.cpp中，在GetGlobalDepth，GetLocalDepth，GetNumBuckets时都需要注意加锁，然后我们在实现Find之前需要实现个getIdx来获取索引的操作，来看一下这个实现

```c++
template<typename K, typename V>
    int ExtendibleHash<K, V>::getIdx(const K &key) const {
        lock_guard<mutex> lock(latch);
        return HashKey(key) & ((1 << globalDepth) - 1);  // return globalDepth length LSBs of HashKey(key)
    }
```

`HashKey(key) & ((1 << globalDepth) - 1)`，无符号数左移补零，减一后变成了0...11111，有globalDepth个1，所以最后取的是低位的globalDepth长度的hash值。

然后看一下Remove的时候

```c++
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
```

注意这里要使用智能指针来分配回收Bucket。

最后最难的是Insert的操作，分为几种情况，当hash到的bucket没有满或者找到是同一个（因为我们这里要用下while循环防止一次拓展不够，因为可能高一位还是一样的还是冲突），那么直接插入就行了。然后当bucket满的时候，存在两个情况，一个是global depth等于local depth时需要先吧directories翻倍，然后再分裂bucket，然后重新用新的hash函数来将冲突的bucket中的entry重新hash后（取localdepth长度的低位）分配到新或旧的bucket中，然后将directories中的每块指向正确的bucket。另一个情况是global depth大于local depth时只需要分裂bucket，不需要翻倍direcrtories了，其他都一样。注意`lock_guard<mutex> lock(latch)`在锁上direcotries时需要用大括号来限制范围。

```c++
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
                auto newBuc = make_shared<Bucket>(cur->localDepth);

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
            cur = directories[idx];
        }
    }
```

最后project没有给出完整的测试代码，我用了网上一个大佬写的测试样例orz，可以全部跑通，clion可以自动导入cmake然后挺好用的，替换掉测试样例后，跑成功了。

# 总结

今天挤时间写完的，，，继续努力吧Orz，有空我把那篇EXTENDIBLE HASH TABLE翻译一下。