
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <unistd.h>

#ifndef _POSIX_THREAD_PROCESS_SHARED
#error "This platform does not support process shared mutex"
#endif

#include "../shmem_barriers_if.h"
#include "../shmem_logger.h"

static syncarea_t area = (syncarea_t) 0;

int shmem_syncarea_init(int numprocs, void* shm_addr, int shmid)
{
  pthread_mutexattr_t mutex_shared_attr;

  area = (syncarea_t) shm_addr;
  area->shmem_key = shmid;  

  PTHREAD_SAFE(pthread_mutexattr_init(&mutex_shared_attr));
  PTHREAD_SAFE(pthread_mutexattr_setpshared(&mutex_shared_attr, 
					    PTHREAD_PROCESS_SHARED));

  PTHREAD_SAFE(pthread_mutex_init(&(area->lock), &mutex_shared_attr));
  PTHREAD_SAFE(pthread_mutexattr_destroy(&mutex_shared_attr));

  int i;
  for( i=0; i<MAXNUM_TEAMS; i++ ) {
    (area->teams[i]).inuse=0;
  }

  sysv_barrier_create( &((area->teams[0]).barr), numprocs );
  area->teams[0].teamid = DART_TEAM_ALL;
  area->teams[0].inuse=1;

  area->nextid=1;

  return 0;
}

int shmem_syncarea_delete(int numprocs, void* shm_addr, int shmid)
{
  // TODO: destroy mutex and barriers
  return 0;
}

int shmem_syncarea_setaddr(void *shm_addr) 
{
  area = (syncarea_t) shm_addr;
  return 0;
}

int shmem_syncarea_get_shmid()
{
  return area->shmem_key;
}

int shmem_syncarea_newteam(dart_team_t *teamid, int numprocs)
{
  int i, slot=-1;
  PTHREAD_SAFE_NORET(pthread_mutex_lock(&(area->lock)));

  // find a free slot
  for( i=1; i<MAXNUM_TEAMS; i++ ) {
    if( !((area->teams[i]).inuse) ) {
      slot=i;
      break;
    }
  }

  if( 1<=slot && slot<MAXNUM_TEAMS ) {
    sysv_barrier_create( &((area->teams[slot]).barr), numprocs );
    area->teams[slot].teamid = area->nextid;
    (*teamid) =area->teams[slot].teamid;
    area->teams[slot].inuse=1;

    (area->nextid)++;
  }

  PTHREAD_SAFE_NORET(pthread_mutex_unlock(&(area->lock)));
  return slot;
}

int shmem_syncarea_findteam(dart_team_t teamid)
{
  int i, res=-1;

  PTHREAD_SAFE_NORET(pthread_mutex_lock(&(area->lock)));

  for(i=0; i<MAXNUM_TEAMS; i++ ) {
    if( (area->teams[i]).inuse &&
	(area->teams[i]).teamid==teamid ) {
      res=i;
      break;
    }
  }
  PTHREAD_SAFE_NORET(pthread_mutex_unlock(&(area->lock)));

  return res;
}

int shmem_syncarea_delteam(dart_team_t teamid, int numprocs)
{
  int i, slot=-1;
  PTHREAD_SAFE_NORET(pthread_mutex_lock(&(area->lock)));

  for(i=0; i<MAXNUM_TEAMS; i++ ) {
    if( (area->teams[i]).inuse &&
	(area->teams[i]).teamid==teamid ) {
      slot=i;
      break;
    }
  }
  
  //slot = shmem_syncarea_findteam(teamid);
  if( 1<=slot && slot<MAXNUM_TEAMS ) {
    // fprintf(stderr, "destroying barrier at slot %d\n", slot);
    if( area->teams[slot].inuse ) {
      sysv_barrier_destroy( &((area->teams[slot]).barr) );
    }
    area->teams[slot].inuse=0;
  }
  
  PTHREAD_SAFE_NORET(pthread_mutex_unlock(&(area->lock)));
  
  return 0;
}

// do a wait for barrier at slot 'slot'"
int shmem_syncarea_barrier_wait(int slot) 
{
  int ret;
  if( 0<=slot && slot<MAXNUM_TEAMS ) {
    ret = sysv_barrier_await( &((area->teams[slot]).barr) );
  } else {
    ret=-1;
  }
  return ret;
}


int sysv_barrier_create(sysv_barrier_t barrier, int num_procs)
{
  pthread_mutexattr_t mutex_shared_attr;
  PTHREAD_SAFE(pthread_mutexattr_init(&mutex_shared_attr));
  PTHREAD_SAFE(pthread_mutexattr_setpshared(&mutex_shared_attr, 
					    PTHREAD_PROCESS_SHARED));
  PTHREAD_SAFE(pthread_mutex_init(&(barrier->mutex), &mutex_shared_attr));
  PTHREAD_SAFE(pthread_mutexattr_destroy(&mutex_shared_attr));
  
  pthread_condattr_t cond_shared_attr;
  PTHREAD_SAFE(pthread_condattr_init(&cond_shared_attr));
  PTHREAD_SAFE(pthread_condattr_setpshared(&cond_shared_attr, 
					   PTHREAD_PROCESS_SHARED));
  PTHREAD_SAFE(pthread_cond_init(&(barrier->cond), &cond_shared_attr));
  PTHREAD_SAFE(pthread_condattr_destroy(&cond_shared_attr));
  
  barrier->num_procs = num_procs;
  barrier->num_waiting = 0;
  
  return 0;
}

int sysv_barrier_destroy(sysv_barrier_t barrier)
{
  PTHREAD_SAFE(pthread_cond_destroy(&(barrier->cond)));
  PTHREAD_SAFE(pthread_mutex_destroy(&(barrier->mutex)));
  return 0;
}

int sysv_barrier_await(sysv_barrier_t barrier)
{
  PTHREAD_SAFE(pthread_mutex_lock(&(barrier->mutex)));
  (barrier->num_waiting)++;
  if (barrier->num_waiting < barrier->num_procs)
    {
      PTHREAD_SAFE(pthread_cond_wait(&(barrier->cond), &(barrier->mutex)));
    }
  else
    {
      barrier->num_waiting = 0;
      PTHREAD_SAFE(pthread_cond_broadcast(&(barrier->cond)));
    }
  PTHREAD_SAFE(pthread_mutex_unlock(&(barrier->mutex)));
  return 0;
}



#if 0

void shmem_barriers_init(int numprocs, void* shm_addr)
{
  int i; 

  area = (syncarea_t) shm_addr;
  for( i=0; i<MAXNUM_TEAMS; i++ ) {
    area->barriers[i]
      }
}

void shmem_barriers_destroy()
{
  // do nothing
}

shmem_barrier_t shmem_barriers_create_barrier(int num_procs_to_wait)
{
  PTHREAD_SAFE_NORET(pthread_mutex_lock(&(area->lock)));
  if (area->num_barriers >= MAXNUM_BARRIERS)
    {
      PTHREAD_SAFE_NORET(pthread_mutex_unlock(&(area->lock)));
      ERROR("Could not create barrier: %s", "maxnum exceeded");
      return -1;
    }
  sysv_barrier_create(&(area->barriers[area->num_barriers]),
		      num_procs_to_wait);
  int result = area->num_barriers;
  area->num_barriers = area->num_barriers + 1;
  PTHREAD_SAFE_NORET(pthread_mutex_unlock(&(area->lock)));
  return result;
}

void shmem_barriers_barrier_wait(shmem_barrier_t barrier)
{
  sysv_barrier_await(&(area->barriers[barrier]));
}

#endif
