#include "ModelDrawerData.h"

CONFIG(int, UnitLodDist).defaultValue(1000).headlessValue(0).deprecated(true);

// sim|draw WS-2 (transforms-dirty-skip.md §8): every N extraction passes,
// re-extract each transform-skipped object into scratch and error-log storage
// mismatches -- the missed-mutation-path detector. Enable for at least one
// full-length replay per gate battery.
CONFIG(int, SimDrawTransformSkipOracle)
	.defaultValue(0)
	.minimumValue(0)
	.description("Every N transform-extraction passes, re-extract skipped objects and log storage mismatches (sim|draw WS-2 transform-skip oracle); 0 = off.");