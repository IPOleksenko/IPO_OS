void kmain(void) {
    const char* s = "KERNEL_OK";

    while (*s) {
        asm volatile(
            "mov $0x0E, %%ah\n\t"
            "mov %0, %%al\n\t"
            "int $0x10\n\t"
            :
            : "r"(*s)
            : "ah", "al"
        );
        s++;
    }

    for (;;) {
        asm volatile("hlt");
    }
}
