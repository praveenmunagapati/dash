
#include <dash/dart/base/assert.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/mutex.h>
#include <dash/dart/base/atomic.h>
#include <dash/dart/if/dart_tasking.h>
#include <dash/dart/tasking/dart_tasking_priv.h>
#include <dash/dart/tasking/dart_tasking_tasklist.h>
#include <dash/dart/tasking/dart_tasking_datadeps.h>
#include <dash/dart/tasking/dart_tasking_remote.h>
#include <dash/dart/tasking/dart_tasking_taskqueue.h>
#include <dash/dart/tasking/dart_tasking_copyin.h>

#include <stdbool.h>

#define DART_DEPHASH_SIZE 1023

/**
 * Management of task data dependencies using a hash map that maps pointers to tasks.
 * The hash map implementation is taken from dart_segment.
 * The hash uses the absolute local address stored in the gptr since that is used
 * throughout the task handling code.
 */

#define IS_OUT_DEP(taskdep) \
  (((taskdep).type == DART_DEP_OUT || (taskdep).type == DART_DEP_INOUT))

#define DEP_ADDR(dep) \
  ((dep).gptr.addr_or_offs.addr)

#define DEP_ADDR_EQ(dep1, dep2) \
  (DEP_ADDR(dep1) == DEP_ADDR(dep2))

typedef struct dart_dephash_elem {
  struct dart_dephash_elem *next;    // list pointer
  dart_task_dep_t           taskdep; // the dependency
  union taskref             task;    // the task referred to by the dependency
  dart_global_unit_t        origin;  // the unit this dependency originated from
} dart_dephash_elem_t;


static dart_dephash_elem_t *freelist_head            = NULL;
static dart_mutex_t         local_deps_mutex         = DART_MUTEX_INITIALIZER;

// list of incoming remote dependency requests defered to matching step
static dart_dephash_elem_t *unhandled_remote_deps    = NULL;
static dart_mutex_t         unhandled_remote_mutex   = DART_MUTEX_INITIALIZER;

// list of tasks that have no local dependencies but wait for a remote release
static task_list_t         *remote_blocked_tasks     = NULL;
static dart_mutex_t         remote_blocked_tasks_mutex  = DART_MUTEX_INITIALIZER;

// list of tasks that have been deferred because they are in a phase
// that is not ready to run yet
dart_taskqueue_t            local_deferred_tasks; // visible outside this CU

static dart_global_unit_t myguid;

static dart_ret_t
release_remote_dependencies(dart_task_t *task);

static void
dephash_recycle_elem(dart_dephash_elem_t *elem);

static inline int hash_gptr(dart_gptr_t gptr)
{
  // use larger types to accomodate for the shifts below
  // and unsigned to force logical shifts (instead of arithmetic)
  // NOTE: we ignore the teamid here because gptr in dependencies contain
  //       global unit IDs
  uint32_t segid  = gptr.segid;
  uint64_t hash   = gptr.addr_or_offs.offset;
  uint64_t unitid = gptr.unitid; // 64-bit required for shift
  // cut off the lower 2 bit, we assume that pointers are 4-byte aligned
  hash >>= 2;
  // mix in unit, team and segment ID
  hash ^= (segid  << 16); // 16 bit segment ID
  hash ^= (unitid << 32); // 24 bit unit ID
  // using a prime number in modulo stirs reasonably well

  DART_LOG_TRACE("hash_gptr(u:%d, s:%d, o:%p) => (%d)",
                 unitid, segid, gptr.addr_or_offs.offset,
                 (hash % DART_DEPHASH_SIZE));

  return (hash % DART_DEPHASH_SIZE);
}

static inline bool release_local_dep_counter(dart_task_t *task) {
  int32_t num_local_deps  = DART_DEC_AND_FETCH32(&task->unresolved_deps);
  int32_t num_remote_deps = DART_FETCH32(&task->unresolved_remote_deps);
  DART_ASSERT_MSG(num_remote_deps >= 0 && num_local_deps >= 0,
                  "Dependency counter underflow detected in task %p [%d,%d]!",
                  task, num_local_deps, num_remote_deps);
  DART_LOG_DEBUG("release_local_dep_counter : Task %p has %d local and %d "
                 "remote unresolved dependencies left", task, num_local_deps,
                 num_remote_deps);
  return (num_local_deps == 0 && num_remote_deps == 0);
}

