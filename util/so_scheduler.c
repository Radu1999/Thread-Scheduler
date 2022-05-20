#include "so_scheduler.h"
#include <stdlib.h>
#include <semaphore.h>
#include <stdio.h>

#define MAX_THREADS 10000

typedef struct {
	pthread_t id;
	so_handler *handler;
	int prio;
  sem_t running;
  sem_t added_to_scheduler;
  unsigned int remaining_time_quantum;
  unsigned int birth_time;
} thread_t;

typedef struct {
    thread_t* threads[MAX_THREADS];
    int size;
} priority_queue;

void swap(thread_t **a, thread_t **b) {
  thread_t *temp = *b;
  *b = *a;
  *a = temp;
}

void heapify(priority_queue *q, int i) {
  if (q->size == 1)
    return;
  else {

    int largest = i;
    int l = 2 * i + 1;
    int r = 2 * i + 2;
    if (r < q->size &&  (q->threads[r]->prio > q->threads[largest]->prio
        || (q->threads[r]->prio == q->threads[largest]->prio && q->threads[r]->birth_time < q->threads[largest]->birth_time)))
      largest = r; 
    if (l < q->size && (q->threads[l]->prio > q->threads[largest]->prio 
      || (q->threads[l]->prio == q->threads[largest]->prio && q->threads[l]->birth_time < q->threads[largest]->birth_time)))
      largest = l;

    if (largest != i) {
      swap(&q->threads[i], &q->threads[largest]);
      heapify(q, largest);
    }
  }
}

void insert(priority_queue *q, thread_t *newThread) {
  if (q->size == 0)
    q->threads[q->size++] = newThread;
  else {
    q->threads[q->size++] = newThread;
    int parrent = (q->size - 2) / 2;
    int son = (q->size - 1);
    while(parrent >= 0 && (q->threads[parrent]->prio < q->threads[son]->prio
        || (q->threads[parrent]->prio == q->threads[son]->prio && q->threads[parrent]->birth_time > q->threads[son]->birth_time)))
    {
      swap(&q->threads[parrent], &q->threads[son]);
      son = parrent;
      parrent = (son - 1) / 2;
    }
  }
}

thread_t* deleteRoot(priority_queue *q) {
  if(q->size == 0)
    return NULL;
  int i = 0;
  swap(&q->threads[i], &q->threads[q->size - 1]);
  q->size--;
  heapify(q, 0);
  return q->threads[q->size];
}



typedef struct {
    unsigned int time_quantum;
    unsigned int io;
    priority_queue *pq;
    priority_queue **waiting;
    thread_t *current_thread;
    sem_t off;
    thread_t* threads[MAX_THREADS];
    int num_threads;
} scheduler_t;

scheduler_t *scheduler;

int so_init(unsigned int time_quantum, unsigned int io) {

    if(scheduler != NULL || time_quantum <= 0 || io > SO_MAX_NUM_EVENTS)
        return -1;

    scheduler = malloc(sizeof(*scheduler));
    if(scheduler == NULL)
        return -1;
    scheduler->time_quantum = time_quantum;
    scheduler->io = io;
    scheduler->current_thread = NULL;
    scheduler->pq = malloc(sizeof(*(scheduler->pq)));
    scheduler->num_threads = 0;
   
    if(scheduler->pq == NULL)
        return -1;
    scheduler->pq->size = 0;

    scheduler->waiting = malloc(SO_MAX_NUM_EVENTS * sizeof(priority_queue*));
    for(int i = 0; i < SO_MAX_NUM_EVENTS; i++) {
        scheduler->waiting[i] = malloc(sizeof(priority_queue));
         scheduler->waiting[i]->size = 0;
    }
    sem_init(&scheduler->off, 0, 0);
    return 0;
}

void add_new_thread(thread_t *thread) {
    if(!scheduler->current_thread) {
        scheduler->current_thread = thread;
        sem_post(&thread->running);
    } else
        insert(scheduler->pq, thread);
}

void start_next_thread(void) {
    if(scheduler->pq->size == 0) {
        scheduler->current_thread = NULL;
        return;
    }
    scheduler->current_thread  = deleteRoot(scheduler->pq);
    sem_post(&scheduler->current_thread->running);
}

void *start_thread(void *args) {
    thread_t *thread = (thread_t *) args;
    add_new_thread(thread);
    sem_post(&thread->added_to_scheduler);
    sem_wait(&thread->running);
    thread->handler(thread->prio);
    start_next_thread();
    return NULL;
}

static unsigned int age = 0;

tid_t so_fork(so_handler *func, unsigned int priority) {
   
    if(priority > SO_MAX_PRIO || !func)
        return INVALID_TID;

    thread_t *thread = malloc(sizeof(*thread));
    if(thread == NULL)
        return INVALID_TID;
    
    thread->handler = func;
    thread->birth_time = age++;
    thread->prio = priority;
    thread->remaining_time_quantum = scheduler->time_quantum;
	  sem_init(&thread->running, 0, 0);
    sem_init(&thread->added_to_scheduler, 0, 0);
    pthread_create(&thread->id, NULL, start_thread, thread);
    scheduler->threads[scheduler->num_threads++] = thread;

    sem_wait(&thread->added_to_scheduler);
    if (scheduler->current_thread != thread)
		  so_exec();
    return thread->id;
}

int so_wait(unsigned int io) {
    if (io >= scheduler->io)
		return -1;

    thread_t *thread = scheduler->current_thread;
    insert(scheduler->waiting[io], thread); 
    start_next_thread();

    sem_wait(&thread->running);

    return 0;

}

void so_exec(void)
{
    thread_t *thread = scheduler->current_thread;
	  if (thread == NULL)
		  return;
    thread->remaining_time_quantum--;
    if(!thread->remaining_time_quantum) {
        thread->remaining_time_quantum = scheduler->time_quantum;
        thread->birth_time = age++;
        add_new_thread(thread);
        scheduler->current_thread = deleteRoot(scheduler->pq);
        if(scheduler->current_thread != thread)
          sem_post(&scheduler->current_thread->running);
    } else if(scheduler->pq->size && scheduler->pq->threads[0]->prio > thread->prio) {
      thread->birth_time = age++;
      add_new_thread(thread);
      scheduler->current_thread = deleteRoot(scheduler->pq);
      sem_post(&scheduler->current_thread->running);
    }

    if (thread != scheduler->current_thread)
		  sem_wait(&thread->running);	
}

int so_signal(unsigned int io) {
    if (io >= scheduler->io)
		  return -1;
    
    int num = 0;
    while(scheduler->waiting[io]->size) {
        thread_t *thread = deleteRoot(scheduler->waiting[io]);
        thread->birth_time = age++;
        add_new_thread(thread);
        num++;
    }
    so_exec();
    return num;
}

void so_end(void)
{
    if(scheduler) {
        for(int i = 0; i < scheduler->num_threads; i++)
          pthread_join(scheduler->threads[i]->id, NULL);
        for(int i = 0; i < scheduler->num_threads; i++) {
          sem_destroy(&scheduler->threads[i]->running);
          sem_destroy(&scheduler->threads[i]->added_to_scheduler);
          free(scheduler->threads[i]);
        }
        free(scheduler->pq);
        for(int i = 0; i < SO_MAX_NUM_EVENTS; i++)
            free(scheduler->waiting[i]);
        sem_destroy(&scheduler->off);
        free(scheduler->waiting);
        free(scheduler);
        scheduler = NULL;
    }
}