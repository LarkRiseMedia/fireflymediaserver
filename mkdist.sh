#!/bin/bash

mv configure.in configure.in.mkdist
cat configure.in.mkdist | sed -e s/AM_INIT_AUTOMAKE.*$/AM_INIT_AUTOMAKE\(mt-daapd,cvs-20040318\)/ > configure.in
./reconf
./configure
make dist
mv configure.in.mkdist configure.in

