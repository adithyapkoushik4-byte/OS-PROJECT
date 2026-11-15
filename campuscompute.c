/*
 * CampusCompute - Intelligent Job Scheduling and Resource Management
 * Operating Systems (UE25CS242A) Level 3 Mini-Project
 *
 * Build : make            (gcc -pthread -lrt)
 * Run   : ./campuscompute help
 *
 * Modules
 *   FR1/FR2  CPU scheduling simulator (FCFS, Round Robin, Priority)
 *   FR3      Concurrent logger (pthreads, bounded buffer, mutex + condvars)
 *   FR4      POSIX shared memory + named semaphores between two processes
 *   FR5      Banker's safety algorithm
 *   FR6      Worker processes (fork/waitpid) with SIMULATED failures
 *   FR7      Resource limits (setrlimit) and cleanup verification
 *   FR8      Command-line interface with usage/error messages
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define VERSION    "1.0"
#define LOGFILE    "campuscompute.log"
#define MAXJ       64
#define MAXSEG     8192
#define DEFAULT_Q  4
#define QN         128      /* RR ready-queue capacity (> MAXJ, each job appears once) */

/* ------------------------------------------------------------------ */
/* Shared application log (thread-safe, append mode)                   */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log = NULL;

static void applog(const char *mod, const char *fmt, ...)
{
    char ts[32], msg[300];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_mu);
    if (!g_log)
        g_log = fopen(LOGFILE, "a");
    if (g_log) {
        fprintf(g_log, "[%s] [%-8s] %s\n", ts, mod, msg);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_log_mu);
}

/* Truncate the log file (only the file this program created). */
static void log_reset(void)
{
    pthread_mutex_lock(&g_log_mu);
    if (g_log) { fclose(g_log); g_log = NULL; }
    FILE *f = fopen(LOGFILE, "w");
    if (f) fclose(f);
    pthread_mutex_unlock(&g_log_mu);
}

static void log_close(void)
{
    pthread_mutex_lock(&g_log_mu);
    if (g_log) { fclose(g_log); g_log = NULL; }
    pthread_mutex_unlock(&g_log_mu);
}

/* Parse an integer in [lo,hi]; returns 0 on success. */
static int parse_int(const char *s, long lo, long hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

/* ================================================================== */
/* FR1 / FR2 : Job management and CPU scheduling                       */
/* ================================================================== */
typedef enum { ST_NEW, ST_READY, ST_RUNNING, ST_TERMINATED } JobStatus;

static const char *status_name(JobStatus s)
{
    switch (s) {
    case ST_NEW:        return "NEW";
    case ST_READY:      return "READY";
    case ST_RUNNING:    return "RUNNING";
    default:            return "TERMINATED";
    }
}

typedef struct {
    int id, arrival, burst, priority;          /* input fields          */
    int remaining, first_start;                /* simulation state      */
    int completion, turnaround, waiting;       /* computed metrics      */
    JobStatus status;
} Job;

typedef struct { int job; int from, to; } Seg;  /* job id 0 == CPU idle  */

typedef struct {
    const char *name;
    int quantum;
    Job j[MAXJ];
    int n;
    Seg seg[MAXSEG];
    int ns;
    double act, atat, awt;
    int switches, makespan;
} Sched;

static void init_job(Job *j, int id, int ar, int bt, int pr)
{
    memset(j, 0, sizeof *j);
    j->id = id; j->arrival = ar; j->burst = bt; j->priority = pr;
    j->remaining = bt; j->first_start = -1; j->status = ST_NEW;
}

/* Repeatable built-in test set (note P2 and P5 share priority 1). */
static int default_jobs(Job *jobs)
{
    static const int d[][4] = {
        /* id arr burst prio */
        {1, 0, 8, 3}, {2, 1, 4, 1}, {3, 2, 9, 4}, {4, 3, 5, 2}, {5, 4, 2, 1}
    };
    int n = (int)(sizeof d / sizeof d[0]);
    for (int i = 0; i < n; i++)
        init_job(&jobs[i], d[i][0], d[i][1], d[i][2], d[i][3]);
    return n;
}

/* File format: one job per line "id arrival burst priority"; '#' comments. */
static int load_jobs(const char *path, Job *jobs)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[256];
    int n = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        int id, ar, bt, pr, c;
        char extra;
        c = sscanf(p, "%d %d %d %d %c", &id, &ar, &bt, &pr, &extra);
        if (c == 5 && extra == '#') c = 4;        /* trailing comment */
        if (c != 4) {
            fprintf(stderr, "error: %s:%d: expected 'id arrival burst priority' "
                            "(4 integers)\n", path, ln);
            fclose(f); return -1;
        }
        if (id <= 0 || ar < 0 || ar > 10000 || bt < 1 || bt > 100 || pr < 0 || pr > 100) {
            fprintf(stderr, "error: %s:%d: value out of range "
                            "(id>0, 0<=arrival<=10000, 1<=burst<=100, 0<=priority<=100)\n",
                    path, ln);
            fclose(f); return -1;
        }
        if (n >= MAXJ) {
            fprintf(stderr, "error: %s: too many jobs (max %d)\n", path, MAXJ);
            fclose(f); return -1;
        }
        for (int i = 0; i < n; i++)
            if (jobs[i].id == id) {
                fprintf(stderr, "error: %s:%d: duplicate job id %d\n", path, ln, id);
                fclose(f); return -1;
            }
        init_job(&jobs[n++], id, ar, bt, pr);
    }
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "error: %s: no jobs found\n", path);
        return -1;
    }
    return n;
}

/* Indices sorted by (arrival, id) - insertion sort, stable and tiny. */
static void order_by_arrival(const Job *j, int n, int *ord)
{
    for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++) {
        int k = ord[i], p = i - 1;
        while (p >= 0 && (j[ord[p]].arrival > j[k].arrival ||
               (j[ord[p]].arrival == j[k].arrival && j[ord[p]].id > j[k].id))) {
            ord[p + 1] = ord[p]; p--;
        }
        ord[p + 1] = k;
    }
}

static void sched_init(Sched *S, const char *name, const Job *in, int n, int q)
{
    memset(S, 0, sizeof *S);
    S->name = name; S->n = n; S->quantum = q;
    for (int i = 0; i < n; i++) {
        S->j[i] = in[i];
        S->j[i].remaining = in[i].burst;
        S->j[i].first_start = -1;
        S->j[i].status = ST_NEW;
    }
}

