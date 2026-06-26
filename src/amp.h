#pragma once
#include <stdbool.h>

void amp_init(void);
void amp_set(bool on);   /* true = powered + unmuted; false = shutdown */