static inline bool release_remote_dep_counter(dart_task_t *task) {
  int32_t num_remote_deps = DART_DEC_AND_FETCH32(&task->unresolved_remote_deps);
  int32_t num_local_deps  = DART_FETCH32(&task->unresolved_deps);
  DART_ASSERT_MSG(num_remote_deps >= 0 && num_local_deps >= 0,
                  "Dependency counter underflow detected in task %p [%d,%d]!",
                  task, num_local_deps, num_remote_deps);
  DART_LOG_DEBUG("release_local_dep_counter : Task %p has %d local and %d "
                 "remote unresolved dependencies left", task, num_local_deps,
                 num_remote_deps);
  if (num_remote_deps == 0) {
    // remove the task from the queue for remotely blocked tasks
    dart__base__mutex_lock(&remote_blocked_tasks_mutex);
    dart_tasking_tasklist_remove(&remote_blocked_tasks, task);
    dart__base__mutex_unlock(&remote_blocked_tasks_mutex);
  }
  return (num_local_deps == 0 && num_remote_deps == 0);
}

/**
 * Initialize the data dependency management system.
 */
dart_ret_t dart_tasking_datadeps_init()
{
  dart_myid(&myguid);
  dart_tasking_taskqueue_init(&local_deferred_tasks);
  return dart_tasking_remote_init();
}

static void
free_dephash_list(dart_dephash_elem_t *list)
{
  dart_dephash_elem_t *elem = list;
  while (elem != NULL) {
    dart_dephash_elem_t *tmp = elem->next;
    dephash_recycle_elem(elem);
    elem = tmp;
  }
}

dart_ret_t dart_tasking_datadeps_reset(dart_task_t *task)
{
  if (task == NULL || task->local_deps == NULL) return DART_OK;

  for (int i = 0; i < DART_DEPHASH_SIZE; ++i) {
    dart_dephash_elem_t *elem = task->local_deps[i];
    free_dephash_list(elem);
  }
  free(task->local_deps);
  task->local_deps = NULL;
  free_dephash_list(task->remote_successor);
  task->remote_successor = NULL;
  task->unresolved_deps  = 0;
  task->unresolved_remote_deps = 0;
  return DART_OK;
}

dart_ret_t dart_tasking_datadeps_fini()
{
  dart_tasking_datadeps_reset(dart__tasking__current_task());
  dart_dephash_elem_t *elem = freelist_head;
  while (elem != NULL) {
    dart_dephash_elem_t *tmp = elem->next;
    free(elem);
    elem = tmp;
  }
  freelist_head = NULL;
  dart_tasking_taskqueue_finalize(&local_deferred_tasks);
  return dart_tasking_remote_fini();
}

/**
 * Check for new remote task dependency requests coming in
 */
dart_ret_t dart_tasking_datadeps_progress()
{
  return dart_tasking_remote_progress();
}

/**
 * Allocate a new element for the dependency hash, possibly from a free-list
 */
static dart_dephash_elem_t *
dephash_allocate_elem(
  const dart_task_dep_t   *dep,
        taskref            task,
        dart_global_unit_t origin)
{
  // take an element from the free list if possible
  dart_dephash_elem_t *elem = NULL;
  if (freelist_head != NULL) {
    dart__base__mutex_lock(&local_deps_mutex);
    if (freelist_head != NULL) {
      DART_STACK_POP(freelist_head, elem);
    }
    dart__base__mutex_unlock(&local_deps_mutex);
  }

  if (elem == NULL){
    elem = calloc(1, sizeof(dart_dephash_elem_t));
  }

  DART_ASSERT(task.local != NULL);
  DART_ASSERT(elem->task.local == NULL);
  elem->task    = task;
  elem->taskdep = *dep;
  elem->origin  = origin;

  return elem;
}


/**
 * Deallocate an element
 */
static void dephash_recycle_elem(dart_dephash_elem_t *elem)
{
  if (elem != NULL) {
    memset(elem, 0, sizeof(*elem));
    dart__base__mutex_lock(&local_deps_mutex);
    DART_STACK_PUSH(freelist_head, elem);
    dart__base__mutex_unlock(&local_deps_mutex);
  }
}

static void dephash_require_alloc(dart_task_t *task)
{
  if (task != NULL && task->local_deps == NULL) {
    // allocate new dependency hash table
    task->local_deps = calloc(DART_DEPHASH_SIZE, sizeof(dart_dephash_elem_t*));
  }
}

/**
 * Add a task with dependency to the local dependency hash table.
 */
