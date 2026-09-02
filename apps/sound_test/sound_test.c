/*
 * sound_test.c - Comprehensive Sound and PC Speaker Test Application for IPO_OS
 *
 * Demonstrates the sound driver capabilities:
 * - Single beeps (various frequencies and durations)
 * - Diatonic musical scales
 * - Melodies (Super Mario theme, Beethoven's Ode to Joy, Star Wars)
 * - Continuous frequency sweep (glissando)
 * - Custom interactive frequency generator using scanf
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <driver/sound.h>
#include <system/timer.h>
#include <ioport.h>

/* Accurate delay helper */
static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile int j = 0; j < 800; j++) {
            io_wait();
        }
    }
}

/* Play a note with a given frequency and duration */
static void play_note(uint32_t freq, uint32_t duration_ms) {
    if (freq == 0 || freq == NOTE_REST) {
        sound_stop();
        delay_ms(duration_ms);
    } else {
        sound_play(freq);
        delay_ms(duration_ms);
        sound_stop();
    }
    /* Small gap between notes for articulation */
    delay_ms(25);
}

/* Structure for musical score */
typedef struct {
    uint16_t note;
    uint16_t duration;
} score_note_t;

/* 1. Musical Scale (Octave 4) */
static const score_note_t scale_c4[] = {
    { NOTE_C4, 250 },
    { NOTE_D4, 250 },
    { NOTE_E4, 250 },
    { NOTE_F4, 250 },
    { NOTE_G4, 250 },
    { NOTE_A4, 250 },
    { NOTE_B4, 250 },
    { NOTE_C5, 400 },
    { NOTE_REST, 100 },
    { NOTE_C5, 200 },
    { NOTE_B4, 200 },
    { NOTE_A4, 200 },
    { NOTE_G4, 200 },
    { NOTE_F4, 200 },
    { NOTE_E4, 200 },
    { NOTE_D4, 200 },
    { NOTE_C4, 400 }
};

/* 2. Super Mario Intro */
static const score_note_t melody_mario[] = {
    { NOTE_E5, 120 },
    { NOTE_E5, 120 },
    { NOTE_REST, 120 },
    { NOTE_E5, 120 },
    { NOTE_REST, 120 },
    { NOTE_C5, 120 },
    { NOTE_E5, 150 },
    { NOTE_G5, 250 },
    { NOTE_REST, 150 },
    { NOTE_G4, 250 }
};

/* 3. Beethoven's Ode to Joy */
static const score_note_t melody_ode_to_joy[] = {
    { NOTE_E4, 250 }, { NOTE_E4, 250 }, { NOTE_F4, 250 }, { NOTE_G4, 250 },
    { NOTE_G4, 250 }, { NOTE_F4, 250 }, { NOTE_E4, 250 }, { NOTE_D4, 250 },
    { NOTE_C4, 250 }, { NOTE_C4, 250 }, { NOTE_D4, 250 }, { NOTE_E4, 250 },
    { NOTE_E4, 350 }, { NOTE_D4, 150 }, { NOTE_D4, 400 },
    { NOTE_REST, 100 },
    { NOTE_E4, 250 }, { NOTE_E4, 250 }, { NOTE_F4, 250 }, { NOTE_G4, 250 },
    { NOTE_G4, 250 }, { NOTE_F4, 250 }, { NOTE_E4, 250 }, { NOTE_D4, 250 },
    { NOTE_C4, 250 }, { NOTE_C4, 250 }, { NOTE_D4, 250 }, { NOTE_E4, 250 },
    { NOTE_D4, 350 }, { NOTE_C4, 150 }, { NOTE_C4, 450 }
};

