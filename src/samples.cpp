#include "samples.h"

#include "ordering.h"
#include "sheet_layout.h"

namespace lugbulk::samples {

std::vector<LabelRecord> sample_records() {
    struct Part {
        const char *id, *description, *lego, *bl;
        std::vector<std::pair<const char*, const char*>> orders;  // (person, qty)
    };
    // Keep in sync with lugbulk-label's samples.py.
    const Part parts[] = {
        {"6097276", "BASE PLATE 32X32", "Bright Green", "Bright Green", {{"Ann Lee", "4"}, {"Bob Roe", "10"}}},
        {"6508677", "BRICK 2X4, TRANSPARENT", "Transparent", "Trans-Clear", {{"Cat Diaz", "100"}}},
        {"6545156", "PLANT, W/ PLATE 1X1, NO. 1", "White", "White", {{"Alexandria Montgomery-Smith", "250"}}},
        {"4211388", "BRICK 1X2", "Medium Stone Grey", "Light Bluish Gray",
         {{"Ann Lee", "25"}, {"Bob Roe", "50"}, {"Dan Wu", "500"}}},
        {"6584302", "FROG", "Black", "Black", {{"Eve Park", "50"}}},
        {"6514224", "FLAT TILE 1X2", "Transparent Light Blue", "Trans-Light Blue", {{"Bob Roe", "75"}}},
    };
    std::vector<LabelRecord> records;
    for (const auto& p : parts) {
        for (const auto& [person, qty] : p.orders) {
            LabelRecord r;
            r.person = person;
            r.element_id = p.id;
            r.description = p.description;
            r.lego_color = p.lego;
            r.bl_color = p.bl;
            r.qty = qty;
            r.image_url = layout::image_url_for(p.id);
            records.push_back(std::move(r));
        }
    }
    return ordering::order_records(std::move(records), ordering::PartOrder::kHeaviest);
}

}  // namespace lugbulk::samples
