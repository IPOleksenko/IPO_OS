#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv) {
    int number;
    char word[128];
    char letter;

    (void)argc;
    (void)argv;

    printf("scanf demo\n");
    printf("Enter an integer: ");
    if (scanf("%d", &number) != 1) {
        printf("Invalid integer\n");
        return 1;
    }
    printf("Read integer: %d\n", number);

    printf("Enter a word: ");
    if (scanf("%s", word) != 1) {
        printf("Invalid word\n");
        return 1;
    }
    printf("Read word: %s\n", word);

    printf("Enter one character: ");
    if (scanf("%c", &letter) != 1) {
        printf("Invalid character\n");
        return 1;
    }
    printf("Read character: %c\n", letter);

    return 0;
}
