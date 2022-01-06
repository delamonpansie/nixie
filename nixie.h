#ifndef NIXIE
#define NIXIE

struct config {
        uint8_t crc;
        unsigned char tube_pwm_freq; // in 10Hz
        unsigned char tube_pwm_duty;
        unsigned char led_red_brightness;
        unsigned char led_green_brightness;
        unsigned char led_blue_brightness;
        unsigned char antipoison_start;
        unsigned char antipoison_duration;
        unsigned char fade_mode;
};

enum op {
        NOP,
        MODE      = _BV(1),
        DOWN      = _BV(2),
        UP        = _BV(3),
        REFRESH
};

// provided by main
extern struct config config;
extern void button_scan();
extern void paint(char x, char y, char z, char d);
extern void wait_frame_sync();

// provided by board
extern unsigned char button_read(); // returns inverted mask of pressed buttons
extern void config_apply();
extern void board_init();

#endif
