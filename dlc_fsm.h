#pragma once
#include <sys/cdefs.h>
#include <stdint.h>
#include <pthread.h>
#include "efd_sem.h"

__BEGIN_DECLS

typedef void (*ev_handler)(int);

#define DLC_FSM_EV_LIMIT 2
typedef struct dlc_fsm {
	int run_flag;
	pthread_t thr_id;

	efd_sem_t sem;
	volatile uint64_t ev_rd;
	volatile uint64_t ev_wr;
	ev_handler ev_cb;
	int ev_q[DLC_FSM_EV_LIMIT];
}dlc_fsm_t;

int dlc_fsm_init(dlc_fsm_t* self, ev_handler cb);
int dlc_fsm_fini(dlc_fsm_t* self);
int dlc_fsm_sign(dlc_fsm_t* self, int ev_new);

__END_DECLS