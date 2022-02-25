#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "platform.h"

#include "util.h"
#include "net.h"

struct irq_entry {
    struct irq_entry *next;
    unsigned int irq;
    int (*handler)(unsigned int irq, void *dev); // 割り込みハンドラ
    int flags;
    char name[16];
    void *dev;
};

/* NOTE: if you want to add/delete the entries after intr_run(), you need to protect these lists with a mutex. */
static struct irq_entry *irqs;

// シグナル集合
static sigset_t sigmask;

static pthread_t tid; // 割り込みを模倣する処理のスレッドのスレッドID
static pthread_barrier_t barrier;

int
intr_request_irq(unsigned int irq, int (*handler)(unsigned int irq, void *dev), int flags, const char *name, void *dev)
{

    struct irq_entry *entry;
    debugf("irq=%u, flags=%d, name=%s", irq, flags, name);

    // リストirqsに指定のirqが登録されていないことを確認する
    for (entry = irqs; entry; entry = entry->next) {
        if (entry->irq == irq) {
            // entry->flagsとflagsの両方が1ならすでに登録されていてもOK
            if (entry->flags ^ INTR_IRQ_SHARED || flags ^ INTR_IRQ_SHARED) {
                errorf("conflicts with already registered IRQs");
                return -1;
            }
        }
    }

    // forのentryとこのentryは無関係(使い回されているだけ)
    entry = memory_alloc(sizeof(*entry));
    if (!entry) {
        errorf("memory_alloc() failure");
        return -1;
    }
    entry->irq = irq;
    entry->handler = handler;
    entry->flags = flags;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->dev = dev;
    entry->next = irqs;
    irqs = entry; // リストの先頭に貼り付ける
    sigaddset(&sigmask, irq);
    debugf("registered: irq=%u, name=%s", irq, name);

    return 0;
}

int
intr_raise_irq(unsigned int irq)
{
    // スレッドIDがtidのスレッドに対して、シグナル番号がirqのシグナルを送る？
    return pthread_kill(tid, (int)irq);
}

static int
intr_timer_setup(struct itimerspec *interval)
{
    timer_t id;
    if (timer_create(CLOCK_REALTIME, NULL, &id) == -1) {
        errorf("timer_create: %s", strerror(errno));
        return -1;
    }
    if (timer_settime(id, 0, interval, NULL) == -1) {
        errorf("timer_settime: %s", strerror(errno));
        return -1;
    }
    return 0;
}

// 割り込みスレッドのエントリポイント （SIGHUPのシグナルが来るまで無限ループ）
static void *
intr_thread(void *arg)
{
    const struct timespec ts = {0, 1000000}; /* 1ms */
    struct itimerspec interval = {ts, ts};
    int terminate = 0, sig, err;
    struct irq_entry *entry;
    debugf("start...");
    pthread_barrier_wait(&barrier); // メインスレッドと同期を取るための処理

    if (intr_timer_setup(&interval) == -1) {
        errorf("intr_timer_setup() failure");
        return NULL;
    }
    while (!terminate) {
        err = sigwait(&sigmask, &sig); // シグナル番号を取得(このシグナル番号を割り込み番号として扱う)
        if (err) {
            errorf("sigwait() %s", strerror(err));
            break;
        }
        switch (sig) {
        case SIGHUP:
            terminate = 1;
            break;
        case SIGALRM:
            net_timer_handler();
            break;
        case SIGUSR1: // ソフトウェア割り込み
            net_softirq_handler();
            break;
        default:
            // 割り込み番号がsigの割り込みリクエストのハンドラを実行する（デバイスで使う）
            for (entry = irqs; entry; entry = entry->next) {
                if (entry->irq == (unsigned int)sig) {
                    debugf("irq=%d, name=%s", entry->irq, entry->name);
                    entry->handler(entry->irq, entry->dev);
                }
            }
            break;
        }
    }
    debugf("terminated");
    return NULL;
}

int
intr_run(void)
{
    int err;
    // sigmaskを変更 TODO: sigmaskとは
    err = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
    if (err) {
        errorf("pthread_sigmask() %s", strerror(err));
        return -1;
    }
    // スレッドを作成。tidにスレッドのIDが代入される
    // スレッドのエントリポイントはintr_thread
    err = pthread_create(&tid, NULL, intr_thread, NULL);
    if (err) {
        errorf("pthread_create() %s", strerror(err));
        return -1;
    }
    pthread_barrier_wait(&barrier);
    return 0;
}

void
intr_shutdown(void)
{
    if (pthread_equal(tid, pthread_self()) != 0) {
        /* Thread not created. */
        return;
    }
    pthread_kill(tid, SIGHUP); // 割り込み処理スレッド(intr_threadが動いている)にシグナルSIGHUPを送信
    pthread_join(tid, NULL);
}

int
intr_init(void)
{
    tid = pthread_self();
    pthread_barrier_init(&barrier, NULL, 2);
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGHUP);
    sigaddset(&sigmask, SIGUSR1);
    sigaddset(&sigmask, SIGALRM);
    return 0;
}
