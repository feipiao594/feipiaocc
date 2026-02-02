gcc -std=c11 -g -fno-common -Wall -Wno-switch -o feipiaocc feipiaocc.c
./feipiaocc feipiaocc.c --tokens --no-codegen > token.txt