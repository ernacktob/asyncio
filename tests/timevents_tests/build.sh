#!/bin/sh

gcc -Wall -Wextra -Werror -pedantic test1.c ../../obj/*.o -o test1
gcc -Wall -Wextra -Werror -pedantic test2.c ../../obj/*.o -o test2
gcc -Wall -Wextra -Werror -pedantic test3.c ../../obj/*.o -o test3
