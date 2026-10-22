// [windows port] A debug server for looking into the running game from a
// tool (tools/gta4-live-mcp): guest memory, cvars and the log over a
// line-delimited JSON protocol on 127.0.0.1. Off unless gta4_debug_server_port
// is set; writes to guest memory only with gta4_debug_server_allow_write.
#pragma once

#include <filesystem>

namespace gta4::debug_server {

// Starts listening when gta4_debug_server_port is set (once; later calls do
// nothing). `logs_dir` is where the app writes its log files.
void Start(const std::filesystem::path& logs_dir);

}  // namespace gta4::debug_server
