#include <stdio.h>
#include <stdlib.h>

int main() {
    char cmd[50];
    int idx;
    int arr[10];
    // taint sources
    // The analyzer should mark 'cmd' and 'idx' as tainted
    scanf("%s", cmd);
    scanf("%d", &idx);

    //taint sink - 1, command injection
    //passes tainted string directly to the OS
    system(cmd);

    //taint sink - 2, buffer overflow
    //uses tainted integer as an array index without bounds checking
    arr[idx] = 100;

    // taint sink - 3, printf statement
    // passes tainted string as the format argument
    printf(cmd);
    return 0;
}

// Goal: This snippet introduces a "taint" via user input and explicitly passes
// it into all three of your tracked danger sinks.

// What to expect in the UI: 
// You should see a red [SECURITY RISK] octagon blocks.