static dart_ret_t dephash_add_local(
  const dart_task_dep_t * dep,
        dart_task_t     * task)
{
  taskref tr;
  tr.local = task;
  dart_dephash_elem_t *elem = dephash_allocate_elem(dep, tr, myguid);

  dart__base__mutex_lock(&(task->parent->mutex));
  dephash_require_alloc(task->parent);
  // put the new entry at the beginning of the list
  int slot = hash_gptr(dep->gptr);
  DART_STACK_PUSH(task->parent->local_deps[slot], elem);
  dart__base__mutex_unlock(&(task->parent->mutex));

  return DART_OK;
}

dart_ret_t
dart_tasking_datadeps_handle_defered_local(
  dart_thread_t    * thread)
{
  dart_tasking_taskqueue_lock(&local_deferred_tasks);

  // also lock the thread's queue for the time we're processing to reduce
  // overhead
  dart_tasking_taskqueue_lock(&thread->queue);

  dart_task_t *task;
  while (
    (task = dart_tasking_taskqueue_pop_unsafe(&local_deferred_tasks)) != NULL)
  {
    // enqueue the task if if has gained no additional remote dependecies
    // since it's deferrement
    // Note: only check remote deps here because we know that local dependenices
    //       have been resolved if the task ended up in this queue.
    // Note: if the task has gained remote dependencies we drop the reference
    //       here because it will be released through a remote dep release later.
    if (DART_FETCH32(&task->unresolved_remote_deps) == 0) {
      dart_tasking_taskqueue_push_unsafe(&thread->queue, task);
    }
  }

  dart_tasking_taskqueue_unlock(&thread->queue);
  dart_tasking_taskqueue_unlock(&local_deferred_tasks);
  return DART_OK;
}

