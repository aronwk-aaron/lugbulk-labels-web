// Built-in sample labels for the live design preview — the same set as
// lugbulk-label's samples.py: a clear part, a white part, a huge part, a
// long name, and several people sharing a part.
#pragma once

#include <vector>

#include "sheet_pivot.h"

namespace lugbulk::samples {

// In label order (see ordering::order_records), numbered "N of M".
std::vector<LabelRecord> sample_records();

}  // namespace lugbulk::samples
