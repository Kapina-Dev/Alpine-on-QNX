#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define THREAD_COUNT 4
#define ITERATIONS 100

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_condition = PTHREAD_COND_INITIALIZER;
static pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
static int ready_threads;
static int start_threads;
static unsigned long counter;

static void *worker(void *argument)
{
    intptr_t index = (intptr_t)argument;
    int i;

    if (pthread_mutex_lock(&lock) != 0) return (void *)1;
    ready_threads++;
    pthread_cond_signal(&ready_condition);
    while (!start_threads)
        if (pthread_cond_wait(&start_condition, &lock) != 0)
            return (void *)2;
    if (pthread_mutex_unlock(&lock) != 0) return (void *)3;

    for (i = 0; i != ITERATIONS; ++i) {
        if (pthread_mutex_lock(&lock) != 0) return (void *)4;
        counter++;
        if (pthread_mutex_unlock(&lock) != 0) return (void *)5;
    }
    return (void *)(index + 10);
}

int main(void)
{
    pthread_t threads[THREAD_COUNT];
    struct timespec deadline;
    void *result;
    int error;
    int i;

    for (i = 0; i != THREAD_COUNT; ++i) {
        error = pthread_create(&threads[i], 0, worker,
            (void *)(intptr_t)i);
        if (error != 0) {
            printf("pthread_create[%d]=%d\n", i, error);
            return 1;
        }
    }
    pthread_mutex_lock(&lock);
    while (ready_threads != THREAD_COUNT)
        pthread_cond_wait(&ready_condition, &lock);
    start_threads = 1;
    pthread_cond_broadcast(&start_condition);
    pthread_mutex_unlock(&lock);

    for (i = 0; i != THREAD_COUNT; ++i) {
        error = pthread_join(threads[i], &result);
        if (error != 0 || result != (void *)(intptr_t)(i + 10)) {
            printf("pthread_join[%d]=%d result=%p\n", i, error, result);
            return 2;
        }
    }
    if (counter != THREAD_COUNT * ITERATIONS) {
        printf("counter=%lu expected=%d\n", counter,
            THREAD_COUNT * ITERATIONS);
        return 3;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&lock);
    error = pthread_cond_timedwait(&ready_condition, &lock, &deadline);
    pthread_mutex_unlock(&lock);
    if (error != ETIMEDOUT) {
        printf("pthread_cond_timedwait=%d expected=%d\n", error, ETIMEDOUT);
        return 4;
    }
    printf("pthread_smoke=PASS threads=%d iterations=%d counter=%lu\n",
        THREAD_COUNT, ITERATIONS, counter);
    return 0;
}