dart_ret_t
dart_tasking_datadeps_handle_defered_remote()
{
  dart_dephash_elem_t *rdep;
  DART_LOG_DEBUG("Handling previously unhandled remote dependencies: %p",
                 unhandled_remote_deps);
  dart_dephash_elem_t **local_deps = dart__tasking__current_task()->local_deps;
  dart__base__mutex_lock(&unhandled_remote_mutex);
  dart_dephash_elem_t *next = unhandled_remote_deps;
  while ((rdep = next) != NULL) {
    next = rdep->next;
    /**
     * Iterate over all possible tasks and find the closest-matching
     * local task that satisfies the remote dependency.
     * For closest task with a higher phase than the resolving task, send direct
     * task dependencies.
     */
    // gptr in dependencies contain global unit IDs
    dart_global_unit_t origin = rdep->origin;

    dart_task_t *candidate            = NULL;
    dart_task_t *direct_dep_candidate = NULL;
    DART_LOG_DEBUG("Handling delayed remote dependency for task %p from unit %i",
                   rdep->task.local, origin.id);
    if (local_deps != NULL) {
      int slot = hash_gptr(rdep->taskdep.gptr);
      for (dart_dephash_elem_t *local = local_deps[slot];
                                local != NULL;
                                local = local->next) {
        dart_task_t *local_task = local->task.local;

        // avoid repeatedly inspecting the same task and only consider
        // matching output dependencies
        if (local_task != candidate &&
            IS_OUT_DEP(local->taskdep) &&
            DEP_ADDR_EQ(local->taskdep, rdep->taskdep)) {
          /*
          * Remote INPUT task dependencies refer to nearest previous phase
          * so every task in the same phase and following
          * phases have to wait for the remote task to complete.
          * Note that we are only accounting for the candidate task in the
          * lowest phase since all later tasks are handled through local
          * dependencies.
          *
          * This matching assumes that the dependencies in local_deps are
          * ordered in phase-descending order.
          *
          * TODO: formulate the relation of local and remote dependencies
          *       between tasks and phase!
          */

          // lock the task to avoid race condiditions in updating the state
          dart__base__mutex_lock(&local_task->mutex);

          if (!IS_ACTIVE_TASK(local_task)) {
            dart__base__mutex_unlock(&local_task->mutex);
            DART_LOG_INFO("Task %p matching remote task %p already finished",
                          local_task, rdep->task.local);

            // if we got here without finding a candidate that is still active
            // we will not find one.
            // no need to continue searching!
            break;
          }

          if (local->taskdep.phase < rdep->taskdep.phase) {
            // local_task is in a previous phase, match!
            candidate = local_task;
            // we've found what we were looking for, keep the local_task locked
            break;
          } else if (local->taskdep.phase >= rdep->taskdep.phase) {
            dart__base__mutex_unlock(&local_task->mutex);
            // make this task a candidate for a direct successor to handle WAR
            // dependencies if it is in an earlier phase
            if (direct_dep_candidate == NULL ||
                direct_dep_candidate->phase > local->taskdep.phase) {
              direct_dep_candidate = local_task;
              DART_LOG_TRACE("Making local task %p a direct dependecy candidate "
                            "for remote task %p",
                            direct_dep_candidate,
                            rdep->task.remote);
            }
          }
          else {
              dart__base__mutex_unlock(&local_task->mutex);
          }
        }
      }
    }

    if (direct_dep_candidate != NULL) {
      // this task has to wait for the remote task to finish because it will
      // overwrite the input of the remote task
      dart_global_unit_t target = origin;
      dart_tasking_remote_direct_taskdep(target, direct_dep_candidate,
                                         rdep->task);
      int32_t unresolved_deps = DART_FETCH_AND_INC32(
                                  &direct_dep_candidate->unresolved_remote_deps);
      DART_LOG_DEBUG("DIRECT task dep: task %p (ph:%i) directly depends on "
                     "remote task %p (ph:%d) at unit %i and has %i remote dependencies",
                     direct_dep_candidate,
                     direct_dep_candidate->phase,
                     rdep->task.local,
                     rdep->taskdep.phase,
                     target.id,
                     unresolved_deps + 1);
      if (unresolved_deps == 0) {
        // insert the task into the list of remotely blocked tasks
        dart__base__mutex_lock(&remote_blocked_tasks_mutex);
        dart_tasking_tasklist_prepend(&remote_blocked_tasks, direct_dep_candidate);
        dart__base__mutex_unlock(&remote_blocked_tasks_mutex);
      }
    }

    if (candidate != NULL) {
      // we have a local task to satisfy the remote task
      DART_LOG_DEBUG("Found local task %p to satisfy remote dependency of "
                     "task %p from origin %i",
                     candidate, rdep->task.remote, origin.id);
      DART_STACK_PUSH(candidate->remote_successor, rdep);
      dart__base__mutex_unlock(&(candidate->mutex));
    } else {
      // the remote dependency cannot be served --> send release
      DART_LOG_DEBUG("Releasing remote task %p from unit %i, "
                     "which could not be handled in phase %i",
                     rdep->task.remote, origin.id,
                     rdep->taskdep.phase);
      dart_tasking_remote_release(origin, rdep->task, &rdep->taskdep);
      dephash_recycle_elem(rdep);
    }
  }

  unhandled_remote_deps = NULL;
  dart__base__mutex_unlock(&unhandled_remote_mutex);

  return DART_OK;
}

static dart_ret_t
dart_tasking_datadeps_handle_local_direct(
  const dart_task_dep_t * dep,
        dart_task_t     * task)
{
  dart_task_t *deptask = dep->task;
  if (deptask != DART_TASK_NULL) {
    dart__base__mutex_lock(&(deptask->mutex));
    if (IS_ACTIVE_TASK(deptask)) {
      dart_tasking_tasklist_prepend(&(deptask->successor), task);
      int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                    &task->unresolved_deps);
      DART_LOG_TRACE("Making task %p a direct local successor of task %p "
                     "(successor: %p, state: %i | num_deps: %i)",
                     task, deptask,
                     deptask->successor, deptask->state, unresolved_deps);
    }
    dart__base__mutex_unlock(&(deptask->mutex));
  }
  return DART_OK;
}


