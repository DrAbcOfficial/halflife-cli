#pragma once

#include <metahook.h>	// pulls in cdll_export.h (no include guard, must not be re-included)

void HUD_Init(void);
void HUD_Frame(double time);