static void add_seg(Sched *S, int job, int from, int to)
{
    if (to <= from || S->ns >= MAXSEG) return;
    S->seg[S->ns].job = job;
    S->seg[S->ns].from = from;
    S->seg[S->ns].to = to;
    S->ns++;
}

static void finalize(Sched *S)
{
    double sc = 0, st = 0, sw = 0;
    for (int i = 0; i < S->n; i++) {
        Job *j = &S->j[i];
        j->turnaround = j->completion - j->arrival;
        j->waiting = j->turnaround - j->burst;
        j->status = ST_TERMINATED;
        sc += j->completion; st += j->turnaround; sw += j->waiting;
        applog("SCHED", "%s: P%d NEW->READY->RUNNING->TERMINATED ct=%d tat=%d wt=%d",
               S->name, j->id, j->completion, j->turnaround, j->waiting);
    }
    S->act = sc / S->n; S->atat = st / S->n; S->awt = sw / S->n;
    S->makespan = S->ns ? S->seg[S->ns - 1].to : 0;
    int prev = -1;
    for (int i = 0; i < S->ns; i++) {
        if (S->seg[i].job == 0) continue;
        if (prev != -1 && prev != S->seg[i].job) S->switches++;
        prev = S->seg[i].job;
    }
}

static void sim_fcfs(const Job *in, int n, Sched *S)
{
    sched_init(S, "FCFS", in, n, 0);
    int ord[MAXJ], t = 0;
    order_by_arrival(S->j, n, ord);
    for (int k = 0; k < n; k++) {
        Job *j = &S->j[ord[k]];
        if (t < j->arrival) { add_seg(S, 0, t, j->arrival); t = j->arrival; }
        j->status = ST_RUNNING;
        j->first_start = t;
        add_seg(S, j->id, t, t + j->burst);
        t += j->burst;
        j->remaining = 0;
        j->completion = t;
    }
    finalize(S);
}

/* Non-preemptive priority: smaller number = higher priority.
 * Tie-break: earlier arrival, then smaller job id. */
static void sim_priority(const Job *in, int n, Sched *S)
{
    sched_init(S, "PRIORITY", in, n, 0);
    int done[MAXJ] = {0}, cnt = 0, t = 0;
    while (cnt < n) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (done[i] || S->j[i].arrival > t) continue;
            if (best == -1) { best = i; continue; }
            const Job *a = &S->j[i], *b = &S->j[best];
            if (a->priority < b->priority ||
               (a->priority == b->priority && a->arrival < b->arrival) ||
               (a->priority == b->priority && a->arrival == b->arrival && a->id < b->id))
                best = i;
        }
        if (best == -1) {                       /* CPU idle until next arrival */
            int nxt = INT_MAX;
            for (int i = 0; i < n; i++)
                if (!done[i] && S->j[i].arrival < nxt) nxt = S->j[i].arrival;
            add_seg(S, 0, t, nxt);
            t = nxt;
            continue;
        }
        Job *j = &S->j[best];
        j->status = ST_RUNNING;
        j->first_start = t;
        add_seg(S, j->id, t, t + j->burst);
        t += j->burst;
        j->remaining = 0;
        j->completion = t;
        done[best] = 1; cnt++;
    }
    finalize(S);
}

static void rr_admit(Sched *S, const int *ord, int n, int *nxt, int t,
                     int *queue, int *qt, int *qc)
{
    while (*nxt < n && S->j[ord[*nxt]].arrival <= t) {
        queue[*qt] = ord[*nxt];
        *qt = (*qt + 1) % QN;
        (*qc)++;
        S->j[ord[*nxt]].status = ST_READY;
        (*nxt)++;
    }
}

/* Round Robin. Tie rule: jobs that arrive at the same instant a quantum
 * expires are queued BEFORE the pre-empted job is re-queued. */
static void sim_rr(const Job *in, int n, int q, Sched *S)
{
    sched_init(S, "ROUND ROBIN", in, n, q);
    int ord[MAXJ], queue[QN], qh = 0, qt = 0, qc = 0, nxt = 0, t = 0, done = 0;
    order_by_arrival(S->j, n, ord);
    rr_admit(S, ord, n, &nxt, t, queue, &qt, &qc);
    while (done < n) {
        if (qc == 0) {                          /* idle gap */
            int na = S->j[ord[nxt]].arrival;
            add_seg(S, 0, t, na);
            t = na;
            rr_admit(S, ord, n, &nxt, t, queue, &qt, &qc);
            continue;
        }
        int idx = queue[qh];
        qh = (qh + 1) % QN; qc--;
        Job *j = &S->j[idx];
        if (j->first_start < 0) j->first_start = t;
        j->status = ST_RUNNING;
        int run = j->remaining < q ? j->remaining : q;
        add_seg(S, j->id, t, t + run);
        t += run;
        j->remaining -= run;
        rr_admit(S, ord, n, &nxt, t, queue, &qt, &qc);
        if (j->remaining > 0) {
            queue[qt] = idx; qt = (qt + 1) % QN; qc++;
            j->status = ST_READY;
        } else {
            j->completion = t;
            done++;
        }
    }
    finalize(S);
}

static void print_sched(const Sched *S)
{
    printf("\n=== %s scheduling ===\n", S->name);
    if (!strcmp(S->name, "FCFS"))
        printf("Rule: jobs run in arrival order; tie-break = smaller job id.\n");
    else if (!strcmp(S->name, "PRIORITY"))
        printf("Rule: non-preemptive; lower number = higher priority.\n"
               "Tie-break: same priority -> earlier arrival, then smaller job id.\n");
    else
        printf("Rule: time quantum = %d. If a job arrives at the instant a quantum\n"
               "expires, it is queued before the pre-empted job.\n", S->quantum);

    printf("\n %-4s %-8s %-6s %-9s %-11s %-6s %-6s %-6s %s\n",
           "Job", "Arrival", "Burst", "Priority", "FirstRun", "CT", "TAT", "WT", "Status");
    printf(" ---- -------- ------ --------- ----------- ------ ------ ------ ----------\n");
    for (int i = 0; i < S->n; i++) {
        const Job *j = &S->j[i];
        char nm[16];
        snprintf(nm, sizeof nm, "P%d", j->id);
        printf(" %-4s %-8d %-6d %-9d %-11d %-6d %-6d %-6d %s\n", nm, j->arrival,
               j->burst, j->priority, j->first_start, j->completion, j->turnaround,
               j->waiting, status_name(j->status));
    }
    printf("\nAverage completion time  : %.2f\n", S->act);
    printf("Average turnaround time  : %.2f\n", S->atat);
    printf("Average waiting time     : %.2f\n", S->awt);

    printf("\nExecution order / Gantt chart:\n  ");
    for (int s = 0; s < S->ns; s++) {
        if (s == 0) printf("%d ", S->seg[s].from);
        if (S->seg[s].job == 0) printf("[IDLE] %d ", S->seg[s].to);
        else                    printf("[P%d] %d ", S->seg[s].job, S->seg[s].to);
    }
    printf("\n");
}

