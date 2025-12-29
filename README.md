# feipiaocc

个人玩具编译器，目标: 仿写 chibicc，尽可能支持 c11 完整语法，所有代码除了编译时用到的 include 放在单文件内

编译:

```bash
gcc -std=c11 -g -fno-common -Wall -Wno-switch -o feipiaocc feipiaocc.c
```