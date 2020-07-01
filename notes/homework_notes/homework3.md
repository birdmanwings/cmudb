# 前言

Homework3，主要涉及的是DBMS的一些操作优化和算法，我们简单过一下

# 正文

## Q1 SortingAlgorithms

We have a database file with a million pages (N = 4,000,000 pages), and we want to sort it using external merge sort. Assume that the DBMS is not using double buffering or blocked I/O, and that it uses quicksort for in-memory sorting. Let B denote the number of buffers.

1. Assume that the DBMS has six buffers. How many passes does the DBMS need to perform in order to sort the file?

   外排序直接套公式：1 + ceil(logB−1(ceil(N/B))) = 1 + ceil(log5(666, 667)) = 10

   1代表第一次读入每B个pages，就是一次扫描，然乎为什么是B-1呢，因为需要留一个buffer来做输出，然后总共有N/B个page需要被merge，所以最后是10

2. Again, assuming that the DBMS has six buffers. What is the total I/O cost to sort the file?

   Cost=2×N×#passes=2×4,000,000×10，因为是一次读一次写所以是2

3. What is the smallest number of buffers B that the DBMS can sort the target file using only two passes?

   We want B where N ≤B∗(B−1) If B=2001,then 4,000,000 ≤ 2001∗ 2000 = 4, 002, 000; smaller B, fails.

   什么意思呢，就是把公式两边转化下就行了，-1是开头那个1做初始化排序

4. What is the smallest number of buffers B that the DBMS can sort the target file using only three passes?

   B ∗ (B − 1)^2 = 160 ∗ 159 ∗ 159 = 4, 044, 960. Anything less, fails.

## Q2 Join Algorithms

Consider relations R(a, b) and S(a, c, d) to be joined on the common attribute a. Assume that there are no indexes.

- There are B = 50 pages in the buffer
- Table R spans M = 2000 pages with 80 tuples per page 
- Table S spans N = 300 pages with 40 tuples per page

Answer the following questions on computing the I/O costs for the joins. You can assume the simplest cost model, where pages are read and written one at a time. You can also assume that you will need one buffer block to hold the evolving output block and one input block to hold the current input block of the inner relation. You may ignore the cost of the final writing of the results.

1. Block nested loop join with R as the outer relation and S as the inner relation:

   M + ceil(M/(B − 2)) × N = 2000 + ceil(2000/48) × 300 = 14, 600

   因为需要一个读，一个写所以是B-2个缓冲池可以利用

2. Block nested loop join with S as the outer relation and R as the inner relation

   N + ceil(N/(B − 2)) × M = 300 + ceil(300/48) × 2000 = 14, 300

3. Sort-merge join with S as the outer relation and R as the inner relation:

   1. What is the cost of sorting the tuples in R on attribute a?

      2 ∗ M ∗ log(M)/log(B) = 2 ∗ 2000 ∗ log(2000) / log(50) = 7772

   2. What is the cost of sorting the tuples in S on attribute a?

      2 ∗ N ∗ log(N) / log(B) = 2 ∗ 300 ∗ log(300) / log(50) = 875

   3. What is the cost of the merge phase assuming there are no duplicates in the join attribute?

      M + N = 2300

   4. What is the cost of the merge phase in the worst case scenario?

      M ∗ N = 600000，最坏的情况是对于两张表包含同样的value,

4. Hash join with S as the outer relation and R as the inner relation. You may ignore recursive

   partitioning and partially filled blocks.

   Hash join的大致原理看下[这篇文章](https://zhuanlan.zhihu.com/p/121301503)

   1. What is the cost of the partition phase?

      2*(M+N)=2*(2000+300)=4600 分块阶段用第一个hash函数将两个表分块，一次读一次写

   2. What is the cost of the probe phase?

      (M+N)=(2000+300)=2300 探测阶段，只要读入表的相应属性来做Hash的比较

# 总结

大概了解了下排序，聚合，联合的几种算法，没有细看具体数学证明。