static int sched_run(const char *pol, const Job *jobs, int n, int q)
{
    static Sched a, b, c;       /* large; keep off the stack */
    if (!strcmp(pol, "fcfs")) {
        sim_fcfs(jobs, n, &a); print_sched(&a);
    } else if (!strcmp(pol, "rr")) {
        sim_rr(jobs, n, q, &b); print_sched(&b);
    } else if (!strcmp(pol, "priority")) {
        sim_priority(jobs, n, &c); print_sched(&c);
    } else if (!strcmp(pol, "compare")) {
        sim_fcfs(jobs, n, &a);
        sim_rr(jobs, n, q, &b);
        sim_priority(jobs, n, &c);
        const Sched *all[3] = { &a, &b, &c };
        printf("\n=== Scheduling comparison (same %d jobs, RR quantum = %d) ===\n\n", n, q);
        printf(" %-12s %-9s %-9s %-9s %-10s %s\n",
               "Policy", "Avg CT", "Avg TAT", "Avg WT", "CtxSwitch", "Makespan");
        printf(" ------------ --------- --------- --------- ---------- --------\n");
        int best = 0;
        for (int i = 0; i < 3; i++) {
            printf(" %-12s %-9.2f %-9.2f %-9.2f %-10d %d\n", all[i]->name, all[i]->act,
                   all[i]->atat, all[i]->awt, all[i]->switches, all[i]->makespan);
            if (all[i]->awt < all[best]->awt) best = i;
        }
        printf("\nLowest average waiting time: %s (%.2f)\n", all[best]->name, all[best]->awt);
        printf("Gantt (FCFS)    : ");
        for (int s = 0; s < a.ns; s++) printf("%sP%d", s ? "|" : "", a.seg[s].job);
        printf("\nGantt (RR)      : ");
        for (int s = 0; s < b.ns; s++) printf("%sP%d", s ? "|" : "", b.seg[s].job);
        printf("\nGantt (PRIORITY): ");
        for (int s = 0; s < c.ns; s++) printf("%sP%d", s ? "|" : "", c.seg[s].job);
        printf("\n");
        applog("SCHED", "comparison done; best avg waiting = %s", all[best]->name);
    } else {
        return -1;
    }
    return 0;
}

static int cmd_sched(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "error: sched needs a policy: fcfs | rr | priority | compare\n"
                        "usage: campuscompute sched <policy> [-f jobs.txt] [-q quantum]\n");
        return 2;
    }
    const char *pol = argv[0], *file = NULL;
    int q = DEFAULT_Q;
    if (strcmp(pol, "fcfs") && strcmp(pol, "rr") && strcmp(pol, "priority") &&
        strcmp(pol, "compare")) {
        fprintf(stderr, "error: unknown policy '%s' (use fcfs, rr, priority or compare)\n", pol);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            file = argv[++i];
        } else if (!strcmp(argv[i], "-q") && i + 1 < argc) {
            if (parse_int(argv[++i], 1, 100, &q)) {
                fprintf(stderr, "error: quantum must be an integer in 1..100\n");
                return 2;
            }
        } else {
            fprintf(stderr, "error: bad option '%s'\nusage: campuscompute sched <policy> "
                            "[-f jobs.txt] [-q quantum]\n", argv[i]);
            return 2;
        }
    }
    Job jobs[MAXJ];
    int n;
    if (file) {
        n = load_jobs(file, jobs);
        if (n < 0) return 1;
        printf("Loaded %d jobs from %s\n", n, file);
    } else {
        n = default_jobs(jobs);
        printf("Using built-in demo job set (%d jobs)\n", n);
    }
    return sched_run(pol, jobs, n, q) == 0 ? 0 : 1;
}

/* ================================================================== */
/* FR3 : Concurrent logger - bounded producer/consumer with pthreads   */
/* ================================================================== */
#define QCAP 5

typedef struct { char text[96]; int producer; } LogItem;   /* producer -1 = stop */

static struct {
    LogItem buf[QCAP];
    int head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t not_full, not_empty;
    int produced, consumed, max_depth, full_waits, empty_waits;
} Q;

static void queue_put(const LogItem *it, int *depth)
{
    pthread_mutex_lock(&Q.mu);
    while (Q.count == QCAP) {               /* buffer full: wait */
        Q.full_waits++;
        pthread_cond_wait(&Q.not_full, &Q.mu);
    }
    Q.buf[Q.tail] = *it;
    Q.tail = (Q.tail + 1) % QCAP;
    Q.count++;
    if (it->producer >= 0) Q.produced++;
    if (Q.count > Q.max_depth) Q.max_depth = Q.count;
    if (depth) *depth = Q.count;
    pthread_cond_signal(&Q.not_empty);
    pthread_mutex_unlock(&Q.mu);
}

static void queue_get(LogItem *it, int *depth)
{
    pthread_mutex_lock(&Q.mu);
    while (Q.count == 0) {                  /* buffer empty: wait */
        Q.empty_waits++;
        pthread_cond_wait(&Q.not_empty, &Q.mu);
    }
    *it = Q.buf[Q.head];
    Q.head = (Q.head + 1) % QCAP;
    Q.count--;
    if (it->producer >= 0) Q.consumed++;
    if (depth) *depth = Q.count;
    pthread_cond_signal(&Q.not_full);
    pthread_mutex_unlock(&Q.mu);
}

typedef struct { int id, items; } ProdArg;
typedef struct { int id, written; } ConsArg;

static void *producer_thread(void *arg)
{
    ProdArg *pa = arg;
    for (int k = 1; k <= pa->items; k++) {
        LogItem it;
        it.producer = pa->id;
        snprintf(it.text, sizeof it.text, "JOB_SUBMITTED job=%d user=student%d",
                 pa->id * 100 + k, pa->id);
        int depth = 0;
        queue_put(&it, &depth);
        printf("[producer %d] enqueued event %d/%d  (queue %d/%d)\n",
               pa->id, k, pa->items, depth, QCAP);
        usleep(1000 + ((pa->id * 3 + k * 5) % 4) * 1000);
    }
    return NULL;
}

