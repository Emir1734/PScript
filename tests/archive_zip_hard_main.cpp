#include <iostream>

int run_archive_zip_corruption_tests();
int run_archive_zip64_tests();
void cleanup_archive_zip_work_root_if_empty();

int main() {
    if (run_archive_zip_corruption_tests() != 0) {
        return 1;
    }
    if (run_archive_zip64_tests() != 0) {
        return 1;
    }
    cleanup_archive_zip_work_root_if_empty();
    std::cout << "archive zip hard tests passed\n";
    return 0;
}
