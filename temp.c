
    #include <stdio.h>
    int main() {
    int x = 3 + 5;
    int y;
    while(x > 0) {
        y = 10;
        x = x - 1;
    }
    scanf("%d", &y);
    printf("%d", y);
    return 0;
}
int dead_func() {
    return 42;
}