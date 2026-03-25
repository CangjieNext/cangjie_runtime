#include <stdio.h>
#include <unistd.h>
#include <stdint.h>

extern "C" void foreignCanBeCalled() {
    printf("[foreign] foreignCanBeCalled: OK\n");
}

extern "C" void longRunningForeignCall() {
    int sleepSeconds = 5;
    printf("[foreign] longRunningForeignCall: started, sleeping for %d seconds...\n", sleepSeconds);
    sleep(sleepSeconds);
    printf("[foreign] longRunningForeignCall: woke up, finished\n");
}


typedef void (*callback)(int);
static callback my_stored_callback = NULL;

extern "C" void set_callback(callback cb) {
    my_stored_callback = cb;
    printf("[foreign] set_callback: callback has been set successfully\n");
}

extern "C" void call_callback(int value) {
    if (my_stored_callback) {
        printf("[foreign] call_callback: calling callback with arg %d\n", value);
        my_stored_callback(value);
    } else {
        printf("[foreign] ERROR: no callback has been set\n");
    }
}
