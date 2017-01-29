#ifndef COMPILE_TIME_ASSERT_H
#define COMPILE_TIME_ASSERT_H

#define COMPILE_TIME_ASSERT(expr) {typedef char COMP_TIME_ASSERT[(expr) ? 1 : 0];}

#endif
