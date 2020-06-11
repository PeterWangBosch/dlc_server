
#include "efd_sem.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>

int efd_sem_init(efd_sem_t *ev) {
  assert(ev);

  ev->efd = eventfd(0, EFD_SEMAPHORE);
  if (ev->efd < 0)
      return (errno);
  return (0);
}

int efd_sem_sign(efd_sem_t *ev) {
  /*
  A write(2) call adds the 8-byte integer value supplied in its
              buffer to the counter.  The maximum value that may be stored
              in the counter is the largest unsigned 64-bit value minus 1
              (i.e., 0xfffffffffffffffe).  If the addition would cause the
              counter's value to exceed the maximum, then the write(2)
              either blocks until a read(2) is performed on the file
              descriptor, or fails with the error EAGAIN if the file
              descriptor has been made nonblocking.

              A write(2) fails with the error EINVAL if the size of the
              supplied buffer is less than 8 bytes, or if an attempt is made
              to write the value 0xffffffffffffffff.
  */
  assert(ev && ev->efd > 0);
  uint64_t e = 1;
  while (1) {
      int rc = (int)write(ev->efd, &e, sizeof(e));
      if (rc < 0)
          return (errno);
      if (rc != sizeof(e))
          continue;
      return (0);
  }
  
}

int efd_sem_wait(efd_sem_t *ev) {
  /*
  Each successful read(2) returns an 8-byte integer.  A read(2)
              fails with the error EINVAL if the size of the supplied buffer
              is less than 8 bytes.

              The value returned by read(2) is in host byte order—that is,
              the native byte order for integers on the host machine.

              The semantics of read(2) depend on whether the eventfd counter
              currently has a nonzero value and whether the EFD_SEMAPHORE
              flag was specified when creating the eventfd file descriptor:

              *  If EFD_SEMAPHORE was not specified and the eventfd counter
                 has a nonzero value, then a read(2) returns 8 bytes
                 containing that value, and the counter's value is reset to
                 zero.

              *  If EFD_SEMAPHORE was specified and the eventfd counter has
                 a nonzero value, then a read(2) returns 8 bytes containing
                 the value 1, and the counter's value is decremented by 1.

              *  If the eventfd counter is zero at the time of the call to
                 read(2), then the call either blocks until the counter
                 becomes nonzero (at which time, the read(2) proceeds as
                 described above) or fails with the error EAGAIN if the file
                 descriptor has been made nonblocking.
  */


  assert(ev && ev->efd > 0);

  uint64_t e = 1;
  while (1) {
      int rc = (int)read(ev->efd, &e, sizeof(e));
      if (rc < 0) {
          if (errno == EAGAIN)
              continue;
          return (errno);
      }
      return (0);
  }
  
}
