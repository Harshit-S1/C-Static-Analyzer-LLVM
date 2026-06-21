#include <stdio.h>

int main() {
    int i = 0;
    int sum = 0;
    int max;
    // protecting 'max' via pointer addressing
    scanf("%d", &max);
    
    // loop header
    while (i < max) {
        sum = sum + i;
        i++;
    }
    // protecting 'sum' from DCE
    printf("Total sum: %d\n", sum);
    return 0;
}

// Goal: Prove that the dominator tree math and live variable analysis (IN/OUT sets) can
// handle complex cyclic graphs (loops) without breaking.

// What to expect in the UI:
// You should see a thick-bordered [LOOP HEADER] block.
// You should see the blue "Loop Back-edge" arrow.
// The Live IN and Live OUT sets should perfectly show i, sum, and max 
// circulating through the loop blocks until max is no longer needed.