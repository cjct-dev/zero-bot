// TEMPORARY: stands in for the Tapo transport until the real implementation (tapo.cpp) is
// integrated, so that the rest of the app links. Every call reports that control is not ready.
// Delete this file when tapo.cpp arrives.
#include "smarthome/tapo.h"

namespace tapo {
namespace {
const char *const kNotYet = "Tapo control is not built into this version yet.";
}
std::string sha1_hex(const std::string &) { return ""; }
bool owned_by(const Device &, const std::string &) { return true; }
bool request(const Device &, const Credentials &, const json &, json &, std::string &err) { err = kNotYet; return false; }
bool get_info(const Device &, const Credentials &, json &, std::string &err) { err = kNotYet; return false; }
bool set_power(const Device &, const Credentials &, bool, std::string &err) { err = kNotYet; return false; }
bool get_energy(const Device &, const Credentials &, json &, std::string &err) { err = kNotYet; return false; }
bool list_sockets(const Device &, const Credentials &, json &, std::string &err) { err = kNotYet; return false; }
bool set_socket_power(const Device &, const Credentials &, const std::string &, bool, std::string &err) { err = kNotYet; return false; }
void forget_sessions() {}
}  // namespace tapo
