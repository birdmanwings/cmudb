#!/usr/bin/env bash
for ((i=0;i<200;i++));
do
  if !(./cmake-build-debug/test/lock_manager_test &> ./res/$i); then
    exit
  else
    echo $i;
  fi
done