static dart_ret_t
dart_tasking_datadeps_handle_copyin(
  const dart_task_dep_t * dep,
        dart_task_t     * task)
{
  int slot;
  dart_gptr_t dest_gptr;

  dest_gptr.addr_or_offs.addr = dep->copyin.dest;
  dest_gptr.flags             = 0;
  dest_gptr.segid             = DART_TASKING_DATADEPS_LOCAL_SEGID;
  dest_gptr.teamid            = 0;
  dest_gptr.unitid            = myguid.id;
  slot = hash_gptr(dest_gptr);
  dart_dephash_elem_t *elem = NULL;
  int iter = 0;
  DART_LOG_TRACE("Handling copyin dep (unit %d, phase %d)",
                 dep->copyin.gptr.unitid, dep->phase);

  do {
    // check whether this is the first task with copyin
    if (task->parent->local_deps != NULL) {
      for (elem = task->parent->local_deps[slot];
          elem != NULL; elem = elem->next) {
        if (elem->taskdep.gptr.addr_or_offs.addr == dep->copyin.dest) {
          if (elem->taskdep.phase < dep->phase) {
            // phases are stored in decending order so we can stop here
            break;
          }
          // So far we can only re-use prefetching in the same phase
          // TODO: can we figure out whether we can go back further?
          //       Might need help from the remote side.
          if (IS_OUT_DEP(elem->taskdep) && dep->phase == elem->taskdep.phase) {
            // we're not the first --> add a dependency to the task that does the copy
            dart_task_t *elem_task = elem->task.local;
            DART_INC_AND_FETCH32(&task->unresolved_deps);
            // lock the task here to avoid race condition
            dart__base__mutex_lock(&(elem_task->mutex));
            dart_tasking_tasklist_prepend(&(elem_task->successor), task);
            dart__base__mutex_unlock(&(elem_task->mutex));

            // add this task to the hash table
            dart_task_dep_t in_dep;
            in_dep.type  = DART_DEP_IN;
            in_dep.gptr  = dest_gptr;
            in_dep.phase = dep->phase;
            dephash_add_local(&in_dep, task);

            DART_LOG_TRACE("Copyin: task %p waits for task %p to copy", task, elem_task);

            // we're done
            return DART_OK;
          }
        }
      }
    }

    if (iter > 0) {
      // this shouldn't happen
      DART_ASSERT_MSG(iter, "FAILED to create copyin task!");
    }

    // we haven't found a task that does the prefetching in this phase
    // so create a new one
    taskref tr;
    tr.local = task;
    DART_LOG_TRACE("Creating copyin task in phase %d (dest %p)",
                   dep->phase, dep->copyin.dest);
    dart_tasking_copyin_create_task(dep, dest_gptr, tr);
  } while (0 == iter++);

  return DART_OK;
}


/**
 * Match a local data dependency.
 * This ignores phases and matches a dependency to the last previous depdendency
 * encountered.
 */
static dart_ret_t
dart_tasking_datadeps_match_local_datadep(
  const dart_task_dep_t * dep,
        dart_task_t     * task)
{
  int slot;
  slot = hash_gptr(dep->gptr);

  // shortcut if no dependencies to match, yet
  if (task->parent->local_deps == NULL) return DART_OK;

  /*
   * iterate over all dependent tasks until we find the first task with
   * OUT|INOUT dependency on the same pointer
   */
  for (dart_dephash_elem_t *elem = task->parent->local_deps[slot];
       elem != NULL; elem = elem->next)
  {
    if (DEP_ADDR_EQ(elem->taskdep, *dep)) {
      dart_task_t *elem_task = elem->task.local;
      if (elem_task == task) {
        // simply upgrade the dependency to an output dependency
        if (elem->taskdep.type == DART_DEP_IN && IS_OUT_DEP(*dep)) {
          elem->taskdep.type = DART_DEP_INOUT;
        }
        // nothing to be done for this dependency
        break;
      }
      DART_LOG_TRACE("Task %p local dependency on %p (s:%i) vs %p (s:%i) "
                     "of task %p",
                     task,
                     DEP_ADDR(*dep),
                     dep->gptr.segid,
                     DEP_ADDR(elem->taskdep),
                     elem->taskdep.gptr.segid,
                     elem_task);

      DART_LOG_TRACE("Checking task %p against task %p "
                     "(deptype: %i vs %i)",
                     elem_task, task, elem->taskdep.type,
                     dep->type);

      if (IS_OUT_DEP(*dep) ||
              (dep->type == DART_DEP_IN  && IS_OUT_DEP(elem->taskdep))) {
        // lock the task here to avoid race condition
        dart__base__mutex_lock(&(elem_task->mutex));
        if (IS_ACTIVE_TASK(elem_task)){
          // check whether this task is already in the successor list
          if (dart_tasking_tasklist_contains(elem_task->successor, task)){
            // the task is already in the list, don't add it again!
            DART_LOG_TRACE("Task %p already a local successor of task %p, skipping",
                          task, elem_task);
          } else {
            int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                          &task->unresolved_deps);
            DART_LOG_TRACE("Making task %p a local successor of task %p "
                          "(successor: %p, state: %i | num_deps: %i)",
                          task, elem_task,
                          elem_task->successor,
                          elem_task->state, unresolved_deps);
            dart_tasking_tasklist_prepend(&(elem_task->successor), task);
          }
        }
        dart__base__mutex_unlock(&(elem_task->mutex));
      }
      if (IS_OUT_DEP(elem->taskdep)) {
        // we can stop at the first OUT|INOUT dependency
        DART_LOG_TRACE("Stopping search for dependencies for task %p at "
                       "first OUT dependency encountered from task %p!",
                       task, elem_task);
        return DART_OK;
      }
    }
  }

  if (!IS_OUT_DEP(*dep)) {
    DART_LOG_TRACE("No matching output dependency found for local input "
        "dependency %p of task %p in phase %i",
        DEP_ADDR(*dep), task, task->phase);
  }
  return DART_OK;
}


