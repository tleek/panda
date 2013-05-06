#!/bin/sh

#./configure --target-list=i386-softmmu,i386-linux-user,\
#arm-linux-user,x86_64-linux-user,x86_64-softmmu \
./configure --target-list=arm-softmmu \
--prefix=`pwd`/install \
--disable-pie \
--extra-cflags="-O2" \
--extra-cxxflags="-O2" \
&& make -j8

#,x86_64-linux-user,x86_64-softmmu\
#,arm-linux-user,arm-softmmu\

#--enable-instr-helpers \
#--with-laredo=../llvm-3.0/projects/laredo/Release+Debug+Asserts \

