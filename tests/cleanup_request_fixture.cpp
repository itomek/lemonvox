// Synthetic qualification helper: exercise production request construction.
#include "protocol.h"
#include <iostream>
#include <iterator>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const std::string transcript((std::istreambuf_iterator<char>(std::cin)), {});
    lemonvox::ProtocolConfiguration config;
    config.endpoint = {"http://synthetic.invalid/v1", true, false, true, true, true};
    config.asr_model = "synthetic-asr";
    config.cleanup_model = argv[1];
    config.cleanup_prompt = lv_cleanup_default_prompt();
    config.cleanup_backend = LV_CLEANUP_BACKEND_LEMONADE;
    lemonvox::TransportRequest request;
    if (lemonvox::Protocol::make_cleanup_request(config, transcript, request) != LV_PROTOCOL_OK) return 1;
    std::cout.write(reinterpret_cast<const char *>(request.body.data()), request.body.size());
}
