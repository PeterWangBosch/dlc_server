#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>


#include "dlc_fsm.h"

static void* dlc_fsm_svc(void* ud)
{
    int rc = 0;
    int new_ev = 0;

    dlc_fsm_t* self = (dlc_fsm_t*)(ud);
    assert(self);

    while (self->run_flag) {

        rc = efd_sem_wait(&self->sem);
        if (rc) {
            fprintf(stderr, "ERROR,DLC_FSM,efd_sem_wait failed:%d\n", rc);
            break;
        }

        if (self->ev_rd == self->ev_wr)
            continue;

        new_ev = self->ev_q[self->ev_rd % DLC_FSM_EV_LIMIT];
        if (!self->run_flag)
            break;

        fprintf(stdout, "INFO,DLC_FSM,drv event:%d@%lu\n", new_ev, self->ev_rd);

        self->ev_cb(new_ev);
        __sync_add_and_fetch(&self->ev_rd, 1);
    }

    fprintf(stdout, "INFO,DLC_FSM,svc finished\n");
    return (NULL);
}


int dlc_fsm_init(dlc_fsm_t* self, ev_handler cb)
{
    int rc = 0;

    memset(self, 0, sizeof(dlc_fsm_t));
    self->ev_rd = 0;
    self->ev_wr = 0;
    self->ev_cb = cb;
    self->run_flag = true;

    rc = efd_sem_init(&self->sem);
    if (rc) {
        fprintf(stderr, "ERROR,DLC_FSM,efd_sem_init failed:%d\n", rc);
        goto DONE;
    }

    rc = pthread_create(&self->thr_id, NULL, dlc_fsm_svc,
        (void*)(self));

    if (rc) {
        fprintf(stderr, "ERROR,DLC_FSM,pthread_create failed:%d\n", rc);
        goto DONE;
    }

    fprintf(stdout, "INFO,DLC_FSM,fsm running\n");
DONE:
    return (rc);
}

int dlc_fsm_fini(dlc_fsm_t* self)
{

    self->run_flag = false;
    dlc_fsm_sign(self, -1);
    pthread_join(self->thr_id, NULL);

    fprintf(stdout, "INFO,DLC_FSM,fsm stoped\n");

    return 0;
}

int dlc_fsm_sign(dlc_fsm_t* self, int ev_new)
{
    assert(self);

    int rc = 0;

    while (true)
    {
        if ((int32_t)(self->ev_wr - self->ev_rd) >=
            (int32_t)DLC_FSM_EV_LIMIT) {
            fprintf(stderr, "ERROR,DLC_FSM,event pool full\n");

            rc = -1;
            goto DONE;
        }
                
        uint64_t ev_cur = self->ev_wr;
        uint64_t ev_upd = ev_cur + 1;

        if (__sync_bool_compare_and_swap(&self->ev_wr, ev_cur, ev_upd)) {
            self->ev_q[ev_cur % DLC_FSM_EV_LIMIT] = ev_new;
            break;
        }            
    }

    rc = efd_sem_sign(&self->sem);
    if (rc) {
        fprintf(stderr, "ERROR,DLC_FSM,efd_sem_sign failed:%d\n", rc);
        goto DONE;
    }

DONE:
    return (rc);
}