/* 4. Imperial March (Star Wars) Theme */
static const score_note_t melody_star_wars[] = {
    { NOTE_A4, 350 }, { NOTE_A4, 350 }, { NOTE_A4, 350 },
    { NOTE_F4, 250 }, { NOTE_C5, 120 },
    { NOTE_A4, 350 }, { NOTE_F4, 250 }, { NOTE_C5, 120 },
    { NOTE_A4, 600 }, { NOTE_REST, 150 },
    { NOTE_E5, 350 }, { NOTE_E5, 350 }, { NOTE_E5, 350 },
    { NOTE_F5, 250 }, { NOTE_C5, 120 },
    { NOTE_GS4, 350 }, { NOTE_F4, 250 }, { NOTE_C5, 120 },
    { NOTE_A4, 600 }
};

static void play_score(const score_note_t *score, size_t count) {
    for (size_t i = 0; i < count; i++) {
        play_note(score[i].note, score[i].duration);
    }
    sound_stop();
}

static void test_beeps(void) {
    printf("[Sound] Testing beeps...\n");
    printf("  - Low beep (220 Hz, A3)\n");
    play_note(NOTE_A3, 300);
    delay_ms(150);

    printf("  - Standard beep (440 Hz, A4)\n");
    play_note(NOTE_A4, 300);
    delay_ms(150);

    printf("  - High beep (880 Hz, A5)\n");
    play_note(NOTE_A5, 300);
    delay_ms(150);

    printf("  - High alert beep (1760 Hz, A6)\n");
    play_note(NOTE_A6, 250);
    delay_ms(150);
    printf("[Sound] Beep test complete!\n");
}

static void test_scale(void) {
    printf("[Sound] Playing C Major Scale (C4 to C5)...\n");
    play_score(scale_c4, sizeof(scale_c4) / sizeof(scale_c4[0]));
    printf("[Sound] Scale playback complete!\n");
}

static void test_mario(void) {
    printf("[Sound] Playing Super Mario Theme...\n");
    play_score(melody_mario, sizeof(melody_mario) / sizeof(melody_mario[0]));
    printf("[Sound] Mario melody complete!\n");
}

static void test_ode_to_joy(void) {
    printf("[Sound] Playing Beethoven's Ode to Joy...\n");
    play_score(melody_ode_to_joy, sizeof(melody_ode_to_joy) / sizeof(melody_ode_to_joy[0]));
    printf("[Sound] Ode to Joy complete!\n");
}

static void test_star_wars(void) {
    printf("[Sound] Playing Imperial March (Star Wars)...\n");
    play_score(melody_star_wars, sizeof(melody_star_wars) / sizeof(melody_star_wars[0]));
    printf("[Sound] Imperial March complete!\n");
}

static void test_sweep(void) {
    printf("[Sound] Frequency sweep (200 Hz -> 2000 Hz)...\n");
    for (uint16_t freq = 200; freq <= 2000; freq += 25) {
        sound_play(freq);
        delay_ms(8);
    }
    sound_stop();
    delay_ms(100);

    printf("[Sound] Reverse frequency sweep (2000 Hz -> 200 Hz)...\n");
    for (uint16_t freq = 2000; freq >= 200; freq -= 25) {
        sound_play(freq);
        delay_ms(8);
    }
    sound_stop();
    printf("[Sound] Sweep test complete!\n");
}

static void test_interactive(void) {
    uint32_t freq = 0;
    uint32_t duration = 0;

    printf("\n=== Custom Sound Generator ===\n");
    printf("Enter frequency in Hz: ");
    if (scanf("%u", &freq) != 1 || freq == 0) {
        printf("Invalid frequency!\n");
        return;
    }

    printf("Enter duration in ms (10 - 5000): ");
    if (scanf("%u", &duration) != 1 || duration < 10 || duration > 5000) {
        printf("Invalid duration!\n");
        return;
    }

    printf("Playing %u Hz for %u ms...\n", freq, duration);
    play_note(freq, duration);
    printf("Done!\n");
}

