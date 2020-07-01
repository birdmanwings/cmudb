#!/usr/bin/env bash
for ((i=0;i<1000;i++));
do
  if !(./cmake-build-debug/test/b_plus_tree_concurrent_test &> ./res/$i); then
    exit
  else
    echo $i;
  fi
done