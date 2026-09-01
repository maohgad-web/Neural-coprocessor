// MGPU Bridge - structured diagnostic logging (brief, section 08)
//
// Every line this add-on writes goes through this interface, so the
// format is one place: a fixed "[MGPU][Tn]" prefix carrying the task id,
// and the inputs to each decision, not just the outcome. The only sink
// is ReShade's log - together with the debug window it is the complete
// instrument set for P0.
//
// Called from the game thread (ReShade callbacks) and the bridge thread;
// reshade::log::message is used concurrently by ReShade itself.
#pragma once

namespace mgpu::diag
{
    void info (const char *line);   // reshade::log::level::info
    void warn (const char *line);   // reshade::log::level::warning
    void error(const char *line);   // reshade::log::level::error
}
