// MGPU Bridge - structured diagnostic logging (brief, section 08)
#include <reshade.hpp>

#include "diag.hpp"

namespace mgpu::diag
{
void info (const char *line)
{
    reshade::log::message(reshade::log::level::info, line);
}

void warn (const char *line)
{
    reshade::log::message(reshade::log::level::warning, line);
}

void error(const char *line)
{
    reshade::log::message(reshade::log::level::error, line);
}
}
