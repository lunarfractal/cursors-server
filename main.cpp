#define ASIO_STANDALONE

#include "cursors.hpp"

std::function<void(int)> signal_handler_callback;

void signal_handler(int signal) {
    if (signal_handler_callback) {
        signal_handler_callback(signal);
    }
}

int main() {
    cursors::server server;

    signal_handler_callback = [&server](int signum) {
        std::cout << "Signal " << signum << " received. Exiting cleanly" << std::endl;
        server.shutdown();
        exit(signum);
    };

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server.run(port);
    return 0;
}
