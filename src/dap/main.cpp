#include "dap/dap_adapter.h"

#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    pscript::dap::DapAdapter adapter;
    return adapter.run(std::cin, std::cout);
}
