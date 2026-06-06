// suki-lsp — SukiCode Language Server
// LSP server for IDE integration (code completion, diagnostics, go-to-definition).

#include "LSPServer.h"
#include <iostream>

int main() {
    suki::lsp::LSPServer server;
    server.run();
    return 0;
}
