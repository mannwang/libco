#include "co_routine.h"
#include <stdio.h>
#include <iostream>

using std::cout;
using std::endl;

void *task0(void *) {
  cout << "1";
  co_yield_ct();
  cout << "2";
  return 0;
}
void *task2(void *) {
  cout << "x";
  co_yield_ct();
  cout << "y";
  return 0;
}

int main() {
	stShareStack_t* share_stack= co_alloc_sharestack(1, 1024 * 128);
	stCoRoutineAttr_t attr;
	attr.stack_size = 0;
	attr.share_stack = share_stack;

  stCoRoutine_t *c0 = 0;
  stCoRoutine_t *c2 = 0;
  co_create( &c0,&attr,task0,NULL );
  co_create( &c2,&attr,task2,NULL );
  co_resume( c0 );
  co_resume( c2 );
  co_resume( c0 );
  co_resume( c2 );
  cout << endl;
  return 0;
}
