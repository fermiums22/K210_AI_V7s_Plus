#include <sys/lock.h>

int pthread_setcancelstate(int state, int *old_state)
{
    (void)state;
    (void)old_state;
    return 0;
}

void _lock_init(_lock_t *lock) { (void)lock; }
void _lock_init_recursive(_lock_t *lock) { (void)lock; }
void _lock_close(_lock_t *lock) { (void)lock; }
void _lock_close_recursive(_lock_t *lock) { (void)lock; }
void _lock_acquire(_lock_t *lock) { (void)lock; }
void _lock_acquire_recursive(_lock_t *lock) { (void)lock; }
int _lock_try_acquire(_lock_t *lock) { (void)lock; return 1; }
int _lock_try_acquire_recursive(_lock_t *lock) { (void)lock; return 1; }
void _lock_release(_lock_t *lock) { (void)lock; }
void _lock_release_recursive(_lock_t *lock) { (void)lock; }
