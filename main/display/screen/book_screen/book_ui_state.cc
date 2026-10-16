#include "book_ui_state.h"

namespace book_screen_detail {

BookUiState& State() {
    static BookUiState s;
    return s;
}

}  // namespace book_screen_detail