/**
 * Match a delayed local data dependency.
 * This is similar to \ref dart_tasking_datadeps_match_local_datadep but
 * handles the local dependency honoring the phase, i.e., later depdendencies
 * are skipped. This also potententially adds dependencies to the graph.
 */
static dart_ret_t
dart_tasking_datadeps_match_delayed_local_datadep(
  const dart_task_dep_t * dep,
        dart_task_t     * task)
{
  int slot;
  slot = hash_gptr(dep->gptr);

  // shortcut if no dependencies to match, yet
  if (task->parent->local_deps == NULL) return DART_OK;

  dart_task_t *next_out_task = NULL; // the task with the next output dependency

  DART_LOG_DEBUG("Handling delayed input dependency in phase %d", dep->phase);

  /*
   * iterate over all dependent tasks until we find the first task with
   * OUT|INOUT dependency on the same pointer
   */
  for (dart_dephash_elem_t *elem = task->parent->local_deps[slot], *prev = NULL;
       elem != NULL; prev = elem, elem = elem->next)
  {
    // skip dependencies that were created in a later phase
    DART_LOG_TRACE("  phase %d vs phase %d", elem->taskdep.phase, dep->phase);
    if (elem->taskdep.phase > dep->phase) {
      if (DEP_ADDR_EQ(elem->taskdep, *dep) && IS_OUT_DEP(elem->taskdep)){
        next_out_task = elem->task.local;
      }
      continue;
    }

    if (DEP_ADDR_EQ(elem->taskdep, *dep)) {
      dart_task_t *elem_task = elem->task.local;
      DART_ASSERT_MSG(elem_task != task,
                      "Cannot insert existing task with delayed dependency!");

      if (IS_OUT_DEP(elem->taskdep)) {
        // lock the task here to avoid race condition
        dart__base__mutex_lock(&(elem_task->mutex));
        if (IS_ACTIVE_TASK(elem_task)){
          int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                        &task->unresolved_deps);
          DART_LOG_TRACE("Making task %p a local successor of task %p using delayed dependency "
                        "(successor: %p, state: %i | num_deps: %i)",
                        task, elem_task,
                        elem_task->successor,
                        elem_task->state, unresolved_deps);

          dart_tasking_tasklist_prepend(&(elem_task->successor), task);
        }
        dart__base__mutex_unlock(&(elem_task->mutex));

        // register this task with the next out task to avoid overwriting
        if (next_out_task) {
          dart__base__mutex_lock(&(next_out_task->mutex));
          DART_ASSERT_MSG(IS_ACTIVE_TASK(next_out_task),
                          "Cannot insert delayed dependency if the next task "
                          "is already running (WTF?!)");
          int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                        &next_out_task->unresolved_deps);
          DART_LOG_TRACE("Making task %p a local successor of next_out_task %p using delayed dependency  "
                        "(successor: %p, state: %i | num_deps: %i)",
                        task, next_out_task,
                        next_out_task->successor,
                        next_out_task->state, unresolved_deps);
          dart_tasking_tasklist_prepend(&(task->successor), next_out_task);
          dart__base__mutex_unlock(&(next_out_task->mutex));
          // no need to add this dependency to the hash table
        } else {
          // there is no later task so we better insert this dependency into
          // the hash table
          taskref tr;
          tr.local = task;
          dart_dephash_elem_t *new_elem = dephash_allocate_elem(dep, tr, myguid);
          dart__base__mutex_lock(&(task->parent->mutex));
          dephash_require_alloc(task->parent);
          if (prev == NULL) {
            // we are still at the head of the hash table slot, i.e.,
            // our match was the very first task we encountered
            new_elem->next                 = task->parent->local_deps[slot];
            task->parent->local_deps[slot] = new_elem;
            DART_LOG_TRACE("Inserting delayed dependency at the beginning of the slot");
          } else {
            new_elem->next = prev->next;
            prev->next     = new_elem;
            DART_LOG_TRACE("Inserting delayed dependency in the middle");
          }
          dart__base__mutex_unlock(&(task->parent->mutex));
        }
        return DART_OK;
      }
    }
  }

  if (!IS_OUT_DEP(*dep)) {
    DART_LOG_TRACE("No matching output dependency found for local input "
        "dependency %p of task %p in phase %i",
        DEP_ADDR(*dep), task, task->phase);
    printf("Couldn't find an active task to match delayed input dependency!\n");
  }
  return DART_OK;
}