static void *consumer_thread(void *arg)
{
    ConsArg *ca = arg;
    for (;;) {
        LogItem it;
        int depth = 0;
        queue_get(&it, &depth);
        if (it.producer < 0) break;         /* poison pill: orderly shutdown */
        usleep(6000);                       /* slow disk: lets the queue fill up */
        applog("LOGGER", "consumer-%d: %s (producer-%d)", ca->id, it.text, it.producer);
        printf("[consumer %d] wrote log entry: %s  (queue %d/%d)\n",
               ca->id, it.text, depth, QCAP);
        ca->written++;
    }
    return NULL;
}

static int logger_run(int np, int items, int nc)
{
    memset(&Q, 0, sizeof Q);
    pthread_mutex_init(&Q.mu, NULL);
    pthread_cond_init(&Q.not_full, NULL);
    pthread_cond_init(&Q.not_empty, NULL);

    pthread_t pt[8], ct[4];
    ProdArg pa[8];
    ConsArg ca[4];
    int pcreated = 0, ccreated = 0;

    printf("\n=== Concurrent logger: %d producers x %d events, %d consumer(s), "
           "buffer size %d ===\n", np, items, nc, QCAP);
    applog("LOGGER", "start: producers=%d items=%d consumers=%d capacity=%d",
           np, items, nc, QCAP);

    for (int i = 0; i < nc; i++) {
        ca[i].id = i + 1; ca[i].written = 0;
        if (pthread_create(&ct[i], NULL, consumer_thread, &ca[i]) != 0) {
            perror("pthread_create(consumer)");
            break;
        }
        ccreated++;
    }
    for (int i = 0; i < np && ccreated > 0; i++) {
        pa[i].id = i + 1; pa[i].items = items;
        if (pthread_create(&pt[i], NULL, producer_thread, &pa[i]) != 0) {
            perror("pthread_create(producer)");
            break;
        }
        pcreated++;
    }
    for (int i = 0; i < pcreated; i++) pthread_join(pt[i], NULL);

    LogItem stop = { "STOP", -1 };          /* one poison pill per consumer */
    for (int i = 0; i < ccreated; i++) queue_put(&stop, NULL);
    int wrote = 0;
    for (int i = 0; i < ccreated; i++) {
        pthread_join(ct[i], NULL);
        wrote += ca[i].written;
    }

    int expect = np * items;
    int ok = (pcreated == np && ccreated == nc && Q.produced == expect &&
              Q.consumed == expect && wrote == expect && Q.count == 0);
    printf("\nLogger summary:\n");
    printf("  events produced        : %d (expected %d)\n", Q.produced, expect);
    printf("  events consumed/logged : %d\n", Q.consumed);
    printf("  max buffer depth       : %d / %d\n", Q.max_depth, QCAP);
    printf("  producer waits (full)  : %d\n", Q.full_waits);
    printf("  consumer waits (empty) : %d\n", Q.empty_waits);
    printf("  threads joined         : %d producers, %d consumers\n", pcreated, ccreated);
    printf("  result                 : %s\n", ok ? "PASS - no events lost, all threads terminated"
                                                  : "FAIL");
    applog("LOGGER", "finished: produced=%d consumed=%d result=%s",
           Q.produced, Q.consumed, ok ? "PASS" : "FAIL");

    pthread_mutex_destroy(&Q.mu);
    pthread_cond_destroy(&Q.not_full);
    pthread_cond_destroy(&Q.not_empty);
    return ok;
}

static int cmd_logger(int argc, char **argv)
{
    int np = 3, items = 6, nc = 2;
    if (argc > 3 ||
        (argc > 0 && parse_int(argv[0], 1, 8, &np)) ||
        (argc > 1 && parse_int(argv[1], 1, 100, &items)) ||
        (argc > 2 && parse_int(argv[2], 1, 4, &nc))) {
        fprintf(stderr, "error: bad logger arguments\n"
                        "usage: campuscompute logger [producers 1-8] [events 1-100] "
                        "[consumers 1-4]\n");
        return 2;
    }
    log_reset();
    int ok = logger_run(np, items, nc);
    printf("Log file written: ./%s\n", LOGFILE);
    return ok ? 0 : 1;
}

/* ================================================================== */
/* FR4 : POSIX shared memory + named semaphores                        */
/* ================================================================== */
typedef struct { int seq; int value; int done; char msg[48]; } Mailbox;

static void ipc_names(char *shm, char *se, char *sf, size_t n)
{
    unsigned u = (unsigned)getuid();
    snprintf(shm, n, "/cc_shm_%u", u);
    snprintf(se, n, "/cc_sem_empty_%u", u);
    snprintf(sf, n, "/cc_sem_full_%u", u);
}

static int sem_wait_timeout(sem_t *s, int sec)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += sec;
    while (sem_timedwait(s, &ts) == -1) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Child = producer. Re-attaches by NAME to demonstrate independent processes. */
