#!/bin/bash
# Independent feature tests for CampusCompute. Each test checks an exit code
# and (optionally) a string in the output.
BIN=./campuscompute
pass=0; fail=0

t() {  # t "name" expected_rc "grep pattern or empty" cmd...
    local name=$1 want=$2 pat=$3; shift 3
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" -eq "$want" ] && { [ -z "$pat" ] || grep -q -- "$pat" <<<"$out"; }; then
        printf "PASS  %s\n" "$name"; pass=$((pass+1))
    else
        printf "FAIL  %s (rc=%s, wanted %s, pattern '%s')\n" "$name" "$rc" "$want" "$pat"; fail=$((fail+1))
    fi
}

[ -x $BIN ] || { echo "build first: make"; exit 1; }
t "help"                      0 "Usage"                $BIN help
t "FCFS"                      0 "Average waiting"      $BIN sched fcfs
t "Round Robin q=3"           0 "time quantum = 3"     $BIN sched rr -q 3
t "Priority (tie-break)"      0 "Tie-break"            $BIN sched priority
t "Compare"                   0 "Lowest average"       $BIN sched compare
t "Sched from file"           0 "Loaded 5 jobs"        $BIN sched fcfs -f jobs.txt
t "Sched idle gap"            0 "IDLE"                 $BIN sched rr -f jobs_idle.txt
t "Bad job file"              1 "expected 'id"         $BIN sched fcfs -f bad_jobs.txt
t "Bad quantum"               2 "quantum must"         $BIN sched rr -q 0
t "Logger"                    0 "PASS"                 $BIN logger 3 6 2
t "Logger 1 consumer"         0 "PASS"                 $BIN logger 2 10 1
t "IPC"                       0 "PASS"                 $BIN ipc 5
t "Banker safe"               0 "SAFE state"           $BIN banker safe
t "Banker unsafe"             0 "UNSAFE state"        $BIN banker unsafe
t "Banker file unsafe"        0 "UNSAFE state"        $BIN banker file banker_unsafe.txt
t "Banker malformed file"     1 "malformed"            $BIN banker file banker_bad.txt
t "Worker normal"             0 "COMPLETED NORMALLY"   $BIN worker normal
t "Worker simulated fail"     0 "SIMULATED failure"    $BIN worker fail
t "Worker simulated crash"    0 "SIMULATED crash"      $BIN worker signal
t "rlimit nofile"             0 "ENFORCED"             $BIN rlimit nofile
t "rlimit cpu"                0 "ENFORCED"             $BIN rlimit cpu
t "verify"                    0 "PASS"                 $BIN verify
t "Invalid command"           2 "unknown command"      $BIN frobnicate
t "No command"                2 "no command"           $BIN
t "Demo"                      0 "9/9 checks passed"    $BIN demo
$BIN cleanup >/dev/null
echo "----"; echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
