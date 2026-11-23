#pragma once

void replay_cpu_switch_in(void *task);
void replay_cpu_switch_out(void *task);

#define traceTASK_SWITCHED_IN() replay_cpu_switch_in(pxCurrentTCBs[portGET_CORE_ID()])
#define traceTASK_SWITCHED_OUT() replay_cpu_switch_out(pxCurrentTCBs[portGET_CORE_ID()])