static void ipc_child(const char *shmn, const char *sen, const char *sfn, int count)
{
    int fd = shm_open(shmn, O_RDWR, 0);
    if (fd < 0) { perror("child shm_open"); _exit(1); }
    Mailbox *mb = mmap(NULL, sizeof *mb, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    sem_t *se = sem_open(sen, 0), *sf = sem_open(sfn, 0);
    if (mb == MAP_FAILED || se == SEM_FAILED || sf == SEM_FAILED) {
        perror("child attach"); _exit(1);
    }
    for (int i = 1; i <= count; i++) {
        if (sem_wait_timeout(se, 5)) { perror("child sem_wait"); _exit(1); }
        mb->seq = i;
        mb->value = i * 10 + 3;
        mb->done = (i == count);
        snprintf(mb->msg, sizeof mb->msg, "result of job %d", i);
        printf("  [producer pid=%d] wrote  seq=%d value=%d \"%s\"\n",
               getpid(), mb->seq, mb->value, mb->msg);
        fflush(stdout);
        sem_post(sf);
    }
    munmap(mb, sizeof *mb);
    close(fd);
    sem_close(se); sem_close(sf);
    _exit(0);
}

static int ipc_run(int count)
{
    char shmn[64], sen[64], sfn[64];
    ipc_names(shmn, sen, sfn, sizeof shmn);
    int fd = -1, rc = 0, created_shm = 0, created_se = 0, created_sf = 0;
    Mailbox *mb = MAP_FAILED;
    sem_t *se = SEM_FAILED, *sf = SEM_FAILED;
    pid_t pid = -1;

    printf("\n=== Shared-memory IPC: %d messages via %s, semaphores %s / %s ===\n",
           count, shmn, sen, sfn);

    /* Remove stale objects with OUR OWN names only (left by a crashed earlier run). */
    shm_unlink(shmn); sem_unlink(sen); sem_unlink(sfn);

    fd = shm_open(shmn, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); goto out; }
    created_shm = 1;
    if (ftruncate(fd, sizeof(Mailbox)) < 0) { perror("ftruncate"); goto out; }
    mb = mmap(NULL, sizeof *mb, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mb == MAP_FAILED) { perror("mmap"); goto out; }
    memset(mb, 0, sizeof *mb);

    se = sem_open(sen, O_CREAT | O_EXCL, 0600, 1);     /* slot empty: 1 */
    if (se == SEM_FAILED) { perror("sem_open(empty)"); goto out; }
    created_se = 1;
    sf = sem_open(sfn, O_CREAT | O_EXCL, 0600, 0);     /* slot full : 0 */
    if (sf == SEM_FAILED) { perror("sem_open(full)"); goto out; }
    created_sf = 1;

    fflush(NULL);
    pid = fork();
    if (pid < 0) { perror("fork"); goto out; }
    if (pid == 0) ipc_child(shmn, sen, sfn, count);

    printf("  [consumer pid=%d] created shared memory + semaphores, producer pid=%d\n",
           getpid(), pid);
    long sum = 0, expect = 0;
    for (int i = 1; i <= count; i++) expect += i * 10 + 3;
    int received = 0, last = 0;
    while (!last) {
        if (sem_wait_timeout(sf, 5)) {
            fprintf(stderr, "  [consumer] timeout/error waiting for producer: %s\n",
                    strerror(errno));
            goto out;
        }
        printf("  [consumer pid=%d] read   seq=%d value=%d \"%s\"\n",
               getpid(), mb->seq, mb->value, mb->msg);
        sum += mb->value;
        received++;
        last = mb->done;
        sem_post(se);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    pid = -1;
    int child_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  received %d messages, checksum %ld (expected %ld), producer exit %s\n",
           received, sum, expect, child_ok ? "OK" : "ABNORMAL");
    rc = (sum == expect && received == count && child_ok);

out:
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    if (mb != MAP_FAILED) munmap(mb, sizeof *mb);
    if (fd >= 0) close(fd);
    if (se != SEM_FAILED) sem_close(se);
    if (sf != SEM_FAILED) sem_close(sf);
    if (created_shm) shm_unlink(shmn);
    if (created_se)  sem_unlink(sen);
    if (created_sf)  sem_unlink(sfn);
    printf("  cleanup: unmapped, shm_unlink(%s), sem_unlink(%s), sem_unlink(%s) done\n",
           shmn, sen, sfn);
    printf("  result : %s\n", rc ? "PASS" : "FAIL");
    applog("IPC", "shared-memory exchange count=%d result=%s", count, rc ? "PASS" : "FAIL");
    return rc;
}

static int cmd_ipc(int argc, char **argv)
{
    int count = 5;
    if (argc > 1 || (argc == 1 && parse_int(argv[0], 1, 1000, &count))) {
        fprintf(stderr, "error: bad ipc argument\nusage: campuscompute ipc [messages 1-1000]\n");
        return 2;
    }
    return ipc_run(count) ? 0 : 1;
}

/* ================================================================== */
/* FR5 : Banker's algorithm (safety check)                             */
/* ================================================================== */
#define MAXP 10
#define MAXM 10

typedef struct {
    int n, m;
    int avail[MAXM];
    int alloc[MAXP][MAXM];
    int max[MAXP][MAXM];
} BankerState;

static void print_matrix(const char *title, int n, int m, int a[][MAXM])
{
    printf("  %s\n      ", title);
    for (int j = 0; j < m; j++) printf("%c  ", 'A' + j);
    printf("\n");
    for (int i = 0; i < n; i++) {
        printf("  P%-2d ", i);
        for (int j = 0; j < m; j++) printf("%-2d ", a[i][j]);
        printf("\n");
    }
}

/* Returns 1 safe, 0 unsafe, -1 invalid state. */
static int banker_safety(const BankerState *s)
{
    int need[MAXP][MAXM], work[MAXM], finish[MAXP] = {0}, seq[MAXP], cnt = 0;
    int n = s->n, m = s->m;

    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) {
            need[i][j] = s->max[i][j] - s->alloc[i][j];      /* Need = Max - Allocation */
            if (need[i][j] < 0) {
                fprintf(stderr, "error: invalid state: Allocation exceeds Maximum for P%d\n", i);
                return -1;
            }
        }
    int tmp[MAXP][MAXM];
    memcpy(tmp, s->alloc, sizeof tmp);
    print_matrix("Allocation", n, m, tmp);
    memcpy(tmp, s->max, sizeof tmp);
    print_matrix("Maximum", n, m, tmp);
    print_matrix("Need = Maximum - Allocation", n, m, need);
    printf("  Available: ");
    for (int j = 0; j < m; j++) printf("%c=%d ", 'A' + j, s->avail[j]);
    printf("\n\n  Safety algorithm trace:\n");

    memcpy(work, s->avail, sizeof work);
    while (cnt < n) {
        int found = 0;
        for (int i = 0; i < n && !found; i++) {
            if (finish[i]) continue;
            int ok = 1;
            for (int j = 0; j < m; j++) if (need[i][j] > work[j]) { ok = 0; break; }
            if (!ok) continue;
            printf("    P%d can finish (Need <= Work). Work: ", i);
            for (int j = 0; j < m; j++) printf("%d ", work[j]);
            printf("-> ");
            for (int j = 0; j < m; j++) work[j] += s->alloc[i][j];
            for (int j = 0; j < m; j++) printf("%d ", work[j]);
            printf("\n");
            finish[i] = 1; seq[cnt++] = i; found = 1;
        }
        if (!found) break;
    }
    if (cnt == n) {
        printf("\n  SAFE state. Safe sequence: ");
        for (int i = 0; i < n; i++) printf("P%d%s", seq[i], i < n - 1 ? " -> " : "\n");
        applog("BANKER", "state SAFE");
        return 1;
    }
    printf("\n  UNSAFE state: no process can proceed with Work = ");
    for (int j = 0; j < m; j++) printf("%d ", work[j]);
    printf("\n  Processes that cannot finish: ");
    for (int i = 0; i < n; i++) if (!finish[i]) printf("P%d ", i);
    printf("\n");
    applog("BANKER", "state UNSAFE");
    return 0;
}

