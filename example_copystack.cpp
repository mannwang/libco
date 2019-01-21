/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include "coctx.h"
#include "co_routine.h"
#include "co_routine_inner.h"

void* RoutineFunc(void* args)
{
	co_enable_hook_sys();
	int* routineid = (int*)args;
	while (true)
	{
		char sBuff[128];
		sprintf(sBuff, "from routineid %d stack addr %p\n", *routineid, sBuff);

		printf("%s", sBuff);
		poll(NULL, 0, 1000); //sleep 1s
		sprintf(sBuff, "end routineid %d stack addr %p\n", *routineid, sBuff);
		printf("%s", sBuff);
    break;
	}
	return NULL;
}

int main()
{
	stShareStack_t* share_stack= co_alloc_sharestack(1, 1024 * 128);
	stCoRoutineAttr_t attr;
	attr.stack_size = 0;
	attr.share_stack = share_stack;

	stCoRoutine_t* co[2];
	int routineid[2];
  //i=0时,创建第一个协程,co_resume切换协程执行,主协程接管main栈空间
  //      第一个协程在poll中添加到1s超时队列中,(NULL,0)未传入任何要处理事件,co_swap切回主协程
  //      co_swap返回到co_resume,再返回到main
  //i=1时,创建第二个协程,co_resume切换第二个协程执行
  //      第二个协程同第一个协程
  //      返回到主协程
  //i=2,for循环结束
  //co_eventloop()轮询等待事件触发,直至超时触发i=0和i=1协程执行
  //协程内部是while死循环,故一直等待->超时->等待->超时...
	for (int i = 0; i < 2; i++)
	{
		routineid[i] = i;
		co_create(&co[i], &attr, RoutineFunc, routineid + i);
		co_resume(co[i]);
	}
	co_eventloop(co_get_epoll_ct(), NULL, NULL);
	return 0;
}