/**
 * Find all tasks this task depends on and add the task to the dependency hash
 * table. All earlier tasks are considered up to the first task with OUT|INOUT
 * dependency.
 */
dart_ret_t dart_tasking_datadeps_handle_task(
    dart_task_t           *task,
    const dart_task_dep_t *deps,
    size_t                 ndeps)
{
  dart_global_unit_t myid;
  dart_myid(&myid);

  DART_LOG_DEBUG("Datadeps: task %p has %zu data dependencies in phase %i",
                 task, ndeps, task->phase);

  for (size_t i = 0; i < ndeps; i++) {
    dart_task_dep_t dep = deps[i];
    if (dep.type == DART_DEP_IGNORE) {
      // ignored
      continue;
    }

    // adjust the phase of the dependency if required
    if (dep.phase == DART_PHASE_TASK) {
      dep.phase = task->phase;
    }

    // get the global unit ID in the dependency
    dart_global_unit_t guid;
    if (dep.gptr.teamid != DART_TEAM_ALL) {
      dart_team_unit_l2g(dep.gptr.teamid,
                         DART_TEAM_UNIT_ID(dep.gptr.unitid),
                         &guid);
    } else {
      guid.id = dep.gptr.unitid;
    }

    if (dep.type != DART_DEP_DIRECT) {
      DART_LOG_TRACE("Datadeps: task %p dependency %zu: type:%i unit:%i "
                     "seg:%i addr:%p phase:%i",
                     task, i, dep.type, guid.id, dep.gptr.segid,
                     DEP_ADDR(dep), dep.phase);
    }

    if (dep.type == DART_DEP_DIRECT) {
      dart_tasking_datadeps_handle_local_direct(&dep, task);
    } else if (dep.type == DART_DEP_COPYIN){
      dart_tasking_datadeps_handle_copyin(&dep, task);
    } else if (guid.id != myid.id) {
        if (task->parent->state == DART_TASK_ROOT) {
          dart_tasking_remote_datadep(&dep, task);
          int32_t unresolved_deps = DART_INC_AND_FETCH32(&task->unresolved_remote_deps);
          DART_LOG_INFO(
            "Sent remote dependency request for task %p "
            "(unit=%i, team=%i, segid=%i, offset=%p, num_deps=%i)",
            task, guid.id, dep.gptr.teamid, dep.gptr.segid,
            dep.gptr.addr_or_offs.addr, unresolved_deps);
          if (unresolved_deps == 1) {
            // insert the task into the list for remotely blocked tasks
            dart__base__mutex_lock(&remote_blocked_tasks_mutex);
            dart_tasking_tasklist_prepend(&remote_blocked_tasks, task);
            dart__base__mutex_unlock(&remote_blocked_tasks_mutex);
          }
        } else {
          DART_LOG_WARN("Ignoring remote dependency in nested task!");
        }
    } else {
      // translate the pointer to a local pointer
      dep.gptr = dart_tasking_datadeps_localize_gptr(dep.gptr);
      if (dep.type == DART_DEP_DELAYED_IN) {
        // delayed input dependencies need some special treatment
        dart_tasking_datadeps_match_delayed_local_datadep(&dep, task);
      } else {
        dart_tasking_datadeps_match_local_datadep(&dep, task);

        // add this task to the hash table
        dephash_add_local(&dep, task);
      }
    }
  }

  return DART_OK;
}

/**
 * Handle an incoming dependency request by enqueuing it for later handling.
 */