static void banker_builtin(int safe, BankerState *s)
{
    memset(s, 0, sizeof *s);
    if (safe) {          /* classic textbook example: 5 processes, 3 resource types */
        static const int al[5][3] = {{0,1,0},{2,0,0},{3,0,2},{2,1,1},{0,0,2}};
        static const int mx[5][3] = {{7,5,3},{3,2,2},{9,0,2},{2,2,2},{4,3,3}};
        s->n = 5; s->m = 3;
        s->avail[0] = 3; s->avail[1] = 3; s->avail[2] = 2;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 3; j++) { s->alloc[i][j] = al[i][j]; s->max[i][j] = mx[i][j]; }
    } else {             /* every process still needs (1,1) but only (1,0) is free */
        static const int al[3][2] = {{1,1},{1,1},{0,1}};
        static const int mx[3][2] = {{2,2},{2,2},{1,2}};
        s->n = 3; s->m = 2;
        s->avail[0] = 1; s->avail[1] = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 2; j++) { s->alloc[i][j] = al[i][j]; s->max[i][j] = mx[i][j]; }
    }
}

/* File format: "n m", Available(m), Allocation(n rows), Maximum(n rows). */
static int banker_load(const char *path, BankerState *s)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno)); return -1; }
    memset(s, 0, sizeof *s);
    int ok = (fscanf(f, "%d %d", &s->n, &s->m) == 2 &&
              s->n >= 1 && s->n <= MAXP && s->m >= 1 && s->m <= MAXM);
    for (int j = 0; ok && j < s->m; j++)
        ok = fscanf(f, "%d", &s->avail[j]) == 1 && s->avail[j] >= 0;
    for (int i = 0; ok && i < s->n; i++)
        for (int j = 0; ok && j < s->m; j++)
            ok = fscanf(f, "%d", &s->alloc[i][j]) == 1 && s->alloc[i][j] >= 0;
    for (int i = 0; ok && i < s->n; i++)
        for (int j = 0; ok && j < s->m; j++)
            ok = fscanf(f, "%d", &s->max[i][j]) == 1 && s->max[i][j] >= 0;
    fclose(f);
    if (!ok) {
        fprintf(stderr, "error: '%s' is malformed. Expected: n m, then Available (m values), "
                        "Allocation (n rows), Maximum (n rows); n<=%d, m<=%d, values>=0\n",
                path, MAXP, MAXM);
        return -1;
    }
    return 0;
}

static int cmd_banker(int argc, char **argv)
{
    BankerState s;
    if (argc >= 1 && !strcmp(argv[0], "safe") && argc == 1) {
        banker_builtin(1, &s);
        printf("\n=== Banker's algorithm: built-in SAFE example ===\n");
    } else if (argc >= 1 && !strcmp(argv[0], "unsafe") && argc == 1) {
        banker_builtin(0, &s);
        printf("\n=== Banker's algorithm: built-in UNSAFE example ===\n");
    } else if (argc == 2 && !strcmp(argv[0], "file")) {
        if (banker_load(argv[1], &s)) return 1;
        printf("\n=== Banker's algorithm: state loaded from %s ===\n", argv[1]);
    } else {
        fprintf(stderr, "error: bad banker arguments\n"
                        "usage: campuscompute banker safe | unsafe | file <path>\n");
        return 2;
    }
    int r = banker_safety(&s);
    return r < 0 ? 1 : 0;       /* both SAFE and UNSAFE are valid analysis results */
}

/* ================================================================== */
/* FR6 : Worker processes - fork / waitpid / SIMULATED failure         */
/* ================================================================== */
enum { W_NORMAL = 0, W_FAIL = 1, W_SIGNAL = 2 };
static const char *wnames[] = { "normal", "fail", "signal" };

static void worker_child(int kind)
{
    unsigned long acc = 0;
    printf("  [worker pid=%d] started (%s)\n", getpid(), wnames[kind]);
    fflush(stdout);
    for (int c = 1; c <= 3; c++) {
        for (unsigned long k = 0; k < 2000000UL; k++) acc += k % 7;
        usleep(100000);
        printf("  [worker pid=%d] finished chunk %d/3 (acc=%lu)\n", getpid(), c, acc);
        fflush(stdout);
    }
    if (kind == W_FAIL) {
        printf("  [worker pid=%d] *** SIMULATED FAILURE *** exiting with code 3 "
               "(injected on purpose, not a real OS fault)\n", getpid());
        fflush(stdout);
        _exit(3);
    }
    if (kind == W_SIGNAL) {
        printf("  [worker pid=%d] *** SIMULATED CRASH *** raising SIGUSR1 on itself "
               "(injected on purpose, not a real OS fault)\n", getpid());
        fflush(stdout);
        raise(SIGUSR1);
        _exit(99);
    }
    printf("  [worker pid=%d] work complete, exiting with code 0\n", getpid());
    fflush(stdout);
    _exit(0);
}

static int worker_report(pid_t pid, int st, int kind)
{
    int ok = 0;
    if (WIFEXITED(st)) {
        int c = WEXITSTATUS(st);
        printf("  [parent] waitpid(%d): exited with status %d\n", pid, c);
        if (kind == W_NORMAL && c == 0) {
            printf("  [parent] worker %d COMPLETED NORMALLY\n", pid); ok = 1;
        } else if (kind == W_FAIL && c == 3) {
            printf("  [parent] worker %d FAILED (non-zero exit) - detected; this was a "
                   "SIMULATED failure\n", pid); ok = 1;
        } else {
            printf("  [parent] worker %d gave UNEXPECTED status\n", pid);
        }
    } else if (WIFSIGNALED(st)) {
        int sg = WTERMSIG(st);
        printf("  [parent] waitpid(%d): killed by signal %d (%s)\n", pid, sg, strsignal(sg));
        if (kind == W_SIGNAL && sg == SIGUSR1) {
            printf("  [parent] worker %d CRASHED - detected; this was a SIMULATED crash\n", pid);
            ok = 1;
        } else {
            printf("  [parent] worker %d gave UNEXPECTED termination\n", pid);
        }
    }
    applog("WORKER", "pid=%d kind=%s detected=%s", pid, wnames[kind], ok ? "as-expected" : "UNEXPECTED");
    return ok;
}

static int worker_run(int kind)
{
    printf("\n=== Worker process test: %s ===\n", wnames[kind]);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (pid == 0) worker_child(kind);
    printf("  [parent pid=%d] forked worker pid=%d, waiting...\n", getpid(), pid);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return 0; }
    }
    return worker_report(pid, st, kind);
}

