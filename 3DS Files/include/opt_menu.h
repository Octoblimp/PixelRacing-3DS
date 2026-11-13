/* The port's Controls and 3D pages of the game's Options screen (opt_menu.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* m_312cc.c fn_00034b04, with the game's own menu props: loads the port's props
 * (tools/make_menu_props.py). */
void optm_load(void);

/* m_20f70.c, Options screen (state 12). The game's page has 7 rows; the port adds Controls
 * and 3D after them. optm_options_rows fills those two (model slots in q[], the label of the
 * selected one in `label`) and returns the row count. */
int optm_options_rows(int32_t *q, char *label);
/* On the Controls or 3D page: fills the row count, model slots, label and title, and returns
 * true; on the game's own page returns false. */
bool optm_fill(int32_t *q, char *label, char *title);

/* m_2d550.c, Options screen input, after the game moved the cursor: true when it took this
 * tick's OK / back (a page of the port's, or opening one). */
bool optm_input(void);

/* m_12a68.c: draws prop i (node o, model slot; pass 1 = the floor reflection) of the Controls
 * or 3D page, or the game's page's rows 7 and 8. Returns false for the game's own rows. */
bool optm_prop(uint8_t *o, int i, int pass, int slot);

/* The Controls page is open (the bottom screen shows a steering meter). */
bool optm_meter(void);
