#ifndef NAUMI_KEYMAP_H
#define NAUMI_KEYMAP_H

/* Full US-QWERTY evdev keycode -> ASCII tables (unshifted + shifted),
   shared by every app that needs real text entry (userland/edit,
   userland/console) — console.c's old KEYMAP only covered lowercase/
   digits, enough for typing bare paths but not for editing prose or
   punctuation-heavy input. Codes match the same Linux evdev numbering
   already relied on elsewhere in this tree (userland/doom's KC_* defines,
   the old console.c table). */

#define KC_ESC 1
#define KC_BACKSPACE 14
#define KC_TAB 15
#define KC_ENTER 28
#define KC_LCTRL 29
#define KC_LSHIFT 42
#define KC_RSHIFT 54
#define KC_LALT 56
#define KC_SPACE 57
#define KC_UP 103
#define KC_LEFT 105
#define KC_RIGHT 106
#define KC_DOWN 108
#define KC_HOME 102
#define KC_END 107
#define KC_DELETE 111

static const char KEYMAP_NORMAL[58] = {
    /*0*/ 0, 0, '1', '2', '3', '4', '5', '6', '7', '8',
    /*10*/ '9', '0', '-', '=', 0, 0, 'q', 'w', 'e', 'r',
    /*20*/ 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
    /*30*/ 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /*40*/ '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    /*50*/ 'm', ',', '.', '/', 0, 0, 0, ' ',
};

static const char KEYMAP_SHIFT[58] = {
    /*0*/ 0, 0, '!', '@', '#', '$', '%', '^', '&', '*',
    /*10*/ '(', ')', '_', '+', 0, 0, 'Q', 'W', 'E', 'R',
    /*20*/ 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
    /*30*/ 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /*40*/ '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    /*50*/ 'M', '<', '>', '?', 0, 0, 0, ' ',
};

static inline char keymap_translate(unsigned short code, int shift) {
    if (code >= sizeof(KEYMAP_NORMAL)) {
        return 0;
    }
    return shift ? KEYMAP_SHIFT[code] : KEYMAP_NORMAL[code];
}

#endif
