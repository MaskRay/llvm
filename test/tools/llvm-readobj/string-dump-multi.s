# REQUIRES: x86-registered-target
# RUN: llvm-mc -filetype=obj -triple x86_64 %s -o %t.o
# RUN: llvm-readobj -p .a -p .b %t.o | FileCheck %s
# RUN: llvm-readelf -p .a -p .b %t.o | FileCheck %s

# CHECK:      String dump of section '.a':
# CHECK-NEXT: [     0] a
# CHECK-EMPTY:
# CHECK-NEXT: String dump of section '.b':
# CHECK-NEXT: [     0] b
# CHECK-EMPTY:
# CHECK-NEXT: String dump of section '.a':
# CHECK-NEXT: [     0] a

.section .a,"a",@progbits,unique,0
.asciz "a"
.section .b,"a",@progbits
.asciz "b"
.section .a,"a",@progbits,unique,1
.asciz "a"
