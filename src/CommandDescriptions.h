#pragma once

#include <string>
#include <string_view>

// A one-line description of a named command, for the command palette; empty for a name that has none.
std::string commandDescription(std::string_view name);
