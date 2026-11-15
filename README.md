# CampusCompute

Command-line prototype demonstrating operating-system concepts: CPU scheduling, a
pthread producer-consumer logger, POSIX shared memory + named semaphores, the Banker's
safety algorithm, fork/waitpid worker monitoring, setrlimit, and resource cleanup.
Written in C for Linux / Ubuntu / WSL Ubuntu.

## Prerequisites
    sudo apt update && sudo apt install build-essential zip unzip

## Build
    make            # builds ./campuscompute   (gcc -Wall -Wextra -Wpedantic -pthread -lrt)

Manual build command: `gcc -Wall -Wextra -Wpedantic -std=gnu11 -O2 -o campuscompute campuscompute.c -pthread -lrt`

## Run
    ./campuscompute help                         # all commands
    ./campuscompute sched fcfs                   # FCFS (built-in 5-job set)
    ./campuscompute sched rr -q 3                # Round Robin, quantum 3 (default 4)
    ./campuscompute sched priority               # non-preemptive priority
    ./campuscompute sched compare                # all three on the same jobs
    ./campuscompute sched fcfs -f jobs.txt       # jobs from a file
    ./campuscompute logger [P] [N] [C]           # producers, events each, consumers
    ./campuscompute ipc [messages]               # shared memory + named semaphores
    ./campuscompute banker safe | unsafe         # built-in Banker's examples
    ./campuscompute banker file banker_unsafe.txt
    ./campuscompute worker normal|fail|signal|all   # fork/waitpid (failures are SIMULATED)
    ./campuscompute rlimit nofile|cpu|all        # setrlimit demonstrations
    ./campuscompute verify                       # confirm nothing was left behind
    ./campuscompute demo                         # integrated demonstration + summary

Job file format: `id arrival burst priority` per line (`#` comments). Lower priority number = higher priority.

## Rules stated by the program
- FCFS tie-break: smaller job id.
- Priority (non-preemptive): lower number first; tie -> earlier arrival, then smaller id.
- Round Robin: default quantum 4 (`-q`); a job arriving exactly when a quantum expires is queued before the pre-empted job.

## Testing
    make test       # runs test.sh: 25 independent feature tests

## Cleanup
    ./campuscompute cleanup          # removes this program's shm/semaphores (/cc_*_<uid>)
    ./campuscompute cleanup --logs   # also removes campuscompute.log
    make clean                       # removes the binary and campuscompute.log

Only objects created by this program are touched. The log file is `campuscompute.log`
in the current directory.

## Design notes
- Logger: bounded circular buffer (5 slots), one mutex, two condition variables (not_full / not_empty); one poison-pill per consumer for orderly shutdown, then pthread_join.
- IPC: parent creates `/cc_shm_<uid>` and two named semaphores (empty=1, full=0) giving strict hand-off; the child re-opens everything by name. sem_timedwait prevents deadlock if a peer dies. All objects are unlinked on every exit path.
- Workers: `fail` exits with code 3; `signal` raises SIGUSR1 on itself. Both are deliberately injected and labelled SIMULATED.
- rlimit: RLIMIT_NOFILE (open() fails with EMFILE) and RLIMIT_CPU (child killed by SIGXCPU), each in a forked child so the parent is unaffected.