static void show_menu(void) {
    printf("\n=========================================\n");
    printf("          IPO_OS SOUND TEST SUITE         \n");
    printf("=========================================\n");
    printf("  1. Beep Test (Low, Mid, High frequencies)\n");
    printf("  2. Musical Scale (C Major C4..C5)\n");
    printf("  3. Melody: Super Mario Theme\n");
    printf("  4. Melody: Beethoven's Ode to Joy\n");
    printf("  5. Melody: Star Wars Imperial March\n");
    printf("  6. Frequency Sweep (Pitch Glissando)\n");
    printf("  7. Custom Tone Generator (Interactive)\n");
    printf("  8. Run All Tests Automatically\n");
    printf("  0. Exit\n");
    printf("=========================================\n");
    printf("Select option [0-8]: ");
}

static void run_all_tests(void) {
    printf("\n>>> Running Full Sound Test Suite <<<\n\n");
    test_beeps();
    delay_ms(300);

    test_scale();
    delay_ms(300);

    test_sweep();
    delay_ms(300);

    test_mario();
    delay_ms(300);

    test_ode_to_joy();
    delay_ms(300);

    test_star_wars();
    printf("\n>>> Full Sound Test Suite Complete! <<<\n");
}

int main(int argc, char **argv) {
    sound_init();

    /* If command line arguments provided */
    if (argc > 1) {
        if (strcmp(argv[1], "beep") == 0) {
            uint32_t freq = 440;
            uint16_t dur = 400;
            if (argc > 2) {
                uint32_t val = 0;
                for (int i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++) {
                    val = val * 10 + (argv[2][i] - '0');
                }
                if (val > 0) freq = val;
            }
            if (argc > 3) {
                uint32_t val = 0;
                for (int i = 0; argv[3][i] >= '0' && argv[3][i] <= '9'; i++) {
                    val = val * 10 + (argv[3][i] - '0');
                }
                if (val > 0) dur = (uint16_t)val;
            }
            printf("Playing beep: %u Hz, %u ms\n", freq, dur);
            play_note(freq, dur);
            return 0;
        } else if (strcmp(argv[1], "scale") == 0) {
            test_scale();
            return 0;
        } else if (strcmp(argv[1], "mario") == 0) {
            test_mario();
            return 0;
        } else if (strcmp(argv[1], "ode") == 0) {
            test_ode_to_joy();
            return 0;
        } else if (strcmp(argv[1], "starwars") == 0) {
            test_star_wars();
            return 0;
        } else if (strcmp(argv[1], "sweep") == 0) {
            test_sweep();
            return 0;
        } else if (strcmp(argv[1], "all") == 0) {
            run_all_tests();
            return 0;
        } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: sound_test [command]\n");
            printf("Commands:\n");
            printf("  beep [freq] [ms]  Play a single beep (default 440Hz 400ms)\n");
            printf("  scale             Play C Major musical scale\n");
            printf("  mario             Play Super Mario melody\n");
            printf("  ode               Play Ode to Joy melody\n");
            printf("  starwars          Play Star Wars Imperial March\n");
            printf("  sweep             Play frequency sweep\n");
            printf("  all               Run all sound tests\n");
            printf("  (no args)         Interactive menu\n");
            return 0;
        } else {
            printf("Unknown option: %s. Use 'sound_test help' for options.\n", argv[1]);
            return 1;
        }
    }

    /* Interactive mode */
    for (;;) {
        show_menu();
        int choice = -1;
        if (scanf("%d", &choice) != 1) {
            printf("\nInvalid input. Exiting.\n");
            break;
        }

        if (choice == 0) {
            printf("Exiting sound test. Goodbye!\n");
            break;
        } else if (choice == 1) {
            test_beeps();
        } else if (choice == 2) {
            test_scale();
        } else if (choice == 3) {
            test_mario();
        } else if (choice == 4) {
            test_ode_to_joy();
        } else if (choice == 5) {
            test_star_wars();
        } else if (choice == 6) {
            test_sweep();
        } else if (choice == 7) {
            test_interactive();
        } else if (choice == 8) {
            run_all_tests();
        } else {
            printf("Invalid selection! Please choose 0-8.\n");
        }
    }

    sound_stop();
    return 0;
}

