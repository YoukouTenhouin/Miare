#include <miare/database.hpp>

int main() {
    auto result = miare::Result<int, miare::WriterBusy>::success(42);
    return result.value() == 42 ? 0 : 1;
}