static int worker_all(void)
{
    printf("\n=== Worker pool: 3 concurrent workers (normal, simulated exit failure, "
           "simulated crash) ===\n");
    pid_t pids[3];
    fflush(NULL);
    for (int k = 0; k < 3; k++) {
        pids[k] = fork();
        if (pids[k] < 0) { perror("fork"); return 0; }
        if (pids[k] == 0) worker_child(k);
        printf("  [parent] forked %s worker pid=%d\n", wnames[k], pids[k]);
    }
    int good = 0;
    for (int left = 3; left > 0; left--) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p < 0) { if (errno == EINTR) { left++; continue; } perror("waitpid"); break; }
        for (int k = 0; k < 3; k++)
            if (pids[k] == p) good += worker_report(p, st, k);
    }
    printf("  pool result: %d/3 workers behaved as expected -> %s\n", good, good == 3 ? "PASS" : "FAIL");
    return good == 3;
}

static int cmd_worker(int argc, char **argv)
{
    if (argc != 1) goto bad;
    if (!strcmp(argv[0], "normal")) return worker_run(W_NORMAL) ? 0 : 1;
    if (!strcmp(argv[0], "fail"))   return worker_run(W_FAIL)   ? 0 : 1;
    if (!strcmp(argv[0], "signal")) return worker_run(W_SIGNAL) ? 0 : 1;
    if (!strcmp(argv[0], "all"))    return worker_all()         ? 0 : 1;
bad:
    fprintf(stderr, "error: bad worker argument\n"
                    "usage: campuscompute worker normal | fail | signal | all\n");
    return 2;
}

/* ================================================================== */
/* FR7 : Resource limits (setrlimit) and cleanup verification          */
/* ================================================================== */
static int rlimit_nofile(void)
{
    printf("\n=== setrlimit(RLIMIT_NOFILE): cap open files at 16 in a child process ===\n");
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (pid == 0) {
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        printf("  [child] before: soft=%lu hard=%lu\n", (unsigned long)rl.rlim_cur,
               (unsigned long)rl.rlim_max);
        struct rlimit nl = { 16, rl.rlim_max < 16 ? rl.rlim_max : rl.rlim_max };
        if (nl.rlim_cur > nl.rlim_max) nl.rlim_cur = nl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &nl) < 0) { perror("setrlimit"); _exit(1); }
        getrlimit(RLIMIT_NOFILE, &rl);
        printf("  [child] after : soft=%lu hard=%lu\n", (unsigned long)rl.rlim_cur,
               (unsigned long)rl.rlim_max);
        int fds[64], cnt = 0, err = 0;
        for (; cnt < 64; cnt++) {
            fds[cnt] = open("/dev/null", O_RDONLY);
            if (fds[cnt] < 0) { err = errno; break; }
        }
        printf("  [child] opened %d extra files, then open() failed: %s (errno=%d)\n",
               cnt, strerror(err), err);
        for (int i = 0; i < cnt; i++) close(fds[i]);
        printf("  [child] observed effect: further open() calls are refused once the limit is hit\n");
        fflush(stdout);
        _exit(err == EMFILE ? 0 : 1);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  [parent] child exit status %d -> limit %s\n",
           WIFEXITED(st) ? WEXITSTATUS(st) : -1, ok ? "ENFORCED (EMFILE seen)" : "NOT observed");
    applog("RLIMIT", "NOFILE test %s", ok ? "PASS" : "FAIL");
    return ok;
}

static int rlimit_cpu(void)
{
    printf("\n=== setrlimit(RLIMIT_CPU): 1 second CPU limit on a busy-loop child ===\n");
    fflush(NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (pid == 0) {
        struct rlimit core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &core);            /* no core file from SIGXCPU */
        struct rlimit cpu = { 1, 2 };
        if (setrlimit(RLIMIT_CPU, &cpu) < 0) { perror("setrlimit"); _exit(1); }
        printf("  [child pid=%d] CPU limit set: soft=1s hard=2s; spinning forever...\n", getpid());
        fflush(stdout);
        alarm(10);                                /* safety net: never hang */
        volatile unsigned long x = 0;
        for (;;) x++;
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    int ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGXCPU;
    if (WIFSIGNALED(st))
        printf("  [parent] child killed by signal %d (%s) after %.2f s\n",
               WTERMSIG(st), strsignal(WTERMSIG(st)), el);
    else
        printf("  [parent] child exited with status %d\n", WEXITSTATUS(st));
    printf("  observed effect: kernel stopped the runaway process when it used its CPU budget -> %s\n",
           ok ? "ENFORCED" : "NOT observed");
    applog("RLIMIT", "CPU test %s (%.2fs)", ok ? "PASS" : "FAIL", el);
    return ok;
}

static int cmd_rlimit(int argc, char **argv)
{
    if (argc != 1) goto bad;
    if (!strcmp(argv[0], "nofile")) return rlimit_nofile() ? 0 : 1;
    if (!strcmp(argv[0], "cpu"))    return rlimit_cpu() ? 0 : 1;
    if (!strcmp(argv[0], "all"))    { int a = rlimit_nofile(); int b = rlimit_cpu(); return (a && b) ? 0 : 1; }
bad:
    fprintf(stderr, "error: bad rlimit argument\nusage: campuscompute rlimit nofile | cpu | all\n");
    return 2;
}

static int count_threads(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    int t = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "Threads: %d", &t) == 1) break;
    fclose(f);
    return t;
}

static int verify_run(void)
{
    char shmn[64], sen[64], sfn[64];
    ipc_names(shmn, sen, sfn, sizeof shmn);
    int ok = 1;
    printf("\n=== Cleanup verification ===\n");

    int fd = shm_open(shmn, O_RDONLY, 0);
    if (fd >= 0) { close(fd); printf("  shared memory %s : STILL PRESENT (leak)\n", shmn); ok = 0; }
    else if (errno == ENOENT) printf("  shared memory %s : released\n", shmn);
    else { printf("  shared memory %s : check failed: %s\n", shmn, strerror(errno)); ok = 0; }

    const char *names[2] = { sen, sfn };
    for (int i = 0; i < 2; i++) {
        sem_t *s = sem_open(names[i], 0);
        if (s != SEM_FAILED) { sem_close(s); printf("  semaphore %s : STILL PRESENT (leak)\n", names[i]); ok = 0; }
        else if (errno == ENOENT) printf("  semaphore %s : released\n", names[i]);
        else { printf("  semaphore %s : check failed: %s\n", names[i], strerror(errno)); ok = 0; }
    }

    int status;
    pid_t p = waitpid(-1, &status, WNOHANG);
    if (p < 0 && errno == ECHILD) printf("  child processes : none left (no zombies)\n");
    else { printf("  child processes : still have children / zombies\n"); ok = 0; }

    int th = count_threads();
    printf("  threads in this process : %d %s\n", th, th == 1 ? "(only main thread)" : "(unexpected)");
    if (th != 1) ok = 0;

    printf("  result : %s\n", ok ? "PASS - nothing created by CampusCompute is left running or allocated"
                                  : "FAIL");
    applog("VERIFY", "cleanup verification %s", ok ? "PASS" : "FAIL");
    return ok;
}

