#include "../../etdb/ETDBFile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main() {
    const char* path = "/tmp/etdb_file_open_regression.bin";
    std::remove(path);

    ETDB::ETDBFile file;
    if (file.open(path, O_RDWR | O_CREAT) != 0) {
        std::fprintf(stderr, "open(create) failed\n");
        return 1;
    }
    if (file.write("A", 1) != 1) {
        std::fprintf(stderr, "write(first) failed\n");
        return 2;
    }
    file.close();

    if (file.open(path, O_RDWR | O_CREAT) != 0) {
        std::fprintf(stderr, "open(reuse) failed\n");
        return 3;
    }
    if (file.seek(0, SEEK_END) < 0) {
        std::fprintf(stderr, "seek(end) failed\n");
        return 4;
    }
    if (file.write("B", 1) != 1) {
        std::fprintf(stderr, "write(second) failed\n");
        return 5;
    }
    file.close();

    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::fprintf(stderr, "fopen(readback) failed\n");
        return 6;
    }
    char buf[4] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    if (n != 2 || std::memcmp(buf, "AB", 2) != 0) {
        std::fprintf(stderr, "unexpected contents: '%s'\n", buf);
        return 7;
    }
    return 0;
}
