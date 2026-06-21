#include <stdio.h>

int main() {
    int a = 10;
    int b = 20;
    int c = a + b;
    int d = c * 2;
    
    // unreachable block testing
    if(0){
        printf("This branch should be completely pruned.\n");
    }
    // dead variable testing
    int dead_var = 999; 
    
    // the only live variable is 'd', which should fold to 60
    printf("Result is: %d\n", d);
    return 0;
    // post-return dead code testing
    printf("This should also be eliminated.\n");
}

// Goal: Prove that the tool can evaluate math at compile-time, track variable usage, and
// brutally eliminate dead/unreachable code.

// What to expect in the UI:
//a, b, and c will disappear.
// d will be folded to 60.
// dead_var will be deleted.