dart_ret_t dart_tasking_datadeps_handle_remote_task(
    const dart_task_dep_t  *rdep,
    const taskref           remote_task,
    dart_global_unit_t      origin)
{
  if (rdep->type != DART_DEP_IN) {
    DART_LOG_ERROR("Remote dependencies with type other than DART_DEP_IN are not supported!");
    return DART_ERR_INVAL;
  }

  DART_LOG_INFO("Enqueuing remote task %p from unit %i for later resolution",
    remote_task.remote, origin.id);
  // cache this request and resolve it later
  dart_dephash_elem_t *rs = dephash_allocate_elem(rdep, remote_task, origin);
  dart__base__mutex_lock(&unhandled_remote_mutex);
  DART_STACK_PUSH(unhandled_remote_deps, rs);
  dart__base__mutex_unlock(&unhandled_remote_mutex);
  return DART_OK;
}


/**
 * Handle the direct task dependency between a local task and
 * its remote successor
 */
dart_ret_t dart_tasking_datadeps_handle_remote_direct(
    dart_task_t       *local_task,
    taskref            remote_task,
    dart_global_unit_t origin)
{
  bool enqueued = false;
  DART_LOG_DEBUG("Remote direct task dependency for task %p: %p",
      local_task, remote_task.remote);
  // dummy dependency
  dart_task_dep_t dep;
  dep.type   = DART_DEP_DIRECT;
  dep.gptr   = DART_GPTR_NULL;
  if (IS_ACTIVE_TASK(local_task)) {
    dart__base__mutex_lock(&(local_task->mutex));
    if (IS_ACTIVE_TASK(local_task)) {
      dart_dephash_elem_t *rs = dephash_allocate_elem(&dep, remote_task, origin);
      DART_STACK_PUSH(local_task->remote_successor, rs);
      enqueued = true;
    }
    dart__base__mutex_unlock(&(local_task->mutex));
  }

  if (!enqueued) {
    // local task done already --> release immediately
    dart_tasking_remote_release(origin, remote_task, &dep);
  }

  return DART_OK;
}

/**
 * Release remote and local dependencies of a local task
 */
dart_ret_t dart_tasking_datadeps_release_local_task(
    dart_task_t   *task)
{
  if (task->state != DART_TASK_CANCELLED)
    release_remote_dependencies(task);

  DART_LOG_TRACE("Releasing local dependencies of task %p", task);

  // release local successors
  dart_task_t *succ;
  while ((succ = dart_tasking_tasklist_pop(&task->successor)) != NULL) {
    DART_LOG_TRACE("  Releasing task %p", succ);
    bool runnable = release_local_dep_counter(succ);

    if (succ->state == DART_TASK_CREATED &&
        runnable) {
      dart__tasking__enqueue_runnable(succ);
    }
  }

  return DART_OK;
}

/**
 * Handle an incoming release of an input dependency.
 * The release might be deferred until after the matching of dependencies
 * has completed.
 */
dart_ret_t dart_tasking_datadeps_release_remote_dep(
  dart_task_t *local_task)
{
  // release the task if it is runnable
  bool runnable = release_remote_dep_counter(local_task);
  if (runnable) {
    // enqueue as runnable
    dart__tasking__enqueue_runnable(local_task);
  }
  return DART_OK;
}

/**
 * Release the remote dependencies of \c task.
 */
static dart_ret_t release_remote_dependencies(dart_task_t *task)
{
  DART_LOG_TRACE("Releasing remote dependencies for task %p (rs:%p)",
                 task, task->remote_successor);
  dart_dephash_elem_t *rs = task->remote_successor;
  while (rs != NULL) {
    dart_dephash_elem_t *tmp = rs;
    rs = rs->next;

    // send the release
    dart_tasking_remote_release(tmp->origin, tmp->task, &tmp->taskdep);
    dephash_recycle_elem(tmp);
  }
  task->remote_successor = NULL;
  return DART_OK;
}


/**
 * Cancel all remaining remote dependencies.
 * All tasks that are still blocked by remote dependencies will be subsequently
 * released if they have no local dependecies.
 */
dart_ret_t dart_tasking_datadeps_cancel_remote_deps()
{
  dart_task_t *task;
  dart__base__mutex_lock(&remote_blocked_tasks_mutex);
  while ((task = dart_tasking_tasklist_pop(&remote_blocked_tasks)) != NULL) {
    task->unresolved_remote_deps = 0;
    int32_t unresolved_deps = DART_FETCH32(&task->unresolved_deps);
    if (unresolved_deps == 0) {
      dart__tasking__enqueue_runnable(task);
    }
  }
  dart__base__mutex_unlock(&remote_blocked_tasks_mutex);
  return DART_OK;
}