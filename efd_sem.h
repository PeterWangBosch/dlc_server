#pragma once
#include <sys/cdefs.h>

__BEGIN_DECLS

typedef struct efd_sem {
	int efd;
}efd_sem_t;

int efd_sem_init(efd_sem_t *ev);
int efd_sem_sign(efd_sem_t *ev);
int efd_sem_wait(efd_sem_t *ev);


__END_DECLS