#ifndef COMPILE_TIME_ASSERT_H
#define COMPILE_TIME_ASSERT_H

#define COMPILE_TIME_ASSERT(expr) {char COMP_TIME_ASSERT[(expr) ? 1 : -1]; (void)COMP_TIME_ASSERT;}

#endif