static int cmd_cleanup(int argc, char **argv)
{
    int logs = 0;
    if (argc > 1 || (argc == 1 && strcmp(argv[0], "--logs"))) {
        fprintf(stderr, "error: bad cleanup argument\nusage: campuscompute cleanup [--logs]\n");
        return 2;
    }
    if (argc == 1) logs = 1;
    char shmn[64], sen[64], sfn[64];
    ipc_names(shmn, sen, sfn, sizeof shmn);
    printf("Removing CampusCompute IPC objects (only names owned by this program):\n");
    printf("  shm_unlink(%s) : %s\n", shmn, shm_unlink(shmn) == 0 ? "removed" : "not present");
    printf("  sem_unlink(%s) : %s\n", sen, sem_unlink(sen) == 0 ? "removed" : "not present");
    printf("  sem_unlink(%s) : %s\n", sfn, sem_unlink(sfn) == 0 ? "removed" : "not present");
    if (logs) {
        log_close();
        printf("  remove(%s) : %s\n", LOGFILE, remove(LOGFILE) == 0 ? "removed" : "not present");
    }
    return 0;
}

/* ================================================================== */
/* Integrated demonstration                                            */
/* ================================================================== */
static int cmd_demo(void)
{
    struct { const char *name; int ok; } r[9];
    int k = 0;
    Job jobs[MAXJ];
    int n = default_jobs(jobs);
    BankerState s;

    log_reset();
    applog("DEMO", "integrated demonstration start");
    printf("##### CampusCompute integrated demonstration #####\n");

    r[k].name = "CPU scheduling (FCFS/RR/Priority)";
    r[k++].ok = sched_run("compare", jobs, n, DEFAULT_Q) == 0;

    r[k].name = "Concurrent logger (pthreads)";
    r[k++].ok = logger_run(3, 6, 2);

    r[k].name = "Shared-memory IPC + semaphores";
    r[k++].ok = ipc_run(5);

    r[k].name = "Banker's safe state";
    banker_builtin(1, &s);
    printf("\n=== Banker's algorithm: SAFE example ===\n");
    r[k++].ok = banker_safety(&s) == 1;

    r[k].name = "Banker's unsafe state";
    banker_builtin(0, &s);
    printf("\n=== Banker's algorithm: UNSAFE example ===\n");
    r[k++].ok = banker_safety(&s) == 0;

    r[k].name = "Worker processes (simulated failures)";
    r[k++].ok = worker_all();

    r[k].name = "setrlimit NOFILE";
    r[k++].ok = rlimit_nofile();

    r[k].name = "setrlimit CPU";
    r[k++].ok = rlimit_cpu();

    r[k].name = "Cleanup verification";
    r[k++].ok = verify_run();

    int pass = 0;
    printf("\n################ INTEGRATED SUMMARY ################\n");
    for (int i = 0; i < k; i++) {
        printf("  %-40s %s\n", r[i].name, r[i].ok ? "PASS" : "FAIL");
        pass += r[i].ok;
    }
    printf("  ----------------------------------------------------\n");
    printf("  %d/%d checks passed. Log file: ./%s\n", pass, k, LOGFILE);
    applog("DEMO", "integrated demonstration finished: %d/%d passed", pass, k);
    return pass == k ? 0 : 1;
}

/* ================================================================== */
/* FR8 : CLI                                                           */
/* ================================================================== */
static void usage(FILE *o)
{
    fprintf(o,
"CampusCompute v" VERSION " - job scheduling and resource management prototype\n\n"
"Usage: campuscompute <command> [arguments]\n\n"
"Commands:\n"
"  sched fcfs|rr|priority|compare [-f jobs.txt] [-q quantum]\n"
"                       CPU scheduling simulation (default quantum %d)\n"
"  logger [P] [N] [C]   producer-consumer logger: P producers, N events each,\n"
"                       C consumers (defaults 3 6 2); writes %s\n"
"  ipc [messages]       shared memory + named semaphores between two processes\n"
"  banker safe|unsafe   Banker's safety algorithm on built-in examples\n"
"  banker file <path>   Banker's safety algorithm on a state read from a file\n"
"  worker normal|fail|signal|all\n"
"                       fork/waitpid demo; 'fail' and 'signal' are SIMULATED failures\n"
"  rlimit nofile|cpu|all  setrlimit() demonstrations (run in child processes)\n"
"  verify               check no processes/threads/IPC objects were left behind\n"
"  cleanup [--logs]     remove this program's IPC objects (and log file with --logs)\n"
"  demo                 run everything and print an integrated summary\n"
"  help                 show this message\n\n"
"Job file format: one job per line: id arrival burst priority  (# comments allowed)\n"
"Priority: lower number = higher priority.\n",
        DEFAULT_Q, LOGFILE);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "error: no command given\n\n");
        usage(stderr);
        return 2;
    }
    const char *c = argv[1];
    int rc;
    if (!strcmp(c, "help") || !strcmp(c, "-h") || !strcmp(c, "--help")) { usage(stdout); return 0; }
    else if (!strcmp(c, "sched"))   rc = cmd_sched(argc - 2, argv + 2);
    else if (!strcmp(c, "logger"))  rc = cmd_logger(argc - 2, argv + 2);
    else if (!strcmp(c, "ipc"))     rc = cmd_ipc(argc - 2, argv + 2);
    else if (!strcmp(c, "banker"))  rc = cmd_banker(argc - 2, argv + 2);
    else if (!strcmp(c, "worker"))  rc = cmd_worker(argc - 2, argv + 2);
    else if (!strcmp(c, "rlimit"))  rc = cmd_rlimit(argc - 2, argv + 2);
    else if (!strcmp(c, "verify"))  rc = verify_run() ? 0 : 1;
    else if (!strcmp(c, "cleanup")) rc = cmd_cleanup(argc - 2, argv + 2);
    else if (!strcmp(c, "demo"))    rc = cmd_demo();
    else {
        fprintf(stderr, "error: unknown command '%s'\nRun './campuscompute help' for usage.\n", c);
        return 2;
    }
    log_close();
    return rc;
}
